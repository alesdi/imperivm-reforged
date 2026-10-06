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

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/game/localization.hpp"
#include "imperivm/core/sim/movement.hpp"
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
  CHECK(state->formation_radius == 31);

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
  CHECK(movement.find(statue)->formation_radius == kDefaultFormationRadius);

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
/// null, and `place_army` returned on its first check -- a hero's warriors have
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

TEST(an_unreachable_goal_stays_within_budget) {
  // A goal sealed behind a solid wall floods the search. Without a budget it
  // would expand every one of a million cells.
  ObstructionGrid grid(64, 64);
  for (std::int32_t y = 0; y < 64; ++y) grid.set_cell(32, y, true);

  PathFinder finder;
  PathRequest request;
  request.start = Point{5 * 16 + 8, 30 * 16 + 8};
  request.goal = Point{60 * 16 + 8, 30 * 16 + 8};
  request.node_budget = 500;
  const Path path = finder.find(grid, request);
  CHECK(path.status == PathStatus::partial);
  CHECK(path.expanded <= 500);
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
  CHECK(front->placement_of("Unit") == FormationPlacement::front_line);
  CHECK(front->placement_of("Peasant") == FormationPlacement::central_block);
  // A class the formation does not mention takes the central block.
  CHECK(front->placement_of("Bowman") == FormationPlacement::central_block);

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

TEST(a_line_formation_puts_everyone_abreast) {
  Result<FormationTable> table = FormationTable::parse(as_bytes(kFormationsXml));
  REQUIRE(table.ok());
  const FormationClassDef* line = table->find("Line");
  REQUIRE(line != nullptr);

  std::vector<FormationMember> members(6);
  for (FormationMember& member : members) {
    member.placement = FormationPlacement::front_line;
    member.formation_radius = kDefaultFormationRadius;
  }
  std::vector<Point> offsets(members.size());
  CHECK(formation_offsets(*line, members, Point{0, kFacingLength}, offsets) == 6);

  // Six at 6:1 is one rank of six, all at the declared front-line offset.
  for (const Point& offset : offsets) CHECK(offset.y == 50);
  // Spread across the rank, centred, one spacing apart.
  CHECK(offsets[0].x == -130);
  CHECK(offsets[5].x == 130);
  for (std::size_t i = 1; i < offsets.size(); ++i) {
    CHECK(offsets[i].x - offsets[i - 1].x == 2 * kDefaultFormationRadius);
  }
}

TEST(a_front_formation_forms_ranks) {
  Result<FormationTable> table = FormationTable::parse(as_bytes(kFormationsXml));
  REQUIRE(table.ok());
  const FormationClassDef* front = table->find("Front");
  REQUIRE(front != nullptr);

  std::vector<FormationMember> members(6);
  for (FormationMember& member : members) {
    member.placement = FormationPlacement::front_line;
    member.formation_radius = kDefaultFormationRadius;
  }
  std::vector<Point> offsets(members.size());
  CHECK(formation_offsets(*front, members, Point{0, kFacingLength}, offsets) == 6);

  // Six at 2:1 is three abreast and two deep: the first rank at the declared
  // offset, the second one spacing behind it.
  CHECK(offsets[0].y == 30);
  CHECK(offsets[1].y == 30);
  CHECK(offsets[2].y == 30);
  CHECK(offsets[3].y == 30 + 2 * kDefaultFormationRadius);
  CHECK(offsets[4].y == 30 + 2 * kDefaultFormationRadius);
  CHECK(offsets[5].y == 30 + 2 * kDefaultFormationRadius);
  CHECK(offsets[0].x == -52);
  CHECK(offsets[1].x == 0);
  CHECK(offsets[2].x == 52);
}

TEST(wings_go_out_to_both_sides) {
  Result<FormationTable> table = FormationTable::parse(as_bytes(kFormationsXml));
  REQUIRE(table.ok());
  const FormationClassDef* cavalry = table->find("Cavalry core");
  REQUIRE(cavalry != nullptr);
  CHECK(cavalry->placement_of("Unit") == FormationPlacement::wings);
  CHECK(cavalry->placement_of("Horse") == FormationPlacement::front_line);

  std::vector<FormationMember> members(4);
  for (FormationMember& member : members) {
    member.placement = FormationPlacement::wings;
    member.formation_radius = kDefaultFormationRadius;
  }
  std::vector<Point> offsets(members.size());
  CHECK(formation_offsets(*cavalry, members, Point{0, kFacingLength}, offsets) == 4);

  // Alternating sides at the declared wing offset, filling outwards.
  CHECK(offsets[0].x == 50);
  CHECK(offsets[1].x == -50);
  CHECK(offsets[2].x == 50 + 2 * kDefaultFormationRadius);
  CHECK(offsets[3].x == -(50 + 2 * kDefaultFormationRadius));
  for (const Point& offset : offsets) CHECK(offset.y == 0);
}

TEST(a_formation_turns_with_its_leader) {
  Result<FormationTable> table = FormationTable::parse(as_bytes(kFormationsXml));
  REQUIRE(table.ok());
  const FormationClassDef* line = table->find("Line");
  REQUIRE(line != nullptr);

  std::vector<FormationMember> members(2);
  for (FormationMember& member : members) {
    member.placement = FormationPlacement::front_line;
    member.formation_radius = kDefaultFormationRadius;
  }
  std::vector<Point> facing_north(2);
  std::vector<Point> facing_east(2);
  (void)formation_offsets(*line, members, Point{0, kFacingLength}, facing_north);
  (void)formation_offsets(*line, members, Point{kFacingLength, 0}, facing_east);

  // Forward is local +y, so facing north the front line is at +y and facing
  // east it is at +x. The rotation is a rotation: lengths are preserved.
  CHECK(facing_north[0].y == 50);
  CHECK(facing_east[0].x == 50);
  for (std::size_t i = 0; i < 2; ++i) {
    CHECK(vector_length(facing_north[i]) == vector_length(facing_east[i]));
  }
}

TEST(formation_offsets_are_deterministic) {
  Result<FormationTable> table = FormationTable::parse(as_bytes(kFormationsXml));
  REQUIRE(table.ok());
  const FormationClassDef* front = table->find("Front");
  REQUIRE(front != nullptr);

  std::vector<FormationMember> members(9);
  for (std::size_t i = 0; i < members.size(); ++i) {
    members[i].id = static_cast<ObjectId>(i + 1);
    members[i].placement = i % 3 == 0 ? FormationPlacement::front_line
                                      : FormationPlacement::central_block;
    members[i].formation_radius = kDefaultFormationRadius;
  }
  std::vector<Point> first(members.size());
  std::vector<Point> second(members.size());
  (void)formation_offsets(*front, members, Point{300, -400}, first);
  (void)formation_offsets(*front, members, Point{300, -400}, second);
  for (std::size_t i = 0; i < first.size(); ++i) CHECK(first[i] == second[i]);
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

  HostCall(World& world, std::initializer_list<Value> args) : arguments(args) {
    context_state.world = &world;
    context.arguments = arguments;
    context.user = &context_state;
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
  CHECK(first.status == HostStatus::suspend);
  CHECK(first.value.as_integer() == 0);  // not arrived yet
  // 400 units at 100 per second is four seconds, which is more than the 2,000
  // ms slice the call was given, so it waits out the slice and the caller's
  // loop re-tests. A destination inside the slice waits only for the arrival:
  // the checks below cover both halves of that `min`.
  CHECK(first.suspend_for == 2000);

  const MoveState* state = movement.find(unit);
  REQUIRE(state != nullptr);
  CHECK(state->has_path);
  CHECK(state->target == (Point{500, 100}));
  CHECK(movement.eta(unit) == 4000);

  // The same order with a slice longer than the journey waits for the arrival.
  HostCall generous(world, {Value::object(kTypeObj, unit), pack_point(Point{500, 100}),
                            Value::integer(0), Value::integer(20000), Value::boolean(true),
                            Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "Goto", 5, generous).suspend_for == 4000);
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
TEST(an_object_goto_arrives_at_the_edge_and_a_point_goto_at_the_point) {
  EdgeField f;
  const ObjectId walker = f.place("Walker", Point{1000, 1000});
  const ObjectId camp = f.place("Camp", Point{1600, 1000});

  const HostOutcome first = f.go(walker, Value::object(kTypeObj, camp), 0);
  CHECK(first.status == HostStatus::suspend);
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
  CHECK(to_point.status == HostStatus::suspend);
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

TEST(a_host_goto_into_a_wall_reports_no_path) {
  HostRegistry registry;
  register_movement_host(registry);

  // A unit sealed in, so no route out exists at all.
  ObstructionGrid grid = open_field(32);
  for (std::int32_t d = -1; d <= 1; ++d) {
    grid.set_cell(9 + d, 9, true);
    grid.set_cell(9 + d, 11, true);
    grid.set_cell(8, 10 + d, true);
    grid.set_cell(10, 10 + d, true);
  }

  World world;
  MovementSystem movement;
  movement.set_grid(std::move(grid));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{9 * 16 + 8, 10 * 16 + 8}, 100);

  HostCall call(world, {Value::object(kTypeObj, unit), pack_point(Point{2000, 2000}),
                        Value::integer(0), Value::integer(2000), Value::boolean(true),
                        Value::integer(0)});
  const HostOutcome outcome = invoke(registry, CallKind::member, "Goto", 5, call);
  // False *and* no path, so `while (!.Goto(...) && .HasPath)` leaves.
  CHECK(outcome.value.as_integer() == 0);
  HostCall has_path(world, {Value::object(kTypeObj, unit)});
  CHECK(invoke(registry, CallKind::member, "HasPath", 0, has_path).value.as_integer() == 0);

  // And it **waits** rather than answering instantly, which the loops that have
  // no `.HasPath` guard need. `UNIT_MOVE.VS` is the whole script
  // `while (!.Goto(pt, 0, 2000, true, 0));` and `UNIT_ENTER.VS` writes
  // `while (!.Goto(pt, 0, 1000, true, 5000));`: an immediate `ok` there spins
  // the VM against an unreachable point until the instruction budget traps it.
  // Suspending for the caller's slice turns that into a poll, and does not
  // change the guarded form, which leaves on `.HasPath` either way.
  CHECK(outcome.status == HostStatus::suspend);
  CHECK(outcome.suspend_for == 2000);
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

TEST(a_host_goto_that_cannot_reach_still_times_out) {
  // The give-up clock has to keep running while there is no route, or
  // `UNIT_ENTER.VS`'s unguarded `while (!.Goto(pt, 0, 1000, true, 5000));`
  // never leaves. Keying continuation on `has_path` restarts it every call;
  // `MoveState::goto_active` is what keeps it running.
  HostRegistry registry;
  register_movement_host(registry);

  ObstructionGrid grid = open_field(32);
  for (std::int32_t d = -1; d <= 1; ++d) {
    grid.set_cell(9 + d, 9, true);
    grid.set_cell(9 + d, 11, true);
    grid.set_cell(8, 10 + d, true);
    grid.set_cell(10, 10 + d, true);
  }

  World world;
  MovementSystem movement;
  movement.set_grid(std::move(grid));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{9 * 16 + 8, 10 * 16 + 8}, 100);

  const auto call_once = [&]() {
    HostCall call(world, {Value::object(kTypeObj, unit), pack_point(Point{2000, 2000}),
                          Value::integer(0), Value::integer(1000), Value::boolean(true),
                          Value::integer(5000)});
    return invoke(registry, CallKind::member, "Goto", 5, call);
  };

  // Five seconds of game time in one-second turns, then the order gives up.
  std::int64_t waited = 0;
  bool gave_up = false;
  for (int i = 0; i < 12 && !gave_up; ++i) {
    const HostOutcome outcome = call_once();
    CHECK(outcome.value.as_integer() == 0);
    if (outcome.status == HostStatus::suspend) {
      waited += outcome.suspend_for;
      world.advance(static_cast<std::int32_t>(outcome.suspend_for));
    } else {
      gave_up = true;
    }
  }
  CHECK(gave_up);
  CHECK(waited == 5000);
}

TEST(a_host_stop_reports_true_and_holds_still) {
  // 34 of `Stop`'s 53 sites are `while (!.Stop(1000));`. A void `Stop` yields
  // nil, `!nil` is true, and every one of those loops spins forever -- so it
  // returns a bool. The suspension is what makes `UNIT_IDLE.VS`'s and
  // `SHIP_IDLE.VS`'s `while(1)` consume game time rather than instructions.
  HostRegistry registry;
  register_movement_host(registry);

  World world;
  MovementSystem movement;
  movement.set_grid(open_field(64));
  attach(world, movement);
  const ObjectId unit = spawn_unit(world, movement, Point{500, 500}, 100);

  HostCall stop(world, {Value::object(kTypeObj, unit), Value::integer(1000)});
  const HostOutcome outcome = invoke(registry, CallKind::member, "Stop", 1, stop);
  CHECK(outcome.value.as_integer() == 1);
  CHECK(outcome.status == HostStatus::suspend);
  CHECK(outcome.suspend_for == 1000);
  CHECK(!world.state(unit)->flags.has_active_path);
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
  CHECK(invoke(registry, CallKind::member, "Goto", 5, go).status == HostStatus::suspend);
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

TEST(set_formation_refuses_a_name_the_file_does_not_declare) {
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

  HostCall bad(world, {Value::object(kTypeObj, hero), Value::string("Testudo")});
  CHECK(invoke(registry, CallKind::member, "SetFormation", 1, bad).status == HostStatus::error);

  HostCall good(world, {Value::object(kTypeObj, hero), Value::string("Line")});
  CHECK(invoke(registry, CallKind::member, "SetFormation", 1, good).status == HostStatus::ok);
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

/// `DoCarryNothing` is the walk slot at or below the default; the two carry
/// setters move it to 16 or 17 for the ambient villager classes and leave
/// every other class alone.
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
  CHECK(movement.state(water).walk_anim == 16);
  CHECK(!carries_nothing(water));
  set("SetCarryGoodsAnim", water);
  CHECK(movement.state(water).walk_anim == 17);
  // A plain villager carries goods on the water slot and no water at all.
  set("SetCarryWaterAnim", plain);
  CHECK(movement.state(plain).walk_anim == 1);
  set("SetCarryGoodsAnim", plain);
  CHECK(movement.state(plain).walk_anim == 16);
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
  // And a handle to nothing answers false.
  CHECK(!carries_nothing(999999));
}
