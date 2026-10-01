// Unit-versus-unit avoidance: the pure half. See
// include/imperivm/core/sim/avoidance.hpp for the evidence.

#include "imperivm/core/sim/avoidance.hpp"

#include "imperivm/core/sim/entrance.hpp"

namespace imperivm::core::sim {

void locks_of_point(Point at, std::int32_t type, std::vector<StaticLock>& out) {
  switch (type) {
    case kLockPointSmall:
      out.push_back(StaticLock{at, kLockRadiusSmall});
      return;
    case kLockPointMedium:
      out.push_back(StaticLock{at, kLockRadiusMedium});
      return;
    case kLockPointRing:
      for (const Point& offset : kLockRing) {
        out.push_back(StaticLock{Point{at.x + offset.x, at.y + offset.y}, kLockRadiusRing});
      }
      return;
    default:
      return;
  }
}

std::int64_t steps_in_leg(std::int64_t length, std::int32_t stride) noexcept {
  if (length <= 0) return 0;
  if (stride <= 0) return 1;
  const std::int64_t n = length / stride;
  if (n == 0) return 1;
  // Both candidates' step lengths in 16.16, against the stride in 16.16. The
  // longer count wins only when it is *strictly* nearer (`jl` at 0x0041a824).
  const std::int64_t target = static_cast<std::int64_t>(stride) << 16;
  const std::int64_t fewer = (length << 16) / n;
  const std::int64_t more = (length << 16) / (n + 1);
  const std::int64_t off_fewer = fewer > target ? fewer - target : target - fewer;
  const std::int64_t off_more = more > target ? more - target : target - more;
  return off_more < off_fewer ? n + 1 : n;
}

std::int64_t next_step_boundary(std::span<const Point> waypoints, std::int32_t stride,
                                std::int64_t after) noexcept {
  std::int64_t start = 0;
  for (std::size_t i = 1; i < waypoints.size(); ++i) {
    // The same floored leg length `point_along` walks, so a boundary lands on
    // exactly the point `point_along` answers for it.
    const std::int64_t leg = distance(waypoints[i - 1], waypoints[i]);
    // Both tests below are equivalences a fault sweep cannot see, kept for the
    // reading: a zero leg cuts into no steps anyway, and a leg ending exactly
    // at `after` has no boundary beyond it.
    if (leg <= 0) continue;
    const std::int64_t end = start + leg;
    if (end > after) {
      const std::int64_t n = steps_in_leg(leg, stride);
      for (std::int64_t k = 1; k <= n; ++k) {
        const std::int64_t boundary = start + leg * k / n;
        if (boundary > after) return boundary;
      }
    }
    start = end;
  }
  return start;
}

Point pick_sidestep(Point step_end, Point mover, std::int32_t mover_radius,
                    std::span<const Neighbour> neighbours, const ObstructionGrid& grid,
                    const Grid& terrain, bool water_unit) noexcept {
  // An equivalence: standing on the step's end, the quarter across is zero,
  // every candidate is the end itself, and `k = 0` wins on cost. Kept because
  // it is `0x00416a29`'s early exit.
  if (step_end == mover) return step_end;

  // A quarter of the step, turned a right angle. Each quarter truncates
  // toward zero (`cdq; and edx, 3; add; sar 2`).
  const std::int32_t dx = step_end.x - mover.x;
  const std::int32_t dy = step_end.y - mover.y;
  const std::int32_t across_x = -dy / kSidestepDivisor;
  const std::int32_t across_y = dx / kSidestepDivisor;

  Point best = step_end;
  std::int32_t best_score = kSidestepUnscored;
  for (std::int32_t k = -kSidestepReach; k <= kSidestepReach; ++k) {
    const Point candidate{step_end.x + k * across_x, step_end.y + k * across_y};
    if (grid.blocked(candidate)) continue;
    const Point midway{(step_end.x + candidate.x) / 2, (step_end.y + candidate.y) / 2};
    if (grid.blocked(midway)) continue;
    const bool deep = terrain_at(terrain, candidate) == kDeepWaterIndex;
    if (deep != water_unit) continue;

    std::int32_t worst = 0;
    for (const Neighbour& n : neighbours) {
      const std::int64_t gap = isqrt(dist_sq(candidate, n.at));
      const std::int64_t overlap = static_cast<std::int64_t>(n.radius) + mover_radius - gap;
      if (overlap > worst) worst = static_cast<std::int32_t>(overlap);
    }
    const std::int32_t score = worst + kSidestepCost * (k < 0 ? -k : k);
    if (score < best_score) {
      best_score = score;
      best = candidate;
    }
  }
  return best;
}

}  // namespace imperivm::core::sim
