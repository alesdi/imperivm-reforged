// The smart pathfinder (`sim/smart_path.hpp`, docs/formats/pathfinder.md,
// "The smart pathfinder: the build" and "... a search"): the areas, the
// cells no area takes, the block map, the connections, the far route the
// grid search's budget cannot reach, the near goal handed back, and the
// build made again from its calls. Every grid is built here.

#include <cstdint>
#include <vector>

#include "imperivm/core/sim/grid_search.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/smart_path.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

constexpr Point centre(std::int32_t cx, std::int32_t cy) { return Point{cx * 16 + 8, cy * 16 + 8}; }

/// The script's whole run, every command in its order, as
/// `DATA\PATHFINDERINIT.VS` makes them.
void build_all(PathFinder& finder, ObstructionGrid& grid) {
  finder.start_smart_build(grid);
  while (finder.smart_build(SpfCall::find_areas, grid)) {
  }
  (void)finder.smart_build(SpfCall::init_data, grid);
  (void)finder.smart_build(SpfCall::init_direction, grid);
  (void)finder.smart_build(SpfCall::init_point_to_area, grid);
  (void)finder.smart_build(SpfCall::init_connect, grid);
  while (finder.smart_build(SpfCall::connect_row, grid)) {
  }
  (void)finder.smart_build(SpfCall::increase, grid);
  while (finder.smart_build(SpfCall::increase_step, grid)) {
  }
  (void)finder.smart_build(SpfCall::done, grid);
}

/// Whether every leg is a clear cell line, from its far end back to its
/// near one, as the live straightening tests it.
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

/// A long wall down column 80 from the top to row 150 of a 160-cell map,
/// open below: a far route has to go round it.
ObstructionGrid long_wall() {
  ObstructionGrid grid(160, 160);
  for (std::int32_t y = 0; y <= 150; ++y) grid.set_cell(80, y, true);
  return grid;
}

}  // namespace

TEST(the_first_area_grows_from_the_lattice_and_takes_its_whole_block) {
  // 0x00410fd0: no candidate yet, so the lattice's first free cell, (3, 3).
  // 0x00411110: inside a 4x4 block every free cell is taken.
  const ObstructionGrid open(64, 64);
  ObstructionGrid live = open;
  AreaFinder finder(open);
  REQUIRE(finder.step(live));
  REQUIRE(finder.seeds().size() == 1);
  CHECK(finder.seeds()[0] == (Cell{3, 3}));
  CHECK(finder.area(3, 3) == 0);
  CHECK(finder.distance(3, 3) == 0);
  CHECK(finder.area(4, 3) == 0);
  CHECK(finder.distance(4, 3) == 1);
  // Across block edges only within 48 steps: (3+30, 3+18) is 48 away and
  // taken, while (60, 60) is not.
  CHECK(finder.area(33, 21) == 0);
  CHECK(finder.distance(33, 21) == 48);
  CHECK(finder.area(60, 60) == kNoArea);
}

TEST(every_free_cell_ends_in_an_area_or_is_blocked) {
  // When no seed is left (0x00411af0), the cells no area took are set
  // blocked in the live grid: here a sealed pocket with no lattice cell.
  ObstructionGrid grid(32, 32);
  // A ring round the single cell (9, 9); the lattice is x, y = 3, 7, 11, ...
  for (std::int32_t x = 8; x <= 10; ++x) {
    grid.set_cell(x, 8, true);
    grid.set_cell(x, 10, true);
  }
  grid.set_cell(8, 9, true);
  grid.set_cell(10, 9, true);
  ObstructionGrid live = grid;
  AreaFinder finder(grid);
  while (finder.step(live)) {
  }
  CHECK(finder.area(9, 9) == kNoArea);
  CHECK(live.blocked_cell(9, 9));
  CHECK(!grid.blocked_cell(9, 9));
  for (std::int32_t y = 0; y < 32; ++y) {
    for (std::int32_t x = 0; x < 32; ++x) {
      if (grid.blocked_cell(x, y) || (x == 9 && y == 9)) continue;
      CHECK(finder.area(x, y) != kNoArea);
      CHECK(!live.blocked_cell(x, y));
    }
  }
}

TEST(a_pocket_with_a_lattice_cell_is_an_area_of_its_own) {
  // A sealed 3x3 room round the lattice cell (7, 7) gets its own seed.
  ObstructionGrid grid(32, 32);
  for (std::int32_t i = 5; i <= 9; ++i) {
    grid.set_cell(i, 5, true);
    grid.set_cell(i, 9, true);
    grid.set_cell(5, i, true);
    grid.set_cell(9, i, true);
  }
  ObstructionGrid live = grid;
  AreaFinder finder(grid);
  while (finder.step(live)) {
  }
  const std::uint16_t room = finder.area(7, 7);
  REQUIRE(room != kNoArea);
  CHECK(finder.area(6, 6) == room);
  CHECK(finder.area(3, 3) != room);
  CHECK(!live.blocked_cell(7, 7));
}

TEST(the_block_map_answers_each_cells_area) {
  // 0x00411c00 and 0x0040e340: through the block, or its overflow table
  // where two areas share it -- for every cell from 2 to n - 3.
  ObstructionGrid grid = long_wall();
  ObstructionGrid live = grid;
  AreaFinder finder(grid);
  while (finder.step(live)) {
  }
  SmartPathfinder smart(finder.size(), finder.seeds());
  smart.init_point_to_area(finder);
  REQUIRE(finder.seeds().size() > 1);
  for (std::int32_t y = 2; y < 158; ++y) {
    for (std::int32_t x = 2; x < 158; ++x) {
      if (finder.area(x, y) == kNoArea) continue;
      CHECK(smart.area_at(centre(x, y)) == finder.area(x, y));
    }
  }
  // Off the map's two-cell margin it is no area.
  CHECK(smart.area_at(centre(1, 40)) == kNoArea);
}

TEST(the_directions_lead_every_cell_of_an_open_map_to_its_seed) {
  // 0x0040db30: on an open map one area wide -- every cell within 48 steps
  // of (3, 3) -- each cell's direction is a neighbour one step nearer the
  // seed, so following them always ends there.
  ObstructionGrid grid(24, 24);
  ObstructionGrid live = grid;
  AreaFinder finder(grid);
  while (finder.step(live)) {
  }
  SmartPathfinder smart(finder.size(), finder.seeds());
  smart.init_directions(finder);
  REQUIRE(finder.seeds().size() == 1);
  for (std::int32_t y = 2; y < 22; ++y) {
    for (std::int32_t x = 2; x < 22; ++x) {
      const std::uint16_t a = finder.area(x, y);
      const std::uint16_t d = finder.distance(x, y);
      if (d == 0) continue;
      std::int32_t nx = x;
      std::int32_t ny = y;
      switch (smart.direction(x, y)) {
        case 0: --nx; break;
        case 1: ++nx; break;
        case 2: --ny; break;
        default: ++ny; break;
      }
      CHECK(finder.area(nx, ny) == a);
      CHECK(finder.distance(nx, ny) + 1 == d);
    }
  }
}

TEST(touching_areas_are_joined_by_a_run_between_their_seeds) {
  // 0x00415620: both lists carry the pair, the cost is the run's length, and
  // the run leaves out the two seeds.
  ObstructionGrid grid = long_wall();
  PathFinder finder;
  build_all(finder, grid);
  REQUIRE(finder.smart_installed());
  const SmartPathfinder& smart = *finder.smart();
  REQUIRE(!smart.connections().empty());
  for (std::uint32_t a = 0; a < smart.areas().size(); ++a) {
    for (const SmartPathfinder::Link& link : smart.areas()[a].links) {
      bool back = false;
      for (const SmartPathfinder::Link& other : smart.areas()[link.area].links) {
        back = back || (other.area == a && other.connection == link.connection);
      }
      CHECK(back);
      const SmartPathfinder::Connection& c = smart.connections()[link.connection];
      std::vector<Point> whole;
      const std::uint32_t lo = a < link.area ? a : link.area;
      const std::uint32_t hi = a < link.area ? link.area : a;
      whole.push_back(smart.areas()[lo].seed);
      whole.insert(whole.end(), c.run.begin(), c.run.end());
      whole.push_back(smart.areas()[hi].seed);
      CHECK(static_cast<std::int64_t>(c.cost) == polyline_length(whole));
    }
  }
}

TEST(a_far_route_round_a_long_wall_arrives_where_the_grid_search_runs_out) {
  // The grid search's 8,000 cells do not reach round the wall; the area
  // search does, and the route it lays crosses nothing.
  ObstructionGrid grid = long_wall();
  PathRequest request;
  request.start = centre(20, 10);
  request.goal = centre(140, 10);

  PathFinder plain;
  const Path before = plain.find(grid, request);
  CHECK(before.status == PathStatus::partial);

  PathFinder finder;
  build_all(finder, grid);
  REQUIRE(finder.smart_installed());
  const Path after = finder.find(grid, request);
  CHECK(after.status == PathStatus::found);
  REQUIRE(after.waypoints.size() >= 3);
  CHECK(after.waypoints.front() == request.start);
  CHECK(after.waypoints.back() == request.goal);
  CHECK(legs_clear(grid, after.waypoints));
  // It goes round the bottom of the wall.
  bool below = false;
  for (const Point& p : after.waypoints) below = below || p.y >= 151 * 16;
  CHECK(below);
}

TEST(a_gate_closes_the_connections_that_cross_it_for_one_search) {
  // 0x00414ae0 over 0x00412540: a gate across the only way round the wall
  // closes every connection whose route meets its axis, and the area search
  // no longer arrives; 0x00415950 opens them again. The grid is not touched.
  ObstructionGrid grid = long_wall();
  PathFinder finder;
  build_all(finder, grid);
  REQUIRE(finder.smart_installed());
  const ObstructionGrid before = grid;
  PathRequest request;
  request.start = centre(20, 10);
  request.goal = centre(140, 10);

  const Point a{80 * 16 + 8, 150 * 16};
  const Point b{80 * 16 + 8, 160 * 16};
  std::vector<std::uint32_t> listed;
  finder.smart()->gate_connections(a, b, listed);
  REQUIRE(!listed.empty());
  // Each listed connection runs from one side of the gate to the other.
  const auto areas = finder.smart()->areas();
  for (const std::uint32_t c : listed) {
    bool found = false;
    for (std::size_t i = 0; i < areas.size() && !found; ++i) {
      for (const SmartPathfinder::Link& link : areas[i].links) {
        if (link.connection != c) continue;
        found = true;
        const bool left = areas[i].seed.x < a.x;
        const bool other_left = areas[link.area].seed.x < a.x;
        CHECK(left != other_left);
      }
    }
    CHECK(found);
  }

  CHECK(finder.close_gate(7, a, b) == listed.size());
  const Path barred = finder.find(grid, request);
  CHECK(barred.status != PathStatus::found);
  CHECK(grid.blocked_cell(80, 155) == before.blocked_cell(80, 155));
  finder.open_gates();
  for (const SmartPathfinder::Connection& c : finder.smart()->connections()) CHECK(!c.disabled);
  CHECK(finder.find(grid, request).status == PathStatus::found);
  // Before the smart pathfinder answers, a gate closes nothing here: the
  // grid search is handed its cells instead.
  PathFinder plain;
  CHECK(plain.close_gate(7, a, b) == 0);
}

TEST(a_goal_in_the_starts_area_goes_to_the_grid_search) {
  // 0x004138d0: a goal point in an area joined to the start's, the start
  // unmoved, is the grid search's -- the same route it lays alone.
  ObstructionGrid grid(64, 64);
  for (std::int32_t y = 4; y <= 12; ++y) grid.set_cell(10, y, true);
  PathRequest request;
  request.start = centre(6, 8);
  request.goal = centre(14, 8);
  PathFinder plain;
  const Path alone = plain.find(grid, request);
  PathFinder finder;
  build_all(finder, grid);
  const Path near = finder.find(grid, request);
  CHECK(near.status == alone.status);
  CHECK(near.waypoints == alone.waypoints);
  CHECK(near.expanded == alone.expanded);
  CHECK(near.expanded > 0);
}

TEST(a_walled_in_start_moves_to_the_first_free_probe) {
  // 0x0040dc90's probes: 16 down first. The route starts from the start
  // itself all the same.
  ObstructionGrid grid = long_wall();
  PathFinder finder;
  build_all(finder, grid);
  PathRequest request;
  request.start = centre(80, 40);  // in the wall
  request.goal = centre(140, 100);
  const Path path = finder.find(grid, request);
  REQUIRE(path.usable());
  CHECK(path.waypoints.front() == request.start);
  CHECK(path.waypoints.back() == request.goal);

  // Walled in all round, it answers no route: the start twice, status 2.
  ObstructionGrid shut(64, 64);
  for (std::int32_t y = 20; y <= 30; ++y) {
    for (std::int32_t x = 20; x <= 30; ++x) shut.set_cell(x, y, true);
  }
  PathFinder sealed;
  build_all(sealed, shut);
  PathRequest inside;
  inside.start = centre(25, 25);
  inside.goal = centre(50, 50);
  const Path none = sealed.find(shut, inside);
  CHECK(none.status == PathStatus::partial);
  CHECK(!none.usable());
}

TEST(the_build_is_made_again_from_its_calls) {
  // What a save keeps: the calls. Made again over the map's grid they give
  // the same structure, the same blocked cells and the same routes.
  const ObstructionGrid map = long_wall();
  ObstructionGrid grid = map;
  PathFinder finder;
  build_all(finder, grid);

  ObstructionGrid again_grid = map;
  PathFinder again;
  again.replay_smart(finder.smart_started(), finder.smart_calls(), again_grid);
  REQUIRE(again.smart_installed());
  CHECK(again.smart_calls() == finder.smart_calls());
  CHECK(again.smart()->areas().size() == finder.smart()->areas().size());
  CHECK(again.smart()->connections().size() == finder.smart()->connections().size());
  for (std::int32_t y = 0; y < 160; ++y) {
    for (std::int32_t x = 0; x < 160; ++x) {
      CHECK(again_grid.blocked_cell(x, y) == grid.blocked_cell(x, y));
    }
  }
  PathRequest request;
  request.start = centre(20, 10);
  request.goal = centre(140, 10);
  CHECK(again.find(again_grid, request).waypoints == finder.find(grid, request).waypoints);

  // Part of the way: the builder stands where the calls left it.
  ObstructionGrid part_grid = map;
  PathFinder part;
  part.start_smart_build(part_grid);
  (void)part.smart_build(SpfCall::find_areas, part_grid);
  (void)part.smart_build(SpfCall::find_areas, part_grid);
  ObstructionGrid part_again_grid = map;
  PathFinder part_again;
  part_again.replay_smart(true, part.smart_calls(), part_again_grid);
  CHECK(part_again.smart_building());
  CHECK(!part_again.smart_installed());
  CHECK(part_again.smart_calls().size() == 2);
}

TEST(the_movement_section_carries_the_build) {
  // Saved as the calls (system section 43); a load into a system whose grid
  // is the map's makes the structure again.
  MovementSystem movement;
  movement.set_grid(long_wall());
  movement.start_smart_build();
  while (movement.smart_build(SpfCall::find_areas)) {
  }
  (void)movement.smart_build(SpfCall::init_data);
  std::vector<std::byte> bytes;
  movement.serialize(bytes);

  MovementSystem loaded;
  loaded.set_grid(long_wall());
  REQUIRE(loaded.deserialize(bytes).ok());
  CHECK(loaded.pathfinder().smart_building());
  CHECK(loaded.pathfinder().smart_calls() == movement.pathfinder().smart_calls());
  CHECK(loaded.grid_generation() == movement.grid_generation());

  // And the rest of the build goes on from there.
  (void)loaded.smart_build(SpfCall::init_direction);
  (void)loaded.smart_build(SpfCall::init_point_to_area);
  (void)loaded.smart_build(SpfCall::init_connect);
  while (loaded.smart_build(SpfCall::connect_row)) {
  }
  (void)loaded.smart_build(SpfCall::done);
  CHECK(loaded.pathfinder().smart_installed());
}

TEST(with_no_builder_the_commands_do_nothing) {
  // The original reads its builder without a test; here a command with none
  // answers false and changes nothing.
  ObstructionGrid grid(32, 32);
  PathFinder finder;
  CHECK(!finder.smart_build(SpfCall::find_areas, grid));
  CHECK(!finder.smart_build(SpfCall::done, grid));
  CHECK(!finder.smart_installed());
  CHECK(finder.smart_calls().empty());
}
