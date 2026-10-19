// The stored stats and their rebuild (`sim/stats.hpp`,
// `docs/formats/unit-stats.md`): each test encodes one rule of 0x005d9df0 or
// one of the moments it runs, on a world built from invented classes.

#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/item.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/stats.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> text(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

struct Bench {
  ClassGraph graph;
  World world;
  MovementSystem movement;
  CombatSystem combat;
  HeroSystem heroes;
  EnvSystem env;
  CommandSystem commands;
  ItemCatalog catalog;
  ClassIndex soldier = kNoClass;
  ClassIndex tough = kNoClass;
  ClassIndex hero_class = kNoClass;
  ClassIndex post = kNoClass;
  ClassIndex sentry = kNoClass;
  ClassIndex slinger = kNoClass;

  Bench() {
    graph.add(text(R"(<class id="Soldier" cpp_class="CVXUnit" parent="">
        <properties maxhealth="200" maxstamina="10" damage="10" damage_type="slash"
                    armor_slash="4" armor_pierce="2" range="100" inventory_size="4"/>
      </class>)"),
              "soldier.sc.xml");
    graph.add(text(R"(<class id="Tough" cpp_class="CVXUnit" parent="Soldier">
        <properties unit_specials="Toughness"/>
      </class>)"),
              "tough.sc.xml");
    graph.add(text(R"(<class id="Chief" cpp_class="CVXHero" parent="Soldier">
        <properties maxhealth="1000" damage="20" max_army="50"/>
      </class>)"),
              "chief.sc.xml");
    graph.add(text(R"(<class id="Sentry" cpp_class="CVXUnit" parent="Soldier"/>)"),
              "sentry.sc.xml");
    graph.add(text(R"(<class id="Pebble" cpp_class="CVXProjectile" parent=""/>)"), "pebble.sc.xml");
    graph.add(text(R"(<class id="Slinger" cpp_class="CVXUnit" parent="Soldier">
        <properties projectile_class="Pebble" range="300"/>
      </class>)"),
              "slinger.sc.xml");
    graph.add(text(R"(<class id="Post" cpp_class="CVXDecor" parent="">
        <properties range="321"/>
      </class>)"),
              "post.sc.xml");
    graph.link();
    post = graph.find("Post");
    sentry = graph.find("Sentry");
    slinger = graph.find("Slinger");
    soldier = graph.find("Soldier");
    tough = graph.find("Tough");
    hero_class = graph.find("Chief");

    // Two invented formations: one that pays, one that takes away.
    Result<FormationTable> forms = FormationTable::parse(text(R"(<Formations>
        <Default Name="Square"/>
        <FormationClass Name="Square" BonusDamage="3" BonusArmor="2" BonusRange="25"/>
        <FormationClass Name="Ragged" BonusDamage="-40" BonusDefenceSlash="-9"/>
      </Formations>)"));
    REQUIRE(forms.ok());
    movement.set_formations(std::move(forms.value()));

    ItemDefinition charm;
    charm.id = "Charm";
    charm.bonus.damage_percent = 50;
    charm.bonus.armor_slash = 3;
    charm.bonus.health = 100;
    (void)catalog.add(charm);
    ItemDefinition stone;
    stone.id = "Stone";
    stone.bonus.damage = 1;
    stone.bonus.armor_pierce_percent = 50;
    (void)catalog.add(stone);
    heroes.items().set_catalog(&catalog);

    world.set_class_graph(&graph);
    combat.set_class_graph(&graph);
    combat.set_world_bound(true);
    world.add_system(&movement);
    world.add_system(&combat);
    world.add_system(&heroes);
    world.add_system(&env);
    world.add_system(&commands);
    world.start();
  }

  ObjectId spawn(ClassIndex cls, PlayerId owner = 1) {
    const ObjectId id =
        world.spawn(cls == hero_class ? NativeClass::hero : NativeClass::unit, nullptr, cls);
    world.set_owner(id, owner);
    world.set_position(id, Point{1000 + static_cast<std::int32_t>(id) * 10, 1000});
    world.set_health(id, cls == hero_class ? 1000 : 200);
    (void)combat.reconcile(world);
    if (cls == hero_class) {
      (void)heroes.register_hero(world, id);
    } else {
      (void)heroes.register_unit(world, id);
    }
    return id;
  }

  [[nodiscard]] const StatBlock& stats(ObjectId id) {
    static const StatBlock none{};
    const Combatant* unit = combat.find(id);
    CHECK(unit != nullptr);
    return unit != nullptr ? unit->stats : none;
  }
  [[nodiscard]] std::int32_t health(ObjectId id) { return combat.health(id); }

  /// `killer` fights `victim`, left one health from death, until the victim
  /// falls. Answers whether it did.
  bool kill(ObjectId killer, ObjectId victim) {
    (void)combat.apply_damage(world, victim, combat.health(victim) - 1);
    if (!combat.order_attack(killer, victim)) return false;
    for (int turn = 0; turn < 10 && combat.is_alive(victim); ++turn) world.advance(400);
    return !combat.is_alive(victim);
  }
};

}  // namespace

/// Items, 0x005d9ff8..0x005da23d: an item whose kind an earlier item in the
/// inventory already has adds nothing, and a percent is of the class's own
/// number, not of the running total. Taking an item out rebuilds too
/// (0x005afc50), and health follows the maximum both ways.
TEST(stats_items_count_each_kind_once_and_take_percents_of_the_class) {
  Bench b;
  const ObjectId u = b.spawn(b.soldier);
  CHECK(b.stats(u).attack == 10);
  CHECK(b.stats(u).max_health == 200);

  const ObjectId first = b.heroes.items().add(b.world, u, "Charm");
  REQUIRE(first != kNoObject);
  CHECK(b.stats(u).attack == 15);  // 10 + 50% of the class's 10
  CHECK(b.stats(u).armour_slash == 7);
  CHECK(b.stats(u).max_health == 300);
  CHECK(b.health(u) == 300);  // 200 of 200 is 300 of 300

  // A second of the same kind changes nothing; another kind adds.
  const ObjectId second = b.heroes.items().add(b.world, u, "Charm");
  REQUIRE(second != kNoObject);
  CHECK(b.stats(u).attack == 15);
  CHECK(b.stats(u).max_health == 300);
  REQUIRE(b.heroes.items().add(b.world, u, "Stone") != kNoObject);
  CHECK(b.stats(u).attack == 16);
  CHECK(b.stats(u).armour_pierce == 3);  // 2 + 50% of 2

  // Out again: one Charm still counts, none does not.
  REQUIRE(b.heroes.items().remove(b.world, first));
  CHECK(b.stats(u).attack == 16);
  REQUIRE(b.heroes.items().remove(b.world, second));
  CHECK(b.stats(u).attack == 11);
  CHECK(b.stats(u).max_health == 200);
  CHECK(b.health(u) == 200);
}

/// The leader's skills, 0x005da34f..0x005da6fe: Epic skills on the hero
/// alone; Team Attack and Team Defense on the army and on the hero, which
/// leads itself; Defensive Cry on both; Frenzy doubles the army's attack and
/// not the hero's. Each lands through the rebuild its trigger makes --
/// attach, the skill writer, the stamp and the expiry.
TEST(stats_a_heros_skills_reach_the_party_through_the_rebuild) {
  Bench b;
  const ObjectId chief = b.spawn(b.hero_class);
  const ObjectId man = b.spawn(b.soldier);
  REQUIRE(b.heroes.attach(b.world, man, chief));

  REQUIRE(b.heroes.set_skill(b.world, chief, HeroSkill::epic_attack, 2));
  CHECK(b.stats(chief).attack == 30);  // 20 + 5 * 2
  CHECK(b.stats(man).attack == 10);
  REQUIRE(b.heroes.set_skill(b.world, chief, HeroSkill::epic_endurance, 1));
  CHECK(b.stats(chief).max_health == 1100);
  CHECK(b.health(chief) == 1100);

  REQUIRE(b.heroes.set_skill(b.world, chief, HeroSkill::team_attack, 3));
  CHECK(b.stats(man).attack == 13);
  CHECK(b.stats(chief).attack == 33);
  REQUIRE(b.heroes.set_skill(b.world, chief, HeroSkill::team_defense, 2));
  CHECK(b.stats(man).armour_slash == 6);
  CHECK(b.stats(man).armour_pierce == 4);

  // A warrior that joins later is rebuilt on joining, and on leaving.
  const ObjectId late = b.spawn(b.soldier);
  CHECK(b.stats(late).attack == 10);
  REQUIRE(b.heroes.attach(b.world, late, chief));
  CHECK(b.stats(late).attack == 13);
  REQUIRE(b.heroes.detach(b.world, late));
  CHECK(b.stats(late).attack == 10);

  REQUIRE(b.heroes.set_skill(b.world, chief, HeroSkill::frenzy, 1));
  REQUIRE(b.heroes.set_skill(b.world, chief, HeroSkill::defensive_cry, 1));
  REQUIRE(b.heroes.use_skill(b.world, chief, HeroSkill::frenzy));
  CHECK(b.stats(man).attack == 26);    // (10 + 3) doubled
  CHECK(b.stats(chief).attack == 33);  // the hero is not doubled
  REQUIRE(b.heroes.use_skill(b.world, chief, HeroSkill::defensive_cry));
  CHECK(b.stats(man).armour_slash == 26);    // 4 + 2 + 20
  CHECK(b.stats(chief).armour_slash == 26);  // the hero too

  // Both have ended by three seconds on, and the expiry rebuilt the party.
  for (int turn = 0; turn < 10; ++turn) b.world.advance(400);
  CHECK(b.stats(man).attack == 13);
  CHECK(b.stats(man).armour_slash == 6);
  CHECK(b.stats(chief).armour_slash == 6);
}

/// Formation, 0x005da46b..0x005da54d: while the running command is
/// `stand_position`, the leader's formation adds its damage, armour and range
/// percent, and then attack, armour and range are floored at 0 -- in this
/// branch only. The command's start and end are the rebuilds (0x005b4f20).
TEST(stats_the_formation_pays_while_the_unit_stands_in_it) {
  Bench b;
  const ObjectId chief = b.spawn(b.hero_class);
  const ObjectId man = b.spawn(b.soldier);
  REQUIRE(b.heroes.attach(b.world, man, chief));

  REQUIRE(b.commands.set_command(b.world, man, kStandPositionCommand, Command{}) != 0);
  b.world.advance(400);
  CHECK(b.stats(man).attack == 13);         // 10 + 3
  CHECK(b.stats(man).armour_slash == 6);    // BonusArmor writes both
  CHECK(b.stats(man).armour_pierce == 4);
  CHECK(b.stats(man).range == 125);         // 100 + 25%
  CHECK(b.combat.range_of(*b.combat.find(man)) == 125);

  // Another command: the bonus goes.
  REQUIRE(b.commands.set_command(b.world, man, "move", Command{}) != 0);
  b.world.advance(400);
  CHECK(b.stats(man).attack == 10);
  CHECK(b.stats(man).range == 100);

  // A formation that takes more than there is stops at zero while standing,
  // and a negative record outside the formation is not floored.
  b.movement.state(chief).formation = "Ragged";
  StatBonus minus;
  minus.armour_slash = -10;
  REQUIRE(b.combat.add_bonus(man, minus));
  CHECK(b.stats(man).armour_slash == -6);
  REQUIRE(b.commands.set_command(b.world, man, kStandPositionCommand, Command{}) != 0);
  b.world.advance(400);
  CHECK(b.stats(man).attack == 0);
  CHECK(b.stats(man).armour_slash == 0);
}

/// Toughness and the Statue of Amon-Ra, 0x005da25d..0x005da349: maximum
/// health gains five per level past the first, by the level the unit's own
/// experience stands at; the statue needs the owner's entry to read exactly
/// `researched`, and reaches a standing unit at its next rebuild.
TEST(stats_toughness_and_the_statue_add_health_per_level) {
  Bench b;
  const ObjectId plain = b.spawn(b.soldier);
  const ObjectId hard = b.spawn(b.tough);
  REQUIRE(b.heroes.set_experience(b.world, plain, b.heroes.experience_for_level(5)));
  REQUIRE(b.heroes.set_experience(b.world, hard, b.heroes.experience_for_level(5)));
  CHECK(b.stats(plain).max_health == 200);
  CHECK(b.stats(hard).max_health == 220);  // 200 + (5 - 1) * 5

  b.env.env().write_string(EnvScope::for_player(player_to_script(1)), "Statue of Amon-Ra",
                           "Researched");
  b.combat.recalc(plain);
  CHECK(b.stats(plain).max_health == 200);  // not exactly `researched`
  b.env.env().write_string(EnvScope::for_player(player_to_script(1)), "Statue of Amon-Ra",
                           "researched");
  CHECK(b.stats(plain).max_health == 200);  // nothing has rebuilt it yet
  b.combat.recalc(plain);
  b.combat.recalc(hard);
  CHECK(b.stats(plain).max_health == 220);
  CHECK(b.stats(hard).max_health == 240);
}

/// `Obj::range` (0x005adde0) reads `[obj+0xd4]` off any object: a combatant's
/// stored range, and for anything else the class's number its constructor
/// put there -- where a non-combatant used to answer 0.
TEST(stats_obj_range_answers_a_non_combatant_its_class_range) {
  Bench b;
  const ObjectId decor = b.world.spawn(NativeClass::decor, nullptr, b.post);
  const ObjectId man = b.spawn(b.soldier);
  REQUIRE(b.combat.find(decor) == nullptr);

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_combat_host(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index = registry.find(script::CallKind::member, "range", 0);
  REQUIRE(index != script::kUnresolvedHost);
  const auto range = [&](ObjectId id) {
    std::vector<script::Value> args{script::Value::object(kTypeObj, id)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "range";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  CHECK(range(decor) == 321);
  CHECK(range(man) == 100);
  CHECK(range(9999) == 0);
}

/// **A kill pays, a blow does not** (0x00511550, `docs/formats/unit-stats.md`,
/// "What a blow pays"): a sixth of the levels the victim stood above the
/// killer, truncated and kept to 0..8, plus one, into the running total
/// `[unit+0x180]` through `SetExperience` with the refresh, so the stored level
/// follows it -- for a unit no record had been made for too. Both levels are
/// of the running totals.
TEST(stats_a_kill_pays_by_the_level_gap_and_the_level_follows) {
  Bench b;
  const ObjectId killer = b.world.spawn(NativeClass::unit, nullptr, b.soldier);
  b.world.set_owner(killer, 1);
  b.world.set_position(killer, Point{1000, 1000});
  b.world.set_health(killer, 200);
  const ObjectId victim = b.spawn(b.soldier, 2);
  b.world.set_position(victim, Point{1040, 1000});
  (void)b.combat.reconcile(b.world);
  REQUIRE(b.heroes.unit(killer) == nullptr);  // nothing has met it yet
  // Level 13 against level 1: twelve levels, two sixths, so 3.
  REQUIRE(b.heroes.set_experience(b.world, victim, b.heroes.experience_for_level(13)));
  REQUIRE(b.kill(killer, victim));
  CHECK(b.heroes.experience(killer) == 3);
  const Combatant* a = b.combat.find(killer);
  REQUIRE(a != nullptr);
  CHECK(a->level == b.heroes.level_for_experience(3));
  CHECK(a->level > 1);

  // Five levels above is under a sixth: 1. (The killer stands at level 3.)
  const ObjectId lesser = b.spawn(b.soldier, 2);
  b.world.set_position(lesser, Point{1040, 1000});
  (void)b.combat.reconcile(b.world);
  REQUIRE(b.heroes.set_experience(b.world, lesser, b.heroes.experience_for_level(8)));
  REQUIRE(b.kill(killer, lesser));
  CHECK(b.heroes.experience(killer) == 4);

  // And far above is kept to 9.
  const ObjectId elder = b.spawn(b.soldier, 2);
  b.world.set_position(elder, Point{1040, 1000});
  (void)b.combat.reconcile(b.world);
  REQUIRE(b.heroes.set_experience(b.world, elder, b.heroes.experience_for_level(200)));
  REQUIRE(b.kill(killer, elder));
  CHECK(b.heroes.experience(killer) == 13);

  // A shot's kill pays its shooter the same way: the projectile's landing
  // (0x004d9bb0) is the blow too. Only a shooter can strike a Sentry here.
  const ObjectId slinger = b.spawn(b.slinger, 1);
  b.world.set_position(slinger, Point{3000, 3000});
  const ObjectId mark = b.spawn(b.soldier, 2);
  b.world.set_position(mark, Point{3200, 3000});
  (void)b.combat.reconcile(b.world);
  REQUIRE(b.combat.profile(b.slinger).shoots);
  REQUIRE(b.kill(slinger, mark));
  CHECK(b.heroes.experience(slinger) == 1);

  // But a Sentry's death pays nothing (0x005400c0).
  const ObjectId sentry = b.spawn(b.sentry, 2);
  b.world.set_position(sentry, Point{3200, 3000});
  (void)b.combat.reconcile(b.world);
  REQUIRE(b.kill(slinger, sentry));
  CHECK(b.heroes.experience(slinger) == 1);
}

/// The owner's modifier `[player+0x84]` (0x0050fec0), which the *Battle
/// tactics* research sets: none keeps the gain, a positive one multiplies it
/// by `m / 100 + 1` (one more with a chance of `m % 100`), and a negative one
/// loses it with a chance of `-m`.
TEST(stats_the_owners_modifier_scales_a_kills_gain) {
  Bench b;
  const ObjectId killer = b.spawn(b.soldier, 1);
  const ObjectId first = b.spawn(b.soldier, 2);
  b.world.set_position(killer, Point{1000, 1000});
  b.world.set_position(first, Point{1040, 1000});
  (void)b.combat.reconcile(b.world);
  REQUIRE(b.combat.player_experience_modifier(1) == 0);
  b.combat.set_player_experience_modifier(1, 200);  // three times, no chance
  REQUIRE(b.kill(killer, first));
  CHECK(b.heroes.experience(killer) == 3);

  b.combat.set_player_experience_modifier(1, -100);  // always lost
  const ObjectId second = b.spawn(b.soldier, 2);
  b.world.set_position(second, Point{1040, 1000});
  (void)b.combat.reconcile(b.world);
  REQUIRE(b.kill(killer, second));
  CHECK(b.heroes.experience(killer) == 3);
}

/// The same gain goes into the killer's hero's pool `[hero+0x2c4]`, and the
/// hero's tick (0x0052fc50) takes a point of experience per whole
/// `ExpFromArmyDivider` (3) of it and keeps the rest.
TEST(stats_a_kill_feeds_the_killers_hero_through_its_pool) {
  Bench b;
  const ObjectId chief = b.spawn(b.hero_class, 1);
  b.world.set_position(chief, Point{8000, 8000});
  const ObjectId man = b.spawn(b.soldier, 1);
  REQUIRE(b.heroes.attach(b.world, man, chief));
  b.world.set_position(man, Point{1000, 1000});
  const ObjectId victim = b.spawn(b.soldier, 2);
  b.world.set_position(victim, Point{1040, 1000});
  (void)b.combat.reconcile(b.world);
  REQUIRE(b.heroes.set_experience(b.world, victim, b.heroes.experience_for_level(25)));
  const std::int32_t before = b.heroes.experience(chief);
  REQUIRE(b.kill(man, victim));
  CHECK(b.heroes.experience(man) == 5);  // 24 levels: four sixths, plus one
  // Five in the pool; a tick takes one point and leaves two.
  for (int turn = 0; turn < 4; ++turn) b.world.advance(400);
  CHECK(b.heroes.experience(chief) == before + 1);
  CHECK(b.heroes.hero(chief)->experience_pool == 2);
}

/// `max_battle_level` (the *Standardization* researches: "cannot gain more
/// experience") holds a non-hero at its level's start: a total that would
/// pass it is set to it, and one already there stays. A hero is not held.
TEST(stats_max_battle_level_holds_a_warrior_but_not_a_hero) {
  Bench b;
  b.env.env().write_string(EnvScope::for_player(player_to_script(1)), "max_battle_level", "3");
  const std::int32_t cap = b.heroes.experience_for_level(3);
  REQUIRE(cap > 0);
  const ObjectId killer = b.spawn(b.soldier, 1);
  const ObjectId victim = b.spawn(b.soldier, 2);
  b.world.set_position(killer, Point{1000, 1000});
  b.world.set_position(victim, Point{1040, 1000});
  (void)b.combat.reconcile(b.world);
  REQUIRE(b.heroes.set_experience(b.world, victim, b.heroes.experience_for_level(200)));
  REQUIRE(b.kill(killer, victim));
  CHECK(b.heroes.experience(killer) == cap);

  const ObjectId chief = b.spawn(b.hero_class, 1);
  b.world.set_position(chief, Point{3000, 3000});
  const ObjectId other = b.spawn(b.soldier, 2);
  b.world.set_position(other, Point{3040, 3000});
  (void)b.combat.reconcile(b.world);
  REQUIRE(b.heroes.set_experience(b.world, other, b.heroes.experience_for_level(200)));
  const std::int32_t had = b.heroes.experience(chief);
  REQUIRE(b.kill(chief, other));
  CHECK(b.heroes.experience(chief) == had + 9);
}

/// The stored level, 0x005d9f0c..0x005da80f: the running total plus the
/// hero's army addend plus the items' experience, through the table; plus the
/// items' levels, the formation's `BonusLevel` and Battle Cry's levels (not
/// for a hero); at least 1; then Discipline's floor. The addend is the hero's
/// tick's (0x0052fc50, every 1,013 ms), from the hero's own stored level, and
/// only a changed addend rebuilds the army.
TEST(stats_the_stored_level_takes_the_addend_items_cry_and_discipline) {
  Bench b;
  const ObjectId chief = b.spawn(b.hero_class);
  const ObjectId man = b.spawn(b.soldier);
  REQUIRE(b.heroes.attach(b.world, man, chief));
  REQUIRE(b.heroes.set_experience(b.world, chief, b.heroes.experience_for_level(10)));
  CHECK(b.combat.find(chief)->level == 10);
  CHECK(b.combat.find(man)->level == 1);  // no tick yet: no addend

  // One tick: 40% of Exp[10], plus one.
  b.world.advance(1100);
  const std::int32_t addend = 40 * b.heroes.experience_for_level(10) / 100 + 1;
  CHECK(b.heroes.hero(chief)->army_addend == addend);
  const std::int32_t standing = b.heroes.level_for_experience(addend);
  CHECK(b.combat.find(man)->level == standing);
  CHECK(b.combat.find(chief)->level == 10);  // a hero is nobody's army

  // Battle Cry: two levels for the army while it lasts, none for the hero.
  REQUIRE(b.heroes.set_skill(b.world, chief, HeroSkill::battle_cry, 2));
  REQUIRE(b.heroes.use_skill(b.world, chief, HeroSkill::battle_cry));
  CHECK(b.combat.find(man)->level == standing + 2);
  CHECK(b.combat.find(chief)->level == 10);
  for (int turn = 0; turn < 15; ++turn) b.world.advance(400);
  CHECK(b.combat.find(man)->level == standing);

  // Discipline is a floor, not an addend.
  REQUIRE(b.heroes.set_skill(b.world, chief, HeroSkill::discipline, 10));
  CHECK(b.combat.find(man)->level == std::max(standing, 12));
  REQUIRE(b.heroes.set_skill(b.world, chief, HeroSkill::discipline, 0));
  CHECK(b.combat.find(man)->level == standing);

  // An item's level adds; its experience goes into the total first.
  ItemDefinition medal;
  medal.id = "Medal";
  medal.bonus.level = 1;
  medal.bonus.experience = 1000;
  (void)b.catalog.add(medal);
  REQUIRE(b.heroes.items().add(b.world, man, "Medal") != kNoObject);
  CHECK(b.combat.find(man)->level ==
        b.heroes.level_for_experience(addend + 1000) + 1);

  // And `Unit::level` answers the stored number.
  CHECK(b.heroes.level(b.world, man) == b.combat.find(man)->level);
}
