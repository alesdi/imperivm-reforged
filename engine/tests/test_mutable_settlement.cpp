// A `Mutable` settlement made into a race's. See sim/mutable_settlement.hpp.

#include "imperivm/core/sim/mutable_settlement.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/world/map.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// The two placeholders, a race's town and village for two races, and the
/// buildings a template lays around them.
constexpr std::string_view kClasses[] = {
    R"(<class id="Building" cpp_class="CVXBuilding"><properties maxhealth="1000"/></class>)",
    R"(<class id="BaseTownhall" parent="Building" cpp_class="CVXTownHall"><properties
         max_population="100" max_units="10000" maxhealth="5000" population="40"
         settlement_gold="2500" settlement_food="200" settlement_maxgold="100000000"
         settlement_maxfood="100000000" can_be_captured="1" produces_gold="1"/></class>)",
    R"(<class id="BaseVillage" parent="Building" cpp_class="CVXTownHall"><properties
         max_population="20" max_units="0" maxhealth="2000" settlement_maxgold="5000"
         settlement_maxfood="5000" produces_food="1"/></class>)",
    R"(<class id="MutableStronghold" parent="BaseTownhall" cpp_class="CVXTownHall">
         <properties race="Mutable" radius="1000"/></class>)",
    R"(<class id="MutableVillage" parent="BaseVillage" cpp_class="CVXTownHall">
         <properties race="Mutable"/></class>)",
    R"(<class id="MutableStrongholdOdd" parent="MutableStronghold" cpp_class="CVXTownHall"/>)",
    R"(<class id="CTownhall" parent="BaseTownhall" cpp_class="CVXTownHall"><properties race="Carthage"/></class>)",
    R"(<class id="GTownhall" parent="BaseTownhall" cpp_class="CVXTownHall"><properties race="Gaul"/></class>)",
    R"(<class id="CVillage" parent="BaseVillage" cpp_class="CVXTownHall"><properties race="Carthage"/></class>)",
    R"(<class id="GVillage" parent="BaseVillage" cpp_class="CVXTownHall"><properties race="Gaul"/></class>)",
    R"(<class id="CWall" parent="Building" cpp_class="CVXBuilding"/>)",
    R"(<class id="CBarracks" parent="Building" cpp_class="CVXBarrack"/>)",
    R"(<class id="GWall" parent="Building" cpp_class="CVXBuilding"/>)",
    R"(<class id="CHouse" parent="Building" cpp_class="CVXBuilding"/>)",
    R"(<class id="Crops" cpp_class="CVXDecor"/>)",
};

/// The template document, in the shape of `RandomMapSettlements`'
/// `map.obj.xml`: each `<settlement>` a template, its members nested. The
/// Carthaginian town's members straddle a 128-unit cell boundary so the
/// alignment is measurable.
constexpr std::string_view kTemplates = R"(<mapobject>
<settlement id="0" player="1" classoffirstbuilding="CTownhall" maxpopulation="100"
  extrasentries="0" maxgold="100000" maxfood="100000" population="60" gold="5000" food="1000"
  name="Carthaginian Town 1">
<scriptobj class="CWall" num="0" player="1" healthperc="100" x="5100" y="5000" flags="0x80800001"/>
<scriptobj class="CTownhall" num="1" player="1" healthperc="100" x="5000" y="5000" flags="0x80800001"/>
<scriptobj class="CBarracks" num="2" player="1" healthperc="100" x="5000" y="5200" flags="0x80800001"/>
<scriptobj class="Crops" num="3" x="5200" y="5200" flags="0x80000000"/>
</settlement>
<settlement id="1" player="1" classoffirstbuilding="GTownhall" maxpopulation="100"
  extrasentries="0" maxgold="100000" maxfood="100000" population="60" gold="5000" food="1000"
  name="Gaul Town 1">
<scriptobj class="GTownhall" num="4" player="1" healthperc="100" x="9000" y="9000" flags="0x80800001"/>
<scriptobj class="GWall" num="5" player="1" healthperc="100" x="9100" y="9000" flags="0x80800001"/>
</settlement>
<settlement id="2" player="1" classoffirstbuilding="CVillage" maxpopulation="70"
  extrasentries="0" maxgold="5000" maxfood="5000" population="30" gold="0" food="100"
  name="Carthaginian Village 1">
<scriptobj class="CVillage" num="6" player="1" healthperc="100" x="12000" y="12000" flags="0x80800001"/>
<scriptobj class="CHouse" num="7" player="1" healthperc="100" x="12100" y="12100" flags="0x80800001"/>
</settlement>
<settlement id="3" player="1" classoffirstbuilding="GVillage" maxpopulation="70"
  extrasentries="0" maxgold="5000" maxfood="5000" population="30" gold="0" food="100"
  name="Gaul Village 1">
<scriptobj class="GVillage" num="8" player="1" healthperc="100" x="14000" y="14000" flags="0x80800001"/>
</settlement>
<settlement id="4" player="1" classoffirstbuilding="CTownhall" maxpopulation="100"
  extrasentries="0" maxgold="100000" maxfood="100000" population="60" gold="5000" food="1000"
  name="Carthaginian Town 2">
</settlement>
<settlement id="5" player="1" classoffirstbuilding="ETownhall" maxpopulation="100"
  extrasentries="0" maxgold="100000" maxfood="100000" population="60" gold="5000" food="1000"
  name="Egyptian Town 1">
<scriptobj class="ETownhall" num="9" player="1" healthperc="100" x="16000" y="16000" flags="0x80800001"/>
<scriptobj class="CWall" num="10" player="1" healthperc="100" x="16100" y="16000" flags="0x80800001"/>
</settlement>
</mapobject>)";

/// A skirmish map: player 1's stronghold and village as placeholders, a
/// neutral placeholder village, a real Gaul town nobody touches, and the
/// alias the mission script reads the stronghold by.
constexpr std::string_view kMap = R"(<mapobject>
<settlement id="0" player="1" classoffirstbuilding="MutableStronghold" name="S_Mine"
  maxpopulation="88" population="12" gold="999999" food="50" maxgold="100000000" maxfood="100000000">
<scriptobj class="MutableStronghold" num="0" player="1" healthperc="100" x="2000" y="2000" flags="0x80800001"/>
</settlement>
<settlement id="1" player="15" classoffirstbuilding="MutableVillage" name="Camp"
  maxpopulation="70" population="30" gold="0" food="100" maxgold="5000" maxfood="5000">
<scriptobj class="MutableVillage" num="1" player="15" healthperc="100" x="2650" y="2150" flags="0x80800001"/>
</settlement>
<settlement id="2" player="2" classoffirstbuilding="GTownhall" name="Theirs"
  maxpopulation="100" population="60" gold="5000" food="1000" maxgold="100000" maxfood="100000">
<scriptobj class="GTownhall" num="2" player="2" healthperc="100" x="7000" y="7000" flags="0x80800001"/>
</settlement>
<settlement id="3" player="1" classoffirstbuilding="MutableVillage" name="Farm"
  maxpopulation="70" population="30" gold="0" food="100" maxgold="5000" maxfood="5000">
<scriptobj class="MutableVillage" num="3" player="1" healthperc="100" x="1000" y="3000" flags="0x80800001"/>
</settlement>
<settlement id="4" player="1" classoffirstbuilding="MutableStrongholdOdd" name="Odd"
  maxpopulation="100" population="60" gold="5000" food="1000" maxgold="100000" maxfood="100000">
<scriptobj class="MutableStrongholdOdd" num="4" player="1" healthperc="100" x="7000" y="1000" flags="0x80800001"/>
</settlement>
<settlement id="5" player="16" classoffirstbuilding="MutableVillage" name="Hamlet"
  maxpopulation="70" population="30" gold="0" food="100" maxgold="5000" maxfood="5000">
<scriptobj class="MutableVillage" num="5" player="16" healthperc="100" x="2600" y="2100" flags="0x80800001"/>
</settlement>
<group name="NO_MyTown" type="0"><obj num="0"/></group>
</mapobject>)";

constexpr std::string_view kMapXml = R"(<map name="Test"><size x="8192" y="8192"/></map>)";

struct Bench {
  ClassGraph graph;
  script::HostRegistry registry;
  /// Grass and a clear passability layer, for the cases that need areas under
  /// the nodes; the others run with no ground at all, as they always have.
  OwnedGrid terrain;
  OwnedGrid pass;
  std::unique_ptr<GameSession> session;

  explicit Bench(std::string_view map = kMap, bool with_ground = false) {
    const char* names[] = {"a.sc.xml", "b.sc.xml", "c.sc.xml", "d.sc.xml", "e.sc.xml",
                           "f.sc.xml", "g.sc.xml", "h.sc.xml", "i.sc.xml", "j.sc.xml",
                           "k.sc.xml", "l.sc.xml", "m.sc.xml", "n.sc.xml", "o.sc.xml"};
    for (std::size_t i = 0; i < std::size(kClasses); ++i) {
      REQUIRE(graph.add(bytes_of(kClasses[i]), names[i]).ok());
    }
    graph.link();
    (void)register_all_hosts(registry);
    SessionInputs inputs;
    inputs.classes = &graph;
    inputs.map_objects = bytes_of(map);
    inputs.map_properties = bytes_of(kMapXml);
    inputs.settlement_templates = bytes_of(kTemplates);
    if (with_ground) {
      auto grass = OwnedGrid::create(64, 8, 8192, 8192);
      REQUIRE(grass.ok());
      (void)grass->grid().fill(3);
      terrain = std::move(grass.value());
      auto clear = OwnedGrid::create(16, 1, 8192, 8192);
      REQUIRE(clear.ok());
      pass = std::move(clear.value());
      inputs.terrain = terrain.bytes();
      inputs.passability = pass.bytes();
    }
    auto made = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(made.ok());
    session = std::move(made.value());
  }

  /// Player 0 a Carthaginian computer; everyone else stays disabled.
  void start(std::int32_t race = static_cast<std::int32_t>(Race::carthage)) {
    MatchOptions options;
    options.human = kNoPlayer;
    options.control_set[0] = true;
    options.controls[0] = PlayerControl::computer;
    options.races[0] = race;
    for (std::size_t i = 0; i < kPlayerCount; ++i) {
      if (i != 0) options.races[i] = kNoRace;
    }
    (void)session->start_match(options);
  }

  const EconomySystem& economy() const { return *economy_of(const_cast<World&>(session->world())); }
  const World& world() const { return session->world(); }
  std::string class_of(ObjectId id) const {
    const WorldObject* slot = world().find(id);
    if (slot == nullptr || slot->class_index == kNoClass) return {};
    return std::string(graph.at(slot->class_index).id);
  }
};

}  // namespace

TEST(settlement_template_library_rebases_each_template_on_its_centre) {
  const Result<SettlementTemplateLibrary> parsed = SettlementTemplateLibrary::parse(bytes_of(kTemplates));
  REQUIRE(parsed.ok());
  const SettlementTemplateLibrary& lib = parsed.value();
  REQUIRE(lib.templates().size() == 6);
  const SettlementTemplate& town = lib.templates()[0];
  CHECK(town.name == "Carthaginian Town 1");
  CHECK(town.class_of_first_building == "CTownhall");
  CHECK(town.max_gold == 100000);
  CHECK(town.max_food == 100000);
  CHECK(town.extra_sentries == 0);
  // The box over the members' positions: x 5000..5200, y 5000..5200.
  const Point centre{5100, 5100};
  CHECK(town.centre == centre);
  CHECK(town.width == 201);
  CHECK(town.height == 201);
  REQUIRE(town.members.size() == 4);
  CHECK(town.members[1].class_name == "CTownhall");
  CHECK(town.members[1].x == -100);
  CHECK(town.members[1].y == -100);
  CHECK(town.members[0].x == 0);
  CHECK(town.members[0].y == -100);

  // Prefix matching, in document order, and a village prefix does not match
  // a town.
  // "Carthaginian Town 2" has no members and is never a match.
  CHECK(lib.matching("Carthaginian Town") == std::vector<std::size_t>{0});
  CHECK(lib.matching("Gaul Village") == std::vector<std::size_t>{3});
  CHECK(lib.matching("Briton Town").empty());
  CHECK(lib.matching("Town").empty());  // a prefix, not a substring
  CHECK(lib.matching("").empty());

  // The eight prefixes, in `Race` order, both kinds.
  CHECK(settlement_template_prefix(0, false) == "Gaul Town");
  CHECK(settlement_template_prefix(1, true) == "Republican Roman Village");
  CHECK(settlement_template_prefix(2, false) == "Carthaginian Town");
  CHECK(settlement_template_prefix(3, true) == "Iberian Village");
  CHECK(settlement_template_prefix(4, false) == "Imperial Roman Town");
  CHECK(settlement_template_prefix(5, true) == "Briton Village");
  CHECK(settlement_template_prefix(6, false) == "Egyptian Town");
  CHECK(settlement_template_prefix(7, true) == "German Village");
  CHECK(settlement_template_prefix(8, false).empty());
  CHECK(settlement_template_prefix(-1, true).empty());
}

TEST(settlement_template_anchor_keeps_the_cell_offset_and_stays_inside_the_map) {
  SettlementTemplate tpl;
  tpl.centre = Point{5100, 5100};  // 5100 = 39 * 128 + 108
  tpl.width = 201;
  tpl.height = 201;
  // The placeholder's cell, plus the template's own offset inside a cell.
  const Point placed = settlement_template_anchor(tpl, Point{2000, 2000}, 8192);
  const Point expected{1920 + 108, 1920 + 108};
  CHECK(placed == expected);
  // Clamped so the whole extent fits: half of 201 is 100, the low edge is
  // 101 and the high edge `8192 - 100 - 2`.
  const Point low = settlement_template_anchor(tpl, Point{-300, 10}, 8192);
  const Point low_expected{101, 108};
  CHECK(low == low_expected);
  const Point high = settlement_template_anchor(tpl, Point{8190, 8100}, 8192);
  const Point high_expected{8192 - 100 - 2, 8192 - 100 - 2};
  CHECK(high == high_expected);
}

TEST(start_match_makes_every_mutable_settlement_into_the_owners_race) {
  Bench b;
  const EconomySystem& economy = b.economy();
  const Settlement* mine = economy.settlements().find_by_name("S_Mine");
  REQUIRE(mine != nullptr);
  const SettlementId mine_id = mine->id;
  const ObjectId placeholder = mine->anchor;
  CHECK(b.class_of(placeholder) == "MutableStronghold");
  CHECK(b.world().named_objects().object("NO_MyTown") == placeholder);
  const std::size_t before = b.world().size();
  // The neutral `Camp` (player 15, 1-based) declares a race of its own in the
  // player table; `Hamlet` (player 16) declares none.
  b.session->world().players().setup(14).race = "Gaul";

  b.start();
  const MaterialiseReport& made = b.session->materialised();
  CHECK(made.strongholds == 1);
  CHECK(made.villages == 3);
  CHECK(made.left == 0);

  // The record is the same one, rebuilt: same id, same name, the population
  // pair kept, the gold clamped to the new ceiling, the food kept, loyalty
  // back at its start.
  mine = economy.settlements().find(mine_id);
  REQUIRE(mine != nullptr);
  CHECK(mine->id == mine_id);
  CHECK(mine->name == "S_Mine");
  CHECK(mine->kind == SettlementKind::stronghold);
  CHECK(mine->owner == 0);
  CHECK(mine->population == 12);
  CHECK(mine->max_population == 88);
  CHECK(mine->warehouse.max_gold == 100000);
  CHECK(mine->warehouse.gold == 100000);
  CHECK(mine->warehouse.food == 50);
  CHECK(mine->holder.max_units == 10000);
  CHECK(mine->can_be_captured);
  CHECK(mine->anchor != placeholder);
  CHECK(b.class_of(mine->anchor) == "CTownhall");
  // The placeholder is gone and the template's buildings stand in its place,
  // on the settlement's roll in spawn order, the anchor first.
  CHECK(b.world().find(placeholder) == nullptr);
  REQUIRE(mine->buildings.size() == 3);
  CHECK(b.class_of(mine->buildings[0].object) == "CTownhall");
  CHECK(b.class_of(mine->buildings[1].object) == "CWall");
  CHECK(b.class_of(mine->buildings[2].object) == "CBarracks");
  CHECK(mine->buildings[2].max_health == 1000);
  // Where: the template's centre lands on the placeholder's cell with its
  // own sub-cell offset, and the members keep their offsets from it. Centre
  // (5100, 5100) -> (2028, 2028); the town hall is (-100, -100) from it.
  const Point hall_at = b.world().find(mine->anchor)->state.position;
  const Point expected_hall{1928, 1928};
  CHECK(hall_at == expected_hall);
  // The buildings belong to the owner; the crops belong to nobody, and are
  // not on the roll.
  CHECK(b.world().find(mine->anchor)->state.owner == 0);
  CHECK(b.world().find(mine->buildings[1].object)->state.owner == 0);
  std::size_t crops = 0;
  for (const WorldObject& slot : b.world().objects()) {
    if (b.class_of(slot.id) != "Crops") continue;
    ++crops;
    CHECK(slot.state.owner == kNoPlayer);
    CHECK(slot.settlement == mine->object);
  }
  CHECK(crops == 1);
  // Four placeholders out; the town's four members, two Carthaginian
  // villages' two each and the Gaul village's one in.
  CHECK(b.world().size() == before - 4 + 9);

  // The alias follows to the new central building.
  CHECK(b.world().named_objects().object("NO_MyTown") == mine->anchor);

  // Player 1's village takes its owner's race; the neutral one takes the
  // nearest converted settlement's, which is the same town.
  const Settlement* farm = economy.settlements().find_by_name("Farm");
  REQUIRE(farm != nullptr);
  CHECK(b.class_of(farm->anchor) == "CVillage");
  CHECK(farm->kind == SettlementKind::village);
  CHECK(farm->warehouse.max_gold == 5000);
  const Settlement* hamlet = economy.settlements().find_by_name("Hamlet");
  REQUIRE(hamlet != nullptr);
  CHECK(b.class_of(hamlet->anchor) == "CVillage");
  CHECK(hamlet->owner == 15);
  // `Camp` has a race of its own and keeps it, in the second pass -- and
  // because the second pass does not feed the nearest-search, `Hamlet`,
  // converted after it and standing nearer to it than to the town, still
  // follows the town.
  const Settlement* camp = economy.settlements().find_by_name("Camp");
  REQUIRE(camp != nullptr);
  CHECK(b.class_of(camp->anchor) == "GVillage");
  // A class that merely descends from the placeholder's is not a
  // placeholder: 0x00552c1b compares class handles.
  const Settlement* odd = economy.settlements().find_by_name("Odd");
  REQUIRE(odd != nullptr);
  CHECK(b.class_of(odd->anchor) == "MutableStrongholdOdd");
  // And a settlement that was never a placeholder is untouched.
  const Settlement* theirs = economy.settlements().find_by_name("Theirs");
  REQUIRE(theirs != nullptr);
  CHECK(b.class_of(theirs->anchor) == "GTownhall");
  CHECK(theirs->warehouse.gold == 5000);

  // The node table was rebuilt over the new anchors: the town's node is
  // centred where its new hall stands, not where the placeholder stood.
  const GaikaTable& nodes = b.world().gaika();
  const GaikaId node = nodes.for_settlement(mine->object);
  REQUIRE(node != kNoGaika);
  REQUIRE(nodes.find(node) != nullptr);
  CHECK(nodes.find(node)->center == hall_at);
}

TEST(a_mutable_settlement_nobody_has_a_race_for_takes_gaul) {
  // No participant at all: every placeholder falls to the second pass, and
  // with nothing converted before it the nearest race is `Gaul` (0x00552f0f
  // starts its search from zero and finds nothing to replace it).
  Bench b;
  MatchOptions options;
  options.human = kNoPlayer;
  for (std::size_t i = 0; i < kPlayerCount; ++i) options.races[i] = kNoRace;
  (void)b.session->start_match(options);
  CHECK(b.session->materialised().strongholds == 1);
  CHECK(b.session->materialised().villages == 3);
  const Settlement* mine = b.economy().settlements().find_by_name("S_Mine");
  REQUIRE(mine != nullptr);
  CHECK(b.class_of(mine->anchor) == "GTownhall");
  const Settlement* farm = b.economy().settlements().find_by_name("Farm");
  REQUIRE(farm != nullptr);
  CHECK(b.class_of(farm->anchor) == "GVillage");
}

TEST(a_race_with_no_usable_template_leaves_the_placeholder_and_counts_it) {
  Bench b;
  b.start(static_cast<std::int32_t>(Race::egypt));
  const MaterialiseReport& made = b.session->materialised();
  // The Egyptian town template's central class is one the graph does not
  // know, and there is no Egyptian village at all: the stronghold and player
  // 1's village are left standing, untouched. The neutral villages follow the
  // nearest *converted* settlement, and there is none, so they are Gaul's.
  CHECK(made.strongholds == 0);
  CHECK(made.villages == 2);
  CHECK(made.left == 2);
  const Settlement* mine = b.economy().settlements().find_by_name("S_Mine");
  REQUIRE(mine != nullptr);
  CHECK(b.class_of(mine->anchor) == "MutableStronghold");
  REQUIRE(mine->buildings.size() == 1);
  CHECK(b.world().find(mine->anchor) != nullptr);
  const Settlement* hamlet = b.economy().settlements().find_by_name("Hamlet");
  REQUIRE(hamlet != nullptr);
  CHECK(b.class_of(hamlet->anchor) == "GVillage");
}

/// Two first-pass towns at the same distance from a neutral village: the
/// earlier `ID` wins, because 0x00552f20 replaces only on strictly less.
TEST(a_neutral_between_two_towns_follows_the_earlier_one_on_a_tie) {
  constexpr std::string_view kTie = R"(<mapobject>
<settlement id="0" player="1" classoffirstbuilding="MutableStronghold" name="West"
  maxpopulation="88" population="12" gold="1000" food="50" maxgold="100000" maxfood="100000">
<scriptobj class="MutableStronghold" num="0" player="1" healthperc="100" x="2000" y="4000" flags="0x80800001"/>
</settlement>
<settlement id="1" player="2" classoffirstbuilding="MutableStronghold" name="East"
  maxpopulation="88" population="12" gold="1000" food="50" maxgold="100000" maxfood="100000">
<scriptobj class="MutableStronghold" num="1" player="2" healthperc="100" x="6000" y="4000" flags="0x80800001"/>
</settlement>
<settlement id="2" player="16" classoffirstbuilding="MutableVillage" name="Between"
  maxpopulation="70" population="30" gold="0" food="100" maxgold="5000" maxfood="5000">
<scriptobj class="MutableVillage" num="2" player="16" healthperc="100" x="4000" y="4000" flags="0x80800001"/>
</settlement>
</mapobject>)";
  Bench b(kTie);
  MatchOptions options;
  options.human = kNoPlayer;
  for (std::size_t i = 0; i < kPlayerCount; ++i) options.races[i] = kNoRace;
  options.control_set[0] = options.control_set[1] = true;
  options.controls[0] = options.controls[1] = PlayerControl::computer;
  options.races[0] = static_cast<std::int32_t>(Race::gaul);
  options.races[1] = static_cast<std::int32_t>(Race::carthage);
  (void)b.session->start_match(options);
  const Settlement* between = b.economy().settlements().find_by_name("Between");
  REQUIRE(between != nullptr);
  CHECK(b.class_of(between->anchor) == "GVillage");
}

// --------------------------------------------------------------------------
// the setup's rules
// --------------------------------------------------------------------------

TEST(the_setups_rules_scale_the_population_set_the_gold_and_reveal_the_map) {
  // 0x005267b3..0x005267dd: every settlement but the wildlife's has its
  // `max_population` scaled by the percent and, when the gold is not
  // "Default", its warehouse set to it; `population` is left alone.
  // 0x00526813: exploration off reveals the whole map.
  Bench b;
  MatchOptions options;
  options.human = kNoPlayer;
  options.control_set[0] = true;
  options.controls[0] = PlayerControl::computer;
  options.races[0] = static_cast<std::int32_t>(Race::carthage);
  for (std::size_t i = 0; i < kPlayerCount; ++i) {
    if (i != 0) options.races[i] = kNoRace;
  }
  options.world_population = 150;
  options.starting_gold = 10000;
  options.exploration = false;
  options.condition_set = true;
  options.condition = VictoryCondition::score_limit;
  options.threshold = "2000";
  (void)b.session->start_match(options);

  const Settlement* odd = b.economy().settlements().find_by_name("Odd");
  REQUIRE(odd != nullptr);
  CHECK(odd->max_population == 150);
  CHECK(odd->population == 60);
  CHECK(odd->gold() == 10000);
  // The wildlife's camp keeps both.
  const Settlement* camp = b.economy().settlements().find_by_name("Camp");
  REQUIRE(camp != nullptr);
  CHECK(camp->owner == kNeutralWildlife);
  CHECK(camp->max_population == 70);
  CHECK(camp->gold() == 0);
  // The rules replaced the map's, and are on the match.
  const MatchSystem* match = match_system_of(b.session->world());
  REQUIRE(match != nullptr);
  CHECK(match->rules().condition == VictoryCondition::score_limit);
  CHECK(match->rules().param == "2000");
  CHECK(match->rules().world_population == 150);
  CHECK(match->rules().starting_gold == 10000);
  CHECK(!match->rules().exploration);
  CHECK(match->rules().fog_of_war);
  const FogSystem* fog = fog_system_of(b.session->world());
  REQUIRE(fog != nullptr);
  CHECK(fog->map().explored(Point{100, 100}, 0));
  CHECK(fog->map().explored(Point{8000, 8000}, 3));

  // The four rules ride the match section.
  std::vector<std::byte> saved;
  match->serialize(saved);
  MatchSystem restored;
  REQUIRE(restored.deserialize(saved).ok());
  CHECK(restored.rules().world_population == 150);
  CHECK(restored.rules().starting_gold == 10000);
  CHECK(!restored.rules().exploration);
  CHECK(restored.rules().param == "2000");
}

TEST(the_setups_defaults_leave_the_map_as_authored) {
  Bench b;
  b.start();
  const Settlement* odd = b.economy().settlements().find_by_name("Odd");
  REQUIRE(odd != nullptr);
  CHECK(odd->max_population == 100);
  CHECK(odd->gold() == 5000);
  const FogSystem* fog = fog_system_of(b.session->world());
  REQUIRE(fog != nullptr);
  CHECK(!fog->map().explored(Point{8000, 8000}, 3));
}

TEST(a_template_lays_its_ground_where_it_lands) {
  // The template map: 4096 units, unpainted (15) but for a 5 x 4 patch of
  // paving with one hole, at cells 10..14 x 10..13, and a member at the
  // patch's middle. Its centre is (800, 736).
  auto pack = OwnedGrid::create(64, 8, 4096, 4096);
  REQUIRE(pack.ok());
  (void)pack->grid().fill(15);
  for (std::uint32_t y = 10; y <= 13; ++y) {
    for (std::uint32_t x = 10; x <= 14; ++x) (void)pack->grid().set_cell(x, y, 7);  // roads
  }
  (void)pack->grid().set_cell(12, 11, 15);  // a hole the map shows through
  SettlementTemplate tpl;
  tpl.centre = Point{800, 736};
  MapObject member;
  member.class_name = "GTownhall";
  member.x = 0;
  member.y = 0;
  tpl.members.push_back(member);

  // The map: 8192 units of grass on a plateau at 115, with a lake of deep
  // water beside where the template will land, a decoration inside and one
  // far away.
  auto terrain = OwnedGrid::create(64, 8, 8192, 8192);
  REQUIRE(terrain.ok());
  (void)terrain->grid().fill(3);
  for (std::uint32_t y = 28; y <= 40; ++y) {
    for (std::uint32_t x = 42; x <= 54; ++x) (void)terrain->grid().set_cell(x, y, 13);
  }
  auto height = OwnedGrid::create(32, 8, 8192, 8192);
  REQUIRE(height.ok());
  (void)height->grid().fill(115);
  auto decor = OwnedGrid::create(64, 16, 8192, 8192);
  REQUIRE(decor.ok());
  (void)decor->grid().set_cell(33, 33, 7);
  (void)decor->grid().set_cell(100, 100, 7);
  std::vector<std::uint8_t> marks;
  GroundLayers layers{&terrain->grid(), &height->grid(), &decor->grid(), &marks};

  // The centre lands at (2048, 2048): template cell (tx, ty) -> map cell
  // (tx + 19, ty + 20), 1248 / 64 = 19.5 rounded down for x.
  const Point origin{2048, 2048};
  const TemplateGround laid = stamp_template_ground(layers, pack->grid(), tpl, origin);
  CHECK(laid.cells_copied == 19);
  CHECK(terrain->grid().cell(29, 30) == 7);   // template (10, 10)
  CHECK(terrain->grid().cell(33, 33) == 7);   // template (14, 13)
  CHECK(terrain->grid().cell(31, 31) == 3);   // the hole keeps the map's grass
  CHECK(terrain->grid().cell(28, 30) == 3);   // outside the patch
  // Sea level under every copied cell, four height cells each, and the
  // limiter's slope from the plateau: 20 per cell, so the cell beside a
  // levelled one is at most 20 and the hole is pulled to 20.
  CHECK(laid.cells_levelled == (19 + laid.shore_cells) * 4);
  CHECK(height->grid().cell(58, 60) == 0);
  CHECK(height->grid().cell(62, 62) <= 20);   // the hole, pulled down
  CHECK(height->grid().cell(57, 60) <= 20);   // beside the patch
  CHECK(height->grid().cell(55, 60) <= 60 && height->grid().cell(55, 60) >= 40);
  CHECK(height->grid().cell(200, 200) == 115); // far away, untouched
  CHECK(laid.cells_sloped > 0);
  // The decoration inside was bulldozed, the far one stands.
  CHECK(laid.decorations_bulldozed == 1);
  CHECK(decor->grid().cell(33, 33) == 0);
  CHECK(decor->grid().cell(100, 100) == 7);
  // The lake's shore near the town: land within four cells of deep water
  // in the margin became shallow water at sea level, and the deep water
  // whose neighbourhood the patch broke turned shallow too.
  CHECK(laid.shore_cells > 0);
  CHECK(terrain->grid().cell(41, 34) == 12);
  CHECK(terrain->grid().cell(42, 30) == 12);
  CHECK(terrain->grid().cell(48, 34) == 13);  // the middle of the lake stays deep
  // A second template's limiter does not move what the first levelled.
  const std::size_t marked = static_cast<std::size_t>(std::count(marks.begin(), marks.end(), 1));
  CHECK(marked == 19 * 4 + laid.shore_cells * 4);
  // A template with no members, or no pack, lays nothing.
  SettlementTemplate empty;
  CHECK(!stamp_template_ground(layers, pack->grid(), empty, origin).any());
}

/// A node outlives its town hall, and a save carries it. The table is built
/// once, over the anchors as they stand, and the original never moves a node
/// again (`GAIKA::Center` is one of the fields its `Persist` names). A load
/// used to rebuild it over the restored world, where a razed hall resolves to
/// nowhere, so the town's node went to (-1,-1) and every squad beside the
/// ruin was filed under another node: Crossroads saved mid-war diverged
/// three turns after the load, when a priest marching on a fallen town lost
/// its route. Nothing hashes the table; this asserts it.
TEST(a_razed_town_hall_leaves_its_node_where_it_stood_across_a_save) {
  Bench b(kMap, /*with_ground=*/true);
  b.start();
  // Ground under the nodes, so each has an area and the table has links: a
  // load that restored the centres and not the links, or not the areas,
  // would pass on a map with neither.
  REQUIRE(b.world().lsa().size() >= 1);
  const Settlement* mine = b.economy().settlements().find_by_name("S_Mine");
  REQUIRE(mine != nullptr);
  const ObjectId town = mine->object;
  const ObjectId hall = mine->anchor;
  const Point hall_at = b.world().find(hall)->state.position;
  const GaikaTable& before = b.world().gaika();
  const GaikaId node = before.for_settlement(town);
  REQUIRE(node != kNoGaika);
  REQUIRE(before.find(node)->center == hall_at);
  REQUIRE(before.find(node)->lsa != kNoLsa);
  REQUIRE(!before.neighbours(node).empty());

  // The hall falls. The running table keeps the node where it was.
  REQUIRE(b.session->world().despawn(hall));
  REQUIRE(b.world().find(hall) == nullptr);
  CHECK(before.find(node)->center == hall_at);
  const Point beside{hall_at.x + 40, hall_at.y + 40};
  const GaikaId filed = before.at(b.world().lsa(), beside);
  CHECK(filed == node);

  const auto saved = b.session->save();
  REQUIRE(saved.ok());
  Bench again(kMap, /*with_ground=*/true);
  REQUIRE(again.session->load(saved.value()).ok());

  // Node for node, the same table -- and a unit beside the ruin is filed
  // where the running game files it.
  const GaikaTable& after = again.world().gaika();
  REQUIRE(after.count() == before.count());
  for (GaikaId id = 1; id <= before.count(); ++id) {
    CHECK(after.find(id)->center == before.find(id)->center);
    CHECK(after.find(id)->lsa == before.find(id)->lsa);
    CHECK(after.find(id)->settlement == before.find(id)->settlement);
    const auto a = before.neighbours(id);
    const auto c = after.neighbours(id);
    CHECK(std::vector<GaikaId>(a.begin(), a.end()) == std::vector<GaikaId>(c.begin(), c.end()));
  }
  CHECK(after.find(node)->center == hall_at);
  CHECK(after.at(again.world().lsa(), beside) == filed);

  // And a second save of the restored game is the first, byte for byte.
  const auto resaved = again.session->save();
  REQUIRE(resaved.ok());
  CHECK(resaved.value() == saved.value());
}

/// The nodes a save carries are checked before they are adopted: an area the
/// rebuilt partition does not have, or a count no map could need, is a save
/// this build refuses rather than one it links into nonsense.
TEST(a_save_whose_nodes_name_no_area_is_refused) {
  Bench b(kMap, /*with_ground=*/true);
  b.start();
  const std::int32_t count = b.world().gaika().count();
  REQUIRE(count >= 1);
  const auto saved = b.session->save();
  REQUIRE(saved.ok());
  const auto reader = SaveReader::open(saved.value());
  REQUIRE(reader.ok());
  const std::span<const std::byte> section = reader->section(kSessionSection);
  // The nodes close the section, sixteen bytes each, after their count.
  const std::size_t end = static_cast<std::size_t>(section.data() - saved.value().data()) + section.size();
  const auto put = [](std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
  };
  {
    // The untouched save loads, so what is refused below is the edit.
    Bench again(kMap, /*with_ground=*/true);
    CHECK(again.session->load(saved.value()).ok());
  }
  for (const std::uint32_t lsa : {static_cast<std::uint32_t>(b.world().lsa().size() + 1), 0xffffffffu}) {
    std::vector<std::byte> bad = saved.value();
    put(bad, end - 8, lsa);  // the last node's area
    Bench again(kMap, /*with_ground=*/true);
    CHECK(!again.session->load(bad).ok());
  }
  {
    std::vector<std::byte> bad = saved.value();
    put(bad, end - 4 - 16 * static_cast<std::size_t>(count), 0xffffffffu);  // the count
    Bench again(kMap, /*with_ground=*/true);
    CHECK(!again.session->load(bad).ok());
  }
}
