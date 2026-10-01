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
inline constexpr std::uint32_t kStateVectorVersion = 25;

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
