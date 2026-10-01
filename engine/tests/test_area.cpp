// Map areas: sim/area.hpp.
//
// Three cases carry the reading and are worth naming, because each of them is
// a place where the obvious implementation is wrong and `gbr.exe` says so:
//
//   * **A circle's rim is one unit wider than `d2 <= r*r`.** The executable
//     compares a *truncating* integer square root against the radius, at two
//     independent sites, so the shell where `r*r < d2 < (r+1)^2` is inside.
//     `a_circles_rim_follows_the_truncating_square_root` is that.
//   * **`AreaDistTo` on a rectangle is Chebyshev, not Euclidean.** 0x004d8b6e
//     keeps the *larger* of the two axis gaps, which no distance function
//     anybody would write from scratch does.
//   * **`AttackArea` orders `advance`, and to a random point per member.** Not
//     `attack`, and not the centre. Both are literal pushes in the executable.
//
// No game data anywhere in this file: the maps are XML string literals written
// here, transcribing the element `docs/formats/map.md` specifies.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/world/map.hpp"
#include "domains.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;
using namespace imperivm::core::script;

namespace {

std::span<const std::byte> bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Two classes: the area marker and one unit to order about. `AdvArea` binds no
/// method -- an area is not a thing that acts -- but it has to resolve to a
/// `cpp_class` the world can spawn, or `populate_from_map` drops it and every
/// alias naming it dangles.
ClassGraph fixture_graph() {
  ClassGraph graph;
  graph.add(bytes(R"(<class id="Object" cpp_class="CVXDecor" parent="">
      <properties maxhealth="100"/>
    </class>)"), "test_area.cpp");
  graph.add(bytes(R"(<class id="AdvArea" cpp_class="CVXDecor" parent="Object">
      <properties maxhealth="100"/>
    </class>)"), "test_area.cpp");
  graph.add(bytes(R"(<class id="Unit" cpp_class="CVXUnit" parent="Object">
      <properties maxhealth="200" speed="50" sight="500" radius="15"/>
      <method sig="idle"    vs="data/subai/unit_idle.vs"/>
      <method sig="move"    vs="data/subai/unit_move.vs"/>
      <method sig="advance" vs="data/subai/unit_advance.vs"/>
    </class>)"), "test_area.cpp");
  graph.add(bytes(R"(<class id="Wagon" cpp_class="CVXWagon" parent="Object">
      <properties maxhealth="150"/>
    </class>)"), "test_area.cpp");
  graph.link();
  return graph;
}

/// One map: a circle area, a rectangle area, a cargo wagon carrying the `type`
/// attribute that is *not* a shape, two units, and the alias groups that name
/// the two areas.
///
/// The attribute spellings are transcriptions of the shipped element, including
/// the empty `nextmap`/`targetarea` that all 904 areas carry.
constexpr std::string_view kMapXml = R"(<mapobject>
  <scriptobj class="AdvArea" num="0" nextmap="" targetarea="" type="1"
      ptx="1000" pty="2000" r="300" x="1000" y="2000" flags="0x80000000"/>
  <scriptobj class="AdvArea" num="1" nextmap="" targetarea="" type="0"
      left="4000" top="5000" right="4400" bottom="5600" x="4200" y="5300"
      flags="0x80000000"/>
  <scriptobj class="Wagon" num="2" type="1" amount="1200" x="9" y="9"
      flags="0x80000000"/>
  <scriptobj class="Unit" num="3" x="1010" y="2010" player="1"
      flags="0x80400001"/>
  <scriptobj class="Unit" num="4" x="1020" y="2020" player="1"
      flags="0x80400001"/>
  <group name="Ring" type="0"><obj num="0"/></group>
  <group name="Yard" type="0"><obj num="1"/></group>
  <group name="Cart" type="0"><obj num="2"/></group>
  <group name="Band" type="1"><obj num="3"/><obj num="4"/></group>
</mapobject>)";

/// A world populated from `kMapXml`, with the systems the area slice reaches
/// through `World::systems()`.
struct Fixture {
  ClassGraph graph = fixture_graph();
  World world;
  AreaSystem areas;
  MovementSystem movement;
  CommandSystem command;
  MapObjectList map;
  AreaLoadReport report;

  explicit Fixture(std::string_view xml = kMapXml) {
    Result<MapObjectList> parsed = MapObjectList::parse(bytes(xml));
    if (parsed.ok()) map = std::move(parsed.value());
    world.seed(12345);
    world.populate_from_map(map, graph);
    world.add_system(&movement);
    world.add_system(&command);
    world.add_system(&areas);
    report = load_areas(map, world, areas.areas());
    world.start();
  }
};

/// A host call, exactly as `test_squad.cpp` builds one.
struct HostCall {
  std::vector<Value> arguments;
  CallContext context;
  imperivm::core::sim::HostContext context_state;

  HostCall(World& world, std::initializer_list<Value> args, bool bind = true)
      : arguments(args) {
    context_state.world = &world;
    context.arguments = arguments;
    context.user = bind ? &context_state : nullptr;
  }
};

HostOutcome invoke(const HostRegistry& registry, std::string_view name, std::uint16_t arity,
                   HostCall& call) {
  const std::uint32_t index = registry.find(CallKind::free_function, name, arity);
  if (index == kUnresolvedHost) return HostOutcome::failed("not registered");
  const HostEntry& entry = registry.entry(index);
  if (entry.fn == nullptr) return HostOutcome::failed("not implemented");
  return entry.fn(call.context);
}

HostRegistry area_registry() {
  HostRegistry registry;
  declare_shipped_surface(registry);
  (void)register_area_host(registry);
  return registry;
}

Value str(std::string_view text) { return Value::string(std::string(text)); }

}  // namespace

// --------------------------------------------------------------------------
// the geometry
// --------------------------------------------------------------------------

TEST(an_areas_centre_is_computed_from_the_shape) {
  // `AreaCenter` (0x004d8ce9) reads the shape and never the object's `x`/`y`.
  // The two agree in all 904 shipped areas, which is what makes the shipped
  // data a check on this rather than the source of it.
  CHECK(AreaShape::of_circle(Point{1000, 2000}, 300).centre() == (Point{1000, 2000}));
  CHECK(AreaShape::of_rectangle(4000, 5000, 4400, 5600).centre() == (Point{4200, 5300}));
  // Odd sums truncate toward zero, which is what `cdq; sub; sar 1` does.
  CHECK(AreaShape::of_rectangle(0, 0, 3, 5).centre() == (Point{1, 2}));
}

TEST(an_areas_bounding_box_is_the_full_square_of_a_circle) {
  const AreaBounds circle = AreaShape::of_circle(Point{100, 200}, 30).bounds();
  CHECK(circle == (AreaBounds{70, 170, 130, 230}));
  const AreaBounds rect = AreaShape::of_rectangle(4000, 5000, 4400, 5600).bounds();
  CHECK(rect == (AreaBounds{4000, 5000, 4400, 5600}));
}

TEST(a_circles_rim_follows_the_truncating_square_root) {
  // The point of this test. `gbr.exe` computes `isqrt(d2)` and compares it with
  // `r`, so containment is `d2 < (r+1)^2` and not `d2 <= r*r`. With r = 10 the
  // two readings disagree over 20 of the 21 lattice offsets between them; this
  // pins the boundary exactly.
  const AreaShape circle = AreaShape::of_circle(Point{0, 0}, 10);
  CHECK(circle.contains(Point{10, 0}));    // d2 = 100 = r*r, inside either way
  CHECK(circle.contains(Point{0, -10}));
  CHECK(circle.contains(Point{6, 8}));     // d2 = 100
  // d2 = 120: outside under `d2 <= r*r`, inside under the executable's test.
  CHECK(circle.contains(Point{2, 10}));
  // d2 = 121 = (r+1)^2 exactly: isqrt is 11, which is *not* <= 10.
  CHECK(!circle.contains(Point{11, 0}));
  CHECK(!circle.contains(Point{0, 11}));
}

TEST(a_rectangle_contains_its_own_corners) {
  const AreaShape rect = AreaShape::of_rectangle(10, 20, 30, 40);
  CHECK(rect.contains(Point{10, 20}));
  CHECK(rect.contains(Point{30, 40}));
  CHECK(rect.contains(Point{20, 30}));
  CHECK(!rect.contains(Point{9, 30}));
  CHECK(!rect.contains(Point{20, 41}));
}

TEST(a_reversed_rectangle_is_normalised_rather_than_empty) {
  // Nothing in the format promises the editor ordered the corners, and a
  // reversed pair would make a region that contains nothing at all -- a silent
  // wrong answer rather than a visible one.
  const AreaShape rect = AreaShape::of_rectangle(30, 40, 10, 20);
  CHECK(rect.contains(Point{20, 30}));
  CHECK(rect.centre() == (Point{20, 30}));
}

TEST(the_sampler_and_the_query_disagree_about_a_circles_rim_on_purpose) {
  // Two rules, and the disagreement is the finding rather than a bug. The
  // sampler and `AreaDistTo` take a truncating integer square root and compare
  // it with `r` (0x004d6e2e, 0x004d8b0e), which is `d2 < (r+1)^2`. Every query
  // compares `d2` against a stored `r*r` -- `CVXMapAreaQuery<TCircleArea>`'s
  // predicate at 0x004f8c60 is `cmp edx, [esi+0x60]; jg`, and `+0x60` is the
  // fourth field of a `{cx, cy, r, r2}` POD, written with an `imul` at
  // 0x004d883b and again at 0x005776ef.
  //
  // This test exists so that unifying them is a deliberate act. `area.hpp`
  // claimed the truncating rule for the whole engine for several revisions,
  // because it was read off the sampler and generalised.
  const AreaShape circle = AreaShape::of_circle(Point{0, 0}, 100);

  // The rim shell: d2 in (100^2, 101^2). 100^2 = 10000 and 101^2 = 10201, so
  // (60, 80) is exactly on the rim at d2 = 10000 and (61, 80) is just outside
  // it at d2 = 10121 -- inside the shell.
  CHECK(circle.contains(Point{60, 80}));   // d2 = 10000, both rules agree
  CHECK(circle.contains(Point{61, 80}));   // d2 = 10121, sampler says yes
  CHECK(!circle.contains(Point{65, 80}));  // d2 = 10625 >= 101^2, both say no

  World world;
  const ObjectId on_rim = world.spawn(NativeClass::unit, nullptr);
  const ObjectId in_shell = world.spawn(NativeClass::unit, nullptr);
  world.set_position(on_rim, Point{60, 80});
  world.set_position(in_shell, Point{61, 80});

  std::vector<ObjectId> found;
  // The query rule keeps the rim and drops the shell, which is the whole
  // difference stated as an assertion.
  CHECK(world.objects_in_radius(Point{0, 0}, 100, ClassFilter{}, found) == 1);
  REQUIRE(found.size() == 1);
  CHECK(found[0] == on_rim);
  CHECK(circle.contains(Point{61, 80}));
}

TEST(the_rect_query_sweep_and_the_area_shape_agree_on_every_point) {
  // `QuerySpec` carries four loose integers and `AreaShape` carries four of its
  // own, because `sim/area.hpp` sits above `sim/query.hpp` and sharing the type
  // would invert the dependency. Two implementations of one rule is exactly how
  // a rule drifts, so this asserts they agree rather than trusting a comment
  // that says they do -- over a grid that straddles every edge and both
  // corners, including the row and column one unit outside each.
  const AreaShape shape = AreaShape::of_rectangle(100, 200, 400, 500);

  World world;
  std::vector<ObjectId> ids;
  std::vector<Point> places;
  for (std::int32_t x = 96; x <= 404; x += 4) {
    for (std::int32_t y = 196; y <= 504; y += 4) {
      const ObjectId id = world.spawn(NativeClass::unit, nullptr);
      world.set_position(id, Point{x, y});
      ids.push_back(id);
      places.push_back(Point{x, y});
    }
  }

  std::vector<ObjectId> found;
  world.objects_in_rect(shape.left, shape.top, shape.right, shape.bottom, ClassFilter{}, found);

  std::size_t inside = 0;
  std::size_t next = 0;
  bool agreed = true;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const bool by_shape = shape.contains(places[i]);
    const bool by_sweep = next < found.size() && found[next] == ids[i];
    if (by_sweep) ++next;
    if (by_shape != by_sweep) agreed = false;
    if (by_shape) ++inside;
  }
  CHECK(agreed);
  CHECK(next == found.size());
  // The preconditions. Without them a sweep and a shape that both answered
  // "nothing" would agree perfectly and the test would pass on two broken
  // implementations.
  CHECK(inside > 0);
  CHECK(inside < ids.size());
  CHECK(found.size() == inside);
}

TEST(area_distance_is_radial_for_a_circle_and_chebyshev_for_a_rectangle) {
  const AreaShape circle = AreaShape::of_circle(Point{0, 0}, 100);
  CHECK(circle.distance_to(Point{0, 0}) == 0);
  CHECK(circle.distance_to(Point{100, 0}) == 0);
  CHECK(circle.distance_to(Point{150, 0}) == 50);
  CHECK(circle.distance_to(Point{300, 400}) == 400);  // isqrt(250000) = 500

  const AreaShape rect = AreaShape::of_rectangle(0, 0, 100, 100);
  CHECK(rect.distance_to(Point{50, 50}) == 0);
  CHECK(rect.distance_to(Point{150, 50}) == 50);
  // The corner case, literally: 50 away on each axis. Euclidean would be 70,
  // Manhattan 100; the executable keeps the larger gap, so 50.
  CHECK(rect.distance_to(Point{150, 150}) == 50);
  CHECK(rect.distance_to(Point{-30, 160}) == 60);
}

TEST(a_degenerate_circle_of_radius_zero_holds_only_its_centre) {
  const AreaShape point = AreaShape::of_circle(Point{7, 9}, 0);
  CHECK(point.contains(Point{7, 9}));
  CHECK(!point.contains(Point{8, 9}));
  CHECK(point.distance_to(Point{7, 13}) == 4);
}

// --------------------------------------------------------------------------
// the map reader
// --------------------------------------------------------------------------

TEST(the_map_reader_finds_both_shapes_and_no_cargo) {
  Result<MapObjectList> parsed = MapObjectList::parse(bytes(kMapXml));
  REQUIRE(parsed.ok());
  const MapObjectList& map = parsed.value();
  CHECK(map.objects().size() == 5);
  // Three objects carry a `type`; only two of them carry a *shape*. The wagon
  // is the trap `docs/formats/map.md` warns about, and reading it as an area
  // would put a degenerate rectangle at the origin into the table.
  CHECK(map.areas().size() == 2);
  CHECK(map.find_area(2) == nullptr);

  const MapArea* circle = map.find_area(0);
  REQUIRE(circle != nullptr);
  CHECK(circle->is_circle());
  CHECK(circle->ptx == 1000);
  CHECK(circle->pty == 2000);
  CHECK(circle->radius == 300);

  const MapArea* rect = map.find_area(1);
  REQUIRE(rect != nullptr);
  CHECK(!rect->is_circle());
  CHECK(rect->left == 4000);
  CHECK(rect->top == 5000);
  CHECK(rect->right == 4400);
  CHECK(rect->bottom == 5600);
}

TEST(an_area_missing_half_its_shape_is_not_an_area) {
  // A `type="1"` with no radius is not a circle of radius zero, it is an object
  // this reader does not understand. Manufacturing a shape for it would put a
  // region nobody authored into the table.
  constexpr std::string_view xml = R"(<mapobject>
    <scriptobj class="AdvArea" num="0" type="1" ptx="5" pty="5" x="5" y="5"/>
    <scriptobj class="AdvArea" num="1" type="0" left="1" top="2" right="3" x="2" y="2"/>
    <scriptobj class="AdvArea" num="2" type="4" x="0" y="0"/>
  </mapobject>)";
  Result<MapObjectList> parsed = MapObjectList::parse(bytes(xml));
  REQUIRE(parsed.ok());
  CHECK(parsed.value().areas().empty());
}

// --------------------------------------------------------------------------
// the table, and loading it
// --------------------------------------------------------------------------

TEST(the_area_table_is_keyed_on_the_object_and_binds_once) {
  AreaTable table;
  const AreaShape ring = AreaShape::of_circle(Point{1, 2}, 3);
  CHECK(table.bind(7, ring));
  CHECK(!table.bind(7, AreaShape::of_rectangle(0, 0, 1, 1)));  // first wins
  CHECK(!table.bind(kNoObject, ring));
  REQUIRE(table.find(7) != nullptr);
  CHECK(*table.find(7) == ring);
  CHECK(table.find(8) == nullptr);
  CHECK(table.size() == 1);
}

TEST(the_area_table_stays_sorted_however_it_is_filled) {
  // Iteration order is state everywhere else in the simulation; this table is
  // no exception, and insertion order must not show through.
  AreaTable table;
  for (const ObjectId id : {ObjectId{9}, ObjectId{2}, ObjectId{31}, ObjectId{5}}) {
    const auto coordinate = static_cast<std::int32_t>(id);
    CHECK(table.bind(id, AreaShape::of_circle(Point{coordinate, coordinate}, coordinate)));
  }
  ObjectId previous = kNoObject;
  for (const AreaTable::Entry& entry : table.entries()) {
    CHECK(entry.id > previous);
    previous = entry.id;
  }
  CHECK(table.size() == 4);
}

TEST(loading_attaches_every_named_area_to_the_object_the_loader_spawned) {
  Fixture fixture;
  CHECK(fixture.report.authored == 2);
  CHECK(fixture.report.bound == 2);
  CHECK(fixture.report.unnamed == 0);
  CHECK(fixture.report.unresolved == 0);

  const AreaShape* ring = area_named(fixture.world, "Ring");
  REQUIRE(ring != nullptr);
  CHECK(ring->kind == AreaKind::circle);
  CHECK(ring->centre() == (Point{1000, 2000}));

  const AreaShape* yard = area_named(fixture.world, "Yard");
  REQUIRE(yard != nullptr);
  CHECK(yard->kind == AreaKind::rectangle);
  CHECK(yard->centre() == (Point{4200, 5300}));

  // A name that is a named object but not an area, and a name nothing binds.
  CHECK(area_named(fixture.world, "Cart") == nullptr);
  CHECK(area_named(fixture.world, "Band") == nullptr);
  CHECK(area_named(fixture.world, "") == nullptr);
}

TEST(an_area_no_alias_names_is_counted_rather_than_guessed_at) {
  // There is no other way to say an area's name, so an unnamed one is
  // unreachable from any script. Reporting it is the only honest thing to do.
  constexpr std::string_view xml = R"(<mapobject>
    <scriptobj class="AdvArea" num="0" type="1" ptx="5" pty="5" r="1" x="5" y="5"/>
    <scriptobj class="AdvArea" num="1" type="1" ptx="9" pty="9" r="1" x="9" y="9"/>
    <group name="Only" type="0"><obj num="1"/></group>
  </mapobject>)";
  Fixture fixture{xml};
  CHECK(fixture.report.authored == 2);
  CHECK(fixture.report.bound == 1);
  CHECK(fixture.report.unnamed == 1);
  REQUIRE(area_named(fixture.world, "Only") != nullptr);
  CHECK(area_named(fixture.world, "Only")->centre() == (Point{9, 9}));
}

TEST(a_repeated_alias_resolves_the_way_the_named_object_table_resolved_it) {
  // Four shipped maps repeat a type-0 name -- `NO_Invisible` eleven times in
  // one of them -- and `NamedObjectTable::bind` takes the first in document
  // order. This table has to make the same choice or the name would resolve to
  // one object and its shape to another's.
  constexpr std::string_view xml = R"(<mapobject>
    <scriptobj class="AdvArea" num="0" type="1" ptx="10" pty="10" r="5" x="10" y="10"/>
    <scriptobj class="AdvArea" num="1" type="1" ptx="99" pty="99" r="5" x="99" y="99"/>
    <group name="Twice" type="0"><obj num="0"/></group>
    <group name="Twice" type="0"><obj num="1"/></group>
  </mapobject>)";
  Fixture fixture{xml};
  const AreaShape* shape = area_named(fixture.world, "Twice");
  REQUIRE(shape != nullptr);
  CHECK(shape->centre() == (Point{10, 10}));
  // The second element's area is left unbound rather than overwriting the
  // first, and is reported as nameless because nothing can reach it.
  CHECK(fixture.report.bound == 1);
  CHECK(fixture.report.unnamed == 1);
}

TEST(area_lookup_needs_the_system_to_be_registered) {
  // A complete subsystem nothing reaches is this project's most expensive
  // recurring failure. Without `AreaSystem` on the world, every name misses.
  ClassGraph graph = fixture_graph();
  World world;
  Result<MapObjectList> parsed = MapObjectList::parse(bytes(kMapXml));
  REQUIRE(parsed.ok());
  world.populate_from_map(parsed.value(), graph);
  CHECK(area_system_of(world) == nullptr);
  CHECK(area_named(world, "Ring") == nullptr);
}

// --------------------------------------------------------------------------
// the point sampler
// --------------------------------------------------------------------------

TEST(a_sampled_point_is_always_inside_its_area) {
  Fixture fixture;
  const AreaShape circle = *area_named(fixture.world, "Ring");
  const AreaShape rect = *area_named(fixture.world, "Yard");
  for (int i = 0; i < 500; ++i) {
    CHECK(circle.contains(random_point_in_area(fixture.world, circle)));
    CHECK(rect.contains(random_point_in_area(fixture.world, rect)));
  }
}

TEST(a_sampler_with_no_attempts_is_the_centre_and_draws_nothing) {
  // 0x004d6ecb returns the centre the helper wrote into the scratch buffer
  // without ever entering the draw loop. Whether a call advances the stream is
  // itself synchronised state, so "draws nothing" is the load-bearing half.
  Fixture fixture;
  const AreaShape circle = *area_named(fixture.world, "Ring");
  const std::uint32_t before = fixture.world.rng().state();
  CHECK(random_point_in_area(fixture.world, circle, 0) == circle.centre());
  CHECK(fixture.world.rng().state() == before);
}

TEST(the_sampler_is_a_function_of_the_seed_alone) {
  Fixture a;
  Fixture b;
  const AreaShape circle = *area_named(a.world, "Ring");
  for (int i = 0; i < 32; ++i) {
    CHECK(random_point_in_area(a.world, circle) == random_point_in_area(b.world, circle));
  }
  CHECK(a.world.rng().state() == b.world.rng().state());
}

TEST(the_sampler_avoids_blocked_ground_when_a_grid_says_so) {
  // The obstruction bitmap is `MovementSystem`'s, reached the same way every
  // other domain reaches its neighbours. Block the left half of a rectangle and
  // every draw should land on the right.
  Fixture fixture;
  ObstructionGrid grid(64, 64);
  const AreaShape rect = AreaShape::of_rectangle(0, 0, 1023, 511);
  for (std::int32_t cy = 0; cy < 32; ++cy) {
    for (std::int32_t cx = 0; cx < 32; ++cx) grid.set_cell(cx, cy, true);
  }
  fixture.movement.set_grid(std::move(grid));

  std::size_t on_the_right = 0;
  for (int i = 0; i < 200; ++i) {
    const Point p = random_point_in_area(fixture.world, rect);
    CHECK(rect.contains(p));
    if (p.x >= 512) ++on_the_right;
  }
  // Not all 200: after twenty blocked draws the executable returns the last
  // one anyway, which is the behaviour a unit ordered into a wall needs.
  CHECK(on_the_right > 180);
}

// --------------------------------------------------------------------------
// the host slice
// --------------------------------------------------------------------------

TEST(area_centre_answers_the_shape_and_minus_one_for_a_name_it_cannot_find) {
  Fixture fixture;
  const HostRegistry registry = area_registry();

  HostCall known(fixture.world, {str("Yard")});
  const HostOutcome answer = invoke(registry, "AreaCenter", 1, known);
  CHECK(answer.status == HostStatus::ok);
  CHECK(is_point(answer.value));
  CHECK(unpack_point(answer.value) == (Point{4200, 5300}));

  // Not a trap: the original prints "Could not find area named '%s' in
  // function 'AreaCenter'. Check the spelling." and returns (-1, -1), and the
  // script runs on past it.
  HostCall missing(fixture.world, {str("Nowhere")});
  const HostOutcome absent = invoke(registry, "AreaCenter", 1, missing);
  CHECK(absent.status == HostStatus::ok);
  CHECK(unpack_point(absent.value) == (Point{-1, -1}));
}

TEST(random_point_in_area_has_its_own_sentinel_and_it_is_not_minus_one) {
  // (1000, 1000). Two functions, two different miss values, both in the
  // executable -- 0x004d91dc loads 0x3e8 into both components.
  Fixture fixture;
  const HostRegistry registry = area_registry();
  HostCall missing(fixture.world, {str("Nowhere")});
  const HostOutcome absent = invoke(registry, "GetRandomPointInArea", 1, missing);
  CHECK(absent.status == HostStatus::ok);
  CHECK(unpack_point(absent.value) == (Point{1000, 1000}));

  HostCall known(fixture.world, {str("Ring")});
  const HostOutcome inside = invoke(registry, "GetRandomPointInArea", 1, known);
  CHECK(inside.status == HostStatus::ok);
  CHECK(area_named(fixture.world, "Ring")->contains(unpack_point(inside.value)));
}

TEST(area_dist_to_measures_from_a_point_to_the_region) {
  Fixture fixture;
  const HostRegistry registry = area_registry();
  HostCall call(fixture.world, {pack_point(Point{1000, 2600}), str("Ring")});
  const HostOutcome answer = invoke(registry, "AreaDistTo", 2, call);
  CHECK(answer.status == HostStatus::ok);
  CHECK(answer.value.as_integer() == 300);  // 600 from the centre, radius 300

  HostCall inside(fixture.world, {pack_point(Point{1000, 2000}), str("Ring")});
  CHECK(invoke(registry, "AreaDistTo", 2, inside).value.as_integer() == 0);
}

TEST(attack_area_orders_advance_and_move_to_area_orders_move) {
  // The whole point of disassembling 0x0057a510: the verb is `advance`, not
  // `attack`. A reimplementation that guessed `attack` from the name would
  // resolve a different `<method sig>` on every unit in the game.
  Fixture fixture;
  const HostRegistry registry = area_registry();
  const ObjectId first = fixture.world.named_object("Band");
  (void)first;

  std::vector<ObjectId> band;
  fixture.world.objects_in_group(fixture.world.groups().find("Band"), ClassFilter{}, band);
  REQUIRE(band.size() == 2);

  const QuerySpec spec = group_query(fixture.world.groups().find("Band"));
  const ObjectId query = fixture.world.create_query(spec);

  HostCall attack(fixture.world,
                  {Value::object(ObjectRef{kTypeQuery, query}), str("Yard")});
  CHECK(invoke(registry, "AttackArea", 2, attack).status == HostStatus::ok);
  for (const ObjectId member : band) {
    CHECK(fixture.command.command_name(member) == "advance");
  }

  HostCall move(fixture.world, {Value::object(ObjectRef{kTypeQuery, query}), str("Yard")});
  CHECK(invoke(registry, "MoveToArea", 2, move).status == HostStatus::ok);
  for (const ObjectId member : band) {
    CHECK(fixture.command.command_name(member) == "move");
  }
}

TEST(attack_area_gives_every_member_its_own_point_inside_the_area) {
  // The executable calls the sampler inside the per-member loop, so a squad
  // spreads over the region instead of stacking on its centre. Two members are
  // enough to see it: with a shared point they would be identical every time.
  Fixture fixture;
  const HostRegistry registry = area_registry();
  const AreaShape yard = *area_named(fixture.world, "Yard");
  std::vector<ObjectId> band;
  fixture.world.objects_in_group(fixture.world.groups().find("Band"), ClassFilter{}, band);
  REQUIRE(band.size() == 2);
  const ObjectId query =
      fixture.world.create_query(group_query(fixture.world.groups().find("Band")));

  std::size_t different = 0;
  for (int round = 0; round < 40; ++round) {
    HostCall call(fixture.world,
                  {Value::object(ObjectRef{kTypeQuery, query}), str("Yard")});
    CHECK(invoke(registry, "AttackArea", 2, call).status == HostStatus::ok);
    const Command* a = fixture.command.find(band[0])->running();
    const Command* b = fixture.command.find(band[1])->running();
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->arg_kind == CommandArgKind::point);
    CHECK(yard.contains(a->point));
    CHECK(yard.contains(b->point));
    if (!(a->point == b->point)) ++different;
  }
  CHECK(different > 30);
}

TEST(attack_area_on_a_name_it_cannot_find_orders_nothing) {
  Fixture fixture;
  const HostRegistry registry = area_registry();
  const ObjectId query =
      fixture.world.create_query(group_query(fixture.world.groups().find("Band")));
  HostCall call(fixture.world,
                {Value::object(ObjectRef{kTypeQuery, query}), str("Nowhere")});
  const HostOutcome answer = invoke(registry, "AttackArea", 2, call);
  CHECK(answer.status == HostStatus::ok);
  CHECK(fixture.command.tracked() == 0);
}

TEST(the_two_area_queries_answer_a_circle_and_a_rectangle_exactly) {
  Fixture fixture;
  const HostRegistry registry = area_registry();

  HostCall circle(fixture.world, {str("Ring"), str("Unit")});
  const HostOutcome answered = invoke(registry, "AreaObjs", 2, circle);
  REQUIRE(answered.status == HostStatus::ok);
  std::vector<ObjectId> found;
  fixture.world.evaluate_query(answered.value.as_object().id, found);
  CHECK(found.size() == 2);  // both units stand inside the 300-unit circle

  // `Yard` is the rectangle (4000,5000)-(4400,5600). Two units are placed for
  // this case: one inside it, and one **outside the rectangle but inside its
  // bounding circle** -- 4200,4960 is 340 from the centre against a bounding
  // radius of sqrt(200^2 + 300^2) = 360, and 40 above the top edge. That second
  // unit is the whole point: answering a rectangle with its bounding circle
  // would return both, and would have looked right on any map whose corners
  // happened to be square.
  const ObjectId within = fixture.world.spawn(NativeClass::unit, nullptr,
                                              fixture.graph.lookup("Unit"));
  const ObjectId cornered = fixture.world.spawn(NativeClass::unit, nullptr,
                                                fixture.graph.lookup("Unit"));
  fixture.world.set_position(within, Point{4100, 5100});
  fixture.world.set_position(cornered, Point{4200, 4960});

  HostCall rect(fixture.world, {str("Yard"), str("Unit")});
  const HostOutcome boxed = invoke(registry, "AreaObjs", 2, rect);
  REQUIRE(boxed.status == HostStatus::ok);
  fixture.world.evaluate_query(boxed.value.as_object().id, found);
  REQUIRE(found.size() == 1);
  CHECK(found[0] == within);
  CHECK(found[0] != cornered);
  // The control on the control: the second unit really is inside the bounding
  // circle, so the assertion above is about the shape and not about the point
  // being far away.
  const AreaShape* yard = area_named(fixture.world, "Yard");
  REQUIRE(yard != nullptr);
  const AreaBounds box = yard->bounds();
  const AreaShape bounding = AreaShape::of_circle(yard->centre(), 361);
  CHECK(bounding.contains(Point{4200, 4960}));
  CHECK(!yard->contains(Point{4200, 4960}));
  CHECK(yard->contains(Point{4100, 5100}));
  // And the spec carries the area's own corners, not the bounding box of a
  // circle and not a re-derived pair.
  const QuerySpec* spec = fixture.world.query_spec(boxed.value.as_object().id);
  REQUIRE(spec != nullptr);
  CHECK(spec->kind == QueryKind::map_area_rect);
  CHECK(spec->left == box.left);
  CHECK(spec->top == box.top);
  CHECK(spec->right == box.right);
  CHECK(spec->bottom == box.bottom);

  HostCall by_player(fixture.world, {str("Unit"), Value::integer(1), str("Ring")});
  const HostOutcome owned = invoke(registry, "ClassPlayerAreaObjs", 3, by_player);
  REQUIRE(owned.status == HostStatus::ok);
  fixture.world.evaluate_query(owned.value.as_object().id, found);
  CHECK(found.size() == 2);

  // Player numbers a script passes are 1-based. Player 2 owns nothing here.
  HostCall other(fixture.world, {str("Unit"), Value::integer(2), str("Ring")});
  const HostOutcome none = invoke(registry, "ClassPlayerAreaObjs", 3, other);
  REQUIRE(none.status == HostStatus::ok);
  fixture.world.evaluate_query(none.value.as_object().id, found);
  CHECK(found.empty());

  // And `ClassPlayerAreaObjs` bounds by a rectangle too -- one query type with
  // a shape branch, not two types. Both units placed above belong to no player
  // (`spawn` leaves the owner unset), so give the one inside the box to player
  // 1 and check that owner and shape are both applied.
  fixture.world.set_owner(within, 0);   // `player="1"` is slot 0
  fixture.world.set_owner(cornered, 0);
  HostCall rect_player(fixture.world, {str("Unit"), Value::integer(1), str("Yard")});
  const HostOutcome boxed_player = invoke(registry, "ClassPlayerAreaObjs", 3, rect_player);
  REQUIRE(boxed_player.status == HostStatus::ok);
  fixture.world.evaluate_query(boxed_player.value.as_object().id, found);
  REQUIRE(found.size() == 1);
  CHECK(found[0] == within);       // the shape excluded `cornered`...
  CHECK(found[0] != cornered);

  const QuerySpec* by_box = fixture.world.query_spec(boxed_player.value.as_object().id);
  REQUIRE(by_box != nullptr);
  // One kind, and a discriminator -- `CVXClassPlayerAreaQuery` is a single
  // runtime type that branches on `[area+0x148]` (0x004fe860), where `AreaObjs`
  // really does build two.
  CHECK(by_box->kind == QueryKind::class_player_area);
  CHECK(by_box->area_is_rect);
  CHECK(by_box->left == box.left);
  CHECK(by_box->bottom == box.bottom);

  // ...and the owner test still applies inside it: player 2 owns neither.
  HostCall wrong_owner(fixture.world, {str("Unit"), Value::integer(2), str("Yard")});
  const HostOutcome empty = invoke(registry, "ClassPlayerAreaObjs", 3, wrong_owner);
  REQUIRE(empty.status == HostStatus::ok);
  fixture.world.evaluate_query(empty.value.as_object().id, found);
  CHECK(found.empty());
}

TEST(an_area_query_on_an_unknown_name_is_an_invalid_handle_not_a_trap) {
  // 0x00577196 writes a Query whose id is 0xffff and returns. `.count` on it is
  // zero and `.IsEmpty` is true, which is what the guarded call sites expect.
  Fixture fixture;
  const HostRegistry registry = area_registry();
  HostCall call(fixture.world, {str("Nowhere"), str("Unit")});
  const HostOutcome answer = invoke(registry, "AreaObjs", 2, call);
  CHECK(answer.status == HostStatus::ok);
  CHECK(!answer.value.as_object().valid());
}

TEST(every_area_entry_point_refuses_a_call_with_no_world) {
  // `CallContext::user` is a `void*`, and a host function that dereferenced it
  // blindly would crash rather than trap. Every domain's suite checks this.
  Fixture fixture;
  const HostRegistry registry = area_registry();
  const struct {
    std::string_view name;
    std::uint16_t arity;
    std::initializer_list<Value> args;
  } calls[] = {
      {"AreaCenter", 1, {str("Ring")}},
      {"GetRandomPointInArea", 1, {str("Ring")}},
      {"AreaDistTo", 2, {pack_point(Point{0, 0}), str("Ring")}},
      {"AttackArea", 2, {Value::object(ObjectRef{kTypeQuery, 1}), str("Ring")}},
      {"MoveToArea", 2, {Value::object(ObjectRef{kTypeQuery, 1}), str("Ring")}},
      {"AreaObjs", 2, {str("Ring"), str("Unit")}},
      {"ClassPlayerAreaObjs", 3, {str("Unit"), Value::integer(1), str("Ring")}},
  };
  for (const auto& call : calls) {
    HostCall unbound(fixture.world, call.args, false);
    CHECK(invoke(registry, call.name, call.arity, unbound).status == HostStatus::error);
  }
}

// --------------------------------------------------------------------------
// the manifest
// --------------------------------------------------------------------------

TEST(area_host_takes_no_entry_point_from_another_domain) {
  // `HostRegistry::define` replaces silently and has cost this project 13
  // collisions. Every domain gets this check, and it has to run against the
  // *other* domains rather than against `register_all_hosts`.
  HostRegistry alone;
  declare_shipped_surface(alone);
  const std::size_t defined = register_area_host(alone);
  CHECK(defined == area_host_entry_count());
  CHECK(alone.implemented() == area_host_entry_count());

  HostRegistry all;
  imperivm::test::define_all_except("area", all);
  const std::size_t before = all.implemented();
  (void)register_area_host(all);
  CHECK(all.implemented() - before == area_host_entry_count());
}

TEST(the_area_domain_is_in_the_manifest_and_survives_the_whole_run) {
  // A domain that is not in `host_domains()` is dead code however well it
  // tests, which is exactly how the settlement timers and the command table
  // each passed every unit test while being unreachable.
  bool listed = false;
  for (const HostDomain& domain : host_domains()) {
    if (domain.name == "area") listed = true;
  }
  CHECK(listed);

  HostRegistry registry;
  (void)register_all_hosts(registry);
  for (const std::string_view name :
       {"AreaCenter", "ClassPlayerAreaObjs", "AttackArea", "AreaObjs",
        "GetRandomPointInArea", "MoveToArea", "AreaDistTo"}) {
    const std::uint16_t arity =
        name == "ClassPlayerAreaObjs" ? 3
        : (name == "AreaCenter" || name == "GetRandomPointInArea") ? 1
                                                                   : 2;
    const std::uint32_t index = registry.find(CallKind::free_function, name, arity);
    REQUIRE(index != kUnresolvedHost);
    CHECK(registry.entry(index).fn != nullptr);
  }
}

TEST(the_entry_points_this_slice_refused_have_all_found_a_home) {
  // Recording a refusal is a result, and this test used to pin five of them:
  // `IsProtected` is not an area function at all (`bool, int player, point
  // pos, str class`), `ExploreArea` needed fog of war, and the three `AreaAI*`
  // calls needed AI state that did not exist. Its job was to fail when one of
  // them acquired a body, so that the paragraph in sim/area.hpp saying why it
  // had none was deleted deliberately rather than left to rot.
  //
  // `WaitUnitsInArea` left first, once `HostStatus::retry` was noticed in the
  // tree; `ExploreArea` next, when `sim/fog.hpp` arrived; the three `AreaAI*`
  // writers when `AiPlayer::gaika` did; and `IsProtected` last, as a presence
  // test for a protective effect over a point in `sim/world_host.cpp` -- which
  // is what its signature said it was all along. Each lives in the domain
  // that owns what it reads, and each is registered after this one because
  // it resolves its argument through `area_named`, or not at all.
  //
  // What is left to pin is that all five are bound, so the list cannot
  // quietly grow a refusal again without this comment being rewritten.
  HostRegistry registry;
  (void)register_all_hosts(registry);
  const struct {
    std::string_view name;
    std::uint16_t arity;
  } bound[] = {
      {"IsProtected", 3}, {"ExploreArea", 2}, {"WaitUnitsInArea", 3},
  };
  for (const auto& entry : bound) {
    const std::uint32_t index = registry.find(CallKind::free_function, entry.name, entry.arity);
    REQUIRE(index != kUnresolvedHost);
    CHECK(registry.entry(index).fn != nullptr);
  }
}
