// Flight: sim/flying.hpp, plus the two entry points that came out of the same
// reading and live elsewhere -- `MIN`/`MAX` in `world_host` and `ForceIdle` in
// `command`.
//
// No game data anywhere in this file. The height layer is eight cells of a
// hand-built `GRID` whose values are `16 * x + y`, so that every interpolation
// below can be read off the arithmetic rather than trusted.
//
// The cases that carry the reading are:
//
//   * **The crow takes off, and the flag that says so finally has a writer.**
//     `in_air` is `!(z_from == ground && z == -1)`, so the three-animation
//     shape `CROW_IDLE.VS` ends with -- fly, land, transition -- is the one
//     that puts a bird up and brings it back down.
//   * **A quarter turn of the scan is 119 units, not 120.** The scan's pi is
//     6.28/360 and nothing else in the executable uses it.
//   * **Both interpolations truncate toward zero**, so a rising slope and a
//     falling one are not mirror images of each other.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "builder.hpp"
#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/flying.hpp"
#include "imperivm/core/sim/projection.hpp"
#include "imperivm/core/sim/heading.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

/// `Point{a, b}` cannot go inside a `CHECK`, because the comma is the
/// preprocessor's before it is C++'s.
[[nodiscard]] Point pt(std::int32_t x, std::int32_t y) { return Point{x, y}; }

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Four 200 ms animations, on a three-row sheet -- the strip is fitted to the
/// image, so a one-row sheet would keep only one frame's hold and the cycle
/// would be 100. Three of them are at the indices `CROW_IDLE.VS`
/// asks for -- `Flying::PlayAnim` passes its argument raw into the array
/// `Obj::PlayAnim` decrements into, so its `16` is slot 17 -- and the 200 is
/// the number `flying_z` divides by.
constexpr std::string_view kEntityXml = R"(<?xml version="1.0"?>
<entity name="bird" type="vx/unit" variations="1">
  <images>
    <image idx="1" file="fly.rle" drawmode="player_color" remaping="none" rows="3" columns="8"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="1" anim_idx="1" anim_frame="1"/>
  </states>
  <anims>
    <anim idx="1" name="fly" startstate="1" endstate="1" frames="3" duration="200"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="100"/>
    </anim>
    <anim idx="17" name="takeoff" startstate="1" endstate="1" frames="3" duration="200"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="100"/>
    </anim>
    <anim idx="18" name="land" startstate="1" endstate="1" frames="3" duration="200"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="100"/>
    </anim>
    <anim idx="19" name="settle" startstate="1" endstate="1" frames="3" duration="200"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="100"/>
    </anim>
  </anims>
</entity>)";

/// A `GRID` in the shipped layout, one byte per cell, `cells` on a side.
std::vector<std::byte> byte_grid(std::uint32_t cell_size, std::uint32_t cells,
                                 bool descending) {
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(cell_size).u32(8).u32(cell_size * cells).u32(cell_size * cells);
  for (std::uint32_t y = 0; y < cells; ++y) {
    for (std::uint32_t x = 0; x < cells; ++x) {
      const std::uint32_t v = 16 * x + y;
      out.u8(descending ? 119 - v : v);
    }
  }
  return {out.span().begin(), out.span().end()};
}

/// A height layer that is the same everywhere -- flat ground, which is where
/// the projection's vertical scale is visible on its own.
std::vector<std::byte> level_grid(std::uint32_t cell_size, std::uint32_t cells,
                                  std::uint32_t value) {
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(cell_size).u32(8).u32(cell_size * cells).u32(cell_size * cells);
  for (std::uint32_t i = 0; i < cells * cells; ++i) out.u8(value);
  return {out.span().begin(), out.span().end()};
}

/// A `.pass`-shaped layer with nothing blocked. **Not optional**: an
/// unconfigured `ObstructionGrid` has zero width, and `blocked()` answers true
/// off the grid on purpose, so a movement system with no grid refuses every
/// point on the map.
std::vector<std::byte> free_pass_grid(std::uint32_t cells) {
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(16).u32(1).u32(16 * cells).u32(16 * cells);
  const std::uint32_t stride = (cells + 7) / 8;
  out.zeros(static_cast<std::size_t>(stride) * cells);
  return {out.span().begin(), out.span().end()};
}

/// A terrain-type layer that is water in one 64-unit cell and land everywhere
/// else. 13 is the type `IsPointInWater` tests for.
std::vector<std::byte> terrain_grid(std::uint32_t cells, std::uint32_t water_x,
                                    std::uint32_t water_y) {
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(64).u32(8).u32(64 * cells).u32(64 * cells);
  for (std::uint32_t y = 0; y < cells; ++y) {
    for (std::uint32_t x = 0; x < cells; ++x) out.u8(x == water_x && y == water_y ? 13 : 1);
  }
  return {out.span().begin(), out.span().end()};
}

ClassGraph bird_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Unit" cpp_class="CVXUnit" parent="">
      <properties maxhealth="100" speed="50" sight="900" radius="10"/>
    </class>)"),
            "unit.sc.xml");
  graph.add(bytes_of(R"(<class id="Crow" cpp_class="CVXFlyingUnit" parent="Unit">
      <properties sight="900"/>
    </class>)"),
            "crow.sc.xml");
  graph.add(bytes_of(R"(<class id="Fish" cpp_class="CVXUnit" parent="Unit">
      <properties water_unit="1"/>
    </class>)"),
            "fish.sc.xml");
  // The only shipped class that declares `initial_z`, and its shipped value;
  // and its shipped `speed`, which is not the parent's.
  graph.add(bytes_of(R"(<class id="Eagle" cpp_class="CVXFlyingUnit" parent="Unit">
      <properties initial_z="200" speed="180"/>
    </class>)"),
            "eagle.sc.xml");
  graph.link();
  return graph;
}

/// Eight cells of 32 units is a 256-unit map, which is what `map_size` says, so
/// the height layer and the map rectangle are the same square -- as they are on
/// every shipped map.
constexpr std::int32_t kCells = 8;
constexpr std::int32_t kHeightCell = 32;
constexpr std::int32_t kMapSize = kCells * kHeightCell;

struct FlightBench {
  ClassGraph graph = bird_graph();
  Result<Entity> entity = Entity::parse(bytes_of(kEntityXml));
  std::vector<std::byte> height_bytes = byte_grid(kHeightCell, kCells, false);
  std::vector<std::byte> terrain_bytes = terrain_grid(kCells / 2, 0, 0);
  std::vector<std::byte> pass_bytes = free_pass_grid(kMapSize / 16);
  World world{TickConfig{100, kDefaultGameSpeed}};
  MatchSystem match;
  MovementSystem movement;
  script::HostRegistry registry;
  HostContext context;
  ClassIndex crow_class = kNoClass;
  ClassIndex fish_class = kNoClass;
  ClassIndex eagle_class = kNoClass;

  FlightBench() {
    REQUIRE(entity.ok());
    world.set_class_graph(&graph);
    crow_class = graph.find("Crow");
    fish_class = graph.find("Fish");
    eagle_class = graph.find("Eagle");
    world.add_system(&match);
    world.add_system(&movement);
    MatchRules rules;
    rules.map_size = kMapSize;
    (void)setup_match(world, match, rules, MatchOptions{});

    const Result<Grid> height = Grid::parse(height_bytes);
    REQUIRE(height.ok());
    world.set_height(height.value());
    const Result<Grid> terrain = Grid::parse(terrain_bytes);
    REQUIRE(terrain.ok());
    world.set_terrain(terrain.value());
    const Result<Grid> pass = Grid::parse(pass_bytes);
    REQUIRE(pass.ok());
    Result<ObstructionGrid> obstruction = ObstructionGrid::from_grid(pass.value());
    REQUIRE(obstruction.ok());
    movement.set_grid(std::move(obstruction.value()));

    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  ObjectId spawn(Point at, ClassIndex cls, NativeClass native = NativeClass::unit) {
    const ObjectId id = world.spawn(native, &entity.value(), cls);
    (void)world.set_position(id, at);
    world.set_owner(id, 1);
    world.set_health(id, 100);
    if (WorldObject* slot = world.find(id); slot != nullptr) slot->sight = 900;
    return id;
  }

  ObjectId crow(Point at) { return spawn(at, crow_class); }

  void face(ObjectId id, Point towards) { movement.face(world, id, towards); }

  /// The arguments the last call was given, kept alive because a
  /// `CallContext` views them -- and because the by-reference entry points
  /// write their answers back into them.
  std::vector<script::Value> arguments;

  script::HostOutcome call(script::CallKind kind, const char* name, std::uint16_t arity,
                           std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(kind, name, arity);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    CHECK(registry.entry(index).fn != nullptr);
    if (registry.entry(index).fn == nullptr) {
      return script::HostOutcome::failed("not implemented");
    }
    arguments = std::move(args);
    script::CallContext ctx;
    ctx.arguments = arguments;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    return registry.entry(index).fn(ctx);
  }

  static script::Value obj(ObjectId id) { return script::Value::object(kTypeObj, id); }
  script::Value point(Point p) { return pack_point(p); }
};

}  // namespace

// --------------------------------------------------------------------------
// the height layer
// --------------------------------------------------------------------------

/// The sample is exact on a cell corner and bilinear between them, and **both
/// divisions truncate toward zero**.
///
/// The layer is `16 * x + y`, so a step of one cell to the right is 16 and a
/// step down is 1. At x = 15 the horizontal term is `16 * 15 / 32`, which is
/// 7.5 -- and the two grids below are the same slope with opposite signs, so a
/// rounding rule that went to the nearest, or to minus infinity, would make
/// them mirror images. They are not: both answers move *toward* the cell the
/// sample started in.
TEST(flying_height_sample_is_bilinear_and_truncates_toward_zero) {
  const std::vector<std::byte> rising = byte_grid(kHeightCell, kCells, false);
  const std::vector<std::byte> falling = byte_grid(kHeightCell, kCells, true);
  const Result<Grid> up = Grid::parse(rising);
  const Result<Grid> down = Grid::parse(falling);
  REQUIRE(up.ok());
  REQUIRE(down.ok());

  // Corners are the cell itself.
  CHECK(sample_height(up.value(), pt(0, 0)) == 0);
  CHECK(sample_height(up.value(), pt(32, 0)) == 16);
  CHECK(sample_height(up.value(), pt(0, 32)) == 1);
  CHECK(sample_height(up.value(), pt(64, 96)) == 35);  // 16*2 + 3

  // Halfway between two cells, on each axis and on both.
  CHECK(sample_height(up.value(), pt(16, 0)) == 8);
  CHECK(sample_height(up.value(), pt(16, 16)) == 8);
  CHECK(sample_height(up.value(), pt(8, 24)) == 4);

  // 16 * 15 / 32 is 7.5 in both directions, and both land on 7 rather than 8.
  CHECK(sample_height(up.value(), pt(15, 0)) == 7);
  CHECK(sample_height(down.value(), pt(15, 0)) == 119 - 7);
}

/// The projection inverse answers **within one screen unit**, and never
/// exactly. That is the algorithm, and it is worth a test that says so.
///
/// The forward rule is `screen.y = world.y * 181 / 256 - height`. It is
/// many-to-one, so a world -> screen -> world round trip cannot be the identity
/// and asserting it would be asserting the wrong thing. The other direction is
/// not the identity either: the scan brackets the answer in a **32-world-unit**
/// window and closes with one *linear* interpolation, while the function inside
/// that window is a truncating scale -- so `screen -> world -> screen` lands
/// one off on about a third of all rows even on dead flat ground, where there
/// is no height term at all. 0x006243b0 does exactly this; solving the algebra
/// instead would be strictly *more* accurate than the original and would answer
/// differently wherever the projection folds.
TEST(flying_screen_to_world_answers_within_one_screen_unit_and_not_exactly) {
  World world;
  const std::vector<std::byte> flat = level_grid(kHeightCell, kCells, 0);
  const Result<Grid> layer = Grid::parse(flat);
  REQUIRE(layer.ok());
  world.set_height(layer.value());

  // Flat ground: the height term vanishes and the scale is all that is left.
  CHECK(world_to_screen(world, pt(70, 0)) == pt(70, 0));
  CHECK(world_to_screen(world, pt(70, 32)) == pt(70, 22));   // 32 * 181 / 256
  CHECK(world_to_screen(world, pt(70, 33)) == pt(70, 23));

  std::int32_t off_by_one = 0;
  for (std::int32_t screen_y = 0; screen_y < 160; ++screen_y) {
    const Point back = screen_to_world(world, pt(70, screen_y));
    CHECK(back.x == 70);  // the horizontal scale is exactly 1
    const std::int32_t off = world_to_screen(world, back).y - screen_y;
    CHECK(off >= -1);
    CHECK(off <= 1);
    if (off != 0) ++off_by_one;
  }
  // Half a hundred of the hundred and sixty. A bound nothing violated would
  // pass against a body that had solved the algebra, which is the reading this
  // is here to rule out.
  CHECK(off_by_one == 50);
}

/// Over ground that rises the scan has to **march**, which is the case it
/// exists for, and it keeps the same one-unit accuracy while doing it.
TEST(flying_screen_to_world_marches_up_a_slope_and_still_closes) {
  World world;
  // `16 * x + y` per cell, so at x = 0 the height rises by 1 every 32 units --
  // gentle enough that the projection stays monotone, which is the regime the
  // round trip is exact in.
  const std::vector<std::byte> rising = byte_grid(kHeightCell, kCells, false);
  const Result<Grid> layer = Grid::parse(rising);
  REQUIRE(layer.ok());
  world.set_height(layer.value());

  // The height term is a subtraction, so a screen row picks out a *farther*
  // world row than it would on the flat.
  CHECK(world_to_screen(world, pt(0, 64)) == pt(0, 45 - 2));
  const Point picked = screen_to_world(world, pt(0, 43));
  CHECK(picked == pt(0, 64));

  bool exact_everywhere = true;
  for (std::int32_t screen_y = 0; screen_y < 140; ++screen_y) {
    const Point back = screen_to_world(world, pt(0, screen_y));
    const std::int32_t off = world_to_screen(world, back).y - screen_y;
    CHECK(off >= -1);
    CHECK(off <= 1);
    if (off != 0) exact_everywhere = false;
  }
  // And it really is inexact here: a bound nothing violates would pass against
  // a body that had solved the algebra instead.
  CHECK(!exact_everywhere);

  // A steep column, where the terrain climbs 16 per cell across x. The scan is
  // not required to be exact here -- the projection folds once the ground rises
  // faster than the scale -- but it must terminate and stay in the world.
  for (std::int32_t screen_y = -40; screen_y < 200; screen_y += 7) {
    const Point back = screen_to_world(world, pt(200, screen_y));
    CHECK(back.x == 200);
    CHECK(back.y > -1000);
    CHECK(back.y < 100000);
  }
}

/// On ground steeper than the projection's own slope the mapping **folds**, and
/// the scan walks straight past the fold -- which is what makes the caller's
/// bounds check load bearing rather than defensive.
///
/// The forward projection is monotone in `world.y` only while the terrain rises
/// more slowly than about 0.707 units per unit. Steeper than that and it turns
/// over: on the cliff below, `world.y` 0, 32, 64 … project to screen 0, -8,
/// -15 …, *decreasing*. The scan marches until it overshoots, so it can answer
/// with a row far past the cliff -- here, one **outside the map entirely**.
/// 0x005c11a0 rejects exactly that with its own map-rectangle test at step 7,
/// and this is the case that test exists for.
TEST(flying_screen_to_world_walks_past_a_fold_and_can_leave_the_map) {
  World world;
  // 30 units of height per 32 of distance, in y -- comfortably past the fold.
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(kHeightCell).u32(8).u32(kHeightCell * kCells).u32(kHeightCell * kCells);
  for (std::uint32_t y = 0; y < kCells; ++y) {
    for (std::uint32_t x = 0; x < kCells; ++x) out.u8(y * 30);
  }
  const std::vector<std::byte> cliff{out.span().begin(), out.span().end()};
  const Result<Grid> layer = Grid::parse(cliff);
  REQUIRE(layer.ok());
  world.set_height(layer.value());

  // The fold, stated: going *down* the map moves *up* the screen.
  CHECK(world_to_screen(world, pt(10, 0)).y == 0);
  CHECK(world_to_screen(world, pt(10, 32)).y == -8);
  CHECK(world_to_screen(world, pt(10, 64)).y == -15);

  // Screen row 0 therefore picks a world row past the far edge of a 256-unit
  // map, and it is still the honest answer: it projects back to where it was
  // asked from.
  const Point far_side = screen_to_world(world, pt(10, 0));
  CHECK(far_side.y > kCells * kHeightCell);
  CHECK(world_to_screen(world, far_side).y >= -1);
  CHECK(world_to_screen(world, far_side).y <= 1);

  // And a screen row above the origin picks a world row above it -- negative,
  // which is off the other edge.
  const Point above = screen_to_world(world, pt(10, -30));
  CHECK(above.y < 0);
  CHECK(world_to_screen(world, above).y == -30);
}

/// The last cell on each axis is flat, because the original clamps the
/// neighbour rather than reading past it (0x0053dc98).
TEST(flying_height_sample_clamps_at_the_far_edge) {
  const std::vector<std::byte> rising = byte_grid(kHeightCell, kCells, false);
  const Result<Grid> up = Grid::parse(rising);
  REQUIRE(up.ok());
  // Cell 7 is the last: every point inside it answers the cell's own value.
  CHECK(sample_height(up.value(), pt(224, 0)) == 112);
  CHECK(sample_height(up.value(), pt(240, 0)) == 112);
  CHECK(sample_height(up.value(), pt(255, 0)) == 112);
  // And a layer with no cells at all is sea level, not a crash.
  CHECK(sample_height(Grid{}, pt(100, 100)) == 0);
}

/// `GetTerrainHeight` clamps into the map rectangle before it samples
/// (0x0051b4f5), so a point off the map answers about the nearest point on it.
///
/// 30,000 rather than a rounder number because a script point is **sixteen bits
/// per component** (`pack_point`), so anything past 32,767 is a different point
/// by the time the entry point sees it.
TEST(flying_get_terrain_height_clamps_into_the_map) {
  FlightBench b;
  CHECK(b.call(script::CallKind::free_function, "GetTerrainHeight", 1,
               {b.point(pt(-4000, -4000))})
            .value.as_integer() == 0);
  CHECK(b.call(script::CallKind::free_function, "GetTerrainHeight", 1,
               {b.point(pt(30000, 0))})
            .value.as_integer() == 112);
  CHECK(b.call(script::CallKind::free_function, "GetTerrainHeight", 1, {b.point(pt(16, 16))})
            .value.as_integer() == 8);
}

// --------------------------------------------------------------------------
// the scan
// --------------------------------------------------------------------------

/// The scan's quarter turn is short by a unit, and its half turn is short by a
/// unit in the other direction, and neither is a rounding accident.
///
/// `PickFlyingPoint` multiplies by `0x7beb58`, which is 6.28/360 -- so a
/// "quarter turn" is 1.5700 radians rather than pi/2, its cosine is 7.96e-4
/// rather than zero, and `trunc(120 * sin)` is 119. The three constants are
/// laid out in `sim/heading.hpp`; this is the one nothing else uses.
TEST(flying_scan_offsets_use_the_third_pi) {
  CHECK(scan_offset(120, 0) == pt(120, 0));
  CHECK(scan_offset(120, 3) == pt(0, 119));
  CHECK(scan_offset(120, 6) == pt(-119, 0));
  CHECK(scan_offset(120, 9) == pt(0, -119));
  // Out of the twelve is not a direction at all.
  CHECK(scan_offset(120, 12) == pt(0, 0));
  CHECK(scan_offset(120, -1) == pt(0, 0));
}

// --------------------------------------------------------------------------
// AdjustFlyDir
// --------------------------------------------------------------------------

/// Away from every edge the argument comes back untouched, and so it does when
/// it already points away from the edge that is near.
TEST(flying_adjust_fly_dir_leaves_a_good_heading_alone) {
  const std::int32_t high = 8191;
  // The middle of the map: no edge is within 800 units.
  CHECK(adjust_fly_dir(pt(100, 0), pt(0, -100), pt(4000, 4000), high) ==
        pt(0, -100));
  // Near the low-y edge, heading north, which is away from it.
  CHECK(adjust_fly_dir(pt(100, 0), pt(0, 100), pt(4000, 500), high) == pt(0, 100));
  // Near the high-x edge, heading west, which is away from it. The whole
  // problem is rotated three quarter turns and the test lands on the same
  // "already heading away" branch.
  CHECK(adjust_fly_dir(pt(0, 100), pt(-100, 0), pt(7500, 4000), high) ==
        pt(-100, 0));
}

/// Heading into the near edge, the bird's *own* heading is turned fifteen
/// degrees and the argument is replaced by the turn.
///
/// The constant is `0x7bea98`, which is `15 * 0.017453292519444445` -- the same
/// pi `point::Rot` uses and not the scan's -- so the answer is
/// `(trunc(100*cos 15), trunc(100*sin 15))`, which is `(96, 25)` rather than
/// `(97, 26)`.
TEST(flying_adjust_fly_dir_turns_fifteen_degrees_toward_open_map) {
  const std::int32_t high = 8191;
  // Near the low-y edge, pointing into it, heading east.
  CHECK(adjust_fly_dir(pt(100, 0), pt(0, -100), pt(4000, 500), high) == pt(96, 25));
  // Heading west instead: the turn goes the other way, and the y that comes
  // out is what makes it acceptable.
  CHECK(adjust_fly_dir(pt(-100, 0), pt(0, -100), pt(4000, 500), high) ==
        pt(-96, 25));
}

/// **In a corner, x decides.** The original tests the two x edges before either
/// y edge and the last case is the fall-through, so a bird past both high edges
/// is treated as being near the high-*x* one.
///
/// Nothing in the shipped content can tell -- a crow flies into a corner rarely
/// and steers out of it either way -- which is exactly why the order has to be
/// transcribed rather than chosen: two readings that agree everywhere the
/// corpus goes and disagree on what the code means.
TEST(flying_adjust_fly_dir_decides_a_corner_by_x) {
  const std::int32_t high = 8191;
  // Past both high edges. Heading south-west-ish, argument pointing west --
  // which, once the frame is turned three quarters for the high-x edge, is
  // already heading away, so the argument comes back untouched.
  CHECK(adjust_fly_dir(pt(0, -100), pt(-100, 0), pt(7500, 7500), high) == pt(-100, 0));
  // Turned two quarters instead -- for the high-y edge -- the same call steers.
}

/// The whole problem really is rotated, so the same case near a different edge
/// gives the same answer turned by the same number of quarter turns.
TEST(flying_adjust_fly_dir_is_the_same_problem_at_every_edge) {
  const std::int32_t high = 8191;
  const Point south = adjust_fly_dir(pt(100, 0), pt(0, -100), pt(4000, 500), high);
  // Near the low-x edge, the frame turns once. The heading and the argument are
  // chosen so that one quarter turn maps them onto the case above exactly.
  const Point west = adjust_fly_dir(pt(0, -100), pt(-100, 0), pt(500, 4000), high);
  // Undoing one quarter turn is `(x, y) -> (y, -x)`.
  CHECK(west == pt(south.y, -south.x));
}

// --------------------------------------------------------------------------
// the flight state
// --------------------------------------------------------------------------

/// The crow's three-animation take-off and landing, and the flag that has never
/// been true before.
///
/// `CROW_IDLE.VS` flies with `PlayAnim(16, pt, 80 + GetTerrainHeight(.pos))`,
/// lands with `PlayAnim(17, ptLand, -1)` and settles with
/// `PlayAnim(18, .pos, -1)`. `in_air` is `!(z_from == ground && z == -1)`, so
/// the first puts the bird up, the second keeps it up because it is still high,
/// and only the third -- which starts from the ground the second one reached --
/// brings it down.
TEST(flying_play_anim_is_the_only_writer_of_in_air) {
  FlightBench b;
  const ObjectId id = b.crow(pt(16, 16));
  const WorldObject* slot = b.world.find(id);
  REQUIRE(slot != nullptr);
  CHECK(!slot->state.flags.in_air);
  CHECK(!slot->state.flags.landing);

  const std::int32_t ground = terrain_height(b.world, pt(16, 16));
  CHECK(ground == 8);

  // Take off toward a point one cell east, at 80 above the ground.
  b.call(script::CallKind::member, "PlayAnim", 3,
         {FlightBench::obj(id), script::Value::integer(16), b.point(pt(48, 16)),
          script::Value::integer(80 + ground)});
  slot = b.world.find(id);
  REQUIRE(slot != nullptr);
  CHECK(slot->state.flags.in_air);
  CHECK(!slot->state.flags.landing);
  CHECK(slot->state.z_from == ground);
  CHECK(slot->state.z_to == 80 + ground);
  // And the bird arrived, which is what makes `CROW_MOVE.VS` terminate.
  CHECK(b.world.resolve_position(id) == pt(48, 16));

  // Let the animation finish, so `z` is the altitude it climbed to.
  b.world.advance_turns(4);
  CHECK(flying_z(b.world, *b.world.find(id)) == 80 + ground);

  // Land: still up, and now descending.
  b.call(script::CallKind::member, "PlayAnim", 3,
         {FlightBench::obj(id), script::Value::integer(17), b.point(pt(48, 16)),
          script::Value::integer(-1)});
  slot = b.world.find(id);
  REQUIRE(slot != nullptr);
  CHECK(slot->state.flags.in_air);
  CHECK(slot->state.flags.landing);
  CHECK(slot->state.z_from == 80 + ground);
  CHECK(slot->state.z_to == terrain_height(b.world, pt(48, 16)));
  b.world.advance_turns(4);

  // Transition to idle, from the ground the landing reached: down at last.
  b.call(script::CallKind::member, "PlayAnim", 3,
         {FlightBench::obj(id), script::Value::integer(18), b.point(pt(48, 16)),
          script::Value::integer(-1)});
  slot = b.world.find(id);
  REQUIRE(slot != nullptr);
  CHECK(!slot->state.flags.in_air);
  CHECK(slot->state.flags.landing);
}

/// A bird in the air is drawn with its body lifted by its altitude above the
/// ground and its shadow left on the ground: `0x0051b240` offsets every layer
/// at depth 1000 or 1050 by `GetTerrainHeight(pos) - Flying::z`, clamped to
/// no more than zero, and only while the airborne bit is set.
///
/// This was the ravens' smear: every airborne crow was drawn standing on the
/// ground under itself, five of them in a few dozen pixels.
TEST(flying_lift_raises_a_birds_body_by_its_altitude_above_the_ground) {
  FlightBench b;
  const ObjectId id = b.spawn(pt(16, 16), b.crow_class, NativeClass::flying_unit);
  const std::int32_t ground = terrain_height(b.world, pt(16, 16));
  CHECK(flying_lift(b.world, *b.world.find(id)) == 0);  // on the ground

  b.call(script::CallKind::member, "PlayAnim", 3,
         {FlightBench::obj(id), script::Value::integer(16), b.point(pt(48, 16)),
          script::Value::integer(80 + ground)});
  // Mid-climb: what `Flying::z` says, less the ground under the bird.
  b.world.advance_turns(1);
  const WorldObject* slot = b.world.find(id);
  const std::int32_t under = terrain_height(b.world, b.world.resolve_position(id));
  const std::int32_t mid = flying_lift(b.world, *slot);
  CHECK(mid == flying_z(b.world, *slot) - under);
  CHECK(mid > 0);
  CHECK(mid < 80 + ground - under);
  // At the top of the climb.
  b.world.advance_turns(4);
  slot = b.world.find(id);
  CHECK(flying_lift(b.world, *slot) == 80 + ground - under);

  // Below the ground is drawn on it, not under it.
  b.world.find(id)->state.z_from = under - 20;
  b.world.find(id)->state.z_to = under - 20;
  CHECK(flying_lift(b.world, *b.world.find(id)) == 0);

  // Only a flying unit: the same altitude on a plain unit lifts nothing,
  // because `0x0051b240` is the flying unit's own visual update.
  const ObjectId walker = b.spawn(pt(16, 16), b.crow_class);
  b.world.find(walker)->state.flags.in_air = true;
  b.world.find(walker)->state.z_from = 200;
  CHECK(flying_z(b.world, *b.world.find(walker)) == 200);
  CHECK(flying_lift(b.world, *b.world.find(walker)) == 0);

  // The layers it moves: the two body depths, and not the shadow's.
  CHECK(flying_lifts_layer(1000));
  CHECK(flying_lifts_layer(1050));
  CHECK(!flying_lifts_layer(800));
  CHECK(!flying_lifts_layer(1001));
  CHECK(!flying_lifts_layer(0));
}

/// **`initial_z` is the one class property that starts an object in the air**,
/// and exactly one shipped class declares it.
///
/// `gbr.exe` 0x0051af6a: the `Flying` constructor reads the class descriptor's
/// `+0xb44` and, when it is not the `0xff1b1e40` sentinel, writes it into
/// `z_from` and ORs in the airborne bit. The property is `initial_z` -- the
/// string at 0x7c8940, stored by the class reader at 0x005a20fd -- and
/// `EAGLE.SC.XML` is the only one of the 845 shipped classes that carries it,
/// at 200. So an eagle is up from the moment it is placed and a crow is not,
/// which is what their two idle scripts each open by assuming.
TEST(flying_initial_z_puts_an_eagle_in_the_air_at_spawn) {
  FlightBench b;
  const ObjectId crow = b.crow(pt(100, 100));
  const WorldObject* on_ground = b.world.find(crow);
  REQUIRE(on_ground != nullptr);
  CHECK(!on_ground->state.flags.in_air);
  CHECK(on_ground->state.z_from == 0);

  const ObjectId eagle = b.spawn(pt(100, 100), b.eagle_class);
  const WorldObject* aloft = b.world.find(eagle);
  REQUIRE(aloft != nullptr);
  CHECK(aloft->state.flags.in_air);
  CHECK(aloft->state.z_from == 200);
  // And `z` answers the stored altitude rather than the ground, because the
  // bird is in the air and has no animation to interpolate along yet.
  CHECK(flying_z(b.world, *aloft) == 200);
}

/// **The ground `in_air` is compared against is the ground under the *bird*,
/// not under the destination.** 0x0051bc36 asks the object where it is and
/// samples there.
///
/// A crow standing at ground level that is told to descend somewhere else stays
/// on the ground: `z_from` is 8, the ground beneath it is 8, and the call is a
/// landing, so the flag clears. Sampled at the destination -- which on this
/// slope is 24 -- the comparison fails and the bird takes off by descending.
TEST(flying_in_air_reads_the_ground_under_the_bird) {
  FlightBench b;
  const ObjectId id = b.crow(pt(16, 16));
  CHECK(terrain_height(b.world, pt(16, 16)) == 8);
  CHECK(terrain_height(b.world, pt(200, 16)) == 100);

  b.call(script::CallKind::member, "PlayAnim", 3,
         {FlightBench::obj(id), script::Value::integer(16), b.point(pt(200, 16)),
          script::Value::integer(-1)});
  const WorldObject* slot = b.world.find(id);
  REQUIRE(slot != nullptr);
  CHECK(!slot->state.flags.in_air);
  CHECK(slot->state.flags.landing);
}

/// A z outside `-1 .. 1024` starts nothing and writes nothing (0x0051bb6f).
TEST(flying_play_anim_refuses_an_out_of_range_z) {
  FlightBench b;
  const ObjectId id = b.crow(pt(16, 16));
  b.call(script::CallKind::member, "PlayAnim", 3,
         {FlightBench::obj(id), script::Value::integer(16), b.point(pt(48, 16)),
          script::Value::integer(2000)});
  const WorldObject* slot = b.world.find(id);
  REQUIRE(slot != nullptr);
  CHECK(!slot->state.flags.in_air);
  CHECK(b.world.resolve_position(id) == pt(16, 16));
}

/// `Flying::z` has three answers and the middle one is a lerp on the animation
/// clock -- the same clock, and the same fraction, the object's position is
/// moving on.
TEST(flying_z_interpolates_over_the_animation) {
  FlightBench b;
  const ObjectId id = b.crow(pt(16, 16));
  WorldObject* slot = b.world.find(id);
  REQUIRE(slot != nullptr);

  // On the ground: the terrain under it, whatever the stored altitudes say.
  slot->state.z_from = 500;
  slot->state.z_to = 900;
  CHECK(flying_z(b.world, *slot) == 8);

  // In the air and not animating: where the last animation left it.
  slot->state.flags.in_air = true;
  CHECK(flying_z(b.world, *slot) == 500);

  // Mid-animation: the cycle is 300 ms and one turn is 100, so a 300-unit
  // climb rises by exactly 100 a turn.
  slot->state.z_from = 100;
  slot->state.z_to = 400;
  CHECK(b.world.play_anim(id, 1, AnimRepeat::hold));
  b.world.advance_turns(1);
  slot = b.world.find(id);
  REQUIRE(slot != nullptr);
  CHECK(slot->animating);
  CHECK(flying_z(b.world, *slot) == 200);
  b.world.advance_turns(1);
  CHECK(flying_z(b.world, *b.world.find(id)) == 300);

  // And at the end of the cycle it is `z_to` **and stays there**, which is the
  // boundary the whole chain runs on: the suspended script resumes at exactly
  // this instant and the altitude it reads becomes the next `z_from`.
  b.world.advance_turns(1);
  CHECK(!b.world.find(id)->animating);
  CHECK(flying_z(b.world, *b.world.find(id)) == 400);
  b.world.advance_turns(5);
  CHECK(flying_z(b.world, *b.world.find(id)) == 400);
}

/// `IsLanding` is both bits and `IsInAir` is one, which is why a bird on the
/// ground that once decided to land is not landing.
TEST(flying_is_landing_is_both_bits) {
  FlightBench b;
  const ObjectId id = b.crow(pt(16, 16));
  WorldObject* slot = b.world.find(id);
  REQUIRE(slot != nullptr);

  const auto landing = [&] {
    return b.call(script::CallKind::member, "IsLanding", 0, {FlightBench::obj(id)})
        .value.as_integer() != 0;
  };
  CHECK(!landing());
  slot->state.flags.landing = true;
  CHECK(!landing());
  slot->state.flags.in_air = true;
  CHECK(landing());
  slot->state.flags.landing = false;
  CHECK(!landing());
}

// --------------------------------------------------------------------------
// the pickers
// --------------------------------------------------------------------------

/// With no flock and an empty sky, the scan accepts its first candidate: every
/// score is the turn angle alone, and due east of a bird already facing east is
/// a turn of zero, which is under sixty.
TEST(flying_pick_flying_point_takes_the_first_good_candidate) {
  FlightBench b;
  const ObjectId id = b.crow(pt(100, 100));
  b.face(id, pt(200, 100));  // due east

  const Point answer = unpack_point(
      b.call(script::CallKind::member, "PickFlyingPoint", 1,
             {FlightBench::obj(id), script::Value::integer(120)})
          .value);
  // The first candidate is 30 degrees, not 0: the loop tests `score < best`
  // against 10,000, accepts, and only then asks whether it was good enough.
  // Either way the answer is the bird's own position plus a 120-unit step and a
  // jitter of at most 24 on each axis.
  const std::int64_t dx = answer.x - 100;
  const std::int64_t dy = answer.y - 100;
  const std::int64_t distance = isqrt(dx * dx + dy * dy);
  CHECK(distance >= 120 - 34);
  CHECK(distance <= 120 + 34);
}

/// **The flock filter is four conditions**, and each of the four has a decoy
/// here that a version missing it would follow instead.
///
/// The mate the original wants is of the receiver's own class, inside
/// `sight - 100`, **animating**, and **in the air** -- a bird whose heading is
/// live because it is mid-flight. `IsInState` is the *absence* of an animation
/// (`sim/anim.cpp`), so the original's `if (IsInState(mate)) continue;` at
/// 0x0051c537 selects a bird that is animating, which is the opposite of what
/// the name suggests and is the reading this page had backwards once.
///
/// Every decoy is spawned **before** the real mate and faces due south, and the
/// real mate faces due north, so any single condition dropped answers
/// `(0, -120)` where the intact filter answers `(0, +120)` -- and the scan,
/// which is what an empty flock falls through to, answers neither.
TEST(flying_pick_flying_point_follows_only_a_live_flock_mate) {
  FlightBench b;
  const ObjectId id = b.crow(pt(100, 100));
  b.face(id, pt(824, 824));  // 45 degrees round from east; the scan would go east

  const auto south = [&](ObjectId who) { b.face(who, pt(100, -900)); };

  // Wrong class: animating and airborne, but a fish is not a crow.
  const ObjectId fish = b.spawn(pt(150, 100), b.fish_class);
  b.world.find(fish)->state.flags.in_air = true;
  CHECK(b.world.play_anim(fish, 1, AnimRepeat::hold));
  south(fish);

  // Right class, animating and airborne, but 850 units away against a flock
  // radius of `sight - 100`, which is 800.
  const ObjectId far_away = b.crow(pt(950, 100));
  b.world.find(far_away)->state.flags.in_air = true;
  CHECK(b.world.play_anim(far_away, 1, AnimRepeat::hold));
  south(far_away);

  // Airborne but settled: its heading is stale, which is the whole point.
  const ObjectId settled = b.crow(pt(160, 100));
  b.world.find(settled)->state.flags.in_air = true;
  south(settled);

  // Animating but on the ground.
  const ObjectId walking = b.crow(pt(170, 100));
  CHECK(b.world.play_anim(walking, 1, AnimRepeat::hold));
  south(walking);

  // And the one that qualifies, facing due north.
  const ObjectId mate = b.crow(pt(180, 100));
  b.world.find(mate)->state.flags.in_air = true;
  CHECK(b.world.play_anim(mate, 1, AnimRepeat::hold));
  b.face(mate, pt(180, 900));

  const Point answer = unpack_point(
      b.call(script::CallKind::member, "PickFlyingPoint", 1,
             {FlightBench::obj(id), script::Value::integer(120)})
          .value);
  // Due north of the follower, within the +/-24 jitter.
  CHECK(answer.x >= 100 - 24);
  CHECK(answer.x <= 100 + 24);
  CHECK(answer.y >= 100 + 120 - 24);
  CHECK(answer.y <= 100 + 120 + 24);
}

/// A mate with **no heading at all** is what separates `point::SetLen` from
/// `GetVecByDir`, and the pickers end in the first.
///
/// 0x00697d81 writes the requested length into *y* and zero into *x*, so a
/// direction-less vector is given one, pointing north; `GetVecByDir`
/// (0x0051b6de) writes the square root -- zero -- into both. The two agree on
/// every other input, so this is the only case that can tell them apart.
TEST(flying_pick_flying_point_ends_in_set_len_not_get_vec_by_dir) {
  FlightBench b;
  const ObjectId id = b.crow(pt(100, 100));
  const ObjectId mate = b.crow(pt(150, 100));
  b.world.find(mate)->state.flags.in_air = true;
  CHECK(b.world.play_anim(mate, 1, AnimRepeat::hold));
  b.movement.state(mate).facing = pt(0, 0);

  const Point answer = unpack_point(
      b.call(script::CallKind::member, "PickFlyingPoint", 1,
             {FlightBench::obj(id), script::Value::integer(120)})
          .value);
  // `SetLen` turns the zero vector into due north at the requested length.
  CHECK(answer.y >= 100 + 120 - 24);
  CHECK(answer.y <= 100 + 120 + 24);
}

// --------------------------------------------------------------------------
// the scan, and which candidate it settles on
// --------------------------------------------------------------------------

/// **First good enough, not best of twelve.** The scan takes the first
/// candidate scoring under 60 and stops looking (0x0051c5a9), which is not the
/// same answer as the minimum.
///
/// A bird at 45 degrees scores candidate 0 at 45 -- under the threshold,
/// so the scan stops there -- while candidate 1 would have scored 15. A version
/// that looked at all twelve would fly 30 degrees further round.
TEST(flying_scan_takes_the_first_good_candidate_not_the_best) {
  FlightBench b;
  const ObjectId id = b.crow(pt(100, 100));
  b.face(id, pt(824, 824));
  CHECK(angle_of_dir(b.movement.state(id).facing) == 45);

  const Point answer = unpack_point(
      b.call(script::CallKind::member, "PickFlyingPoint", 1,
             {FlightBench::obj(id), script::Value::integer(120)})
          .value);
  // Candidate 0 is `(120, 0)`; candidate 1 is `(103, 59)`, and the jitter is
  // at most 24, so the two do not overlap in y.
  CHECK(answer.y >= 100 - 24);
  CHECK(answer.y <= 100 + 24);
  CHECK(answer.x >= 100 + 120 - 24);
}

/// **The turn penalty is folded into `[0, 180]`**, so a turn the short way
/// round scores as the short way round.
///
/// A bird at 10 degrees reads candidate 0, at 0 degrees, as `0 - 10`, which is
/// 350 once lifted into `[0, 360)`. Folded, that is 10 and the scan accepts it
/// at once; unfolded it is 350, and candidate 1 -- at 29, nineteen away -- is
/// the first under 60, thirty degrees round from where the bird was pointing.
TEST(flying_scan_folds_the_turn_penalty) {
  FlightBench b;
  const ObjectId id = b.crow(pt(100, 100));
  b.face(id, pt(100 + 1008, 100 + 179));
  CHECK(angle_of_dir(b.movement.state(id).facing) == 10);

  const Point answer = unpack_point(
      b.call(script::CallKind::member, "PickFlyingPoint", 1,
             {FlightBench::obj(id), script::Value::integer(120)})
          .value);
  // Candidate 0 is `(120, 0)`; unfolded, candidate 1 at `(103, 59)` wins.
  CHECK(answer.x >= 100 + 120 - 24);
  CHECK(answer.y >= 100 - 24);
  CHECK(answer.y <= 100 + 24);
}

/// **The crowding term counts airborne creatures and nothing else.** What makes
/// a spot bad is other birds already flying there, not the town underneath it.
///
/// The original passes flag mask `0x400000` -- the `in_air` bit -- to its
/// collector at 0x0051c380. One grounded object parked on candidate 0 must
/// therefore change nothing; counted, it would add 50, push candidate 0 over
/// the threshold, and send the bird to candidate 2.
TEST(flying_scan_crowding_counts_only_airborne_neighbours) {
  FlightBench b;
  const ObjectId id = b.crow(pt(100, 100));
  b.face(id, pt(824, 824));

  // Parked exactly on candidate 0, and on the ground. A fish so that it is not
  // in the flock -- this test is about the crowding term, not the filter.
  const ObjectId parked = b.spawn(pt(220, 100), b.fish_class);
  CHECK(!b.world.find(parked)->state.flags.in_air);

  const Point answer = unpack_point(
      b.call(script::CallKind::member, "PickFlyingPoint", 1,
             {FlightBench::obj(id), script::Value::integer(120)})
          .value);
  CHECK(answer.x >= 100 + 120 - 24);
  CHECK(answer.y >= 100 - 24);
  CHECK(answer.y <= 100 + 24);
}

/// A crow entirely alone in the world lands straight ahead/// A crow entirely alone in the world lands straight ahead: there is no mate to
/// try, no mate in front of it, and therefore no give-up roll at all.
TEST(flying_pick_landing_point_lands_ahead_when_alone) {
  FlightBench b;
  const ObjectId id = b.crow(pt(100, 100));
  b.world.find(id)->state.flags.in_air = true;  // it only asks while flying
  b.face(id, pt(100, 200));  // due north

  const Point answer = unpack_point(
      b.call(script::CallKind::member, "PickLandingPoint", 1,
             {FlightBench::obj(id), script::Value::integer(120)})
          .value);
  CHECK(answer.x != -1);
  CHECK(answer.y >= 100 + 120 - 24);
  CHECK(answer.y <= 100 + 120 + 24);
}

/// A receiver that no longer resolves answers **(0, 0)**, which the crow's own
/// `ptLand.x >= 0` guard accepts as a place to land.
TEST(flying_pick_landing_point_answers_zero_for_a_dead_receiver) {
  FlightBench b;
  const ObjectId id = b.crow(pt(100, 100));
  CHECK(b.world.despawn(id));
  CHECK(unpack_point(b.call(script::CallKind::member, "PickLandingPoint", 1,
                            {FlightBench::obj(id), script::Value::integer(120)})
                         .value) == pt(0, 0));
  CHECK(unpack_point(b.call(script::CallKind::member, "PickFlyingPoint", 1,
                            {FlightBench::obj(id), script::Value::integer(120)})
                         .value) == pt(0, 0));
}

/// A water unit may only land on water and a land unit only off it, which is
/// the same class property `IsWaterUnit` reads and the same terrain byte
/// `IsPointInWater` tests.
///
/// The landing test is not reachable from a host entry point on its own, so it
/// is driven through `PickLandingPoint`: a fish beside a settled fish, over
/// land, can never find a spot, and the give-up branch is the only answer left.
TEST(flying_landing_requires_water_for_a_water_unit) {
  FlightBench b;
  // The one water cell is 64 units wide at the origin; the fish sit far from it.
  const ObjectId self = b.spawn(pt(200, 200), b.fish_class);
  b.world.find(self)->state.flags.in_air = true;
  b.face(self, pt(200, 300));
  const ObjectId mate = b.spawn(pt(200, 300), b.fish_class);
  (void)mate;

  // Every candidate around the mate is dry land, so the five tries all fail and
  // the give-up roll is the only thing left. Five sixths of the time that is
  // (-1, -1); the remaining sixth lands straight ahead. Either answer proves
  // the candidates were refused -- what must never happen is a spot beside the
  // mate.
  const Point answer = unpack_point(
      b.call(script::CallKind::member, "PickLandingPoint", 1,
             {FlightBench::obj(self), script::Value::integer(120)})
          .value);
  const bool gave_up = answer == pt(-1, -1);
  const bool straight = answer.y > 200;
  CHECK(gave_up || straight);
}

// --------------------------------------------------------------------------
// the landing rule, tested where it lives
// --------------------------------------------------------------------------

/// **A landing spot must be water if and only if the class is a water unit**,
/// which is the same class property `IsWaterUnit` reads and the same terrain
/// byte `IsPointInWater` tests.
///
/// Driving this through `PickLandingPoint` cannot work: the picker reaches it
/// through five jittered candidates and a give-up roll, so the test would be
/// asserting about the generator. Every fault injected into the landing rule
/// survived a picker-level test and none survives this one.
TEST(flying_can_land_matches_water_against_the_class) {
  FlightBench b;
  const ObjectId crow = b.crow(pt(600, 600));
  const ObjectId fish = b.spawn(pt(600, 600), b.fish_class);
  const WorldObject* land_unit = b.world.find(crow);
  const WorldObject* water_unit = b.world.find(fish);
  REQUIRE(land_unit != nullptr);
  REQUIRE(water_unit != nullptr);

  // The layer is water in cell (0, 0) only, which is 64 units wide -- and the
  // half-cell bias means the water cell is what a point within 32 units of it
  // rounds to, so `(20, 20)` is over water and `(200, 200)` is not.
  CHECK(!can_land(b.world, *land_unit, pt(20, 20)));
  CHECK(can_land(b.world, *water_unit, pt(20, 20)));
  CHECK(can_land(b.world, *land_unit, pt(200, 200)));
  CHECK(!can_land(b.world, *water_unit, pt(200, 200)));
}

/// The half-cell bias is the original's and it moves the boundary by half a
/// cell, so a point *between* two cells lands on the nearer one.
///
/// Cell 0 of the 64-unit terrain layer is water and cell 1 is not. Without the
/// bias the boundary is at 64; with it, at 32.
TEST(flying_can_land_biases_by_half_a_terrain_cell) {
  FlightBench b;
  const ObjectId fish = b.spawn(pt(600, 600), b.fish_class);
  const WorldObject* water_unit = b.world.find(fish);
  REQUIRE(water_unit != nullptr);
  CHECK(can_land(b.world, *water_unit, pt(31, 0)));
  CHECK(!can_land(b.world, *water_unit, pt(32, 0)));
}

/// Off the map is not a place to land, on either test.
TEST(flying_can_land_refuses_outside_the_map) {
  FlightBench b;
  const ObjectId crow = b.crow(pt(200, 200));
  const WorldObject* slot = b.world.find(crow);
  REQUIRE(slot != nullptr);
  CHECK(can_land(b.world, *slot, pt(200, 200)));
  CHECK(!can_land(b.world, *slot, pt(-1, 200)));
  CHECK(!can_land(b.world, *slot, pt(200, -1)));
  CHECK(!can_land(b.world, *slot, pt(kMapSize, 200)));
  // And the *biased* test bites first: 224 is on the map and 224 + 32 is not.
  CHECK(!can_land(b.world, *slot, pt(kMapSize - 1, 200)));
}

/// An obstructed cell is not a place to land either.
TEST(flying_can_land_refuses_an_obstructed_cell) {
  FlightBench b;
  const ObjectId crow = b.crow(pt(200, 200));
  const WorldObject* slot = b.world.find(crow);
  REQUIRE(slot != nullptr);
  CHECK(can_land(b.world, *slot, pt(160, 160)));
  b.movement.mutable_grid().set_cell(10, 10, true);  // 16 units per cell
  CHECK(!can_land(b.world, *slot, pt(160, 160)));
}

/// `heading_ok` is a distance window **and** a cosine window, and both halves
/// are needed: a spot straight ahead but too close is refused, and one at the
/// right distance but off to the side is too.
///
/// The window is 50% to 200% of the reference and a cosine over 57/100, which
/// is about 55.2 degrees. Both divides truncate, so the boundaries are the
/// original's exactly.
TEST(flying_heading_ok_is_a_distance_window_and_a_cone) {
  const Point from{1000, 1000};
  const Point north{0, 1024};

  // Straight ahead, inside the window.
  CHECK(heading_ok(from, north, pt(1000, 1100), 100));
  // Straight ahead, too close: 40 units against a 100-unit reference is 40%.
  CHECK(!heading_ok(from, north, pt(1000, 1040), 100));
  // Straight ahead, too far: 250% of the reference.
  CHECK(!heading_ok(from, north, pt(1000, 1250), 100));
  // The two edges of the window, which truncation puts exactly here.
  CHECK(heading_ok(from, north, pt(1000, 1050), 100));
  CHECK(heading_ok(from, north, pt(1000, 1200), 100));
  CHECK(!heading_ok(from, north, pt(1000, 1201), 100));

  // Inside the window but behind the bird.
  CHECK(!heading_ok(from, north, pt(1000, 900), 100));
  // Ninety degrees off: the cosine is zero, which is under 57/100.
  CHECK(!heading_ok(from, north, pt(1100, 1000), 100));
  // Forty-five degrees off: cos is 70/100, which is over.
  CHECK(heading_ok(from, north, pt(1071, 1071), 100));
  // Sixty degrees off: cos is 50/100, which is under.
  CHECK(!heading_ok(from, north, pt(1087, 1050), 100));

  // A bird with no heading at all divides by a forced 1 rather than by zero
  // (0x0051b674), and the dot product against the zero vector is zero -- so the
  // answer is a clean *no* rather than a crash. Worth pinning, because the
  // obvious `if (denominator == 0) return true;` reads just as plausible and is
  // the opposite answer.
  CHECK(!heading_ok(from, pt(0, 0), pt(1000, 1100), 100));
}

/// **The pickers draw an exact number of times, in an exact order, with exact
/// bounds** -- and that is a stronger statement than any assertion about where
/// the bird ends up.
///
/// One generator serves the whole simulation, so a picker that draws once too
/// often, or draws its X before its Y, changes every number every other system
/// sees for the rest of the session. Comparing the generator's own state
/// against a reference advanced by the expected sequence pins the count, the
/// order and the bounds together; comparing coordinates pins none of them.
///
/// `PickLandingPoint` against a mate it can never land beside is five attempts
/// of two draws, then the give-up roll: eleven draws, and the roll decides
/// whether two more follow.
TEST(flying_pick_landing_point_draws_exactly_what_it_says) {
  FlightBench b;
  constexpr std::uint32_t kSeed = 0x2a;
  const ObjectId id = b.crow(pt(1000, 1000));
  b.world.find(id)->state.flags.in_air = true;
  b.face(id, pt(1000, 1900));  // due north

  // 500 units ahead: every candidate around it is four to six times the
  // hundred-unit reference away, so `heading_ok` refuses all five.
  const ObjectId mate = b.crow(pt(1000, 1500));
  CHECK(!b.world.find(mate)->state.flags.in_air);

  b.world.rng().seed(kSeed);
  const Point answer = unpack_point(
      b.call(script::CallKind::member, "PickLandingPoint", 1,
             {FlightBench::obj(id), script::Value::integer(120)})
          .value);

  Rng reference;
  reference.seed(kSeed);
  for (int attempt = 0; attempt < 5; ++attempt) {
    (void)reference.between(-100, 100);  // Y first
    (void)reference.between(-100, 100);
  }
  const std::int32_t roll = reference.between(0, 5);
  if (roll != 0) {
    // Five sixths: give up, and draw nothing more.
    CHECK(answer == pt(-1, -1));
  } else {
    (void)reference.between(-24, 24);
    (void)reference.between(-24, 24);
  }
  CHECK(b.world.rng().state() == reference.state());
}

/// The same, on the branch with no mate to try at all: two draws and no roll.
///
/// The roll only happens when there was a mate to try **or** a mate in front,
/// so a bird entirely alone lands ahead without rolling -- which is a branch,
/// not a probability, and shows up here as the generator being two draws on
/// rather than three.
TEST(flying_pick_landing_point_alone_draws_only_its_jitter) {
  FlightBench b;
  constexpr std::uint32_t kSeed = 0x51c6a0;
  const ObjectId id = b.crow(pt(1000, 1000));
  b.world.find(id)->state.flags.in_air = true;
  b.face(id, pt(1000, 1900));

  b.world.rng().seed(kSeed);
  const Point answer = unpack_point(
      b.call(script::CallKind::member, "PickLandingPoint", 1,
             {FlightBench::obj(id), script::Value::integer(120)})
          .value);

  Rng reference;
  reference.seed(kSeed);
  const std::int32_t dy = reference.between(-24, 24);
  const std::int32_t dx = reference.between(-24, 24);
  CHECK(b.world.rng().state() == reference.state());
  CHECK(answer == pt(1000 + dx, 1000 + 120 + dy));
}

/// `PickFlyingPoint` draws twice, Y first, in `+/-(n/5)`.
TEST(flying_pick_flying_point_draws_y_then_x) {
  FlightBench b;
  constexpr std::uint32_t kSeed = 0x51c3d0;
  const ObjectId id = b.crow(pt(1000, 1000));
  b.face(id, pt(1824, 1824));

  b.world.rng().seed(kSeed);
  const Point answer = unpack_point(
      b.call(script::CallKind::member, "PickFlyingPoint", 1,
             {FlightBench::obj(id), script::Value::integer(120)})
          .value);

  Rng reference;
  reference.seed(kSeed);
  const std::int32_t dy = reference.between(-24, 24);
  const std::int32_t dx = reference.between(-24, 24);
  CHECK(b.world.rng().state() == reference.state());
  // Candidate 0, which is due east of a bird at 45 degrees.
  CHECK(answer == pt(1000 + 120 + dx, 1000 + dy));
}

/// `GetAverageDirection` divides by the count and truncates, and an empty
/// receiver answers the zero vector rather than dividing by nothing.
TEST(flying_get_average_direction_is_the_mean_heading) {
  FlightBench b;
  const ObjectId a = b.crow(pt(100, 100));
  const ObjectId c = b.crow(pt(120, 100));
  b.face(a, pt(200, 100));  // due east
  b.face(c, pt(120, 200));  // due north

  const Point mean = unpack_point(
      b.call(script::CallKind::member, "GetAverageDirection", 0,
             {script::Value::object(kTypeObjList, 0)})
          .value);
  // An `ObjList` handle that names nothing is an empty receiver.
  CHECK(mean == pt(0, 0));

  // A query over both birds: the mean of (1024, 0) and (0, 1024).
  const QuerySpec spec = objs_in_range(a, 900, ClassFilter::of(b.crow_class));
  const ObjectId query = b.world.create_query(spec);
  const Point both = unpack_point(
      b.call(script::CallKind::member, "GetAverageDirection", 0,
             {script::Value::object(kTypeQuery, query)})
          .value);
  CHECK(both == pt(kFacingLength / 2, kFacingLength / 2));
}

// --------------------------------------------------------------------------
// the small free functions
// --------------------------------------------------------------------------

/// `RandomOffset(n)` draws **Y first**, which is the whole of what a test of it
/// can check: one generator serves the simulation and swapping two consecutive
/// draws changes every number after them.
TEST(flying_random_offset_draws_y_before_x) {
  FlightBench b;
  const std::uint32_t seed = 0x2a;
  b.world.rng().seed(seed);
  const Point drawn = unpack_point(
      b.call(script::CallKind::free_function, "RandomOffset", 1, {script::Value::integer(200)})
          .value);

  Rng reference;
  reference.seed(seed);
  const std::int32_t y = reference.between(-200, 200);
  const std::int32_t x = reference.between(-200, 200);
  CHECK(drawn == pt(x, y));
  // And the other order really is a different answer, so the check discriminates.
  CHECK(drawn != pt(y, x));
}

/// `MIN` and `MAX` over integers, including the negative case a `<` on unsigned
/// would get wrong.
TEST(flying_min_and_max_are_signed) {
  FlightBench b;
  const auto min = [&](std::int32_t a, std::int32_t c) {
    return b.call(script::CallKind::free_function, "MIN", 2,
                  {script::Value::integer(a), script::Value::integer(c)})
        .value.as_integer();
  };
  const auto max = [&](std::int32_t a, std::int32_t c) {
    return b.call(script::CallKind::free_function, "MAX", 2,
                  {script::Value::integer(a), script::Value::integer(c)})
        .value.as_integer();
  };
  CHECK(min(3, 7) == 3);
  CHECK(min(7, 3) == 3);
  CHECK(min(-5, 2) == -5);
  CHECK(max(3, 7) == 7);
  CHECK(max(-5, 2) == 2);
  CHECK(max(-5, -9) == -5);
}

/// `Unit::dir` is `GetDir` under another name, and an unresolvable receiver
/// answers the zero vector rather than faulting the way the original does.
TEST(flying_dir_is_the_heading) {
  FlightBench b;
  const ObjectId id = b.crow(pt(100, 100));
  b.face(id, pt(200, 100));
  CHECK(unpack_point(b.call(script::CallKind::member, "dir", 0, {FlightBench::obj(id)}).value) ==
        pt(kFacingLength, 0));
  CHECK(b.world.despawn(id));
  CHECK(unpack_point(b.call(script::CallKind::member, "dir", 0, {FlightBench::obj(id)}).value) ==
        pt(0, 0));
}

/// **An eagle flies on its class's `speed`, and it has never walked.**
///
/// `EAGLE_MOVE.VS` steps `GetVecByDir(dir, .speed / 2)` while it turns and
/// `.speed` (or that scaled by two animation lengths) when it flies straight,
/// and hands the point to `PlayAnim(anim, newPos, newZ)`. `Unit::speed`
/// (`0x005d8ad0`) is the class descriptor's `+0x2c8`, which the class reader
/// fills from `speed` -- not the movement record, which an eagle never gets
/// because nothing ever orders it to walk. Answered from that record, `.speed`
/// was 0 for every eagle, every step was `(0, 0)`, and all fifteen on Balcans
/// sat on their start points for the whole match (playtest #18).
///
/// The bench's idle state loops the flight animation, which is the shape the
/// owner suspected: `IsInState` goes false for the one animation `PlayAnim`
/// starts and comes back true when it has played, because `PlayAnim` holds and
/// does not loop -- and the bird is at `newPos`.
TEST(flying_an_eagle_steps_its_class_speed_and_arrives) {
  FlightBench b;
  const ObjectId id = b.spawn(pt(32, 128), b.eagle_class, NativeClass::flying_unit);
  const auto obj = FlightBench::obj(id);
  const auto number = [&](const char* name, std::vector<script::Value> args) {
    // The arity is taken before the call: a call's arguments are evaluated in
    // no fixed order, and GCC moves `args` away before it is measured.
    const auto arity = static_cast<std::uint16_t>(args.size() - 1);
    return b.call(script::CallKind::member, name, arity, std::move(args)).value.as_integer();
  };

  // Never walked, and `.speed` is still the class's -- not the parent's 50.
  CHECK(b.movement.find(id) == nullptr);
  CHECK(number("speed", {obj}) == 180);
  CHECK(number("IsInState", {obj}) != 0);

  // One straight step of the script, with its own arithmetic.
  b.face(id, pt(1000, 128));
  const Point dir = unpack_point(b.call(script::CallKind::member, "dir", 0, {obj}).value);
  const Point vec = unpack_point(b.call(script::CallKind::free_function, "GetVecByDir", 2,
                                        {b.point(dir), script::Value::integer(180)})
                                     .value);
  CHECK(vec == pt(180, 0));
  const Point new_pos = pt(32 + vec.x, 128 + vec.y);
  const script::HostOutcome flew =
      b.call(script::CallKind::member, "PlayAnim", 3,
             {obj, script::Value::integer(0), b.point(new_pos), script::Value::integer(200)});
  CHECK(flew.status == script::HostStatus::suspend);
  CHECK(flew.suspend_for > 0);
  CHECK(b.world.resolve_position(id) == new_pos);
  CHECK(number("IsInState", {obj}) == 0);

  // The animation plays once and ends, loop-shaped state or not.
  b.world.advance_turns(3);
  CHECK(number("IsInState", {obj}) != 0);
  CHECK(b.world.resolve_position(id) == new_pos);

  // `SetSpeedFactor` makes a movement record; `.speed` still reads the class,
  // because `0x005d8ad0` applies no factor.
  b.call(script::CallKind::member, "SetSpeedFactor", 1, {obj, script::Value::integer(150)});
  CHECK(b.movement.find(id) != nullptr);
  CHECK(number("speed", {obj}) == 180);

  // A class that does not declare one answers its parent's, as the descriptor
  // copies it.
  const ObjectId crow = b.crow(pt(64, 64));
  CHECK(number("speed", {FlightBench::obj(crow)}) == 50);
}
