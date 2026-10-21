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
//   3. **Who is in a party.** `[unit+0x1c0]` is the member's slot in its
//      hero's formation object (`sim/formation.hpp`: written by the slot
//      assignment, 0x005f3115, cleared by a removal, 0x005f4686), which this
//      engine keeps as `Formation::slots`. What the avoidance step reads is
//      still the order: a march's members and its hero carry the hero's id in
//      `MoveState::party` from the route aimed at the formation
//      (`aim_at_formation`, `SetFormation` 0x00417790) until an order that
//      is not a march. The original's slot outlasts an order, so a member
//      sent elsewhere is still of the party there; that was not followed.
//      **The march is the formation's own route.** As in the original, the
//      formation lays a route of its own from the hero (`SetDest`,
//      0x005f2cc0), steps its centre 200 at a time along it
//      (`CreateNextSample`, 0x005f5bd0), and every member's route and the
//      hero's are aimed at it: at each step a unit lays its route to its
//      station afresh when a sample has been made since, and makes the next
//      sample when it is within half its step and 133 of the station
//      (`formation_step`, 0x00419b20). Each step is timed to reach the
//      station at the sample's deadline (0x00419e52), so the army and the
//      hero walk the formation's pace. Such a route arrives once the
//      formation's path is spent and the unit stands on its station, on a
//      free spot when the march's flag was set, which is when `0x004178d0`
//      names the member an owner (`lock_owner`). A follower strides 200 for
//      the avoidance, the hero his walk step: an engine choice
//      (`formation_stride_or_walk`). See `docs/formats/formation.md`, "The
//      formation's route".
//      **Not reproduced: the original's claim on its stations.**
//      `CVXFormObj::PlaceSample` (0x005f5a20) ends by moving every station
//      its member cannot stand on (0x005f5b7b -> 0x005f3fe0): a flood from
//      the sample centre over the collision cells within max(formation
//      radius, 1,024) (0x005f5b02, 0x005f3c60) marks the cells it reaches;
//      each member in turn keeps its station's cell if it is reached and not
//      yet taken, and takes it (0x005f29a0); otherwise it goes to the
//      cheapest reached cell near its station (0x005f25d0, 0x005f2370), a
//      cell it does not take, so two moved stations can share one. Here
//      each member walks to its station as laid, and one in a building walks
//      to the nearest point it can reach. Built and measured on Crossroads
//      while the march was still the hero's route, the claim made crowding
//      worse (overlap median 25 -> 38 on seeds 1-6, 28 -> 40 on seeds 7-12),
//      so it is not wired; it has not been measured on the formation's own
//      route. See `docs/plan.html`.
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
#include "imperivm/core/sim/formation.hpp"
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

/// `MoveState::goto_failed_at` while the unit's `Goto` calls are laying
/// routes: the original's `-1` in `[unit+0x150]`.
inline constexpr std::int64_t kNoGotoFailure = -1;

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
  /// off has an order and no route, and the next `Goto` to the same place is
  /// the same order searching again from where it stands, not a new one.
  /// Cleared by `stop`, by arrival, and by a new order to somewhere else.
  bool goto_active = false;

  /// The exact accumulator described in the file header: the sum over turns of
  /// `speed * speed_factor * turn.length`. Divided by `kSpeedScale` it is the
  /// distance travelled along `waypoints`.
  std::int64_t progress = 0;

  /// Game time the unit last moved, for `Unit.TimeWithoutWalking`.
  GameTime last_moved = 0;
  /// When the `Goto` family last began failing to lay a route, or
  /// `kNoGotoFailure` while it has not -- `[unit+0x150]` in `gbr.exe`, which
  /// keeps `-1` for the same. Stamped by the first call that lays no route and
  /// is not arrived, and cleared by a call that lays one or arrives and by the
  /// end of the unit's command; the give-up and the re-search draw both read
  /// it. **Not hashed**: nothing the original dumps prints it. See
  /// `GotoOrder::give_up`.
  GameTime goto_failed_at = kNoGotoFailure;

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
  /// `GotoAttack` set and `GotoEnter` does not: the order **owns a
  /// destination lock** at the end of every route it is laid, and it is
  /// arrived only on a free spot. Cleared for a class with
  /// `ignore_passability`, which `0x004178d0` and `SetMedia` (`0x00418770`)
  /// both refuse a lock. On a march member it carries the formation's own
  /// flag instead, and owns a lock only once the march is over
  /// (`MovementSystem::lock_owner`). **Saved, and not hashed**: path media,
  /// like the rest of this block. See `sim/avoidance.hpp`, "Destination
  /// locks".
  bool dest_lock = false;
  /// `Unit::Stop` has asked a moving unit to stop: `CVXPathRetry`'s flag bit 1
  /// (`[retry+0x20] |= 2`, slot 6 at 0x004178b0). The path follower
  /// (0x00419ee0) then walks on, step by step, until the unit stands on a
  /// passable free spot or its route runs out (`MovementSystem::decide`).
  /// Cleared by every new order and by `stop`. **Saved, and not hashed**:
  /// path media, and what it decides -- where the unit comes to rest -- is.
  bool stop_requested = false;
  /// `CVXPathRetry`'s flag bit 2 (`[retry+0x20] & 4`): the last search laid
  /// no route. `RecastPathfind` (0x00419730) clears bits 1 and 2 when it
  /// starts; when its search fails (0x00419110 answers 0) it deletes the path
  /// it built, **keeps the media the retry already had** -- the route being
  /// walked, if there is one -- and, for a retry not aimed at a formation
  /// (`[retry+0xc]` clear, 0x00419a17), sets this bit (0x00419a1e) and answers
  /// with the path follower's own step (0x00419a48 .. 0x00419a5d). The
  /// follower (0x00419ee0) tests bits 1 and 2 together (`test al, 6` at
  /// 0x00419ee9), so a failed search walks the old route on exactly as
  /// `Unit::Stop` does -- to the first passable free spot, for an order that
  /// owns a lock, and not a step for one that does not -- instead of
  /// stopping where it is. `IsArrived` (0x00417c10) is false while either bit
  /// is set. `SetDest` also searches again whenever one is set, whatever the
  /// failing unit's draw says (0x0041a4da, 0x0041a5da; `run_goto`,
  /// `order_goto`).
  ///
  /// When the walk ends the order stays: the unit's step (0x005d3b50) deletes
  /// a route with no next step only when no `Goto`-family call is waiting on
  /// it (bit 0x10000 of `[unit+0x194]`, which the call sets as it suspends),
  /// and a walk on after a failed search is inside the very call whose
  /// `SetDest` failed, so its re-entry finds the order and answers not
  /// arrived. **Inferred** for a halt with no call waiting, which this engine
  /// keeps too. `Unit::Stop`'s halt drops the order, as its re-entry does.
  /// **Saved, and not hashed**: path media, like `stop_requested`.
  bool route_failed = false;
  /// Either of the follower's stop bits (`[retry+0x20] & 6`).
  [[nodiscard]] bool stopping() const noexcept { return stop_requested || route_failed; }
  /// `CVXPathRetry`'s flag bit 3 (`[retry+0x20] & 8`): the free-spot search
  /// (`0x004180b0`) has run for this destination. Set when it runs
  /// (`0x004191ab`), whether or not it re-aims; cleared only by `SetDest` to
  /// a different destination (`0x0041a594`) and with the retry, which the
  /// `Goto` family deletes on arrival. **Saved, and not hashed**.
  bool free_spot_tried = false;
  /// Flag bit 6 (`& 0x40`): the route being walked was re-aimed at a free
  /// spot (`0x00419275`). It too keeps the search from running, and the path
  /// follower clears it when that route has been walked to its end
  /// (`0x0041a097`, `0x0041a0ae`). **Saved, and not hashed**.
  bool free_spot_aimed = false;
  /// The gates the route crosses and where, in route order: `0x00418fb0`'s
  /// list, made when the route is laid (`GateLines::crossings`). The step
  /// stops before the first one not yet passed while that gate bars the
  /// mover (`gate_waves_through`). **Saved, and not hashed**: path media,
  /// like the route it describes. See `sim/gate.hpp`, "The step".
  std::vector<GateCrossing> gate_crossings;
  /// On a formation march, the hero's and every marching member's: the pace
  /// the march gives it, in `rate()`'s units, or 0 for none -- the unit then
  /// walks at its own `speed` and factor. The original times each step of a
  /// route aimed at a formation to reach the unit's station at the sample's
  /// deadline (0x00419e52 .. 0x00419ecd), so a member walks the formation's
  /// pace and not its own, and so does the hero, whose route is aimed at the
  /// formation too (0x0052e830). The formation's step sets it at each of the
  /// unit's steps (`formation_rate`); any order that is not a march clears
  /// it.
  /// **Saved, and not hashed**: path media like the rest of this block, and
  /// what it decides -- where the unit is -- is hashed.
  std::int64_t march_rate = 0;
  /// The route is aimed at the formation of the hero in `party` (`SetFormation`,
  /// 0x00417790: the retry's mode 2), and follows its samples.
  bool form_aimed = false;
  /// `[retry+0x10]`: the formation's sample count when this route was laid;
  /// -1 to lay it again at the next step.
  std::int32_t form_seen = -1;

  /// The unit's own speed in world units per second, after the factor.
  [[nodiscard]] std::int64_t own_rate() const noexcept {
    return static_cast<std::int64_t>(speed) * speed_factor;
  }
  /// Effective speed: the march's pace while it sets one, else `own_rate()`.
  [[nodiscard]] std::int64_t rate() const noexcept {
    return march_rate > 0 ? march_rate : own_rate();
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

/// One `<Class Name=… CentralBlock=… FrontLine=… Wings=…/>`: the class and
/// the three integer weights the layout shares its units out by
/// (docs/formats/formation.md, "The layout"; the loader is 0x005f6ca0).
struct FormationRole {
  std::string class_name;
  std::int32_t central = 0;  ///< `CentralBlock`
  std::int32_t front = 0;    ///< `FrontLine`
  std::int32_t wings = 0;    ///< `Wings`, half to each wing
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
  /// `Width`/`Height`: the step the front line's grid grows by
  /// (0x005f02e0). Each defaults to 1; the loader refuses a 0.
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

  /// The `<Class>` entries in file order. The layout puts them in the
  /// loader's order, most derived first (`formation_lay_out`).
  std::vector<FormationRole> roles;
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

  // -- formations ---------------------------------------------------------
  //
  // One `Formation` per hero (`[hero+0x2b0]`, `sim/formation.hpp`), kept here
  // because the march that reads it is this system's. Made the first time a
  // unit joins, a shape is set or a march is set up, as the hero's
  // constructor makes it; dropped with the hero's movement state.

  /// The hero's formation, or null before anything made one.
  [[nodiscard]] const Formation* formation(ObjectId hero) const noexcept;
  [[nodiscard]] Formation* formation(ObjectId hero) noexcept;
  /// The same, made when missing. The reference lasts until the next
  /// formation is made.
  Formation& formation_of(ObjectId hero);
  /// Every formation, by hero id.
  [[nodiscard]] const std::vector<Formation>& formation_list() const noexcept {
    return formation_objects_;
  }
  /// `AddUnit` (0x005f47e0): `unit` joins `hero`'s formation, pending.
  void join_formation(ObjectId hero, ObjectId unit);
  /// The hero's destructor (0x005304b0): his formation is deleted.
  void drop_formation(ObjectId hero);
  /// `RemoveUnit` (0x005f3290): `unit` leaves `hero`'s formation, and its
  /// route is deleted (0x005d3830 with 0), as the original's last act there.
  void leave_formation(World& world, ObjectId hero, ObjectId unit);

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
  /// `party` is the hero whose party the unit walks in, or `kNoObject` for
  /// any other order, which also takes the unit out of a march it was in. A
  /// march's routes are aimed at the formation instead (`aim_at_formation`).
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
  /// Keeps the position, drops the route: what `Unit.Stop` does to a unit
  /// whose route owns no lock, and what every other caller means by stopping.
  void stop(World& world, ObjectId id);
  /// `Unit.Stop`'s first entry on a unit with a route (0x005d6d4a): ask the
  /// route to stop. A route whose order owns a lock (`lock_owner`) is walked
  /// on until the unit stands on a passable free spot or the route runs out;
  /// any other stops where it is, as `stop`. Returns whether the unit still
  /// has a route to walk.
  bool request_stop(World& world, ObjectId id);
  /// Whether `id`'s route is aimed at a formation (`MoveState::form_aimed`)
  /// whose path goes on (`formation_going_on`): what `0x004178d0` and the
  /// formation step ask the formation object (0x005f22f0).
  [[nodiscard]] bool marching(ObjectId id, const MoveState& move) const noexcept;

  // -- the formation's route (docs/formats/formation.md) -------------------

  /// `CVXFormObj::SetDest` (0x005f2cc0) and the retry's `SetDest` under it
  /// (0x0041a4c0): the centre on the hero, the lock flag, no sample yet, and
  /// the formation's own route laid to `dest`'s band from where the hero
  /// stands -- unless the goal, band and inner bound are the route's already,
  /// when it is searched for only with nothing left of it. Answers whether
  /// the formation has a route to step along; `going_on` is set to match.
  /// The caller settles the orientation and hands out `form_move`.
  bool formation_set_dest(World& world, ObjectId hero, Point dest, std::int32_t range,
                          std::int32_t min_range, bool lock);
  /// `CreateNextSample` (0x005f5bd0): the centre one step on along the
  /// formation's route, the stations laid round it, the sample timed. False
  /// when no step can be taken, which clears `going_on`.
  bool formation_next_sample(World& world, ObjectId hero);
  /// The same at `now`, a decision inside the movement step.
  bool formation_next_sample(World& world, ObjectId hero, GameTime now);
  /// `SetFormation` (0x00417790): `id`'s route aimed at `hero`'s formation --
  /// the hero's own included -- and laid to its station.
  void aim_at_formation(World& world, ObjectId id, ObjectId hero, bool lock);
  /// `RecastPathfind` for a unit aimed at a formation: its route laid again to
  /// its station as it stands now (what `UpdateMove` does for every member
  /// with a retry once it has laid the current sample again, 0x005f5e30).
  void reroute_to_station(World& world, ObjectId id);
  /// 0x005f22f0: false only when the formation has a route and it is spent.
  [[nodiscard]] bool formation_going_on(ObjectId hero) const noexcept;
  /// Where `id` stands in `form`: the hero at the centre (0x005f5a84), a
  /// member at its slot's sample; `kNoSample` for neither.
  [[nodiscard]] static Point station_of(const Formation& form, ObjectId id) noexcept;
  /// `0x004178d0`: whether `id`'s order owns a destination lock -- the owner
  /// it names, or none. A march member's does once the march is over and the
  /// formation's flag was set; any other order's when `dest_lock` is set.
  [[nodiscard]] bool lock_owner(ObjectId id, const MoveState& move) const noexcept;
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
  /// `CVXPathRetry::IsArrived` (0x00417c10) on the order the unit holds: in
  /// its band round its goal -- the target's position for an object goal --
  /// and, for an order that owns a lock, on a free spot. False with no order
  /// (`goto_active`). What the `Goto` family answers at its re-entry.
  [[nodiscard]] bool is_arrived(const World& world, ObjectId id) const;

  /// The pathfinder. Scratch state, never hashed; of it only the smart
  /// pathfinder's build is saved, as the calls that made it.
  [[nodiscard]] PathFinder& pathfinder() noexcept { return finder_; }
  [[nodiscard]] const PathFinder& pathfinder() const noexcept { return finder_; }

  /// The smart pathfinder's builder over the grid as it is now (0x004165a0;
  /// docs/formats/pathfinder.md). Nothing while one exists.
  void start_smart_build();
  /// One `SPF*` command (`PathFinder::smart_build`). The cells the areas
  /// finder blocks at its end go into this system's grid **without** a new
  /// generation: the original sets the bits in its live grid and searches
  /// nothing again for it.
  bool smart_build(SpfCall call);

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

  /// What `ring_goal` aims a search at.
  struct RingGoal {
    /// Where the search heads (`[0x008bfc10]`): the goal point nearest the
    /// band's centre, before any is struck off, or the centre with none.
    Point at;
    /// Whether any goal point is left. `false` is the search's status 1: no
    /// goal point is left -- none passable, or every one taken.
    bool free = true;
    /// Every goal point laid, in order, each marked live or struck off --
    /// the request's `goal_points`. Valid until the next `ring_goal`.
    std::span<const GoalPoint> points;
  };

  /// The request's owner as the search asks about it: `spot_free` with the
  /// unit's radius (0x0040a990, `[class+0x2dc]`).
  class OwnerSpot final : public SpotTest {
   public:
    OwnerSpot(const MovementSystem& movement, const World& world, ObjectId id)
        : movement_(movement), world_(world), id_(id), radius_(movement.radius_of(world, id)) {}
    [[nodiscard]] bool free(Point at) const override {
      return movement_.spot_free(world_, id_, at, radius_);
    }
    [[nodiscard]] std::int32_t radius() const override { return radius_; }

   private:
    const MovementSystem& movement_;
    const World& world_;
    ObjectId id_;
    std::int32_t radius_;
  };
  /// Where a route for an arrival band ends: `0x00417830`'s goal rings, and
  /// for an `owner` -- the unit, when its order owns a lock; `kNoObject`
  /// otherwise -- without the points a standing unit or another's lock
  /// covers (`0x0040a310`). A `range` of 0 is the centre alone. See the
  /// definition.
  [[nodiscard]] RingGoal ring_goal(const World& world, ObjectId owner, Point centre,
                                   std::int32_t range, std::int32_t min_range) const;

  /// `0x004180b0`: a free spot for `id` out from `centre`, along one heading
  /// drawn from the world's generator. Draws whatever it finds. See the
  /// definition.
  [[nodiscard]] bool free_spot(World& world, ObjectId id, Point centre, Point& out) const;

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
    std::uint64_t free_spot_searches = 0;  ///< full bands that asked `0x004180b0`
    std::uint64_t free_spot_aims = 0;      ///< and were re-aimed at what it found
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
  /// it hashed, all of it needed to resume where the save was taken. Last,
  /// the smart pathfinder's build as the `SPF*` calls made on it.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** the obstruction grid and the formation table, which are
  /// load-time data the embedder supplies from the map and the class data --
  /// the grid must be set first; and `PathFinder`'s scratch. The smart
  /// pathfinder's build is restored by making its saved calls again over that
  /// grid (`PathFinder::replay_smart`).
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

  /// `RecastPathfind` for the formation's route (0x00419730 with no unit):
  /// nothing when the formation stands in its band already; otherwise a
  /// search from `start` with no owner, gates read for the hero's side.
  /// Answers whether the route has anything left afterwards.
  bool formation_recast(World& world, Formation& form, Point start);
  /// `IsArrived` (0x00417c10) for the formation's route: where the formation
  /// stands (0x005f21c0: the hero before any sample, the centre after) within
  /// `[min_range, range]` of the goal by the integer root.
  [[nodiscard]] bool formation_arrived(const World& world, const Formation& form) const;
  /// Where the formation stands, 0x005f21c0.
  [[nodiscard]] Point formation_position(const World& world, const Formation& form) const;
  /// 0x00418210 and 0x00418260 on the formation's route: the first gate
  /// still ahead is within `max(275, FormRadius) + 210` and bars the hero's
  /// side (class 3).
  [[nodiscard]] bool formation_gate_bars(const World& world, const Formation& form) const;
  /// The retry's step (0x00419ee0) for the formation's route, into `out`.
  bool formation_retry_step(World& world, Formation& form, Point& out);
  /// `UpdateMinSpeed` (0x005f5640) and the sample's deadline (0x005f5850).
  void formation_time_sample(const World& world, Formation& form, GameTime now) const;
  /// Where `id` is at `now`: inside the movement step, along its route as
  /// its account stands then; otherwise where the world has it.
  [[nodiscard]] Point position_now(const World& world, ObjectId id, GameTime now) const;
  /// The pace a unit aimed at `form` walks with `left` of its route to go.
  [[nodiscard]] std::int64_t formation_rate(const Formation& form, const MoveState& move,
                                            std::int64_t left, GameTime now) const;
  /// What a step on a route aimed at a formation (0x00419b20) leaves to do:
  /// walk the route as it now stands, or nothing more at this decision.
  enum class FormStep { walk, done };
  FormStep formation_step(World& world, std::size_t entry, GameTime now);
  /// A unit aimed at a formation standing on its station with no route left
  /// to walk: its order stays (`HasPath` reads the retry, 0x005d6ebf), and it
  /// asks again at `until`.
  void stand_on_station(World& world, ObjectId id, MoveState& move, Point here, GameTime until);
  /// The path follower's stop branch (0x00419ee0 with `[retry+0x20] & 6`):
  /// whether the unit stops at `here` rather than take its route's next step
  /// -- an order that owns no lock, a route with nothing left, a passable free
  /// spot, or a gate ahead that bars it.
  [[nodiscard]] bool stop_branch_halts(const World& world, ObjectId id, const MoveState& move,
                                       Point here, std::int64_t travelled) const;
  /// And the stop: `Unit::Stop`'s (bit 1) ends the order with the route, as
  /// its re-entry deletes the retry; a failed search's (bit 2 alone) ends the
  /// walk and keeps the order.
  void halt(World& world, ObjectId id, MoveState& move);
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
  /// The unit's walk animation's step (0x005d2920), or 0 with none.
  [[nodiscard]] std::int32_t walk_step(const World& world, ObjectId id,
                                       const MoveState& move) const;
  [[nodiscard]] std::int32_t formation_stride_or_walk(const World& world, ObjectId id,
                                                      const MoveState& move) const;

  std::vector<Entry> states_;  ///< sorted by id; iteration order is world state
  ObstructionGrid grid_;
  const ClassGraph* classes_ = nullptr;
  FormationTable formations_;
  PathFinder finder_;
  std::uint32_t grid_generation_ = 1;

  /// World state, saved and (the radius) hashed: see `formation`.
  std::vector<Formation> formation_objects_;

  /// World state, saved: see `static_locks()`.
  std::vector<StaticLock> static_locks_;
  bool static_locks_built_ = false;

  // -- scratch, rebuilt every turn; never hashed, never saved -------------
  AvoidanceCounters counters_;
  mutable std::vector<ClassTraits> traits_;
  mutable std::vector<std::uint8_t> traits_known_;
  mutable const ClassGraph* traits_graph_ = nullptr;
  /// `spot_free`'s candidates, and `ring_goal`'s.
  mutable std::vector<ObjectId> spot_scratch_;
  /// `ring_goal`'s goal points and what may cover them.
  mutable std::vector<Point> ring_points_;
  mutable std::vector<GoalPoint> ring_goal_points_;
  mutable std::vector<StaticLock> ring_takers_;
  /// Per entry, how its route is walked this turn: still, step by step, or
  /// the whole turn at once.
  std::vector<std::uint8_t> mode_;
  /// The boundary the running turn began at, and the one it ends at.
  GameTime turn_t0_ = 0;
  GameTime turn_end_ = 0;
  /// Inside `advance`'s decisions, where `mode_` and `since_` describe the
  /// turn being stepped.
  bool deciding_ = false;
  /// A route search's gate lines, and the crossings it is tested against.
  CellOverlay barrier_;
  std::vector<GateCrossing> crossings_;
  /// The gates handed to the smart pathfinder for one search.
  std::vector<const GateLines::Line*> gates_;
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
  /// How long the first run of the call waits before its re-entry answers:
  /// the whole of it (0x005d65e8), whatever the route. See `run_goto`.
  std::int64_t slice = 0;
  /// **How long the call may go without a route before it ends the script.**
  /// `0` ends it at the first search that lays none, a negative value never
  /// does, and a walk, however long, is never timed. The four bodies read it
  /// identically -- `Goto` to a point (0x005d61b0, give-up at 0x005d64db),
  /// `GotoEnter` (0x005d6620, at 0x005d6942), and `Goto` to an object
  /// (0x005d6b90) and `GotoAttack` (0x005d6a90) through their shared body
  /// 0x005d40c0 (at 0x005d4429):
  ///
  ///   * **arrival is the band and nothing else.** On re-entry the call answers
  ///     `CVXPathRetry::IsArrived` (0x00417c10), and a point route is arrived
  ///     only within its range. The flag that would call a finished walk
  ///     arrived (bit 0x80 of `[retry+0x20]`) is set only by the formation
  ///     march's step (0x00419c4c, 0x00419e19, inside 0x00419b20). So the end
  ///     of a partial route is not arrival, and neither is a route that was
  ///     never laid.
  ///   * **the failure is stamped, not the order.** When `SetDest`
  ///     (0x0041a4c0) lays no route and the unit is not arrived, the call
  ///     stamps `[unit+0x150]` with the time if it is clear, and when
  ///     `give_up >= 0` and the time since the stamp (0x005d2e90) has reached
  ///     it, it returns 2 -- **the script ends there** (`script/host.hpp`).
  ///     A laid route or an arrival clears the stamp (0x005d6528, 0x005d6482);
  ///     a new order does not. Here it is `MoveState::goto_failed_at`.
  ///   * **a failing unit searches less and less often.** Before `SetDest`,
  ///     with the stamp set, the call draws `rand(0, 2n)` from the world's
  ///     generator, `n` being the whole 2,048 ms spans since the stamp, at
  ///     most 10 (0x005d2eb0); anything but 0 is `SetDest`'s first argument,
  ///     which for an unchanged goal skips the search and answers from the
  ///     route the record already has (0x0041a513, 0x0041a53c). A unit five
  ///     seconds stuck searches one call in five, and one stuck for 21
  ///     seconds or more one call in twenty-one -- **but only with no stop
  ///     bit set**: a search that laid nothing leaves bit 2
  ///     (`MoveState::route_failed`), and `SetDest` then searches at every
  ///     call whatever was drawn (0x0041a4da), so the draw binds a unit whose
  ///     route has run out, not one whose search failed.
  ///   * **the end of the unit's command clears the stamp**, inferred: slot 9
  ///     of the seven unit vtables (0x005d2940) clears it, with the walking
  ///     flags, before the base (0x005b5160) pops the command queue's head.
  ///     So each command's give-up starts from its own first failure.
  ///
  /// This used to be read as a total timeout in which anything non-positive
  /// meant no limit, from the shipped loop `while (!.Goto(pt, 0, 2000, true,
  /// 0) && .HasPath)`: a `0` read as "at once" seemed to stop every such loop
  /// on its second call. It does not, because a walk is never timed. What the
  /// misreading cost is the other half: a unit whose route was cut back to
  /// where it stands never left `UNIT_ADVANCE.VS`'s loop, and searched again
  /// every two seconds for as long as the match lasted.
  std::int64_t give_up = -1;
  /// `SetDest`'s lock flag: `Goto` (`0x005d645c`) and `GotoAttack`
  /// (`0x005d435b`, `0x005d43a6`) pass 1, `GotoEnter` (`0x005d68c3`) 0.
  bool lock_destination = false;
  /// Whether the call drops the unit's combat target once the unit is out of
  /// any holder: the point form of `Goto` does, at 0x005d639a, after its
  /// step-out waits and before it measures anything. Not a field of the
  /// original's; set by `Goto` and `GotoEnter` (0x005d6810), not by
  /// `GotoAttack`, whose entry point sets the target instead (0x005d6b20).
  bool drop_target = false;
  /// The object forms' worker (0x005d40c0): `Goto(Obj)` and `GotoAttack`.
  /// A target that is the mover answers true at once; one the mover may not
  /// approach (`goto_may_approach`) lays no route and waits the slice, and so
  /// does a target naming nothing (`target` is then `kNoObject` with this set).
  bool object_form = false;
};

/// The range `SetDest` keeps for a point goal (0x0041a4c0): at most 999.
[[nodiscard]] constexpr std::int32_t point_route_range(std::int32_t range) noexcept {
  return range > 999 ? 999 : range;
}

/// The range `SetDest` keeps for an object goal (0x0041a5c0): clamped to 999
/// and then, above 919, stored as 920 -- so 920 is the most an object
/// route's band ever is.
[[nodiscard]] constexpr std::int32_t object_route_range(std::int32_t range) noexcept {
  if (range > 999) range = 999;
  return range > 919 ? 920 : range;
}

/// Whether `mover` may route to `target` (0x005d40c0 at 0x005d4291 ..
/// 0x005d42d7): anything that is not a unit, and a unit in no holder that is
/// either not hidden or hidden from nobody on the mover's side. **Approximated:**
/// the side mask `[record+0x18]` is not modelled, so a hidden unit is
/// approached only by its own player's units.
[[nodiscard]] bool goto_may_approach(const World& world, ObjectId mover, ObjectId target);

/// The shared body of the `Goto` family. Returns the host outcome the entry
/// point should return: `true` on arrival, `false` with a suspension while
/// walking or searching, a `retry` while a held unit waits to step out of its
/// holder, and `finish` once it has gone `give_up` without a route.
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
/// `Stop(ms)` **returns a bool and may suspend**, which the call sites settle
/// and an earlier reading of it as a void procedure got wrong. Of its 53
/// sites, 34 are `while (!.Stop(1000));` and 7 are `if (.Stop(2000))`; a void
/// `Stop` yields nil, `!nil` is true, and every one of those `while` loops
/// spins until the instruction budget traps the script. 0x005d6c90 answers
/// true **at once, without suspending,** for a unit with no route (and a held
/// one); only a unit with a route is asked to stop, suspended for `ms`, and
/// answered false at the re-entry while it is still walking to a free spot to
/// stop on (`stop_impl` in `src/sim/movement.cpp`). The idle loops that call
/// it are paced by the `Idle` or `Sleep` beside it, not by `Stop`.
///
/// `Goto`'s five arguments are `(destination, range, slice, flag, give_up)`.
/// The first two are established: the shipped call sites pass `.range`,
/// `.sight`, `other.sight/3` and `GetConst("GiveDistance")` in the second
/// position, which are all distances, and the dumps print exactly such a
/// tolerance as `Range`. `slice` is how long the call's first run waits
/// before its re-entry answers whether the unit has arrived -- the whole of
/// it, whatever the route (0x005d65e8; `run_goto` has the protocol) -- and the
/// loop it sits in, `while (!.Goto(pt, 0, 2000, true, 0) && .HasPath)`, calls
/// again on a false. `give_up` is read from
/// `gbr.exe` (0x005d61b0): how long the call may go without a route before the
/// script ends, never when negative; `GotoOrder::give_up` has the addresses
/// and what the earlier reading of it as a total timeout cost. The boolean is
/// recorded and not acted on: the original reads it only when no route is
/// laid (0x005d6565), to pick an animation the call then waits out.
///
/// `Goto/4` is not one of `gbr.exe`'s registrations -- both of its `Goto`s
/// take five -- and the handful of shipped four-argument calls are bound here
/// with no give-up, which is the conservative reading of an argument nobody
/// passed.
void register_movement_host(script::HostRegistry& registry);

}  // namespace imperivm::core::sim
