// The obstruction grid and the pathfinder. See include/imperivm/core/sim/path.hpp.

#include "imperivm/core/sim/path.hpp"

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

/// A* step costs. 14/10 approximates sqrt(2) to within 1%, and both are
/// integers, which is the whole point.
constexpr std::int32_t kStepStraight = 10;
constexpr std::int32_t kStepDiagonal = 14;

/// The neighbour order. Fixed, and listed once: four orthogonals then four
/// diagonals, each group clockwise from north. Ties in the heap are broken by
/// cell index rather than by this order, so it does not decide the route -- but
/// it does decide which of two equal-cost parents a node records, so it is
/// still part of the definition.
struct Step {
  std::int32_t dx;
  std::int32_t dy;
  std::int32_t cost;
};
constexpr Step kSteps[8] = {
    {0, -1, kStepStraight}, {1, 0, kStepStraight},  {0, 1, kStepStraight},  {-1, 0, kStepStraight},
    {1, -1, kStepDiagonal}, {1, 1, kStepDiagonal},  {-1, 1, kStepDiagonal}, {-1, -1, kStepDiagonal},
};

/// Octile distance in the same units as the step costs: admissible for an
/// 8-connected grid with these two costs, and exact on open ground, so A*
/// expands almost nothing when the way is clear.
[[nodiscard]] constexpr std::int32_t octile(std::int32_t ax, std::int32_t ay, std::int32_t bx,
                                            std::int32_t by) noexcept {
  const std::int32_t dx = ax > bx ? ax - bx : bx - ax;
  const std::int32_t dy = ay > by ? ay - by : by - ay;
  const std::int32_t lo = dx < dy ? dx : dy;
  const std::int32_t hi = dx < dy ? dy : dx;
  return kStepStraight * (hi - lo) + kStepDiagonal * lo;
}

/// How many waypoints ahead the smoothing pass will look for a shortcut.
///
/// Unbounded greedy smoothing is quadratic in the length of the route, and a
/// route can be a thousand cells long. Sixteen cells is 256 world units, which
/// is four unit widths -- long enough to straighten every corner a grid search
/// produces, short enough that smoothing stays linear.
constexpr std::size_t kSmoothWindow = 16;

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
// A*
// --------------------------------------------------------------------------

void PathFinder::reset(const ObstructionGrid& grid) {
  const std::int32_t width = grid.width();
  const std::int32_t height = grid.height();
  if (width != width_ || height != height_) {
    width_ = width;
    height_ = height;
    nodes_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), Node{});
    generation_ = 0;
  }
  // Wrapping the generation would make a stale node look current, so the
  // arrays are cleared on the wrap rather than trusted.
  if (++generation_ == 0) {
    std::fill(nodes_.begin(), nodes_.end(), Node{});
    generation_ = 1;
  }
  heap_.clear();
  cells_.clear();
}

void PathFinder::push(Open entry) {
  heap_.push_back(entry);
  std::size_t i = heap_.size() - 1;
  while (i > 0) {
    const std::size_t parent = (i - 1) / 2;
    const Open& p = heap_[parent];
    const bool higher = entry.f < p.f || (entry.f == p.f && entry.h < p.h) ||
                        (entry.f == p.f && entry.h == p.h && entry.cell < p.cell);
    if (!higher) break;
    heap_[i] = p;
    i = parent;
  }
  heap_[i] = entry;
}

PathFinder::Open PathFinder::pop() {
  const Open top = heap_.front();
  const Open moved = heap_.back();
  heap_.pop_back();
  if (heap_.empty()) return top;
  std::size_t i = 0;
  for (;;) {
    const std::size_t left = 2 * i + 1;
    if (left >= heap_.size()) break;
    std::size_t best = left;
    const std::size_t right = left + 1;
    if (right < heap_.size()) {
      const Open& l = heap_[left];
      const Open& r = heap_[right];
      const bool right_first = r.f < l.f || (r.f == l.f && r.h < l.h) ||
                               (r.f == l.f && r.h == l.h && r.cell < l.cell);
      if (right_first) best = right;
    }
    const Open& c = heap_[best];
    const bool child_first = c.f < moved.f || (c.f == moved.f && c.h < moved.h) ||
                             (c.f == moved.f && c.h == moved.h && c.cell < moved.cell);
    if (!child_first) break;
    heap_[i] = c;
    i = best;
  }
  heap_[i] = moved;
  return top;
}

Path PathFinder::find(const ObstructionGrid& grid, const PathRequest& request) {
  if (request.barrier != nullptr && !request.barrier->empty()) {
    const CellOverlay& barrier = *request.barrier;
    return find_over(grid, request, [&grid, &barrier](std::int32_t cx, std::int32_t cy) {
      return grid.blocked_cell(cx, cy) || barrier.contains(cx, cy);
    });
  }
  return find_over(grid, request,
                   [&grid](std::int32_t cx, std::int32_t cy) { return grid.blocked_cell(cx, cy); });
}

template <typename Blocked>
Path PathFinder::find_over(const ObstructionGrid& grid, const PathRequest& request,
                           const Blocked& blocked) {
  Path out;
  if (grid.empty()) return out;

  const std::int32_t start_cx = ObstructionGrid::cell_of(request.start.x);
  const std::int32_t start_cy = ObstructionGrid::cell_of(request.start.y);
  if (!grid.in_bounds(start_cx, start_cy)) return out;

  out.waypoints.push_back(request.start);

  // Already there. The arrival test is on squared distance in integers: no
  // square root, and no floating-point comparison anywhere.
  if (within(request.start, request.goal, request.arrival_range)) {
    out.status = PathStatus::arrived;
    return out;
  }

  // A clear straight line is the common case for a short move, and running the
  // search for it would be both slower and rougher. A mover that ignores
  // passability takes it whatever lies across it.
  if (request.ignore_passability || line_clear_over(request.start, request.goal, blocked)) {
    out.waypoints.push_back(request.goal);
    out.status = PathStatus::found;
    out.length = polyline_length(out.waypoints);
    return out;
  }

  // A goal inside a footprint is ordinary: scripts send units to a building's
  // position. Retarget to the nearest free cell so the search has something to
  // reach, and keep the original goal only for the arrival test.
  Point goal = request.goal;
  const std::int32_t goal_cx_raw = ObstructionGrid::cell_of(goal.x);
  const std::int32_t goal_cy_raw = ObstructionGrid::cell_of(goal.y);
  if (blocked(goal_cx_raw, goal_cy_raw)) goal = nearest_free_over(goal, 32, blocked);

  const std::int32_t goal_cx = ObstructionGrid::cell_of(goal.x);
  const std::int32_t goal_cy = ObstructionGrid::cell_of(goal.y);

  reset(grid);
  const std::int32_t width = grid.width();
  const auto index_of = [width](std::int32_t cx, std::int32_t cy) {
    return static_cast<std::uint32_t>(cy) * static_cast<std::uint32_t>(width) +
           static_cast<std::uint32_t>(cx);
  };

  // The arrival range in cost units, so the goal test is a cheap comparison on
  // the heuristic rather than a distance per node.
  const std::int32_t arrival_cells =
      request.arrival_range > 0 ? request.arrival_range / kCollisionCellSize : 0;

  const std::uint32_t start_index = index_of(start_cx, start_cy);
  {
    Node& node = nodes_[start_index];
    node = Node{};
    node.generation = generation_;
    node.g = 0;
    node.h = octile(start_cx, start_cy, goal_cx, goal_cy);
    node.f = node.h;
    node.parent = start_index;
  }
  push(Open{nodes_[start_index].f, nodes_[start_index].h, start_index});

  std::uint32_t best_cell = start_index;
  std::int32_t best_h = nodes_[start_index].h;
  std::uint32_t reached = 0;
  bool found = false;

  while (!heap_.empty()) {
    const Open top = pop();
    Node& current = nodes_[top.cell];
    if (current.generation != generation_ || current.closed) continue;
    // A stale heap entry for a cell whose cost has since improved.
    if (top.f != current.f || top.h != current.h) continue;
    current.closed = true;
    ++out.expanded;
    ++total_expanded_;

    const std::int32_t cx = static_cast<std::int32_t>(top.cell % static_cast<std::uint32_t>(width));
    const std::int32_t cy = static_cast<std::int32_t>(top.cell / static_cast<std::uint32_t>(width));

    if (current.h < best_h || (current.h == best_h && top.cell < best_cell)) {
      best_h = current.h;
      best_cell = top.cell;
    }

    if ((cx == goal_cx && cy == goal_cy) ||
        (arrival_cells > 0 && octile(cx, cy, goal_cx, goal_cy) <= arrival_cells * kStepStraight)) {
      reached = top.cell;
      found = true;
      break;
    }

    if (out.expanded >= request.node_budget) break;

    const std::int32_t g = current.g;
    for (const Step& step : kSteps) {
      const std::int32_t nx = cx + step.dx;
      const std::int32_t ny = cy + step.dy;
      if (blocked(nx, ny)) continue;
      // No corner cutting: a diagonal needs both of its orthogonal neighbours
      // free, or a unit slides through the corner of a building.
      if (step.dx != 0 && step.dy != 0) {
        if (blocked(cx + step.dx, cy) || blocked(cx, cy + step.dy)) continue;
      }
      const std::uint32_t neighbour = index_of(nx, ny);
      Node& node = nodes_[neighbour];
      if (node.generation != generation_) {
        node = Node{};
        node.generation = generation_;
        node.g = g + step.cost;
        node.h = octile(nx, ny, goal_cx, goal_cy);
        node.f = node.g + node.h;
        node.parent = top.cell;
        push(Open{node.f, node.h, neighbour});
        continue;
      }
      if (node.closed) continue;
      const std::int32_t tentative = g + step.cost;
      if (tentative >= node.g) continue;
      node.g = tentative;
      node.f = tentative + node.h;
      node.parent = top.cell;
      push(Open{node.f, node.h, neighbour});
    }
  }

  const std::uint32_t terminal = found ? reached : best_cell;
  if (!found && terminal == start_index) {
    out.status = PathStatus::partial;
    out.length = 0;
    return out;
  }

  // Walk the parents back, then reverse. The chain is finite because every
  // parent has a strictly smaller `g` except the start, which is its own.
  cells_.clear();
  for (std::uint32_t cell = terminal;; cell = nodes_[cell].parent) {
    cells_.push_back(cell);
    if (cell == start_index) break;
    if (cells_.size() > nodes_.size()) break;  // defensive: a corrupt chain
  }
  std::reverse(cells_.begin(), cells_.end());

  std::vector<Point> raw;
  raw.reserve(cells_.size() + 1);
  raw.push_back(request.start);
  for (std::size_t i = 1; i < cells_.size(); ++i) {
    const std::uint32_t cell = cells_[i];
    const std::int32_t cx = static_cast<std::int32_t>(cell % static_cast<std::uint32_t>(width));
    const std::int32_t cy = static_cast<std::int32_t>(cell / static_cast<std::uint32_t>(width));
    raw.push_back(Point{ObstructionGrid::centre_of(cx), ObstructionGrid::centre_of(cy)});
  }
  // The real destination, when it is reachable, rather than the centre of its
  // cell: a unit ordered to a point should stand on the point.
  if (found && !raw.empty() && line_clear_over(raw.back(), request.goal, blocked)) {
    raw.back() = request.goal;
  }

  if (request.smooth && raw.size() > 2) {
    std::vector<Point> smoothed;
    smoothed.reserve(raw.size());
    smoothed.push_back(raw.front());
    std::size_t anchor = 0;
    while (anchor + 1 < raw.size()) {
      std::size_t furthest = anchor + 1;
      const std::size_t limit = std::min(raw.size() - 1, anchor + kSmoothWindow);
      for (std::size_t candidate = limit; candidate > anchor + 1; --candidate) {
        if (line_clear_over(raw[anchor], raw[candidate], blocked)) {
          furthest = candidate;
          break;
        }
      }
      smoothed.push_back(raw[furthest]);
      anchor = furthest;
    }
    raw.swap(smoothed);
  }

  out.waypoints = std::move(raw);
  out.status = found ? PathStatus::found : PathStatus::partial;
  out.length = polyline_length(out.waypoints);
  return out;
}

}  // namespace imperivm::core::sim
