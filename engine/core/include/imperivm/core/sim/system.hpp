#pragma once

/// The seam Part 5's systems attach to, and the object state they share.
///
/// `World` owns the clock and the objects and deliberately stays small.
/// Movement, combat, economy, heroes and the rest are **systems**: separate
/// units of behaviour that the same turn loop drives, none of which requires
/// `World` to grow a field. This header defines that seam so the domains can be
/// built independently and still compose.
///
/// ## Everything here is world state
///
/// Imperivm is lockstep-deterministic, and the original's desync dumps record
/// exactly which fields it treats as synchronised. `docs/engine/state-vector.md`
/// specifies them per class, measured over nine dumps and 11,670 object blocks.
/// The fields below are the common header every object derived from `CVXDecor`
/// carries, reproduced faithfully rather than redesigned.
///
/// Three rules apply to every line of code that touches this state:
///
///   1. **No floating point.** Fixed point or integers only.
///   2. **One RNG**, explicitly seeded, serialised as world state. A system that
///      wants randomness draws from the world's generator; it never owns one.
///   3. **Iteration order is state.** Systems run in a fixed order, and each
///      iterates objects in a stable order. Never an unordered container.
///
/// ## What the original does *not* hash
///
/// Of the seven hash channels the dumps record, `pathfinder`, `exploration`,
/// `scriptstate` and `aihash` are zero in all nine. The shipped build kept
/// pathfinding and fog of war out of the determinism contract. That is a
/// constraint to respect rather than improve on: those subsystems must not feed
/// back into hashed state.

#include <cstdint>
#include <span>
#include <string_view>

#include "imperivm/core/game/registry.hpp"
#include "imperivm/core/sim/tick.hpp"

namespace imperivm::core::sim {

class World;

// `ObjectId`, `kNoObject`, `PlayerId` and `kNoPlayer` are **re-exported**, not
// redeclared. `game/registry.hpp` defines all four in `imperivm::core`, and
// they were separately declared here too -- same types, same values -- until a
// test `using namespace`d both namespaces and every mention of `kNoPlayer` in
// it became ambiguous between two identical constants. That is a latent form of
// the bug `sim/host_context.hpp` describes: harmless until two pieces meet.
//
// These `using` declarations name the same entities rather than new ones, so
// `sim::kNoObject` keeps working for the code that spells it that way and there
// is only ever one constant to be ambiguous about.
using core::kNoObject;
using core::kNoPlayer;
using core::ObjectId;
using core::PlayerId;

/// The original allocates 16-bit handles from a monotonic counter and never
/// reuses them, so a stale reference reads as dead rather than as somebody
/// else. It reached 21,754 after roughly 29 minutes of game time, which a long
/// game could plausibly exhaust; what it did on wrap is unknown. We use 32 bits
/// internally and expose a 16-bit view only where compatibility demands it.
[[nodiscard]] constexpr std::uint16_t compat_handle(ObjectId id) noexcept {
  return static_cast<std::uint16_t>(id);
}

/// A position in world units. Maps are 8,192 to 32,768 units square, and the
/// coordinate space is bounded by 16,383 on a standard map.
struct Point {
  std::int32_t x = 0;
  std::int32_t y = 0;

  friend constexpr bool operator==(const Point&, const Point&) = default;
};

/// The position of an object that is inside a holder.
///
/// The original stores `(-1, -1)` and keeps no coordinates for a garrisoned
/// object: its location *is* its holder's. Of 1,225 objects at `(-1, -1)` in the
/// dumps, 1,217 have a non-null holder, and of the 6,355 with a real position,
/// none does. Reproduce this rather than caching a shadow position, or the two
/// will disagree and the disagreement will be a desync.
inline constexpr Point kHeldPosition{-1, -1};

/// The two engine-reserved neutral players.
///
/// The dumps encode an object's owner as a one-hot field in the low 16 bits of
/// `SyncFlags`, and only eight values ever occur: unowned (`kNoPlayer`, from
/// `game/registry.hpp`), players 0 through 4, and these two -- 14 carries
/// wildlife and map garrisons, 15 a smaller passive set.
inline constexpr PlayerId kNeutralWildlife = 14;
inline constexpr PlayerId kNeutralPassive = 15;

/// Cached category bits, mirroring the high half of `SyncFlags`.
///
/// The original keeps these denormalised on the object rather than deriving
/// them from the class, and `is_unit` and `is_building` form an exact partition
/// with everything else. Bits observed but unexplained, and bits never observed
/// at all, are documented in `docs/engine/state-vector.md`; do not invent
/// meanings for them.
struct ObjectFlags {
  bool is_unit = false;
  bool is_building = false;
  bool is_hero = false;
  bool has_active_path = false;
  /// `SyncFlags` bit 21, and **the polarity is inverted**: the bit means
  /// *hidden*, so `Obj::IsVisible` is its negation. Named for the bit rather
  /// than for the script call so that packing stays a straight copy; the one
  /// place the inversion happens is `IsVisible`/`SetVisible`.
  bool hidden = false;
  /// `SyncFlags` bit 19 -- the object is a member of the **party**, the group
  /// of units that follows the hero across an adventure's map boundary.
  ///
  /// `Unit::GetParty` (`gbr.exe` `0x005d7b10`) is nothing but
  /// `(obj[0x2c] >> 0x13) & 1`, on the same word at `+0x2c` that
  /// `Obj::IsVisible` (`0x005ab970`) reads bit 21 of. See `sim/world.hpp`'s
  /// `kSyncParty` for the rest of the chain.
  bool in_party = false;
  /// `SyncFlags` bit 27 -- the object is a **spawn template**: placed by the
  /// map, holding a handle, and not in play. Every collection path skips it,
  /// so it is invisible to queries, to `ObjList`s and to the systems that
  /// build their work from either; `SpawnGroup` mints copies of it with this
  /// clear. See `kSyncUnspawned` in `sim/world.hpp` for the evidence.
  bool unspawned = false;
  /// `SetNoAIFlag` -- the unit is the script's, and the AI is to leave it
  /// alone.
  ///
  /// **Not on the `SyncFlags` word.** Every flag above is a bit of the object's
  /// `+0x2c`; this one is bit 18 (`0x00040000`) of a second word at `+0x194`,
  /// which `Unit::SetNoAIFlag` (0x005de600) and the free form (0x00435470)
  /// both write.
  ///
  /// **The map authors it and this paragraph used to deny that**, on both
  /// counts: it said no `<scriptobj>` can author the flag and that it starts
  /// false on every object. It is the map's `UnitFlags` attribute, it is set on
  /// 14,586 of the retail install's 16,171 units, and the bit is clear on every
  /// wild animal without exception. See `kUnitFlagNoAI` in `sim/world.hpp` for
  /// the reading and the counts.
  ///
  /// **And it is read now**, by `Unit::GetFlags`, which is the only entry point
  /// that asks and is asked about nothing else at any of its five shipped call
  /// sites. Its three readers in `gbr.exe` -- 0x0043ad2c, 0x00446c8e,
  /// 0x0044745c -- are still all inside the AI-helper machinery, which is a
  /// thread of its own; 230 call sites set it.
  bool no_ai = false;
  /// `Unit::IsDiseased` -- bit 1 (`0x2`) of the same second word at `+0x194`.
  /// `Unit::Disease` (0x005d9140) sets it and, when the unit carries no
  /// effect handle at `+0x1c4`, spawns a visual of class index 0x1a and
  /// stores that handle beside it; the visual is presentation and is not
  /// reproduced. Nothing in `.text` clears the bit through a script entry
  /// point: a diseased unit stays diseased. `ECHARIOT_ENGAGE.VS` reads it to
  /// decide whether a chariot still owes the target a disease shot, so it is
  /// simulation state and is saved and hashed with the rest of this word.
  bool diseased = false;
  /// `Obj::SetCmdEnable(false)` -- the object no longer takes a player's
  /// orders. `0x005ac710` writes the `bool` as a whole word at `[obj+0x9c]`
  /// (after refusing a dead receiver with its own message), and the one
  /// shipped writer is `SETTLEMENT_BEHAVIOR_AMBIENT.VS`, which dismisses a
  /// villager -- `AddCommand(false, "dismiss"); SetCmdEnable(false)` -- as it
  /// sends it home. Named for the clear so the default packs as 0, the same
  /// device `hidden` uses. **Hashed and saved**: what an object will do with a
  /// click is something two peers have to agree on.
  ///
  /// **The reader is inferred.** No script surface reads the word back, so
  /// the only place it can matter is where a player's order meets the object
  /// -- `is_commandable` in `sim/orders.hpp`, which refuses the object while
  /// this is set. That is the reading the name and the one writer support;
  /// the original's own consumer was not followed into the interface code.
  bool commands_disabled = false;
  /// Bit 28 (`0x10000000`) of the second flag word at `[obj+0x194]`, and it
  /// is named for its one reader: 0x005110c6 in the damage formula **halves
  /// the blow** landing on a unit that carries it, after every special
  /// ability has had its say. One writer: the cover of mercy's per-animation
  /// sweep, which raises it on everyone it shelters (`or [unit+0x194],
  /// 0x10000000` at 0x005ca3ba) and lowers it on those who stepped out.
  /// **Hashed and saved**: how hard a blow lands is something two peers have
  /// to agree on.
  ///
  /// This used to name `Unit::StartTraining` as a second writer, and that was
  /// wrong: the training pair goes through `vtbl+0x44` / `+0x48`, which are
  /// the `SyncFlags` setters `Unit::SetParty` uses on bit 19 of `+0x2c`, so
  /// their bit 28 is `training` below, on the other word.
  bool half_damage = false;
  /// `SyncFlags` bit 28 (`0x10000000`) of the word at `[obj+0x2c]`: the unit
  /// is training. `Unit::StartTraining` / `StopTraining` (0x005d5de0 /
  /// 0x005d5e30) set and clear it through the object's `vtbl+0x44` /
  /// `vtbl+0x48`, and `Unit::BestTrainingTarget`'s predicate (0x005d3d02) is
  /// its one reader: a training unit picks its sparring partner among the
  /// units that carry it. No map object carries it -- it is absent from the
  /// census in `docs/engine/state-vector.md` -- which is what makes it a
  /// runtime bit. **Hashed and saved**, and packed into the sync word the
  /// dumps show, where a training unit would show it.
  bool training = false;
  /// `Catapult::IsBuilt` -- a siege engine that has finished being assembled.
  ///
  /// **Not on the `SyncFlags` word either**, and not even on `no_ai`'s second
  /// word: it is the plain `int` at `catapult + 0x208`, which
  /// `Catapult::IsBuilt` (0x004e2dc0) tests `!= 0` and `Catapult::SetBuilt`
  /// (0x004e2e20) writes 1 to. The `CVXCatapult` constructor zeroes it at
  /// 0x004e1ede, so a catapult is born unbuilt and stays that way until
  /// something says otherwise.
  ///
  /// It is a flag here rather than an `int` because every reader in `gbr.exe`
  /// and every one of the three shipped call sites asks only whether it is
  /// non-zero, and because the only two writers write 0 and 1.
  ///
  /// **Hashed**, unlike `no_ai`. The nine desync dumps print no field for it --
  /// `CVXCatapult` prints exactly the `CVXBuilding` block -- but that is a
  /// statement about what the dumper writes, not about the determinism
  /// contract: `state_hash` is this engine's own check, and a flag that
  /// decides which half of `CATAPULT_IDLE.VS` runs is one two peers must agree
  /// on. The four channels the standing decision keeps out of the hash are
  /// pathfinder, exploration, scriptstate and aihash; this is none of them.
  bool built = false;
  /// `Flying::IsInAir` -- bit 22 of `[obj+0x194]`, the second flag word, not
  /// the `+0x2c` `SyncFlags` one. `0x0051bd20` is a shift and a mask and
  /// nothing else.
  ///
  /// **The map sets it on 46 units and this paragraph used to say nothing
  /// did.** Bit 22 of the map's `UnitFlags` occurs on 46 objects in the retail
  /// install and every one of them is a flying unit, so 46 birds start their
  /// map in the air and take `CROW_IDLE.VS`'s other branch. See
  /// `kUnitFlagInAir` in `sim/world.hpp`.
  ///
  /// Nothing here *changes* it after load: the bit is the flying-unit state
  /// machine's and this engine has no flight, so a bird that starts on the
  /// ground stays there and one that starts airborne stays up. Both branches
  /// of both crow scripts are safe under that -- they differ in a step length
  /// and an animation index, not in what they require of the engine.
  bool in_air = false;
  /// `Flying::IsLanding` -- `[obj+0x1d4]`, and its writer is the *only* thing
  /// that makes `in_air` mean anything.
  ///
  /// `0x0051bc2b` is `[obj+0x1d4] = (z == -1)` on every `Flying::PlayAnim`, so
  /// it means *this animation is a descent*: the crow's `PlayAnim(17, ptLand,
  /// -1)` and `PlayAnim(18, .pos, -1)` are the two calls that set it and every
  /// other flying animation clears it. `Flying::IsLanding` (0x0051bd80) is
  /// `in_air && landing`, which is a bird that is still up but on its way
  /// down, and `PickLandingPoint` uses exactly that disjunction to decide
  /// whether a flock-mate is somewhere it makes sense to land beside.
  ///
  /// Hashed and saved with the rest of the word: it is half of a two-bit state
  /// machine that decides which branch of `CROW_IDLE.VS` a peer runs.
  bool landing = false;
  /// `Unit::IsCursed` -- bit 27 of `[obj+0x194]`, the same second flag word
  /// `no_ai`, `in_air`, `noselect`, `autocast` and `landing` live on.
  ///
  /// **Written here on the strength of the script pair, not of a writer in
  /// `gbr.exe`.** `SHAMAN_IDLE.VS` is the whole corpus for both entry points
  /// and it round-trips them itself -- `if (tgt.IsCursed) break;` guards the
  /// `tgt.Curse();` eleven lines later, so a shaman that could not read its own
  /// write would curse the same target forever. That behaviour is what is
  /// reproduced.
  ///
  /// What is *not* established is where the original sets it. `Unit::Curse`
  /// (0x005d90c0) hands off to 0x005d4880, the spell machinery -- an SEH frame,
  /// the global spell tables, a dozen callees -- and a byte-pattern sweep for
  /// writes to bit 27 of `[reg+0x194]` finds none anywhere in `.text`: three
  /// readers (`IsCursed`, 0x005107d1, and 0x005d477d, which is a
  /// *already-cursed, skip* guard inside the curse path itself) and no `or`.
  /// So either the write is an encoding the sweep missed or the original has
  /// the same gap this engine's `in_air` had. Recorded rather than resolved.
  ///
  /// Whatever else 0x005d4880 does -- a stat penalty, a timer, an animation --
  /// is not recovered and none of it is invented here. `Curse` sets this bit
  /// and nothing more.
  bool cursed = false;
  /// `Unit::SetNoselectFlag` -- bit 23 of `[obj+0x194]`. **The same mask as the
  /// Building bit, on the other word**, which is worth saying out loud once.
  ///
  /// Write-only from script: an exhaustive search of the 16 places `gbr.exe`
  /// tests this bit finds them all inside the selection, minimap and interface
  /// code and none of them registered as a host function. Every one of the six
  /// shipped call sites passes `true`, on ambient props -- hens, fish, the
  /// settlement's wandering villagers -- always beside `SetMinimapFlag(true)`.
  bool noselect = false;
  /// `Ship::IsBuilding` -- a ship still under construction at its shipyard.
  ///
  /// The original stores a **count** at `[ship+0x230]` and `0x005c6fc0` tests
  /// it `> 0`; a bool is the whole of what the one reader can see, and the
  /// count's other users are the shipyard's, which this engine does not have.
  /// So nothing sets it, `SHIP_IDLE.VS`'s `while (.IsBuilding()) Sleep(100)`
  /// falls through on its first test, and a ship that a map placed is a ship
  /// that is finished -- which is what all three shipped ships are.
  bool building = false;
  /// `Hero::SetAutocast` -- bit 2 of `[hero+0x194]`, the same word `no_ai`,
  /// `in_air` and `noselect` live on.
  ///
  /// Whether a hero casts its spells without being told. 34 shipped call sites,
  /// **every one of them inside a map container** and 29 of the 34 passing
  /// `true`: it is how a mission turns a scripted hero loose. The five `false`
  /// sites are one block in `5_Great_Battles_Britain` map 3, where a hero
  /// changes sides and is quietened first.
  ///
  /// Read as well as written, which is why the pair is bound together:
  /// `Hero::autocast` (0x0052f740) is the same bit shifted down by two.
  /// **Nothing in this engine acts on it yet** -- there is no autocast in the
  /// AI -- so it is state a script can set and read back, which is what the
  /// 34 sites need and no more than the evidence supports.
  bool autocast = false;
  /// `Unit::SetMinimapFlag` -- bit 26 of `[obj+0x194]`, forcing the object onto
  /// the minimap. `SetNoselectFlag`'s neighbour in every sense: the same word,
  /// the same body shape, and every one of its eighty shipped calls sits on the
  /// line after one. Both are on ambient decoration -- hens, fish, the
  /// settlement's wandering villagers -- which should show on the minimap and
  /// not be clickable. The bit's four readers in `gbr.exe` are all in the
  /// minimap and interface code; nothing here draws one yet.
  bool on_minimap = false;

  // -- the gate's three ------------------------------------------------
  //
  // `[gate+0x210]`, `[gate+0x214]` and `[gate+0x208]` in `gbr.exe`. Flags
  // rather than a system of their own because the first two are **booleans in
  // the original too** -- `0x00529510` writes the literal 1 rather than
  // counting -- and the third is a target state with two values. Two gate
  // fields are deliberately not here: see `Gate::LookAround` in
  // `sim/world_host.cpp`.

  /// `Gate::AreEnemiesAround`. Set by `LookAround` and by nothing else, which
  /// is what makes the pair meaningless without it -- and why `GATE_IDLE.VS`
  /// calls `LookAround(350)` as the first statement of its loop body.
  bool enemies_near = false;
  /// `Gate::AreFriendsAround`.
  bool friends_near = false;
  /// The gate's target state: raised or lowered. `OpenNow` and `CloseNow`
  /// write it and nothing else does. Whether the gate lets units through is
  /// not a flag: it is where the portcullis stands, `gate_lets_through` over
  /// the motion (`sim/gate.hpp`).
  bool gate_open = false;

  /// `Unit::SetEntering` -- bit 20 of `[obj+0x194]`, the second flag word.
  ///
  /// A unit on its way *into* something. `0x005d8240` is a read-modify-write of
  /// that one bit and nothing else: `and 0xffefffff`, then `or 0x100000` when
  /// the argument is true.
  ///
  /// **Write-only from script, and bound anyway**, which is `SetNoselectFlag`'s
  /// case exactly: 21 shipped sites, no registered getter, and no reader
  /// anywhere in `gbr.exe`'s host table. The corpus says what it is for --
  /// `UNIT_MOVE_ENTER.VS` sets it true before its `GotoEnter` and false after,
  /// and the six unit-type attack scripts clear it on the way past -- so it is
  /// the flag that keeps a second command from re-targeting a unit that is
  /// already walking through a door.
  ///
  /// Hashed and saved with the rest of the word, for the reason every other
  /// script-written flag here is: two peers that disagree about it would have
  /// to disagree about something eventually, and the bit costs nothing.
  bool entering = false;

  /// `Druid::SetSummoningDeath` -- this death is a **transformation, not a
  /// casualty**.
  ///
  /// `0x005135a0` writes `[obj+0x1d4]` and has **no guard and no message at
  /// all**: an unresolvable receiver silently does nothing, which is unusual
  /// enough among the flag setters to be worth saying.
  ///
  /// The two summoning scripts are the whole corpus for it and they are one
  /// idiom: `u = Place("EagleSummoned", .posRH, .player); u.SetStamina(10);
  /// SwapSelectedObj(this, u); .SetSummoningDeath(true); .Damage(.health);` --
  /// the druid marks itself and then kills itself, and the eagle it just placed
  /// is what walks away. Without the flag that is a dead druid on the
  /// casualty list and a death notice on the screen.
  ///
  /// `IsSummoningDeath` reads it back, which is what makes it state rather than
  /// a message: two of the shipped scripts ask.
  bool summoning_death = false;

  /// `Obj::SetMessengerStatus` -- bit 26 of the **`+0x2c` word**, not the
  /// `+0x194` one the four flags above live on.
  ///
  /// A unit walking a scripted errand. Twenty-six shipped call sites and all but
  /// three are campaign cutscene sequences setting it `true` and `false` around
  /// a walk -- `NO_Scipio.SetMessengerStatus(true); ... (false);` -- with the
  /// other three on `SETTLEMENT_BEHAVIOR_AMBIENT.VS`'s wandering villagers.
  ///
  /// **It is read by the druids, which is what it is for.** The bit is a clause
  /// in three of the caster search predicates -- heal, cripple and (misapplied
  /// to the caster) curse -- so a messenger is not healed, not crippled and not
  /// revitalised while it is on its errand.
  ///
  /// The original also ORs it into the spatial grid's per-cell aggregate so a
  /// whole cell can be rejected before its objects are walked. This engine has
  /// no such aggregate -- `World::collect` walks objects -- so there is nothing
  /// to keep in step.
  bool messenger = false;

  /// The `ondie` hook has been fired for this object. `[obj+0xbc]` in the
  /// original, and **not** on either flag word: it is a plain dword the shared
  /// death-and-erase routine `0x005141b0` guards its entry on and writes 1 to
  /// on the way out (`0x0051446b`), so that routine runs at most once per
  /// object however many of its seven callers reach it.
  ///
  /// Here it governs exactly one thing, which is the one thing that routine
  /// does that this engine reproduces: the class's `ondie` script. Without it
  /// a unit killed in combat and then `Erase`d by a script during its death
  /// animation would run the hook twice, and `MILITARY_ONDIE.VS` would pay
  /// Warrior Tales twice for one casualty.
  ///
  /// **Hashed and saved.** The nine dumps print no field for it, which is
  /// `built`'s case exactly and is a statement about what the dumper writes
  /// rather than about the determinism contract. What makes it worth carrying
  /// is the window: the original sets the latch as the object is going away,
  /// while this engine leaves a corpse standing for its death animation
  /// first (`CombatSystem::death_duration_of`), and **fifteen objects sit in
  /// the dying state across those same dumps** -- so a save can be taken with it
  /// set, and two peers that disagreed about whether the payout had already
  /// happened would pay it a different number of times.
  ///
  /// Set for every object that reaches the routine, bound `ondie` or not: the
  /// write is at the tail and is not conditional on the launch. See
  /// `sim/hooks.hpp`.
  bool ondie_fired = false;
};

/// The common header every simulated object carries.
///
/// Deliberately the fields the original prints, in the original's terms. A
/// field we cannot explain is absent rather than guessed: `Anim.x`/`Anim.y`
/// look like a step vector but static objects hold `(0, 1)` rather than
/// `(0, 0)` and garrisoned ones hold map-coordinate garbage, so the spec marks
/// them unknown and so do we.
struct ObjectState {
  Point position;
  PlayerId owner = kNoPlayer;
  ObjectFlags flags;

  /// Null unless the object is inside something, in which case `position` is
  /// `kHeldPosition`.
  ObjectId holder = kNoObject;

  std::int32_t health = 0;
  std::int32_t stamina = 0;

  /// `Unit::user` -- one int per object that belongs entirely to the scripts.
  ///
  /// `0x005d8bf0` reads `[obj+0x164]` and `0x005d7c80` writes it, and that is
  /// the whole of both bodies: no clamp, no range, no side effect, and nothing
  /// in the engine reads the field for itself. It is a scratch slot the shipped
  /// content uses for whatever it likes -- `CROW_IDLE.VS` sets it to 1 to mean
  /// *this crow has decided to land*, and the three Britain ship sequences use
  /// it as a per-transport loaded flag they then sum.
  ///
  /// **Hashed and saved regardless.** A field only scripts touch is still a
  /// field two peers must agree on: `if (ol[i].AsFlying().user == 1)` steers a
  /// whole flock, and a save that dropped it would reload a crow that had
  /// forgotten it was landing.
  std::int32_t user = 0;

  /// `Flying::z` -- the altitude an animation is interpolating away from, and
  /// the one it is interpolating towards. `[obj+0x1cc]` and `[obj+0x1d0]`.
  ///
  /// **A bird has no stored altitude.** `0x0051b040` computes one on demand
  /// from these two and the animation clock: on the ground it is the terrain
  /// height under the object, mid-animation it is
  /// `z_from + (z_to - z_from) * elapsed / cycle`, and outside an animation it
  /// is `z_from`. Both are written by `Flying::PlayAnim` (0x0051bbf4 and
  /// 0x0051bc0e) -- `z_from` from `Flying::z` itself, so each animation starts
  /// where the last one left off, and `z_to` from the call's own `z` argument
  /// or, when that argument is `-1`, from the terrain height at the
  /// destination.
  ///
  /// The unit is the **height layer's byte**, not world units: the argument is
  /// range-checked to `-1 .. 1024` and every shipped site builds it out of
  /// `GetTerrainHeight`, which answers 0..255. `docs/formats/map.md` records
  /// that the step size from those 256 levels to world units is a projection
  /// question; nothing in the simulation has to answer it, because the whole
  /// of this arithmetic stays inside the layer's own scale.
  ///
  /// Hashed and saved for the reason `user` above is: `CROW_IDLE.VS` reads
  /// `z = .z` and branches on it every iteration of its flight loop, so two
  /// peers that disagree about a bird's altitude fly it to two different
  /// places.
  std::int32_t z_from = 0;
  std::int32_t z_to = 0;

  /// `Wagon::amount` and `Wagon::restype` -- `[obj+0x1cc]` and `[obj+0x1d4]`,
  /// a mule's cargo and what kind it is.
  ///
  /// **On the object, not on a trade record.** `Wagon::amount` (0x005ebdb0) and
  /// `Wagon::restype` (0x005ebe10) are plain reads of those two fields, and
  /// `Wagon::LoadGold`/`LoadFood` (0x005ebfd0, 0x005ebf10) are the only writers
  /// a script has. `EconomySystem::Wagon` is a different thing -- a settlement
  /// -to-settlement shipment with a build timer and no world object -- and the
  /// two must not be confused: a mule the AI walks around with is this, and
  /// `ES_OUTPOSTSELLGOLD.VS` sums `AsWagon(o).amount` over a *group of objects*
  /// to find out how much gold is on the road.
  ///
  /// `cargo_resource` uses `Resource`'s own numbering, and that is measured
  /// rather than assumed: `LoadFood` writes the literal 1 into `[+0x1d4]` and
  /// `LoadGold` writes 0, which is `Resource::food` and `Resource::gold`.
  ///
  /// The field is not cleared when the cargo is spent, in either engine -- so a
  /// mule that has delivered still reports the kind it was carrying, and only
  /// `amount` says whether anything is there.
  ///
  /// **Hashed and saved**, for the reason `user` above is: a script branches on
  /// it, and the branch decides whether an outpost sells.
  std::int32_t cargo = 0;
  std::int32_t cargo_resource = 0;

  /// `Druid::SetJupiterAngerTarget` / `GetJupiterAngerTarget` -- the object
  /// handle at `[obj+0x1d0]`, and a getter/setter pair over one field, which is
  /// what makes it worth storing.
  ///
  /// `PRIEST_JUPITER_ANGER.VS` writes it while the spell is running and
  /// `PRIEST_ONDIE.VS` reads it back when the priest dies, to punish whoever
  /// was being punished. The setter (0x00513540) stores the target's handle and
  /// asks nothing; the **getter** (0x005134b0) is the one with a rule: it
  /// resolves the handle and answers it only when the object still exists *and*
  /// carries bit 22 of `[obj+0x2c]`, which is `is_unit`. Anything else is the
  /// invalid handle, which is what the one reader's `IsAlive` guard is written
  /// for.
  ///
  /// **Hashed and saved.** A priest that dies decides whether to strike, and
  /// two peers that disagree about its target strike two different things.
  ObjectId jupiter_target = kNoObject;

  /// `Unit::GetShipToBoard` -- the ship handle at `[unit+0x18c]`, and **not a
  /// membership flag**.
  ///
  /// `Ship::NotifyBoardUnit` (0x005c7fa0) stamps it and nothing anywhere clears
  /// it: the cancel erases the unit from the ship's list and does not touch the
  /// unit, and a successful board does not either. So it names *the ship that
  /// last asked for this unit*, and it outlives the asking -- which is exactly
  /// what `UNIT_BOARD_ONFINISH.VS` needs when it reaches back through
  /// `.GetShipToBoard` to cancel a boarding that has just been interrupted.
  ///
  /// Which units a ship is actually waiting for is a different thing and lives
  /// in `BoardingTable`; `sim/boarding.hpp` sets out why the two cannot be one
  /// field.
  ///
  /// **Hashed and saved**, for the reason `user` above is: two `..._ONFINISH`
  /// scripts branch on it, and the branch decides whether a ship keeps waiting.
  ObjectId ship_to_board = kNoObject;

  /// `Settlement::MostDamagedBuilding`'s ranking key -- `[building+0x1f8]`,
  /// the damage a building has taken since `ClearDamageTaken` last zeroed
  /// it. Combat adds every point removed from a building here; nothing else
  /// writes it, and nothing reads it but the two entry points, so a script
  /// that never clears it sees a running total. Kept on every object rather
  /// than a building-only record for the reason `cargo` is: one state, one
  /// hash. **Hashed and saved**: it decides which building the AI repairs.
  std::int32_t damage_taken = 0;

  /// `Unit::GetParryMode` -- `[unit+0x1a0]`, an `int` holding 0 or 1.
  ///
  /// A soldier told to raise its shield and stand. `SetParryMode(bool)`
  /// (0x005d8f80) writes it and `GetParryMode()` (0x005d9050) reads it back,
  /// which is the registered-getter-against-registered-setter pair that makes a
  /// value worth storing.
  ///
  /// **Both bodies are gated on the unit having the `Parry` special**, which is
  /// bit 0 of the 64-bit specials mask at `[unit+0x198]` -- entry 0 of the same
  /// 36-name table `Unit::HasSpecial` and `sim/globals.cpp` share. A unit whose
  /// class does not offer `Parry` cannot enter parry mode and reads back 0 even
  /// if something else had written the field. `UNIT_ENTER_PARRY_MODE_VERIFY.VS`
  /// asks both questions in that order -- `u.HasSpecial(parry)` over the whole
  /// selection first, then `u.GetParryMode() == 0` -- so the gate is visible
  /// from the corpus as well as from the executable.
  ///
  /// An `int` rather than a `bool` because the two shipped readers compare it
  /// against 0 and 1 numerically and because the field is an `int` in the
  /// original; the setter's argument is a `bool` and only ever writes those two.
  ///
  /// **Hashed and saved.** It decides which of two commands a selection may be
  /// given, and 31 shipped sub-AI scripts clear it as their first statement, so
  /// two peers that disagree about it give different orders.
  ///
  /// One half of the original is deliberately absent: entering parry mode also
  /// *spawns* an effect object of class `Parry` (0x00518650 entry 4) and stores
  /// its handle at `[unit+0x1c8]`, and leaving destroys it. That is the same
  /// spell-effect machinery `Sacrifice` belongs to, which this engine does not
  /// have; the flag is what every shipped reader reads.
  std::int32_t parry_mode = 0;

  /// `Building::IsBroken` -- `[building+0x204]`, a damage tier from 0 to 3.
  ///
  /// **State, not a derived value, because it has hysteresis.** 0x004db3d0
  /// computes `pct = health * 100 / maxhealth` and picks a raw tier -- 3 below
  /// `BuildingStateThreshold2`, 2 below `BuildingStateThreshold1`, 1 below
  /// `BuildingStateThreshold0`, else 0 -- and then refuses a *one-step* change
  /// unless the reading has cleared the boundary by `BuildingStateHysteresis`
  /// as well: stepping up to `old + 1` needs `pct <= threshold[old] - h`, and
  /// back down to `old - 1` needs `pct >= threshold[new] + h`. A jump of more
  /// than one step is taken immediately. So the tier depends on the tier
  /// before it, and two buildings at the same health can legitimately be in
  /// different tiers.
  ///
  /// `Building::IsBroken()` is `tier == 3` and nothing else; `RRepair`
  /// (0x004dd250) refuses to act on anything else. `IsVeryBroken` is **not**
  /// this field -- see `m_is_very_broken` in `sim/world_host.cpp`.
  ///
  /// Hashed and saved with the rest of the header. Written by
  /// `World::set_health`, which is the one place health changes.
  std::int32_t damage_state = 0;

  /// `Building::GetUITarget` / `SetUITarget` -- the **player's** manual target
  /// for this building, and the one field here that the interface writes and
  /// the simulation reads.
  ///
  /// `0x004ddc50` hands back the 16-bit handle at `[obj+0x1fc]` with a
  /// sub-object offset of zero, or the invalid handle -- **with no message** --
  /// when the receiver does not resolve. The two shipped readers are the tower
  /// scripts: `TOWER_GUARD.VS` takes it, checks `IsAlive` and `IsValidTarget`,
  /// and shoots it; `CATAPULT_TOWER_GUARD.VS` re-reads it *inside* its volley
  /// loop, which is what lets a player retarget a tower mid-shot.
  ///
  /// **Hashed and saved**, and the reason is the loop above: it decides what a
  /// tower fires at, so two peers that disagreed about it would fire at two
  /// different things. That it originates in a click does not make it
  /// per-screen -- the click becomes a command, and the command is replicated;
  /// what would be per-screen is the *selection*, which lives on
  /// `SelectionTable` for exactly the opposite reason.
  ObjectId ui_target = kNoObject;

  /// `Obj::MistAction`'s tag: the mist that has this unit, or none.
  ///
  /// 0x005ca050 writes the mist's handle to `[unit+0x14c]` on every unit it
  /// damages, and skips a unit whose slot resolves to a *different* live
  /// mist -- so two overlapping mists do not both burn the same unit, and a
  /// unit walks free of a mist only when that mist is gone. Nothing clears
  /// it; a dead mist's handle simply stops resolving. **Hashed and saved**:
  /// it decides who takes the next tick of damage.
  ObjectId mist = kNoObject;

  /// The cover of mercy this unit stands under, or none.
  ///
  /// The original keeps the transpose -- a list of the sheltered on the cover
  /// object at `[cover+0x14c]`, rewritten every action and walked to lower
  /// `half_damage` on those it listed last time. One handle per unit is that
  /// list turned round, and it differs in exactly one arrangement, two covers
  /// over one unit: there the original's second cover to act clears the bit
  /// the first just raised and the first re-raises it next animation, where
  /// this engine's tag keeps the bit up under whichever cover wrote last. A
  /// labelled reading, in the unit's favour. **Hashed and saved** with the
  /// bit it governs.
  ObjectId sheltered_by = kNoObject;

  /// `Teleport::destination` -- the far end of a teleport pair, 20 sites.
  ///
  /// `0x005cdbc0` is a one-field read of the 16-bit object id at
  /// `[teleport+0x208]`, handed back as a handle; a dead receiver logs and
  /// answers the invalid handle. It is the root of every teleport expression in
  /// the corpus -- `tel.destination.pos`, `tel.destination.radius`,
  /// `tel.destination.GetExitPoint(...)`, `tel.destination.settlement.AddUnit`
  /// -- so nothing else in that family is reachable without it.
  ///
  /// **The map authors it, and it took a census to say how.** A teleport's
  /// `<scriptobj>` carries `destination_set`, which names the *settlement* the
  /// paired teleport belongs to, by that settlement's `id` attribute. Across
  /// the 28 shipped `map.obj.xml` documents there are 54 teleports; every one
  /// is inside a settlement, no settlement holds two, and the relation is
  /// symmetric in 54 of 54 -- so "the teleport of settlement *n*" names exactly
  /// one object and the pair resolves both ways. `MapObject::destination_set`
  /// carries the attribute and `populate_from_map` does the resolution once, at
  /// load, which is where the original does it too.
  ///
  /// **Hashed and saved** rather than re-derived, for `WorldObject::sight`'s
  /// reason: a save that rebuilt it would make its meaning depend on the map
  /// file staying put. It is also not strictly derivable after load -- nothing
  /// keeps the map's settlement id table -- so a rebuild would need state this
  /// engine does not carry.
  ObjectId teleport_destination = kNoObject;

  /// `Teleport::Traverse(player)` -- which players have used this teleport.
  ///
  /// `0x005cdcb0` converts the 1-based player to an index, bounds-checks it
  /// against 16, reads that player's own bit from its record and **ORs it into
  /// the 32-bit mask at `[teleport+0x214]`**. It moves nothing: the six shipped
  /// sites do the movement themselves with `SetPos` and `Goto`, and call this
  /// on *both* ends of the pair afterwards. So it records that a player has
  /// been through, and nothing more.
  ///
  /// The original's bounds check has **no lower half** -- `player - 1` is only
  /// tested against 16 with a signed compare, so `Traverse(0)` indexes one
  /// stride below the player table and reads whatever is there. That is a fault
  /// rather than a behaviour and is guarded here instead of mirrored.
  ///
  /// **Hashed and saved.** What reads it is not settled -- `FindTeleport` is
  /// the natural consumer and has not been read to the end -- but the mask is a
  /// per-instance persist field of the original's, written by gameplay, and a
  /// reload that forgot which teleports a player had found would be a different
  /// game.
  std::uint32_t traversed_by = 0;

  [[nodiscard]] bool is_held() const noexcept { return holder != kNoObject; }
};

/// A unit of simulation behaviour driven by the turn loop.
///
/// Systems are registered with the world in a fixed order and run in that order
/// every turn; the order is part of the simulation's definition, so changing it
/// changes results. A system owns its own state and reaches objects through the
/// world rather than holding pointers across turns, since ids outlive
/// addresses.
class System {
 public:
  virtual ~System() = default;

  /// A stable name, used in diagnostics and in the run-order manifest.
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// Advance by one turn. `turn.length` is the game time elapsed, which varies:
  /// the original renegotiates it, and lengths of 200, 400, 799 and 800 all
  /// occur. Never assume a constant.
  virtual void advance(World& world, const Turn& turn) = 0;

  /// Fold this system's state into the world hash. Only state that the original
  /// hashes belongs here — notably **not** pathfinding or fog.
  virtual void hash(std::uint64_t& accumulator) const { (void)accumulator; }

  /// Called once after the world is populated and before the first turn.
  virtual void start(World& world) { (void)world; }
};

}  // namespace imperivm::core::sim
