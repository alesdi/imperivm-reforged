#pragma once

// The original's smart pathfinder: the structure `DATA\PATHFINDERINIT.VS`
// builds a slice at a time, and the search that answers every route once
// `SPFDone` has installed it.
//
// Specification: docs/formats/pathfinder.md, "The smart pathfinder: the
// build" and "The smart pathfinder: a search". Everything here is written
// from that prose; the addresses are there so each rule can be checked
// against the executable.
//
// In outline:
//
//   * `AreaFinder` (0x0040fe00) cuts the free cells of a copy of the grid
//     into **areas**, one per call (0x00411af0): a seed, then a growth that
//     takes every cell of its own 4x4 block and crosses a block edge only
//     within 48 steps. When no seed is left, every cell no area took is set
//     blocked in the live grid.
//   * `SmartPathfinder` (0x00416200) holds one record per area, a two-bit
//     direction per cell back to its area's seed (0x0040db30), the 4x4
//     block map from a point to its area (0x00411c00, 0x0040e340), and the
//     **connections** between areas, each a cost and a run of points laid by
//     the grid search (0x00415620).
//   * `SmartBuild` (0x0040ff80) is the builder the nine `SPF*` commands
//     drive, stage by stage.
//   * `SmartPathfinder::search` (0x004138d0) hands a near goal to the grid
//     search and lays every other route as an A* over the areas, along their
//     seeds and connection runs, mended cell by cell where a run crosses
//     something blocked.
//
// Like the grid search, nothing here is hashed (`path.hpp`), and every route
// comes out of it, so it is a pure function of (grid, structure, request):
// integers throughout, the library heap's own order, no unordered container.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "imperivm/core/sim/grid_search.hpp"
#include "imperivm/core/sim/path.hpp"

namespace imperivm::core::sim {

/// An area id that names no area: an unassigned cell, an empty block.
inline constexpr std::uint16_t kNoArea = 0xFFFF;

/// The areas finder (0x0040fe00): a square of cells, each an area id and a
/// distance from that area's seed, cut over a copy of the grid taken when the
/// finder is made.
class AreaFinder {
 public:
  /// The growth's limit (0x00411af0 passes 0x30).
  static constexpr std::uint16_t kReach = 48;

  AreaFinder() = default;
  /// Copies `live`. The square's side is the grid's width.
  explicit AreaFinder(const ObstructionGrid& live);

  /// 0x00411af0: grow the next area and answer true; or, with no seed left,
  /// set every cell no area took blocked in `live` and answer false.
  bool step(ObstructionGrid& live);

  [[nodiscard]] std::int32_t size() const noexcept { return n_; }
  [[nodiscard]] std::uint16_t area(std::int32_t x, std::int32_t y) const noexcept {
    return static_cast<std::uint16_t>(at(x, y) & 0xFFFFu);
  }
  [[nodiscard]] std::uint16_t distance(std::int32_t x, std::int32_t y) const noexcept {
    return static_cast<std::uint16_t>(at(x, y) >> 16);
  }
  /// Each area's seed, in id order.
  [[nodiscard]] std::span<const Cell> seeds() const noexcept { return seeds_; }

 private:
  [[nodiscard]] std::uint32_t at(std::int32_t x, std::int32_t y) const noexcept {
    if (x < 0 || y < 0 || x >= n_ || y >= n_) return 0xFFFFFFFFu;
    return cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(n_) +
                  static_cast<std::size_t>(x)];
  }
  void set(std::int32_t x, std::int32_t y, std::uint32_t value) noexcept {
    cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(n_) +
           static_cast<std::size_t>(x)] = value;
  }
  [[nodiscard]] bool free(std::int32_t x, std::int32_t y) const noexcept {
    return !copy_.blocked_cell(x, y);
  }
  bool pick_seed(Cell& out);
  void grow(Cell seed, std::uint16_t id);

  ObstructionGrid copy_;
  std::vector<std::uint32_t> cells_;
  std::vector<Cell> candidates_;
  std::vector<Cell> seeds_;
  std::int32_t n_ = 0;
  std::int32_t scan_x_ = 3;
  std::int32_t scan_y_ = 3;
};

/// The structure (0x00416200 and what the build stages fill in).
class SmartPathfinder {
 public:
  /// The area search's budget (`[this+0x20] = 0x10000`, 0x00413690).
  static constexpr std::uint32_t kBudget = 0x10000;

  /// One entry of an area's list: the neighbour and the connection to it.
  struct Link {
    std::uint16_t area = 0;
    std::uint16_t connection = 0;
  };
  /// An area's record (44 bytes in the original): its seed's centre, and its
  /// neighbours in the order they were joined.
  struct Area {
    Point seed;
    std::vector<Link> links;
  };
  /// A connection (20 bytes): its cost, whether it is closed (`+2`: by a gate
  /// that bars the mover, for one search; see `gate_connections`), and its run
  /// from the lower id's seed to the higher's, the two seeds left out.
  struct Connection {
    std::uint16_t cost = 0;
    bool disabled = false;
    std::vector<Point> run;
  };

  /// 0x00416200 without its build flag: a record per seed, in seed order.
  SmartPathfinder(std::int32_t size, std::span<const Cell> seeds);

  /// 0x0040db30.
  void init_directions(const AreaFinder& finder);
  /// 0x00411c00.
  void init_point_to_area(const AreaFinder& finder);
  /// 0x00415620: join areas `a` and `b`, laying the run with `grid` over
  /// `live`.
  void join(std::uint32_t a, std::uint32_t b, GridSearch& grid, const ObstructionGrid& live);
  /// 0x00415e30: the pairs `SPFIncreaseConnect` will join, appended to `out`.
  void collect_pairs(std::vector<std::pair<std::uint32_t, std::uint32_t>>& out) const;

  /// 0x00412540: every connection whose route -- the lower id's seed, the
  /// run, the higher id's seed -- meets the segment `a`..`b` (a gate's axis)
  /// by 0x0040aab0, as `route_crossing` tests it; by each area in id order,
  /// then its list, each connection from its lower id. Appended to `out`.
  void gate_connections(Point a, Point b, std::vector<std::uint32_t>& out) const;
  /// Close (0x00414ae0) or open (0x00415950) the listed connections. The
  /// area search does not follow a closed one.
  void set_closed(std::span<const std::uint32_t> connections, bool closed) noexcept;

  /// 0x0040e340: the area under a world point, or `kNoArea` off the block
  /// map (see the definition).
  [[nodiscard]] std::uint32_t area_at(Point world) const noexcept;

  /// 0x004138d0. `goals` is the request's goal window (`request_goals`);
  /// `view` the live grid. Gates reach it as closed connections, never as
  /// cells (`PathFinder::close_gate`).
  Path search(GridSearch& grid, const CellView& view, const PathRequest& request,
              const GoalCells& goals);

  [[nodiscard]] std::span<const Area> areas() const noexcept { return areas_; }
  [[nodiscard]] std::span<const Connection> connections() const noexcept { return connections_; }
  [[nodiscard]] std::uint8_t direction(std::int32_t x, std::int32_t y) const noexcept;

 private:
  struct Node {
    std::uint32_t g = 0xFFFFFFFFu;
    std::uint32_t h = 0;
    std::uint32_t parent = 0xFFFFFFFFu;
    bool goal = false;
    bool joined = false;
  };

  /// 0x00413690. Answers the end area; `arrived` whether it is a goal.
  std::uint32_t area_search(std::uint32_t start, Point heading, bool& arrived);
  /// 0x00412040: the walk from `from` to `area`'s seed by the directions,
  /// appended to `out` forwards or reversed. False when it loops.
  bool walk(Point from, std::uint32_t area, bool reversed, std::vector<Point>& out) const;
  [[nodiscard]] std::uint32_t key(std::uint32_t n) const noexcept {
    return nodes_[n].g + nodes_[n].h;
  }
  void push(std::uint32_t n);
  std::uint32_t pop();
  void sift_up(std::size_t hole, std::size_t top, std::uint32_t value);

  std::int32_t n_ = 0;
  std::vector<Area> areas_;
  std::vector<Connection> connections_;
  /// Two bits a cell, one byte here: 0 left, 1 right, 2 up, 3 down.
  std::vector<std::uint8_t> directions_;
  std::vector<std::uint16_t> blocks_;
  std::int32_t blocks_w_ = 0;
  std::int32_t blocks_h_ = 0;
  std::vector<std::array<std::uint16_t, 16>> overflow_;

  std::vector<Node> nodes_;
  std::vector<std::uint32_t> heap_;
};

/// The builder (0x0040ff80): the structure being built, the areas finder,
/// the connect pass's range and row, and the pairs `SPFIncreaseConnect`
/// collected. Each method is one of the nine commands.
class SmartBuild {
 public:
  /// 0x004165a0's builder: the finder copies `live` now.
  explicit SmartBuild(const ObstructionGrid& live);

  /// `SPFFindAreas_Quant` (0x004129a0 -> 0x00411af0).
  bool find_areas(ObstructionGrid& live);
  /// `SPFInitData` (0x004166b0 -> 0x00416520).
  void init_data();
  /// `SPFInitDirectionData` (0x0040dd10).
  void init_directions();
  /// `SPFInitPointToAreaData` (0x00412980).
  void init_point_to_area();
  /// `SPFInitConnectData` (0x0040d1d0).
  void init_connect();
  /// `SPFCalcConnectHighRes_Quant` (0x00415a40).
  bool connect_row(GridSearch& grid, const ObstructionGrid& live);
  /// `SPFIncreaseConnect` (0x004161d0).
  void increase();
  /// `SPFIncreaseConnect_Quant` (0x00415a90 -> 0x004159b0).
  bool increase_step(GridSearch& grid, const ObstructionGrid& live);
  /// `SPFDone` (0x0040f8c0): the structure, for the caller to install. Null
  /// when `SPFInitData` never ran.
  std::unique_ptr<SmartPathfinder> finish();

  [[nodiscard]] const AreaFinder& finder() const noexcept { return finder_; }

 private:
  AreaFinder finder_;
  std::unique_ptr<SmartPathfinder> smart_;
  // The connect pass's range and row (`[builder+8..+0x18]`), as the builder
  // is made before `SPFInitConnectData` sets them.
  std::int32_t x0_ = 0;
  std::int32_t y0_ = 0;
  std::int32_t x1_ = -1;
  std::int32_t y1_ = -1;
  std::int32_t row_ = 0;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> pairs_;
  std::size_t next_pair_ = 0;
};

}  // namespace imperivm::core::sim
