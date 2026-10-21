// The original's grid search. See include/imperivm/core/sim/grid_search.hpp
// and docs/formats/pathfinder.md, "The grid search".

#include "imperivm/core/sim/grid_search.hpp"

#include <algorithm>

namespace imperivm::core::sim {
namespace {

/// One step's cost, 2.0 in 16.16 (0x0041b140 adds 0x20000 to the parent's).
constexpr std::uint32_t kStepCost = 0x20000;
/// A node not reached in this search: cost so far "infinite", estimate 0 --
/// what 0x0041ac80 resets every node to before a search.
constexpr std::uint32_t kUnreached = 0xFFFFFFFFu;
/// The estimate a node is given once it has been expanded (`[node+8]`), which
/// is also how the original tells a closed node from an open one.
constexpr std::uint32_t kClosed = 0xFFFFFFFFu;
constexpr std::uint32_t kNoNode = 0xFFFFFFFFu;

/// The cell a world coordinate falls in, as the original takes it: an
/// arithmetic shift, so a negative coordinate floors.
[[nodiscard]] constexpr std::int32_t cell_of(std::int32_t world) noexcept { return world >> 4; }

[[nodiscard]] constexpr Point centre_of(Cell c) noexcept {
  return Point{c.x * kCollisionCellSize + kCollisionCellSize / 2,
               c.y * kCollisionCellSize + kCollisionCellSize / 2};
}

[[nodiscard]] constexpr std::uint64_t cell_key(std::int32_t x, std::int32_t y) noexcept {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 32) |
         static_cast<std::uint32_t>(x);
}

/// The estimate from a cell `(dx, dy)` cells off the heading's cell: twice the
/// straight distance in 16.16, `trunc(sqrt(dx^2 + dy^2) * 131072)` -- the
/// float at 0x007ac654 is 2^17 -- which the original takes with the FPU's
/// square root and `_ftol`. Multiplying by a power of two is exact, so this
/// is the floored root of the square scaled by 2^34, which equals the FPU's
/// answer wherever a double rounds the root to the same side of a multiple of
/// 2^-17: every distance under 1,024 cells. Past that a square within a few
/// units of `(k+1)^2 / 2^34` could round up where this floors; no shipped map
/// is that large. (53-bit precision control assumed, as `point::Rot`.)
[[nodiscard]] std::uint32_t estimate(std::int32_t dx, std::int32_t dy) noexcept {
  const std::int64_t square = static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dy) * dy;
  return static_cast<std::uint32_t>(isqrt(square << 34));
}

/// `isqrt` of a squared cell distance as the original hands it to 0x0067c3d0:
/// an unsigned 32-bit square.
[[nodiscard]] std::uint32_t root_of(std::int32_t dx, std::int32_t dy) noexcept {
  const std::uint32_t square = static_cast<std::uint32_t>(dx) * static_cast<std::uint32_t>(dx) +
                               static_cast<std::uint32_t>(dy) * static_cast<std::uint32_t>(dy);
  return static_cast<std::uint32_t>(isqrt(square));
}

}  // namespace

// --------------------------------------------------------------------------
// cell lines
// --------------------------------------------------------------------------

bool line_clear(const CellView& view, Cell a, Cell b, LineState& state) {
  // 0x0040b1b0 on each cell: a blocked cell stops the line there; so does a
  // diagonal step from the previous cell when **both** corners it passes are
  // blocked -- one free corner lets it through.
  walk_cells(a.x, a.y, b.x, b.y, [&](std::int32_t x, std::int32_t y) {
    bool stop = view.blocked(x, y);
    if (!stop) {
      const std::int32_t sx = x - state.prev.x;
      const std::int32_t sy = y - state.prev.y;
      if ((sx == 1 || sx == -1) && (sy == 1 || sy == -1) && view.blocked(x, state.prev.y) &&
          view.blocked(state.prev.x, y)) {
        stop = true;
      }
    }
    if (stop) {
      state.clear = false;
      state.hit = Cell{x, y};
      return false;
    }
    state.prev = Cell{x, y};
    return true;
  });
  return state.clear;
}

// --------------------------------------------------------------------------
// the goal window
// --------------------------------------------------------------------------

void GoalCells::assign(std::span<const GoalPoint> points) {
  cells_.clear();
  live_.clear();
  min_ = Cell{0, 0};
  max_ = Cell{-1, -1};
  for (const GoalPoint& p : points) {
    const std::int32_t cx = cell_of(p.at.x);
    const std::int32_t cy = cell_of(p.at.y);
    const std::uint64_t k = cell_key(cx, cy);
    auto it = std::lower_bound(cells_.begin(), cells_.end(), k,
                               [](const Entry& e, std::uint64_t v) { return e.key < v; });
    if (it != cells_.end() && it->key == k) {
      // 0x00409b80 keeps four points to a cell; a fifth is not registered,
      // so it is no goal point at all -- not even for the straight line.
      if (it->count >= 4) continue;
      ++it->count;
    } else {
      it = cells_.insert(it, Entry{k, p.at, p.live, 1});
      if (cells_.size() == 1) {
        min_ = max_ = Cell{cx, cy};
      } else {
        min_.x = std::min(min_.x, cx);
        min_.y = std::min(min_.y, cy);
        max_.x = std::max(max_.x, cx);
        max_.y = std::max(max_.y, cy);
      }
    }
    if (p.live) live_.push_back(p.at);
  }
}

bool GoalCells::at(Cell cell, Point& out) const noexcept {
  if (cell.x < min_.x || cell.x > max_.x || cell.y < min_.y || cell.y > max_.y) return false;
  const std::uint64_t k = cell_key(cell.x, cell.y);
  const auto it = std::lower_bound(cells_.begin(), cells_.end(), k,
                                   [](const Entry& e, std::uint64_t v) { return e.key < v; });
  // 0x0041b140 reads the cell's first slot: a cell whose first point was
  // struck off is no goal cell, whatever its other slots hold.
  if (it == cells_.end() || it->key != k || !it->live) return false;
  out = it->first;
  return true;
}

// --------------------------------------------------------------------------
// the heap
// --------------------------------------------------------------------------
//
// The library heap of the original's compiler (0x0041b0b0 pushes, 0x0041adf0
// and 0x0041ada0 pop), as a min-heap on a node's cost so far plus its
// estimate. The heap holds nodes, not keys: a node whose cost drops while it
// waits is **not** moved, and every comparison reads the key as it stands.
// The pop order -- and so which of two equal routes is laid, and where the
// budget runs out -- depends on that, so it is kept.

void GridSearch::sift_up(std::size_t hole, std::size_t top, std::uint32_t value) {
  // 0x0041ada0: up while the parent's key is strictly greater.
  const std::uint32_t k = key(value);
  while (top < hole) {
    const std::size_t parent = (hole - 1) / 2;
    if (!(k < key(heap_[parent]))) break;
    heap_[hole] = heap_[parent];
    hole = parent;
  }
  heap_[hole] = value;
}

void GridSearch::push(std::uint32_t n) {
  heap_.push_back(n);
  sift_up(heap_.size() - 1, 0, n);
}

std::uint32_t GridSearch::pop() {
  const std::uint32_t top = heap_.front();
  if (heap_.size() > 1) {
    // The last entry fills the hole the top leaves: down through the smaller
    // child (the right one when the two keys are equal), then up again.
    const std::uint32_t moved = heap_.back();
    heap_.back() = top;
    const std::size_t len = heap_.size() - 1;
    std::size_t hole = 0;
    std::size_t child = 2;
    while (child < len) {
      if (key(heap_[child]) > key(heap_[child - 1])) --child;
      heap_[hole] = heap_[child];
      hole = child;
      child = 2 * hole + 2;
    }
    if (child == len) {
      heap_[hole] = heap_[len - 1];
      hole = len - 1;
    }
    sift_up(hole, 0, moved);
  }
  heap_.pop_back();
  return top;
}

// --------------------------------------------------------------------------
// the search
// --------------------------------------------------------------------------

void GridSearch::reset(std::int32_t width, std::int32_t height) {
  if (width != width_ || height != height_) {
    width_ = width;
    height_ = height;
    const std::size_t cells = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    g_.assign(cells, kUnreached);
    h_.assign(cells, 0);
    parent_.assign(cells, kNoNode);
    gen_.assign(cells, 0);
    generation_ = 0;
  }
  // A wrapped generation would make a stale node look current.
  if (++generation_ == 0) {
    std::fill(gen_.begin(), gen_.end(), 0u);
    generation_ = 1;
  }
  heap_.clear();
}

void GridSearch::touch(std::uint32_t n) noexcept {
  if (gen_[n] == generation_) return;
  gen_[n] = generation_;
  g_[n] = kUnreached;
  h_[n] = 0;
  parent_[n] = kNoNode;
}

bool GridSearch::straight_line(const CellView& view, const PathRequest& request,
                               const GoalCells& goals, Path& out) const {
  // The goal is the live goal point nearest the start, the first on a tie, by
  // a squared distance in 32 bits; with none left, where the search heads.
  Point target = request.goal;
  std::uint32_t best = 0xFFFFFFFFu;
  for (const Point& p : goals.live()) {
    const std::uint32_t dx = static_cast<std::uint32_t>(request.start.x - p.x);
    const std::uint32_t dy = static_cast<std::uint32_t>(request.start.y - p.y);
    const std::uint32_t d = dx * dx + dy * dy;
    if (d < best) {
      best = d;
      target = p;
    }
  }
  const bool none_left = best == 0xFFFFFFFFu;
  out.waypoints.clear();
  if (!request.ignore_passability) {
    LineState state;
    if (!line_clear(view, Cell{cell_of(request.start.x), cell_of(request.start.y)},
                    Cell{cell_of(target.x), cell_of(target.y)}, state)) {
      return false;
    }
    if (request.owner != nullptr && none_left) {
      // Every goal point taken and the way open: walk from the heading back
      // towards the start in steps of the owner's radius for the first free
      // spot (0x0040a990), and with none a route that goes nowhere.
      const std::int32_t dx = request.start.x - target.x;
      const std::int32_t dy = request.start.y - target.y;
      const std::int32_t radius = request.owner->radius();
      const std::int32_t steps =
          radius > 0 ? static_cast<std::int32_t>(root_of(dx, dy)) / radius : 0;
      Point probe = target;
      for (std::int32_t i = 0; i < steps; ++i) {
        probe.y += dy / steps;
        probe.x += dx / steps;
        if (request.owner->free(probe)) {
          out.waypoints = {request.start, probe};
          out.status = PathStatus::found;
          out.length = polyline_length(out.waypoints);
          return true;
        }
      }
      out.waypoints = {request.start, request.start};
      out.status = PathStatus::no_goal;
      out.length = 0;
      out.cut = true;
      return true;
    }
  }
  out.waypoints = {request.start, target};
  out.status = PathStatus::found;
  out.length = polyline_length(out.waypoints);
  return true;
}

std::uint32_t GridSearch::astar(const CellView& view, Cell start, Point heading,
                                const GoalCells& goals, bool& found, Point& goal_point,
                                std::uint32_t& popped) {
  const std::int32_t gx = cell_of(heading.x);
  const std::int32_t gy = cell_of(heading.y);
  const auto index_of = [this](std::int32_t x, std::int32_t y) {
    return static_cast<std::uint32_t>(y) * static_cast<std::uint32_t>(width_) +
           static_cast<std::uint32_t>(x);
  };
  const std::uint32_t first = index_of(start.x, start.y);
  touch(first);
  g_[first] = 0;
  h_[first] = estimate(start.x - gx, start.y - gy);
  parent_[first] = kNoNode;
  push(first);
  std::uint32_t best = first;
  std::uint32_t best_h = h_[first];
  std::uint32_t budget = kBudget;
  found = false;
  popped = 0;
  // The four neighbours in the order the original tries them: below, left,
  // above, right.
  constexpr std::int32_t kDx[4] = {0, -1, 0, 1};
  constexpr std::int32_t kDy[4] = {1, 0, -1, 0};
  for (;;) {
    if (heap_.empty()) return best;
    const std::uint32_t current = pop();
    ++popped;
    // The budget is spent on the pop: the 8,001st ends the search before its
    // cell is looked at.
    if (budget-- == 0) return best;
    const std::int32_t cx = static_cast<std::int32_t>(current % static_cast<std::uint32_t>(width_));
    const std::int32_t cy = static_cast<std::int32_t>(current / static_cast<std::uint32_t>(width_));
    if (goals.at(Cell{cx, cy}, goal_point)) {
      found = true;
      return current;
    }
    // (0x0041b140 would stop here when no goal point was ever registered;
    // the goal points' strike-off, 0x0040a310, sets that count to one before
    // any search, so it never does.)
    const std::uint32_t step = g_[current] + kStepCost;
    for (int i = 0; i < 4; ++i) {
      const std::int32_t nx = cx + kDx[i];
      const std::int32_t ny = cy + kDy[i];
      if (view.blocked(nx, ny)) continue;
      const std::uint32_t n = index_of(nx, ny);
      touch(n);
      // A cheaper parent wins; of two equal ones, the later wins on a cell
      // whose coordinates sum to an odd number and the earlier on an even one.
      if (step > g_[n]) continue;
      if (step == g_[n] && ((nx ^ ny) & 1) == 0) continue;
      // Waiting in the heap is an estimate neither 0 nor the closed mark: a
      // node at the heading's own cell (estimate 0) or an expanded one is put
      // in again.
      const bool waiting = h_[n] != 0 && h_[n] != kClosed;
      g_[n] = step;
      parent_[n] = current;
      if (!waiting) {
        h_[n] = estimate(nx - gx, ny - gy);
        push(n);
      }
    }
    if (h_[current] < best_h) {
      best = current;
      best_h = h_[current];
    }
    h_[current] = kClosed;
  }
}

Path GridSearch::run(CellView view, const PathRequest& request, const GoalCells& goals,
                     bool both_ways) {
  Path out;
  const ObstructionGrid& grid = *view.grid;
  if (grid.empty()) return out;
  const Cell start{cell_of(request.start.x), cell_of(request.start.y)};
  if (!grid.in_bounds(start.x, start.y)) return out;

  view.open = false;
  if (straight_line(view, request, goals, out)) return out;

  // The start's cell is passable for the search and for the straightening.
  view.open = true;
  view.open_x = start.x;
  view.open_y = start.y;
  reset(grid.width(), grid.height());

  bool found = false;
  Point goal_point{};
  std::uint32_t popped = 0;
  std::uint32_t node = astar(view, start, request.goal, goals, found, goal_point, popped);
  out.expanded = popped;
  total_expanded_ += popped;

  const auto cell_at = [this](std::uint32_t n) {
    return Cell{static_cast<std::int32_t>(n % static_cast<std::uint32_t>(width_)),
                static_cast<std::int32_t>(n / static_cast<std::uint32_t>(width_))};
  };

  // Where the route ends (0x0041ba00). Arrived: the goal point of the cell.
  // Not arrived: the centre of the cell it gave up at -- and for an owner,
  // the centre of the first cell back along the way that is a free spot, or
  // the start itself when none is.
  Point end = found ? goal_point : centre_of(cell_at(node));
  if (!found && request.owner != nullptr) {
    for (;;) {
      const Point centre = centre_of(cell_at(node));
      if (request.owner->free(centre)) {
        end = centre;
        break;
      }
      out.cut = true;
      if (parent_[node] == kNoNode) {
        end = request.start;
        break;
      }
      node = parent_[node];
    }
  }

  // The cell chain, end first: each cell's parent, except that a corner
  // between two cells a diagonal step apart is cut.
  chain_.clear();
  for (std::uint32_t n = node; n != kNoNode;) {
    const Cell here = cell_at(n);
    chain_.push_back(here);
    std::uint32_t next = parent_[n];
    if (next != kNoNode && parent_[next] != kNoNode) {
      const Cell after = cell_at(parent_[next]);
      const std::int32_t sx = here.x - after.x;
      const std::int32_t sy = here.y - after.y;
      if ((sx == 1 || sx == -1) && (sy == 1 || sy == -1)) next = parent_[next];
    }
    n = next;
  }
  std::reverse(chain_.begin(), chain_.end());

  straighten(view, chain_, both_ways, out.waypoints);
  out.waypoints.push_back(end);
  out.waypoints.front() = request.start;
  out.status = found ? PathStatus::found : PathStatus::partial;
  if (goals.live().empty()) out.status = PathStatus::no_goal;
  out.length = polyline_length(out.waypoints);
  return out;
}

// --------------------------------------------------------------------------
// straightening
// --------------------------------------------------------------------------

void straighten(const CellView& view, std::span<const Cell> cells, bool both_ways,
                std::vector<Point>& out) {
  // 0x0040c6b0. From an anchor -- the chain's first cell to begin with -- it
  // looks back from the chain's end for the farthest cell it can see by a
  // clear cell line, stepping over a seventh of the distance at a time once
  // the distance passes a limit (50 cells, then 300; past 500 it gives up on
  // the anchor, takes back the last point it laid and anchors on the chain
  // cell instead). Seeing the end, it lays the anchor and stops. Otherwise it
  // lays the anchor and moves it: with the two-way test (`both_ways`, the
  // smart pathfinder's connections) to the cell it saw; with the one-way test
  // to where the line from the first cell past it that it cannot see, run
  // back towards the anchor, first comes within two cells of where that line
  // was stopped -- or, when it never does, to the cell it saw.
  const std::int32_t n = static_cast<std::int32_t>(cells.size());
  if (n == 0) return;
  if (cells.front() == cells.back()) {
    out.push_back(centre_of(cells.front()));
    return;
  }
  // Each pass either moves the anchor on or gives a limit up; the original
  // has no bound, this one stops a chain it cannot finish rather than spin.
  std::size_t guard = static_cast<std::size_t>(n) * 64 + 1024;
  std::int32_t anchor = 0;
  Cell a = cells[0];
  std::uint32_t limit = 50;
  while (guard-- > 0) {
    std::int32_t j = n - 1;
    while (j >= anchor) {
      const Cell c = cells[static_cast<std::size_t>(j)];
      if (j == n - 1 || c != a) {
        LineState state;
        bool clear = true;
        if (both_ways) clear = line_clear(view, a, c, state);
        if (clear && line_clear(view, c, a, state)) break;
      }
      const std::uint32_t d = root_of(a.x - c.x, a.y - c.y);
      const std::uint32_t skip = d < limit ? 0 : d / 7;
      j = j - 1 - static_cast<std::int32_t>(skip);
    }
    if (j == n - 1) {
      out.push_back(centre_of(a));
      return;
    }
    if (j < anchor) {
      limit += 250;
      if (limit > 500) {
        if (!out.empty()) out.pop_back();
        a = cells[static_cast<std::size_t>(anchor)];
        out.push_back(centre_of(a));
        limit = 50;
      }
      continue;
    }
    if (both_ways) {
      out.push_back(centre_of(a));
      a = cells[static_cast<std::size_t>(j)];
      continue;
    }
    // The first chain cell past the one it saw that it cannot see -- one line
    // state carried from test to test, as the original carries it.
    LineState state;
    std::int32_t k = j;
    do {
      ++k;
      if (k >= n) break;
      line_clear(view, cells[static_cast<std::size_t>(k)], a, state);
    } while (state.clear);
    Cell next = cells[static_cast<std::size_t>(j)];
    if (k < n) {
      const Cell from = cells[static_cast<std::size_t>(k - 1)];
      const Cell hit = state.hit;
      walk_cells(from.x, from.y, a.x, a.y, [&](std::int32_t x, std::int32_t y) {
        if (x < hit.x - 2 || x > hit.x + 2 || y < hit.y - 2 || y > hit.y + 2) return true;
        next = Cell{x, y};
        return false;
      });
    }
    out.push_back(centre_of(a));
    anchor = j;
    a = next;
    limit = 50;
  }
  out.push_back(centre_of(a));
}

}  // namespace imperivm::core::sim
