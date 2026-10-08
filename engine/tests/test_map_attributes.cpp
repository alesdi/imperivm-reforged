// The authored per-object attributes the map carries and the simulation
// reads at load: `Level`, `hs<Skill>`, `slot<n>`, `amount`, `stamina`,
// `display_name`. Every shipped hero started at level 1 with no skills and
// no items, every veteran at level 1 and every loaded trader empty until
// this was read off the loaders (0x005dd3a0, 0x00530810, 0x005af280,
// 0x005ec690); see `GameSession::Impl::apply_authored_attributes` and
// `World::spawn_map_object`.

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/infobar.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

ClassGraph attribute_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)"),
            "test_map_attributes.cpp");
  graph.add(bytes_of(R"(<class id="Unit" parent="Object" cpp_class="CVXUnit">
      <properties sight="400" maxhealth="200" maxstamina="10" damage="40" range="40"
                  radius="10" selection_radius="10" inventory_size="2"
                  display_name="Legionary"/>
    </class>)"),
            "test_map_attributes.cpp");
  graph.add(bytes_of(R"(<class id="Hero" parent="Unit" cpp_class="CVXHero">
      <properties maxhealth="1000" max_army="50" inventory_size="2" display_name="Hero"/>
      <properties HeroSkills="Administration, Team attack"/>
    </class>)"),
            "test_map_attributes.cpp");
  graph.add(bytes_of(R"(<class id="Trader" parent="Unit" cpp_class="CVXWagon"/>)"),
            "test_map_attributes.cpp");
  graph.add(bytes_of(R"(<class id="Sentry" parent="Unit" cpp_class="CVXUnit"/>)"),
            "test_map_attributes.cpp");
  graph.add(bytes_of(R"(<class id="Hut" parent="Object" cpp_class="CVXBuilding">
      <properties maxhealth="500" maxstamina="0" radius="30"/>
    </class>)"),
            "test_map_attributes.cpp");
  graph.link();
  return graph;
}

constexpr std::string_view kItems = R"(<items>
  <item id="Snake skin" level="0" name="Snake skin" important="no">
    <bonus health="0" damage="4" armor_slash="0" armor_pierce="0" level="0" experience="0"/>
  </item>
  <item id="King's Belt" level="0" name="King's belt" important="yes">
    <bonus health="600" damage="0" armor_slash="10" armor_pierce="10" level="0" experience="0"/>
  </item>
</items>)";

/// The shipped shape, attributes and all: a hero with two skills, two items
/// and a name; a level-9 legionary; a loaded trader; a building whose
/// `stamina` disagrees with its class's `maxstamina`; a unit with an item
/// the catalogue does not know.
constexpr std::string_view kMap = R"(<mapobject>
	<scriptobj
		class="Hero"
		num="0"
	hsTeamAttack="3"
	hsAdministration="2"
	display_name="Aeneas"
	Level="11"
	UnitFlags="262144"
	player="1"
	healthperc="100"
	stamina="10"
	inventorysize="2"
	slot0="Snake skin"
	slot1="King's Belt"
	x="1000"
	y="1000"
	flags="0x81400001"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="Unit"
		num="1"
	Level="9"
	UnitFlags="262144"
	player="1"
	healthperc="100"
	stamina="10"
	inventorysize="0"
	x="1100"
	y="1000"
	flags="0x80400001"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="Trader"
		num="2"
	amount="2000"
	Level="0"
	UnitFlags="262144"
	player="1"
	healthperc="100"
	stamina="10"
	inventorysize="0"
	x="1200"
	y="1000"
	flags="0x80400001"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="Hut"
		num="3"
	player="1"
	healthperc="100"
	stamina="20"
	inventorysize="0"
	x="2000"
	y="2000"
	flags="0x80800001"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="Unit"
		num="4"
	Level="0"
	UnitFlags="262144"
	player="1"
	healthperc="100"
	stamina="10"
	inventorysize="1"
	slot0="Crown of nothing"
	x="1300"
	y="1000"
	flags="0x80400001"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="Sentry"
		num="5"
	Level="0"
	UnitFlags="0"
	player="1"
	healthperc="100"
	stamina="10"
	inventorysize="0"
	x="1400"
	y="1000"
	flags="0x80400001"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="Sentry"
		num="6"
	Level="0"
	UnitFlags="262144"
	player="1"
	healthperc="100"
	stamina="10"
	inventorysize="0"
	x="1500"
	y="1000"
	flags="0x80400001"
	dir.x="0"
	dir.y="1"/>
</mapobject>
)";

struct Bench {
  script::HostRegistry registry;
  ClassGraph graph = attribute_graph();
  std::unique_ptr<GameSession> session;

  Bench() {
    (void)register_all_hosts(registry);
    SessionInputs inputs;
    inputs.classes = &graph;
    inputs.map_objects = bytes_of(kMap);
    inputs.items = bytes_of(kItems);
    auto made = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(made.ok());
    session = std::move(made.value());
  }

  ObjectId object(std::size_t authored) const {
    return session->populated().object_ids.at(authored);
  }
  HeroSystem& heroes() { return *hero_system_of(session->world()); }
};

}  // namespace

TEST(the_maps_level_skills_and_items_land_on_the_hero) {
  Bench b;
  const ObjectId aeneas = b.object(0);
  REQUIRE(aeneas != kNoObject);
  HeroSystem& heroes = b.heroes();
  // `Level="11"` is 0-based: the loader sets the experience to the twelfth
  // threshold, and `level_for_experience` of that is 12.
  CHECK(heroes.inherent_level(aeneas) == 12);
  CHECK(heroes.level(aeneas) == 12);
  CHECK(heroes.experience(aeneas) == 0);
  CHECK(heroes.skill(aeneas, HeroSkill::team_attack) == 3);
  CHECK(heroes.skill(aeneas, HeroSkill::administration) == 2);
  CHECK(heroes.skill(aeneas, HeroSkill::healing) == 0);
  // One point per level (0x0052da80), capped by ten per offered skill:
  // min(20, 12) - 5 spent.
  CHECK(heroes.available_skill_points(aeneas) == 7);
  CHECK(heroes.items().count_for(aeneas) == 2);
  CHECK(heroes.items().has_type(aeneas, "Snake skin"));
  CHECK(heroes.items().has_type(aeneas, "King's Belt"));

  const WorldObject* slot = b.session->world().find(aeneas);
  REQUIRE(slot != nullptr);
  CHECK(slot->display_name == "Aeneas");
  CHECK(slot->state.stamina == 10);
}

TEST(a_veteran_unit_a_loaded_trader_and_a_building_take_their_authored_values) {
  Bench b;
  HeroSystem& heroes = b.heroes();
  // `Level="9"` is level 10; `Level="0"` is level 1, which is where a unit
  // starts anyway.
  CHECK(heroes.inherent_level(b.object(1)) == 10);
  CHECK(heroes.inherent_level(b.object(2)) == 1);
  // `amount` is the wagon's cargo, `[obj+0x1cc]`, which `Wagon::amount` reads.
  const WorldObject* trader = b.session->world().find(b.object(2));
  REQUIRE(trader != nullptr);
  CHECK(trader->state.cargo == 2000);
  // `stamina="20"` on a building whose class says `maxstamina="0"`: the
  // loader writes the attribute (0x005af4e9), which is why the dumps show 20.
  const WorldObject* hut = b.session->world().find(b.object(3));
  REQUIRE(hut != nullptr);
  CHECK(hut->state.stamina == 20);
  CHECK(hut->display_name.empty());
  // An item the catalogue does not know is refused, as `AddItem` refuses it,
  // and the unit is otherwise whole.
  CHECK(heroes.items().count_for(b.object(4)) == 0);
  CHECK(b.session->world().find(b.object(4)) != nullptr);
}

/// A sentry's own word is replaced, not merged. The `CVXUnit` constructor
/// gives every `Sentry` heir `UNITFLAG_NOAI` and the minimap bit (0x005d3323,
/// `World::allocate`), and the unit loader then stores the map's `UnitFlags`
/// over the whole of `[unit+0x194]` (0x005dd6a9) -- so a sentry the map places
/// has exactly the bits its map gives it, and one `Place` makes keeps both.
TEST(a_map_placed_sentry_takes_its_maps_unit_flags_over_the_constructors) {
  Bench b;
  World& world = b.session->world();
  const WorldObject* bare = world.find(b.object(5));
  const WorldObject* quiet = world.find(b.object(6));
  REQUIRE(bare != nullptr);
  REQUIRE(quiet != nullptr);
  CHECK(!bare->state.flags.no_ai);
  CHECK(!bare->state.flags.on_minimap);
  CHECK(quiet->state.flags.no_ai);
  CHECK(!quiet->state.flags.on_minimap);
  // And one a script places is the constructor's.
  const ObjectId placed = world.spawn_of_class(b.graph.find("Sentry"));
  REQUIRE(placed != kNoObject);
  CHECK(world.find(placed)->state.flags.no_ai);
  CHECK(world.find(placed)->state.flags.on_minimap);
}

TEST(the_authored_values_survive_a_save) {
  Bench b;
  const auto saved = b.session->save();
  REQUIRE(saved.ok());
  Bench again;
  REQUIRE(again.session->load(saved.value()).ok());
  const ObjectId aeneas = again.object(0);
  HeroSystem& heroes = again.heroes();
  CHECK(heroes.inherent_level(aeneas) == 12);
  CHECK(heroes.skill(aeneas, HeroSkill::team_attack) == 3);
  CHECK(heroes.available_skill_points(aeneas) == 7);
  CHECK(heroes.items().count_for(aeneas) == 2);
  const WorldObject* slot = again.session->world().find(aeneas);
  REQUIRE(slot != nullptr);
  CHECK(slot->display_name == "Aeneas");
  CHECK(again.session->world().find(again.object(2))->state.cargo == 2000);
  CHECK(again.session->world().find(again.object(3))->state.stamina == 20);
}

TEST(the_info_bar_shows_the_objects_own_name_over_the_classs) {
  Bench b;
  const ObjectId aeneas = b.object(0);
  b.session->set_local_player(0);
  b.session->selections().player(0).add(aeneas);
  InfoBar bar(*b.session, {}, {});
  CHECK(bar.describe(0).name == "Aeneas");
  b.session->selections().player(0).clear();
  b.session->selections().player(0).add(b.object(1));
  CHECK(bar.describe(0).name == "Legionary");
}
