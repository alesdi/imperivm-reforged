// The class point round trip, and the doors it builds.
//
// What is under test here is `sim/entrance.hpp`'s *composition* -- project,
// offset in screen space, unproject, then reject by map, passability and ground
// -- and not the projection itself, which `test_flying.cpp` measures against its
// own one-screen-unit bound. So the expected world point is computed by calling
// `world_to_screen` and `screen_to_world` directly: this file asserts that the
// two are put together in that order and with the offset in the middle, which is
// exactly the part `entrance.cpp` owns.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/entrance.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/lsa.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/projection.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "builder.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

constexpr std::uint32_t kExtent = 1024;      // a 1024-unit square map
constexpr std::uint32_t kTerrainCell = 64;   // 16 cells across
constexpr std::uint32_t kPassCell = 16;      // 64 cells across
constexpr std::uint32_t kHeightCell = 32;    // 32 cells across

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Dead flat ground, so the height term vanishes and the round trip is the
/// scale alone. Terrain relief is `test_flying.cpp`'s subject, not this one's.
std::vector<std::byte> flat_height() {
  imperivm::test::Builder out;
  const std::uint32_t cells = kExtent / kHeightCell;
  out.text(kGridMagic).u32(kHeightCell).u32(8).u32(kExtent).u32(kExtent);
  for (std::uint32_t i = 0; i < cells * cells; ++i) out.u8(0);
  return {out.span().begin(), out.span().end()};
}

/// Whether a 64-unit terrain cell column is deep water: the western four and
/// the eastern four. **Two seas rather than one**, and they are disjoint, which
/// is what lets a test tell "the first water door" from "the last".
[[nodiscard]] bool wet_column(std::int32_t cx) {
  const auto cells = static_cast<std::int32_t>(kExtent / kTerrainCell);
  return cx >= 0 && cx < cells && (cx < 4 || cx >= cells - 4);
}

/// Land everywhere except those two, which are terrain index 13.
std::vector<std::byte> terrain_with_two_seas() {
  imperivm::test::Builder out;
  const std::uint32_t cells = kExtent / kTerrainCell;
  out.text(kGridMagic).u32(kTerrainCell).u32(8).u32(kExtent).u32(kExtent);
  for (std::uint32_t y = 0; y < cells; ++y) {
    for (std::uint32_t x = 0; x < cells; ++x) {
      out.u8(wet_column(static_cast<std::int32_t>(x)) ? 13 : 1);
    }
  }
  return {out.span().begin(), out.span().end()};
}

/// The coastline, walled the way every shipped map walls it: a 16-unit cell is
/// blocked when one of its four neighbours disagrees about being deep water.
///
/// Without this the partition's water pass floods the whole map from its first
/// seed, because the fill tests passability and never terrain -- which is what
/// `sim/lsa.hpp` records, and why the sea being a place of its own is a fact
/// about the maps rather than about the algorithm.
std::vector<std::byte> pass_walling_the_shore() {
  imperivm::test::Builder out;
  const std::uint32_t cells = kExtent / kPassCell;
  const auto wet = &wet_column;
  out.text(kGridMagic).u32(kPassCell).u32(1).u32(kExtent).u32(kExtent);
  const std::uint32_t stride = (cells + 7) / 8;
  for (std::uint32_t y = 0; y < cells; ++y) {
    for (std::uint32_t byte = 0; byte < stride; ++byte) {
      std::uint32_t value = 0;
      for (std::uint32_t bit = 0; bit < 8; ++bit) {
        const auto x = static_cast<std::int32_t>(byte * 8 + bit);
        if (x >= static_cast<std::int32_t>(cells)) continue;
        // Both seas are whole terrain-cell columns, so every coastline is
        // vertical and one 16-unit cell either side of it is blocked.
        const std::int32_t tx = x / 4;
        if (wet(tx) != wet(tx - 1) || wet(tx) != wet(tx + 1)) value |= 1u << bit;
      }
      out.u8(value);
    }
  }
  return {out.span().begin(), out.span().end()};
}

/// Nothing blocked, unless `wall` names a 16-unit cell column to block.
std::vector<std::byte> pass_layer(std::int32_t wall = -1) {
  imperivm::test::Builder out;
  const std::uint32_t cells = kExtent / kPassCell;
  out.text(kGridMagic).u32(kPassCell).u32(1).u32(kExtent).u32(kExtent);
  const std::uint32_t stride = (cells + 7) / 8;
  for (std::uint32_t y = 0; y < cells; ++y) {
    for (std::uint32_t byte = 0; byte < stride; ++byte) {
      std::uint32_t value = 0;
      for (std::uint32_t bit = 0; bit < 8; ++bit) {
        if (wall >= 0 && static_cast<std::int32_t>(byte * 8 + bit) == wall) value |= 1u << bit;
      }
      out.u8(value);
    }
  }
  return {out.span().begin(), out.span().end()};
}

ObstructionGrid obstruction(const std::vector<std::byte>& bytes) {
  const Result<Grid> parsed = Grid::parse(bytes);
  CHECK(parsed.ok());
  if (!parsed.ok()) return ObstructionGrid{};
  Result<ObstructionGrid> grid = ObstructionGrid::from_grid(parsed.value());
  CHECK(grid.ok());
  return grid.ok() ? std::move(grid.value()) : ObstructionGrid{};
}

/// A gatehouse with two land doors and one that lands in the western sea.
///
/// The offsets are **screen** pixels, which is what `<point x= y=>` authors, and
/// the `y` of every one of them is zero so that the horizontal spread is the
/// only thing separating the doors. Type 2 is an *exit* point and is here to
/// prove the type tag is read: nothing that asks for enter points may return it.
constexpr std::string_view kGateEntity =
    "<entity name=\"gate\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points>"
    "<point idx=\"0\" type=\"1\" x=\"-128\" y=\"0\"/>"
    "<point idx=\"1\" type=\"1\" x=\"128\" y=\"0\"/>"
    "<point idx=\"2\" type=\"1\" x=\"-320\" y=\"0\"/>"
    "<point idx=\"3\" type=\"2\" x=\"0\" y=\"64\"/>"
    "<point idx=\"4\" type=\"12\" x=\"64\" y=\"0\"/>"
    "</points>"
    "</entity>";

/// A world with the three layers, a movement system holding the passability
/// grid, and one gatehouse at the middle of the map.
struct Bench {
  ClassGraph graph;
  World world;
  MovementSystem movement;
  // The economy is here rather than in a derived bench because nothing in this
  // file serialises a world, which is the reason a system usually has to be
  // kept out of a shared fixture.
  EconomySystem economy;
  // **Held, not passed as temporaries.** A `Grid` is a view over the bytes it
  // was parsed from, so a layer built out of a vector that died at the end of
  // the statement reads as all zeroes -- which looks exactly like a map with no
  // water on it, and is why these are members.
  std::vector<std::byte> height_bytes = flat_height();
  std::vector<std::byte> terrain_bytes = terrain_with_two_seas();
  std::vector<std::byte> pass_bytes;
  Result<Entity> gate_entity = Entity::parse(bytes_of(kGateEntity));
  ObjectId gate = kNoObject;

  /// `wall` blocks one 16-unit column; `shore` walls the coastline instead, so
  /// that the partition can tell the sea from the land.
  explicit Bench(std::int32_t wall = -1, bool shore = false) {
    const Result<Grid> height = Grid::parse(height_bytes);
    REQUIRE(height.ok());
    world.set_height(height.value());
    const Result<Grid> terrain = Grid::parse(terrain_bytes);
    REQUIRE(terrain.ok());
    world.set_terrain(terrain.value());
    REQUIRE(world.add_system(&movement));
    REQUIRE(world.add_system(&economy));
    economy.start(world);
    pass_bytes = shore ? pass_walling_the_shore() : pass_layer(wall);
    movement.set_grid(obstruction(pass_bytes));
    world.mutable_lsa().build(world.terrain(), movement.grid());

    graph.add(bytes_of(R"(<class id="BaseShipyard" cpp_class="CVXBuilding" parent=""/>)"),
              "baseshipyard.sc.xml");
    graph.add(bytes_of(R"(<class id="RShipyard" cpp_class="CVXBuilding" parent="BaseShipyard"/>)"),
              "rshipyard.sc.xml");
    graph.add(bytes_of(R"(<class id="Hut" cpp_class="CVXBuilding" parent=""/>)"), "hut.sc.xml");
    // The exit-vector family: a class that declares the pair, one that inherits
    // it, one that overrides it, and one that declares only half.
    graph.add(bytes_of(R"(<class id="Dock" cpp_class="CVXBuilding" parent="">
      <properties exit_vector_x="-120" exit_vector_y="80"/></class>)"),
              "dock.sc.xml");
    graph.add(bytes_of(R"(<class id="DockHeir" cpp_class="CVXBuilding" parent="Dock"/>)"),
              "dockheir.sc.xml");
    graph.add(bytes_of(R"(<class id="DockOwn" cpp_class="CVXBuilding" parent="Dock">
      <properties exit_vector_x="10" exit_vector_y="-20"/></class>)"),
              "dockown.sc.xml");
    graph.add(bytes_of(R"(<class id="DockHalf" cpp_class="CVXBuilding" parent="Dock">
      <properties exit_vector_x="99"/></class>)"),
              "dockhalf.sc.xml");
    graph.link();
    world.set_class_graph(&graph);

    REQUIRE(gate_entity.ok());
    gate = world.spawn(NativeClass::building, &gate_entity.value());
    CHECK(world.set_position(gate, Point{512, 512}));
    CHECK(world.set_health(gate, 1000));
  }

  ObjectId walker(Point where, NativeClass kind = NativeClass::unit) {
    const ObjectId id = world.spawn(kind, nullptr);
    CHECK(world.set_position(id, where));
    CHECK(world.set_health(id, 100));
    return id;
  }

  /// A building of `class_name` at the middle of the map carrying `entity`.
  ObjectId dock(const Entity& entity, const char* class_name) {
    const ObjectId id = world.spawn(NativeClass::building, &entity, graph.find(class_name));
    CHECK(world.set_position(id, Point{512, 512}));
    CHECK(world.set_health(id, 1000));
    return id;
  }

  /// A settlement anchored on `anchor`, with `members` bound to it.
  SettlementId town(ObjectId anchor, std::vector<ObjectId> members) {
    SettlementInit init;
    init.settlement_object = anchor;
    init.anchor = anchor;
    init.owner = 1;
    init.kind = SettlementKind::stronghold;
    const SettlementId id = economy.create(world, init);
    for (const ObjectId member : members) {
      // `create` already binds the anchor, and a second bind of the same
      // object is refused rather than duplicated -- so ask first.
      bool already = false;
      if (const Settlement* set = economy.settlements().find(id); set != nullptr) {
        for (const SettlementBuilding& b : set->buildings) {
          if (b.object == member) already = true;
        }
      }
      if (!already) CHECK(economy.settlements().add_building(id, member, 1000));
    }
    return id;
  }

  script::Value settlement_value(SettlementId id) {
    const Settlement* set = economy.settlements().find(id);
    CHECK(set != nullptr);
    if (set == nullptr) return script::Value::object(script::ObjectRef{});
    return script::Value::object(kTypeSettlement, set->object);
  }

  /// Where the class point at screen offset `(dx, dy)` on the gate must land,
  /// computed from the projection rather than from `entrance.cpp`.
  Point expected(std::int32_t dx, std::int32_t dy) {
    const Point screen = world_to_screen(world, world.resolve_position(gate));
    return screen_to_world(world, Point{screen.x + dx, screen.y + dy});
  }
};

/// One member call through the registry, whatever it is.
script::HostOutcome member_call(Bench& b, const char* name, std::vector<script::Value> args) {
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index = registry.find(script::CallKind::member, name,
                                            static_cast<std::uint16_t>(args.size() - 1));
  CHECK(index != script::kUnresolvedHost);
  if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
  CHECK(registry.entry(index).fn != nullptr);
  if (registry.entry(index).fn == nullptr) return script::HostOutcome::failed("not implemented");
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = name;
  ctx.kind = script::CallKind::member;
  return registry.entry(index).fn(ctx);
}

/// One `building.GetEnterPoint(unit)` through the registry.
Point enter_point(Bench& b, ObjectId building, ObjectId unit) {
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index = registry.find(script::CallKind::member, "GetEnterPoint", 1);
  CHECK(index != script::kUnresolvedHost);
  if (index == script::kUnresolvedHost) return Point{};
  CHECK(registry.entry(index).fn != nullptr);
  if (registry.entry(index).fn == nullptr) return Point{};
  std::vector<script::Value> args{script::Value::object(kTypeObj, building),
                                  script::Value::object(kTypeObj, unit)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "GetEnterPoint";
  ctx.kind = script::CallKind::member;
  const script::HostOutcome out = registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(is_point(out.value));
  return unpack_point(out.value);
}

/// A seed whose **first** `between(0, 10000)` is exactly `want`.
///
/// Searched for rather than written down, so that the three tests that need a
/// particular draw say which draw they need and nothing else -- and so that
/// none of them has to be revisited when `sim/rng.hpp` stops being a
/// placeholder. Zero means the search failed, which the callers `REQUIRE`.
[[nodiscard]] std::uint32_t seed_drawing(std::int32_t want) {
  for (std::uint32_t seed = 1; seed < 1000000; ++seed) {
    Rng probe{seed};
    if (probe.between(0, 10000) == want) return seed;
  }
  return 0;
}

/// One `building.GetExitPoint(a, b)` through the registry, either overload.
Point exit_point(Bench& b, ObjectId building, script::Value first, script::Value second) {
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index = registry.find(script::CallKind::member, "GetExitPoint", 2);
  CHECK(index != script::kUnresolvedHost);
  if (index == script::kUnresolvedHost) return Point{};
  CHECK(registry.entry(index).fn != nullptr);
  if (registry.entry(index).fn == nullptr) return Point{};
  std::vector<script::Value> args{script::Value::object(kTypeObj, building), std::move(first),
                                  std::move(second)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "GetExitPoint";
  ctx.kind = script::CallKind::member;
  const script::HostOutcome out = registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(is_point(out.value));
  return unpack_point(out.value);
}

}  // namespace

/// The offset goes on in **screen** space, between the two halves of the round
/// trip. Applying it in world space instead would put every door in the wrong
/// place by the vertical scale -- about 30% out on the `y` axis -- and would be
/// invisible to any test whose points all had `y = 0`.
TEST(entrance_a_class_point_is_projected_offset_and_unprojected) {
  Bench b;
  // `x` passes straight through both halves, so a purely horizontal offset
  // lands exactly where it says.
  CHECK(class_point_in_world(b.world, b.gate, -128, 0) == b.expected(-128, 0));
  CHECK(class_point_in_world(b.world, b.gate, -128, 0).x == 512 - 128);
  CHECK(class_point_in_world(b.world, b.gate, 128, 0).x == 512 + 128);

  // A vertical offset is the case that separates the two orders: 64 screen
  // units is about 90 world units, not 64.
  const Point south = class_point_in_world(b.world, b.gate, 0, 64);
  CHECK(south == b.expected(0, 64));
  CHECK(south.x == 512);
  CHECK(south.y > 512 + 80);
  CHECK(south.y < 512 + 100);

  // Off the map is the sentinel, and it is a real answer rather than an error.
  CHECK(class_point_in_world(b.world, b.gate, -4096, 0) == kNoClassPoint);
  CHECK(class_point_in_world(b.world, b.gate, 0, 4096) == kNoClassPoint);
  // And so is a handle that names nothing.
  CHECK(class_point_in_world(b.world, static_cast<ObjectId>(9999), 0, 0) == kNoClassPoint);
}

/// **All** the points of a type, in table order -- because the four readers of
/// one typed point draw from the list at random, so "the first" is the wrong
/// shape as well as the wrong point.
TEST(entrance_the_points_of_a_type_come_back_whole_and_in_table_order) {
  Bench b;
  std::vector<Point> enters;
  class_points_of_type(b.world, b.gate, kEnterPointType, enters);
  REQUIRE(enters.size() == 3);
  CHECK(enters[0] == b.expected(-128, 0));
  CHECK(enters[1] == b.expected(128, 0));
  CHECK(enters[2] == b.expected(-320, 0));

  // The type tag is read, so the exit and the gate are each their own list --
  // and passability plays no part here, unlike `building_doors`.
  std::vector<Point> exits;
  class_points_of_type(b.world, b.gate, kExitPointType, exits);
  REQUIRE(exits.size() == 1);
  CHECK(exits[0] == b.expected(0, 64));
  std::vector<Point> gates;
  class_points_of_type(b.world, b.gate, kEnterExitPointType, gates);
  REQUIRE(gates.size() == 1);
  CHECK(gates[0] == b.expected(64, 0));

  // A type the class declares none of, and an object with no class at all,
  // append nothing rather than a sentinel.
  std::vector<Point> none;
  class_points_of_type(b.world, b.gate, kWaterSourcePointType, none);
  class_points_of_type(b.world, b.walker(Point{100, 100}), kEnterPointType, none);
  CHECK(none.empty());

  CHECK(class_has_point_type(b.world, b.gate, kEnterPointType));
  CHECK(class_has_point_type(b.world, b.gate, kEnterExitPointType));
  CHECK(!class_has_point_type(b.world, b.gate, kFixSitePointType));
}

/// The screen rectangle is the projection of the map rectangle with two
/// margins, and it is **tighter than the map check that follows it** -- which is
/// the whole reason both are applied.
TEST(entrance_the_screen_rectangle_is_the_maps_with_a_margin) {
  Bench b;
  const ScreenBounds bounds = screen_bounds(b.world);
  CHECK(bounds.min_x == 0);
  CHECK(bounds.min_y == 0);
  // One height cell in from the map's own high corner.
  CHECK(bounds.max_x == 1023 - 32);
  // And the largest a terrain height can be, plus that cell, off the south
  // edge -- the terms that make the inverse scan safe rather than arbitrary.
  CHECK(bounds.max_y == project_scale(1023) - 255 - 32);

  // A point whose *world* landing would be comfortably on the map and whose
  // *screen* point is past the southern bound is rejected, which is exactly the
  // band the map check alone would let through.
  const Point screen = world_to_screen(b.world, b.world.resolve_position(b.gate));
  const std::int32_t past = bounds.max_y - screen.y + 1;
  const Point landed = screen_to_world(b.world, Point{screen.x, bounds.max_y + 1});
  CHECK(landed.x >= 0);
  CHECK(landed.y >= 0);
  CHECK(landed.x <= 1023);
  CHECK(landed.y <= 1023);  // on the map by the second test...
  CHECK(class_point_in_world(b.world, b.gate, 0, past) == kNoClassPoint);  // ...and out by the first
  CHECK(class_point_in_world(b.world, b.gate, 0, past - 1) != kNoClassPoint);
}

/// `enter_point_near`, the picker `Building::GetEnterPoint` and
/// `Squad::UseTeleport` share.
///
/// The two entry points differ only in where the water flag comes from --
/// `GetEnterPoint` asks whether the unit is a ship, `UseTeleport` passes a
/// literal false -- so what is shared is this, and what it has to get right is
/// the list it picks from and the nearest-wins scan over it.
TEST(entrance_enter_point_near_picks_the_nearest_of_the_chosen_list) {
  Bench b;
  const Point west = b.expected(-128, 0);
  const Point east = b.expected(128, 0);
  const Point sea = b.expected(-320, 0);

  // Nearest wins, and which door is nearest is the only thing the unit decides.
  const ObjectId from_west = b.walker(Point{west.x - 200, west.y});
  const ObjectId from_east = b.walker(Point{east.x + 200, east.y});
  CHECK(enter_point_near(b.world, b.gate, from_west, false) == west);
  CHECK(enter_point_near(b.world, b.gate, from_east, false) == east);

  // **The flag picks the list, not the door.** Asked for water, the same two
  // units both get the one door in the sea -- which is in neither of the
  // answers above.
  CHECK(enter_point_near(b.world, b.gate, from_west, true) == sea);
  CHECK(enter_point_near(b.world, b.gate, from_east, true) == sea);

  // An empty list is the building's own position, which is the fallback
  // `GetEnterPoint` wraps and every caller relies on being a real point.
  const ObjectId hut = b.world.spawn(NativeClass::building, nullptr);
  const Point home{700, 700};
  CHECK(b.world.set_position(hut, home));
  CHECK(enter_point_near(b.world, hut, from_west, false) == home);
}

/// The doors are filed by the ground each one landed on, and a door nobody can
/// stand in is discarded before the split rather than sorted into it.
TEST(entrance_doors_are_split_by_ground_and_filtered_by_passability) {
  Bench b;
  std::vector<Point> land;
  building_doors(b.world, &b.movement.grid(), b.gate, kEnterPointType, false, land);
  REQUIRE(land.size() == 2);
  CHECK(land[0] == b.expected(-128, 0));
  CHECK(land[1] == b.expected(128, 0));

  // The third type-1 point lands in the western sea, so it is the water list's
  // and appears in neither the land list nor the exit list.
  std::vector<Point> sea;
  building_doors(b.world, &b.movement.grid(), b.gate, kEnterPointType, true, sea);
  REQUIRE(sea.size() == 1);
  CHECK(sea[0] == b.expected(-320, 0));
  CHECK(sea[0].x < 256);  // and it really is in the water

  // The type tag is read: the exit point is not an enter point.
  std::vector<Point> exits;
  building_doors(b.world, &b.movement.grid(), b.gate, kExitPointType, false, exits);
  REQUIRE(exits.size() == 1);
  CHECK(exits[0] == b.expected(0, 64));

  // A wall through the eastern door's 16-unit cell takes that door away, and
  // takes it away from *both* lists rather than moving it between them.
  Bench walled{(512 + 128) / static_cast<std::int32_t>(kPassCell)};
  std::vector<Point> left;
  building_doors(walled.world, &walled.movement.grid(), walled.gate, kEnterPointType, false,
                 left);
  REQUIRE(left.size() == 1);
  CHECK(left[0] == walled.expected(-128, 0));

  // With no layer at all the test is skipped rather than failed: an
  // `ObstructionGrid` reads out of bounds as blocked, which would otherwise
  // leave every building in a system-less world doorless.
  std::vector<Point> unchecked;
  building_doors(b.world, nullptr, b.gate, kEnterPointType, false, unchecked);
  CHECK(unchecked.size() == 2);
}

TEST(get_enter_point_answers_the_door_nearest_the_unit) {
  Bench b;
  const ObjectId east = b.walker(Point{900, 512});
  const ObjectId west = b.walker(Point{100, 512});
  CHECK(enter_point(b, b.gate, east) == b.expected(128, 0));
  CHECK(enter_point(b, b.gate, west) == b.expected(-128, 0));
}

/// A water-borne unit reads the *other* list, and the two lists are disjoint --
/// so this cannot pass by accident on a body that ignored the argument.
/// The door list `GetEnterPoint` reads is the **filtered** one: a wall through
/// the nearest door's cell sends the unit round to the other side.
TEST(get_enter_point_will_not_send_a_unit_to_a_blocked_door) {
  Bench walled{(512 + 128) / static_cast<std::int32_t>(kPassCell)};
  const ObjectId east = walled.walker(Point{900, 512});
  // Nearest by distance is the eastern door; it is not standable, so it is not
  // in the list at all and the western one answers.
  CHECK(enter_point(walled, walled.gate, east) == walled.expected(-128, 0));
}

TEST(get_enter_point_sends_a_ship_to_the_water_door) {
  Bench b;
  const ObjectId ship = b.walker(Point{100, 512}, NativeClass::ship);
  CHECK(enter_point(b, b.gate, ship) == b.expected(-320, 0));
  // The same position on foot takes the nearest *land* door instead.
  const ObjectId walker = b.walker(Point{100, 512});
  CHECK(enter_point(b, b.gate, walker) == b.expected(-128, 0));
}

/// **Every fallback is a real point**, which is why not one of the 18 shipped
/// sites tests the result before walking to it.
TEST(get_enter_point_falls_back_to_a_position_and_never_to_a_sentinel) {
  Bench b;
  const ObjectId unit = b.walker(Point{900, 512});
  const ObjectId dead = b.walker(Point{700, 300});
  CHECK(b.world.set_health(dead, 0));

  // A building that names nothing, or is dead, answers the unit's position.
  CHECK(enter_point(b, static_cast<ObjectId>(9999), unit) == (Point{900, 512}));
  CHECK(b.world.set_health(b.gate, 0));
  CHECK(enter_point(b, b.gate, unit) == (Point{900, 512}));
  // With the unit gone too there is nothing left to answer but the origin.
  CHECK(enter_point(b, b.gate, dead) == (Point{0, 0}));
  CHECK(b.world.set_health(b.gate, 1000));

  // A unit that names nothing, or is dead, answers the building's position.
  CHECK(enter_point(b, b.gate, static_cast<ObjectId>(9999)) == (Point{512, 512}));
  CHECK(enter_point(b, b.gate, dead) == (Point{512, 512}));

  // And so does a building with no door of the kind asked for: the walker here
  // is a unit, whose class declares no points at all.
  const ObjectId doorless = b.walker(Point{300, 300});
  CHECK(enter_point(b, doorless, unit) == (Point{300, 300}));
}

/// Four exit doors on one building: two on land, west and east, one out over
/// the deep water, and an *enter* point so that a body reading the wrong list
/// has somewhere wrong to read. The `x` offsets pass through the round trip
/// untouched, so they are world offsets from the building as well as screen
/// ones. This map has an eastern sea as well as a western one, which is why the
/// eastern door is at `192` and not further out: past about `224` it would be a
/// water door too, and the two lists would stop being disjoint.
constexpr std::string_view kFourExitEntity =
    "<entity name=\"quay\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points>"
    "<point idx=\"0\" type=\"2\" x=\"-128\" y=\"0\"/>"
    "<point idx=\"1\" type=\"2\" x=\"192\" y=\"0\"/>"
    "<point idx=\"2\" type=\"2\" x=\"-320\" y=\"0\"/>"
    "<point idx=\"3\" type=\"1\" x=\"0\" y=\"64\"/>"
    "</points>"
    "</entity>";

/// **The weight is the inverse *fourth* power of the distance**, which is the
/// one thing about this picker that cannot be guessed from its name.
///
/// Two doors, one twice as far as the other. A fourth power gives the far one
/// one draw in seventeen; an inverse *square* -- the reading the word "distance"
/// invites -- would give it one in five. The band between those two is what this
/// measures, and it is wide enough that the generator's identity does not
/// matter.
TEST(pick_door_weights_by_the_fourth_power_of_the_distance) {
  const std::vector<Point> doors{Point{100, 0}, Point{200, 0}};
  Rng rng{12345};
  int far = 0;
  for (int i = 0; i < 2000; ++i) {
    if (pick_door_by_distance(doors, Point{0, 0}, rng) == doors[1]) ++far;
  }
  // 1/17 of 2000 is about 118; 1/5 would be 400.
  CHECK(far > 40);
  CHECK(far < 240);
}

/// Equal distances are a uniform draw, and every door is reachable -- including
/// the last, which is also the answer the largest draw falls through to.
TEST(pick_door_among_equal_doors_is_uniform) {
  const std::vector<Point> doors{Point{-100, 0}, Point{0, -100}, Point{100, 0}, Point{0, 100}};
  Rng rng{11};
  int seen[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4000; ++i) {
    const Point answer = pick_door_by_distance(doors, Point{0, 0}, rng);
    for (std::size_t k = 0; k < doors.size(); ++k) {
      if (answer == doors[k]) ++seen[k];
    }
  }
  for (const int count : seen) {
    CHECK(count > 800);
    CHECK(count < 1200);
  }
}

/// **A door standing on the reference point is the `1e-7` case**, and it wins
/// every draw but the two at the ends: the smallest answers the first element
/// whatever its weight, and the largest falls through to the last. Both of those
/// are the original's, not this transcription's.
TEST(pick_door_a_door_on_the_reference_point_wins_all_but_the_extremes) {
  const std::vector<Point> doors{Point{50, 0}, Point{0, 0}, Point{80, 0}};
  Rng rng{9};
  int here = 0;
  for (int i = 0; i < 4000; ++i) {
    if (pick_door_by_distance(doors, Point{0, 0}, rng) == doors[1]) ++here;
  }
  // 9,999 of the 10,001 draws, so about 3,999 of 4,000.
  CHECK(here > 3980);

  // The two that are not.
  const std::uint32_t lowest = seed_drawing(0);
  const std::uint32_t highest = seed_drawing(10000);
  REQUIRE(lowest != 0);
  REQUIRE(highest != 0);
  Rng at_lowest{lowest};
  CHECK(pick_door_by_distance(doors, Point{0, 0}, at_lowest) == doors.front());
  Rng at_highest{highest};
  CHECK(pick_door_by_distance(doors, Point{0, 0}, at_highest) == doors.back());
}

/// **The weight floor is what keeps the smallest draw honest.** The original's
/// weights are `double` and never reach zero, so its first element always wins
/// the zero draw; a fixed-point weight that truncated to nothing would hand that
/// draw to a later door instead. This first door is a hundred times further than
/// the second, so the fourth power puts its exact weight at `1e-8` of the
/// nearest's -- four orders of magnitude below what twenty bits can carry.
TEST(pick_door_the_smallest_draw_answers_the_first_door_however_light) {
  const std::vector<Point> doors{Point{1000, 0}, Point{10, 0}};
  const std::uint32_t lowest = seed_drawing(0);
  REQUIRE(lowest != 0);
  Rng rng{lowest};
  CHECK(pick_door_by_distance(doors, Point{0, 0}, rng) == doors.front());
}

/// **The bar is crossed strictly**, which is visible on exactly the draws where
/// the running sum lands on it and nowhere else.
///
/// Two doors at equal distance carry equal weight, so the first one's running
/// sum is exactly half the total and a draw of exactly half the range puts the
/// two sides of the comparison equal. The original takes the *second* door
/// there; `>=` would take the first, and would be invisible to every other test
/// in this file because it moves one draw in ten thousand and one.
TEST(pick_door_the_bar_is_crossed_strictly) {
  const std::vector<Point> doors{Point{-100, 0}, Point{100, 0}};
  const std::uint32_t half = seed_drawing(5000);
  REQUIRE(half != 0);
  Rng rng{half};
  CHECK(pick_door_by_distance(doors, Point{0, 0}, rng) == doors.back());
}

/// **Whether the call advanced the stream is synchronised state.** One draw for
/// a one-door building, where the answer was never in doubt; none at all for a
/// list the original would never have handed its picker.
TEST(pick_door_draws_exactly_once_and_never_for_an_empty_list) {
  Rng rng{7};
  const std::vector<Point> one{Point{5, 5}};
  Rng expected = rng;
  (void)expected.between(0, 10000);
  CHECK(pick_door_by_distance(one, Point{0, 0}, rng) == one.front());
  CHECK(rng == expected);

  const Rng before = rng;
  CHECK(pick_door_by_distance({}, Point{0, 0}, rng) == kNoClassPoint);
  CHECK(rng == before);
}

/// The list is the class's type-**2** points. The gate declares three enter
/// points and one exit, so a body that read the enter list would answer one of
/// three doors none of which is this one.
TEST(get_exit_point_reads_the_exit_list_and_not_the_enter_list) {
  Bench b;
  const Point answer = exit_point(b, b.gate, pack_point(Point{900, 512}), script::Value::integer(0));
  CHECK(answer == b.expected(0, 64));
  CHECK(!(answer == b.expected(128, 0)));
  CHECK(!(answer == b.expected(-128, 0)));
}

/// **The reference point is the argument, not the receiver's position**, which
/// is the whole difference from `GetEnterPoint` and the reason six of the seven
/// shipped sites pass a point they computed themselves. The building does not
/// move between these two loops; only the point does.
TEST(get_exit_point_measures_from_its_argument_and_not_from_the_receiver) {
  Bench b;
  const Result<Entity> quay = Entity::parse(bytes_of(kFourExitEntity));
  REQUIRE(quay.ok());
  const ObjectId keep = b.dock(quay.value(), "Hut");

  const Point west = b.expected(-128, 0);
  const Point east = b.expected(192, 0);
  int near_west = 0;
  int near_east = 0;
  for (int i = 0; i < 200; ++i) {
    if (exit_point(b, keep, pack_point(Point{300, 512}), script::Value::integer(0)) == west) {
      ++near_west;
    }
    if (exit_point(b, keep, pack_point(Point{700, 512}), script::Value::integer(0)) == east) {
      ++near_east;
    }
  }
  CHECK(near_west > 180);
  CHECK(near_east > 180);
}

/// The two overloads differ in **where the water flag comes from and nothing
/// else**. The land and water lists are disjoint here, so neither arm can pass
/// on a body that ignored its argument.
TEST(get_exit_point_takes_the_water_flag_from_the_boolean_and_from_the_object) {
  Bench b;
  const Result<Entity> quay = Entity::parse(bytes_of(kFourExitEntity));
  REQUIRE(quay.ok());
  const ObjectId keep = b.dock(quay.value(), "Hut");
  const Point water = b.expected(-320, 0);
  const Point land = b.expected(-128, 0);

  // `(point, bool)` -- six of the seven shipped sites, all passing false.
  CHECK(exit_point(b, keep, pack_point(Point{300, 512}), script::Value::integer(1)) == water);
  CHECK(exit_point(b, keep, pack_point(Point{300, 512}), script::Value::integer(0)) == land);

  // `(Obj, point)` -- `RUIN_BEHAVIOR.VS`, and the flag is read off the object.
  const ObjectId ship = b.walker(Point{100, 512}, NativeClass::ship);
  const ObjectId foot = b.walker(Point{100, 512});
  CHECK(exit_point(b, keep, script::Value::object(kTypeObj, ship),
                   pack_point(Point{300, 512})) == water);
  CHECK(exit_point(b, keep, script::Value::object(kTypeObj, foot),
                   pack_point(Point{300, 512})) == land);
}

/// **Every fallback is a point the caller already had**, and two of the three
/// cost no draw. The `(-1, -1)` two shipped scripts test for is their own
/// argument coming back, never something this invents.
TEST(get_exit_point_falls_back_to_the_callers_point_and_the_buildings_position) {
  Bench b;
  const script::Value here = pack_point(Point{700, 300});

  // A receiver that names nothing answers the point argument, and silently.
  Rng before = b.world.rng();
  CHECK(exit_point(b, static_cast<ObjectId>(9999), here, script::Value::integer(0)) ==
        (Point{700, 300}));
  CHECK(b.world.rng() == before);

  // A building with no exit door answers its own position, also without a draw:
  // the original never reaches its picker with an empty list.
  const ObjectId doorless = b.walker(Point{300, 300});
  before = b.world.rng();
  CHECK(exit_point(b, doorless, here, script::Value::integer(0)) == (Point{300, 300}));
  CHECK(b.world.rng() == before);

  // **The receiver is not tested for life.** Neither overload asks after the
  // handle resolves, so a dead building still answers with its doors.
  CHECK(b.world.set_health(b.gate, 0));
  CHECK(exit_point(b, b.gate, here, script::Value::integer(0)) == b.expected(0, 64));
}

/// A dock with one land door and nothing on the water: everything the shipyard
/// fixup needs to have something to do.
///
/// The `x` offsets are chosen against the map this file builds. Deep water is
/// everything west of world x 224 once the terrain sampler's `+32` rounding is
/// applied, so a point at 232 is on land and its **western** neighbour, one
/// 16-unit cell away at 216, is not.
constexpr std::string_view kDockEntity =
    "<entity name=\"dock\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points>"
    "<point idx=\"0\" type=\"1\" x=\"-280\" y=\"0\"/>"
    "<point idx=\"1\" type=\"2\" x=\"-280\" y=\"0\"/>"
    "</points>"
    "</entity>";

/// **The shipyard fixup**, and it searches from a pool that is not the door
/// list: every class slot that landed on the map, of every type and before the
/// passability test, stepped one 16-unit ring outward in the order west, east,
/// north, south and then the diagonals, first hit winning.
TEST(entrance_a_shipyard_with_no_water_door_is_given_one) {
  Bench b;
  const Result<Entity> dock = Entity::parse(bytes_of(kDockEntity));
  REQUIRE(dock.ok());
  const ObjectId yard = b.dock(dock.value(), "RShipyard");

  // Its one enter point is on land, so the land list is the ordinary answer and
  // the fixup does not run on it.
  std::vector<Point> land;
  building_doors(b.world, &b.movement.grid(), yard, kEnterPointType, false, land);
  REQUIRE(land.size() == 1);
  CHECK(land[0].x == 232);

  // The water list is empty, so the fixup steps west off the same point.
  std::vector<Point> water;
  building_doors(b.world, &b.movement.grid(), yard, kEnterPointType, true, water);
  REQUIRE(water.size() == 1);
  CHECK(water[0].x == 232 - 16);
  CHECK(water[0].y == land[0].y);
  // Exactly one point, and the search stops -- it does not fill the list.

  // The exit list gets its own fixup, from the same pool.
  std::vector<Point> exit_water;
  building_doors(b.world, &b.movement.grid(), yard, kExitPointType, true, exit_water);
  REQUIRE(exit_water.size() == 1);
  CHECK(exit_water[0].x == 232 - 16);
}

/// **Only a shipyard**, which is the gate on the whole mechanism: the same
/// entity on a class that does not descend from `BaseShipyard` keeps its empty
/// list, and so does one with no class at all.
/// The pool the fixup searches from is **every slot that landed**, of every
/// type -- not the door lists. A type-3 point is not a door and can still be
/// where a shipyard's water door comes from.
///
/// It also settles the step order, which the one-sided case above cannot: this
/// candidate sits out in the sea, so its western *and* eastern neighbours are
/// both water and only the order decides which is returned.
constexpr std::string_view kDeepDockEntity =
    "<entity name=\"deep\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points>"
    "<point idx=\"0\" type=\"3\" x=\"-412\" y=\"0\"/>"
    "<point idx=\"1\" type=\"1\" x=\"-280\" y=\"0\"/>"
    "</points>"
    "</entity>";

TEST(entrance_the_fixup_searches_from_every_slot_and_steps_west_first) {
  Bench b;
  const Result<Entity> deep = Entity::parse(bytes_of(kDeepDockEntity));
  REQUIRE(deep.ok());
  const ObjectId yard = b.dock(deep.value(), "RShipyard");

  // The only enter point is on land, so the water list starts empty.
  std::vector<Point> land;
  building_doors(b.world, &b.movement.grid(), yard, kEnterPointType, false, land);
  REQUIRE(land.size() == 1);
  CHECK(land[0].x == 232);

  std::vector<Point> water;
  building_doors(b.world, &b.movement.grid(), yard, kEnterPointType, true, water);
  REQUIRE(water.size() == 1);
  // From the *type-3* point at 100, not from the enter point at 232 -- and one
  // cell west of it rather than one cell east, both of which are open water.
  CHECK(water[0].x == 100 - 16);
}

TEST(entrance_the_fixup_is_a_shipyard_rule_and_not_a_general_one) {
  Bench b;
  const Result<Entity> dock = Entity::parse(bytes_of(kDockEntity));
  REQUIRE(dock.ok());

  std::vector<Point> hut;
  building_doors(b.world, &b.movement.grid(), b.dock(dock.value(), "Hut"), kEnterPointType, true,
                 hut);
  CHECK(hut.empty());

  std::vector<Point> classless;
  const ObjectId bare = b.world.spawn(NativeClass::building, &dock.value());
  CHECK(b.world.set_position(bare, Point{512, 512}));
  building_doors(b.world, &b.movement.grid(), bare, kEnterPointType, true, classless);
  CHECK(classless.empty());
}

// --------------------------------------------------------------------------
// GetEnterExit and FindNearEnterExit
// --------------------------------------------------------------------------

/// Three gates, so "one of them, at random" is a different answer from "the
/// first" -- which is what 19 of the installation's 91 gate-bearing classes
/// make observable.
constexpr std::string_view kThreeGateEntity =
    "<entity name=\"gates\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points>"
    "<point idx=\"0\" type=\"12\" x=\"-256\" y=\"0\"/>"
    "<point idx=\"1\" type=\"12\" x=\"0\" y=\"-64\"/>"
    "<point idx=\"2\" type=\"12\" x=\"256\" y=\"0\"/>"
    "</points>"
    "</entity>";

/// No gate at all, so the "nothing to choose from" arm can be told apart from
/// the "chose the only one" arm -- including in whether it draws.
constexpr std::string_view kGatelessEntity =
    "<entity name=\"blind\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points><point idx=\"0\" type=\"3\" x=\"32\" y=\"0\"/></points>"
    "</entity>";

TEST(get_enter_exit_on_a_building_draws_among_its_gates) {
  Bench b;
  const Result<Entity> gates = Entity::parse(bytes_of(kThreeGateEntity));
  REQUIRE(gates.ok());
  const ObjectId keep = b.dock(gates.value(), "Hut");

  std::vector<Point> all;
  class_points_of_type(b.world, keep, kEnterExitPointType, all);
  REQUIRE(all.size() == 3);

  // Every answer is one of the three, and across enough draws it is not always
  // the same one -- which is the assertion "the first" would fail.
  bool seen[3] = {false, false, false};
  for (int i = 0; i < 40; ++i) {
    const Point answer = unpack_point(
        member_call(b, "GetEnterExit", {script::Value::object(kTypeObj, keep)}).value);
    bool matched = false;
    for (std::size_t k = 0; k < all.size(); ++k) {
      if (answer == all[k]) {
        seen[k] = true;
        matched = true;
      }
    }
    CHECK(matched);
  }
  CHECK(seen[0]);
  CHECK(seen[1]);
  CHECK(seen[2]);
}

/// **Whether the call advances the stream is synchronised state**, so the two
/// arms have to differ in draws and not only in answers.
TEST(get_enter_exit_draws_once_when_there_is_a_gate_and_never_when_there_is_not) {
  Bench b;
  const Result<Entity> gates = Entity::parse(bytes_of(kThreeGateEntity));
  const Result<Entity> blind = Entity::parse(bytes_of(kGatelessEntity));
  REQUIRE(gates.ok());
  REQUIRE(blind.ok());
  const ObjectId keep = b.dock(gates.value(), "Hut");
  const ObjectId shed = b.dock(blind.value(), "Hut");

  Rng before = b.world.rng();
  (void)member_call(b, "GetEnterExit", {script::Value::object(kTypeObj, keep)});
  CHECK(!(b.world.rng() == before));
  // One draw, not one per gate.
  Rng expected = before;
  (void)expected.between(0, 2);
  CHECK(b.world.rng() == expected);

  before = b.world.rng();
  const Point none = unpack_point(
      member_call(b, "GetEnterExit", {script::Value::object(kTypeObj, shed)}).value);
  CHECK(none == kNoClassPoint);
  CHECK(b.world.rng() == before);

  // And a receiver that names nothing is the same silent sentinel.
  before = b.world.rng();
  CHECK(unpack_point(member_call(b, "GetEnterExit",
                                 {script::Value::object(kTypeObj, static_cast<ObjectId>(9999))})
                         .value) == kNoClassPoint);
  CHECK(b.world.rng() == before);
}

/// On a **Settlement** the same name answers a building, and the only test it
/// applies is "does this one's class declare a gate at all".
TEST(get_enter_exit_on_a_settlement_draws_among_the_buildings_that_have_one) {
  Bench b;
  const Result<Entity> gates = Entity::parse(bytes_of(kThreeGateEntity));
  const Result<Entity> blind = Entity::parse(bytes_of(kGatelessEntity));
  REQUIRE(gates.ok());
  REQUIRE(blind.ok());
  const ObjectId hall = b.dock(blind.value(), "Hut");
  const ObjectId north = b.dock(gates.value(), "Hut");
  const ObjectId south = b.dock(gates.value(), "Hut");
  const SettlementId town = b.town(hall, {hall, north, south});

  bool saw_north = false;
  bool saw_south = false;
  for (int i = 0; i < 40; ++i) {
    const script::Value answer =
        member_call(b, "GetEnterExit", {b.settlement_value(town)}).value;
    REQUIRE(answer.is_object());
    // Never the gateless hall, whichever way the draw fell.
    CHECK(answer.as_object().id != hall);
    if (answer.as_object().id == north) saw_north = true;
    if (answer.as_object().id == south) saw_south = true;
  }
  CHECK(saw_north);
  CHECK(saw_south);

  // A settlement whose buildings have no gate answers the invalid handle and
  // draws nothing.
  const ObjectId alone = b.dock(blind.value(), "Hut");
  const SettlementId hamlet = b.town(alone, {alone});
  const Rng before = b.world.rng();
  const script::Value empty =
      member_call(b, "GetEnterExit", {b.settlement_value(hamlet)}).value;
  REQUIRE(empty.is_object());
  CHECK(empty.as_object().type == script::kNoType);
  CHECK(b.world.rng() == before);
}

/// **The dispatch is on the handle the caller passed, not on what the handle
/// can be resolved to.** This is the shipped shape -- `bDamaged.GetEnterExit`
/// is called on a building that belongs to the settlement two lines above --
/// and a dispatch that asked "does this resolve to a settlement" instead would
/// take the wrong arm for every one of those sites, answering a Building where
/// the script assigns a point.
TEST(get_enter_exit_reads_a_buildings_own_gate_even_inside_a_settlement) {
  Bench b;
  const Result<Entity> gates = Entity::parse(bytes_of(kThreeGateEntity));
  const Result<Entity> blind = Entity::parse(bytes_of(kGatelessEntity));
  REQUIRE(gates.ok());
  REQUIRE(blind.ok());
  const ObjectId hall = b.dock(blind.value(), "Hut");
  const ObjectId keep = b.dock(gates.value(), "Hut");
  (void)b.town(hall, {hall, keep});
  // The back-link the economy resolves `set.gold` on a building through.
  REQUIRE(b.world.find(keep) != nullptr);
  b.world.find(keep)->settlement = hall;

  std::vector<Point> all;
  class_points_of_type(b.world, keep, kEnterExitPointType, all);
  REQUIRE(all.size() == 3);

  const script::Value answer =
      member_call(b, "GetEnterExit", {script::Value::object(kTypeObj, keep)}).value;
  // A point, not a handle -- the Building arm.
  REQUIRE(is_point(answer));
  const Point where = unpack_point(answer);
  CHECK((where == all[0] || where == all[1] || where == all[2]));
}

/// **No aliveness filter**, which is the thing a reimplementation would add on
/// its own and would answer a different building for.
TEST(get_enter_exit_on_a_settlement_will_hand_back_a_dead_building) {
  Bench b;
  const Result<Entity> gates = Entity::parse(bytes_of(kThreeGateEntity));
  const Result<Entity> blind = Entity::parse(bytes_of(kGatelessEntity));
  REQUIRE(gates.ok());
  REQUIRE(blind.ok());
  const ObjectId hall = b.dock(blind.value(), "Hut");
  const ObjectId ruin = b.dock(gates.value(), "Hut");
  CHECK(b.world.set_health(ruin, 0));
  const SettlementId town = b.town(hall, {hall, ruin});

  const script::Value answer = member_call(b, "GetEnterExit", {b.settlement_value(town)}).value;
  REQUIRE(answer.is_object());
  CHECK(answer.as_object().id == ruin);
}

/// The deterministic sibling: every gate of every building competes, the
/// nearest wins, and a tie goes to the earlier one.
TEST(find_near_enter_exit_keeps_the_nearest_gate_of_any_building) {
  Bench b;
  const Result<Entity> gates = Entity::parse(bytes_of(kThreeGateEntity));
  const Result<Entity> blind = Entity::parse(bytes_of(kGatelessEntity));
  REQUIRE(gates.ok());
  REQUIRE(blind.ok());
  const ObjectId hall = b.dock(blind.value(), "Hut");
  const ObjectId keep = b.dock(gates.value(), "Hut");
  const SettlementId town = b.town(hall, {hall, keep});

  std::vector<Point> all;
  class_points_of_type(b.world, keep, kEnterExitPointType, all);
  REQUIRE(all.size() == 3);

  const auto near = [&](Point from) {
    const Rng before = b.world.rng();
    const Point answer = unpack_point(
        member_call(b, "FindNearEnterExit", {b.settlement_value(town), pack_point(from)}).value);
    // Deterministic, unlike its neighbour.
    CHECK(b.world.rng() == before);
    return answer;
  };
  // The western gate, the northern one and the eastern one, each picked out by
  // standing beside it -- so all three of the building's gates really compete
  // rather than only its first.
  CHECK(near(Point{0, 512}) == all[0]);
  CHECK(near(Point{512, 0}) == all[1]);
  CHECK(near(Point{1000, 512}) == all[2]);

  // A settlement with no gate anywhere is the sentinel.
  const ObjectId alone = b.dock(blind.value(), "Hut");
  const SettlementId hamlet = b.town(alone, {alone});
  CHECK(unpack_point(
            member_call(b, "FindNearEnterExit", {b.settlement_value(hamlet), pack_point(Point{})})
                .value) == kNoClassPoint);
}

// --------------------------------------------------------------------------
// GetExitVector / exit_vector
// --------------------------------------------------------------------------

/// **Two names, two entry points in the executable, one value** -- and it is
/// the one member of this family that reads no point at all.
TEST(exit_vector_is_a_class_scalar_pair_and_not_a_point) {
  Bench b;
  const Result<Entity> blind = Entity::parse(bytes_of(kGatelessEntity));
  REQUIRE(blind.ok());
  const auto vector_of = [&](const char* class_name, const char* name) {
    const ObjectId id =
        b.world.spawn(NativeClass::building, &blind.value(), b.graph.find(class_name));
    CHECK(b.world.set_position(id, Point{512, 512}));
    return unpack_point(member_call(b, name, {script::Value::object(kTypeObj, id)}).value);
  };

  // World units, not a direction: the corpus scales this by five.
  CHECK(vector_of("Dock", "GetExitVector") == (Point{-120, 80}));
  // Both names reach the same body.
  CHECK(vector_of("Dock", "exit_vector") == (Point{-120, 80}));
  // Inherited, and overridden.
  CHECK(vector_of("DockHeir", "GetExitVector") == (Point{-120, 80}));
  CHECK(vector_of("DockOwn", "GetExitVector") == (Point{10, -20}));

  // **The pair rule**: a class that declares only `exit_vector_x` takes
  // *neither* half from itself. A per-key resolution would answer (99, 80)
  // here, which is a vector no class ever authored.
  CHECK(vector_of("DockHalf", "GetExitVector") == (Point{-120, 80}));

  // A class with nothing anywhere up its chain, and a handle that names
  // nothing, are the same sentinel -- which no shipped caller tests.
  CHECK(vector_of("Hut", "GetExitVector") == (Point{-1, -1}));
  CHECK(unpack_point(member_call(b, "GetExitVector",
                                 {script::Value::object(kTypeObj, static_cast<ObjectId>(9999))})
                         .value) == (Point{-1, -1}));
}

// --------------------------------------------------------------------------
// Settlement::WaterLsa
// --------------------------------------------------------------------------

/// A yard with one exit door out in the open sea and one on land, so the *exit
/// water* list is what the answer has to come from and not either of the other
/// three. The type-1 point is there to make the enter lists non-empty, which
/// keeps the shipyard fixup out of the way.
constexpr std::string_view kYardEntity =
    "<entity name=\"yard\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points>"
    "<point idx=\"0\" type=\"1\" x=\"-280\" y=\"0\"/>"
    "<point idx=\"1\" type=\"2\" x=\"-280\" y=\"0\"/>"
    "<point idx=\"2\" type=\"2\" x=\"-412\" y=\"0\"/>"
    "</points>"
    "</entity>";

/// **Not a settlement property**: it is a shipyard's water area, reached
/// through the settlement's central building, and a settlement whose central
/// building is anything else answers 0.
TEST(water_lsa_answers_the_area_the_shipyards_water_door_stands_in) {
  Bench b{-1, /*shore=*/true};
  // The walled coast gives the partition three areas: the two seas and the land
  // between them. Without it there is one area and nothing to tell apart.
  REQUIRE(b.world.lsa().size() == 3);
  const LsaId sea = b.world.lsa().at(Point{100, 512});
  const LsaId land = b.world.lsa().at(Point{512, 512});
  REQUIRE(sea != kNoLsa);
  REQUIRE(land != kNoLsa);
  REQUIRE(sea != land);
  CHECK(b.world.lsa().water(sea));

  const Result<Entity> yard = Entity::parse(bytes_of(kYardEntity));
  const Result<Entity> blind = Entity::parse(bytes_of(kGatelessEntity));
  REQUIRE(yard.ok());
  REQUIRE(blind.ok());

  const ObjectId dock = b.dock(yard.value(), "RShipyard");
  const SettlementId port = b.town(dock, {dock});
  const script::HostOutcome out = member_call(b, "WaterLsa", {b.settlement_value(port)});
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.as_integer() == sea);

  // The same entity on a class that is not a shipyard answers 0, which is the
  // arm every non-shipyard settlement in the game takes.
  const ObjectId hut = b.dock(yard.value(), "Hut");
  const SettlementId village = b.town(hut, {hut});
  CHECK(member_call(b, "WaterLsa", {b.settlement_value(village)}).value.as_integer() == kNoLsa);

  // And a shipyard with no water door at all -- where the original reads
  // through a null pointer -- answers 0 here.
  const ObjectId dry = b.dock(blind.value(), "RShipyard");
  const SettlementId inland = b.town(dry, {dry});
  const script::HostOutcome dry_out = member_call(b, "WaterLsa", {b.settlement_value(inland)});
  // **`ok`, not a refusal.** Both readers use the answer straight away, so a
  // trap here would stop a mission where the original quietly kept going.
  CHECK(dry_out.status == script::HostStatus::ok);
  CHECK(dry_out.value.as_integer() == kNoLsa);
}

/// **The first water door, not the last**, which takes a yard with a door in
/// each of two disjoint seas to see at all.
TEST(water_lsa_answers_the_area_of_the_first_water_door) {
  Bench b{-1, /*shore=*/true};
  const LsaId west = b.world.lsa().at(Point{100, 512});
  const LsaId east = b.world.lsa().at(Point{924, 512});
  REQUIRE(west != kNoLsa);
  REQUIRE(east != kNoLsa);
  REQUIRE(west != east);

  // Two exit points, west first in table order.
  const Result<Entity> straddle = Entity::parse(bytes_of(
      "<entity name=\"straddle\" type=\"vx/building\" variations=\"1\" pass_file=\"\" "
      "radius=\"1\"><points>"
      "<point idx=\"0\" type=\"2\" x=\"-412\" y=\"0\"/>"
      "<point idx=\"1\" type=\"2\" x=\"412\" y=\"0\"/>"
      "</points></entity>"));
  REQUIRE(straddle.ok());
  const ObjectId dock = b.dock(straddle.value(), "RShipyard");
  std::vector<Point> doors;
  building_doors(b.world, &b.movement.grid(), dock, kExitPointType, true, doors);
  REQUIRE(doors.size() == 2);

  const SettlementId port = b.town(dock, {dock});
  CHECK(member_call(b, "WaterLsa", {b.settlement_value(port)}).value.as_integer() == west);
}

/// **The land exclusion**, which needs a map whose coast is *not* walled: the
/// partition's fill tests passability and never terrain, so the sea and the
/// land become one area, and the shipyard's water door then stands in the
/// settlement's own area. The original refuses that answer and so does this.
TEST(water_lsa_refuses_the_settlements_own_area) {
  Bench b;  // nothing blocked, so one area over the whole map
  REQUIRE(b.world.lsa().size() == 1);
  const Result<Entity> yard = Entity::parse(bytes_of(kYardEntity));
  REQUIRE(yard.ok());
  const ObjectId dock = b.dock(yard.value(), "RShipyard");
  std::vector<Point> doors;
  building_doors(b.world, &b.movement.grid(), dock, kExitPointType, true, doors);
  // There *is* a water door -- the terrain says so -- and it is in the same
  // area as the yard, which is the case the exclusion exists for.
  REQUIRE(doors.size() == 1);
  CHECK(b.world.lsa().at(doors.front()) == b.world.lsa().at(Point{512, 512}));

  const SettlementId port = b.town(dock, {dock});
  CHECK(member_call(b, "WaterLsa", {b.settlement_value(port)}).value.as_integer() == kNoLsa);
}

/// The equality `SHIPYARD_IDLE.VS` turns on -- "is my supplier on the same sea
/// as me" -- which is the whole reason the value has to be an area id and not
/// a position.
TEST(water_lsa_is_equal_for_two_yards_on_one_sea) {
  Bench b{-1, /*shore=*/true};
  const Result<Entity> yard = Entity::parse(bytes_of(kYardEntity));
  REQUIRE(yard.ok());

  // Both well inside the screen rectangle's southern margin, which excludes the
  // last few hundred world units of the map -- see `screen_bounds`.
  const ObjectId north = b.dock(yard.value(), "RShipyard");
  CHECK(b.world.set_position(north, Point{512, 192}));
  const ObjectId south = b.dock(yard.value(), "RShipyard");
  CHECK(b.world.set_position(south, Point{512, 544}));
  const SettlementId a = b.town(north, {north});
  const SettlementId c = b.town(south, {south});

  const std::int32_t first = member_call(b, "WaterLsa", {b.settlement_value(a)}).value.as_integer();
  const std::int32_t second = member_call(b, "WaterLsa", {b.settlement_value(c)}).value.as_integer();
  CHECK(first != kNoLsa);
  CHECK(first == second);
}
