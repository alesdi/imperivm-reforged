// The original's smart pathfinder. See include/imperivm/core/sim/smart_path.hpp
// and docs/formats/pathfinder.md, "The smart pathfinder: the build" and
// "The smart pathfinder: a search".

#include "imperivm/core/sim/smart_path.hpp"

#include "imperivm/core/sim/gate.hpp"

#include <algorithm>
#include <deque>

namespace imperivm::core::sim {
namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
/// The estimate an expanded record is given (`[node+4] = -1`), which is also
/// how the area search tells a closed record from an open one.
constexpr std::uint32_t kClosed = 0xFFFFFFFFu;

[[nodiscard]] constexpr Point centre_of(Cell c) noexcept {
  return Point{c.x * kCollisionCellSize + kCollisionCellSize / 2,
               c.y * kCollisionCellSize + kCollisionCellSize / 2};
}

/// A world coordinate to its cell by an arithmetic shift, as most of the
/// original's sites take it.
[[nodiscard]] constexpr std::int32_t shift_cell(std::int32_t world) noexcept { return world >> 4; }

/// A world coordinate to its cell by a division that truncates towards zero,
/// as the route's points are taken to cells (0x00413ff6 .. 0x0041409e) and
/// where the search heads is (0x00413d1b).
[[nodiscard]] constexpr std::int32_t div_cell(std::int32_t world) noexcept {
  return world / kCollisionCellSize;
}

/// The integer square root of a squared distance taken in 32 unsigned bits,
/// as 0x0067c3d0 is handed it -- and as the area search's truncated
/// floating-point root of the same square comes out.
[[nodiscard]] std::uint32_t root_of(Point a, Point b) noexcept {
  const std::uint32_t dx = static_cast<std::uint32_t>(a.x - b.x);
  const std::uint32_t dy = static_cast<std::uint32_t>(a.y - b.y);
  return static_cast<std::uint32_t>(isqrt(static_cast<std::int64_t>(dx * dx + dy * dy)));
}

}  // namespace

// --------------------------------------------------------------------------
// the areas finder
// --------------------------------------------------------------------------

AreaFinder::AreaFinder(const ObstructionGrid& live) : copy_(live), n_(live.width()) {
  // 0x0040fe00: a square of the map's width, every cell unassigned, over a
  // copy of the grid as it is now; the lattice cursor at (3, 3).
  if (n_ < 0) n_ = 0;
  cells_.assign(static_cast<std::size_t>(n_) * static_cast<std::size_t>(n_), 0xFFFFFFFFu);
}

bool AreaFinder::pick_seed(Cell& out) {
  // 0x00410fd0. The candidates at exactly the reach, in order; the first is
  // the seed and stays in the list.
  std::size_t kept = 0;
  for (const Cell c : candidates_) {
    if (distance(c.x, c.y) == kReach) candidates_[kept++] = c;
  }
  candidates_.resize(kept);
  if (!candidates_.empty()) {
    out = candidates_.front();
    return true;
  }
  // Otherwise the lattice, every fourth cell from (3, 3), for a free cell no
  // area holds. The cursor stays on the cell it answers.
  while (scan_y_ < n_) {
    while (scan_x_ < n_) {
      if (free(scan_x_, scan_y_) && distance(scan_x_, scan_y_) == 0xFFFF) {
        out = Cell{scan_x_, scan_y_};
        return true;
      }
      scan_x_ += 4;
    }
    scan_x_ = 3;
    scan_y_ += 4;
  }
  return false;
}

void AreaFinder::grow(Cell seed, std::uint16_t id) {
  // 0x00411110 with the limit 48: breadth first from the seed.
  struct Entry {
    std::int32_t x;
    std::int32_t y;
    std::uint32_t d;
  };
  std::deque<Entry> queue;
  queue.push_back(Entry{seed.x, seed.y, 0});
  set(seed.x, seed.y, id);
  // The neighbours in the original's order: below, above, right, left.
  constexpr std::int32_t kDx[4] = {0, 0, 1, -1};
  constexpr std::int32_t kDy[4] = {1, -1, 0, 0};
  while (!queue.empty()) {
    const Entry e = queue.front();
    queue.pop_front();
    const std::uint32_t d = e.d + 1;
    for (int k = 0; k < 4; ++k) {
      const std::int32_t nx = e.x + kDx[k];
      const std::int32_t ny = e.y + kDy[k];
      if (!free(nx, ny)) continue;
      // Whether the step leaves the cell's 4x4 block; blocks start at cell 2.
      // The coordinates are never negative here, so `& 3` is the remainder.
      bool crosses = false;
      switch (k) {
        case 0: crosses = ((e.y + 2) & 3) == 3; break;
        case 1: crosses = ((e.y + 2) & 3) == 0; break;
        case 2: crosses = ((e.x + 2) & 3) == 3; break;
        default: crosses = ((e.x + 2) & 3) == 0; break;
      }
      const std::uint16_t held = area(nx, ny);
      const std::uint16_t held_d = distance(nx, ny);
      bool keep = (crosses || held == id) && (d > kReach || held_d <= d);
      if (keep && held != id && held != kNoArea) {
        // Another area's cell, not to be taken by distance: taken all the
        // same when it has no way back to its own seed left -- no neighbour
        // of it in its area at one step less.
        const std::uint16_t back = static_cast<std::uint16_t>(held_d - 1);
        const auto leads = [&](std::int32_t x, std::int32_t y) {
          return area(x, y) == held && distance(x, y) == back;
        };
        if (!leads(nx - 1, ny) && !leads(nx + 1, ny) && !leads(nx, ny - 1) &&
            !leads(nx, ny + 1)) {
          keep = false;
        }
      }
      if (!keep) {
        queue.push_back(Entry{nx, ny, d});
        set(nx, ny, (d << 16) | id);
      } else if (d > kReach) {
        candidates_.push_back(Cell{nx, ny});
      }
    }
  }
}

bool AreaFinder::step(ObstructionGrid& live) {
  Cell seed{};
  if (!pick_seed(seed)) {
    // 0x00411af0's end: every cell no area took is blocked in the live grid.
    for (std::int32_t y = 0; y < n_; ++y) {
      for (std::int32_t x = 0; x < n_; ++x) {
        if (area(x, y) == kNoArea && live.in_bounds(x, y)) live.set_cell(x, y, true);
      }
    }
    return false;
  }
  grow(seed, static_cast<std::uint16_t>(seeds_.size()));
  seeds_.push_back(seed);
  return true;
}

// --------------------------------------------------------------------------
// the structure
// --------------------------------------------------------------------------

SmartPathfinder::SmartPathfinder(std::int32_t size, std::span<const Cell> seeds) : n_(size) {
  // 0x00416200: a record per seed, its point the seed cell's centre.
  areas_.resize(seeds.size());
  for (std::size_t i = 0; i < seeds.size(); ++i) areas_[i].seed = centre_of(seeds[i]);
  if (n_ < 0) n_ = 0;
  // The direction field's memory starts zeroed: a cell it never writes
  // points left.
  directions_.assign(static_cast<std::size_t>(n_) * static_cast<std::size_t>(n_), 0);
}

std::uint8_t SmartPathfinder::direction(std::int32_t x, std::int32_t y) const noexcept {
  if (x < 0 || y < 0 || x >= n_ || y >= n_) return 0;
  return directions_[static_cast<std::size_t>(y) * static_cast<std::size_t>(n_) +
                     static_cast<std::size_t>(x)];
}

void SmartPathfinder::init_directions(const AreaFinder& finder) {
  // 0x0040db30: towards the neighbour in the same area one step nearer its
  // seed -- left, right, above -- and below when none of the three is.
  for (std::int32_t y = 2; y < n_ - 2; ++y) {
    for (std::int32_t x = 2; x < n_ - 2; ++x) {
      const std::uint16_t a = finder.area(x, y);
      const std::uint16_t back = static_cast<std::uint16_t>(finder.distance(x, y) - 1);
      const auto leads = [&](std::int32_t nx, std::int32_t ny) {
        return finder.area(nx, ny) == a && finder.distance(nx, ny) == back;
      };
      std::uint8_t d = 3;
      if (leads(x - 1, y)) {
        d = 0;
      } else if (leads(x + 1, y)) {
        d = 1;
      } else if (leads(x, y - 1)) {
        d = 2;
      }
      directions_[static_cast<std::size_t>(y) * static_cast<std::size_t>(n_) +
                  static_cast<std::size_t>(x)] = d;
    }
  }
}

void SmartPathfinder::init_point_to_area(const AreaFinder& finder) {
  // 0x00411c00. A block per 4x4 cells from cell 2, enough of them for every
  // cell from 2 to n - 3, all empty to begin with.
  blocks_w_ = n_ >= 5 ? ((n_ - 5) >> 2) + 1 : 0;
  blocks_h_ = blocks_w_;
  blocks_.assign(static_cast<std::size_t>(blocks_w_) * static_cast<std::size_t>(blocks_h_), kNoArea);
  overflow_.clear();
  for (std::int32_t y = 2; y < n_ - 2; ++y) {
    for (std::int32_t x = 2; x < n_ - 2; ++x) {
      const std::int32_t bx = (x - 2) >> 2;
      const std::int32_t by = (y - 2) >> 2;
      std::uint16_t& block = blocks_[static_cast<std::size_t>(by) * static_cast<std::size_t>(blocks_w_) +
                                     static_cast<std::size_t>(bx)];
      const std::uint16_t a = finder.area(x, y);
      if (block == kNoArea) {
        block = a;
      } else if (a != kNoArea && a != block && block < 0x8000) {
        // A second area in the block: the block's sixteen cells, column by
        // column, go to the overflow list, and the block names that entry.
        std::array<std::uint16_t, 16> table{};
        const std::int32_t ox = 2 + 4 * bx;
        const std::int32_t oy = 2 + 4 * by;
        for (std::int32_t col = 0; col < 4; ++col) {
          for (std::int32_t row = 0; row < 4; ++row) {
            table[static_cast<std::size_t>(col * 4 + row)] = finder.area(ox + col, oy + row);
          }
        }
        overflow_.push_back(table);
        block = static_cast<std::uint16_t>(0x8000u + (overflow_.size() - 1));
      }
    }
  }
}

std::uint32_t SmartPathfinder::area_at(Point world) const noexcept {
  // 0x0040e340: the cell less two, its block, and the block's overflow entry
  // when it has one. A point off the block map -- within two cells of the
  // top or left edge, or past the last block -- reads memory the original
  // never wrote; here it is no area, and the search's callers guard on it.
  const std::int32_t cx = shift_cell(world.x) - 2;
  const std::int32_t cy = shift_cell(world.y) - 2;
  if (cx < 0 || cy < 0) return kNoArea;
  const std::int32_t bx = cx >> 2;
  const std::int32_t by = cy >> 2;
  if (bx >= blocks_w_ || by >= blocks_h_) return kNoArea;
  const std::uint16_t block =
      blocks_[static_cast<std::size_t>(by) * static_cast<std::size_t>(blocks_w_) +
              static_cast<std::size_t>(bx)];
  if (block <= 0x7FFF) return block;
  const std::size_t entry = block - 0x8000u;
  if (entry >= overflow_.size()) return kNoArea;
  return overflow_[entry][static_cast<std::size_t>((cx & 3) * 4 + (cy & 3))];
}

void SmartPathfinder::join(std::uint32_t a, std::uint32_t b, GridSearch& grid,
                           const ObstructionGrid& live) {
  // 0x00415620.
  if (a == b || b == kNoArea || b >= 0x8000) return;
  if (b < a) std::swap(a, b);
  // Not in the original, whose ids always name a record.
  if (b >= areas_.size()) return;
  for (const Link& link : areas_[a].links) {
    if (link.area == b) return;
  }
  const auto index = static_cast<std::uint16_t>(connections_.size());
  areas_[a].links.push_back(Link{static_cast<std::uint16_t>(b), index});
  areas_[b].links.push_back(Link{static_cast<std::uint16_t>(a), index});

  // The run: the structure's grid search from a's seed to b's alone, no
  // owner, the straightening flag set.
  const GoalPoint goal{areas_[b].seed, true};
  PathRequest request;
  request.start = areas_[a].seed;
  request.goal = areas_[b].seed;
  request.goal_points = std::span<const GoalPoint>(&goal, 1);
  request.goal_points_given = true;
  GoalCells goals;
  goals.assign(request.goal_points);
  CellView view;
  view.grid = &live;
  const Path route = grid.run(view, request, goals, /*both_ways=*/true);

  Connection connection;
  connection.cost = static_cast<std::uint16_t>(route.length);
  if (route.waypoints.size() > 2) {
    connection.run.assign(route.waypoints.begin() + 1, route.waypoints.end() - 1);
  }
  connections_.push_back(std::move(connection));
}

void SmartPathfinder::collect_pairs(
    std::vector<std::pair<std::uint32_t, std::uint32_t>>& out) const {
  // 0x00415e30: a pair (A, C) wherever going A -> B -> C costs more than 1.3
  // times the straight distance between the seeds. The constant is a double;
  // for these integers the test is exactly 13 d < 10 (cost + cost).
  for (std::uint32_t a = 0; a < areas_.size(); ++a) {
    for (const Link& ab : areas_[a].links) {
      const std::uint32_t first = connections_[ab.connection].cost;
      for (const Link& bc : areas_[ab.area].links) {
        const std::uint64_t d = root_of(areas_[a].seed, areas_[bc.area].seed);
        const std::uint64_t sum = static_cast<std::uint64_t>(connections_[bc.connection].cost) + first;
        if (13 * d < 10 * sum) out.emplace_back(a, bc.area);
      }
    }
  }
}

// --------------------------------------------------------------------------
// the area search
// --------------------------------------------------------------------------
//
// The library heap again (0x00410ca0 pushes through 0x0040e0f0; the pop is
// 0x0040e140), in the same shape as the grid search's: records, not keys,
// every comparison reading cost plus estimate as they stand.

void SmartPathfinder::sift_up(std::size_t hole, std::size_t top, std::uint32_t value) {
  const std::uint32_t k = key(value);
  while (top < hole) {
    const std::size_t parent = (hole - 1) / 2;
    if (!(k < key(heap_[parent]))) break;
    heap_[hole] = heap_[parent];
    hole = parent;
  }
  heap_[hole] = value;
}

void SmartPathfinder::push(std::uint32_t n) {
  heap_.push_back(n);
  sift_up(heap_.size() - 1, 0, n);
}

std::uint32_t SmartPathfinder::pop() {
  const std::uint32_t top = heap_.front();
  if (heap_.size() > 1) {
    const std::uint32_t moved = heap_.back();
    heap_.back() = top;
    const std::size_t len = heap_.size() - 1;
    std::size_t hole = 0;
    std::size_t child = 2;
    while (child < len) {
      // The right child unless the left is strictly smaller.
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

std::uint32_t SmartPathfinder::area_search(std::uint32_t start, Point heading, bool& arrived) {
  // 0x00413690.
  heap_.clear();
  Node& first = nodes_[start];
  first.g = 0;
  first.h = root_of(areas_[start].seed, heading);
  first.parent = kNone;
  push(start);
  std::uint32_t best = start;
  std::uint32_t best_h = first.h;
  std::uint32_t budget = kBudget;
  arrived = false;
  for (;;) {
    if (heap_.empty()) return best;
    const std::uint32_t current = pop();
    if (budget-- == 0) return best;
    if (nodes_[current].goal) {
      arrived = true;
      return current;
    }
    for (const Link& link : areas_[current].links) {
      const Connection& connection = connections_[link.connection];
      if (connection.disabled) continue;
      const std::uint32_t g = connection.cost + nodes_[current].g;
      Node& next = nodes_[link.area];
      // Strictly cheaper only.
      if (!(g < next.g)) continue;
      // Waiting in the heap is an estimate neither 0 nor the closed mark.
      const bool waiting = next.h != 0 && next.h != kClosed;
      next.g = g;
      next.parent = current;
      if (!waiting) {
        next.h = root_of(areas_[link.area].seed, heading);
        push(link.area);
      }
    }
    if (nodes_[current].h < best_h) {
      best = current;
      best_h = nodes_[current].h;
    }
    nodes_[current].h = kClosed;
  }
}

bool SmartPathfinder::walk(Point from, std::uint32_t area, bool reversed,
                           std::vector<Point>& out) const {
  // 0x00412040: from the point's cell by the directions until the seed's
  // cell, each new cell's centre laid. The original gives up when a point it
  // lays is one it has laid before (looked for once the walk is over a
  // hundred points long); the step depends on the cell alone, so that is a
  // walk caught in a loop, which is what a set of the cells walked catches.
  const Point seed = areas_[area].seed;
  const std::int32_t sx = shift_cell(seed.x);
  const std::int32_t sy = shift_cell(seed.y);
  std::int32_t x = shift_cell(from.x);
  std::int32_t y = shift_cell(from.y);
  std::vector<Point> laid{from};
  std::vector<std::uint64_t> seen;
  const auto key_of = [](std::int32_t cx, std::int32_t cy) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy)) << 32) |
           static_cast<std::uint32_t>(cx);
  };
  const auto visit = [&](std::int32_t cx, std::int32_t cy) {
    const std::uint64_t k = key_of(cx, cy);
    const auto it = std::lower_bound(seen.begin(), seen.end(), k);
    if (it != seen.end() && *it == k) return false;
    seen.insert(it, k);
    return true;
  };
  (void)visit(x, y);
  while (x != sx || y != sy) {
    // Off the direction field the original reads memory it never wrote; here
    // the walk fails.
    if (x < 0 || y < 0 || x >= n_ || y >= n_) return false;
    switch (direction(x, y)) {
      case 0: --x; break;
      case 1: ++x; break;
      case 2: --y; break;
      default: ++y; break;
    }
    if (!visit(x, y)) return false;
    laid.push_back(centre_of(Cell{x, y}));
  }
  if (reversed) {
    out.insert(out.end(), laid.rbegin(), laid.rend());
  } else {
    out.insert(out.end(), laid.begin(), laid.end());
  }
  return true;
}

// --------------------------------------------------------------------------
// gates
// --------------------------------------------------------------------------

void SmartPathfinder::gate_connections(Point a, Point b, std::vector<std::uint32_t>& out) const {
  // 0x00412540: for each area, each neighbour with a higher id, the route
  // laid out as the seed, the run and the neighbour's seed, its legs met
  // against the axis one by one; the first leg that meets it lists the
  // connection.
  std::vector<Point> route;
  for (std::size_t area = 0; area < areas_.size(); ++area) {
    for (const Link& link : areas_[area].links) {
      if (link.area <= area) continue;
      const Connection& connection = connections_[link.connection];
      route.clear();
      route.push_back(areas_[area].seed);
      route.insert(route.end(), connection.run.begin(), connection.run.end());
      route.push_back(areas_[link.area].seed);
      if (route_crossing(route, a, b) >= 0) out.push_back(link.connection);
    }
  }
}

void SmartPathfinder::set_closed(std::span<const std::uint32_t> connections, bool closed) noexcept {
  for (const std::uint32_t c : connections) {
    if (c < connections_.size()) connections_[c].disabled = closed;
  }
}

// --------------------------------------------------------------------------
// a search
// --------------------------------------------------------------------------

Path SmartPathfinder::search(GridSearch& grid, const CellView& live_view,
                             const PathRequest& request, const GoalCells& goals) {
  Path out;
  const ObstructionGrid& live = *live_view.grid;
  // Every live-grid test the smart pathfinder makes reads the grid as the
  // caller laid it, gate lines and all, and no cell is opened.
  CellView view = live_view;
  view.open = false;

  // 0x0040e260: every record's flags clear, estimate 0, cost all-ones.
  nodes_.assign(areas_.size(), Node{});

  if (request.ignore_passability) return grid.run(view, request, goals, false);

  // A walled-in start moves to the first free probe round it (0x0040dc90).
  Point start = request.start;
  const auto blocked_at = [&](Point p) { return view.blocked(shift_cell(p.x), shift_cell(p.y)); };
  if (blocked_at(start)) {
    static constexpr Point kProbes[12] = {{0, 16},  {0, -16},  {16, 0},   {-16, 0},
                                          {0, 32},  {0, -32},  {32, 0},   {-32, 0},
                                          {16, 16}, {-16, -16}, {16, -16}, {-16, 16}};
    bool moved = false;
    for (const Point probe : kProbes) {
      const Point at{start.x + probe.x, start.y + probe.y};
      if (!blocked_at(at)) {
        start = at;
        moved = true;
        break;
      }
    }
    if (!moved) {
      out.waypoints = {start, start};
      out.status = PathStatus::partial;
      return out;
    }
  }

  const std::uint32_t start_area = area_at(start);
  // Not in the original, which would index past its records: a start off the
  // block map goes to the grid search.
  if (start_area >= areas_.size()) return grid.run(live_view, request, goals, false);
  nodes_[start_area].joined = true;
  for (const Link& link : areas_[start_area].links) nodes_[link.area].joined = true;

  const std::vector<Point>& points = goals.live();
  bool near = false;
  for (const Point& p : points) {
    const std::uint32_t a = area_at(p);
    if (a >= areas_.size()) continue;
    nodes_[a].goal = true;
    if (nodes_[a].joined) near = true;
  }
  if (points.empty()) {
    // No goal point left: the area under where the search heads, when that
    // is more than a cell in from the top and left edges, on the map and
    // free.
    const Point h = request.goal;
    if (shift_cell(h.x) > 1 && shift_cell(h.y) > 1 && h.x < live.extent_x() &&
        h.y < live.extent_y() && !view.blocked(shift_cell(h.x), shift_cell(h.y))) {
      const std::uint32_t a = area_at(h);
      if (a < areas_.size()) nodes_[a].goal = true;
    }
  }

  // A near goal from where the unit stands: the grid search.
  if (near && start == request.start) return grid.run(live_view, request, goals, false);

  if (grid.straight_line(view, request, goals, out)) return out;

  // The area search.
  bool arrived = false;
  const std::uint32_t reached = area_search(start_area, request.goal, arrived);
  if (points.empty()) arrived = false;

  // The route's points, end first (0x004138d0 .. 0x00413fb3).
  std::vector<Point> route;
  bool failed = false;
  Point end{};
  if (!arrived) {
    // The first cell on the line from where the search heads towards the
    // best area's seed that the best area holds (0x0040e500, 0x0040e3d0).
    const Point seed = areas_[reached].seed;
    bool hit = false;
    Point at{};
    walk_cells(div_cell(request.goal.x), div_cell(request.goal.y), div_cell(seed.x),
               div_cell(seed.y), [&](std::int32_t x, std::int32_t y) {
                 const Point c = centre_of(Cell{x, y});
                 if (x > 1 && y > 1 && c.x < live.extent_x() && c.y < live.extent_y() &&
                     !view.blocked(x, y) && area_at(c) == reached) {
                   hit = true;
                   at = c;
                   return false;
                 }
                 return true;
               });
    if (!hit) {
      end = seed;
    } else {
      if (!walk(at, reached, false, route)) failed = true;
      end = at;
    }
  } else {
    // Of the goal points in the reached area, the nearest to the area it was
    // reached from (the start, when it is the start's).
    const std::uint32_t parent = nodes_[reached].parent;
    const Point from = parent == kNone ? start : areas_[parent].seed;
    std::uint32_t best = kNone;
    std::size_t pick = points.size();
    for (std::size_t i = 0; i < points.size(); ++i) {
      if (area_at(points[i]) != reached) continue;
      const std::uint32_t d = root_of(points[i], from);
      if (d < best) {
        best = d;
        pick = i;
      }
    }
    if (pick < points.size() && walk(points[pick], reached, false, route)) {
      end = points[pick];
    } else {
      failed = true;
    }
  }

  // Each area back to the start's: its seed, then the run to its parent
  // (0x00411ee0), as stored from the lower id's seed or reversed.
  std::uint32_t area = reached;
  while (nodes_[area].parent != kNone) {
    const std::uint32_t parent = nodes_[area].parent;
    route.push_back(areas_[area].seed);
    for (const Link& link : areas_[area].links) {
      if (link.area != parent) continue;
      const std::vector<Point>& run = connections_[link.connection].run;
      if (area < parent) {
        route.insert(route.end(), run.begin(), run.end());
      } else {
        route.insert(route.end(), run.rbegin(), run.rend());
      }
      break;
    }
    area = parent;
  }
  route.push_back(areas_[area].seed);
  if (!walk(start, area, true, route)) failed = true;

  if (failed) {
    out.waypoints = {start, start};
    out.status = points.empty() ? PathStatus::no_goal : PathStatus::partial;
    return out;
  }

  // The cells, start first: a cell line between each pair of points from
  // the last pair to the first (0x00412a70), each line's last cell dropped
  // but the final one's.
  std::vector<Cell> cells;
  for (std::size_t k = route.size() - 1; k >= 1; --k) {
    walk_cells(div_cell(route[k].x), div_cell(route[k].y), div_cell(route[k - 1].x),
               div_cell(route[k - 1].y), [&](std::int32_t x, std::int32_t y) {
                 cells.push_back(Cell{x, y});
                 return true;
               });
    if (k != 1 && !cells.empty()) cells.pop_back();
  }

  // The patch (0x00412260): free cells copied; a blocked stretch bridged by
  // the grid search's A* alone from the cell before it to the next free one.
  std::vector<Cell> laid;
  bool patched = false;
  std::vector<Cell> bridge;
  for (std::size_t i = 0; i < cells.size();) {
    if (!view.blocked(cells[i].x, cells[i].y)) {
      laid.push_back(cells[i]);
      ++i;
      continue;
    }
    // The first cell is the start's, which is free; the original would read
    // before the list here.
    if (i == 0) {
      ++i;
      continue;
    }
    const Cell before = cells[i - 1];
    std::size_t j = i;
    while (j < cells.size() && view.blocked(cells[j].x, cells[j].y)) ++j;
    if (j == cells.size()) break;
    const Point target = centre_of(cells[j]);
    const GoalPoint alone{target, true};
    GoalCells window;
    window.assign(std::span<const GoalPoint>(&alone, 1));
    grid.chain(view, before, target, window, bridge);
    for (std::size_t k = 1; k + 1 < bridge.size(); ++k) laid.push_back(bridge[k]);
    patched = true;
    i = j;
  }

  // An owner's route that did not arrive is cut back to a free spot.
  if (request.owner != nullptr && !arrived && !laid.empty()) {
    for (;;) {
      if (laid.size() < 2 || request.owner->free(centre_of(laid.back()))) {
        end = laid.size() == 1 ? request.start : centre_of(laid.back());
        break;
      }
      laid.pop_back();
      out.cut = true;
    }
  }

  straighten(view, laid, /*both_ways=*/false, out.waypoints);
  if (out.waypoints.empty()) out.waypoints.push_back(request.start);
  out.waypoints.front() = request.start;
  out.waypoints.push_back(end);
  out.length = polyline_length(out.waypoints);
  // The goal window as the patches left it: one point each.
  if (points.empty() && !patched) {
    out.status = PathStatus::no_goal;
  } else {
    out.status = arrived ? PathStatus::found : PathStatus::partial;
  }
  return out;
}

// --------------------------------------------------------------------------
// the builder
// --------------------------------------------------------------------------

SmartBuild::SmartBuild(const ObstructionGrid& live) : finder_(live) {}

bool SmartBuild::find_areas(ObstructionGrid& live) { return finder_.step(live); }

void SmartBuild::init_data() {
  smart_ = std::make_unique<SmartPathfinder>(finder_.size(), finder_.seeds());
}

void SmartBuild::init_directions() {
  if (smart_ != nullptr) smart_->init_directions(finder_);
}

void SmartBuild::init_point_to_area() {
  if (smart_ != nullptr) smart_->init_point_to_area(finder_);
}

void SmartBuild::init_connect() {
  x0_ = 2;
  y0_ = 2;
  x1_ = finder_.size() - 3;
  y1_ = finder_.size() - 3;
  row_ = y0_;
}

bool SmartBuild::connect_row(GridSearch& grid, const ObstructionGrid& live) {
  // 0x00415880 over the row, then the row moves on (0x00415a40).
  if (smart_ != nullptr) {
    for (std::int32_t x = x0_; x <= x1_; ++x) {
      const std::uint16_t a = finder_.area(x, row_);
      if (a == kNoArea) continue;
      smart_->join(a, finder_.area(x - 1, row_), grid, live);
      smart_->join(a, finder_.area(x, row_ - 1), grid, live);
      smart_->join(a, finder_.area(x + 1, row_), grid, live);
      smart_->join(a, finder_.area(x, row_ + 1), grid, live);
    }
  }
  ++row_;
  return row_ <= y1_;
}

void SmartBuild::increase() {
  // 0x004161d0: the pairs are added to the list, and the next to join is
  // its first.
  if (smart_ != nullptr) smart_->collect_pairs(pairs_);
  next_pair_ = 0;
}

bool SmartBuild::increase_step(GridSearch& grid, const ObstructionGrid& live) {
  // 0x004159b0: ten pairs at a time.
  const std::size_t last = std::min(next_pair_ + 10, pairs_.size());
  while (next_pair_ < last) {
    if (smart_ != nullptr) smart_->join(pairs_[next_pair_].first, pairs_[next_pair_].second, grid, live);
    ++next_pair_;
  }
  return next_pair_ < pairs_.size();
}

std::unique_ptr<SmartPathfinder> SmartBuild::finish() { return std::move(smart_); }

}  // namespace imperivm::core::sim
