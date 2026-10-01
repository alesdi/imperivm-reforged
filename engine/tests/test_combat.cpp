// Combat, on synthetic data only.
//
// Not one of these cases reads a game file. The retail balance constants are
// compiled into `CombatConstants::shipped()` and the parsers are exercised
// against literal text, so the whole suite runs in a freestanding core with no
// filesystem beneath it -- which is the condition the conformance harness has
// to meet (docs/engine/architecture.md).
//
// The cases are grouped as the header is: the damage pipeline, the counter
// table, target acquisition, death, and determinism under partition.

#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/world/map.hpp"
#include "builder.hpp"
#include "test.hpp"

namespace {

using namespace imperivm::core;
using namespace imperivm::core::sim;

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Two made-up classes with the stats of a Hastatus and a Gaulish Swordsman,
/// written out by hand so that no test depends on a pack being present. The
/// real numbers are checked against the real class graph in the scratch
/// harness described in the report, not here.
constexpr ClassIndex kHastatus = 1;
constexpr ClassIndex kSwordsman = 2;
constexpr ClassIndex kArcher = 3;
constexpr ClassIndex kPeasant = 4;
constexpr ClassIndex kGate = 5;

CombatProfile melee(std::int32_t damage, std::int32_t armour, std::int32_t health) {
  CombatProfile p;
  p.damage = damage;
  p.damage_type = DamageType::slash;
  p.armor_slash = armour;
  p.armor_pierce = armour;
  p.max_health = health;
  p.range = 17;
  p.min_range = 2;
  p.radius = 15;
  p.selection_radius = 15;
  p.sight = 500;
  p.attack_interval = 1000;
  return p;
}

Combatant unit(ObjectId id, ClassIndex cls, PlayerId owner, std::int32_t health,
               Point position = {}) {
  Combatant c;
  c.id = id;
  c.class_index = cls;
  c.owner = owner;
  c.health = health;
  c.position = position;
  return c;
}

/// A system with the two melee profiles registered and no class graph.
CombatSystem make_system() {
  CombatSystem combat;
  combat.set_profile(kHastatus, melee(16, 12, 200));   // RHastatus, retail stats
  combat.set_profile(kSwordsman, melee(12, 6, 200));   // GSwordsman, retail stats
  return combat;
}

}  // namespace

// ---------------------------------------------------------------------------
// the damage pipeline
// ---------------------------------------------------------------------------

TEST(damage_subtracts_armour) {
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 16;
  in.armour = 6;
  const DamageBreakdown out = resolve_damage(in, k);
  CHECK(out.after_armour == 10);
  CHECK(out.final_damage == 10);
  CHECK(!out.floored_by_min_percent);
  CHECK(!out.floored_by_min_damage);
}

/// **A sheltered victim takes half, and the floor comes after.** 0x005110c6
/// halves after every other modifier and before the cap, so `MinDamage` lifts
/// a halved blow the way it lifts any other: 3 halved is 1, floored to 3.
TEST(a_sheltered_victim_takes_half_the_blow_ahead_of_the_floor) {
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 16;
  in.armour = 6;
  in.halved = true;
  DamageBreakdown out = resolve_damage(in, k);
  CHECK(out.after_armour == 10);
  CHECK(out.halved);
  CHECK(out.final_damage == 5);
  // Odd numbers truncate toward zero: 11 halved is 5.
  in.armour = 5;
  CHECK(resolve_damage(in, k).final_damage == 5);
  // The swordsman's 3 on a hastatus is floored to 3 whether halved or not.
  in.attack = 12;
  in.armour = 12;
  out = resolve_damage(in, k);
  CHECK(out.halved);
  CHECK(out.floored_by_min_damage);
  CHECK(out.final_damage == 3);
  in.halved = false;
  out = resolve_damage(in, k);
  CHECK(!out.halved);
  CHECK(out.final_damage == 3);
}

TEST(damage_floors_at_min_percent_of_the_attackers_damage) {
  // MinPercentOfAttackersDamage = 20. A Hastatus (16) against an Iberian
  // Defender's armour of 22 would otherwise deal -6.
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 16;
  in.armour = 22;
  const DamageBreakdown out = resolve_damage(in, k);
  CHECK(out.after_armour == 3);  // 16 * 20 / 100 = 3, truncating
  CHECK(out.floored_by_min_percent);
  CHECK(out.final_damage == 3);
}

TEST(damage_floors_at_min_damage) {
  // MinDamage = 3 is an absolute floor and outranks the percentage one for a
  // weak attacker: a Gaulish Archer's 6 damage gives a 20% floor of 1.
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 6;
  in.armour = 20;
  const DamageBreakdown out = resolve_damage(in, k);
  CHECK(out.after_armour == 1);
  CHECK(out.floored_by_min_percent);
  CHECK(out.final_damage == 3);
  CHECK(out.floored_by_min_damage);
}

TEST(damage_of_type_none_is_no_damage_at_all) {
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 50;
  in.type = DamageType::none;
  CHECK(resolve_damage(in, k).final_damage == 0);
}

TEST(siege_damage_meets_no_armour) {
  // There is no armor_siege property in the class graph, so a catapult's 120
  // lands whole on a Carthaginian Noble's armour of 20.
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 120;
  in.type = DamageType::siege;
  in.armour = 20;
  const DamageBreakdown out = resolve_damage(in, k);
  CHECK(out.armour == 0);
  CHECK(out.final_damage == 120);
}

TEST(penetration_ignores_armour) {
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 16;
  in.armour = 12;
  in.ignores_armour = true;
  CHECK(resolve_damage(in, k).final_damage == 16);
}

TEST(level_difference_uses_the_shipped_table) {
  const LevelDifferenceTable table = LevelDifferenceTable::shipped();
  CHECK(table.percent(0) == 0);
  CHECK(table.percent(1) == 5);
  CHECK(table.percent(5) == 20);
  CHECK(table.percent(10) == 40);
  CHECK(table.percent(20) == 60);
  CHECK(table.percent(40) == 80);
  CHECK(table.percent(100) == 90);
  CHECK(table.percent(1000) == 90);  // saturates
  CHECK(table.percent(-5) == 20);    // magnitude only
  // Interpolated between breakpoints: 3 sits two fifths of the way from
  // (1, 5) to (5, 20), so 5 + 15 * 2 / 4 = 12.
  CHECK(table.percent(3) == 12);
  CHECK(table.percent(15) == 50);
}

TEST(level_difference_table_parses_the_const_ini_spelling) {
  const LevelDifferenceTable table =
      LevelDifferenceTable::parse("1,5 5,20 10,40 20,60 40,80 100,90");
  CHECK(table.count == 6);
  CHECK(table == LevelDifferenceTable::shipped());
  // Garbage keeps the shipped table rather than silently disabling the term.
  CHECK(LevelDifferenceTable::parse("") == LevelDifferenceTable::shipped());
}

TEST(a_level_advantage_raises_damage_and_a_deficit_lowers_it) {
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 20;
  in.armour = 0;

  in.attacker_level = 6;
  in.defender_level = 1;  // difference 5 -> 20%
  const DamageBreakdown up = resolve_damage(in, k);
  CHECK(up.level_difference == 5);
  CHECK(up.level_percent == 20);
  CHECK(up.final_damage == 24);

  in.attacker_level = 1;
  in.defender_level = 6;
  const DamageBreakdown down = resolve_damage(in, k);
  CHECK(down.level_difference == -5);
  CHECK(down.final_damage == 16);
}

TEST(charge_multiplies_after_armour) {
  // UNIT_SPECIALS.INI: "deals 8 times his normal damage (after enemy armor is
  // applied)". 16 - 6 = 10, times 8.
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 16;
  in.armour = 6;
  in.multiplier_percent = 100 * k.charge_damage_factor;
  CHECK(resolve_damage(in, k).final_damage == 80);
}

TEST(settlement_damage_is_amplified) {
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 16;
  in.armour = 6;
  in.in_settlement = true;
  CHECK(resolve_damage(in, k).final_damage == 20);  // 10 * 200 / 100
}

TEST(damage_is_never_negative_and_never_overflows) {
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 0;
  CHECK(resolve_damage(in, k).final_damage == 0);

  in.attack = -50;
  CHECK(resolve_damage(in, k).final_damage == 0);

  in.attack = 2000000000;
  in.multiplier_percent = 800;
  CHECK(resolve_damage(in, k).final_damage > 0);  // saturates, does not wrap
}

TEST(const_ini_parses_the_gameplay_section_only) {
  static constexpr std::string_view ini =
      "[VXTIME]\n"
      "MinDamage = 999\n"
      "[GamePlay]\n"
      ";difficulty level adjustments\n"
      "EasyDifficultyLevelAddend = 10\n"
      "NormalDifficultyLevelAddend = 5\n"
      "HardDifficultyLevelAddend = 0\n"
      "MinDamage = 3\n"
      "MinPercentOfAttackersDamage = 20\n"
      "LevelExpDivider = 8 ; exp_gain = target_level / LevelExpDivider + 1;\n"
      "LevelDifferenceDamageTable = 1,5 5,20 10,40 20,60 40,80 100,90\n"
      "DamageAmplify = 200\n"
      "[Scroll]\n"
      "MinDamage = 111\n";
  const CombatConstants k = CombatConstants::from_const_ini(bytes_of(ini));
  CHECK(k.min_damage == 3);  // not the 999 above the section, nor the 111 below
  CHECK(k.min_percent_of_attackers_damage == 20);
  CHECK(k.damage_amplify == 200);
  CHECK(k.level_exp_divider == 8);  // the inline comment is stripped
  CHECK(k.level_difference == LevelDifferenceTable::shipped());
  CHECK(k.difficulty_addend(Difficulty::easy) == 10);
  CHECK(k.difficulty_addend(Difficulty::normal) == 5);
  CHECK(k.difficulty_addend(Difficulty::hard) == 0);
}

TEST(const_ini_keeps_shipped_values_for_absent_keys) {
  const CombatConstants k = CombatConstants::from_const_ini(bytes_of("[GamePlay]\n"));
  CHECK(k.min_damage == 3);
  CHECK(k.charge_damage_factor == 8);
  const CombatConstants empty = CombatConstants::from_const_ini({});
  CHECK(empty.min_damage == 3);
}

TEST(experience_follows_the_const_ini_recurrence) {
  const CombatConstants k = CombatConstants::shipped();
  // Exp[level] = Exp[level-1] + Pos(level - 1) * 50 + 125
  CHECK(k.experience_for_level(1) == 0);
  CHECK(k.experience_for_level(2) == 175);
  CHECK(k.experience_for_level(3) == 400);
  CHECK(k.experience_for_level(4) == 675);
  CHECK(k.level_for_experience(0) == 1);
  CHECK(k.level_for_experience(174) == 1);
  CHECK(k.level_for_experience(175) == 2);
  CHECK(k.level_for_experience(400) == 3);
  // exp_gain = target_level / 8 + 1
  CHECK(k.experience_gain(0) == 1);
  CHECK(k.experience_gain(8) == 2);
  CHECK(k.experience_gain(24) == 4);
}

// ---------------------------------------------------------------------------
// the counter-unit table
// ---------------------------------------------------------------------------

TEST(counter_table_reads_the_shipped_xml_shape) {
  // Three synthetic classes, so the load path is exercised end to end without
  // a pack: the document shape is the retail one and the coefficients are two
  // real rows from DATA\COUNTERUNITS.XML.
  ClassGraph graph;
  static constexpr std::string_view a = "<class id=\"RHastatus\" cpp_class=\"CVXUnit\"/>";
  static constexpr std::string_view b = "<class id=\"GSwordsman\" cpp_class=\"CVXUnit\"/>";
  static constexpr std::string_view c = "<class id=\"GSpearman\" cpp_class=\"CVXUnit\"/>";
  CHECK(graph.add(bytes_of(a), "a").ok());
  CHECK(graph.add(bytes_of(b), "b").ok());
  CHECK(graph.add(bytes_of(c), "c").ok());
  graph.link();

  static constexpr std::string_view xml =
      "<counterunits>\n"
      "  <unit class=\"RHastatus\">\n"
      "    <counter class=\"GSwordsman\" coeff=\"33\"/>\n"
      "    <counter class=\"GSpearman\" coeff=\"63\"/>\n"
      "  </unit>\n"
      "  <unit class=\"GSwordsman\">\n"
      "    <counter class=\"NoSuchClass\" coeff=\"50\"/>\n"
      "  </unit>\n"
      "</counterunits>\n";
  CounterTable table;
  CHECK(table.load(bytes_of(xml), graph).ok());
  CHECK(table.size() == 2);
  CHECK(table.unresolved() == 1);  // a dangling name is dropped, not fatal

  const ClassIndex hastatus = graph.find("RHastatus");
  const ClassIndex swordsman = graph.find("GSwordsman");
  const ClassIndex spearman = graph.find("GSpearman");
  // The listed class counters the enclosing one, and the relation is directed.
  CHECK(table.strength(swordsman, hastatus) == 33);
  CHECK(table.strength(spearman, hastatus) == 63);
  CHECK(table.strength(hastatus, swordsman) == 0);

  // A document that is not this one is refused rather than half-read.
  CounterTable other;
  CHECK(!other.load(bytes_of("<somethingelse/>"), graph).ok());
}

TEST(counter_table_add_overwrites_and_stays_ordered) {
  CounterTable table;
  table.add(3, 1, 33);
  table.add(2, 1, 64);
  table.add(3, 2, 98);
  table.add(2, 1, 70);  // same pair again
  CHECK(table.size() == 3);
  CHECK(table.strength(2, 1) == 70);
  CHECK(table.strength(3, 1) == 33);
  CHECK(table.strength(3, 2) == 98);
  CHECK(table.strength(1, 3) == 0);
  table.clear();
  CHECK(table.empty());
}

TEST(counter_mode_off_leaves_damage_alone) {
  CounterTable table;
  table.add(kSwordsman, kHastatus, 64);
  CHECK(table.damage_percent(kSwordsman, kHastatus, CounterMode::off) == 100);
}

TEST(counter_bonus_mode_helps_the_counter_unit) {
  CounterTable table;
  table.add(kSwordsman, kHastatus, 64);  // GSwordsman counters RHastatus at 64
  CHECK(table.damage_percent(kSwordsman, kHastatus, CounterMode::counter_bonus) == 164);
  // and does nothing in the other direction
  CHECK(table.damage_percent(kHastatus, kSwordsman, CounterMode::counter_bonus) == 100);
}

TEST(countered_penalty_mode_is_the_other_reading) {
  CounterTable table;
  table.add(kSwordsman, kHastatus, 64);
  // Under this reading the Hastatus, being countered, hits for 64%.
  CHECK(table.damage_percent(kHastatus, kSwordsman, CounterMode::countered_penalty) == 64);
  CHECK(table.damage_percent(kSwordsman, kHastatus, CounterMode::countered_penalty) == 100);
}

TEST(the_counter_multiplier_lands_between_armour_and_level) {
  const CombatConstants k = CombatConstants::shipped();
  DamageInputs in;
  in.attack = 16;
  in.armour = 6;             // after armour: 10
  in.counter_percent = 164;  // -> 16
  in.attacker_level = 6;
  in.defender_level = 1;     // +20% -> 19
  const DamageBreakdown out = resolve_damage(in, k);
  CHECK(out.after_armour == 10);
  CHECK(out.after_counter == 16);
  CHECK(out.after_level == 19);
  CHECK(out.final_damage == 19);
}

TEST(a_counter_relationship_changes_who_wins) {
  // Two identical units, one of which counters the other. Hits to kill must
  // differ in the direction the table says.
  CombatSystem combat;
  combat.set_profile(kHastatus, melee(20, 0, 200));
  combat.set_profile(kSwordsman, melee(20, 0, 200));
  CounterTable table;
  table.add(kSwordsman, kHastatus, 100);  // swordsman counters hastatus, hard
  combat.set_counter_table(table);
  combat.set_counter_mode(CounterMode::counter_bonus);

  combat.add(unit(1, kHastatus, 0, 200));
  combat.add(unit(2, kSwordsman, 1, 200));
  CHECK(combat.preview(1, 2).final_damage == 20);   // no relationship
  CHECK(combat.preview(2, 1).final_damage == 40);   // doubled by the table

  combat.set_counter_mode(CounterMode::off);
  CHECK(combat.preview(2, 1).final_damage == 20);
}

// ---------------------------------------------------------------------------
// target acquisition
// ---------------------------------------------------------------------------

TEST(the_nearest_enemy_wins_when_nothing_else_differs) {
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{300, 0}));
  combat.add(unit(3, kSwordsman, 1, 200, Point{100, 0}));
  CHECK(combat.best_target(1) == 3);
}

TEST(target_factor_makes_a_target_less_attractive) {
  // Peaceful sets target_factor = 1000; its own child BaseMage overrides it
  // back to 100. A peasant twice as close as a soldier is still not picked.
  CombatSystem combat = make_system();
  CombatProfile peasant = melee(0, 0, 100);
  peasant.target_factor = 1000;
  combat.set_profile(kPeasant, peasant);

  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kPeasant, 1, 100, Point{100, 0}));
  combat.add(unit(3, kSwordsman, 1, 200, Point{200, 0}));
  CHECK(combat.best_target(1) == 3);
}

TEST(target_priority_makes_a_target_more_attractive) {
  // Gate declares target_priority = 200 against Building's 100.
  CombatSystem combat = make_system();
  CombatProfile gate = melee(0, 0, 5000);
  gate.target_priority = 200;
  gate.radius = 15;
  combat.set_profile(kGate, gate);

  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{130, 0}));
  combat.add(unit(3, kGate, 1, 5000, Point{160, 0}));
  // 130^2 * 100 / 100 = 16,900 against 160^2 * 100 / 200 = 12,800.
  CHECK(combat.best_target(1) == 3);
}

TEST(a_target_factor_of_minus_one_is_never_targeted) {
  CombatSystem combat = make_system();
  CombatProfile deer = melee(0, 0, 100);
  deer.target_factor = -1;  // Crow, Deer, Eagle, Fish, Hen
  combat.set_profile(kPeasant, deer);
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kPeasant, 1, 100, Point{10, 0}));
  CHECK(combat.best_target(1) == sim::kNoObject);
}

TEST(allies_and_unowned_objects_are_not_targets) {
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 0, 200, Point{50, 0}));   // same owner
  combat.add(unit(3, kSwordsman, sim::kNoPlayer, 200, Point{60, 0}));
  CHECK(combat.best_target(1) == sim::kNoObject);

  combat.add(unit(4, kSwordsman, 1, 200, Point{70, 0}));
  CHECK(combat.best_target(1) == 4);
  combat.set_allied(0, 1, true);
  CHECK(combat.best_target(1) == sim::kNoObject);
  combat.set_allied(0, 1, false);
  CHECK(combat.best_target(1) == 4);
}

TEST(targets_beyond_sight_are_not_acquired) {
  CombatSystem combat = make_system();  // sight 500
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{501, 0}));
  CHECK(combat.best_target(1) == sim::kNoObject);
  combat.set_position(2, Point{500, 0});
  CHECK(combat.best_target(1) == 2);
}

TEST(range_is_measured_between_surfaces_and_squared) {
  CombatSystem combat = make_system();  // range 17, radius 15 both sides
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{47, 0}));  // 17 + 15 + 15
  CHECK(combat.in_attack_range(*combat.find(1), *combat.find(2)));
  combat.set_position(2, Point{48, 0});
  CHECK(!combat.in_attack_range(*combat.find(1), *combat.find(2)));
}

TEST(min_range_is_a_dead_zone) {
  // A catapult declares min_range 301 and range 800.
  CombatSystem combat = make_system();
  CombatProfile catapult = melee(120, 0, 1000);
  catapult.damage_type = DamageType::siege;
  catapult.range = 800;
  catapult.min_range = 301;
  catapult.radius = 50;
  combat.set_profile(kArcher, catapult);

  combat.add(unit(1, kArcher, 0, 1000, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{300, 0}));
  CHECK(!combat.in_attack_range(*combat.find(1), *combat.find(2)));
  combat.set_position(2, Point{400, 0});
  CHECK(combat.in_attack_range(*combat.find(1), *combat.find(2)));
  combat.set_position(2, Point{900, 0});
  CHECK(!combat.in_attack_range(*combat.find(1), *combat.find(2)));
}

TEST(ties_in_score_keep_the_lower_object_id) {
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(7, kSwordsman, 1, 200, Point{100, 0}));
  combat.add(unit(3, kSwordsman, 1, 200, Point{0, 100}));
  CHECK(combat.best_target(1) == 3);
}

// ---------------------------------------------------------------------------
// the target-selection family
//
// Every case below pairs the filtered answer with the *unfiltered* answer over
// the same board, and the two differ in every one of them. A filter that did
// nothing at all would pass a test that only asserted the filtered answer; the
// control is what makes each of these say something. Each was also run once
// with its filter deliberately disabled in `best_target`, and each failed.
// ---------------------------------------------------------------------------

TEST(mis_zero_damage_skips_candidates_that_deal_no_damage) {
  // `BestTargetInSquadSightMisZeroDamage`: `0x005db6a5` rejects a candidate
  // whose own `damage` is not positive. A wagon standing between an idle unit
  // and a swordsman is not what the idle unit walks off to fight.
  CombatSystem combat = make_system();
  combat.set_profile(kPeasant, melee(0, 0, 100));  // Peaceful: damage = 0

  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kPeasant, 1, 100, Point{100, 0}));
  combat.add(unit(3, kSwordsman, 1, 200, Point{200, 0}));

  // Control: without the filter the harmless one is nearer and therefore wins.
  CHECK(combat.best_target(1) == 2);

  TargetFilter armed;
  armed.require_armed = true;
  CHECK(combat.best_target(1, armed) == 3);

  // Measured against `Obj::attack`, which is `damage + attack_bonus`: a
  // peasant a hero has buffed into something that can hit back is a target.
  combat.find(2)->attack_bonus = 5;
  CHECK(combat.best_target(1, armed) == 2);
  combat.find(2)->attack_bonus = 0;

  // Nothing armed in sight is no target at all, not the nearest harmless one.
  combat.remove(3);
  CHECK(combat.best_target(1) == 2);
  CHECK(combat.best_target(1, armed) == sim::kNoObject);
}

TEST(no_independent_skips_player_fourteen_and_only_player_fourteen) {
  // `BestNoIndependentTargetInSquadSight`: `0x005d45b9` ands the attacker's
  // enemy-player mask with 0xffffbfff, which clears bit 14 and nothing else.
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, sim::kNeutralWildlife, 200, Point{100, 0}));
  combat.add(unit(3, kSwordsman, sim::kNeutralPassive, 200, Point{200, 0}));
  combat.add(unit(4, kSwordsman, 1, 200, Point{300, 0}));

  CHECK(combat.best_target(1) == 2);  // control: the nearest, wildlife or not

  TargetFilter no_independents;
  no_independents.exclude_independents = true;
  // 15 is *not* cleared by that mask, so the passive neutral is still a target
  // and the real player behind it is not reached.
  CHECK(combat.best_target(1, no_independents) == 3);
  combat.remove(3);
  CHECK(combat.best_target(1, no_independents) == 4);
}

TEST(exclusive_takes_the_named_class_and_no_other) {
  // `BestTargetInSquadSightExclusive`: `0x005db774` rejects outright, where the
  // plain `/1` form only penalises. Exact class -- the executable compares the
  // class object's own name, so this is an index equality and not an ancestry
  // walk.
  CombatSystem combat = make_system();
  combat.set_profile(kArcher, melee(8, 4, 120));

  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kArcher, 1, 120, Point{100, 0}));
  combat.add(unit(3, kSwordsman, 1, 200, Point{200, 0}));

  CHECK(combat.best_target(1) == 2);  // control: the archer is nearer

  TargetFilter only_swordsmen;
  only_swordsmen.only_class = kSwordsman;
  CHECK(combat.best_target(1, only_swordsmen) == 3);

  combat.remove(3);
  CHECK(combat.best_target(1, only_swordsmen) == sim::kNoObject);
}

/// The plain `/1` form **prefers** where `Exclusive` **requires**, and the two
/// differ by one argument to 0x005dc950.
///
/// With the exclusive flag clear, 0x005db774 adds 200 to a mismatch instead of
/// rejecting it. `TargetFilter::prefer_class` records why that 200 is taken to
/// its limit here rather than scaled -- and this test is the pair of cases the
/// limit gets right, plus the one it does not.
TEST(prefer_class_outranks_the_score_and_falls_back_when_nothing_matches) {
  CombatSystem combat = make_system();
  combat.set_profile(kArcher, melee(8, 4, 120));

  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kArcher, 1, 120, Point{100, 0}));
  combat.add(unit(3, kSwordsman, 1, 200, Point{200, 0}));

  CHECK(combat.best_target(1) == 2);  // control: the archer is nearer

  TargetFilter prefer_swordsmen;
  prefer_swordsmen.prefer_class = kSwordsman;
  CHECK(combat.best_target(1, prefer_swordsmen) == 3);

  // **And it is a preference, not a filter.** Take the swordsman away and the
  // archer is the answer again -- where `Exclusive` above answers nothing.
  combat.remove(3);
  CHECK(combat.best_target(1, prefer_swordsmen) == 2);

  // Within the preferred class the ordinary score still orders, and the earlier
  // id keeps a tie: the first swordsman met must not be displaced by a second
  // one standing the same distance away.
  combat.add(unit(4, kSwordsman, 1, 200, Point{300, 0}));
  combat.add(unit(5, kSwordsman, 1, 200, Point{-300, 0}));
  CHECK(combat.best_target(1, prefer_swordsmen) == 4);
  combat.remove(4);
  CHECK(combat.best_target(1, prefer_swordsmen) == 5);

  // A preference for a class nobody has is no preference at all: every
  // candidate takes the same penalty in the original, so the ranking is the
  // unfiltered one.
  TargetFilter prefer_absent;
  prefer_absent.prefer_class = kPeasant;
  CHECK(combat.best_target(1, prefer_absent) == combat.best_target(1));
}

TEST(for_pos_takes_only_what_the_attacker_could_already_strike) {
  // `BestTargetForPos`: `0x005dd280` sets the in-range flag, which turns on the
  // `[min_range, range]` test at `0x005db820`. Reach here is 17 + 15 + 15 = 47.
  CombatSystem combat = make_system();
  CombatProfile awkward = melee(12, 6, 200);
  awkward.target_factor = 1000;  // less attractive, so the control differs
  combat.set_profile(kPeasant, awkward);

  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kPeasant, 1, 200, Point{40, 0}));    // in reach, dull
  combat.add(unit(3, kSwordsman, 1, 200, Point{60, 0}));  // out of reach

  // Control: 1601 * 1000 / 100 = 16,010 against 3601 * 100 / 100 = 3,601, so
  // the ordinary acquisition walks past the one it could already hit.
  CHECK(combat.best_target(1) == 3);

  TargetFilter here;
  here.require_in_attack_range = true;
  CHECK(combat.best_target(1, here) == 2);

  // Nothing within reach is no target, even with a candidate well inside sight.
  combat.remove(2);
  CHECK(combat.best_target(1) == 3);
  CHECK(combat.best_target(1, here) == sim::kNoObject);
}

TEST(in_range_sweeps_around_the_point_but_still_scores_from_the_attacker) {
  // `BestTargetInRange(pt, r)`: `0x005dd060` passes `pt` as the grid-sweep
  // centre and the attacker's own position as the predicate's origin, so the
  // circle and the score measure from different places.
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));  // sight 500
  combat.add(unit(2, kSwordsman, 1, 200, Point{100, 0}));
  combat.add(unit(3, kSwordsman, 1, 200, Point{1000, 0}));
  combat.add(unit(4, kSwordsman, 1, 200, Point{700, 0}));
  combat.add(unit(5, kSwordsman, 1, 200, Point{1450, 0}));  // 450 from the centre

  // Control: the attacker's own sight of 500 reaches only unit 2.
  CHECK(combat.best_target(1) == 2);

  TargetFilter around;
  around.centre_on_point = true;
  around.centre = Point{1000, 0};
  around.radius = 400;
  // 2 is 900 from the centre and outside the circle. 3 sits on the centre and 4
  // is 300 from it, so a search that scored from the *centre* would answer 3 --
  // it answers 4, because 4 is the nearer of the two to the attacker.
  CHECK(combat.best_target(1, around) == 4);

  // A radius of zero or less is the attacker's sight, still around the point.
  TargetFilter defaulted;
  defaulted.centre_on_point = true;
  defaulted.centre = Point{1000, 0};
  defaulted.radius = 0;
  CHECK(combat.best_target(1, defaulted) == 4);  // 700 is within 500 of 1000
  combat.remove(4);
  CHECK(combat.best_target(1, defaulted) == 3);
  combat.remove(3);
  // Unit 5 is 450 from the centre: inside the 500 the fallback substitutes and
  // outside the 400 the caller asked for. This is the pair that says the given
  // radius is used at all rather than always deferring to sight.
  CHECK(combat.best_target(1, defaulted) == 5);
  CHECK(combat.best_target(1, around) == sim::kNoObject);
  combat.remove(5);
  CHECK(combat.best_target(1, defaulted) == sim::kNoObject);  // 2 is 900 away
}

TEST(prefer_undiseased_is_the_plain_acquisition_while_no_disease_exists) {
  // `BestTargetInSquadSight_PreferUndiseased` adds 200 to a diseased
  // candidate's score at `0x005db6e0` -- a penalty, not an exclusion. Nothing
  // in this engine sets disease and `Combatant` has no field for it, so no
  // candidate can carry the penalty and the two entry points agree exactly.
  // This case exists so that adding a disease state without revisiting the
  // filter is caught here rather than in a battle.
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{300, 0}));
  combat.add(unit(3, kSwordsman, 1, 200, Point{100, 0}));
  CHECK(combat.best_target(1, TargetFilter{}) == combat.best_target(1));
  CHECK(combat.best_target(1, TargetFilter{}) == 3);
}

// ---------------------------------------------------------------------------
// a fight, death, and removal
// ---------------------------------------------------------------------------

TEST(a_fight_runs_to_a_death_and_the_body_is_removed) {
  World world;
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  CHECK(combat.order_attack(1, 2));

  // Hastatus 16 against armour 6 is 10 a hit; the swordsman's 12 against
  // armour 12 falls to the 20% floor of 2 -- but MinDamage lifts it to 3.
  CHECK(combat.preview(1, 2).final_damage == 10);
  CHECK(combat.preview(2, 1).final_damage == 3);

  int hastatus_strikes = 0;
  bool hastatus_alive = true;
  bool swordsman_alive = true;
  for (int turn = 0; turn < 200 && (hastatus_alive && swordsman_alive); ++turn) {
    world.advance(400);
    combat.advance(world, world.turn());
    for (const CombatEvent& event : combat.events()) {
      if (event.kind == CombatEvent::Kind::strike && event.attacker == 1) ++hastatus_strikes;
    }
    hastatus_alive = combat.find(1) != nullptr;
    swordsman_alive = combat.find(2) != nullptr;
  }
  // 200 health at 10 a hit is 20 hits; the swordsman would need 67 at 3 a hit.
  CHECK(!swordsman_alive);
  CHECK(hastatus_alive);
  CHECK(hastatus_strikes == 20);
  const Combatant* winner = combat.find(1);
  CHECK(winner != nullptr);
  if (winner != nullptr) {
    CHECK(winner->health == 200 - 19 * 3);    // it took 19 hits back
    CHECK(winner->target == sim::kNoObject);  // the target handle is dropped
    CHECK(winner->attacks == 0);              // and the counter with it
  }
}

/// Playtest #19: a siege engine its crew has not assembled does not fight.
/// An engine fires through `Catapult::Attack`, which `CATAPULT_IDLE.VS` sets
/// going (`autofire`) only after `SetBuilt`; at one point of health, with its
/// builders still on their way, it used to shoot at whatever came into range.
/// Built, the same engine fights, and an ordinary unit never asks.
TEST(an_unbuilt_siege_engine_takes_no_part_in_the_fight) {
  World world;
  const ObjectId engine = world.spawn(NativeClass::catapult, nullptr);
  const ObjectId enemy = world.spawn(NativeClass::unit, nullptr);
  const ObjectId soldier = world.spawn(NativeClass::unit, nullptr);
  CombatSystem combat = make_system();
  // Its health is the profile's here, so that the swordsman's blows back
  // cannot end the test early.
  combat.add(unit(engine, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(enemy, kSwordsman, 1, 200, Point{40, 0}));
  combat.add(unit(soldier, kHastatus, 0, 200, Point{-40, 0}));
  const auto strikes_by = [&](ObjectId attacker, int turns) {
    int strikes = 0;
    for (int turn = 0; turn < turns; ++turn) {
      world.advance(400);
      combat.advance(world, world.turn());
      for (const CombatEvent& event : combat.events()) {
        if (event.kind == CombatEvent::Kind::strike && event.attacker == attacker) ++strikes;
      }
    }
    return strikes;
  };
  CHECK(strikes_by(engine, 10) == 0);
  REQUIRE(combat.find(engine) != nullptr);
  CHECK(combat.find(engine)->target == kNoObject);
  // The gate is the engine's: a unit beside it, whose `built` is never set,
  // fought all along.
  REQUIRE(combat.find(soldier) != nullptr);
  CHECK(combat.find(soldier)->target == enemy);
  world.find(engine)->state.flags.built = true;
  CHECK(strikes_by(engine, 10) > 0);
}

/// **The flag is read off the world by the blow, not by the preview.**
/// `ObjectFlags::half_damage` is world state; `hit` reads it where the world
/// is in hand, and `preview`, which has none, shows the unsheltered number.
TEST(a_sheltered_world_object_takes_half_of_every_landed_blow) {
  World world;
  CombatSystem combat = make_system();
  combat.set_world_bound(true);
  const ObjectId a = world.spawn(NativeClass::unit, nullptr, kHastatus);
  const ObjectId b = world.spawn(NativeClass::unit, nullptr, kSwordsman);
  world.mutable_state(a)->owner = 0;
  world.set_position(a, Point{0, 0});
  world.mutable_state(a)->health = 200;
  world.mutable_state(b)->owner = 1;
  world.set_position(b, Point{40, 0});
  world.mutable_state(b)->health = 200;
  combat.add(unit(a, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(b, kSwordsman, 1, 200, Point{40, 0}));
  world.mutable_state(b)->flags.half_damage = true;
  CHECK(combat.order_attack(a, b));
  CHECK(combat.preview(a, b).final_damage == 10);

  const auto strike_on = [&](ObjectId victim) {
    for (int turn = 0; turn < 20; ++turn) {
      world.advance(400);
      combat.advance(world, world.turn());
      for (const CombatEvent& event : combat.events()) {
        if (event.kind == CombatEvent::Kind::strike && event.defender == victim) {
          return event.damage;
        }
      }
      combat.clear_events();
    }
    return -1;
  };
  CHECK(strike_on(b) == 5);
  combat.clear_events();
  world.mutable_state(b)->flags.half_damage = false;
  CHECK(strike_on(b) == 10);
}

// ---------------------------------------------------------------------------
// what a fight shows
// ---------------------------------------------------------------------------

namespace {

/// A unit's four fighting slots, written from docs/formats/ent-xml.md: slot 5
/// the swing (state 2 to 2), slot 9 the death, slot 18 `toidle` (2 to 1).
/// Eight facing columns. The death's holds grow towards the end, the shape
/// the shipped strips have -- the corpse lies there for most of it.
constexpr std::string_view kFighterEntity = R"(<?xml version="1.0"?>
<entity name="fighter" type="vx/unit" variations="8">
  <images>
    <image idx="1" file="idle.rle" drawmode="player_color" remaping="none" rows="4" columns="8"/>
    <image idx="2" file="fight.rle" drawmode="player_color" remaping="none" rows="4" columns="8"/>
    <image idx="3" file="death.rle" drawmode="player_color" remaping="none" rows="4" columns="8"/>
    <image idx="4" file="turn.rle" drawmode="player_color" remaping="none" rows="3" columns="8"/>
  </images>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="1" anim_idx="65536" anim_frame="65536"/>
    <state idx="2" name="fight" image_idx="1" image_row="0" anim_idx="65536" anim_frame="65536"/>
  </states>
  <anims>
    <anim idx="5" name="attack" startstate="2" endstate="2" frames="6" duration="1000"
          default_duration="0" action_time="500" step="0">
      <replace layer="1" image="2"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="250"/>
      <frame idx="3" duration="250"/>
      <frame idx="4" duration="250"/>
      <frame idx="5" duration="250"/>
      <frame idx="6" duration="0"/>
    </anim>
    <anim idx="9" name="die" startstate="1" endstate="1" frames="6" duration="3000"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="3"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="400"/>
      <frame idx="4" duration="1000"/>
      <frame idx="5" duration="1500"/>
      <frame idx="6" duration="0"/>
    </anim>
    <anim idx="18" name="toidle" startstate="2" endstate="1" frames="5" duration="300"
          default_duration="0" action_time="0" step="0">
      <replace layer="1" image="4"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="100"/>
      <frame idx="3" duration="100"/>
      <frame idx="4" duration="100"/>
      <frame idx="5" duration="0"/>
    </anim>
  </anims>
</entity>)";

/// One turn of both: the world (and whatever systems it holds), then combat.
void fight_turn(World& world, CombatSystem& combat) {
  world.advance(400);
  combat.advance(world, world.turn());
}

/// Place a world object and its combatant at the same spot.
ObjectId place(World& world, CombatSystem& combat, const Entity* entity, ClassIndex cls,
               PlayerId owner, std::int32_t health, Point at) {
  const ObjectId id = world.spawn(NativeClass::unit, entity, cls);
  world.mutable_state(id)->owner = owner;
  world.set_position(id, at);
  world.mutable_state(id)->health = health;
  combat.add(unit(id, cls, owner, health, at));
  return id;
}

}  // namespace

/// **A blow is seen: the attacker turns to its target and swings.** Combat
/// used to write its own `Combatant::anim` and nothing the renderer reads, so
/// an engaged unit froze in whatever pose it arrived in, facing wherever it
/// had last walked.
///
/// The dumps say what an engaged unit shows: all 92 `action=2` blocks print
/// `Anim=4`, the attack slot less one, and 88 of the 92 whose target resolves
/// carry `target - own position` as their vector exactly.
TEST(a_blow_turns_the_attacker_to_its_target_and_plays_one_swing) {
  const auto fighter = Entity::parse(bytes_of(kFighterEntity));
  REQUIRE(fighter.ok());
  World world;
  CombatSystem combat = make_system();
  combat.set_world_bound(true);
  // The target stands due west; the attacker was placed facing column 0.
  const ObjectId a = place(world, combat, &fighter.value(), kHastatus, 0, 200, Point{540, 500});
  const ObjectId b = place(world, combat, &fighter.value(), kSwordsman, 1, 200, Point{500, 500});
  const std::uint32_t west = facing_column(Point{-40, 0}, 8);
  REQUIRE(west != 0u);
  const WorldObject* slot = world.find(a);
  REQUIRE(slot != nullptr && slot->object != nullptr);
  CHECK(slot->object->anim.variation == 0u);

  CHECK(combat.order_attack(a, b));
  fight_turn(world, combat);
  REQUIRE(combat.health(b) < 200);  // the blow landed this turn
  CHECK(slot->object->anim.variation == west);
  CHECK(slot->object->anim.anim_slot == kAnimAttack);
  CHECK(slot->animating);
  CHECK(slot->repeat == AnimRepeat::hold);

  // **One swing per blow.** Held, not looped: once the cycle is over the
  // unit stands in its last frame until the next blow restarts it, and the
  // restart is what keeps swing and blow together.
  std::int32_t blows = 1;
  bool restarted = false;
  for (int turn = 0; turn < 6; ++turn) {
    const std::int32_t before = combat.health(b);
    fight_turn(world, combat);
    if (combat.health(b) < before) {
      ++blows;
      restarted = restarted || slot->object->anim.elapsed_ms < 400;
    }
  }
  CHECK(blows >= 3);
  CHECK(restarted);
  CHECK(slot->object->anim.anim_slot == kAnimAttack);
}

/// The attacker's facing goes where movement keeps it when movement tracks
/// the unit, so the next walk -- and a hero's formation, which is laid off
/// the leader's heading -- starts from where the fight left it.
TEST(a_tracked_attacker_turns_through_movement) {
  const auto fighter = Entity::parse(bytes_of(kFighterEntity));
  REQUIRE(fighter.ok());
  World world;
  MovementSystem movement;
  REQUIRE(world.add_system(&movement));
  movement.set_grid(ObstructionGrid(64, 64));
  CombatSystem combat = make_system();
  combat.set_world_bound(true);
  const ObjectId a = place(world, combat, &fighter.value(), kHastatus, 0, 200, Point{500, 540});
  const ObjectId b = place(world, combat, &fighter.value(), kSwordsman, 1, 200, Point{500, 500});
  movement.state(a).speed = 100;
  // A record that last faced east.
  movement.face(world, a, Point{900, 540});
  const Point east = movement.find(a)->facing;

  CHECK(combat.order_attack(a, b));
  fight_turn(world, combat);
  REQUIRE(combat.health(b) < 200);
  const Point faced = movement.find(a)->facing;
  CHECK(!(faced == east));
  CHECK(faced.x == 0);
  CHECK(faced.y < 0);  // towards the target, which is at lower y
  CHECK(world.find(a)->object->anim.variation == facing_column(faced, 8));
  // And no record is made up for a unit movement never had: the defender
  // has not swung, and the attacker's swing did not give it one.
  CHECK(movement.find(b) == nullptr);
}

/// **A unit that stops fighting goes back to idle through `toidle`**, and
/// only once its swing has run out: slot 18 is the 2-to-1 edge of the
/// entity's own state machine. Here the target dies, so the next look finds
/// nobody.
TEST(a_unit_whose_fight_is_over_settles_through_toidle) {
  const auto fighter = Entity::parse(bytes_of(kFighterEntity));
  REQUIRE(fighter.ok());
  World world;
  CombatSystem combat = make_system();
  combat.set_world_bound(true);
  // Blows every 600 against a 1,000 ms swing, so the look that finds the
  // target dead comes while the killing swing is still in the air -- which is
  // when settling too early would show.
  CombatProfile quick = melee(16, 12, 200);
  quick.attack_interval = 600;
  combat.set_profile(kHastatus, quick);
  const ObjectId a = place(world, combat, &fighter.value(), kHastatus, 0, 200, Point{540, 500});
  const ObjectId b = place(world, combat, &fighter.value(), kSwordsman, 1, 20, Point{500, 500});
  CHECK(combat.order_attack(a, b));
  const WorldObject* slot = world.find(a);
  REQUIRE(slot != nullptr && slot->object != nullptr);

  bool seen_toidle = false;
  bool cut_short = false;
  std::int32_t settles = 0;
  for (int turn = 0; turn < 20; ++turn) {
    world.advance(400);
    // What the swing is doing once this turn's animation pass is done, which
    // is what combat sees when it decides.
    const std::int32_t was = slot->object->anim.anim_slot;
    const bool was_running = slot->animating;
    const std::int32_t was_elapsed = slot->object->anim.elapsed_ms;
    combat.advance(world, world.turn());
    if (slot->object->anim.anim_slot == kAnimToIdle) {
      seen_toidle = true;
      // Never over a swing still in progress.
      if (was == kAnimAttack && was_running) cut_short = true;
      // Started once: a settled unit is not settled again every time it
      // looks for a target and finds none.
      if (was != kAnimToIdle || slot->object->anim.elapsed_ms < was_elapsed) ++settles;
    }
  }
  CHECK(combat.is_dying(b) || combat.find(b) == nullptr);
  CHECK(seen_toidle);
  CHECK(!cut_short);
  CHECK(settles == 1);
  CHECK(slot->object->anim.anim_slot == kAnimToIdle);
}

/// **A death is seen, and lasts as long as the animation that shows it.** The
/// dying state used to hold the victim's last pose for an invented second and
/// then pop it out of the world.
///
/// The shipped death strips end on the corpse -- long holds over the rows
/// where the body lies, rots and goes -- and the one battle dump has 14
/// objects in `action=8` up to 14 s after their own last blow. So the object
/// stays for one cycle of its entity's slot 9 and leaves when it ends; the
/// system-wide duration is only for an entity with no death to measure.
TEST(a_dying_unit_plays_its_death_and_leaves_when_the_animation_ends) {
  const auto fighter = Entity::parse(bytes_of(kFighterEntity));
  REQUIRE(fighter.ok());
  World world;
  CombatSystem combat = make_system();
  combat.set_world_bound(true);
  combat.set_death_duration(1000);  // the fallback, which must not be what decides
  const ObjectId a = place(world, combat, &fighter.value(), kHastatus, 0, 200, Point{540, 500});
  const ObjectId b = place(world, combat, &fighter.value(), kSwordsman, 1, 5, Point{500, 500});
  CHECK(combat.order_attack(a, b));
  fight_turn(world, combat);
  REQUIRE(combat.is_dying(b));
  const WorldObject* corpse = world.find(b);
  REQUIRE(corpse != nullptr && corpse->object != nullptr);
  CHECK(corpse->object->anim.anim_slot == kAnimDie);
  CHECK(corpse->repeat == AnimRepeat::hold);
  // The blow fell somewhere in the turn that just ended.
  const GameTime turn_end = world.time();
  const Combatant* dying = combat.find(b);
  REQUIRE(dying != nullptr);
  CHECK(fighter.value().timeline_for_slot(kAnimDie).cycle() == 3000);
  CHECK(dying->death_time > turn_end - 400 + 3000);
  CHECK(dying->death_time <= turn_end + 3000);

  // Well past the fallback: still lying there, still animating.
  while (world.time() < turn_end + 2400) fight_turn(world, combat);
  const WorldObject* still = world.find(b);
  CHECK(still != nullptr);
  CHECK(combat.is_dying(b));
  CHECK(still != nullptr && still->animating);
  // And gone once the strip has run.
  while (world.time() < turn_end + 3400) fight_turn(world, combat);
  CHECK(world.find(b) == nullptr);
  CHECK(combat.find(b) == nullptr);
}

/// **What a fight shows does not depend on how time was cut into turns.**
/// Combat strikes at exact instants inside a turn, and the cursors are in the
/// `slots` hash; a swing or a death started at the end of the turn instead of
/// at its instant would put two peers on different frames whenever their
/// turns fell differently, and the conformance harness's partition check
/// would see it. Two runs, one in 400s and one in 300s and 500s, compared
/// every 1600 game-time units.
TEST(a_fight_shows_the_same_frames_however_time_is_cut_into_turns) {
  const auto fighter = Entity::parse(bytes_of(kFighterEntity));
  REQUIRE(fighter.ok());
  struct Run {
    World world;
    CombatSystem combat = make_system();
    ObjectId a = kNoObject;
    ObjectId b = kNoObject;
  };
  const auto start = [&](Run& run) {
    run.combat.set_world_bound(true);
    run.a = place(run.world, run.combat, &fighter.value(), kHastatus, 0, 200, Point{540, 500});
    run.b = place(run.world, run.combat, &fighter.value(), kSwordsman, 1, 30, Point{500, 500});
    CHECK(run.combat.order_attack(run.a, run.b));
  };
  Run even;
  Run uneven;
  start(even);
  start(uneven);
  const auto cursor_of = [](const Run& run, ObjectId id) {
    const WorldObject* slot = run.world.find(id);
    AnimCursor out;
    out.anim_slot = -1;
    if (slot != nullptr && slot->object != nullptr) out = slot->object->anim;
    return out;
  };
  bool saw_death = false;
  for (int check = 0; check < 4; ++check) {
    for (int i = 0; i < 4; ++i) {
      even.world.advance(400);
      even.combat.advance(even.world, even.world.turn());
    }
    // Boundaries at 300, 800, 1100, 1600: the blow near 1000 falls in a turn
    // that ends at 1200 in one run and at 1100 in the other.
    for (const GameTime length : {300, 500, 300, 500}) {
      uneven.world.advance(length);
      uneven.combat.advance(uneven.world, uneven.world.turn());
    }
    REQUIRE(even.world.time() == uneven.world.time());
    for (const ObjectId id : {even.a, even.b}) {
      const AnimCursor x = cursor_of(even, id);
      const AnimCursor y = cursor_of(uneven, id);
      CHECK(x.anim_slot == y.anim_slot);
      CHECK(x.elapsed_ms == y.elapsed_ms);
      CHECK(x.step == y.step);
      CHECK(x.variation == y.variation);
    }
    saw_death = saw_death || cursor_of(even, even.b).anim_slot == kAnimDie;
  }
  CHECK(saw_death);
}

/// **A corpse neither fights nor is fought.** `order_attack` checked `alive`,
/// which stays true through the dying state, so a corpse's own
/// `while (.Attack(u))` put it back into `engaging` -- health 0, striking,
/// and out of the drain that would have removed it.
TEST(a_corpse_cannot_be_ordered_to_attack_or_be_attacked) {
  World world;
  CombatSystem combat = make_system();
  combat.set_death_duration(1000);
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 5, Point{40, 0}));
  combat.add(unit(3, kHastatus, 0, 200, Point{0, 40}));
  CHECK(combat.order_attack(1, 2));
  world.advance(400);
  combat.advance(world, world.turn());
  REQUIRE(combat.is_dying(2));

  CHECK(!combat.order_attack(2, 1));  // the corpse does not get up
  CHECK(!combat.order_attack(3, 2));  // and is not a target
  const Combatant* corpse = combat.find(2);
  REQUIRE(corpse != nullptr);
  CHECK(corpse->action == Action::dying);
  CHECK(corpse->anim == kAnimDying);

  // It leaves on time, which a resurrected corpse never did.
  for (int turn = 0; turn < 5 && combat.find(2) != nullptr; ++turn) {
    world.advance(400);
    combat.advance(world, world.turn());
  }
  CHECK(combat.find(2) == nullptr);
}

/// **A corpse does not walk.** All 15 dying objects across the nine dumps are
/// without `SyncFlags` bit 17. A unit killed mid-walk stops where it fell,
/// its death is not frozen on the first row by movement's arrival handling,
/// and an order that reaches it afterwards moves nothing.
TEST(a_unit_killed_mid_walk_stops_and_its_death_keeps_playing) {
  const auto fighter = Entity::parse(bytes_of(kFighterEntity));
  REQUIRE(fighter.ok());
  // Both systems registered, in the session's order, because movement asks
  // the world for combat to learn who is dying.
  World world;
  MovementSystem movement;
  CombatSystem combat = make_system();
  REQUIRE(world.add_system(&movement));
  REQUIRE(world.add_system(&combat));
  movement.set_grid(ObstructionGrid(64, 64));
  combat.set_world_bound(true);
  const ObjectId a = place(world, combat, &fighter.value(), kHastatus, 0, 200, Point{540, 500});
  const ObjectId b = place(world, combat, &fighter.value(), kSwordsman, 1, 30, Point{500, 500});
  movement.state(b).speed = 5;  // slow enough to stay in reach for three blows
  movement.state(b).walk_anim = kAnimAttack;  // any slot the fixture has
  CHECK(movement.order_goto(world, b, Point{500, 100}, 0, 0) != MoveOutcome::blocked);
  CHECK(combat.order_attack(a, b));
  bool walked = false;
  for (int turn = 0; turn < 12 && !combat.is_dying(b); ++turn) {
    walked = walked || movement.find(b)->walking;
    world.advance(400);
  }
  REQUIRE(combat.is_dying(b));
  CHECK(walked);
  CHECK(!movement.find(b)->has_path);
  const Point fell = world.state(b)->position;

  // An order after death, as a hero's army order would send one.
  (void)movement.order_goto(world, b, Point{500, 100}, 0, 0);
  world.advance(400);
  world.advance(400);
  CHECK(world.state(b)->position == fell);
  CHECK(!movement.find(b)->has_path);
  const WorldObject* corpse = world.find(b);
  REQUIRE(corpse != nullptr && corpse->object != nullptr);
  CHECK(corpse->object->anim.anim_slot == kAnimDie);
  CHECK(corpse->animating);
  CHECK(corpse->object->anim.elapsed_ms > 0);
}

TEST(death_passes_through_the_dying_state) {
  World world;
  CombatSystem combat = make_system();
  combat.set_death_duration(1000);
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 5, Point{40, 0}));
  CHECK(combat.order_attack(1, 2));

  world.advance(400);
  combat.advance(world, world.turn());
  const Combatant* dying = combat.find(2);
  CHECK(dying != nullptr);
  if (dying != nullptr) {
    // The dump's dying object: action=8, Anim=8, health at or below zero.
    CHECK(dying->action == Action::dying);
    CHECK(dying->anim == kAnimDying);
    CHECK(dying->health <= 0);
  }
  CHECK(!combat.is_alive(2));

  // It leaves the world when the animation ends, not before.
  for (int turn = 0; turn < 5 && combat.find(2) != nullptr; ++turn) {
    world.advance(400);
    combat.advance(world, world.turn());
  }
  CHECK(combat.find(2) == nullptr);
}

/// Every landed blow is credited twice, and a kill is worth the victim's class
/// maximum rather than what was left of it.
///
/// `gbr.exe` books the *health delta* (0x0051151d) into the attacker's
/// `damage_inflicted` and the victim's `damage_taken` -- one number, two rows --
/// and on death adds `[victim + 0x3c] + 0x294`, the class maximum, to the
/// killer's `kill_healths` and the victim's `die_healths`.
TEST(a_fight_credits_both_sides_and_a_kill_is_worth_a_full_bar) {
  World world;
  MatchSystem match;
  REQUIRE(world.add_system(&match));
  CombatSystem combat = make_system();
  combat.set_death_duration(1000);
  // Five health against a ten-damage swing: the blow removes five, not ten.
  // Overkill is the one thing the delta reading and the damage reading
  // disagree about.
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 5, Point{40, 0}));
  CHECK(combat.preview(1, 2).final_damage == 10);
  CHECK(combat.order_attack(1, 2));

  world.advance(400);
  combat.advance(world, world.turn());

  CHECK(match.score(0).damage_inflicted == 5);
  CHECK(match.score(1).damage_taken == 5);
  CHECK(match.score(0).damage_taken == 0);
  CHECK(match.score(1).damage_inflicted == 0);

  // The swordsman's profile says 200, and that is what the kill is worth --
  // not the 5 it had left and not the 5 that were removed.
  CHECK(match.score(0).kill_healths == 200);
  CHECK(match.score(1).die_healths == 200);
  CHECK(match.score(1).kill_healths == 0);
  CHECK(match.score(0).die_healths == 0);

  // Dying is not dying twice. The body stays in the world for the animation,
  // and nothing books again while it does.
  for (int turn = 0; turn < 5 && combat.find(2) != nullptr; ++turn) {
    world.advance(400);
    combat.advance(world, world.turn());
  }
  CHECK(combat.find(2) == nullptr);
  CHECK(match.score(0).kill_healths == 200);

  // And the rating moves off its floor of 10 because of it: the killer's
  // (200 >> 1 + 5 + 1000) * 100 over an untouched 10,000, and the victim's
  // 1000 * 100 over (200 >> 1 + 5 + 10000).
  CHECK(military_rating(match.score(0)) == (100 + 5 + 1000) * 100 / 10000);
  CHECK(military_rating(match.score(1)) == 1000 * 100 / (100 + 5 + 10000));
}

/// An allied blow books nothing at all, on either side.
///
/// 0x00511550 routes it into a capped non-lethal branch at 0x0051164f that
/// jumps past every counter. A player is allied to itself, so a unit damaging
/// its own side is the same case.
TEST(an_allied_blow_is_not_counted) {
  World world;
  MatchSystem match;
  REQUIRE(world.add_system(&match));
  world.players().set(0, 1, Relation::allied, true);
  world.players().set(1, 0, Relation::allied, true);
  REQUIRE(world.players().are_allied(0, 1));

  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  // `order_attack` does not consult diplomacy, which is what lets the case be
  // constructed at all: acquisition would never have picked this target.
  CHECK(combat.order_attack(1, 2));
  world.advance(400);
  combat.advance(world, world.turn());

  CHECK(combat.find(2)->health < 200);  // the damage landed
  CHECK(match.score(0).damage_inflicted == 0);
  CHECK(match.score(1).damage_taken == 0);
}

/// Combat is what stamps a squad's "last fight", on the swing rather than the
/// landing -- the same hook, and for the same reason, as the hero's clock.
TEST(a_blow_stamps_the_victims_squad_as_well_as_its_hero) {
  World world;
  HeroSystem heroes;
  REQUIRE(world.add_system(&heroes));
  CombatSystem combat = make_system();

  const ObjectId attacker = world.spawn(NativeClass::unit, nullptr);
  const ObjectId hero = world.spawn(NativeClass::hero, nullptr);
  const ObjectId victim = world.spawn(NativeClass::unit, nullptr);
  world.set_owner(attacker, 0);
  world.set_owner(hero, 1);
  world.set_owner(victim, 1);
  for (const ObjectId id : {attacker, hero, victim}) world.set_health(id, 200);
  heroes.register_hero(world, hero);
  heroes.register_unit(world, victim);
  REQUIRE(heroes.attach(world, victim, hero));
  const SquadKey key = heroes.squad_of(hero);
  REQUIRE(key.valid());

  combat.add(unit(attacker, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(victim, kSwordsman, 1, 200, Point{40, 0}));
  CHECK(combat.order_attack(attacker, victim));
  world.advance(400);
  combat.advance(world, world.turn());

  const Squad* squad = heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  CHECK(squad->last_attacker == attacker);
  CHECK(squad->last_fight_time > 0);
  // And the hero's own clock, which is the other half of the same call.
  REQUIRE(heroes.hero(hero) != nullptr);
  CHECK(heroes.hero(hero)->army_attacked_unit == victim);
}

/// A blow the victim survives books damage and nothing else.
///
/// `gbr.exe` books `kill_healths` and `die_healths` inside the death block at
/// 0x005118c0, which the resolver reaches only when the virtual that applies
/// the damage reports the victim dead.
TEST(a_blow_that_does_not_kill_books_no_kill) {
  World world;
  MatchSystem match;
  REQUIRE(world.add_system(&match));
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  CHECK(combat.order_attack(1, 2));

  world.advance(400);
  combat.advance(world, world.turn());

  CHECK(combat.is_alive(2));
  CHECK(match.score(0).damage_inflicted == 10);
  CHECK(match.score(1).damage_taken == 10);
  CHECK(match.score(0).kill_healths == 0);
  CHECK(match.score(1).die_healths == 0);
}

/// A world with no `MatchSystem` books nothing and does not fall over, which is
/// the state every other case in this file runs in.
TEST(combat_without_a_match_still_fights) {
  World world;
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  CHECK(combat.order_attack(1, 2));
  world.advance(400);
  combat.advance(world, world.turn());
  CHECK(combat.find(2)->health == 190);
}

TEST(a_stale_target_handle_reads_as_gone_rather_than_as_somebody_else) {
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  CHECK(combat.order_attack(1, 2));
  CHECK(combat.find(1)->target == 2);
  CHECK(combat.remove(2));
  CHECK(combat.find(1)->target == sim::kNoObject);
}

TEST(apply_damage_and_heal_clamp) {
  // The world is here only because a kill fires the class's `ondie` hook and
  // the hook needs somewhere to look; this one has no runner, so the fire is a
  // no-op and the arithmetic below is unchanged. See `sim/hooks.hpp`.
  World world;
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200));
  CHECK(combat.apply_damage(world, 1, 50) == 50);
  CHECK(combat.health(1) == 150);
  CHECK(combat.heal(1, 500) == 50);  // clamped to maxhealth 200
  CHECK(combat.health(1) == 200);
  CHECK(combat.apply_damage(world, 1, 1000) == 200);
  CHECK(!combat.is_alive(1));
  CHECK(combat.apply_damage(world, 1, 10) == 0);  // already dying
}

TEST(experience_accrues_per_damaging_attack) {
  World world;
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  combat.order_attack(1, 2);
  world.advance(400);
  combat.advance(world, world.turn());
  const Combatant* attacker = combat.find(1);
  CHECK(attacker != nullptr);
  // Against a level-1 target: 1 / 8 + 1 = 1 per attack.
  if (attacker != nullptr) CHECK(attacker->experience == attacker->attacks);
}

TEST(the_difficulty_addend_moves_the_level_term) {
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  const std::int32_t even = combat.preview(1, 2).final_damage;
  // Easy is 10; at a level difference of 10 the table pays 40%.
  combat.set_player_level_addend(0, combat.constants().difficulty_addend(Difficulty::easy));
  const DamageBreakdown eased = combat.preview(1, 2);
  CHECK(eased.level_difference == 10);
  CHECK(eased.level_percent == 40);
  CHECK(eased.final_damage == even * 140 / 100);
}

// ---------------------------------------------------------------------------
// ranged and siege
// ---------------------------------------------------------------------------

TEST(a_ranged_attack_puts_a_projectile_in_the_air) {
  World world;
  CombatSystem combat = make_system();
  CombatProfile archer = melee(20, 0, 150);
  archer.damage_type = DamageType::pierce;
  archer.range = 500;
  archer.sight = 600;
  archer.projectile = 99;
  archer.attack_interval = 2000;
  combat.set_profile(kArcher, archer);

  combat.add(unit(1, kArcher, 0, 150, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{400, 0}));
  combat.order_attack(1, 2);

  world.advance(100);
  combat.advance(world, world.turn());
  CHECK(combat.projectiles().size() == 1);
  CHECK(combat.health(2) == 200);  // nothing has landed yet
  if (!combat.projectiles().empty()) {
    const Projectile& shot = combat.projectiles().front();
    CHECK(shot.id != sim::kNoObject);        // it holds a world handle
    CHECK(world.find(shot.id) != nullptr);
    CHECK(shot.impact > shot.launched);
  }

  for (int turn = 0; turn < 10 && combat.health(2) == 200; ++turn) {
    world.advance(100);
    combat.advance(world, world.turn());
  }
  CHECK(combat.health(2) < 200);
  CHECK(combat.projectiles().empty());  // and the shot left the world
}

TEST(splash_damage_reaches_neighbours) {
  World world;
  CombatSystem combat = make_system();
  CombatProfile catapult = melee(120, 0, 1000);
  catapult.damage_type = DamageType::siege;
  catapult.range = 800;
  catapult.min_range = 301;
  catapult.sight = 850;
  catapult.radius = 50;
  catapult.splash_radius = 100;
  catapult.projectile = 99;
  catapult.attack_interval = 15000;
  combat.set_profile(kArcher, catapult);

  combat.add(unit(1, kArcher, 0, 1000, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{500, 0}));
  combat.add(unit(3, kSwordsman, 1, 200, Point{560, 0}));   // inside the splash
  combat.add(unit(4, kSwordsman, 1, 200, Point{700, 0}));   // outside it
  combat.order_attack(1, 2);

  for (int turn = 0; turn < 20; ++turn) {
    world.advance(200);
    combat.advance(world, world.turn());
  }
  CHECK(combat.find(2) == nullptr || combat.health(2) < 200);
  CHECK(combat.find(3) == nullptr || combat.health(3) < 200);
  CHECK(combat.health(4) == 200);
}

// ---------------------------------------------------------------------------
// determinism
// ---------------------------------------------------------------------------

namespace {

/// A three-a-side melee, run over a caller-supplied sequence of turn lengths.
std::uint64_t run_battle(std::span<const std::int32_t> turn_lengths) {
  World world;
  CombatSystem combat;
  combat.set_profile(kHastatus, melee(16, 12, 200));
  combat.set_profile(kSwordsman, melee(12, 6, 200));
  combat.set_profile(kArcher, melee(30, 8, 380));

  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kHastatus, 0, 200, Point{0, 40}));
  combat.add(unit(3, kArcher, 0, 380, Point{0, 80}));
  combat.add(unit(4, kSwordsman, 1, 200, Point{40, 0}));
  combat.add(unit(5, kSwordsman, 1, 200, Point{40, 40}));
  combat.add(unit(6, kArcher, 1, 380, Point{40, 80}));

  for (const std::int32_t length : turn_lengths) {
    world.advance(length);
    combat.advance(world, world.turn());
  }

  std::uint64_t hash = 0xCBF29CE484222325ull;
  combat.hash(hash);
  return hash;
}

std::vector<std::int32_t> repeat(std::int32_t length, int count) {
  return std::vector<std::int32_t>(static_cast<std::size_t>(count), length);
}

}  // namespace

TEST(identical_worlds_fight_identically) {
  const std::vector<std::int32_t> turns = repeat(400, 40);
  CHECK(run_battle(turns) == run_battle(turns));
}

TEST(turn_length_partition_does_not_change_the_battle) {
  // 16,000 game-time units of fighting, cut up four different ways. The retail
  // dumps show 200, 400, 799 and 800 all occurring, and a session
  // renegotiating mid-game, so this is the property that matters.
  const std::uint64_t reference = run_battle(repeat(400, 40));
  CHECK(run_battle(repeat(200, 80)) == reference);
  CHECK(run_battle(repeat(800, 20)) == reference);
  CHECK(run_battle(repeat(1600, 10)) == reference);

  std::vector<std::int32_t> mixed;
  for (int i = 0; i < 10; ++i) {
    mixed.push_back(400);
    mixed.push_back(800);
    mixed.push_back(200);
    mixed.push_back(200);
  }
  CHECK(run_battle(mixed) == reference);
}

TEST(a_ranged_exchange_is_partition_invariant_too) {
  const auto run = [](std::span<const std::int32_t> lengths) {
    World world;
    CombatSystem combat;
    CombatProfile archer;
    archer.damage = 20;
    archer.damage_type = DamageType::pierce;
    archer.max_health = 150;
    archer.range = 500;
    archer.sight = 600;
    archer.radius = 15;
    archer.projectile = 99;
    archer.attack_interval = 1300;
    combat.set_profile(kArcher, archer);
    combat.add(unit(1, kArcher, 0, 150, Point{0, 0}));
    combat.add(unit(2, kArcher, 1, 150, Point{300, 0}));
    for (const std::int32_t length : lengths) {
      world.advance(length);
      combat.advance(world, world.turn());
    }
    std::uint64_t hash = 0xCBF29CE484222325ull;
    combat.hash(hash);
    return hash;
  };
  const std::vector<std::int32_t> a = repeat(400, 30);
  const std::vector<std::int32_t> b = repeat(200, 60);
  const std::vector<std::int32_t> c = repeat(1200, 10);
  CHECK(run(a) == run(b));
  CHECK(run(a) == run(c));
}

TEST(the_hash_notices_every_field_it_covers) {
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{10, 20}));
  const auto digest = [&combat] {
    std::uint64_t h = 0xCBF29CE484222325ull;
    combat.hash(h);
    return h;
  };
  const std::uint64_t before = digest();
  combat.find(1)->health -= 1;
  CHECK(digest() != before);
  combat.find(1)->health += 1;
  CHECK(digest() == before);
  combat.find(1)->position.x += 1;
  CHECK(digest() != before);
}

// ---------------------------------------------------------------------------
// host functions
// ---------------------------------------------------------------------------

TEST(the_combat_host_slice_is_defined_and_reaches_the_system) {
  imperivm::core::script::HostRegistry registry;
  const std::size_t before = registry.implemented();
  define_combat_host(registry);
  CHECK(registry.implemented() > before);

  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));

  // Owned here, not borrowed: `CallContext::user` is a `HostContext*` for every
  // domain now, and a `CombatHostContext*` behind it is the exact confusion
  // sim/host_context.hpp exists to make impossible. The context carries the
  // world and the type id; the system is found through the world by name, the
  // way heroes and movement already find theirs.
  World world;
  world.add_system(&combat);
  CHECK(combat_system_of(world) == &combat);
  HostContext context_state;
  context_state.world = &world;
  context_state.object_type = 1;

  const auto call = [&](const char* name, std::uint16_t arity,
                        std::vector<imperivm::core::script::Value> args) {
    const std::uint32_t index =
        registry.find(imperivm::core::script::CallKind::member, name, arity);
    CHECK(index != 0xFFFFFFFFu);
    // A declared-but-undefined entry point has a null `fn`, and calling one
    // crashes the whole binary before any test reports. That happened here the
    // moment combat gave `IsEnemy/1` up to the player domain, so the harness
    // now refuses instead of dereferencing.
    if (index == 0xFFFFFFFFu || registry.entry(index).fn == nullptr) {
      CHECK(false);
      return imperivm::core::script::HostOutcome::failed("not implemented here");
    }
    imperivm::core::script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context_state;
    ctx.name = name;
    ctx.kind = imperivm::core::script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };

  using imperivm::core::script::Value;
  const Value me = Value::object(1, 1);
  const Value them = Value::object(1, 2);

  CHECK(call("attack", 0, {me}).value.as_integer() == 16);
  CHECK(call("range", 0, {me}).value.as_integer() == 17);
  // `IsEnemy/1` is not combat's any more -- `sim/player_host.cpp` owns it,
  // because the relation is the asymmetric matrix in `World::players()` and the
  // corpus calls it with an integer player as well as an object. Combat's
  // symmetric ally table could express neither.
  CHECK(call("IsValidTarget", 1, {me, them}).value.as_integer() == 1);
  CHECK(call("InRange", 1, {me, them}).value.as_integer() == 1);
  CHECK(call("BestTargetInSquadSight", 0, {me}).value.as_object().id == 2);

  call("Damage", 1, {them, Value::integer(50)});
  CHECK(combat.health(2) == 150);
  // The original refuses negative damage by name; so does this.
  const auto refused = call("Damage", 1, {them, Value::integer(-1)});
  CHECK(refused.status == imperivm::core::script::HostStatus::error);

  call("Heal", 1, {them, Value::integer(10)});
  CHECK(combat.health(2) == 160);
  // **`Attack` suspends**, which is the whole difference between a working
  // `while (.Attack(u))` and a busy loop. `gbr.exe` registers `Unit::Attack`
  // through 0x00699eb0 -- the *suspending* registrar, the one `Sleep`, every
  // `Wait*`, `Stop`, `Idle` and all four `Goto*` go through -- and not through
  // 0x00699bb0. See `host_attack` for the whole chain.
  const auto attacked = call("Attack", 1, {me, them});
  CHECK(attacked.status == imperivm::core::script::HostStatus::suspend);
  CHECK(attacked.value.as_integer() == 1);
  // The slice is the attacker's own strike interval, never zero: a zero-length
  // suspension resumes in the same pass, which is the loop this prevents.
  CHECK(attacked.suspend_for > 0);
  CHECK(combat.find(1)->target == 2);
  // A refused order does not suspend. 0x005ddbd2 pushes 0 and returns `eax = 0`
  // -- this protocol's "done" -- where only the accepted path returns 1. So
  // `while (.Attack(u))` leaves the moment the target stops being attackable
  // rather than spending game time failing.
  const auto self_attack = call("Attack", 1, {me, me});
  CHECK(self_attack.status == imperivm::core::script::HostStatus::ok);
  CHECK(self_attack.value.as_integer() == 0);
  CHECK(self_attack.suspend_for == 0);
  // Nothing at no health is a valid target -- a unit's `vtbl + 0x50` is
  // `health == 0`, and a building's zero is tested apart (see `targetable`).
  // `SetHealth(0)` runs no death, so this is the target test's own clause.
  const std::int32_t kept = combat.health(2);
  call("SetHealth", 1, {them, Value::integer(0)});
  CHECK(call("IsValidTarget", 1, {me, them}).value.as_integer() == 0);
  call("SetHealth", 1, {them, Value::integer(kept)});
  CHECK(call("IsValidTarget", 1, {me, them}).value.as_integer() == 1);
  // `KillCommand` is deliberately *not* called here: it means "end the running
  // command", not "kill the receiver", and it belongs to sim/command.cpp. A
  // unit dies through the damage pipeline, which is what this asserts.
  combat.apply_damage(world, 2, combat.health(2));
  CHECK(!combat.is_alive(2));
}

TEST(time_without_attack_counts_from_the_stamp_the_last_blow_left) {
  // Three instructions in `gbr.exe` (0x005d7f93): the clock, minus
  // `unit + 0x1a4`, pushed as an int. What makes it worth a test is the two
  // ends, which pull in opposite directions and which a plausible
  // implementation gets backwards.
  imperivm::core::script::HostRegistry registry;
  define_combat_host(registry);

  // One combatant and no enemy: this test is about the arithmetic, and a
  // second unit 40 away would fight and stamp the field under it.
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  World world;
  world.add_system(&combat);
  HostContext context_state;
  context_state.world = &world;
  context_state.object_type = 1;

  const auto since = [&](ObjectId id) -> std::int64_t {
    const std::uint32_t index =
        registry.find(imperivm::core::script::CallKind::member, "TimeWithoutAttack", 0);
    CHECK(index != 0xFFFFFFFFu);
    if (index == 0xFFFFFFFFu || registry.entry(index).fn == nullptr) {
      CHECK(false);
      return std::int64_t{-1};
    }
    imperivm::core::script::CallContext ctx;
    std::vector<imperivm::core::script::Value> args{
        imperivm::core::script::Value::object(1, static_cast<std::uint32_t>(id))};
    ctx.arguments = args;
    ctx.user = &context_state;
    ctx.name = "TimeWithoutAttack";
    ctx.kind = imperivm::core::script::CallKind::member;
    return registry.entry(index).fn(ctx).value.as_integer();
  };

  world.advance_turns(10);
  const GameTime now = world.time();
  REQUIRE(now > 0);

  // **A unit that has never attacked reports the whole session, not zero.**
  // `last attack time` starts at 0 and 0x005d7f9f subtracts it unconditionally
  // -- there is no "never attacked" sentinel and no clamp. Reading this as zero
  // would tell every `if (u.TimeWithoutAttack > n)` in the corpus that a unit
  // which has stood idle all game just struck.
  CHECK(since(1) == now);

  // A stamp, and the count restarts from it.
  combat.find(1)->last_attack_time = now - 500;
  CHECK(since(1) == 500);
  combat.find(1)->last_attack_time = now;
  CHECK(since(1) == 0);

  // **An unknown receiver is zero** -- the opposite end from the never-attacked
  // unit above, and 0x005d7f7f is explicit about it: the diagnostic path pushes
  // 0, so a dead unit reads as having just attacked rather than as having
  // waited forever. The two defaults are not interchangeable and this pins
  // which is which.
  CHECK(since(9999) == 0);

  // And a stamp *ahead* of the clock reads zero rather than negative. This is
  // reachable: `World::time()` is the boundary the last turn began at while
  // `CombatSystem` schedules on `[time + 1, end]`, so a blow struck in the
  // final instant of a turn lands past it. The original cannot produce a
  // negative here -- it reads one clock -- so neither does this.
  combat.find(1)->last_attack_time = now + 1;
  CHECK(since(1) == 0);
}

TEST(the_combat_host_slice_leaves_thirteen_entry_points_to_their_owners) {
  // `HostRegistry::define` replaces silently, so an entry point two domains
  // both claim behaves like whichever registered last -- and each domain's own
  // tests keep passing, because each builds its own registry. These twelve
  // collided; combat gives up all thirteen. `health`, `maxhealth`, `stamina`,
  // `IsAlive`, `IsDead`, `radius` and `sight` go to the object model, which
  // answers for every object rather than only for registered combatants;
  // `level`, `inherentlevel`, `experience`, `SetLevel` and `SetExperience` go
  // to heroes, which owns unit progression. Pinned here so that re-adding one
  // fails in this file as well as in test_host_setup.cpp.
  imperivm::core::script::HostRegistry registry;
  imperivm::core::script::declare_shipped_surface(registry);
  define_combat_host(registry);

  const struct {
    const char* name;
    std::uint16_t arity;
  } not_ours[] = {
      {"health", 0},        {"maxhealth", 0},  {"stamina", 0},    {"IsAlive", 0},
      {"IsDead", 0},        {"radius", 0},     {"sight", 0},      {"level", 0},
      {"inherentlevel", 0}, {"experience", 0}, {"SetLevel", 1},   {"SetExperience", 1},
      // Not a duplicate but a misreading: this domain answered it by killing
      // the receiver, and `ESH_FOODTRADE.VS` calls it on a loaded mule.
      {"KillCommand", 0},
  };
  for (const auto& entry : not_ours) {
    const std::uint32_t index =
        registry.find(imperivm::core::script::CallKind::member, entry.name, entry.arity);
    // Still declared -- the surface is the whole inventory -- and still
    // unimplemented by this domain, so a script that reaches it before its
    // owner registers traps with its own name.
    CHECK(index != imperivm::core::script::kUnresolvedHost);
    if (index != imperivm::core::script::kUnresolvedHost) {
      CHECK(registry.entry(index).fn == nullptr);
    }
  }
}

TEST(a_combat_host_call_without_a_context_fails_loudly) {
  imperivm::core::script::HostRegistry registry;
  define_combat_host(registry);
  const std::uint32_t index =
      registry.find(imperivm::core::script::CallKind::member, "attack", 0);
  CHECK(index != 0xFFFFFFFFu);
  std::vector<imperivm::core::script::Value> args{imperivm::core::script::Value::integer(1)};
  imperivm::core::script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = nullptr;
  const auto out = registry.entry(index).fn(ctx);
  CHECK(out.status == imperivm::core::script::HostStatus::error);
  CHECK(out.error != nullptr);

  // The other half of the same refusal: a real context over a world that has no
  // combat system registered. Under `HostContext` the system is a lookup rather
  // than a field, so "no system" is a state a caller can reach and it has to
  // trap by its own name rather than share one with "no world".
  World bare;
  HostContext context_state;
  context_state.world = &bare;
  ctx.user = &context_state;
  const auto unbound = registry.entry(index).fn(ctx);
  CHECK(unbound.status == imperivm::core::script::HostStatus::error);
  REQUIRE(unbound.error != nullptr);
  CHECK(std::string_view(unbound.error) != std::string_view(out.error));
}

TEST(the_target_selection_family_is_defined_answers_and_refuses_a_null_context) {
  using imperivm::core::script::CallKind;
  using imperivm::core::script::Value;

  imperivm::core::script::HostRegistry registry;
  imperivm::core::script::declare_shipped_surface(registry);
  define_combat_host(registry);

  // The seven this domain claims, and the one it does not. `BestTargetInGAIKA`
  // belongs to the GAIKA slice and is bound in `sim/ai.cpp`, so it is absent
  // *here* without being unimplemented -- this registry never runs
  // `register_ai_host`.
  //
  // **`BestTargetInSquadSight/1` used to be in the second group** and this
  // comment used to say it was left trapping because its rule is a 200-point
  // nudge on a scale this system does not use. The scale argument still holds
  // and is why the preference is not scaled; what did not hold is the
  // conclusion, because the alternative to an unscaled preference was not a
  // faithful answer but two scripts that never run. See
  // `TargetFilter::prefer_class`.
  const struct {
    const char* name;
    std::uint16_t arity;
    bool ours;
  } family[] = {
      {"BestTargetInSquadSight", 0, true},
      {"BestTargetInSquadSightMisZeroDamage", 0, true},
      {"BestNoIndependentTargetInSquadSight", 0, true},
      {"BestTargetInSquadSight_PreferUndiseased", 0, true},
      {"BestTargetInSquadSightExclusive", 1, true},
      {"BestTargetForPos", 0, true},
      {"BestTargetInRange", 2, true},
      {"BestTargetInSquadSight", 1, true},
      {"BestTargetInGAIKA", 0, false},
  };
  for (const auto& entry : family) {
    const std::uint32_t index = registry.find(CallKind::member, entry.name, entry.arity);
    // Declared either way: the surface is the whole shipped inventory.
    CHECK(index != imperivm::core::script::kUnresolvedHost);
    if (index == imperivm::core::script::kUnresolvedHost) continue;
    CHECK((registry.entry(index).fn != nullptr) == entry.ours);
    if (!entry.ours) continue;

    // A null `CallContext::user` must refuse, not dereference.
    std::vector<Value> args{Value::object(1, 1), Value::integer(0), Value::integer(0)};
    imperivm::core::script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = nullptr;
    ctx.name = entry.name;
    ctx.kind = CallKind::member;
    const auto refused = registry.entry(index).fn(ctx);
    CHECK(refused.status == imperivm::core::script::HostStatus::error);
  }

  // And the one that carries 859 of Numantia's traps answers through the host
  // path, with the same board as the system-level case above.
  CombatSystem combat = make_system();
  combat.set_profile(kPeasant, melee(0, 0, 100));
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kPeasant, 1, 100, Point{100, 0}));
  combat.add(unit(3, kSwordsman, 1, 200, Point{200, 0}));
  World world;
  world.add_system(&combat);
  HostContext context_state;
  context_state.world = &world;
  context_state.object_type = 1;

  const auto call = [&](const char* name, std::uint16_t arity, std::vector<Value> args) {
    const std::uint32_t index = registry.find(CallKind::member, name, arity);
    // Calling a declared-but-undefined entry point dereferences a null `fn` and
    // takes the whole binary down before any test reports, so refuse instead.
    if (index == imperivm::core::script::kUnresolvedHost ||
        registry.entry(index).fn == nullptr) {
      CHECK(false);
      return imperivm::core::script::HostOutcome::failed("not implemented here");
    }
    imperivm::core::script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context_state;
    ctx.name = name;
    ctx.kind = CallKind::member;
    return registry.entry(index).fn(ctx);
  };

  const Value me = Value::object(1, 1);
  CHECK(call("BestTargetInSquadSight", 0, {me}).value.as_object().id == 2);
  CHECK(call("BestTargetInSquadSightMisZeroDamage", 0, {me}).value.as_object().id == 3);
  // No class graph behind this system, so `Exclusive` can resolve no name and
  // answers "nothing" rather than failing -- which is what the executable's
  // string compare does too.
  CHECK(call("BestTargetInSquadSightExclusive", 1, {me, Value::string("GSwordsman")})
            .value.as_object()
            .id == sim::kNoObject);
  // **And the plain `/1` answers the opposite on the same input**, because a
  // name that resolves to nothing is a penalty every candidate takes rather
  // than a rejection every candidate fails. Same board, same string, and the
  // two entry points part company on it.
  CHECK(call("BestTargetInSquadSight", 1, {me, Value::string("GSwordsman")})
            .value.as_object()
            .id == 2);
}

// ---------------------------------------------------------------------------
// profiles
// ---------------------------------------------------------------------------

TEST(armour_is_selected_by_the_attackers_damage_type) {
  CombatProfile p;
  p.armor_slash = 12;
  p.armor_pierce = 4;
  CHECK(p.armour_against(DamageType::slash) == 12);
  CHECK(p.armour_against(DamageType::pierce) == 4);
  CHECK(p.armour_against(DamageType::siege) == 0);
  CHECK(p.armour_against(DamageType::none) == 0);
}

TEST(damage_type_names_round_trip) {
  CHECK(damage_type_from_name("slash") == DamageType::slash);
  CHECK(damage_type_from_name("pierce") == DamageType::pierce);
  CHECK(damage_type_from_name("siege") == DamageType::siege);
  CHECK(damage_type_from_name("none") == DamageType::none);
  // Unit's own declaration is slash, so that is the fallback.
  CHECK(damage_type_from_name("nonsense") == DamageType::slash);
  CHECK(damage_type_name(DamageType::siege) == "siege");
}

TEST(binding_to_the_world_shares_position_owner_and_health) {
  // The integration path: combatant ids are world object ids, movement owns
  // ObjectState::position, and combat writes health back into it.
  World world;
  CombatSystem combat = make_system();
  combat.set_world_bound(true);

  const ObjectId a = world.spawn(NativeClass::unit, nullptr, kHastatus);
  const ObjectId b = world.spawn(NativeClass::unit, nullptr, kSwordsman);
  world.mutable_state(a)->owner = 0;
  world.set_position(a, Point{0, 0});
  world.mutable_state(a)->health = 200;
  world.mutable_state(b)->owner = 1;
  world.set_position(b, Point{4000, 0});  // out of sight to begin with
  world.mutable_state(b)->health = 200;

  Combatant one = unit(a, kHastatus, 0, 200);
  Combatant two = unit(b, kSwordsman, 1, 200);
  combat.add(one);
  combat.add(two);

  world.advance(400);
  combat.advance(world, world.turn());
  CHECK(combat.health(b) == 200);  // nothing in reach

  // Movement walks it in; combat reads the new position off the world. It
  // takes a scan interval to notice, which is the point of the acquire cadence.
  world.set_position(b, Point{40, 0});
  for (int turn = 0; turn < 5 && combat.health(b) == 200; ++turn) {
    world.advance(400);
    combat.advance(world, world.turn());
  }
  // `REQUIRE`, not `CHECK`: a fault that stops `a` being enrolled at all makes
  // this a null dereference, and a SIGSEGV here kills the run before the
  // harness prints a single `FAIL` line or its check tally. Two of the faults
  // this file's audit injected were "caught" only that way.
  REQUIRE(combat.find(a) != nullptr);
  CHECK((combat.find(a)->position == Point{0, 0}));
  CHECK(combat.health(b) < 200);
  CHECK(world.state(b)->health == combat.health(b));  // written back
}

TEST(a_class_that_cannot_attack_never_strikes) {
  World world;
  CombatSystem combat = make_system();
  CombatProfile peasant = melee(0, 0, 100);
  peasant.damage_type = DamageType::none;
  combat.set_profile(kPeasant, peasant);
  combat.add(unit(1, kPeasant, 0, 100, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  CHECK(combat.best_target(1) == sim::kNoObject);
  for (int turn = 0; turn < 5; ++turn) {
    world.advance(400);
    combat.advance(world, world.turn());
  }
  CHECK(combat.health(2) == 200);
}

// ---------------------------------------------------------------------------
// enrolment: the wiring, not the rules
// ---------------------------------------------------------------------------
//
// Everything above this line builds a `CombatSystem` and hands it combatants.
// That is exactly how the system came to be complete, tested, registered in
// `kSystemOrder` -- and dead in every real session: `CombatSystem::add` had no
// caller outside this file, so `units_` was empty, `advance` returned on its
// first line, and two hostile armies could stand on top of each other forever.
// The tests below start from a map instead, which is the only shape that can
// notice.

namespace {

/// Four classes covering the whole of the enrolment predicate: a decor root
/// that must be excluded, two hostile unit classes that must be enrolled and
/// must fight, and a building that must be enrolled as a target without ever
/// striking anything.
ClassGraph enrolment_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Object" cpp_class="CVXDecor" parent="">
      <properties maxhealth="100"/>
    </class>)"), "test_combat.cpp");
  graph.add(bytes_of(R"(<class id="Rock" cpp_class="CVXDecor" parent="Object">
      <properties maxhealth="100"/>
    </class>)"), "test_combat.cpp");
  graph.add(bytes_of(R"(<class id="Legionary" cpp_class="CVXUnit" parent="Object">
      <properties maxhealth="200" maxstamina="10" damage="40" damage_type="slash"
                  armor_slash="4" armor_pierce="4" range="17" min_range="2"
                  radius="15" sight="500" attack_delay="400"/>
    </class>)"), "test_combat.cpp");
  graph.add(bytes_of(R"(<class id="Gaul" cpp_class="CVXUnit" parent="Object">
      <properties maxhealth="200" maxstamina="10" damage="30" damage_type="slash"
                  armor_slash="2" armor_pierce="2" range="17" min_range="2"
                  radius="15" sight="500" attack_delay="400"/>
    </class>)"), "test_combat.cpp");
  graph.add(bytes_of(R"(<class id="Hut" cpp_class="CVXBuilding" parent="Object">
      <properties maxhealth="1000" radius="40" sight="600" target_priority="100"/>
    </class>)"), "test_combat.cpp");
  graph.link();
  return graph;
}

/// Two enemies within reach of each other, one building, one rock. `healthperc`
/// is spelled out on two of them because the loader has to resolve it against
/// the class maximum rather than ignoring it.
constexpr std::string_view kEnrolmentMap = R"(<mapobject>
  <scriptobj class="Rock" num="0" x="100" y="100" flags="0x80000000"/>
  <scriptobj class="Legionary" num="1" x="1000" y="1000" player="1"
      healthperc="100" flags="0x80400001"/>
  <scriptobj class="Gaul" num="2" x="1040" y="1000" player="2"
      healthperc="50" flags="0x80400002"/>
  <scriptobj class="Hut" num="3" x="3000" y="3000" player="1"
      health="777" flags="0x80800001"/>
</mapobject>)";

}  // namespace

TEST(combat_enrols_units_and_buildings_and_nothing_else) {
  ClassGraph graph = enrolment_graph();
  const Result<MapObjectList> map = MapObjectList::parse(bytes_of(kEnrolmentMap));
  REQUIRE(map.ok());

  World world;
  CombatSystem combat;
  combat.set_class_graph(&graph);
  combat.set_world_bound(true);
  world.add_system(&combat);
  world.populate_from_map(map.value(), graph, nullptr);
  world.start();

  // Four objects placed, three of them combatants: the rock is `CVXDecor`, and
  // `gbr.exe` allocates a `CVXDecor` with `new(0x78)` while `Obj::health` reads
  // `[obj + 0xE8]` -- a decor object has no health field at all.
  // Four placed objects plus the three per-session singletons the loader
  // mints, which are internal and have no class.
  CHECK(world.objects().size() == 7);
  REQUIRE(combat.combatants().size() == 3);
  for (const Combatant& c : combat.combatants()) {
    CHECK(c.class_index != graph.lookup("Rock"));
  }
  // Ascending object id, which is spawn order, which is map document order.
  CHECK(combat.combatants()[0].id < combat.combatants()[1].id);
  CHECK(combat.combatants()[1].id < combat.combatants()[2].id);

  // Seeded from the map, not from the class defaults: `healthperc="50"` on a
  // 200-point class is 100, and the one object in the whole retail corpus with
  // an absolute `health` gets that figure verbatim.
  const Combatant* legion = combat.combatants().data() + 0;
  const Combatant* gaul = combat.combatants().data() + 1;
  const Combatant* hut = combat.combatants().data() + 2;
  CHECK(legion->health == 200);
  CHECK(gaul->health == 100);
  CHECK(hut->health == 777);
  CHECK(legion->owner == 0);   // `player="1"` is slot 0
  CHECK(gaul->owner == 1);
  CHECK((legion->position == Point{1000, 1000}));
  // Full stamina off the class, which is where the stamina rules are written.
  CHECK(legion->stamina == 10);
  CHECK(hut->stamina == 0);
}

TEST(combat_enrolled_from_a_map_actually_kills) {
  // The failure this whole exercise exists to prevent: two hostile armies
  // standing next to each other while nothing happens. 40 units apart, both
  // classes armed, both sides mutual enemies by default (a zero relations row
  // means bit 0 clear, which is "enemy").
  ClassGraph graph = enrolment_graph();
  const Result<MapObjectList> map = MapObjectList::parse(bytes_of(kEnrolmentMap));
  REQUIRE(map.ok());

  World world;
  CombatSystem combat;
  combat.set_class_graph(&graph);
  combat.set_world_bound(true);
  world.add_system(&combat);
  world.populate_from_map(map.value(), graph, nullptr);
  world.start();

  REQUIRE(combat.combatants().size() == 3);
  const ObjectId gaul = combat.combatants()[1].id;
  const std::int32_t before = combat.health(gaul);
  std::size_t strikes = 0;
  std::size_t deaths = 0;
  for (int turn = 0; turn < 40; ++turn) {
    world.advance(400);
    for (const CombatEvent& event : combat.events()) {
      if (event.kind == CombatEvent::Kind::strike) ++strikes;
      if (event.kind == CombatEvent::Kind::death) ++deaths;
    }
  }
  CHECK(strikes > 0);
  CHECK(deaths > 0);
  CHECK(combat.health(gaul) < before);
  // Dead is gone: the dying state has a duration and its end is the object
  // leaving the world, so the roll and the world agree afterwards.
  CHECK(combat.find(gaul) == nullptr);
  CHECK(world.find(gaul) == nullptr);
}

namespace {

/// A catapult the shipped way -- `CVXCatapult` under a decor root, the retail
/// `Catapult`'s numbers -- and a footman for it to shoot at.
ClassGraph siege_enrolment_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Object" cpp_class="CVXDecor" parent="">
      <properties maxhealth="100"/>
    </class>)"), "test_combat.cpp");
  graph.add(bytes_of(R"(<class id="Catapult" cpp_class="CVXCatapult" parent="Object">
      <properties maxhealth="1000" damage="120" damage_type="siege" range="800"
                  min_range="301" radius="50" sight="850" attack_delay="500"
                  splash_radius="100"/>
    </class>)"), "test_combat.cpp");
  graph.add(bytes_of(R"(<class id="Gaul" cpp_class="CVXUnit" parent="Object">
      <properties maxhealth="200" maxstamina="10" damage="30" damage_type="slash"
                  armor_slash="2" armor_pierce="2" range="17" min_range="2"
                  radius="15" sight="500" attack_delay="400"/>
    </class>)"), "test_combat.cpp");
  graph.link();
  return graph;
}

/// An engine at full health with an enemy 500 units off -- inside its 800 and
/// outside its 301 -- and, beside the attributes a map carries, a stray
/// `Built="1"`: the key `gbr.exe`'s save serialiser writes for the flag, which
/// a map's reader does not read.
constexpr std::string_view kPlacedEngineMap = R"(<mapobject>
  <scriptobj class="Catapult" num="0" x="1000" y="1000" player="1"
      healthperc="100" Built="1" flags="0x80800001"/>
  <scriptobj class="Gaul" num="1" x="1500" y="1000" player="2"
      healthperc="100" flags="0x80400002"/>
</mapobject>)";

}  // namespace

/// **A siege engine a map places is unbuilt, at whatever health the map gives
/// it**, and so takes no part in the fight until its crew finish it.
///
/// No shipped map places one: a sweep of all 29 `map.obj.xml` documents, the
/// random-map templates and `RandomMap.pak` finds no object of any of the eight
/// `CVXCatapult` classes, nor of `RamUnit`, `BCatapultUnit`, `TCatapultUnit` or
/// `CatapultTower`. So this is the rule an editor-made map meets, read off the
/// original:
///   * the `CVXCatapult` constructor zeroes `[cat+0x208]` (0x004e1ede) and sets
///     bit 25 of the `+0x2c` word through `vtbl+0x44`;
///   * a map object is read through `vtbl+0x6c`, which `CVXCatapult` does not
///     override -- it is the building reader (0x004dee10), which calls `Obj`'s
///     (0x005af280) -- and neither touches `+0x208`. The flag's name, `Built`,
///     is registered only by the class's persist serialiser (0x004e1c90), beside
///     `BuildFrame`, `Attack`, `Rotate` and `HasTowers`: a saved game's field,
///     not a map's attribute;
///   * the only writers of 1 are `Catapult::SetBuilt` (0x004e2e64) and the
///     engine's death handler, `vtbl+0xb0` (0x004e4000), which `Obj::Damage`
///     calls once the health reaches zero.
/// And `CATAPULT_IDLE.VS` does not take full health as finished: it counts
/// `healthleft` down from `.maxhealth - 1` whatever `.health` is, so a full
/// engine is built over the same time as a fresh one, and an uncrewed one
/// decays after `CatapultBuildOnEmptyStartDamageTime`.
TEST(a_map_placed_siege_engine_is_unbuilt_and_holds_its_fire) {
  ClassGraph graph = siege_enrolment_graph();
  const Result<MapObjectList> map = MapObjectList::parse(bytes_of(kPlacedEngineMap));
  REQUIRE(map.ok());

  World world;
  CombatSystem combat;
  combat.set_class_graph(&graph);
  combat.set_world_bound(true);
  world.add_system(&combat);
  world.populate_from_map(map.value(), graph, nullptr);
  world.start();

  REQUIRE(combat.combatants().size() == 2);
  const ObjectId engine = combat.combatants()[0].id;
  const ObjectId gaul = combat.combatants()[1].id;
  REQUIRE(world.find(engine) != nullptr);
  REQUIRE(world.find(engine)->object->is_a(NativeClass::catapult));
  // Full health as authored, and unbuilt all the same.
  CHECK(world.find(engine)->state.health == 1000);
  CHECK(!world.find(engine)->state.flags.built);

  const auto strikes_by = [&](ObjectId attacker, int turns) {
    int strikes = 0;
    for (int turn = 0; turn < turns; ++turn) {
      world.advance(400);
      for (const CombatEvent& event : combat.events()) {
        if ((event.kind == CombatEvent::Kind::strike || event.kind == CombatEvent::Kind::launch) &&
            event.attacker == attacker) {
          ++strikes;
        }
      }
    }
    return strikes;
  };
  CHECK(strikes_by(engine, 40) == 0);
  CHECK(combat.health(gaul) == 200);
  // The arrangement is one the engine can fight in: built, the same engine
  // shoots the same enemy.
  world.find(engine)->state.flags.built = true;
  CHECK(strikes_by(engine, 40) > 0);
}

TEST(combat_population_follows_objects_that_appear_and_disappear) {
  // A load-time-only population answers for a world that no longer exists.
  ClassGraph graph = enrolment_graph();
  World world;
  CombatSystem combat;
  combat.set_class_graph(&graph);
  combat.set_world_bound(true);
  world.add_system(&combat);
  world.start();
  CHECK(combat.combatants().empty());

  const ObjectId late =
      world.spawn(NativeClass::unit, nullptr, graph.lookup("Legionary"));
  world.mutable_state(late)->owner = 0;
  world.mutable_state(late)->health = 200;
  // A decor object spawned at the same time must stay out.
  (void)world.spawn(NativeClass::decor, nullptr, graph.lookup("Rock"));

  world.advance(400);
  REQUIRE(combat.combatants().size() == 1);
  CHECK(combat.combatants()[0].id == late);
  CHECK(combat.combatants()[0].health == 200);

  world.despawn(late);
  world.advance(400);
  CHECK(combat.combatants().empty());
}

/// A building counts what it takes, from a blow and from `Obj::Damage` alike,
/// and never overkill; a unit does not.
TEST(a_building_keeps_a_running_total_of_the_damage_it_takes) {
  ClassGraph graph = enrolment_graph();
  World world;
  CombatSystem combat = make_system();
  const ObjectId hut = world.spawn(NativeClass::building, nullptr, graph.lookup("Hut"));
  const ObjectId man = world.spawn(NativeClass::unit, nullptr, graph.lookup("Legionary"));
  combat.add(unit(hut, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(man, kSwordsman, 1, 200, Point{40, 0}));

  // A blow: the swordsman's 12 against the hut's armour of 12 lands the
  // pipeline's minimum, and whatever it is, the counter is the health delta.
  CHECK(combat.order_attack(man, hut));
  world.advance(400);
  combat.advance(world, world.turn());
  const std::int32_t blow = 200 - combat.find(hut)->health;
  CHECK(blow > 0);
  CHECK(world.state(hut)->damage_taken == blow);

  // `Obj::Damage`: the same counter.
  CHECK(combat.apply_damage(world, hut, 50) == 50);
  CHECK(world.state(hut)->damage_taken == blow + 50);
  CHECK(combat.apply_damage(world, hut, 30) == 30);
  CHECK(world.state(hut)->damage_taken == blow + 80);
  // Overkill is not counted: what is left is what is booked.
  const std::int32_t left = combat.find(hut)->health;
  CHECK(combat.apply_damage(world, hut, 1000) == left);
  CHECK(world.state(hut)->damage_taken == 200);
  // A unit has no such total -- not from the hut's blows back, not from
  // `Obj::Damage`.
  CHECK(combat.find(man)->health < 200);
  CHECK(world.state(man)->damage_taken == 0);
  CHECK(combat.apply_damage(world, man, 50) == 50);
  CHECK(world.state(man)->damage_taken == 0);
}

/// A building brought to no health stands there broken: `gbr.exe`'s building
/// vtables answer "not dead" at `+0x50` and do nothing at `+0xb0`, and the
/// target test turns away a building at zero health. So the hut below goes to
/// 0 and no lower, runs no death, stays in the world and on the roll long past
/// any death animation, sits in the broken tier -- and nobody goes on hitting
/// it.
TEST(a_building_at_no_health_stands_broken_and_is_no_longer_a_target) {
  ClassGraph graph = enrolment_graph();
  // An enemy hut on a sliver of health, in the legionary's reach (17 + 15 + 40
  // surface to surface), and nothing else for him to fight.
  constexpr std::string_view kHutMap = R"(<mapobject>
  <scriptobj class="Legionary" num="0" x="1000" y="1000" player="1"
      healthperc="100" flags="0x80400001"/>
  <scriptobj class="Hut" num="1" x="1060" y="1000" player="2"
      health="30" flags="0x80800002"/>
</mapobject>)";
  const Result<MapObjectList> map = MapObjectList::parse(bytes_of(kHutMap));
  REQUIRE(map.ok());

  World world;
  CombatSystem combat;
  combat.set_class_graph(&graph);
  combat.set_world_bound(true);
  world.add_system(&combat);
  world.populate_from_map(map.value(), graph, nullptr);
  world.start();

  REQUIRE(combat.combatants().size() == 2);
  const ObjectId man = combat.combatants()[0].id;
  const ObjectId hut = combat.combatants()[1].id;
  REQUIRE(world.find(hut)->state.flags.is_building);
  REQUIRE(combat.best_target(man) == hut);

  std::size_t blows_at_zero = 0;
  std::size_t hut_deaths = 0;
  bool reached_zero = false;
  // Forty turns of 400 ms: long past `death_duration_`, so a hut that had
  // entered the dying state would be gone from both the roll and the world.
  for (int turn = 0; turn < 40; ++turn) {
    world.advance(400);
    for (const CombatEvent& event : combat.events()) {
      if (event.defender != hut) continue;
      if (event.kind == CombatEvent::Kind::death) ++hut_deaths;
      if (event.kind == CombatEvent::Kind::strike) {
        CHECK(event.remaining_health >= 0);
        if (reached_zero) ++blows_at_zero;
        if (event.remaining_health == 0) reached_zero = true;
      }
    }
  }
  REQUIRE(reached_zero);
  CHECK(hut_deaths == 0);
  CHECK(blows_at_zero == 0);
  REQUIRE(combat.find(hut) != nullptr);
  REQUIRE(world.find(hut) != nullptr);
  CHECK(combat.find(hut)->health == 0);
  CHECK(combat.find(hut)->action != Action::dying);
  CHECK(combat.is_alive(hut));
  CHECK(world.state(hut)->health == 0);
  CHECK(world.state(hut)->damage_state == 3);  // `IsBroken`
  // Not a target any more, by any of the three doors.
  CHECK(combat.find(man)->target != hut);
  CHECK(combat.best_target(man) == kNoObject);
  CHECK(!combat.order_attack(man, hut));

  // `Obj::Damage` on it removes nothing and kills nothing.
  CHECK(combat.apply_damage(world, hut, 500) == 0);
  CHECK(combat.find(hut)->health == 0);
  CHECK(combat.find(hut)->action != Action::dying);
  // A unit is still mortal: the same call on the legionary kills him.
  combat.apply_damage(world, man, 10000);
  CHECK(combat.find(man)->action == Action::dying);
}

/// Unbound, an id says nothing about the world, so a synthetic combatant whose
/// id a world building happens to hold is still mortal.
TEST(an_unbound_combatant_is_mortal_whatever_the_world_holds_at_its_id) {
  ClassGraph graph = enrolment_graph();
  World world;
  CombatSystem combat = make_system();
  const ObjectId hut = world.spawn(NativeClass::building, nullptr, graph.lookup("Hut"));
  REQUIRE(world.find(hut)->state.flags.is_building);
  combat.add(unit(hut, kHastatus, 0, 200, Point{0, 0}));
  combat.apply_damage(world, hut, 1000);
  CHECK(combat.find(hut)->action == Action::dying);
}

TEST(is_combat_object_reads_the_native_class_hierarchy) {
  ClassGraph graph = enrolment_graph();
  World world;
  const ObjectId rock = world.spawn(NativeClass::decor, nullptr, graph.lookup("Rock"));
  const ObjectId man = world.spawn(NativeClass::unit, nullptr, graph.lookup("Legionary"));
  const ObjectId hut = world.spawn(NativeClass::building, nullptr, graph.lookup("Hut"));
  const ObjectId gate = world.spawn(NativeClass::gate, nullptr, graph.lookup("Hut"));
  const ObjectId shot = world.spawn(NativeClass::catapult_shot, nullptr, graph.lookup("Hut"));
  const ObjectId held = world.spawn_internal(InternalKind::holder);
  // An object whose class did not resolve has no profile to fight with.
  const ObjectId classless = world.spawn(NativeClass::unit, nullptr, kNoClass);

  CHECK(!is_combat_object(*world.find(rock)));
  CHECK(is_combat_object(*world.find(man)));
  CHECK(is_combat_object(*world.find(hut)));
  CHECK(is_combat_object(*world.find(gate)));
  CHECK(!is_combat_object(*world.find(shot)));
  CHECK(!is_combat_object(*world.find(held)));
  CHECK(!is_combat_object(*world.find(classless)));
}

TEST(reconcile_is_inert_without_world_binding) {
  // Every synthetic test above registers ids of its own. Reconciling those
  // against a world would delete all of them, which is why `world_bound_`
  // gates it.
  World world;
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  CHECK(combat.reconcile(world) == 2);
  world.advance(400);
  combat.advance(world, world.turn());
  CHECK(combat.combatants().size() == 2);
}

// ---------------------------------------------------------------------------
// the session, which is where both of these were dead
// ---------------------------------------------------------------------------
//
// The tests above bind the system to a world by hand, so they would still pass
// with `GameSession` doing nothing at all -- which is exactly the state this
// file found the engine in. These two go through `GameSession::create`, which
// is the only caller a shipped map ever has.

namespace {

/// A `GRID` payload in the shipped layout: "DIRG", cell size, bits per cell,
/// the two extents, then row-major cells with the bits of a row running least
/// significant first. `cells` many cells on a side.
std::vector<std::byte> pass_grid_wh(std::uint32_t cell_size, std::uint32_t bits_per_cell,
                                    std::uint32_t cells_x, std::uint32_t cells_y,
                                    std::uint32_t blocked_per_row) {
  imperivm::test::Builder out;
  out.text(kGridMagic)
      .u32(cell_size)
      .u32(bits_per_cell)
      .u32(cell_size * cells_x)
      .u32(cell_size * cells_y);
  const std::uint32_t stride = (cells_x * bits_per_cell + 7) / 8;
  for (std::uint32_t row = 0; row < cells_y; ++row) {
    for (std::uint32_t byte = 0; byte < stride; ++byte) {
      std::uint32_t value = 0;
      for (std::uint32_t bit = 0; bit < 8; ++bit) {
        if (byte * 8 + bit < blocked_per_row) value |= 1u << bit;
      }
      out.u8(value);
    }
  }
  return {out.span().begin(), out.span().end()};
}

std::vector<std::byte> pass_grid(std::uint32_t cell_size, std::uint32_t bits_per_cell,
                                 std::uint32_t cells, std::uint32_t blocked_per_row) {
  return pass_grid_wh(cell_size, bits_per_cell, cells, cells, blocked_per_row);
}

}  // namespace

TEST(a_session_enrols_combatants_and_feeds_movement_its_grid) {
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = enrolment_graph();
  const std::vector<std::byte> grid = pass_grid(16, 1, 64, 8);

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.map_objects = bytes_of(kEnrolmentMap);
  inputs.passability = grid;

  auto session = GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(session.ok());
  GameSession& run = *session.value();

  // Combat. Without `set_world_bound(true)` in `GameSession::create` this is
  // zero on every shipped map, and `CombatSystem::advance` returns on its first
  // line for the whole match.
  const CombatSystem* fight = combat_system_of(run.world());
  REQUIRE(fight != nullptr);
  REQUIRE(fight->combatants().size() == 3);
  CHECK(fight->world_bound());

  // Movement. Without `set_grid` the search runs over an empty grid, which is
  // not neutral: it is a world with no walls and no water.
  const MovementSystem* move = movement_system(run.world());
  REQUIRE(move != nullptr);
  CHECK(move->grid().width() == 64);
  CHECK(move->grid().height() == 64);
  CHECK(move->grid().count_blocked() == 64 * 8);
  CHECK(move->grid().blocked(Point{0, 0}));
  CHECK(!move->grid().blocked(Point{1000, 0}));

  // And it fights, through the session rather than through a hand-built world.
  run.advance(40, 400);
  CHECK(fight->combatants().size() < 3);
}

TEST(a_session_refuses_a_grid_that_is_not_the_collision_layer) {
  // The terrain-type layer declares 64 units and 8 bits per cell. Adopting it
  // as an obstruction bitmap would put every unit in the world four cells out
  // and read a terrain index as a passability bit, so `from_grid` refuses it
  // and the session degrades to the empty grid rather than to a wrong one.
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = enrolment_graph();
  const std::vector<std::byte> terrain = pass_grid(64, 8, 32, 4);

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.map_objects = bytes_of(kEnrolmentMap);
  inputs.passability = terrain;

  auto session = GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(session.ok());
  const MovementSystem* move = movement_system(session.value()->world());
  REQUIRE(move != nullptr);
  CHECK(move->grid().empty());
}

// ---------------------------------------------------------------------------
// what enrolment must not lose
// ---------------------------------------------------------------------------
//
// The six cases below were not chosen; they were *found*. Every fault in the
// enrolment mechanism that a careful reader could plausibly write was injected
// into the engine one at a time and the whole suite re-run. Twenty-two of the
// twenty-eight were caught by the tests above. These are the six that were not,
// and each is written against the observable a real session would show rather
// than against the line of code that produces it.

namespace {

/// Two classes built to make a long fight: 4,000 health against 5 damage, so
/// nobody dies inside the sixty turns the case below runs. The point is to let
/// real accumulated state get large enough that it cannot be mistaken for state
/// re-earned in a single turn -- see the comment in the test.
[[nodiscard]] ClassGraph attrition_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Object" cpp_class="CVXDecor" parent="">
      <properties maxhealth="100"/>
    </class>)"), "test_combat.cpp");
  graph.add(bytes_of(R"(<class id="Legionary" cpp_class="CVXUnit" parent="Object">
      <properties maxhealth="4000" maxstamina="10" damage="5" damage_type="slash"
                  armor_slash="0" armor_pierce="0" range="17" min_range="2"
                  radius="15" sight="500" attack_delay="400"/>
    </class>)"), "test_combat.cpp");
  graph.add(bytes_of(R"(<class id="Gaul" cpp_class="CVXUnit" parent="Object">
      <properties maxhealth="4000" maxstamina="10" damage="5" damage_type="slash"
                  armor_slash="0" armor_pierce="0" range="17" min_range="2"
                  radius="15" sight="500" attack_delay="400"/>
    </class>)"), "test_combat.cpp");
  graph.link();
  return graph;
}

}  // namespace

TEST(enrolment_keeps_what_a_combatant_has_earned) {
  // The merge walks two ascending sequences and skips past anything in `units_`
  // whose object has gone. Delete that skip and the two sequences fall out of
  // step the first time an object leaves by any route other than combat's own
  // death path -- a script kill, a demolition -- and from then on *every
  // higher-id combatant is re-seeded from the world on every turn*. Health
  // survives, because the world binding writes it back; experience, the current
  // target, the attack streak and the attack cadence do not. A veteran army
  // would quietly become a fresh one, one turn after an unrelated unit died.
  //
  // **The obvious version of this test does not work, and the reason is worth
  // writing down.** A re-seeded combatant gets `next_action_time = 0`, which is
  // in the past, so it acts at once -- twice inside one 400 ms turn -- and
  // re-acquires its old target and earns experience again on the spot. Run the
  // fight for a handful of turns and the re-seeded pair come out with exactly
  // the experience the real pair had, and every assertion passes. The fault was
  // injected and survived precisely that way. So the fight here is deliberately
  // *long and slow* -- 4,000 health against 5 damage, sixty turns -- and the
  // assertion is that the survivors' state is far larger than a single turn
  // could rebuild. That gap is the whole test.
  ClassGraph graph = attrition_graph();
  World world;
  CombatSystem combat;
  combat.set_class_graph(&graph);
  combat.set_world_bound(true);
  world.add_system(&combat);
  world.start();

  // The bystander goes first so that its id is *below* the pair that fight:
  // the skip only matters for ids above a departed one. Owned by a third
  // player and parked far outside anybody's 500-unit sight.
  const ObjectId bystander =
      world.spawn(NativeClass::unit, nullptr, graph.lookup("Legionary"));
  world.mutable_state(bystander)->owner = 2;
  world.mutable_state(bystander)->health = 4000;
  world.set_position(bystander, Point{100000, 100000});

  const ObjectId roman = world.spawn(NativeClass::unit, nullptr, graph.lookup("Legionary"));
  world.mutable_state(roman)->owner = 0;
  world.mutable_state(roman)->health = 4000;
  world.set_position(roman, Point{1000, 1000});

  const ObjectId gaul = world.spawn(NativeClass::unit, nullptr, graph.lookup("Gaul"));
  world.mutable_state(gaul)->owner = 1;
  world.mutable_state(gaul)->health = 4000;
  world.set_position(gaul, Point{1040, 1000});

  for (int turn = 0; turn < 60; ++turn) world.advance(400);

  REQUIRE(combat.combatants().size() == 3);
  const Combatant* before = combat.find(roman);
  REQUIRE(before != nullptr);
  const std::int32_t experience = before->experience;
  const std::int32_t attacks = before->attacks;
  const GameTime last = before->last_attack_time;
  // The preconditions, and the one that carries the test: a single turn can
  // rebuild at most two strikes' worth, so anything above ten is unambiguous.
  REQUIRE(experience > 10);
  REQUIRE(attacks > 10);
  REQUIRE(before->target == gaul);
  REQUIRE(last > 0);
  // Nobody has died, so the only thing that leaves the world is the bystander.
  REQUIRE(combat.is_alive(gaul));

  // The bystander leaves. It is below both fighters in id order, so a merge
  // that cannot skip it loses its place for everything above.
  world.despawn(bystander);
  world.advance(400);

  const Combatant* after = combat.find(roman);
  REQUIRE(after != nullptr);
  CHECK(combat.combatants().size() == 2);
  CHECK(after->experience >= experience);
  CHECK(after->attacks >= attacks);
  CHECK(after->last_attack_time >= last);
  CHECK(after->target == gaul);
  // And the same for the higher-id half of the pair, which a lost place hits
  // just as hard.
  const Combatant* other = combat.find(gaul);
  REQUIRE(other != nullptr);
  CHECK(other->experience > 10);
  CHECK(other->attacks > 10);
}

TEST(an_internal_object_is_not_a_combatant_even_when_it_looks_like_one) {
  // `is_combat_object` rejects internal objects *first*, and today that reject
  // is masked twice over: `World::spawn_internal` allocates with `kNoClass` and
  // a null native object, so a settlement, holder or warehouse would fail the
  // next two tests anyway. Deleting the internal check therefore changes
  // nothing that any test built through the world can see -- which means the
  // test that walks a spawned holder passes for the wrong reason.
  //
  // So this one builds the object the guard actually exists for: internal, with
  // a resolved class and the unit bit set. Nothing produces that today. The day
  // the economy hangs a `maxhealth` on a settlement it will, and the three
  // per-session singletons every shipped map mints would enrol as combatants,
  // be scored as targets and be despawned by combat's dying path.
  WorldObject slot;
  slot.id = 1;
  slot.class_index = 1;
  slot.state.flags.is_unit = true;

  // The control: exactly the same object with `internal` cleared *is* a
  // combatant, so the assertion below is about the internal bit and nothing
  // else.
  slot.internal = InternalKind::none;
  CHECK(is_combat_object(slot));

  for (const InternalKind kind : {InternalKind::settlement, InternalKind::holder,
                                  InternalKind::warehouse, InternalKind::query}) {
    slot.internal = kind;
    CHECK(!is_combat_object(slot));
  }

  // And with the building bit instead, which is the other half of the accept.
  slot.state.flags.is_unit = false;
  slot.state.flags.is_building = true;
  slot.internal = InternalKind::none;
  CHECK(is_combat_object(slot));
  slot.internal = InternalKind::settlement;
  CHECK(!is_combat_object(slot));
}

TEST(enrolment_clamps_a_map_that_authors_more_health_than_the_class_allows) {
  // `<scriptobj health="...">` wins outright over the class maximum in
  // `World::populate_from_map` -- that is the map's own figure and it is right
  // that it wins. It is not bounded there, though, so combat bounds it here.
  //
  // No shipped map exercises this: one object in the whole installation carries
  // an absolute `health` (`Townhall` num=0 in `randommap.BFHP`, health 2000)
  // and `Townhall` resolves `maxhealth` 5000 through `Building`, so the corpus
  // never authors more health than a class allows. A rule the corpus cannot
  // reach is a rule only a synthetic case can hold in place.
  //
  // Unclamped, such a unit fights from an inflated pool -- and `heal` *does*
  // clamp to the class maximum, so the first heal it ever receives silently
  // caps it lower than it started.
  ClassGraph graph = enrolment_graph();
  constexpr std::string_view kOverfullMap = R"(<mapobject>
  <scriptobj class="Legionary" num="0" x="1000" y="1000" player="1"
      health="500" flags="0x80400001"/>
  <scriptobj class="Legionary" num="1" x="2000" y="1000" player="1"
      health="120" flags="0x80400001"/>
</mapobject>)";
  const Result<MapObjectList> map = MapObjectList::parse(bytes_of(kOverfullMap));
  REQUIRE(map.ok());

  World world;
  CombatSystem combat;
  combat.set_class_graph(&graph);
  combat.set_world_bound(true);
  world.add_system(&combat);
  world.populate_from_map(map.value(), graph, nullptr);
  world.start();

  REQUIRE(combat.combatants().size() == 2);
  // The precondition: the world really did take the map's figure verbatim, so
  // the clamp below is combat's and not the loader's.
  const ObjectId overfull = combat.combatants()[0].id;
  REQUIRE(world.state(overfull) != nullptr);
  CHECK(world.state(overfull)->health == 500);

  CHECK(combat.combatants()[0].health == 200);   // clamped to `maxhealth`
  CHECK(combat.combatants()[1].health == 120);   // and a legal figure is kept
  CHECK(combat.max_health(overfull) == 200);
  // The heal asymmetry, stated as an assertion rather than as a comment.
  CHECK(combat.heal(overfull, 1000) == 0);
  CHECK(combat.health(overfull) == 200);
}

TEST(a_map_enrolled_combatant_starts_at_level_one_with_no_experience) {
  // `experience` and `level` are the two fields `reconcile` refuses to read
  // from the map, and the refusal is load-bearing: the map's `Level` attribute
  // spans 0..59 over the 28 shipped documents while the dumps' `level` is
  // 1..24, and the host API has two different levels behind one name. So the
  // seed is a constant -- and a constant nothing asserts is a constant that can
  // drift. Seeded at 0 instead of 1, the baseline is off by one against every
  // level that does *not* come from enrolment (a difficulty addend, a levelled
  // hero) and `award_experience` promotes the unit on its first blow; seeded
  // with experience, every map-placed unit arrives pre-levelled and hits harder
  // than an identical unit a script spawns later.
  ClassGraph graph = enrolment_graph();
  const Result<MapObjectList> map = MapObjectList::parse(bytes_of(kEnrolmentMap));
  REQUIRE(map.ok());

  World world;
  CombatSystem combat;
  combat.set_class_graph(&graph);
  combat.set_world_bound(true);
  world.add_system(&combat);
  world.populate_from_map(map.value(), graph, nullptr);
  world.start();

  REQUIRE(combat.combatants().size() == 3);
  for (const Combatant& c : combat.combatants()) {
    CHECK(c.level == 1);
    CHECK(c.experience == 0);
    CHECK(c.base_effective_level() == 1);
  }

  // Two units seeded identically therefore meet at a level difference of zero,
  // and the damage they deal carries no level term at all.
  const ObjectId roman = combat.combatants()[0].id;
  const ObjectId gaul = combat.combatants()[1].id;
  const DamageBreakdown even = combat.preview(roman, gaul);
  CHECK(even.level_difference == 0);
  CHECK(even.level_percent == 0);
  CHECK(even.after_counter == even.after_level);

  // And the addend moves it by exactly what it says, which is only true if the
  // baseline is 1. At 0 the difference would be the same but every absolute
  // level a hero or a script sets would be one out.
  combat.set_player_level_addend(0, 3);
  const DamageBreakdown tilted = combat.preview(roman, gaul);
  CHECK(tilted.level_difference == 3);

  // The first damaging blow pays exactly `experience_gain(defender_level)`,
  // which is a figure the `CONST.INI` recurrence fixes and a non-zero seed
  // would put out of reach.
  combat.set_player_level_addend(0, 0);
  REQUIRE(combat.order_attack(roman, gaul));
  // Counted rather than assumed: a turn is long enough for more than one
  // strike, so the assertion is `strikes * gain` and not `gain`.
  std::int32_t strikes = 0;
  for (int turn = 0; turn < 8 && strikes == 0; ++turn) {
    world.advance(400);
    for (const CombatEvent& event : combat.events()) {
      if (event.kind == CombatEvent::Kind::strike && event.attacker == roman &&
          event.damage > 0) {
        ++strikes;
      }
    }
  }
  const Combatant* after = combat.find(roman);
  REQUIRE(after != nullptr);
  REQUIRE(strikes > 0);
  CHECK(after->experience == strikes * combat.constants().experience_gain(1));
}

TEST(the_obstruction_grid_keeps_a_rectangular_map_the_right_way_round) {
  // Every grid the tests above build is square, so a transposed pair of extents
  // is invisible in all of them. On a map that is not square it is not
  // invisible at all: `in_bounds` rejects every real cell past the shorter
  // axis -- and `blocked_cell` reads out of bounds as **solid**, so A* refuses
  // to path there at all -- while cells inside the longer axis are indexed with
  // the wrong stride, putting walls and water in the wrong places. A unit would
  // be unable to walk into a third of the map and would collide with obstacles
  // that are not there.
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = enrolment_graph();
  // 96 cells across, 32 down, with the first 8 cells of every row blocked.
  const std::vector<std::byte> grid = pass_grid_wh(16, 1, 96, 32, 8);

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.map_objects = bytes_of(kEnrolmentMap);
  inputs.passability = grid;

  auto session = GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(session.ok());
  const MovementSystem* move = movement_system(session.value()->world());
  REQUIRE(move != nullptr);

  CHECK(move->grid().width() == 96);
  CHECK(move->grid().height() == 32);
  CHECK(move->grid().count_blocked() == 32 * 8);

  // The two ends of the long axis, which a transpose puts out of bounds. Cell
  // 95 is the last column; at 16 world units per cell it is 1520..1535.
  CHECK(!move->grid().blocked(Point{1528, 8}));
  CHECK(move->grid().blocked(Point{8, 8}));
  // And the row past the short axis really is off the map, in both readings --
  // asserted so that the two checks above cannot be satisfied by a grid that is
  // merely larger than it should be.
  CHECK(move->grid().blocked(Point{8, 1000}));
}

/// `HealStamina(n)` adds, clamps, refuses a negative -- and **fans out over a
/// group**, which eight of its eleven shipped sites need.
///
/// `gbr.exe` registers it twice, `Obj::HealStamina` (0x005ab700) and
/// `Query::HealStamina` (0x00579100). `T_CalgacusArmy.HealStamina(10)` occurs
/// five times, `Group("Ghouls").HealStamina(10)` twice, `Guards.HealStamina(2)`
/// once; a single-receiver implementation answers those eight by doing nothing
/// and passes every test written against the other three.
///
/// It is `SetStamina`'s guarded sibling: `Obj::SetStamina` (0x005ab440) is a
/// raw pass-through with no clamp at all, and this one adds, clamps to
/// `maxstamina`, and rejects a negative amount outright -- complaint and all,
/// into the sink that is a bare `ret`. So it cannot be used to drain, and the
/// three druid scripts that pay for a spell do it with
/// `SetStamina(.stamina - cost)` on the line above.
TEST(combat_heal_stamina_adds_clamps_refuses_negatives_and_fans_out) {
  imperivm::core::script::HostRegistry registry;
  imperivm::core::script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_objlist_host(registry);
  define_combat_host(registry);

  CombatSystem combat = make_system();
  World world;
  world.add_system(&combat);
  // Three combatants, and the same three as world objects, because
  // `receiver_objects` resolves through the world.
  for (ObjectId id = 1; id <= 3; ++id) {
    CHECK(world.spawn(NativeClass::unit, nullptr) == id);
    combat.add(unit(id, kHastatus, 0, 200, Point{0, 0}));
    combat.find(id)->stamina = 4;
  }
  HostContext context_state;
  context_state.world = &world;
  context_state.object_type = 1;

  const auto call = [&](const char* name, std::uint16_t arity,
                        std::vector<imperivm::core::script::Value> args)
      -> imperivm::core::script::HostOutcome {
    const std::uint32_t index =
        registry.find(imperivm::core::script::CallKind::member, name, arity);
    CHECK(index != 0xFFFFFFFFu);
    if (index == 0xFFFFFFFFu || registry.entry(index).fn == nullptr) {
      CHECK(false);
      return imperivm::core::script::HostOutcome::failed("not implemented here");
    }
    imperivm::core::script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context_state;
    ctx.name = name;
    ctx.kind = imperivm::core::script::CallKind::member;
    ctx.script = 11;
    return registry.entry(index).fn(ctx);
  };

  using imperivm::core::script::Value;
  const Value one = Value::object(1, 1);

  // `max_stamina` is 0 on the fixture's profile, so give it a ceiling first --
  // and note that a zero ceiling would make every clamp look like it worked.
  CombatProfile profile = melee(16, 12, 200);
  profile.max_stamina = 10;
  combat.set_profile(kHastatus, profile);

  // Additive, not a target value.
  CHECK(call("HealStamina", 1, {one, Value::integer(3)}).status ==
        imperivm::core::script::HostStatus::ok);
  CHECK(combat.find(1)->stamina == 7);

  // Clamped to `maxstamina`, where `SetStamina` is not.
  call("HealStamina", 1, {one, Value::integer(100)});
  CHECK(combat.find(1)->stamina == 10);
  call("SetStamina", 1, {one, Value::integer(999)});
  CHECK(combat.find(1)->stamina == 999);
  call("SetStamina", 1, {one, Value::integer(4)});

  // A negative amount is a rejected no-op, and it reports success: the original
  // formats "Negative heal stamina in function 'Obj::HealStamina'" into a sink
  // that is a bare `ret` and returns.
  const auto negative = call("HealStamina", 1, {one, Value::integer(-3)});
  CHECK(negative.status == imperivm::core::script::HostStatus::ok);
  CHECK(combat.find(1)->stamina == 4);

  // And the group form, which is the dominant one. A list of all three.
  ObjListPool& pool = objlist_pool_of(world);
  const ObjListId list = pool.acquire(11, 0);
  REQUIRE(list != kNoObjList);
  *pool.mutable_items(list) = std::vector<ObjectId>{1, 2, 3};
  call("HealStamina", 1, {make_objlist_value(list), Value::integer(2)});
  CHECK(combat.find(1)->stamina == 6);
  CHECK(combat.find(2)->stamina == 6);
  CHECK(combat.find(3)->stamina == 6);
}

// ---------------------------------------------------------------------------
// EnemiesInSight and GetPointOnTarget
// ---------------------------------------------------------------------------

namespace {

/// A world with combat and the whole host surface, and a helper that spawns a
/// combatant *and* the world object behind it -- `EnemiesInSight` reads both,
/// because the holder link and the unit bit live on the world side.
struct SightBench {
  imperivm::core::script::HostRegistry registry;
  ClassGraph graph;
  CombatSystem combat;
  World world;
  HostContext context;
  ClassIndex soldier_class = kNoClass;
  ClassIndex villager_class = kNoClass;
  ClassIndex wall_class = kNoClass;

  SightBench() {
    // A real graph, because `EnemiesInSight` reads `WorldObject::sight` -- the
    // same field `Obj::sight` answers -- and that is resolved from the class at
    // spawn. A combat profile alone would leave every looker blind.
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
        R"(<class id="Soldier" parent="Object" cpp_class="CVXUnit">
             <properties sight="500" maxhealth="200"/></class>)",
        R"(<class id="Villager" parent="Object" cpp_class="CVXUnit">
             <properties sight="120" maxhealth="40"/></class>)",
        R"(<class id="Wall" parent="Object" cpp_class="CVXBuilding">
             <properties sight="300" maxhealth="4000"/></class>)",
    };
    const char* names[] = {"object.sc.xml", "soldier.sc.xml", "villager.sc.xml", "wall.sc.xml"};
    for (int i = 0; i < 4; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    soldier_class = graph.find("Soldier");
    villager_class = graph.find("Villager");
    wall_class = graph.find("Wall");

    combat.set_profile(soldier_class, melee(16, 12, 200));
    combat.set_profile(villager_class, melee(1, 0, 40));
    combat.set_profile(wall_class, melee(16, 20, 4000));

    imperivm::core::script::declare_shipped_surface(registry);
    register_all_hosts(registry);
    world.set_class_graph(&graph);
    world.add_system(&combat);
    context.world = &world;
    context.object_type = 1;
  }

  ObjectId soldier(PlayerId owner, Point where, ClassIndex cls = kNoClass) {
    const ClassIndex which = cls == kNoClass ? soldier_class : cls;
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, which);
    world.set_position(id, where);
    world.set_owner(id, owner);
    world.set_health(id, 200);
    combat.add(unit(id, which, owner, 200, where));
    return id;
  }

  imperivm::core::script::HostOutcome call(const char* name, std::uint16_t arity,
                                           std::vector<imperivm::core::script::Value> args) {
    const std::uint32_t index =
        registry.find(imperivm::core::script::CallKind::member, name, arity);
    CHECK(index != imperivm::core::script::kUnresolvedHost);
    if (index == imperivm::core::script::kUnresolvedHost ||
        registry.entry(index).fn == nullptr) {
      return imperivm::core::script::HostOutcome::failed("not implemented");
    }
    imperivm::core::script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = imperivm::core::script::CallKind::member;
    return registry.entry(index).fn(ctx);
  }

  bool enemies_in_sight(ObjectId id) {
    const auto out = call("EnemiesInSight", 0, {imperivm::core::script::Value::object(1, id)});
    CHECK(out.status == imperivm::core::script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  }
};

}  // namespace

/// The predicate is a circle of the looker's own `sight`, and the rim counts.
TEST(combat_enemies_in_sight_is_a_circle_of_the_lookers_own_sight) {
  SightBench b;
  const ObjectId looker = b.soldier(0, Point{0, 0});          // sight 500
  const ObjectId foe = b.soldier(1, Point{400, 0});

  CHECK(b.enemies_in_sight(looker));

  // Just outside. `jg skip` on the squared compare, so 500 is in and 501 is out.
  b.combat.set_position(foe, Point{501, 0});
  b.world.set_position(foe, Point{501, 0});
  CHECK(!b.enemies_in_sight(looker));

  b.combat.set_position(foe, Point{500, 0});
  b.world.set_position(foe, Point{500, 0});
  CHECK(b.enemies_in_sight(looker));
}

/// An ally is not an enemy, a dead one is nobody, and a **one-damage** object
/// is not an enemy in sight however hostile it is.
///
/// That last bound is the one worth pinning: the target sweep's own armed
/// clause is `> 0` and this one is `> 1` (`0x005d530e`). A test written against
/// `> 0` passes on every ordinary soldier and gets the villager case wrong.
TEST(combat_enemies_in_sight_needs_an_armed_living_enemy) {
  SightBench b;
  const ObjectId looker = b.soldier(0, Point{0, 0});
  const ObjectId friend_ = b.soldier(0, Point{100, 0});
  CHECK(!b.enemies_in_sight(looker));
  CHECK(friend_ != kNoObject);

  const ObjectId foe = b.soldier(1, Point{100, 0});
  CHECK(b.enemies_in_sight(looker));

  // Dead: `vtbl + 0x50` is the first guard the sweep applies to a candidate.
  b.combat.find(foe)->alive = false;
  CHECK(!b.enemies_in_sight(looker));
  b.combat.find(foe)->alive = true;

  // A one-damage enemy. `melee(1, ...)` is armed by the target sweep's rule and
  // not by this one.
  b.combat.find(foe)->class_index = b.villager_class;
  CHECK(!b.enemies_in_sight(looker));

  // Two damage is enough.
  CombatProfile feeble = melee(2, 0, 40);
  b.combat.set_profile(b.villager_class, feeble);
  CHECK(b.enemies_in_sight(looker));
}

/// Two guards on the *looker* answer false before any sweep runs: it must be
/// alive, and it must not be inside a holder.
TEST(combat_enemies_in_sight_is_false_for_a_dead_or_garrisoned_looker) {
  SightBench b;
  const ObjectId looker = b.soldier(0, Point{0, 0});
  const ObjectId barracks = b.world.spawn(NativeClass::building, nullptr, b.wall_class);
  b.soldier(1, Point{100, 0});
  REQUIRE(b.enemies_in_sight(looker));

  // `word [unit + 0x154] != 0xffff`: a garrisoned unit sees nothing. Asserted
  // with the enemy still standing there, so this cannot pass by the sweep
  // finding nothing.
  REQUIRE(b.world.put_in_holder(looker, barracks));
  CHECK(!b.enemies_in_sight(looker));
  REQUIRE(b.world.remove_from_holder(looker, Point{0, 0}));
  b.combat.set_position(looker, Point{0, 0});
  CHECK(b.enemies_in_sight(looker));

  b.combat.find(looker)->alive = false;
  CHECK(!b.enemies_in_sight(looker));

  // A receiver that is not a combatant at all -- the building -- is false, not
  // a refusal.
  const auto out = b.call("EnemiesInSight", 0,
                          {imperivm::core::script::Value::object(1, barracks)});
  CHECK(out.status == imperivm::core::script::HostStatus::ok);
  CHECK(out.value.as_integer() == 0);
}

/// A building standing among enemies is not "enemies in sight" to anyone: the
/// sweep requires `kSyncUnit` on the *candidate*.
TEST(combat_enemies_in_sight_ignores_a_candidate_that_is_not_a_unit) {
  SightBench b;
  const ObjectId looker = b.soldier(0, Point{0, 0});

  // An enemy building, armed and alive and well inside sight.
  const ObjectId fort = b.world.spawn(NativeClass::building, nullptr, b.wall_class);
  b.world.set_position(fort, Point{200, 0});
  b.world.set_owner(fort, 1);
  b.world.set_health(fort, 4000);
  b.combat.add(unit(fort, b.wall_class, 1, 4000, Point{200, 0}));
  CHECK(!b.enemies_in_sight(looker));

  // The same object as a unit is seen, which is what makes the line above a
  // statement about the flag rather than about the fixture.
  const ObjectId trooper = b.soldier(1, Point{200, 0});
  CHECK(b.enemies_in_sight(looker));
  CHECK(trooper != kNoObject);
}

/// `.GetPointOnTarget(o)` answers a point on the target -- the target's own
/// position, which is the only candidate this engine has (see the note at
/// `host_get_point_on_target` for the footprint arm it does not reproduce).
TEST(combat_get_point_on_target_answers_the_targets_position) {
  SightBench b;
  const ObjectId engine_ = b.soldier(0, Point{0, 0});
  const ObjectId wall = b.world.spawn(NativeClass::building, nullptr, b.wall_class);
  b.world.set_position(wall, Point{900, 400});

  const auto out = b.call("GetPointOnTarget", 1,
                          {imperivm::core::script::Value::object(1, engine_),
                           imperivm::core::script::Value::object(1, wall)});
  REQUIRE(out.status == imperivm::core::script::HostStatus::ok);
  REQUIRE(is_point(out.value));
  CHECK((unpack_point(out.value) == Point{900, 400}));

  // A target that does not resolve is `(0, 0)` and not a refusal: both arms of
  // the original push a point whatever happens.
  const auto missing = b.call("GetPointOnTarget", 1,
                              {imperivm::core::script::Value::object(1, engine_),
                               imperivm::core::script::Value::object(1, 999999)});
  REQUIRE(missing.status == imperivm::core::script::HostStatus::ok);
  REQUIRE(is_point(missing.value));
  CHECK((unpack_point(missing.value) == Point{0, 0}));

  // A garrisoned target resolves through its holder, like every other position
  // read in this engine.
  const ObjectId rider = b.soldier(1, Point{50, 50});
  REQUIRE(b.world.put_in_holder(rider, wall));
  const auto held = b.call("GetPointOnTarget", 1,
                           {imperivm::core::script::Value::object(1, engine_),
                            imperivm::core::script::Value::object(1, rider)});
  REQUIRE(is_point(held.value));
  CHECK((unpack_point(held.value) == Point{900, 400}));
}

/// A hidden enemy is not an enemy in sight.
///
/// Bit 21 of `[obj + 0x2c]` means *not visible* -- `Obj::IsVisible` negates it
/// -- and the sweep's hidden arm (0x005d537e) makes a candidate carrying it
/// pass an owner-mask test before it counts. Reproduced as "a hidden object is
/// not seen", which is what that test comes to for the side doing the looking.
TEST(combat_enemies_in_sight_does_not_see_a_hidden_enemy) {
  SightBench b;
  const ObjectId looker = b.soldier(0, Point{0, 0});
  const ObjectId sneak = b.soldier(1, Point{100, 0});
  REQUIRE(b.enemies_in_sight(looker));

  REQUIRE(b.world.find(sneak) != nullptr);
  b.world.find(sneak)->state.flags.hidden = true;
  CHECK(!b.enemies_in_sight(looker));

  b.world.find(sneak)->state.flags.hidden = false;
  CHECK(b.enemies_in_sight(looker));
}

/// The radius is `Obj::sight` -- the world object's -- and not the combat
/// profile's shadow of it.
///
/// The two are the same number on every object a map places, which is exactly
/// why this has to be asserted somewhere they are not: `HERO_RETREAT_VERIFY.VS`
/// reads `.EnemiesInSight()` and `.sight` in one expression, and a predicate
/// answering from a different field than the one the script divides by would
/// send a hero the wrong way at a distance neither number explains.
TEST(combat_enemies_in_sight_reads_the_same_sight_the_script_reads) {
  SightBench b;
  const ObjectId looker = b.soldier(0, Point{0, 0});
  const ObjectId foe = b.soldier(1, Point{400, 0});

  // The class says 500 and the profile now says 50. The enemy at 400 is inside
  // one and outside the other.
  CombatProfile myopic = melee(16, 12, 200);
  myopic.sight = 50;
  b.combat.set_profile(b.soldier_class, myopic);

  const auto seen = b.call("sight", 0, {imperivm::core::script::Value::object(1, looker)});
  REQUIRE(seen.status == imperivm::core::script::HostStatus::ok);
  CHECK(seen.value.as_integer() == 500);
  CHECK(b.enemies_in_sight(looker));
  CHECK(foe != kNoObject);
}

// ---------------------------------------------------------------------------
// the hero's "we are being attacked" clock
// ---------------------------------------------------------------------------

/// A strike stamps the **defender's hero**, and a script's `Obj::Damage` does
/// not.
///
/// The original calls the target's `vtbl + 0xa4` from the attack action
/// (0x005d5faa), not from wherever damage lands, so damage dealt out of nowhere
/// moves nobody's clock. That distinction is the whole reason the stamp is in
/// `strike` rather than in `apply_damage`, and it is what this measures.
TEST(combat_a_strike_stamps_the_defenders_hero_and_a_script_damage_does_not) {
  ClassGraph graph = enrolment_graph();
  World world;
  CombatSystem combat = make_system();
  HeroSystem heroes;
  combat.set_class_graph(&graph);
  combat.set_world_bound(false);
  REQUIRE(world.add_system(&combat));
  REQUIRE(world.add_system(&heroes));

  const ObjectId leader = world.spawn(NativeClass::hero, nullptr);
  const ObjectId victim = world.spawn(NativeClass::unit, nullptr);
  const ObjectId enemy = world.spawn(NativeClass::unit, nullptr);
  for (const ObjectId id : {leader, victim, enemy}) world.set_health(id, 200);
  world.set_owner(leader, 0);
  world.set_owner(victim, 0);
  world.set_owner(enemy, 1);
  heroes.register_hero(world, leader);
  heroes.register_unit(world, victim);
  REQUIRE(heroes.attach(world, victim, leader));

  // **The victim cannot strike back**, and that is deliberate: a mutual fight
  // stamps the leader either way and would let "stamp the attacker's hero"
  // pass. `can_attack()` is `damage > 0`, so a zero-damage profile is a unit
  // that is only ever hit.
  constexpr ClassIndex kUnarmed = 91;
  combat.set_profile(kUnarmed, melee(0, 12, 200));
  combat.add(unit(leader, kUnarmed, 0, 200, Point{0, 0}));
  combat.add(unit(victim, kUnarmed, 0, 200, Point{100, 0}));
  combat.add(unit(enemy, kSwordsman, 1, 200, Point{110, 0}));

  REQUIRE(heroes.hero(leader) != nullptr);
  CHECK(heroes.hero(leader)->army_attacked_at == 0);

  // Damage from nowhere -- `Obj::Damage(n)`'s path. No attacker, no virtual, no
  // stamp.
  combat.apply_damage(world, victim, 10);
  CHECK(heroes.hero(leader)->army_attacked_at == 0);

  // A real fight. The two are 10 apart, well inside `melee`'s range of 17.
  REQUIRE(combat.order_attack(enemy, victim));
  std::size_t turns = 0;
  while (heroes.hero(leader)->army_attacked_at == 0 && turns < 40) {
    world.advance(400);
    ++turns;
  }
  CHECK(heroes.hero(leader)->army_attacked_at != 0);
  CHECK(heroes.hero(leader)->army_attacked_unit == victim);

  // And the attacker's own hero -- it has none -- is untouched, which is the
  // half a "stamp both sides" reading would get wrong. Asserted by giving the
  // *attacker* a hero of its own and checking that hero's clock never moves,
  // because "the enemy has no hero" is a fact about the fixture and not about
  // the rule.
  CHECK(heroes.hero(enemy) == nullptr);
  const ObjectId enemy_leader = world.spawn(NativeClass::hero, nullptr);
  world.set_owner(enemy_leader, 1);
  world.set_health(enemy_leader, 200);
  heroes.register_hero(world, enemy_leader);
  heroes.register_unit(world, enemy);
  REQUIRE(heroes.attach(world, enemy, enemy_leader));
  const GameTime mine = heroes.hero(leader)->army_attacked_at;
  for (int turn = 0; turn < 10; ++turn) world.advance(400);
  REQUIRE(heroes.hero(enemy_leader) != nullptr);
  CHECK(heroes.hero(enemy_leader)->army_attacked_at == 0);
  // While the defender's kept moving, so the fight really was still running.
  CHECK(heroes.hero(leader)->army_attacked_at > mine);
}

/// `SetExperienceModifier(player, percent)` is **per player**, and the *Battle
/// tactics* research is what writes it.
///
/// 0x005d2fb0 stores it in the player record at `+0x84`, not in a global, so
/// two players in one match can be on different rates -- which is the whole
/// difference between this and the `BattleTacticsExpModifier` constant that has
/// been standing in for it.
TEST(the_experience_modifier_is_per_player_and_scales_only_that_players_gain) {
  World world;
  CombatSystem combat = make_system();
  REQUIRE(world.add_system(&combat));

  // Untouched, every player reads the constant, which ships as 100.
  CHECK(combat.constants().experience_modifier == 100);
  for (PlayerId p = 0; p < 4; ++p) CHECK(combat.player_experience_modifier(p) == 100);

  combat.set_player_experience_modifier(0, 300);
  CHECK(combat.player_experience_modifier(0) == 300);
  CHECK(combat.player_experience_modifier(1) == 100);
  // Overwriting a player already in the table replaces rather than appends.
  combat.set_player_experience_modifier(0, 200);
  CHECK(combat.player_experience_modifier(0) == 200);

  // And it scales the gain. Two identical fights, one on each side of the
  // table, so the only difference between the two numbers is the modifier.
  const auto fight = [](CombatSystem& c, World& w) {
    c.order_attack(1, 2);
    w.advance(400);
    c.advance(w, w.turn());
    const Combatant* a = c.find(1);
    return a == nullptr ? -1 : a->experience;
  };
  World plain_world;
  CombatSystem plain = make_system();
  plain.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  plain.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  const std::int32_t at_100 = fight(plain, plain_world);
  CHECK(at_100 > 0);

  World doubled_world;
  CombatSystem doubled = make_system();
  doubled.set_player_experience_modifier(0, 200);
  doubled.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  doubled.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  CHECK(fight(doubled, doubled_world) == 2 * at_100);

  // The **attacker's** modifier, not the defender's: setting the other side's
  // changes nothing.
  World theirs_world;
  CombatSystem theirs = make_system();
  theirs.set_player_experience_modifier(1, 200);
  theirs.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  theirs.add(unit(2, kSwordsman, 1, 200, Point{40, 0}));
  CHECK(fight(theirs, theirs_world) == at_100);
}

/// It is turn state: research writes it mid-game, so it has to move the hash
/// and survive a save. The difficulty addends beside it are match setup and do
/// neither.
TEST(the_experience_modifier_is_hashed_and_round_trips) {
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  std::uint64_t before = 0;
  combat.hash(before);
  combat.set_player_experience_modifier(3, 250);
  std::uint64_t after = 0;
  combat.hash(after);
  CHECK(after != before);

  std::vector<std::byte> bytes;
  combat.serialize(bytes);
  CombatSystem loaded = make_system();
  REQUIRE(loaded.deserialize(bytes).ok());
  CHECK(loaded.player_experience_modifier(3) == 250);
  CHECK(loaded.player_experience_modifier(2) == 100);
  std::uint64_t reloaded = 0;
  loaded.hash(reloaded);
  CHECK(reloaded == after);

  // And a re-save is byte-identical, which is what the round trip promises.
  std::vector<std::byte> again;
  loaded.serialize(again);
  CHECK(again == bytes);

  // Setting the same player twice **replaces**. A getter cannot see the
  // difference -- an appended duplicate sorts ahead of the old row and answers
  // first -- so the assertion is on the bytes, where a second row shows.
  CombatSystem twice = make_system();
  twice.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  twice.set_player_experience_modifier(3, 40);
  twice.set_player_experience_modifier(3, 250);
  std::vector<std::byte> twice_bytes;
  twice.serialize(twice_bytes);
  CHECK(twice_bytes == bytes);
}

/// The host entry point, which is where the two arguments and the 1-based
/// player live. `ONFINISH_RESEARCH.VS` writes
/// `SetExperienceModifier(.player, GetConst("BattleTacticsExpModifier"))`.
TEST(set_experience_modifier_takes_a_one_based_player_then_the_percentage) {
  World world;
  CombatSystem combat = make_system();
  REQUIRE(world.add_system(&combat));

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_combat_host(registry);
  HostContext context;
  context.world = &world;
  const std::uint32_t index =
      registry.find(script::CallKind::free_function, "SetExperienceModifier", 2);
  REQUIRE(index != script::kUnresolvedHost);
  REQUIRE(registry.entry(index).fn != nullptr);
  const auto call = [&](std::int32_t player, std::int32_t percent) {
    std::vector<script::Value> args{script::Value::integer(player),
                                    script::Value::integer(percent)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "SetExperienceModifier";
    ctx.kind = script::CallKind::free_function;
    return registry.entry(index).fn(ctx).status;
  };

  // 1-based, and the two arguments are in that order and not the other.
  CHECK(call(1, 250) == script::HostStatus::ok);
  CHECK(combat.player_experience_modifier(0) == 250);
  CHECK(combat.player_experience_modifier(1) == 100);
  // ...and the percentage did not land as a player id.
  CHECK(combat.player_experience_modifier(250 % 16) == 100);

  CHECK(call(16, 40) == script::HostStatus::ok);
  CHECK(combat.player_experience_modifier(15) == 40);

  // Outside 1..16 writes nothing rather than refusing: the original
  // range-checks and returns. **Asserted on the bytes**, because a row filed
  // under `kNoPlayer` answers for nobody and no getter could see it -- it would
  // only show up as a save that no longer matches, which is a desync a year
  // later rather than a failing test now.
  std::vector<std::byte> in_range;
  combat.serialize(in_range);
  CHECK(call(0, 999) == script::HostStatus::ok);
  CHECK(call(17, 999) == script::HostStatus::ok);
  CHECK(call(-3, 999) == script::HostStatus::ok);
  for (PlayerId p = 1; p < 15; ++p) CHECK(combat.player_experience_modifier(p) == 100);
  std::vector<std::byte> after_junk;
  combat.serialize(after_junk);
  CHECK(after_junk == in_range);
}

/// `u.RecalcBonuses()` is a no-op, and the assertion is that it *runs* and
/// changes nothing -- this engine writes the modifiers at the source rather
/// than caching them, so there is nothing to rebuild.
TEST(recalc_bonuses_runs_and_rebuilds_nothing) {
  World world;
  CombatSystem combat = make_system();
  REQUIRE(world.add_system(&combat));
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  REQUIRE(combat.find(1) != nullptr);
  combat.find(1)->attack_bonus = 7;
  combat.find(1)->armour_bonus = 3;

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_combat_host(registry);
  HostContext context;
  context.world = &world;
  const std::uint32_t index = registry.find(script::CallKind::member, "RecalcBonuses", 0);
  REQUIRE(index != script::kUnresolvedHost);
  REQUIRE(registry.entry(index).fn != nullptr);
  std::vector<script::Value> args{script::Value::object(kTypeObj, 1)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "RecalcBonuses";
  ctx.kind = script::CallKind::member;
  CHECK(registry.entry(index).fn(ctx).status == script::HostStatus::ok);
  CHECK(combat.find(1)->attack_bonus == 7);
  CHECK(combat.find(1)->armour_bonus == 3);

  // And a receiver naming no unit runs too, rather than refusing.
  args[0] = script::Value::object(kTypeObj, 9999);
  ctx.arguments = args;
  CHECK(registry.entry(index).fn(ctx).status == script::HostStatus::ok);
}

// ---------------------------------------------------------------------------
// Unit::AddBonus
// ---------------------------------------------------------------------------

namespace {

/// The entry point under its own name, with the receiver and five ints the
/// registration declares. Returns the host status so a refusal is visible.
script::HostStatus call_add_bonus(World& world, ObjectId receiver, std::int32_t a,
                                  std::int32_t b, std::int32_t c, std::int32_t d,
                                  std::int32_t e) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  define_combat_host(registry);
  HostContext context;
  context.world = &world;
  const std::uint32_t index = registry.find(script::CallKind::member, "AddBonus", 5);
  if (index == script::kUnresolvedHost || registry.entry(index).fn == nullptr) {
    return script::HostStatus::error;
  }
  std::vector<script::Value> args{script::Value::object(kTypeObj, receiver),
                                  script::Value::integer(a), script::Value::integer(b),
                                  script::Value::integer(c), script::Value::integer(d),
                                  script::Value::integer(e)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "AddBonus";
  ctx.kind = script::CallKind::member;
  return registry.entry(index).fn(ctx).status;
}

}  // namespace

/// **The shipped call site, and the only one.**
/// `3_Great_Losses_Egypt`'s `seq2.vs` opens with
/// `GoodHero.obj.AsUnit().AddBonus(0, -10, 0, 0, 0)` -- ten points of slash
/// armour taken off the hero. A negative addend is the ordinary case here, not
/// an edge one.
TEST(add_bonus_takes_armour_off_the_hero) {
  World world;
  CombatSystem combat = make_system();
  REQUIRE(world.add_system(&combat));
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  REQUIRE(combat.find(1) != nullptr);
  // `melee(16, 12, 200)`: 12 against both channels.
  CHECK(combat.armour_of(*combat.find(1), DamageType::slash) == 12);

  CHECK(call_add_bonus(world, 1, 0, -10, 0, 0, 0) == script::HostStatus::ok);
  CHECK(combat.find(1)->bonus.armour_slash == -10);
  CHECK(combat.armour_of(*combat.find(1), DamageType::slash) == 2);
  // Only the channel it named. Pierce is untouched, which is the whole point
  // of the record holding two armour numbers rather than one.
  CHECK(combat.armour_of(*combat.find(1), DamageType::pierce) == 12);
  // And a blow lands harder for it: 16 attack against 2 armour rather than 12.
  combat.add(unit(2, kHastatus, 1, 200, Point{20, 0}));
  const DamageBreakdown out = combat.preview(2, 1);
  CHECK(out.after_armour == 14);
}

/// **Argument 5 is never read, and argument 4 lands in both slots.** This is
/// the original's own slip -- `0x005e0ce0` pops four ints where
/// `Unit::RemoveBonus` next door pops five -- and it is faithful on purpose.
/// The shipped site passes 0 for both, so nothing in the installation can tell.
TEST(add_bonus_drops_its_fifth_argument) {
  World world;
  CombatSystem combat = make_system();
  REQUIRE(world.add_system(&combat));
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  REQUIRE(combat.find(1) != nullptr);

  CHECK(call_add_bonus(world, 1, 0, 0, 0, 30, 500) == script::HostStatus::ok);
  const Combatant* held = combat.find(1);
  REQUIRE(held != nullptr);
  CHECK(held->bonus.max_health == 30);
  // 30, not 500: the fourth argument, copied into the fifth slot.
  CHECK(held->bonus.max_stamina == 30);
}

/// Each of the four arguments that *are* read lands on its own stat, and the
/// readers a script uses see it.
TEST(add_bonus_moves_each_stat_it_names) {
  World world;
  CombatSystem combat = make_system();
  REQUIRE(world.add_system(&combat));
  CombatProfile p = melee(16, 12, 200);
  p.max_stamina = 100;
  combat.set_profile(kSwordsman, p);
  combat.add(unit(1, kSwordsman, 0, 200, Point{0, 0}));
  REQUIRE(combat.find(1) != nullptr);

  CHECK(call_add_bonus(world, 1, 5, -3, 7, 40, 0) == script::HostStatus::ok);
  const Combatant* held = combat.find(1);
  REQUIRE(held != nullptr);
  CHECK(combat.attack_of(*held) == 21);                              // 16 + 5
  CHECK(combat.armour_of(*held, DamageType::slash) == 9);            // 12 - 3
  CHECK(combat.armour_of(*held, DamageType::pierce) == 19);          // 12 + 7
  CHECK(combat.max_health_of(*held) == 240);                         // 200 + 40
  CHECK(combat.max_stamina_of(*held) == 140);                        // 100 + 40, see above
  CHECK(combat.max_health(1) == 240);
  // `siege` and `none` meet no armour at all, so neither addend reaches them.
  CHECK(combat.armour_of(*held, DamageType::siege) == 0);
}

/// **It accumulates.** `0x005e0c7b` is the arm taken when a record already
/// exists, and it adds into the stored one rather than replacing it.
TEST(add_bonus_accumulates_over_calls) {
  World world;
  CombatSystem combat = make_system();
  REQUIRE(world.add_system(&combat));
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));

  CHECK(call_add_bonus(world, 1, 5, 0, 0, 0, 0) == script::HostStatus::ok);
  CHECK(call_add_bonus(world, 1, 5, 0, 0, 0, 0) == script::HostStatus::ok);
  CHECK(call_add_bonus(world, 1, -2, 0, 0, 0, 0) == script::HostStatus::ok);
  REQUIRE(combat.find(1) != nullptr);
  CHECK(combat.find(1)->bonus.attack == 8);
  CHECK(combat.attack_of(*combat.find(1)) == 24);
}

/// A receiver this system has no combatant for is a no-op rather than a trap:
/// the original prints its invalid-object line and returns.
TEST(add_bonus_ignores_an_unknown_receiver) {
  World world;
  CombatSystem combat = make_system();
  REQUIRE(world.add_system(&combat));
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));

  std::vector<std::byte> before;
  combat.serialize(before);
  CHECK(call_add_bonus(world, 9999, 5, 5, 5, 5, 5) == script::HostStatus::ok);
  std::vector<std::byte> after;
  combat.serialize(after);
  CHECK(after == before);
  CHECK(!combat.add_bonus(9999, StatBonus{}));
}

/// **`RemoveBonus` floors each addend at zero and `AddBonus` does not**, which
/// is not symmetric and means any removal wipes a negative bonus.
TEST(remove_bonus_floors_at_zero) {
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  REQUIRE(combat.find(1) != nullptr);

  StatBonus put;
  put.attack = 10;
  put.armour_slash = -10;
  CHECK(combat.add_bonus(1, put));
  CHECK(combat.find(1)->bonus.attack == 10);
  CHECK(combat.find(1)->bonus.armour_slash == -10);

  StatBonus take;
  take.attack = 4;
  CHECK(combat.remove_bonus(1, take));
  CHECK(combat.find(1)->bonus.attack == 6);
  // Nothing was subtracted from the armour and it moved anyway: the floor ran
  // over every one of the five, not only the one the caller named.
  CHECK(combat.find(1)->bonus.armour_slash == 0);

  // Over-subtracting stops at zero rather than going negative.
  take.attack = 100;
  CHECK(combat.remove_bonus(1, take));
  CHECK(combat.find(1)->bonus.attack == 0);
  CHECK(combat.find(1)->bonus.empty());
  CHECK(!combat.remove_bonus(9999, take));
}

/// **A Mutate drops the record**, because the original mutates by minting a
/// fresh object and the table is keyed on the id that goes with the old one.
TEST(add_bonus_does_not_survive_a_mutate) {
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  StatBonus put;
  put.attack = 9;
  put.max_health = 50;
  CHECK(combat.add_bonus(1, put));
  CHECK(combat.attack_of(*combat.find(1)) == 25);

  CHECK(combat.reclass(1, kSwordsman));
  REQUIRE(combat.find(1) != nullptr);
  CHECK(combat.find(1)->bonus.empty());
  CHECK(combat.attack_of(*combat.find(1)) == 12);   // the new class, and nothing on top
  CHECK(combat.max_health_of(*combat.find(1)) == 200);
}

/// The maximum moves and the current health does not, which is what the
/// original's rebuild does: it writes `[obj+0xc8]` and never touches
/// `[obj+0xc0]`. So a bonus does not heal, and a *negative* one leaves a unit
/// standing above its own maximum until something else clamps it.
TEST(add_bonus_moves_the_maximum_and_not_the_health) {
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  StatBonus put;
  put.max_health = 100;
  CHECK(combat.add_bonus(1, put));
  CHECK(combat.find(1)->health == 200);
  CHECK(combat.max_health(1) == 300);
  // ...and the raised maximum is what `Heal` now fills to.
  CHECK(combat.heal(1, 1000) == 100);
  CHECK(combat.find(1)->health == 300);

  StatBonus down;
  down.max_health = -250;
  CHECK(combat.add_bonus(1, down));
  CHECK(combat.max_health(1) == 50);
  CHECK(combat.find(1)->health == 300);  // left standing over its own maximum

  // And the next `Heal` is what pulls it down, because `heal` clamps to the
  // cap after adding rather than refusing when it is already above it. So a
  // negative maximum does not cost health at the moment it lands; it costs it
  // the first time anything heals the unit at all, by 250 for an offer of 10.
  CHECK(combat.heal(1, 10) == -250);
  CHECK(combat.find(1)->health == 50);
}

/// The record reaches the blow, on both sides of it.
///
/// The attacker's addend is `[obj+0xdc]`, which is the number the original's
/// damage step loads; the defender's is `[obj+0xe4]` or `[obj+0xe8]` by
/// channel. Neither is the class property any more once a script has moved it.
TEST(add_bonus_reaches_the_damage_formula_from_both_sides) {
  CombatSystem combat = make_system();
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));    // 16 attack
  combat.add(unit(2, kSwordsman, 1, 200, Point{20, 0}));  // 6 armour
  CHECK(combat.preview(1, 2).after_armour == 10);

  StatBonus sharper;
  sharper.attack = 9;
  CHECK(combat.add_bonus(1, sharper));
  CHECK(combat.preview(1, 2).after_armour == 19);

  StatBonus tougher;
  tougher.armour_slash = 5;
  CHECK(combat.add_bonus(2, tougher));
  CHECK(combat.preview(1, 2).after_armour == 14);
  // Still the subtraction rather than `MinPercentOfAttackersDamage`'s floor,
  // which would make the two sides indistinguishable.
  CHECK(!combat.preview(1, 2).floored_by_min_percent);

  // `melee` gives both channels the same armour, so the pierce addend is what
  // separates the two: a slash attacker does not meet it.
  StatBonus pierce_only;
  pierce_only.armour_pierce = 1000;
  CHECK(combat.add_bonus(2, pierce_only));
  CHECK(combat.preview(1, 2).after_armour == 14);
}

/// A shot in the air carries the attack it was launched with -- that is
/// `Projectile::attack`, settled at launch -- but the armour it meets is read
/// at impact, so a bonus that lands mid-flight still counts.
TEST(add_bonus_reaches_a_projectile_at_impact) {
  World world;
  CombatSystem combat = make_system();
  CombatProfile archer = melee(60, 0, 150);
  archer.damage_type = DamageType::pierce;
  archer.range = 500;
  archer.sight = 600;
  archer.projectile = 99;
  archer.attack_interval = 2000;
  combat.set_profile(kArcher, archer);
  combat.add(unit(1, kArcher, 0, 150, Point{0, 0}));
  combat.add(unit(2, kSwordsman, 1, 2000, Point{400, 0}));  // 6 against pierce
  combat.order_attack(1, 2);

  world.advance(100);
  combat.advance(world, world.turn());
  REQUIRE(combat.projectiles().size() == 1);
  // Ten points of pierce armour, put on after the arrow left the bow.
  StatBonus tougher;
  tougher.armour_pierce = 10;
  CHECK(combat.add_bonus(2, tougher));

  const std::int32_t before = combat.health(2);
  for (int turn = 0; turn < 10 && combat.health(2) == before; ++turn) {
    world.advance(100);
    combat.advance(world, world.turn());
  }
  const std::int32_t with_bonus = before - combat.health(2);
  CHECK(with_bonus > 0);

  // The same shot against the same unit with no addend, for the comparison.
  CombatSystem plain = make_system();
  plain.set_profile(kArcher, archer);
  World plain_world;
  plain.add(unit(1, kArcher, 0, 150, Point{0, 0}));
  plain.add(unit(2, kSwordsman, 1, 2000, Point{400, 0}));
  plain.order_attack(1, 2);
  const std::int32_t plain_before = plain.health(2);
  for (int turn = 0; turn < 11 && plain.health(2) == plain_before; ++turn) {
    plain_world.advance(100);
    plain.advance(plain_world, plain_world.turn());
  }
  CHECK(plain_before - plain.health(2) == with_bonus + 10);
}

/// `HealStamina` fills to the raised maximum, and the maximum is the *stamina*
/// addend -- which is the one place the two halves of the fourth argument can
/// be told apart, because nothing that goes through the entry point can make
/// them differ.
TEST(add_bonus_raises_what_heal_stamina_fills_to) {
  World world;
  CombatSystem combat;
  CombatProfile p = melee(16, 12, 200);
  p.max_stamina = 100;
  combat.set_profile(kHastatus, p);
  REQUIRE(world.add_system(&combat));
  // A world object as well as a combatant: `HealStamina` fans out through
  // `receiver_objects`, which resolves through the world.
  REQUIRE(world.spawn(NativeClass::unit, nullptr) == 1);
  combat.add(unit(1, kHastatus, 0, 200, Point{0, 0}));
  REQUIRE(combat.find(1) != nullptr);
  combat.find(1)->stamina = 0;

  StatBonus put;
  put.max_health = 7;   // deliberately different from the stamina addend
  put.max_stamina = 60;
  CHECK(combat.add_bonus(1, put));
  CHECK(combat.max_stamina_of(*combat.find(1)) == 160);
  CHECK(combat.max_health_of(*combat.find(1)) == 207);

  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_objlist_host(registry);
  define_combat_host(registry);
  HostContext context;
  context.world = &world;
  context.object_type = kTypeObj;
  const std::uint32_t index = registry.find(script::CallKind::member, "HealStamina", 1);
  REQUIRE(index != script::kUnresolvedHost);
  REQUIRE(registry.entry(index).fn != nullptr);
  std::vector<script::Value> args{script::Value::object(kTypeObj, 1),
                                  script::Value::integer(1000)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "HealStamina";
  ctx.kind = script::CallKind::member;
  CHECK(registry.entry(index).fn(ctx).status == script::HostStatus::ok);
  CHECK(combat.find(1)->stamina == 160);
}
