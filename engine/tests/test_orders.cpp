// Selection, the default order, and issuing: sim/orders.hpp.
//
// No game data in this file. Every class and every `<commands>` document is a
// literal written here, but the *shapes* are transcriptions: the `<defaultcmd>`
// blocks below are the shipped ones from `UNIT.SC.XML`, `HERO.SC.XML`,
// `SENTRY.SC.XML` and `CCATAPULT.SC.XML`, verb for verb and in order, because
// the cases that carry the reading only exist in that arrangement.
//
// The cases that carry the reading are:
//
//   * **A five-deep candidate list, not a mapping.** `Unit` against `Building`
//     is (attack_independent, capture, attack, enter, approach) and which one
//     runs is decided by the `verify=` scripts, not by a table in the engine.
//   * **Most derived `<defaultcmd target>` wins.** `UNIT.SC.XML`'s comment
//     `<!-- no capture for towers, gates and walls -->` is only true if
//     `target="Tower"` shadows `target="Building"`, and a hero is only ever
//     joined rather than walked up to if `target="Hero"` shadows
//     `target="Unit"`.
//   * **A child's block replaces the one it inherits, but two blocks for one
//     target in one file concatenate.** `RAMUNIT.SC.XML` declares
//     `target="Unit"` as (approach) alone with no `<nodefcmdinherit/>`, which
//     only means anything if it erases `Unit`'s (attack, ...) -- so between
//     classes it replaces. `HERO.SC.XML` declares `target="Unit"` twice in one
//     file, and replacing there would make the first of the two dead code --
//     so within a class it concatenates. The control below computes both
//     `ClassGraph::resolved_default_cmds` and `default_command_candidates` and
//     asserts they disagree on exactly that.
//   * **`ctrl="1"` partitions the list.** `Unit`'s ground block is
//     (move, advance[ctrl]); if the modifier only *added* candidates then
//     unconditional `move` would win always and `advance` would be dead code.
//   * **The class graph drops one half of an overloaded `<method sig>`.**
//     `Unit` declares `attack` twice, and only the first carries
//     `verify="unit_attack_verify.vs"`. Pinned here, because until it is fixed
//     `attack` resolves unconditionally and a unit right-clicking its own town
//     hall attacks it.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/feedback.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;
using namespace imperivm::core::script;

namespace {

std::span<const std::byte> bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// --------------------------------------------------------------------------
// fixtures
// --------------------------------------------------------------------------

/// The rows the `<defaultcmd>` blocks below name, with the `method=` and
/// `costgold=` values the shipped files carry where they carry any.
/// `stay_hidden` is deliberately absent: it appears in three shipped
/// `<defaultcmd>` blocks and in no `<commands>` file at all, which is the
/// engine's `Unknown Command: %s` case.
constexpr std::string_view kCommandsXml = R"(<commands>
<cmd name="move" priority="0" offset="1" key="m"/>
<cmd name="advance" priority="5" offset="1" cursor="attack"/>
<cmd name="attack" priority="1" offset="1" key="a" cursor="attack"/>
<cmd name="attack_unit_type" cursor="attack"/>
<cmd name="approach" priority="1"/>
<cmd name="attack_independent" priority="1" cursor="do_something"/>
<cmd name="capture" priority="2" offset="1" key="u" cursor="do_something"/>
<cmd name="enter" priority="4" cursor="do_something"/>
<cmd name="attach" priority="3" cursor="do_something"/>
<cmd name="attack_ground" priority="1" cursor="attack"/>
<cmd name="catapult_attack" priority="1" method="attack" cursor="attack"/>
<cmd name="trainhastatus" costgold="60" costfood="20" execdelay="9000"
     method="train" param="RHastatus"/>
</commands>)";

/// A class tree with the shipped `<defaultcmd>` blocks on it.
///
/// `Object` -> `Unit` -> {`Military` -> `Hero`, `Sentry`, `Catapult`}, and
/// `Building` -> `Tower`, which is the part of the real tree these cases touch.
ClassGraph fixture_graph() {
  ClassGraph graph;
  graph.add(bytes(R"(<class id="Object" cpp_class="CVXDecor" parent=""/>)"), "object.sc.xml");

  // `UNIT.SC.XML`, blocks and verbs verbatim, plus the `<method>` rows the
  // verbs resolve through. `attack` is declared twice exactly as it ships.
  graph.add(bytes(R"(<class id="Unit" cpp_class="CVXUnit" parent="Object">
      <properties maxhealth="200" selection_radius="15"/>
      <method sig="idle"    vs="data/subai/unit_idle.vs"/>
      <method sig="move"    vs="data/subai/unit_move.vs"/>
      <method sig="advance" vs="data/subai/unit_advance.vs"/>
      <method sig="approach" vs="data/subai/unit_approach.vs"/>
      <method sig="attach"  vs="data/subai/unit_attach.vs" verify="data/subai/unit_attach_verify.vs"/>
      <method sig="attack"  vs="data/subai/unit_attack.vs" verify="data/subai/unit_attack_verify.vs"/>
      <method sig="attack"  vs="data/subai/unit_advance.vs"/>
      <method sig="enter"   vs="data/subai/unit_enter.vs" verify="data/subai/unit_enter_verify.vs"/>
      <method sig="capture" vs="data/subai/unit_capture.vs" verify="data/subai/unit_capture_verify.vs"/>
      <method sig="attack_independent" vs="data/subai/unit_capture.vs" verify="data/subai/attack_independent_verify.vs"/>
      <method sig="attack_unit_type" vs="data/subai/unit_attack_unit_type.vs" verify="data/subai/unit_attack_verify.vs"/>
      <method sig="stay_hidden" vs="data/subai/unit_stay_hidden.vs" verify="data/subai/unit_stay_verify.vs"/>
      <defaultcmd target="">
        <cmd name="move"/>
        <cmd name="advance" ctrl="1"/>
      </defaultcmd>
      <defaultcmd target="Unit">
        <cmd name="attack"/>
        <cmd name="stay_hidden"/>
        <cmd name="approach"/>
        <cmd name="attack_unit_type" ctrl="1"/>
      </defaultcmd>
      <defaultcmd target="Hero">
        <cmd name="attach"/>
        <cmd name="attack"/>
        <cmd name="approach"/>
      </defaultcmd>
      <defaultcmd target="Tower">
        <cmd name="attack"/>
        <cmd name="enter"/>
        <cmd name="approach"/>
      </defaultcmd>
      <defaultcmd target="Building">
        <cmd name="attack_independent"/>
        <cmd name="capture"/>
        <cmd name="attack"/>
        <cmd name="enter"/>
        <cmd name="approach"/>
      </defaultcmd>
    </class>)"),
            "unit.sc.xml");

  graph.add(bytes(R"(<class id="Military" cpp_class="CVXUnit" parent="Unit"/>)"),
            "military.sc.xml");

  // `HERO.SC.XML`: two `target="Unit"` blocks, eight lines apart, and no
  // `approach` in either -- the fallback comes from `Unit`.
  graph.add(bytes(R"(<class id="Hero" cpp_class="CVXHero" parent="Military">
      <method sig="moveinfight" vs="data/subai/hero_moveinfight.vs" verify="data/subai/hero_moveinfight_verify.vs"/>
      <defaultcmd target="Unit">
        <cmd name="attack"/>
        <cmd name="attack_unit_type" ctrl="1"/>
      </defaultcmd>
      <defaultcmd target="Unit">
        <cmd name="stay_hidden"/>
      </defaultcmd>
    </class>)"),
            "hero.sc.xml");

  // `SENTRY.SC.XML`: `<nodefcmdinherit/>`, so a sentry has no ground block at
  // all and right-clicking terrain with one selected does nothing.
  graph.add(bytes(R"(<class id="Sentry" cpp_class="CVXUnit" parent="Unit">
      <nodefcmdinherit/>
      <defaultcmd target="Unit">
        <cmd name="attack"/>
        <cmd name="stay_hidden"/>
      </defaultcmd>
    </class>)"),
            "sentry.sc.xml");

  // `CCATAPULT.SC.XML`: both halves of the ground block spelled out, because
  // it is overriding an inherited (move, advance[ctrl]) pair on both sides.
  graph.add(bytes(R"(<class id="Catapult" cpp_class="CVXUnit" parent="Unit">
      <method sig="attack_ground" vs="data/subai/catapult_attack_ground.vs" verify="data/subai/catapult_attack_ground_verify.vs"/>
      <defaultcmd target="">
        <cmd name="attack_ground"/>
        <cmd name="attack_ground" ctrl="1"/>
      </defaultcmd>
    </class>)"),
            "catapult.sc.xml");

  graph.add(bytes(R"(<class id="Building" cpp_class="CVXTownHall" parent="Object">
      <properties maxhealth="1000"/>
    </class>)"),
            "building.sc.xml");
  // `TOWER.SC.XML` really does ship `non_selectable="1"`; so do `Wall` and
  // `FakeTower`, and nothing else in the 823 class files does.
  graph.add(bytes(R"(<class id="Tower" cpp_class="CVXTownHall" parent="Building">
      <properties non_selectable="1"/>
    </class>)"),
            "tower.sc.xml");
  graph.link();
  return graph;
}

/// A verifier that answers from a script-path allow list. What the class graph
/// binds decides *which* verifier runs; this decides what it says.
class TableVerifier final : public OrderVerifier {
 public:
  std::vector<std::string> pass;
  std::vector<std::string> unknown;
  std::vector<std::string> asked;

  OrderVerdict verify(std::string_view vs_path, ObjectId, const OrderTarget&) override {
    asked.emplace_back(vs_path);
    for (const std::string& entry : unknown) {
      if (entry == vs_path) return OrderVerdict::unknown;
    }
    for (const std::string& entry : pass) {
      if (entry == vs_path) return OrderVerdict::pass;
    }
    return OrderVerdict::fail;
  }
};

struct Fixture {
  ClassGraph graph = fixture_graph();
  World world;
  CommandSystem commands;
  CommandTable table;
  // The group spread asks whether a unit belongs to a hero's army, which is
  // this system's answer. Nothing else in this file needs it.
  HeroSystem heroes;

  ClassIndex unit = kNoClass;
  ClassIndex military = kNoClass;
  ClassIndex hero = kNoClass;
  ClassIndex sentry = kNoClass;
  ClassIndex catapult = kNoClass;
  ClassIndex building = kNoClass;
  ClassIndex tower = kNoClass;

  Fixture() {
    world.set_class_graph(&graph);
    unit = graph.find("Unit");
    military = graph.find("Military");
    hero = graph.find("Hero");
    sentry = graph.find("Sentry");
    catapult = graph.find("Catapult");
    building = graph.find("Building");
    tower = graph.find("Tower");
    (void)table.merge(bytes(kCommandsXml));
    CommandTable copy;
    (void)copy.merge(bytes(kCommandsXml));
    commands.set_table(std::move(copy));
    world.add_system(&commands);
    world.add_system(&heroes);

    // Player 1 and player 2 control themselves and nobody else; player 3 is
    // player 1's ally with control shared both ways. `0x35` is the shipped
    // self-relation word, `sim/player.hpp`.
    for (PlayerId p = 0; p < 4; ++p) {
      world.players().set(p, p, Relation::share_control, true);
    }
    // Player 3 *grants* player 1 control: the row is the granter's.
    world.players().set(3, 1, Relation::share_control, true);
  }

  ObjectId spawn(ClassIndex cls, PlayerId owner = 1, Point at = Point{100, 100}) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, cls);
    world.set_position(id, at);
    world.set_owner(id, owner);
    world.set_health(id, 100);
    return id;
  }

  std::vector<std::string> candidates(ClassIndex actor, ClassIndex target, bool modifier) {
    std::vector<OrderCandidate> out;
    default_command_candidates(graph, actor, target, modifier, out);
    std::vector<std::string> names;
    for (const OrderCandidate& c : out) names.emplace_back(c.command);
    return names;
  }

  std::vector<std::string> verbs(ObjectId id) const {
    std::vector<std::string> out;
    const CommandQueue* q = commands.find(id);
    if (q == nullptr) return out;
    for (const Command& command : q->entries) out.emplace_back(command.verb);
    return out;
  }
};

}  // namespace

// --------------------------------------------------------------------------
// selection
// --------------------------------------------------------------------------

TEST(a_selection_is_ordered_and_duplicate_free) {
  Selection selection;
  CHECK(selection.add(7));
  CHECK(selection.add(3));
  CHECK(selection.add(11));
  // A second add is not an error, it is a no-op that must not disturb order:
  // a drag box over units already selected must not reshuffle them.
  CHECK(!selection.add(3));
  REQUIRE(selection.size() == 3);
  CHECK(selection.at(0) == 7);
  CHECK(selection.at(1) == 3);
  CHECK(selection.at(2) == 11);
  CHECK(selection.at(3) == kNoObject);
  CHECK(selection.index_of(3) == 1);
  CHECK(selection.index_of(99) == 3);

  // Removing from the middle keeps the rest in order.
  CHECK(selection.remove(3));
  CHECK(!selection.remove(3));
  REQUIRE(selection.size() == 2);
  CHECK(selection.at(0) == 7);
  CHECK(selection.at(1) == 11);

  CHECK(!selection.toggle(7));
  CHECK(selection.toggle(7));
  CHECK(selection.at(1) == 7);  // toggled back on, at the end

  // `kNoObject` is never a member.
  CHECK(!selection.add(kNoObject));
}

TEST(pruning_drops_the_dead_and_the_gone) {
  Fixture f;
  const ObjectId alive = f.spawn(f.unit);
  const ObjectId dying = f.spawn(f.unit);
  Selection selection;
  selection.assign(std::vector<ObjectId>{alive, dying, 4242});
  f.world.set_health(dying, 0);  // `IsAlive` is health above zero
  CHECK(selection.prune(f.world) == 2);
  REQUIRE(selection.size() == 1);
  CHECK(selection.at(0) == alive);
}

TEST(a_selection_table_keeps_one_slot_per_screen) {
  // Sixteen slots so that one process may stand in for several screens; the
  // script entry points reach exactly the one `local_player` names, which is
  // `selection_entry_points_answer_only_the_local_screen` below.
  SelectionTable table;
  CHECK(table.player(1).empty());

  CHECK(table.select(1, 10));
  CHECK(table.select(1, 11));
  CHECK(table.select(2, 20));
  CHECK(table.player(1).size() == 2);
  CHECK(table.player(2).size() == 1);

  table.clear(1);
  CHECK(table.player(1).empty());
  CHECK(table.player(2).size() == 1);  // one screen's clear leaves the others

  // An out-of-range player is inert rather than out of bounds.
  CHECK(table.select(200, 5));
  CHECK(table.player(1).empty());
}

TEST(a_swap_appends_rather_than_substituting_in_place) {
  // `SwapSelectedObj` is Deselect's helper then Select's (0x004c84e0), so the
  // replacement lands at the end. This test is the correction: it read
  // `at(1) == 77` when the swap was a substitution in place.
  Selection selection;
  selection.assign(std::vector<ObjectId>{10, 11, 12});
  CHECK(selection.swap(11, 77));
  REQUIRE(selection.size() == 3);
  CHECK(selection.at(0) == 10);
  CHECK(selection.at(1) == 12);
  CHECK(selection.at(2) == 77);

  // `from` absent is a no-op, and nothing is appended.
  CHECK(!selection.swap(4242, 88));
  CHECK(selection.size() == 3);

  // A null `to` leaves the removal standing.
  CHECK(selection.swap(10, kNoObject));
  REQUIRE(selection.size() == 2);
  CHECK(selection.at(0) == 12);

  // And a `to` that is already selected is removed-from, not duplicated: the
  // druid's wolf may already be in the list.
  CHECK(selection.swap(12, 77));
  REQUIRE(selection.size() == 1);
  CHECK(selection.at(0) == 77);
}

TEST(last_selection_time_is_minus_one_until_the_selection_changes) {
  SelectionTable table;
  CHECK(table.last_selected(10) == -1);

  table.select(1, 10);
  // The stamp is the selection-changed handler's, not `select`'s: until
  // somebody says the selection changed, nothing has been stamped.
  CHECK(table.last_selected(10) == -1);
  table.note_selection_changed(1, 500);
  CHECK(table.last_selected(10) == 500);

  // Every member of the new selection is restamped, not only the newcomer.
  table.select(1, 11);
  table.note_selection_changed(1, 900);
  CHECK(table.last_selected(10) == 900);
  CHECK(table.last_selected(11) == 900);

  // And the stamp outlives the selection: `BUILDINGSADVICE9.VS` asks about
  // buildings the player is no longer looking at.
  table.clear(1);
  table.note_selection_changed(1, 1200);
  CHECK(table.last_selected(10) == 900);
  CHECK(table.last_selected(11) == 900);
  CHECK(table.last_selected(4242) == -1);
}

TEST(swapping_and_forgetting_reach_every_players_selection) {
  SelectionTable table;
  table.select(1, 10);
  table.select(1, 20);
  table.select(2, 10);
  table.select(3, 99);
  CHECK(table.swap_object(10, 77) == 2);
  // Appended, so 20 keeps the place 10 used to have and 77 goes last.
  CHECK(table.player(1).at(0) == 20);
  CHECK(table.player(1).at(1) == 77);
  CHECK(table.player(2).at(0) == 77);
  CHECK(table.player(3).at(0) == 99);
  CHECK(table.forget(77) == 2);
  CHECK(table.player(1).size() == 1);
}

TEST(a_selection_table_round_trips) {
  SelectionTable table;
  table.select(1, 10);
  table.select(1, 11);
  table.select(5, 500);
  table.select(5, 40);
  table.note_selection_changed(5, 4321);
  table.select(5, 900);
  table.note_selection_changed(5, 8888);
  table.clear(5);  // stamped, but empty

  std::vector<std::byte> bytes_out;
  table.serialize(bytes_out);

  SelectionTable loaded;
  REQUIRE(loaded.deserialize(bytes_out).ok());
  REQUIRE(loaded.player(1).size() == 2);
  CHECK(loaded.player(1).at(0) == 10);
  CHECK(loaded.player(1).at(1) == 11);
  CHECK(loaded.player(5).empty());
  // The stamps ride along, and they are the reason an empty selection is not
  // an empty section: 500 was selected once and the tutorial can still tell.
  // More than one, so that the ascending order the reader relies on is
  // exercised rather than asserted: a single stamp round-trips whatever order
  // it is in.
  CHECK(loaded.last_selected(40) == 8888);
  CHECK(loaded.last_selected(500) == 8888);
  CHECK(loaded.last_selected(900) == 8888);
  CHECK(loaded.last_selected(10) == -1);

  // Truncation at every length is refused rather than read past.
  for (std::size_t cut = 0; cut < bytes_out.size(); ++cut) {
    SelectionTable partial;
    CHECK(!partial.deserialize(std::span(bytes_out).first(cut)).ok());
  }
  // A bad magic is refused even at the full length.
  std::vector<std::byte> corrupt = bytes_out;
  corrupt[0] = std::byte{0xFF};
  SelectionTable bad;
  CHECK(!bad.deserialize(corrupt).ok());

  // A file from before the stamps arrived is refused rather than widened: it
  // carried a per-player latch where the counts now are, so reading it as this
  // version would produce selections out of a bitfield.
  std::vector<std::byte> older = bytes_out;
  older[4] = std::byte{1};
  SelectionTable stale;
  CHECK(!stale.deserialize(older).ok());

  // Stamps out of order are refused rather than sorted. Ascending and unique is
  // what every read of the table relies on, and a file is not a promise.
  SelectionTable two;
  two.select(1, 10);
  two.select(1, 20);
  two.note_selection_changed(1, 7);
  std::vector<std::byte> stamped;
  two.serialize(stamped);
  SelectionTable round;
  REQUIRE(round.deserialize(stamped).ok());
  // The last two stamps are the tail: swap the two object ids to break the
  // order without changing the length.
  const std::size_t tail = stamped.size() - 2 * (4 + 8);
  std::swap(stamped[tail], stamped[tail + 12]);
  SelectionTable unsorted;
  CHECK(!unsorted.deserialize(stamped).ok());
}

TEST(non_selectable_classes_and_dead_and_garrisoned_objects_are_not_selectable) {
  Fixture f;
  const ObjectId soldier = f.spawn(f.unit);
  const ObjectId hall = f.spawn(f.building);
  const ObjectId tower = f.spawn(f.tower);
  CHECK(is_selectable(f.world, soldier));
  // A building with no `non_selectable` flag is selectable: that is the only
  // thing the flag's presence on exactly three classes can mean.
  CHECK(is_selectable(f.world, hall));
  // `TOWER.SC.XML`, `WALL.SC.XML`, `FAKETOWER.SC.XML`.
  CHECK(!is_selectable(f.world, tower));

  f.world.set_health(soldier, 0);
  CHECK(!is_selectable(f.world, soldier));
  f.world.set_health(soldier, 100);

  CHECK(f.world.put_in_holder(soldier, hall));
  CHECK(!is_selectable(f.world, soldier));

  CHECK(!is_selectable(f.world, 999999));
}

TEST(commandable_is_share_control_not_ownership) {
  // `GROUP_UNITSOUT.VS` filters with `DiplGetShareControl(u.player, player)`
  // before it selects or commands anything.
  Fixture f;
  const ObjectId mine = f.spawn(f.unit, 1);
  const ObjectId theirs = f.spawn(f.unit, 2);
  const ObjectId allys = f.spawn(f.unit, 3);
  CHECK(is_commandable(f.world, mine, 1));
  CHECK(!is_commandable(f.world, theirs, 1));
  // Player 3 grants player 1 control. Not ownership: a shared-control ally's
  // units really are commandable, and that is the rule the shipped script uses.
  CHECK(is_commandable(f.world, allys, 1));
  CHECK(!is_commandable(f.world, mine, 2));

  f.world.set_health(mine, 0);
  CHECK(!is_commandable(f.world, mine, 1));
  CHECK(!is_commandable(f.world, 999999, 1));
}

// --------------------------------------------------------------------------
// candidates
// --------------------------------------------------------------------------

TEST(the_ground_block_is_partitioned_by_the_modifier) {
  Fixture f;
  // `Unit`: (move, advance[ctrl]). If ctrl merely *added* `advance`, `move`
  // would be first and unconditional and `advance` would never run.
  CHECK(f.candidates(f.unit, kNoClass, false) == std::vector<std::string>{"move"});
  CHECK(f.candidates(f.unit, kNoClass, true) == std::vector<std::string>{"advance"});

  // `CCatapult` overrides both halves with the same verb, which is why both
  // halves are spelled out in the shipped file -- and its block *replaces* the
  // inherited (move, advance[ctrl]), so `move` is not behind it.
  CHECK(f.candidates(f.catapult, kNoClass, false) == std::vector<std::string>{"attack_ground"});
  CHECK(f.candidates(f.catapult, kNoClass, true) == std::vector<std::string>{"attack_ground"});
}

TEST(most_derived_target_class_wins) {
  Fixture f;
  // `<!-- no capture for towers, gates and walls -->`: `target="Tower"` must
  // shadow `target="Building"`, or `capture` reappears.
  const std::vector<std::string> at_tower = f.candidates(f.unit, f.tower, false);
  CHECK((at_tower == std::vector<std::string>{"attack", "enter", "approach"}));

  const std::vector<std::string> at_building = f.candidates(f.unit, f.building, false);
  CHECK((at_building == std::vector<std::string>{"attack_independent", "capture", "attack",
                                                 "enter", "approach"}));

  // A hero is a unit, and `target="Unit"` is declared *before* `target="Hero"`.
  // Document order would offer (attack, stay_hidden, approach) and never reach
  // `attach`, so right-clicking your own hero would walk up to it.
  const std::vector<std::string> at_hero = f.candidates(f.unit, f.hero, false);
  CHECK((at_hero == std::vector<std::string>{"attach", "attack", "approach"}));
}

TEST(a_child_block_replaces_but_two_blocks_in_one_file_concatenate) {
  Fixture f;
  // `HERO.SC.XML` declares `target="Unit"` twice: both halves are live, and
  // together they replace `Unit`'s own block rather than extending it, so
  // `Unit`'s `approach` is gone.
  const std::vector<std::string> hero_at_unit = f.candidates(f.hero, f.unit, false);
  CHECK((hero_at_unit == std::vector<std::string>{"attack", "stay_hidden"}));
  // The ctrl half of the first of the two blocks is reachable too.
  CHECK((f.candidates(f.hero, f.unit, true) ==
         std::vector<std::string>{"attack_unit_type"}));

  // A class with no blocks of its own inherits its parent's whole table.
  CHECK((f.candidates(f.military, f.unit, false) ==
         std::vector<std::string>{"attack", "stay_hidden", "approach"}));
}

TEST(nodefcmdinherit_stops_the_walk) {
  Fixture f;
  // `UNIT.XML` marks move, attack, capture, advance and approach
  // `<nsrc obj="Sentry"/>`; `<nodefcmdinherit/>` is what keeps a sentry from
  // reaching them here, since `<src>`/`<nsrc>` are not implemented.
  CHECK(f.candidates(f.sentry, kNoClass, false).empty());
  CHECK(f.candidates(f.sentry, f.building, false).empty());
  CHECK((f.candidates(f.sentry, f.unit, false) ==
         std::vector<std::string>{"attack", "stay_hidden"}));
}

TEST(a_class_graph_with_no_blocks_and_a_bad_index_yield_nothing) {
  Fixture f;
  std::vector<OrderCandidate> out;
  CHECK(default_command_candidates(f.graph, kNoClass, f.unit, false, out) == 0);
  CHECK(default_command_candidates(f.graph, 100000, f.unit, false, out) == 0);
  CHECK(default_command_candidates(f.graph, f.building, f.unit, false, out) == 0);
  CHECK(out.empty());
}

// --------------------------------------------------------------------------
// the two divergences from the class graph, pinned
// --------------------------------------------------------------------------

TEST(control_resolved_default_cmds_loses_a_block_that_candidates_keeps) {
  // Not a claim about which is right stated in a comment: the two answers are
  // computed here and asserted different, so the day `class_graph.cpp` changes
  // its merge this test says so.
  Fixture f;
  const std::vector<DefaultCommandBlock> merged = f.graph.resolved_default_cmds(f.hero);
  const DefaultCommandBlock* at_unit = nullptr;
  std::size_t blocks_for_unit = 0;
  for (const DefaultCommandBlock& block : merged) {
    if (block.target == "Unit") {
      ++blocks_for_unit;
      at_unit = &block;
    }
  }
  REQUIRE(at_unit != nullptr);
  // Replacement keyed on `target` collapses `HERO.SC.XML`'s two blocks into
  // one, and the one it keeps is the second: (stay_hidden).
  CHECK(blocks_for_unit == 1);
  REQUIRE(at_unit->cmds.size() == 1);
  CHECK(at_unit->cmds[0].name == "stay_hidden");

  // Concatenation keeps `attack`, so a hero can attack a unit by right-clicking
  // it. That verb is absent from the merged block above.
  const std::vector<std::string> kept = f.candidates(f.hero, f.unit, false);
  bool has_attack = false;
  for (const std::string& name : kept) has_attack |= name == "attack";
  CHECK(has_attack);
}

TEST(control_the_class_graph_drops_the_verifier_of_an_overloaded_method_sig) {
  // `Unit` declares `attack` twice: first with `vs="unit_attack.vs"` and
  // `verify="unit_attack_verify.vs"` (the object form), then with
  // `vs="unit_advance.vs"` and no verifier (the point form). `upsert` keeps
  // the last, so the verifier is gone -- and `attack` becomes unconditional.
  //
  // Measured over the shipped data: 21 of the 173 `<defaultcmd>` rows resolve
  // to a different `verify=` under first-wins than under last-wins, 20 of them
  // `attack`. Until this is fixed, a unit right-clicking its own town hall
  // attacks it.
  Fixture f;
  const std::vector<ClassMethod> methods = f.graph.resolved_methods(f.unit);
  const ClassMethod* attack = nullptr;
  std::size_t attack_rows = 0;
  for (const ClassMethod& method : methods) {
    if (method.sig == "attack") {
      ++attack_rows;
      attack = &method;
    }
  }
  REQUIRE(attack != nullptr);
  CHECK(attack_rows == 1);                        // both declarations collapsed into one
  CHECK(attack->vs == "data/subai/unit_advance.vs");  // the *point* form survived
  CHECK(attack->verify.empty());                  // and its verifier went with the other one

  // The methods that are *not* overloaded keep theirs, so this is specific to
  // the duplicate and not a general failure to read `verify=`.
  const ClassMethod* enter = nullptr;
  for (const ClassMethod& method : methods) {
    if (method.sig == "enter") enter = &method;
  }
  REQUIRE(enter != nullptr);
  CHECK(enter->verify == "data/subai/unit_enter_verify.vs");
}

// --------------------------------------------------------------------------
// resolution
// --------------------------------------------------------------------------

TEST(a_unit_enters_its_own_building_and_captures_an_enemys) {
  Fixture f;
  const ObjectId soldier = f.spawn(f.unit, 1);
  const ObjectId mine = f.spawn(f.building, 1);
  const ObjectId theirs = f.spawn(f.building, 2);

  // Own building: `attack_independent` and `capture` verify `IsEnemy` and fail,
  // `attack`'s verifier is the one the class graph has dropped (so it is taken
  // unverified -- see the pinned control above), and `enter` never gets asked.
  // With `attack`'s verifier restored this becomes `enter`; the resolution
  // machinery is the same either way, so the case is written against the
  // verifier set rather than against the bug.
  TableVerifier verifier;
  verifier.pass = {"data/subai/unit_enter_verify.vs"};
  DefaultOrder order = resolve_default_order(f.world, f.table, soldier,
                                             OrderTarget{Point{5, 5}, mine}, false, &verifier);
  CHECK(order.status == DefaultOrderStatus::resolved);
  CHECK(order.command == "attack");
  CHECK(order.unverified == 1);
  // The two verifiers ahead of it really were asked, in order.
  REQUIRE(verifier.asked.size() == 2);
  CHECK(verifier.asked[0] == "data/subai/attack_independent_verify.vs");
  CHECK(verifier.asked[1] == "data/subai/unit_capture_verify.vs");

  // Enemy building: `capture` passes and nothing behind it is consulted.
  TableVerifier enemy;
  enemy.pass = {"data/subai/unit_capture_verify.vs"};
  order = resolve_default_order(f.world, f.table, soldier, OrderTarget{Point{5, 5}, theirs}, false,
                               &enemy);
  CHECK(order.status == DefaultOrderStatus::resolved);
  CHECK(order.command == "capture");
  CHECK(order.verb == "capture");
  CHECK(order.unverified == 0);
  CHECK(enemy.asked.size() == 2);  // attack_independent, then capture
}

TEST(bare_ground_is_move_and_ctrl_ground_is_advance) {
  Fixture f;
  const ObjectId soldier = f.spawn(f.unit);
  TableVerifier verifier;
  DefaultOrder order = resolve_default_order(f.world, f.table, soldier, OrderTarget{Point{9, 9}},
                                             false, &verifier);
  CHECK(order.status == DefaultOrderStatus::resolved);
  CHECK(order.command == "move");
  CHECK(order.unverified == 1);  // `move` binds no `verify=`, and never did
  CHECK(verifier.asked.empty());

  order = resolve_default_order(f.world, f.table, soldier, OrderTarget{Point{9, 9}}, true,
                               &verifier);
  CHECK(order.status == DefaultOrderStatus::resolved);
  CHECK(order.command == "advance");
}

TEST(a_command_named_by_no_commands_row_is_skipped_and_counted) {
  // `stay_hidden` really is one: three shipped `<defaultcmd>` blocks name it
  // and no `<commands>` file declares it. The engine's own message is
  // `Unknown Command: %s`, and a missing row must be skipped rather than
  // resolve to nothing.
  Fixture f;
  const ObjectId soldier = f.spawn(f.unit, 1);
  const ObjectId friend_ = f.spawn(f.unit, 1);

  // A table holding only `approach`: (attack, stay_hidden, approach) then has
  // two rows missing ahead of the one that exists.
  CommandTable sparse;
  (void)sparse.merge(bytes(R"(<commands><cmd name="approach" priority="1"/></commands>)"));
  TableVerifier verifier;
  DefaultOrder order = resolve_default_order(
      f.world, sparse, soldier, OrderTarget{Point{1, 1}, friend_}, false, &verifier);
  CHECK(order.status == DefaultOrderStatus::resolved);
  CHECK(order.command == "approach");
  CHECK(order.unknown_commands == 2);

  // A sentry's list is (attack, stay_hidden) with nothing behind it, so when
  // every row is missing the click resolves to nothing at all.
  const ObjectId watcher = f.spawn(f.sentry, 1);
  CommandTable empty;
  (void)empty.merge(bytes(R"(<commands/>)"));
  order = resolve_default_order(f.world, empty, watcher, OrderTarget{Point{1, 1}, friend_}, false,
                               &verifier);
  CHECK(order.status == DefaultOrderStatus::none);
  CHECK(order.unknown_commands == 2);
}

TEST(an_unverifiable_candidate_blocks_rather_than_falling_through) {
  Fixture f;
  const ObjectId soldier = f.spawn(f.unit, 1);
  const ObjectId theirs = f.spawn(f.building, 2);

  // `attack_independent` cannot be evaluated. Taking `capture` instead would
  // emit a verb the original might not have, so resolution stops.
  TableVerifier verifier;
  verifier.unknown = {"data/subai/attack_independent_verify.vs"};
  verifier.pass = {"data/subai/unit_capture_verify.vs"};
  const DefaultOrder order = resolve_default_order(
      f.world, f.table, soldier, OrderTarget{Point{5, 5}, theirs}, false, &verifier);
  CHECK(order.status == DefaultOrderStatus::blocked);
  CHECK(order.blocked_by == "attack_independent");
  CHECK(order.def == nullptr);

  // No verifier at all is the same thing for every conditional candidate.
  const DefaultOrder no_verifier = resolve_default_order(
      f.world, f.table, soldier, OrderTarget{Point{5, 5}, theirs}, false, nullptr);
  CHECK(no_verifier.status == DefaultOrderStatus::blocked);
  CHECK(no_verifier.blocked_by == "attack_independent");
}

TEST(resolution_survives_a_world_with_no_class_graph_and_a_dead_target) {
  Fixture f;
  const ObjectId soldier = f.spawn(f.unit, 1);
  TableVerifier verifier;

  // A target that has already been despawned is a click on the ground where it
  // used to be: the point half of the order is still good.
  const DefaultOrder gone = resolve_default_order(
      f.world, f.table, soldier, OrderTarget{Point{3, 3}, 987654}, false, &verifier);
  CHECK(gone.status == DefaultOrderStatus::resolved);
  CHECK(gone.command == "move");

  World bare;
  const ObjectId orphan = bare.spawn(NativeClass::unit, nullptr, kNoClass);
  const DefaultOrder none =
      resolve_default_order(bare, f.table, orphan, OrderTarget{Point{}}, false, &verifier);
  CHECK(none.status == DefaultOrderStatus::none);
}

// --------------------------------------------------------------------------
// issuing
// --------------------------------------------------------------------------

TEST(replace_aborts_the_queue_and_append_adds_to_the_back) {
  Fixture f;
  const ObjectId soldier = f.spawn(f.unit);
  const CommandDef* move = f.table.find("move");
  REQUIRE(move != nullptr);

  CHECK(issue_order(f.world, soldier, *move, OrderTarget{Point{10, 20}}, OrderMode::replace) != 0);
  CHECK((f.verbs(soldier) == std::vector<std::string>{"move"}));

  // Shift-click: `AddCommand(false, ...)`. The running command is untouched.
  CHECK(issue_order(f.world, soldier, *move, OrderTarget{Point{30, 40}}, OrderMode::append) != 0);
  CHECK((f.verbs(soldier) == std::vector<std::string>{"move", "move"}));

  const CommandQueue* queue = f.commands.find(soldier);
  REQUIRE(queue != nullptr);
  REQUIRE(queue->entries.size() == 2);
  CHECK((queue->entries[0].point == Point{10, 20}));
  CHECK((queue->entries[1].point == Point{30, 40}));
  // `Unit.GetCommanded`: this came from a player, not from a script.
  CHECK(queue->entries[0].user);
  CHECK(queue->entries[1].user);

  // `SetCommand` replaces the whole queue and aborts what is running.
  CHECK(issue_order(f.world, soldier, *move, OrderTarget{Point{50, 60}}, OrderMode::replace) != 0);
  CHECK((f.verbs(soldier) == std::vector<std::string>{"move"}));
  CHECK((f.commands.find(soldier)->entries[0].point == Point{50, 60}));
}

TEST(a_shift_click_on_a_resting_unit_runs_rather_than_queueing_behind_idle) {
  // `UNIT_IDLE.VS` is a `while (1)` that never returns. A shift-clicked order
  // appended behind it would never start, so an append through the order core
  // ends a resting `idle` first (`CommandSystem::append_order`, a reading).
  Fixture f;
  const ObjectId soldier = f.spawn(f.unit);
  const CommandDef* move = f.table.find("move");
  REQUIRE(move != nullptr);
  (void)f.commands.set_command(f.world, soldier, "idle", Command{});
  CHECK(issue_order(f.world, soldier, *move, OrderTarget{Point{30, 40}}, OrderMode::append) != 0);
  CHECK((f.verbs(soldier) == std::vector<std::string>{"move"}));
  CHECK(f.commands.find(soldier)->entries[0].user);
}

TEST(the_object_argument_wins_over_the_point_and_the_row_supplies_costs) {
  Fixture f;
  const ObjectId soldier = f.spawn(f.unit);
  const ObjectId hall = f.spawn(f.building, 2);
  const CommandDef* capture = f.table.find("capture");
  REQUIRE(capture != nullptr);
  CHECK(issue_order(f.world, soldier, *capture, OrderTarget{Point{7, 7}, hall},
                    OrderMode::replace) != 0);
  const CommandQueue* queue = f.commands.find(soldier);
  REQUIRE(queue != nullptr);
  REQUIRE(!queue->entries.empty());
  CHECK(queue->entries[0].arg_kind == CommandArgKind::object);
  CHECK(queue->entries[0].object == hall);

  // `method=` is what gets queued, not `name`: `catapult_attack` is
  // `method="attack"` in the shipped `CATAPULT.XML`.
  const CommandDef* catapult_attack = f.table.find("catapult_attack");
  REQUIRE(catapult_attack != nullptr);
  const ObjectId engine = f.spawn(f.catapult);
  CHECK(issue_order(f.world, engine, *catapult_attack, OrderTarget{Point{7, 7}, hall},
                    OrderMode::replace) != 0);
  CHECK((f.verbs(engine) == std::vector<std::string>{"attack"}));

  // Costs and `execdelay` ride along from the row, the way `ExecCmd` carries
  // them. They are recorded, not charged.
  const CommandDef* train = f.table.find("trainhastatus");
  REQUIRE(train != nullptr);
  const ObjectId barracks = f.spawn(f.building);
  CHECK(issue_order(f.world, barracks, *train, OrderTarget{Point{}}, OrderMode::append) != 0);
  const CommandQueue* barracks_queue = f.commands.find(barracks);
  REQUIRE(barracks_queue != nullptr);
  REQUIRE(!barracks_queue->entries.empty());
  CHECK(barracks_queue->entries[0].cost_gold == 60);
  CHECK(barracks_queue->entries[0].cost_food == 20);
  CHECK(barracks_queue->entries[0].delay == 9000);
  CHECK(barracks_queue->entries[0].param == "RHastatus");
}

TEST(issuing_to_a_world_with_no_command_system_returns_zero) {
  Fixture f;
  World bare;
  const ObjectId orphan = bare.spawn(NativeClass::unit, nullptr, kNoClass);
  const CommandDef* move = f.table.find("move");
  REQUIRE(move != nullptr);
  CHECK(issue_order(bare, orphan, *move, OrderTarget{Point{}}, OrderMode::replace) == 0);
}

/// **The group spread**, which is what `<cmd offset="1">` is for.
///
/// Eighteen shipped rows carry it and every one is a verb a group is given:
/// `move`, `attack`, `advance`, `capture`, `patrol` among them. Each member
/// keeps its own offset from the group's centroid, so a selection arrives in
/// the shape it set off in. The reader used to drop the attribute and the order
/// path never applied it, so every unit in a selection was sent to the
/// identical cell and they converged on one point.
TEST(a_group_order_keeps_each_members_offset_from_the_centre) {
  Fixture f;
  const auto point_of = [&](ObjectId id) {
    const CommandQueue* q = f.commands.find(id);
    CHECK(q != nullptr);
    CHECK(q != nullptr && !q->entries.empty());
    return q != nullptr && !q->entries.empty() ? q->entries.front().point : Point{};
  };

  // Three units in a row, twenty apart. Their centroid is the middle one.
  const ObjectId left = f.spawn(f.unit, 1);
  const ObjectId middle = f.spawn(f.unit, 1);
  const ObjectId right = f.spawn(f.unit, 1);
  CHECK(f.world.set_position(left, Point{80, 100}));
  CHECK(f.world.set_position(middle, Point{100, 100}));
  CHECK(f.world.set_position(right, Point{120, 100}));

  const std::vector<ObjectId> squad{left, middle, right};
  TableVerifier verifier;
  const OrderReport report = issue_default_order(f.world, f.table, squad,
                                                 OrderTarget{Point{500, 500}},
                                                 OrderMode::replace, false, 1, &verifier);
  CHECK(report.issued == 3);
  CHECK((f.verbs(middle) == std::vector<std::string>{"move"}));
  // The shape survives: the centre lands on the click, the others beside it.
  CHECK(point_of(middle) == (Point{500, 500}));
  CHECK(point_of(left) == (Point{480, 500}));
  CHECK(point_of(right) == (Point{520, 500}));
}

TEST(a_lone_unit_and_an_unmarked_verb_and_an_object_target_take_no_spread) {
  Fixture f;
  const auto point_of = [&](ObjectId id) {
    const CommandQueue* q = f.commands.find(id);
    return q != nullptr && !q->entries.empty() ? q->entries.front().point : Point{};
  };
  TableVerifier verifier;

  // One actor is its own centroid, so there is nothing to spread.
  const ObjectId alone = f.spawn(f.unit, 1);
  CHECK(f.world.set_position(alone, Point{80, 100}));
  const std::vector<ObjectId> one{alone};
  CHECK(issue_default_order(f.world, f.table, one, OrderTarget{Point{500, 500}},
                            OrderMode::replace, false, 1, &verifier)
            .issued == 1);
  CHECK(point_of(alone) == (Point{500, 500}));

  // **A targeted verb is aimed at the object and takes no spread**: every
  // member attacking one soldier wants that soldier, not a ring around it.
  const ObjectId a = f.spawn(f.unit, 1);
  const ObjectId b = f.spawn(f.unit, 1);
  const ObjectId enemy = f.spawn(f.unit, 2);
  CHECK(f.world.set_position(a, Point{80, 100}));
  CHECK(f.world.set_position(b, Point{120, 100}));
  CHECK(f.world.set_position(enemy, Point{900, 900}));
  const std::vector<ObjectId> pair{a, b};
  OrderTarget at_enemy;
  at_enemy.object = enemy;
  at_enemy.point = Point{900, 900};
  CHECK(issue_default_order(f.world, f.table, pair, at_enemy, OrderMode::replace, false, 1,
                            &verifier)
            .issued == 2);
  const CommandQueue* qa = f.commands.find(a);
  REQUIRE(qa != nullptr);
  REQUIRE(!qa->entries.empty());
  CHECK(qa->entries.front().arg_kind == CommandArgKind::object);
  CHECK(qa->entries.front().object == enemy);

  // **A stale object handle is a point order and spreads like one.** The test
  // here is `issue_order`'s own -- it prefers the object only while the object
  // exists -- so the two cannot disagree about which argument an order carries.
  const ObjectId e1 = f.spawn(f.unit, 1);
  const ObjectId e2 = f.spawn(f.unit, 1);
  CHECK(f.world.set_position(e1, Point{80, 100}));
  CHECK(f.world.set_position(e2, Point{120, 100}));
  const std::vector<ObjectId> two{e1, e2};
  OrderTarget stale;
  stale.object = static_cast<ObjectId>(999999);  // names nothing
  stale.point = Point{700, 700};
  CHECK(issue_default_order(f.world, f.table, two, stale, OrderMode::replace, false, 1,
                            &verifier)
            .issued == 2);
  CHECK(point_of(e1) == (Point{680, 700}));
  CHECK(point_of(e2) == (Point{720, 700}));

  // **A verb the table does not mark takes no spread.** `move` carries
  // `offset="1"` in the shipped rows and in the fixture's; a table that says
  // otherwise must be obeyed, so this one is built without it.
  CommandTable plain;
  CHECK(plain.merge(bytes(R"(<commands><cmd name="move" priority="0" key="m"/></commands>)")).ok());
  const ObjectId c = f.spawn(f.unit, 1);
  const ObjectId d = f.spawn(f.unit, 1);
  CHECK(f.world.set_position(c, Point{80, 100}));
  CHECK(f.world.set_position(d, Point{120, 100}));
  const std::vector<ObjectId> unmarked{c, d};
  CHECK(issue_default_order(f.world, plain, unmarked, OrderTarget{Point{500, 500}},
                            OrderMode::replace, false, 1, &verifier)
            .issued == 2);
  CHECK(point_of(c) == (Point{500, 500}));
  CHECK(point_of(d) == (Point{500, 500}));
}

/// **A selection wider than `delta` is several groups**, each moving about its
/// own centre rather than the whole selection's.
///
/// `delta = max(SubdivDelta, 45 * isqrt(N - 1))`, and `DATA\CONST.INI` says
/// `SubdivDelta = 300 ;selection subdivision for offsets` -- the file naming
/// this mechanism. Two pairs 2,000 apart are two clusters, so each pair keeps
/// its offset from *its* centre and both pairs land on the click.
TEST(a_selection_wider_than_the_subdivision_delta_moves_as_several_groups) {
  Fixture f;
  const auto point_of = [&](ObjectId id) {
    const CommandQueue* q = f.commands.find(id);
    return q != nullptr && !q->entries.empty() ? q->entries.front().point : Point{};
  };
  TableVerifier verifier;

  // Two tight pairs, far apart. Within a pair the members are 40 apart; the
  // pairs are 2,000 apart, which is well past the 300 default.
  const ObjectId near_a = f.spawn(f.unit, 1, Point{80, 100});
  const ObjectId near_b = f.spawn(f.unit, 1, Point{120, 100});
  const ObjectId far_a = f.spawn(f.unit, 1, Point{2080, 100});
  const ObjectId far_b = f.spawn(f.unit, 1, Point{2120, 100});

  const std::vector<ObjectId> all{near_a, near_b, far_a, far_b};
  CHECK(issue_default_order(f.world, f.table, all, OrderTarget{Point{5000, 5000}},
                            OrderMode::replace, false, 1, &verifier)
            .issued == 4);

  // Each pair is centred on its own midpoint, so both pairs straddle the click.
  CHECK(point_of(near_a) == (Point{4980, 5000}));
  CHECK(point_of(near_b) == (Point{5020, 5000}));
  CHECK(point_of(far_a) == (Point{4980, 5000}));
  CHECK(point_of(far_b) == (Point{5020, 5000}));

  // **`SubdivDelta` is a floor, not an alternative.** With four selected the
  // size term is `45 * isqrt(3)` = 45, so a pair 200 apart is one cluster only
  // because 300 wins. Without the floor these would be two clusters and each
  // unit would land on the click.
  Fixture g;
  const auto point_in = [&](ObjectId id) {
    const CommandQueue* q = g.commands.find(id);
    return q != nullptr && !q->entries.empty() ? q->entries.front().point : Point{};
  };
  TableVerifier other;
  const ObjectId west = g.spawn(g.unit, 1, Point{900, 100});
  const ObjectId east = g.spawn(g.unit, 1, Point{1100, 100});
  const std::vector<ObjectId> pair{west, east};
  CHECK(issue_default_order(g.world, g.table, pair, OrderTarget{Point{5000, 5000}},
                            OrderMode::replace, false, 1, &other)
            .issued == 2);
  CHECK(point_in(west) == (Point{4900, 5000}));
  CHECK(point_in(east) == (Point{5100, 5000}));
}

/// And `SubdivDelta` really is read from `CONST.INI` rather than compiled in:
/// a map that lowers it subdivides more finely.
TEST(the_subdivision_delta_comes_from_the_constants) {
  Fixture f;
  EnvSystem env;
  REQUIRE(f.world.add_system(&env));
  const auto ini = IniDocument::parse(
      bytes("[GamePlay]\nSubdivDelta = 50 ;selection subdivision for offsets\n"));
  REQUIRE(ini.ok());
  CHECK(env.load_constants(ini.value()) == 1);

  const auto point_of = [&](ObjectId id) {
    const CommandQueue* q = f.commands.find(id);
    return q != nullptr && !q->entries.empty() ? q->entries.front().point : Point{};
  };
  TableVerifier verifier;
  // 200 apart. Under the shipped 300 these are one cluster and straddle the
  // click; under 50 they are two, and each lands on it.
  const ObjectId west = f.spawn(f.unit, 1, Point{900, 100});
  const ObjectId east = f.spawn(f.unit, 1, Point{1100, 100});
  const std::vector<ObjectId> pair{west, east};
  CHECK(issue_default_order(f.world, f.table, pair, OrderTarget{Point{5000, 5000}},
                            OrderMode::replace, false, 1, &verifier)
            .issued == 2);
  CHECK(point_of(west) == (Point{5000, 5000}));
  CHECK(point_of(east) == (Point{5000, 5000}));
}

/// **A cluster never mixes two armies**, whatever the distance. Two units
/// standing on the same spot go to different places if they follow different
/// heroes.
TEST(a_cluster_never_mixes_units_from_two_armies) {
  Fixture f;
  const auto point_of = [&](ObjectId id) {
    const CommandQueue* q = f.commands.find(id);
    return q != nullptr && !q->entries.empty() ? q->entries.front().point : Point{};
  };
  TableVerifier verifier;

  const ObjectId loose_a = f.spawn(f.unit, 1, Point{80, 100});
  const ObjectId loose_b = f.spawn(f.unit, 1, Point{120, 100});
  const ObjectId captain = f.spawn(f.hero, 1, Point{100, 100});
  const ObjectId soldier = f.spawn(f.unit, 1, Point{100, 100});
  f.heroes.register_hero(f.world, captain);
  f.heroes.register_unit(f.world, soldier);
  CHECK(f.heroes.attach(f.world, soldier, captain));
  REQUIRE(f.heroes.unit(soldier) != nullptr);
  CHECK(f.heroes.unit(soldier)->hero == captain);

  const std::vector<ObjectId> mixed{loose_a, loose_b, soldier};
  CHECK(issue_default_order(f.world, f.table, mixed, OrderTarget{Point{5000, 5000}},
                            OrderMode::replace, false, 1, &verifier)
            .issued == 3);

  // The two loose units are their own cluster, centred between them.
  CHECK(point_of(loose_a) == (Point{4980, 5000}));
  CHECK(point_of(loose_b) == (Point{5020, 5000}));
  // The soldier is alone in its cluster and in a hero's army, so it takes the
  // scatter arm -- with `n == 1` the radius is zero, so it lands on the click.
  CHECK(point_of(soldier) == (Point{5000, 5000}));
}

/// **A unit in a hero's army is scattered, not shape-preserved** -- and that is
/// the arm that can *restore* a shape rather than only keep one.
///
/// A group flattened into a line by a click against the map edge stays flat for
/// ever under the offset rule. The army arm draws independently on each axis in
/// `[-r, r]` with `r = 45 * isqrt(n - 1)`, so nine warriors get +/-90 and a line
/// becomes a blob again.
TEST(a_heros_army_is_scattered_and_recovers_from_a_line) {
  Fixture f;
  const auto point_of = [&](ObjectId id) {
    const CommandQueue* q = f.commands.find(id);
    return q != nullptr && !q->entries.empty() ? q->entries.front().point : Point{};
  };
  TableVerifier verifier;

  // Nine warriors in a dead straight line, all following one hero.
  const ObjectId captain = f.spawn(f.hero, 1, Point{500, 1000});
  f.heroes.register_hero(f.world, captain);
  std::vector<ObjectId> army;
  for (int i = 0; i < 9; ++i) {
    const ObjectId soldier =
        f.spawn(f.unit, 1, Point{100 + static_cast<std::int32_t>(i) * 20, 1000});
    f.heroes.register_unit(f.world, soldier);
    CHECK(f.heroes.attach(f.world, soldier, captain));
    army.push_back(soldier);
  }

  CHECK(issue_default_order(f.world, f.table, army, OrderTarget{Point{5000, 5000}},
                            OrderMode::replace, false, 1, &verifier)
            .issued == 9);

  // `r` is 45 * isqrt(8) = 45 * 2 = 90. Every destination is inside the square,
  // and -- the point of the whole thing -- they differ on **both** axes, which
  // the shape-preserving arm could never do from a flat line.
  bool y_moved = false;
  bool x_moved = false;
  // **Two independent draws, not one.** A body that drew once and used it on
  // both axes would put every warrior on the same diagonal.
  bool axes_differ = false;
  for (const ObjectId soldier : army) {
    const Point at = point_of(soldier);
    CHECK(at.x >= 5000 - 90);
    CHECK(at.x <= 5000 + 90);
    CHECK(at.y >= 5000 - 90);
    CHECK(at.y <= 5000 + 90);
    if (at.x != 5000) x_moved = true;
    if (at.y != 5000) y_moved = true;
    if (at.x - 5000 != at.y - 5000) axes_differ = true;
  }
  CHECK(x_moved);
  CHECK(y_moved);
  CHECK(axes_differ);
}

/// Within a cluster the members are bucketed by class, so `r` is that class's
/// count rather than the cluster's -- and a mixed group is two sub-orders.
TEST(a_cluster_is_bucketed_by_class) {
  Fixture f;
  const auto point_of = [&](ObjectId id) {
    const CommandQueue* q = f.commands.find(id);
    return q != nullptr && !q->entries.empty() ? q->entries.front().point : Point{};
  };
  TableVerifier verifier;
  verifier.pass = {"data/subai/catapult_attack_ground_verify.vs"};

  // Two classes standing together. The centroid is the cluster's, shared; what
  // differs per bucket is `r`, which the loose arm does not use -- so what this
  // asserts is that bucketing does not disturb the offsets.
  const ObjectId a = f.spawn(f.unit, 1, Point{80, 100});
  const ObjectId b = f.spawn(f.unit, 1, Point{120, 100});
  const ObjectId engine = f.spawn(f.catapult, 1, Point{100, 100});

  const std::vector<ObjectId> mixed{a, b, engine};
  const OrderReport report = issue_default_order(f.world, f.table, mixed,
                                                 OrderTarget{Point{5000, 5000}},
                                                 OrderMode::replace, false, 1, &verifier);
  CHECK(report.issued == 3);
  // One centroid for the cluster, whichever bucket a member is in.
  CHECK(point_of(a) == (Point{4980, 5000}));
  CHECK(point_of(b) == (Point{5020, 5000}));
  CHECK(point_of(engine) == (Point{5000, 5000}));
  // And the two classes really did resolve different verbs.
  CHECK((f.verbs(a) == std::vector<std::string>{"move"}));
  CHECK((f.verbs(engine) == std::vector<std::string>{"attack_ground"}));
}

TEST(a_player_order_resolves_per_actor_and_refuses_what_it_does_not_control) {
  Fixture f;
  const ObjectId soldier = f.spawn(f.unit, 1);
  const ObjectId engine = f.spawn(f.catapult, 1);
  const ObjectId theirs = f.spawn(f.unit, 2);

  TableVerifier verifier;
  // The catapult's ground verb is conditional; the soldier's `move` is not.
  verifier.pass = {"data/subai/catapult_attack_ground_verify.vs"};
  const std::vector<ObjectId> actors{soldier, engine, theirs};
  const OrderReport report = issue_default_order(f.world, f.table, actors, OrderTarget{Point{4, 4}},
                                                 OrderMode::replace, false, 1, &verifier);
  CHECK(report.issued == 2);
  CHECK(report.refused == 1);
  CHECK(report.blocked == 0);

  // A mixed selection really does produce two different verbs against one
  // click: the catapult's ground block overrides the one it inherits.
  CHECK((f.verbs(soldier) == std::vector<std::string>{"move"}));
  CHECK((f.verbs(engine) == std::vector<std::string>{"attack_ground"}));
  CHECK(f.verbs(theirs).empty());
}

TEST(a_player_order_reports_what_it_could_not_resolve) {
  Fixture f;
  const ObjectId watcher = f.spawn(f.sentry, 1);
  const ObjectId soldier = f.spawn(f.unit, 1);
  const ObjectId theirs = f.spawn(f.building, 2);

  TableVerifier verifier;
  verifier.unknown = {"data/subai/attack_independent_verify.vs"};
  const std::vector<ObjectId> actors{watcher, soldier};
  const OrderReport report = issue_default_order(f.world, f.table, actors,
                                                 OrderTarget{Point{4, 4}, theirs},
                                                 OrderMode::replace, false, 1, &verifier);
  CHECK(report.issued == 0);
  CHECK(report.unresolved == 1);  // the sentry inherits no block for a building
  CHECK(report.blocked == 1);     // the soldier hit the unverifiable candidate
  CHECK(f.verbs(soldier).empty());
}

TEST(the_selection_overload_orders_exactly_the_issuers_selection) {
  Fixture f;
  const ObjectId mine = f.spawn(f.unit, 1);
  const ObjectId also_mine = f.spawn(f.unit, 1);
  const ObjectId theirs = f.spawn(f.unit, 2);

  SelectionTable selections;
  selections.select(1, mine);
  selections.select(1, also_mine);
  selections.select(2, theirs);

  TableVerifier verifier;
  const OrderReport report = issue_default_order(f.world, f.table, selections,
                                                 OrderTarget{Point{8, 8}}, OrderMode::replace,
                                                 false, 1, &verifier);
  CHECK(report.issued == 2);
  CHECK(report.refused == 0);
  CHECK(f.verbs(theirs).empty());
}

TEST(the_match_loop_overloads_find_the_worlds_own_command_table) {
  Fixture f;
  const ObjectId soldier = f.spawn(f.unit, 1);
  CHECK(order_command_table(f.world) != nullptr);

  TableVerifier verifier;
  const DefaultOrder order =
      resolve_default_order(f.world, soldier, OrderTarget{Point{2, 2}}, false, &verifier);
  CHECK(order.status == DefaultOrderStatus::resolved);
  CHECK(order.command == "move");

  SelectionTable selections;
  selections.select(1, soldier);
  const OrderReport report = issue_default_order(f.world, selections, OrderTarget{Point{2, 2}},
                                                 OrderMode::replace, false, 1, &verifier);
  CHECK(report.issued == 1);
  CHECK((f.verbs(soldier) == std::vector<std::string>{"move"}));

  // A world with no command system refuses once per selected actor rather than
  // reporting a silent zero.
  World bare;
  CHECK(order_command_table(bare) == nullptr);
  const ObjectId orphan = bare.spawn(NativeClass::unit, nullptr, kNoClass);
  SelectionTable other;
  other.select(1, orphan);
  other.select(1, orphan + 1);
  const OrderReport none = issue_default_order(bare, other, OrderTarget{Point{}},
                                               OrderMode::replace, false, 1, &verifier);
  CHECK(none.issued == 0);
  CHECK(none.refused == 2);
  CHECK(resolve_default_order(bare, orphan, OrderTarget{Point{}}, false, &verifier).status ==
        DefaultOrderStatus::none);
}

// --------------------------------------------------------------------------
// the script verifier
// --------------------------------------------------------------------------

namespace {

/// Compile one `.vs` source into `scheduler` under `name`, reporting why not.
bool build(Scheduler& scheduler, const HostRegistry& registry, std::string_view source,
           const char* name) {
  Diagnostic diagnostic;
  const auto parsed = parse(bytes(source), name, &diagnostic);
  if (!parsed.ok()) {
    std::printf("  parse %s:%u: %.*s\n", name, diagnostic.line,
                static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
    return false;
  }
  CompileError error;
  auto chunk = compile(parsed.value(), &registry, &error);
  if (!chunk.ok()) {
    std::printf("  compile %s:%u: %s\n", name, error.line, error.message.c_str());
    return false;
  }
  scheduler.add_chunk(std::move(chunk.value()));
  return true;
}

}  // namespace

TEST(the_script_verifier_runs_a_real_predicate_off_to_the_side) {
  Fixture f;
  HostRegistry registry;
  register_all_hosts(registry);
  Scheduler scheduler;
  scheduler.set_registry(&registry);
  HostContext context;
  context.world = &f.world;
  scheduler.set_user(&context);

  // The shape every shipped verifier declares: `bool, Obj this, Obj other`.
  REQUIRE(build(scheduler, registry,
                "// bool, Obj this, Obj other\nreturn this.IsEnemy(other);",
                "data/subai/unit_attack_verify.vs"));
  REQUIRE(build(scheduler, registry, "// bool, Obj this, point pt\nreturn false;",
                "data/subai/never.vs"));
  REQUIRE(build(scheduler, registry, "// bool, Obj this, Obj other\nreturn 1 / (1 - 1);",
                "data/subai/trapping.vs"));

  const ObjectId mine = f.spawn(f.unit, 1);
  const ObjectId theirs = f.spawn(f.unit, 2);
  // `IsEnemy` reads the relations matrix; player 2 is nobody's declared friend,
  // and player 1 is its own.
  ScriptOrderVerifier verifier(scheduler, context);

  const OrderVerdict against_enemy = verifier.verify("data/subai/unit_attack_verify.vs", mine,
                                                     OrderTarget{Point{1, 1}, theirs});
  const OrderVerdict against_self =
      verifier.verify("data/subai/unit_attack_verify.vs", mine, OrderTarget{Point{1, 1}, mine});
  // Whatever the diplomacy fixture says, the two must differ: a predicate that
  // answered the same for an enemy and for itself would be answering nothing.
  CHECK(against_enemy != against_self);
  CHECK(against_enemy != OrderVerdict::unknown);
  CHECK(against_self == OrderVerdict::fail);

  // A point target passes the point as the second argument.
  CHECK(verifier.verify("data/subai/never.vs", mine, OrderTarget{Point{3, 4}}) ==
        OrderVerdict::fail);

  // Not in the library, and trapping, are both `unknown` -- never `fail`.
  CHECK(verifier.verify("data/subai/no_such_file.vs", mine, OrderTarget{Point{}}) ==
        OrderVerdict::unknown);
  CHECK(verifier.missing() == 1);
  CHECK(verifier.verify("data/subai/trapping.vs", mine, OrderTarget{Point{1, 1}, theirs}) ==
        OrderVerdict::unknown);
  CHECK(verifier.traps() == 1);

  // Running a verifier leaves no coroutine behind: it is asked on every mouse
  // move, and a `ScriptRecord` per hover would end up in the save.
  CHECK(scheduler.live_count() == 0);

  verifier.reset_counts();
  CHECK(verifier.traps() == 0);
  CHECK(verifier.missing() == 0);
}

namespace {

/// A library that compiles one known source on first ask, and counts asks.
class OneFileLibrary final : public ScriptLibrary {
 public:
  OneFileLibrary(Scheduler& scheduler, const HostRegistry& registry)
      : scheduler_(&scheduler), registry_(&registry) {}
  std::uint32_t chunk_for(std::string_view path) override {
    ++asks;
    if (path != "data/subai/unit_attack_verify.vs") return kNoChunk;
    if (const std::uint32_t found = scheduler_->find_chunk(path); found != kNoChunk) return found;
    if (!build(*scheduler_, *registry_, "// bool, Obj this, Obj other\nreturn this.IsEnemy(other);",
               "data/subai/unit_attack_verify.vs")) {
      return kNoChunk;
    }
    return scheduler_->find_chunk(path);
  }
  std::size_t asks = 0;

 private:
  Scheduler* scheduler_;
  const HostRegistry* registry_;
};

}  // namespace

TEST(a_verifier_nothing_preloaded_is_compiled_on_first_ask) {
  // No `verify=` is preloaded: only the `[Scripts]` manifest and the `idle`
  // methods are. So a player's first right click on an enemy found the attack
  // verifier missing and blocked, and kept blocking until some computer
  // player's attack compiled it through the command system -- on Alesia, 8 of
  // 8 legionaries refused an ordered attack at turn 8.
  Fixture f;
  HostRegistry registry;
  register_all_hosts(registry);
  Scheduler scheduler;
  scheduler.set_registry(&registry);
  OneFileLibrary library(scheduler, registry);
  HostContext context;
  context.world = &f.world;
  context.library = &library;
  scheduler.set_user(&context);
  const ObjectId mine = f.spawn(f.unit, 1);
  const ObjectId theirs = f.spawn(f.unit, 2);
  REQUIRE(scheduler.find_chunk("data/subai/unit_attack_verify.vs") == kNoChunk);

  ScriptOrderVerifier verifier(scheduler, context);
  const OrderVerdict verdict = verifier.verify("data/subai/unit_attack_verify.vs", mine,
                                               OrderTarget{Point{1, 1}, theirs});
  CHECK(verdict != OrderVerdict::unknown);
  CHECK(verifier.missing() == 0);
  CHECK(library.asks == 1);
  // Once compiled it is found, not compiled again.
  (void)verifier.verify("data/subai/unit_attack_verify.vs", mine, OrderTarget{Point{1, 1}, theirs});
  CHECK(library.asks == 1);
  // A file the library does not have is still missing, still `unknown`.
  CHECK(verifier.verify("data/subai/no_such_file.vs", mine, OrderTarget{Point{}}) ==
        OrderVerdict::unknown);
  CHECK(verifier.missing() == 1);
  CHECK(scheduler.live_count() == 0);
}

TEST(a_script_verifier_with_a_world_less_context_refuses_rather_than_dereferences) {
  // `CallContext::user` is a `HostContext*`, and a host function that
  // dereferenced a null world would take the process down. A verifier runs
  // arbitrary host calls, so the same rule applies to what it hands them.
  HostRegistry registry;
  register_all_hosts(registry);
  Scheduler scheduler;
  scheduler.set_registry(&registry);
  HostContext context;  // `world` deliberately null
  REQUIRE(build(scheduler, registry, "// bool, Obj this, Obj other\nreturn this.IsEnemy(other);",
                "data/subai/unit_attack_verify.vs"));
  ScriptOrderVerifier verifier(scheduler, context);
  const OrderVerdict verdict =
      verifier.verify("data/subai/unit_attack_verify.vs", 1, OrderTarget{Point{}, 2});
  // The host call fails, the script traps, and the verifier says so rather
  // than making something up.
  CHECK(verdict == OrderVerdict::unknown);
  CHECK(verifier.traps() == 1);
}

// --------------------------------------------------------------------------
// the selection entry points
// --------------------------------------------------------------------------
//
// Three claims this half is built to catch, and each is a correction to what
// this file used to assert:
//
//   1. **The player id is a guard, not an index.** `ClearSelection(2)` on the
//      machine whose screen is player 1's does *nothing*. A binding that
//      indexed by the argument would pass every test written against one
//      player, which is what the corpus's call sites all are.
//   2. **`SwapSelectedObj` appends.** It is Deselect's helper and then
//      Select's, so the summoned wolf goes to the end and the druid's position
//      is not kept.
//   3. **`Is`/`WasSelectionAssigned` are about control groups**, and they must
//      be able to disagree in the direction `GENERALADVICE6.VS` nests them:
//      `Was` is the environment key the *player* sets by pressing ctrl-1, `Is`
//      is whether a group is non-empty now. A script assigning a group must
//      move one and not the other.

namespace {

/// A world, a registry, a scheduler and one screen.
struct Screen {
  ClassGraph graph = fixture_graph();
  World world;
  EnvSystem env;
  SelectionTable selections;
  ShortcutTable shortcuts;
  HostRegistry registry;
  WorldHost host{world};
  Scheduler scheduler;
  HostContext context;

  explicit Screen(PlayerId local = 1) {
    world.seed(5);
    world.set_class_graph(&graph);
    world.add_system(&env);
    register_all_hosts(registry);
    scheduler.set_registry(&registry);
    scheduler.set_host(&host);
    context.world = &world;
    context.object_type = kTypeObj;
    context.selections = &selections;
    context.shortcuts = &shortcuts;
    context.local_player = local;
    scheduler.set_user(&context);
    install_objlist_lifetime(scheduler);
  }

  ObjectId spawn(PlayerId owner = 1, const char* name = nullptr) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
    world.set_position(id, Point{100, 100});
    world.set_owner(id, owner);
    world.set_health(id, 100);
    if (name != nullptr) (void)world.named_objects().bind(name, id);
    return id;
  }

  /// Run one source to completion with `subject` as the receiver. False when it
  /// failed to compile or trapped, with the reason printed.
  bool run(std::string_view source, ObjectId subject = kNoObject) {
    if (!build(scheduler, registry, source, "data/subai/selection_case.vs")) return false;
    const std::uint32_t index = scheduler.chunk_count() - 1;
    if (scheduler.spawn(index, {}, ObjectRef{kTypeObj, subject}) == kNoScript) return false;
    for (int pass = 0; pass < 4 && scheduler.live_count() > 0; ++pass) {
      const RunReport report = scheduler.advance(1000);
      for (const FailedScript& trap : report.traps) {
        std::printf("  trap %s:%u: %s\n", trap.source_name.c_str(), trap.trap.line,
                    trap.trap.detail.c_str());
        return false;
      }
    }
    return scheduler.live_count() == 0;
  }

  [[nodiscard]] std::int32_t out(std::string_view key) const {
    return env.env().read_int(EnvScope::root(), key);
  }
};

}  // namespace

TEST(selection_entry_points_answer_only_the_local_screen) {
  // The correction. `ClearSelection` (0x005a8120), `Obj::Select` (0x005abfc0)
  // and the two-argument `Obj::Deselect` all compare the player's record
  // against the local one and return having done nothing when they differ.
  Screen screen(/*local=*/1);
  const ObjectId mine = screen.spawn(1, "NO_Mine");
  const ObjectId theirs = screen.spawn(2, "NO_Theirs");

  // Player 2 is not this screen. Neither call may reach anything -- and the
  // slot the argument names has to be occupied for that to mean anything: a
  // binding that indexed by the argument would clear an empty selection and
  // look identical.
  screen.selections.select(2, theirs);
  REQUIRE(screen.run("// void\nClearSelection(3);\n"));
  CHECK(screen.selections.player(2).size() == 1);
  screen.selections.clear(2);
  REQUIRE(screen.run("// void\nGetNamedObj(\"NO_Theirs\").obj.Select(3);\n"));
  CHECK(screen.selections.player(1).empty());
  CHECK(screen.selections.player(2).empty());

  // Player 2 in *script* numbering is `PlayerId` 1, which is this screen.
  REQUIRE(screen.run("// void\nGetNamedObj(\"NO_Mine\").obj.Select(2);\n"));
  REQUIRE(screen.selections.player(1).size() == 1);
  CHECK(screen.selections.player(1).at(0) == mine);

  // And the clear reaches the same one slot, and only it.
  screen.selections.select(2, theirs);
  REQUIRE(screen.run("// void\nClearSelection(2);\n"));
  CHECK(screen.selections.player(1).empty());
  CHECK(screen.selections.player(2).size() == 1);
}

TEST(select_drops_the_dead_and_the_unselectable_without_a_word) {
  Screen screen(/*local=*/1);
  const ObjectId dead = screen.spawn(1, "NO_Dead");
  screen.world.set_health(dead, 0);
  const ObjectId flagged = screen.spawn(1, "NO_Flagged");
  screen.world.mutable_state(flagged)->flags.noselect = true;
  const ObjectId ordinary = screen.spawn(1, "NO_Ordinary");

  // All three are an ordinary return: the original prints into the discard
  // sink for the first two and says nothing at all for the third, and a trap
  // here would stop `CREATE_GOLD_MULE_BIG.VS` dead.
  REQUIRE(screen.run("// void\nGetNamedObj(\"NO_Dead\").obj.Select(2);\n"));
  REQUIRE(screen.run("// void\nGetNamedObj(\"NO_Flagged\").obj.Select(2);\n"));
  CHECK(screen.selections.player(1).empty());

  REQUIRE(screen.run("// void\nGetNamedObj(\"NO_Ordinary\").obj.Select(2);\n"));
  CHECK(screen.selections.player(1).size() == 1);

  // An unresolvable receiver is the third silent case.
  REQUIRE(screen.run("// void\nObj u;\nu.Select(2);\n"));
  CHECK(screen.selections.player(1).size() == 1);
}

TEST(deselect_has_no_player_guard_and_select_does) {
  // The no-argument `Obj::Deselect` (0x005ac0e0) reaches the selection without
  // comparing anything, which is the difference between it and the two-argument
  // form nothing calls. With more than one screen kept in one process that is
  // observable: it removes from all of them.
  Screen screen(/*local=*/1);
  const ObjectId subject = screen.spawn(1, "NO_Subject");
  screen.selections.select(1, subject);
  screen.selections.select(2, subject);

  REQUIRE(screen.run("// void\nGetNamedObj(\"NO_Subject\").obj.Deselect();\n"));
  CHECK(screen.selections.player(1).empty());
  CHECK(screen.selections.player(2).empty());

  // And both halves are unconditional. 0x005e7e00 erases whether or not the
  // object was there and runs the handler either way, so deselecting something
  // nobody had selected still restamps what is selected.
  const ObjectId kept = screen.spawn(1);
  const ObjectId bystander = screen.spawn(1, "NO_Bystander");
  screen.selections.select(1, kept);
  screen.selections.note_selection_changed(1, 1);
  CHECK(screen.selections.last_selected(kept) == 1);
  REQUIRE(screen.run("// void\nGetNamedObj(\"NO_Bystander\").obj.Deselect();\n"));
  CHECK(screen.selections.player(1).size() == 1);
  CHECK(screen.selections.last_selected(kept) > 1);
  CHECK(screen.selections.last_selected(bystander) == -1);
}

TEST(swap_selected_obj_appends_and_is_guarded_on_membership) {
  // `DRUID_WOLF_SUMMONING.VS`: `SwapSelectedObj(this, u)`, where `this` is
  // about to damage itself to death.
  Screen screen(/*local=*/1);
  const ObjectId druid = screen.spawn(1, "NO_Druid");
  const ObjectId escort = screen.spawn(1);
  const ObjectId wolf = screen.spawn(1, "NO_Wolf");
  screen.selections.select(1, druid);
  screen.selections.select(1, escort);

  REQUIRE(screen.run("// void\n"
      "SwapSelectedObj(GetNamedObj(\"NO_Druid\").obj, GetNamedObj(\"NO_Wolf\").obj);\n"));
  REQUIRE(screen.selections.player(1).size() == 2);
  CHECK(screen.selections.player(1).at(0) == escort);
  CHECK(screen.selections.player(1).at(1) == wolf);

  // A `from` that is not selected leaves the list alone -- 0x004c84e0 asks
  // first (0x005e3930), and a summoning by an unselected druid must not put a
  // wolf on the player's screen.
  const ObjectId stranger = screen.spawn(1, "NO_Stranger");
  screen.selections.note_selection_changed(1, 1);
  REQUIRE(screen.run("// void\n"
      "SwapSelectedObj(GetNamedObj(\"NO_Stranger\").obj, GetNamedObj(\"NO_Wolf\").obj);\n"));
  CHECK(screen.selections.player(1).size() == 2);
  // And it did not run the selection-changed handler either: 0x004c84e0 asks
  // first and skips the removal, the append *and* the notify together.
  CHECK(screen.selections.last_selected(escort) == 1);
  CHECK(screen.selections.last_selected(stranger) == -1);
}

/// `selu` is the selection's **head** when that is a unit, and invalid
/// otherwise -- a selected building with a unit behind it is not a unit.
TEST(selu_is_the_selections_head_when_it_is_a_unit) {
  Screen screen(/*local=*/1);
  const ObjectId soldier = screen.spawn(1);
  const ObjectId hall = screen.world.spawn(NativeClass::building, nullptr);
  screen.world.set_owner(hall, 1);
  screen.world.set_health(hall, 100);
  const auto probe = [&]() -> std::int32_t {
    const bool ran = screen.run("// void\n"
                                "EnvWriteInt(\"/valid\", selu.IsValid());\n"
                                "if (selu.IsValid) EnvWriteInt(\"/player\", selu.player);\n");
    CHECK(ran);
    return ran ? screen.out("/valid") : -1;
  };
  // Nothing selected.
  CHECK(probe() == 0);
  // A unit first: it.
  screen.selections.select(1, soldier);
  screen.selections.select(1, hall);
  CHECK(probe() == 1);
  CHECK(screen.out("/player") == 2);
  // The building first: invalid, though a unit is selected behind it.
  screen.selections.clear(1);
  screen.selections.select(1, hall);
  screen.selections.select(1, soldier);
  CHECK(probe() == 0);
}

TEST(get_selection_hands_back_a_copy_in_order) {
  // `BUILD_CATAPULT_VERIFY.VS` runs `GetCanExecCmd` and `ClearDead` over what
  // it gets, and neither may touch what the player has selected. 0x005b5690
  // allocates a fresh list and copies into it.
  Screen screen(/*local=*/1);
  // Two different owners so that selection order is readable from the script
  // side: `SCDEBUG.XML` indexes the list backwards, so order is observable and
  // a reversed copy is a different answer.
  const ObjectId first = screen.spawn(0);
  const ObjectId second = screen.spawn(4);
  screen.selections.select(1, first);
  screen.selections.select(1, second);

  REQUIRE(screen.run(
      "// void\n"
      "ObjList l;\n"
      "l = _GetSelection();\n"
      "EnvWriteInt(\"/count\", l.count());\n"
      "EnvWriteInt(\"/p0\", l[0].player);\n"
      "EnvWriteInt(\"/p1\", l[1].player);\n"
      "l.Clear();\n"
      "EnvWriteInt(\"/after\", l.count());\n"));
  CHECK(screen.out("/count") == 2);
  CHECK(screen.out("/p0") == 1);  // the first selected, first in the copy
  CHECK(screen.out("/p1") == 5);
  CHECK(screen.out("/after") == 0);
  // Untouched: the copy was the script's.
  CHECK(screen.selections.player(1).size() == 2);
  CHECK(screen.selections.player(1).at(0) == first);

  // A handle that is not an object is not a receiver either. Member lookup is
  // by name and arity, so an `ObjList` reaches `Obj::Select` as readily as a
  // unit does -- and an `ObjList` value is `(kTypeObjList, pool index)`, which
  // read as an object id names whatever object happens to wear that number.
  // Here that is `first`, which is why the selection would grow.
  screen.selections.clear(1);
  REQUIRE(screen.run("// void\n_GetSelection().Select(2);\n"));
  CHECK(screen.selections.player(1).empty());
  screen.selections.select(1, first);
  screen.selections.select(1, second);

  // With nothing selected it is an empty list rather than a refusal.
  screen.selections.clear(1);
  REQUIRE(screen.run("// void\nEnvWriteInt(\"/empty\", _GetSelection().count());\n"));
  CHECK(screen.out("/empty") == 0);
}

TEST(the_two_selection_assigned_questions_are_about_control_groups) {
  // `GENERALADVICE6.VS` nests them `if (Was...) break; else if (Is...) break;`
  // around a hint, so they must be able to disagree in both directions. They
  // read two different things and neither is the selection.
  Screen screen(/*local=*/1);
  const ObjectId subject = screen.spawn(1, "NO_Subject");
  screen.selections.select(1, subject);
  screen.selections.note_selection_changed(1, 100);

  // A full selection and no control group: both answer no, which is the case a
  // binding that read the selection would get wrong.
  REQUIRE(screen.run(
      "// void\n"
      "EnvWriteInt(\"/is\", IsSelectionAssigned(2));\n"
      "EnvWriteInt(\"/was\", WasSelectionAssigned(2));\n"));
  CHECK(screen.out("/is") == 0);
  CHECK(screen.out("/was") == 0);

  // A group assigned -- `SetShortcutSel`'s doing, or a script's -- moves `Is`
  // and leaves `Was` alone. That is the whole point of asking two questions:
  // the tutorial wants to know whether the *player* has learned the habit.
  const std::vector<ObjectId> group{subject};
  screen.shortcuts.assign(1, 3, group);
  REQUIRE(screen.run(
      "// void\n"
      "EnvWriteInt(\"/is\", IsSelectionAssigned(2));\n"
      "EnvWriteInt(\"/was\", WasSelectionAssigned(2));\n"));
  CHECK(screen.out("/is") == 1);
  CHECK(screen.out("/was") == 0);

  // And the key the input layer sets moves `Was` with no group in sight.
  screen.shortcuts.clear();
  screen.env.env().write_int(EnvScope::root(), "/Player2/SelectionAssigned", 1 << 3);
  REQUIRE(screen.run(
      "// void\n"
      "EnvWriteInt(\"/is\", IsSelectionAssigned(2));\n"
      "EnvWriteInt(\"/was\", WasSelectionAssigned(2));\n"));
  CHECK(screen.out("/is") == 0);
  CHECK(screen.out("/was") == 1);

  // The key is per player, and it is asked about by the script's own numbering.
  REQUIRE(screen.run("// void\nEnvWriteInt(\"/other\", WasSelectionAssigned(5));\n"));
  CHECK(screen.out("/other") == 0);
  // An out-of-range player is a key nothing has written, not a refusal -- and
  // `Is` bounds its argument to 1..16 before it indexes anything at all.
  screen.shortcuts.assign(1, 3, group);
  REQUIRE(screen.run(
      "// void\n"
      "EnvWriteInt(\"/wild\", WasSelectionAssigned(99));\n"
      "EnvWriteInt(\"/wildis\", IsSelectionAssigned(99));\n"
      "EnvWriteInt(\"/zerois\", IsSelectionAssigned(0));\n"));
  CHECK(screen.out("/wild") == 0);
  CHECK(screen.out("/wildis") == 0);
  CHECK(screen.out("/zerois") == 0);
}

TEST(last_selection_time_answers_minus_one_for_never_and_for_nothing) {
  // `BUILDINGSADVICE9.VS` counts the player's buildings that answer anything
  // else. One answer for "never selected" and for "not a thing", which is what
  // 0x005abd60 does by initialising its result before it tries the handle.
  Screen screen(/*local=*/1);
  const ObjectId subject = screen.spawn(1, "NO_Subject");

  REQUIRE(screen.run("// void\n"
      "EnvWriteInt(\"/t\", GetNamedObj(\"NO_Subject\").obj._LastSelectionTime());\n"));
  CHECK(screen.out("/t") == -1);
  REQUIRE(screen.run("// void\nObj u;\nEnvWriteInt(\"/gone\", u._LastSelectionTime());\n"));
  CHECK(screen.out("/gone") == -1);

  // `Select` stamps, because it says the selection changed.
  REQUIRE(screen.run("// void\nGetNamedObj(\"NO_Subject\").obj.Select(2);\n"));
  REQUIRE(screen.run("// void\n"
      "EnvWriteInt(\"/t\", GetNamedObj(\"NO_Subject\").obj._LastSelectionTime());\n"));
  CHECK(screen.out("/t") > 0);

  // And it survives being deselected: the question is about the habit, not
  // about what is on screen now.
  const std::int32_t stamped = screen.out("/t");
  REQUIRE(screen.run("// void\nClearSelection(2);\n"));
  REQUIRE(screen.run("// void\n"
      "EnvWriteInt(\"/t\", GetNamedObj(\"NO_Subject\").obj._LastSelectionTime());\n"));
  CHECK(screen.out("/t") == stamped);
}

// --------------------------------------------------------------------------
// ExecDefaultCmd, the script form of the whole click
// --------------------------------------------------------------------------

/// **One call site, and it was the sole blocker of `GROUP_UNITSOUT.VS`.**
///
/// `ObjList::ExecDefaultCmd(pt, obj, bReplace, bModifier)` and the `Obj` form
/// are the click's own core (0x004f4ff0) called with the player -1. What is
/// under test: both receivers reach the same resolution and the same spread
/// as a click, the two flags are `SetCommand`/`AddCommand` and the `ctrl`
/// candidates, the verifiers run on the calling script's scheduler, control
/// is not re-tested but death is, and the queued commands are not marked as
/// a player action.
namespace {

struct Dispatch {
  ClassGraph graph = fixture_graph();
  World world;
  CommandSystem commands;
  HeroSystem heroes;
  HostRegistry registry;
  WorldHost host{world};
  Scheduler scheduler;
  HostContext context;

  Dispatch() {
    world.seed(5);
    world.set_class_graph(&graph);
    CommandTable table;
    (void)table.merge(bytes(kCommandsXml));
    commands.set_table(std::move(table));
    world.add_system(&commands);
    world.add_system(&heroes);
    for (PlayerId p = 0; p < 4; ++p) world.players().set(p, p, Relation::share_control, true);
    register_all_hosts(registry);
    scheduler.set_registry(&registry);
    scheduler.set_host(&host);
    context.world = &world;
    context.object_type = kTypeObj;
    scheduler.set_user(&context);
    install_objlist_lifetime(scheduler);
  }

  ObjectId spawn(PlayerId owner, Point at) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, graph.find("Unit"));
    world.set_position(id, at);
    world.set_owner(id, owner);
    world.set_health(id, 100);
    return id;
  }

  Value list_of(std::initializer_list<ObjectId> ids) {
    const ObjListId list = objlist_pool_of(world).acquire(7, static_cast<std::uint32_t>(lists++));
    objlist_pool_of(world).mutable_items(list)->assign(ids.begin(), ids.end());
    return make_objlist_value(list);
  }

  /// The call as the VM makes it, with or without a scheduler behind it.
  HostOutcome exec(Value receiver, Point at, ObjectId target, bool replace, bool modifier,
                   bool with_scheduler = true) {
    std::vector<Value> args = {receiver, pack_point(at),
                               target == kNoObject ? Value::object(ObjectRef{kNoType, 0})
                                                   : Value::object(ObjectRef{kTypeObj, target}),
                               Value::boolean(replace), Value::boolean(modifier)};
    const std::uint32_t index = registry.find(CallKind::member, "ExecDefaultCmd", 4);
    CHECK(index != kUnresolvedHost);
    if (index == kUnresolvedHost) return HostOutcome::failed("not declared");
    const HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return HostOutcome::failed("not implemented");
    CallContext ctx;
    ctx.arguments = args;
    ctx.host = &host;
    ctx.scheduler = with_scheduler ? &scheduler : nullptr;
    ctx.script = 7;
    ctx.user = &context;
    ctx.name = "ExecDefaultCmd";
    ctx.kind = CallKind::member;
    return entry.fn(ctx);
  }

  std::vector<std::string> verbs(ObjectId id) {
    std::vector<std::string> out;
    if (const CommandQueue* q = commands.find(id)) {
      for (const Command& c : q->entries) out.push_back(c.verb);
    }
    return out;
  }
  const Command* running(ObjectId id) {
    const CommandQueue* q = commands.find(id);
    return q == nullptr ? nullptr : q->running();
  }

  std::size_t lists = 0;
};

}  // namespace

TEST(exec_default_cmd_orders_a_list_like_a_click_and_marks_nobody) {
  Dispatch d;
  const ObjectId left = d.spawn(1, Point{80, 100});
  const ObjectId middle = d.spawn(1, Point{100, 100});
  const ObjectId right = d.spawn(1, Point{120, 100});

  CHECK(d.exec(d.list_of({left, middle, right}), Point{500, 500}, kNoObject, true, false).status ==
        HostStatus::ok);
  // The ground block's `move`, with the click's own spread: the centre lands
  // on the point and the others keep their offsets.
  CHECK((d.verbs(middle) == std::vector<std::string>{"move"}));
  CHECK(d.running(middle)->point == (Point{500, 500}));
  CHECK(d.running(left)->point == (Point{480, 500}));
  CHECK(d.running(right)->point == (Point{520, 500}));
  // Issued as player "nobody": not a player action.
  CHECK(!d.running(middle)->user);
  CHECK(d.running(middle)->arg_kind == CommandArgKind::point);
}

TEST(exec_default_cmd_on_an_object_receiver_runs_the_verifier_on_the_callers_scheduler) {
  // A `Hero` target's first candidate is `attach`, whose method carries a
  // `verify=`; `attack` behind it does not (the fixture declares it twice, as
  // the shipped file does, and the second declaration drops the verifier --
  // the header's overload note). So the verdict decides between the two.
  Dispatch d;
  REQUIRE(build(d.scheduler, d.registry,
                "// bool, Obj this, Obj other\nreturn !this.IsEnemy(other);",
                "data/subai/unit_attach_verify.vs"));
  const ObjectId mine = d.spawn(1, Point{100, 100});
  const ObjectId leader = d.world.spawn(NativeClass::hero, nullptr, d.graph.find("Hero"));
  d.world.set_position(leader, Point{300, 300});
  d.world.set_owner(leader, 1);
  d.world.set_health(leader, 100);

  // The `Obj` form: one actor, a friendly hero under the cursor, `attach`
  // verified by the real predicate, aimed at the object and not spread.
  CHECK(d.exec(Value::object(ObjectRef{kTypeObj, mine}), Point{300, 300}, leader, true, false)
            .status == HostStatus::ok);
  CHECK((d.verbs(mine) == std::vector<std::string>{"attach"}));
  CHECK(d.running(mine)->arg_kind == CommandArgKind::object);
  CHECK(d.running(mine)->object == leader);
  // The verifier left no coroutine behind.
  CHECK(d.scheduler.live_count() == 0);

  // An enemy hero: the predicate says no, and `attack` is the next candidate.
  const ObjectId enemy = d.world.spawn(NativeClass::hero, nullptr, d.graph.find("Hero"));
  d.world.set_position(enemy, Point{200, 200});
  d.world.set_owner(enemy, 2);
  d.world.set_health(enemy, 100);
  const ObjectId other = d.spawn(1, Point{100, 140});
  CHECK(d.exec(Value::object(ObjectRef{kTypeObj, other}), Point{200, 200}, enemy, true, false)
            .status == HostStatus::ok);
  CHECK((d.verbs(other) == std::vector<std::string>{"attack"}));
}

TEST(exec_default_cmd_without_a_scheduler_blocks_on_the_first_conditional_candidate) {
  Dispatch d;
  const ObjectId mine = d.spawn(1, Point{100, 100});
  const ObjectId leader = d.world.spawn(NativeClass::hero, nullptr, d.graph.find("Hero"));
  d.world.set_position(leader, Point{300, 300});
  d.world.set_owner(leader, 1);
  d.world.set_health(leader, 100);
  CHECK(d.exec(d.list_of({mine}), Point{300, 300}, leader, true, false, false).status ==
        HostStatus::ok);
  // `attach` needs a verifier nobody can run: blocked, nothing queued.
  CHECK(d.verbs(mine).empty());
  // Bare ground needs none.
  CHECK(d.exec(d.list_of({mine}), Point{400, 400}, kNoObject, true, false, false).status ==
        HostStatus::ok);
  CHECK((d.verbs(mine) == std::vector<std::string>{"move"}));
}

TEST(exec_default_cmd_appends_when_replace_is_false_and_reads_the_modifier) {
  Dispatch d;
  const ObjectId unit = d.spawn(1, Point{100, 100});
  CHECK(d.exec(d.list_of({unit}), Point{200, 200}, kNoObject, true, false).status == HostStatus::ok);
  CHECK(d.exec(d.list_of({unit}), Point{300, 300}, kNoObject, false, false).status == HostStatus::ok);
  CHECK((d.verbs(unit) == std::vector<std::string>{"move", "move"}));
  // `bModifier` selects the `ctrl="1"` candidate: `advance` on bare ground.
  CHECK(d.exec(d.list_of({unit}), Point{400, 400}, kNoObject, true, true).status == HostStatus::ok);
  CHECK((d.verbs(unit) == std::vector<std::string>{"advance"}));
}

TEST(exec_default_cmd_vouches_for_control_but_not_for_the_dead_or_the_missing) {
  Dispatch d;
  // Player 2 grants nobody control, and the script form does not ask.
  const ObjectId theirs = d.spawn(2, Point{100, 100});
  const ObjectId dead = d.spawn(1, Point{120, 100});
  d.world.set_health(dead, 0);
  CHECK(d.exec(d.list_of({theirs, dead, 999999}), Point{500, 500}, kNoObject, true, false).status ==
        HostStatus::ok);
  CHECK((d.verbs(theirs) == std::vector<std::string>{"move"}));
  CHECK(d.verbs(dead).empty());

  // A receiver that is neither a list nor a live object does nothing and
  // does not trap; a missing point is the caller's error.
  CHECK(d.exec(Value::integer(3), Point{1, 1}, kNoObject, true, false).status == HostStatus::ok);
  CHECK(d.exec(Value::object(ObjectRef{kTypeObj, 999999}), Point{1, 1}, kNoObject, true, false)
            .status == HostStatus::ok);
  // A world running no `CommandSystem` has no table and no queues: nothing
  // happens, and the script that asked keeps running.
  World bare;
  bare.set_class_graph(&d.graph);
  HostContext bare_context;
  bare_context.world = &bare;
  const ObjectId alone = bare.spawn(NativeClass::unit, nullptr, d.graph.find("Unit"));
  std::vector<Value> lonely = {Value::object(ObjectRef{kTypeObj, alone}), pack_point(Point{1, 1}),
                               Value::object(ObjectRef{kNoType, 0}), Value::boolean(true),
                               Value::boolean(false)};
  CallContext bare_ctx;
  bare_ctx.arguments = lonely;
  bare_ctx.user = &bare_context;
  CHECK(d.registry.entry(d.registry.find(CallKind::member, "ExecDefaultCmd", 4)).fn(bare_ctx).status ==
        HostStatus::ok);

  std::vector<Value> bad = {d.list_of({theirs}), Value::integer(1), Value::integer(0),
                            Value::boolean(true), Value::boolean(false)};
  CallContext ctx;
  ctx.arguments = bad;
  ctx.user = &d.context;
  CHECK(d.registry.entry(d.registry.find(CallKind::member, "ExecDefaultCmd", 4)).fn(ctx).status ==
        HostStatus::error);
}

/// `SetCmdEnable(false)` takes an object out of the reach of a player's
/// orders; `true` puts it back; a dead object is left as it is.
TEST(set_cmd_enable_decides_whether_an_object_is_commandable) {
  Dispatch d;
  const ObjectId unit = d.spawn(1, Point{100, 100});
  CHECK(is_commandable(d.world, unit, 1));
  const auto set = [&](ObjectId id, bool on) {
    std::vector<Value> args = {Value::object(ObjectRef{kTypeObj, id}), Value::boolean(on)};
    CallContext ctx;
    ctx.arguments = args;
    ctx.user = &d.context;
    return d.registry.entry(d.registry.find(CallKind::member, "SetCmdEnable", 1)).fn(ctx).status;
  };
  CHECK(set(unit, false) == HostStatus::ok);
  CHECK(d.world.find(unit)->state.flags.commands_disabled);
  CHECK(!is_commandable(d.world, unit, 1));
  // And the click's own path refuses it: nothing is queued.
  CHECK(d.exec(d.list_of({unit}), Point{500, 500}, kNoObject, true, false).status == HostStatus::ok);
  CHECK(d.verbs(unit).empty());
  CHECK(set(unit, true) == HostStatus::ok);
  CHECK(is_commandable(d.world, unit, 1));
  // Dead: the original prints and returns; the flag is untouched.
  d.world.set_health(unit, 0);
  CHECK(set(unit, false) == HostStatus::ok);
  CHECK(!d.world.find(unit)->state.flags.commands_disabled);
  CHECK(set(999999, false) == HostStatus::ok);
}
