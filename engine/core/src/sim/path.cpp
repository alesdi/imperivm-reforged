// The obstruction grid and the pathfinder. See include/imperivm/core/sim/path.hpp.

#include "imperivm/core/sim/path.hpp"

#include "imperivm/core/sim/grid_search.hpp"

#include <algorithm>
#include <utility>

namespace imperivm::core::sim {
namespace {

/// sin(d) in Q15 for d in 0..90, rounded to nearest. The other three quadrants
/// come from the symmetries, so this table is the whole trigonometry of the
/// engine.
constexpr std::int32_t kSinTable[91] = {
    0,     572,   1144,  1715,  2286,  2856,  3425,  3993,  4560,  5126,  5690,  6252,  6813,
    7371,  7927,  8481,  9032,  9580,  10126, 10668, 11207, 11743, 12275, 12803, 13328, 13848,
    14365, 14876, 15384, 15886, 16384, 16877, 17364, 17847, 18324, 18795, 19261, 19720, 20174,
    20622, 21063, 21498, 21926, 22348, 22763, 23170, 23571, 23965, 24351, 24730, 25102, 25466,
    25822, 26170, 26510, 26842, 27166, 27482, 27789, 28088, 28378, 28660, 28932, 29197, 29452,
    29698, 29935, 30163, 30382, 30592, 30792, 30983, 31164, 31336, 31499, 31651, 31795, 31928,
    32052, 32166, 32270, 32365, 32449, 32524, 32588, 32643, 32688, 32723, 32748, 32763, 32768};

constexpr std::int32_t kQ15One = 32768;

/// Divide rounding to nearest, half away from zero. Symmetric in the sign, so
/// rotating a vector and rotating its negation give exactly opposite results.
[[nodiscard]] constexpr std::int64_t div_round(std::int64_t numerator,
                                               std::int64_t denominator) noexcept {
  if (denominator == 0) return 0;
  const std::int64_t half = denominator / 2;
  return numerator >= 0 ? (numerator + half) / denominator : (numerator - half) / denominator;
}

/// `ObstructionGrid::line_is_clear` over any notion of blocked.
template <typename Blocked>
[[nodiscard]] bool line_clear_over(Point a, Point b, const Blocked& blocked) noexcept {
  const std::int32_t span = distance(a, b);
  // A quarter of a cell cannot step over one, whatever the slope.
  const std::int32_t step = kCollisionCellSize / 4;
  const std::int32_t samples = span / step + 1;
  for (std::int32_t i = 0; i <= samples; ++i) {
    const Point p{static_cast<std::int32_t>(
                      a.x + (static_cast<std::int64_t>(b.x - a.x) * i) / samples),
                  static_cast<std::int32_t>(
                      a.y + (static_cast<std::int64_t>(b.y - a.y) * i) / samples)};
    if (blocked(ObstructionGrid::cell_of(p.x), ObstructionGrid::cell_of(p.y))) return false;
  }
  return true;
}

/// `ObstructionGrid::nearest_free` over any notion of blocked.
template <typename Blocked>
[[nodiscard]] Point nearest_free_over(Point world, std::int32_t max_rings,
                                      const Blocked& blocked) noexcept {
  const std::int32_t cx = ObstructionGrid::cell_of(world.x);
  const std::int32_t cy = ObstructionGrid::cell_of(world.y);
  if (!blocked(cx, cy)) return world;
  const auto centre = [](std::int32_t x, std::int32_t y) {
    return Point{ObstructionGrid::centre_of(x), ObstructionGrid::centre_of(y)};
  };
  for (std::int32_t r = 1; r <= max_rings; ++r) {
    // A fixed ring order: top row left to right, bottom row, then the two
    // sides. Deterministic, and biased towards nothing in particular.
    for (std::int32_t x = cx - r; x <= cx + r; ++x) {
      if (!blocked(x, cy - r)) return centre(x, cy - r);
    }
    for (std::int32_t x = cx - r; x <= cx + r; ++x) {
      if (!blocked(x, cy + r)) return centre(x, cy + r);
    }
    for (std::int32_t y = cy - r + 1; y <= cy + r - 1; ++y) {
      if (!blocked(cx - r, y)) return centre(cx - r, y);
    }
    for (std::int32_t y = cy - r + 1; y <= cy + r - 1; ++y) {
      if (!blocked(cx + r, y)) return centre(cx + r, y);
    }
  }
  return world;
}

}  // namespace

// --------------------------------------------------------------------------
// integer geometry
// --------------------------------------------------------------------------

// `isqrt` is defined in sim/world_host.cpp: one square root for the whole
// simulation, declared in both headers so neither has to include the other.

std::int32_t distance(Point a, Point b) noexcept {
  return static_cast<std::int32_t>(isqrt(dist_sq(a, b)));
}

std::int32_t sin_q15(std::int32_t degrees) noexcept {
  std::int32_t d = degrees % 360;
  if (d < 0) d += 360;
  if (d <= 90) return kSinTable[d];
  if (d <= 180) return kSinTable[180 - d];
  if (d <= 270) return -kSinTable[d - 180];
  return -kSinTable[360 - d];
}

std::int32_t cos_q15(std::int32_t degrees) noexcept { return sin_q15(degrees + 90); }

Point rotate_degrees(Point v, std::int32_t degrees) noexcept {
  const std::int64_t c = cos_q15(degrees);
  const std::int64_t s = sin_q15(degrees);
  const std::int64_t x = v.x;
  const std::int64_t y = v.y;
  return Point{static_cast<std::int32_t>(div_round(x * c - y * s, kQ15One)),
               static_cast<std::int32_t>(div_round(x * s + y * c, kQ15One))};
}

Point set_length(Point v, std::int32_t length) noexcept {
  const std::int32_t current = vector_length(v);
  if (current == 0) return Point{0, 0};
  return Point{static_cast<std::int32_t>(
                   div_round(static_cast<std::int64_t>(v.x) * length, current)),
               static_cast<std::int32_t>(
                   div_round(static_cast<std::int64_t>(v.y) * length, current))};
}

// --------------------------------------------------------------------------
// the obstruction grid
// --------------------------------------------------------------------------

ObstructionGrid::ObstructionGrid(std::int32_t width, std::int32_t height)
    : width_(width > 0 ? width : 0), height_(height > 0 ? height : 0) {
  const std::size_t bits =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  words_.assign((bits + 63) / 64, 0);
}

Result<ObstructionGrid> ObstructionGrid::from_grid(const Grid& grid) {
  if (grid.cell_size() != static_cast<std::uint32_t>(kCollisionCellSize) ||
      grid.bits_per_cell() != 1) {
    // A 64-unit terrain grid reinterpreted as obstruction would look plausible
    // and be wrong everywhere, so it is refused rather than accepted.
    return FormatError::malformed;
  }
  ObstructionGrid out(static_cast<std::int32_t>(grid.width()),
                      static_cast<std::int32_t>(grid.height()));
  for (std::int32_t y = 0; y < out.height_; ++y) {
    for (std::int32_t x = 0; x < out.width_; ++x) {
      if (grid.blocked(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y))) {
        out.set_cell(x, y, true);
      }
    }
  }
  return out;
}

bool ObstructionGrid::blocked_cell(std::int32_t cx, std::int32_t cy) const noexcept {
  if (!in_bounds(cx, cy)) return true;
  const std::size_t bit = bit_index(cx, cy);
  return ((words_[bit >> 6] >> (bit & 63)) & 1u) != 0;
}

void ObstructionGrid::set_cell(std::int32_t cx, std::int32_t cy, bool blocked) noexcept {
  if (!in_bounds(cx, cy)) return;
  const std::size_t bit = bit_index(cx, cy);
  const std::uint64_t mask = std::uint64_t{1} << (bit & 63);
  if (blocked) {
    words_[bit >> 6] |= mask;
  } else {
    words_[bit >> 6] &= ~mask;
  }
}

namespace {

/// Walk a `.pass` stamp, calling `apply(cx, cy)` for every blocked cell of it,
/// translated to map cells. Shared by `stamp` and `unstamp` so the anchor
/// arithmetic exists once.
template <typename Apply>
void walk_stamp(const Grid& mask, Point anchor, std::int32_t vertical_correction,
                Apply&& apply) {
  if (mask.bits_per_cell() != 1) return;
  if (mask.cell_size() != static_cast<std::uint32_t>(kCollisionCellSize)) return;
  const std::int32_t half_x = static_cast<std::int32_t>(mask.width()) / 2;
  const std::int32_t half_y = static_cast<std::int32_t>(mask.height()) / 2;
  const std::int32_t anchor_cx = ObstructionGrid::cell_of(anchor.x);
  const std::int32_t anchor_cy = ObstructionGrid::cell_of(anchor.y);
  for (std::uint32_t y = 0; y < mask.height(); ++y) {
    for (std::uint32_t x = 0; x < mask.width(); ++x) {
      if (!mask.blocked(x, y)) continue;
      apply(anchor_cx + static_cast<std::int32_t>(x) - half_x,
            anchor_cy + static_cast<std::int32_t>(y) - half_y + vertical_correction);
    }
  }
}

}  // namespace

void ObstructionGrid::stamp(const Grid& mask, Point anchor,
                            std::int32_t vertical_correction) noexcept {
  walk_stamp(mask, anchor, vertical_correction,
             [this](std::int32_t cx, std::int32_t cy) { set_cell(cx, cy, true); });
}

void ObstructionGrid::unstamp(const Grid& mask, Point anchor,
                              std::int32_t vertical_correction) noexcept {
  walk_stamp(mask, anchor, vertical_correction,
             [this](std::int32_t cx, std::int32_t cy) { set_cell(cx, cy, false); });
}

bool ObstructionGrid::passable_3x3(Point world) const noexcept {
  const std::int32_t cx = cell_of(world.x);
  const std::int32_t cy = cell_of(world.y);
  for (std::int32_t dy = -1; dy <= 1; ++dy) {
    for (std::int32_t dx = -1; dx <= 1; ++dx) {
      if (blocked_cell(cx + dx, cy + dy)) return false;
    }
  }
  return true;
}

bool ObstructionGrid::line_is_clear(Point a, Point b) const noexcept {
  return line_clear_over(
      a, b, [this](std::int32_t cx, std::int32_t cy) { return blocked_cell(cx, cy); });
}

Point ObstructionGrid::clamp_to_map(Point world) const noexcept {
  if (empty()) return world;
  const std::int32_t max_x = extent_x() - 1;
  const std::int32_t max_y = extent_y() - 1;
  return Point{std::clamp(world.x, 0, max_x), std::clamp(world.y, 0, max_y)};
}

Point ObstructionGrid::nearest_free(Point world, std::int32_t max_rings) const noexcept {
  return nearest_free_over(
      world, max_rings, [this](std::int32_t cx, std::int32_t cy) { return blocked_cell(cx, cy); });
}


std::size_t ObstructionGrid::count_blocked() const noexcept {
  std::size_t total = 0;
  const std::size_t bits =
      static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
  for (std::size_t bit = 0; bit < bits; ++bit) {
    if ((words_[bit >> 6] >> (bit & 63)) & 1u) ++total;
  }
  return total;
}

// --------------------------------------------------------------------------
// a search's own blocked cells
// --------------------------------------------------------------------------

void CellOverlay::clear() noexcept {
  keys_.clear();
  min_x_ = 0;
  min_y_ = 0;
  max_x_ = -1;
  max_y_ = -1;
}

void CellOverlay::add(std::int32_t cx, std::int32_t cy) {
  if (keys_.empty()) {
    min_x_ = max_x_ = cx;
    min_y_ = max_y_ = cy;
  } else {
    min_x_ = std::min(min_x_, cx);
    max_x_ = std::max(max_x_, cx);
    min_y_ = std::min(min_y_, cy);
    max_y_ = std::max(max_y_, cy);
  }
  keys_.push_back(key(cx, cy));
}

void CellOverlay::seal() {
  std::sort(keys_.begin(), keys_.end());
  keys_.erase(std::unique(keys_.begin(), keys_.end()), keys_.end());
}

bool CellOverlay::contains(std::int32_t cx, std::int32_t cy) const noexcept {
  if (cx < min_x_ || cx > max_x_ || cy < min_y_ || cy > max_y_) return false;
  return std::binary_search(keys_.begin(), keys_.end(), key(cx, cy));
}

// --------------------------------------------------------------------------
// polylines
// --------------------------------------------------------------------------

std::int64_t polyline_length(std::span<const Point> waypoints) noexcept {
  std::int64_t total = 0;
  for (std::size_t i = 1; i < waypoints.size(); ++i) {
    total += distance(waypoints[i - 1], waypoints[i]);
  }
  return total;
}

Point point_along(std::span<const Point> waypoints, std::int64_t distance_along,
                  std::size_t* segment) noexcept {
  if (waypoints.empty()) {
    if (segment != nullptr) *segment = 0;
    return Point{0, 0};
  }
  if (distance_along <= 0 || waypoints.size() == 1) {
    if (segment != nullptr) *segment = 0;
    return waypoints[0];
  }
  std::int64_t remaining = distance_along;
  for (std::size_t i = 1; i < waypoints.size(); ++i) {
    const std::int64_t leg = distance(waypoints[i - 1], waypoints[i]);
    if (leg <= 0) continue;
    if (remaining >= leg) {
      remaining -= leg;
      continue;
    }
    if (segment != nullptr) *segment = i - 1;
    const Point a = waypoints[i - 1];
    const Point b = waypoints[i];
    return Point{
        static_cast<std::int32_t>(a.x + div_round(static_cast<std::int64_t>(b.x - a.x) * remaining,
                                                  leg)),
        static_cast<std::int32_t>(a.y + div_round(static_cast<std::int64_t>(b.y - a.y) * remaining,
                                                  leg))};
  }
  if (segment != nullptr) *segment = waypoints.size() - 1;
  return waypoints.back();
}

// --------------------------------------------------------------------------
// the route search
// --------------------------------------------------------------------------

PathFinder::PathFinder()
    : grid_(std::make_unique<GridSearch>()), goals_(std::make_unique<GoalCells>()) {}
PathFinder::~PathFinder() = default;
PathFinder::PathFinder(PathFinder&&) noexcept = default;
PathFinder& PathFinder::operator=(PathFinder&&) noexcept = default;

std::uint64_t PathFinder::total_expanded() const noexcept { return grid_->total_expanded(); }

void request_goals(const ObstructionGrid& grid, const PathRequest& request, GoalCells& out) {
  if (request.goal_points_given) {
    out.assign(request.goal_points);
    return;
  }
  // A range of 0 lays the goal alone (0x00409cf0), and only where a unit can
  // stand: on the map and on a free cell (0x00409b80).
  const std::int32_t cx = ObstructionGrid::cell_of(request.goal.x);
  const std::int32_t cy = ObstructionGrid::cell_of(request.goal.y);
  if (grid.in_bounds(cx, cy) && !grid.blocked_cell(cx, cy)) {
    const GoalPoint alone{request.goal, true};
    out.assign(std::span<const GoalPoint>(&alone, 1));
  } else {
    out.assign({});
  }
}

Path PathFinder::find(const ObstructionGrid& grid, const PathRequest& request) {
  request_goals(grid, request, *goals_);
  CellView view;
  view.grid = &grid;
  view.barrier = request.barrier != nullptr && !request.barrier->empty() ? request.barrier : nullptr;
  Path out = grid_->run(view, request, *goals_, /*both_ways=*/false);
  // A route that goes nowhere is no route: the original answers whether the
  // length is non-zero, and a caller that sees one point lays nothing.
  if (out.waypoints.size() >= 2 && out.length == 0) {
    out.waypoints.resize(1);
    if (out.status == PathStatus::found) out.status = PathStatus::arrived;
  }
  return out;
}

}  // namespace imperivm::core::sim
