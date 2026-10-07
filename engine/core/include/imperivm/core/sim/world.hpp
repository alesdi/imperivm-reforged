#pragma once

// The world the tick loop advances: objects, their state, their animation
// cursors, the queries over them, the systems that drive them, and the
// deterministic ordering all of it is kept in.
//
// Model and evidence: docs/engine/tick.md (the clock),
//                     docs/engine/state-vector.md (the object model),
//                     sim/system.hpp (the seam the domain systems attach to).
//
// Two rules from docs/engine/architecture.md are load-bearing here rather than
// decorative:
//
//   * **Iteration order is world state.** Objects live in a vector in spawn
//     order and are ticked in that order. Not a hash map, not a set, not a
//     pointer-keyed anything: an unordered container would make the tick order
//     depend on allocation addresses, and two peers would diverge on the first
//     interaction that cared. Every query below yields ids in that same order.
//   * **No wall clock.** `advance()` takes no wall-clock duration. It runs one
//     lockstep turn of a length that is world state, and the platform decides
//     when to call it and what the next length should be.
//
// The turn length is **not** constant -- see sim/tick.hpp for the measurement.
// So the property that matters is not "N identical ticks equal one batch of N"
// but the stronger one it generalises to: **any sequence of declared turn
// lengths leaves every animation cursor where the single turn of their sum
// would.** `test_tick.cpp` asserts that over the sequence [400, 800, 200] and
// over a thousand turns. It holds because nothing here integrates: game time
// is the exact integer sum of the declared lengths, and an animation's frame
// is a pure function of elapsed time reduced modulo its cycle.
//
// ## The object table is wider than the class hierarchy
//
// `docs/engine/state-vector.md` counts 32 runtime types in the dumps against 26
// declared `cpp_class` values. Sixteen of the 32 are not classes at all: eight
// are engine-internal state objects (`CVXSettlement`, `CVXHolder`,
// `CVXWarehouse`, `CVXItem`, `CVXItemScript`, and three per-session singletons),
// seven are query objects, and one is `CVXDecor` under a second name. They all
// come out of the same monotonic handle counter and they are all in the hashed
// slot table, so `WorldObject` has to be able to be one of them. That is what
// `InternalKind` is, and why `WorldObject::object` is allowed to be null.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/game/registry.hpp"
#include "imperivm/core/sim/array.hpp"
#include "imperivm/core/sim/boarding.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/conversation.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/spatial_index.hpp"
#include "imperivm/core/sim/gaika_table.hpp"
#include "imperivm/core/sim/gate.hpp"
#include "imperivm/core/sim/lsa.hpp"
#include "imperivm/core/sim/system.hpp"
#include "imperivm/core/sim/tick.hpp"
#include "imperivm/core/world/map.hpp"

namespace imperivm::core::edit {
struct PassMask;
}  // namespace imperivm::core::edit

namespace imperivm::core::sim {

/// Add `delta` game-time units to a cursor's elapsed time and reduce the
/// result to the timeline's canonical range.
///
/// Split out and taking a 64-bit delta because a batched advance can be
/// arbitrarily long: a million turns of 400 units overflows a 32-bit elapsed
/// time long before it overflows a turn counter, and an overflow is a desync.
///
/// This is where step-equivalence actually lives. `(a + b) mod c` does not
/// depend on where the sum was split, and saturation at `c` is idempotent, so
/// composing this over any partition of an interval gives one answer.
[[nodiscard]] std::int32_t advance_elapsed(const AnimTimeline& timeline, std::int32_t elapsed,
                                           GameTime delta, AnimRepeat repeat) noexcept;
/// The same, for a timeline of which only the cycle is known: what the view
/// keeps of an animation it saw at the turn end before (`sim/glide.hpp`). A
/// cycle of 0 or less is an animation that cannot advance, and answers 0.
[[nodiscard]] std::int32_t advance_elapsed(std::int32_t cycle, std::int32_t elapsed, GameTime delta,
                                           AnimRepeat repeat) noexcept;

// --------------------------------------------------------------------------
// SyncFlags
// --------------------------------------------------------------------------
//
// The dumps' `SyncFlags` word, decomposed. 49 distinct values over 7,580
// objects, and the word is structurally two halves: a one-hot owner in the low
// 16 bits and cached category bits above. See docs/engine/state-vector.md.

/// Bit 31, set on every one of the 7,580 objects: the slot is in use.
inline constexpr std::uint32_t kSyncLive = 0x80000000u;
/// Bit 24 -- set on all 31 heroes in the corpus and on nothing else.
inline constexpr std::uint32_t kSyncHero = 0x01000000u;
/// Bit 23 -- 2,551 objects, exactly the building family.
inline constexpr std::uint32_t kSyncBuilding = 0x00800000u;
/// Bit 22 -- 3,484 objects, exactly the unit family. With bit 23 this is an
/// exact partition of the class tree: 3,484 + 2,551 = 6,035, and the remaining
/// 1,545 are the decor, item holders and script objects that are neither.
inline constexpr std::uint32_t kSyncUnit = 0x00400000u;
/// Bit 21 -- **hidden**, and note the polarity: `Obj::IsVisible` in `gbr.exe`
/// (0x005ab970) returns `!((SyncFlags >> 21) & 1)`, so the bit set means *not*
/// visible. The target sweep tests the same bit at 0x005dba19, which is why an
/// invisible unit is not a target. No object in the nine dumps carries it, so
/// it costs nothing in the hash until something sets it.
inline constexpr std::uint32_t kSyncHidden = 0x00200000u;
/// Bit 19 -- **party membership**: the units that travel with the hero from one
/// adventure map to the next. Five facts, all read out of `gbr.exe`:
///
///   1. `Unit::GetParty` (`0x005d7b10`, registered at `0x005dfd6a` as
///      `bool GetParty(Unit)`) is exactly `(obj[0x2c] >> 0x13) & 1` --
///      `0x005d7b4e`-`0x005d7b57`. `obj + 0x2c` is this word: `Obj::IsVisible`
///      (`0x005ab970`) reads bit 21 of the same offset, which is `kSyncHidden`.
///   2. `Unit::SetParty(Unit, bool)` (`0x005d7b60`) writes nothing itself. It
///      dispatches on the argument to two methods of the singleton at
///      `[0x00996ff4]` -- vtable (base `0x007b8d28`) slot `0x98` when true,
///      slot `0x9c` when false. Slot `0x98` is `0x004cebd0`, which *sets* this
///      bit through the object's own `vtbl + 0x44` and files the handle in a
///      deque at `this + 0x1028`; slot `0x9c` is `0x004cd500`, which removes
///      the handle and *clears* the bit through `vtbl + 0x48`.
///   3. The free entry point `Party()` (`0x004ca9c0`, registered at
///      `0x004cad30` with the signature string `"ObjList"`) returns the
///      contents of that same deque, `[0x00996ff4] + 0x1028`. The bit and the
///      list are two views of one membership.
///   4. `0x004d0000` prints `"\nStoring party before moving to another map:\n"`
///      (`0x007b8ee0`) and, per member, `"Party unit of class %s, this ptr is
///      0x%08x, handle is %d\n"` (`0x007b8ea4`); it picks the members by
///      testing this bit at `0x004d0101`. The engine names the bit itself.
///   5. Nothing sets it at load. `<scriptobj flags>` bits 16..21 are clear on
///      all 27,070 objects across the 29 shipped maps (docs/formats/map.md), so
///      a freshly loaded world has an empty party -- and bit 19 is therefore
///      *not* one of that document's unexplained bits (26 and 29; 27 is
///      `kSyncUnspawned` below).
///
/// **`UNITFLAG_NOAI` is not this bit's neighbour.** It is `0x00040000`, bit 18,
/// of a *different* word: `obj + 0x194`, which is the map's `UnitFlags`
/// attribute. `Unit::GetFlags(Unit, int)` masks `[obj + 0x194]` (`0x005d7e75`),
/// `Unit::SetNoAIFlag` clears and sets `0x40000` there (`0x005de666`), and
/// `Unit::SetMinimapFlag` writes `0x4000000` to the same word (`0x005d8807`).
/// The adjacency of 18 and 19 is a coincidence across two unrelated words.
///
/// This paragraph used to end by saying the map file does not carry that word,
/// and **it does**: see `kUnitFlagNoAI` below.
inline constexpr std::uint32_t kSyncParty = 0x00080000u;
/// Bit 28 -- a unit that is training. Runtime only; see `ObjectFlags::training`.
inline constexpr std::uint32_t kSyncTraining = 0x10000000u;
/// Bit 27 -- **not in play**: an object the map placed as a spawn *template*,
/// which exists and holds a handle but is not yet part of the world a script
/// can see. `SpawnGroup` is what brings copies of them in.
///
/// `docs/formats/map.md` listed this as one of three `<scriptobj flags>` bits
/// "set on units only and not explained". It is explained now, from both ends:
///
///   1. `CVXGroup::Add` (`0x00572410`) routes on it. It tests
///      `[obj + 0x2c] & 0x08000000` and files the object in the group's
///      *template* deque at `+0x54` when the bit is set and in its live deque
///      at `+0x24` when it is clear -- and `CVXGroup::Spawn` (`0x00572760`)
///      iterates the template deque, clearing this bit on each copy it mints
///      (`vtbl + 0x48`, the flag-clear helper) so the copy files itself live.
///   2. Every collection path skips it. The area-query grid sweep at
///      `0x004fc6f4`, the `ObjList` collector at `0x0041f4e1` and some forty
///      other sites all read `test [obj + 0x2c], 0x8000000` / `jne skip`.
///      An object carrying it is in the handle table and in nothing else.
///
/// The corpus agrees and a control separates it from its neighbours. Across
/// the 29 shipped maps the bit partitions the 803 type-1 `<group>` elements
/// 576 all-marked / 209 none-marked / 18 mixed, and of the **214** groups a
/// script spawns by literal name, 212 are all-marked and **none** is
/// none-marked -- against 55.7 expected if the bit were unrelated to spawning.
/// Every other bit either fails that test or is degenerate: bit 22 splits
/// 794/9 and bit 29 splits 789/12, so neither has a side to be wrong about.
///
/// It is 39% of the shipped map objects -- 10,610 of 27,070, from 16% of
/// `6_Great_loses_Boudicca` to 77% of `5_Great_Battles_Britain` -- so a world
/// that treats templates as live starts a campaign map with half again as many
/// objects as the original, all of them enrolled, ticked and targetable.
///
/// No object in the nine dumps carries it, which is consistent rather than
/// confirming: they are random-map games, which author no groups and so have
/// no templates. Bit 29 is absent from them for the same uninformative reason.
inline constexpr std::uint32_t kSyncUnspawned = 0x08000000u;
/// Bit 17 -- 1,161 objects, perfectly predicting a printed `PathType` line in
/// one direction (1,112 yes, 0 false negatives; the 49 exceptions are wagons
/// unloading, which path without printing one).
inline constexpr std::uint32_t kSyncHasPath = 0x00020000u;
/// The owner half. Only eight values occur, every one zero or a single bit.
inline constexpr std::uint32_t kSyncOwnerMask = 0x0000FFFFu;

// -- the second flag word, `[obj+0x194]` ------------------------------------
//
// A different word from every constant above, carried by the map as the
// decimal `UnitFlags` attribute on all 16,171 units and on nothing else.
// Eight distinct values occur, using five bits; these are the two this project
// can name, and `docs/formats/map.md` still carries the other three as open.

/// Bit 18 -- `UNITFLAG_NOAI`, and the constant every shipped `Unit::GetFlags`
/// call site asks about. `ObjectFlags::no_ai`.
///
/// **The map authors it, on 14,586 of the 16,171 units**, which contradicts
/// what `ObjectFlags::no_ai` said for as long as nothing read the flag. The
/// correlation is not subtle: the bit is clear on *every wild animal in the
/// retail install* -- 226 deer, 207 crows, 101 wolves, 80 fish, 46 eagles, 25
/// boars, without one exception -- and set on the soldiers. That is what the
/// flag means read plainly: a campaign places its troops under the mission
/// script, which hands them over with `SetNoAIFlag(ol, false)` when it wants
/// them fighting on their own, while wildlife belongs to the AI from the first
/// tick and no script ever claims it.
///
/// It is also why the flag could not stay unread. `TOWNHALL_AUTOTRAIN.VS` adds
/// exactly the units *without* it, so a world that loaded it false would
/// enrol every soldier a map places into the town hall's training list.
inline constexpr std::uint32_t kUnitFlagNoAI = 0x00040000u;
/// Bit 22 -- `Flying::IsInAir`. `ObjectFlags::in_air`.
///
/// 46 of the retail install's units carry it and **every one of them is a
/// flying unit**, which is the whole of the evidence and is enough: the bit
/// this engine already named `in_air` from `0x0051bd20` occurs only on the
/// things that can be in the air. So 46 birds start a map airborne, and
/// `ObjectFlags::in_air`'s claim that nothing sets it is retired.
inline constexpr std::uint32_t kUnitFlagInAir = 0x00400000u;
/// Bit 26 of the same word, `Unit::SetMinimapFlag`. No shipped map authors
/// it; it matters here because the `CVXUnit` constructor sets it on every
/// sentry together with `kUnitFlagNoAI`, and a map-placed unit's `UnitFlags`
/// replaces the whole word (0x005dd6a9), this bit included.
inline constexpr std::uint32_t kUnitFlagOnMinimap = 0x04000000u;

/// Pack an object's owner and category bits into the original's word.
///
/// Bits 21 and 26 -- observed on `THuntress` and on ambient villagers
/// respectively, and on nothing else -- are deliberately not produced. Naming a
/// bit from one class's worth of evidence is how a guess gets built on.
[[nodiscard]] std::uint32_t pack_sync_flags(const ObjectState& state) noexcept;

/// The owner encoded in the low half, or `kNoPlayer` when no bit is set.
/// A word with more than one owner bit is malformed -- not one object in 7,580
/// has two -- and reads as `kNoPlayer` rather than as the lowest bit.
[[nodiscard]] PlayerId unpack_owner(std::uint32_t sync_flags) noexcept;

/// The category bits of the high half.
[[nodiscard]] ObjectFlags unpack_flags(std::uint32_t sync_flags) noexcept;

/// The cached category bits a native class implies.
///
/// `is_unit` and `is_building` come straight from `native_class_is_a`, whose
/// hierarchy reproduces the dumps' partition exactly: the unit family is
/// CVXUnit and its five descendants, the building family is CVXBuilding and its
/// seven, and decor, script objects and item holders are neither.
[[nodiscard]] ObjectFlags flags_for_native_class(NativeClass id) noexcept;

// --------------------------------------------------------------------------
// objects
// --------------------------------------------------------------------------

/// What an object is, when it is not one of the 26 declared classes.
///
/// These have no `cpp_class` anywhere in `DATA\CLASSES` -- verified by direct
/// scan of `data.pak` -- because they are engine-internal. They are
/// unambiguously state: they occupy handles, other objects reference them by
/// handle, and they sit inside the hashed slot table.
enum class InternalKind : std::uint8_t {
  /// An ordinary data-driven object; `WorldObject::object` is non-null.
  none = 0,
  /// `CVXSettlement` -- 328 blocks, 9/9 dumps. A town's economy and capture
  /// state. **Not the same thing as a town hall**: its `class` names an anchor
  /// object that can be an outpost, a teleport, a catapult or a ruin, and the
  /// settlement count exceeds the town-hall count in every dump.
  settlement,
  /// `CVXHolder` -- 331 blocks. The general "this object contains units"
  /// container, attached to settlements and to ships alike.
  holder,
  /// `CVXWarehouse` -- 328 blocks. Prints only its handle and is referenced by
  /// nothing; its contents are printed on the settlement.
  warehouse,
  /// `CVXItem` -- 170 blocks, 3/9 dumps. An inventory item instance.
  item,
  /// `CVXItemScript` -- 1 block in the whole corpus.
  item_script,
  /// The three per-session singletons, always at three consecutive handles.
  ai_helper,
  player_bonus,
  player_scripts,
  /// A query object. `WorldObject::query` indexes its spec.
  query,
  count,
};

/// The runtime type name the original prints for an internal kind.
[[nodiscard]] std::string_view internal_type_name(InternalKind kind) noexcept;

/// The four `[GamePlay]` keys that turn a building's health into a damage tier.
///
/// `CONST.INI` ships `BuildingStateThreshold0 = 75`, `1 = 50`, `2 = 25` and
/// `BuildingStateHysteresis = 3`, and `gbr.exe` reads them by name at
/// 0x004db730, refusing to start without them ("Error reading required value :
/// BuildingStateThreshold0") and rejecting any value outside 0..100.
///
/// Compiled defaults, overridable, and the same standing as `EconomyRules`:
/// the shipped numbers are here so a synthetic world tiers correctly with no
/// `CONST.INI` at all, and `GameSession::create` overwrites them from the file
/// when there is one.
struct BuildingStateRules {
  /// The health percentage below which a building is *tier 1*.
  std::int32_t threshold0 = 75;
  std::int32_t threshold1 = 50;
  std::int32_t threshold2 = 25;
  /// How far past a boundary the reading must go before a **one-step** change
  /// is committed. A jump of more than one step ignores it.
  std::int32_t hysteresis = 3;
};

/// The tier `current` becomes at `health / max_health`, under `rules`.
///
/// Free and pure so that the rule can be tested without a world, and because
/// it is the only part of the mechanism with any arithmetic in it. A
/// `max_health` of zero has no percentage and holds the tier where it is: an
/// object with no health is not a damaged building, it is a decor.
[[nodiscard]] std::int32_t next_building_state(std::int32_t current, std::int32_t health,
                                               std::int32_t max_health,
                                               const BuildingStateRules& rules) noexcept;

/// The leg a flying unit's `PlayAnim` set it flying: where the animation took
/// it from and where it put it. `sim/flying.hpp`'s `flight_progress` reads it;
/// see there for what the original does with the two ends.
struct FlightLeg {
  Point from;
  Point to;
  /// Written by `Flying::PlayAnim` once its animation has started, cleared by
  /// every other start.
  bool valid = false;
};

/// One object in the world.
///
/// The native object owns the art binding and the animation cursor; this adds
/// the simulation state (`ObjectState`, the common header every `CVXDecor`
/// descendant carries), the resolved animation timeline, and the links the
/// dumps show but the loader had no way to compute.
///
/// `id` lives here rather than only on `object` because an internal object has
/// no `object` at all. It is written once, at spawn, in one place, and the
/// native object's copy is written from it in that same statement, so the two
/// cannot drift.
struct WorldObject {
  /// Stable, monotonic, never reused. Ascending across `World::objects()`.
  ObjectId id = kNoObject;

  /// Null exactly when `internal != InternalKind::none`.
  std::unique_ptr<NativeObject> object;
  InternalKind internal = InternalKind::none;

  /// The synchronised common header. **This is the authoritative position,
  /// owner, health and stamina**; the same-named fields on `NativeObject` are
  /// pre-Part-5 placeholders that the simulation neither reads nor writes.
  ObjectState state;

  /// The class this object was spawned from, or `kNoClass`. Duplicated from
  /// `NativeObject::class_index` so that class filters can run over internal
  /// objects (which have no native object) without a null check per candidate.
  ClassIndex class_index = kNoClass;

  /// The settlement this object belongs to, or `kNoObject`. `Building.settlement`
  /// is the fifth most used member in the whole host API (432 call sites), so
  /// the link is real even though the dumps only show it from the other side.
  ObjectId settlement = kNoObject;

  /// The map's `display_name`, when the element carries one (93 objects in
  /// the retail install: `Aeneas`, `PtolomaeusXI`, `Grand priest`). The
  /// unit loader (0x005dd3a0) assigns it into the string at `[unit+0xa0]`
  /// over the class's display name, and that string is what the info bar
  /// shows for the object. Saved with the object; not hashed, because it is
  /// a line of display text that no rule reads.
  std::string display_name;

  /// Sight radius in world units, seeded from the class property `sight` at
  /// spawn.
  ///
  /// **Turn state, and hashed.** It used to be neither: nothing could move it,
  /// so it was written into the save only because a reload could not recompute
  /// it, and it was deliberately kept out of `state_hash`. `Obj::SetSight`
  /// (0x005ab360) moves it -- `TAVERN_EXPEDITION.VS` widens a scout's -- and a
  /// radius two peers disagree about is a scout that finds two different
  /// things.
  std::int32_t sight = 0;

  /// Index into the world's query table when `internal == InternalKind::query`.
  std::uint32_t query = 0;

  AnimTimeline timeline;
  AnimRepeat repeat = AnimRepeat::loop;
  /// False when the object is holding a still pose rather than playing an
  /// animation -- the common case, since 579 of the 889 entities declare no
  /// animation at all.
  bool animating = false;
  /// The construction frame `Catapult::SetBuildFrame` last set -- a frame of
  /// the animation playing, counted as the entity declares its frames -- or
  /// -1 for none. What a half-built siege engine is drawn at: `gbr.exe` keeps
  /// it at `[obj+0x20c]`, freezes the sprite on it (0x004e2ed0), and
  /// `SetBuilt` writes -1 back and lets the sprite run (0x004e2e20). Read the
  /// row through `build_frame_row` (`sim/anim.hpp`). Presentation: not hashed,
  /// not saved, read by nothing in the simulation; a loaded engine shows its
  /// animation's own step until `CATAPULT_IDLE.VS` sets it again, one 500 ms
  /// cycle on.
  std::int32_t build_frame = -1;
  /// The leg a bird is flying, which the view draws it along rather than at
  /// its end (`flight_progress`, `sim/flying.hpp`). Presentation: not hashed,
  /// not saved, read by nothing in the simulation -- the position the
  /// simulation reads is already the leg's end. A loaded bird is drawn at the
  /// end of the leg it was flying until its script's next `PlayAnim`, one
  /// animation on.
  FlightLeg flight;

  /// Where the world's spatial index has filed this object. Derived: not
  /// hashed, not saved, rebuilt on load. See `sim/spatial_index.hpp`.
  SpatialSlot spatial;
  /// A gate's portcullis motion, which `gate_raise` turns into how far it is
  /// raised. State: the gate lets units through once it stands above 20, so
  /// it is hashed and saved -- for gates, which are the only objects that
  /// write it. See `sim/gate.hpp`.
  GateMotion gate;
};

/// Resolve an entity path to a loaded entity.
///
/// The core cannot open a file, so populating a world from a map delegates art
/// binding the same way `ClassGraph::validate` delegates existence checks. A
/// null resolver is fine: every object is then spawned with no entity, which is
/// exactly what an object whose class names no entity gets anyway.
class EntityResolver {
 public:
  virtual ~EntityResolver() = default;
  [[nodiscard]] virtual const Entity* resolve(std::string_view path) const = 0;
};

/// Resolves an entity's `.pass` mask -- a file in the art pack beside its
/// definition, which the core cannot open -- to the parsed mask with its
/// bounds, or null for an entity that names none. The body every
/// implementation shares is `edit::PassMaskLibrary::resolve`; what differs
/// is how the bytes are read. The session asks it once per class at the
/// passability rebuild of match start (`sim/session.hpp`).
class PassMaskResolver {
 public:
  virtual ~PassMaskResolver() = default;
  [[nodiscard]] virtual const edit::PassMask* mask_of(const Entity* entity) = 0;
};

/// The seven hash channels the original records, plus its roll-up.
///
/// Reproduced as a struct so a run can be diffed against a dump's `[HASHES]`
/// block field by field. **Four of the channels are zero in all nine dumps** --
/// the shipped build kept the pathfinder, fog of war, script state and AI out of
/// the determinism contract -- and they are kept here at zero deliberately.
/// Anything that would make them non-zero has pulled a subsystem into hashed
/// state that the original left out.
struct WorldHashes {
  /// The object slot table. Non-zero and distinct in all nine dumps.
  std::uint64_t slots = 0;
  /// Script coroutine states. Non-zero in all nine; zero here until the
  /// scheduler registers a system that folds them in.
  std::uint64_t threads = 0;
  /// The net command stream. Zero in the two dumps with `cmdsprocessed = 0`.
  std::uint64_t netcmds = 0;
  /// Purpose unknown; non-zero in exactly one dump.
  std::uint64_t extrahash = 0;

  std::uint64_t pathfinder = 0;   ///< 0/9 -- must stay zero
  std::uint64_t exploration = 0;  ///< 0/9 -- must stay zero
  std::uint64_t scriptstate = 0;  ///< 0/9 -- must stay zero
  std::uint64_t aihash = 0;       ///< 0/9 -- must stay zero

  /// Equals `[DESYNC].Hash` in all nine dumps.
  std::uint64_t hash_of_hashes = 0;
};

// --------------------------------------------------------------------------
// named object groups, and named objects
// --------------------------------------------------------------------------
//
// `map.obj.xml`'s `<group>` element carries a `type`, and the two values are
// **two different things behind two different host entry points**, not one
// thing with a flavour. `gbr.exe` settles that, and it is the stronger source:
//
//   * the editor's integrity checker emits `<name>=Group("` for one kind and
//     `<name>=GetNamedObj("` for the other, and keeps separate diagnostic codes
//     for them (`GROUP_NAME_REUSED`, `NAMEDOBJ_OVERRIDES_GROUP`), warning that a
//     name "used for both a group and a named object" makes *the group*
//     unreachable from sequence scripts;
//   * the runtime type list holds `CVXGroupQuery` **and** `CVXNamedObjQuery`,
//     side by side;
//   * `NamedObj` is its own script type, with `obj`, `IsValid` and `IsDead`
//     members and its own `Could not find named object named '%s'` diagnostic.
//
// The shipped scripts agree without a single exception. Over every call site in
// the map scripts that passes a string literal:
//
//     Group("...")            42 -- 42 type 1, 0 type 0
//     GetNamedObj("...")      66 -- 66 type 0, 0 type 1
//     SpawnGroup("...")      150 -- 150 type 1, 0 type 0
//     .AddToGroup("...")     125 -- 125 type 1, 0 type 0
//     .RemoveFromGroup(...)  100 -- 100 type 1, 0 type 0
//
// 483 typed resolutions, zero crossover. So `GroupTable` holds the type-1
// groups, `NamedObjectTable` holds the type-0 aliases, and neither answers for
// the other. An earlier revision of this header merged them into one name-keyed
// table on the strength of the call-site evidence alone, which made
// `Group("Caesar")` resolve an alias that the retail engine would not have
// found; `gbr.exe` is what corrected it.

/// The world's named object groups: `map.obj.xml`'s `<group type="1">`
/// elements, plus every group the running scripts have made since.
///
/// ## Why this is world state and not load-time data
///
/// A group looks like static authoring -- a name over a set of `<scriptobj>`
/// numbers, fixed in the map file -- and it is not. The shipped scripts move
/// objects between groups while the game runs, and they name groups that no map
/// file ever declared. `DATA\AI\ES_OUTPOSTSELLGOLD.VS` alone settles it, inside
/// the 577-script `data.pak` corpus and without leaving it:
///
///     ol = Group("GoldMules" + idPlayer).GetObjList();   // line 21
///     wagon.AddToGroup("GoldMules" + idPlayer);          // line 38
///     Group("GoldMules" + idPlayer).RemoveFromAllGroups();  // lines 48, 54
///
/// No map declares a `GoldMules<n>`; the group exists because line 38 put a
/// wagon in it, and it empties again because line 48 took it out. The same
/// shape drives training: `DATA\SUBAI\ARENA_BEHAVIOR.VS` does
/// `newunit.AddToGroup("Player" + .player + groupname)` on every unit an arena
/// produces, which is the group `EvalGroup("Player" + idPlayer + "BVikingLord")`
/// reads in `DATA\AI\GETARMYNEED.VS`.
///
/// The scripts shipped *inside* the map containers say the same thing far more
/// loudly. Over all 885 `.vs` files in the retail install -- the 577 in
/// `data.pak` and the 308 in the 24 adventure, conquest and scenario containers
/// -- the mutators are as common as the reader: `Group` 216 call sites,
/// `AddToGroup` 185, `RemoveFromGroup` 138, `RemoveFromAllGroups` 22. And of
/// the 528 literal `Group("...")` sites in map scripts, 111 -- 21 distinct names
/// -- name a group their own `map.obj.xml` does not contain. All 21 are targets
/// of an `AddToGroup` in the same map's scripts. `Conquests\mediterranean`
/// map 4 opens its second sequence by folding twelve authored groups into one
/// that the file does not declare:
///
///     Group("Oasis_Guards1").AddToGroup("Oasis_Guards");   // ... 2 .. 12
///     while (Group("Oasis_Guards").count != 0)
///
/// So membership is hashed and iterated like everything else here, and the
/// deferred decision in `sim/query.hpp` -- whether the group table is state --
/// resolves to yes.
///
/// ## Names, and why the spec carries an integer
///
/// `Group(n)` takes a string at all nine `data.pak` call sites and at all 216
/// across the install, while `QuerySpec::group` is an `int32`. The integer is
/// this table's index: a name is interned once, in first-mention order, and the
/// index is what the query object holds and what the state hash sees. That
/// keeps a query's hashed identity independent of how long its name is, and it
/// is the only reading under which the same script run on two peers builds the
/// same specs -- interning order is script execution order, which is lockstep.
///
/// No shipped map repeats a type-1 group name, so seeding never merges; the
/// four maps that do repeat a name repeat a type-0 one, which is
/// `NamedObjectTable`'s problem rather than this table's.
///
/// ## Which operations create a group and which refuse
///
/// `gbr.exe` carries a `Could not find group named '%s' in function '...'.
/// Check the spelling.` for `ObjList::RemoveFromGroup`, `Query::RemoveFromGroup`,
/// `SpawnGroup`, `SpawnGroupInHolder` and `SpawnGroupInShip` -- and **for no
/// form of `AddToGroup`**, on `Obj`, `ObjList` or `Query`. So `AddToGroup`
/// creates a group that does not exist and the others refuse one, which is
/// exactly what `mediterranean` map 4 needs: it builds `Oasis_Guards`, a name no
/// map file declares, out of twelve `AddToGroup` calls. `intern` is the creating
/// form and `find` the refusing one; callers pick.
///
/// ## Ordering
///
/// **Members are kept in first-add order, not sorted.** This header said the
/// opposite for a long time, on the reasoning that the only way a script reads
/// a group is through a query and every query yields ascending ids, so nothing
/// could observe the stored order. Both halves of that are wrong: `SpawnGroup`
/// reads the stored order directly rather than through a query, and the state
/// hash folds the member list in the order it is held.
///
/// `gbr.exe` settles it at instruction level. `CVXGroup::Add` (0x00572410) is
/// a `std::find` over the member deque followed by a `push_back` when the find
/// came back empty -- a linear dedup and an append, with no comparison of ids
/// anywhere in it -- and `CVXGroup::Remove` (0x00572210) is the same `find`
/// followed by an `erase` of the one element, which keeps the survivors in
/// their relative order. So a group is a set whose order is the order things
/// joined it, and the shipped line that separates the two readings is
/// `mediterranean` map 4's `Q_Interceptors1..4.AddToGroup("Q_Slingers")`:
/// four cohorts folded into one name, in the order the script names them,
/// which is not the order their ids run in.
///
/// It is load-bearing rather than cosmetic because the *spawn* path walks this
/// list: `CVXGroup::Spawn` (0x00572760) iterates the template deque in stored
/// order and mints one object per entry, so the stored order is the order new
/// ids are handed out in, and every id downstream of a spawn moves with it.
///
/// **A group query yields that order too**, which is the one place this table
/// is allowed to break `sim/query.hpp`'s otherwise engine-wide ascending rule.
/// It is not a free choice: of the 216 `Group(...)` sites in the 885 shipped
/// scripts, none indexes the query, but 32 reach `.SetCommand` on it and 25
/// `.GetObjList`, and both apply an operation *per member in evaluation
/// order* -- so the order lands in the command queue, which is serialised
/// state. Selecting the right set in the wrong order would have been a silent
/// divergence in exactly the channel a save round-trip compares.
///
/// The cost is confined to `World::evaluate_query`'s set-op branch, which
/// merged its two operands with `std::set_intersection` and friends and so
/// required sorted input. It sorts copies now. That leaves set-op results
/// ascending, which is its own labelled divergence -- the original's
/// `CVXSetOpQuery` composes two unsorted deques with a linear `Contains` and
/// yields neither operand's order -- but it is a query-layer question rather
/// than a group-table one, and no shipped site indexes a set-op query either.
class GroupTable {
 public:
  /// No such group. Distinct from every valid index, which are 0..size()-1.
  static constexpr std::int32_t kNoGroup = -1;

  /// The index of `name`, or `kNoGroup`. Does not create.
  [[nodiscard]] std::int32_t find(std::string_view name) const noexcept;

  /// The index of `name`, creating an empty group if there is none.
  ///
  /// Creating on lookup rather than only on `AddToGroup` is deliberate:
  /// `Group("X")` is a persistent query object that has to keep answering after
  /// something is later added to `X`, and `mediterranean` map 4 tests
  /// `Group("Oasis_Guards").count` on a name the map file never declared. An
  /// index that could not be minted until the first member arrived would make
  /// that query permanently empty. Interning is idempotent and deterministic,
  /// so a read that creates is still a read as far as the peers are concerned.
  std::int32_t intern(std::string_view name);

  [[nodiscard]] std::size_t size() const noexcept { return groups_.size(); }
  [[nodiscard]] bool valid(std::int32_t group) const noexcept {
    return group >= 0 && static_cast<std::size_t>(group) < groups_.size();
  }
  /// The interned name, or empty for an index this table does not hold.
  [[nodiscard]] std::string_view name(std::int32_t group) const noexcept;
  /// Members, ascending by id. Empty for an index this table does not hold.
  [[nodiscard]] std::span<const ObjectId> members(std::int32_t group) const noexcept;
  [[nodiscard]] bool contains(std::int32_t group, ObjectId id) const noexcept;

  /// Add `id` to `group`. False when the group does not exist, the id is
  /// `kNoObject`, or it is already a member -- a group is a set, and the 23,408
  /// authored memberships contain no repeat.
  bool add(std::int32_t group, ObjectId id);
  /// Remove `id` from `group`. False when it was not a member.
  bool remove(std::int32_t group, ObjectId id);
  /// Remove `id` from every group. Returns how many it was in. This is both
  /// `RemoveFromAllGroups` and what `World::despawn` calls, so that a dead
  /// object cannot linger in the hashed table.
  std::size_t remove_from_all(ObjectId id);

  /// Fold into the world hash: the count, then each group's index, name and
  /// members in order. The name is folded because two tables holding the same
  /// sets under different names are different worlds -- the next `Group("...")`
  /// resolves differently in each.
  void hash(std::uint64_t& accumulator) const noexcept;

 private:
  struct Entry {
    std::string name;
    std::vector<ObjectId> members;  ///< first-add order, no duplicates
  };
  /// Interning order, which is index order. A vector, never a map: iteration
  /// order is state, and a name lookup is a linear scan over a table that is
  /// 803 entries at its shipped worst.
  std::vector<Entry> groups_;
};

/// The world's named objects: `map.obj.xml`'s `<group type="0">` elements.
///
/// A name for exactly one object. 1,378 ship, every one with exactly one
/// `<obj num>`, and they cover the areas, the town halls, the gates and the
/// heroes conversations speak through. `GetNamedObj(name)` reads this table and
/// nothing else does: 66 of the 67 literal call sites in the shipped map
/// scripts resolve here and none resolves into `GroupTable`.
///
/// The scripts also reach these through a bare identifier with an `.obj`
/// member -- `Village2.obj.player`, `britVillage2.obj.SetCommand("tribute",
/// britTown1.obj)` -- because the editor writes a `Name = GetNamedObj("Name")`
/// preamble into each map's sequence scripts. Same lookup, different spelling.
///
/// ## Binding, and what stays unknown
///
/// The binding survives its object's death rather than being cleared with it.
/// That is not a choice: `gbr.exe` gives `NamedObj` an `IsDead` member and the
/// message `Named unit %s is dead or not initialized!`, which only mean
/// something if the name still resolves to a dead object. So `World::despawn`
/// prunes `GroupTable` and deliberately leaves this table alone, and callers
/// test the handle rather than trusting it.
///
/// **Whether the binding can be rewritten is not established.** `SpawnNamed`
/// exists and refuses an unknown name (`Could not find named object named
/// '%s'`), so it reads this table; whether it also rebinds the name to whatever
/// it just spawned is invisible from outside `gbr.exe`. There is no rebinding
/// entry point here until that is answered.
///
/// The table is hashed anyway. It is object state, two peers that disagree
/// about `Village2.obj` diverge on the next command issued through it, and a
/// table that only ever shrinks is still a table two peers must agree on.
/// The rectangles a script has named, interned by value.
///
/// A VS `rect` is a **value**, not a handle: `rc2 = rc1` copies four integers,
/// and `rect::Set` mutates in place. Four `int32`s do not fit in a
/// `script::Value`'s 32-bit object id, so a rectangle has to live in a table --
/// and the table is what makes value semantics work rather than fighting them.
///
/// Entries are **immutable and content-addressed**: `intern` returns the
/// existing index for a geometry it already holds and appends otherwise, so two
/// equal rectangles are one entry and equality is index equality. A mutation
/// (`rc.Set(...)`) interns the new geometry and writes the new index back into
/// the caller's local through the VM's writeback, exactly as `point::Set`
/// already writes a new packed point back. Nothing is ever modified in place,
/// so nothing needs cloning on assignment -- which is the opposite of
/// `ObjListPool`, whose entries are mutable and must *not* be cloned.
///
/// Bounded by the number of distinct rectangles a session names. In the shipped
/// corpus that is **one**: all 29 `rect` declarations in all 885 scripts are
/// assigned from `GetMapRect()`, and `rect::Set`, `IntersectRects` and
/// `AddRects` have no call sites at all.
///
/// Not folded into the world hash. An index is a function of the geometry and
/// of the order geometries first appeared, and indices reach only script locals
/// -- which this engine does not hash, matching `scriptstate` being zero in all
/// nine reference dumps.
class RectTable {
 public:
  static constexpr std::uint32_t kNoRect = 0xFFFFFFFFu;

  struct Rect {
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t right = 0;
    std::int32_t bottom = 0;
    friend constexpr bool operator==(const Rect&, const Rect&) noexcept = default;
  };

  /// The index of `r`, appending it when the table does not already hold it.
  std::uint32_t intern(const Rect& r);

  /// The rectangle at `index`, or null.
  [[nodiscard]] const Rect* find(std::uint32_t index) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return rects_.size(); }
  [[nodiscard]] std::span<const Rect> all() const noexcept { return rects_; }

 private:
  std::vector<Rect> rects_;
};

class NamedObjectTable {
 public:
  /// No such name. Distinct from every valid index, which are 0..size()-1.
  static constexpr std::int32_t kNoName = -1;

  /// The index of `name`, or `kNoName`. Never creates: every consumer in the
  /// executable refuses an unknown name rather than minting one.
  [[nodiscard]] std::int32_t find(std::string_view name) const noexcept;

  /// Bind `name` to `id`. False when the name is already bound, which is how
  /// the four maps that repeat a type-0 name resolve: **first in document order
  /// wins**.
  ///
  /// That tie-break is *inference*. `5_Great_Loses_German` carries eleven
  /// `<group name="NO_Invisible" type="0">` elements over eleven different
  /// objects, and `NO_OUT8`, `Village2` and `sandruins2` are each doubled in
  /// another map; nothing in the file or in `gbr.exe` says which one
  /// `GetNamedObj` should answer with. First-wins is the only rule that does
  /// not depend on how the loader happens to iterate. The one shipped script
  /// that touches a repeated name uses it as a conversation-actor string
  /// (`C_Conv.SetActor("NO_Invisible", ...)`) rather than as an object, so
  /// nothing in the corpus can tell the readings apart.
  bool bind(std::string_view name, ObjectId id);

  [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }
  [[nodiscard]] bool valid(std::int32_t index) const noexcept {
    return index >= 0 && static_cast<std::size_t>(index) < names_.size();
  }
  [[nodiscard]] std::string_view name(std::int32_t index) const noexcept;
  /// The bound object, which may since have died. `kNoObject` for an index this
  /// table does not hold.
  [[nodiscard]] ObjectId object(std::int32_t index) const noexcept;
  [[nodiscard]] ObjectId object(std::string_view name) const noexcept {
    return object(find(name));
  }

  /// Point an already-bound name at a different object, or bind it if it is
  /// free. False only for `kNoObject`.
  ///
  /// `bind` refuses a name it already holds, because at load time the second
  /// `<group type="0">` of a repeated name must not overwrite the first.
  /// Spawning is the other case, and `gbr.exe` settles a question
  /// `docs/formats/map.md` records as open -- "whether a named-object binding
  /// can be rewritten". It can, and spawning is what rewrites it:
  /// `CVXGroup::Spawn`'s inner loop (`0x005728c1`-`0x0057291c`) walks the
  /// named-object table, asks each entry through a virtual whether it names the
  /// *template* (`vtbl + 0x24`, against the template's id at `[templ + 8]`),
  /// and on a hit calls `Obj::SetName` (`0x005b6120`) on the **copy** with that
  /// entry's string. So the name follows the object into play, which is why
  /// `NO_Scipio` resolves to nothing until `SpawnGroup("Q_Scipio")` has run and
  /// to the spawned hero afterwards.
  bool rebind(std::string_view name, ObjectId id);

  /// The name bound to `object`, or empty -- `Obj::name`'s answer.
  ///
  /// **Ascending name order, and that is not this table's own order.**
  /// `gbr.exe` 0x0054b3a0 walks the named-object manager's
  /// `std::map<std::string, ...>` with `++it` (0x004f84a0) comparing each
  /// node's mapped pointer against the object, and returns the first key that
  /// matches -- so when one object carries two names, the alphabetically
  /// smaller one wins. This table is a vector in document order, which would
  /// answer with the earlier declaration instead, so the smallest is chosen
  /// explicitly rather than inherited from the storage. On a miss the original
  /// returns the global empty string at 0x0082a438; so does this.
  ///
  /// `kNoObject` never matches, so an unbound name -- one whose object died,
  /// or a `<group type="0">` naming a template that has not spawned -- cannot
  /// be reached backwards through it.
  [[nodiscard]] std::string_view name_of(ObjectId object) const noexcept;

  void hash(std::uint64_t& accumulator) const noexcept;

 private:
  struct Entry {
    std::string name;
    ObjectId object = kNoObject;
  };
  /// Document order, which is index order. A vector, for the same reason
  /// `GroupTable` uses one.
  std::vector<Entry> names_;
};

/// Where the class script hooks are launched from. Forward-declared rather
/// than included: `sim/hooks.hpp` declares `fire_class_hook` over a `World&`,
/// so including it here would be a cycle, and a pointer needs only the name.
class ClassHookRunner;

/// What a system may watch on the world without the world knowing the
/// system: the two moments `gbr.exe` books its per-player unit statistics
/// at. `Unit::SetPlayer` (0x005dc3f0) counts a `Military` unit produced for
/// its new owner and raises that owner's *most at once*; the unit's
/// destructor (0x005db4c0) counts one lost for its owner. `MatchSystem`
/// implements this and installs itself in `start`. Configuration, like the
/// hook runner: not owned, not hashed, not serialised; null is silence.
class WorldObserver {
 public:
  virtual ~WorldObserver() = default;
  /// An object in play took an owner: at its spawn (`previous` is
  /// `kNoPlayer`), or through `set_owner` when the owner changed.
  virtual void owner_set(World& world, ObjectId id, PlayerId previous, PlayerId now) = 0;
  /// An object is about to leave the world; it still resolves.
  virtual void despawning(World& world, const WorldObject& object) = 0;
};

/// The simulation.
///
/// Owns the clock, the objects, the query table, the RNG and the ordered list
/// of systems. Behaviour -- movement, combat, economy, heroes -- arrives as
/// `System` implementations registered with `add_system`, and none of them
/// needs this class to grow a field to do it.
class World {
 public:
  World() = default;
  explicit World(TickConfig config) noexcept : clock_(config) {}

  [[nodiscard]] Clock& clock() noexcept { return clock_; }
  [[nodiscard]] const Clock& clock() const noexcept { return clock_; }
  [[nodiscard]] GameTime time() const noexcept { return clock_.time(); }
  /// Turns run. The dumps' `Tick` field is this plus one.
  [[nodiscard]] std::uint64_t turns() const noexcept { return clock_.turns(); }
  [[nodiscard]] const Turn& turn() const noexcept { return clock_.turn(); }

  // -- the class graph ---------------------------------------------------

  /// Bind the class graph, which class filters and the `sight` property are
  /// read through. Not owned, and expected to outlive the world. Optional: a
  /// world with no graph resolves no class filter, so every filter matches
  /// everything, which is what a synthetic test wants.
  void set_class_graph(const ClassGraph* graph) noexcept { classes_ = graph; }

  // -- the class script hooks --------------------------------------------

  /// Where `ondie`, `onkill` and `onenter` are launched from, or null.
  ///
  /// The seam a *system* reaches the scheduler through. `CombatSystem::advance`
  /// holds a world and no scheduler, and the death hook has to fire from there
  /// as well as from `Erase`, so the launcher arrives the way the compiler
  /// arrives at a host function: as an interface the core declares and the
  /// session implements. See `sim/hooks.hpp` for the whole argument.
  ///
  /// **Configuration, not state.** Not owned, not hashed, not serialised, and
  /// expected to outlive the world -- `set_class_graph`'s terms exactly. Null
  /// is the ordinary case for a synthetic test and must degrade to silence,
  /// which `fire_class_hook` does while still taking the `ondie` latch.
  void set_hook_runner(ClassHookRunner* runner) noexcept { hooks_ = runner; }
  [[nodiscard]] ClassHookRunner* hook_runner() const noexcept { return hooks_; }
  /// The statistics seam; see `WorldObserver`. Same terms as the hook runner.
  void set_observer(WorldObserver* observer) noexcept { observer_ = observer; }
  [[nodiscard]] WorldObserver* observer() const noexcept { return observer_; }

  /// The building damage tiers. Configuration, like the class graph beside it:
  /// it comes from `CONST.INI`, is the same on both sides of a save, and is
  /// neither hashed nor serialised. The *tier* it produces is all three.
  void set_building_state_rules(const BuildingStateRules& rules) noexcept {
    building_states_ = rules;
  }
  [[nodiscard]] const BuildingStateRules& building_state_rules() const noexcept {
    return building_states_;
  }

  /// The map's terrain-type layer, or an unparsed grid.
  ///
  /// `Maps/<n>/Terrain.terrain.grid`: 64 units per cell, one byte per cell, and
  /// the only consumer in the simulation is `IsPointInWater`, which asks whether
  /// a cell's type is 13. **Configuration, not state**: it comes from the map,
  /// is the same on both sides of a save, and is neither hashed nor serialised
  /// -- the same standing as the class graph beside it and as
  /// `MovementSystem`'s obstruction grid.
  ///
  /// Held by value because a `Grid` is a header and a span, and the bytes it
  /// spans belong to the caller: `SessionInputs` says every payload must outlive
  /// the session.
  void set_terrain(const Grid& terrain) noexcept { terrain_ = terrain; }
  [[nodiscard]] const Grid& terrain() const noexcept { return terrain_; }

  /// The connected-region partition and the AI's node table over it.
  ///
  /// **Both are map-derived configuration, like the class graph and the group
  /// table**: a pure function of the terrain layer and the settlement list, so
  /// a load rebuilds them rather than restoring them, and neither is hashed.
  /// `sim/lsa.hpp` and `sim/gaika.hpp` say what each is and, for the node
  /// table, exactly which half of it is an approximation.
  ///
  /// They live here rather than on a system because two domains read them --
  /// the AI and the economy -- and neither owns the other. Building them is
  /// `GameSession`'s job, after the settlements exist.
  [[nodiscard]] LsaPartition& mutable_lsa() noexcept { return lsa_; }
  [[nodiscard]] const LsaPartition& lsa() const noexcept { return lsa_; }
  [[nodiscard]] GaikaTable& mutable_gaika() noexcept { return gaika_; }
  [[nodiscard]] const GaikaTable& gaika() const noexcept { return gaika_; }

  /// Every gate's line, which a unit's route search lays for the gates that
  /// bar it. Derived: never hashed, never saved, learnt from the gates as
  /// they appear and forgotten by `deserialize`. See `sim/gate.hpp`.
  [[nodiscard]] GateLines& gate_lines() noexcept { return gate_lines_; }
  [[nodiscard]] const GateLines& gate_lines() const noexcept { return gate_lines_; }

  /// The map's height layer, or an unparsed grid.
  ///
  /// `Maps/<n>/Terrain.height.grid`: **32** units per cell, one byte per cell,
  /// 0 is sea level and the full 0..255 range is used. The only consumer in
  /// the simulation is `GetTerrainHeight`, which bilinearly interpolates it,
  /// and the only consumers of *that* are the birds, whose altitude arithmetic
  /// stays inside the layer's own scale from end to end.
  ///
  /// Same standing as `terrain()` beside it: configuration, not state -- it
  /// comes from the map, is identical on both sides of a save, and is neither
  /// hashed nor serialised. Held by value because a `Grid` is a header and a
  /// span over bytes the caller owns.
  ///
  /// A world with no layer answers 0 everywhere, which is sea level, which is
  /// what every session did before this field existed.
  void set_height(const Grid& height) noexcept { height_ = height; }
  [[nodiscard]] const Grid& height() const noexcept { return height_; }
  [[nodiscard]] const ClassGraph* class_graph() const noexcept { return classes_; }

  /// Bind the entity resolver, for the spawns that happen *after* load.
  ///
  /// `populate_from_map` and `deserialize` each take one as an argument,
  /// because each is handed everything it needs by its caller. A script that
  /// spawns mid-match is not: `Place` mints an object from a class name with
  /// nothing but a `CallContext` in scope, and an object spawned with a null
  /// entity is an object with no art and no animation timeline.
  ///
  /// Same contract as the class graph beside it -- not owned, expected to
  /// outlive the world, load-time state rather than simulation state. It is
  /// **not hashed and not serialised**, for the reason `serialize` already
  /// gives about the resolved `Entity*`: an address in a save is a save that
  /// only loads in the process that wrote it. Optional, and a world without
  /// one spawns objects with no entity, which is the same honest degradation a
  /// null resolver gives at load.
  void set_entity_resolver(const EntityResolver* entities) noexcept { entities_ = entities; }
  [[nodiscard]] const EntityResolver* entity_resolver() const noexcept { return entities_; }

  /// Whether `id`'s class is `base` or descends from it -- the class *tree*
  /// test the dumps' queries perform, not a leaf comparison.
  [[nodiscard]] bool class_is_a(ObjectId id, ClassIndex base) const noexcept;
  [[nodiscard]] bool matches_filter(const WorldObject& slot, const ClassFilter& filter) const noexcept;

  // -- objects -----------------------------------------------------------

  /// Spawn an object of native class `id` bound to `entity`, which may be null
  /// for a class that names no entity. Ids are assigned from 1 in spawn order
  /// and are never reused, so a stale handle reads as missing rather than as
  /// somebody else.
  ///
  /// `class_index` is the class graph entry the object was spawned from, and is
  /// what class filters test. It may be `kNoClass`.
  ObjectId spawn(NativeClass id, const Entity* entity, ClassIndex class_index = kNoClass);

  /// Spawn an engine-internal object: a settlement, a holder, a warehouse, an
  /// item, or one of the singletons. Prefer the composite allocators below
  /// where one applies -- allocation order is observable state.
  ObjectId spawn_internal(InternalKind kind);

  /// Bring one spawn template into play: mint a copy of `templ` and give it
  /// the template's class, position, owner, health, settlement and
  /// **memberships**. Returns `kNoObject` when `templ` is not a live,
  /// non-internal object carrying `ObjectFlags::unspawned`.
  ///
  /// This is `CVXGroup::Spawn`'s per-template body (`0x00572760`, the loop from
  /// `0x005727ab`), which does five things to each copy and this does the same
  /// five:
  ///
  ///   1. mints it from the template through a virtual, so it is the template's
  ///      class rather than a class named by the caller;
  ///   2. clears the unspawned bit on the copy (`vtbl + 0x48` with
  ///      `0x08000000`), which is what makes the copy -- and not the template
  ///      -- the thing every later `CVXGroup::Add` files as live;
  ///   3. places it at *the template's own* coordinates, read back through the
  ///      template's `vtbl + 0x3c` and written through the copy's `+ 0x38`;
  ///   4. walks the named-object table and rebinds to the copy every name bound
  ///      to the template -- which is why `NO_Scipio` resolves only after
  ///      `SpawnGroup("Q_Scipio")` has run;
  ///   5. walks the *whole group table* and, for every group whose template
  ///      half holds the template, adds the copy. So a copy inherits every
  ///      membership its template had, not just the group being spawned.
  ///
  /// `holder` is `SpawnGroupInHolder`'s second argument: when it is not
  /// `kNoObject` the copy is placed inside it rather than at the template's
  /// coordinates, which for a held object means `kHeldPosition` and a holder
  /// link. The template keeps its own position either way.
  ///
  /// **The copy does not get its idle script here.** Starting scripts needs the
  /// class graph and the chunk library, which are `GameSession`'s; it sweeps
  /// for objects with no script at the top of each turn.
  ObjectId spawn_from_template(ObjectId templ, ObjectId holder = kNoObject);

  /// Mint one object of `class_index`, the way a *script* spawns rather than
  /// the way a map does.
  ///
  /// `Place` and `_PlaceEx` name a class and get an object; `SpawnGroup` copies
  /// a template and `populate_from_map` reads authored attributes, so this is
  /// the third and last way an object comes into being, and the only one with
  /// no authored record behind it. What the map would have supplied it takes
  /// from the class instead: the native class from `cpp_class`, the art from
  /// the bound entity resolver, and full `maxhealth` -- a placed object is
  /// undamaged, because there is no `healthperc` for it to carry.
  ///
  /// Position and owner are deliberately **not** set. Both entry points write
  /// them afterwards through `SetPlayer` and `SetPos`, in that order, and
  /// `_PlaceEx` sets the owner only on the branch where the class is a script
  /// object -- so folding either in here would make that asymmetry
  /// unreachable. See `sim/world_host.cpp`.
  ///
  /// A ship gets its holder at `handle + 1`, as at load: 3 of 3 in the corpus,
  /// and the only place a standalone holder occurs. Returns `kNoObject` when
  /// the class is not in the graph, or its `cpp_class` names no native class.
  ObjectId spawn_of_class(ClassIndex class_index);

  /// `Unit::Mutate`: **this object becomes one of `to`, keeping its id.**
  ///
  /// The original (0x005deef0) does it by replacement -- it mints a fresh
  /// object of the new class, copies a list of fields into it, destroys the
  /// old one, and then *swaps the handles* so that every script reference and
  /// every group membership still names the survivor. Ids here are stable and
  /// never reused, so the swap is the one part that does not translate: the
  /// slot is re-classed in place instead, which reaches the same place by the
  /// road this object model already has.
  ///
  /// **What survives**, and each is a field the copy names: `position`,
  /// `owner`, `health` (`[obj+0xc0]`), `stamina` (`[obj+0xc4]`) and `holder`
  /// (`[obj+0x154]`), plus two flag bits of the `+0x2c` word -- bit 19,
  /// `in_party`, and bit 26, `messenger`. Everything keyed by object id
  /// survives because the id does: the unit's food (`[obj+0x16c]`, the
  /// `Feeder`'s), its experience (`[obj+0x180]`, which the copy carries and
  /// which `inherentlevel` is derived from), its carried items, its name
  /// binding, its group memberships and its settlement.
  ///
  /// **What does not**: everything class-derived is the new class's --
  /// `sight`, the entity behind the art, and every combat profile figure
  /// including the maximum health, so a unit whose health was above the new
  /// class's maximum keeps the higher number until something clamps it. And
  /// everything that belonged to the destroyed object is gone: the rest of
  /// `ObjectState` (`user` at `[obj+0x164]` is conspicuously *not* in the copy
  /// list), the command queue, the running script, and the squad -- the copy
  /// writes `[new+0x174] = 0` outright.
  ///
  /// **Six copied fields are not modelled here** and are recorded rather than
  /// invented: `[obj+0x84]`, `[obj+0x178]`, `[obj+0x18c]`, `[obj+0x1a8]`,
  /// `[obj+0x1ac]`, the string at `[obj+0xa0]`, the point pair at
  /// `[obj+0x48]`/`[obj+0x4c]`, and bit 16 of the `+0x2c` word.
  ///
  /// Refuses -- and changes nothing -- for an unknown class, a class whose
  /// `cpp_class` names no native class, an internal object, and an object that
  /// is not there. **Callers must reconcile the systems** that cache a class:
  /// `sim/world_host.cpp`'s `Unit::Mutate` is the one that does.
  bool mutate_class(ObjectId id, ClassIndex to);

  /// Every member of `group` that is a spawn template, in stored order --
  /// `CVXGroup`'s deque at `+0x54`. This is what `SpawnGroup` iterates.
  ///
  /// Held as one list plus a per-object flag rather than as the original's two
  /// deques. The split is a partition of the same sequence, and `Add` appends
  /// to whichever half the flag selects, so filtering one list on read
  /// reproduces both halves *and* their orders exactly, with one place for a
  /// membership to live instead of two that can disagree.
  [[nodiscard]] std::size_t templates_in_group(std::int32_t group,
                                               std::vector<ObjectId>& out) const;

  /// Remove an object. Surviving objects keep their relative order, and the id
  /// is never handed out again: the dumps' handle density falls monotonically
  /// with game age (0.93 at tick 2, 0.052 after 8,569 ticks), so gaps
  /// accumulate and are never refilled.
  bool despawn(ObjectId id);

  [[nodiscard]] std::span<const WorldObject> objects() const noexcept { return objects_; }
  [[nodiscard]] std::size_t size() const noexcept { return objects_.size(); }
  [[nodiscard]] const WorldObject* find(ObjectId id) const noexcept;
  [[nodiscard]] WorldObject* find(ObjectId id) noexcept;

  /// `Erase` on the object whose script is running, held until the slice ends.
  ///
  /// `gbr.exe` keeps a one-bit latch at `[0x009bdb14]`: `CObject::Erase`
  /// (0x005a7580) opens by comparing the receiver against the currently-running
  /// script object at `[0x00a77ebc]`, and when they are the same it sets the
  /// latch and returns having destroyed nothing. The runner then calls the
  /// object's `vtbl[0x24]` (0x005a7ac0) after the bytecode returns, which reads
  /// the latch, performs the erase for real, and clears it.
  ///
  /// The id is kept here where the original keeps a bit, because the original
  /// recovers the object from the callback's own receiver and this engine has
  /// no receiver to recover it from. The two agree on every reachable path.
  ///
  /// **Not hashed and not serialised**, and neither is the original's latch: it
  /// is set and consumed inside one script step, so no save and no hash of a
  /// world between turns can ever see it armed. `sim/world_host.cpp` installs
  /// the scheduler hook that drains it.
  void defer_erase(ObjectId id) noexcept { deferred_erase_ = id; }
  [[nodiscard]] ObjectId deferred_erase() const noexcept { return deferred_erase_; }
  void clear_deferred_erase() noexcept { deferred_erase_ = kNoObject; }

  /// The id the next spawn will use. Exposed so a composite allocation can be
  /// asserted contiguous rather than assumed to be.
  [[nodiscard]] ObjectId next_id() const noexcept { return next_id_; }

  // -- object state ------------------------------------------------------

  [[nodiscard]] const ObjectState* state(ObjectId id) const noexcept;
  /// Direct mutable access, for systems that own a field outright. Prefer the
  /// named mutators below for `position` and `holder`, whose invariant is not
  /// local to either field.
  [[nodiscard]] ObjectState* mutable_state(ObjectId id) noexcept;

  /// Move an object to `to`.
  ///
  /// **Refuses on a held object.** A garrisoned object's position is not
  /// stored: the original writes `(-1, -1)` and its location *is* its holder's.
  /// Of 1,225 objects at `(-1, -1)` in the dumps 1,217 have a holder, and of
  /// the 6,355 with a real position **none** does. Writing a shadow position
  /// under a holder would give two answers to one question, and the
  /// disagreement would be a desync -- so take the object out of the holder
  /// first.
  bool set_position(ObjectId id, Point to) noexcept;

  bool set_owner(ObjectId id, PlayerId owner) noexcept;

  /// Set an object's health, and **re-tier it**.
  ///
  /// The tier is `ObjectState::damage_state`, and it has hysteresis, so it
  /// cannot be recomputed on demand by a reader: the answer depends on the
  /// tier before it. The original recomputes on the damage path, through a
  /// virtual (0x004db3d0) called from the building constructor and from
  /// whatever changed the health; this is that recompute, and **this is the
  /// only place health changes in this engine** -- `CombatSystem` publishes
  /// through it too, so a struck wall re-tiers in the same statement the
  /// damage lands in.
  bool set_health(ObjectId id, std::int32_t health) noexcept;
  bool set_stamina(ObjectId id, std::int32_t stamina) noexcept;

  /// Put `id` inside `holder`, which drops its position to `kHeldPosition`.
  /// Fails if either object is missing, if they are the same object, or if the
  /// holder is itself inside `id` -- a containment cycle has no position at all.
  bool put_in_holder(ObjectId id, ObjectId holder) noexcept;

  /// Take `id` out of its holder and place it at `at`.
  bool remove_from_holder(ObjectId id, Point at) noexcept;

  /// Where an object actually is: its own position, or -- following the holder
  /// chain -- its holder's. `kHeldPosition` when the chain does not bottom out
  /// in a positioned object.
  [[nodiscard]] Point resolve_position(ObjectId id) const noexcept;

  /// The original's `SyncFlags` word for an object, for diffing against a dump.
  [[nodiscard]] std::uint32_t sync_flags(ObjectId id) const noexcept;

  // -- composite allocation ----------------------------------------------
  //
  // The rigidest structural finding in the dumps, and the reason these exist
  // rather than three separate calls at each site: **allocation order is
  // observable state**, so the grouping has to be reproduced, not merely the
  // objects.

  /// Settlement, Holder, Warehouse at three consecutive ids.
  ///
  /// 328 of 328 across all nine dumps, no exceptions: a `CVXSettlement` is
  /// always immediately followed by a `CVXHolder` at handle+1 and a
  /// `CVXWarehouse` at handle+2. The three are allocated as one unit when a
  /// settlement is created.
  struct SettlementIds {
    ObjectId settlement = kNoObject;
    ObjectId holder = kNoObject;
    ObjectId warehouse = kNoObject;
  };
  SettlementIds spawn_settlement(PlayerId owner);

  /// A ship and the holder that carries its boarded units, at two consecutive
  /// ids. 3 of 3 in the corpus; the three orphan `CVXHolder`s in the dumps are
  /// exactly these, each at handle+1 of a `CVXShip`.
  struct ShipIds {
    ObjectId ship = kNoObject;
    ObjectId holder = kNoObject;
  };
  ShipIds spawn_ship(const Entity* entity, ClassIndex class_index = kNoClass);

  /// AIHelper, PlayerBonus, PlayerScripts at three consecutive ids. Exactly one
  /// group per session in all nine dumps (779/780/781, 640/641/642, ...).
  ///
  /// **Their fields are the largest single gap in the evidence**: all three
  /// print only a handle, so their state is real -- they are inside the hashed
  /// slot table -- and entirely invisible. They exist here to occupy the right
  /// handles, and hold nothing.
  struct SingletonIds {
    ObjectId ai_helper = kNoObject;
    ObjectId player_bonus = kNoObject;
    ObjectId player_scripts = kNoObject;
  };
  SingletonIds spawn_singletons();

  // -- queries -----------------------------------------------------------

  /// Create a query object. It takes a handle from the same counter as
  /// everything else, and that is the point: **a stateless `find()` would
  /// desync**, because the allocation feeds the slot hash.
  ObjectId create_query(const QuerySpec& spec);

  [[nodiscard]] const QuerySpec* query_spec(ObjectId id) const noexcept;
  [[nodiscard]] QuerySpec* mutable_query_spec(ObjectId id) noexcept;
  /// Query objects live and die like everything else; this is `despawn`.
  bool destroy_query(ObjectId id) { return despawn(id); }

  /// Evaluate a query into `out`, which is cleared first. Ids come out in
  /// ascending order, which is spawn order. Returns the count.
  ///
  /// Evaluation is not cached: an invalidation rule is not recoverable from the
  /// corpus, and a wrong one is worse than none. The *object* persists, which
  /// is the part the hash can see.
  std::size_t evaluate_query(ObjectId id, std::vector<ObjectId>& out) const;

  /// How deeply set-op queries may nest before evaluation gives up and returns
  /// empty. A script can compose a query with itself, directly or round a
  /// longer loop, and the dumps say nothing about what the original did; a
  /// fixed bound is at least identical on every peer, which a stack overflow
  /// is not. The deepest composition in the corpus is a set op over two set
  /// ops, so this is generous by an order of magnitude.
  static constexpr std::uint32_t kMaxQueryDepth = 32;

  // -- spatial queries ---------------------------------------------------
  //
  // All of these clear `out` and append in ascending id order.

  /// Objects standing within `radius` of `center`. **A held object is not
  /// found**, by this or any other area query: it stands at `(-1, -1)` in no
  /// grid cell, as in the original (`sim/spatial_index.hpp`).
  std::size_t objects_in_radius(Point center, std::int32_t radius, const ClassFilter& filter,
                                std::vector<ObjectId>& out) const;

  /// Objects standing inside an axis-aligned box, **corners inclusive**, and
  /// no held object, exactly as `objects_in_radius`.
  ///
  /// The corners are taken as given. `objs_in_rect` normalises them when it
  /// builds the spec, which is the one place a map's authoring order reaches.
  std::size_t objects_in_rect(std::int32_t left, std::int32_t top, std::int32_t right,
                              std::int32_t bottom, const ClassFilter& filter,
                              std::vector<ObjectId>& out) const;

  /// Objects within `observer`'s sight radius of its own stored position,
  /// excluding the observer itself -- so a held observer looks out from
  /// `(-1, -1)`, as 0x004ff080 does.
  std::size_t objects_in_sight(ObjectId observer, const ClassFilter& filter,
                               std::vector<ObjectId>& out) const;

  /// Objects of a class owned by a player, optionally bounded by a circle.
  /// `radius <= 0` means the whole map, and `player == kNoPlayer` any owner.
  std::size_t objects_of_class_for_player(const ClassFilter& filter, PlayerId player, Point center,
                                          std::int32_t radius, std::vector<ObjectId>& out) const;

  /// The same, bounded by an axis-aligned box instead of a circle. Corners
  /// inclusive, exactly as `objects_in_rect`.
  std::size_t objects_of_class_for_player_in_rect(const ClassFilter& filter, PlayerId player,
                                                  std::int32_t left, std::int32_t top,
                                                  std::int32_t right, std::int32_t bottom,
                                                  std::vector<ObjectId>& out) const;

  /// Objects whose owner stands in a given relation to `viewer`.
  ///
  /// `flags_type` is `CVXPlayerFlagsQuery`'s: 1 for `FriendlyObjs`, 2 for
  /// `EnemyObjs`, 3 for `ControllableObjs`. See the implementation for which
  /// half of that is proven and which is inferred.
  std::size_t objects_by_relation(const ClassFilter& filter, PlayerId viewer,
                                  std::int32_t flags_type, std::vector<ObjectId>& out) const;

  /// The `CVXUnitsInSettlementQuery` sweep, in all three of its scopes.
  ///
  /// `scope` is the mode field the original interns on; see `SettlementScope`
  /// in `sim/query.hpp`. The two halves it selects between are collected by
  /// different rules and only the second of them is sorted, which is the
  /// original's asymmetry rather than an oversight -- `Settlement::CollectUnits`
  /// (`gbr.exe` 0x005c58f0) runs `std::sort` (0x004fb510) and `std::unique`
  /// (0x0050e300) over the result **inside the ring branch**, and the garrison
  /// branch's early exit at 0x005c5a0a jumps clean past both.
  std::size_t units_in_settlement(ObjectId settlement, const ClassFilter& filter,
                                  std::vector<ObjectId>& out,
                                  SettlementScope scope = SettlementScope::garrison) const;
  std::size_t buildings_in_settlement(ObjectId settlement, const ClassFilter& filter,
                                      std::vector<ObjectId>& out) const;

  /// Every non-internal object standing in the box, corners inclusive, in
  /// spawn order and with **no other test** -- a spawn template is included --
  /// beyond the one every area query makes: a held object stands nowhere. The raw sweep the filtered queries above are
  /// built on, for a caller whose own rule differs from theirs -- `EnemyInRange`
  /// is one.
  std::size_t objects_located_in_rect(std::int32_t left, std::int32_t top, std::int32_t right,
                                      std::int32_t bottom, std::vector<ObjectId>& out) const;

  /// True when the spatial index agrees with the object table. The simulation
  /// never asks; the tests do, and a build configured with
  /// `-DIMPERIVM_SPATIAL_CHECK=ON` checks every query's answer against the
  /// straight scan besides.
  [[nodiscard]] bool spatial_index_consistent() const {
    return index_.consistent(objects_);
  }

  // -- named object groups -----------------------------------------------

  /// The live group table. See `GroupTable` above for why it is state.
  [[nodiscard]] GroupTable& groups() noexcept { return groups_; }
  [[nodiscard]] const GroupTable& groups() const noexcept { return groups_; }

  /// The index `Group("name")` should put in a `QuerySpec`, interning the name
  /// if it is new. Mutating, because interning is; use `groups().find(name)`
  /// for a lookup that must not create.
  std::int32_t group_index(std::string_view name) { return groups_.intern(name); }

  /// The members of a group that the world still holds, ascending by id and
  /// passed through `filter`.
  ///
  /// Internal objects are excluded by the shared `collect`, as they are from
  /// every other query. That costs nothing here: a group's members are
  /// `<scriptobj>`s and units scripts placed, and neither is internal.
  std::size_t objects_in_group(std::int32_t group, const ClassFilter& filter,
                               std::vector<ObjectId>& out) const;

  /// The named objects: `<group type="0">`, which `GetNamedObj` reads and
  /// `Group` does not. See `NamedObjectTable` above.
  [[nodiscard]] RectTable& rects() noexcept { return rects_; }
  [[nodiscard]] const RectTable& rects() const noexcept { return rects_; }

  [[nodiscard]] NamedObjectTable& named_objects() noexcept { return named_objects_; }
  [[nodiscard]] const NamedObjectTable& named_objects() const noexcept {
    return named_objects_;
  }

  /// What `GetNamedObj(name)` answers: the object bound to `name`, or
  /// `kNoObject` when nothing is.
  ///
  /// **The handle may be dead.** `NamedObj::IsDead` exists in `gbr.exe`, so the
  /// binding outlives the object and testing it is the caller's job; this does
  /// not filter. `named_object_alive` is the filtered form where a caller wants
  /// one.
  [[nodiscard]] ObjectId named_object(std::string_view name) const noexcept {
    return named_objects_.object(name);
  }
  /// As above, but `kNoObject` when the world no longer holds the object.
  [[nodiscard]] ObjectId named_object_alive(std::string_view name) const noexcept;

  // -- boarding ----------------------------------------------------------

  /// Which units each ship is waiting for. See `sim/boarding.hpp`: it is a
  /// relation between objects that no object owns, which is what `GroupTable`
  /// and `NamedObjectTable` beside it are too, and it is world state for the
  /// same reason -- a script loops on `ship.NumUnitsToBoard`.
  [[nodiscard]] const BoardingTable& boarding() const noexcept { return boarding_; }
  [[nodiscard]] BoardingTable& mutable_boarding() noexcept { return boarding_; }

  /// Everything a holder contains, directly. Ascending id order.
  std::size_t contents_of(ObjectId holder, std::vector<ObjectId>& out) const;

  /// Squared distance between two objects' resolved positions, in world units.
  /// 64-bit because a full-map diagonal squared is about 5.4e8 and a sum of two
  /// of those would not be, and an overflow is a desync rather than a wrong
  /// number. `-1` when either object is missing.
  [[nodiscard]] std::int64_t distance_squared(ObjectId a, ObjectId b) const noexcept;

  // -- populating from a map ---------------------------------------------

  /// What `populate_from_map` did, for reporting and for conformance diffing.
  struct PopulateReport {
    std::size_t map_objects = 0;       ///< `<scriptobj>` entries seen
    std::size_t map_settlements = 0;   ///< `<settlement>` entries seen
    std::size_t spawned = 0;           ///< data-driven objects created
    std::size_t settlements = 0;       ///< settlement composites created
    std::size_t holders = 0;           ///< holders created (settlements + ships)
    std::size_t warehouses = 0;
    std::size_t ships = 0;
    std::size_t singletons = 0;        ///< always 3
    std::size_t unresolved_class = 0;  ///< class not in the graph; not spawned
    std::size_t map_groups = 0;        ///< `<group>` elements seen, both types
    std::size_t groups = 0;            ///< type-1 names interned
    std::size_t named_objects = 0;     ///< type-0 names bound
    std::size_t memberships = 0;       ///< `<obj num>` references made good
    /// `<obj num>` references whose object was not spawned -- an unresolved
    /// class. Zero on every shipped map that this world can spawn in full.
    std::size_t unresolved_members = 0;
    /// Type-0 elements whose name was already bound. Non-zero on exactly the
    /// four maps that repeat an alias name; see `NamedObjectTable::bind`.
    std::size_t duplicate_names = 0;
    /// Teleports whose `destination_set` resolved to a spawned teleport. 54 of
    /// 54 across the retail install; see `ObjectState::teleport_destination`.
    std::size_t teleport_pairs = 0;
    std::size_t total_objects = 0;     ///< the world's object count afterwards
    /// The settlement object each `<settlement>` element became, index-aligned
    /// with `MapObjectList::settlements()` and `kNoObject` where the element
    /// was never referenced by an object and so never allocated.
    ///
    /// A join, not a count. The loader allocates the settlement triple lazily,
    /// on first reference, so settlement ids are in order of first *use* rather
    /// than in document order and a positional match against the map would be
    /// wrong. `GameSession::seed_economy` needs the pairing to carry the map's
    /// `<settlement name>` onto `Settlement::name`, and this is where the
    /// loader already knows it.
    std::vector<ObjectId> settlement_ids;
    /// The object each `<scriptobj>` became, index-aligned with
    /// `MapObjectList::objects()` and `kNoObject` where the class did not
    /// resolve. The editor's join back to the authored list.
    std::vector<ObjectId> object_ids;
  };

  /// Spawn simulated objects for every object a map places.
  ///
  /// **Allocation order is observable state**, and the tick-2 dumps show it
  /// directly. Reading handles off the ASUS dump:
  ///
  ///     0 Settlement, 1 Holder, 2 Warehouse, 3..13 that settlement's objects,
  ///    14 Settlement, 15 Holder, 16 Warehouse, 17..58 the next one's, ...
  ///   787 AIHelper, 788 PlayerBonus, 789 PlayerScripts,
  ///   831.. the query objects, up to the maximum handle of 900
  ///
  /// so the order reproduced here is:
  ///
  ///   1. Every `<scriptobj>` in document order, with a settlement's
  ///      Settlement/Holder/Warehouse triple minted immediately *before* the
  ///      first object that belongs to it. The triple is contiguous in 328 of
  ///      328 cases across all nine dumps. A ship takes a holder at its id + 1.
  ///   2. Any settlement no object referred to.
  ///   3. The three per-session singletons, as one contiguous group.
  ///
  /// Queries are deliberately **not** minted here: they are created by running
  /// scripts, which is why they sit above the singletons in every dump.
  ///
  /// The map's `player` attribute is **1-based** (`player="1"` is slot 0) and 0
  /// means unowned; that conversion happens here.
  /// One `<scriptobj>` into the world at `at`, owned by 1-based `player` (0
  /// for none), linked to `settlement`: the per-object half of
  /// `populate_from_map`, with the authored health, `unspawned`, `no_ai` and
  /// `in_air` bits read the same way. `kNoObject` when the class graph does
  /// not know the class or its `cpp_class`; `*was_ship` tells the caller a
  /// holder came with it. The class graph is the world's own (`set_class_graph`).
  ///
  /// Public because a settlement template is a list of these placed at an
  /// offset (`sim/mutable_settlement.hpp`), and a second copy of the health and
  /// flag rules would be a second place for them to drift.
  ObjectId spawn_map_object(const MapObject& placed, Point at, ObjectId settlement,
                            std::int32_t player, const EntityResolver* entities,
                            bool* was_ship = nullptr);

  PopulateReport populate_from_map(const MapObjectList& map, const ClassGraph& graph,
                                   const EntityResolver* entities = nullptr);

  // -- systems -----------------------------------------------------------

  /// Register a system. Not owned: a system outlives no world and owning it
  /// here would put four domains' allocation into one vector.
  ///
  /// **Registration order is the run order and is part of the simulation's
  /// definition.** Changing it changes results, and it changes the hash, which
  /// is deliberate -- a silently reordered pipeline is a desync you cannot see.
  /// Registering the same system twice, or a null one, is refused.
  ///
  /// The world folds each system's index and `name()` into the hash before
  /// calling its `hash`, so order shows up even when a system's own
  /// contribution is commutative -- an XOR or a sum, which are the two obvious
  /// things to write. A domain does not have to think about this.
  bool add_system(System* system);

  [[nodiscard]] std::span<System* const> systems() const noexcept { return systems_; }

  /// Call `start()` on every system, in registration order. Once, after the
  /// world is populated and before the first turn.
  void start();

  // -- players -----------------------------------------------------------

  /// The sixteen players and the relations between them.
  ///
  /// A world member rather than a system's private state because three
  /// unrelated domains read it -- combat cannot pick a target, the economy
  /// cannot decide who a tribute may go to, and the AI cannot pick an
  /// objective, without knowing who is whose enemy -- and because it is
  /// serialised and hashed with the rest of the world. See `sim/player.hpp`.
  [[nodiscard]] PlayerTable& players() noexcept { return players_; }
  [[nodiscard]] const PlayerTable& players() const noexcept { return players_; }

  // -- script collections ------------------------------------------------

  /// The `ObjList`s the running scripts hold. See `sim/objlist.hpp` for why
  /// their lifetime is keyed by declaration site rather than by execution.
  [[nodiscard]] ObjListPool& objlists() noexcept { return objlists_; }
  [[nodiscard]] const ObjListPool& objlists() const noexcept { return objlists_; }

  /// The `IntArray`s and `StrArray`s every live script holds. See
  /// `sim/array.hpp`; the pool is here for the same reason `objlists_` is.
  /// The squad lists every live script holds. Same standing as `objlists()`
  /// and `arrays()`: saved, not hashed, released with the script.
  [[nodiscard]] SquadListPool& squadlists() noexcept { return squadlists_; }
  [[nodiscard]] const SquadListPool& squadlists() const noexcept { return squadlists_; }

  /// The conversations every live script holds -- what `Init` bound and what
  /// `SetActor` bound to it. Same standing again: saved, not hashed, released
  /// with the script. See `sim/conversation.hpp`.
  [[nodiscard]] ConversationPool& conversations() noexcept { return conversations_; }
  [[nodiscard]] const ConversationPool& conversations() const noexcept { return conversations_; }

  [[nodiscard]] ArrayPool& arrays() noexcept { return arrays_; }
  [[nodiscard]] const ArrayPool& arrays() const noexcept { return arrays_; }

  // -- randomness --------------------------------------------------------

  [[nodiscard]] Rng& rng() noexcept { return rng_; }
  [[nodiscard]] const Rng& rng() const noexcept { return rng_; }
  /// The dump's `syncseed`.
  void seed(std::uint32_t value) noexcept { rng_.seed(value); }

  /// The dump's `cmdidseed`: the next command id to allocate, monotone, from
  /// the same counter family as the command-queue entry ids. Not a generator.
  [[nodiscard]] std::uint32_t command_id_seed() const noexcept { return next_command_id_; }
  void set_command_id_seed(std::uint32_t value) noexcept { next_command_id_ = value; }
  std::uint32_t next_command_id() noexcept { return next_command_id_++; }

  // -- animation ---------------------------------------------------------
  //
  // The shape the script VM's `PlayAnim` host function will bind to. It is not
  // bound here -- that is the VM's side of the boundary -- but nothing below
  // needs anything the VM cannot supply: an object handle, a slot number and a
  // repeat mode.

  /// Start animation `slot` from its first frame.
  ///
  /// Returns false when the entity declares no such slot, which is **ordinary
  /// and not an error**: shipped `.vs` code calls `PlayAnim(0, ...)` and
  /// `PlayAnim(16, ...)`, and no entity in the retail data declares either. The
  /// cursor is left alone in that case rather than blanked, so a failed script
  /// call cannot freeze a unit mid-walk.
  bool play_anim(ObjectId id, std::int32_t slot, AnimRepeat repeat = AnimRepeat::hold);

  /// Enter `state_idx` and loop whatever animation the state names, if any.
  /// A state with `anim_idx == kNoAnim` -- 987 of them -- is a still pose, and
  /// leaves the object not animating.
  bool enter_state(ObjectId id, std::int32_t state_idx);

  /// Stop animating and hold the current frame.
  bool stop_anim(ObjectId id);

  /// Whether a slot would resolve. Same null-tolerant contract as `play_anim`.
  [[nodiscard]] bool has_anim(ObjectId id, std::int32_t slot) const noexcept;

  // -- the loop ----------------------------------------------------------

  /// Run one turn at the current length.
  const Turn& advance();
  /// Run one turn of a declared length, which becomes the current length.
  /// This is how the lockstep layer renegotiates: it hands over the length the
  /// peers agreed on for this turn.
  const Turn& advance(std::int32_t turn_length);
  /// Run a sequence of declared turns. Indistinguishable from running them one
  /// call at a time -- that is the property `test_tick.cpp` asserts.
  void advance(std::span<const std::int32_t> turn_lengths);
  /// Run `count` turns at the current length.
  void advance_turns(std::uint64_t count);

  /// FNV-1a over the ordered simulation state: the conformance harness's
  /// comparison value, and what the step-equivalence test checks.
  ///
  /// Covers exactly what a turn can change. It is deliberately not a hash of
  /// the whole object -- the entity pointer is load-time state, and an address
  /// in a hash is not reproducible between runs.
  ///
  /// This is the `slots` channel of the original's `[HASHES]` block, which is
  /// one of only three that is ever non-zero across the nine dumps (`slots`,
  /// `threads`, `netcmds`; `extrahash` fires in one). `pathfinder`,
  /// `exploration`, `scriptstate` and `aihash` are zero in every dump -- the
  /// shipped build deliberately kept those subsystems out of the determinism
  /// contract, and nothing here should ever pull them in.
  [[nodiscard]] std::uint64_t state_hash() const noexcept;

  /// The whole `[HASHES]` block, for diffing against a dump. `slots` is
  /// `state_hash()`; the four permanently-zero channels stay zero.
  [[nodiscard]] WorldHashes hashes() const noexcept;

  // -- the saved game ----------------------------------------------------
  //
  // The world's own round trip. Format and rationale: docs/formats/save.md,
  // and `sim/save.hpp` for the envelope this section lives in.

  /// Append this world's state to `out`, as one self-describing section with
  /// its own magic and version -- the shape `Scheduler::serialize` established.
  ///
  /// **What is written is everything a turn can change, plus everything a turn
  /// reads that a reload could not recompute.** That is a wider set than
  /// `state_hash` covers: the player table is not hashed and is written all the
  /// same, because the alternative is a field that silently comes back as a
  /// default. The per-object sight radius used to be in that set too and no
  /// longer is -- `Obj::SetSight` made it turn state, so it is now hashed as
  /// well as written. See the format document for the field-by-field list and
  /// for what is deliberately left out.
  ///
  /// What is *not* written is load-time state that the class graph and the
  /// entity resolver reproduce exactly: the resolved `AnimTimeline`, the
  /// `Entity*`, and the class-graph pointer itself. An address in a save is a
  /// save that only loads in the process that wrote it.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this world's state with the one in `bytes`.
  ///
  /// The class graph must already be bound (`set_class_graph`) if the save has
  /// objects with classes: entity paths and animation timelines are re-resolved
  /// through it, exactly as `populate_from_map` resolves them. `entities` may
  /// be null, which restores every object with no entity -- the same honest
  /// degradation a null resolver gives at load.
  ///
  /// Registered systems are left alone: they are not owned here, and the save
  /// carries their state in their own sections. The object table, the queries,
  /// the groups, the names, the clock, the RNG and the players are replaced.
  ///
  /// **Atomic.** Everything is decoded into locals first, so a truncated or
  /// malformed save leaves the world exactly as it was.
  Status deserialize(std::span<const std::byte> bytes, EntityResolver* entities = nullptr);

 private:
  void run_turn(std::int32_t length);
  /// Append `id` to `out` if it passes `filter` and is not an internal object.
  void collect(const WorldObject& slot, const ClassFilter& filter,
               std::vector<ObjectId>& out) const;
  std::size_t evaluate_query(ObjectId id, std::vector<ObjectId>& out, std::uint32_t depth) const;
  ObjectId allocate(InternalKind kind, std::unique_ptr<NativeObject> object,
                    const Entity* entity, ClassIndex class_index);
  /// The shared body of every spatial query: `test(slot, at)` over every
  /// non-internal object whose resolved position `at` lies in `box`, and
  /// `collect` over those it accepts, in spawn order. `test` must imply
  /// `box.contains(at)`. Candidates come from the grid when the box is small
  /// enough to be worth it, and from a straight scan otherwise; the answer is
  /// the same either way. `filtered` false skips `collect`'s own tests.
  template <typename Test>
  std::size_t sweep(const SpatialBox& box, const ClassFilter& filter, bool filtered,
                    std::vector<ObjectId>& out, Test&& test) const;
  template <typename Test>
  std::size_t scan(const ClassFilter& filter, bool filtered, std::vector<ObjectId>& out,
                   Test&& test) const;
  void reindex(WorldObject& slot) { index_.file(slot); }
  void rebuild_index();
  [[nodiscard]] std::int32_t sight_for_class(ClassIndex index) const noexcept;

  Clock clock_{};
  /// Spawn order, which is ascending id order: `despawn` erases and never
  /// reorders, so `find` is a binary search and every iteration is stable.
  std::vector<WorldObject> objects_;
  ObjectId next_id_ = 1;
  /// Derived from `objects_`: never hashed, never saved, rebuilt on load.
  SpatialIndex index_;

  /// Query specs, indexed by `WorldObject::query`. Grows only; a destroyed
  /// query leaves its slot behind rather than compacting, because compaction
  /// would renumber every live query and the numbering is hashed.
  std::vector<QuerySpec> queries_;

  ObjectId deferred_erase_ = kNoObject;
  const ClassGraph* classes_ = nullptr;
  ClassHookRunner* hooks_ = nullptr;
  WorldObserver* observer_ = nullptr;
  BuildingStateRules building_states_;
  Grid terrain_;
  LsaPartition lsa_;
  GaikaTable gaika_;
  GateLines gate_lines_;
  Grid height_;
  const EntityResolver* entities_ = nullptr;
  std::vector<System*> systems_;  ///< registration order is run order
  Rng rng_;
  std::uint32_t next_command_id_ = 1;
  PlayerTable players_;
  /// Named object groups, seeded from `map.obj.xml` and mutated by scripts.
  /// Hashed -- unlike `objlists_` below, this is not script-private state: it
  /// is a property of the objects, it survives the script that wrote it, and
  /// two peers that disagree about it will send different armies to different
  /// places on the next `Group("...").SetCommand(...)`.
  GroupTable groups_;
  RectTable rects_;
  /// `<group type="0">`: one object per name. Hashed for the same reason.
  NamedObjectTable named_objects_;
  /// Ships and the units they are waiting for. Hashed for the same reason
  /// again: `SHIP_BOARD.VS` and `UNIT_BOARD_COMMON.VS` both loop on it.
  BoardingTable boarding_;
  /// Script-owned collections. Not hashed: `scriptstate` is zero in all nine
  /// desync dumps, so the original kept script state out of the determinism
  /// contract and so do we.
  ObjListPool objlists_;
  ArrayPool arrays_;
  SquadListPool squadlists_;
  ConversationPool conversations_;
};

}  // namespace imperivm::core::sim
