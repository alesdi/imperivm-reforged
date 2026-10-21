#pragma once

// The original's grid search, the one route search `gbr.exe` has before its
// smart pathfinder is built and the one that pathfinder hands a near goal to.
//
// Specification: docs/formats/pathfinder.md, "The grid search". Everything
// here is written from that prose; the addresses are there so each rule can
// be checked against the executable.
//
// In outline (0x0041bc60):
//
//   1. **A straight line first** (0x0040b580): to the live goal point nearest
//      the start, or to where the search heads when none is left. A cell line
//      (`walk_cells`, 0x0040b2b0) that meets no blocked cell -- and no
//      diagonal step between two blocked corners (0x0040b1b0) -- is the
//      route.
//   2. **Otherwise A\*** (0x0041ba00 -> 0x0041b140) over the grid's cells,
//      four-connected, step 2.0 and estimate twice the straight distance to
//      the heading's cell, both in 16.16; a binary heap in the shape of the
//      C++ library the original was built with, keyed on the cost so far
//      plus the estimate as they stand when it is compared; **8,000 cells
//      popped at most**. The first popped cell that holds a live goal point
//      ends it there. Otherwise it answers the expanded cell with the
//      smallest estimate.
//   3. An owner's unfinished route is cut back along its cells to the first
//      whose centre is a free spot; the cell chain cuts each L-shaped corner
//      to a diagonal; and a straightening pass (0x0040c6b0) turns the chain
//      into the route's points.
//
// Nothing here is hashed (`path.hpp`, "Why the pathfinder may not be
// hashed"), but every route a unit walks comes out of it, so it is a pure
// function of (grid, request): integers throughout, the heap's own order,
// and no unordered container.

#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/sim/path.hpp"

namespace imperivm::core::sim {

/// What the grid search sees as blocked: the grid, a request's barrier on top
/// of it, and the start's cell lifted for the search (0x0041bc60 clears the
/// start's bit before 0x0041ba00 and restores it after the route is laid).
struct CellView {
  const ObstructionGrid* grid = nullptr;
  const CellOverlay* barrier = nullptr;
  /// The start's cell, free for this search whatever the grid says. `open`
  /// says whether it is in force.
  std::int32_t open_x = 0;
  std::int32_t open_y = 0;
  bool open = false;

  [[nodiscard]] bool blocked(std::int32_t cx, std::int32_t cy) const noexcept {
    if (open && cx == open_x && cy == open_y) return false;
    if (grid->blocked_cell(cx, cy)) return true;
    return barrier != nullptr && barrier->contains(cx, cy);
  }
};

/// A cell of the grid.
struct Cell {
  std::int32_t x = 0;
  std::int32_t y = 0;
  friend constexpr bool operator==(Cell, Cell) noexcept = default;
};

/// The state a cell line carries from cell to cell (0x0040b1b0's record):
/// whether it is still clear, where it was stopped, and the cell before.
/// A fresh one starts clear with the previous cell at (-100, -100), so the
/// first cell never counts as a diagonal step.
struct LineState {
  bool clear = true;
  Cell hit{};
  Cell prev{-100, -100};
};

/// 0x0040b2b0: walk the cells from `(x0, y0)` to `(x1, y1)`, the original's
/// Bresenham in its eight octants, calling `visit(x, y)` on each until it
/// answers false. Declared here so the smart pathfinder's line walks
/// (0x0040c3f0, 0x00412a70) can share it.
template <typename Visit>
void walk_cells(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, Visit&& visit);

/// 0x0040b2b0 with 0x0040b1b0's test: whether the cell line from `a` to `b`
/// is clear, continuing `state`.
bool line_clear(const CellView& view, Cell a, Cell b, LineState& state);

/// The goal points of one request, laid out the way the original's goal
/// window holds them (0x00409b80): each in its own cell, at most four to a
/// cell -- a fifth is not registered at all -- and a cell counts as a goal
/// cell when the **first** point registered in it is still live.
class GoalCells {
 public:
  void assign(std::span<const GoalPoint> points);
  /// The live points, in the order they were registered (`[0x008c517c]`).
  [[nodiscard]] const std::vector<Point>& live() const noexcept { return live_; }
  /// The goal point a popped cell ends the search at, if it is a goal cell.
  [[nodiscard]] bool at(Cell cell, Point& out) const noexcept;

 private:
  struct Entry {
    std::uint64_t key = 0;
    Point first{};
    bool live = false;
    std::uint8_t count = 0;
  };
  std::vector<Entry> cells_;  ///< sorted by key
  std::vector<Point> live_;
  Cell min_{0, 0};
  Cell max_{-1, -1};
};

/// The grid search (0x0041bc60 and what it calls). Holds its node arrays
/// between searches and stamps them with a generation instead of clearing
/// them, which changes nothing a search answers.
class GridSearch {
 public:
  /// The original's budget: cells popped before it gives up (`[this+0x20]`,
  /// set at 0x0041b155).
  static constexpr std::uint32_t kBudget = 8000;

  /// One search over `view` (whose `grid` is set; `open` is managed here).
  /// `both_ways` is the straightening's flag (`[this+0x18]`): 0 for the
  /// live grid search, 1 for the one the smart pathfinder's build lays its
  /// connections with (0x00416200).
  Path run(CellView view, const PathRequest& request, const GoalCells& goals, bool both_ways);

  /// 0x0040b580. True when it laid the route (`out` filled). The smart
  /// pathfinder tries it too (0x004138d0).
  bool straight_line(const CellView& view, const PathRequest& request, const GoalCells& goals,
                     Path& out) const;

  /// 0x0041ba00 with no owner, as the smart pathfinder's patch calls it
  /// (0x00412260): the A* alone from `start` towards `heading`, and the cell
  /// chain it ends at, start first and corners cut, in `out`.
  void chain(const CellView& view, Cell start, Point heading, const GoalCells& goals,
             std::vector<Cell>& out);

  /// Cells popped since construction. A diagnostic.
  [[nodiscard]] std::uint64_t total_expanded() const noexcept { return total_expanded_; }

 private:
  /// The chain from node `node` back to the start, corners cut, reversed.
  void build_chain(std::uint32_t node, std::vector<Cell>& out) const;
  /// 0x0041b140: the A\* proper. Answers the end node and whether it is a
  /// goal cell; `goal_point` is the goal point it ends at when it is.
  std::uint32_t astar(const CellView& view, Cell start, Point heading, const GoalCells& goals,
                      bool& found, Point& goal_point, std::uint32_t& popped);

  void reset(std::int32_t width, std::int32_t height);
  [[nodiscard]] std::uint32_t g(std::uint32_t n) const noexcept {
    return gen_[n] == generation_ ? g_[n] : 0xFFFFFFFFu;
  }
  [[nodiscard]] std::uint32_t h(std::uint32_t n) const noexcept {
    return gen_[n] == generation_ ? h_[n] : 0u;
  }
  void touch(std::uint32_t n) noexcept;
  [[nodiscard]] std::uint32_t key(std::uint32_t n) const noexcept { return g(n) + h(n); }
  void push(std::uint32_t n);
  std::uint32_t pop();
  void sift_up(std::size_t hole, std::size_t top, std::uint32_t value);

  std::vector<std::uint32_t> g_;
  std::vector<std::uint32_t> h_;
  std::vector<std::uint32_t> parent_;
  std::vector<std::uint32_t> gen_;
  std::vector<std::uint32_t> heap_;
  std::vector<Cell> chain_;
  std::int32_t width_ = 0;
  std::int32_t height_ = 0;
  std::uint32_t generation_ = 0;
  std::uint64_t total_expanded_ = 0;
};

/// 0x0040c6b0: the cell chain `cells`, start first, as route points, each the
/// centre of its cell, appended to `out`. See the definition.
void straighten(const CellView& view, std::span<const Cell> cells, bool both_ways,
                std::vector<Point>& out);

// --------------------------------------------------------------------------

template <typename Visit>
void walk_cells(std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                Visit&& visit) {
  // Each octant as the original lays it out: the error term starts and moves
  // as below, the minor axis steps when the term is below 1, and the walk
  // ends once the major axis passes the end. Two cells never differ on both
  // axes by more than one.
  const std::int32_t dx = x1 - x0;
  const std::int32_t dy = y1 - y0;
  const std::int32_t adx = dx < 0 ? -dx : dx;
  const std::int32_t ady = dy < 0 ? -dy : dy;
  std::int32_t x = x0;
  std::int32_t y = y0;
  if (ady < adx) {
    if (dx < 1) {
      if (dy < 1) {
        std::int32_t e = 2 * dy - dx;
        while (visit(x, y)) {
          std::int32_t inc = dy;
          if (e < 1) {
            --y;
            inc = dy - dx;
          }
          e += 2 * inc;
          if (--x < x1) return;
        }
      } else {
        std::int32_t e = -(dx + 2 * dy);
        while (visit(x, y)) {
          std::int32_t inc = dy;
          if (e < 1) {
            ++y;
            inc = dy + dx;
          }
          e -= 2 * inc;
          if (--x < x1) return;
        }
      }
    } else if (dy < 0) {
      std::int32_t e = dx + 2 * dy;
      while (visit(x, y)) {
        std::int32_t inc = dy;
        if (e < 1) {
          --y;
          inc = dy + dx;
        }
        e += 2 * inc;
        if (++x > x1) return;
      }
    } else {
      std::int32_t e = dx - 2 * dy;
      while (visit(x, y)) {
        if (e < 1) {
          ++y;
          e += 2 * (dx - dy);
        } else {
          e -= 2 * dy;
        }
        if (++x > x1) return;
      }
    }
    return;
  }
  if (dy < 1) {
    if (dx < 0) {
      std::int32_t e = 2 * dx - dy;
      while (visit(x, y)) {
        std::int32_t inc = dx;
        if (e < 1) {
          --x;
          inc = dx - dy;
        }
        e += 2 * inc;
        if (--y < y1) return;
      }
    } else {
      std::int32_t e = -(dy + 2 * dx);
      while (y1 <= y && visit(x, y)) {
        std::int32_t inc = dx;
        if (e < 1) {
          ++x;
          inc = dy + dx;
        }
        e -= 2 * inc;
        --y;
      }
    }
  } else if (dx < 0) {
    std::int32_t e = dy + 2 * dx;
    while (visit(x, y)) {
      std::int32_t inc = dx;
      if (e < 1) {
        --x;
        inc = dy + dx;
      }
      e += 2 * inc;
      if (++y > y1) return;
    }
  } else {
    std::int32_t e = dy - 2 * dx;
    while (visit(x, y)) {
      if (e < 1) {
        ++x;
        e += 2 * (dy - dx);
      } else {
        e -= 2 * dx;
      }
      if (++y > y1) return;
    }
  }
}

}  // namespace imperivm::core::sim
