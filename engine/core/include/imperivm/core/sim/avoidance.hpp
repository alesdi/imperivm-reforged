#pragma once

// Unit-versus-unit avoidance: the arithmetic of `gbr.exe`'s cooperative step
// and destination locks, as pure functions.
//
// `sim/movement.hpp` owns the state and the turn loop; this header owns the
// rules, so that each of them can be tested without a world. Everything below
// is read out of the executable, and every figure names the address it comes
// from; the four places the executable underdetermines what to build are
// labelled inference where they are decided, in `sim/movement.hpp`, and what
// was read and deliberately not built is set out below with its reason; the
// destination locks carry their own two inferences, below.
//
// ## Units contribute nothing to the obstruction grid
//
// The map's `Terrain.pass.grid` is static and authored (`sim/path.hpp`), and a
// unit is never stamped into it. What keeps two units apart is found at run
// time, through the world's spatial buckets: 256-unit buckets, 128 to a row,
// each a linked list through `[obj+0x34]` with a summary of its members'
// `SyncFlags` at `[bucket+0x10]` (the walk is `0x00417090`). Every object is a
// circle of its class's `radius` property (`[class+0x2dc]`, which the class
// reader stores at `0x005a1a8c`). There is no dynamic occupancy grid.
//
// ## The cooperative step: `CVXPathCoop`
//
// Every path a unit owns is wrapped. `CVXPathRetry::SetMedia` (`0x00418770`)
// puts a `CVXPathCoop` (`0x00417530`, 0x1c bytes: the wrapped path, the owner,
// and one `int`) around whatever route it is handed, and the coop's step
// (`0x004172b0`, vtable slot 2 of `0x007ac464`) runs before every step the
// unit takes:
//
//   1. It asks the wrapped path for its next point (`CVXSPath`'s step,
//      `0x0041a740`, which **advances on every call**) and a duration.
//   2. It looks for everything within **its own radius + 40** of that point
//      (`0x0041733b`) and keeps, through the callback `0x00416f60`:
//        * a unit (`SyncFlags` bit 22) whose health is not zero (the unit's
//          slot `0x50`, `0x004e1f50`, is `[obj+0xc0] == 0`), that is not the
//          mover, and that is not in the mover's **party** -- both
//          `[unit+0x1c0]` records present and naming the same march
//          (`0x005d2f80`);
//        * a `CVXDestLock` with **no owner** (`[lock+0x70] == 0`). A lock with
//          an owner is skipped here.
//      Everything kept is remembered for step 5. It **blocks** when its centre
//      is nearer than the two radii summed; a blocker whose `SyncFlags` bit 17
//      is set (it has a path) is *soft*, anything else -- a unit standing
//      still, or a lock -- is *hard*.
//   3. Free: the wait resets to 0 and the unit takes the step.
//   4. Blocked: the wait `retrytime` (`+0x10`) climbs `0 -> 50 -> 75 -> 100 ->
//      666` (the jump table at `0x004174a8`, indexed through `0x004174bc`).
//      A hard block goes straight to 666. A soft one goes there early on a
//      draw of `rand(1, 7) == 1` from the synchronised generator (slot `0x14`
//      of `[0x996ff4] + 0x12a0`). Anything short of 666 is a **hold**: the
//      unit stands where it is for that many milliseconds, and because the
//      wrapped path has already advanced, the next test is at the step after.
//   5. 666 means *give way*: unless the blocked point is the route's last,
//      `0x00416a00` tries five points across the direction of travel and the
//      unit steps to the cheapest -- or to the blocked point itself -- and the
//      wait resets. Nothing ever waits more than 50 + 75 + 100 ms before it
//      moves again, so two units cannot deadlock; they slide past or through.
//
// Nobody is pushed. A standing unit is an obstacle the mover goes round.
//
// ## Destination locks: `CVXDestLock`
//
// A world object of class `DestLock` (`cpp_class="CVXDestLock"`, a debug
// entity that `0x0059f540` blanks outside the editor). Two kinds:
//
//   * **Ownerless** -- reproduced. The map-object loader (`0x00540a20`) ends by
//     calling `0x00540780`, which reads the entity's `<point>` table: type 9
//     makes one lock of radius 20, type 10 one of radius 40, and type 11 a
//     **ring of six** radius-40 locks, 40 units out at steps of
//     `1.0466666…` radians -- the double at `0x007c2498`, which is `3.14 / 3`
//     -- with `_ftol` on each axis. `RAM_ATTACK.VS` calls type 10
//     `etDestLock2`. Not made while a saved game is being restored
//     (`[0x00a8734c]`), because then they come from the save. The cooperative
//     step treats them as hard blocks: they are how a doorway or a tree's
//     footprint keeps a column from giving way into it.
//   * **Owned** -- reproduced (playtest report #13). `SetMedia` makes one at
//     the end of every route whose path retry carries flag bit 0 -- which
//     `Goto` and `GotoAttack` set, passing 1 as `SetDest`'s last argument
//     (`0x005d645c`, `0x005d435b`, `0x005d43a6`), and `GotoEnter` does not
//     (`0x005d68c3`) -- unless the owner's class has `ignore_passability`
//     (`[class+0x31c]`, stored at `0x005a5317`). Its radius is its owner's
//     (`0x0040a580` leaves `[lock+0x74]` at 0); `SetMedia` deletes it with the
//     route it ends, and the `Goto` family's arrival deletes the whole retry
//     (`0x005d3830(0)`); the destructor (`0x0040a660`) clears the owner's
//     `dstlock` field.
//
// **Neither kind is in the sync set.** The constructor (`0x0040a580`) sets
// bit 18 of the `SyncFlags` word and clears bit 31 -- the bit that is set on
// all 7,580 objects the nine dumps print -- and no lock appears in any dump.
//
// ### How owned locks are built here
//
// The cooperative step skips them (`[lock+0x70] != 0` at `0x00416fd2`). Their
// reader is the *is this spot free* test `0x0040a990` / `0x0040a880` -- any
// live unit that is not the asker and has no path, any lock that is not the
// asker's own, within its radius + 40 (or the radius of class `ShipBattle`,
// for a water unit), taken when nearer than the two radii -- and it has no
// march exemption. `MovementSystem::spot_free` is that test. What calls it,
// and what this engine does for each:
//
//   * `CVXPathRetry::IsArrived` (`0x00417c10`), which will not call a unit
//     arrived on a spot that is not free when its retry owns a lock
//     (`0x004178d0`). Built: `decide`, `walk_turn` and `run_goto` ask it. Inside
//     the band on a taken spot a route walks on; at the route's end the path
//     follower (`0x00419ee0`, `0x0041a056`) re-lays it (`RecastPathfind`,
//     `0x00419730`) from where the unit stands.
//   * the path follower's stop branch (0x00419ee0, taken while `Unit::Stop`'s
//     bit 1 is set at `[retry+0x20]`), which stops the owner on the first
//     point that is passable and free and walks on otherwise; with no owner
//     it takes no step. Built: `MovementSystem::request_stop` and `decide`.
//   * the smart pathfinder, handed the owner (`0x004141bd` .. `0x00414263`),
//     which cuts a found route back from its end, one cell at a time, to the
//     first cell whose centre is free, and never tests the start's cell.
//     Built as `truncate_to_free`, **inferred** onto this engine's smoothed
//     polyline in steps of a cell's width. The condition at `0x004141cc` is
//     that the search reached no goal point; this engine cuts every route with
//     an owner, which comes to the same, because a goal point it can reach is
//     one nothing covered when it searched (below).
//   * `0x0040b580`'s walk towards an unreachable goal in radius-sized steps,
//     inside a search this engine does not have. Not built.
//   * the free-spot search `0x004180b0`, which the route search `0x00419110`
//     asks when an owner's search found no goal point left and got nowhere
//     (`0x00419197`: the path's status 1 and a route of no length). Built:
//     `MovementSystem::free_spot`, in `lay_path`. See "Going round", below.
//
// The lock itself is not a separate table: it is the last point of a route
// whose `MoveState::dest_lock` is set, for as long as the route exists. The
// original's lock outlives the route by the time between the unit's arrival
// and its script's next `Goto`-family call; here the unit is standing on or
// short of that point by then, and a standing unit is itself taken.
//
// The search's goal is not the band's centre either. `0x00417830` gives it
// rings of sixteen points (`0x0040a1e0`, `0x00409cf0`) at `r - 1` for `r` from
// the range down to the minimum in steps of 40, six at most -- a ring at 0 is
// the centre once -- and the route ends on the first its search reaches --
// **inferred** here as the one nearest the unit in a straight line;
// `MovementSystem::ring_goal`. A route to the centre stepped straight over a
// band narrower than a walk step and ended on the target itself, which is
// where report #13's attackers all stood.
//
// ### Going round
//
// For an owner, the smart pathfinder strikes goal points off before it
// searches (`0x0040a310`, at `0x004138fe`): every standing unit and every lock
// but the owner's own -- the free-spot test's takers, `0x00409650` -- strikes
// the points within its radius plus the owner's, **that distance included**,
// among the bodies whose centre lies in the goal set's box widened by twice
// the margin. So a unit whose nearest side of a target is taken routes to the
// nearest point that is not, round the target, rather than to the taken one
// and back to a free spot behind it -- which is where this engine's attackers
// waited before.
//
// With no goal point left (status 1) the search heads for the one nearest the
// centre, as they were added (`[0x008bfc10]`), and the route is cut back. When
// that gets the unit nowhere, and the retry has not already (flags 8 and 0x40
// of `[retry+0x20]`), the route search sets flag 8 and asks `0x004180b0`:
//
//   * one heading, `rand(0, 359)` degrees from the synchronised generator --
//     **a draw, and so state** -- and a step of the unit's radius along it,
//     `(trunc(r sin a), trunc(r cos a))` with `Rot`'s constant (`0x007ac490`);
//   * up to forty probes from the band's centre outward, the first passable
//     free spot (`0x0040a990`) the answer; a probe off the map ends it. Its
//     opening test, on `[squad+0x7c]`, reads a field nothing sets but to 0
//     and never refuses.
//
// A spot more than 200 units nearer the centre than the unit stands
// (`0x00419230`, both distances through the integer square root) becomes the
// route's goal, exactly, and flag 0x40 is set; the order's band stays the
// target's, so arriving there is not arriving. Flag 8 lasts until `SetDest`
// names another destination (`0x0041a594`); 0x40 until the route has been
// walked to its end (`0x0041a097`). So a unit queued behind a full band is
// sent round it once, and one already at its edge draws and stays.
//
// An earlier reading -- re-aim or refuse an order to an occupied point up front
// -- refused 298 orders in 300 turns on Balcans, almost all deer grazing a few
// dozen units from a herd-mate. That is not what these do: an order is never
// refused, a route is cut short, and a unit on a taken spot stays there, not
// arrived, until its script asks again.

#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

/// The query around a step's end point reaches this far past the mover's own
/// radius: `add eax, 0x28` at `0x0041733b`. A blocker bigger than 40 whose
/// centre is between the query and the summed radii is not seen, which is the
/// original's and is kept.
inline constexpr std::int32_t kCoopQueryMargin = 40;

/// The *give way* value of `retrytime`: `0x29a`.
inline constexpr std::int32_t kGiveWay = 666;

/// The escalation, as `0x004174a8`'s table has it.
inline constexpr std::int32_t kFirstHold = 50;
inline constexpr std::int32_t kSecondHold = 75;
inline constexpr std::int32_t kThirdHold = 100;

/// A soft block gives way early on `rand(1, kEarlyGiveWayOdds) == 1`.
inline constexpr std::int32_t kEarlyGiveWayOdds = 7;

/// Sidestep candidates run `k = -2 .. 2` (`0x00416cc5`), each `k` quarters
/// of the step turned a right angle (`sar 2` at `0x00416a7b`), and a
/// candidate costs `5 * |k|` on top of its worst overlap (`0x00416c97`).
inline constexpr std::int32_t kSidestepReach = 2;
inline constexpr std::int32_t kSidestepDivisor = 4;
inline constexpr std::int32_t kSidestepCost = 5;
/// The score a candidate has to beat: `0xffff` at `0x00416a99`.
inline constexpr std::int32_t kSidestepUnscored = 0xffff;

/// A formation march's sampled path steps 200 units (`0x0041994c`); every
/// other route steps its unit's walk stride.
inline constexpr std::int32_t kFormationStride = 200;

/// The free-spot search (`0x004180b0`) draws its heading as `rand(0, 359)`
/// degrees (`0x00418100`) and probes at most forty points along it
/// (`0x004181d0`); the route search re-aims at what it finds only when that
/// is more than 200 units nearer the goal than the unit stands
/// (`0x00419230`).
inline constexpr std::int32_t kFreeSpotHeadings = 360;
inline constexpr std::int32_t kFreeSpotProbes = 40;
inline constexpr std::int32_t kFreeSpotGain = 200;

/// The `<point>` types that make ownerless locks, and their radii
/// (`0x005407d7` .. `0x005407f4`).
inline constexpr std::int32_t kLockPointSmall = 9;
inline constexpr std::int32_t kLockPointMedium = 10;
inline constexpr std::int32_t kLockPointRing = 11;
inline constexpr std::int32_t kLockRadiusSmall = 20;
inline constexpr std::int32_t kLockRadiusMedium = 40;
/// A type-11 point is not one lock of 80 but six of 40 (`0x00540987`).
inline constexpr std::int32_t kLockRadiusRing = 40;

/// The ring's six offsets: `(trunc(40 * cos(i * 1.0466666666666666)),
/// trunc(40 * sin(i * 1.0466666666666666)))` for `i = 0 .. 5`. Computed from
/// the retail double, not from pi: none of the twelve products lies within a
/// millionth of an integer, so the x87's wider mantissa cannot move one.
inline constexpr Point kLockRing[6] = {{40, 0},   {20, 34},  {-19, 34},
                                       {-39, 0},  {-20, -34}, {19, -34}};

/// An ownerless lock: a circle no unit may give way into.
struct StaticLock {
  Point at;
  std::int32_t radius = 0;
  friend constexpr bool operator==(const StaticLock&, const StaticLock&) = default;
};

/// The locks one `<point>` of `type` at world position `at` makes, appended.
/// Types other than 9, 10 and 11 make none.
void locks_of_point(Point at, std::int32_t type, std::vector<StaticLock>& out);

/// The cooperative step's escalation: `0 -> 50 -> 75 -> 100 -> 666`, and any
/// other value unchanged -- the table's default arm.
[[nodiscard]] constexpr std::int32_t next_retry_time(std::int32_t retry) noexcept {
  switch (retry) {
    case 0:
      return kFirstHold;
    case kFirstHold:
      return kSecondHold;
    case kSecondHold:
      return kThirdHold;
    case kThirdHold:
      return kGiveWay;
    default:
      return retry;
  }
}

/// How many equal steps `CVXSPath` cuts a leg of `length` into for a stride
/// of `stride` (`0x0041a7c2` .. `0x0041a824`): `n = length / stride`, or one
/// more when that makes the step nearer the stride, compared in 16.16; never
/// fewer than one.
[[nodiscard]] std::int64_t steps_in_leg(std::int64_t length, std::int32_t stride) noexcept;

/// The first step boundary strictly after `after` along `waypoints`, as an arc
/// length -- each leg cut by `steps_in_leg`, so a step never rounds a corner
/// and every waypoint is a boundary. The route's length when there is none.
[[nodiscard]] std::int64_t next_step_boundary(std::span<const Point> waypoints,
                                              std::int32_t stride, std::int64_t after) noexcept;

/// Something the sidestep scores against: where it is and the radius
/// `0x00416a00` reads for it -- its class's, which for a lock is the
/// `DestLock` class's `radius`, 0, rather than the lock's own.
struct Neighbour {
  Point at;
  std::int32_t radius = 0;
};

/// `0x00416a00`: where a unit at `mover` gives way instead of stepping to
/// `step_end`.
///
/// With `(dx, dy) = step_end - mover`, candidate `k` for `k = -2 .. 2` is
/// `step_end + k * (trunc(-dy / 4), trunc(dx / 4))`. A candidate is admissible
/// when its cell and the cell of its midpoint with `step_end` (halved with
/// truncation) are both free, and when it is deep water (terrain index 13,
/// sampled half a cell rounded) exactly when the mover is a water unit. It
/// scores `5 * |k|` plus the largest positive `r_n + mover_radius - isqrt(d²)`
/// over `neighbours`. The strictly lowest score under `0xffff` wins, so a tie
/// keeps the earlier `k`; when nothing is admissible, or `step_end` is where
/// the mover stands, the answer is `step_end`.
[[nodiscard]] Point pick_sidestep(Point step_end, Point mover, std::int32_t mover_radius,
                                  std::span<const Neighbour> neighbours,
                                  const ObstructionGrid& grid, const Grid& terrain,
                                  bool water_unit) noexcept;

}  // namespace imperivm::core::sim
