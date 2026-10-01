// Class graph tests.
//
// No game data: CI has no installation, and this project never carries game
// assets. The retail corpus is covered instead by `imcheck classes`, which
// diffs the whole resolved graph against the Python reference reader. What is
// pinned here is the behaviour that corpus cannot demonstrate -- the merge
// rules at the boundaries, and the tolerance the retail data depends on.

#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "test.hpp"

using namespace imperivm::core;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// The documents outlive the graph in every test here, because the graph
/// interns what it keeps -- which is the point of the arena, and worth having
/// a test rely on rather than an assurance in a comment.
Status add(ClassGraph& graph, std::string_view xml, std::string_view source) {
  return graph.add(bytes_of(xml), source);
}

std::string_view value_of(const std::vector<ClassProperty>& bag, std::string_view key) {
  for (const auto& entry : bag) {
    if (entry.key == key) return entry.value;
  }
  return {};
}

/// A pack index that is empty except for what a test puts in it.
class FakeProbe : public ResourceProbe {
 public:
  void add(std::string path) { paths_.push_back(std::move(path)); }
  bool exists(std::string_view path) const override {
    for (const auto& known : paths_) {
      if (known == path) return true;
    }
    return false;
  }
  bool sound_exists(std::string_view value) const override { return exists(value); }

 private:
  std::vector<std::string> paths_;
};

/// A small tree: Object -> Unit -> Military -> Hero, used by several tests.
ClassGraph three_deep() {
  ClassGraph graph;
  add(graph,
      R"(<class id="Object" cpp_class="CVXDecor" parent="" entity="">
           <properties race="None" maxhealth="100" radius="0"/>
           <properties interface="thumb"/>
         </class>)",
      "OBJECT.SC.XML");
  add(graph,
      R"(<class id="Unit" cpp_class="CVXUnit" parent="Object" entity="">
           <properties maxhealth="120" speed="90"/>
         </class>)",
      "UNIT.SC.XML");
  add(graph,
      R"(<class id="Military" cpp_class="CVXUnit" parent="Unit" entity="">
           <properties damage="10"/>
         </class>)",
      "MILITARY.SC.XML");
  add(graph,
      R"(<class id="Hero" cpp_class="CVXHero" parent="Military" entity="Units/Hero/hero.ent.xml">
           <properties maxhealth="800"/>
         </class>)",
      "HERO.SC.XML");
  graph.link();
  return graph;
}

}  // namespace

TEST(class_graph_parses_a_class_definition) {
  ClassGraph graph;
  REQUIRE(add(graph,
              R"(<class id="GShipyard1" altid="Shipyard 1" cpp_class="CVXTownHall"
                        parent="BaseShipyard" entity="Buildings/Shipyard/s.ent.xml"
                        entity_winter="Buildings/Shipyard/s_w.ent.xml">
                   <properties radius="245" selection_radius="250"/>
                   <behavior script="data/subai/arena_behavior.vs"/>
                   <method sig="train" vs="data/subai/barrack_train.vs"/>
                   <sounds select="BarracksSelect"/>
                 </class>)",
              "SHIPYARD 1.SC.XML")
              .ok());
  graph.link();
  REQUIRE(graph.size() == 1);

  const ClassDefinition& shipyard = graph.at(0);
  CHECK(shipyard.id == "GShipyard1");
  CHECK(shipyard.altid == "Shipyard 1");
  CHECK(shipyard.cpp_class == "CVXTownHall");
  CHECK(shipyard.parent == "BaseShipyard");
  CHECK(shipyard.source == "SHIPYARD 1.SC.XML");
  CHECK(value_of(shipyard.properties, "radius") == "245");
  REQUIRE(shipyard.behaviors.size() == 1);
  CHECK(shipyard.behaviors[0] == "data/subai/arena_behavior.vs");
  REQUIRE(shipyard.methods.size() == 1);
  CHECK(shipyard.methods[0].sig == "train");
  CHECK(value_of(shipyard.sounds, "select") == "BarracksSelect");

  // The file name is not the identity; `id` is.
  CHECK(graph.find("SHIPYARD 1.SC.XML") == kNoClass);
  CHECK(graph.find("GShipyard1") == 0);
}

TEST(class_graph_rejects_documents_that_are_not_class_definitions) {
  ClassGraph graph;
  CHECK(!add(graph, "<class id=\"A\" cpp_class=", "BROKEN.SC.XML").ok());
  CHECK(!add(graph, R"(<entity name="x"/>)", "WRONGROOT.SC.XML").ok());
  CHECK(!add(graph, R"(<class cpp_class="CVXDecor" parent=""/>)", "NOID.SC.XML").ok());
  CHECK(!add(graph, R"(<class id="A" parent=""/>)", "NOCPP.SC.XML").ok());
  CHECK(graph.size() == 0);
}

TEST(class_graph_merges_repeated_properties_within_a_class) {
  // `Object` uses 19 separate <properties> elements; BASERUINS writes
  // `itemtype` eleven times and, under last-wins, keeps the eleventh.
  ClassGraph graph;
  REQUIRE(add(graph,
              R"(<class id="BaseRuins" cpp_class="CVXItemHolder" parent="">
                   <properties itemtype="Elephant Tusk"/>
                   <properties radius="30"/>
                   <properties itemtype="Boar teeth"/>
                   <properties itemtype="Gem of Wisdom" radius="45"/>
                   <sounds select="a"/>
                   <sounds select="b" die="c"/>
                 </class>)",
              "BASERUINS.SC.XML")
              .ok());
  graph.link();
  REQUIRE(graph.size() == 1);
  CHECK(value_of(graph.at(0).properties, "itemtype") == "Gem of Wisdom");
  CHECK(value_of(graph.at(0).properties, "radius") == "45");
  CHECK(value_of(graph.at(0).sounds, "select") == "b");
  CHECK(value_of(graph.at(0).sounds, "die") == "c");
}

TEST(class_graph_resolves_inheritance_with_the_child_winning) {
  const ClassGraph graph = three_deep();
  const ClassIndex hero = graph.find("Hero");
  REQUIRE(hero != kNoClass);

  const auto resolved = graph.resolved_properties(hero);
  CHECK(value_of(resolved, "maxhealth") == "800");  // Hero over Unit over Object
  CHECK(value_of(resolved, "speed") == "90");       // from Unit
  CHECK(value_of(resolved, "damage") == "10");      // from Military
  CHECK(value_of(resolved, "race") == "None");      // seeded by Object
  CHECK(value_of(resolved, "interface") == "thumb");
  CHECK(value_of(resolved, "nosuchproperty").empty());

  // The single-property accessor walks the same chain without building the bag.
  CHECK(graph.property(hero, "maxhealth") == "800");
  CHECK(graph.property(hero, "race") == "None");
  CHECK(graph.property(hero, "nosuchproperty").empty());

  const ClassIndex unit = graph.find("Unit");
  REQUIRE(unit != kNoClass);
  CHECK(value_of(graph.resolved_properties(unit), "maxhealth") == "120");
}

TEST(class_graph_reports_depth_children_and_ancestry) {
  const ClassGraph graph = three_deep();
  REQUIRE(graph.roots().size() == 1);
  const ClassIndex object = graph.roots()[0];
  CHECK(graph.at(object).id == "Object");
  CHECK(graph.depth(object) == 1);
  CHECK(graph.max_depth() == 4);
  CHECK(graph.subtree_size(object) == 4);

  const ClassIndex hero = graph.find("Hero");
  REQUIRE(hero != kNoClass);
  CHECK(graph.depth(hero) == 4);
  CHECK(graph.subtree_size(hero) == 1);
  CHECK(graph.children(hero).empty());

  const auto chain = graph.ancestry(hero);
  REQUIRE(chain.size() == 4);
  CHECK(graph.at(chain[0]).id == "Hero");
  CHECK(graph.at(chain[1]).id == "Military");
  CHECK(graph.at(chain[2]).id == "Unit");
  CHECK(graph.at(chain[3]).id == "Object");
}

TEST(class_graph_orders_children_by_id) {
  // Iteration order is world state, so it must not depend on the order the
  // files happened to arrive in.
  ClassGraph graph;
  add(graph, R"(<class id="Root" cpp_class="CVXDecor" parent=""/>)", "R.SC.XML");
  add(graph, R"(<class id="Zeta" cpp_class="CVXDecor" parent="Root"/>)", "Z.SC.XML");
  add(graph, R"(<class id="Alpha" cpp_class="CVXDecor" parent="Root"/>)", "A.SC.XML");
  add(graph, R"(<class id="Mid" cpp_class="CVXDecor" parent="Root"/>)", "M.SC.XML");
  graph.link();

  const ClassIndex root = graph.find("Root");
  REQUIRE(root != kNoClass);
  const auto children = graph.children(root);
  REQUIRE(children.size() == 3);
  CHECK(graph.at(children[0]).id == "Alpha");
  CHECK(graph.at(children[1]).id == "Mid");
  CHECK(graph.at(children[2]).id == "Zeta");
}

TEST(class_graph_merges_methods_by_sig) {
  ClassGraph graph;
  add(graph,
      R"(<class id="Unit" cpp_class="CVXUnit" parent="">
           <method sig="attack" vs="unit_attack.vs" verify="unit_attack_verify.vs"/>
           <method sig="engage" vs="unit_engage.vs"/>
           <method sig="attack" vs="unit_advance.vs"/>
           <method sig="board" vs="board.vs" onfinish="board_finish.vs"/>
         </class>)",
      "UNIT.SC.XML");
  add(graph,
      R"(<class id="Hero" cpp_class="CVXHero" parent="Unit">
           <method sig="attack" vs="hero_attack.vs"/>
           <method sig="taunt" vs="hero_taunt.vs"/>
         </class>)",
      "HERO.SC.XML");
  graph.link();

  const ClassIndex unit = graph.find("Unit");
  REQUIRE(unit != kNoClass);
  const auto unit_methods = graph.resolved_methods(unit);
  REQUIRE(unit_methods.size() == 3);
  // Duplicate <method sig> within a class: last wins, which also discards the
  // verify script bound to the first declaration. Assumed, not proven -- see
  // docs/formats/sc-xml.md.
  for (const auto& method : unit_methods) {
    if (method.sig == "attack") {
      CHECK(method.vs == "unit_advance.vs");
      CHECK(method.verify.empty());
    }
    if (method.sig == "board") CHECK(method.onfinish == "board_finish.vs");
  }

  const ClassIndex hero = graph.find("Hero");
  REQUIRE(hero != kNoClass);
  const auto hero_methods = graph.resolved_methods(hero);
  REQUIRE(hero_methods.size() == 4);
  for (const auto& method : hero_methods) {
    if (method.sig == "attack") CHECK(method.vs == "hero_attack.vs");
  }
}

TEST(class_graph_accumulates_behaviors) {
  // Outpost declares the generic behaviours and COutpost adds a faction one.
  // Replacement would strip the outpost of its inherited guard logic.
  ClassGraph graph;
  add(graph,
      R"(<class id="Outpost" cpp_class="CVXTownHall" parent="">
           <behavior script="outpost_behavior_guard.vs"/>
           <behavior script="outpost_behavior.vs"/>
         </class>)",
      "OUTPOST.SC.XML");
  add(graph,
      R"(<class id="COutpost" cpp_class="CVXOutpost" parent="Outpost">
           <behavior script="coutpost_behavior.vs"/>
           <behavior script="outpost_behavior.vs"/>
         </class>)",
      "COUTPOST.SC.XML");
  graph.link();

  const ClassIndex coutpost = graph.find("COutpost");
  REQUIRE(coutpost != kNoClass);
  // Own first, then the parent's, and the script both declare is in the list
  // twice: 0x005a5fd0 appends without looking.
  const auto scripts = graph.resolved_behaviors(coutpost);
  REQUIRE(scripts.size() == 4);
  CHECK(scripts[0] == "coutpost_behavior.vs");
  CHECK(scripts[1] == "outpost_behavior.vs");
  CHECK(scripts[2] == "outpost_behavior_guard.vs");
  CHECK(scripts[3] == "outpost_behavior.vs");
}

TEST(class_graph_keeps_at_most_eight_behaviors) {
  // Five of the parent's and five of the child's: the child's five first, then
  // the parent's until the list holds eight.
  ClassGraph graph;
  add(graph,
      R"(<class id="Base" cpp_class="CVXBuilding" parent="">
           <behavior script="p1.vs"/><behavior script="p2.vs"/><behavior script="p3.vs"/>
           <behavior script="p4.vs"/><behavior script="p5.vs"/>
         </class>)",
      "BASE.SC.XML");
  add(graph,
      R"(<class id="Leaf" cpp_class="CVXBuilding" parent="Base">
           <behavior script="c1.vs"/><behavior script="c2.vs"/><behavior script="c3.vs"/>
           <behavior script="c4.vs"/><behavior script="c5.vs"/>
         </class>)",
      "LEAF.SC.XML");
  graph.link();
  const auto scripts = graph.resolved_behaviors(graph.find("Leaf"));
  REQUIRE(scripts.size() == ClassGraph::kMaxBehaviors);
  CHECK(scripts[0] == "c1.vs");
  CHECK(scripts[4] == "c5.vs");
  CHECK(scripts[5] == "p1.vs");
  CHECK(scripts[7] == "p3.vs");
  CHECK(graph.resolved_behaviors(graph.find("Base")).size() == 5);
}

TEST(class_graph_accumulates_default_commands) {
  ClassGraph graph;
  add(graph,
      R"(<class id="Unit" cpp_class="CVXUnit" parent="">
           <defaultcmd target=""><cmd name="move"/><cmd name="advance" ctrl="1"/></defaultcmd>
           <defaultcmd target="Unit"><cmd name="attack"/></defaultcmd>
         </class>)",
      "UNIT.SC.XML");
  add(graph,
      R"(<class id="Hero" cpp_class="CVXHero" parent="Unit">
           <defaultcmd target="Building"><cmd name="enter"/></defaultcmd>
           <defaultcmd target="Unit"><cmd name="attack_independent"/></defaultcmd>
         </class>)",
      "HERO.SC.XML");
  graph.link();

  const ClassIndex hero = graph.find("Hero");
  REQUIRE(hero != kNoClass);
  const auto blocks = graph.resolved_default_cmds(hero);
  REQUIRE(blocks.size() == 3);

  // Inherited blocks keep their position; a child's block for a target it
  // already inherits replaces that block in place.
  CHECK(blocks[0].target.empty());
  REQUIRE(blocks[0].cmds.size() == 2);
  CHECK(blocks[0].cmds[0].name == "move");
  CHECK(blocks[0].cmds[0].ctrl == false);
  CHECK(blocks[0].cmds[1].name == "advance");
  CHECK(blocks[0].cmds[1].ctrl == true);

  CHECK(blocks[1].target == "Unit");
  REQUIRE(blocks[1].cmds.size() == 1);
  CHECK(blocks[1].cmds[0].name == "attack_independent");

  CHECK(blocks[2].target == "Building");
}

TEST(class_graph_honours_nodefcmdinherit) {
  // Only Sentry and Wagon opt out, and both are classes whose interaction
  // verbs differ sharply from Unit's.
  ClassGraph graph;
  add(graph,
      R"(<class id="Unit" cpp_class="CVXUnit" parent="">
           <defaultcmd target=""><cmd name="move"/></defaultcmd>
           <defaultcmd target="Unit"><cmd name="attack"/></defaultcmd>
         </class>)",
      "UNIT.SC.XML");
  add(graph,
      R"(<class id="Wagon" cpp_class="CVXWagon" parent="Unit">
           <nodefcmdinherit/>
           <defaultcmd target="Wagon"><cmd name="capture"/></defaultcmd>
         </class>)",
      "WAGON.SC.XML");
  add(graph, R"(<class id="GWagon" cpp_class="CVXWagon" parent="Wagon"/>)", "GWAGON.SC.XML");
  graph.link();

  const ClassIndex wagon = graph.find("Wagon");
  REQUIRE(wagon != kNoClass);
  const auto blocks = graph.resolved_default_cmds(wagon);
  REQUIRE(blocks.size() == 1);
  CHECK(blocks[0].target == "Wagon");

  // The opt-out is not itself inherited as a rule, but its effect is: a child
  // of Wagon starts from Wagon's table, which is already free of Unit's.
  const ClassIndex gwagon = graph.find("GWagon");
  REQUIRE(gwagon != kNoClass);
  const auto inherited = graph.resolved_default_cmds(gwagon);
  REQUIRE(inherited.size() == 1);
  CHECK(inherited[0].target == "Wagon");
}

TEST(class_graph_resolves_info_bar_slots_per_index) {
  ClassGraph graph;
  add(graph,
      R"(<class id="Unit" cpp_class="CVXUnit" parent="">
           <value0 icon="level.bmp" script="return .AsUnit.level;" rollover="Level" flags="-1"/>
           <value1 icon="hp.bmp" rollover="Health"/>
         </class>)",
      "UNIT.SC.XML");
  add(graph,
      R"(<class id="BaseBuilding" cpp_class="CVXBuilding" parent="Unit">
           <value0 icon="pop.bmp" rollover="Population"/>
           <value1/>
         </class>)",
      "BASEBUILDING.SC.XML");
  graph.link();

  const ClassIndex building = graph.find("BaseBuilding");
  REQUIRE(building != kNoClass);
  const auto values = graph.resolved_values(building);
  CHECK(values[0].present);
  CHECK(values[0].icon == "pop.bmp");
  CHECK(values[0].rollover == "Population");
  CHECK(values[0].flags.empty());  // not inherited slot-wise; the slot is replaced whole
  // An empty <value1/> is how a class blanks an inherited slot.
  CHECK(values[1].present);
  CHECK(values[1].icon.empty());
  CHECK(!values[2].present);
}

TEST(class_graph_selects_seasonal_entities) {
  ClassGraph graph;
  add(graph,
      R"(<class id="BaseBarracks" cpp_class="CVXBarrack" parent=""
                entity="Buildings/RBarracks/RBarracks_as.ent.xml"
                entity_winter="Buildings/RBarracks/RBarracks_w.ent.xml"/>)",
      "BARRACKS.SC.XML");
  // 14 classes declare the three seasonal paths and no `entity` at all, which
  // leaves summer with nothing -- an open question, not a bug to paper over.
  add(graph,
      R"(<class id="Tree" cpp_class="CVXDecor" parent=""
                entity_spring="t_as.ent.xml" entity_autumn="t_as.ent.xml"
                entity_winter="t_w.ent.xml"/>)",
      "TREE.SC.XML");
  add(graph, R"(<class id="Abstract" cpp_class="CVXDecor" parent="" entity=""/>)", "ABS.SC.XML");
  graph.link();

  const ClassIndex barracks = graph.find("BaseBarracks");
  REQUIRE(barracks != kNoClass);
  CHECK(graph.entity_path(barracks, Season::summer) == "Buildings/RBarracks/RBarracks_as.ent.xml");
  CHECK(graph.entity_path(barracks, Season::spring) == "Buildings/RBarracks/RBarracks_as.ent.xml");
  CHECK(graph.entity_path(barracks, Season::autumn) == "Buildings/RBarracks/RBarracks_as.ent.xml");
  CHECK(graph.entity_path(barracks, Season::winter) == "Buildings/RBarracks/RBarracks_w.ent.xml");

  const ClassIndex tree = graph.find("Tree");
  REQUIRE(tree != kNoClass);
  CHECK(graph.entity_path(tree, Season::spring) == "t_as.ent.xml");
  CHECK(graph.entity_path(tree, Season::winter) == "t_w.ent.xml");
  CHECK(graph.entity_path(tree, Season::summer).empty());

  const ClassIndex abstract = graph.find("Abstract");
  REQUIRE(abstract != kNoClass);
  CHECK(graph.entity_path(abstract, Season::winter).empty());
}

TEST(class_graph_resolves_id_before_altid) {
  // Three altid values are claimed by more than one class and one collides
  // with a real id, so id must win and the first altid registration after it.
  ClassGraph graph;
  add(graph, R"(<class id="Inn" cpp_class="CVXTownHall" parent="" altid="Inn"/>)", "INN.SC.XML");
  add(graph, R"(<class id="GTavern" cpp_class="CVXTavern" parent="" altid="Tavern"/>)",
      "GTAVERN.SC.XML");
  add(graph, R"(<class id="ITavern" cpp_class="CVXTavern" parent="" altid="Tavern"/>)",
      "ITAVERN.SC.XML");
  add(graph, R"(<class id="TTavern" cpp_class="CVXTavern" parent="" altid="Tavern"/>)",
      "TTAVERN.SC.XML");
  add(graph, R"(<class id="Tavern" cpp_class="CVXTavern" parent=""/>)", "TAVERN.SC.XML");
  graph.link();
  REQUIRE(graph.size() == 5);

  // A real id shadows every altid claiming the same name.
  const ClassIndex real = graph.lookup("Tavern");
  REQUIRE(real != kNoClass);
  CHECK(graph.at(real).id == "Tavern");

  // find() never consults altid at all: `parent` is always an id.
  CHECK(graph.find("Tavern") == real);

  const ClassIndex inn = graph.lookup("Inn");
  REQUIRE(inn != kNoClass);
  CHECK(graph.at(inn).id == "Inn");

  CHECK(graph.lookup("NoSuchClass") == kNoClass);

  const auto issues = graph.validate();
  int collisions = 0;
  for (const auto& issue : issues) {
    if (issue.kind == ClassIssueKind::altid_collision) ++collisions;
  }
  // GTavern, ITavern and TTavern all lose to the real `Tavern`; `Inn` names
  // itself, which is not a collision.
  CHECK(collisions == 3);
}

TEST(class_graph_falls_back_to_altid_when_no_id_matches) {
  ClassGraph graph;
  add(graph, R"(<class id="GShipyard1" cpp_class="CVXTownHall" parent="" altid="Shipyard 1"/>)",
      "S1.SC.XML");
  add(graph, R"(<class id="GShipyard2" cpp_class="CVXTownHall" parent="" altid="Shipyard 2"/>)",
      "S2.SC.XML");
  graph.link();

  const ClassIndex first = graph.lookup("Shipyard 1");
  REQUIRE(first != kNoClass);
  CHECK(graph.at(first).id == "GShipyard1");
  CHECK(graph.find("Shipyard 1") == kNoClass);
  CHECK(graph.lookup("Shipyard 4") == kNoClass);  // an importsettlement value, not a class
}

TEST(class_graph_keeps_the_first_of_two_classes_claiming_one_id) {
  ClassGraph graph;
  CHECK(add(graph, R"(<class id="A" cpp_class="CVXDecor" parent="" entity="first.ent.xml"/>)",
            "FIRST.SC.XML")
            .ok());
  // Reported, not fatal: a second file claiming an id is a packaging mistake,
  // not a reason to lose the other 844 classes.
  CHECK(add(graph, R"(<class id="A" cpp_class="CVXUnit" parent="" entity="second.ent.xml"/>)",
            "SECOND.SC.XML")
            .ok());
  graph.link();
  REQUIRE(graph.size() == 1);
  CHECK(graph.at(0).cpp_class == "CVXDecor");

  const auto issues = graph.validate();
  REQUIRE(issues.size() == 1);
  CHECK(issues[0].kind == ClassIssueKind::duplicate_id);
  CHECK(issues[0].source == "SECOND.SC.XML");
  CHECK(issues[0].detail == "id=A");
}

TEST(class_graph_tolerates_a_dangling_parent) {
  ClassGraph graph;
  add(graph, R"(<class id="Object" cpp_class="CVXDecor" parent=""/>)", "OBJECT.SC.XML");
  add(graph, R"(<class id="Orphan" cpp_class="CVXDecor" parent="Missing">
                  <properties radius="7"/>
                </class>)",
      "ORPHAN.SC.XML");
  graph.link();

  REQUIRE(graph.size() == 2);
  const ClassIndex orphan = graph.find("Orphan");
  REQUIRE(orphan != kNoClass);
  // The class loads and resolves; it simply becomes a root of its own.
  CHECK(graph.depth(orphan) == 1);
  CHECK(value_of(graph.resolved_properties(orphan), "radius") == "7");
  CHECK(graph.roots().size() == 2);

  const auto issues = graph.validate();
  REQUIRE(issues.size() == 1);
  CHECK(issues[0].kind == ClassIssueKind::dangling_parent);
  CHECK(issues[0].detail == "parent=Missing");
}

TEST(class_graph_reports_a_cycle_without_hanging) {
  ClassGraph graph;
  add(graph, R"(<class id="A" cpp_class="CVXDecor" parent="B"/>)", "A.SC.XML");
  add(graph, R"(<class id="B" cpp_class="CVXDecor" parent="A"/>)", "B.SC.XML");
  graph.link();

  const ClassIndex a = graph.find("A");
  REQUIRE(a != kNoClass);
  // Ancestry stops at the first repeat rather than looping for ever.
  const auto chain = graph.ancestry(a);
  CHECK(chain.size() == 2);
  CHECK(graph.resolved_properties(a).empty());

  const auto issues = graph.validate();
  REQUIRE(issues.size() == 2);
  CHECK(issues[0].kind == ClassIssueKind::cycle);
  CHECK(issues[1].kind == ClassIssueKind::cycle);
}

TEST(class_graph_reports_dangling_resources_without_failing_the_load) {
  // The retail data has 38 dangling references and the retail engine runs. A
  // loader that rejected a class over a missing select sound would lose the
  // outposts and the stonehenges.
  ClassGraph graph;
  REQUIRE(add(graph,
              R"(<class id="BaseTavern" cpp_class="CVXTavern" parent=""
                        entity="Buildings/Tavern/Tavern_as.ent.xml"
                        entity_winter="Buildings/Tavern/Tavern_w.ent.xml">
                   <properties icon="gameres/icons/Anibal.bmp" projectile_class="Arrow"/>
                   <behavior script="data/subai/present.vs"/>
                   <method sig="attack" vs="data/subai/missing.vs" verify="data/subai/present.vs"/>
                   <sounds select="Sounds/selection/gone.wav" command=""/>
                   <value0 icon="gameres/infobar/missing.bmp"/>
                 </class>)",
              "BASETAVERN.SC.XML")
              .ok());
  graph.link();
  REQUIRE(graph.size() == 1);

  FakeProbe probe;
  probe.add("data/subai/present.vs");
  probe.add("Buildings/Tavern/Tavern_w.ent.xml");

  const auto issues = graph.validate(&probe);
  int entities = 0, scripts = 0, sounds = 0, icons = 0, class_refs = 0;
  for (const auto& issue : issues) {
    switch (issue.kind) {
      case ClassIssueKind::missing_entity: ++entities; break;
      case ClassIssueKind::missing_script: ++scripts; break;
      case ClassIssueKind::missing_sound: ++sounds; break;
      case ClassIssueKind::missing_icon: ++icons; break;
      case ClassIssueKind::dangling_class_ref: ++class_refs; break;
      default: break;
    }
  }
  CHECK(entities == 1);     // the summer/spring/autumn path; winter exists
  CHECK(scripts == 1);      // the missing vs; the verify script and behavior exist
  CHECK(sounds == 1);       // the select path; an empty channel means silent, not missing
  CHECK(icons == 2);        // the class icon and the info-bar slot icon
  CHECK(class_refs == 1);   // projectile_class="Arrow" names no class here

  // And the class is still fully usable.
  CHECK(graph.property(0, "icon") == "gameres/icons/Anibal.bmp");
}

TEST(class_graph_validates_class_name_properties_through_altid) {
  ClassGraph graph;
  add(graph, R"(<class id="Arrow" cpp_class="CVXScriptObj" parent=""/>)", "ARROW.SC.XML");
  add(graph, R"(<class id="GSentry1" cpp_class="CVXUnit" parent="" altid="Sentry One"/>)",
      "SENTRY.SC.XML");
  add(graph,
      R"(<class id="BBowman" cpp_class="CVXUnit" parent="">
           <properties projectile_class="Arrow" sentry_class_name="Sentry One"
                       select_class="NoSuchClass" importsettlement="Shipyard 4"/>
         </class>)",
      "BBOWMAN.SC.XML");
  graph.link();

  const auto issues = graph.validate();
  // `importsettlement` names a settlement template, not a class: validating it
  // as a class reference invents failures (see docs/data-model.md).
  REQUIRE(issues.size() == 1);
  CHECK(issues[0].kind == ClassIssueKind::dangling_class_ref);
  CHECK(issues[0].detail == "select_class=NoSuchClass");
}

TEST(class_graph_interns_every_string_it_keeps) {
  // The documents a graph is built from do not outlive the loader in the real
  // engine, so nothing the graph hands back may point into them.
  ClassGraph graph;
  {
    std::string document =
        R"(<class id="Temporary" cpp_class="CVXDecor" parent="" entity="a.ent.xml">
             <properties display_name="A name that is not in this binary's literals"/>
           </class>)";
    REQUIRE(add(graph, document, std::string("TEMPORARY.SC.XML")).ok());
    document.assign(document.size(), 'x');  // scribble over the buffer
  }
  graph.link();

  REQUIRE(graph.size() == 1);
  CHECK(graph.at(0).id == "Temporary");
  CHECK(graph.at(0).source == "TEMPORARY.SC.XML");
  CHECK(graph.property(0, "display_name") == "A name that is not in this binary's literals");
}

TEST(class_graph_survives_a_long_inheritance_chain) {
  // Retail is seven deep. Nothing in the format says it has to be, and a
  // resolver that recursed would be a stack overflow waiting for bad data.
  constexpr int kDepth = 2000;
  ClassGraph graph;
  std::vector<std::string> documents;
  documents.reserve(kDepth);
  for (int i = 0; i < kDepth; ++i) {
    std::string parent = i == 0 ? std::string() : "C" + std::to_string(i - 1);
    documents.push_back("<class id=\"C" + std::to_string(i) + "\" cpp_class=\"CVXDecor\" parent=\"" +
                        parent + "\"><properties depth=\"" + std::to_string(i) +
                        "\" seeded=\"yes\"/></class>");
  }
  bool all_added = true;
  for (int i = 0; i < kDepth; ++i) {
    all_added = all_added && add(graph, documents[static_cast<std::size_t>(i)], "generated").ok();
  }
  REQUIRE(all_added);
  graph.link();

  REQUIRE(graph.size() == kDepth);
  CHECK(graph.max_depth() == kDepth);
  const ClassIndex leaf = graph.find("C" + std::to_string(kDepth - 1));
  REQUIRE(leaf != kNoClass);
  CHECK(graph.depth(leaf) == kDepth);
  const auto resolved = graph.resolved_properties(leaf);
  CHECK(value_of(resolved, "depth") == std::to_string(kDepth - 1));
  CHECK(value_of(resolved, "seeded") == "yes");
  CHECK(graph.validate().empty());
}
