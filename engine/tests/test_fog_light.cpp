// The fog manager's light grid. See include/imperivm/core/sim/fog_light.hpp.
//
// Synthetic throughout: every number here is one the header reads off the
// executable, and the world is three classes and a handful of objects.

#include <string>
#include <string_view>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/fog_light.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
using imperivm::core::ClassGraph;
using imperivm::core::NativeClass;

namespace {

constexpr Point pt(std::int32_t x, std::int32_t y) noexcept { return Point{x, y}; }

/// A scout (sight 400, over the coarse pass's 362), a mole (sight 100, the
/// smallest `SetSight` admits), an animal, and a building with a sight.
struct Bench {
  ClassGraph graph;
  World world;
  ExplorationMap map;
  FogLight light;
  FogLight::Setup setup;

  Bench() {
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
        R"(<class id="Scout" parent="Object" cpp_class="CVXUnit">
             <properties sight="400" maxhealth="80"/></class>)",
        R"(<class id="Mole" parent="Object" cpp_class="CVXUnit">
             <properties sight="100" maxhealth="80"/></class>)",
        R"(<class id="Animal" parent="Object" cpp_class="CVXUnit">
             <properties sight="100" maxhealth="80"/></class>)",
        R"(<class id="Deer" parent="Animal" cpp_class="CVXUnit">
             <properties sight="100" maxhealth="80"/></class>)",
        R"(<class id="Tower" parent="Object" cpp_class="CVXBuilding">
             <properties sight="600" maxhealth="500"/></class>)",
    };
    const char* names[] = {"object.sc.xml", "scout.sc.xml", "mole.sc.xml",
                           "animal.sc.xml", "deer.sc.xml",   "tower.sc.xml"};
    for (int i = 0; i < 6; ++i) {
      REQUIRE(graph.add(bytes(docs[i]), names[i]).ok());
    }
    graph.link();
    world.set_class_graph(&graph);
    map.resize(8192);
    light.resize(8192);
  }

  static std::span<const std::byte> bytes(std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                      text.size()};
  }

  ObjectId place(const char* cls, Point at, PlayerId owner, NativeClass kind = NativeClass::unit) {
    const ObjectId id = world.spawn(kind, nullptr, graph.find(cls));
    CHECK(world.set_owner(id, owner));
    CHECK(world.set_position(id, at));
    CHECK(world.set_health(id, 50));
    return id;
  }

  /// The whole map, snapped.
  void snap() {
    light.rebuild(world, &map, 0, setup, FogLight::Rect{0, 0, 8191, 8191}, /*snap=*/true);
  }
};

}  // namespace

// --------------------------------------------------------------------------
// the rebuild
// --------------------------------------------------------------------------

TEST(fog_light_a_source_lights_a_circle_with_a_thirty_two_unit_ramp_over_the_floor) {
  Bench b;
  b.map.explore_all();
  // A scout at a cell corner, so the distances below are exact.
  b.place("Scout", pt(4096, 4096), 0);
  b.snap();

  // The floor everywhere explored and unlit.
  CHECK(b.light.word_at(pt(100, 100)) == FogLight::kFloor);
  // 0x3e00 inside `s - 32 = 368`: the cell at (4096 + 352, 4096) is 352 from
  // the source at its corner.
  CHECK(b.light.word_at(pt(4096, 4096)) == FogLight::kLitFull);
  CHECK(b.light.word_at(pt(4096 + 352, 4096)) == FogLight::kLitFull);
  // The ramp: the corner at +384 is 384 away, `d2 = 147456`; `outer2 =
  // 160000`, `inner2 = 135424`, `scale = 7680 / 24576 = 0.3125`, so
  // `0x2000 + trunc(12544 * 0.3125) = 0x2000 + 3920`.
  CHECK(b.light.word_at(pt(4096 + 384, 4096)) == FogLight::kFloor + 3920);
  // The corner at +400 is on the edge: `outer2 - d2 = 0`, the floor exactly.
  CHECK(b.light.word_at(pt(4096 + 400, 4096)) == FogLight::kFloor);
  // Beyond it, the floor.
  CHECK(b.light.word_at(pt(4096 + 416, 4096)) == FogLight::kFloor);
  // Evaluated at the corner, not the centre: the cell whose corner is 368
  // away but whose centre is farther is lit in full.
  CHECK(b.light.word_at(pt(4096 + 368, 4096)) == FogLight::kLitFull);

  // A second source overlapping the ramp only raises it: from 750 out, the
  // corners at 384 and 400 are 366 and 350 away, inside its 368.
  b.place("Scout", pt(4096 + 750, 4096), 0);
  b.snap();
  CHECK(b.light.word_at(pt(4096 + 384, 4096)) == FogLight::kLitFull);
  CHECK(b.light.word_at(pt(4096 + 400, 4096)) == FogLight::kLitFull);
}

TEST(fog_light_who_lights_the_grid_and_who_does_not) {
  Bench b;
  b.map.explore_all();
  // The local player is 0. Player 1 shares its view with nobody; player 2
  // is one player 0 shares its view with.
  b.world.players().set(0, 2, Relation::share_view, true);
  b.place("Scout", pt(1000, 1000), 1);
  b.place("Scout", pt(3000, 3000), 2);
  // A building with a sight lights like a unit.
  b.place("Tower", pt(5000, 5000), 0, NativeClass::building);
  // A dead scout, an unspawned one, and one with no sight.
  const ObjectId dead = b.place("Scout", pt(7000, 1000), 0);
  REQUIRE(b.world.set_health(dead, 0));
  const ObjectId ghost = b.place("Scout", pt(1000, 7000), 0);
  b.world.find(ghost)->state.flags.unspawned = true;
  const ObjectId blind = b.place("Scout", pt(7000, 7000), 0);
  b.world.find(blind)->sight = 0;
  b.snap();

  CHECK(b.light.word_at(pt(1000, 1000)) == FogLight::kFloor);      // player 1
  CHECK(b.light.word_at(pt(3000, 3000)) == FogLight::kLitFull);    // shared view
  CHECK(b.light.word_at(pt(5000, 5000)) == FogLight::kLitFull);    // the tower
  CHECK(b.light.word_at(pt(7000, 1000)) == FogLight::kFloor);      // dead
  CHECK(b.light.word_at(pt(1000, 7000)) == FogLight::kFloor);      // unspawned
  CHECK(b.light.word_at(pt(7000, 7000)) == FogLight::kFloor);      // no sight
}

TEST(fog_light_the_coarse_pass_lights_an_interior_cell_wholesale_and_drops_its_source) {
  Bench b;
  b.map.explore_all();
  // Nine scouts, one per 256-cell of a 3 x 3 block, each at its cell's
  // corner. Their *class* sight is 400 > 362, so every cell is marked and
  // the middle one, with all eight neighbours marked, is interior. Their
  // *own* sight is narrowed to 100, so what the coarse pass lights and what
  // the circles light can be told apart.
  for (std::int32_t j = 0; j < 3; ++j) {
    for (std::int32_t i = 0; i < 3; ++i) {
      const ObjectId id = b.place("Scout", pt(4096 + i * 256, 4096 + j * 256), 0);
      b.world.find(id)->sight = 100;
    }
  }
  // One more in the interior cell, its own sight widened past everything
  // around. Standing in an interior cell it is dropped from the precise
  // pass, so the widening lights nothing.
  const ObjectId middle = b.place("Scout", pt(4096 + 256 + 100, 4096 + 256 + 100), 0);
  b.world.find(middle)->sight = 1200;
  b.snap();

  // The interior 256-cell is 0x3c00 throughout -- level 30, not 31 -- where
  // no neighbour's 100 reaches: its centre is 181 from every corner scout.
  CHECK(b.light.word_at(pt(4096 + 256 + 128, 4096 + 256 + 128)) == FogLight::kLitCoarse);
  CHECK(b.light.word_at(pt(4096 + 256 + 16, 4096 + 256 + 16)) == FogLight::kLitCoarse);
  // ...except where a neighbour's precise circle reaches in and lifts it:
  // the cell whose corner is 16 from the right-hand scout.
  CHECK(b.light.word_at(pt(4096 + 512 - 16, 4096 + 256)) == FogLight::kLitFull);
  // The two scouts standing in the interior cell lit nothing of their own:
  // 900 to the right of the wide one is 750 from the nearest other scout.
  CHECK(b.light.word_at(pt(4096 + 256 + 100 + 900, 4096 + 256 + 100)) == FogLight::kFloor);
  CHECK(b.light.word_at(pt(4096 + 256 + 100 + 200, 4096 + 256 + 100)) == FogLight::kFloor);
  // An edge cell of the block is not interior: its own scout lights its
  // corner in full, and the rest of it stays at the floor.
  CHECK(b.light.word_at(pt(4096, 4096)) == FogLight::kLitFull);
  CHECK(b.light.word_at(pt(4096 + 128, 4096 + 128)) == FogLight::kFloor);
}

TEST(fog_light_the_exploration_map_caps_the_light) {
  Bench b;
  // A scout at the corner of 1024-cell (4,4), its 400 exploring cell (4,4)
  // partially: the rim runs from 352 to 448 out.
  b.place("Scout", pt(4096, 4096), 0);
  b.map.explore_circle(pt(4096, 4096), 400, 0);
  REQUIRE(b.map.state(4, 4, 0) == ExplorationMap::State::partial);
  b.snap();

  // Never seen: black.
  CHECK(b.light.word_at(pt(100, 100)) == 0);
  CHECK(b.light.target_at(pt(100, 100)) == 0);
  // Lit and inside the rim's 15s: the cap is `15 * 1024 = 0x3c00`, which is
  // where a partial cell's full light lands -- lit, and level 30 not 31.
  CHECK(b.light.word_at(pt(4096 + 32, 4096 + 32)) == FogLight::kLitCoarse);
  // On the lattice, 352 out: `(448 - 352) * 15 / 96 = 15` -- the ramp's top;
  // 384 out: `64 * 15 / 96 = 10`, a cap of `10 * 1024 = 0x2800`, under the
  // sight ramp's `0x2000 + 3920` there: capped.
  CHECK(b.map.fine_at(4 * 32 + 12, 4 * 32, 0) == 10);
  CHECK(b.light.target_at(pt(4096 + 384, 4096)) == 10 * 1024);
  CHECK(b.light.word_at(pt(4096 + 384, 4096)) == 10 * 1024);
  // 416 out: `32 * 15 / 96 = 5`, cap `0x1400`, below the floor: capped.
  CHECK(b.map.fine_at(4 * 32 + 13, 4 * 32, 0) == 5);
  CHECK(b.light.word_at(pt(4096 + 416, 4096)) == 5 * 1024);
  // Between two lattice points the cap is the blend: the 16-cell at 432 is
  // halfway from 416 (5) to 448 (0), so `2.5 * 1024 = 0xa00`.
  CHECK(b.light.word_at(pt(4096 + 432, 4096)) == 2560);
  // Beyond the rim, inside the partial cell: nibble 0, black.
  CHECK(b.light.word_at(pt(4096 + 480, 4096)) == 0);
  // A fully explored cell is left at the floor.
  b.map.explore(pt(100, 100), 0);
  b.snap();
  CHECK(b.light.word_at(pt(100, 100)) == FogLight::kFloor);
}

TEST(fog_light_the_switches) {
  Bench b;
  b.place("Scout", pt(4096, 4096), 0);
  b.map.explore(pt(4096, 4096), 0);
  // Fog of war off: the wholesale light, the objects skipped, the
  // never-seen ground still black.
  b.setup.fog_of_war = false;
  b.snap();
  CHECK(b.light.word_at(pt(4096, 4096)) == FogLight::kLitCoarse);
  CHECK(b.light.word_at(pt(100, 100)) == 0);
  // Exploration off: the floor stands where nothing has been.
  b.setup.fog_of_war = true;
  b.setup.exploration = false;
  b.snap();
  CHECK(b.light.word_at(pt(100, 100)) == FogLight::kFloor);
  CHECK(b.light.word_at(pt(4096, 4096)) == FogLight::kLitFull);
  // Both off: the grid is not touched.
  b.light.resize(8192);
  b.setup.fog_of_war = false;
  b.snap();
  CHECK(b.light.word_at(pt(4096, 4096)) == 0);
  CHECK(b.light.word_at(pt(100, 100)) == 0);
  // A local player past the eighth has no exploration pass: the floor.
  b.setup = FogLight::Setup{};
  b.light.rebuild(b.world, &b.map, 9, b.setup, FogLight::Rect{0, 0, 8191, 8191}, true);
  CHECK(b.light.word_at(pt(100, 100)) == FogLight::kFloor);
}

// --------------------------------------------------------------------------
// the fade
// --------------------------------------------------------------------------

TEST(fog_light_the_displayed_grid_slides_to_the_target_in_four_ticks) {
  Bench b;
  b.map.explore_all();
  const FogLight::Rect all{0, 0, 8191, 8191};
  b.snap();
  CHECK(b.light.word_at(pt(4096, 4096)) == FogLight::kFloor);

  // A scout appears. Three ticks pass without a rebuild; the fourth
  // rebuilds into the delta and takes the first step.
  b.place("Scout", pt(4096, 4096), 0);
  for (int i = 0; i < 3; ++i) CHECK(!b.light.tick(b.world, &b.map, 0, b.setup, all));
  CHECK(b.light.word_at(pt(4096, 4096)) == FogLight::kFloor);
  CHECK(b.light.tick(b.world, &b.map, 0, b.setup, all));
  // `(0x3e00 - 0x2000) / 4 = 0x780` a tick.
  CHECK(b.light.target_at(pt(4096, 4096)) == FogLight::kLitFull);
  CHECK(b.light.word_at(pt(4096, 4096)) == FogLight::kFloor + 0x780);
  CHECK(!b.light.tick(b.world, &b.map, 0, b.setup, all));
  CHECK(b.light.word_at(pt(4096, 4096)) == FogLight::kFloor + 2 * 0x780);
  CHECK(!b.light.tick(b.world, &b.map, 0, b.setup, all));
  CHECK(!b.light.tick(b.world, &b.map, 0, b.setup, all));
  CHECK(b.light.word_at(pt(4096, 4096)) == FogLight::kLitFull);

  // The scout leaves: down again, symmetric.
  REQUIRE(b.world.set_position(b.world.objects().front().id, pt(100, 100)));
  CHECK(b.light.tick(b.world, &b.map, 0, b.setup, all));
  CHECK(b.light.word_at(pt(4096, 4096)) == FogLight::kLitFull - 0x780);
  for (int i = 0; i < 3; ++i) CHECK(!b.light.tick(b.world, &b.map, 0, b.setup, all));
  CHECK(b.light.word_at(pt(4096, 4096)) == FogLight::kFloor);

  // The period.
  CHECK(FogLight::period_ms(1000) == 100);
  CHECK(FogLight::period_ms(2000) == 50);
  CHECK(FogLight::period_ms(500) == 100);
  CHECK(FogLight::period_ms(0) == 100);
}

TEST(fog_light_a_move_snaps_the_strips_it_exposes) {
  Bench b;
  b.map.explore_all();
  b.place("Scout", pt(6000, 6000), 0);
  // A view around the origin: grown by 2048, it reaches 3071.
  const FogLight::Rect near{0, 0, 1023, 1023};
  b.light.show(b.world, &b.map, 0, b.setup, near);
  CHECK(b.light.word_at(pt(3000, 3000)) == FogLight::kFloor);
  // Beyond the grown rect nothing was computed: still zero.
  CHECK(b.light.word_at(pt(6000, 6000)) == 0);
  // The camera jumps to the scout: the exposed strips are snapped, not
  // faded -- the lit cell is lit at once.
  const FogLight::Rect far{5500, 5500, 6500, 6500};
  b.light.show(b.world, &b.map, 0, b.setup, far);
  CHECK(b.light.word_at(pt(6000, 6000)) == FogLight::kLitFull);
  CHECK(b.light.word_at(pt(4000, 4000)) == FogLight::kFloor);
  // Showing the same rect again touches nothing.
  b.light.show(b.world, &b.map, 0, b.setup, far);
  CHECK(b.light.word_at(pt(6000, 6000)) == FogLight::kLitFull);
}

// --------------------------------------------------------------------------
// the draw and the hide test
// --------------------------------------------------------------------------

TEST(fog_light_the_darkening_factor_is_sixty_eight_l_over_thirty_one) {
  CHECK(FogLight::factor_of(FogLight::kLitFull) == 32);
  CHECK(FogLight::factor_of(FogLight::kLitCoarse) == 32);
  CHECK(FogLight::factor_of(0x3dff) == 32);
  CHECK(FogLight::factor_of(0x3a01) == 32);
  // 0x3a00 itself is not above the threshold: `L = 14`, 30/32.
  CHECK(FogLight::factor_of(0x3a00) == 30);
  // The floor: `L = 8`, `68 * 8 / 31 = 17`.
  CHECK(FogLight::factor_of(FogLight::kFloor) == 17);
  CHECK(FogLight::factor_of(0x2fff) == 24);
  CHECK(FogLight::factor_of(0x400) == 2);
  CHECK(FogLight::factor_of(0x3ff) == 0);
  CHECK(FogLight::factor_of(0) == 0);
}

TEST(fog_light_the_hide_test_is_for_units_and_buildings_are_never_hidden) {
  Bench b;
  b.map.explore_all();
  b.place("Scout", pt(4096, 4096), 0);
  b.world.players().set(3, 0, Relation::share_view, true);
  // An enemy scout in the light, one on the floor, one on the ramp.
  const ObjectId seen = b.place("Scout", pt(4096 + 100, 4096), 1);
  const ObjectId unseen = b.place("Scout", pt(1000, 1000), 1);
  const ObjectId rim = b.place("Scout", pt(4096 + 390, 4096), 1);
  // An enemy building on the floor, an enemy deer on the floor, a unit
  // whose owner shares its view, and a hidden unit in the light.
  const ObjectId house = b.place("Tower", pt(1000, 2000), 1, NativeClass::building);
  const ObjectId deer = b.place("Deer", pt(2000, 2000), 1);
  const ObjectId friendly = b.place("Scout", pt(3000, 1000), 3);
  const ObjectId spy = b.place("Scout", pt(4096 + 50, 4096 + 50), 1);
  b.world.find(spy)->state.flags.hidden = true;
  b.snap();

  const auto hides = [&](ObjectId id) {
    return b.light.hides(b.world, *b.world.find(id), 0, &b.map, b.setup);
  };
  CHECK(!hides(seen));
  CHECK(hides(unseen));
  // 390 out: the ramp there is `0x2000 + trunc((160000 - 152100) * 0.3125)
  // = 0x2000 + 2468 = 0x29a4`, sampled between the corners at 384 and 400
  // -- under 0x3000 either way. Hidden, though the ground is lit a little.
  CHECK(b.light.sample(pt(4096 + 390, 4096)) > FogLight::kFloor);
  CHECK(hides(rim));
  CHECK(!hides(house));
  CHECK(!hides(deer));
  CHECK(!hides(friendly));
  CHECK(hides(spy));
  // The deer on never-seen ground is hidden like any unit.
  ExplorationMap dark;
  dark.resize(8192);
  CHECK(b.light.hides(b.world, *b.world.find(deer), 0, &dark, b.setup));
  // Fog of war off hides nothing.
  FogLight::Setup off;
  off.fog_of_war = false;
  CHECK(!b.light.hides(b.world, *b.world.find(unseen), 0, &b.map, off));
  // Off the grid is hidden.
  FogLight none;
  CHECK(none.hides(b.world, *b.world.find(seen), 0, &b.map, b.setup));
}
