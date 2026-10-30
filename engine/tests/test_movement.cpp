// Movement, pathfinding and formations.
//
// No game data anywhere in this file. Every grid is built here, every
// formation table is a string literal written for the test, and every speed is
// a number chosen so the expected position can be worked out by hand. A test
// that needs an installation is a test CI cannot run.
//
// The determinism cases are the point of the file. Two of them are worth
// naming:
//
//   * **Partition invariance.** One turn of 800 must leave a unit on exactly
//     the same coordinate as two of 400 or four of 200 -- not near it, on it.
//     That is the property that makes the lockstep turn length safe to
//     renegotiate, and it holds because movement accumulates an exact integer
//     product and never adds a rounded step to a position.
//   * **Pathfinder reproducibility.** A fresh searcher and one whose scratch
//     arrays have been dirtied by other searches must return byte-identical
//     routes, because the open set is ordered by a total order rather than by
//     insertion.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/game/localization.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/grid_search.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;
using namespace imperivm::core::script;

namespace {

// -- building grids ---------------------------------------------------------

/// A `GRID` container in the one-bit, 16-unit configuration every `.pass` file
/// and every map's `Terrain.pass.grid` uses. Built here so that the reader is
/// exercised on the shape it will actually meet.
std::vector<std::byte> make_grid_bytes(std::uint32_t width, std::uint32_t height) {
  const std::uint32_t stride = (width + 7) / 8;
  std::vector<std::byte> bytes(kGridHeaderSize + static_cast<std::size_t>(stride) * height,
                               std::byte{0});
  const char magic[4] = {'D', 'I', 'R', 'G'};
  for (int i = 0; i < 4; ++i) bytes[i] = static_cast<std::byte>(magic[i]);
  const auto put = [&bytes](std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      bytes[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
    }
  };
  put(4, 16);
  put(8, 1);
  put(12, width * 16);
  put(16, height * 16);
  return bytes;
}

void set_bit(std::vector<std::byte>& bytes, std::uint32_t width, std::uint32_t x,
             std::uint32_t y) {
  const std::uint32_t stride = (width + 7) / 8;
  const std::size_t at = kGridHeaderSize + static_cast<std::size_t>(y) * stride + x / 8;
  bytes[at] |= static_cast<std::byte>(1u << (x % 8));
}

/// An empty world `cells` x `cells` collision cells across.
ObstructionGrid open_field(std::int32_t cells) { return ObstructionGrid(cells, cells); }

/// A world with a vertical wall at column `wall_x`, with a gap of one cell at
/// row `gap_y`. The only way past is through the gap.
ObstructionGrid wall_with_gap(std::int32_t cells, std::int32_t wall_x, std::int32_t gap_y) {
  ObstructionGrid grid(cells, cells);
  for (std::int32_t y = 0; y < cells; ++y) {
    if (y == gap_y) continue;
    grid.set_cell(wall_x, y, true);
  }
  return grid;
}

// -- building worlds --------------------------------------------------------

/// Spawn a unit at `where` and give it a speed. Entity is null, which the
/// registry supports: 579 of the 889 shipped entities declare no animation and
/// a unit with none still moves.
ObjectId spawn_unit(World& world, MovementSystem& movement, Point where, std::int32_t speed) {
  const ObjectId id = world.spawn(NativeClass::unit, nullptr);
  world.set_position(id, where);
  MoveState& state = movement.state(id);
  state.speed = speed;
  return id;
}

Point position_of(World& world, ObjectId id) { return world.resolve_position(id); }

/// Register the system and start it, which is what the tick loop does.
void attach(World& world, MovementSystem& movement) {
  world.add_system(&movement);
  world.start();
}

/// Run a sequence of declared turn lengths, exactly as the lockstep layer
/// would. `World::advance` runs the registered systems itself, so there is no
/// second call here -- and a test that made one would double every step.
void run(World& world, MovementSystem& movement, std::initializer_list<std::int32_t> lengths) {
  (void)movement;
  for (const std::int32_t length : lengths) world.advance(length);
}

}  // namespace

// --------------------------------------------------------------------------
// integer geometry
// --------------------------------------------------------------------------

TEST(isqrt_is_exact_and_floors) {
  CHECK(isqrt(0) == 0);
  CHECK(isqrt(1) == 1);
  CHECK(isqrt(2) == 1);
  CHECK(isqrt(15) == 3);
  CHECK(isqrt(16) == 4);
  CHECK(isqrt(9999) == 99);
  CHECK(isqrt(10000) == 100);
  // A 16,384-unit map's diagonal squared, which is the largest value the
  // simulation can hand this.
  CHECK(isqrt(2LL * 16384 * 16384) == 23170);
  CHECK(isqrt(-5) == 0);
}

TEST(distance_matches_pythagoras) {
  CHECK(distance(Point{0, 0}, Point{3, 4}) == 5);
  CHECK(distance(Point{100, 100}, Point{100, 400}) == 300);
  CHECK(within(Point{0, 0}, Point{3, 4}, 5));
  CHECK(!within(Point{0, 0}, Point{3, 4}, 4));
}

TEST(rotation_is_exact_at_the_right_angles) {
  CHECK(sin_q15(0) == 0);
  CHECK(sin_q15(30) == 16384);
  CHECK(sin_q15(90) == 32768);
  CHECK(sin_q15(180) == 0);
  CHECK(sin_q15(270) == -32768);
  // Periodic, including for negative angles: scripts write `rand(30) - 15`.
  CHECK(sin_q15(-90) == sin_q15(270));
  CHECK(sin_q15(450) == sin_q15(90));

  const Point up{0, 1000};
  const Point right = rotate_degrees(up, 90);
  CHECK(right.x == -1000);
  CHECK(right.y == 0);
  const Point back = rotate_degrees(up, 180);
  CHECK(back.x == 0);
  CHECK(back.y == -1000);
  // A full turn is the identity, which a rounding scheme that is not symmetric
  // in the sign would not give.
  CHECK(rotate_degrees(Point{123, -456}, 360) == (Point{123, -456}));
}

TEST(set_length_rescales_and_refuses_the_zero_vector) {
  const Point scaled = set_length(Point{3, 4}, 50);
  CHECK(scaled.x == 30);
  CHECK(scaled.y == 40);
  CHECK(vector_length(scaled) == 50);
  CHECK(set_length(Point{0, 0}, 100) == (Point{0, 0}));
}

// --------------------------------------------------------------------------
// the obstruction grid
// --------------------------------------------------------------------------

TEST(grid_adopts_a_one_bit_sixteen_unit_container) {
  std::vector<std::byte> bytes = make_grid_bytes(64, 64);
  set_bit(bytes, 64, 10, 20);
  set_bit(bytes, 64, 11, 20);
  Result<Grid> grid = Grid::parse(bytes);
  REQUIRE(grid.ok());
  Result<ObstructionGrid> obstruction = ObstructionGrid::from_grid(*grid);
  REQUIRE(obstruction.ok());
  CHECK(obstruction->width() == 64);
  CHECK(obstruction->height() == 64);
  CHECK(obstruction->count_blocked() == 2);
  CHECK(obstruction->blocked_cell(10, 20));
  CHECK(obstruction->blocked_cell(11, 20));
  CHECK(!obstruction->blocked_cell(12, 20));
  // Addressed in world units: cell 10 spans 160..175.
  CHECK(obstruction->blocked(Point{160, 320}));
  CHECK(obstruction->blocked(Point{175, 335}));
  CHECK(!obstruction->blocked(Point{176, 336}));
}

TEST(grid_refuses_a_terrain_resolution_container) {
  // A 64-unit, 8-bit terrain grid. Reinterpreting it as obstruction would look
  // plausible and be wrong everywhere, so it has to be refused.
  std::vector<std::byte> bytes(kGridHeaderSize + 16 * 16, std::byte{0});
  const char magic[4] = {'D', 'I', 'R', 'G'};
  for (int i = 0; i < 4; ++i) bytes[i] = static_cast<std::byte>(magic[i]);
  const auto put = [&bytes](std::size_t at, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      bytes[at + static_cast<std::size_t>(i)] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
    }
  };
  put(4, 64);
  put(8, 8);
  put(12, 16 * 64);
  put(16, 16 * 64);
  Result<Grid> grid = Grid::parse(bytes);
  REQUIRE(grid.ok());
  CHECK(!ObstructionGrid::from_grid(*grid).ok());
}

TEST(off_the_map_is_blocked) {
  const ObstructionGrid grid = open_field(32);
  CHECK(grid.blocked_cell(-1, 0));
  CHECK(grid.blocked_cell(0, -1));
  CHECK(grid.blocked_cell(32, 0));
  CHECK(!grid.blocked_cell(0, 0));
  CHECK(!grid.passable_3x3(Point{8, 8}));    // the corner: neighbours are off-map
  CHECK(grid.passable_3x3(Point{100, 100}));
}

TEST(a_footprint_stamps_its_holes) {
  // A ring: blocked border, free interior. The interior must stay free, which
  // is what makes a courtyard and a gate gap behave.
  std::vector<std::byte> bytes = make_grid_bytes(8, 8);
  for (std::uint32_t i = 2; i <= 5; ++i) {
    set_bit(bytes, 8, i, 2);
    set_bit(bytes, 8, i, 5);
    set_bit(bytes, 8, 2, i);
    set_bit(bytes, 8, 5, i);
  }
  Result<Grid> mask = Grid::parse(bytes);
  REQUIRE(mask.ok());

  ObstructionGrid grid = open_field(32);
  // The stamp's anchor is the centre of its own grid: cell (4,4) of an 8x8.
  grid.stamp(*mask, Point{16 * 16 + 8, 16 * 16 + 8});
  CHECK(grid.count_blocked() == 12);
  CHECK(grid.blocked_cell(14, 14));   // the ring
  CHECK(!grid.blocked_cell(15, 15));  // the courtyard
  grid.unstamp(*mask, Point{16 * 16 + 8, 16 * 16 + 8});
  CHECK(grid.count_blocked() == 0);
}

TEST(line_of_sight_sees_a_wall) {
  const ObstructionGrid grid = wall_with_gap(32, 16, 8);
  CHECK(grid.line_is_clear(Point{100, 128}, Point{200, 128}));       // through the gap
  CHECK(!grid.line_is_clear(Point{100, 400}, Point{400, 400}));      // through the wall
  CHECK(grid.line_is_clear(Point{100, 400}, Point{200, 400}));       // short of it
}

// --------------------------------------------------------------------------
// point-to-point movement
// --------------------------------------------------------------------------

TEST(movement_scales_with_the_turn_length) {
  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);

  // 100 world units per second, at the neutral factor. A 400-unit turn is
  // therefore 40 units and an 800-unit turn is 80.
  const ObjectId unit = spawn_unit(world, movement, Point{100, 100}, 100);
  CHECK(movement.order_goto(world, unit, Point{900, 100}, 0) == MoveOutcome::moving);

  run(world, movement, {400});
  CHECK(position_of(world, unit) == (Point{140, 100}));
  run(world, movement, {800});
  CHECK(position_of(world, unit) == (Point{220, 100}));
  run(world, movement, {200});
  CHECK(position_of(world, unit) == (Point{240, 100}));
  // 799, the off-speed turn one retail dump records.
  run(world, movement, {799});
  CHECK(position_of(world, unit) == (Point{319, 100}));
}

/// **`MoveState::speed` is resolved from the class, and nothing did it before.**
///
/// The field's own comment says it is "copied in by the loader"; no loader ever
/// copied it, so every unit in every real session had a speed of zero, laid a
/// perfect path and walked none of it. Every test in this file sets `speed` by
/// hand -- which is exactly why none of them could notice.
TEST(a_fresh_move_state_takes_its_speed_from_the_class) {
  const auto bytes = [](std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  ClassGraph graph;
  graph.add(bytes("<class id=\"Unit\" cpp_class=\"CVXUnit\" parent=\"\">"
                  "<properties speed=\"120\" formation_radius=\"31\"/></class>"),
            "unit.sc.xml");
  graph.add(bytes("<class id=\"Statue\" cpp_class=\"CVXDecor\" parent=\"\"/>"), "statue.sc.xml");
  graph.link();

  World world;
  world.set_class_graph(&graph);
  MovementSystem movement;
  REQUIRE(world.add_system(&movement));
  movement.set_grid(ObstructionGrid(64, 64));
  movement.set_class_graph(&graph);

  const ObjectId walker = world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  CHECK(world.set_position(walker, Point{100, 100}));

  // Nothing is resolved until an order is laid: the state does not exist yet.
  CHECK(movement.find(walker) == nullptr);
  CHECK(movement.order_goto(world, walker, Point{500, 100}, 0, 0) != MoveOutcome::blocked);
  const MoveState* state = movement.find(walker);
  REQUIRE(state != nullptr);
  CHECK(state->speed == 120);

  // And it really walks, which a speed of zero would not.
  const Point before = world.resolve_position(walker);
  world.advance(1000);
  const Point after = world.resolve_position(walker);
  CHECK(after.x > before.x);

  // A class that declares neither keeps the defaults rather than zeroing them.
  const ObjectId statue = world.spawn(NativeClass::decor, nullptr, graph.find("Statue"));
  CHECK(world.set_position(statue, Point{100, 300}));
  (void)movement.order_goto(world, statue, Point{200, 300}, 0, 0);
  REQUIRE(movement.find(statue) != nullptr);
  CHECK(movement.find(statue)->speed == 0);

  // With no class graph at all the defaults stand, which is what every test in
  // this file that sets `speed` by hand depends on.
  MovementSystem bare;
  REQUIRE(world.add_system(&bare) || true);
  bare.set_grid(ObstructionGrid(64, 64));
  (void)bare.order_goto(world, walker, Point{500, 100}, 0, 0);
  REQUIRE(bare.find(walker) != nullptr);
  CHECK(bare.find(walker)->speed == 0);
}

constexpr std::string_view kWalkerEntity = R"(<?xml version="1.0"?>
<entity name="fixture" type="vx/unit" variations="8">
  <images>
    <image idx="1" file="idle.rle" drawmode="player_color" remaping="none" rows="4" columns="8"/>
    <image idx="2" file="toattack.rle" drawmode="player_color" remaping="none" rows="3" columns="8"/>
    <image idx="3" file="fire.rle" drawmode="clouds" remaping="none" rows="4" columns="1"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="1" anim_idx="13" anim_frame="1"/>
    <state idx="2" name="attack" image_idx="1" image_row="0" anim_idx="65536" anim_frame="65536"/>
    <state idx="3" name="broken" image_idx="1" image_row="2" anim_idx="65536" anim_frame="65536"/>
  </states>
  <anims>
    <anim idx="13" name="idle" startstate="1" endstate="1" frames="6" duration="264"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="66"/>
      <frame idx="3" duration="66"/>
      <frame idx="4" duration="66"/>
      <frame idx="5" duration="66"/>
      <frame idx="6" duration="0"/>
    </anim>
    <anim idx="19" name="ToAttack" startstate="1" endstate="2" frames="5" duration="300"
          default_duration="0" action_time="100" step="0">
      <replace layer="1" image="2"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="100"/>
      <frame idx="4" duration="100"/>
      <frame idx="5" duration="0"/>
    </anim>
    <anim idx="1" name="fire" startstate="1" endstate="1" frames="6" duration="0"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="3"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="50"/>
      <frame idx="3" duration="50"/>
      <frame idx="4" duration="50"/>
      <frame idx="5" duration="50"/>
      <frame idx="6" duration="0"/>
    </anim>
  </anims>
</entity>)";

/// **A moving unit plays its walk cycle**, and stops it on arrival.
///
/// `MoveState::walk_anim` was written by `SetWalkAnim` and read by nobody, so
/// a unit under orders slid to its destination on a still frame. The call has
/// to be on the *transition*: `World::play_anim` restarts the timeline at step
/// zero, so playing it every turn would pin the sprite on its first frame --
/// which looks exactly like the bug it fixes, and is what `MoveState::walking`
/// is for.
/// **A session takes the formation table from its inputs**, and before this
/// nothing gave it one.
///
/// `MovementSystem::set_formations` had no caller outside these tests, so
/// `formations()` was empty in every real session, `default_formation()` was
/// null, and the march returned on its first check -- a hero's warriors have
/// never formed up. It is the same shape of gap as the four `session.cpp`
/// already names beside `set_scheduler`.
TEST(a_session_loads_the_formation_table_from_its_inputs) {
  const auto bytes = [](std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  constexpr std::string_view kFormations = R"(<Formations>
  <Default Name="Front"/>
  <FormationClass Name="Front" Width="2" Height="1" OffsetFrontLineByY="30">
    <Class Name="Unit" FrontLine="1"/>
  </FormationClass>
  <FormationClass Name="Line" Width="6" Height="1" OffsetFrontLineByY="50">
    <Class Name="Unit" FrontLine="1"/>
  </FormationClass>
</Formations>)";

  ClassGraph graph;
  graph.add(bytes("<class id=\"Unit\" cpp_class=\"CVXUnit\" parent=\"\"/>"), "unit.sc.xml");
  graph.link();

  script::HostRegistry registry;
  imperivm::core::sim::SessionInputs inputs;
  inputs.classes = &graph;
  inputs.formations = bytes(kFormations);
  auto session = imperivm::core::sim::GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(session.ok());

  const MovementSystem* movement = movement_system(session.value()->world());
  REQUIRE(movement != nullptr);
  CHECK(movement->formations().formations().size() == 2);
  REQUIRE(movement->formations().default_formation() != nullptr);
  CHECK(movement->formations().default_formation()->name == "Front");
  CHECK(movement->formations().find("Line") != nullptr);

  // A session with no table is still legal -- a conformance run has none --
  // and answers null rather than crashing.
  imperivm::core::sim::SessionInputs bare;
  bare.classes = &graph;
  auto plain = imperivm::core::sim::GameSession::create(registry, bare, /*seed=*/1);
  REQUIRE(plain.ok());
  const MovementSystem* none = movement_system(plain.value()->world());
  REQUIRE(none != nullptr);
  CHECK(none->formations().default_formation() == nullptr);
}

// -- facing -----------------------------------------------------------------

TEST(the_eight_cardinal_headings_land_on_their_own_columns) {
  // Column 0 is towards the viewer, which is world +y, and the columns turn
  // the way the sheets do: -x (screen left) next. `UNITS\BOAR\WALK` is the
  // frame table this was read off -- head-on at 0, left profile at 2, seen
  // from behind at 4.
  const std::int32_t l = kFacingLength;
  CHECK(facing_column(Point{0, l}, 8) == 0u);   // south, towards the viewer
  CHECK(facing_column(Point{-l, l}, 8) == 1u);  // south-west
  CHECK(facing_column(Point{-l, 0}, 8) == 2u);  // west
  CHECK(facing_column(Point{-l, -l}, 8) == 3u);
  CHECK(facing_column(Point{0, -l}, 8) == 4u);  // north, away from the viewer
  CHECK(facing_column(Point{l, -l}, 8) == 5u);
  CHECK(facing_column(Point{l, 0}, 8) == 6u);  // east
  CHECK(facing_column(Point{l, l}, 8) == 7u);

  // Twelve variations put the same four cardinals a quarter of the way apart,
  // which is the other granularity the shipped sheets use.
  CHECK(facing_column(Point{0, l}, 12) == 0u);
  CHECK(facing_column(Point{-l, 0}, 12) == 3u);
  CHECK(facing_column(Point{0, -l}, 12) == 6u);
  CHECK(facing_column(Point{l, 0}, 12) == 9u);

  // A count of one is a prop with no facings, and a heading of nothing names
  // no direction. Both are column 0 rather than an error.
  CHECK(facing_column(Point{-l, 0}, 1) == 0u);
  CHECK(facing_column(Point{0, 0}, 8) == 0u);
  CHECK(facing_column(Point{0, 0}, 0) == 0u);
}

TEST(a_wedge_is_centred_on_its_own_heading) {
  // Rounding, not truncation: a heading a degree either side of due west is
  // still drawn due west, and the wedge only changes half way to its
  // neighbour. 22 degrees off south is column 0 and 23 is column 1.
  CHECK(facing_column(Point{-1000, 40}, 8) == 2u);
  CHECK(facing_column(Point{-1000, -40}, 8) == 2u);
  CHECK(facing_column(Point{-404, 1000}, 8) == 0u);   // 22.0 degrees
  CHECK(facing_column(Point{-424, 1000}, 8) == 1u);   // 23.0 degrees
  CHECK(facing_column(Point{404, 1000}, 8) == 0u);    // and mirrored
  CHECK(facing_column(Point{424, 1000}, 8) == 7u);

  // Every column of every count the data uses is reachable, and the sequence
  // is a monotone sweep with no column visited twice: a facing table with a
  // hole in it would show up as a unit that never draws one of its poses.
  for (const std::int32_t count : {4, 8, 12, 32, 36}) {
    std::vector<int> seen(static_cast<std::size_t>(count), 0);
    // Sample each wedge at its centre, walking the circle in the sheets'
    // own direction: south, then towards -x.
    for (std::int32_t i = 0; i < count; ++i) {
      const std::int32_t degrees = i * 360 / count;
      // `rotate_degrees` turns +y towards -x, which is the direction the
      // columns advance in, so a positive rotation of the column-0 heading
      // sweeps the sheet in column order.
      const Point heading = rotate_degrees(Point{0, kFacingLength}, degrees);
      const std::uint32_t column = facing_column(heading, count);
      CHECK(column == static_cast<std::uint32_t>(i));
      seen[column] = 1;
    }
    for (const int hit : seen) CHECK(hit == 1);
  }
}

TEST(a_walking_unit_shows_the_column_it_is_walking_along) {
  const auto bytes = [](std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  // The fixture declares `variations="8"`, so its facings are the eight.
  const auto entity = imperivm::core::Entity::parse(bytes(kWalkerEntity));
  REQUIRE(entity.ok());

  World world;
  MovementSystem movement;
  REQUIRE(world.add_system(&movement));
  movement.set_grid(ObstructionGrid(64, 64));

  const ObjectId walker = world.spawn(NativeClass::unit, &entity.value());
  CHECK(world.set_position(walker, Point{500, 500}));
  movement.state(walker).speed = 100;

  const WorldObject* slot = world.find(walker);
  REQUIRE(slot != nullptr);
  REQUIRE(slot->object != nullptr);
  // Spawned facing the viewer, like every object the map authors placed.
  CHECK(slot->object->anim.variation == 0u);

  // **The bug this covers:** the walk was simulated and the sprite never
  // turned, because nothing wrote the column the renderer reads. Walking west
  // has to show the west column from the first turn, not eventually.
  CHECK(movement.order_goto(world, walker, Point{100, 500}, 0, 0) != MoveOutcome::blocked);
  CHECK(slot->object->anim.variation == 2u);
  world.advance(200);
  CHECK(slot->object->anim.variation == 2u);

  // Turning around turns the sprite around with it.
  movement.stop(world, walker);
  CHECK(movement.order_goto(world, walker, Point{900, 500}, 0, 0) != MoveOutcome::blocked);
  CHECK(slot->object->anim.variation == 6u);
  world.advance(200);
  CHECK(slot->object->anim.variation == 6u);

  // `Face` names a heading without laying a path, and it shows too.
  movement.face(world, walker, Point{900, 100});
  CHECK(slot->object->anim.variation == 5u);
  // A point on top of the unit names no direction and leaves the pose alone.
  movement.face(world, walker, movement.position(world, walker));
  CHECK(slot->object->anim.variation == 5u);
}

TEST(a_moving_unit_plays_its_walk_animation_and_stops_it_on_arrival) {
  const auto bytes = [](std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  // `test_anim.cpp`'s entity, whose slot 13 is a looping six-frame cycle.
  const auto entity = imperivm::core::Entity::parse(bytes(kWalkerEntity));
  REQUIRE(entity.ok());

  World world;
  MovementSystem movement;
  REQUIRE(world.add_system(&movement));
  movement.set_grid(ObstructionGrid(64, 64));

  const ObjectId walker = world.spawn(NativeClass::unit, &entity.value());
  CHECK(world.set_position(walker, Point{100, 100}));
  movement.state(walker).speed = 100;
  // The locomotion slot this entity actually declares.
  movement.state(walker).walk_anim = 13;

  const WorldObject* slot = world.find(walker);
  REQUIRE(slot != nullptr);
  CHECK(!slot->animating);

  CHECK(movement.order_goto(world, walker, Point{600, 100}, 0, 0) != MoveOutcome::blocked);
  world.advance(100);
  // Walking: the cycle is running and the state says so.
  CHECK(movement.find(walker)->walking);
  const bool started = slot->animating;
  CHECK(started);

  // **It is not restarted every turn.** Two more turns and the cursor has
  // advanced rather than sitting at zero.
  world.advance(100);
  world.advance(100);
  if (slot->object != nullptr) CHECK(slot->object->anim.elapsed_ms > 0);

  // Run to arrival; the cycle stops.
  for (int i = 0; i < 40 && movement.find(walker)->has_path; ++i) world.advance(200);
  CHECK(!movement.find(walker)->has_path);
  CHECK(!movement.find(walker)->walking);
  CHECK(!slot->animating);
}

/// **Arrival stops the walk, and only the walk.** Something else can take the
/// cursor over while a unit is still on its route -- combat's swing, a death,
/// a script's `PlayAnim` -- and arrival used to freeze whatever was playing.
/// The case that found it: a unit killed mid-walk had its death pinned on the
/// first row the turn after, by the walk's own arrival.
TEST(arrival_leaves_an_animation_that_is_not_the_walk_alone) {
  const auto bytes = [](std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  const auto entity = imperivm::core::Entity::parse(bytes(kWalkerEntity));
  REQUIRE(entity.ok());

  World world;
  MovementSystem movement;
  REQUIRE(world.add_system(&movement));
  movement.set_grid(ObstructionGrid(64, 64));

  const ObjectId walker = world.spawn(NativeClass::unit, &entity.value());
  CHECK(world.set_position(walker, Point{100, 100}));
  movement.state(walker).speed = 100;
  movement.state(walker).walk_anim = 13;
  CHECK(movement.order_goto(world, walker, Point{300, 100}, 0, 0) != MoveOutcome::blocked);
  world.advance(100);
  REQUIRE(movement.find(walker)->walking);

  // Mid-route, the fixture's looping slot 1 takes the cursor over.
  REQUIRE(world.play_anim(walker, 1, AnimRepeat::loop));
  for (int i = 0; i < 40 && movement.find(walker)->has_path; ++i) world.advance(200);
  REQUIRE(!movement.find(walker)->has_path);
  world.advance(200);
  const WorldObject* slot = world.find(walker);
  REQUIRE(slot != nullptr && slot->object != nullptr);
  CHECK(!movement.find(walker)->walking);
  CHECK(slot->object->anim.anim_slot == 1);
  CHECK(slot->animating);
}

TEST(a_unit_stops_on_arrival) {
  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);

  const ObjectId unit = spawn_unit(world, movement, Point{100, 100}, 100);
  movement.order_goto(world, unit, Point{200, 100}, 0);
  // 100 units at 100 per second is one second: three turns of 400 overshoot it.
  run(world, movement, {400, 400, 400});
  CHECK(position_of(world, unit) == (Point{200, 100}));
  const MoveState* state = movement.find(unit);
  REQUIRE(state != nullptr);
  CHECK(!state->has_path);
  CHECK(state->last_outcome == MoveOutcome::arrived);

  // And stays stopped.
  run(world, movement, {800, 800});
  CHECK(position_of(world, unit) == (Point{200, 100}));
}

TEST(a_unit_stops_at_its_arrival_range) {
  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);

  const ObjectId unit = spawn_unit(world, movement, Point{100, 100}, 100);
  // The deer's class declares range="17"; a unit told to approach a point with
  // a tolerance stops inside it rather than on it.
  movement.order_goto(world, unit, Point{600, 100}, 100);
  run(world, movement, {800, 800, 800, 800, 800, 800});
  const Point where = position_of(world, unit);
  CHECK(within(where, Point{600, 100}, 100));
  const MoveState* state = movement.find(unit);
  REQUIRE(state != nullptr);
  CHECK(!state->has_path);
  CHECK(state->last_outcome == MoveOutcome::arrived);
}

TEST(an_order_into_a_sealed_room_is_refused) {
  // A cell walled in on all four sides: reachable by nothing.
  ObstructionGrid grid = open_field(32);
  grid.set_cell(19, 20, true);
  grid.set_cell(21, 20, true);
  grid.set_cell(20, 19, true);
  grid.set_cell(20, 21, true);
  grid.set_cell(19, 19, true);
  grid.set_cell(21, 21, true);
  grid.set_cell(19, 21, true);
  grid.set_cell(21, 19, true);

  World world;
  MovementSystem movement;
  movement.set_grid(std::move(grid));
  attach(world, movement);

  const Point sealed{20 * 16 + 8, 20 * 16 + 8};
  const ObjectId unit = spawn_unit(world, movement, Point{100, 100}, 100);
  // The destination is free but walled in. The search cannot reach it, so the
  // unit is given the best route it found -- which is to the wall and no
  // further. A route that gets most of the way there is what the original does;
  // refusing to move at all would be worse.
  CHECK(movement.order_goto(world, unit, sealed, 0) == MoveOutcome::exhausted);
  for (int i = 0; i < 40; ++i) run(world, movement, {800});
  const MoveState* stuck = movement.find(unit);
  REQUIRE(stuck != nullptr);
  CHECK(!stuck->has_path);
  CHECK(stuck->last_outcome == MoveOutcome::exhausted);
  CHECK(!within(position_of(world, unit), sealed, kCollisionCellSize));
  // Re-ordering from where it stopped now has nowhere to go at all.
  CHECK(movement.order_goto(world, unit, sealed, 0) == MoveOutcome::blocked);

  // Standing *in* the sealed cell, nothing is reachable at all.
  const ObjectId trapped = spawn_unit(world, movement, Point{20 * 16 + 8, 20 * 16 + 8}, 100);
  CHECK(movement.order_goto(world, trapped, Point{100, 100}, 0) == MoveOutcome::blocked);
  const MoveState* state = movement.find(trapped);
  REQUIRE(state != nullptr);
  CHECK(!state->has_path);
  run(world, movement, {800, 800});
  CHECK(position_of(world, trapped) == (Point{20 * 16 + 8, 20 * 16 + 8}));
}

TEST(a_unit_walks_around_an_obstacle) {
  const std::int32_t cells = 40;
  const ObstructionGrid grid = wall_with_gap(cells, 20, 5);

  World world;
  MovementSystem movement;
  movement.set_grid(grid);
  attach(world, movement);

  // Start and finish level with each other, either side of the wall. The only
  // way across is the gap at row 5, well to the north.
  const Point start{10 * 16 + 8, 30 * 16 + 8};
  const Point goal{30 * 16 + 8, 30 * 16 + 8};
  const ObjectId unit = spawn_unit(world, movement, start, 400);
  REQUIRE(movement.order_goto(world, unit, goal, 0) == MoveOutcome::moving);

  const MoveState* state = movement.find(unit);
  REQUIRE(state != nullptr);
  // The route is longer than the straight line, because the straight line is
  // through a wall.
  CHECK(state->path_length > distance(start, goal));
  // And it goes through the gap: some waypoint is north of the start.
  bool went_north = false;
  for (const Point& waypoint : state->waypoints) {
    if (waypoint.y < 10 * 16) went_north = true;
    // No waypoint may sit inside the wall.
    CHECK(!grid.blocked(waypoint));
  }
  CHECK(went_north);

  // And the unit actually gets there. 400 units per second over a route of
  // about 1,300 units is four seconds; twenty turns of 800 is sixteen.
  for (int i = 0; i < 20; ++i) run(world, movement, {800});
  CHECK(position_of(world, unit) == goal);
}

TEST(a_route_cut_by_a_new_building_is_re_laid) {
  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);

  const ObjectId unit = spawn_unit(world, movement, Point{100, 500}, 200);
  REQUIRE(movement.order_goto(world, unit, Point{900, 500}, 0) == MoveOutcome::moving);
  run(world, movement, {800});

  // A wall goes up across the route, with a gap to the south.
  ObstructionGrid& grid = movement.mutable_grid();
  for (std::int32_t y = 0; y < 40; ++y) grid.set_cell(40, y, true);

  run(world, movement, {800});
  const MoveState* state = movement.find(unit);
  REQUIRE(state != nullptr);
  CHECK(state->has_path);
  for (const Point& waypoint : state->waypoints) {
    CHECK(!movement.grid().blocked(waypoint));
  }
  // The detour is real: the route is no longer a straight line.
  CHECK(state->waypoints.size() > 2);
}

/// **A class with `ignore_passability` walks straight, whatever the grid says.**
///
/// The sentries are its only users: `WALL_PATROL.VS` places one on a wall's
/// walkway, blocked ground, and sends it along the walk with
/// `while (!.Goto(pt, 0, 2000, true, 0));`. Searched over the grid, that goto
/// never arrived, and every sentry of every town stood where it was placed --
/// stacked behind a tower, never reaching its guard order, never fighting.
/// `gbr.exe` passes the class's flag into every path request, and the smart
/// pathfinder answers it with the two-point route before any search
/// (`0x0040b580`).
TEST(a_class_that_ignores_passability_walks_straight_along_a_wall) {
  const auto bytes = [](std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  ClassGraph graph;
  graph.add(bytes("<class id=\"Unit\" cpp_class=\"CVXUnit\" parent=\"\">"
                  "<properties speed=\"200\"/></class>"),
            "unit.sc.xml");
  graph.add(bytes("<class id=\"Sentry\" cpp_class=\"CVXUnit\" parent=\"Unit\">"
                  "<properties ignore_passability=\"1\"/></class>"),
            "sentry.sc.xml");
  graph.add(bytes("<class id=\"Unset\" cpp_class=\"CVXUnit\" parent=\"Unit\">"
                  "<properties ignore_passability=\"0\"/></class>"),
            "unset.sc.xml");
  graph.link();

  // A wall three cells thick along rows 19..21, the whole width of the map:
  // nothing standing on it reaches anything by the grid.
  ObstructionGrid grid = open_field(64);
  for (std::int32_t x = 0; x < 64; ++x) {
    for (std::int32_t y = 19; y <= 21; ++y) grid.set_cell(x, y, true);
  }

  World world;
  world.set_class_graph(&graph);
  MovementSystem movement;
  movement.set_grid(std::move(grid));
  movement.set_class_graph(&graph);
  attach(world, movement);

  const Point from{5 * 16 + 8, 20 * 16 + 8};
  const Point to{50 * 16 + 8, 20 * 16 + 8};
  const ObjectId sentry = world.spawn(NativeClass::unit, nullptr, graph.find("Sentry"));
  const ObjectId soldier = world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  const ObjectId unset = world.spawn(NativeClass::unit, nullptr, graph.find("Unset"));
  CHECK(world.set_position(sentry, from));
  CHECK(world.set_position(soldier, from));
  CHECK(world.set_position(unset, from));
  CHECK(movement.ignores_passability(world, sentry));
  CHECK(!movement.ignores_passability(world, soldier));
  CHECK(!movement.ignores_passability(world, unset));

  // The sentry is given the straight line, start then goal.
  REQUIRE(movement.order_goto(world, sentry, to, 0, 0) == MoveOutcome::moving);
  const MoveState* state = movement.find(sentry);
  REQUIRE(state != nullptr);
  REQUIRE(state->waypoints.size() == 2);
  CHECK(state->waypoints.front() == from);
  CHECK(state->waypoints.back() == to);
  CHECK(state->path_length == distance(from, to));
  // Anyone else standing there has nowhere to go, the flag set to 0 included.
  CHECK(movement.order_goto(world, soldier, to, 0, 0) == MoveOutcome::blocked);
  CHECK(movement.order_goto(world, unset, to, 0, 0) == MoveOutcome::blocked);

  // And it gets there: 720 units at 200 a second is under four seconds.
  for (int i = 0; i < 6; ++i) run(world, movement, {800});
  CHECK(position_of(world, sentry) == to);
  CHECK(position_of(world, soldier) == from);

  // A grid that changes under its route does not re-lay it round anything.
  REQUIRE(movement.order_goto(world, sentry, from, 0, 0) == MoveOutcome::moving);
  run(world, movement, {800});
  for (std::int32_t y = 0; y < 64; ++y) movement.mutable_grid().set_cell(30, y, true);
  run(world, movement, {800});
  state = movement.find(sentry);
  REQUIRE(state != nullptr);
  CHECK(state->has_path);
  CHECK(state->waypoints.size() == 2);
  CHECK(state->waypoints.back() == from);
  for (int i = 0; i < 6; ++i) run(world, movement, {800});
  CHECK(position_of(world, sentry) == from);
}

// --------------------------------------------------------------------------
// determinism
// --------------------------------------------------------------------------

namespace {

/// Build the same scenario every time: three units of different speeds crossing
/// a walled map, so that the comparison covers routing as well as stepping.
struct Scenario {
  World world;
  MovementSystem movement;
  std::vector<ObjectId> units;
};

void build(Scenario& scenario) {
  scenario.movement.set_grid(wall_with_gap(48, 24, 6));
  attach(scenario.world, scenario.movement);
  const std::int32_t speeds[3] = {150, 80, 240};
  for (int i = 0; i < 3; ++i) {
    const Point start{5 * 16 + 8, (20 + 3 * i) * 16 + 8};
    const ObjectId id = spawn_unit(scenario.world, scenario.movement, start, speeds[i]);
    scenario.movement.order_goto(scenario.world, id, Point{40 * 16 + 8, (20 + 3 * i) * 16 + 8}, 0);
    scenario.units.push_back(id);
  }
}

std::uint64_t hash_of(Scenario& scenario) {
  // `World::state_hash` folds every registered system in registration order,
  // so this covers positions, `SyncFlags` bit 17 and the movement state at once.
  return scenario.world.state_hash();
}

}  // namespace

TEST(the_same_world_advanced_twice_lands_identically) {
  Scenario a;
  Scenario b;
  build(a);
  build(b);
  for (int i = 0; i < 30; ++i) {
    run(a.world, a.movement, {400});
    run(b.world, b.movement, {400});
  }
  CHECK(hash_of(a) == hash_of(b));
  for (std::size_t i = 0; i < a.units.size(); ++i) {
    CHECK(position_of(a.world, a.units[i]) == position_of(b.world, b.units[i]));
  }
}

TEST(a_turn_partition_does_not_move_a_unit) {
  // 4,800 game-time units, cut four different ways. The lockstep layer
  // renegotiates the turn length as latency moves -- one retail log runs
  // 400, 400, ..., 460, 800, 800 -- so this is the case that actually happens.
  Scenario whole;
  Scenario halves;
  Scenario quarters;
  Scenario ragged;
  build(whole);
  build(halves);
  build(quarters);
  build(ragged);

  for (int i = 0; i < 6; ++i) run(whole.world, whole.movement, {800});
  for (int i = 0; i < 12; ++i) run(halves.world, halves.movement, {400});
  for (int i = 0; i < 24; ++i) run(quarters.world, quarters.movement, {200});
  // An uneven partition of the same 4,800: 799 + 1 + 400 + 200 + 200 + 800 +
  // 1,200 + 1,200. It includes the 799 the one off-speed retail dump records
  // and two lengths outside the observed range, because nothing enforces the
  // range and a system that only works on round numbers is not exact.
  run(ragged.world, ragged.movement, {799, 1, 400, 200, 200, 800, 1200, 1200});

  for (std::size_t i = 0; i < whole.units.size(); ++i) {
    const Point reference = position_of(whole.world, whole.units[i]);
    CHECK(position_of(halves.world, halves.units[i]) == reference);
    CHECK(position_of(quarters.world, quarters.units[i]) == reference);
    CHECK(position_of(ragged.world, ragged.units[i]) == reference);
  }

  // The world hash agrees too, which is the stronger statement: it covers the
  // progress accumulator, the target and the facing, not just the coordinate.
  // The clock is in it as well, and all four partitions have run the same total
  // game time -- but not the same number of turns, so `turns()` is excluded by
  // comparing the movement contribution on its own.
  std::uint64_t whole_hash = 0;
  std::uint64_t halves_hash = 0;
  std::uint64_t quarters_hash = 0;
  std::uint64_t ragged_hash = 0;
  whole.movement.hash(whole_hash);
  halves.movement.hash(halves_hash);
  quarters.movement.hash(quarters_hash);
  ragged.movement.hash(ragged_hash);
  CHECK(whole_hash == halves_hash);
  CHECK(whole_hash == quarters_hash);
  CHECK(whole_hash == ragged_hash);

  // And `walking` is in it: the walk-cycle memory decides whether the next
  // turn restarts an animation, which lands in the object hash a turn later.
  MoveState* first = whole.movement.find(whole.units.front());
  REQUIRE(first != nullptr);
  first->walking = !first->walking;
  std::uint64_t flipped = 0;
  whole.movement.hash(flipped);
  CHECK(flipped != whole_hash);
}

TEST(a_speed_change_keeps_partition_invariance) {
  // The accumulator is exact across a rate change too, as long as the change
  // falls on a boundary both partitions share -- which it does, because a host
  // call happens between turns.
  Scenario whole;
  Scenario halves;
  build(whole);
  build(halves);

  run(whole.world, whole.movement, {800, 800});
  run(halves.world, halves.movement, {400, 400, 400, 400});
  for (const ObjectId id : whole.units) whole.movement.state(id).speed_factor = 170;
  for (const ObjectId id : halves.units) halves.movement.state(id).speed_factor = 170;
  run(whole.world, whole.movement, {800, 800, 800});
  run(halves.world, halves.movement, {400, 400, 400, 400, 400, 400});

  for (std::size_t i = 0; i < whole.units.size(); ++i) {
    CHECK(position_of(whole.world, whole.units[i]) ==
          position_of(halves.world, halves.units[i]));
  }
}

TEST(the_pathfinder_does_not_depend_on_its_scratch_state) {
  const ObstructionGrid grid = wall_with_gap(64, 32, 10);
  PathRequest request;
  request.start = Point{5 * 16 + 8, 50 * 16 + 8};
  request.goal = Point{60 * 16 + 8, 50 * 16 + 8};

  PathFinder fresh;
  const Path reference = fresh.find(grid, request);
  REQUIRE(reference.status == PathStatus::found);
  REQUIRE(reference.waypoints.size() > 2);

  // A searcher whose arrays have been dirtied by unrelated searches, including
  // ones on a differently shaped grid, must produce the identical route.
  PathFinder dirty;
  for (int i = 0; i < 5; ++i) {
    PathRequest noise;
    noise.start = Point{16 * i + 8, 16 * i + 8};
    noise.goal = Point{16 * (40 - i) + 8, 16 * (3 * i + 1) + 8};
    (void)dirty.find(grid, noise);
    (void)dirty.find(wall_with_gap(24, 12, 3), noise);
  }
  const Path repeated = dirty.find(grid, request);
  CHECK(repeated.status == reference.status);
  CHECK(repeated.length == reference.length);
  REQUIRE(repeated.waypoints.size() == reference.waypoints.size());
  for (std::size_t i = 0; i < reference.waypoints.size(); ++i) {
    CHECK(repeated.waypoints[i] == reference.waypoints[i]);
  }
  // And the search itself is reproducible node for node.
  CHECK(repeated.expanded == reference.expanded);
}

TEST(a_reachable_goal_is_found_round_a_wall) {
  // The grid search (0x0041b140) goes the long way round a wall, well within
  // its 8,000 cells.
  ObstructionGrid grid(64, 64);
  for (std::int32_t y = 0; y < 60; ++y) grid.set_cell(32, y, true);

  PathFinder finder;
  PathRequest request;
  request.start = Point{5 * 16 + 8, 5 * 16 + 8};
  request.goal = Point{60 * 16 + 8, 5 * 16 + 8};
  const Path path = finder.find(grid, request);
  CHECK(path.status == PathStatus::found);
  REQUIRE(!path.waypoints.empty());
  CHECK(path.waypoints.back() == request.goal);
  CHECK(path.expanded > 50);
}

TEST(an_unreachable_goal_answers_the_reachable_cell_nearest_it) {
  // A goal sealed behind a solid wall: the grid search (0x0041b140) floods
  // the start's side -- 2,048 cells, inside its budget -- and, its heap
  // empty, answers status 2 with the route to the expanded cell whose
  // estimate is least, ended at that cell's centre (0x0041ba00).
  ObstructionGrid grid(64, 64);
  for (std::int32_t y = 0; y < 64; ++y) grid.set_cell(32, y, true);

  PathFinder finder;
  PathRequest request;
  request.start = Point{5 * 16 + 8, 30 * 16 + 8};
  request.goal = Point{60 * 16 + 8, 40 * 16 + 3};
  const Path path = finder.find(grid, request);
  CHECK(path.status == PathStatus::partial);
  REQUIRE(!path.waypoints.empty());
  CHECK(path.waypoints.back() == (Point{31 * 16 + 8, 40 * 16 + 8}));
  CHECK(path.length > 0);
  // The start's side holds 2,048 cells; the search pops every one.
  CHECK(path.expanded >= 2048);
  CHECK(path.expanded <= GridSearch::kBudget + 1);

  // Standing on that cell already, there is nowhere nearer to go.
  PathRequest again = request;
  again.start = Point{31 * 16 + 8, 40 * 16 + 8};
  const Path stay = finder.find(grid, again);
  CHECK(stay.status == PathStatus::partial);
  CHECK(stay.waypoints.size() == 1);
  CHECK(stay.length == 0);
}

// --------------------------------------------------------------------------
// formations
// --------------------------------------------------------------------------

namespace {

/// A formation table shaped like `DATA\FORMATIONS.XML` but written here. The
/// attribute names and the three placement flags are the shipped file's; the
/// numbers are chosen so the expected offsets are obvious.
constexpr std::string_view kFormationsXml = R"(<Formations>
  <Default Name="Front"/>
  <FormationClass Name="Front" Description="d" Width="2" Height="1" BonusLevel="4"
                  OffsetFrontLineByY="30" OffsetWingsByX="0" OffsetWingsByY="0">
    <Class Name="Unit" FrontLine="1"/>
    <Class Name="Peasant" CentralBlock="1"/>
  </FormationClass>
  <FormationClass Name="Line" Description="d" Width="6" Height="1" BonusDamage="2"
                  BonusArmor="2" BonusRange="20" OffsetFrontLineByY="50"
                  OffsetWingsByX="0" OffsetWingsByY="0">
    <Class Name="Unit" FrontLine="1"/>
  </FormationClass>
  <FormationClass Name="Cavalry core" Description="" Width="2" Height="1" BonusDamage="4"
                  BonusRange="20" OffsetFrontLineByY="30" OffsetWingsByX="50"
                  OffsetWingsByY="0">
    <Class Name="Unit" Wings="1"/>
    <Class Name="Horse" FrontLine="1"/>
  </FormationClass>
</Formations>)";

std::span<const std::byte> as_bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

TEST(the_formation_table_reads_what_the_file_declares) {
  Result<FormationTable> table = FormationTable::parse(as_bytes(kFormationsXml));
  REQUIRE(table.ok());
  CHECK(table->default_name() == "Front");
  CHECK(table->formations().size() == 3);

  const FormationClassDef* front = table->find("Front");
  REQUIRE(front != nullptr);
  CHECK(front->width == 2);
  CHECK(front->height == 1);
  CHECK(front->bonus_level == 4);
  CHECK(front->offset_front_line_y == 30);
  // The `<Class>` entries in file order, each with its three weights.
  REQUIRE(front->roles.size() == 2);
  CHECK(front->roles[0].class_name == "Unit");
  CHECK(front->roles[0].front == 1);
  CHECK(front->roles[0].central == 0);
  CHECK(front->roles[1].class_name == "Peasant");
  CHECK(front->roles[1].central == 1);
  CHECK(table->find("Cavalry core")->roles[0].wings == 1);

  // `SetFormation(cmdparam)` passes map-authored strings, so lookup folds case.
  CHECK(table->find("cavalry core") != nullptr);
  CHECK(table->find("Nonesuch") == nullptr);
  CHECK(table->default_formation() == front);
}

TEST(the_bonus_attributes_land_where_the_executable_puts_them) {
  // The original's loader walks attributes in file order and `BonusArmor`
  // writes both defence fields, so the same two attributes give different
  // tables depending on which is written first. The shipped file never puts
  // both on one element; this is the case that would show if the parser read
  // by name instead.
  constexpr std::string_view kXml = R"(<Formations>
  <FormationClass Name="ArmorThenSlash" BonusArmor="6" BonusDefenceSlash="5"/>
  <FormationClass Name="SlashThenArmor" BonusDefenceSlash="5" BonusArmor="6"/>
  <FormationClass Name="PierceOnly" BonusDefencePierce="3" BonusAttack="4"/>
</Formations>)";
  Result<FormationTable> table = FormationTable::parse(as_bytes(kXml));
  REQUIRE(table.ok());

  const FormationClassDef* first = table->find("ArmorThenSlash");
  REQUIRE(first != nullptr);
  CHECK(first->bonus_defence_slash == 5);
  CHECK(first->bonus_defence_pierce == 6);

  const FormationClassDef* second = table->find("SlashThenArmor");
  REQUIRE(second != nullptr);
  CHECK(second->bonus_defence_slash == 6);
  CHECK(second->bonus_defence_pierce == 6);

  const FormationClassDef* third = table->find("PierceOnly");
  REQUIRE(third != nullptr);
  CHECK(third->bonus_defence_slash == 0);
  CHECK(third->bonus_defence_pierce == 3);
  // Parsed and carried, read by nothing: the executable has no such name.
  CHECK(third->bonus_attack == 4);
}

namespace {

/// The classes the layout tests lay: a root `Unit` with the shipped spacing
/// radius, a hero with the shipped `radius` of 45, and three kinds of
/// soldier with their own `formation_priority`, one of them wider.
ClassGraph layout_graph() {
  ClassGraph graph;
  graph.add(as_bytes(R"(<class id="Unit" cpp_class="CVXUnit" parent="">
      <properties formation_radius="26" radius="15"/></class>)"),
            "unit.sc.xml");
  graph.add(as_bytes(R"(<class id="Leader" cpp_class="CVXHero" parent="Unit">
      <properties radius="45" formation_priority="1100"/></class>)"),
            "leader.sc.xml");
  graph.add(as_bytes(R"(<class id="Archer" cpp_class="CVXUnit" parent="Unit">
      <properties formation_priority="500"/></class>)"),
            "archer.sc.xml");
  graph.add(as_bytes(R"(<class id="Sword" cpp_class="CVXUnit" parent="Unit">
      <properties formation_priority="1100"/></class>)"),
            "sword.sc.xml");
  graph.add(as_bytes(R"(<class id="Horse" cpp_class="CVXUnit" parent="Unit">
      <properties formation_radius="35" formation_priority="2000"/></class>)"),
            "horse.sc.xml");
  graph.link();
  return graph;
}

/// `count` records of `klass`, laid in `shape` round a hero of radius 45.
struct Laid {
  std::vector<FormationRecord> records;
  std::int32_t radius = 0;
};
Laid lay(const ClassGraph& graph, std::string_view xml, std::string_view shape,
         std::span<const ClassIndex> classes) {
  Laid out;
  Result<FormationTable> table = FormationTable::parse(as_bytes(xml));
  CHECK(table.ok());
  const FormationClassDef* def = table.ok() ? table->find(shape) : nullptr;
  CHECK(def != nullptr);
  if (def == nullptr) return out;
  for (const ClassIndex klass : classes) out.records.push_back({klass, Point{}});
  out.radius = formation_lay_out(graph, *def, 45, out.records);
  return out;
}

std::vector<Point> offsets_of(const Laid& laid) {
  std::vector<Point> out;
  for (const FormationRecord& record : laid.records) out.push_back(record.offset);
  return out;
}

constexpr std::string_view kBlockXml = R"(<Formations><Default Name="Block"/>
  <FormationClass Name="Block"><Class Name="Unit" CentralBlock="1"/></FormationClass>
  <FormationClass Name="Mixed" OffsetFrontLineByY="30">
    <Class Name="Unit" CentralBlock="1"/><Class Name="Horse" FrontLine="1"/></FormationClass>
</Formations>)";

}  // namespace

/// The central block (0x005f04b0) is square rings round the hero: ring one at
/// `R + S` -- the hero's `radius` 45 and twice the soldiers' 26 -- with room
/// for eight. Eight fill it: one along the back, a pair down the sides, one
/// along the front, then the four corners; the radius is the corner's
/// distance, `isqrt(2 * 97^2)`.
TEST(a_block_of_eight_is_the_first_ring_round_the_hero) {
  const ClassGraph graph = layout_graph();
  const std::vector<ClassIndex> classes(8, graph.find("Unit"));
  const Laid laid = lay(graph, kBlockXml, "Block", classes);
  const std::vector<Point> want = {{0, 97},   {-97, 0},  {97, 0},   {0, -97},
                                   {-97, 97}, {97, 97}, {-97, -97}, {97, -97}};
  CHECK(offsets_of(laid) == want);
  CHECK(laid.radius == 137);
}

/// A ring of six is the corners and two more, `6 - 4 = 4*0 + 2`: with no
/// front line one goes to the back and one to the front; a ring of five puts
/// its one extra at the front; three left over make a last short ring of the
/// front corners and the back centre, one ring further out.
TEST(a_short_ring_spreads_its_remainder_the_way_the_original_does) {
  const ClassGraph graph = layout_graph();
  const ClassIndex unit = graph.find("Unit");
  {
    const std::vector<ClassIndex> classes(6, unit);
    const std::vector<Point> want = {{0, 97}, {0, -97}, {-97, 97}, {97, 97}, {-97, -97}, {97, -97}};
    CHECK(offsets_of(lay(graph, kBlockXml, "Block", classes)) == want);
  }
  {
    const std::vector<ClassIndex> classes(5, unit);
    const std::vector<Point> want = {{0, -97}, {-97, 97}, {97, 97}, {-97, -97}, {97, -97}};
    CHECK(offsets_of(lay(graph, kBlockXml, "Block", classes)) == want);
  }
  {
    const std::vector<ClassIndex> classes(11, unit);
    const Laid laid = lay(graph, kBlockXml, "Block", classes);
    REQUIRE(laid.records.size() == 11);
    CHECK(laid.records[8].offset == (Point{-149, -149}));
    CHECK(laid.records[9].offset == (Point{149, -149}));
    CHECK(laid.records[10].offset == (Point{0, 149}));
    CHECK(laid.radius == 210);
  }
  {
    // One alone stands at the front of the first ring.
    const std::vector<ClassIndex> classes(1, unit);
    const Laid laid = lay(graph, kBlockXml, "Block", classes);
    CHECK(offsets_of(laid) == (std::vector<Point>{{0, -97}}));
    CHECK(laid.radius == 97);
  }
}

/// The front line (0x005f0b60) is a grid that grows by `Width` and `Height`
/// until it holds everyone and is then cut back: six in a 6-by-1 `Line` are
/// one row of six, spread from the centre out to a half-width of
/// `3 * 52 - 26`, standing `52 + 45 + 50` ahead -- one row, the empty block's
/// depth (the hero's radius) and the line's offset.
TEST(a_line_is_one_row_spread_from_the_centre_out) {
  const ClassGraph graph = layout_graph();
  constexpr std::string_view kXml = R"(<Formations><Default Name="Line"/>
    <FormationClass Name="Line" Width="6" Height="1" OffsetFrontLineByY="50">
      <Class Name="Unit" FrontLine="1"/></FormationClass></Formations>)";
  const std::vector<ClassIndex> classes(6, graph.find("Unit"));
  const Laid laid = lay(graph, kXml, "Line", classes);
  const std::vector<Point> want = {{-26, -147}, {26, -147},  {-78, -147},
                                   {78, -147},  {-130, -147}, {130, -147}};
  CHECK(offsets_of(laid) == want);
  CHECK(laid.radius == 196);
}

/// Five in a 2-by-1 `Front`: the grid grows to 4 by 2, is cut back to 3 by 2;
/// the first row has a centre and the pair at the edges, the second only the
/// pair.
TEST(a_front_of_five_is_three_then_two) {
  const ClassGraph graph = layout_graph();
  constexpr std::string_view kXml = R"(<Formations><Default Name="Front"/>
    <FormationClass Name="Front" Width="2" Height="1" OffsetFrontLineByY="30">
      <Class Name="Unit" FrontLine="1"/></FormationClass></Formations>)";
  const std::vector<ClassIndex> classes(5, graph.find("Unit"));
  const std::vector<Point> want = {{0, -179}, {-52, -179}, {52, -179}, {-52, -127}, {52, -127}};
  CHECK(offsets_of(lay(graph, kXml, "Front", classes)) == want);
}

/// The wings (0x005f0d90) take half the `Wings` weight each, so units of one
/// class go left, right, left, right; each wing stands off the front line's
/// edge by `OffsetWingsByX` and lays its columns outwards from there.
TEST(the_wings_share_a_class_out_and_stand_off_the_front_lines_edge) {
  const ClassGraph graph = layout_graph();
  constexpr std::string_view kXml = R"(<Formations><Default Name="Wings"/>
    <FormationClass Name="Wings" OffsetWingsByX="50">
      <Class Name="Unit" Wings="2"/></FormationClass></Formations>)";
  const std::vector<ClassIndex> classes(4, graph.find("Unit"));
  const Laid laid = lay(graph, kXml, "Wings", classes);
  const std::vector<Point> want = {{-147, -45}, {-147, 7}, {147, -45}, {147, 7}};
  CHECK(offsets_of(laid) == want);
  CHECK(laid.radius == 153);
}

/// The `<Class>` entries are met most derived first (the loader, 0x005f6ca0),
/// so `Horse` takes its own entry although `Unit`'s is written first; within
/// a group the slots go by `formation_priority`, lowest first; and each
/// group's spacing is its own widest -- the horse's 35, the block's 26. The
/// block's three are a short last ring, and the line's one stands at its
/// centre, the block's half-width out.
TEST(the_layout_groups_by_the_most_derived_entry_and_orders_by_priority) {
  const ClassGraph graph = layout_graph();
  const ClassIndex sword = graph.find("Sword");
  const ClassIndex archer = graph.find("Archer");
  const ClassIndex horse = graph.find("Horse");
  const std::vector<ClassIndex> classes = {sword, archer, horse, sword};
  const Laid laid = lay(graph, kBlockXml, "Mixed", classes);
  REQUIRE(laid.records.size() == 4);
  CHECK(laid.records[0].klass == archer);
  CHECK(laid.records[1].klass == sword);
  CHECK(laid.records[2].klass == sword);
  CHECK(laid.records[3].klass == horse);
  const std::vector<Point> want = {{-97, -97}, {97, -97}, {0, 97}, {0, -197}};
  CHECK(offsets_of(laid) == want);
  CHECK(laid.radius == 197);
}

/// A sample turns each offset into the formation's direction (0x005f2900):
/// local -y onto the direction, local +x onto it turned a quarter to
/// `(-dy, dx)`, each coordinate truncated toward zero.
TEST(a_sample_turns_the_offsets_into_the_formations_direction) {
  const Point centre{1000, 1000};
  CHECK(formation_turn(centre, Point{0, -5}, Point{0, -97}) == (Point{1000, 903}));
  CHECK(formation_turn(centre, Point{3, 4}, Point{0, -100}) == (Point{1060, 1080}));
  CHECK(formation_turn(centre, Point{3, 4}, Point{10, 0}) == (Point{992, 1006}));
  CHECK(formation_turn(centre, Point{3, 4}, Point{7, 0}) == (Point{995, 1004}));
  CHECK(formation_turn(centre, Point{1, 1}, Point{0, -10}) == (Point{1007, 1007}));
  CHECK(formation_turn(centre, Point{-1, -1}, Point{0, -10}) == (Point{993, 993}));
}

/// The claim on the stations (0x005f3fe0; docs/formats/formation.md, "The
/// claim on the stations"). A 64-cell square of open ground, the centre at
/// (520, 520) -- cell (32, 32) -- and the 1,024 floor: 64 cells either side.
///
///   * A station on a cell somebody has is kept exactly, not snapped.
///   * A station in a wall east of the centre (cells 40..47 by 28..36), at
///     cell (44, 32): the box from the centre's cell to it is row 32, x
///     32..44, and an open cell there costs half its distance from the
///     centre in whole cells plus the square of its distance from cell 44:
///     x 39 is 3 + 25 = 28, x 38 is 3 + 36. It goes to (39, 32)'s centre.
///   * Two stations in cell (37, 37): the first keeps it and takes it; the
///     second goes, within the box (32..37)^2, to (36, 36): four diagonals
///     from the centre (half: 2) and one cell off (1), 3, where (36, 37) and
///     (37, 36) cost 3 + 1.
///   * A station inside a sealed ring (cells 18..22) is on a cell the flood
///     never reached: nobody's. In the box (20..32)^2 the cheapest is 16,
///     first met in row order at (23, 20).
///   * Nothing open anywhere: the cell the member stands in.
TEST(a_formation_claims_each_member_a_cell_the_flood_reached_and_nobody_took) {
  ObstructionGrid grid(64, 64);
  for (std::int32_t x = 40; x <= 47; ++x) {
    for (std::int32_t y = 28; y <= 36; ++y) grid.set_cell(x, y, true);
  }
  for (std::int32_t i = 18; i <= 22; ++i) {
    grid.set_cell(i, 18, true);
    grid.set_cell(i, 22, true);
    grid.set_cell(18, i, true);
    grid.set_cell(22, i, true);
  }
  CellOverlay none;
  none.seal();
  constexpr Point kCentre{520, 520};
  const std::vector<Point> positions(5, Point{100, 900});
  std::vector<Point> stations{Point{525, 470}, Point{712, 520}, Point{600, 600},
                              Point{605, 597}, Point{328, 328}};
  formation_claim_stations(grid, none, kCentre, kClaimRadiusFloor, positions, stations);
  CHECK((stations[0] == Point{525, 470}));
  CHECK((stations[1] == Point{632, 520}));
  CHECK((stations[2] == Point{600, 600}));
  CHECK((stations[3] == Point{584, 584}));
  CHECK((stations[4] == Point{376, 328}));

  // Asked for a radius of 32 the square is 2 cells either side and the flood
  // stops a diagonal short of 2: it reaches the centre's 3 x 3 alone. Of row
  // 32 the square holds x 32..34, and 34 is not reached: 33 costs 0 + 121
  // against the centre's 0 + 144.
  std::vector<Point> near{Point{712, 520}};
  formation_claim_stations(grid, none, kCentre, 32, positions, near);
  CHECK((near[0] == Point{536, 520}));

  // Half the distance from the centre counts: from cell (44, 29), (39, 29)
  // costs 4 + 25 and (39, 30) 3 + 25, so the later row wins.
  std::vector<Point> high{Point{712, 472}};
  formation_claim_stations(grid, none, kCentre, kClaimRadiusFloor, positions, high);
  CHECK((high[0] == Point{632, 488}));

  // A strip with no cell answers below any cost, so it ends the search with
  // what the strips before it wrote. Everything west of column 30 closed,
  // and the box from the centre to cell (30, 40) closed too: the box has
  // nothing, the west strip has nothing, and the member goes to the cell it
  // stands in, though the strips east and south are open.
  ObstructionGrid west(64, 64);
  for (std::int32_t y = 0; y < 64; ++y) {
    for (std::int32_t x = 0; x < 30; ++x) west.set_cell(x, y, true);
  }
  for (std::int32_t x = 30; x <= 32; ++x) {
    for (std::int32_t y = 32; y <= 40; ++y) west.set_cell(x, y, true);
  }
  std::vector<Point> stuck{Point{488, 648}};
  const std::vector<Point> standing{Point{1000, 900}};
  formation_claim_stations(west, none, kCentre, kClaimRadiusFloor, standing, stuck);
  CHECK((stuck[0] == Point{1000, 904}));

  // Nothing open at all.
  ObstructionGrid shut(64, 64);
  for (std::int32_t x = 0; x < 64; ++x) {
    for (std::int32_t y = 0; y < 64; ++y) shut.set_cell(x, y, true);
  }
  std::vector<Point> nowhere{Point{712, 520}};
  formation_claim_stations(shut, none, kCentre, kClaimRadiusFloor, positions, nowhere);
  CHECK((nowhere[0] == Point{104, 904}));

  // The gate bracket's cells are closed for the claim as the grid's are: a
  // line across row 30 from x 0 to 63 (and the row below) cuts the north
  // off, so a station north of it is on a cell nobody has and comes back to
  // the nearest reached cell in the box, row 32 just south of the line.
  CellOverlay line;
  for (std::int32_t x = 0; x < 64; ++x) {
    line.add(x, 30);
    line.add(x, 31);
  }
  line.seal();
  ObstructionGrid open(64, 64);
  std::vector<Point> north{Point{520, 300}};
  formation_claim_stations(open, line, kCentre, kClaimRadiusFloor, positions, north);
  CHECK((north[0] == Point{520, 520}));
  std::vector<Point> bare{Point{520, 300}};
  formation_claim_stations(open, none, kCentre, kClaimRadiusFloor, positions, bare);
  CHECK((bare[0] == Point{520, 300}));
}

/// `PlaceSample` claims the stations it has just placed, with the movement
/// system's grid: a slot whose sample lands in a wall is moved off it before
/// the reassign, and one on open ground keeps its sample.
TEST(placing_a_sample_moves_a_station_that_lands_in_a_wall) {
  ClassGraph graph = layout_graph();
  World world;
  world.set_class_graph(&graph);
  MovementSystem movement;
  movement.set_class_graph(&graph);
  ObstructionGrid grid(128, 128);
  // A wall across rows 56..58 (y 896..943), under the front slot's sample.
  for (std::int32_t x = 0; x < 128; ++x) {
    for (std::int32_t y = 56; y <= 58; ++y) grid.set_cell(x, y, true);
  }
  movement.set_grid(std::move(grid));
  REQUIRE(world.add_system(&movement));
  Result<FormationTable> table = FormationTable::parse(as_bytes(kBlockXml));
  REQUIRE(table.ok());
  const ObjectId hero = world.spawn(NativeClass::hero, nullptr, graph.find("Leader"));
  REQUIRE(world.set_position(hero, Point{1000, 1000}));
  const ObjectId a = world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  const ObjectId b = world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  REQUIRE(world.set_position(a, Point{1000, 1100}));
  REQUIRE(world.set_position(b, Point{1000, 1050}));
  Formation form;
  form.hero = hero;
  formation_add(form, a);
  formation_add(form, b);
  std::vector<ObjectId> joined;
  formation_place_sample(world, form, *table, Point{1000, 1000}, Point{0, -1}, joined);
  REQUIRE(form.slots.size() == 2);
  // The front slot's sample (1000, 903) is in cell (62, 56), in the wall:
  // the box from the centre's cell (62, 62) to it holds rows 59..62 open,
  // and row 59 is cheapest (half of 3 is 1, plus 3 squared): (1000, 952).
  // The back one, (1000, 1097), stays. The reassign then runs on those.
  std::vector<Point> samples{form.slots[0].sample, form.slots[1].sample};
  std::sort(samples.begin(), samples.end(),
            [](Point l, Point r) { return l.y < r.y; });
  CHECK((samples[0] == Point{1000, 952}));
  CHECK((samples[1] == Point{1000, 1097}));
}

/// Joining only marks the layout, and leaving takes the slot out at once
/// (0x005f47e0, 0x005f3290): the layout -- the slots' units in slot order,
/// then the pending ones -- gives each the first slot of its own class, and
/// hands back the pending ones for `form_move`. A reassign then gives each
/// member the slot nearest it relative to the means (0x005f56d0).
TEST(the_roster_waits_for_the_layout_and_the_reassign_matches_where_members_stand) {
  ClassGraph graph = layout_graph();
  World world;
  world.set_class_graph(&graph);
  Result<FormationTable> table = FormationTable::parse(as_bytes(kBlockXml));
  REQUIRE(table.ok());
  const ObjectId hero = world.spawn(NativeClass::hero, nullptr, graph.find("Leader"));
  REQUIRE(world.set_position(hero, Point{1000, 1000}));
  const ObjectId a = world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  const ObjectId b = world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  // `a` stands at the back, `b` at the front of a formation facing -y.
  REQUIRE(world.set_position(a, Point{1000, 1100}));
  REQUIRE(world.set_position(b, Point{1000, 900}));

  Formation form;
  form.hero = hero;
  formation_add(form, a);
  formation_add(form, b);
  CHECK(form.slots.empty());
  CHECK(form.pending.size() == 2);
  CHECK(form.radius == 0);
  CHECK(formation_reach(&form, 45) == 45);

  std::vector<ObjectId> joined;
  REQUIRE(formation_rebuild(world, form, *table, joined));
  CHECK(joined == (std::vector<ObjectId>{a, b}));
  REQUIRE(form.slots.size() == 2);
  // Two in a block, no front line: the front centre, then the back centre.
  CHECK(form.slots[0].offset == (Point{0, -97}));
  CHECK(form.slots[1].offset == (Point{0, 97}));
  CHECK(form.slots[0].unit == a);
  CHECK(form.slots[1].unit == b);
  CHECK(form.radius == 97);
  CHECK(formation_reach(&form, 45) == 97);
  CHECK(form.reassign);
  // Nothing marked, nothing laid.
  joined.clear();
  CHECK(!formation_rebuild(world, form, *table, joined));

  // The sample, facing -y: the reassign puts `b`, in front, on the front slot.
  formation_place_sample(world, form, *table, Point{1000, 1000}, Point{0, -1}, joined);
  CHECK(form.slots[0].sample == (Point{1000, 903}));
  CHECK(form.slots[1].sample == (Point{1000, 1097}));
  CHECK(form.slots[0].unit == b);
  CHECK(form.slots[1].unit == a);
  CHECK(!form.reassign);

  // Leaving takes the slot out and marks the layout; the radius stands.
  CHECK(formation_remove(form, b));
  CHECK(form.slots.size() == 1);
  CHECK(form.died == 1);
  CHECK(form.rebuild);
  CHECK(form.radius == 97);
  CHECK(!formation_remove(form, b));
}

// --------------------------------------------------------------------------
// the host slice
// --------------------------------------------------------------------------
//
// Called through the registry rather than through the VM: the entry points are
// plain function pointers and a `CallContext` is plain data, so a test can
// exercise the whole movement slice without compiling a script. The VM's own
// dispatch is `test_vs_vm.cpp`'s subject and does not need testing twice.

namespace {

/// One prepared host call: the argument window, and the world behind it.
struct HostCall {
  std::vector<Value> arguments;
  CallContext context;
  // Owned here, not borrowed: `CallContext::user` is a `HostContext*` for every
  // domain now, and a raw `World*` behind it is the exact confusion
  // sim/host_context.hpp exists to make impossible.
  imperivm::core::sim::HostContext context_state;

  /// The call's word of state (`CallContext::scratch`), as the VM keeps it.
  std::int32_t scratch = 0;

  HostCall(World& world, std::initializer_list<Value> args) : arguments(args) {
    context_state.world = &world;
    context.arguments = arguments;
    context.user = &context_state;
    context.scratch = &scratch;
  }

  /// Run the same call again after its wait, as the VM does after a `retry`.
  HostCall& reenter() {
    context.first_call = false;
    return *this;
  }
};

HostOutcome invoke(const HostRegistry& registry, CallKind kind, std::string_view name,
                   std::uint16_t arity, HostCall& call) {
  const std::uint32_t index = registry.find(kind, name, arity);
  if (index == kUnresolvedHost) return HostOutcome::failed("not registered");
  const HostEntry& entry = registry.entry(index);
  if (entry.fn == nullptr) return HostOutcome::failed("not implemented");
  return entry.fn(call.context);
}

}  // namespace

TEST(the_movement_host_slice_is_wired) {
  HostRegistry registry;
  register_movement_host(registry);
  // Every entry point the slice claims, at the arity the inventory records.
  const struct {
    CallKind kind;
    const char* name;
    std::uint16_t arity;
  } expected[] = {
      {CallKind::member, "Goto", 4},        {CallKind::member, "Goto", 5},
      {CallKind::member, "Face", 1},        {CallKind::member, "SetSpeedFactor", 1},
      {CallKind::member, "SetWalkAnim", 0}, {CallKind::member, "SetWalkAnim", 1},
      {CallKind::member, "DoCarryNothing", 0},
      {CallKind::member, "SetCarryWaterAnim", 0}, {CallKind::member, "SetCarryGoodsAnim", 0},
      {CallKind::member, "Stop", 1},        {CallKind::member, "HasPath", 0},
      {CallKind::member, "PathTo", 3},      {CallKind::member, "PathDestFound", 0},
      {CallKind::member, "ClipDestToMap", 1}, {CallKind::member, "TimeWithoutWalking", 0},
      {CallKind::member, "GetDir", 0},      {CallKind::member, "SetPos", 1},
      {CallKind::member, "SetFormation", 1}, {CallKind::member, "dest", 0},
      {CallKind::member, "speed", 0},       {CallKind::member, "formation", 0},
      {CallKind::member, "FormRadius", 0},
      {CallKind::free_function, "IsPassable3x3", 1},
      {CallKind::free_function, "FormDescription", 1},
  };
  for (const auto& entry : expected) {
    const std::uint32_t index = registry.find(entry.kind, entry.name, entry.arity);
    CHECK(index != kUnresolvedHost);
    if (index != kUnresolvedHost) CHECK(registry.entry(index).fn != nullptr);
  }
}

TEST(form_description_is_the_stand_ground_rollover) {
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);
  // The fixture's three formations, plus the two shapes the shipped file never
  // has: a pierce-only defence bonus, and a negative one.
  constexpr std::string_view kXml = R"(<Formations>
  <Default Name="Front"/>
  <FormationClass Name="Front" BonusLevel="4"/>
  <FormationClass Name="Line" BonusDamage="2" BonusArmor="2" BonusRange="20"/>
  <FormationClass Name="Turtle" BonusDefencePierce="3"/>
  <FormationClass Name="Rout" BonusLevel="-1" BonusAttack="4"/>
  <FormationClass Name="Plain"/>
  <FormationClass Name="Nudge" BonusRange="1"/>
</Formations>)";
  Result<FormationTable> table = FormationTable::parse(as_bytes(kXml));
  REQUIRE(table.ok());
  movement.set_formations(std::move(table.value()));

  const auto describe = [&](const char* name, const game::TranslationTable* translations) {
    HostCall call(world, {Value::string(name)});
    call.context_state.translations = translations;
    const HostOutcome out = invoke(registry, CallKind::free_function, "FormDescription", 1, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.is_string() ? out.value.as_string() : std::string("<not a string>");
  };

  constexpr std::string_view kHeading = "Army bonus during stand ground\n";
  constexpr std::string_view kLevel = "<imagetransp gameres/infobar/common/level_ico.bmp>";
  constexpr std::string_view kDamage = "<imagetransp gameres/infobar/common/atack ico.bmp>";
  constexpr std::string_view kDefence = "<imagetransp gameres/infobar/common/defense ico.bmp>";
  constexpr std::string_view kRange = "<imagetransp gameres/infobar/common/piercing ico.bmp>";

  // One group: icon, plus sign, value, space.
  CHECK(describe("Front", nullptr) == std::string(kHeading) + std::string(kLevel) + "+4 ");
  // Three groups in the fixed order, and the range one is a percentage under
  // the piercing icon.
  CHECK(describe("Line", nullptr) == std::string(kHeading) + std::string(kDamage) + "+2 " +
                                         std::string(kDefence) + "+2 " + std::string(kRange) +
                                         "+20% ");
  // The quirk: a pierce bonus opens the defence group, and the slash value is
  // what gets printed.
  CHECK(describe("Turtle", nullptr) == std::string(kHeading) + std::string(kDefence) + "0 ");
  // A negative value carries its own sign; `BonusAttack` is invisible.
  CHECK(describe("Rout", nullptr) == std::string(kHeading) + std::string(kLevel) + "-1 ");
  // No bonuses is the heading alone, not the empty string.
  CHECK(describe("Plain", nullptr) == std::string(kHeading));
  // The sign test is `> 0`, so a bonus of exactly one still gets its plus. A
  // fault that made it `> 1` survived a sweep until this line existed.
  CHECK(describe("Nudge", nullptr) == std::string(kHeading) + std::string(kRange) + "+1% ");
  // An unknown formation is the empty string, not the heading and not an error.
  CHECK(describe("Nonesuch", nullptr).empty());

  // The heading goes through the language pack when there is one.
  constexpr std::string_view kTable =
      "<translationtable><translationtableentry text=\"Army bonus during stand ground\" "
      "justtext=\"Army bonus during stand ground\" context=\"\" result=\"Bonus\" "
      "comment=\"\"/></translationtable>";
  const auto translations = game::TranslationTable::parse(as_bytes(kTable));
  REQUIRE(translations.ok());
  CHECK(describe("Front", &translations.value()) == "Bonus\n" + std::string(kLevel) + "+4 ");

  // Not a string is refused, as `SetFormation` refuses it.
  HostCall bad(world, {Value::integer(3)});
  CHECK(invoke(registry, CallKind::free_function, "FormDescription", 1, bad).status ==
        HostStatus::error);
}

TEST(a_host_goto_lays_a_path_and_suspends) {
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{100, 100}, 100);

  // `.Goto(pt, 0, 2000, true, 0)`, exactly as `DEER_IDLE.VS` writes it.
  HostCall call(world, {Value::object(kTypeObj, unit), pack_point(Point{500, 100}),
                        Value::integer(0), Value::integer(2000), Value::boolean(true),
                        Value::integer(0)});
  const HostOutcome first = invoke(registry, CallKind::member, "Goto", 5, call);
  // The first run lays the route and waits; the answer is its re-entry's.
  CHECK(first.status == HostStatus::retry);
  CHECK(first.suspend_for == 2000);
  CHECK(call.scratch == 1);  // it laid a route

  const MoveState* state = movement.find(unit);
  REQUIRE(state != nullptr);
  CHECK(state->has_path);
  CHECK(state->target == (Point{500, 100}));
  CHECK(movement.eta(unit) == 4000);

  // A slice longer than the journey is waited whole all the same
  // (0x005d65e8): the script goes on when the slice runs out, not when the
  // unit arrives.
  HostCall generous(world, {Value::object(kTypeObj, unit), pack_point(Point{500, 100}),
                            Value::integer(0), Value::integer(20000), Value::boolean(true),
                            Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "Goto", 5, generous).suspend_for == 20000);
  // And `SyncFlags` bit 17 is on the object, where the world hash sees it.
  REQUIRE(world.state(unit) != nullptr);
  CHECK(world.state(unit)->flags.has_active_path);

  HostCall has_path(world, {Value::object(kTypeObj, unit)});
  CHECK(invoke(registry, CallKind::member, "HasPath", 0, has_path).value.as_integer() == 1);

  run(world, movement, {2000, 2000});
  CHECK(position_of(world, unit) == (Point{500, 100}));

  // Called again on arrival it reports success and does not suspend, which is
  // what ends `while (!.Goto(...) && .HasPath)`.
  HostCall arrived(world, {Value::object(kTypeObj, unit), pack_point(Point{500, 100}),
                           Value::integer(0), Value::integer(2000), Value::boolean(true),
                           Value::integer(0)});
  const HostOutcome second = invoke(registry, CallKind::member, "Goto", 5, arrived);
  CHECK(second.status == HostStatus::ok);
  CHECK(second.value.as_integer() == 1);
  CHECK(!world.state(unit)->flags.has_active_path);
}

namespace {

/// A walker of radius 15 and a camp of radius 100, for the edge-to-edge tests.
struct EdgeField {
  ClassGraph graph;
  World world;
  MovementSystem movement;
  HostRegistry registry;

  EdgeField() {
    graph.add(as_bytes(R"(<class id="Walker" cpp_class="CVXUnit" parent="">
      <properties speed="100" radius="15"/></class>)"),
              "walker.sc.xml");
    graph.add(as_bytes(R"(<class id="Camp" cpp_class="CVXUnit" parent="">
      <properties speed="0" radius="100"/></class>)"),
              "camp.sc.xml");
    graph.add(as_bytes(R"(<class id="Bare" cpp_class="CVXUnit" parent="">
      <properties speed="100"/></class>)"),
              "bare.sc.xml");
    // A radius below zero, which is what an undeclared number reads as in the
    // original's class constructor; it must take nothing off, not add to it.
    graph.add(as_bytes(R"(<class id="Sunk" cpp_class="CVXUnit" parent="">
      <properties speed="100" radius="-40"/></class>)"),
              "sunk.sc.xml");
    graph.link();
    world.set_class_graph(&graph);
    movement.set_class_graph(&graph);
    movement.set_grid(open_field(128));
    attach(world, movement);
    register_movement_host(registry);
    (void)register_world_host(registry);
  }

  ObjectId place(const char* class_name, Point where) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, graph.find(class_name));
    CHECK(world.set_position(id, where));
    movement.state(id).speed = class_name == std::string_view("Camp") ? 0 : 100;
    return id;
  }

  std::int32_t dist_to(ObjectId self, const Value& other) {
    HostCall call(world, {Value::object(kTypeObj, self), other});
    const HostOutcome out = invoke(registry, CallKind::member, "DistTo", 1, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -999999;
  }

  HostOutcome go(ObjectId self, const Value& where, std::int32_t range) {
    HostCall call(world, {Value::object(kTypeObj, self), where, Value::integer(range),
                          Value::integer(751), Value::boolean(true), Value::integer(0)});
    return invoke(registry, CallKind::member, "Goto", 5, call);
  }
};

}  // namespace

// `Obj::DistTo` is two registrations in `gbr.exe`: to a point (0x005aa350) the
// distance less the receiver's class radius, to an object (0x005aa3e0, through
// 0x005a77b0) the distance less both radii. Both can go negative.
TEST(dist_to_measures_from_the_edges_not_the_centres) {
  EdgeField f;
  const ObjectId walker = f.place("Walker", Point{1000, 1000});
  const ObjectId camp = f.place("Camp", Point{1300, 1000});
  const ObjectId bare = f.place("Bare", Point{1000, 1400});

  // 300 between centres, less 15 and 100.
  CHECK(f.dist_to(walker, Value::object(kTypeObj, camp)) == 185);
  CHECK(f.dist_to(camp, Value::object(kTypeObj, walker)) == 185);
  // To a point, only the receiver's own radius comes off.
  CHECK(f.dist_to(walker, pack_point(Point{1300, 1000})) == 285);
  CHECK(f.dist_to(camp, pack_point(Point{1000, 1000})) == 200);
  // Standing on the camp's own anchor is inside it: negative, as the original.
  const ObjectId inside = f.place("Walker", Point{1300, 1000});
  CHECK(f.dist_to(inside, Value::object(kTypeObj, camp)) == -115);
  CHECK(f.dist_to(inside, pack_point(Point{1300, 1000})) == -15);
  // A class that declares no radius takes nothing off, rather than its
  // constructor's negative sentinel.
  CHECK(f.dist_to(bare, pack_point(Point{1000, 1000})) == 400);
  CHECK(f.dist_to(bare, Value::object(kTypeObj, walker)) == 385);
  const ObjectId sunk = f.place("Sunk", Point{1000, 1600});
  CHECK(f.dist_to(sunk, pack_point(Point{1000, 1000})) == 600);
  CHECK(f.dist_to(walker, Value::object(kTypeObj, sunk)) == 585);
}

// And `Goto` to an object measures the same gap (0x00417c10, route mode 1),
// so a unit sent to a camp with range 0 stops where the camp begins rather
// than on its anchor -- which is where `UNIT_CAPTURE.VS`'s mirror walk used to
// find it, aiming at its own feet and never yielding.
/// **The answer comes at the re-entry** (0x005d61b0 at 0x005d6340): run
/// again after its wait, the call measures nothing new and lays nothing new.
/// Not arrived, it answers false and keeps the route, so the script's loop
/// calls again; arrived, it answers true and drops the route (0x005d3830
/// with 0). A first run that laid no route answers false at its re-entry
/// whatever, and a re-entry that finds no order -- something stopped the unit
/// during the wait -- runs as a first run. With the answer given before the
/// wait, the loop's next call was what found the unit arrived.
TEST(a_host_goto_answers_at_its_reentry) {
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{100, 100}, 100);

  HostCall call(world, {Value::object(kTypeObj, unit), pack_point(Point{500, 100}),
                        Value::integer(0), Value::integer(2000), Value::boolean(true),
                        Value::integer(0)});
  REQUIRE(invoke(registry, CallKind::member, "Goto", 5, call).status == HostStatus::retry);
  run(world, movement, {2000});
  // Half way: false, and still walking.
  const HostOutcome midway = invoke(registry, CallKind::member, "Goto", 5, call.reenter());
  CHECK(midway.status == HostStatus::ok);
  CHECK(midway.value.as_integer() == 0);
  CHECK(movement.find(unit)->has_path);
  CHECK(movement.find(unit)->goto_active);

  // The loop calls again: a first run, which waits the slice once more.
  HostCall again(world, {Value::object(kTypeObj, unit), pack_point(Point{500, 100}),
                         Value::integer(0), Value::integer(2000), Value::boolean(true),
                         Value::integer(0)});
  REQUIRE(invoke(registry, CallKind::member, "Goto", 5, again).status == HostStatus::retry);
  run(world, movement, {2000});
  CHECK(position_of(world, unit) == (Point{500, 100}));
  const HostOutcome there = invoke(registry, CallKind::member, "Goto", 5, again.reenter());
  CHECK(there.status == HostStatus::ok);
  CHECK(there.value.as_integer() == 1);
  CHECK(!movement.find(unit)->goto_active);  // the order is dropped

  // Stopped during the wait: the re-entry finds no order and runs as a first
  // run -- here, already arrived, so true at once.
  HostCall third(world, {Value::object(kTypeObj, unit), pack_point(Point{700, 100}),
                         Value::integer(0), Value::integer(2000), Value::boolean(true),
                         Value::integer(0)});
  REQUIRE(invoke(registry, CallKind::member, "Goto", 5, third).status == HostStatus::retry);
  movement.stop(world, unit);
  const HostOutcome rerun = invoke(registry, CallKind::member, "Goto", 5, third.reenter());
  CHECK(rerun.status == HostStatus::retry);  // a first run again: laid and waiting
  CHECK(movement.find(unit)->goto_active);

  // A first run that laid no route answers false at its re-entry, even if
  // the unit is somehow in the band by then.
  movement.stop(world, unit);
  HostCall blocked(world, {Value::object(kTypeObj, unit), pack_point(Point{500, 100}),
                           Value::integer(1000), Value::integer(2000), Value::boolean(true),
                           Value::integer(-1)});
  blocked.scratch = 0;
  (void)movement.order_goto(world, unit, Point{900, 100}, 0, 0, kNoObject, true);
  const HostOutcome unlaid = invoke(registry, CallKind::member, "Goto", 5, blocked.reenter());
  CHECK(unlaid.status == HostStatus::ok);
  CHECK(unlaid.value.as_integer() == 0);
}

TEST(an_object_goto_arrives_at_the_edge_and_a_point_goto_at_the_point) {
  EdgeField f;
  const ObjectId walker = f.place("Walker", Point{1000, 1000});
  const ObjectId camp = f.place("Camp", Point{1600, 1000});

  const HostOutcome first = f.go(walker, Value::object(kTypeObj, camp), 0);
  CHECK(first.status == HostStatus::retry);
  // Short turns, because arrival is tested where a turn ends.
  for (int i = 0; i < 100; ++i) run(f.world, f.movement, {100});
  const Point stood = position_of(f.world, walker);
  // Stopped with the edges touching, 115 short of the anchor, not on it.
  CHECK(stood.x >= 1600 - 115 && stood.x <= 1600 - 115 + 10);
  CHECK(f.dist_to(walker, Value::object(kTypeObj, camp)) <= 0);
  CHECK(f.dist_to(walker, Value::object(kTypeObj, camp)) >= -10);
  // Asked again, it has arrived and says so without suspending.
  const HostOutcome again = f.go(walker, Value::object(kTypeObj, camp), 0);
  CHECK(again.status == HostStatus::ok);
  CHECK(again.value.as_integer() == 1);
  // The same range against the camp's *point* is centre to centre and walks on.
  const HostOutcome to_point = f.go(walker, pack_point(Point{1600, 1000}), 0);
  CHECK(to_point.status == HostStatus::retry);
  CHECK(to_point.value.as_integer() == 0);

  // A range adds to the gap, it does not replace it: 50 more out.
  const ObjectId other = f.place("Walker", Point{1000, 1800});
  const ObjectId target = f.place("Camp", Point{1600, 1800});
  (void)f.go(other, Value::object(kTypeObj, target), 50);
  for (int i = 0; i < 100; ++i) run(f.world, f.movement, {100});
  const Point there = position_of(f.world, other);
  CHECK(there.x >= 1600 - 165 && there.x <= 1600 - 165 + 10);

  // A negative radius takes nothing off the gap: the sunk mover stops at the
  // camp's 100, not 60 from its anchor.
  const ObjectId sunk = f.place("Sunk", Point{1000, 200});
  const ObjectId far_camp = f.place("Camp", Point{1600, 200});
  (void)f.go(sunk, Value::object(kTypeObj, far_camp), 0);
  for (int i = 0; i < 100; ++i) run(f.world, f.movement, {100});
  const Point sank = position_of(f.world, sunk);
  CHECK(sank.x >= 1600 - 100 && sank.x <= 1600 - 100 + 10);
}

namespace {

/// A unit walled into a single cell: no route out exists at all.
ObstructionGrid sealed_cell(std::int32_t cx, std::int32_t cy) {
  ObstructionGrid grid = open_field(32);
  for (std::int32_t d = -1; d <= 1; ++d) {
    grid.set_cell(cx + d, cy - 1, true);
    grid.set_cell(cx + d, cy + 1, true);
    grid.set_cell(cx - 1, cy + d, true);
    grid.set_cell(cx + 1, cy + d, true);
  }
  return grid;
}

/// `.Goto(dest, 0, slice, true, give_up)` on `unit`.
HostOutcome goto_once(const HostRegistry& registry, World& world, ObjectId unit, Point dest,
                      std::int32_t slice, std::int32_t give_up) {
  HostCall call(world, {Value::object(kTypeObj, unit), pack_point(dest), Value::integer(0),
                        Value::integer(slice), Value::boolean(true), Value::integer(give_up)});
  return invoke(registry, CallKind::member, "Goto", 5, call);
}

}  // namespace

TEST(a_host_goto_into_a_wall_reports_no_path) {
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(sealed_cell(9, 10));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{9 * 16 + 8, 10 * 16 + 8}, 100);

  // With a negative give-up the call never ends the script.
  const HostOutcome outcome = goto_once(registry, world, unit, Point{2000, 2000}, 2000, -1);
  // False *and* no path, so `while (!.Goto(...) && .HasPath)` leaves.
  CHECK(outcome.value.as_integer() == 0);
  HostCall has_path(world, {Value::object(kTypeObj, unit)});
  CHECK(invoke(registry, CallKind::member, "HasPath", 0, has_path).value.as_integer() == 0);

  // And it **waits** rather than answering instantly, which the loops that have
  // no `.HasPath` guard need: an immediate `ok` in `while (!.Goto(pt, 0, 2000,
  // true, -1));` spins the VM against an unreachable point until the
  // instruction budget traps it. The original waits the slice too (0x005d65e8).
  CHECK(outcome.status == HostStatus::retry);
  CHECK(outcome.suspend_for == 2000);
}

namespace {

/// A unit's art for the `Goto` family's animation half: slot 20 is the
/// "look around" (900), slot 19 the 1-to-2 lead-in (300), slot 18 the 2-to-1
/// edge back (120), slot 13 a fidget.
constexpr std::string_view kLookerEntity = R"(<?xml version="1.0"?>
<entity name="looker" type="vx/unit" variations="8">
  <images>
    <image idx="1" file="idle.rle" drawmode="player_color" remaping="none" rows="4" columns="8"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="1" anim_idx="65536" anim_frame="65536"/>
    <state idx="2" name="attack" image_idx="1" image_row="0" anim_idx="65536" anim_frame="65536"/>
  </states>
  <anims>
    <anim idx="13" name="idle" startstate="1" endstate="1" frames="3" duration="200"
          default_duration="0" action_time="0" step="0">
      <frame idx="1" duration="100"/><frame idx="2" duration="100"/><frame idx="3" duration="0"/>
    </anim>
    <anim idx="18" name="ToIdle" startstate="2" endstate="1" frames="3" duration="120"
          default_duration="0" action_time="0" step="0">
      <frame idx="1" duration="60"/><frame idx="2" duration="60"/><frame idx="3" duration="0"/>
    </anim>
    <anim idx="19" name="ToAttack" startstate="1" endstate="2" frames="4" duration="300"
          default_duration="0" action_time="100" step="0">
      <frame idx="1" duration="100"/><frame idx="2" duration="100"/><frame idx="3" duration="100"/>
      <frame idx="4" duration="0"/>
    </anim>
    <anim idx="20" name="look" startstate="1" endstate="1" frames="4" duration="900"
          default_duration="0" action_time="0" step="0">
      <frame idx="1" duration="300"/><frame idx="2" duration="300"/><frame idx="3" duration="300"/>
      <frame idx="4" duration="0"/>
    </anim>
  </anims>
</entity>)";

[[nodiscard]] std::span<const std::byte> looker_bytes() {
  return {reinterpret_cast<const std::byte*>(kLookerEntity.data()), kLookerEntity.size()};
}

}  // namespace

/// **With no route laid, the family's flag plays the look-around** (0x005d65a0
/// in `Goto`, the same in `GotoEnter` and the object worker 0x005d40c0): the
/// action mode 0, the entity's animation index 0x13 (the script's slot 20)
/// started towards the goal, and the wait the longer of the slice and its
/// length. Without the flag, a unit resting anywhere but its first state is
/// put back there with a transition (0x0053e860), and the wait is the slice.
TEST(a_goto_that_lays_no_route_looks_around_or_settles) {
  const auto entity = imperivm::core::Entity::parse(looker_bytes());
  REQUIRE(entity.ok());
  HostRegistry registry;
  register_movement_host(registry);
  World world;
  MovementSystem movement;
  movement.set_grid(sealed_cell(9, 10));
  attach(world, movement);
  const Point cell{9 * 16 + 8, 10 * 16 + 8};
  const ObjectId unit = world.spawn(NativeClass::unit, &entity.value());
  REQUIRE(world.set_position(unit, cell));
  movement.state(unit).speed = 100;
  world.set_anim_mode(unit, 4);

  HostCall look(world, {Value::object(kTypeObj, unit), pack_point(Point{900, 160}),
                        Value::integer(0), Value::integer(500), Value::boolean(true),
                        Value::integer(-1)});
  const HostOutcome looked = invoke(registry, CallKind::member, "Goto", 5, look);
  CHECK(looked.status == HostStatus::retry);
  CHECK(looked.suspend_for == 900);
  CHECK(world.find(unit)->clock.slot == 20);
  CHECK(world.find(unit)->clock.mode == 0);
  // Turned towards the goal, east.
  CHECK(movement.find(unit)->facing.x > 0);
  // A slice longer than the animation is the wait.
  movement.stop(world, unit);
  HostCall longer(world, {Value::object(kTypeObj, unit), pack_point(Point{900, 160}),
                          Value::integer(0), Value::integer(2000), Value::boolean(true),
                          Value::integer(-1)});
  CHECK(invoke(registry, CallKind::member, "Goto", 5, longer).suspend_for == 2000);

  // Without the flag: the lead-in has left the unit in state 2, and the call
  // starts the 2-to-1 edge back.
  world.advance(1000);
  REQUIRE(world.start_animation(unit, 19, world.time()) == 300);
  world.advance(300);
  REQUIRE(world.anim_rest_state_at(*world.find(unit), world.time()) == 2);
  movement.stop(world, unit);
  HostCall plain(world, {Value::object(kTypeObj, unit), pack_point(Point{900, 160}),
                         Value::integer(0), Value::integer(500), Value::boolean(false),
                         Value::integer(-1)});
  const HostOutcome settled = invoke(registry, CallKind::member, "Goto", 5, plain);
  CHECK(settled.suspend_for == 500);
  CHECK(world.find(unit)->clock.slot == 18);

  // Resting in the first state already, nothing starts.
  world.advance(200);
  movement.stop(world, unit);
  HostCall rest(world, {Value::object(kTypeObj, unit), pack_point(Point{900, 160}),
                        Value::integer(0), Value::integer(500), Value::boolean(false),
                        Value::integer(-1)});
  CHECK(invoke(registry, CallKind::member, "Goto", 5, rest).suspend_for == 500);
  CHECK(world.anim_slot_at(*world.find(unit), world.time()) == kNoAnim);
}

/// **A route laid sets the walk's action mode** (3) and arms the animation
/// timer for 1 ms when it is not pending (0x005d65c0); **an arrival at the
/// first run with an animation still playing waits for its end** and answers
/// true at the re-entry (0x005d6482 .. 0x005d64a8).
TEST(a_goto_walks_in_mode_three_and_an_arrival_waits_out_the_animation) {
  const auto entity = imperivm::core::Entity::parse(looker_bytes());
  REQUIRE(entity.ok());
  HostRegistry registry;
  register_movement_host(registry);
  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);
  const ObjectId unit = world.spawn(NativeClass::unit, &entity.value());
  REQUIRE(world.set_position(unit, Point{100, 100}));
  movement.state(unit).speed = 100;

  HostCall walk(world, {Value::object(kTypeObj, unit), pack_point(Point{500, 100}),
                        Value::integer(0), Value::integer(2000), Value::boolean(true),
                        Value::integer(-1)});
  CHECK(invoke(registry, CallKind::member, "Goto", 5, walk).suspend_for == 2000);
  CHECK(world.find(unit)->clock.mode == 3);
  CHECK(world.anim_timer_left(*world.find(unit), world.time()) == 1);
  movement.stop(world, unit);

  // Standing at the goal with the lead-in 100 ms in: wait the other 200.
  REQUIRE(world.start_animation(unit, 19, world.time()) == 300);
  world.advance(100);
  const Point here = world.resolve_position(unit);
  HostCall there(world, {Value::object(kTypeObj, unit), pack_point(here), Value::integer(50),
                         Value::integer(2000), Value::boolean(true), Value::integer(-1)});
  const HostOutcome waiting = invoke(registry, CallKind::member, "Goto", 5, there);
  CHECK(waiting.status == HostStatus::retry);
  CHECK(waiting.suspend_for == 200);
  world.advance(200);
  const HostOutcome arrived = invoke(registry, CallKind::member, "Goto", 5, there.reenter());
  CHECK(arrived.status == HostStatus::ok);
  CHECK(arrived.value.as_integer() == 1);

  // With nothing playing, true at once.
  HostCall still(world, {Value::object(kTypeObj, unit), pack_point(here), Value::integer(50),
                         Value::integer(2000), Value::boolean(true), Value::integer(-1)});
  const HostOutcome at_once = invoke(registry, CallKind::member, "Goto", 5, still);
  CHECK(at_once.status == HostStatus::ok);
  CHECK(at_once.value.as_integer() == 1);
}

TEST(a_host_goto_with_a_zero_give_up_ends_the_script_at_the_first_failed_search) {
  // `UNIT_ADVANCE.VS` and `UNIT_MOVE.VS` pass 0. In `gbr.exe` (0x005d61b0) a
  // call that lays no route and is not arrived stamps the failure, and once
  // the give-up has elapsed since -- at once, for 0 -- returns 2 and the
  // script ends there. Read as "no limit", a unit with nowhere to go stayed on
  // its order for the rest of the match.
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(sealed_cell(9, 10));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{9 * 16 + 8, 10 * 16 + 8}, 100);

  const HostOutcome outcome = goto_once(registry, world, unit, Point{2000, 2000}, 2000, 0);
  CHECK(outcome.status == HostStatus::finish);
  CHECK(!movement.find(unit)->goto_active);
  CHECK(movement.find(unit)->goto_failed_at == world.time());
}

TEST(a_host_goto_times_the_failure_and_not_the_walk) {
  // `UNIT_ENTER.VS`'s `while (!.Goto(pt, 0, 1000, true, 5000));`, and the
  // 2,000 ms give-up below, against a walk of four seconds that ends at a
  // wall. Read as a total timeout the order stopped after two seconds of
  // walking; in the original nothing times a walk, and the clock starts when
  // the route runs out and the search from there lays none.
  HostRegistry registry;
  register_movement_host(registry);

  // A wall across x = 30 cells, the whole height of the field.
  ObstructionGrid grid = open_field(64);
  for (std::int32_t y = 0; y < 64; ++y) grid.set_cell(30, y, true);
  World world;
  MovementSystem movement;
  movement.set_grid(std::move(grid));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{10 * 16 + 8, 20 * 16 + 8}, 100);
  const Point beyond{40 * 16 + 8, 20 * 16 + 8};

  bool ended = false;
  GameTime last_route = -1;
  GameTime ended_at = -1;
  for (int i = 0; i < 60 && !ended; ++i) {
    const HostOutcome out = goto_once(registry, world, unit, beyond, 500, 2000);
    CHECK(out.status == HostStatus::retry || out.status == HostStatus::finish);
    CHECK(out.value.as_integer() == 0);
    if (movement.find(unit)->has_path) last_route = world.time();
    ended = out.status == HostStatus::finish;
    if (ended) ended_at = world.time();
    run(world, movement, {500});
  }
  REQUIRE(ended);
  // It walked for longer than the give-up before the route ran out ...
  CHECK(last_route > 2000);
  // ... reached the wall ...
  CHECK(position_of(world, unit).x >= 28 * 16);
  // ... and ended two seconds after the last route, to the half-second call.
  CHECK(ended_at - last_route >= 2000);
  CHECK(ended_at - last_route <= 2500);
}

TEST(a_host_goto_keeps_its_failure_stamp_across_orders_until_a_route_is_laid) {
  // The stamp is the unit's (`[unit+0x150]`), not the order's: a new
  // destination does not clear it, a laid route does (0x005d6528).
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(sealed_cell(9, 10));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{9 * 16 + 8, 10 * 16 + 8}, 100);

  CHECK(goto_once(registry, world, unit, Point{2000, 2000}, 1000, -1).status ==
        HostStatus::retry);
  const GameTime stamped = world.time();
  CHECK(movement.find(unit)->goto_failed_at == stamped);
  world.advance(3000);
  // Somewhere else, with a give-up of 2,500: three seconds of failing are
  // already behind it, so the script ends at this call.
  CHECK(goto_once(registry, world, unit, Point{100, 2000}, 1000, 2500).status ==
        HostStatus::finish);
  CHECK(movement.find(unit)->goto_failed_at == stamped);

  // The wall comes down and a route is laid: the stamp clears.
  movement.set_grid(open_field(32));
  CHECK(goto_once(registry, world, unit, Point{400, 160}, 1000, 0).status ==
        HostStatus::retry);
  CHECK(movement.find(unit)->has_path);
  CHECK(movement.find(unit)->goto_failed_at == kNoGotoFailure);
}

TEST(a_failing_host_goto_draws_and_searches_less_often) {
  // 0x005d2eb0: with the stamp set, each call draws `rand(0, 2n)` from the
  // world's generator, `n` the whole 2,048 ms spans since the stamp, at most
  // 10; anything but 0 tells `SetDest` not to search the goal it already has.
  // The wall is taken away under a unit still failing, so whether a call
  // searched shows as whether it now has a route.
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(sealed_cell(9, 10));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{9 * 16 + 8, 10 * 16 + 8}, 100);
  const Point away{25 * 16 + 8, 10 * 16 + 8};

  // The first failure stamps and draws nothing: the stamp was clear.
  const Rng clear = world.rng();
  (void)goto_once(registry, world, unit, away, 1000, -1);
  CHECK(world.rng() == clear);
  const GameTime stamped = movement.find(unit)->goto_failed_at;
  REQUIRE(stamped == world.time());

  // Inside the first 2,048 ms the draw is `rand(0, 0)`: taken, and it always
  // searches.
  world.advance(1000);
  Rng expect = world.rng();
  (void)expect.between(0, 0);
  (void)goto_once(registry, world, unit, away, 1000, -1);
  CHECK(world.rng() == expect);
  CHECK(!movement.find(unit)->has_path);

  // Six seconds on: `rand(0, 4)` and up. The field opens just before the
  // first call whose draw is not 0, which must not search; the first call
  // after that whose draw is 0 must. Every call takes exactly its draw.
  //
  // The draw binds only with neither of the follower's stop bits set
  // (0x0041a4da): a search that laid nothing leaves bit 2, and then the next
  // call searches whatever it drew (the test after this one). So each call
  // here starts as a unit whose route has been walked to its end does -- no
  // route, no stop bit, the order still on.
  world.advance(5000);
  bool opened = false;
  bool skipped = false;
  bool searched = false;
  for (int i = 0; i < 80 && !searched; ++i) {
    const std::int64_t spans = std::min<std::int64_t>((world.time() - stamped) >> 11, 10);
    expect = world.rng();
    const bool draws_zero = expect.between(0, static_cast<std::int32_t>(2 * spans)) == 0;
    if (!opened && !draws_zero) {
      movement.set_grid(open_field(32));
      opened = true;
    }
    movement.find(unit)->route_failed = false;
    (void)goto_once(registry, world, unit, away, 100, -1);
    CHECK(world.rng() == expect);
    searched = movement.find(unit)->has_path;
    CHECK(searched == (opened && draws_zero));
    skipped = skipped || (opened && !searched);
    world.advance(100);
  }
  CHECK(skipped);
  CHECK(searched);
  CHECK(movement.find(unit)->goto_failed_at == kNoGotoFailure);
}

TEST(a_search_that_laid_nothing_searches_again_at_the_next_call_whatever_the_draw) {
  // `SetDest` tests the path follower's stop bits before it compares the goal
  // (the stop-bit test at 0x0041a4da, 0x0041a5da) and takes its new-goal branch
  // when one is set: the search runs whatever the draw before it said, and the
  // free-spot search's flag 8 is forgotten (0x0041a594). A search that laid
  // nothing leaves bit 2 (0x00419a1e, `MoveState::route_failed`).
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(sealed_cell(9, 10));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{9 * 16 + 8, 10 * 16 + 8}, 100);
  const Point away{25 * 16 + 8, 10 * 16 + 8};

  (void)goto_once(registry, world, unit, away, 1000, -1);
  const GameTime stamped = movement.find(unit)->goto_failed_at;
  REQUIRE(stamped == world.time());
  REQUIRE(movement.find(unit)->route_failed);

  // Far enough on that the draw is `rand(0, 4)` or more; wait for a call
  // whose draw is not 0 and open the field just before it.
  world.advance(6000);
  bool tested = false;
  for (int i = 0; i < 80 && !tested; ++i) {
    const std::int64_t spans = std::min<std::int64_t>((world.time() - stamped) >> 11, 10);
    Rng expect = world.rng();
    const bool draws_zero = expect.between(0, static_cast<std::int32_t>(2 * spans)) == 0;
    if (draws_zero) {
      // Still sealed: the search runs and fails again, and the bit stays.
      (void)goto_once(registry, world, unit, away, 100, -1);
      CHECK(world.rng() == expect);
      CHECK(movement.find(unit)->route_failed);
      world.advance(100);
      continue;
    }
    movement.set_grid(open_field(32));
    movement.find(unit)->free_spot_tried = true;
    (void)goto_once(registry, world, unit, away, 100, -1);
    // The draw is taken all the same, and ignored.
    CHECK(world.rng() == expect);
    CHECK(movement.find(unit)->has_path);
    CHECK(!movement.find(unit)->route_failed);
    CHECK(!movement.find(unit)->free_spot_tried);
    CHECK(movement.find(unit)->goto_failed_at == kNoGotoFailure);
    tested = true;
  }
  CHECK(tested);
}

/// `PathTo` asks the same question with the same flag.
TEST(path_to_answers_a_straight_line_for_a_class_that_ignores_passability) {
  const auto bytes = [](std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  };
  ClassGraph graph;
  graph.add(bytes("<class id=\"Unit\" cpp_class=\"CVXUnit\" parent=\"\"/>"), "unit.sc.xml");
  graph.add(bytes("<class id=\"Sentry\" cpp_class=\"CVXUnit\" parent=\"Unit\">"
                  "<properties ignore_passability=\"1\"/></class>"),
            "sentry.sc.xml");
  graph.link();

  ObstructionGrid grid = open_field(32);
  for (std::int32_t x = 0; x < 32; ++x) {
    for (std::int32_t y = 9; y <= 11; ++y) grid.set_cell(x, y, true);
  }

  World world;
  world.set_class_graph(&graph);
  MovementSystem movement;
  movement.set_grid(std::move(grid));
  movement.set_class_graph(&graph);
  attach(world, movement);
  HostRegistry registry;
  register_movement_host(registry);

  const Point from{3 * 16 + 8, 10 * 16 + 8};
  const Point to{20 * 16 + 8, 10 * 16 + 8};
  const ObjectId sentry = world.spawn(NativeClass::unit, nullptr, graph.find("Sentry"));
  const ObjectId soldier = world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
  CHECK(world.set_position(sentry, from));
  CHECK(world.set_position(soldier, from));

  HostCall straight(world, {Value::object(kTypeObj, sentry), pack_point(to), Value::integer(0), Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "PathTo", 3, straight).value.as_integer() ==
        static_cast<std::int32_t>(distance(from, to)));
  HostCall walled(world, {Value::object(kTypeObj, soldier), pack_point(to), Value::integer(0), Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "PathTo", 3, walled).value.as_integer() == -1);
}

/// `PathTo` answers the path record's length (0x005de354 reads `+0x34`) for
/// whatever route the search laid -- a partial one included, with no 32,000
/// -- and -1 only when the search answered 0.
TEST(path_to_answers_the_length_of_a_partial_route) {
  ObstructionGrid grid = open_field(64);
  for (std::int32_t y = 0; y < 64; ++y) grid.set_cell(32, y, true);
  World world;
  MovementSystem movement;
  movement.set_grid(std::move(grid));
  attach(world, movement);
  HostRegistry registry;
  register_movement_host(registry);

  const ObjectId unit = spawn_unit(world, movement, Point{10 * 16 + 8, 20 * 16 + 8}, 100);
  // Beyond the wall: the route runs to the reachable cell nearest the goal,
  // (31, 20), a straight 21 cells.
  HostCall beyond(world, {Value::object(kTypeObj, unit), pack_point(Point{50 * 16 + 8, 20 * 16 + 8}),
                          Value::integer(0), Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "PathTo", 3, beyond).value.as_integer() == 21 * 16);
}

/// `PathTo`'s ring builder (0x00409cf0) stops after one point only for a
/// range of exactly 0. A negative range is the same ring of sixteen, each
/// point the range times its direction, so the walk is to the ring at the
/// range's size and not to the point.
TEST(path_to_with_a_negative_range_walks_to_the_ring_and_not_the_point) {
  World world;
  MovementSystem movement;
  movement.set_grid(open_field(128));
  attach(world, movement);
  HostRegistry registry;
  register_movement_host(registry);

  const ObjectId unit = spawn_unit(world, movement, Point{1000, 1000}, 100);
  const auto path_to = [&](std::int32_t range) {
    HostCall call(world, {Value::object(kTypeObj, unit), pack_point(Point{1300, 1000}),
                          Value::integer(range), Value::integer(0)});
    return invoke(registry, CallKind::member, "PathTo", 3, call).value.as_integer();
  };
  CHECK(path_to(0) == 300);
  CHECK(path_to(100) == 200);
  CHECK(path_to(-100) == 200);
}

TEST(both_forms_of_goto_drop_the_combat_target) {
  // The point form empties `[unit+0x1a8]` once the unit is out of any holder
  // (0x005d639a), the object form on every entry before anything else
  // (0x005d6c2f): a new order supersedes combat, as it does for `Stop`.
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  CombatSystem combat;
  world.add_system(&combat);
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{500, 500}, 100);
  const ObjectId foe = spawn_unit(world, movement, Point{540, 500}, 100);
  const ObjectId post = spawn_unit(world, movement, Point{1500, 500}, 100);
  Combatant fighter;
  fighter.id = unit;
  fighter.owner = 0;
  fighter.health = 100;
  fighter.target = foe;
  fighter.attacks = 3;
  (void)combat.add(fighter);

  HostCall to_point(world, {Value::object(kTypeObj, unit), pack_point(Point{900, 500}),
                            Value::integer(0), Value::integer(2000), Value::boolean(true),
                            Value::integer(0)});
  (void)invoke(registry, CallKind::member, "Goto", 5, to_point);
  CHECK(combat.find(unit)->target == kNoObject);
  CHECK(combat.find(unit)->attacks == 0);

  // The same order again, walking: still dropped, since each call here is a
  // first entry of the original's.
  combat.find(unit)->target = foe;
  (void)invoke(registry, CallKind::member, "Goto", 5, to_point);
  CHECK(combat.find(unit)->target == kNoObject);

  combat.find(unit)->target = foe;
  combat.find(unit)->attacks = 4;
  HostCall to_object(world, {Value::object(kTypeObj, unit), Value::object(kTypeObj, post),
                             Value::integer(0), Value::integer(2000), Value::boolean(true),
                             Value::integer(0)});
  (void)invoke(registry, CallKind::member, "Goto", 5, to_object);
  CHECK(combat.find(unit)->target == kNoObject);
  CHECK(combat.find(unit)->attacks == 0);

  // A unit with no target keeps its count: the writes are made on a change.
  combat.find(unit)->attacks = 2;
  (void)invoke(registry, CallKind::member, "Goto", 5, to_point);
  CHECK(combat.find(unit)->attacks == 2);
}

TEST(a_host_stop_on_a_standing_unit_answers_true_at_once_and_drops_its_target) {
  // 34 of `Stop`'s 53 sites are `while (!.Stop(1000));`. A void `Stop` yields
  // nil, `!nil` is true, and every one of those loops spins forever -- so it
  // returns a bool. For a unit with no route 0x005d6c90 answers true **without
  // suspending** (0x005d6d9b): `ms` is not waited, and the idle loops are paced
  // by the `Idle` beside it. The first entry drops the combat target
  // (0x005d6cf7, `[unit+0x1a8]` and the attack count beside it) whatever the
  // route.
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  CombatSystem combat;
  world.add_system(&combat);
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{500, 500}, 100);
  const ObjectId foe = spawn_unit(world, movement, Point{540, 500}, 100);
  Combatant fighter;
  fighter.id = unit;
  fighter.owner = 0;
  fighter.health = 100;
  fighter.target = foe;
  fighter.attacks = 3;
  (void)combat.add(fighter);

  HostCall stop(world, {Value::object(kTypeObj, unit), Value::integer(1000)});
  const HostOutcome outcome = invoke(registry, CallKind::member, "Stop", 1, stop);
  CHECK(outcome.value.as_integer() == 1);
  CHECK(outcome.status == HostStatus::ok);
  CHECK(outcome.suspend_for == 0);
  CHECK(!world.state(unit)->flags.has_active_path);
  CHECK(combat.find(unit)->target == kNoObject);
  CHECK(combat.find(unit)->attacks == 0);

  // A unit with no target keeps its count: the setter writes only on a change.
  combat.find(unit)->attacks = 2;
  (void)invoke(registry, CallKind::member, "Stop", 1, stop);
  CHECK(combat.find(unit)->attacks == 2);

  // A walking unit loses its target on the first entry too, and is suspended.
  combat.find(unit)->target = foe;
  HostCall walk(world, {Value::object(kTypeObj, unit), pack_point(Point{900, 500}),
                        Value::integer(0), Value::integer(2000), Value::boolean(true),
                        Value::integer(0)});
  (void)invoke(registry, CallKind::member, "Goto", 5, walk);
  REQUIRE(world.state(unit)->flags.has_active_path);
  combat.find(unit)->target = foe;
  CHECK(invoke(registry, CallKind::member, "Stop", 1, stop).status == HostStatus::retry);
  CHECK(combat.find(unit)->target == kNoObject);
}

TEST(a_host_stop_on_a_walking_unit_answers_at_its_re_entry) {
  // 0x005d6c90 on a unit with a route: the first entry asks the route to stop
  // and suspends with it in hand; the re-entry answers false while the unit
  // still has a step to take and true once it has stood still. A `Goto` route
  // owns a lock, so it stops on the first free spot -- here the next step,
  // with nobody about.
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{500, 500}, 100);

  HostCall walk(world, {Value::object(kTypeObj, unit), pack_point(Point{900, 500}),
                        Value::integer(0), Value::integer(2000), Value::boolean(true),
                        Value::integer(0)});
  (void)invoke(registry, CallKind::member, "Goto", 5, walk);
  CHECK(world.state(unit)->flags.has_active_path);

  HostCall stop(world, {Value::object(kTypeObj, unit), Value::integer(1000)});
  const HostOutcome first = invoke(registry, CallKind::member, "Stop", 1, stop);
  CHECK(first.status == HostStatus::retry);
  CHECK(first.suspend_for == 1000);
  CHECK(world.state(unit)->flags.has_active_path);
  CHECK(movement.find(unit)->stop_requested);

  stop.context.first_call = false;
  const HostOutcome early = invoke(registry, CallKind::member, "Stop", 1, stop);
  CHECK(early.status == HostStatus::ok);
  CHECK(early.value.as_integer() == 0);

  world.advance(200);
  CHECK(!world.state(unit)->flags.has_active_path);
  const HostOutcome done = invoke(registry, CallKind::member, "Stop", 1, stop);
  CHECK(done.status == HostStatus::ok);
  CHECK(done.value.as_integer() == 1);
  CHECK(position_of(world, unit).x < 600);
}

TEST(the_deer_prelude_runs_through_the_host) {
  // `.Face(pt); .SetSpeedFactor(7); .SetWalkAnim(13);` -- the three calls
  // DEER_IDLE.VS makes before it starts walking.
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);
  const ObjectId deer = spawn_unit(world, movement, Point{500, 500}, 150);

  HostCall face(world, {Value::object(kTypeObj, deer), pack_point(Point{500, 900})});
  CHECK(invoke(registry, CallKind::member, "Face", 1, face).status == HostStatus::ok);
  HostCall factor(world, {Value::object(kTypeObj, deer), Value::integer(7)});
  CHECK(invoke(registry, CallKind::member, "SetSpeedFactor", 1, factor).status ==
        HostStatus::ok);
  HostCall anim(world, {Value::object(kTypeObj, deer), Value::integer(13)});
  CHECK(invoke(registry, CallKind::member, "SetWalkAnim", 1, anim).status == HostStatus::ok);

  const MoveState* state = movement.find(deer);
  REQUIRE(state != nullptr);
  CHECK(state->facing == (Point{0, kFacingLength}));
  CHECK(state->speed_factor == 7);
  CHECK(state->walk_anim == 13);

  // `.GetDir` hands that facing back, and it is long enough to survive
  // `pt.SetLen(15 + rand(50))` -- which is the next thing the script does.
  HostCall dir(world, {Value::object(kTypeObj, deer)});
  const HostOutcome heading = invoke(registry, CallKind::member, "GetDir", 0, dir);
  CHECK(is_point(heading.value));
  CHECK(unpack_point(heading.value) == (Point{0, kFacingLength}));

  // At factor 7 a speed-150 deer ambles: 10 units per second, so 4 in a
  // 400-unit turn. That is the arithmetic in the movement header, checked.
  HostCall go(world, {Value::object(kTypeObj, deer), pack_point(Point{500, 900}),
                      Value::integer(0), Value::integer(2000), Value::boolean(true),
                      Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "Goto", 5, go).status == HostStatus::retry);
  run(world, movement, {400});
  CHECK(position_of(world, deer) == (Point{500, 504}));
}

TEST(host_geometry_reads_the_grid) {
  HostRegistry registry;
  register_movement_host(registry);

  ObstructionGrid grid = open_field(32);
  grid.set_cell(20, 20, true);

  World world;
  MovementSystem movement;
  movement.set_grid(std::move(grid));
  attach(world, movement);

  // `IsPassable3x3(pt)` is a footprint test: the neighbour of a blocked cell
  // fails it even though the cell itself is free.
  HostCall clear(world, {pack_point(Point{100, 100})});
  CHECK(invoke(registry, CallKind::free_function, "IsPassable3x3", 1, clear)
            .value.as_integer() == 1);
  HostCall beside(world, {pack_point(Point{19 * 16 + 8, 20 * 16 + 8})});
  CHECK(invoke(registry, CallKind::free_function, "IsPassable3x3", 1, beside)
            .value.as_integer() == 0);

  // `ClipDestToMap` folds a point back inside the world square.
  const ObjectId unit = spawn_unit(world, movement, Point{100, 100}, 100);
  HostCall clip(world, {Value::object(kTypeObj, unit), pack_point(Point{9000, -50})});
  const HostOutcome clipped = invoke(registry, CallKind::member, "ClipDestToMap", 1, clip);
  REQUIRE(is_point(clipped.value));
  CHECK(unpack_point(clipped.value) == (Point{32 * 16 - 1, 0}));
}

/// `Hero::SetFormation` (0x0052eca0) prints an unknown name into the dead
/// printer and goes on: the hero keeps the formation it had.
TEST(set_formation_ignores_a_name_the_file_does_not_declare) {
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(open_field(32));
  Result<FormationTable> table = FormationTable::parse(as_bytes(kFormationsXml));
  REQUIRE(table.ok());
  movement.set_formations(std::move(*table));
  attach(world, movement);
  const ObjectId hero = spawn_unit(world, movement, Point{100, 100}, 100);

  HostCall good(world, {Value::object(kTypeObj, hero), Value::string("Line")});
  CHECK(invoke(registry, CallKind::member, "SetFormation", 1, good).status == HostStatus::ok);
  HostCall bad(world, {Value::object(kTypeObj, hero), Value::string("Testudo")});
  CHECK(invoke(registry, CallKind::member, "SetFormation", 1, bad).status == HostStatus::ok);
  HostCall read(world, {Value::object(kTypeObj, hero)});
  CHECK(invoke(registry, CallKind::member, "formation", 0, read).value.as_string() == "Line");

  // Unset, the property reports the file's `<Default Name="Front"/>`.
  const ObjectId other = spawn_unit(world, movement, Point{200, 200}, 100);
  HostCall fallback(world, {Value::object(kTypeObj, other)});
  CHECK(invoke(registry, CallKind::member, "formation", 0, fallback).value.as_string() ==
        "Front");
}

TEST(a_host_call_without_a_world_fails_rather_than_guessing) {
  HostRegistry registry;
  register_movement_host(registry);
  std::vector<Value> arguments{Value::object(kTypeObj, 1)};
  CallContext context;
  context.arguments = arguments;
  context.user = nullptr;
  const std::uint32_t index = registry.find(CallKind::member, "HasPath", 0);
  REQUIRE(index != kUnresolvedHost);
  const HostOutcome outcome = registry.entry(index).fn(context);
  // A missing world must trap with a name, not answer "no path" -- a silently
  // wrong answer here is a behavioural divergence the harness cannot trace.
  CHECK(outcome.status == HostStatus::error);
  CHECK(outcome.error != nullptr);
}

/// `DoCarryNothing` is the walk index at zero -- slot 1 here, the script's
/// numbering, one above the index the original stores (0x005d8350 writes
/// `n - 1`). The two carry setters store index 16 or 17, slots 17 or 18, for
/// the ambient villager classes and leave every other class alone.
TEST(the_carry_trio_is_the_walk_animation_slot) {
  HostRegistry registry;
  register_movement_host(registry);
  ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Unit" cpp_class="CVXUnit"><properties sight="100"/></class>)",
      R"(<class id="GWVillagerAmbient" parent="Unit" cpp_class="CVXUnit"/>)",
      R"(<class id="EVillagerAmbient" parent="Unit" cpp_class="CVXUnit"/>)",
      R"(<class id="RPeasant" parent="Unit" cpp_class="CVXUnit"/>)",
      // An heir is not the class: the compare is on the id, whole.
      R"(<class id="GWVillagerAmbientElder" parent="GWVillagerAmbient" cpp_class="CVXUnit"/>)",
  };
  const char* names[] = {"unit.sc.xml", "gw.sc.xml", "e.sc.xml", "rp.sc.xml", "elder.sc.xml"};
  for (int i = 0; i < 5; ++i) {
    graph.add({reinterpret_cast<const std::byte*>(docs[i].data()), docs[i].size()}, names[i]);
  }
  graph.link();
  World world;
  world.set_class_graph(&graph);
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);
  const auto spawn = [&](const char* cls) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, graph.find(cls));
    world.set_position(id, Point{100, 100});
    (void)movement.state(id);
    return id;
  };
  const auto carries_nothing = [&](ObjectId id) {
    HostCall call(world, {Value::object(script::ObjectRef{kTypeObj, id})});
    const HostOutcome out = invoke(registry, CallKind::member, "DoCarryNothing", 0, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  };
  const auto set = [&](const char* name, ObjectId id) {
    HostCall call(world, {Value::object(script::ObjectRef{kTypeObj, id})});
    CHECK(invoke(registry, CallKind::member, name, 0, call).status == HostStatus::ok);
  };

  const ObjectId water = spawn("GWVillagerAmbient");
  const ObjectId plain = spawn("EVillagerAmbient");
  const ObjectId peasant = spawn("RPeasant");
  const ObjectId elder = spawn("GWVillagerAmbientElder");
  CHECK(carries_nothing(water));
  CHECK(carries_nothing(plain));

  set("SetCarryWaterAnim", water);
  CHECK(movement.state(water).walk_anim == 17);
  CHECK(!carries_nothing(water));
  set("SetCarryGoodsAnim", water);
  CHECK(movement.state(water).walk_anim == 18);
  // A plain villager carries goods on the water slot and no water at all.
  set("SetCarryWaterAnim", plain);
  CHECK(movement.state(plain).walk_anim == 1);
  set("SetCarryGoodsAnim", plain);
  CHECK(movement.state(plain).walk_anim == 17);
  CHECK(!carries_nothing(plain));
  // A peasant, and an heir of the class, are left alone.
  set("SetCarryGoodsAnim", peasant);
  set("SetCarryWaterAnim", peasant);
  set("SetCarryGoodsAnim", elder);
  CHECK(movement.state(peasant).walk_anim == 1);
  CHECK(movement.state(elder).walk_anim == 1);
  CHECK(carries_nothing(peasant));
  // The no-argument `SetWalkAnim` is what puts the bucket down.
  HostCall reset(world, {Value::object(script::ObjectRef{kTypeObj, water})});
  CHECK(invoke(registry, CallKind::member, "SetWalkAnim", 0, reset).status == HostStatus::ok);
  CHECK(carries_nothing(water));
  // A slot the original would call "something" -- the deer's grazing 13 --
  // reads the same way here.
  movement.state(water).walk_anim = 13;
  CHECK(!carries_nothing(water));
  // `SetWalkAnim(0)` stores index -1, which is not zero: something carried.
  HostCall zero(world, {Value::object(script::ObjectRef{kTypeObj, water}), Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "SetWalkAnim", 1, zero).status == HostStatus::ok);
  CHECK(!carries_nothing(water));
  // And a handle to nothing answers false.
  CHECK(!carries_nothing(999999));
}

/// `GotoEnter` (0x005d6620) drops the combat target on its first pass once the
/// unit is out of any holder (0x005d6810..0x005d6820), as the point `Goto` does.
TEST(goto_enter_drops_the_combat_target) {
  HostRegistry registry;
  register_movement_host(registry);
  (void)register_command_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  CombatSystem combat;
  world.add_system(&combat);
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{500, 500}, 100);
  const ObjectId foe = spawn_unit(world, movement, Point{540, 500}, 100);
  Combatant fighter;
  fighter.id = unit;
  fighter.owner = 0;
  fighter.health = 100;
  fighter.target = foe;
  fighter.attacks = 3;
  (void)combat.add(fighter);

  HostCall enter(world, {Value::object(kTypeObj, unit), pack_point(Point{900, 500}),
                         Value::integer(0), Value::integer(2000), Value::boolean(false),
                         Value::integer(0)});
  (void)invoke(registry, CallKind::member, "GotoEnter", 5, enter);
  CHECK(combat.find(unit)->target == kNoObject);
  CHECK(combat.find(unit)->attacks == 0);
}

// --------------------------------------------------------------------------
// the host slice against gbr.exe's bodies
// --------------------------------------------------------------------------

namespace {

/// A movement-and-commands world on an open field, for the host bodies that
/// touch a unit's command queue.
struct HostField {
  World world;
  CommandSystem commands;
  HeroSystem heroes;
  MovementSystem movement;
  HostRegistry registry;

  explicit HostField(ObstructionGrid grid = open_field(64)) {
    movement.set_grid(std::move(grid));
    world.add_system(&commands);
    world.add_system(&heroes);
    attach(world, movement);
    register_movement_host(registry);
  }

  ObjectId unit(Point where, NativeClass kind = NativeClass::unit) {
    const ObjectId id = world.spawn(kind, nullptr);
    CHECK(world.set_position(id, where));
    world.set_owner(id, 1);
    movement.state(id).speed = 100;
    return id;
  }

  /// One member call with `script` as the running script.
  HostOutcome call(std::string_view name, std::initializer_list<Value> args,
                   script::ScriptId script = script::kNoScript) {
    HostCall c(world, args);
    c.context.script = script;
    return invoke(registry, CallKind::member, name,
                  static_cast<std::uint16_t>(args.size() - 1), c);
  }

  /// `id` running `verb`, its coroutine `script`.
  void running(ObjectId id, std::string_view verb, script::ScriptId script) {
    (void)commands.set_command(world, id, verb, Command{});
    CommandQueue* queue = commands.find(id);
    REQUIRE(queue != nullptr);
    REQUIRE(!queue->entries.empty());
    queue->entries.front().script = script;
    queue->entries.front().started = true;
  }
};

}  // namespace

/// Every body in the slice opens with the handle check and, on a miss,
/// formats its complaint into 0x00686eb0 -- a bare `ret` in retail -- and
/// pushes a default. The script goes on.
TEST(a_receiver_that_names_nothing_gets_each_bodys_default) {
  HostField f;
  const ObjectId gone = f.unit(Point{100, 100});
  const ObjectId other = f.unit(Point{300, 100});
  REQUIRE(f.world.despawn(gone));
  for (const Value& dead :
       {Value::object(kTypeObj, gone), Value::object(script::ObjectRef{script::kNoType, 0})}) {
    const Value to = pack_point(Point{500, 100});
    const auto go = [&](const Value& where) {
      return f.call("Goto", {dead, where, Value::integer(0), Value::integer(1000),
                             Value::boolean(true), Value::integer(-1)});
    };
    // The point form answers true (0x005d61b0), the object form false.
    const HostOutcome point_goto = go(to);
    CHECK(point_goto.status == HostStatus::ok);
    CHECK(point_goto.value.as_integer() == 1);
    const HostOutcome object_goto = go(Value::object(kTypeObj, other));
    CHECK(object_goto.status == HostStatus::ok);
    CHECK(object_goto.value.as_integer() == 0);

    const auto answers = [&](std::string_view name, std::initializer_list<Value> args) {
      const HostOutcome out = f.call(name, args);
      CHECK(out.status == HostStatus::ok);
      return out.value;
    };
    CHECK(answers("Stop", {dead, Value::integer(1000)}).as_integer() == 0);
    CHECK(answers("HasPath", {dead}).as_integer() == 0);
    CHECK(answers("DoCarryNothing", {dead}).as_integer() == 0);
    CHECK(answers("PathTo", {dead, to, Value::integer(0), Value::integer(0)}).as_integer() == -1);
    CHECK(answers("TimeWithoutWalking", {dead}).as_integer() == 0);
    CHECK(answers("speed", {dead}).as_integer() == 0);
    CHECK(answers("FormRadius", {dead}).as_integer() == 0);
    CHECK(unpack_point(answers("dest", {dead})) == kHeldPosition);
    CHECK(unpack_point(answers("GetDir", {dead})) == (Point{0, 1}));
    CHECK(answers("formation", {dead}).as_string().empty());
    // The setters do nothing and say nothing.
    (void)answers("Face", {dead, to});
    (void)answers("SetPos", {dead, to});
    (void)answers("SetSpeedFactor", {dead, Value::integer(7)});
    (void)answers("SetWalkAnim", {dead, Value::integer(13)});
    (void)answers("SetWalkAnim", {dead});
    (void)answers("SetCarryWaterAnim", {dead});
    (void)answers("SetFormation", {dead, Value::string("Line")});
    // `PathDestFound` makes no check and faults: refused by name.
    CHECK(f.call("PathDestFound", {dead}).status == HostStatus::error);
  }
  // A receiver of another shape is not a handle the compiler can produce.
  CHECK(f.call("HasPath", {Value::integer(4)}).status == HostStatus::error);
}

/// 0x005d61b0 clamps its destination into `[map+0xb4]..[map+0xc0]` and
/// writes it back into its own argument before anything else.
TEST(a_point_goto_walks_to_its_destination_clamped_to_the_map) {
  HostField f;  // 64 cells, so the map runs 0..1023
  const ObjectId unit = f.unit(Point{100, 100});
  CHECK(goto_once(f.registry, f.world, unit, Point{5000, -300}, 1000, -1).status ==
        HostStatus::retry);
  REQUIRE(f.movement.find(unit) != nullptr);
  CHECK(f.movement.find(unit)->has_path);
  CHECK(f.movement.find(unit)->target == (Point{1023, 0}));
}

/// **`SetDest` keeps a capped range** (0x0041a4c0, 0x0041a5c0): a point goal
/// at most 999, an object goal clamped to 999 and then, above 919, 920 -- and
/// the object form of `Goto` routes to a unit as an object goal, whose band
/// is the gap between the edges, and to anything else as a point goal at its
/// centre with both radii added before the cap. Uncapped, a `Goto` with the
/// range of a long sight arrived from further away than the original lets it.
TEST(the_goto_family_keeps_setdests_range_caps) {
  HostField f;
  const ObjectId walker = f.unit(Point{100, 100});
  const auto go = [&](Value where, std::int32_t range) {
    return f.call("Goto", {Value::object(kTypeObj, walker), where, Value::integer(range),
                           Value::integer(500), Value::boolean(true), Value::integer(-1)});
  };
  // Out of range of each other by more than any cap.
  CHECK(go(pack_point(Point{1000, 1000}), 5000).status == HostStatus::retry);
  CHECK(f.movement.find(walker)->range == 999);
  CHECK(go(pack_point(Point{1000, 1000}), 400).status == HostStatus::retry);
  CHECK(f.movement.find(walker)->range == 400);
  // A unit: 920 at most, the radii (none here) on top.
  const ObjectId other = f.unit(Point{1000, 1000});
  f.world.set_owner(other, 2);
  CHECK(go(Value::object(kTypeObj, other), 950).status == HostStatus::retry);
  CHECK(f.movement.find(walker)->range == 920);
  CHECK(go(Value::object(kTypeObj, other), 919).status == HostStatus::retry);
  CHECK(f.movement.find(walker)->range == 919);
  // Not a unit: a point goal, 999 at most.
  const ObjectId post = f.unit(Point{1000, 1000}, NativeClass::building);
  CHECK(go(Value::object(kTypeObj, post), 950).status == HostStatus::retry);
  CHECK(f.movement.find(walker)->range == 950);
  CHECK(go(Value::object(kTypeObj, post), 5000).status == HostStatus::retry);
  CHECK(f.movement.find(walker)->range == 999);
}

/// `GotoEnter` (0x005d6620) answers **true** for a receiver naming nothing
/// and `GotoAttack` (0x005d6a90) **false**; `GotoEnter` clamps its point to
/// the map and writes it back into its own argument, as `Goto` does; and
/// `GotoAttack` makes its argument the combat target and approaches only what
/// it may, so a garrisoned enemy lays no route and waits the slice.
TEST(goto_enter_and_goto_attack_read_their_receivers_and_targets_as_the_original) {
  HostField f;
  register_command_host(f.registry);
  CombatSystem combat;
  f.world.add_system(&combat);
  const ObjectId walker = f.unit(Point{100, 100});
  const ObjectId gone = f.unit(Point{200, 200});
  REQUIRE(f.world.despawn(gone));
  const Value there = pack_point(Point{5000, -300});
  const HostOutcome enter_gone =
      f.call("GotoEnter", {Value::object(kTypeObj, gone), there, Value::integer(0),
                           Value::integer(500), Value::boolean(true), Value::integer(-1)});
  CHECK(enter_gone.status == HostStatus::ok);
  CHECK(enter_gone.value.as_integer() == 1);
  const HostOutcome attack_gone =
      f.call("GotoAttack", {Value::object(kTypeObj, gone), Value::object(kTypeObj, walker),
                            Value::integer(500), Value::boolean(true)});
  CHECK(attack_gone.status == HostStatus::ok);
  CHECK(attack_gone.value.as_integer() == 0);

  HostCall enter(f.world, {Value::object(kTypeObj, walker), there, Value::integer(0),
                           Value::integer(500), Value::boolean(true), Value::integer(-1)});
  CHECK(invoke(f.registry, CallKind::member, "GotoEnter", 5, enter).status == HostStatus::retry);
  CHECK(f.movement.find(walker)->target == (Point{1023, 0}));
  CHECK(unpack_point(enter.arguments[1]) == (Point{1023, 0}));

  // An enemy inside a tower: the target is handed over, no route is laid.
  f.movement.stop(f.world, walker);
  const ObjectId tower = f.unit(Point{600, 100}, NativeClass::building);
  const ObjectId inside = f.unit(Point{600, 140});
  f.world.set_owner(inside, 2);
  REQUIRE(f.world.put_in_holder(inside, tower));
  Combatant mine;
  mine.id = walker;
  mine.owner = 1;
  mine.position = Point{100, 100};
  mine.health = 100;
  combat.add(mine);
  HostCall attack(f.world, {Value::object(kTypeObj, walker), Value::object(kTypeObj, inside),
                            Value::integer(750), Value::boolean(true)});
  const HostOutcome out = invoke(f.registry, CallKind::member, "GotoAttack", 3, attack);
  CHECK(out.status == HostStatus::retry);
  CHECK(out.suspend_for == 750);
  CHECK(attack.scratch == 0);
  CHECK(!f.movement.find(walker)->has_path);
  REQUIRE(combat.find(walker) != nullptr);
  CHECK(combat.find(walker)->target == inside);
  // With no order to answer on, its re-entry is a first run again: it waits.
  CHECK(invoke(f.registry, CallKind::member, "GotoAttack", 3, attack.reenter()).status ==
        HostStatus::retry);
}

/// The object `Goto` routes only to a target it may approach (0x005d40c0 at
/// 0x005d4291..0x005d42d7), and a target that is the mover answers true.
TEST(an_object_goto_routes_only_to_a_target_it_may_approach) {
  HostField f;
  const ObjectId walker = f.unit(Point{100, 100});
  const auto go = [&](ObjectId target) {
    return f.call("Goto", {Value::object(kTypeObj, walker), Value::object(kTypeObj, target),
                           Value::integer(0), Value::integer(751), Value::boolean(true),
                           Value::integer(-1)});
  };
  const auto refused = [&](const HostOutcome& out) {
    return out.status == HostStatus::retry && out.value.as_integer() == 0 &&
           out.suspend_for == 751 && !f.movement.find(walker)->has_path;
  };

  const HostOutcome self = go(walker);
  CHECK(self.status == HostStatus::ok);
  CHECK(self.value.as_integer() == 1);

  // A unit inside a holder.
  const ObjectId tower = f.unit(Point{600, 100}, NativeClass::building);
  const ObjectId inside = f.unit(Point{600, 140});
  REQUIRE(f.world.put_in_holder(inside, tower));
  CHECK(refused(go(inside)));

  // A hidden unit of another side, and one of the walker's own.
  const ObjectId lurker = f.unit(Point{600, 400});
  f.world.set_owner(lurker, 2);
  f.world.mutable_state(lurker)->flags.hidden = true;
  CHECK(refused(go(lurker)));
  const ObjectId scout = f.unit(Point{400, 600});
  f.world.mutable_state(scout)->flags.hidden = true;
  CHECK(go(scout).status == HostStatus::retry);
  CHECK(f.movement.find(walker)->has_path);
  CHECK(f.movement.find(walker)->target_object == scout);
}

/// A unit standing at `(-1, -1)` with no holder -- what `Disappear` leaves --
/// waits 500 ms and runs the call again (0x005d6312).
TEST(a_goto_by_a_unit_standing_nowhere_asks_again_every_half_second) {
  HostField f;
  const ObjectId unit = f.unit(Point{100, 100});
  REQUIRE(f.world.set_position(unit, kHeldPosition));
  const HostOutcome out = goto_once(f.registry, f.world, unit, Point{500, 500}, 2000, -1);
  CHECK(out.status == HostStatus::retry);
  CHECK(out.suspend_for == 500);
  CHECK(!f.movement.find(unit)->has_path);
}

/// `TimeWithoutWalking` (0x005d6ee0) is the clock less the `Goto` failure
/// stamp `[unit+0x150]`, 0 while it is clear -- not how long the unit has
/// stood still.
TEST(time_without_walking_is_how_long_goto_has_failed_to_lay_a_route) {
  HostField f(sealed_cell(9, 10));
  const ObjectId sealed = f.unit(Point{9 * 16 + 8, 10 * 16 + 8});
  const ObjectId idle = f.unit(Point{400, 400});
  const auto without = [&](ObjectId id) {
    return f.call("TimeWithoutWalking", {Value::object(kTypeObj, id)}).value.as_integer();
  };
  f.world.advance(3000);
  // Three seconds standing about is not three seconds without a route.
  CHECK(without(idle) == 0);
  CHECK(without(sealed) == 0);

  CHECK(goto_once(f.registry, f.world, sealed, Point{2000, 2000}, 1000, -1).status ==
        HostStatus::retry);
  CHECK(without(sealed) == 0);
  f.world.advance(1500);
  CHECK(without(sealed) == 1500);
  // A route laid clears the stamp.
  f.movement.set_grid(open_field(32));
  CHECK(goto_once(f.registry, f.world, sealed, Point{400, 160}, 1000, -1).status ==
        HostStatus::retry);
  CHECK(without(sealed) == 0);
}

/// `dest` (0x005d6f50) is where the route goes -- the route record's
/// `vtbl+0x10`, the media's last point -- and the unit's `posRH` without one;
/// `PathDestFound` (0x005d8830) is the same last point, or `(-1, -1)`.
TEST(dest_and_path_dest_found_answer_the_end_of_the_route) {
  HostField f(sealed_cell(20, 6));
  const ObjectId unit = f.unit(Point{100, 100});
  const Value self = Value::object(kTypeObj, unit);
  const auto point_of = [&](std::string_view name) {
    const HostOutcome out = f.call(name, {self});
    CHECK(out.status == HostStatus::ok);
    return unpack_point(out.value);
  };
  CHECK(point_of("dest") == (Point{100, 100}));
  CHECK(point_of("PathDestFound") == kHeldPosition);

  // Into the sealed cell: the route that is laid stops short of the order.
  const Point walled{20 * 16 + 8, 6 * 16 + 8};
  (void)goto_once(f.registry, f.world, unit, walled, 1000, -1);
  const MoveState* move = f.movement.find(unit);
  REQUIRE(move != nullptr);
  REQUIRE(move->has_path);
  REQUIRE(!move->waypoints.empty());
  CHECK(move->waypoints.back() != walled);
  CHECK(point_of("dest") == move->waypoints.back());
  CHECK(point_of("PathDestFound") == move->waypoints.back());
}

/// `PathTo`'s object form (0x005de390) goes to the target's `posRH` with both
/// class radii added to the range, answers -1 for a target that names nothing,
/// and a unit aboard a ship answers 1,000,000 (0x005de1e1).
TEST(path_to_an_object_reaches_its_edge_and_a_passenger_answers_a_million) {
  EdgeField f;
  const ObjectId walker = f.place("Walker", Point{1000, 1000});
  const ObjectId camp = f.place("Camp", Point{1100, 1000});
  const auto path_to = [&](ObjectId who, const Value& where) {
    HostCall call(f.world, {Value::object(kTypeObj, who), where, Value::integer(0),
                            Value::integer(0)});
    const HostOutcome out = invoke(f.registry, CallKind::member, "PathTo", 3, call);
    CHECK(out.status == HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -999;
  };
  // 100 between centres: a walk of 100 to the point. To the object the goal
  // is the ring of sixteen points at its band, 15 + 100, round its centre
  // (0x00409cf0), and the walker inside it is answered the walk out to the
  // nearest of them, (985, 1000): 15, where this used to answer 0.
  CHECK(path_to(walker, pack_point(Point{1100, 1000})) == 100);
  CHECK(path_to(walker, Value::object(kTypeObj, camp)) == 15);
  // Standing on the point it was asked for, the route has no length: -1.
  CHECK(path_to(walker, pack_point(Point{1000, 1000})) == -1);

  const ObjectId doomed = f.place("Walker", Point{1000, 1400});
  REQUIRE(f.world.despawn(doomed));
  CHECK(path_to(walker, Value::object(kTypeObj, doomed)) == -1);

  const World::ShipIds ship = f.world.spawn_ship(nullptr);
  REQUIRE(f.world.set_position(ship.ship, Point{2000, 2000}));
  const ObjectId sailor = f.place("Walker", Point{2000, 2100});
  REQUIRE(f.world.put_in_holder(sailor, ship.holder));
  CHECK(path_to(sailor, pack_point(Point{1000, 1000})) == 1000000);
}

/// `Hero::FormRadius` (0x0052ec40 -> 0x005f2350) is the formation's radius
/// `[form+0x34]` when it has members and one, and the hero's class `radius`
/// otherwise -- not the ranks' spacing `formation_radius`.
TEST(form_radius_is_the_formations_reach_or_the_heros_own_radius) {
  ClassGraph graph;
  graph.add(as_bytes(R"(<class id="Leader" cpp_class="CVXHero" parent="">
    <properties speed="100" radius="22" formation_radius="26"/></class>)"),
            "leader.sc.xml");
  graph.link();
  HostField f;
  f.world.set_class_graph(&graph);
  f.movement.set_class_graph(&graph);
  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr, graph.find("Leader"));
  REQUIRE(f.world.set_position(hero, Point{300, 300}));
  const auto radius = [&]() {
    return f.call("FormRadius", {Value::object(kTypeObj, hero)}).value.as_integer();
  };
  CHECK(radius() == 22);
  f.movement.formation_of(hero).radius = 240;
  // A radius with no slot behind it is not the formation's.
  CHECK(radius() == 22);
  f.movement.formation_of(hero).slots.push_back(FormationSlot{f.unit(Point{340, 300})});
  CHECK(radius() == 240);
  // Nor is a radius of 0, whatever the slots.
  f.movement.formation_of(hero).radius = 0;
  CHECK(radius() == 22);
}

/// A hero's `HasPath` (0x0052fe40) is its own route or any member of its army
/// whose route follows the formation; a held unit has none (0x005d6e7f).
TEST(a_hero_has_a_path_while_his_column_is_still_marching) {
  HostField f;
  const ObjectId hero = f.unit(Point{300, 300}, NativeClass::hero);
  const ObjectId marcher = f.unit(Point{200, 300});
  const ObjectId stray = f.unit(Point{200, 500});
  HeroRecord& record = f.heroes.register_hero(f.world, hero);
  record.army = {marcher, stray};
  const auto has_path = [&](ObjectId id) {
    return f.call("HasPath", {Value::object(kTypeObj, id)}).value.as_integer() != 0;
  };
  // A member on an errand of its own is not the march.
  (void)f.movement.order_goto(f.world, stray, Point{900, 900}, 0);
  REQUIRE(f.movement.find(stray)->has_path);
  CHECK(!has_path(hero));
  // Nor is one that merely walks in his party: only a route aimed at his
  // formation (mode 2, `SetFormation` 0x00417790) is.
  (void)f.movement.order_goto(f.world, marcher, Point{800, 300}, 0, 0, hero);
  REQUIRE(f.movement.find(marcher)->has_path);
  CHECK(!has_path(hero));
  Formation& form = f.movement.formation_of(hero);
  form.path = true;
  form.going_on = true;
  form.samples = 1;
  form.slots.push_back({marcher, f.world.find(marcher)->class_index, Point{}, Point{800, 300}});
  f.movement.aim_at_formation(f.world, marcher, hero, false);
  REQUIRE(f.movement.find(marcher)->has_path);
  CHECK(has_path(hero));

  const ObjectId tower = f.unit(Point{700, 700}, NativeClass::building);
  REQUIRE(f.world.put_in_holder(stray, tower));
  CHECK(!has_path(stray));
}

/// `Obj::Face` (0x005ac4a0) and `Obj::SetPos` (0x005ac130) end the object's
/// commands (`vtbl+0xc0` with 1) unless the call comes from its own command
/// script; `ObjList::Face` (0x0055e5d0) ends every member's.
TEST(face_and_set_pos_from_another_script_end_the_objects_commands) {
  HostField f;
  const ObjectId a = f.unit(Point{100, 100});
  const ObjectId b = f.unit(Point{300, 100});
  const Value pt = pack_point(Point{100, 600});
  f.running(a, "guard", 5);
  f.running(b, "guard", 6);

  // From a's own command script: a keeps its command.
  CHECK(f.call("Face", {Value::object(kTypeObj, a), pt}, 5).status == HostStatus::ok);
  CHECK(f.commands.command_name(a) == "guard");
  CHECK(f.call("SetPos", {Value::object(kTypeObj, a), pack_point(Point{120, 100})}, 5).status ==
        HostStatus::ok);
  CHECK(f.commands.command_name(a) == "guard");
  CHECK(position_of(f.world, a) == (Point{120, 100}));

  // From b's script, a sequence, or nothing at all: a's commands end.
  CHECK(f.call("Face", {Value::object(kTypeObj, a), pt}, 6).status == HostStatus::ok);
  CHECK(f.commands.command_name(a) != "guard");
  f.running(a, "guard", 5);
  CHECK(f.call("SetPos", {Value::object(kTypeObj, a), pack_point(Point{140, 100})}).status ==
        HostStatus::ok);
  CHECK(f.commands.command_name(a) != "guard");

  // The list form faces every member and ends every member's commands, its
  // own script or not.
  f.running(a, "guard", 5);
  const ObjListId list = f.world.objlists().acquire_temporary(5);
  f.world.objlists().mutable_items(list)->assign({a, b});
  CHECK(f.call("Face", {make_objlist_value(list), pt}, 5).status == HostStatus::ok);
  CHECK(f.commands.command_name(a) != "guard");
  CHECK(f.commands.command_name(b) != "guard");
  CHECK(f.movement.state(b).facing.y > 0);
  CHECK(f.movement.state(b).facing.x < 0);
}

/// The unit's own `SetPos` (0x005d39e0) takes a held unit out of its holder
/// before it places it, unless the point is `(-1, -1)`.
TEST(set_pos_takes_a_held_unit_out_of_its_holder) {
  HostField f;
  const ObjectId tower = f.unit(Point{500, 500}, NativeClass::building);
  const ObjectId unit = f.unit(Point{500, 540});
  REQUIRE(f.world.put_in_holder(unit, tower));
  CHECK(f.call("SetPos", {Value::object(kTypeObj, unit), pack_point(kHeldPosition)}).status ==
        HostStatus::ok);
  CHECK(f.world.state(unit)->is_held());
  CHECK(f.call("SetPos", {Value::object(kTypeObj, unit), pack_point(Point{800, 800})}).status ==
        HostStatus::ok);
  CHECK(!f.world.state(unit)->is_held());
  CHECK(position_of(f.world, unit) == (Point{800, 800}));
}

// -- the formation's own route ---------------------------------------------------

/// The formation's route is stepped a leg at a time (0x0041a740): a leg of
/// 1,000 is five steps of exactly 200, the last the leg's end; the next leg,
/// 30 long, is one step, and once it is taken nothing is left (0x0041a700).
TEST(the_formations_route_cuts_each_leg_into_steps_nearest_200) {
  FormationRoute route;
  formation_route_lay(route, {Point{0, 0}, Point{1000, 0}, Point{1000, 30}}, true);
  CHECK(route.remaining == 1030 * 65536);
  CHECK(formation_route_left(route) == 1030);
  std::vector<Point> seen;
  Point p{};
  while (formation_route_step(route, p)) {
    seen.push_back(p);
    formation_route_commit(route);
  }
  const std::vector<Point> expected = {Point{200, 0}, Point{400, 0}, Point{600, 0},
                                       Point{800, 0}, Point{1000, 0}, Point{1000, 30}};
  CHECK(seen == expected);
  CHECK(route.remaining == 0);
  CHECK(formation_route_left(route) == 0);
}

/// A leg of 3,700 is 19 steps of 194.7 rather than 18 of 205.6: whichever
/// comes nearer 200. Each point is the leg's start plus the step's 16.16
/// offset times the step number, shifted right -- floored, so a step west
/// lands one unit further than a truncation would.
TEST(a_formation_step_is_the_whole_count_nearest_200_and_its_offset_is_floored) {
  FormationRoute east;
  formation_route_lay(east, {Point{300, 500}, Point{4000, 500}}, true);
  Point p{};
  REQUIRE(formation_route_step(east, p));
  CHECK(east.steps == 19);
  CHECK(p == (Point{494, 500}));
  formation_route_commit(east);
  CHECK(formation_route_left(east) == 3505);
  REQUIRE(formation_route_step(east, p));
  CHECK(p == (Point{689, 500}));

  FormationRoute west;
  formation_route_lay(west, {Point{4000, 500}, Point{300, 500}}, true);
  REQUIRE(formation_route_step(west, p));
  CHECK(west.steps == 19);
  CHECK(p == (Point{3805, 500}));
}

/// A leg of no length takes no step, though what is left of the route is not
/// spent; and what is left is read as the signed high word of its 16.16 value
/// (0x0040b890).
TEST(a_formation_route_leg_of_no_length_takes_no_step) {
  FormationRoute route;
  formation_route_lay(route, {Point{100, 100}, Point{100, 100}, Point{300, 100}}, true);
  Point p{};
  CHECK(!formation_route_step(route, p));
  CHECK(formation_route_left(route) == 200);

  FormationRoute far;
  far.remaining = static_cast<std::int32_t>(40000u * 65536u);
  CHECK(formation_route_left(far) == 40000 - 65536);
}
