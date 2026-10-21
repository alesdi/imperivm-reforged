// The grid search (`sim/grid_search.hpp`, docs/formats/pathfinder.md, "The
// grid search"): the straight line, the cell line's corner rule, the A*'s
// budget, the goal window's first slot, the owner's cut-back and the
// straightening. Every grid is built here.

#include <cstdint>
#include <functional>
#include <vector>

#include "imperivm/core/sim/grid_search.hpp"
#include "imperivm/core/sim/path.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

constexpr Point centre(std::int32_t cx, std::int32_t cy) { return Point{cx * 16 + 8, cy * 16 + 8}; }

/// A wall down column `x`, rows `from` to `to` inclusive.
void wall(ObstructionGrid& grid, std::int32_t x, std::int32_t from, std::int32_t to) {
  for (std::int32_t y = from; y <= to; ++y) grid.set_cell(x, y, true);
}

/// An owner whose free spots a test chooses.
class Spots final : public SpotTest {
 public:
  Spots(std::int32_t radius, std::function<bool(Point)> free) : radius_(radius), free_(free) {}
  [[nodiscard]] bool free(Point at) const override { return free_(at); }
  [[nodiscard]] std::int32_t radius() const override { return radius_; }

 private:
  std::int32_t radius_;
  std::function<bool(Point)> free_;
};

/// Whether every leg of a route is a clear cell line, walked as the live
/// straightening tests it: from the leg's far end back to its near one. The
/// cell line is not symmetric -- where a step ties, the two directions pick
/// different cells -- so a leg may graze a wall's corner the other way.
bool legs_clear(const ObstructionGrid& grid, const std::vector<Point>& route) {
  const CellView view{&grid};
  for (std::size_t i = 1; i < route.size(); ++i) {
    LineState state;
    if (!line_clear(view, Cell{route[i].x / 16, route[i].y / 16},
                    Cell{route[i - 1].x / 16, route[i - 1].y / 16}, state)) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST(the_straight_line_is_the_route_when_its_cells_are_clear) {
  // 0x0040b580 before any A*: nothing popped, start then goal.
  const ObstructionGrid grid(32, 32);
  PathFinder finder;
  PathRequest request;
  request.start = Point{40, 40};
  request.goal = Point{330, 151};
  const Path path = finder.find(grid, request);
  CHECK(path.status == PathStatus::found);
  REQUIRE(path.waypoints.size() == 2);
  CHECK(path.waypoints.front() == request.start);
  CHECK(path.waypoints.back() == request.goal);
  CHECK(path.expanded == 0);
}

TEST(a_diagonal_step_between_two_blocked_corners_is_not_clear) {
  // 0x0040b1b0: a diagonal step is stopped only when both corners it passes
  // are blocked; one free corner lets it through.
  ObstructionGrid grid(8, 8);
  grid.set_cell(3, 2, true);
  grid.set_cell(2, 3, true);
  const CellView view{&grid};
  LineState shut;
  CHECK(!line_clear(view, Cell{2, 2}, Cell{3, 3}, shut));
  CHECK(shut.hit == (Cell{3, 3}));
  grid.set_cell(2, 3, false);
  LineState open;
  CHECK(line_clear(view, Cell{2, 2}, Cell{3, 3}, open));
  // The first cell is never a diagonal step: the line starts from (-100, -100).
  grid.set_cell(2, 3, true);
  LineState first;
  CHECK(line_clear(view, Cell{3, 3}, Cell{3, 3}, first));
}

TEST(the_search_gives_up_on_the_eight_thousand_and_first_cell) {
  // 0x0041b140's budget: 8,000, counted down after each pop, ends the search
  // when it was already 0 -- on the 8,001st cell. The goal is sealed off
  // behind a wall with 30,000 cells on the start's side; the answer is status
  // 2, ended at the centre of the popped cell with the smallest estimate.
  ObstructionGrid grid(200, 200);
  wall(grid, 150, 0, 199);
  PathFinder finder;
  PathRequest request;
  request.start = centre(10, 100);
  request.goal = centre(180, 100);
  const Path path = finder.find(grid, request);
  CHECK(path.status == PathStatus::partial);
  CHECK(path.expanded == GridSearch::kBudget + 1);
  REQUIRE(path.waypoints.size() >= 2);
  CHECK(path.waypoints.back().x % 16 == 8);
  CHECK(path.waypoints.back().y % 16 == 8);
  CHECK(path.waypoints.back().x < 150 * 16);
  CHECK(legs_clear(grid, path.waypoints));
}

TEST(a_goal_cell_whose_first_point_is_struck_is_no_goal_cell) {
  // 0x0041b140 reads a cell's first registered point: struck there, the
  // cell ends no search, whatever else is registered in it. The live list
  // keeps the order points were laid in.
  const GoalPoint struck_first[] = {{Point{100, 100}, false}, {Point{104, 104}, true}};
  GoalCells goals;
  goals.assign(struck_first);
  Point at{};
  CHECK(!goals.at(Cell{6, 6}, at));
  REQUIRE(goals.live().size() == 1);
  CHECK(goals.live().front() == (Point{104, 104}));

  const GoalPoint live_first[] = {{Point{104, 104}, true}, {Point{100, 100}, false}};
  goals.assign(live_first);
  CHECK(goals.at(Cell{6, 6}, at));
  CHECK(at == (Point{104, 104}));
}

TEST(a_fifth_point_in_one_cell_is_not_registered) {
  // 0x00409b80 keeps four points to a cell; the fifth is dropped outright,
  // from the straight line's list too.
  const GoalPoint five[] = {{Point{100, 100}, true},
                            {Point{101, 100}, true},
                            {Point{102, 100}, true},
                            {Point{103, 100}, true},
                            {Point{104, 100}, true}};
  GoalCells goals;
  goals.assign(five);
  CHECK(goals.live().size() == 4);
}

TEST(an_owners_unfinished_route_is_cut_back_to_a_free_spot) {
  // 0x0041ba00: a route that does not arrive, for an owner, ends at the
  // centre of the first cell back along the way that is a free spot
  // (0x0040a990) -- here, the first west of x = 200.
  ObstructionGrid grid(64, 64);
  wall(grid, 32, 0, 63);
  const Spots owner(15, [](Point p) { return p.x < 200; });
  PathFinder finder;
  PathRequest request;
  request.start = centre(5, 30);
  request.goal = centre(60, 30);
  request.owner = &owner;
  const Path path = finder.find(grid, request);
  CHECK(path.status == PathStatus::partial);
  CHECK(path.cut);
  REQUIRE(path.waypoints.size() >= 2);
  CHECK(path.waypoints.back() == centre(11, 30));

  // Nowhere free: the route ends where it began, and goes nowhere.
  const Spots taken(15, [](Point) { return false; });
  request.owner = &taken;
  const Path stay = finder.find(grid, request);
  CHECK(stay.cut);
  CHECK(stay.waypoints.size() == 1);
  CHECK(stay.length == 0);
}

TEST(an_owner_with_no_goal_point_left_walks_back_from_the_heading) {
  // 0x0040b580 with every goal point struck and the way open: from where
  // the search heads, back towards the start in steps of the owner's radius,
  // the first free spot is the route's end.
  const ObstructionGrid grid(64, 64);
  const GoalPoint struck[] = {{Point{500, 100}, false}};
  const Spots owner(20, [](Point p) { return p.x <= 400; });
  PathFinder finder;
  PathRequest request;
  request.start = Point{100, 100};
  request.goal = Point{500, 100};
  request.goal_points = struck;
  request.goal_points_given = true;
  request.owner = &owner;
  const Path path = finder.find(grid, request);
  CHECK(path.status == PathStatus::found);
  REQUIRE(path.waypoints.size() == 2);
  CHECK(path.waypoints.back() == (Point{400, 100}));

  // None free on the way: status 1, a route that goes nowhere.
  const Spots taken(20, [](Point) { return false; });
  request.owner = &taken;
  const Path none = finder.find(grid, request);
  CHECK(none.status == PathStatus::no_goal);
  CHECK(none.cut);
  CHECK(none.waypoints.size() == 1);
}

TEST(a_search_with_no_goal_point_left_answers_status_one) {
  // 0x0041bc60: the A* heads for `[0x008bfc10]` and, with no live goal
  // point, cannot arrive; the status is 1 whatever it reached.
  ObstructionGrid grid(64, 64);
  wall(grid, 32, 0, 59);
  const GoalPoint struck[] = {{centre(60, 5), false}};
  PathFinder finder;
  PathRequest request;
  request.start = centre(5, 5);
  request.goal = centre(60, 5);
  request.goal_points = struck;
  request.goal_points_given = true;
  const Path path = finder.find(grid, request);
  CHECK(path.status == PathStatus::no_goal);
  REQUIRE(path.waypoints.size() >= 2);
  CHECK(path.waypoints.back() == centre(60, 5));
}

TEST(a_route_round_a_wall_is_straightened_to_a_few_clear_legs) {
  // 0x0040c6b0 turns the cell chain into points each of which sees the
  // next by a clear cell line.
  ObstructionGrid grid(64, 64);
  wall(grid, 32, 0, 59);
  PathFinder finder;
  PathRequest request;
  request.start = centre(5, 5);
  request.goal = centre(60, 5);
  const Path path = finder.find(grid, request);
  CHECK(path.status == PathStatus::found);
  REQUIRE(path.waypoints.size() >= 3);
  CHECK(path.waypoints.size() <= 6);
  CHECK(path.waypoints.front() == request.start);
  CHECK(path.waypoints.back() == request.goal);
  CHECK(legs_clear(grid, path.waypoints));
}

TEST(a_route_of_no_length_is_no_route) {
  // 0x0041bc60 answers whether the length is non-zero: a unit on its goal
  // has nowhere to go.
  const ObstructionGrid grid(16, 16);
  PathFinder finder;
  PathRequest request;
  request.start = Point{40, 40};
  request.goal = Point{40, 40};
  const Path path = finder.find(grid, request);
  CHECK(path.status == PathStatus::arrived);
  CHECK(path.waypoints.size() == 1);
}
