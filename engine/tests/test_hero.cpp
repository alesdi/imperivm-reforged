// Heroes, squads, skills, progression and items.
//
// **No game data.** Every number asserted below is quoted, in the test that
// asserts it, from the file it came out of: `DATA\SKILLS.INI` for the skill
// descriptions, `DATA\CONST.INI` `[GamePlay]` for the engine constants,
// `DATA\ITEMS.XML` for the item bonuses, and `DATA\CLASSES\HERO.SC.XML` for
// `max_army="50"`. CI has no game installation and this project must never
// carry game assets, so the class definitions here are three-line fixtures
// written to the same schema.
//
// Three properties are worth more than the rest.
//
// **The rates must be exact.** `SKILLS.INI` gives unusually precise numbers --
// "+2 max attached warriors per skill point", "+5% speed per point" -- and a
// reimplementation that is close is a reimplementation that desyncs. So the
// arithmetic is checked at several point values, not one.
//
// **The squad shape must match the dumps.** Measured over the four desync dumps
// that contain heroes: every member of a hero's army prints the same
// `squad=<n>(<p>)` as the hero, 24 heroes of 24, and the squad's membership is
// exactly `1 + army size`, 24 of 24. `squad_is_the_hero_plus_its_army` is that
// finding as an assertion.
//
// **Partitioning a turn must not change anything.** Egoism fires on a
// five-second cadence and draws from the world RNG, so it is the one thing here
// that could care how the turn length was negotiated. It must not.

#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
namespace sim = imperivm::core::sim;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A class graph with a hero and a warrior, carrying only the properties this
/// domain reads. `max_army="50"` and `maxhealth="1000"` are `HERO.SC.XML`'s own
/// values; `maxstamina="10"` and `inventory_size="4"` are `UNIT.SC.XML`'s.
ClassGraph fixture_graph() {
  ClassGraph graph;
  graph.add(bytes(R"(<class id="Warrior" cpp_class="CVXUnit" parent="">
      <properties maxhealth="200" maxstamina="10" speed="50" inventory_size="4"/>
    </class>)"),
            "warrior.sc.xml");
  graph.add(bytes(R"(<class id="Hero" cpp_class="CVXHero" parent="Warrior">
      <properties maxhealth="1000" max_army="50" speed="120" heroarmyexpgain="40"/>
    </class>)"),
            "hero.sc.xml");
  // Every shipped sentry class lists `Freedom`; `RamUnit` is the class
  // `Unit::HasFreedom` asks after by name.
  graph.add(bytes(R"(<class id="Sentry" cpp_class="CVXUnit" parent="Warrior">
      <properties unit_specials="Keen sight, Freedom"/>
    </class>)"),
            "sentry.sc.xml");
  graph.add(bytes(R"(<class id="RamUnit" cpp_class="CVXUnit" parent="Warrior"/>)"),
            "ramunit.sc.xml");
  graph.add(bytes(R"(<class id="BCatapultUnit" cpp_class="CVXUnit" parent="RamUnit"/>)"),
            "bcatapultunit.sc.xml");
  graph.add(bytes(R"(<class id="Keen" cpp_class="CVXUnit" parent="Warrior">
      <properties unit_specials="Keen sight"/>
    </class>)"),
            "keen.sc.xml");
  graph.add(bytes(R"(<class id="Bag" cpp_class="CVXItemHolder" parent="">
      <properties inventory_size="16"/>
    </class>)"),
            "bag.sc.xml");
  // `DEFITEMHOLDER.SC.XML`'s `delete_empty="1"` -- the prop a dropped item
  // makes, which goes away once it is emptied. `DEADTREE` and `TREETRUNK`
  // declare 0 and stay, which is what `Bag` above stands in for.
  graph.add(bytes(R"(<class id="Chest" cpp_class="CVXItemHolder" parent="Bag">
      <properties delete_empty="1"/>
    </class>)"),
            "chest.sc.xml");
  graph.link();
  return graph;
}

/// A world, a graph, and a hero system already wired together.
struct Fixture {
  ClassGraph graph = fixture_graph();
  World world;
  HeroSystem heroes;
  ClassIndex hero_class = kNoClass;
  ClassIndex warrior_class = kNoClass;
  ClassIndex bag_class = kNoClass;
  ClassIndex chest_class = kNoClass;

  Fixture() {
    world.set_class_graph(&graph);
    hero_class = graph.find("Hero");
    warrior_class = graph.find("Warrior");
    bag_class = graph.find("Bag");
    chest_class = graph.find("Chest");
    world.add_system(&heroes);
  }

  ObjectId spawn_hero(PlayerId owner = 1, std::int32_t health = 1000) {
    const ObjectId id = world.spawn(NativeClass::hero, nullptr, hero_class);
    world.set_owner(id, owner);
    world.set_health(id, health);
    world.set_stamina(id, 10);
    heroes.register_hero(world, id);
    return id;
  }

  /// `points` unspent skill points: the balance derives from the level (one
  /// a level, 0x0052da80) over the skills the class offers, so a hero of
  /// level `points` offering everything has that many to spend.
  void grant(ObjectId hero, std::int32_t points) {
    if (HeroRecord* record = heroes.hero(hero)) {
      for (bool& offered : record->offered) offered = true;
    }
    (void)heroes.set_level(hero, points);
  }

  ObjectId spawn_warrior(PlayerId owner = 1, std::int32_t health = 200) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, warrior_class);
    world.set_owner(id, owner);
    world.set_health(id, health);
    world.set_stamina(id, 5);
    heroes.register_unit(world, id);
    return id;
  }

  ObjectId spawn_bag(ClassIndex which = kNoClass) {
    const ObjectId id =
        world.spawn(NativeClass::item_holder, nullptr, which == kNoClass ? bag_class : which);
    world.set_owner(id, 1);
    return id;
  }
};

/// Four items lifted straight out of `DATA\ITEMS.XML`, attributes and all.
constexpr std::string_view kItemsXml = R"(<items>
  <item id="Bear teeth amulet" level="0" name="Bear teeth amulet" important="no">
    <bonus health="0" damage="4" armor_slash="0" armor_pierce="0" level="0" experience="0"/>
  </item>
  <item id="King's Belt" level="0" name="King's belt" important="yes">
    <bonus health="600" damage="0" armor_slash="10" armor_pierce="10" level="0" experience="0"/>
  </item>
  <item id="Veteran Offence" level="0" usecount="0" name="Veteran Offence" important="no">
    <bonus health="0" damage="0" damage_percent="20" armor_slash="0" armor_pierce="0"
           level="0" experience="0"/>
  </item>
  <item id="Healing herbs" level="0" usecount="1" name="Healing herbs" important="yes"
        use_script="file://data/ItemScripts/healing herbs.vs">
    <bonus health="0" damage="0" armor_slash="0" armor_pierce="0" level="0" experience="0"/>
  </item>
  <item id="Boar teeth" level="0" name="Boar teeth" important="yes">
    <bonus health="0" damage="0" armor_slash="0" armor_pierce="0" level="5" experience="0"/>
  </item>
</items>)";

}  // namespace

// --------------------------------------------------------------------------
// skill identity
// --------------------------------------------------------------------------

TEST(the_twenty_five_skills_are_in_skills_ini_order) {
  // `DATA\SKILLS.INI` declares 25 sections, and `DATA\AI\HEROSKILL DEFAULT.VS`
  // lists the 25 skill script names in exactly this order.
  CHECK(kHeroSkillCount == 25);
  CHECK(hero_skill_name(HeroSkill::administration) == "Administration");
  CHECK(hero_skill_name(HeroSkill::team_attack) == "Team attack");
  CHECK(hero_skill_name(HeroSkill::discipline) == "Discipline");
  CHECK(hero_skill_name(HeroSkill::euphoria) == "Euphoria");
  CHECK(hero_skill_id("Administration") == 0);
  CHECK(hero_skill_id("Euphoria") == 24);
}

TEST(hero_skill_id_accepts_the_command_param_and_the_script_constant) {
  // `DATA\COMMANDS\HERO.XML` passes `param="Battle cry"` -- the section name --
  // to `HeroSkillId`, and `HERO_SKILL_BEHAVIOUR.VS` uses `hsBattleCry`.
  CHECK(hero_skill_id("Battle cry") == static_cast<std::int32_t>(HeroSkill::battle_cry));
  CHECK(hero_skill_id("hsBattleCry") == static_cast<std::int32_t>(HeroSkill::battle_cry));
  CHECK(hero_skill_id("battle CRY") == static_cast<std::int32_t>(HeroSkill::battle_cry));
  // `VERIFY_HERO_SKILL.VS` tests `if (skill < 0) return false`.
  CHECK(hero_skill_id("Not a skill") == -1);
  CHECK(hero_skill_id("") == -1);
}

TEST(active_skills_are_the_eight_with_a_command) {
  // The eight `command =` lines in `SKILLS.INI`, and the eight `skill_*`
  // entries in `DATA\COMMANDS\HERO.XML` with their `coststamina`.
  CHECK(hero_skill_stamina_cost(HeroSkill::battle_cry) == 6);
  CHECK(hero_skill_stamina_cost(HeroSkill::healing) == 6);
  CHECK(hero_skill_stamina_cost(HeroSkill::ceasefire) == 6);
  CHECK(hero_skill_stamina_cost(HeroSkill::charge) == 6);
  CHECK(hero_skill_stamina_cost(HeroSkill::assault) == 6);
  CHECK(hero_skill_stamina_cost(HeroSkill::frenzy) == 4);
  CHECK(hero_skill_stamina_cost(HeroSkill::rush) == 4);
  CHECK(hero_skill_stamina_cost(HeroSkill::defensive_cry) == 4);
  CHECK(hero_skill_stamina_cost(HeroSkill::administration) == 0);

  std::int32_t active = 0;
  for (std::size_t i = 0; i < kHeroSkillCount; ++i) {
    if (hero_skill_is_active(static_cast<HeroSkill>(i))) ++active;
  }
  CHECK(active == 8);
  // "4 is the minimal skill cost" -- HERO_SKILL_BEHAVIOUR.VS.
  CHECK(hero_skill_stamina_cost(HeroSkill::frenzy) == 4);
}

// --------------------------------------------------------------------------
// attachment
// --------------------------------------------------------------------------

TEST(attaching_sets_the_hero_handle_and_the_squad) {
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();

  CHECK(f.heroes.hero_of(warrior) == sim::kNoObject);
  CHECK(f.heroes.squad_of(warrior) == kNoSquad);

  REQUIRE(f.heroes.attach(f.world, warrior, hero));
  CHECK(f.heroes.hero_of(warrior) == hero);
  CHECK(f.heroes.squad_of(warrior) == f.heroes.squad_of(hero));
  CHECK(f.heroes.army_size(hero) == 1);
  CHECK(f.heroes.has_army(hero));
}

/// A unit or a hero the world spawned after the system adopted the map is
/// registered by `attach` itself; an object that is neither is still refused.
TEST(attaching_registers_a_late_spawn_on_demand) {
  Fixture f;
  f.heroes.start(f.world);
  // Spawned the way `Place` spawns: no `register_*` call anywhere.
  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr, f.hero_class);
  const ObjectId warrior = f.world.spawn(NativeClass::unit, nullptr, f.warrior_class);
  const ObjectId bag = f.world.spawn(NativeClass::item_holder, nullptr, f.bag_class);
  for (const ObjectId id : {hero, warrior, bag}) {
    f.world.set_owner(id, 1);
    f.world.set_health(id, 100);
  }
  CHECK(f.heroes.unit(warrior) == nullptr);
  CHECK(f.heroes.hero(hero) == nullptr);

  REQUIRE(f.heroes.attach(f.world, warrior, hero));
  CHECK(f.heroes.hero_of(warrior) == hero);
  REQUIRE(f.heroes.hero(hero) != nullptr);
  CHECK(f.heroes.hero(hero)->squad.valid());
  CHECK(f.heroes.squad_of(warrior) == f.heroes.hero(hero)->squad);
  // A bag is not a unit and gets no record for asking.
  CHECK(!f.heroes.attach(f.world, bag, hero));
  CHECK(f.heroes.unit(bag) == nullptr);
  // Nor is a plain unit a hero to attach to.
  const ObjectId other = f.world.spawn(NativeClass::unit, nullptr, f.warrior_class);
  f.world.set_owner(other, 1);
  f.world.set_health(other, 100);
  CHECK(!f.heroes.attach(f.world, other, warrior));
  CHECK(f.heroes.hero(warrior) == nullptr);
}

/// A squad remembers the **first** blow of a tick, not the last.
///
/// `gbr.exe` 0x0041e6c0 compares the squad's stored time against the current
/// tick and returns before writing anything when they are equal. So two
/// attackers landing in the same tick leave the *earlier* one on record, and
/// the pair `SQUADMONITOR.VS` reads -- the time and the attacker -- can never
/// describe two different blows.
TEST(a_squad_records_the_first_blow_of_a_tick_and_not_the_last) {
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  REQUIRE(f.heroes.attach(f.world, warrior, hero));
  const SquadKey key = f.heroes.squad_of(hero);
  REQUIRE(key.valid());
  const auto squad = [&] { return f.heroes.squads().find(key); };

  const ObjectId tower = f.spawn_warrior(2);
  const ObjectId archer = f.spawn_warrior(2);
  CHECK(squad()->last_fight_time == 0);
  CHECK(squad()->last_attacker == sim::kNoObject);

  record_squad_attacked(f.world, warrior, tower, 500);
  CHECK(squad()->last_fight_time == 500);
  CHECK(squad()->last_attacker == tower);

  // Same tick, second attacker: the record does not move.
  record_squad_attacked(f.world, hero, archer, 500);
  CHECK(squad()->last_attacker == tower);

  // A later tick does.
  record_squad_attacked(f.world, hero, archer, 900);
  CHECK(squad()->last_fight_time == 900);
  CHECK(squad()->last_attacker == archer);

  // Any member stamps the squad, and a unit in no squad stamps nothing.
  const ObjectId loner = f.spawn_warrior();
  record_squad_attacked(f.world, loner, tower, 1200);
  CHECK(squad()->last_fight_time == 900);

  // A dead victim is not attacked, it is finished with.
  f.world.set_health(warrior, 0);
  record_squad_attacked(f.world, warrior, tower, 1500);
  CHECK(squad()->last_fight_time == 900);
  f.world.set_health(warrior, 200);

  // And a spawn template is not in play at all.
  f.world.mutable_state(warrior)->flags.unspawned = true;
  record_squad_attacked(f.world, warrior, tower, 1500);
  CHECK(squad()->last_fight_time == 900);
  f.world.mutable_state(warrior)->flags.unspawned = false;
  record_squad_attacked(f.world, warrior, tower, 1500);
  CHECK(squad()->last_fight_time == 1500);

  // The attacker is stored as given: it is not re-checked for liveness, which
  // is why the shipped site tests `IsValid` itself.
  REQUIRE(f.world.despawn(tower));
  CHECK(squad()->last_attacker == tower);
  CHECK(f.world.find(tower) == nullptr);
}

TEST(squad_is_the_hero_plus_its_army) {
  // Measured over the four dumps with heroes: 24 of 24 heroes have exactly
  // `1 + army size` squad members, and every member's squad equals the hero's.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  std::vector<ObjectId> army;
  for (int i = 0; i < 7; ++i) {
    army.push_back(f.spawn_warrior());
    CHECK(f.heroes.attach(f.world, army.back(), hero));
  }

  const SquadKey key = f.heroes.squad_of(hero);
  REQUIRE(key.valid());
  const Squad* squad = f.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  CHECK(squad->size() == army.size() + 1);
  CHECK(squad->leader == hero);
  CHECK(squad->members[0] == hero);
  for (std::size_t i = 0; i < army.size(); ++i) {
    CHECK(squad->members[i + 1] == army[i]);      // attach order
    CHECK(f.heroes.squad_of(army[i]) == key);
  }
  // The squad's player is the hero's owner, which is why the dumps can derive
  // `p` from `SyncFlags`: 2,972 of 2,972 agreements, zero exceptions.
  CHECK(key.player == 1);
}

TEST(squad_indices_are_per_player_and_start_at_one) {
  // The dumps show index 4 existing simultaneously for players 0, 2, 3 and 14,
  // and 0 meaning "no squad".
  Fixture f;
  const ObjectId a = f.spawn_hero(0);
  const ObjectId b = f.spawn_hero(2);
  const ObjectId c = f.spawn_hero(0);
  CHECK(f.heroes.squad_of(a).index == 1);
  CHECK(f.heroes.squad_of(a).player == 0);
  CHECK(f.heroes.squad_of(b).index == 1);  // a different player's 1
  CHECK(f.heroes.squad_of(b).player == 2);
  CHECK(f.heroes.squad_of(c).index == 2);
}

TEST(the_attachment_cap_is_max_army_from_the_class) {
  // `HERO.SC.XML`: `max_army="50"`, and no hero in any dump exceeds it.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  CHECK(f.heroes.max_army(hero) == 50);

  for (int i = 0; i < 50; ++i) {
    const ObjectId w = f.spawn_warrior();
    CHECK(f.heroes.attach(f.world, w, hero));
  }
  CHECK(f.heroes.army_size(hero) == 50);
  CHECK(f.heroes.army_full(hero));

  const ObjectId one_too_many = f.spawn_warrior();
  CHECK(!f.heroes.attach(f.world, one_too_many, hero));
  CHECK(f.heroes.army_size(hero) == 50);
  CHECK(f.heroes.hero_of(one_too_many) == sim::kNoObject);
}

TEST(administration_raises_the_cap_by_two_per_point) {
  // SKILLS.INI: "Increases the maximum number of warriors attached to the hero
  // by 2 per skill point". CONST.INI: `UnitsPerAdministrationLevel = 2`.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  f.grant(hero, 10);

  for (std::int32_t points = 0; points <= 10; ++points) {
    REQUIRE(f.heroes.set_skill(hero, HeroSkill::administration, points));
    CHECK(f.heroes.max_army(hero) == 50 + 2 * points);
  }
  // Ten points is the goal every AI tactic script sets, and the cap it buys.
  CHECK(f.heroes.max_army(hero) == 70);
}

TEST(administration_lets_more_warriors_in_than_the_bare_cap) {
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  f.grant(hero, 3);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::administration, 3));
  CHECK(f.heroes.max_army(hero) == 56);

  for (int i = 0; i < 56; ++i) {
    CHECK(f.heroes.attach(f.world, f.spawn_warrior(), hero));
  }
  CHECK(f.heroes.army_size(hero) == 56);
  CHECK(!f.heroes.attach(f.world, f.spawn_warrior(), hero));
}

TEST(attachment_refuses_what_the_verify_script_refuses) {
  Fixture f;
  const ObjectId hero = f.spawn_hero(1);
  const ObjectId mine = f.spawn_warrior(1);
  const ObjectId theirs = f.spawn_warrior(2);

  CHECK(!f.heroes.attach(f.world, hero, hero));           // `if (me == h) return`
  CHECK(!f.heroes.attach(f.world, mine, sim::kNoObject));      // `!hero.IsValid()`
  CHECK(!f.heroes.attach(f.world, theirs, hero));         // `.IsEnemy(hero)`

  f.heroes.unit(mine)->has_freedom = true;
  CHECK(!f.heroes.attach(f.world, mine, hero));           // `if (.HasFreedom) return`
  f.heroes.unit(mine)->has_freedom = false;
  CHECK(f.heroes.attach(f.world, mine, hero));
  // `if (.hero == hero) return false` -- already there, nothing to do.
  CHECK(f.heroes.attach(f.world, mine, hero));
  CHECK(f.heroes.army_size(hero) == 1);
}

/// `Unit::HasFreedom` (0x005d7d30) is the specials word's `freedom` bit --
/// which the class's `unit_specials` fills -- or a `RamUnit` heir. Nothing set
/// the record's flag, so every sentry could be signed into a hero's army, and
/// `SQUADMONITOR.VS` did it: the towns' sentries marched off with the armies.
TEST(a_unit_whose_class_lists_freedom_or_descends_from_ramunit_is_free) {
  Fixture f;
  const ObjectId hero = f.spawn_hero(1);
  const auto spawn = [&](std::string_view cls) {
    const ObjectId id = f.world.spawn(NativeClass::unit, nullptr, f.graph.find(cls));
    f.world.set_owner(id, 1);
    f.world.set_health(id, 200);
    f.heroes.register_unit(f.world, id);
    return id;
  };
  const ObjectId sentry = spawn("Sentry");
  const ObjectId catapult = spawn("BCatapultUnit");
  const ObjectId keen = spawn("Keen");
  const ObjectId plain = f.spawn_warrior(1);
  CHECK(sim::unit_has_freedom(f.world, sentry));
  CHECK(sim::unit_has_freedom(f.world, catapult));
  CHECK(!sim::unit_has_freedom(f.world, keen));
  CHECK(!sim::unit_has_freedom(f.world, plain));
  CHECK(!sim::unit_has_freedom(f.world, sim::kNoObject));
  CHECK(!f.heroes.attach(f.world, sentry, hero));
  CHECK(!f.heroes.attach(f.world, catapult, hero));
  CHECK(f.heroes.attach(f.world, keen, hero));
  CHECK(f.heroes.attach(f.world, plain, hero));
  CHECK(f.heroes.army_size(hero) == 2);
}

TEST(detach_clears_the_hero_handle_and_the_squad) {
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId a = f.spawn_warrior();
  const ObjectId b = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, a, hero));
  CHECK(f.heroes.attach(f.world, b, hero));

  CHECK(f.heroes.detach(a));
  CHECK(f.heroes.hero_of(a) == sim::kNoObject);
  CHECK(f.heroes.squad_of(a) == kNoSquad);
  CHECK(f.heroes.army_size(hero) == 1);
  const Squad* squad = f.heroes.squads().find(f.heroes.squad_of(hero));
  REQUIRE(squad != nullptr);
  CHECK(squad->size() == 2);       // hero + b
  CHECK(squad->members[1] == b);   // order preserved
  CHECK(!f.heroes.detach(a));      // idempotent, and says so
}

TEST(detach_army_empties_the_squad_but_keeps_the_hero_in_it) {
  // `HERO_LEAVE_ARMY.VS` is one statement: `this.AsHero().DetachArmy();`
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  for (int i = 0; i < 5; ++i) CHECK(f.heroes.attach(f.world, f.spawn_warrior(), hero));

  CHECK(f.heroes.detach_army(hero) == 5);
  CHECK(f.heroes.army_size(hero) == 0);
  CHECK(!f.heroes.has_army(hero));
  const Squad* squad = f.heroes.squads().find(f.heroes.squad_of(hero));
  REQUIRE(squad != nullptr);
  CHECK(squad->size() == 1);
  CHECK(squad->members[0] == hero);
}

TEST(attaching_to_a_second_hero_leaves_the_first) {
  Fixture f;
  const ObjectId first = f.spawn_hero();
  const ObjectId second = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();

  CHECK(f.heroes.attach(f.world, warrior, first));
  CHECK(f.heroes.attach(f.world, warrior, second));
  CHECK(f.heroes.hero_of(warrior) == second);
  CHECK(f.heroes.army_size(first) == 0);
  CHECK(f.heroes.army_size(second) == 1);
  CHECK(f.heroes.squad_of(warrior) == f.heroes.squad_of(second));
}

TEST(armies_full_percent_is_over_a_players_heroes) {
  Fixture f;
  const ObjectId a = f.spawn_hero(3);
  const ObjectId b = f.spawn_hero(3);
  CHECK(f.heroes.armies_full_percent(3) == 0);
  CHECK(f.heroes.armies_full_percent(4) == 0);  // no hero at all

  for (int i = 0; i < 25; ++i) CHECK(f.heroes.attach(f.world, f.spawn_warrior(3), a));
  // 25 of a combined 100.
  CHECK(f.heroes.armies_full_percent(3) == 25);
  for (int i = 0; i < 25; ++i) CHECK(f.heroes.attach(f.world, f.spawn_warrior(3), b));
  CHECK(f.heroes.armies_full_percent(3) == 50);
}

// --------------------------------------------------------------------------
// skill arithmetic
// --------------------------------------------------------------------------

TEST(team_attack_adds_one_damage_per_point_to_every_attached_warrior) {
  // SKILLS.INI: "Increases the damage of every attached warrior by 1 per skill
  // point". CONST.INI: `AttackPerTeamAttackLevel = 1`.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId a = f.spawn_warrior();
  const ObjectId b = f.spawn_warrior();
  const ObjectId outsider = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, a, hero));
  CHECK(f.heroes.attach(f.world, b, hero));
  f.grant(hero, 10);

  for (std::int32_t points = 0; points <= 10; ++points) {
    REQUIRE(f.heroes.set_skill(hero, HeroSkill::team_attack, points));
    CHECK(f.heroes.modifiers_for(a).damage_add == points);
    CHECK(f.heroes.modifiers_for(b).damage_add == points);
    CHECK(f.heroes.modifiers_for(outsider).damage_add == 0);  // not attached
  }
}

TEST(team_defense_adds_one_armour_per_point_to_both_armour_types) {
  // SKILLS.INI: "+1 armour per point". CONST.INI:
  // `DefensePerTeamDefenseLevel = 1`. The class properties are `armor_slash`
  // and `armor_pierce`, so "armour" is both.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, warrior, hero));
  f.grant(hero, 10);

  for (std::int32_t points : {0, 1, 4, 7, 10}) {
    REQUIRE(f.heroes.set_skill(hero, HeroSkill::team_defense, points));
    const UnitModifiers mods = f.heroes.modifiers_for(warrior);
    CHECK(mods.armor_slash_add == points);
    CHECK(mods.armor_pierce_add == points);
  }
}

TEST(quick_march_adds_five_percent_speed_per_point_as_integer_arithmetic) {
  // SKILLS.INI: "+5% speed per point". CONST.INI:
  // `SpeedPercentPerQuickMarchLevel = 5`. Points sum into one percentage and
  // are applied once -- they do not compound, and there is no float anywhere.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, warrior, hero));
  f.grant(hero, 10);

  constexpr std::int32_t kBaseSpeed = 50;  // `UNIT.SC.XML` `speed="50"`
  for (std::int32_t points = 0; points <= 10; ++points) {
    REQUIRE(f.heroes.set_skill(hero, HeroSkill::quick_march, points));
    CHECK(f.heroes.modifiers_for(warrior).speed_percent == 5 * points);
  }
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::quick_march, 10));
  CHECK(apply_percent(kBaseSpeed, 50) == 75);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::quick_march, 1));
  // 50 * 5 / 100 = 2.5, truncated to 2. A rounding implementation would say 53.
  CHECK(apply_percent(kBaseSpeed, 5) == 52);
}

TEST(epic_endurance_adds_a_hundred_hero_max_health_per_point) {
  // SKILLS.INI: "+100 hero max health per point". CONST.INI:
  // `HealthPerEpicEnduranceLevel = 100`. It is on the hero, not on the army.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, warrior, hero));
  f.grant(hero, 10);

  for (std::int32_t points : {0, 1, 3, 10}) {
    REQUIRE(f.heroes.set_skill(hero, HeroSkill::epic_endurance, points));
    CHECK(f.heroes.hero_modifiers(hero).max_health_add == 100 * points);
    // `HERO.SC.XML` `maxhealth="1000"` is the base it lands on.
    CHECK(f.heroes.max_health_of(f.world, hero) == 1000 + 100 * points);
    CHECK(f.heroes.modifiers_for(warrior).max_health_add == 0);
  }
}

TEST(the_other_hero_only_skills_use_their_own_rates) {
  // `AttackPerEpicAttackLevel = 5`, `DefensePerEpicArmorLevel = 2`,
  // `SightPerScoutLevel = 50` -- all three from CONST.INI, and all three
  // matching their SKILLS.INI descriptions word for word.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  f.grant(hero, 30);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::epic_attack, 4));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::epic_armor, 3));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::scout, 6));

  const UnitModifiers mods = f.heroes.hero_modifiers(hero);
  CHECK(mods.damage_add == 20);
  CHECK(mods.armor_slash_add == 6);
  CHECK(mods.armor_pierce_add == 6);
  CHECK(mods.sight_add == 300);
}

TEST(the_chance_skills_report_percentages_and_never_roll_here) {
  // Concealment 2%/point (`AvoidChancePercentPerConcealmentLevel`), Euphoria
  // 10%/point (`PercentPerEuphoriaLevel`), and Vigor, Recovery and Survival at
  // the 10%/point their SKILLS.INI descriptions give -- those three have no
  // CONST.INI constant, which is the only place this file's numbers are not
  // from the engine's own table.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, warrior, hero));
  f.grant(hero, 30);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::concealment, 5));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::euphoria, 4));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::vigor, 3));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::recovery, 2));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::survival, 7));

  const UnitModifiers mods = f.heroes.modifiers_for(warrior);
  CHECK(mods.evade_percent == 10);
  CHECK(mods.euphoria_percent == 40);
  CHECK(mods.vigor_percent == 30);
  CHECK(mods.recovery_percent == 20);
  CHECK(mods.survival_percent == 70);

  // The RNG must be untouched: whether a draw happens is synchronised state,
  // and the system that resolves the event is the one that draws.
  const std::uint32_t before = f.world.rng().state();
  (void)f.heroes.modifiers_for(warrior);
  CHECK(f.world.rng().state() == before);
}

TEST(a_skill_cannot_be_bought_without_points_and_stops_at_ten) {
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  // One point per level (0x0052da80): a fresh hero is level 1 and has one.
  CHECK(f.heroes.available_skill_points(hero) == 1);
  CHECK(!f.heroes.set_skill(hero, HeroSkill::team_attack, 2));
  CHECK(f.heroes.skill(hero, HeroSkill::team_attack) == 0);
  // A level-up is a point without anyone granting it.
  const std::int32_t next = f.heroes.experience_to_next_level(1);
  REQUIRE(next > 0);
  (void)f.heroes.add_experience(hero, next);
  CHECK(f.heroes.inherent_level(hero) == 2);
  CHECK(f.heroes.available_skill_points(hero) == 2);

  f.grant(hero, 4);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::team_attack, 4));
  CHECK(f.heroes.available_skill_points(hero) == 0);
  // Selling back returns the points, which is what makes `SetSkill` safe for
  // the AI's `SetSkill(id, skillpoints + available)` idiom.
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::team_attack, 1));
  CHECK(f.heroes.available_skill_points(hero) == 3);

  f.grant(hero, 100);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::team_attack, 99));
  CHECK(f.heroes.skill(hero, HeroSkill::team_attack) == 10);

  // `HERO_SKILL_BEHAVIOUR.VS` tests `skillpoints < 0` explicitly.
  CHECK(f.heroes.skill(sim::kNoObject, HeroSkill::team_attack) == -1);
  CHECK(f.heroes.skill(hero, -1) == -1);
  CHECK(f.heroes.skill(hero, 25) == -1);
}

TEST(a_hero_class_offers_the_five_skills_its_heroskills_property_names) {
  // Fifteen classes declare `HeroSkills` and each names exactly five; the
  // remaining 99 `CVXHero` classes inherit one of those fifteen lists. The
  // shape is the shipped one: five entries, comma-separated, with spaces after
  // the commas and names that contain spaces themselves, so the split has to
  // trim without splitting on the space.
  //
  // legal-ok: the five names are `HeroSkill`'s own, spelled the way
  // `hero_skill_id` parses them -- `sim/hero.cpp` has to contain the same five
  // strings to resolve any of them. Renaming them here would test nothing.
  ClassGraph graph;
  graph.add(bytes(R"(<class id="Legion" cpp_class="CVXHero" parent="">
      <properties maxhealth="1000" max_army="50"/>
      <properties HeroSkills="Administration, Team attack, Team defense, Quick March, Discipline"/>
    </class>)"),
            "legion.sc.xml");
  graph.add(bytes(R"(<class id="Nameless" cpp_class="CVXHero" parent="">
      <properties maxhealth="1000" max_army="50"/>
    </class>)"),
            "nameless.sc.xml");
  graph.link();

  World world;
  world.set_class_graph(&graph);
  HeroSystem heroes;
  world.add_system(&heroes);

  const ObjectId legion = world.spawn(NativeClass::hero, nullptr, graph.find("Legion"));
  world.set_owner(legion, 1);
  world.set_health(legion, 1000);
  heroes.register_hero(world, legion);

  CHECK(heroes.offers_skill(legion, HeroSkill::administration));
  CHECK(heroes.offers_skill(legion, HeroSkill::team_attack));
  CHECK(heroes.offers_skill(legion, HeroSkill::team_defense));
  CHECK(heroes.offers_skill(legion, HeroSkill::quick_march));
  CHECK(heroes.offers_skill(legion, HeroSkill::discipline));
  CHECK(!heroes.offers_skill(legion, HeroSkill::battle_cry));
  CHECK(!heroes.offers_skill(legion, HeroSkill::egoism));

  std::int32_t offered = 0;
  for (std::size_t i = 0; i < kHeroSkillCount; ++i) {
    if (heroes.offers_skill(legion, static_cast<HeroSkill>(i))) ++offered;
  }
  CHECK(offered == 5);

  // Not a gate: nothing in the corpus shows the engine refusing a `SetSkill`
  // outside the list, and dropping an AI's request silently would be worse.
  heroes.set_level(legion, 3);
  CHECK(heroes.set_skill(legion, HeroSkill::battle_cry, 3));

  // A class that declares nothing offers everything, which is also what a
  // synthetic world with no class graph gets.
  const ObjectId nameless = world.spawn(NativeClass::hero, nullptr, graph.find("Nameless"));
  world.set_owner(nameless, 1);
  world.set_health(nameless, 1000);
  heroes.register_hero(world, nameless);
  for (std::size_t i = 0; i < kHeroSkillCount; ++i) {
    CHECK(heroes.offers_skill(nameless, static_cast<HeroSkill>(i)));
  }
}

// --------------------------------------------------------------------------
// discipline and levels
// --------------------------------------------------------------------------

TEST(discipline_sets_a_level_floor_of_two_plus_points) {
  // SKILLS.INI: "Increases the level of every attached warrior so it is at
  // least 2 plus the number of skill points". CONST.INI:
  // `DisciplineBonusBase = 2`, `DisciplineBonusStep = 1`.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId rookie = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, rookie, hero));
  f.grant(hero, 10);

  CHECK(f.heroes.level(rookie) == 1);
  for (std::int32_t points = 1; points <= 10; ++points) {
    REQUIRE(f.heroes.set_skill(hero, HeroSkill::discipline, points));
    CHECK(f.heroes.modifiers_for(rookie).level_floor == 2 + points);
    CHECK(f.heroes.level(rookie) == 2 + points);
    // It is a floor, not a write: the earned level is untouched.
    CHECK(f.heroes.inherent_level(rookie) == 1);
  }
}

TEST(discipline_never_lowers_a_veteran) {
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId veteran = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, veteran, hero));
  CHECK(f.heroes.set_level(veteran, 12));
  f.grant(hero, 10);

  REQUIRE(f.heroes.set_skill(hero, HeroSkill::discipline, 3));  // floor 5
  CHECK(f.heroes.level(veteran) == 12);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::discipline, 10));  // floor 12
  CHECK(f.heroes.level(veteran) == 12);
}

TEST(a_detached_warrior_loses_the_floor) {
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, warrior, hero));
  f.grant(hero, 10);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::discipline, 8));
  CHECK(f.heroes.level(warrior) == 10);
  CHECK(f.heroes.detach(warrior));
  CHECK(f.heroes.level(warrior) == 1);
}

// --------------------------------------------------------------------------
// timed skills
// --------------------------------------------------------------------------

TEST(battle_cry_lifts_levels_for_five_seconds_and_then_stops) {
  // SKILLS.INI: "When used increases the level of every attached warrior by 1
  // per skill point (for 5 seconds)". CONST.INI: `BattleCryTime = 5000`,
  // `LevelsPerBattleCryLevel = 1`. Game time is milliseconds at 100% speed.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, warrior, hero));
  f.grant(hero, 10);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::battle_cry, 4));

  CHECK(f.heroes.skill_duration(hero, HeroSkill::battle_cry) == 5000);
  REQUIRE(f.heroes.use_skill(f.world, hero, HeroSkill::battle_cry));
  CHECK(f.heroes.skill_in_effect(hero, HeroSkill::battle_cry, 0));
  CHECK(f.heroes.modifiers_for(warrior, 0).level_add == 4);
  CHECK(f.heroes.modifiers_for(warrior, 4999).level_add == 4);
  CHECK(f.heroes.modifiers_for(warrior, 5000).level_add == 0);

  // `VERIFY_HERO_SKILL.VS`: "Skill already in effect".
  CHECK(!f.heroes.use_skill(f.world, hero, HeroSkill::battle_cry));

  // Run past the end and the effect is cleared, whatever the turn length.
  f.world.advance(800);
  f.world.advance(800);
  f.world.advance_turns(6);
  CHECK(f.world.time() >= 5000);
  CHECK(!f.heroes.skill_in_effect(hero, HeroSkill::battle_cry, f.world.time()));
  CHECK(f.heroes.modifiers_for(warrior).level_add == 0);
  CHECK(f.heroes.use_skill(f.world, hero, HeroSkill::battle_cry));
}

TEST(the_timed_skills_use_their_own_base_and_step) {
  // CONST.INI, four pairs:
  //   CeasefireBaseTime 1000 + TimePerCeasefireLevel 1000 per point
  //   AssaultBaseTime   1000 + TimePerAssaultLevel   1000 per point
  //   FrenzyBaseTime    1000 + TimePerFrenzyLevel    1000 per point
  //   DefensiveCryBaseTime 1000 + TimePerDefensiveCryLevel 2000 per point
  // and SKILLS.INI says "1 second plus 1 additional second per skill point" for
  // the first three and "1 second plus 2 additional seconds" for the fourth.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  f.grant(hero, 40);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::ceasefire, 3));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::assault, 3));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::frenzy, 3));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::defensive_cry, 3));

  CHECK(f.heroes.skill_duration(hero, HeroSkill::ceasefire) == 4000);
  CHECK(f.heroes.skill_duration(hero, HeroSkill::assault) == 4000);
  CHECK(f.heroes.skill_duration(hero, HeroSkill::frenzy) == 4000);
  CHECK(f.heroes.skill_duration(hero, HeroSkill::defensive_cry) == 7000);
}

TEST(defensive_cry_assault_ceasefire_and_frenzy_land_on_the_army) {
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior(1, 100);
  CHECK(f.heroes.attach(f.world, warrior, hero));
  f.grant(hero, 40);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::defensive_cry, 2));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::assault, 2));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::ceasefire, 2));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::frenzy, 2));

  REQUIRE(f.heroes.use_skill(f.world, hero, HeroSkill::defensive_cry));
  REQUIRE(f.heroes.use_skill(f.world, hero, HeroSkill::assault));
  REQUIRE(f.heroes.use_skill(f.world, hero, HeroSkill::ceasefire));
  REQUIRE(f.heroes.use_skill(f.world, hero, HeroSkill::frenzy));

  const UnitModifiers mods = f.heroes.modifiers_for(warrior, 0);
  // "increases the armor of all attached warriors by 20" --
  // `DefensiveCryDefense = 20`, flat, not per point.
  CHECK(mods.armor_slash_add == 20);
  CHECK(mods.armor_pierce_add == 20);
  CHECK(mods.ignore_enemy_armor);
  CHECK(mods.ceasefire);
  CHECK(mods.frenzy);
  CHECK(mods.damage_percent == 100);  // "doubles the damage"
  // "halves the health", once, when the cry goes up.
  CHECK(f.world.state(warrior)->health == 50);
}

TEST(healing_charge_and_rush_are_instantaneous) {
  // SKILLS.INI: Healing "restores 10 health points per skill point"
  // (`HealthPerHealingLevel = 10`); Charge "+1 stamina per skill point"; Rush
  // "+1 stamina per skill point, while decreasing their health".
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior(1, 100);
  CHECK(f.heroes.attach(f.world, warrior, hero));
  f.grant(hero, 30);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::healing, 5));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::charge, 3));
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::rush, 2));

  REQUIRE(f.heroes.use_skill(f.world, hero, HeroSkill::healing));
  CHECK(f.world.state(warrior)->health == 150);
  CHECK(f.heroes.skill_duration(hero, HeroSkill::healing) == 0);
  // No lingering effect, so it can be used again straight away.
  CHECK(f.heroes.use_skill(f.world, hero, HeroSkill::healing));
  // `maxhealth="200"` on the fixture class clamps the overheal.
  CHECK(f.world.state(warrior)->health == 200);

  REQUIRE(f.heroes.use_skill(f.world, hero, HeroSkill::charge));
  CHECK(f.world.state(warrior)->stamina == 8);  // 5 + 3, under `maxstamina=10`

  const std::int32_t before = f.world.state(warrior)->health;
  REQUIRE(f.heroes.use_skill(f.world, hero, HeroSkill::rush));
  CHECK(f.world.state(warrior)->stamina == 10);
  // `RushDamage = 6` per point. SKILLS.INI says 5 and the AI heuristic in
  // HERO_SKILL_BEHAVIOUR.VS assumes 10; the engine constant is the one that
  // ran. If conformance ever disagrees here, this is the line to revisit.
  CHECK(f.world.state(warrior)->health == before - 12);
}

TEST(a_passive_skill_cannot_be_used_and_an_unlearned_one_cannot_either) {
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  f.grant(hero, 10);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::administration, 5));
  CHECK(!f.heroes.use_skill(f.world, hero, HeroSkill::administration));
  // "You need to have at least 1 point to can use this skill" -- verify script.
  CHECK(!f.heroes.use_skill(f.world, hero, HeroSkill::battle_cry));
}

// --------------------------------------------------------------------------
// experience
// --------------------------------------------------------------------------

TEST(the_experience_curve_is_the_one_const_ini_writes_out) {
  // ```ini
  // LevelExpDivider = 8 ; exp_gain = target_level / LevelExpDivider + 1;
  // ExpA=50  ExpB=125  ExpT=1
  // ; Exp[level] = Exp[Level-1] + Pos(level - T) * ExpA + ExpB
  // ```
  Fixture f;
  const HeroSystem& h = f.heroes;
  CHECK(h.experience_to_next_level(1) == 125);   // Pos(0)*50 + 125
  CHECK(h.experience_to_next_level(2) == 175);   // Pos(1)*50 + 125
  CHECK(h.experience_to_next_level(5) == 325);   // Pos(4)*50 + 125
  CHECK(h.experience_to_next_level(12) == 675);

  CHECK(h.experience_for_kill(0) == 1);
  CHECK(h.experience_for_kill(7) == 1);
  CHECK(h.experience_for_kill(8) == 2);
  CHECK(h.experience_for_kill(24) == 4);
}

TEST(experience_accumulates_within_a_level_and_resets_on_level_up) {
  // The dumps carry `level = 24` beside `experience = 74`, and the very first
  // increment is 125, so the dumped counter is progress within a level rather
  // than a running total.
  Fixture f;
  const ObjectId loner = f.spawn_warrior();
  CHECK(f.heroes.inherent_level(loner) == 1);

  CHECK(f.heroes.add_experience(loner, 100) == 0);
  CHECK(f.heroes.experience(loner) == 100);
  CHECK(f.heroes.inherent_level(loner) == 1);

  CHECK(f.heroes.add_experience(loner, 25) == 1);
  CHECK(f.heroes.inherent_level(loner) == 2);
  CHECK(f.heroes.experience(loner) == 0);

  // 175 to leave level 2, 225 to leave level 3: 400 buys both at once.
  CHECK(f.heroes.add_experience(loner, 400) == 2);
  CHECK(f.heroes.inherent_level(loner) == 4);
  CHECK(f.heroes.experience(loner) == 0);
}

TEST(a_heros_warriors_earn_a_premium_and_the_hero_takes_a_share) {
  // `HERO.SC.XML` `heroarmyexpgain="40"`, and CONST.INI
  // `ArmyBonusExperiencePercent = 40`, `ExpFromArmyDivider = 3`,
  // `ExpPercentPerLeadershipLevel = 2`.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, warrior, hero));

  CHECK(f.heroes.modifiers_for(warrior).experience_percent == 40);
  // Level 12 both sides, so the awards land well below the 675 the next level
  // costs and the counters can be read directly.
  CHECK(f.heroes.set_level(warrior, 12));
  CHECK(f.heroes.set_level(hero, 12));

  f.heroes.add_experience(warrior, 100);
  CHECK(f.heroes.experience(warrior) == 140);  // 100 + 40%
  CHECK(f.heroes.experience(hero) == 46);      // 140 / 3, truncated

  // "Increases the experience the hero provides to his attached warriors by 2%
  // per skill point."
  f.grant(hero, 10);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::leadership, 10));
  CHECK(f.heroes.modifiers_for(warrior).experience_percent == 60);
  CHECK(f.heroes.set_experience(warrior, 0));
  CHECK(f.heroes.set_experience(hero, 0));
  f.heroes.add_experience(warrior, 100);
  CHECK(f.heroes.experience(warrior) == 160);
  CHECK(f.heroes.experience(hero) == 53);

  // A hero of its own has no premium and no one to pass a share to.
  CHECK(f.heroes.modifiers_for(hero).experience_percent == 0);
}

TEST(set_level_writes_the_inherent_level_the_way_the_mushroom_does) {
  // `MUSHROOM.VS`: `.SetLevel(.inherentlevel + 1)`. Effective level then moves
  // with the modifiers on top; the earned level does not.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, warrior, hero));
  CHECK(f.heroes.set_level(warrior, f.heroes.inherent_level(warrior) + 1));
  CHECK(f.heroes.inherent_level(warrior) == 2);
  CHECK(f.heroes.level(warrior) == 2);

  f.grant(hero, 10);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::discipline, 6));
  CHECK(f.heroes.level(warrior) == 8);
  CHECK(f.heroes.inherent_level(warrior) == 2);
  // `BONUSSCRIPTS\003 HERO.VS` places a hero with `SetLevel(12)`.
  CHECK(f.heroes.set_level(hero, 12));
  CHECK(f.heroes.inherent_level(hero) == 12);
  CHECK(f.heroes.experience(hero) == 0);
}

// --------------------------------------------------------------------------
// items
// --------------------------------------------------------------------------

TEST(the_item_catalogue_reads_items_xml) {
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  CHECK(catalog.size() == 5);

  const ItemTypeIndex belt = catalog.find("King's Belt");
  REQUIRE(belt != kNoItemType);
  const ItemDefinition* def = catalog.at(belt);
  REQUIRE(def != nullptr);
  CHECK(def->name == "King's belt");
  CHECK(def->bonus.health == 600);
  CHECK(def->bonus.armor_slash == 10);
  CHECK(def->bonus.armor_pierce == 10);
  CHECK(def->important);

  const ItemDefinition* offence = catalog.at(catalog.find("Veteran Offence"));
  REQUIRE(offence != nullptr);
  CHECK(offence->bonus.damage_percent == 20);

  const ItemDefinition* herbs = catalog.at(catalog.find("Healing herbs"));
  REQUIRE(herbs != nullptr);
  CHECK(herbs->usecount == 1);
  // The `file://` scheme is stripped; the path is what the scheduler will load.
  CHECK(herbs->script(ItemScriptHook::use) == "data/ItemScripts/healing herbs.vs");

  CHECK(catalog.find("Not an item") == kNoItemType);
  // `BONUSSCRIPTS\103 BOAR TOOTH.VS` asks for "Boar tooth", which no item is.
  CHECK(catalog.find("Boar tooth") == kNoItemType);
  CHECK(catalog.find("bear teeth AMULET") != kNoItemType);  // tolerant lookup
}

TEST(an_item_is_held_by_its_owner_and_has_no_position_of_its_own) {
  // `docs/engine/state-vector.md`: a held object's position is `(-1,-1)` and
  // its location is its holder's. `CVXItem.owner handle` is that relationship.
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);

  const ObjectId warrior = f.spawn_warrior();
  f.world.set_position(warrior, Point{4000, 5000});

  const ObjectId item = f.heroes.items().add(f.world, warrior, "Bear teeth amulet");
  REQUIRE(item != sim::kNoObject);
  const ItemInstance* instance = f.heroes.items().find(item);
  REQUIRE(instance != nullptr);
  CHECK(instance->owner == warrior);
  CHECK(f.world.state(item)->holder == warrior);
  CHECK(f.world.state(item)->position == kHeldPosition);
  CHECK(f.world.resolve_position(item) == (Point{4000, 5000}));
  CHECK(f.world.find(item)->internal == InternalKind::item);

  CHECK(f.heroes.items().count_for(warrior) == 1);
  CHECK(f.heroes.items().has_type(warrior, "Bear teeth amulet"));
  CHECK(!f.heroes.items().has_type(warrior, "King's Belt"));
  CHECK(f.heroes.items().index_of(warrior, item) == 1);  // 1-based
}

TEST(item_bonuses_reach_the_owners_modifiers) {
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);

  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, warrior, hero));
  REQUIRE(f.heroes.items().add(f.world, warrior, "Bear teeth amulet") != sim::kNoObject);
  REQUIRE(f.heroes.items().add(f.world, warrior, "King's Belt") != sim::kNoObject);
  REQUIRE(f.heroes.items().add(f.world, warrior, "Veteran Offence") != sim::kNoObject);
  REQUIRE(f.heroes.items().add(f.world, warrior, "Boar teeth") != sim::kNoObject);

  const UnitModifiers mods = f.heroes.modifiers_for(warrior);
  CHECK(mods.damage_add == 4);
  CHECK(mods.damage_percent == 20);
  CHECK(mods.max_health_add == 600);
  CHECK(mods.armor_slash_add == 10);
  CHECK(mods.armor_pierce_add == 10);
  CHECK(mods.level_add == 5);  // `Boar teeth` is `<bonus level="5">`
  CHECK(f.heroes.level(warrior) == 6);
  // `UNIT.SC.XML` `maxhealth` is 200 in this fixture; the belt adds 600.
  CHECK(f.heroes.max_health_of(f.world, warrior) == 800);
}

TEST(inventory_size_caps_what_an_owner_can_hold) {
  // `UNIT.SC.XML` `inventory_size="4"`, `ITEMHOLDER.SC.XML` 16.
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);

  const ObjectId warrior = f.spawn_warrior();
  CHECK(f.heroes.items().capacity_of(f.world, warrior) == 4);
  for (int i = 0; i < 4; ++i) {
    CHECK(f.heroes.items().add(f.world, warrior, "Bear teeth amulet") != sim::kNoObject);
  }
  // The shipped bonus scripts test the result of `AddItem` and retry, so a
  // refusal is ordinary.
  CHECK(f.heroes.items().add(f.world, warrior, "Bear teeth amulet") == sim::kNoObject);
  CHECK(f.heroes.items().count_for(warrior) == 4);

  const ObjectId bag = f.spawn_bag();
  CHECK(f.heroes.items().capacity_of(f.world, bag) == 16);
}

TEST(giving_putting_and_dropping_move_the_holder_relationship) {
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);

  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  const ObjectId bag = f.spawn_bag();
  f.world.set_position(bag, Point{100, 200});

  const ObjectId item = f.heroes.items().add(f.world, hero, "King's Belt");
  REQUIRE(item != sim::kNoObject);

  CHECK(f.heroes.items().give(f.world, item, warrior));
  CHECK(f.heroes.items().count_for(hero) == 0);
  CHECK(f.heroes.items().count_for(warrior) == 1);
  CHECK(f.world.state(item)->holder == warrior);

  CHECK(f.heroes.items().put(f.world, item, bag));
  CHECK(f.heroes.items().count_for(bag) == 1);
  CHECK(f.world.state(item)->holder == bag);

  CHECK(f.heroes.items().drop(f.world, item, Point{7000, 8000}));
  CHECK(f.heroes.items().count_for(bag) == 0);
  CHECK(f.world.state(item)->holder == sim::kNoObject);
  CHECK(f.world.state(item)->position == (Point{7000, 8000}));
}

TEST(a_consumable_is_destroyed_when_its_charges_run_out) {
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);
  const ObjectId warrior = f.spawn_warrior();

  const ObjectId herbs = f.heroes.items().add(f.world, warrior, "Healing herbs");
  REQUIRE(herbs != sim::kNoObject);
  CHECK(f.heroes.items().use_count(herbs) == 1);  // `usecount="1"`
  CHECK(f.heroes.items().use(f.world, herbs, 1));
  CHECK(f.heroes.items().find(herbs) == nullptr);
  CHECK(f.world.find(herbs) == nullptr);
  CHECK(f.heroes.items().count_for(warrior) == 0);

  // `usecount="0"` is not a consumable -- `Bear teeth amulet` is worn, not
  // spent -- so spending from it never destroys it.
  const ObjectId amulet = f.heroes.items().add(f.world, warrior, "Bear teeth amulet");
  REQUIRE(amulet != sim::kNoObject);
  CHECK(f.heroes.items().use(f.world, amulet, 5));
  CHECK(f.heroes.items().find(amulet) != nullptr);
  // `Ring of Power`'s script grows its own counter from zero; `set_use_count`
  // is how it does that.
  f.heroes.items().set_use_count(amulet, 500);
  CHECK(f.heroes.items().use_count(amulet) == 500);
}

TEST(exchange_item_keeps_the_handle_and_changes_the_type) {
  // `VETERAN GUILD.VS`: `owner.ExchangeItem(this, "Veteran Medal")`.
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);
  const ObjectId warrior = f.spawn_warrior();

  const ObjectId item = f.heroes.items().add(f.world, warrior, "Bear teeth amulet");
  REQUIRE(item != sim::kNoObject);
  CHECK(f.heroes.modifiers_for(warrior).damage_add == 4);
  CHECK(f.heroes.items().exchange(item, "King's Belt"));
  CHECK(f.heroes.items().find(item)->id == item);  // same handle
  CHECK(f.heroes.modifiers_for(warrior).damage_add == 0);
  CHECK(f.heroes.modifiers_for(warrior).max_health_add == 600);
  CHECK(!f.heroes.items().exchange(item, "Not an item"));
}

TEST(remove_items_of_type_takes_them_all) {
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);
  const ObjectId warrior = f.spawn_warrior();

  for (int i = 0; i < 3; ++i) {
    REQUIRE(f.heroes.items().add(f.world, warrior, "Bear teeth amulet") != sim::kNoObject);
  }
  REQUIRE(f.heroes.items().add(f.world, warrior, "King's Belt") != sim::kNoObject);
  CHECK(f.heroes.items().remove_all_of_type(f.world, warrior, "Bear teeth amulet") == 3);
  CHECK(f.heroes.items().count_for(warrior) == 1);
  CHECK(f.heroes.items().has_type(warrior, "King's Belt"));
}

TEST(forgetting_an_object_disbands_its_army_and_destroys_its_items) {
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);

  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, warrior, hero));
  REQUIRE(f.heroes.items().add(f.world, hero, "King's Belt") != sim::kNoObject);

  f.heroes.forget(f.world, hero);
  CHECK(f.heroes.hero(hero) == nullptr);
  CHECK(f.heroes.hero_of(warrior) == sim::kNoObject);
  CHECK(f.heroes.squad_of(warrior) == kNoSquad);
  CHECK(f.heroes.items().size() == 0);
}

// --------------------------------------------------------------------------
// the turn
// --------------------------------------------------------------------------

TEST(clear_dead_removes_despawned_warriors_from_the_army) {
  // `.army.ClearDead` opens both `HERO_SKILL_BEHAVIOUR.VS` and
  // `HERO_DETACH_BEHAVIOR.VS`.
  Fixture f;
  const ObjectId hero = f.spawn_hero();
  const ObjectId a = f.spawn_warrior();
  const ObjectId b = f.spawn_warrior();
  CHECK(f.heroes.attach(f.world, a, hero));
  CHECK(f.heroes.attach(f.world, b, hero));

  f.world.despawn(a);
  f.world.advance(400);
  CHECK(f.heroes.army_size(hero) == 1);
  CHECK(f.heroes.hero_of(a) == sim::kNoObject);
  const Squad* squad = f.heroes.squads().find(f.heroes.squad_of(hero));
  REQUIRE(squad != nullptr);
  CHECK(squad->size() == 2);
  CHECK(!squad->contains(a));
}

TEST(egoism_drains_a_warrior_on_a_five_second_cadence) {
  // SKILLS.INI: "Once every 5 seconds the hero steals up to 20 health points
  // per skill point from a random warrior attached to him." CONST.INI:
  // `HealthPerEgoismLevel = 20`.
  Fixture f;
  const ObjectId hero = f.spawn_hero(1, 100);
  const ObjectId warrior = f.spawn_warrior(1, 200);
  CHECK(f.heroes.attach(f.world, warrior, hero));
  f.grant(hero, 10);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::egoism, 2));  // 40 a drain

  f.world.advance(400);  // arms the cadence
  CHECK(f.world.state(warrior)->health == 200);
  f.world.advance_turns(20);  // to 8,400: one drain at 5,400
  CHECK(f.world.time() == 8400);
  CHECK(f.world.state(warrior)->health == 160);
  CHECK(f.world.state(hero)->health == 140);

  f.world.advance_turns(10);  // to 12,400: a second drain at 10,400
  CHECK(f.world.state(warrior)->health == 120);
  CHECK(f.world.state(hero)->health == 180);
}

TEST(a_turn_partition_does_not_change_a_hero) {
  // The property the whole determinism contract rests on: any sequence of
  // declared turn lengths must leave the world where the single turn of their
  // sum would. Egoism is the case that could break it -- it fires on a fixed
  // cadence and draws from the world RNG -- so it is the case built here.
  const auto run = [](const std::vector<std::int32_t>& lengths) {
    Fixture f;
    f.world.seed(0x1234abcd);
    const ObjectId hero = f.spawn_hero(1, 100);
    std::vector<ObjectId> army;
    for (int i = 0; i < 6; ++i) {
      army.push_back(f.spawn_warrior(1, 500));
      f.heroes.attach(f.world, army.back(), hero);
    }
    f.grant(hero, 10);
    f.heroes.set_skill(hero, HeroSkill::egoism, 3);
    f.heroes.set_skill(hero, HeroSkill::battle_cry, 4);
    f.heroes.use_skill(f.world, hero, HeroSkill::battle_cry);
    for (const std::int32_t length : lengths) f.world.advance(length);

    std::uint64_t hash = 0xcbf29ce484222325ull;
    f.heroes.hash(hash);
    // Plus the health Egoism moved and the RNG state the draws left behind.
    // `World::state_hash()` is deliberately not folded in: it covers the whole
    // slot table including the turn counter, which of course differs between a
    // single turn and a hundred and fifty of them.
    for (const UnitRecord& record : f.heroes.units()) {
      hash = (hash ^ static_cast<std::uint64_t>(
                         static_cast<std::uint32_t>(f.world.state(record.id)->health))) *
             0x100000001b3ull;
    }
    return hash ^ f.world.rng().state();
  };

  constexpr std::int32_t kTotal = 60000;
  const std::vector<std::int32_t> whole{kTotal};
  const std::vector<std::int32_t> many(150, 400);

  // The four lengths the nine dumps actually contain, cycled, with the last
  // turn trimmed so the three sequences cover exactly the same interval.
  std::vector<std::int32_t> ragged;
  constexpr std::int32_t kObserved[] = {800, 400, 799, 200};
  for (std::int32_t total = 0, i = 0; total < kTotal; ++i) {
    std::int32_t next = kObserved[i % 4];
    if (total + next > kTotal) next = kTotal - total;
    ragged.push_back(next);
    total += next;
  }

  CHECK(run(whole) == run(many));
  CHECK(run(whole) == run(ragged));

  // And two identical worlds are identical, which is the weaker half of the
  // same statement and the one a regression usually breaks first.
  CHECK(run(many) == run(many));
}

TEST(two_identical_worlds_produce_identical_squads) {
  const auto build = []() {
    Fixture f;
    const ObjectId a = f.spawn_hero(0);
    const ObjectId b = f.spawn_hero(1);
    for (int i = 0; i < 4; ++i) f.heroes.attach(f.world, f.spawn_warrior(0), a);
    for (int i = 0; i < 9; ++i) f.heroes.attach(f.world, f.spawn_warrior(1), b);
    f.heroes.detach_army(a);
    for (int i = 0; i < 3; ++i) f.heroes.attach(f.world, f.spawn_warrior(0), a);
    std::uint64_t hash = 0xcbf29ce484222325ull;
    f.heroes.hash(hash);
    return hash;
  };
  CHECK(build() == build());
}

TEST(start_adopts_a_populated_world) {
  ClassGraph graph = fixture_graph();
  World world;
  world.set_class_graph(&graph);
  HeroSystem heroes;
  world.add_system(&heroes);

  const ObjectId hero = world.spawn(NativeClass::hero, nullptr, graph.find("Hero"));
  const ObjectId warrior = world.spawn(NativeClass::unit, nullptr, graph.find("Warrior"));
  world.set_owner(hero, 2);
  world.set_owner(warrior, 2);
  world.set_health(hero, 1000);
  world.set_health(warrior, 200);
  world.start();

  // `flags_for_native_class` marks the hero, so `start` finds it.
  REQUIRE(heroes.hero(hero) != nullptr);
  REQUIRE(heroes.unit(warrior) != nullptr);
  CHECK(heroes.hero(warrior) == nullptr);
  CHECK(heroes.squad_of(hero).valid());
  CHECK(heroes.squad_of(hero).player == 2);
  CHECK(heroes.max_army(hero) == 50);
  CHECK(heroes.attach(world, warrior, hero));
}

/// `IncKills` adds, does not clamp, and refuses a dead receiver.
///
/// `gbr.exe` 0x005d7810 is one `add` to the field the serialiser names `kills`,
/// behind two guards that both report and both return having written nothing:
/// an unresolvable receiver, and a dead one through the `IsDead` virtual --
/// so a unit that dies in the same tick as the kill it scored does not get the
/// credit.
/// **Three entry points over two slots**, and the shape of the family is that
/// `Get` does not read what `Set` writes.
///
/// `Set` writes the *request* and `Has` tests it; `Get` reads the *settled*
/// orientation, which the party-formation code fills and this engine does not
/// model. So the pair `Set`/`Has` works end to end, and `Get` answers the unset
/// value -- which is what the original answers for any party that has not
/// finished forming, and what all three readers are shaped for.
TEST(final_party_orientation_is_a_request_slot_and_a_settled_one) {
  Fixture f;
  const ObjectId hero = f.spawn_hero();

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_hero_host(registry);
  HostContext context;
  context.world = &f.world;
  const auto call = [&](const char* name, std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(script::CallKind::member, name,
                                              static_cast<std::uint16_t>(args.size() - 1));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };
  const script::Value self = script::Value::object(kTypeObj, hero);

  // Unset is `(1024, 1024)`, which is what the readers subtract back to zero.
  CHECK(unpack_point(call("GetFinalPartyOrientation", {self}).value) == (Point{1024, 1024}));
  CHECK(!call("HasFinalPartyOrientationRequest", {self}).value.truthy_scalar());

  // A request is stored with the bias taken off, so `Has` sees it...
  CHECK(call("SetFinalPartyOrientation", {self, pack_point(Point{1024 + 30, 1024 - 12})}).status ==
        script::HostStatus::ok);
  CHECK(f.heroes.hero(hero)->party_orientation_request == (Point{30, -12}));
  CHECK(call("HasFinalPartyOrientationRequest", {self}).value.truthy_scalar());
  // ...and `Get` still answers the settled slot, which nothing has filled.
  CHECK(unpack_point(call("GetFinalPartyOrientation", {self}).value) == (Point{1024, 1024}));

  // **A request of exactly the bias is no request**, because the test is on the
  // stored value and the bias exists so that "unset" can be zero.
  CHECK(call("SetFinalPartyOrientation", {self, pack_point(Point{1024, 1024})}).status ==
        script::HostStatus::ok);
  CHECK(!call("HasFinalPartyOrientationRequest", {self}).value.truthy_scalar());

  // **One axis is enough.** The original ORs the two words, so a request that
  // is due north -- zero on one axis -- is still a request.
  CHECK(call("SetFinalPartyOrientation", {self, pack_point(Point{1024 + 5, 1024})}).status ==
        script::HostStatus::ok);
  CHECK(f.heroes.hero(hero)->party_orientation_request == (Point{5, 0}));
  CHECK(call("HasFinalPartyOrientationRequest", {self}).value.truthy_scalar());
  CHECK(call("SetFinalPartyOrientation", {self, pack_point(Point{1024, 1024 - 8})}).status ==
        script::HostStatus::ok);
  CHECK(call("HasFinalPartyOrientationRequest", {self}).value.truthy_scalar());

  // The settled slot, when something does fill it, comes back biased.
  f.heroes.hero(hero)->party_orientation = Point{-7, 9};
  CHECK(unpack_point(call("GetFinalPartyOrientation", {self}).value) ==
        (Point{1024 - 7, 1024 + 9}));

  // A receiver that names no hero: `(-1, -1)` from `Get` -- which the readers
  // then turn into a large negative point rather than into "no orientation",
  // and that is the original's answer rather than a tidier one -- `false` from
  // `Has`, and nothing at all from `Set`.
  const script::Value nobody = script::Value::object(kTypeObj, static_cast<ObjectId>(9999));
  CHECK(unpack_point(call("GetFinalPartyOrientation", {nobody}).value) == (Point{-1, -1}));
  CHECK(!call("HasFinalPartyOrientationRequest", {nobody}).value.truthy_scalar());
  CHECK(call("SetFinalPartyOrientation", {nobody, pack_point(Point{1, 1})}).status ==
        script::HostStatus::ok);
}

TEST(inc_kills_adds_to_a_living_unit_and_to_nothing_else) {
  Fixture f;
  const ObjectId warrior = f.spawn_warrior();
  const ObjectId corpse = f.spawn_warrior();

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_hero_host(registry);
  HostContext context;
  context.world = &f.world;
  const auto inc = [&](ObjectId id, std::int32_t n) {
    const std::uint32_t index = registry.find(script::CallKind::member, "IncKills", 1);
    CHECK(index != script::kUnresolvedHost);
    script::CallContext ctx;
    std::vector<script::Value> args{script::Value::object(kTypeObj, id),
                                    script::Value::integer(n)};
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "IncKills";
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };

  CHECK(f.heroes.kills(warrior) == 0);
  CHECK(inc(warrior, 1).status == script::HostStatus::ok);
  CHECK(inc(warrior, 1).status == script::HostStatus::ok);
  CHECK(f.heroes.kills(warrior) == 2);

  // No clamp in either direction: the original is one `add`.
  CHECK(inc(warrior, -5).status == script::HostStatus::ok);
  CHECK(f.heroes.kills(warrior) == -3);

  // A dead receiver books nothing, and does not refuse either.
  f.world.set_health(corpse, 0);
  CHECK(inc(corpse, 1).status == script::HostStatus::ok);
  CHECK(f.heroes.kills(corpse) == 0);

  // And a handle that names nothing.
  CHECK(inc(kNoObject, 1).status == script::HostStatus::ok);
  CHECK(inc(static_cast<ObjectId>(9999), 1).status == script::HostStatus::ok);
}

/// `OpenItemHolder` empties a holder into the receiver, back to front, and
/// stops when the receiver is full.
///
/// 0x0053c350 walks the holder's slots **downwards**, which is only observable
/// when the receiver runs out of room: the holder keeps its *lowest* slots. The
/// bool is "at least one item moved", not "a window opened" -- the only user
/// interface on this path is the floating caption at 0x005a93a0, which is
/// gated on the local camera and the local player's fog and is deliberately
/// absent here.
TEST(open_item_holder_empties_a_holder_back_to_front_until_the_taker_is_full) {
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_hero_host(registry);
  HostContext context;
  context.world = &f.world;
  const auto open = [&](ObjectId taker, ObjectId holder) -> bool {
    const std::uint32_t index =
        registry.find(script::CallKind::member, "OpenItemHolder", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return false;
    script::CallContext ctx;
    std::vector<script::Value> args{script::Value::object(kTypeObj, taker),
                                    script::Value::object(kTypeObj, holder)};
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "OpenItemHolder";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.truthy_scalar();
  };

  // `Warrior` declares `inventory_size="4"`, `Bag` 16 -- so six items go in and
  // only four come out.
  const ObjectId warrior = f.spawn_warrior();
  const ObjectId bag = f.spawn_bag();
  std::vector<ObjectId> put;
  for (int i = 0; i < 6; ++i) {
    const ObjectId item = f.heroes.items().add(f.world, bag, "Bear teeth amulet");
    REQUIRE(item != sim::kNoObject);
    put.push_back(item);
  }

  CHECK(open(warrior, bag));
  CHECK(f.heroes.items().count_for(warrior) == 4);
  CHECK(f.heroes.items().count_for(bag) == 2);
  // Back to front: the two left behind are the two *lowest* handles, which is
  // the half a forward walk would get exactly backwards.
  std::vector<ObjectId> left;
  (void)f.heroes.items().contents_for(bag, left);
  REQUIRE(left.size() == 2);
  CHECK(left[0] == put[0]);
  CHECK(left[1] == put[1]);

  // Full already: nothing moves and the answer is false, not a refusal.
  CHECK(!open(warrior, bag));
  CHECK(f.heroes.items().count_for(bag) == 2);

  // `delete_empty="1"` takes the emptied holder with it; a plain `Bag` stays.
  const ObjectId hero = f.spawn_hero();
  const ObjectId chest = f.spawn_bag(f.chest_class);
  REQUIRE(f.heroes.items().add(f.world, chest, "King's Belt") != sim::kNoObject);
  CHECK(open(hero, chest));
  CHECK(f.world.find(chest) == nullptr);

  const ObjectId hero2 = f.spawn_hero();
  const ObjectId plain = f.spawn_bag();
  REQUIRE(f.heroes.items().add(f.world, plain, "King's Belt") != sim::kNoObject);
  CHECK(open(hero2, plain));
  CHECK(f.world.find(plain) != nullptr);

  // A `delete_empty` holder that is only *partly* emptied stays: the guard is
  // on the holder being empty, not on anything having moved.
  const ObjectId hero3 = f.spawn_hero();
  const ObjectId big_chest = f.spawn_bag(f.chest_class);
  for (int i = 0; i < 6; ++i) {
    REQUIRE(f.heroes.items().add(f.world, big_chest, "Bear teeth amulet") != sim::kNoObject);
  }
  CHECK(open(hero3, big_chest));
  CHECK(f.heroes.items().count_for(big_chest) == 2);
  CHECK(f.world.find(big_chest) != nullptr);

  // An empty holder moves nothing, so it is false -- and it is not destroyed
  // either, because the original guards that on `moved` as well.
  const ObjectId empty = f.spawn_bag(f.chest_class);
  CHECK(!open(hero2, empty));
  CHECK(f.world.find(empty) != nullptr);

  // A dead receiver, and a holder that names nothing.
  const ObjectId corpse = f.spawn_warrior();
  const ObjectId loot = f.spawn_bag();
  REQUIRE(f.heroes.items().add(f.world, loot, "King's Belt") != sim::kNoObject);
  f.world.set_health(corpse, 0);
  CHECK(!open(corpse, loot));
  CHECK(f.heroes.items().count_for(loot) == 1);
  CHECK(!open(warrior, static_cast<ObjectId>(9999)));
  CHECK(!open(static_cast<ObjectId>(9999), loot));
}

/// The tally is saved and hashed, on `ObjectState::traversed_by`'s precedent:
/// nothing in this engine reads it yet, and a field two peers could disagree
/// about is a field the hash has to carry.
TEST(the_kill_tally_round_trips_and_moves_the_hash) {
  Fixture f;
  const ObjectId warrior = f.spawn_warrior();
  std::uint64_t before = 0;
  f.heroes.hash(before);
  CHECK(f.heroes.add_kills(warrior, 7));
  std::uint64_t after = 0;
  f.heroes.hash(after);
  CHECK(after != before);
  CHECK(!f.heroes.add_kills(static_cast<ObjectId>(9999), 1));

  std::vector<std::byte> saved;
  f.heroes.serialize(saved);
  HeroSystem restored;
  REQUIRE(restored.deserialize(saved).ok());
  CHECK(restored.kills(warrior) == 7);
  std::uint64_t round_tripped = 0;
  restored.hash(round_tripped);
  CHECK(round_tripped == after);

  // A section written by the previous shape is refused rather than read short.
  // The literal 5 is the point: if `kSectionVersion` had not moved, the bytes
  // above would already carry it and this patch would be a no-op.
  std::vector<std::byte> older = saved;
  REQUIRE(older.size() > 8);
  older[4] = std::byte{5};
  older[5] = std::byte{0};
  older[6] = std::byte{0};
  older[7] = std::byte{0};
  HeroSystem refused;
  CHECK(!refused.deserialize(older).ok());
}

// --------------------------------------------------------------------------
// host bindings
// --------------------------------------------------------------------------

TEST(the_hero_host_entry_points_are_defined_and_dispatch) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  const std::size_t before = registry.implemented();
  define_hero_host(registry);
  CHECK(registry.implemented() > before);

  // The names must be the shipped ones, at the shipped arities, or the
  // declaration and the definition are two different entry points.
  const auto member = [&](std::string_view name, std::uint16_t arity) {
    const std::uint32_t index = registry.find(script::CallKind::member, name, arity);
    if (index == script::kUnresolvedHost) return false;
    return registry.entry(index).fn != nullptr;
  };
  CHECK(member("hero", 0));
  CHECK(member("army", 0) == false);  // an ObjList: deliberately not ours
  CHECK(member("experience", 0));
  CHECK(member("inherentlevel", 0));
  CHECK(member("level", 0));
  CHECK(member("SetLevel", 1));
  CHECK(member("SetExperience", 1));
  CHECK(member("IncKills", 1));
  CHECK(member("AttachTo", 1));
  CHECK(member("DetachFrom", 1));
  CHECK(member("DetachArmy", 0));
  CHECK(member("HasArmy", 0));
  CHECK(member("IsHeroArmyFull", 0));
  CHECK(member("GetSkill", 1));
  CHECK(member("SetSkill", 2));
  CHECK(member("UseSkill", 1));
  CHECK(member("SkillInEffect", 1));
  CHECK(member("AvailableSkillPoints", 0));
  CHECK(member("AddItem", 1));
  CHECK(member("HasItem", 1));
  CHECK(member("FindItem", 1));
  CHECK(member("GiveItem", 2));
  CHECK(member("PutItem", 2));
  CHECK(member("item_count", 0));
  CHECK(member("RemoveItemsOfType", 1));
  CHECK(member("GetSquad", 0));
  CHECK(registry.find(script::CallKind::free_function, "HeroSkillId", 1) !=
        script::kUnresolvedHost);
}

/// The item counter, by both routes, and the four ways of missing.
///
/// The free forms take no argument naming an item: they read the running
/// script's `This`, which is how every shipped item script is started. The
/// members take an explicit handle. They are the same field.
TEST(the_use_count_is_one_field_reached_two_ways) {
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  define_hero_host(registry);

  sim::HostContext context_state;
  context_state.world = &f.world;
  script::Scheduler scheduler;
  WorldHost host{f.world};
  scheduler.set_registry(&registry);
  scheduler.set_host(&host);
  scheduler.set_user(&context_state);

  // A script attached to an object, which is what `This` is.
  const auto script_on = [&](ObjectId owner) -> script::ScriptId {
    const auto parsed = script::parse(bytes("Sleep(1000000);"), "item.vs");
    CHECK(parsed.ok());
    if (!parsed.ok()) return script::kNoScript;
    auto chunk = script::compile(parsed.value(), &registry);
    CHECK(chunk.ok());
    if (!chunk.ok()) return script::kNoScript;
    const std::uint32_t index = scheduler.add_chunk(std::move(chunk.value()));
    return scheduler.spawn(index, {}, script::ObjectRef{kTypeObj, owner});
  };

  const auto call = [&](script::CallKind kind, std::string_view name,
                        std::vector<script::Value> args,
                        script::ScriptId running) -> script::HostOutcome {
    const std::uint32_t index = registry.find(
        kind, name,
        static_cast<std::uint16_t>(args.size() - (kind == script::CallKind::member)));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context_state;
    ctx.name = name;
    ctx.kind = kind;
    ctx.script = running;
    ctx.scheduler = &scheduler;
    return entry.fn(ctx);
  };

  const ObjectId warrior = f.spawn_warrior();
  const ObjectId ring = f.heroes.items().add(f.world, warrior, "Bear teeth amulet");
  REQUIRE(ring != sim::kNoObject);
  f.heroes.items().set_use_count(ring, 40);

  const script::ScriptId on_item = script_on(ring);
  const script::ScriptId on_warrior = script_on(warrior);
  const script::ScriptId orphan = script_on(sim::kNoObject);

  // The free form reads the item the script is running on.
  CHECK(call(script::CallKind::free_function, "GetUseCount", {}, on_item).value.as_integer() == 40);
  // A script whose `This` is not an item, and one with no `This` at all, answer
  // **0** rather than refusing: `0x00534aa0` prints and pushes zero, and
  // `HEALING WATER.VS` runs straight on into `if (nHealth == 0)`.
  const script::HostOutcome not_item = call(script::CallKind::free_function, "GetUseCount", {}, on_warrior);
  CHECK(not_item.status == script::HostStatus::ok);
  CHECK(not_item.value.as_integer() == 0);
  CHECK(call(script::CallKind::free_function, "GetUseCount", {}, orphan).value.as_integer() == 0);

  // `ItemUsed(n)` spends, and a negative `n` credits -- `RING OF POWER.VS`
  // tops itself back up that way.
  CHECK(call(script::CallKind::free_function, "ItemUsed", {script::Value::integer(15)}, on_item).status ==
        script::HostStatus::ok);
  CHECK(f.heroes.items().use_count(ring) == 25);
  (void)call(script::CallKind::free_function, "ItemUsed", {script::Value::integer(-5)}, on_item);
  CHECK(f.heroes.items().use_count(ring) == 30);
  // Spending on a script that is not on an item does nothing and does not trap.
  CHECK(call(script::CallKind::free_function, "ItemUsed", {script::Value::integer(1)}, on_warrior).status ==
        script::HostStatus::ok);
  CHECK(f.heroes.items().use_count(ring) == 30);
  // An argument that is not a number is **no charges**, not one. The arity is
  // fixed at one, so this is the only way the fallback is reachable at all.
  (void)call(script::CallKind::free_function, "ItemUsed", {script::Value::string("x")}, on_item);
  CHECK(f.heroes.items().use_count(ring) == 30);

  // **And through the entry point, the destroying case**, which is the whole
  // difference between `spend` and `use`: the amulet is `usecount="0"` in the
  // catalogue -- never a consumable -- so `Obj::UseItem`'s rule would keep it,
  // and `ItemUsed` takes it away.
  (void)call(script::CallKind::free_function, "ItemUsed", {script::Value::integer(30)}, on_item);
  CHECK(f.heroes.items().find(ring) == nullptr);
  CHECK(f.world.find(ring) == nullptr);
  // Everything after this asks about a handle that no longer names an item.
  CHECK(call(script::CallKind::free_function, "GetUseCount", {}, on_item).value.as_integer() == 0);

  // The members reach the same field by handle. A fresh item, because the last
  // call above consumed the first one.
  const ObjectId purse = f.heroes.items().add(f.world, warrior, "Bear teeth amulet");
  REQUIRE(purse != sim::kNoObject);
  f.heroes.items().set_use_count(purse, 30);
  const script::ScriptId on_purse = script_on(purse);
  const script::Value ring_ref = script::Value::object(kTypeObj, purse);
  CHECK(call(script::CallKind::member, "use_count", {ring_ref}, on_warrior).value.as_integer() == 30);
  (void)call(script::CallKind::member, "SetUseCount", {ring_ref, script::Value::integer(500)}, on_warrior);
  CHECK(call(script::CallKind::free_function, "GetUseCount", {}, on_purse).value.as_integer() == 500);
  // **Without `ItemStore::set_use_count`'s clamp**: 0x00535100 is a plain store.
  (void)call(script::CallKind::member, "SetUseCount", {ring_ref, script::Value::integer(-7)}, on_warrior);
  CHECK(call(script::CallKind::member, "use_count", {ring_ref}, on_warrior).value.as_integer() == -7);

  // **The two ways of missing differ, and only here.** An unresolvable handle
  // is -1; a handle that resolves to something that is not an item is 0.
  CHECK(call(script::CallKind::member, "use_count", {script::Value::object(kTypeObj, sim::kNoObject)},
             on_warrior)
            .value.as_integer() == -1);
  CHECK(call(script::CallKind::member, "use_count", {script::Value::object(kTypeObj, warrior)}, on_warrior)
            .value.as_integer() == 0);

  // `max_items` is the other half of the corpus's only comparison,
  // `if (u.item_count < u.max_items)`. It is the class's `inventory_size` and
  // it inherits: `Hero` declares none and takes `Warrior`'s four.
  const ObjectId bag = f.spawn_bag();
  const ObjectId hero = f.spawn_hero();
  const auto capacity = [&](ObjectId id) {
    return call(script::CallKind::member, "max_items", {script::Value::object(kTypeObj, id)},
                on_warrior)
        .value.as_integer();
  };
  CHECK(capacity(warrior) == 4);
  CHECK(capacity(bag) == 16);
  CHECK(capacity(hero) == 4);
  // An unresolvable receiver is **0**, which is the safe direction: `item_count
  // < 0` is false, so the unit is simply not sent after the loot.
  const script::HostOutcome none = call(script::CallKind::member, "max_items",
                                        {script::Value::object(kTypeObj, sim::kNoObject)},
                                        on_warrior);
  CHECK(none.status == script::HostStatus::ok);
  CHECK(none.value.as_integer() == 0);
}

/// `ItemUsed` is not `UseItem`, and `ItemStore` now says so with two functions.
TEST(item_used_destroys_at_or_below_zero_with_no_consumable_rule) {
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);
  const ObjectId warrior = f.spawn_warrior();

  // `Bear teeth amulet` is `usecount="0"` -- never a consumable -- so `use`
  // refuses to destroy it however much is spent. That rule is `Obj::UseItem`'s.
  const ObjectId amulet = f.heroes.items().add(f.world, warrior, "Bear teeth amulet");
  REQUIRE(amulet != sim::kNoObject);
  CHECK(f.heroes.items().use(f.world, amulet, 5));
  CHECK(f.heroes.items().find(amulet) != nullptr);
  CHECK(f.heroes.items().use_count(amulet) == 0);  // clamped

  // `spend` is `ItemUsed`, and it has no such rule: the same item, the same
  // zero count, spending zero, is **destroyed**. `HEALING WATER.VS`'s
  // zero-healing exit is `ItemUsed(GetUseCount())`, which is how a script says
  // "consume me".
  const ObjectId second = f.heroes.items().add(f.world, warrior, "Bear teeth amulet");
  REQUIRE(second != sim::kNoObject);
  CHECK(f.heroes.items().spend(f.world, second, 0));
  CHECK(f.heroes.items().find(second) == nullptr);
  CHECK(f.world.find(second) == nullptr);

  // The count is written before the test and is not clamped, so an item that
  // survives can hold a value `set_use_count` would have refused... and one
  // that does not survive went negative on the way out.
  const ObjectId third = f.heroes.items().add(f.world, warrior, "Bear teeth amulet");
  f.heroes.items().set_use_count(third, 3);
  CHECK(f.heroes.items().spend(f.world, third, 1));
  CHECK(f.heroes.items().use_count(third) == 2);
  CHECK(f.heroes.items().spend(f.world, third, 9));  // 2 - 9 = -7, destroyed
  CHECK(f.heroes.items().find(third) == nullptr);

  // A handle that names no item is the only false.
  CHECK(!f.heroes.items().spend(f.world, warrior, 1));
}

/// `hero.army` -- 92 sites, defined in `sim/objlist.cpp` rather than here.
///
/// Tested from the hero side because that is where the membership lives and
/// where a defect would come from; registered from the other because the pool
/// does.
/// `Hero::IsArmyOutside` is one word per member, and the four answers it can
/// give are four separate branches in `gbr.exe`.
TEST(is_army_outside_is_false_while_any_member_is_inside_a_holder) {
  Fixture f;
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  define_hero_host(registry);

  sim::HostContext context_state;
  context_state.world = &f.world;

  const std::uint32_t index = registry.find(script::CallKind::member, "IsArmyOutside", 0);
  REQUIRE(index != script::kUnresolvedHost);
  REQUIRE(registry.entry(index).fn != nullptr);

  const auto ask = [&](script::Value receiver) -> script::HostOutcome {
    std::vector<script::Value> args{receiver};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context_state;
    ctx.name = "IsArmyOutside";
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };
  const auto outside = [&](script::Value receiver) {
    const script::HostOutcome out = ask(receiver);
    return out.status == script::HostStatus::ok && out.value.as_integer() != 0;
  };

  const ObjectId hero = f.spawn_hero();
  const script::Value hero_ref = script::Value::object(kTypeObj, hero);

  // An empty army does not keep anybody waiting.
  CHECK(outside(hero_ref));

  const ObjectId first = f.spawn_warrior();
  const ObjectId second = f.spawn_warrior();
  REQUIRE(f.heroes.attach(f.world, first, hero));
  REQUIRE(f.heroes.attach(f.world, second, hero));
  CHECK(outside(hero_ref));

  // One member indoors is enough, and it is the *holder* that decides -- no
  // distance is consulted, so a garrison anywhere at all holds the answer down.
  const ObjectId barracks = f.world.spawn(NativeClass::building, nullptr);
  const sim::Point outdoors = f.world.state(second)->position;
  REQUIRE(f.world.put_in_holder(second, barracks));
  CHECK(!outside(hero_ref));

  REQUIRE(f.world.remove_from_holder(second, outdoors));
  CHECK(outside(hero_ref));

  // A member whose handle no longer resolves is skipped rather than counted as
  // indoors: 0x0052ff96 jumps to the increment. This is what lets the shipped
  // `while (!.IsArmyOutside())` loops terminate over a dead army.
  REQUIRE(f.world.put_in_holder(first, barracks));
  CHECK(!outside(hero_ref));
  f.world.despawn(first);
  CHECK(outside(hero_ref));

  // An unresolvable receiver is the original's error path, which pushes zero.
  CHECK(!outside(script::Value::object(kTypeObj, sim::kNoObject)));
}

TEST(a_heros_army_comes_back_as_a_list_in_attach_order) {
  Fixture f;
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  define_hero_host(registry);
  (void)register_objlist_host(registry);

  sim::HostContext context_state;
  context_state.world = &f.world;

  const auto call = [&](script::CallKind kind, std::string_view name,
                        std::vector<script::Value> args) -> script::HostOutcome {
    const std::uint32_t index =
        registry.find(kind, name, static_cast<std::uint16_t>(args.size() -
                                                             (kind == script::CallKind::member)));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context_state;
    ctx.name = name;
    ctx.kind = kind;
    ctx.script = 3;  // the pool keys a temporary list on the calling script
    return entry.fn(ctx);
  };

  const ObjectId hero = f.spawn_hero();
  const ObjectId first = f.spawn_warrior();
  const ObjectId second = f.spawn_warrior();
  const script::Value hero_ref = script::Value::object(kTypeObj, hero);

  // Empty before anyone attaches, and an *empty list* rather than a refusal --
  // `for (j = 0; j < h.army.count; ...)` has to terminate.
  const script::HostOutcome empty = call(script::CallKind::member, "army", {hero_ref});
  REQUIRE(empty.status == script::HostStatus::ok);
  REQUIRE(is_objlist(empty.value));
  CHECK(call(script::CallKind::member, "count", {empty.value}).value.as_integer() == 0);

  REQUIRE(f.heroes.attach(f.world, first, hero));
  REQUIRE(f.heroes.attach(f.world, second, hero));

  const script::HostOutcome full = call(script::CallKind::member, "army", {hero_ref});
  REQUIRE(full.status == script::HostStatus::ok);
  REQUIRE(is_objlist(full.value));
  CHECK(call(script::CallKind::member, "count", {full.value}).value.as_integer() == 2);

  // **Attach order, not id order.** `HeroRecord::army` is a vector appended to
  // by `attach`, iteration order is world state, and a list that sorted would
  // command the same two units in a different sequence on a peer that attached
  // them in the other order.
  const ObjListPool& pool = objlist_pool_of(f.world);
  const std::span<const ObjectId> items = pool.items(objlist_of(full.value));
  REQUIRE(items.size() == 2);
  CHECK(items[0] == first);
  CHECK(items[1] == second);

  // A snapshot, not the hero's own vector. `gbr.exe` 0x00530580 returns a live
  // reference -- `{handle, 0x1cc, hero + 0x1cc}` -- and the divergence is
  // recorded at `m_army`; this pins which of the two this engine does, so that
  // changing it is a decision rather than an accident.
  REQUIRE(f.heroes.detach(first));
  CHECK(call(script::CallKind::member, "count", {full.value}).value.as_integer() == 2);
  CHECK(call(script::CallKind::member, "count",
             {call(script::CallKind::member, "army", {hero_ref}).value})
            .value.as_integer() == 1);

  // An invalid receiver is a fresh empty list, not a refusal: 0x005305ad
  // diagnoses and allocates one, so `h.army.count` on a dead hero is 0.
  const script::HostOutcome dead =
      call(script::CallKind::member, "army", {script::Value::object(kTypeObj, 9999)});
  REQUIRE(dead.status == script::HostStatus::ok);
  REQUIRE(is_objlist(dead.value));
  CHECK(call(script::CallKind::member, "count", {dead.value}).value.as_integer() == 0);
}

TEST(a_host_call_reaches_the_system_through_the_call_context) {
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_hero_host(registry);

  // Owned here, not borrowed: `CallContext::user` is a `HostContext*` for every
  // domain now, and a `HeroHostState*` behind it is the exact confusion
  // sim/host_context.hpp exists to make impossible. The context carries the
  // world alone; the system is found through it.
  sim::HostContext context_state;
  context_state.world = &f.world;
  CHECK(hero_system_of(f.world) == &f.heroes);

  const ObjectId hero = f.spawn_hero();
  const ObjectId warrior = f.spawn_warrior();

  const auto call = [&](script::CallKind kind, std::string_view name,
                        std::vector<script::Value> args) -> script::HostOutcome {
    const std::uint32_t index =
        registry.find(kind, name, static_cast<std::uint16_t>(args.size() -
                                                             (kind == script::CallKind::member)));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context_state;
    ctx.name = name;
    ctx.kind = kind;
    return entry.fn(ctx);
  };

  constexpr script::TypeId kUnit = 7;
  const script::Value hero_ref = script::Value::object(kUnit, hero);
  const script::Value warrior_ref = script::Value::object(kUnit, warrior);

  CHECK(call(script::CallKind::member, "AttachTo", {warrior_ref, hero_ref})
            .value.as_integer() == 1);
  CHECK(call(script::CallKind::member, "hero", {warrior_ref}).value.as_object().id == hero);
  CHECK(call(script::CallKind::member, "HasArmy", {hero_ref}).value.as_integer() == 1);
  CHECK(call(script::CallKind::member, "IsHeroArmyFull", {hero_ref}).value.as_integer() == 0);

  CHECK(call(script::CallKind::free_function, "HeroSkillId",
             {script::Value::string("Discipline")})
            .value.as_integer() == static_cast<std::int32_t>(HeroSkill::discipline));

  f.grant(hero, 5);
  call(script::CallKind::member, "SetSkill",
       {hero_ref, script::Value::integer(static_cast<std::int32_t>(HeroSkill::discipline)),
        script::Value::integer(4)});
  CHECK(call(script::CallKind::member, "GetSkill",
             {hero_ref, script::Value::integer(static_cast<std::int32_t>(HeroSkill::discipline))})
            .value.as_integer() == 4);
  // Discipline 4 floors the warrior at 6.
  CHECK(call(script::CallKind::member, "level", {warrior_ref}).value.as_integer() == 6);
  CHECK(call(script::CallKind::member, "inherentlevel", {warrior_ref}).value.as_integer() == 1);

  CHECK(call(script::CallKind::member, "AddItem",
             {warrior_ref, script::Value::string("King's Belt")})
            .value.as_integer() == 1);
  CHECK(call(script::CallKind::member, "HasItem",
             {warrior_ref, script::Value::string("King's Belt")})
            .value.as_integer() == 1);
  CHECK(call(script::CallKind::member, "item_count", {warrior_ref}).value.as_integer() == 1);

  CHECK(call(script::CallKind::member, "DetachFrom", {warrior_ref, hero_ref})
            .value.as_integer() == 1);
  CHECK(f.heroes.hero_of(warrior) == sim::kNoObject);
}

TEST(a_host_call_without_its_state_traps_rather_than_guessing) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_hero_host(registry);
  const std::uint32_t index = registry.find(script::CallKind::member, "hero", 0);
  REQUIRE(index != script::kUnresolvedHost);

  std::vector<script::Value> args{script::Value::object(1, 42)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = nullptr;
  const script::HostOutcome out = registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::error);
  CHECK(out.error != nullptr);

  // The other half of the same refusal: a real context over a world that has no
  // hero system registered. Under `HostContext` the system is a lookup rather
  // than a field, so "no system" is a state a caller can reach and it has to
  // trap by its own name rather than share one with "no world".
  World bare;
  sim::HostContext context_state;
  context_state.world = &bare;
  ctx.user = &context_state;
  const script::HostOutcome unbound = registry.entry(index).fn(ctx);
  CHECK(unbound.status == script::HostStatus::error);
  REQUIRE(unbound.error != nullptr);
  CHECK(std::string_view(unbound.error) != std::string_view(out.error));
}

/// `SetAutocast` and `autocast` are one bit, and the getter is bound because a
/// flag nothing can read is a flag nothing can be written against.
///
/// Bit 2 of `[hero+0x194]`, the same second flag word `no_ai`, `in_air` and
/// `noselect` live on. Thirty-four shipped sites, every one inside a map
/// container, and twenty-nine of them pass `true`: it is how a mission turns a
/// scripted hero loose. Nothing in this engine acts on it yet -- there is no
/// autocast in the AI -- so it is state a script sets and reads back, which is
/// what the sites need and no more than the evidence supports.
TEST(hero_autocast_is_one_bit_that_a_script_can_set_and_read_back) {
  Fixture f;
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  define_hero_host(registry);

  sim::HostContext context_state;
  context_state.world = &f.world;

  const auto call = [&](std::string_view name,
                        std::vector<script::Value> args) -> script::HostOutcome {
    const std::uint32_t index = registry.find(
        script::CallKind::member, name, static_cast<std::uint16_t>(args.size() - 1));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context_state;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return entry.fn(ctx);
  };

  const ObjectId hero = f.spawn_hero();
  const script::Value hero_ref = script::Value::object(kTypeObj, hero);
  const script::Value missing_ref = script::Value::object(kTypeObj, 9999);

  const auto get = [&](const script::Value& who) {
    const script::HostOutcome out = call("autocast", {who});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  };

  CHECK(!get(hero_ref));
  CHECK(call("SetAutocast", {hero_ref, script::Value::boolean(true)}).status ==
        script::HostStatus::ok);
  CHECK(get(hero_ref));
  CHECK(f.world.state(hero)->flags.autocast);
  // The `false` path ships too -- five sites, one block, where a hero changes
  // sides and is quietened first.
  CHECK(call("SetAutocast", {hero_ref, script::Value::boolean(false)}).status ==
        script::HostStatus::ok);
  CHECK(!get(hero_ref));

  // An unresolvable receiver writes nothing, and the getter answers false
  // rather than reproducing the retail stack corruption: `Hero::autocast`
  // returns there without pushing its result *and* without restoring the stack
  // pointer, which is a fault and not a behaviour.
  CHECK(call("SetAutocast", {missing_ref, script::Value::boolean(true)}).status ==
        script::HostStatus::ok);
  CHECK(!get(missing_ref));
}

// ==========================================================================
// the caster searches
// ==========================================================================

namespace {

/// A caster and the six kinds of thing its predicates have an opinion about.
struct CasterBench {
  ClassGraph graph;
  World world;
  HeroSystem heroes;
  EnvSystem env;
  CombatSystem combat;
  script::HostRegistry registry;
  sim::HostContext context;
  ClassIndex caster_class = kNoClass;
  ClassIndex plain_class = kNoClass;
  ClassIndex frail_class = kNoClass;
  ClassIndex mage_class = kNoClass;
  ClassIndex ram_class = kNoClass;
  ClassIndex peaceful_class = kNoClass;
  ClassIndex sentry_class = kNoClass;

  CasterBench() {
    const std::string docs[] = {
        R"(<class id="Military" cpp_class="CVXUnit"><properties sight="900" maxhealth="200"
             radius="10" range="50" damage="7" level="3"/></class>)",
        R"(<class id="Druid" parent="Military" cpp_class="CVXDruid"><properties
             sight="900" maxhealth="100" radius="10" range="50" level="3"/></class>)",
        R"(<class id="Soldier" parent="Military" cpp_class="CVXUnit"><properties
             maxhealth="200" radius="10" damage="7"/></class>)",
        // Half the soldier's maximum, so that a *fraction* and an absolute
        // health order two candidates differently -- which is what separates
        // the real score from the obvious wrong one.
        R"(<class id="Frail" parent="Military" cpp_class="CVXUnit"><properties
             maxhealth="100" radius="10" damage="7"/></class>)",
        R"(<class id="BaseMage" parent="Military" cpp_class="CVXDruid"><properties
             maxhealth="200" radius="10" damage="9"/></class>)",
        R"(<class id="RamUnit" parent="Military" cpp_class="CVXUnit"><properties
             maxhealth="200" radius="10" damage="20"/></class>)",
        R"(<class id="Peaceful" parent="Military" cpp_class="CVXUnit"><properties
             maxhealth="200" radius="10" damage="1"/></class>)",
        R"(<class id="Sentry" parent="Military" cpp_class="CVXUnit"><properties
             maxhealth="200" radius="10" damage="3"/></class>)",
    };
    const char* names[] = {"military.sc.xml", "druid.sc.xml",    "soldier.sc.xml",
                           "frail.sc.xml",    "mage.sc.xml",     "ram.sc.xml",
                           "peaceful.sc.xml", "sentry.sc.xml"};
    for (int i = 0; i < 8; ++i) graph.add(bytes(docs[i]), names[i]);
    graph.link();
    world.set_class_graph(&graph);
    world.add_system(&heroes);
    world.add_system(&env);
    world.add_system(&combat);
    caster_class = graph.find("Druid");
    plain_class = graph.find("Soldier");
    frail_class = graph.find("Frail");
    mage_class = graph.find("BaseMage");
    ram_class = graph.find("RamUnit");
    peaceful_class = graph.find("Peaceful");
    sentry_class = graph.find("Sentry");
    // `HealAmount`, the constant the script adds to the caster's level and the
    // engine adds to the same to get its threshold.
    env.set_constant("HealAmount", 40);
    // `RevitalizeAmount`, which `FindUnitToRevitalize` reads the same way and
    // uses for the opposite test: heal parks a candidate whose *deficit* is
    // under its constant, revitalize parks one whose deficit is under it.
    env.set_constant("RevitalizeAmount", 60);
    script::declare_shipped_surface(registry);
    (void)register_world_host(registry);
    define_hero_host(registry);
    define_combat_host(registry);
    context.world = &world;
    context.object_type = kTypeObj;
  }

  /// The native class is a parameter because one predicate in this family
  /// tests the **RTTI descriptor** rather than a graph name: `0x00511f00`
  /// rejects a `CVXDruid` candidate through the type object at 0x00821174, and
  /// `NativeClass::druid` is that descriptor. A candidate spawned as a unit
  /// carrying a `cpp_class="CVXDruid"` graph class would not exercise it.
  ObjectId spawn(ClassIndex which, Point at, PlayerId owner, std::int32_t health = 200,
                 std::int32_t stamina = 0, NativeClass native = NativeClass::unit) {
    const ObjectId id = world.spawn(native, nullptr, which);
    world.set_owner(id, owner);
    world.set_health(id, health);
    world.set_stamina(id, stamina);
    (void)world.set_position(id, at);
    heroes.register_unit(world, id);
    return id;
  }

  script::HostOutcome call(const char* name, std::vector<script::Value> args) {
    const std::uint16_t arity = static_cast<std::uint16_t>(args.size() - 1);
    const std::uint32_t index = registry.find(script::CallKind::member, name, arity);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    CHECK(registry.entry(index).fn != nullptr);
    if (registry.entry(index).fn == nullptr) {
      return script::HostOutcome::failed("not implemented");
    }
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  }

  static script::Value obj(ObjectId id) { return script::Value::object(kTypeObj, id); }

  ObjectId found(const char* name, ObjectId self, std::vector<script::Value> extra = {}) {
    std::vector<script::Value> args{obj(self)};
    for (const script::Value& v : extra) args.push_back(v);
    const script::HostOutcome out = call(name, args);
    CHECK(out.status == script::HostStatus::ok);
    if (!out.value.is_object() || out.value.as_object().type == script::kNoType) {
      return kNoObject;
    }
    return out.value.as_object().id;
  }
};

}  // namespace

/// `CanSee` is a stealth test and nothing else -- and the **argument** is the
/// object being looked at.
///
/// `0x005ab9d0` returns true for anything not carrying the hidden bit, from
/// anywhere on the map: no line of sight, no radius, no terrain, no exploration
/// state. The name promises fog and the body does not need any. It pops the
/// argument first and complains about it by name, and never checks the receiver
/// at all -- with a stealthed argument it dereferences the receiver's owner and
/// faults, which is a fault and not a behaviour.
TEST(hero_can_see_is_a_stealth_test_and_the_argument_is_the_one_looked_at) {
  CasterBench b;
  const ObjectId watcher = b.spawn(b.caster_class, Point{0, 0}, 1);
  const ObjectId far = b.spawn(b.plain_class, Point{15000, 15000}, 2);

  const auto sees = [&](ObjectId a, ObjectId c) {
    const script::HostOutcome out = b.call("CanSee", {CasterBench::obj(a), CasterBench::obj(c)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  };

  // Across the whole map, through anything, and an enemy at that.
  CHECK(sees(watcher, far));

  // Hidden is the only thing that stops it -- and only on the *argument*.
  b.world.mutable_state(far)->flags.hidden = true;
  CHECK(!sees(watcher, far));
  b.world.mutable_state(far)->flags.hidden = false;
  b.world.mutable_state(watcher)->flags.hidden = true;
  CHECK(sees(watcher, far));

  CHECK(!sees(watcher, 9999));
  CHECK(!sees(9999, far));
}

/// `FindUnitToRevitalize` minimises **reach**, floors every real candidate at
/// 256, and parks the ones the spell would overshoot at a flat 1000.
///
/// The score is `max(0, edgeDistance - range) + 256`, so distance is the only
/// thing that separates two candidates the spell fits -- health does not enter
/// it at all, which is the sharpest difference from `FindUnitToHeal`. A
/// candidate whose `health + RevitalizeAmount` would pass its own `maxhealth`
/// scores 1000 instead, and 1000 is above every reachable real score by
/// construction: that is what the `+256` floor is for.
TEST(hero_find_unit_to_revitalize_minimises_reach_and_parks_the_overtopped) {
  CasterBench b;
  const ObjectId druid = b.spawn(b.caster_class, Point{0, 0}, 1, 100);
  CHECK(b.found("FindUnitToRevitalize", druid) == kNoObject);

  // At full health: not wounded, not a candidate.
  (void)b.spawn(b.plain_class, Point{100, 0}, 1, 200);
  CHECK(b.found("FindUnitToRevitalize", druid) == kNoObject);

  // 160 + 60 overshoots 200: parked at 1000, and still the answer alone.
  const ObjectId topped = b.spawn(b.plain_class, Point{100, 0}, 1, 160);
  CHECK(b.found("FindUnitToRevitalize", druid) == topped);

  // 140 + 60 lands exactly on 200, so it is *not* an overshoot -- the test is
  // strictly greater. 380 units of edge less 50 of range, plus the floor: 586.
  const ObjectId far_fit = b.spawn(b.plain_class, Point{400, 0}, 1, 140);
  CHECK(b.found("FindUnitToRevitalize", druid) == far_fit);

  // Inside the caster's own reach the distance term clamps to zero, so this
  // scores the bare floor of 256 and wins.
  const ObjectId near_fit = b.spawn(b.plain_class, Point{60, 0}, 1, 140);
  CHECK(b.found("FindUnitToRevitalize", druid) == near_fit);

  // **Health does not order two candidates that both fit.** A far one at 60
  // health is far more wounded than the near one at 140 and still loses, which
  // is the whole difference from heal's health fraction.
  const ObjectId far_worse = b.spawn(b.plain_class, Point{400, 0}, 1, 60);
  CHECK(b.found("FindUnitToRevitalize", druid) == near_fit);
  (void)far_worse;

  // Every exclusion, one at a time, each restored before the next.
  b.world.mutable_state(near_fit)->flags.messenger = true;
  CHECK(b.found("FindUnitToRevitalize", druid) == far_fit);
  b.world.mutable_state(near_fit)->flags.messenger = false;

  b.world.mutable_state(near_fit)->flags.hidden = true;
  CHECK(b.found("FindUnitToRevitalize", druid) == far_fit);
  b.world.mutable_state(near_fit)->flags.hidden = false;

  const Point near_at = b.world.state(near_fit)->position;
  REQUIRE(b.world.put_in_holder(near_fit, druid));
  CHECK(b.found("FindUnitToRevitalize", druid) == far_fit);
  REQUIRE(b.world.remove_from_holder(near_fit, near_at));

  b.world.mutable_state(near_fit)->flags.is_unit = false;
  CHECK(b.found("FindUnitToRevitalize", druid) == far_fit);
  b.world.mutable_state(near_fit)->flags.is_unit = true;
  CHECK(b.found("FindUnitToRevitalize", druid) == near_fit);

  // **Inside the caster's reach every candidate scores the same**, because the
  // distance term clamps at zero and the floor is a constant. So a nearer
  // in-range candidate does not displace an earlier one: the tie goes to the
  // lower object id, which is the deterministic answer and not an accident.
  const ObjectId closer = b.spawn(b.plain_class, Point{30, 0}, 1, 140);
  CHECK(b.found("FindUnitToRevitalize", druid) == near_fit);
  (void)closer;

  // The three exclusions that are not flags. **Each is spawned as a hero at
  // point-blank range**, so it would score 128 and beat every candidate above
  // if its exclusion were dropped -- without that, the reach clamp puts it in a
  // tie the earlier object already wins, and the assertion would pass whether
  // the exclusion existed or not. The fault sweep found all three that way.
  const auto blocker = [&](ClassIndex which, NativeClass native, PlayerId owner) {
    const ObjectId id = b.spawn(which, Point{30, 0}, owner, 140, 0, native);
    b.world.mutable_state(id)->flags.is_hero = true;
    return id;
  };

  // An enemy is never revitalised, however close.
  b.world.players().set(1, 2, Relation::allied, false);
  (void)blocker(b.plain_class, NativeClass::unit, 2);
  CHECK(b.found("FindUnitToRevitalize", druid) == near_fit);

  // A ram is excluded by graph class, and a **druid** by its RTTI descriptor --
  // the one exclusion in this family that is not a graph name.
  (void)blocker(b.ram_class, NativeClass::unit, 1);
  CHECK(b.found("FindUnitToRevitalize", druid) == near_fit);
  (void)blocker(b.mage_class, NativeClass::druid, 1);
  CHECK(b.found("FindUnitToRevitalize", druid) == near_fit);
}

/// `FindUnitToHide` answers nothing, for everybody, and that is the finding
/// rather than a gap.
///
/// The scan admits only players in the caster's friendly mask and the predicate
/// then requires `Obj::IsEnemy` of the same pair. They are complements of one
/// bit, so the conjunction is empty on any data. What this test pins is that
/// the entry point **answers** rather than traps -- the original returns an
/// invalid handle and `DRUID_HIDE_GROUND.VS` retries and walks on.
TEST(hero_find_unit_to_hide_answers_nothing_for_ally_enemy_and_self) {
  CasterBench b;
  const ObjectId druid = b.spawn(b.caster_class, Point{0, 0}, 1, 100);

  // An ally right next to the caster: admitted by the mask, refused by the
  // `IsEnemy` clause.
  const ObjectId ally = b.spawn(b.plain_class, Point{30, 0}, 1, 140);
  CHECK(b.found("FindUnitToHide", druid) == kNoObject);

  // An enemy right next to the caster: refused by the mask.
  b.world.players().set(1, 2, Relation::allied, false);
  const ObjectId foe = b.spawn(b.plain_class, Point{30, 0}, 2, 140);
  CHECK(b.found("FindUnitToHide", druid) == kNoObject);

  // At full health, wounded, hero, hidden already -- none of it matters,
  // because the two clauses that decide it never look at any of them.
  b.world.set_health(ally, 200);
  b.world.mutable_state(foe)->flags.is_hero = true;
  CHECK(b.found("FindUnitToHide", druid) == kNoObject);

  // And **it answers rather than traps**, which is the whole reason it has a
  // body: a declared name with no body would stop the script at the line.
  const script::HostOutcome out = b.call("FindUnitToHide", {CasterBench::obj(druid)});
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.is_object());
  CHECK(out.value.as_object().type == script::kNoType);
}

/// A hero halves its own score, and nothing in `FindUnitToHeal` does this.
///
/// The halving is applied to the finished score, floor included, so a hero has
/// to be **more than twice as far** as a plain unit before the plain unit is
/// preferred -- which is a much stronger preference than a tie-break.
TEST(hero_find_unit_to_revitalize_halves_a_heros_score) {
  CasterBench b;
  const ObjectId druid = b.spawn(b.caster_class, Point{0, 0}, 1, 100);
  // 380 - 50 + 256 = 586.
  const ObjectId plain = b.spawn(b.plain_class, Point{400, 0}, 1, 140);
  // 580 - 50 + 256 = 786, and half of that is 393. Half as good a position and
  // a better answer.
  const ObjectId champion = b.spawn(b.plain_class, Point{600, 0}, 1, 140);
  b.world.mutable_state(champion)->flags.is_hero = true;
  CHECK(b.found("FindUnitToRevitalize", druid) == champion);

  // Take the halving away and the arithmetic reverses, which is what makes the
  // assertion above about the halving rather than about the distance.
  b.world.mutable_state(champion)->flags.is_hero = false;
  CHECK(b.found("FindUnitToRevitalize", druid) == plain);

  // **The floor is inside the halving, and that is observable.** A hero's score
  // is `(reach + 256) / 2`, not `reach / 2 + 256` and not `reach / 2`: the
  // floor is halved along with everything else, which shifts a hero down by a
  // further 128 relative to a plain unit. These two are chosen to sit in the
  // window where that 128 decides it -- 200 against 600 of raw reach, so with
  // the floor the hero scores 428 against 456 and wins, and without it 300
  // against 200 and loses.
  CasterBench c;
  const ObjectId caster = c.spawn(c.caster_class, Point{0, 0}, 1, 100);
  const ObjectId ordinary = c.spawn(c.plain_class, Point{270, 0}, 1, 140);
  const ObjectId marginal = c.spawn(c.plain_class, Point{670, 0}, 1, 140);
  c.world.mutable_state(marginal)->flags.is_hero = true;
  CHECK(c.found("FindUnitToRevitalize", caster) == marginal);
  (void)ordinary;
}

/// `FindUnitToHeal` takes the **most wounded**, parks the lightly wounded at a
/// flat score rather than rejecting them, and penalises distance beyond reach.
///
/// The score is `300 * health / maxhealth + max(0, edgeDistance - range)`,
/// minimised -- a health *fraction*, so a unit at half of 200 loses to one at
/// half of 100 only if the second is nearer. A candidate whose deficit is under
/// `self.level + GetConst("HealAmount")` scores a flat 1000: too lightly
/// wounded to be worth a whole heal, but still the answer when nothing better
/// is standing there.
TEST(hero_find_unit_to_heal_takes_the_most_wounded_and_parks_the_rest) {
  CasterBench b;
  const ObjectId druid = b.spawn(b.caster_class, Point{0, 0}, 1, 100);
  CHECK(b.found("FindUnitToHeal", druid) == kNoObject);

  // Untouched: not a candidate at all.
  (void)b.spawn(b.plain_class, Point{100, 0}, 1, 200);
  CHECK(b.found("FindUnitToHeal", druid) == kNoObject);

  // Deficit 20, under the threshold of `level 3 + HealAmount 40`: parked at
  // 1000, and still the answer because nothing else is wounded.
  const ObjectId scratched = b.spawn(b.plain_class, Point{100, 0}, 1, 180);
  CHECK(b.found("FindUnitToHeal", druid) == scratched);

  // Deficit 150 and the same distance: a real score, which beats the park.
  const ObjectId bleeding = b.spawn(b.plain_class, Point{100, 0}, 1, 50);
  CHECK(b.found("FindUnitToHeal", druid) == bleeding);

  // Worse still, and it wins on the health fraction.
  const ObjectId dying = b.spawn(b.plain_class, Point{100, 0}, 1, 10);
  CHECK(b.found("FindUnitToHeal", druid) == dying);

  // The distance term: the same wound far away loses to the nearer one.
  const ObjectId distant = b.spawn(b.plain_class, Point{800, 0}, 1, 10);
  CHECK(b.found("FindUnitToHeal", druid) == dying);

  // Every exclusion in the predicate, one at a time.
  CHECK(b.found("FindUnitToHeal", druid) == dying);
  b.world.mutable_state(dying)->flags.messenger = true;
  CHECK(b.found("FindUnitToHeal", druid) == bleeding);
  b.world.mutable_state(dying)->flags.messenger = false;
  b.world.mutable_state(dying)->flags.hidden = true;
  CHECK(b.found("FindUnitToHeal", druid) == bleeding);
  b.world.mutable_state(dying)->flags.hidden = false;

  // An enemy is never healed, however badly hurt.
  const ObjectId foe = b.spawn(b.plain_class, Point{100, 0}, 2, 1);
  b.world.players().set(1, 2, Relation::allied, false);
  CHECK(b.found("FindUnitToHeal", druid) == dying);
  (void)foe;

  // A ram is excluded by class; a `Sentry` and a `Peaceful` are **not** --
  // that is the difference from `FindUnitToLearn`, whose exclusion set is a
  // different three, three instructions away in the executable.
  const ObjectId ram = b.spawn(b.ram_class, Point{100, 0}, 1, 1);
  CHECK(b.found("FindUnitToHeal", druid) == dying);
  (void)ram;
  const ObjectId sentry = b.spawn(b.sentry_class, Point{100, 0}, 1, 1);
  CHECK(b.found("FindUnitToHeal", druid) == sentry);

  // **The score is a fraction, not an absolute**, and the two readings have to
  // be made to disagree or the test proves nothing. With maxima of 200 and 100
  // the fractions are `1.5h` and `3h`, so they invert whenever the bigger
  // unit's health is between the smaller's and twice it: at 60 of 200 the
  // soldier scores 90 and a frail unit at 40 of 100 scores 120, so **the
  // soldier wins on the fraction** -- while a score on raw health would pick
  // the frail one, because 40 is less than 60. Both deficits clear the
  // threshold, so neither is parked.
  // Everything wounded so far is put back to full, so that the pair below is
  // the whole field and the comparison is between the two of them.
  for (const ObjectId healed : {scratched, bleeding, dying, distant}) {
    b.world.set_health(healed, 200);
  }
  b.world.set_health(sentry, 60);
  const ObjectId frail = b.spawn(b.frail_class, Point{100, 0}, 1, 40);
  CHECK(b.found("FindUnitToHeal", druid) == sentry);
  // And the inversion really is an inversion: drop the frail one to 20 -- a
  // fraction of 60, below the sentry's 90 -- and it takes the lead.
  b.world.set_health(frail, 20);
  CHECK(b.found("FindUnitToHeal", druid) == frail);
}

/// Curse maximises `attack`; cripple maximises `stamina` behind a real
/// predicate. Both take enemies only.
TEST(hero_curse_takes_the_hardest_hitter_and_cripple_the_freshest) {
  CasterBench b;
  const ObjectId caster = b.spawn(b.caster_class, Point{0, 0}, 1, 100);
  b.world.players().set(1, 2, Relation::allied, false);
  b.world.players().set(2, 1, Relation::allied, false);

  CHECK(b.found("GetBestCurseTarget", caster) == kNoObject);
  CHECK(b.found("GetBestCrippleTarget", caster) == kNoObject);

  // **An ally is never a target of either, and it has to be an ally that would
  // otherwise win.** A candidate with no combat record scores zero and loses to
  // everything, so an ally without one proves nothing; this one hits hardest of
  // anything on the field and has the most stamina.
  const ObjectId friend_ = b.spawn(b.plain_class, Point{100, 0}, 1, 200, 99);
  b.combat.set_profile(b.plain_class, [] {
    CombatProfile p;
    p.damage = 7;
    p.max_health = 200;
    return p;
  }());
  b.combat.add([&] {
    Combatant c;
    c.id = friend_;
    c.class_index = b.plain_class;
    c.owner = 1;
    c.health = 200;
    return c;
  }());
  b.combat.find(friend_)->attack_bonus = 900;
  CHECK(b.found("GetBestCurseTarget", caster) == kNoObject);
  CHECK(b.found("GetBestCrippleTarget", caster) == kNoObject);

  const ObjectId soldier = b.spawn(b.plain_class, Point{100, 0}, 2, 200, 10);
  b.combat.set_profile(b.plain_class, [] {
    CombatProfile p;
    p.damage = 7;
    p.max_health = 200;
    return p;
  }());
  b.combat.add([&] {
    Combatant c;
    c.id = soldier;
    c.class_index = b.plain_class;
    c.owner = 2;
    c.health = 200;
    return c;
  }());
  CHECK(b.found("GetBestCurseTarget", caster) == soldier);
  CHECK(b.found("GetBestCrippleTarget", caster) == soldier);

  // A harder hitter wins the curse; it has no stamina, so cripple keeps the
  // soldier. Two searches, two stats, one circle.
  const ObjectId ram = b.spawn(b.ram_class, Point{150, 0}, 2, 200, 0);
  b.combat.set_profile(b.ram_class, [] {
    CombatProfile p;
    p.damage = 20;
    p.max_health = 200;
    return p;
  }());
  b.combat.add([&] {
    Combatant c;
    c.id = ram;
    c.class_index = b.ram_class;
    c.owner = 2;
    c.health = 200;
    return c;
  }());
  CHECK(b.found("GetBestCurseTarget", caster) == ram);
  // ...and cripple excludes a ram by class anyway, twice over.
  CHECK(b.found("GetBestCrippleTarget", caster) == soldier);

  // A mage is excluded from the curse and not from the cripple.
  const ObjectId mage = b.spawn(b.mage_class, Point{120, 0}, 2, 200, 90);
  b.combat.set_profile(b.mage_class, [] {
    CombatProfile p;
    p.damage = 99;
    p.max_health = 200;
    return p;
  }());
  b.combat.add([&] {
    Combatant c;
    c.id = mage;
    c.class_index = b.mage_class;
    c.owner = 2;
    c.health = 200;
    return c;
  }());
  CHECK(b.found("GetBestCurseTarget", caster) == ram);
  CHECK(b.found("GetBestCrippleTarget", caster) == mage);

  // Cripple's own exclusions, which curse does not share.
  b.world.mutable_state(mage)->flags.messenger = true;
  CHECK(b.found("GetBestCrippleTarget", caster) == soldier);
  CHECK(b.found("GetBestCurseTarget", caster) == ram);
  b.world.mutable_state(mage)->flags.messenger = false;
  b.world.set_stamina(mage, 0);
  CHECK(b.found("GetBestCrippleTarget", caster) == soldier);

  // **Zero stamina is a rejection, not a low score.** An enemy with none of it
  // and nothing else against it must still lose to one with a single point --
  // which a "highest stamina wins" that started its best at zero would give it
  // anyway, so the case needs a rival with less than the best and more than
  // none.
  b.world.set_stamina(soldier, 1);
  const ObjectId spent = b.spawn(b.plain_class, Point{110, 0}, 2, 200, 0);
  CHECK(b.found("GetBestCrippleTarget", caster) == soldier);
  // And with nothing else on the field it is still not a target: an enemy with
  // no stamina to take is not a candidate, however alone it is. **Two clauses
  // enforce that and either alone would do** -- the predicate's floor and the
  // score's, which starts at zero -- so a fault in either survives and only the
  // behaviour can be pinned. This is that pin.
  b.world.set_stamina(soldier, 0);
  CHECK(b.found("GetBestCrippleTarget", caster) == kNoObject);
  b.world.set_stamina(soldier, 1);
  (void)spent;

  // And an enemy ram with the most stamina on the field is excluded by class,
  // which the earlier ram could not show because it had none.
  const ObjectId fresh_ram = b.spawn(b.ram_class, Point{130, 0}, 2, 200, 90);
  CHECK(b.found("GetBestCrippleTarget", caster) == soldier);
  // A `Peaceful` with the same is excluded too.
  const ObjectId quiet = b.spawn(b.peaceful_class, Point{130, 0}, 2, 200, 95);
  CHECK(b.found("GetBestCrippleTarget", caster) == soldier);
  (void)fresh_ram;
  (void)quiet;
}

/// `FindUnitBelowILevel(n)` wants an ally whose **inherent** level is strictly
/// below `n`, and only a `Military` or a `BaseMage`.
TEST(hero_find_unit_below_ilevel_wants_an_ally_strictly_below_the_ceiling) {
  CasterBench b;
  const ObjectId caster = b.spawn(b.caster_class, Point{0, 0}, 1, 100);
  const auto below = [&](std::int32_t ceiling) {
    return b.found("FindUnitBelowILevel", caster, {script::Value::integer(ceiling)});
  };

  const ObjectId pupil = b.spawn(b.plain_class, Point{100, 0}, 1);
  CHECK(b.heroes.set_level(pupil, 5));
  CHECK(below(5) == kNoObject);  // strictly below, so 5 does not qualify
  CHECK(below(6) == pupil);

  // An enemy is not taught.
  b.world.players().set(1, 2, Relation::allied, false);
  const ObjectId foe = b.spawn(b.plain_class, Point{100, 0}, 2);
  CHECK(b.heroes.set_level(foe, 1));
  CHECK(below(5) == kNoObject);

  // A `Sentry` is excluded by class even though it is `Military`.
  const ObjectId sentry = b.spawn(b.sentry_class, Point{100, 0}, 1);
  CHECK(b.heroes.set_level(sentry, 1));
  CHECK(below(5) == kNoObject);

  // A messenger is not taught either.
  const ObjectId messenger = b.spawn(b.plain_class, Point{100, 0}, 1);
  CHECK(b.heroes.set_level(messenger, 1));
  b.world.mutable_state(messenger)->flags.messenger = true;
  CHECK(below(5) == kNoObject);
  b.world.mutable_state(messenger)->flags.messenger = false;
  CHECK(below(5) == messenger);

  CHECK(b.found("FindUnitBelowILevel", 9999, {script::Value::integer(9)}) == kNoObject);
}

// ---------------------------------------------------------------------------
// when the army was last struck
// ---------------------------------------------------------------------------

/// `record_army_attacked` stamps the victim's **hero**, and carries the
/// victim's own handle -- which is what `Hero::LastAttacker` returns and why
/// its name is a misnomer.
TEST(hero_a_struck_warrior_stamps_its_heros_clock) {
  Fixture f;
  const ObjectId leader = f.spawn_hero();
  const ObjectId soldier = f.spawn_warrior();
  REQUIRE(f.heroes.attach(f.world, soldier, leader));

  const HeroRecord* record = f.heroes.hero(leader);
  REQUIRE(record != nullptr);
  CHECK(record->army_attacked_at == 0);
  CHECK(record->army_attacked_unit == kNoObject);

  record_army_attacked(f.world, soldier, 7200);
  REQUIRE(f.heroes.hero(leader) != nullptr);
  CHECK(f.heroes.hero(leader)->army_attacked_at == 7200);
  // The **victim**, not an attacker: 0x005dc23f stores `word [victim+8]`.
  CHECK(f.heroes.hero(leader)->army_attacked_unit == soldier);
}

/// A hero with no hero of its own stamps on itself; a unit in nobody's army
/// stamps nowhere.
TEST(hero_a_struck_hero_stamps_itself_and_a_loose_unit_stamps_nothing) {
  Fixture f;
  const ObjectId lone = f.spawn_hero();
  record_army_attacked(f.world, lone, 3000);
  REQUIRE(f.heroes.hero(lone) != nullptr);
  CHECK(f.heroes.hero(lone)->army_attacked_at == 3000);
  CHECK(f.heroes.hero(lone)->army_attacked_unit == lone);

  // A warrior attached to nobody. Nothing to write on, and nothing crashes.
  const ObjectId loose = f.spawn_warrior();
  record_army_attacked(f.world, loose, 9000);
  CHECK(f.heroes.hero(lone)->army_attacked_at == 3000);

  // A second hero, attached into the first's army, stamps the **leader** --
  // the handle at `[victim+0x170]` is consulted before the `kSyncHero` branch.
  const ObjectId follower = f.spawn_hero(1);
  f.heroes.register_unit(f.world, follower);
  REQUIRE(f.heroes.attach(f.world, follower, lone));
  record_army_attacked(f.world, follower, 5000);
  CHECK(f.heroes.hero(lone)->army_attacked_at == 5000);
  CHECK(f.heroes.hero(lone)->army_attacked_unit == follower);
  REQUIRE(f.heroes.hero(follower) != nullptr);
  CHECK(f.heroes.hero(follower)->army_attacked_at == 0);
}

/// `TimePastLastAttack()` is `now - stamp`, and a hero that has never been
/// struck answers the whole clock -- which is "long ago", the answer all three
/// shipped readers want.
TEST(hero_time_past_last_attack_is_the_clock_minus_the_stamp) {
  Fixture f;
  const ObjectId leader = f.spawn_hero();
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_world_host(registry);
  define_hero_host(registry);
  HostContext context;
  context.world = &f.world;

  const auto ask = [&](ObjectId id) {
    const std::uint32_t index =
        registry.find(script::CallKind::member, "TimePastLastAttack", 0);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost || registry.entry(index).fn == nullptr) return -1;
    std::vector<script::Value> args{script::Value::object(kTypeObj, id)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "TimePastLastAttack";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };

  f.world.clock().advance(10000);
  CHECK(ask(leader) == 10000);

  record_army_attacked(f.world, leader, 7500);
  CHECK(ask(leader) == 2500);

  // The three shipped readers all compare against 2500 or 5000, so the boundary
  // is worth pinning: `HERO_SNEAK_VERIFY.VS` is `> 2500` and answers false here.
  f.world.clock().advance(400);
  CHECK(ask(leader) == 2900);

  // A stamp in the future -- which two clocks can produce -- clamps at zero
  // rather than going negative.
  record_army_attacked(f.world, leader, 99999);
  CHECK(ask(leader) == 0);

  // A receiver that is not a registered hero answers the whole clock.
  CHECK(ask(999999) == 10400);
}

/// `h.maxarmy` is the **cap**, not the class attribute.
///
/// 0x0052e380 reads `[hero+0x1f8]`, and `IsHeroArmyFull` (0x0052e320) compares
/// the army size at `[+0x1e4]` against that same field -- so the two have to
/// agree about where the ceiling is, which is what pins this to
/// `HeroSystem::max_army` and not to `HeroRecord::max_army`. Administration
/// moves them together.
TEST(hero_maxarmy_reads_the_cap_that_is_full_and_not_the_class_number) {
  Fixture f;
  const ObjectId hero = f.spawn_hero();

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_hero_host(registry);
  HostContext context;
  context.world = &f.world;
  const auto call = [&](const char* name, script::Value receiver) {
    const std::uint32_t index = registry.find(script::CallKind::member, name, 0);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    std::vector<script::Value> args{receiver};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };
  const script::Value self = script::Value::object(kTypeObj, hero);

  CHECK(call("maxarmy", self).value.as_integer() == 50);
  CHECK(f.heroes.hero(hero)->max_army == 50);

  // Three points of Administration is +6, and the entry point moves with the
  // cap rather than with the stored class number, which stays at 50.
  f.grant(hero, 3);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::administration, 3));
  CHECK(call("maxarmy", self).value.as_integer() == 56);
  CHECK(f.heroes.hero(hero)->max_army == 50);

  // And it is the number `IsHeroArmyFull` turns over at.
  for (int i = 0; i < 56; ++i) CHECK(f.heroes.attach(f.world, f.spawn_warrior(), hero));
  CHECK(call("IsHeroArmyFull", self).value.truthy_scalar());
  CHECK(call("maxarmy", self).value.as_integer() == 56);

  // A receiver that is not a registered hero answers 0. `IsHeroArmyFull`
  // answers *true* on the same miss -- the thunk pushes 1 after its diagnostic
  // -- and that oddity is that entry point's own.
  const ObjectId stranger = f.spawn_warrior();
  const script::Value other = script::Value::object(kTypeObj, stranger);
  CHECK(call("maxarmy", other).value.as_integer() == 0);
}

/// `GetRandomHeroClass(base, player)` -- two exclusions, one of them a
/// preference, and a class id back.
TEST(get_random_hero_class_skips_a_hero_you_have_and_prefers_an_unseen_face) {
  ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor" parent=""/>)",
      R"(<class id="BritonHero" cpp_class="CVXHero" parent="Object"/>)",
      // Two Britons on one portrait, as `BHero1` and `BHero1a` are, and one on
      // its own -- which is what separates "already have it" from "already have
      // that face".
      R"(<class id="BHero1" cpp_class="CVXHero" parent="BritonHero"><properties icon="icons/BHero1.bmp"/></class>)",
      R"(<class id="BHero1a" cpp_class="CVXHero" parent="BritonHero"><properties icon="icons/BHero1.bmp"/></class>)",
      R"(<class id="BHero2" cpp_class="CVXHero" parent="BritonHero"><properties icon="icons/BHero2.bmp"/></class>)",
      R"(<class id="EgyptianHero" cpp_class="CVXHero" parent="Object"/>)",
      R"(<class id="EHero1" cpp_class="CVXHero" parent="EgyptianHero"><properties icon="icons/EHero1.bmp"/></class>)",
  };
  const char* names[] = {"object.sc.xml", "britonhero.sc.xml", "bhero1.sc.xml",
                         "bhero1a.sc.xml", "bhero2.sc.xml",    "egyptianhero.sc.xml",
                         "ehero1.sc.xml"};
  for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    REQUIRE(graph.add(bytes(docs[i]), names[i]).ok());
  }
  graph.link();

  Fixture f;
  f.world.set_class_graph(&graph);
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_hero_host(registry);
  HostContext context;
  context.world = &f.world;
  const std::uint32_t index =
      registry.find(script::CallKind::free_function, "GetRandomHeroClass", 2);
  REQUIRE(index != script::kUnresolvedHost);
  REQUIRE(registry.entry(index).fn != nullptr);
  const auto ask = [&](const char* base, std::int32_t player) {
    std::vector<script::Value> args{script::Value::string(std::string(base)),
                                    script::Value::integer(player)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "GetRandomHeroClass";
    ctx.kind = script::CallKind::free_function;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_string() ? std::string(out.value.as_string()) : std::string("<no>");
  };
  /// Every answer the walk can give over many draws, so a rule that removed a
  /// class shows as an absence rather than as luck.
  const auto seen = [&](const char* base, std::int32_t player) {
    std::vector<std::string> got;
    for (int i = 0; i < 200; ++i) {
      std::string one = ask(base, player);
      if (std::find(got.begin(), got.end(), one) == got.end()) got.push_back(one);
    }
    std::sort(got.begin(), got.end());
    return got;
  };
  const auto has = [](const std::vector<std::string>& all, const char* what) {
    return std::find(all.begin(), all.end(), std::string(what)) != all.end();
  };

  // The subtree, base included -- and nothing from the other race's subtree.
  std::vector<std::string> all = seen("BritonHero", 1);
  CHECK(has(all, "BritonHero"));
  CHECK(has(all, "BHero1"));
  CHECK(has(all, "BHero1a"));
  CHECK(has(all, "BHero2"));
  CHECK(!has(all, "EHero1"));
  CHECK(!has(all, "EgyptianHero"));

  // A hero of the player's own removes **its class** outright...
  const ObjectId gawain = f.world.spawn(NativeClass::unit, nullptr, graph.find("BHero1"));
  f.world.find(gawain)->state.flags.is_hero = true;
  REQUIRE(f.world.set_owner(gawain, 0));
  all = seen("BritonHero", 1);
  CHECK(!has(all, "BHero1"));
  // ...and pushes the sibling that shares its portrait out of the narrow list,
  // so `BHero1a` is only reachable once nothing else is.
  CHECK(!has(all, "BHero1a"));
  CHECK(has(all, "BHero2"));
  CHECK(has(all, "BritonHero"));

  // The face rule is a **preference**: with every unclashed candidate gone,
  // the wide list answers and `BHero1a` comes back.
  const ObjectId other = f.world.spawn(NativeClass::unit, nullptr, graph.find("BHero2"));
  f.world.find(other)->state.flags.is_hero = true;
  REQUIRE(f.world.set_owner(other, 0));
  const ObjectId root = f.world.spawn(NativeClass::unit, nullptr, graph.find("BritonHero"));
  f.world.find(root)->state.flags.is_hero = true;
  REQUIRE(f.world.set_owner(root, 0));
  all = seen("BritonHero", 1);
  CHECK(all == std::vector<std::string>{"BHero1a"});

  // Another player's heroes are not the asker's. Player 2 sees everything.
  all = seen("BritonHero", 2);
  CHECK(has(all, "BHero1"));
  CHECK(has(all, "BHero2"));
  CHECK(has(all, "BritonHero"));

  // **A candidate that declares no icon never clashes**, even when the player
  // already has a faceless hero -- the original's test is on the *candidate's*
  // bitmap pointer being non-null, and two classes with no icon both hold null.
  // `BritonHero` is owned above and `EgyptianHero` declares no icon either, so
  // this asks the other subtree.
  all = seen("EgyptianHero", 1);
  CHECK(has(all, "EgyptianHero"));
  CHECK(has(all, "EHero1"));

  // A non-hero object of a hero class excludes nothing: the sweep's predicate
  // is the hero flag, not the class.
  Fixture g;
  g.world.set_class_graph(&graph);
  HostContext other_context;
  other_context.world = &g.world;
  const ObjectId statue = g.world.spawn(NativeClass::unit, nullptr, graph.find("BHero1"));
  REQUIRE(g.world.set_owner(statue, 0));
  {
    std::vector<script::Value> args{script::Value::string(std::string("BHero1")),
                                    script::Value::integer(1)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &other_context;
    ctx.name = "GetRandomHeroClass";
    ctx.kind = script::CallKind::free_function;
    CHECK(std::string(registry.entry(index).fn(ctx).value.as_string()) == "BHero1");
  }

  // A name the graph does not know is the empty string, which is `Place`
  // spawning nothing -- and so is a subtree the player has exhausted.
  CHECK(ask("NoSuchHero", 1).empty());
  CHECK(ask("BHero1", 1).empty());
}

/// `o.GetItem(n)` is **1-based**, and past the end it answers the invalid
/// handle rather than refusing.
TEST(get_item_indexes_the_inventory_from_one) {
  Fixture f;
  ItemCatalog catalog;
  REQUIRE(catalog.load(bytes(kItemsXml)).ok());
  f.heroes.items().set_catalog(&catalog);
  const ObjectId hero = f.spawn_hero();

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_hero_host(registry);
  HostContext context;
  context.world = &f.world;
  const auto call = [&](const char* name, std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(script::CallKind::member, name,
                                              static_cast<std::uint16_t>(args.size() - 1));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };
  const script::Value self = script::Value::object(kTypeObj, hero);
  const auto item_at = [&](std::int32_t n) {
    const script::HostOutcome out = call("GetItem", {self, script::Value::integer(n)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() ? static_cast<ObjectId>(out.value.as_object().id)
                                 : sim::kNoObject;
  };

  // An empty inventory answers nothing for every index.
  CHECK(item_at(1) == sim::kNoObject);
  CHECK(item_at(0) == sim::kNoObject);
  CHECK(item_at(-1) == sim::kNoObject);

  const char* kThree[] = {"Bear teeth amulet", "King's Belt", "Veteran Offence"};
  for (const char* name : kThree) {
    REQUIRE(f.heroes.items().add(f.world, hero, name) != sim::kNoObject);
  }
  CHECK(call("item_count", {self}).value.as_integer() == 3);

  // **One-based**, and in `contents_for`'s order -- the same order
  // `RemoveItem` indexes into from the other side.
  std::vector<ObjectId> order;
  f.heroes.items().contents_for(hero, order);
  REQUIRE(order.size() == 3);
  CHECK(item_at(1) == order[0]);
  CHECK(item_at(2) == order[1]);
  CHECK(item_at(3) == order[2]);
  // Zero is not the first item, which is the whole of what "1-based" means.
  CHECK(item_at(0) == sim::kNoObject);
  // ...and past the end is the same invalid handle a missing receiver gets.
  CHECK(item_at(4) == sim::kNoObject);

  const script::Value stranger = script::Value::object(kTypeObj, 9999);
  const script::HostOutcome missed =
      call("GetItem", {stranger, script::Value::integer(1)});
  CHECK(missed.status == script::HostStatus::ok);
  CHECK(!missed.value.as_object().valid());
}

// ==========================================================================
// death: what the death virtual undoes, and when
// ==========================================================================
//
// `gbr.exe`'s unit death virtual (0x005db270) runs from the damage that killed
// the unit, and after the `ondie` hook it detaches the unit from its hero
// (0x005db190), takes it out of its squad (0x0041eee0 -> 0x004448a0), and only
// then sets the dying state; a hero's override detaches its army first
// (0x0052fb00). All 15 dying units across the nine dumps print
// `hero handle 65535` and `squad=0(0)`. These tests drive the death through
// `CombatSystem`, which is where this engine's deaths happen.

namespace {

/// The hero fixture with a world-bound combat system beside it, and a death
/// long enough that a corpse is still in the world for everything below.
struct DeathBench {
  Fixture f;
  CombatSystem combat;

  DeathBench() {
    f.world.add_system(&combat);
    combat.set_world_bound(true);
    combat.set_death_duration(60000);
  }

  /// Tell combat about everything spawned so far.
  void enlist() { combat.start(f.world); }

  void kill(ObjectId id) { (void)combat.apply_damage(f.world, id, combat.health(id)); }
};

}  // namespace

TEST(a_warrior_leaves_its_heros_army_and_squad_in_the_instant_it_dies) {
  DeathBench b;
  const ObjectId hero = b.f.spawn_hero();
  const ObjectId warrior = b.f.spawn_warrior();
  const ObjectId other = b.f.spawn_warrior();
  REQUIRE(b.f.heroes.attach(b.f.world, warrior, hero));
  REQUIRE(b.f.heroes.attach(b.f.world, other, hero));
  const SquadKey squad = b.f.heroes.squad_of(hero);
  REQUIRE(b.f.heroes.squads().find(squad)->size() == 3);
  b.enlist();

  b.kill(warrior);
  // No turn in between: the corpse is still in the world and still dying.
  REQUIRE(b.combat.is_dying(warrior));
  REQUIRE(b.f.world.find(warrior) != nullptr);
  CHECK(b.f.heroes.hero_of(warrior) == kNoObject);
  CHECK(b.f.heroes.squad_of(warrior) == kNoSquad);
  CHECK(b.f.heroes.army_size(hero) == 1);
  CHECK(b.f.heroes.squads().find(squad)->size() == 2);
  CHECK(!b.f.heroes.squads().find(squad)->contains(warrior));
  // The survivor is where it was.
  CHECK(b.f.heroes.hero_of(other) == hero);
  CHECK(b.f.heroes.squad_of(other) == squad);
}

TEST(a_dead_hero_releases_its_army_and_its_squad) {
  // 0x0052fbb0: the hero's override runs `DetachArmy` before the unit path.
  DeathBench b;
  const ObjectId hero = b.f.spawn_hero();
  const ObjectId first = b.f.spawn_warrior();
  const ObjectId second = b.f.spawn_warrior();
  REQUIRE(b.f.heroes.attach(b.f.world, first, hero));
  REQUIRE(b.f.heroes.attach(b.f.world, second, hero));
  const SquadKey squad = b.f.heroes.squad_of(hero);
  b.enlist();

  b.kill(hero);
  REQUIRE(b.combat.is_dying(hero));
  CHECK(b.f.heroes.army_size(hero) == 0);
  for (const ObjectId member : {first, second}) {
    CHECK(b.f.heroes.hero_of(member) == kNoObject);
    CHECK(b.f.heroes.squad_of(member) == kNoSquad);
  }
  // The hero leaves its own squad, and an emptied squad is gone.
  CHECK(b.f.heroes.squad_of(hero) == kNoSquad);
  CHECK(b.f.heroes.hero(hero)->squad == kNoSquad);
  CHECK(b.f.heroes.squads().find(squad) == nullptr);
}

TEST(an_ai_squad_is_one_smaller_the_moment_a_member_dies) {
  // Leaderless AI squads were never pruned at all: `clear_dead` walks hero
  // armies only, so a dead member was counted by `Squad` size, by the bodies
  // census and by `Squadize`'s capacity of ten for the rest of the match.
  DeathBench b;
  const ObjectId a = b.f.spawn_warrior();
  const ObjectId c = b.f.spawn_warrior();
  const SquadKey key = b.f.heroes.squads().create(1);
  for (const ObjectId id : {a, c}) {
    REQUIRE(b.f.heroes.squads().join(key, id));
    b.f.heroes.unit(id)->squad = key;
  }
  b.enlist();

  b.kill(a);
  REQUIRE(b.combat.is_dying(a));
  REQUIRE(b.f.heroes.squads().find(key) != nullptr);
  CHECK(b.f.heroes.squads().find(key)->size() == 1);
  CHECK(b.f.heroes.squad_of(a) == kNoSquad);
  CHECK(b.f.heroes.squads().squad_of(a) == kNoSquad);
  CHECK(b.f.heroes.squad_of(c) == key);

  // And the last one out takes the squad with it.
  b.kill(c);
  CHECK(b.f.heroes.squads().find(key) == nullptr);
}

TEST(egoism_never_drains_a_corpse_in_its_dying_window) {
  // `run_egoism` draws a random member of the army. A corpse still listed there
  // is a draw that drains nobody -- `stolen` comes out at zero on a unit at
  // zero health -- so the living warrior would be skipped at the corpse's odds.
  DeathBench b;
  b.f.world.seed(0x5eed1234);
  const ObjectId hero = b.f.spawn_hero(1, 100);
  const ObjectId corpse = b.f.spawn_warrior(1, 200);
  const ObjectId living = b.f.spawn_warrior(1, 200);  // the class maximum
  REQUIRE(b.f.heroes.attach(b.f.world, corpse, hero));
  REQUIRE(b.f.heroes.attach(b.f.world, living, hero));
  b.f.grant(hero, 10);
  REQUIRE(b.f.heroes.set_skill(hero, HeroSkill::egoism, 1));  // 20 a drain
  b.enlist();

  b.kill(corpse);
  REQUIRE(b.combat.is_dying(corpse));
  // Combat is unbound once the corpse is made, for a reason that is not this
  // test's: its end-of-turn write puts its own health figures back into the
  // world, and Egoism drains the world's, so a bound combat would undo every
  // drain before it could be counted. Unbound, it neither writes nor despawns,
  // and the corpse lies there for as long as the test needs it to.
  b.combat.set_world_bound(false);
  // Eight drains, at 5,000 .. 40,000.
  b.f.world.advance(400);
  b.f.world.advance_turns(100);
  REQUIRE(b.f.world.find(corpse) != nullptr);
  REQUIRE(b.combat.is_dying(corpse));
  CHECK(b.f.world.state(living)->health == 200 - 8 * 20);
}

TEST(wisdom_draws_once_and_credits_the_hero_one_point_on_success) {
  // 0x005db270's first block: a hero with Wisdom draws `[0, 99]` and pays when
  // `PercentPerWisdomLevel * points` is greater than the draw -- strictly.
  const auto run = [](std::int32_t percent, std::int32_t points, bool attached,
                      std::int32_t& gained, bool& drew) {
    DeathBench b;
    b.f.world.seed(0x0badcafe);
    const ObjectId hero = b.f.spawn_hero();
    const ObjectId warrior = b.f.spawn_warrior();
    if (attached) REQUIRE(b.f.heroes.attach(b.f.world, warrior, hero));
    REQUIRE(b.f.heroes.load_skill(hero, HeroSkill::wisdom, points));
    b.f.heroes.constants().percent_per_wisdom_level = percent;
    b.enlist();
    const std::uint32_t before = b.f.world.rng().state();
    const std::int32_t experience = b.f.heroes.experience(hero);
    b.kill(warrior);
    gained = b.f.heroes.experience(hero) - experience;
    drew = b.f.world.rng().state() != before;
  };

  // The draw the seed gives, found the way the death will find it.
  Rng probe(0x0badcafe);
  const std::int32_t draw = probe.between(0, 99);

  std::int32_t gained = 0;
  bool drew = false;
  run(draw + 1, 1, true, gained, drew);  // product one above the draw: paid
  CHECK(drew);
  CHECK(gained == 1);
  run(draw, 1, true, gained, drew);  // product equal to the draw: not paid
  CHECK(drew);
  CHECK(gained == 0);
  run(100, 0, true, gained, drew);  // no Wisdom: no draw at all
  CHECK(!drew);
  CHECK(gained == 0);
  run(100, 5, false, gained, drew);  // no hero: no draw at all
  CHECK(!drew);
  CHECK(gained == 0);
}

TEST(a_held_unit_keeps_its_squad_through_death_and_leaves_it_at_the_erase) {
  // The death virtual's fifth step: a unit in a holder is erased on the spot
  // and the erase does the detaching, so the death path itself skips them.
  // This engine does not erase it there -- it lies in the holder for its death
  // strip like any corpse -- and the erase that ends the corpse is where it
  // leaves. An AI squad, because `clear_dead` would prune a hero's army at the
  // same moment and hide which of the two did it.
  DeathBench b;
  b.combat.set_death_duration(1000);
  const ObjectId held = b.f.spawn_warrior();
  const ObjectId other = b.f.spawn_warrior();
  const SquadKey key = b.f.heroes.squads().create(1);
  for (const ObjectId id : {held, other}) {
    REQUIRE(b.f.heroes.squads().join(key, id));
    b.f.heroes.unit(id)->squad = key;
  }
  const ObjectId holder = b.f.world.spawn_internal(InternalKind::holder);
  REQUIRE(b.f.world.put_in_holder(held, holder));
  b.enlist();

  b.kill(held);
  REQUIRE(b.combat.is_dying(held));
  CHECK(b.f.heroes.squad_of(held) == key);
  CHECK(b.f.heroes.squads().find(key)->size() == 2);

  b.f.world.advance_turns(5);
  REQUIRE(b.f.world.find(held) == nullptr);
  CHECK(b.f.heroes.squad_of(held) == kNoSquad);
  CHECK(b.f.heroes.squads().find(key)->size() == 1);
}
