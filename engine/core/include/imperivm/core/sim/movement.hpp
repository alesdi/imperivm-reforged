#pragma once

// Units that move: the turn-driven movement system, the formation table, and
// the movement slice of the `.vs` host API.
//
// Contract: sim/system.hpp. Grid and search: sim/path.hpp.
// Evidence: docs/engine/state-vector.md (`PathType`, `Min Range`/`Range`,
//           `method`, `action`), docs/formats/vs-host-api.md (the entry points).
//
// ## Movement is exact, and exactness is the whole design
//
// The turn length is renegotiated every turn -- 200, 400, 799 and 800 all occur
// in the retail dumps -- so a movement system must scale with `turn.length` and
// must land in the same place however an interval is cut up. `sim/tick.hpp`
// states the property for animation; the same one is required here, and for a
// stronger reason: where units are **is** hashed, and the original's dumps
// prove it.
//
// The way to get it is the way the tick loop gets it: **do not integrate.**
// A moving unit accumulates
//
//     progress += speed * speed_factor * turn.length          (exact integers)
//
// and its position is a pure function of that one running total against the
// path polyline. Integer addition does not care how an interval was split, so
// one turn of 800 and two of 400 leave the unit on precisely the same
// coordinate -- not within a unit of it, on it. A system that instead computed
// a per-turn step and added it to the position would round once per turn and
// drift apart under repartitioning, which is a desync that only shows up in a
// long game.
//
// `kSpeedScale` is 100 * 1000: a hundred because `SetSpeedFactor` is a
// percentage (the shipped calls are 7, 150 and 170), and a thousand because a
// game-time unit is a millisecond and a class's `speed` property is world units
// per second. A `speed="150"` deer at factor 7 covers 4 world units in a
// 400-unit turn; at factor 170 it covers 102.
//
// ## What is hashed and what is not
//
// `pathfinder` is zero in all nine of the original's desync dumps: the shipped
// build kept pathfinding out of the determinism contract, and so do we. The
// route a unit is following is not folded into the hash -- only the state the
// dumps actually print for a mover, which is its position, whether it has an
// active path (`SyncFlags` bit 17), and the path's target point and arrival
// tolerances. `progress` and `speed_factor` are added because they decide where
// the unit will be next turn, and a hash that cannot catch a divergence one
// turn before it becomes visible is worth less than one that can.
//
// ## Where a position lives
//
// In `ObjectState::position`, through `World::set_position`, which refuses on a
// garrisoned object and is the only writer. This system keeps no shadow copy:
// two copies of a coordinate are two chances to disagree, and the disagreement
// is a desync. `ObjectState::flags.has_active_path` -- `SyncFlags` bit 17, which
// predicts the dumps' `PathType` line perfectly -- is likewise written on the
// object, so the world hash covers it and this system does not hash it twice.
//
// ## Units keep out of each other's way, and that subdivides the turn
//
// `sim/avoidance.hpp` sets out what `gbr.exe` does; this is how it is run. A
// route whose walk animation declares a `step` is walked in steps of that
// length, and before each one the unit asks whether the step's end is taken.
// The answer depends on where everybody else is **at that instant**, and the
// instant is a game time inside the turn, not the turn's end.
//
// So a turn is played out as a queue of decisions ordered by `(game time,
// id)`: each unit's next one is due when its progress reaches the end of its
// step, or when its hold runs out, and both are exact -- the arrival time is a
// ceiling division of the same integer accumulator, and a hold is a whole
// number of milliseconds. Everybody else is evaluated at the decision's time
// from the same accumulator. None of this depends on where a turn boundary
// falls, so partition invariance survives: `[800]` and `[400, 400]` take the
// same decisions at the same instants, draw the same numbers in the same
// order, and leave every unit on the same coordinate. A turn boundary only
// settles accounts -- positions written, facings drawn.
//
// A route with no stride -- no entity, or a walk animation with no `step` --
// has no cooperative step and is walked turn by turn exactly as before. The
// original has no such route: its sampled path divides by the stride.
//
// **Four things are labelled inference.**
//
//   1. **Running out of steps during a hold.** Each test consumes a step of the
//      sampled route, so a unit held long enough runs its route out; the
//      original then hands back to `CVXPathRetry`, which re-lays the route and
//      so restarts the escalation. Here the last step is simply tested again,
//      and the escalation carries on to 666. The alternative is a re-lay per
//      exhausted hold, which would cost a search and reset the wait.
//   2. **How long a step takes.** The original's step comes with a duration:
//      the sampled path's, rescaled by distance when the coop lets it through
//      (`0x004173a7`), and a fixed one from `0x005d39c0` when it gives way.
//      Here every step takes its length at the unit's rate, which is what the
//      accumulator already means. A give-way step therefore takes as long as
//      its distance says rather than the original's constant; `0x005d39c0`
//      was not read.
//   3. **Who is in a party.** `[unit+0x1c0]` is filled by the original's party
//      move, which this engine does not have; its formation march is
//      `place_army`. So a march's members and its hero carry the hero's id in
//      `MoveState::party` from the order `place_army` gives them until an
//      order that is not a march; members step `kFormationStride`, the hero
//      keeps its own.
//   4. **Birds in the air.** A unit with `in_air` set neither tests nor
//      blocks. The callback asks only for bit 22 and a health; whether an
//      airborne flyer is linked into the ground buckets at all was not
//      followed, and a crow overhead that stopped a column would be the
//      stranger of the two readings.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/avoidance.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/system.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

/// Denominator of the progress accumulator: percent times milliseconds.
///
/// `speed` is world units per second (`CONST.INI` puts game time on a
/// millisecond scale) and `SetSpeedFactor` is a percentage of it.
inline constexpr std::int64_t kSpeedScale = 100 * 1000;

/// The canonical length a facing vector is kept at.
///
/// `Unit.GetDir` hands its result straight to `point.SetLen`, so the vector has
/// to be long enough that its direction survives integer division. A unit
/// vector would quantise every heading to one of eight; 1,024 holds a heading
/// to better than a tenth of a degree. Map objects are authored with
/// `dir=(0,1)`, which this normalises to `(0, 1024)`.
inline constexpr std::int32_t kFacingLength = 1024;

/// `SetSpeedFactor`'s neutral value: the class's own `speed`.
inline constexpr std::int32_t kDefaultSpeedFactor = 100;

/// Which of an entity's `variations` a heading is drawn as: the sprite sheet
/// column, which is what `AnimCursor::variation` holds.
///
/// **This is the mapping `docs/engine/rendering.md` recorded as unestablished,
/// and it is why every unit in the game faced the camera.** The simulation had
/// a facing on every mover and wrote it nowhere the renderer could read, so
/// `WorldView` drew column 0 for everything -- the pose the artists happen to
/// have put first.
///
/// Column 0 is the heading `(0, +1)`: **towards the viewer**, down the screen,
/// which is also the `dir=(0,1)` every map object is authored with. The index
/// then increases the way the sheets turn, which is clockwise on screen --
/// towards world -x (screen left) first, so on an eight-column sheet 2 is west,
/// 4 is north and 6 is east. Read off the frame tables: `UNITS\BOAR\WALK`
/// (8 columns) and `UNITS\CNUMIDIANRIDER\WALK` (12) both show the animal
/// head-on at 0, in left profile a quarter of the way round, and from behind at
/// the halfway column.
///
/// `variations` is the entity's facing granularity -- 8 for infantry, 12 for
/// cavalry and large units, 32 and 36 on a handful of ships and wheels, 1 for
/// everything that does not turn. It is the *entity's* count and not the
/// sheet's: an entity may stack a one-column layer on a twelve-column body, and
/// reducing a facing to a layer that does not have it is `WorldView`'s wrap.
/// A count below 2 has no facings and is always column 0.
///
/// Integer throughout, like everything else the simulation hashes: the heading
/// is reduced to a sixteen-bit fraction of a turn and rounded to the nearest
/// wedge. The arctangent is a rational approximation, so the wedge boundaries
/// land within a tenth of a degree of where the exact angles would put them --
/// measured, not assumed, over every hundredth of a degree of every count the
/// data uses. The narrowest wedge in that data is 10 degrees, so the only
/// headings this can decide differently from exact arithmetic are the ones
/// already sitting on a boundary, where either answer is as good.
[[nodiscard]] std::uint32_t facing_column(Point facing, std::int32_t variations) noexcept;

/// `Unit`'s `formation_radius` property, which 134 unit classes inherit.
inline constexpr std::int32_t kDefaultFormationRadius = 26;

// --------------------------------------------------------------------------
// per-object movement state
// --------------------------------------------------------------------------

/// Why a unit stopped. Mirrors what the dumps let us distinguish and no more.
enum class MoveOutcome : std::uint8_t {
  idle,       ///< no path; nothing to do
  moving,     ///< advancing along a route
  arrived,    ///< reached the destination, or its arrival annulus
  blocked,    ///< no route exists, or the route was cut by a new obstruction
  exhausted,  ///< the route ran out short of the goal (a partial path)
};

/// One object's movement state.
///
/// The route is here rather than in the hash: see the file header. Everything
/// above the route line is state the original prints for a mover.
struct MoveState {
  /// `Anim.x`/`Anim.y` look like this and are **marked unknown** in the state
  /// vector -- static objects hold `(0,1)` and garrisoned ones hold garbage --
  /// so this is our own facing, derived from `Face` and from travel, and is
  /// deliberately not claimed to be that field.
  Point facing{0, kFacingLength};

  /// The class's `speed` property, in world units per second. Copied in by the
  /// loader because a system may not reach into the class graph per turn.
  std::int32_t speed = 0;
  /// Whether the class-resolved fields above have been copied in yet. Part of
  /// the record so that a save restores what it resolved rather than resolving
  /// again against a class graph that may have been rebuilt.
  bool class_resolved = false;
  /// `SetSpeedFactor`, a percentage. 7, 150 and 170 are the shipped values.
  std::int32_t speed_factor = kDefaultSpeedFactor;
  /// `SetWalkAnim`'s argument: which animation slot locomotion plays. The deer
  /// switches between 13 (idle amble) and 1 (walk) as it grazes and bolts.
  std::int32_t walk_anim = 1;
  /// Whether the walk cycle is playing, so that starting it is a transition and
  /// not a poll. `World::play_anim` restarts the timeline, so a system that
  /// called it every turn would freeze the sprite on frame zero.
  bool walking = false;

  /// The class's `formation_radius` property, which `Hero.FormRadius` reports
  /// and `formation_offsets` spaces ranks by. `Unit` declares 26.
  std::int32_t formation_radius = kDefaultFormationRadius;
  /// `Obj.SetFormation` / the `formation` property. Empty means the table's
  /// `<Default Name="…"/>` applies.
  std::string formation;

  /// `PathType point=(x,y)`: where the unit is going.
  Point target;
  /// `PathType handle=<h>`: a moving target, re-read each turn. `kNoObject`
  /// when the destination is a fixed point.
  ObjectId target_object = kNoObject;
  /// `Range` and `Min Range`: the arrival annulus. A unit stops once it is
  /// within `range` of the target; `min_range` is carried because the dumps
  /// print it and engagement will need it, and is not yet acted on.
  std::int32_t range = 0;
  std::int32_t min_range = 0;

  /// `SyncFlags` bit 17, which predicts the presence of a `PathType` line
  /// perfectly over 11,670 object blocks.
  bool has_path = false;

  /// Whether a `Goto` order is outstanding, as distinct from whether a route
  /// exists right now.
  ///
  /// The two differ exactly where it matters. A unit whose destination is walled
  /// off has an order and no route, and `Goto`'s give-up timeout has to keep
  /// running for it -- otherwise `while (!.Goto(pt, 0, 1000, true, 5000));`
  /// (`UNIT_ENTER.VS`, which has no `.HasPath` guard) never times out, because
  /// keying the continuation test on `has_path` restarts the clock on every
  /// call. Cleared by `stop`, by arrival, and by a new order to somewhere else.
  bool goto_active = false;

  /// The exact accumulator described in the file header: the sum over turns of
  /// `speed * speed_factor * turn.length`. Divided by `kSpeedScale` it is the
  /// distance travelled along `waypoints`.
  std::int64_t progress = 0;

  /// Game time the unit last moved, for `Unit.TimeWithoutWalking`.
  GameTime last_moved = 0;
  /// Game time the current `Goto` began, for its give-up timeout.
  GameTime goto_started = 0;

  /// The route. **Not hashed** -- see the file header.
  std::vector<Point> waypoints;
  std::int64_t path_length = 0;
  /// The grid generation the route was built against. A route is re-validated
  /// only when the grid has changed under it, which is what makes the check
  /// cheap enough to run every turn.
  std::uint32_t path_generation = 0;
  /// Whether the route reaches the goal or merely gets as close as it could.
  bool path_complete = false;

  MoveOutcome last_outcome = MoveOutcome::idle;

  // -- unit-versus-unit avoidance (sim/avoidance.hpp) --------------------
  //
  // `CVXPathCoop`'s one field, the step the sampled route is walking, and the
  // march the order belongs to. **Saved, and not hashed**: all of it is path
  // media -- `CVXPathCoop` and `CVXSPath` are types in the executable's
  // persistent factory (codes 14 and 15, `0x006859a3` and `0x006859e8`), so
  // the original saves them, and neither is an object in the slot table the
  // `slots` channel covers, while `pathfinder` is zero in all nine dumps. What
  // it *decides* is hashed: the position it leaves the unit at, and every
  // draw it takes from the world's generator.

  /// The sampled route's step length: the walk animation's `step`, or
  /// `kFormationStride` on a formation march. **Zero means the route has no
  /// cooperative step** and the unit walks it the way every unit walked before
  /// avoidance existed.
  std::int32_t stride = 0;
  /// Standing still until `hold_until`, when the next step is decided; the
  /// alternative is walking to `step_end`, where it is decided.
  bool holding = false;
  GameTime hold_until = 0;
  /// The step being walked, as arc lengths along `waypoints`. After a hold
  /// `step_end` is the step that was *tested* and consumed, which is ahead of
  /// the unit: the next test is at the step after it.
  std::int64_t step_start = 0;
  std::int64_t step_end = 0;
  /// Lateral displacement from the route at `step_start` and at `step_end`,
  /// interpolated in between. Non-zero only across a sidestep and the step
  /// that walks back from it.
  Point offset_from;
  Point offset_to;
  /// `CVXPathCoop::retrytime`: 0, 50, 75 or 100.
  std::int32_t retry_time = 0;
  /// The hero whose formation march this order belongs to -- the original's
  /// `[unit+0x1c0]` record -- or `kNoObject`. Two units of one march do not
  /// block each other.
  ObjectId party = kNoObject;
  /// `CVXPathRetry`'s flag bit 0 (`[retry+0x20]`), which `Goto` and
  /// `GotoAttack` set and `GotoEnter` and a formation march do not: the order
  /// **owns a destination lock** at the end of every route it is laid, and it
  /// is arrived only on a free spot. Cleared for a class with
  /// `ignore_passability`, which `0x004178d0` and `SetMedia` (`0x00418770`)
  /// both refuse a lock. **Saved, and not hashed**: path media, like the rest
  /// of this block. See `sim/avoidance.hpp`, "Destination locks".
  bool dest_lock = false;
  /// The gates the route crosses and where, in route order: `0x00418fb0`'s
  /// list, made when the route is laid (`GateLines::crossings`). The step
  /// stops before the first one not yet passed while that gate bars the
  /// mover (`gate_waves_through`). **Saved, and not hashed**: path media,
  /// like the route it describes. See `sim/gate.hpp`, "The step".
  std::vector<GateCrossing> gate_crossings;

  /// Effective speed in world units per second, after the factor.
  [[nodiscard]] std::int64_t rate() const noexcept {
    return static_cast<std::int64_t>(speed) * speed_factor;
  }
  /// Distance covered along the route so far.
  [[nodiscard]] std::int64_t travelled() const noexcept { return progress / kSpeedScale; }
};

// --------------------------------------------------------------------------
// formations
// --------------------------------------------------------------------------
//
// `DATA\FORMATIONS.XML` declares six of them. Every attribute below is read
// from that file; nothing is invented, and the two things the file does not say
// -- how a block is shaped and which way it faces -- are marked as inferences
// where they are computed.

/// Where a class stands in a formation. The file gives each `<Class>` exactly
/// one of these three flags.
enum class FormationPlacement : std::uint8_t {
  central_block,  ///< around the hero; the default for anything unlisted
  front_line,     ///< ahead of him, by `OffsetFrontLineByY`
  wings,          ///< out to either side, by `OffsetWingsByX`/`ByY`
};

/// One `<Class Name=… FrontLine|Wings|CentralBlock="1"/>`.
struct FormationRole {
  std::string class_name;
  FormationPlacement placement = FormationPlacement::central_block;
};

/// One `<FormationClass>`.
///
/// The bonuses are carried through verbatim because the file declares them;
/// this system does not apply them. Their one reader is `FormDescription`,
/// below, which is also what settled which attribute lands in which field. The
/// original's loader (0x005f7c30) walks an element's attributes **in file
/// order** and recognises five names: `BonusDamage`, `BonusDefencePierce`,
/// `BonusDefenceSlash`, `BonusArmor` and `BonusRange`, plus `BonusLevel`.
/// `BonusArmor` is not a field of its own: it writes *both* defence fields, so
/// a later `BonusDefenceSlash` on the same element overrides half of it and an
/// earlier one is overwritten. The parser reproduces that order dependence
/// rather than reading by name, although no shipped element declares both.
///
/// `BonusAttack` is a name the executable never reads. The shipped file gives
/// the sixth formation `BonusAttack="4"`, and on the original that attribute is
/// inert: not stored, not described, not applied. It is parsed here so that a
/// reader of the file can see the number, and nothing consumes it.
struct FormationClassDef {
  std::string name;
  std::string description;
  /// `Width`/`Height`. **Read as an aspect ratio, not a cell count** -- `Line`
  /// is 6x1 and described as "all warriors at the front line", `Block` declares
  /// neither and is described as gathering around the hero, and a literal
  /// reading would cap `Front` at two soldiers. See `formation_offsets`.
  std::int32_t width = 1;
  std::int32_t height = 1;

  std::int32_t bonus_level = 0;           ///< `BonusLevel`
  std::int32_t bonus_damage = 0;          ///< `BonusDamage`
  std::int32_t bonus_defence_slash = 0;   ///< `BonusDefenceSlash`, or `BonusArmor`
  std::int32_t bonus_defence_pierce = 0;  ///< `BonusDefencePierce`, or `BonusArmor`
  std::int32_t bonus_range = 0;           ///< `BonusRange`, a percentage
  /// `BonusAttack`: declared once in the shipped file, read by nothing in the
  /// executable. See the struct note.
  std::int32_t bonus_attack = 0;

  std::int32_t offset_front_line_y = 0;
  std::int32_t offset_wings_x = 0;
  std::int32_t offset_wings_y = 0;

  std::vector<FormationRole> roles;

  /// Where `class_name` stands. Matching is exact and case-insensitive against
  /// the `<Class Name>` values; an unlisted class takes the central block,
  /// which is what `Block` -- the formation that lists only `Unit` -- needs.
  [[nodiscard]] FormationPlacement placement_of(std::string_view class_name) const noexcept;
};

/// `DATA\FORMATIONS.XML`.
class FormationTable {
 public:
  [[nodiscard]] static Result<FormationTable> parse(std::span<const std::byte> xml);

  /// `<Default Name="Front"/>`.
  [[nodiscard]] std::string_view default_name() const noexcept { return default_name_; }
  [[nodiscard]] const std::vector<FormationClassDef>& formations() const noexcept {
    return formations_;
  }
  /// Case-insensitive, because `SetFormation` is called with map-authored
  /// strings (`cmdparam`) as well as literals.
  [[nodiscard]] const FormationClassDef* find(std::string_view name) const noexcept;
  [[nodiscard]] const FormationClassDef* default_formation() const noexcept {
    return find(default_name_);
  }

 private:
  std::vector<FormationClassDef> formations_;
  std::string default_name_ = "Front";
};

/// One soldier to place.
struct FormationMember {
  ObjectId id = kNoObject;
  FormationPlacement placement = FormationPlacement::central_block;
  /// The class's `formation_radius` property (26 on `Unit`), which sets the
  /// spacing. Zero falls back to `kDefaultFormationSpacing / 2`.
  std::int32_t formation_radius = 0;
};

/// Spacing when nothing declares a `formation_radius`: twice `Unit`'s 26, so
/// two neighbours in a rank stand exactly touching.
inline constexpr std::int32_t kDefaultFormationSpacing = 2 * kDefaultFormationRadius;

/// Where each member stands, relative to the leader, facing `facing`.
///
/// Members are bucketed by placement in the order given, and each bucket is
/// laid out as a rectangle whose aspect follows the formation's `Width:Height`:
/// for `n` members, `columns = isqrt(n * Width / Height)` clamped to at least
/// one, rows following. `Line` (6:1) therefore puts six abreast, `Front` (2:1)
/// three by two, and `Block` (1:1, the file declares no size) two by three.
/// **The rectangle shape is an inference**; the offsets, the bucketing and the
/// three placements are all read from the file.
///
/// The result is rotated into `facing`, whose forward direction is local +y --
/// the axis `OffsetFrontLineByY` names. Writes `out.size()` offsets and returns
/// how many it wrote, which is `min(members.size(), out.size())`.
std::size_t formation_offsets(const FormationClassDef& formation,
                              std::span<const FormationMember> members, Point facing,
                              std::span<Point> out) noexcept;

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

/// Advances every unit with an active path, once per turn.
///
/// Objects are visited in ascending id, which is spawn order, which is the
/// order the world stores them in. The per-object states live in one vector
/// sorted by id and are found by binary search: no unordered container, and no
/// dependence on allocation addresses.
class MovementSystem : public System {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "movement"; }

  void start(World& world) override;
  void advance(World& world, const Turn& turn) override;
  void hash(std::uint64_t& accumulator) const override;

  // -- the map -----------------------------------------------------------

  /// Install the map's obstruction bitmap. Takes it by value: a map's
  /// `Terrain.pass.grid` is authored data that cannot be regenerated
  /// (docs/engine/projection.md measured a 70.5% ceiling on reproducing it),
  /// so it is loaded from the shipped file and owned here.
  /// The class graph a fresh `MoveState` resolves its class properties from.
  /// Not owned; null leaves them at their defaults.
  ///
  /// **`MoveState::speed` says it is "copied in by the loader" and no loader
  /// ever did**, so every unit in every session had a speed of zero and could
  /// not move a world unit however good its path was. It is resolved here, once
  /// per object when its first order is laid, because a system may not reach
  /// into the class graph per turn -- which is what that comment was asking for
  /// and what nothing supplied. `CombatSystem::set_class_graph` is the same
  /// arrangement for the same reason.
  void set_class_graph(const ClassGraph* classes) noexcept { classes_ = classes; }
  [[nodiscard]] const ClassGraph* class_graph() const noexcept { return classes_; }

  void resolve_class_fields(const World& world, ObjectId id, MoveState& move);
  void play_locomotion(World& world, ObjectId id, MoveState& move);

  /// Point `move` at `heading` and show it: the sheet column `facing_column`
  /// resolves the heading to goes onto the object's animation cursor, which is
  /// what the renderer draws.
  ///
  /// **Every write to `MoveState::facing` goes through here**, so the pose on
  /// screen cannot fall behind the heading the simulation is walking along --
  /// which it did for as long as nothing wrote the cursor at all. A `heading`
  /// of `(0, 0)` names no direction and leaves both alone.
  void set_facing(World& world, ObjectId id, MoveState& move, Point heading) const;

  void set_grid(ObstructionGrid grid);
  [[nodiscard]] const ObstructionGrid& grid() const noexcept { return grid_; }
  /// Mutable access, for stamping a building in as it completes. Every call
  /// bumps the generation, so routes crossing the changed area are revalidated.
  [[nodiscard]] ObstructionGrid& mutable_grid() noexcept;
  [[nodiscard]] std::uint32_t grid_generation() const noexcept { return grid_generation_; }

  void set_formations(FormationTable table) { formations_ = std::move(table); }
  [[nodiscard]] const FormationTable& formations() const noexcept { return formations_; }

  /// Cap on nodes one search may expand. Lower it on a large map if a frame
  /// budget demands; it changes routes, so it is world configuration.
  void set_node_budget(std::uint32_t budget) noexcept { node_budget_ = budget; }

  // -- per-object state --------------------------------------------------

  [[nodiscard]] MoveState& state(ObjectId id);
  [[nodiscard]] const MoveState* find(ObjectId id) const noexcept;
  [[nodiscard]] MoveState* find(ObjectId id) noexcept;
  void forget(ObjectId id);
  /// Ids with movement state, ascending. Iteration order is world state.
  [[nodiscard]] std::size_t tracked() const noexcept { return states_.size(); }

  /// The object's position, from the object itself. `kHeldPosition` when it is
  /// inside a holder, and `(0,0)` when there is no such object.
  [[nodiscard]] Point position(World& world, ObjectId id) const noexcept;
  void set_position(World& world, ObjectId id, Point where) const noexcept;
  /// Write `SyncFlags` bit 17 -- `ObjectState::flags.has_active_path` -- so the
  /// world hash and a dump comparison both see it.
  void mark_path(World& world, ObjectId id, bool active) const noexcept;

  // -- orders ------------------------------------------------------------

  /// `Unit.Goto(dest, range, …)`: path to `dest` and start walking.
  ///
  /// Returns `arrived` when the unit is already within `range` -- the caller's
  /// loop then exits without a search -- `moving` when a route was laid,
  /// `exhausted` when only a partial route exists, and `blocked` when there is
  /// no route at all. A blocked order clears the path, so the shipped idiom
  /// `while (!.Goto(pt, …) && .HasPath)` terminates rather than spinning.
  ///
  /// `party` is the hero whose formation march this is (`place_army`), or
  /// `kNoObject` for any other order, which also takes the unit out of a march
  /// it was in.
  ///
  /// `lock_destination` is `SetDest`'s last argument (`MoveState::dest_lock`):
  /// `Goto` and `GotoAttack` pass it, and a route they lay ends on a free spot
  /// and reserves it.
  MoveOutcome order_goto(World& world, ObjectId id, Point dest, std::int32_t range,
                         std::int32_t min_range = 0, ObjectId party = kNoObject,
                         bool lock_destination = false);
  /// The same, for a moving target. The route is re-laid when the target has
  /// moved more than `repath_threshold()` from where it was.
  MoveOutcome order_goto_object(World& world, ObjectId id, ObjectId target, std::int32_t range,
                                std::int32_t min_range = 0, bool lock_destination = false);
  /// `Unit.Stop`. Keeps the position, drops the route.
  void stop(World& world, ObjectId id);
  /// `Unit.Face(pt)`: look towards a world point. A point on top of the unit
  /// leaves the facing alone, since it names no direction.
  void face(World& world, ObjectId id, Point towards);

  /// How far a tracked target may drift before the route is rebuilt. Four
  /// collision cells: shorter thrashes the pathfinder, longer lets a unit walk
  /// visibly past its quarry.
  [[nodiscard]] static constexpr std::int32_t repath_threshold() noexcept {
    return 4 * kCollisionCellSize;
  }

  /// Remaining distance along the route, in world units.
  [[nodiscard]] std::int64_t remaining(ObjectId id) const noexcept;
  /// Game time until arrival at the current rate, rounded up. `-1` when the
  /// unit is not moving or its rate is zero.
  [[nodiscard]] std::int64_t eta(ObjectId id) const noexcept;

  /// The pathfinder. Scratch state only, never hashed, never serialised.
  [[nodiscard]] PathFinder& pathfinder() noexcept { return finder_; }

  // -- avoidance ---------------------------------------------------------

  /// An object's `radius` class property, which is what every avoidance test
  /// measures it by. Zero for an object with no class, or none declared.
  [[nodiscard]] std::int32_t radius_of(const World& world, ObjectId id) const;

  /// Whether an object's class sets `ignore_passability`, which makes every
  /// route it is given a straight line (`PathRequest::ignore_passability`).
  [[nodiscard]] bool ignores_passability(const World& world, ObjectId id) const;

  /// `0x0040a990`: whether `at` is a free spot for `asker`, a body of
  /// `radius`. Taken by a live ground unit other than the asker that is
  /// standing (no active path, `SyncFlags` bit 17 clear), or by any lock that
  /// is not the asker's own -- ownerless, or owned by another unit's route --
  /// whose centre is nearer than the two radii summed. There is no march
  /// exemption here: `0x004094d0` does not ask `0x005d2f80`.
  [[nodiscard]] bool spot_free(const World& world, ObjectId asker, Point at,
                               std::int32_t radius) const;

  /// Where a route for an arrival band ends: `0x00417830`'s goal rings. See
  /// the definition.
  [[nodiscard]] Point ring_goal(const World& world, Point from, Point centre, std::int32_t range,
                                std::int32_t min_range) const;

  /// The owned destination lock `id`'s route holds, if any: its route's last
  /// point, radius the owner's.
  [[nodiscard]] bool owned_lock(const World& world, ObjectId id, StaticLock& lock) const;

  /// The ownerless locks, in the order they were made.
  [[nodiscard]] const std::vector<StaticLock>& static_locks() const noexcept {
    return static_locks_;
  }
  /// Make the ownerless locks from every placed object's `<point>` types 9,
  /// 10 and 11 -- the map-object loader's last act (`0x00540a20`). Done once
  /// per match, by `start`; a restored save brings its own.
  void build_static_locks(const World& world);
  /// Replace them outright. For tests, and for a save.
  void set_static_locks(std::vector<StaticLock> locks);

  /// What the cooperative step has done since this system was built. **Diagnostics**: never hashed, never saved, and a
  /// loaded session counts from zero. `imrun` prints them, because a mechanism
  /// that never fires on a real map looks exactly like one that works.
  struct AvoidanceCounters {
    std::uint64_t decisions = 0;  ///< steps tested
    std::uint64_t blocked = 0;    ///< of which the step's end was taken
    std::uint64_t holds = 0;      ///< and the unit stood
    std::uint64_t early = 0;      ///< and a draw gave way before the wait ran out
    std::uint64_t sidesteps = 0;  ///< gave way to somewhere that was not the step's end
    std::uint64_t gate_waits = 0; ///< stood before a gate that barred the way
    std::uint64_t gate_searches = 0;  ///< routes searched again with enemy gates laid
  };
  [[nodiscard]] const AvoidanceCounters& avoidance() const noexcept { return counters_; }

  /// How many bodies stand on one another right now. **Diagnostics**, read
  /// from the world as it is and never hashed: playtest report #13 was a squad
  /// of horsemen drawn through each other, and a count is what a corpus run
  /// can hold where a screenshot cannot.
  ///
  /// Counts live ground units with a `radius` that are standing (no active
  /// path) -- the bodies `gbr.exe`'s free-spot test (`0x0040a990`) sees -- and,
  /// among them, the pairs whose centres are nearer than their summed radii
  /// (the test's own "taken", `0x0040a96f`), and of those the pairs nearer
  /// than half that, which is one body drawn through the other.
  struct OverlapCensus {
    std::size_t bodies = 0;   ///< standing units counted
    std::size_t touching = 0; ///< pairs nearer than `r1 + r2`
    std::size_t stacked = 0;  ///< pairs nearer than `(r1 + r2) / 2`
  };
  [[nodiscard]] OverlapCensus overlap_census(const World& world) const;

  // -- the saved game ----------------------------------------------------
  //
  // Layout and rationale: docs/formats/save.md. The definitions live together
  // in `src/sim/save_systems.cpp` rather than in this domain's own `.cpp`, so
  // that the ten systems' state vectors are one file to audit: adding a field
  // here and forgetting its section shows up as a diff that does not touch the
  // one place every section is written.

  /// Append this system's state to `out`, as one self-describing section with
  /// its own magic and version -- the shape `Scheduler::serialize` established.
  ///
  /// **Written:** the per-object `MoveState` table in id order, and
  /// `grid_generation_`. The waypoint list inside each `MoveState` is written
  /// too -- a laid path is not recomputable, because the grid it was laid
  /// against can have moved under it, and a unit that came back with an empty
  /// path would silently stop where it stood. So is each route's avoidance
  /// state -- stride, hold, step, sidestep offsets, wait, march -- and, after
  /// the table, the ownerless locks with the flag that they were made: none of
  /// it hashed, all of it needed to resume where the save was taken.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** the obstruction grid, the formation table and the node
  /// budget, which are load-time data the embedder supplies from the map and
  /// the class data; and `PathFinder`, which the declaration above already
  /// calls scratch state, never hashed, never serialised.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  struct Entry {
    ObjectId id = kNoObject;
    MoveState state;
  };

  /// The class properties avoidance reads per object, resolved once per class.
  struct ClassTraits {
    std::int32_t radius = 0;
    bool water_unit = false;
    bool ignore_passability = false;
  };
  /// Something that can block a step: a unit on the ground, as it stood when
  /// the turn began. See `index_bodies`.
  struct Body {
    ObjectId id = kNoObject;
    std::size_t entry = 0;  ///< into `states_`, or `kUntracked`
    Point start;
    std::int32_t radius = 0;
  };
  static constexpr std::size_t kUntracked = static_cast<std::size_t>(-1);

  [[nodiscard]] std::size_t lower_bound(ObjectId id) const noexcept;
  /// `from` is where the unit stands, when that is not yet written to the
  /// world -- the middle of a turn's cooperative steps. Null reads the world.
  MoveOutcome lay_path(World& world, ObjectId id, MoveState& move, Point goal,
                       std::int32_t range, GameTime now, const Point* from = nullptr);
  /// Cut a route back to its last free point (`0x004141bd`). Returns whether
  /// anything was cut.
  bool truncate_to_free(const World& world, ObjectId id, std::vector<Point>& waypoints) const;
  /// 0x00419110's gates: `first` is the route searched with none laid; when
  /// it crosses a gate that counts the mover an enemy, the search runs again
  /// with every such gate's line laid, and the answer is the route to walk.
  /// See `sim/gate.hpp`, "Who a search lays it for".
  [[nodiscard]] Path route_past_gates(World& world, ObjectId id, Point from,
                                      const PathRequest& request, Path first);
  /// 0x00418210 and 0x00418260: whether the first gate the route crosses
  /// past `travelled` is within reach and bars the mover, which then stands.
  [[nodiscard]] bool held_at_gate(const World& world, ObjectId id, const MoveState& move,
                                  std::int64_t travelled) const;
  /// `IsArrived`'s free-spot half, for a route that owns a lock.
  [[nodiscard]] bool arrival_spot_free(const World& world, ObjectId id, const MoveState& move,
                                       Point here) const;
  /// A route that ended on a taken spot is laid again from where the unit
  /// stands (`RecastPathfind`, `0x00419730`). A route that cannot move it
  /// stops it there, not arrived.
  void recast(World& world, std::size_t entry, Point here, GameTime now);
  /// The turn-start housekeeping every route gets: a dead owner, a drifting
  /// target, a grid that changed under the route.
  void prepare(World& world, Entry& entry, GameTime now);
  /// A route with no cooperative step, walked one whole turn at once.
  void walk_turn(World& world, const Turn& turn, Entry& entry);

  // The cooperative step. See the header and `advance`.
  [[nodiscard]] bool cooperates(const World& world, const MoveState& move, ObjectId id) const;
  [[nodiscard]] Point offset_at(const MoveState& move, std::int64_t travelled) const noexcept;
  [[nodiscard]] Point cooperative_position(const MoveState& move, std::int64_t progress) const;
  [[nodiscard]] std::int64_t progress_at(std::size_t entry, GameTime t) const;
  [[nodiscard]] Point body_position(const World& world, const Body& body, GameTime t) const;
  [[nodiscard]] GameTime next_decision(const MoveState& move, GameTime now) const noexcept;
  void decide(World& world, std::size_t entry, GameTime now);
  void arrive(World& world, std::size_t entry, bool in_range);
  void settle(World& world, const Turn& turn, std::size_t entry);
  void index_bodies(const World& world);
  void index_static_locks();

  [[nodiscard]] ClassTraits traits_of(const World& world, ObjectId id) const;
  [[nodiscard]] std::int32_t formation_stride_or_walk(const World& world, ObjectId id,
                                                      const MoveState& move) const;

  std::vector<Entry> states_;  ///< sorted by id; iteration order is world state
  ObstructionGrid grid_;
  const ClassGraph* classes_ = nullptr;
  FormationTable formations_;
  PathFinder finder_;
  std::uint32_t grid_generation_ = 1;
  std::uint32_t node_budget_ = 20000;

  /// World state, saved: see `static_locks()`.
  std::vector<StaticLock> static_locks_;
  bool static_locks_built_ = false;

  // -- scratch, rebuilt every turn; never hashed, never saved -------------
  AvoidanceCounters counters_;
  mutable std::vector<ClassTraits> traits_;
  mutable std::vector<std::uint8_t> traits_known_;
  mutable const ClassGraph* traits_graph_ = nullptr;
  /// `spot_free`'s candidates.
  mutable std::vector<ObjectId> spot_scratch_;
  /// Per entry, how its route is walked this turn: still, step by step, or
  /// the whole turn at once.
  std::vector<std::uint8_t> mode_;
  /// The boundary the running turn began at, and the one it ends at.
  GameTime turn_t0_ = 0;
  GameTime turn_end_ = 0;
  /// A route search's gate lines, and the crossings it is tested against.
  CellOverlay barrier_;
  std::vector<GateCrossing> crossings_;
  std::vector<Neighbour> neighbours_;
  /// Per entry, the game time its accumulator has been brought up to.
  std::vector<GameTime> since_;
  std::vector<GameTime> scheduled_;
  std::vector<Point> turn_start_;
  /// The units standing at the turn's start, bucketed; and the ones with a
  /// route, which are read where they are at each instant instead.
  std::vector<Body> bodies_;
  std::vector<Body> movers_;
  /// A bucket grid over `bodies_` by position, and over the static locks:
  /// `kBucket`-unit cells, compressed rows.
  struct Buckets {
    std::int32_t columns = 0;
    std::int32_t rows = 0;
    std::vector<std::uint32_t> first;  ///< columns * rows + 1 offsets
    std::vector<std::uint32_t> items;
  };
  Buckets body_buckets_;
  Buckets lock_buckets_;
  bool lock_buckets_valid_ = false;
};

// --------------------------------------------------------------------------
// the host slice
// --------------------------------------------------------------------------

/// One `Goto`-family order, as the host call sites spell it.
///
/// Four entry points share this shape -- `Goto/4`, `Goto/5`, `GotoEnter/5` and
/// `GotoAttack/3,4` -- and they differ only in where the fields come from, so
/// the semantics live in one function rather than in three copies that could
/// drift. `sim/command.hpp` owns the two cross-domain members and calls
/// `run_goto` for the walking half.
struct GotoOrder {
  /// Where to go. Ignored when `target` is set, which is re-read every turn.
  Point dest;
  ObjectId target = kNoObject;
  /// The arrival annulus. `range` is `Goto`'s second argument; `min_range` is
  /// only supplied by `GotoAttack` and `FormSetupAndMoveTo`.
  std::int32_t range = 0;
  std::int32_t min_range = 0;
  /// How long one call may wait before returning to the caller's loop. The
  /// call suspends for the lesser of this and the time to arrival.
  std::int64_t slice = 0;
  /// Total timeout. **Anything non-positive means no limit**; see the note on
  /// `register_movement_host`. For an `enter` order it is something else; see
  /// there.
  std::int64_t give_up = -1;
  /// `GotoEnter` (0x005d6620), whose give-up `gbr.exe` reads directly:
  ///
  ///   * **arrival is the band and nothing else.** On re-entry the call answers
  ///     `CVXPathRetry::IsArrived` (0x00417c10), and a point route is arrived
  ///     only within its range. The flag that would call a finished walk
  ///     arrived (bit 0x80 of `[retry+0x20]`) is set only by the formation
  ///     march's step (0x00419c4c, 0x00419e19, inside 0x00419b20). So the end
  ///     of a partial route is not arrival, and neither is a route that was
  ///     never laid.
  ///   * **`give_up` times the failure, not the walk.** When `SetDest`
  ///     (0x0041a4c0) lays no route and the unit is not arrived, the call
  ///     stamps `[unit+0x150]` with the time if it is clear (0x005d6942),
  ///     and when `give_up >= 0` and the time since the stamp has reached it
  ///     (0x005d2e90) it returns 2 -- **the script ends there** (`script/
  ///     host.hpp`). A laid route or an arrival clears the stamp (0x005d698f,
  ///     0x005d68e9). So `0` gives up on the first failed search and a
  ///     negative value never does; a walk, however long, is never timed.
  ///     Here `goto_started` carries the stamp for such an order.
  bool enter = false;
  /// `SetDest`'s lock flag: `Goto` (`0x005d645c`) and `GotoAttack`
  /// (`0x005d435b`, `0x005d43a6`) pass 1, `GotoEnter` (`0x005d68c3`) 0.
  bool lock_destination = false;
};

/// The shared body of the `Goto` family. Returns the host outcome the entry
/// point should return: `true` on arrival, `false` with a suspension while
/// walking, `false` outright when the order has timed out -- and, for an
/// `enter` order that has gone `give_up` without a route, `finish`.
[[nodiscard]] script::HostOutcome run_goto(script::CallContext& ctx, World& world,
                                           MovementSystem& movement, ObjectId id,
                                           const GotoOrder& order);

/// The movement system a world is running, or null when none is registered.
///
/// Found by name over `World::systems()`, which is the seam the world already
/// has: a host function is handed a `World*` and nothing else, and adding a
/// `MovementSystem*` field to `World` to solve that would make every domain add
/// one. Registration order is world state, so this walk is deterministic.
[[nodiscard]] MovementSystem* movement_system(World& world) noexcept;

/// Implement the movement slice of the `.vs` host API.
///
/// Exactly these entry points, with the arities `docs/formats/vs-host-api.md`
/// records, so that nothing here widens the declared surface:
///
///   free      `IsPassable3x3/1`
///   Unit &c.  `Goto/4`, `Goto/5`, `Face/1`, `SetSpeedFactor/1`, `SetWalkAnim/0`,
///             `SetWalkAnim/1`, `Stop/1`, `HasPath/0`, `PathTo/3`,
///             `PathDestFound/0`, `ClipDestToMap/1`, `TimeWithoutWalking/0`,
///             `GetDir/0`, `SetPos/1`, `SetFormation/1`
///   properties `dest/0`, `speed/0`, `formation/0`, `FormRadius/0`
///
/// The world reaches these the way `sim/world_host.hpp` established: the
/// embedder sets `CallContext::user` to a `HostContext*` (`sim/host_context.hpp`,
/// and it used to be a bare `World*` here), points are the
/// `(kTypePoint, x, y)` values that header defines, and object handles
/// are `(kTypeObj, id)`. Nothing here mints a representation of its own.
///
/// `Stop(ms)` **returns a bool and suspends**, which the call sites settle and
/// an earlier reading of it as a void procedure got wrong. Of its 53 sites, 34
/// are `while (!.Stop(1000));` and 7 are `if (.Stop(2000))`; a void `Stop`
/// yields nil, `!nil` is true, and every one of those `while` loops spins until
/// the instruction budget traps the script. The reading that makes all 53 work
/// is **halt, then hold still for `ms`, and report whether the hold completed**:
/// it explains the argument (a duration, 50 to 10,000), it explains why
/// `UNIT_IDLE.VS`'s `while(1)` and `SHIP_IDLE.VS`'s `while(1)` consume game time
/// at all, and it makes `while (!.Stop(1000));` one call rather than a spin.
/// Nothing here can interrupt a hold, so it always returns true after
/// suspending; what would settle the false case is a retail trace of a unit
/// ordered to stop while an animation is mid-swing.
///
/// `Goto`'s five arguments are `(destination, range, slice, flag, give_up)`.
/// The first two are established: the shipped call sites pass `.range`,
/// `.sight`, `other.sight/3` and `GetConst("GiveDistance")` in the second
/// position, which are all distances, and the dumps print exactly such a
/// tolerance as `Range`. **The last three are inferred**, from their value
/// distributions and from the loop the calls sit inside:
/// `while (!.Goto(pt, 0, 2000, true, 0) && .HasPath)`. `slice` is read as the
/// game-time budget one call may wait for -- the call suspends for the lesser
/// of it and the time to arrival, and the loop re-tests -- and `give_up` as a
/// total timeout, where **anything non-positive means no limit**. Both `-1` and
/// `0` occur and both have to mean "no limit": `0` is the commonest value in the
/// corpus and appears inside `while (!.Goto(...) && .HasPath)`, so reading it as
/// an immediate timeout stops every such loop on its second call. The boolean is recorded and not acted on;
/// no reading of it is better supported than another, and guessing would put an
/// unearned behaviour in front of whoever settles it.
void register_movement_host(script::HostRegistry& registry);

}  // namespace imperivm::core::sim
