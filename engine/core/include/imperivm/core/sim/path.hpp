#pragma once

// The obstruction grid, the integer geometry the simulation moves on, and a
// deterministic A* over both.
//
// Specifications: docs/formats/pass.md (the grid container and its polarity),
//                 docs/engine/projection.md (why the grid is authored, not derived).
//
// ## Three facts this file is built on
//
//   1. **Collision granularity is 16 world units.** A `.pass` stamp and a map's
//      `Terrain.pass.grid` both declare `cell_size = 16`; the terrain-type grid
//      declares 64. They are four times apart and cannot share an index, so
//      this file never touches the terrain grid.
//   2. **The map's obstruction bitmap is authored data.** `projection.md`
//      measured a 70.5% ceiling on reproducing it from object placement -- the
//      rest is hand-painted -- so `ObstructionGrid::from_grid` adopts the
//      shipped bits verbatim. `stamp` exists for objects that appear *after*
//      load (a building finished mid-game), not to rebuild what shipped.
//   3. **A set bit means blocked.** Off the edge of the grid also means
//      blocked: a unit may not walk out of the world, and returning "free" for
//      an out-of-range cell would let a search escape into unbounded space.
//
// ## Why the pathfinder may not be hashed
//
// Of the original's seven hash channels, `pathfinder` is zero in all nine
// desync dumps: the shipped build kept pathfinding out of the determinism
// contract (docs/engine/state-vector.md). That is a constraint, not a defect to
// repair. So nothing in this file is folded into the world hash -- not the open
// list, not the scratch arrays, not the resulting waypoints.
//
// It does *not* follow that the pathfinder may be sloppy. Where units end up
// **is** hashed, and a path decides that, so the search has to be a pure
// function of (grid, request):
//
//   * No floating point. Costs are integers (10 orthogonal, 14 diagonal, an
//     integer approximation of 10*sqrt(2) accurate to 1%), the heuristic is the
//     matching octile distance, and every length is an integer square root.
//   * No unordered container. The open set is a binary heap ordered by the
//     **total** order `(f, h, cell)`. Ties therefore cannot be broken by
//     insertion order, which means the sequence of popped nodes is uniquely
//     determined by the grid and the request -- independent of how the heap
//     happened to sift, of allocation addresses, and of whether scratch buffers
//     were reused.
//   * A fixed neighbour order, listed once in `path.cpp`.
//
// ## Bounded work
//
// A search that cannot reach its goal would otherwise flood the whole map:
// a 16,384-unit map is 1,024 x 1,024 cells, a million nodes. `PathRequest`
// carries a node budget, and an exhausted search returns the best partial route
// it found rather than nothing, because a unit that walks most of the way and
// stops is what the original does and is better than one that refuses to move.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

/// World units per collision cell. Four times finer than `kTerrainTileSize`.
inline constexpr std::int32_t kCollisionCellSize = 16;

// --------------------------------------------------------------------------
// integer geometry
// --------------------------------------------------------------------------
//
// Everything a movement or formation calculation needs to know about angles and
// distances, with no floating point anywhere. `point.SetLen` and `point.Rot`
// are shipped host entry points -- `DEER_IDLE.VS` calls both on the first three
// lines -- so this is interoperability surface, not convenience.

/// Floor of the square root, exact for every non-negative input.
///
/// Declared here so that geometry has it without dragging the script VM's
/// headers in; **defined once, in `sim/world_host.cpp`**, which is where the
/// foundation put it. There is exactly one right answer only if everybody uses
/// the same function, and `std::sqrt` is floating point and therefore forbidden
/// (docs/engine/architecture.md, rule 1).
[[nodiscard]] std::int64_t isqrt(std::int64_t value) noexcept;

/// Squared distance. 64-bit because a 16,384-unit map squares to 2.7e8 per
/// axis, and a comparison of two of those must not wrap.
[[nodiscard]] constexpr std::int64_t dist_sq(Point a, Point b) noexcept {
  const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
  const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
  return dx * dx + dy * dy;
}

/// Euclidean distance, rounded down. Every distance comparison in the
/// simulation should prefer `dist_sq` against a squared bound; this exists for
/// the places that need a length in world units (path arc lengths, `SetLen`).
[[nodiscard]] std::int32_t distance(Point a, Point b) noexcept;

/// Length of a vector from the origin.
[[nodiscard]] inline std::int32_t vector_length(Point v) noexcept {
  return distance(Point{0, 0}, v);
}

/// Whether `a` and `b` are no more than `range` apart, without a square root.
[[nodiscard]] constexpr bool within(Point a, Point b, std::int32_t range) noexcept {
  if (range < 0) return false;
  const std::int64_t r = range;
  return dist_sq(a, b) <= r * r;
}

/// Sine and cosine of an integer number of degrees, in Q15 (32768 = 1).
///
/// A 91-entry table and the three symmetries, so that `sin_q15(30)` is 16384
/// exactly and `sin_q15(d) == sin_q15(d + 360)` for every `d`, including
/// negative ones. There is no interpolation: degrees are the resolution the
/// scripts ask in (`pt.Rot(rand(180))`, `pt.Rot(rand(30)-15)`).
[[nodiscard]] std::int32_t sin_q15(std::int32_t degrees) noexcept;
[[nodiscard]] std::int32_t cos_q15(std::int32_t degrees) noexcept;

/// `point.Rot(degrees)`: rotate about the origin.
///
/// Rounds each component to nearest, half away from zero, so that rotating by
/// zero is the identity and rotating a short vector does not collapse it.
[[nodiscard]] Point rotate_degrees(Point v, std::int32_t degrees) noexcept;

/// Rescale to `length`, keeping the direction. **Movement's, not the script
/// entry point's** -- `point::SetLen` lives in `sim/world_host.cpp` and does
/// not share this.
///
/// A zero vector stays zero here, and that is now known to differ from the
/// original: 0x00697d81 writes `L` into *y* and zero into *x*, so `gbr.exe`
/// gives a direction-less vector a direction, pointing north. The reasoning
/// that stood here -- `DEER_IDLE.VS` calls `.GetDir` first precisely so it has
/// one, and map objects are authored `dir=(0,1)` rather than `(0,0)` -- is
/// sound and reaches the wrong conclusion; both facts are true and neither
/// implies the engine leaves the vector alone.
///
/// This one is left as it is **on purpose**: its callers are in
/// `sim/movement.cpp`, and changing what a facing vector does would move every
/// map's world hash. The divergence is recorded rather than repaired, and the
/// two spellings should be reconciled when the conformance baselines are next
/// rebuilt.
[[nodiscard]] Point set_length(Point v, std::int32_t length) noexcept;

// --------------------------------------------------------------------------
// the obstruction grid
// --------------------------------------------------------------------------

/// Static passability at 16-unit granularity: one bit per cell, set is blocked.
///
/// Owns its bits rather than viewing them, because a map's grid outlives the
/// container buffer it was read from and a mid-game building has to be able to
/// stamp itself in. A 16,384-unit map costs 128 KB.
class ObstructionGrid {
 public:
  ObstructionGrid() = default;

  /// An empty grid `width` x `height` cells. Everything free.
  ObstructionGrid(std::int32_t width, std::int32_t height);

  /// Adopt a shipped `GRID` payload -- a map's `Terrain.pass.grid`, or a
  /// `.pass` stamp read on its own.
  ///
  /// Requires `cell_size == 16` and `bits_per_cell == 1`. A 64-unit terrain
  /// grid is refused rather than silently reinterpreted, which is the mistake
  /// this returns an error to prevent.
  [[nodiscard]] static Result<ObstructionGrid> from_grid(const Grid& grid);

  [[nodiscard]] std::int32_t width() const noexcept { return width_; }
  [[nodiscard]] std::int32_t height() const noexcept { return height_; }
  [[nodiscard]] std::int32_t extent_x() const noexcept { return width_ * kCollisionCellSize; }
  [[nodiscard]] std::int32_t extent_y() const noexcept { return height_ * kCollisionCellSize; }
  [[nodiscard]] bool empty() const noexcept { return width_ <= 0 || height_ <= 0; }

  [[nodiscard]] static constexpr std::int32_t cell_of(std::int32_t world) noexcept {
    // Floor division: negative world coordinates exist (`kHeldPosition` is
    // (-1,-1)) and truncation would fold them onto cell 0.
    return world >= 0 ? world / kCollisionCellSize
                      : -((-world + kCollisionCellSize - 1) / kCollisionCellSize);
  }
  /// The centre of a cell, which is what a path waypoint lands on.
  [[nodiscard]] static constexpr std::int32_t centre_of(std::int32_t cell) noexcept {
    return cell * kCollisionCellSize + kCollisionCellSize / 2;
  }

  [[nodiscard]] bool in_bounds(std::int32_t cx, std::int32_t cy) const noexcept {
    return cx >= 0 && cy >= 0 && cx < width_ && cy < height_;
  }

  /// Blocked, or outside the grid. Off-map is blocked on purpose: see the
  /// header.
  [[nodiscard]] bool blocked_cell(std::int32_t cx, std::int32_t cy) const noexcept;
  [[nodiscard]] bool blocked(Point world) const noexcept {
    return blocked_cell(cell_of(world.x), cell_of(world.y));
  }

  void set_cell(std::int32_t cx, std::int32_t cy, bool blocked) noexcept;

  /// Stamp a `.pass` footprint anchored at `anchor`, **laid flat and the
  /// way up it is stored** -- which is not what the original does, and
  /// nothing in the simulation calls this. The original's stamp (0x00546a80)
  /// mirrors the rows about the anchor and puts each cell through the
  /// inverse projection with the terrain height, and it is transcribed in
  /// `core/world/editor.hpp` (`edit::stamp_footprint`), where every shipped
  /// map layer proves it. The map's pre-baked layer is what the simulation
  /// adopts; when a building spawned in play needs a footprint, that stamp
  /// is the one to call, not this. `vertical_correction` is the guess this
  /// kept for a question `pass.md` has since answered.
  /// Blocked cells are OR-ed in; clear cells of the stamp leave the map alone,
  /// which is what makes the interior holes of a footprint (the courtyard of
  /// `BHOUSE01`, the gap in a gate) behave.
  void stamp(const Grid& mask, Point anchor, std::int32_t vertical_correction = 0) noexcept;
  /// Clear the cells a stamp covers. For an object that is removed.
  void unstamp(const Grid& mask, Point anchor, std::int32_t vertical_correction = 0) noexcept;

  /// `IsPassable3x3(pt)`: the cell under `pt` and its eight neighbours are all
  /// free. The shipped scripts use this to test a destination before walking to
  /// it, so it is a footprint test, not a point test.
  [[nodiscard]] bool passable_3x3(Point world) const noexcept;

  /// Whether the straight segment `a`..`b` crosses no blocked cell.
  ///
  /// Samples every `kCollisionCellSize / 4` world units, which cannot skip a
  /// cell. Used for path smoothing, and cheap enough that a unit with a clear
  /// line to its destination never runs the search at all.
  [[nodiscard]] bool line_is_clear(Point a, Point b) const noexcept;

  /// `Unit.ClipDestToMap`: the nearest point inside the world square.
  [[nodiscard]] Point clamp_to_map(Point world) const noexcept;

  /// The nearest free cell to `world`, searched in rings of increasing radius
  /// and, within a ring, in a fixed order. Used when a destination lands inside
  /// a building. Returns `world` unchanged when nothing is free within
  /// `max_rings`.
  [[nodiscard]] Point nearest_free(Point world, std::int32_t max_rings = 32) const noexcept;

  /// Set cells. A cross-check against the shipped file, nothing more.
  [[nodiscard]] std::size_t count_blocked() const noexcept;

 private:
  [[nodiscard]] std::size_t bit_index(std::int32_t cx, std::int32_t cy) const noexcept {
    return static_cast<std::size_t>(cy) * static_cast<std::size_t>(width_) +
           static_cast<std::size_t>(cx);
  }

  std::vector<std::uint64_t> words_;
  std::int32_t width_ = 0;
  std::int32_t height_ = 0;
};

/// Cells one search treats as blocked on top of the grid, and nobody else
/// does: a closed gate's line, laid for a single search and gone after it.
///
/// `gbr.exe` lays a gate's line into its one grid before a search and takes it
/// out again after (0x0041945f lays, 0x00419544 and 0x004196b6 lift), so what
/// the search sees and what every other reader of the grid sees differ for the
/// length of the search. This is that difference held apart, so the grid
/// itself never changes. See `sim/gate.hpp`, "The barrier".
///
/// A sorted set of cells with its bounding box: a test outside the box is four
/// comparisons, and the box is a few gates' worth of cells.
class CellOverlay {
 public:
  void clear() noexcept;
  void add(std::int32_t cx, std::int32_t cy);
  /// Sort and deduplicate. Call once after the last `add`, before any test.
  void seal();
  [[nodiscard]] bool empty() const noexcept { return keys_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return keys_.size(); }
  [[nodiscard]] bool contains(std::int32_t cx, std::int32_t cy) const noexcept;

 private:
  [[nodiscard]] static constexpr std::uint64_t key(std::int32_t cx, std::int32_t cy) noexcept {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy)) << 32) |
           static_cast<std::uint32_t>(cx);
  }
  std::vector<std::uint64_t> keys_;
  std::int32_t min_x_ = 0;
  std::int32_t min_y_ = 0;
  std::int32_t max_x_ = -1;
  std::int32_t max_y_ = -1;
};

// --------------------------------------------------------------------------
// the search
// --------------------------------------------------------------------------

enum class PathStatus : std::uint8_t {
  /// Already inside the arrival range; the route is a single point.
  arrived,
  /// A complete route to the goal (or into its arrival annulus).
  found,
  /// The goal could not be reached, either because it is walled off or because
  /// the node budget ran out. The route leads to the closest node reached.
  partial,
  /// Nothing to do: no grid, or the start is off the map.
  unreachable,
};

struct PathRequest {
  Point start;
  Point goal;
  /// Stop this far short of `goal`. `Goto`'s second argument, and 0 for a plain
  /// move. A unit approaching a building stops at its radius rather than
  /// walking into it.
  std::int32_t arrival_range = 0;
  /// Cap on expanded nodes. Reached, the search returns its best partial route.
  /// 20,000 nodes covers roughly a 140-cell radius of open ground, which is
  /// 2,200 world units -- more than a screen.
  std::uint32_t node_budget = 20000;
  /// Skip the smoothing pass and return one waypoint per cell. For tests that
  /// want to see the raw search.
  bool smooth = true;
  /// The mover's class sets `ignore_passability` (`[class+0x31c]`, stored at
  /// `0x005a5317`): the route is the straight line from `start` to `goal`,
  /// whatever the grid says. Every path request `gbr.exe` makes passes that
  /// field as the pathfinder's last argument (`0x00419986`, `0x005de31e`,
  /// `0x005de593`), and the smart pathfinder's goal handling (`0x0040b580`)
  /// answers a non-zero one with a two-point route, start then goal, before
  /// any search (`0x0040b627` → `0x0040b756`). The sentries are its only
  /// users: they are placed on a wall's walkway, which the grid blocks, and
  /// walk along it. *Inferred:* the point list the goal is taken from
  /// (`0x008c517c`) holds the request's goal.
  bool ignore_passability = false;
  /// Cells blocked for this search alone, on top of the grid: the lines of the
  /// gates that bar this mover (`sim/gate.hpp`). Null or empty is the grid as
  /// it stands. Every grid test the search makes -- the straight line, the
  /// goal's free cell, the expansion, the smoothing -- sees them.
  const CellOverlay* barrier = nullptr;
};

/// A route: a polyline in world units, `waypoints[0]` being `start`.
struct Path {
  std::vector<Point> waypoints;
  PathStatus status = PathStatus::unreachable;
  /// Arc length of the polyline, in world units.
  std::int64_t length = 0;
  /// Nodes the search expanded. Diagnostics only -- never hashed.
  std::uint32_t expanded = 0;

  [[nodiscard]] bool usable() const noexcept {
    return status == PathStatus::found || status == PathStatus::partial;
  }
};

/// A* over the obstruction grid.
///
/// Holds its scratch arrays between calls, sized to the grid, and stamps them
/// with a generation counter instead of clearing them. That is a pure
/// optimisation: the arrays are never read before they are written in a given
/// search, so a fresh `PathFinder` and a reused one produce identical routes.
/// **None of this is world state and none of it is hashed.**
class PathFinder {
 public:
  [[nodiscard]] Path find(const ObstructionGrid& grid, const PathRequest& request);

  /// Total nodes expanded since construction. A budget diagnostic.
  [[nodiscard]] std::uint64_t total_expanded() const noexcept { return total_expanded_; }

 private:
  struct Node {
    std::int32_t g = 0;
    std::int32_t f = 0;
    std::int32_t h = 0;
    std::uint32_t parent = 0;
    std::uint32_t generation = 0;
    bool closed = false;
  };
  /// Heap entry. Ordered by `(f, h, cell)`, a total order over distinct cells,
  /// so the pop sequence does not depend on insertion order.
  struct Open {
    std::int32_t f = 0;
    std::int32_t h = 0;
    std::uint32_t cell = 0;
  };

  /// The search over whatever `blocked(cx, cy)` says is blocked: the grid
  /// alone, or the grid and a request's barrier. A template so that the
  /// common search, with no barrier, pays nothing for the other.
  template <typename Blocked>
  Path find_over(const ObstructionGrid& grid, const PathRequest& request, const Blocked& blocked);
  void reset(const ObstructionGrid& grid);
  void push(Open entry);
  Open pop();

  std::vector<Node> nodes_;
  std::vector<Open> heap_;
  std::vector<std::uint32_t> cells_;  ///< the raw cell route, reused
  std::int32_t width_ = 0;
  std::int32_t height_ = 0;
  std::uint32_t generation_ = 0;
  std::uint64_t total_expanded_ = 0;
};

/// Arc length of a polyline, in world units.
[[nodiscard]] std::int64_t polyline_length(std::span<const Point> waypoints) noexcept;

/// The point `distance` along a polyline, and the segment it falls in.
///
/// Clamps to the last waypoint past the end. `segment` is written with the
/// index of the segment start, which lets a caller keep a facing direction
/// without re-scanning.
[[nodiscard]] Point point_along(std::span<const Point> waypoints, std::int64_t distance,
                                std::size_t* segment = nullptr) noexcept;

}  // namespace imperivm::core::sim
