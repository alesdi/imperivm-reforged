#pragma once

// The saved game: what a save contains, and the reader and writer for it.
//
// Specification: docs/formats/save.md
//
// ## Why this is small
//
// Three pieces of the job were already done before this file existed. The
// block container (`formats/bfhp.hpp`) reads and writes byte-identically -- 23
// of 23 retail containers rebuild bit for bit -- and the original keeps its
// saved games in exactly that container, so nothing here has to invent a file.
// The script VM already round-trips a suspended coroutine
// (`script/vm.hpp`), and the scheduler already round-trips the whole set of
// them with a magic and a version (`script/scheduler.cpp`). What was missing
// is the piece that says **what a saved game contains** -- which is this.
//
// So this file is a *container of sections*, not a monolithic blob:
//
//   * `World` writes its own section (`World::serialize`), because the world
//     owns the object table, the clock, the RNG and the two name tables.
//   * `script::Scheduler` and `SelectionTable` already have serialise pairs and
//     are written through them, unchanged.
//   * every registered `System` gets a section of its own, named
//     `system:<System::name()>`, whose bytes the system produces. `System` has
//     no `serialize` in its interface -- adding one would put a virtual on a
//     seam eleven domains share -- so those bytes arrive here from the caller
//     that owns the system, and leave here the same way. `GameSession::save`
//     and `GameSession::load` are that caller; the pairs themselves live in
//     `src/sim/save_systems.cpp`.
//
// The section table is the extension point: a domain that grows a serialise
// pair adds a section and nothing in this file changes.
//
// ## Versioning, from the first commit
//
// A save written today must be **refused** by a build whose state vector has
// changed, not loaded into something subtly wrong. There are two version
// numbers and they mean different things:
//
//   * `kSaveFormatVersion` -- the envelope's own layout (the header, the
//     section framing). Changes when the framing changes, which should be
//     almost never.
//   * `kStateVectorVersion` -- **the meaning of the bytes inside the
//     sections.** Bump it whenever a field is added to, removed from, or
//     reinterpreted in any section, including a section owned by a system.
//     A mismatch is `FormatError::unsupported` at the header, before a single
//     object is touched.
//
// The original did the same thing and said so: `gbr.exe` carries
// `Savegame version of this build: %d`, `Last savegame loaded version: %d`, and
// for the replay stream `"This replay is in invalid format and cannot be
// loaded. It has probably been saved with a different version of the game!"`.
//
// It also refused on a **map** mismatch, with its own message: `"The map you
// played this game on when the game was saved (%s1) is missing or has been
// changed"`. `SaveMeta::map` carries that identity and `LoadOptions::map`
// checks it.
//
// ## The check that makes a save correct
//
// A save is correct when loading it reproduces the simulation exactly. That is
// checkable rather than assertable: the meta section records
// `World::hashes()` as it stood when the save was written -- the original does
// this too, its persist stream has a `GameHashes` group with the same channel
// names -- and `verify_hashes` recomputes them after a load and compares.
//
// This is not decoration. A system whose state was **not** restored shows up
// here as a mismatched `slots` channel at load time, loudly, instead of as a
// divergence twenty turns later.
//
// **It has a blind spot, and it is named.** `CommandSystem` has no `hash`
// override, `AiSystem::hash` is a no-op and `AreaSystem::hash` is deliberately
// empty, so losing one of those three sections passes this check unchanged.
// What catches them is a byte comparison: save, load, save again, require the
// two blobs to be identical. `engine/tools/imsave` does that over a real map
// and `engine/tests/test_save.cpp` asserts both halves -- that the hash really
// cannot see them, and that the bytes really can. See docs/formats/save.md.
//
// ## Freestanding
//
// Byte spans in, byte vectors out. `engine/core` cannot open a file
// (docs/engine/architecture.md), so the caller reads and writes; putting the
// result inside a `.bfhp` container is the app's job.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/sim/tick.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::script {
class Scheduler;
}  // namespace imperivm::core::script

namespace imperivm::core::sim {

class SelectionTable;

// --------------------------------------------------------------------------
// little-endian output
// --------------------------------------------------------------------------
//
// The convention `Scheduler::serialize` established, hoisted here so that the
// world, the save envelope and any future section agree by construction rather
// than by three private copies that drift. `ByteReader` is the matching
// reader and already exists (`formats/byte_reader.hpp`).

namespace bytes {

inline void put_u8(std::vector<std::byte>& out, std::uint32_t value) {
  out.push_back(static_cast<std::byte>(value & 0xFFu));
}

inline void put_u16(std::vector<std::byte>& out, std::uint32_t value) {
  put_u8(out, value);
  put_u8(out, value >> 8);
}

inline void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
  put_u16(out, value);
  put_u16(out, value >> 16);
}

inline void put_i32(std::vector<std::byte>& out, std::int32_t value) {
  put_u32(out, static_cast<std::uint32_t>(value));
}

inline void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
  put_u32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFu));
  put_u32(out, static_cast<std::uint32_t>(value >> 32));
}

inline void put_i64(std::vector<std::byte>& out, std::int64_t value) {
  put_u64(out, static_cast<std::uint64_t>(value));
}

/// Length-prefixed, not NUL-terminated: a name with an embedded zero is
/// hostile input, not a truncation point.
inline void put_string(std::vector<std::byte>& out, std::string_view text) {
  put_u32(out, static_cast<std::uint32_t>(text.size()));
  for (const char c : text) put_u8(out, static_cast<std::uint8_t>(c));
}

[[nodiscard]] bool get_u64(ByteReader& reader, std::uint64_t& out) noexcept;
[[nodiscard]] bool get_i64(ByteReader& reader, std::int64_t& out) noexcept;
[[nodiscard]] bool get_i32(ByteReader& reader, std::int32_t& out) noexcept;
[[nodiscard]] bool get_string(ByteReader& reader, std::string& out);

}  // namespace bytes

// --------------------------------------------------------------------------
// the envelope
// --------------------------------------------------------------------------

/// `"ISAV"`, little-endian.
inline constexpr std::uint32_t kSaveMagic = 0x56415349u;

/// The framing: header plus a sequence of named sections. See the file header
/// for how this differs from `kStateVectorVersion`.
inline constexpr std::uint32_t kSaveFormatVersion = 1;

/// **The meaning of the bytes.** Bump on any change to any section's contents.
///
/// 2: the world section grew the `ObjList` pool, and every one of the ten
/// systems grew a section of its own. A version-1 save described a strictly
/// smaller game and there is no honest way to widen one, so it is refused.
///
/// 4: the hero section grew `army_attacked_at` and `army_attacked_unit` -- when
/// a hero's army was last struck, and which member -- which
/// `Hero::TimePastLastAttack` reads.
///
/// 5: the selection section lost the per-player `WasSelectionAssigned` latch --
/// that entry point turned out to read an environment key and never this table
/// -- and gained the `_LastSelectionTime` stamps.
///
/// 8: the world section grew the teleport pair and its per-player traversal
/// mask (`kWorldVersion` 23). Both are per-object and both are hashed, so a
/// version-7 file describes a world this build cannot reconstruct.
/// 12: the hero section grew the party's requested and settled final
/// orientations, which `Hero::SetFinalPartyOrientation` writes and
/// `GetFinalPartyOrientation` reads. Both are hashed, so a version-11 file
/// describes a world this build would hash differently.
/// 13: the AI section grew the per-player GAIKA view -- a slot map and a
/// priority-ranked record per node, which `SetPriority` reorders and
/// `LAIKA(player, i)` reads by rank. `AiSystem::hash` is a no-op, so a version
/// -12 file would load and then answer differently rather than fail a hash.
/// 14: the world section's object flag word grew the `ondie` latch
/// (`kWorldVersion` 24) -- whether a class's death hook has already run for
/// that object. It is hashed, so a version-13 file describes a world this
/// build would hash differently, and it decides a research payout, so loading
/// one as if the bit were clear would pay it twice.
/// 15: the script section's per-coroutine `Execution` payload lost
/// `chunk_index`. It was an index into the writing session's chunk library and
/// the reading session overwrote it from the record's source name before
/// anything could read it -- so the only thing it ever did was make a save's
/// bytes depend on the order that session happened to compile in, which is
/// what four of the shipped conquest's seven maps failed the byte-identical
/// re-save check on.
/// 16: the combat section's per-combatant block grew `Unit::AddBonus`'s five
/// addends. They are not hashed -- no modifier on a combatant is, and the
/// original recomputes the fields they feed rather than carrying them -- so a
/// version-15 file would load, hash the same, and then answer `.attack`,
/// `.maxhealth` and `.maxstamina` at the class numbers for every unit a script
/// had put a bonus on. Nothing but the section bytes catches that.
/// 17: the save grew the `session` section -- `GameSession`'s own bookkeeping,
/// which is one number: the object-id watermark below which every object has
/// been offered its class's `idle` script. It is not hashed and no system owns
/// it, and a load that did not carry it left the restored session's watermark
/// where `start_session` had put it -- the map's own `next_id()` -- so the
/// first turn after the load offered `idle` to every object spawned since the
/// match began. A trained unit walking into its town under an `enter` command
/// got a second `UNIT_IDLE.VS`, whose `Stop(1000)` dropped the route the
/// original was still walking: the save sweep's Balcans and Crossroads
/// divergence, four turns after the load.
/// 18: the match section's player rows grew the end-of-match report's eight
/// counters -- food spent, units produced, killed, lost and most at once,
/// gold captured, produced and converted (`PlayerScoreCounters`). Not
/// hashed: nothing a script reaches reads them, so a load that dropped them
/// would report zeros on the Statistics screen and diverge nowhere else.
/// 19: the movement section grew unit-versus-unit avoidance -- per `MoveState`
/// the route's stride, the hold, the step being walked with its two sidestep
/// offsets, `CVXPathCoop`'s wait and the march it belongs to; and the section
/// closes with the ownerless locks and the flag that says they were made. None
/// of it is hashed (it is path media, the `pathfinder` channel), so a
/// version-18 file would load and hash the same and then release every held
/// unit at once and forget every building's doorway locks.
/// 20: the session section grew the AI's nodes (session layout 4). A node
/// keeps the centre its town hall had when the table was built; a version-19
/// file has only the rebuild, which puts a razed town's node at (-1,-1) and
/// files every squad near it under another node -- not hashed, and it
/// decides where the AI marches.
/// 21: a settlement keeps eight timers, not ten. `sentries` and `supply` were
/// the economy's copies of `TOWNHALL_SENTRIES_CONTROL.VS` and
/// `VILLAGE_BEHAVIOR_SUPPLY.VS`, retired now that class behaviours start and
/// those two scripts run themselves; the timer array is written without a
/// count, so a version-20 file would misread every settlement after the first.
/// 22: a script value that is an object carries a second word (`ObjectRef::
/// aux`), because a VS point is two 32-bit integers and no longer two sixteen-
/// bit halves of one; the script section's values are four bytes longer, so a
/// version-21 file would misread every coroutine after its first object value.
/// 23: the movement section's `MoveState` grew the order's lock flag
/// (`CVXPathRetry` flag bit 0), which makes the end of the unit's route its
/// owned destination lock (playtest report #13). The original keeps a lock as
/// an object, and its save carries it as it carries the ownerless ones. Not
/// hashed -- path media -- so a version-22 file would load, hash the same, and
/// then let a unit walk onto a spot another unit's route had reserved.
/// 24: the world section carries a gate's portcullis motion (world version
/// 33) and the object flag word grew `gate_passable`: a gate lets units
/// through once its portcullis passes 20 (playtest report #16), so where it
/// stands is state, and a version-23 file has a gate at its target with no
/// barrier to say whether the grid held one.
/// 25: a gate bars a route per search rather than in the shared grid
/// (`sim/gate.hpp`), so the object flag word lost `gate_passable` (world
/// version 34) -- nothing is laid in the grid for it to describe -- and the
/// movement section's `MoveState` grew the route's gate crossings
/// (0x00418fb0's list), which the step reads to stop before a gate that bars
/// the mover. Not hashed -- path media -- and a version-24 file would load a
/// route crossing a closed gate with no crossing to stop it there.
/// 26: the movement section's `MoveState` grew the free-spot search's two
/// `CVXPathRetry` flags, bits 3 and 6 (playtest report #13): whether the
/// search has run for this destination and whether the route walked was
/// re-aimed by it. Not hashed -- path media -- but the search draws from the
/// world's generator, so a version-25 file would load with both clear and
/// let a unit draw again where the original would not.
/// 27: the movement section's `MoveState` carries `Goto`'s failure stamp
/// (`[unit+0x150]`, `goto_failed_at`) in the slot that held the time the
/// order began, which nothing reads any more: `give_up` times the failure,
/// not the walk (`GotoOrder::give_up`). Same width, new meaning, so a
/// version-26 file would load every walking unit as failing since its order
/// began -- not hashed, and it would end scripts early and draw from the
/// world's generator where the original does not.
/// 28: the movement section's `MoveState` grew `Unit::Stop`'s request
/// (`CVXPathRetry` flag bit 1, 0x004178b0) and the formation's lock flag a
/// march's hero carries (`[form+0x8c]`). Not hashed -- path media -- and a
/// version-27 file would load a unit told to stop as one walking on to its
/// destination.
/// 29: the squad section carries the AI's order queues -- per player the
/// records `Squad::SendTo` posts, the free chain, the drain's timer and its
/// running script -- and each squad's index into its player's (`[squad+0x26]`).
/// Hashed, so a version-28 file describes a world this build would hash
/// differently; and a load without them would carry out no order posted
/// before the save, and drain the rest on another turn.
/// 30: the campaign section carries the thread each running sequence waits
/// on (`SequenceEntry::running`), by name. A version-29 file would load every
/// sequence running at the save with the fresh session's thread ids, so its
/// end would never mark it `"Finished"` -- or a stale id would mark it so at
/// once -- and `RunSequence` would refuse or restart it on that reading.
/// 31: the movement section carries a march's formation radius
/// (`MoveState::form_extent`, `[form+0x34]`), which decides how far short of a
/// gate the march stands. Hashed, so a version-30 file describes a world this
/// build would hash differently.
/// 32: the movement section carries the pace a formation march gives the hero
/// and each marching member (`MoveState::march_rate`, 0x00419e52). Not hashed
/// -- path media -- and a version-31 file would load a marching army walking
/// at its own speeds, the hero out ahead of it, until his next placement.
/// 33: a settlement's loyalty is `gbr.exe`'s pair -- the loyalty in tenths
/// and the change pending for the next cap tick -- where the same two words
/// held the loyalty in points and what the window had lost, and every
/// settlement keeps one more timer (`LoyaltyInterval`'s gain tick). Hashed,
/// and a version-32 file would load a garrisoned town at hundreds of
/// tenths' worth of points over the 100 the original allows.
/// 34: two query fields change meaning. A set-op query's `op` is `gbr.exe`'s
/// ordinal (0 union, 1 intersect, where 0 was intersect), and a sight query
/// carries its radius (`QuerySpec::radius`, `sight - 100` frozen at
/// construction, where it was 0 and the live sight was read) -- and
/// `ObjsInRange` is a sight query, where it was an anchored circle. A
/// version-33 file would load every union as an intersection and every sight
/// query as one that sees only its own spot.
/// 35: the script section (its own version 2) carries each script's arming
/// order (`ScriptRecord::armed`) and `StartPlayerScript`'s slots. A
/// version-34 file has neither: scripts due on the same millisecond would run
/// in id order rather than the order they went to sleep, and a player script
/// already running could be started a second time.
/// 36: a settlement carries the sentries put back into its wall
/// (`Settlement::sentries_put_back`, `[settlement+0x100]`), which `DelSentry`
/// spends before the roster. Hashed; a version-35 file would load every wall
/// with none banked, and the next sentry to die would come off the roster.
/// 37: a unit's `experience` (`UnitRecord`, the hero section) is `gbr.exe`'s
/// running total at `[unit+0x180]`, and the inherent level is derived from it
/// (0x005d28c0) rather than kept beside it. A version-36 file holds progress
/// within the level and the level as a second word; read as a total, every
/// veteran would load at level 1.
/// 38: a squad carries its `Enemies` list (`Squad::enemies`, what its look
/// last saw, `[squad+0x64]`) and when its look next fires (`look_due`). Not
/// hashed; a version-37 file would load every squad blind and never looking,
/// and its members would find no target until a new member joined.
/// 39: a combatant carries its stored stats (`Combatant::stats`: maximum
/// health and stamina, range, attack and both armours, `[obj+0xc8..0xe8]`),
/// which the rebuild `recalc_bonuses` (0x005d9df0) writes, in place of the
/// three modifier words `attack_bonus`, `armour_bonus` and `level_floor`
/// nothing wrote. A version-38 file has no stats to load, and every unit
/// would fight with zero attack until something rebuilt it. The combatant
/// also loses its own `experience` word: the running total is the hero
/// section's `UnitRecord::experience` alone, and `Combatant::level` is the
/// stored level the rebuild writes. A hero record carries its army addend
/// (`HeroRecord::army_addend`, `[hero+0x2c0]`).
/// 40: the world section carries each query's cache after the query specs
/// (`World::QueryCache`: the list its last refresh made and when the next one
/// falls due, `[query+0x4c]`). A version-39 file has neither, and a query
/// is no longer evaluated on every read: docs/formats/query-model.md.
/// 41: the economy section grew the wagons' loyalty rows -- a wagon's
/// loyalty, pending change, capture flag and its two capture timers
/// (`WagonLoyalty`), which `Wagon::loyalty`, `SetLoyalty` and
/// `DecreaseLoyalty` read and write -- and lost its shipment table: a
/// settlement's mule and a squad's food wagon are wagon objects in the world
/// section now, not rows with a build timer. Hashed, so a version-40 file
/// would load a caravan mid-capture as one nobody had touched, and its mules
/// in transit as loads nobody carried.
/// 42: siege fire is the scripts' (`docs/formats/siege-fire.md`). Every
/// object carries a catapult's own target (`ObjectState::catapult_target`,
/// `[cat+0x228]`) and two more flag bits, `towers_posted` and `has_towers`;
/// a version-41 file would load every engine with no target and every
/// outpost and Roman tower still owed its fake towers, which would be
/// placed a second time.
/// 43: every script thread carries its context string
/// (`script::ScriptRecord::context`, the scheduler section's version 3): `/` on
/// a sequence's, the seat on an AI's. A version-42 file would load every
/// thread with none, and a relative root `Env*` key in a sequence would stop
/// meaning the absolute one. The same round keeps each AI seat's `AIRun` ids
/// and settlement slots on its row (system section version 37), and a
/// retrying call's word of state (`Execution::retry_scratch`, the coroutine
/// payload's version 5), which the `Goto` family's re-entry answers on.
/// 44: the movement section carries each route's failed-search bit
/// (`MoveState::route_failed`, `[retry+0x20] & 4`; system section version
/// 38). A version-43 file would load a unit walking on from a failed search
/// as one walking to the end of its old route, and one stopped after it as
/// arrived when it stands in its band.
/// 45: the squad section carries each GAIKA node's squad deque after the
/// order queues (`SquadTable::filed_under`, `[node+0x18]`), and then the
/// node manager's MAIKA list (`SquadTable::routes`, `+0x38`; system section
/// version 39). A version-44 file has none, and every walk of a node's
/// squads -- `GetSquads`, `Count`, `Eval`, `GetAIControlledUnits` -- reads
/// the deque in the order its squads arrived: docs/formats/gaika.md.
/// 46: the movement section carries every hero's formation object
/// (`sim/formation.hpp`, `[hero+0x2b0]`: its slots, pending units, marks,
/// sample and flags; system section version 40), and the movement record no
/// longer carries the shape, the spacing, the lock flag or the radius that
/// stood in for it. A version-45 file has no slots, so a loaded march would
/// stand until its hero laid the formation out again.
/// 47: the world section (its version 36) carries each object's grid arrival
/// stamp (`SpatialSlot::arrival`) and the next one: the order of a grid
/// cell's list, which is who came into the cell first and the order every
/// area sweep answers in (`sim/spatial_index.hpp`). Not hashed. A version-46
/// file has no stamps, and a load would list every cell in id order.
/// 48: every combatant carries the danger counter the target searches raise
/// and read (`Combatant::danger`, `[unit+0x1b0]`, and its stamp
/// `danger_time`, `[unit+0x1b4]`; system section version 41,
/// `docs/formats/target-score.md`). A version-47 file would load every unit
/// as nobody's recent pick, and the next searches would bunch on it.
/// 49: an object's `idle` is its command queue's head from its start
/// (`CommandSystem::start_slot`, 0x005aec40 -> 0x005b5160 -> 0x005b4f20),
/// not a script running beside an empty queue, and a command's `onfinish`
/// runs inside the end that calls it, so the command section keeps no
/// finishing commands (system section version 42). A version-48 file would
/// load every resting object with an empty queue and its `idle` running
/// outside it, which nothing in this build ends.
/// 50: the movement section carries the smart pathfinder's build -- whether
/// `0x004165a0`'s builder was made and every `SPF*` call made on it since
/// (`PathFinder::smart_calls`; system section version 43) -- and a load makes
/// the structure again from the saved map's grid by those calls. Not hashed.
/// A version-49 file has no build, so a loaded match would route with the
/// grid search for good while its `pathfinderinit.vs` thread went on calling
/// commands on a builder that was never made.
/// 51: the movement section carries each formation's own route -- its points,
/// the stepper's counters and what is left, its goal and band, whether it
/// goes on, the sample's deadline and speed, and the units still to be handed
/// `form_move` -- and every route aimed at a formation says so with the
/// sample it was laid to and whether its search made a lock (system section
/// version 44). A version-50 file has
/// no formation route, and its marches would stand still at their last
/// sample.
/// 52: the object's flag word carries `force_idle` (`[unit+0x1b8]`, which
/// `Unit::ForceIdle` and `Ship::Idle` raise and the idle activity's first
/// draw spends, docs/formats/idle.md) and `ai_registered` (the unit counted
/// in its owner's class tally, docs/formats/class-tally.md), and the hero
/// section carries the sixteen class tallies `Count` reads (system section
/// version 45). A version-51 file has no tally, and every `Count` after a
/// load would answer 0 until the units it counts were born again.
/// 53: the object's flag word carries a sacrifice's stop flag
/// (`sacrifice_stopped`, `[sacrifice+0x15c]`, docs/formats/sacrifice.md),
/// and the hero section carries `Place`'s deferred-filing list where the
/// squad watermark was (system section version 46): every other birth
/// registers and files its unit with the AI at once. A version-52 file has a
/// watermark there.
/// 54: the hero section loses `Place`'s deferred-filing list (system section
/// version 47): the move hook (0x0041f530) registers and files a unit at the
/// move, `Place`'s included, so nothing waits for a sweep. A version-53 file
/// has the list between the item store and the tallies.
/// 55: the movement section carries each formation's blocking gate
/// (`[form+0x90]`, after `going_on`) and each route's gate it has compared
/// sides at (`[retry+0x18]`, after `form_lock_laid`), system section version
/// 48 (docs/formats/formation.md, "The blocking gate").
inline constexpr std::uint32_t kStateVectorVersion = 55;

/// The section the world writes.
inline constexpr std::string_view kWorldSection = "world";
/// The section `script::Scheduler` writes.
inline constexpr std::string_view kScriptSection = "script";
/// The section `SelectionTable` writes.
inline constexpr std::string_view kSelectionSection = "selection";
/// The section `GameSession` writes for itself: the bookkeeping that belongs to
/// neither the world nor any system. `read_save` leaves it to the session, as
/// it leaves the system sections to their owners.
inline constexpr std::string_view kSessionSection = "session";
/// Prefix for a system's own section: `system:movement`, `system:combat`, ...
inline constexpr std::string_view kSystemSectionPrefix = "system:";

/// `kSystemSectionPrefix` + `name`. The one place the spelling lives.
[[nodiscard]] std::string system_section_name(std::string_view name);

/// A section is at most this many bytes. Not a format limit -- the length is a
/// u32 -- but a sanity bound so that a corrupt length cannot ask for a
/// gigabyte-sized reserve before the read that would have failed.
inline constexpr std::uint32_t kMaxSectionBytes = 0x2000'0000u;  // 512 MiB

/// One named blob in the save.
///
/// `name` is ASCII, at most 255 bytes, and unique within a save. Order is the
/// order they were added, and it is preserved byte for byte on rewrite.
struct SaveSection {
  std::string name;
  std::vector<std::byte> payload;
};

/// What the save says about itself.
///
/// Every field here is checkable at load time, and every one of them exists
/// because the original checked the same thing: `gbr.exe` records the build's
/// savegame version, the map the game was saved on, and a `GameHashes` group
/// whose channel names are the ones in `WorldHashes`.
struct SaveMeta {
  /// The state-vector version the save was written under. Compared against
  /// `kStateVectorVersion` before anything is read.
  std::uint32_t state_vector = kStateVectorVersion;

  /// Which map this game is on, in whatever spelling the embedder uses --
  /// typically the container-relative path. Empty means "the writer did not
  /// say", which a loader may accept but cannot check.
  std::string map;

  /// `Clock::turns()` and `Clock::time()` at the moment of the save.
  /// Redundant with the world section and deliberately so: it lets a save
  /// browser list a save without decoding the object table.
  std::uint64_t turns = 0;
  GameTime time = 0;

  /// `World::hashes()` at the moment of the save. The four channels that are
  /// zero in all nine retail dumps are **not stored** -- encoding a field whose
  /// only legal value is zero invites somebody to change it -- and
  /// `hash_of_hashes` is not stored either, being a pure function of the rest.
  /// `verify_hashes` recomputes both.
  std::uint64_t slots = 0;
  std::uint64_t threads = 0;
  std::uint64_t netcmds = 0;
  std::uint64_t extrahash = 0;

  /// The system run order at save time, from `World::systems()`.
  ///
  /// Stored for the same reason `conformance::Trace` stores it: run order is
  /// folded into `slots` before any system's own contribution, so a pipeline
  /// that has changed explains every hash difference under it. A load into a
  /// differently-ordered pipeline is refused by name rather than diagnosed as a
  /// desync twenty turns later.
  std::vector<std::string> systems;
};

// --------------------------------------------------------------------------
// writing
// --------------------------------------------------------------------------

/// Builds a save, section by section, in the order sections are added.
///
/// Deterministic by construction: no map, no set, no length-dependent
/// ordering. Two worlds in identical states produce byte-identical saves, which
/// is what `test_save.cpp` asserts and what makes a save diffable.
class SaveWriter {
 public:
  /// Start a save. The meta section is written first, always, so that a reader
  /// can reject a save on version or map before decoding anything else.
  explicit SaveWriter(SaveMeta meta) : meta_(std::move(meta)) {}

  /// Append a section. Refuses an empty name, a name over 255 bytes, a
  /// duplicate name, a name containing a byte outside printable ASCII, and a
  /// payload over `kMaxSectionBytes`.
  Status add(std::string_view name, std::span<const std::byte> payload);

  /// The finished bytes. Calling it twice produces the same bytes.
  [[nodiscard]] std::vector<std::byte> finish() const;

  [[nodiscard]] const SaveMeta& meta() const noexcept { return meta_; }
  [[nodiscard]] std::span<const SaveSection> sections() const noexcept { return sections_; }

 private:
  SaveMeta meta_;
  std::vector<SaveSection> sections_;
};

/// Everything a save needs beyond the world.
///
/// All optional. A save with no scheduler is a save of a game with no scripts
/// running, which is exactly what a systems-only conformance run is.
struct SaveInputs {
  const script::Scheduler* scheduler = nullptr;
  const SelectionTable* selections = nullptr;

  /// One entry per system that can serialise itself, produced by whoever owns
  /// the system. The names are bare system names; `write_save` applies
  /// `kSystemSectionPrefix`.
  ///
  /// **A system registered on the world with no entry here is not saved**, and
  /// `write_save` says so through `SaveReport::unsaved_systems` rather than
  /// quietly producing a save that will not load back. `GameSession::save`
  /// fills this for all eleven and refuses outright if the list comes back
  /// non-empty, so for that caller it is a wiring assertion.
  std::span<const SaveSection> systems;

  /// `GameSession`'s own section, already encoded; written as `kSessionSection`
  /// when non-empty. `write_save` does not look inside it.
  std::span<const std::byte> session;

  /// Map identity, for `SaveMeta::map`.
  std::string_view map;
};

/// What a save turned out to contain.
struct SaveReport {
  /// Registered systems for which `SaveInputs::systems` had no section. Every
  /// name here is state that the save does not carry.
  std::vector<std::string> unsaved_systems;
  std::size_t bytes = 0;
};

/// Write a save of `world` and the things around it.
///
/// Never fails on a well-formed world: the failure modes are all in
/// `SaveInputs` (a duplicate or malformed section name), and they are reported
/// rather than ignored.
Result<SaveReport> write_save(const World& world, const SaveInputs& inputs,
                              std::vector<std::byte>& out);

// --------------------------------------------------------------------------
// reading
// --------------------------------------------------------------------------

/// Why this build will not read a save written by another, said before
/// anything is touched: the envelope's framing version or its state vector is
/// not this build's (`SaveReader::open` refuses both as `unsupported`).
/// Names the version found and the one this build reads -- "written with
/// state vector 21; this build reads state vector 22" -- so a player holding
/// an old save knows it is the build and not the file. Empty when both
/// versions are this build's, or when the bytes are not a save at all (too
/// short, another magic), which `open` reports on its own.
[[nodiscard]] std::string save_version_refusal(std::span<const std::byte> bytes);

/// A parsed save: the meta block, and the sections by name.
///
/// Holds spans into the caller's bytes and copies nothing. The bytes must
/// outlive the reader.
class SaveReader {
 public:
  /// Parse the envelope. Checks the magic, both versions, the section framing,
  /// duplicate names, and that the sections account for every byte -- a save
  /// with trailing bytes is refused rather than half-read.
  [[nodiscard]] static Result<SaveReader> open(std::span<const std::byte> bytes);

  [[nodiscard]] const SaveMeta& meta() const noexcept { return meta_; }

  /// The section's payload, or an empty span if there is none. Use `has` to
  /// tell an absent section from an empty one.
  [[nodiscard]] std::span<const std::byte> section(std::string_view name) const noexcept;
  [[nodiscard]] bool has(std::string_view name) const noexcept;

  /// `section(system_section_name(name))`.
  [[nodiscard]] std::span<const std::byte> system_section(std::string_view name) const noexcept;

  /// Names in file order.
  [[nodiscard]] std::span<const std::string> names() const noexcept { return names_; }

  SaveReader() = default;

 private:
  SaveMeta meta_;
  std::vector<std::string> names_;
  std::vector<std::span<const std::byte>> payloads_;
};

/// How strict a load is.
struct LoadOptions {
  script::Scheduler* scheduler = nullptr;
  SelectionTable* selections = nullptr;

  /// Resolves entity art for the objects being restored. Null is fine and
  /// means every object comes back with no entity, exactly as a world
  /// populated with a null resolver does.
  EntityResolver* entities = nullptr;

  /// If non-empty, must equal `SaveMeta::map` or the load is refused. The
  /// original refuses the same way; see the file header.
  std::string_view map;

  /// Require the world's registered systems to be the same names in the same
  /// order as `SaveMeta::systems`. On by default: run order is folded into the
  /// hash, so a pipeline difference invalidates every check below it.
  bool require_same_pipeline = true;
};

/// What a load restored, and what it did not.
struct LoadReport {
  SaveMeta meta;
  /// Sections present in the file that this load did not consume. A system
  /// section for a system the world has not registered lands here; so does a
  /// section written by a newer build that added one.
  std::vector<std::string> unconsumed;
  /// Registered systems the save had no section for. These keep whatever state
  /// they had, which is *not* the saved state -- so a non-empty list means the
  /// hash check below will fail if that system contributes to the hash.
  std::vector<std::string> unrestored_systems;
  std::size_t objects = 0;
};

/// Restore `world`, the scheduler and the selections from `bytes`.
///
/// System sections are **not** applied here: this cannot reach inside a
/// `System`. The caller pulls each one out of the reader and hands it to the
/// system that owns it, then calls `verify_hashes` once everything is back.
///
/// `world` is fully replaced on success and left untouched on failure -- the
/// world section is decoded into a temporary and moved in only once every field
/// has been read, so a truncated save cannot leave half a game behind.
Result<LoadReport> read_save(std::span<const std::byte> bytes, World& world,
                             const LoadOptions& options);

/// The same, over an already-parsed reader, for a caller that wants to inspect
/// the meta block before committing to the load.
Result<LoadReport> read_save(const SaveReader& reader, World& world, const LoadOptions& options);

/// Compare `world.hashes()` against what the save recorded.
///
/// Call it **after** every system section has been applied. Returns
/// `FormatError::malformed` on a mismatch, and names the channel through
/// `channel`, which is the actionable half: `slots` means object or system
/// state, `threads` means the scheduler.
///
/// This is the property the whole file exists for. A save that passes this has
/// restored every piece of state that the simulation's own hash can see.
struct HashMismatch {
  bool ok = true;
  std::string_view channel;
  std::uint64_t expected = 0;
  std::uint64_t actual = 0;
};

[[nodiscard]] HashMismatch verify_hashes(const World& world, const SaveMeta& meta) noexcept;

}  // namespace imperivm::core::sim
