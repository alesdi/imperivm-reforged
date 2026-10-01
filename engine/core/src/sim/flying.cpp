// Flight: the pickers, the altitude and the height layer. See
// include/imperivm/core/sim/flying.hpp for what is read off the executable,
// what is inferred, and what is still unknown.

#include "imperivm/core/sim/flying.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/heading.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::HostRegistry;
using script::Value;

/// The terrain-type byte a landing spot must match. The same 13
/// `IsPointInWater` tests for, and `sim/world_host.cpp` says where it comes
/// from; repeated here as a named constant rather than reached across for,
/// because the two answer different questions about the same layer.
constexpr std::uint32_t kTerrainWater = 13;

/// How close to an edge `AdjustFlyDir` starts steering. 0x0051bed2 and its
/// three siblings, all `0x320`.
constexpr std::int32_t kEdgeMargin = 800;

/// `PickFlyingPoint`'s scan: twelve directions, thirty degrees apart, and a
/// score under this one is accepted without looking at the rest (0x0051c5a9).
constexpr std::int32_t kScanSteps = 12;
constexpr std::int32_t kGoodEnough = 60;
/// The best-so-far the scan starts at (0x0051c55d). A candidate has to beat it,
/// so twelve candidates at 10,000 or more leave it unchanged -- see the
/// header's note about what the original returns then.
constexpr std::int32_t kNoCandidate = 10000;
/// The radius the crowding term counts airborne neighbours in, and its weight.
constexpr std::int32_t kCrowdRadius = 100;
constexpr std::int32_t kCrowdWeight = 50;

/// `PickLandingPoint`: five tries per flock-mate, jittered by this much, and
/// the reference distance the accepted spot is measured against.
constexpr std::int32_t kLandingTries = 5;
constexpr std::int32_t kLandingJitter = 100;
constexpr std::int32_t kLandingReference = 100;
/// `0x0051b635` and `0x0051b63a`: between half and twice the reference.
constexpr std::int32_t kNearestPercent = 50;
constexpr std::int32_t kFarthestPercent = 200;
/// `0x0051b68f`: the cosine, in percent, a landing spot must be inside of.
/// 57/100 is about 55.2 degrees.
constexpr std::int32_t kAheadPercent = 57;

/// The flock query's radius when the argument is `-1` (0x004fee3c): the
/// receiver's own sight, less a hundred.
constexpr std::int32_t kSightMargin = 100;

constexpr std::int32_t kQ30Shift = 30;
constexpr std::int64_t kQ30One = std::int64_t{1} << kQ30Shift;

/// `cos(a * 6.28/360)` and `sin(a * 6.28/360)` at Q30, for a = 0, 30, ... 330.
///
/// Generated from the retail constant at `0x7beb58` and checked against the
/// double the original computes for every radius from 0 to 4,000: zero
/// mismatches. Q30 is enough because a radius that a map can hold is under
/// 32,768, so the product is within 3e-5 of the double.
constexpr std::int64_t kScanCosQ30[kScanSteps] = {
    1073741824, 930030172,  537364499,   855049,     -535883284, -929174337,
    -1073740462, -930883649, -538844352, -2565146,   534400709,  928316144,
};
constexpr std::int64_t kScanSinQ30[kScanSteps] = {
    0,          536624062,  929602549,  1073741484, 930457205,  538104596,
    1710098,    -535142166, -928745535, -1073738760, -931309501, -539583766,
};

// --------------------------------------------------------------------------
// small shared readings
// --------------------------------------------------------------------------

/// The receiver as a live object, or null.
[[nodiscard]] const WorldObject* receiver_object(CallContext& ctx, World*& world) noexcept {
  world = world_of(ctx);
  if (world == nullptr || ctx.count() == 0) return nullptr;
  const Value& value = ctx.arg(0);
  if (!value.is_object()) return nullptr;
  const script::ObjectRef ref = value.as_object();
  if (ref.type != kTypeObj) return nullptr;
  return world->find(ref.id);
}

[[nodiscard]] std::int32_t int_arg(CallContext& ctx, std::size_t index) noexcept {
  if (ctx.count() <= index) return 0;
  const Value& value = ctx.arg(index);
  return value.is_integer() ? value.as_integer() : 0;
}

/// `[obj+0x48]`/`[obj+0x4c]` -- the heading `Obj::GetDir` returns.
///
/// This engine keeps it on `MoveState::facing`, normalised to `kFacingLength`,
/// and that is the same field in every sense that matters here: `Obj::GetDir`
/// (0x005ac629) reads the original's, `GetDir` reads this one, and all four of
/// this file's readers of a heading are readers of what `GetDir` returns.
/// The *length* is this engine's own -- 1,024 rather than whatever the original
/// leaves there -- which every consumer here immediately normalises away with
/// `SetLen` or an angle.
[[nodiscard]] Point facing_of(World& world, ObjectId id) noexcept {
  MovementSystem* movement = movement_system(world);
  if (movement == nullptr) return Point{0, kFacingLength};
  const MoveState* state = movement->find(id);
  return state == nullptr ? Point{0, kFacingLength} : state->facing;
}

/// The map's inclusive high corner, in world units.
///
/// The rectangle the original keeps at `[world+0xb4]` is `(0, 0, w-1, h-1)` and
/// `GetMapRect` already says so. A world with no match system falls back to the
/// height layer's own extent, which is the same square by construction (all six
/// of a map's layers cover what `map.xml` declares), so that a synthetic world
/// with a layer and no match still clamps somewhere sensible.
[[nodiscard]] std::int32_t map_high(const World& world) noexcept {
  if (const MatchSystem* match = match_system_of(world); match != nullptr) {
    const std::int32_t size = match->rules().map_size;
    if (size > 0) return size - 1;
  }
  const std::uint32_t extent = world.height().extent_x();
  return extent > 0 ? static_cast<std::int32_t>(extent) - 1 : 0;
}

[[nodiscard]] std::int32_t clamp_to(std::int32_t v, std::int32_t low, std::int32_t high) noexcept {
  if (v < low) return low;
  if (v > high) return high;
  return v;
}

/// `point::SetLen`'s arithmetic, including its answer for the zero vector.
///
/// **Not `GetVecByDir`**, and the two differ exactly here: 0x00697d81 writes the
/// requested length into *y* and zero into *x*, so a direction-less vector is
/// given one, pointing north. Both pickers end in this and neither ends in
/// `sim/heading.cpp`'s `vec_by_dir`, which answers the zero vector instead.
[[nodiscard]] Point set_len(Point v, std::int32_t length) noexcept {
  const std::int64_t x = v.x;
  const std::int64_t y = v.y;
  const std::int64_t current = isqrt(x * x + y * y);
  if (current == 0) return Point{0, length};
  return Point{static_cast<std::int32_t>(x * length / current),
               static_cast<std::int32_t>(y * length / current)};
}

/// The `+/-(n/5)` jitter both pickers end with, drawn **Y first**.
///
/// The draw order is not cosmetic: one generator serves the whole simulation
/// and swapping two consecutive draws changes every number after them. Y first
/// is what 0x0051c639 and 0x0051ca9d do, and `RandomOffset` (0x004c7c50) and
/// `PickLandingPoint`'s per-mate jitter draw in the same order.
[[nodiscard]] Point jitter(World& world, std::int32_t spread) noexcept {
  const std::int32_t y = world.rng().between(-spread, spread);
  const std::int32_t x = world.rng().between(-spread, spread);
  return Point{x, y};
}

/// The flock: objects of the receiver's own class within `sight - 100` of it.
///
/// `0x004fee10` with a radius argument of `-1`, which that helper turns into
/// the receiver's `sight` less a hundred, and a class string taken from the
/// receiver's own class descriptor. Resolved to a `ClassIndex` here rather than
/// to the name and back, which is the same set: the descriptor's name is the
/// class's name.
///
/// **The receiver is in its own flock**, at distance zero, exactly as the
/// original's circle is. Both callers then test each member for a state the
/// receiver does not have when it calls, so nothing turns on it -- but it is a
/// membership rule and guessing at it silently would be the wrong shape of
/// mistake.
std::size_t flock_of(World& world, const WorldObject& slot, std::vector<ObjectId>& out) {
  const std::int32_t radius = slot.sight - kSightMargin;
  const ClassFilter filter = slot.class_index == kNoClass ? ClassFilter{}
                                                          : ClassFilter::of(slot.class_index);
  return world.objects_in_radius(world.resolve_position(slot.id), radius, filter, out);
}

// --------------------------------------------------------------------------
// the height layer
// --------------------------------------------------------------------------

/// `(b - a) * f / cell`, truncated toward zero, which is what the original's
/// `cdq; and edx, cell-1; add eax, edx; sar` idiom computes.
[[nodiscard]] std::int32_t lerp_step(std::int32_t a, std::int32_t b, std::int32_t f,
                                     std::int32_t cell) noexcept {
  return a + (b - a) * f / cell;
}

// --------------------------------------------------------------------------
// PickFlyingPoint
// --------------------------------------------------------------------------

/// `0x0051c2f0` -- how bad a candidate offset is. Lower is better.
///
/// Two terms and no third: fifty per airborne creature within a hundred units
/// of where the candidate lands, plus the turn the bird would have to make to
/// get there. The turn is the difference of two `GetAngleByDir` readings --
/// `0x0051b550`, called at `0x0051c33d` and `0x0051c346`, is the same `asin`
/// arithmetic as the entry point -- folded into `[0, 180]`. See
/// `sim/heading.hpp`.
[[nodiscard]] std::int32_t score_candidate(World& world, const WorldObject& slot, Point offset,
                                           std::vector<ObjectId>& scratch) {
  const Point at = world.resolve_position(slot.id);
  const Point candidate{at.x + offset.x, at.y + offset.y};

  std::int32_t turn = angle_of_dir(offset) - angle_of_dir(facing_of(world, slot.id));
  if (turn < 0) turn += 360;
  if (turn > 180) turn = 360 - turn;

  // The original passes flag mask 0x400000 to its collector, which is the
  // `in_air` bit: what makes a spot bad is other birds already flying there,
  // not the town underneath it.
  world.objects_in_radius(candidate, kCrowdRadius, ClassFilter{}, scratch);
  std::int32_t crowd = 0;
  for (const ObjectId id : scratch) {
    const WorldObject* other = world.find(id);
    if (other != nullptr && other->state.flags.in_air) ++crowd;
  }
  return kCrowdWeight * crowd + turn;
}

/// The heading `PickFlyingPoint` settles on, before `SetLen` and the jitter.
[[nodiscard]] Point pick_flying_dir(World& world, const WorldObject& slot, std::int32_t radius) {
  std::vector<ObjectId> members;
  std::vector<ObjectId> scratch;

  // Flock-follow first: the first mate that is **in** a state and **in** the
  // air, which is a bird whose heading is live because it is mid-flight.
  // `IsInState` is the *absence* of an animation (`sim/anim.cpp`), so the
  // original's `if (IsInState(mate)) continue;` at 0x0051c537 selects a bird
  // that is animating.
  flock_of(world, slot, members);
  for (const ObjectId id : members) {
    const WorldObject* mate = world.find(id);
    if (mate == nullptr || !mate->animating) continue;
    if (!mate->state.flags.in_air) continue;
    return facing_of(world, id);
  }

  // Otherwise the twelve-direction scan: first candidate under 60, else the
  // best of twelve. The zero vector is what the original leaves in place when
  // nothing scores at all; see the header.
  Point best{0, 0};
  std::int32_t best_score = kNoCandidate;
  for (std::int32_t i = 0; i < kScanSteps; ++i) {
    const Point candidate = scan_offset(radius, i);
    const std::int32_t score = score_candidate(world, slot, candidate, scratch);
    if (score >= best_score) continue;
    best_score = score;
    best = candidate;
    if (score < kGoodEnough) break;
  }
  return best;
}

// --------------------------------------------------------------------------
// PickLandingPoint
// --------------------------------------------------------------------------

/// `0x0051c210` -- may this object put itself down here?
///
/// Three tests and every one of them exists in this engine already: inside the
/// map rectangle (tested twice, once biased by half a terrain cell on both
/// axes), the terrain-type byte at the biased point being water **if and only
/// if** the class is a water unit, and the obstruction cell under the raw point
/// being free.
///
/// The `+32` bias is `IsPointInWater`'s own, and `docs/formats/pass.md` records
/// it: it rounds a point to the nearest 64-unit cell rather than the one it sits
/// inside.
[[nodiscard]] bool can_land_impl(World& world, const WorldObject& slot, Point at) noexcept {
  const std::int32_t high = map_high(world);
  const Grid& terrain = world.terrain();
  const std::int32_t half =
      terrain.cell_size() == 0 ? 0 : static_cast<std::int32_t>(terrain.cell_size() / 2);

  if (at.x + half < 0 || at.x + half > high) return false;
  if (at.y + half < 0 || at.y + half > high) return false;
  if (at.x < 0 || at.x > high || at.y < 0 || at.y > high) return false;

  // A world with no layer has nothing to disagree with the class about, so the
  // water test passes for a land unit and fails for a water one -- which is
  // the same answer `IsPointInWater` gives without a layer.
  const bool wants_water = class_flag(world, slot, "water_unit");
  bool is_water = false;
  if (terrain.cell_size() != 0) {
    const auto cx = static_cast<std::uint32_t>(at.x + half) / terrain.cell_size();
    const auto cy = static_cast<std::uint32_t>(at.y + half) / terrain.cell_size();
    is_water = terrain.cell(cx, cy) == kTerrainWater;
  }
  if (is_water != wants_water) return false;

  const MovementSystem* movement = movement_system(world);
  if (movement == nullptr) return true;  // no grid: nothing is obstructed
  return !movement->grid().blocked(at);
}

/// `0x0051b600` -- is `at` a sensible distance and direction from the bird?
///
/// Between half and twice `reference` away, and within about 55 degrees of the
/// heading. Both comparisons are done in percent with a truncating divide,
/// which is the original's, and the denominator is forced to 1 when it would be
/// zero rather than guarded with a branch -- 0x0051b674, and it matters because
/// a bird with no heading would otherwise divide by nothing.
[[nodiscard]] bool heading_ok_impl(Point from, Point heading, Point at,
                                   std::int32_t reference) noexcept {
  const std::int64_t vx = static_cast<std::int64_t>(at.x) - from.x;
  const std::int64_t vy = static_cast<std::int64_t>(at.y) - from.y;
  const std::int64_t distance = isqrt(vx * vx + vy * vy);
  if (reference == 0) return false;
  const std::int64_t percent = distance * 100 / reference;
  if (percent < kNearestPercent || percent > kFarthestPercent) return false;

  const std::int64_t hx = heading.x;
  const std::int64_t hy = heading.y;
  std::int64_t denominator = isqrt(hx * hx + hy * hy) * distance;
  if (denominator == 0) denominator = 1;
  const std::int64_t dot = hx * vx + hy * vy;
  return dot * 100 / denominator > kAheadPercent;
}

// --------------------------------------------------------------------------
// the entry points
// --------------------------------------------------------------------------

/// `GetTerrainHeight(pt)` -- 9 sites, all flight.
HostOutcome fn_get_terrain_height(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetTerrainHeight: no world");
  if (!is_point(ctx.arg(0))) return HostOutcome::failed("GetTerrainHeight: expected a point");
  return HostOutcome::ok_with(Value::integer(terrain_height(*world, unpack_point(ctx.arg(0)))));
}

/// `RandomOffset(n)` -- a point drawn uniformly from the square `[-n, n]^2`.
///
/// 0x004c7c50, and the whole body is two `between(-n, n)` draws, **Y first**.
/// Both sites are `HEN_IDLE.VS`: a 200-unit wander and a 20-unit shuffle whose
/// result the script then compares against its own position, which is how a hen
/// notices it drew (0, 0) and faces `(10, 10)` instead.
HostOutcome fn_random_offset(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RandomOffset: no world");
  if (!ctx.arg(0).is_integer()) return HostOutcome::failed("RandomOffset: expected a size");
  const std::int32_t n = ctx.arg(0).as_integer();
  const std::int32_t y = world->rng().between(-n, n);
  const std::int32_t x = world->rng().between(-n, n);
  return HostOutcome::ok_with(pack_point(Point{x, y}));
}

/// `Unit::dir` -- the heading, as a point. 0x0051c1d0.
///
/// The same two loads `Obj::GetDir` makes, with **no null check**: the original
/// resolves the receiver, leaves the pointer at zero when it cannot, and reads
/// `[0x48]` off it anyway. That is a fault, not a value, so there is nothing to
/// reproduce; an unresolvable receiver answers the zero vector here.
HostOutcome m_dir(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("dir: no world");
  if (slot == nullptr) return HostOutcome::ok_with(pack_point(Point{0, 0}));
  return HostOutcome::ok_with(pack_point(facing_of(*world, slot->id)));
}

/// `Flying::z` -- 5 sites, all `CROW_IDLE.VS`.
HostOutcome m_z(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("z: no world");
  // 0x0051be13 formats a complaint and pushes **0**, which is sea level.
  if (slot == nullptr) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(Value::integer(flying_z(*world, *slot)));
}

/// `Flying::IsLanding` -- `in_air && landing`. 0x0051bd80, zero shipped sites.
///
/// Registered anyway, and the reason is that it is now *decidable*: the flag it
/// reads has a writer for the first time, and an entry point that the executable
/// registers and this engine leaves trapping is a trap waiting for the first
/// modded script. `Flying::IsInAir` beside it has seven sites and the same two
/// bits behind it.
HostOutcome m_is_landing(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("IsLanding: no world");
  return HostOutcome::ok_with(Value::boolean(slot != nullptr && slot->state.flags.in_air &&
                                             slot->state.flags.landing));
}

/// `Flying::PickFlyingPoint(n)` -- where to fly next. 0x0051c3d0.
HostOutcome m_pick_flying_point(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("PickFlyingPoint: no world");
  // 0x0051c430 writes (0, 0) and returns. The crow's guard is on
  // `PickLandingPoint`'s answer rather than this one, so nothing tests it.
  if (slot == nullptr) return HostOutcome::ok_with(pack_point(Point{0, 0}));

  const std::int32_t radius = int_arg(ctx, 1);
  const Point heading = pick_flying_dir(*world, *slot, radius);
  const Point step = set_len(heading, radius);
  const Point drift = jitter(*world, radius / 5);
  const Point at = world->resolve_position(slot->id);
  return HostOutcome::ok_with(
      pack_point(Point{at.x + step.x + drift.x, at.y + step.y + drift.y}));
}

/// `Flying::PickLandingPoint(n)` -- where to come down. 0x0051c6a0.
///
/// It shares `PickFlyingPoint`'s signature and its flock query and nothing
/// else. For each mate that is **on the ground or already landing** it tries
/// five spots around that mate; failing every mate, a one-in-six decides
/// between giving up with `(-1, -1)` and a straight-ahead landing.
///
/// The `ahead` tally is counted over **every** mate, before the on-the-ground
/// test, and only decides whether the give-up roll happens at all -- so a bird
/// with mates in front of it and none of them landable still usually gives up,
/// and a bird entirely alone lands straight ahead without rolling.
HostOutcome m_pick_landing_point(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("PickLandingPoint: no world");
  // 0x0051c701, and **the crow accepts it**: `ptLand.x >= 0` is the shipped
  // guard and zero passes it. Transcribed rather than improved -- the value is
  // the original's answer for a receiver that no longer exists.
  if (slot == nullptr) return HostOutcome::ok_with(pack_point(Point{0, 0}));

  const std::int32_t radius = int_arg(ctx, 1);
  const Point at = world->resolve_position(slot->id);
  const Point heading = facing_of(*world, slot->id);

  std::vector<ObjectId> members;
  flock_of(*world, *slot, members);

  std::int32_t ahead = 0;
  bool tried = false;
  for (const ObjectId id : members) {
    const WorldObject* mate = world->find(id);
    if (mate == nullptr) continue;
    const Point mate_at = world->resolve_position(id);

    // The "mates in front of me" tally, over every mate and before anything
    // else. Same cosine test `heading_ok` ends with, without the distance half.
    {
      const std::int64_t vx = static_cast<std::int64_t>(mate_at.x) - at.x;
      const std::int64_t vy = static_cast<std::int64_t>(mate_at.y) - at.y;
      const std::int64_t hx = heading.x;
      const std::int64_t hy = heading.y;
      std::int64_t denominator = isqrt(hx * hx + hy * hy) * isqrt(vx * vx + vy * vy);
      if (denominator == 0) denominator = 1;
      if ((hx * vx + hy * vy) * 100 / denominator > kAheadPercent) ++ahead;
    }

    if (mate->state.flags.in_air && !mate->state.flags.landing) continue;
    tried = true;
    for (std::int32_t attempt = 0; attempt < kLandingTries; ++attempt) {
      const Point drift = jitter(*world, kLandingJitter);
      const Point candidate{mate_at.x + drift.x, mate_at.y + drift.y};
      if (!can_land_impl(*world, *mate, candidate)) continue;
      if (!heading_ok_impl(at, heading, candidate, kLandingReference)) continue;
      return HostOutcome::ok_with(pack_point(candidate));
    }
  }

  if (tried || ahead > 0) {
    if (world->rng().between(0, 5) != 0) {
      return HostOutcome::ok_with(pack_point(Point{-1, -1}));
    }
  }

  const Point step = set_len(heading, radius);
  const Point drift = jitter(*world, radius / 5);
  return HostOutcome::ok_with(
      pack_point(Point{at.x + step.x + drift.x, at.y + step.y + drift.y}));
}

/// `Flying::AdjustFlyDir(point)` -- steer away from the map's edge, in place.
///
/// Registered with the by-reference point type 0x106 and the body stores
/// through the resolved pointer at 0x0051c090, exactly as `point::Rot` does.
/// The single shipped site is `CROW_IDLE.VS`'s `.AdjustFlyDir(ptDir);` on the
/// line before its `PlayAnim`.
HostOutcome m_adjust_fly_dir(CallContext& ctx) {
  World* world = nullptr;
  const WorldObject* slot = receiver_object(ctx, world);
  if (world == nullptr) return HostOutcome::failed("AdjustFlyDir: no world");
  if (!is_point(ctx.arg(1))) return HostOutcome::failed("AdjustFlyDir: expected a point");
  // 0x0051bea4: an unresolvable receiver leaves the argument alone.
  if (slot == nullptr) return HostOutcome::ok_void();
  ctx.out(1) = pack_point(adjust_fly_dir(facing_of(*world, slot->id), unpack_point(ctx.arg(1)),
                                         world->resolve_position(slot->id), map_high(*world)));
  return HostOutcome::ok_void();
}

/// `Query::GetAverageDirection()` -- the mean heading of a query's members.
///
/// 0x005799d0 sums `[obj+0x48]` and `[obj+0x4c]` over the query's list and
/// divides each by the count, **with no null check**: a stale handle in the
/// list is dereferenced and the original faults. An empty query answers (0, 0)
/// after a complaint, which is 0x00579a60 and is the one degenerate case that
/// is a value rather than a crash.
///
/// The one shipped site is `CROW_IDLE.VS`'s `ptDir = crows.GetAverageDirection()`,
/// guarded by `crows.count != 0` on the same line.
HostOutcome m_get_average_direction(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetAverageDirection: no world");
  const std::vector<ObjectId> members = receiver_objects(*world, ctx.arg(0));
  if (members.empty()) return HostOutcome::ok_with(pack_point(Point{0, 0}));

  std::int64_t x = 0;
  std::int64_t y = 0;
  for (const ObjectId id : members) {
    const Point facing = facing_of(*world, id);
    x += facing.x;
    y += facing.y;
  }
  const auto count = static_cast<std::int64_t>(members.size());
  // Truncating toward zero, which is what the original's `idiv` does. The sums
  // are 64-bit here and 32-bit there; a map would need four million birds
  // pointing the same way to tell the difference.
  return HostOutcome::ok_with(pack_point(Point{static_cast<std::int32_t>(x / count),
                                               static_cast<std::int32_t>(y / count)}));
}

/// The number of `define` calls `register_flying_host` makes. Kept next to the
/// list so the two cannot drift.
constexpr std::size_t kEntryCount = 9;

}  // namespace

// --------------------------------------------------------------------------
// the arithmetic
// --------------------------------------------------------------------------

bool can_land(World& world, const WorldObject& slot, Point at) noexcept {
  return can_land_impl(world, slot, at);
}

bool heading_ok(Point from, Point heading, Point at, std::int32_t reference) noexcept {
  return heading_ok_impl(from, heading, at, reference);
}

std::int32_t sample_height(const Grid& layer, Point at) noexcept {
  const std::uint32_t cell = layer.cell_size();
  if (cell == 0) return 0;
  if (at.x < 0 || at.y < 0) return 0;

  const auto ux = static_cast<std::uint32_t>(at.x);
  const auto uy = static_cast<std::uint32_t>(at.y);
  const std::uint32_t cx = ux / cell;
  const std::uint32_t cy = uy / cell;
  const auto fx = static_cast<std::int32_t>(ux % cell);
  const auto fy = static_cast<std::int32_t>(uy % cell);
  const auto size = static_cast<std::int32_t>(cell);

  // 0x0053dc98 and 0x0053dcb8: the cell to the right and the row below are
  // clamped at the last one, so the far edge repeats rather than wrapping.
  const std::uint32_t nx = cx + 1 < layer.width() ? cx + 1 : cx;
  const std::uint32_t ny = cy + 1 < layer.height() ? cy + 1 : cy;

  const auto top = lerp_step(static_cast<std::int32_t>(layer.cell(cx, cy)),
                             static_cast<std::int32_t>(layer.cell(nx, cy)), fx, size);
  const auto bottom = lerp_step(static_cast<std::int32_t>(layer.cell(cx, ny)),
                                static_cast<std::int32_t>(layer.cell(nx, ny)), fx, size);
  return lerp_step(top, bottom, fy, size);
}

std::int32_t terrain_height(const World& world, Point at) noexcept {
  const std::int32_t high = map_high(world);
  return sample_height(world.height(),
                       Point{clamp_to(at.x, 0, high), clamp_to(at.y, 0, high)});
}

std::int32_t flying_z(const World& world, const WorldObject& slot) noexcept {
  if (!slot.state.flags.in_air) {
    return terrain_height(world, world.resolve_position(slot.id));
  }
  // With no animation ever started there is nothing to interpolate along, and
  // the altitude is the one the last one ended at.
  if (slot.object == nullptr || !slot.timeline.valid()) return slot.state.z_from;
  const std::int32_t cycle = slot.timeline.cycle();
  if (cycle <= 0) return slot.state.z_from;

  // **The boundary is the whole mechanism.** The original tests `now` against
  // the animation's own `[start, end]` with `jb`/`ja` (0x0051b08f), so the
  // instant the animation *ends* still interpolates and answers `z_to` exactly
  // -- and that is the instant the suspended script resumes and calls
  // `PlayAnim` again, which reads `Flying::z` for the next `z_from`. Answer
  // `z_from` there instead and the chain never advances: every animation would
  // start where the last one started and `CROW_IDLE.VS`'s bird would never
  // climb. `AnimTimeline` saturates `elapsed` at the cycle rather than running
  // it on, so the clamp below is where that boundary lives here.
  const std::int32_t elapsed = clamp_to(slot.object->anim.elapsed_ms, 0, cycle);
  const std::int64_t span = static_cast<std::int64_t>(slot.state.z_to) - slot.state.z_from;
  return static_cast<std::int32_t>(slot.state.z_from + span * elapsed / cycle);
}

std::int32_t flying_lift(const World& world, const WorldObject& slot) noexcept {
  if (slot.object == nullptr || !slot.object->is_a(NativeClass::flying_unit)) return 0;
  if (!slot.state.flags.in_air) return 0;
  const std::int32_t ground = terrain_height(world, world.resolve_position(slot.id));
  const std::int32_t lift = flying_z(world, slot) - ground;
  // Below the ground is drawn on it: the original clamps the offset at zero.
  return lift > 0 ? lift : 0;
}

void begin_flight_anim(World& world, ObjectId id, Point at, std::int32_t z) noexcept {
  WorldObject* slot = world.find(id);
  if (slot == nullptr) return;
  const std::int32_t ground_here = terrain_height(world, world.resolve_position(id));

  // 0x0051bbed: `z_from` is `Flying::z` itself, so an animation starts where
  // the last one left off rather than where the object nominally is.
  const std::int32_t from = flying_z(world, *slot);
  slot->state.z_from = from;
  slot->state.z_to = z == -1 ? terrain_height(world, at) : z;
  slot->state.flags.landing = z == -1;
  // 0x0051bc4e. A bird is in the air unless it is descending and already down.
  slot->state.flags.in_air = !(from == ground_here && z == -1);
}

Point scan_offset(std::int32_t radius, std::int32_t index) noexcept {
  if (index < 0 || index >= kScanSteps) return Point{0, 0};
  const std::int64_t r = radius;
  return Point{static_cast<std::int32_t>(r * kScanCosQ30[index] / kQ30One),
               static_cast<std::int32_t>(r * kScanSinQ30[index] / kQ30One)};
}

Point adjust_fly_dir(Point heading, Point p, Point pos, std::int32_t map_high) noexcept {
  const std::int32_t low = kEdgeMargin;
  const std::int32_t high = map_high - kEdgeMargin;
  if (pos.x >= low && pos.x <= high && pos.y >= low && pos.y <= high) return p;

  // Which edge is near, as a number of quarter turns that puts it at low y.
  // The order is the original's and the last case is the fall-through, so a
  // corner is decided by x before y.
  std::int32_t quarters = 0;
  if (pos.x > high) {
    quarters = 3;
  } else if (pos.x < low) {
    quarters = 1;
  } else if (pos.y > high) {
    quarters = 2;
  }

  // `(x, y) -> (-y, x)`, applied to the heading and to the argument together.
  const auto turn = [](Point v) { return Point{-v.y, v.x}; };
  Point dir = heading;
  Point q = p;
  for (std::int32_t i = 0; i < quarters; ++i) {
    dir = turn(dir);
    q = turn(q);
  }
  // Already heading away from the near edge: the original writes nothing at all
  // here, so the caller's point keeps the value it came in with.
  if (q.y > 0) return p;

  // Fifteen degrees, toward whichever side the *heading's* own x says, and the
  // same constant `point::Rot` uses: 0x7bea98 is 15 * 0.017453292519444445.
  const Point turned = rotate_like_gbr(dir, dir.x > 0 ? -15 : 15);
  const bool better = turned.y > 0 || (dir.x > 0 ? turned.x > q.x : turned.x < q.x);
  Point out = better ? turned : q;

  for (std::int32_t i = 0, back = (4 - quarters) % 4; i < back; ++i) out = turn(out);
  return out;
}

std::size_t flying_host_entry_count() noexcept { return kEntryCount; }

std::size_t register_flying_host(HostRegistry& registry) {
  std::size_t defined = 0;
  const auto def = [&](CallKind kind, std::string_view name, std::uint16_t arity,
                       script::HostFn fn) {
    registry.define(kind, name, arity, fn);
    ++defined;
  };

  // Descending corpus call frequency.
  def(CallKind::free_function, "GetTerrainHeight", 1, &fn_get_terrain_height);  // 9
  def(CallKind::member, "z", 0, &m_z);                                          // 5
  def(CallKind::free_function, "RandomOffset", 1, &fn_random_offset);           // 2
  def(CallKind::member, "PickLandingPoint", 1, &m_pick_landing_point);          // 2
  def(CallKind::member, "dir", 0, &m_dir);                                      // 2
  def(CallKind::member, "PickFlyingPoint", 1, &m_pick_flying_point);            // 1
  def(CallKind::member, "AdjustFlyDir", 1, &m_adjust_fly_dir);                  // 1
  def(CallKind::member, "GetAverageDirection", 0, &m_get_average_direction);    // 1
  // Zero shipped sites, and not in `declare_shipped_surface` for that reason.
  // See its body for why it is here anyway.
  def(CallKind::member, "IsLanding", 0, &m_is_landing);                         // 0
  return defined;
}

}  // namespace imperivm::core::sim
