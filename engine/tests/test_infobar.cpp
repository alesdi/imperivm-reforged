// The info bar's contents, read from the data through a running session.
//
// The bar's six stat slots are inline `.vs` in the class XML, the tags and
// the portrait are class properties, and which class is read depends on the
// selection by the executable's rule. Every one of those is exercised here
// over a synthetic session whose classes declare exactly what the retail
// ones do, so that the retail data can only differ in its values.

#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/infobar.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/session.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace script = imperivm::core::script;
using imperivm::core::ClassGraph;
using imperivm::core::Result;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) noexcept {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

/// One in-memory script, the shape `test_group.cpp` uses.
class OneScript final : public ScriptResolver {
 public:
  OneScript(std::string path, std::string text)
      : path_(std::move(path)), text_(std::move(text)) {}
  std::span<const std::byte> source(std::string_view path) override {
    return path == path_ ? bytes_of(text_) : std::span<const std::byte>{};
  }

 private:
  std::string path_;
  std::string text_;
};

/// The shipped declarations, cut down: a unit with the six retail slots, a
/// peasant, a ranged unit, a sentry, a hero, and the `Empty`, `Multi`,
/// `MultiOne`, `MultiOneRanged` and `RPeasantMulti` pseudo-classes.
ClassGraph info_graph() {
  ClassGraph graph;
  const char* docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
      R"(<class id="Unit" parent="Object" cpp_class="CVXUnit">
      <properties maxhealth="200" maxstamina="10" damage="10" armor_slash="3" max_food="20"/>
      <properties interface="thumb,unit,items"/>
      <properties icon="gameres/icons/multi.bmp"/>
      <properties display_name="Unit" display_name_plural="Units"/>
      <value0 icon="gameres/infobar/common/level ico.bmp" script="return .AsUnit.level;" rollover="Level"/>
      <value1 icon="gameres/infobar/common/attack ico.bmp" script="return .AsUnit.damage;" rollover="Damage"/>
      <value2 icon="gameres/infobar/common/stamina ico.bmp" script="return .AsUnit.stamina;" rollover="Stamina"/>
      <value3 icon="gameres/infobar/common/defense ico.bmp" script="return .AsUnit.armor_slash;" rollover="Armor"/>
      <value4 icon="gameres/infobar/common/health ico.bmp" script="return .AsUnit.health + '/' + .AsUnit.maxhealth;" rollover="Health"/>
      <value5 icon="gameres/infobar/common/food ico.bmp" script="return .AsUnit.food + '/' + .AsUnit.maxfood;" rollover="Food"/>
      <method sig="idle" vs="unit_idle.vs"/>
    </class>)",
      R"(<class id="Melee" parent="Unit" cpp_class="CVXUnit"/>)",
      R"(<class id="Ranged" parent="Unit" cpp_class="CVXUnit"><properties projectile_class="Arrow"/></class>)",
      R"(<class id="Peasant" parent="Unit" cpp_class="CVXUnit"><properties interface="thumb,unit"/></class>)",
      R"(<class id="Sentry" parent="Unit" cpp_class="CVXUnit"/>)",
      R"(<class id="Hero" parent="Unit" cpp_class="CVXHero"><properties interface="thumb,hero,items,holder"/></class>)",
      R"(<class id="RHastatus" parent="Melee" cpp_class="CVXUnit">
      <properties maxhealth="600" damage="40" armor_slash="12"/>
      <properties unit_specials="Spike armor, Combat skill"/>
      <properties icon="gameres/icons/RHastatus.bmp"/>
      <properties display_name="Hastatus" display_name_plural="Hastati"/>
    </class>)",
      R"(<class id="RArcher" parent="Ranged" cpp_class="CVXUnit"><properties icon="gameres/icons/RArcher.bmp"/></class>)",
      R"(<class id="RPeasant" parent="Peasant" cpp_class="CVXUnit"><properties icon="gameres/icons/RPeasant.bmp"/></class>)",
      R"(<class id="RSentry" parent="Sentry" cpp_class="CVXUnit"/>)",
      R"(<class id="RHero1" parent="Hero" cpp_class="CVXHero"><properties display_name="Kaeso"/></class>)",
      R"(<class id="Empty" parent="Object" cpp_class="CVXScriptObj"><properties interface="empty"/></class>)",
      R"(<class id="Multi" parent="Object" cpp_class="CVXScriptObj">
      <properties icon="gameres/icons/multi.bmp"/>
      <properties interface="thumb,unit,holder"/>
      <value0 icon="gameres/infobar/common/level ico.bmp" script="return SelAvgLevel();" rollover="Level"/>
      <value2 icon="gameres/infobar/common/health ico.bmp" script="return SelHealth() * 100 / SelMaxHealth() + '%';" rollover="Health"/>
    </class>)",
      R"(<class id="MultiOne" parent="Object" cpp_class="CVXScriptObj">
      <properties interface="thumb,unit"/>
      <value0 icon="gameres/infobar/common/level ico.bmp" script="return SelAvgLevel();" rollover="Level"/>
    </class>)",
      R"(<class id="MultiOneRanged" parent="MultiOne" cpp_class="CVXScriptObj"/>)",
      R"(<class id="RPeasantMulti" parent="MultiOne" cpp_class="CVXScriptObj"><properties interface="thumb,unit"/></class>)",
  };
  for (const char* doc : docs) graph.add(bytes_of(doc), "test_infobar.cpp");
  graph.link();
  return graph;
}

constexpr std::string_view kMap = R"(<mapobject>
	<scriptobj class="RHastatus" num="0" player="1" x="100" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RHastatus" num="1" player="1" x="200" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RArcher" num="2" player="1" x="300" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RPeasant" num="3" player="1" x="400" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RPeasant" num="4" player="1" x="500" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RSentry" num="5" player="1" x="600" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
	<scriptobj class="RHero1" num="6" player="1" x="700" y="100" flags="0xA0400002" dir.x="0" dir.y="1"/>
</mapobject>)";

constexpr std::string_view kSpecials =
    "[Spike armor]\nicon = gameres/infobar/unit specials/spike armor.bmp\nname = Spike armor\n"
    "[Combat skill]\nicon = gameres/infobar/unit specials/combat skill.bmp\nname = Combat skill\n";

struct Fixture {
  script::HostRegistry registry;
  ClassGraph graph = info_graph();
  OneScript scripts{"unit_idle.vs", "//void, Obj This\nSleep(100000);\n"};
  std::unique_ptr<GameSession> session;
  std::unique_ptr<InfoBar> bar;

  Fixture() {
    register_all_hosts(registry);
    SessionInputs inputs;
    inputs.classes = &graph;
    inputs.scripts = &scripts;
    inputs.map_objects = bytes_of(kMap);
    auto made = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(made.ok());
    session = std::move(made.value());
    session->set_local_player(0);
    (void)session->start_match(MatchOptions{});
    (void)session->start_object_scripts();
    // One turn, so that combat has enrolled everyone: `damage` and
    // `armor_slash` are answered from the combatant, not the class.
    session->advance(1, 400);
    bar = std::make_unique<InfoBar>(*session, std::span<const std::byte>{}, bytes_of(kSpecials));
  }

  void select(std::initializer_list<ObjectId> ids) {
    Selection& selection = session->selections().player(0);
    selection.clear();
    for (const ObjectId id : ids) selection.add(id);
  }
};

}  // namespace

TEST(infobar_reads_one_units_declarations) {
  Fixture f;
  f.select({1});
  const SelectionInfo info = f.bar->describe(0);
  CHECK(info.class_id == "RHastatus");
  REQUIRE(info.tags.size() == 3);
  CHECK(info.tags[0] == "thumb" && info.tags[1] == "unit" && info.tags[2] == "items");
  CHECK(info.name == "Hastatus");
  CHECK(info.icon == "gameres/icons/RHastatus.bmp");
  CHECK(info.health == 100);
  for (const std::string& c : info.complaints) std::printf("complaint: %s\n", c.c_str());
  CHECK(info.complaints.empty());
  // The six slots, each its inline script's answer against `this`.
  REQUIRE(info.values[0].present);
  CHECK(info.values[0].text == "1");
  CHECK(info.values[0].icon == "gameres/infobar/common/level ico.bmp");
  CHECK(info.values[1].present && info.values[1].text == "40");
  CHECK(info.values[3].present && info.values[3].text == "12");
  CHECK(info.values[4].present && info.values[4].text == "600/600");
  CHECK(info.values[5].present);
  // The specials, from `unit_specials` through the INI.
  REQUIRE(info.specials.size() == 2);
  CHECK(info.specials[0].icon == "gameres/infobar/unit specials/spike armor.bmp");
  CHECK(info.specials[1].text == "Combat skill");
  CHECK(info.queue.empty());
  CHECK(info.holder.empty());
  CHECK(info.items.empty());
}

TEST(infobar_shows_the_empty_class_for_no_selection) {
  Fixture f;
  f.select({});
  const SelectionInfo info = f.bar->describe(0);
  CHECK(info.class_id == "Empty");
  REQUIRE(info.tags.size() == 1);
  CHECK(info.tags[0] == "empty");
  CHECK(info.name.empty());
  CHECK(info.health == -1);
  for (const InfoSlot& slot : info.values) CHECK(!slot.present);
}

TEST(infobar_picks_the_selections_class_by_the_executables_rule) {
  Fixture f;
  World& world = f.session->world();
  // Two of one class: `MultiOne`. A ranged one first: `MultiOneRanged`.
  CHECK(selection_class(world, std::vector<ObjectId>{1, 2}) == "MultiOne");
  CHECK(selection_class(world, std::vector<ObjectId>{3, 3}) == "MultiOneRanged");
  // Two classes: `Multi`, unless a sentry leads.
  CHECK(selection_class(world, std::vector<ObjectId>{1, 3}) == "Multi");
  CHECK(selection_class(world, std::vector<ObjectId>{6, 1}) == "MultiOne");
  // A hero anywhere in front: `Multi`, whatever else is there.
  CHECK(selection_class(world, std::vector<ObjectId>{7, 7}) == "Multi");
  // Peasants of one race, and the class exists: `RPeasantMulti`.
  CHECK(selection_class(world, std::vector<ObjectId>{4, 5}) == "RPeasantMulti");
  // A dead id contributes nothing; all dead is `Empty`.
  CHECK(selection_class(world, std::vector<ObjectId>{999, 1}) == "RHastatus");
  CHECK(selection_class(world, std::vector<ObjectId>{999}) == "Empty");
}

TEST(infobar_evaluates_the_multi_classes_over_the_selection) {
  Fixture f;
  f.select({1, 3});  // a hastatus and an archer: `Multi`
  const SelectionInfo info = f.bar->describe(0);
  CHECK(info.class_id == "Multi");
  CHECK(info.complaints.empty());
  // `SelAvgLevel()` over two level-1 units.
  REQUIRE(info.values[0].present);
  CHECK(info.values[0].text == "1");
  // `SelHealth() * 100 / SelMaxHealth() + '%'`: 600 + 200 of 600 + 200.
  REQUIRE(info.values[2].present);
  CHECK(info.values[2].text == "100%");
  CHECK(!info.values[1].present);
  CHECK(info.health == -1);
  CHECK(info.selected == 2);
}

TEST(infobar_lists_a_heros_army_in_the_holder_strip) {
  Fixture f;
  World& world = f.session->world();
  HeroSystem* heroes = hero_system_of(world);
  REQUIRE(heroes != nullptr);
  REQUIRE(heroes->hero(7) != nullptr);
  // Two hastati and an archer attached, in that order; a peasant left out.
  CHECK(heroes->attach(world, 1, 7));
  CHECK(heroes->attach(world, 3, 7));
  CHECK(heroes->attach(world, 2, 7));

  // One hero: its army, without the hero, one cell per class and owner in
  // the order a class is first met, each numbered with its count.
  CHECK(holder_list(world, std::vector<ObjectId>{7}) == (std::vector<ObjectId>{1, 3, 2}));
  f.select({7});
  SelectionInfo info = f.bar->describe(0);
  REQUIRE(info.holder.size() == 2);
  CHECK(info.holder[0].icon == "gameres/icons/RHastatus.bmp");
  CHECK(info.holder[0].number == "2");
  CHECK(info.holder[0].objects == (std::vector<ObjectId>{1, 2}));
  CHECK(info.holder[0].health == 100);
  CHECK(info.holder[1].icon == "gameres/icons/RArcher.bmp");
  CHECK(info.holder[1].number == "1");
  CHECK(info.holder[1].objects == (std::vector<ObjectId>{3}));
  CHECK(info.holder[0].key != info.holder[1].key);

  // The hero with a peasant: a `Multi` selection lists the selected objects
  // and then the army, and the army folds into the hero's cell, whose number
  // counts the army and whose click selects the hero alone.
  CHECK(holder_list(world, std::vector<ObjectId>{7, 4}) == (std::vector<ObjectId>{7, 4, 1, 3, 2}));
  f.select({7, 4});
  info = f.bar->describe(0);
  REQUIRE(info.holder.size() == 2);
  CHECK(info.holder[0].object == 7);
  CHECK(info.holder[0].number == "3");
  CHECK(info.holder[0].objects == (std::vector<ObjectId>{7}));
  CHECK(info.holder[1].objects == (std::vector<ObjectId>{4}));

  // Not a `Multi` selection -- two of one class, or only sentries -- lists
  // nothing; nor does a lone unit with no holder.
  CHECK(holder_list(world, std::vector<ObjectId>{1, 2}).empty());
  CHECK(holder_list(world, std::vector<ObjectId>{6}).empty());
  CHECK(holder_list(world, std::vector<ObjectId>{}).empty());
  // Two classes: both listed, one cell each.
  CHECK(holder_list(world, std::vector<ObjectId>{1, 4}) == (std::vector<ObjectId>{1, 4}));

  // A hero with no army: a cell of its own in a multiple selection, with no
  // number.
  CHECK(heroes->detach_army(world, 7) == 3);
  f.select({7, 4});
  info = f.bar->describe(0);
  REQUIRE(info.holder.size() == 2);
  CHECK(info.holder[0].number.empty());
}
