// Settlement economy tests.
//
// No game data. Every number a fixture is built around is measured, and the
// comment beside it says from what: `DATA\CONST.INI` for the balance
// constants, `DATA\CLASSES\*.SC.XML` for the class properties, the shipped
// `DATA\SUBAI\*.vs` behaviours for the rules, and the nine
// `Logs/*/desync.txt` dumps for the shapes the state has to take.
//
// The test that matters most is the last one. The turn length is renegotiated
// mid-session -- 200, 400, 799 and 800 all occur -- so an interval cannot be a
// turn count, and it cannot be a per-turn accumulator either: a settlement has
// ten intervals and a long turn would run all of one before any of another
// while short turns interleave them. **Any partition of an interval must leave
// the economy in bit-identical state.** If that fails, the conformance harness
// is worthless until it is found.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/array.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/item.hpp"
#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/rng.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/orders.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

/// A receiver naming no settlement answers the type's zero -- `Settlement::
/// health` (0x005c1e63) prints and pushes 0, `IsStronghold` (0x0042508b)
/// pushes false -- and writes nothing. Every settlement member here that used
/// to assert a refusal asserts this instead.
[[nodiscard]] bool zero_answer(const script::HostOutcome& out) {
  return out.status == script::HostStatus::ok &&
         (!out.value.is_integer() || out.value.as_integer() == 0) && !out.value.is_object();
}

/// `BaseTownhall`: `produces_gold=1`, `produces_food=0`, `efficiency=1`,
/// `population=40`, `max_population=100`, `settlement_gold=2500`,
/// `settlement_food=200`, `settlement_maxgold=100000000`. `foodperpop` is 45 or
/// 100 depending on race; `ITownhall` and `TTownhall` ship 45.
SettlementInit town_hall() {
  SettlementInit init;
  init.kind = SettlementKind::stronghold;
  init.can_be_captured = true;
  init.can_be_attacked = true;
  init.produces_gold = true;
  init.efficiency = 1;
  init.food_per_pop = 45;
  init.population = 40;
  init.max_population = 100;
  init.gold = 2500;
  init.food = 200;
  init.max_gold = 100000000;
  init.max_food = 100000000;
  init.max_units = 10000;
  return init;
}

/// `BaseVillage`: `produces_food=1`, `efficiency=1`, `population=12`,
/// `max_population=20`, `settlement_food=100`, `settlement_maxfood=5000`,
/// `max_units=0`.
SettlementInit village() {
  SettlementInit init;
  init.kind = SettlementKind::village;
  init.can_be_captured = true;
  init.can_be_attacked = true;
  init.produces_food = true;
  init.efficiency = 1;
  init.food_per_pop = 45;
  init.population = 12;
  init.max_population = 20;
  init.food = 100;
  init.max_gold = 5000;
  init.max_food = 5000;
  return init;
}

/// Run `total` game-time units as turns of `length`, plus whatever is left.
void run(EconomySystem& economy, World& world, std::int32_t length, std::int64_t total) {
  std::int64_t elapsed = 0;
  while (elapsed < total) {
    const std::int32_t step =
        static_cast<std::int32_t>(std::min<std::int64_t>(length, total - elapsed));
    world.advance(step);
    economy.advance(world, world.turn());
    elapsed += step;
  }
}

std::uint64_t economy_hash(const EconomySystem& economy) {
  std::uint64_t h = 1469598103934665603ull;
  economy.hash(h);
  return h;
}

}  // namespace

// --------------------------------------------------------------------------
// the production rate quad
// --------------------------------------------------------------------------

/// The dumps print `capture/attack/gold/food` with exactly four non-zero rate
/// pairs over 328 settlements, and this is all four.
TEST(economy_production_rates_match_the_dumps) {
  EconomyRules rules;
  std::int32_t gold = -1;
  std::int32_t food = -1;

  settlement_production_rates(rules, true, false, gold, food);  // every town hall
  CHECK(gold == 24);
  CHECK(food == 0);

  settlement_production_rates(rules, false, true, gold, food);  // every village
  CHECK(gold == 0);
  CHECK(food == 24);

  settlement_production_rates(rules, false, false, gold, food);  // ruins, outposts
  CHECK(gold == 0);
  CHECK(food == 0);

  // `TTownhall` is the only shipped class that sets both flags, and the only
  // settlement the dumps print as `1 1 12 24`.
  settlement_production_rates(rules, true, true, gold, food);
  CHECK(gold == 12);
  CHECK(food == 24);
}

// --------------------------------------------------------------------------
// production
// --------------------------------------------------------------------------

/// `ProductionGoldBase = 24 ;percent from population` every
/// `ProductionInterval = 2000`, truncating: 40 x 24 / 100 = 9.6 -> 9.
TEST(economy_production_is_an_integer_percentage_of_population) {
  World world;
  EconomySystem economy;
  const SettlementId id = economy.create(town_hall());
  economy.set_resource(id, Resource::gold, 0);
  economy.start(world);

  run(economy, world, 400, 2000);
  CHECK(economy.resource(id, Resource::gold) == 9);

  run(economy, world, 400, 2000);
  CHECK(economy.resource(id, Resource::gold) == 18);
}

/// A population small enough that the percentage truncates to nothing produces
/// nothing: 3 x 24 / 100 = 0, not 1. Rounding up here would give a starving
/// village a free income.
TEST(economy_production_truncates_to_zero) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.population = 3;
  init.gold = 0;
  const SettlementId id = economy.create(init);
  economy.start(world);

  run(economy, world, 800, 20000);  // ten production intervals
  CHECK(economy.resource(id, Resource::gold) == 0);
}

/// `efficiency` is 0 on `Outpost`, `BaseShipyard` and `BaseRuins`, and those
/// produce nothing however large their population.
TEST(economy_zero_efficiency_produces_nothing) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.efficiency = 0;
  init.gold = 0;
  const SettlementId id = economy.create(init);
  economy.start(world);

  run(economy, world, 400, 10000);
  CHECK(economy.resource(id, Resource::gold) == 0);
}

/// `settlement_maxgold` / `settlement_maxfood` are hard caps, and a zero cap --
/// which is what `Ruins1` and `Teleport` ship -- means the store holds nothing.
TEST(economy_production_saturates_at_the_cap) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.gold = 0;
  init.max_gold = 20;
  const SettlementId id = economy.create(init);
  economy.start(world);

  run(economy, world, 400, 100000);
  CHECK(economy.resource(id, Resource::gold) == 20);

  SettlementInit ruin = town_hall();
  ruin.gold = 0;
  ruin.max_gold = 0;
  const SettlementId ruin_id = economy.create(ruin);
  run(economy, world, 400, 10000);
  CHECK(economy.resource(ruin_id, Resource::gold) == 0);
}

// --------------------------------------------------------------------------
// population
// --------------------------------------------------------------------------

/// `PopulationGrowthRate = 1` per `PopulationGrowthInterval = 20000`, and the
/// boundary is exact: nothing at 19,999, one head at 20,000.
TEST(economy_growth_fires_on_the_interval_boundary) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.food = 100000;  // never short
  const SettlementId id = economy.create(init);
  economy.start(world);

  run(economy, world, 199, 19999);
  CHECK(economy.find(id)->population == 40);

  run(economy, world, 1, 1);
  CHECK(economy.find(id)->population == 41);
}

TEST(economy_growth_stops_at_max_population) {
  World world;
  EconomySystem economy;
  SettlementInit init = village();
  init.population = 19;
  init.max_population = 20;
  init.food = 100000;
  const SettlementId id = economy.create(init);
  economy.start(world);

  run(economy, world, 800, 200000);  // ten growth intervals
  CHECK(economy.find(id)->population == 20);
}

/// gbr.exe's growth tick (0x005c0f60) reads no food: a town hall with an empty
/// store grows like a full one, and `foodperpop` is never charged. Charging it
/// was an inference from the property's name, and it starved every town hall
/// the AI fed by wagon alone.
TEST(economy_growth_costs_no_food) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.food = 0;
  const SettlementId id = economy.create(init);
  economy.start(world);

  run(economy, world, 400, 40000);
  CHECK(economy.find(id)->population == 42);
  CHECK(economy.resource(id, Resource::food) == 0);
}

/// The same tick skips a population below `MinPopulation = 10`, and every
/// settlement of player 14 or 15.
TEST(economy_growth_skips_a_population_below_the_floor_and_the_neutral_players) {
  World world;
  EconomySystem economy;
  SettlementInit low = town_hall();
  low.population = 9;
  const SettlementId low_id = economy.create(low);
  SettlementInit wild = town_hall();
  wild.owner = kNeutralWildlife;
  const SettlementId wild_id = economy.create(wild);
  SettlementInit passive = village();
  passive.owner = kNeutralPassive;
  const SettlementId passive_id = economy.create(passive);
  SettlementInit floor = town_hall();
  floor.population = 10;
  const SettlementId floor_id = economy.create(floor);
  economy.start(world);

  run(economy, world, 400, 40000);
  CHECK(economy.find(low_id)->population == 9);
  CHECK(economy.find(wild_id)->population == 40);
  CHECK(economy.find(passive_id)->population == 12);
  CHECK(economy.find(floor_id)->population == 12);
}

/// `PopulationDecreasePercent = 10` per `PopulationDecreaseInterval = 4000`,
/// read off the decrease tick (0x005c0fb0): it trims a tenth of the excess
/// over `max_population`, at least one head, and stops at the maximum.
TEST(economy_decrease_trims_the_excess_over_max_population) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.population = 200;
  init.food = 0;
  init.produces_gold = false;  // nothing to distract the arithmetic
  const SettlementId id = economy.create(init);
  SettlementInit dump = town_hall();  // the late dumps' `165 150`
  dump.population = 165;
  dump.max_population = 150;
  const SettlementId dump_id = economy.create(dump);
  economy.start(world);

  run(economy, world, 400, 4000);
  CHECK(economy.find(id)->population == 190);       // 200 - (200 - 100) * 10 / 100
  CHECK(economy.find(dump_id)->population == 164);  // 15 * 10 / 100 = 1, the floor

  run(economy, world, 400, 4000);
  CHECK(economy.find(id)->population == 181);  // 190 - 9

  run(economy, world, 400, 400000);
  CHECK(economy.find(id)->population == 100);  // the maximum, and no lower
  CHECK(economy.find(dump_id)->population == 150);
}

/// The decrease is not starvation: a town hall at or under its maximum keeps
/// its people with nothing in the store, for as long as it stands.
TEST(economy_an_empty_store_costs_no_population) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.population = 60;
  init.food = 0;
  const SettlementId id = economy.create(init);
  economy.start(world);

  run(economy, world, 400, 400000);
  CHECK(economy.find(id)->population == 80);  // twenty growth intervals, no losses
}

/// The production tick (0x005c1122) gives player 14 and 15 nothing, and makes
/// food only from a population at `MinPopulation` or above; gold has no floor.
TEST(economy_production_skips_the_neutral_players_and_food_below_the_floor) {
  World world;
  EconomySystem economy;
  SettlementInit wild = town_hall();
  wild.owner = kNeutralPassive;
  wild.gold = 0;
  const SettlementId wild_id = economy.create(wild);
  SettlementInit small = village();
  small.population = 9;
  small.food = 0;
  const SettlementId small_id = economy.create(small);
  economy.start(world);

  run(economy, world, 400, 20000);
  CHECK(economy.resource(wild_id, Resource::gold) == 0);
  CHECK(economy.resource(small_id, Resource::food) == 0);
}

// --------------------------------------------------------------------------
// command costs
// --------------------------------------------------------------------------

/// The test `VERIFY_CMDCOST_BUILDING.VS` and its four siblings share:
/// `cmdcost_pop > 0 && cmdcost_pop + MinPopulation > population`.
TEST(economy_spending_respects_the_min_population_floor) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.population = 14;
  init.gold = 1000;
  init.food = 1000;
  const SettlementId id = economy.create(init);
  economy.start(world);

  // 14 population, floor 10: four heads are spendable, five are not.
  CHECK(economy.can_afford(id, 0, 0, 4) == SpendResult::ok);
  CHECK(economy.can_afford(id, 0, 0, 5) == SpendResult::not_enough_population);
  CHECK(economy.can_afford(id, 2000, 0, 0) == SpendResult::not_enough_gold);
  CHECK(economy.can_afford(id, 0, 2000, 0) == SpendResult::not_enough_food);

  CHECK(economy.spend(id, 100, 50, 4) == SpendResult::ok);
  CHECK(economy.resource(id, Resource::gold) == 900);
  CHECK(economy.resource(id, Resource::food) == 950);
  CHECK(economy.find(id)->population == 10);

  // A refused spend charges nothing.
  CHECK(economy.spend(id, 100, 50, 1) == SpendResult::not_enough_population);
  CHECK(economy.resource(id, Resource::gold) == 900);
}

// --------------------------------------------------------------------------
// transport
// --------------------------------------------------------------------------

/// Supply is the village's behaviour, not the economy's. `VILLAGE_BEHAVIOR_
/// SUPPLY.VS` is a class `<behavior>` of `BaseVillage` and its heirs, started
/// on the object like every other (`start_behaviors`), and it sends the mules
/// through `CreateMuleGold`/`CreateMuleFood`. The economy used to run a copy
/// of it on a settlement timer; with the script running too, a village would
/// have shipped twice. So a supplied village left to the economy alone ships
/// nothing, however long it waits.
TEST(economy_leaves_supply_to_the_village_behaviour) {
  World world;
  EconomySystem economy;
  const SettlementId source = economy.create(village());
  const SettlementId sink = economy.create(town_hall());
  economy.set_supplied(source, sink);
  economy.find(source)->food_rate = 0;
  economy.set_resource(source, Resource::food, 2000);
  economy.start(world);

  run(economy, world, 400, 30000);
  CHECK(economy.wagons().empty());
  // The host side the script uses still does what it did.
  const std::int32_t before = economy.resource(source, Resource::food);
  REQUIRE(before > 1000);
  CHECK(economy.create_mule(source, sink, Resource::food, 1000) != 0);
  CHECK(economy.resource(source, Resource::food) == before - 1000);
  CHECK(economy.wagons().size() == 1);
}

/// `WagonBuildTime = 7000`: the cargo leaves the source at once and arrives at
/// the destination one build time later, capped by the destination's store.
TEST(economy_wagon_delivers_after_the_build_time) {
  World world;
  EconomySystem economy;
  const SettlementId source = economy.create(village());
  const SettlementId sink = economy.create(town_hall());
  economy.find(source)->food_rate = 0;
  economy.find(sink)->gold_rate = 0;
  economy.set_resource(sink, Resource::food, 0);
  economy.start(world);

  CHECK(economy.create_mule(source, sink, Resource::food, 500) != 0);
  CHECK(economy.resource(source, Resource::food) == 0);  // village starts at 100

  run(economy, world, 400, 6800);
  CHECK(economy.wagons().size() == 1);
  CHECK(economy.resource(sink, Resource::food) == 0);

  run(economy, world, 400, 200);
  CHECK(economy.wagons().empty());
  CHECK(economy.resource(sink, Resource::food) == 100);  // the village had 100
}

/// A wagon lost on the road takes its cargo with it. That is the whole point of
/// carrying resources on an object rather than teleporting them.
TEST(economy_lost_wagon_loses_its_cargo) {
  World world;
  EconomySystem economy;
  economy.set_auto_deliver(false);
  const SettlementId source = economy.create(town_hall());
  const SettlementId sink = economy.create(town_hall());
  economy.find(sink)->gold_rate = 0;  // hold the destination's store still
  economy.set_resource(sink, Resource::gold, 0);
  economy.start(world);

  const std::uint32_t wagon = economy.create_mule(source, sink, Resource::gold, 1000);
  REQUIRE(wagon != 0);
  run(economy, world, 800, 8000);
  REQUIRE(economy.wagons().size() == 1);
  CHECK(economy.wagons()[0].ready());
  CHECK(economy.lose_wagon(wagon));
  CHECK(economy.resource(sink, Resource::gold) == 0);
  CHECK(economy.find_wagon(wagon) == nullptr);
}

/// `Squad::SendFoodWagon`'s wagon: loaded now, built later, and then **never
/// delivered** -- it stays in the table as the larder of the unit it follows,
/// and only the feeder's draw empties it. See `Wagon::follow`.
TEST(economy_feeding_wagon_is_a_larder_and_not_a_delivery) {
  World world;
  EconomySystem economy;
  const SettlementId source = economy.create(town_hall());
  economy.find(source)->gold_rate = 0;
  economy.set_resource(source, Resource::food, 1000);
  economy.start(world);
  constexpr ObjectId kLeader = 41;
  constexpr ObjectId kOther = 42;

  // Nobody to follow is no wagon, and nothing to load is no wagon.
  CHECK(economy.create_feeding_mule(source, kNoObject, 300) == 0);
  CHECK(economy.create_feeding_mule(source, kLeader, 0) == 0);
  CHECK(economy.wagons().empty());

  const std::uint32_t wagon = economy.create_feeding_mule(source, kLeader, 300);
  REQUIRE(wagon != 0);
  CHECK(economy.resource(source, Resource::food) == 700);
  {
    const Wagon* w = economy.find_wagon(wagon);
    REQUIRE(w != nullptr);
    CHECK(w->follow == kLeader);
    CHECK(w->follows());
    CHECK(w->destination == kNoSettlement);
    CHECK(w->resource == Resource::food);
    CHECK(w->amount == 300);
  }
  const ObjectId squad[] = {kOther, kLeader};
  const ObjectId strangers[] = {kOther};
  // Not built yet: nothing to draw.
  CHECK(economy.draw_from_larder(squad, 20) == 0);

  // Built, and still here: a wagon with a unit to follow is not delivered.
  run(economy, world, 400, 7200);
  REQUIRE(economy.wagons().size() == 1);
  CHECK(economy.wagons()[0].ready());
  // Drawn only by a squad the followed unit is in, only by a positive want,
  // and never past what it holds.
  CHECK(economy.draw_from_larder(strangers, 20) == 0);
  CHECK(economy.draw_from_larder(squad, 0) == 0);
  CHECK(economy.draw_from_larder(squad, 20) == 20);
  CHECK(economy.find_wagon(wagon)->amount == 280);
  CHECK(economy.draw_from_larder(squad, 1000) == 280);
  // Emptied, and gone.
  CHECK(economy.find_wagon(wagon) == nullptr);
  CHECK(economy.wagons().empty());

  // A gold wagon following somebody is never a larder.
  const std::uint32_t gold = economy.create_mule(source, kNoSettlement, Resource::gold, 100);
  REQUIRE(gold != 0);
  economy.set_auto_deliver(false);
  run(economy, world, 400, 7200);
  REQUIRE(economy.wagons().size() == 1);
  CHECK(economy.draw_from_larder(squad, 20) == 0);
}

/// The unit a wagon follows is world state: two peers that disagree about it
/// disagree about which army eats next week.
TEST(economy_hash_covers_the_unit_a_wagon_follows) {
  World world;
  EconomySystem a, b;
  const SettlementId sa = a.create(town_hall());
  const SettlementId sb = b.create(town_hall());
  a.set_resource(sa, Resource::food, 1000);
  b.set_resource(sb, Resource::food, 1000);
  a.start(world);
  b.start(world);
  REQUIRE(a.create_feeding_mule(sa, 41, 300) != 0);
  REQUIRE(b.create_feeding_mule(sb, 42, 300) != 0);
  std::uint64_t ha = 1469598103934665603ull, hb = 1469598103934665603ull;
  a.hash(ha);
  b.hash(hb);
  CHECK(ha != hb);
}

// --------------------------------------------------------------------------
// capture and loyalty
// --------------------------------------------------------------------------

/// `LoyaltySettlementsInitial = 10` at creation; `DecreaseLoyalty(1)` per
/// capture beat; `SetPlayer` then `SetLoyalty(11)` at zero, which is
/// `UNIT_CAPTURE.VS` line for line.
TEST(economy_capture_needs_loyalty_at_zero) {
  World world;
  EconomySystem economy;
  const SettlementId id = economy.create(town_hall());
  economy.start(world);
  CHECK(economy.find(id)->loyalty == 10);

  CHECK(!economy.capture(id, 3));  // loyalty is not zero yet

  for (int i = 0; i < 10; ++i) {
    economy.decrease_loyalty(id, 1);
    // The cap window has to roll over, or the eleventh point cannot be taken.
    run(economy, world, 400, 2000);
  }
  CHECK(economy.find(id)->loyalty == 0);

  CHECK(economy.capture(id, 3));
  CHECK(economy.find(id)->owner == 3);
  CHECK(economy.find(id)->loyalty == 11);
}

/// `LoyaltyChangeCap = 10` per `LoyaltyCapInterval = 2000` -- the constant's own
/// comment says "No more than … decreased in LoyaltyCapInterval".
TEST(economy_loyalty_decrease_is_capped_per_window) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  const SettlementId id = economy.create(init);
  economy.set_loyalty(id, 100);
  economy.start(world);

  CHECK(economy.decrease_loyalty(id, 40) == 10);
  CHECK(economy.decrease_loyalty(id, 40) == 0);
  CHECK(economy.find(id)->loyalty == 90);

  run(economy, world, 400, 2000);  // window rolls over
  CHECK(economy.decrease_loyalty(id, 40) == 10);
  CHECK(economy.find(id)->loyalty == 80);
}

/// `LoyaltyIncreasePerUnit = 10` per garrisoned unit, which is what makes
/// `LoyaltyUnitsOutTreshold = 151` a number rather than a mystery: a settlement
/// holding fifteen units sits above it.
TEST(economy_garrison_raises_loyalty) {
  World world;
  EconomySystem economy;
  const SettlementId id = economy.create(town_hall());
  economy.start(world);

  for (ObjectId u = 1; u <= 15; ++u) CHECK(economy.garrison_add(id, u));
  CHECK(economy.find(id)->holder.count() == 15);
  CHECK(economy.find(id)->loyalty == 10 + 15 * 10);
  CHECK(economy.find(id)->loyalty > economy.rules().loyalty_units_out_threshold);

  CHECK(economy.garrison_remove(id, 7));
  CHECK(economy.find(id)->loyalty == 150);
}

/// `max_units` on the holder: `AddUnit` refuses past it, `ForceAddUnit` does
/// not -- which is how an outpost places its whole garrison and how a Teuton
/// tent admits units it "does not allow inside".
TEST(economy_holder_cap_binds_add_but_not_force_add) {
  World world;
  EconomySystem economy;
  SettlementInit init = village();  // max_units = 0
  const SettlementId id = economy.create(init);
  economy.start(world);

  CHECK(!economy.garrison_add(id, 1));
  CHECK(economy.garrison_force_add(id, 1));
  CHECK(economy.find(id)->holder.count() == 1);
}

/// `capture_health_percent`: 100 on `Building` (always capturable), 50 on
/// `Outpost` (only once it is half wrecked). INFERRED reading, flagged as such.
TEST(economy_capture_health_percent_gates_the_target) {
  World world;
  EconomySystem economy;
  const ObjectId anchor = world.spawn(NativeClass::outpost, nullptr);
  REQUIRE(world.find(anchor) != nullptr);
  world.set_health(anchor, 5000);

  SettlementInit init;
  init.kind = SettlementKind::outpost;
  init.anchor = anchor;
  init.anchor_max_health = 5000;  // Outpost's `maxhealth`
  init.can_be_captured = true;
  init.capture_health_percent = 50;
  const SettlementId id = economy.create(world, init);
  economy.start(world);

  CHECK(!economy.is_valid_capture_target(world, id));
  world.set_health(anchor, 2500);
  CHECK(economy.is_valid_capture_target(world, id));
}

// --------------------------------------------------------------------------
// repair, burning, sentries
// --------------------------------------------------------------------------

/// `PopulationRepairBase = 2` per head every `RepairInterval = 2000`, applied to
/// one building at a time with `first to repair` as the cursor.
TEST(economy_repair_heals_one_building_per_interval_in_turn) {
  World world;
  EconomySystem economy;
  const ObjectId a = world.spawn(NativeClass::building, nullptr);
  const ObjectId b = world.spawn(NativeClass::building, nullptr);
  for (ObjectId id : {a, b}) {
    REQUIRE(world.state(id) != nullptr);
    world.set_health(id, 1000);
  }

  SettlementInit init = town_hall();
  init.anchor = a;
  init.anchor_max_health = 5000;
  init.population = 10;  // budget 20 per interval
  const SettlementId id = economy.create(world, init);
  economy.add_building(world, id, b, 5000);
  economy.start(world);

  run(economy, world, 400, 2000);
  CHECK(world.state(a)->health == 1020);
  CHECK(world.state(b)->health == 1000);

  run(economy, world, 400, 2000);
  CHECK(world.state(a)->health == 1020);
  CHECK(world.state(b)->health == 1020);
}

/// `BurnTime = 500`, `GoldBurnAmount = 20`, `FoodBurnAmount = 20`.
TEST(economy_burning_costs_twenty_of_each_every_five_hundred) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.produces_gold = false;
  init.gold = 100;
  init.food = 100;
  const SettlementId id = economy.create(init);
  economy.set_burning(id, true);
  economy.start(world);

  run(economy, world, 400, 1000);  // two burn beats
  CHECK(economy.resource(id, Resource::gold) == 60);
  CHECK(economy.resource(id, Resource::food) == 60);

  run(economy, world, 400, 4000);  // and then the store is empty, not negative
  CHECK(economy.resource(id, Resource::gold) == 0);
  CHECK(economy.resource(id, Resource::food) == 0);
}

/// Sentries are the town hall's behaviour. `TOWNHALL_SENTRIES_CONTROL.VS`
/// adds `InitialStrongholdSentries` to the maximum once it runs and four every
/// `SentriesAddTime` after, through `AddMaxSentries` and `AddSentries`; the
/// economy used to do both itself as well, so a town hall running its
/// behaviour ended up with twice the wall. Left to the economy, a stronghold
/// has the map's `extrasentries` for a maximum and grows no sentries.
TEST(economy_leaves_sentries_to_the_town_halls_behaviour) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.extra_sentries = 2;
  const SettlementId id = economy.create(init);
  economy.start(world);
  CHECK(economy.find(id)->max_sentries == 2);
  run(economy, world, 800, 30000 * 3);
  CHECK(economy.find(id)->sentries == 0);
  // What the script calls, still answering as it did.
  CHECK(economy.add_max_sentries(id, 20));
  CHECK(economy.find(id)->max_sentries == 22);
  CHECK(economy.add_sentries(id, 4));
  CHECK(economy.find(id)->sentries == 4);
  CHECK(economy.find(id)->sentries_ready == 4);
}

// --------------------------------------------------------------------------
// trade
// --------------------------------------------------------------------------

/// `TAVERN_GETLOAN.VS` and `TAVERN_REPAYLOAN.VS`, both line for line. The loan
/// is *assigned* rather than added, which is what `SetLoan(LoanAmount)` does.
TEST(economy_tavern_loan_matches_the_script) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.gold = 0;
  const SettlementId id = economy.create(init);
  economy.start(world);

  CHECK(economy.take_loan(id));
  CHECK(economy.resource(id, Resource::gold) == 4000);  // LoanAmount
  CHECK(economy.find(id)->loan == 4000);

  economy.set_resource(id, Resource::gold, 1000);
  CHECK(economy.repay_loan(id));  // gold <= loan: pay what there is
  CHECK(economy.resource(id, Resource::gold) == 0);
  CHECK(economy.find(id)->loan == 3000);

  economy.set_resource(id, Resource::gold, 5000);
  CHECK(economy.repay_loan(id));  // gold > loan: clear it
  CHECK(economy.resource(id, Resource::gold) == 2000);
  CHECK(economy.find(id)->loan == 0);
}

/// `LoanInterestPercent = 10` every `TimeProductionForLoan = 180000`. INFERRED:
/// those two constants are the only loan values no script reads.
TEST(economy_loan_accrues_interest) {
  World world;
  EconomySystem economy;
  const SettlementId id = economy.create(town_hall());
  economy.start(world);
  CHECK(economy.take_loan(id));

  run(economy, world, 800, 180000);
  CHECK(economy.find(id)->loan == 4400);
}

/// `TAVERN_INVESTMENT.VS`: 4,000 out, `InvestmentGoldRevenue = 6000` back.
TEST(economy_investment_pays_two_thousand) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.gold = 4000;
  const SettlementId id = economy.create(init);
  economy.start(world);

  CHECK(economy.invest(id));
  CHECK(economy.resource(id, Resource::gold) == 6000);
  CHECK(economy.gold_converted(id) == 2000);

  economy.set_resource(id, Resource::gold, 100);
  CHECK(!economy.invest(id));  // cannot pay: nothing happens
  CHECK(economy.resource(id, Resource::gold) == 100);
}

/// `EMARKET_BUYSLAVES.VS`: `SetPopulation(max_population)` for `SlaveryGold`.
TEST(economy_buying_slaves_fills_the_settlement) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.population = 12;
  init.gold = 2000;
  const SettlementId id = economy.create(init);
  economy.start(world);

  CHECK(economy.buy_slaves(id));
  CHECK(economy.find(id)->population == 100);
  CHECK(economy.resource(id, Resource::gold) == 0);
}

/// The three population writers disagree about the ceiling, and each one is
/// transcribed rather than tidied.
///
/// `SetPopulation` clamps (`gbr.exe` 0x005c2200), `AddToPopulation` does not
/// (0x005c2250 is one `add`), and `AddToMaxPopulation` moves the ceiling
/// itself (0x005c2290, also one `add`). The late desync dumps show settlements
/// at `101 100` and `165 150`, which is what an uncapped `AddToPopulation`
/// produces and is *not* evidence that `SetPopulation` runs free -- reading it
/// that way is what left the clamp out for as long as it was out.
///
/// `ONFINISH_RESEARCH.VS` is where all three meet: "Free Beer" is
/// `SetPopulation(population + 20)` and "Housing" is `AddToMaxPopulation(20)`.
/// Without the ceiling the first is unbounded growth; without the second the
/// researchable does nothing at all.
TEST(economy_the_three_population_writers_disagree_about_the_ceiling) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();  // max_population = 100
  init.population = 90;
  const SettlementId id = economy.create(init);
  economy.start(world);
  const Settlement* s = economy.find(id);
  REQUIRE(s != nullptr);

  // "Free Beer" stops at the ceiling.
  CHECK(economy.set_population(id, s->population + 20));
  CHECK(s->population == 100);

  // "Housing" raises the ceiling, and then the same call goes further.
  CHECK(economy.add_max_population(id, 20));
  CHECK(s->max_population == 120);
  CHECK(economy.set_population(id, s->population + 20));
  CHECK(s->population == 120);

  // `AddToPopulation` ignores the ceiling in both directions, which is how the
  // dumps got to `101 100` in the first place.
  CHECK(economy.add_population(id, 45));
  CHECK(s->population == 165);
  CHECK(s->max_population == 120);

  // And `SetPopulation` pulls it straight back down to the ceiling.
  CHECK(economy.set_population(id, 200));
  CHECK(s->population == 120);

  // Both floors are this engine's, not the original's; no shipped call site
  // reaches either.
  CHECK(economy.set_population(id, -5));
  CHECK(s->population == 0);
  CHECK(economy.add_max_population(id, -1000));
  CHECK(s->max_population == 0);
  // A ceiling of zero is a real state -- an outpost declares one -- and it
  // pins the population there.
  CHECK(economy.set_population(id, 50));
  CHECK(s->population == 0);

  CHECK(!economy.add_max_population(kNoSettlement, 1));
}

/// `OUTPOST_IDLE.VS`, both branches. `GOutpostFoodSales = 20` of food becomes
/// `GOutpostGoldReturn = 10` gold; `ROutpostGoldReturn = 8` accrues on a
/// deposit of at least `ROutpostRequiredGold = 2000`.
TEST(economy_outpost_trade_converts_on_refresh) {
  World world;
  EconomySystem economy;

  SettlementInit gaul;
  gaul.kind = SettlementKind::outpost;
  gaul.owner = 1;
  gaul.max_gold = 10000;
  gaul.max_food = 10000;
  gaul.food = 100;
  const SettlementId seller = economy.create(gaul);
  economy.find(seller)->outpost_trade = OutpostTrade::sell_food;

  SettlementInit roman = gaul;
  roman.food = 0;
  roman.gold = 2000;
  const SettlementId banker = economy.create(roman);
  economy.find(banker)->outpost_trade = OutpostTrade::gold_interest;

  economy.start(world);
  run(economy, world, 400, 2000);  // one OutpostRefresh

  CHECK(economy.resource(seller, Resource::food) == 80);
  CHECK(economy.resource(seller, Resource::gold) == 10);
  CHECK(economy.resource(banker, Resource::gold) == 2008);
}

/// `UNIT_ON_ENTER.VS`: `SpoilsOfWarGold = 100` per item carried in.
TEST(economy_spoils_of_war_pay_a_hundred_each) {
  World world;
  EconomySystem economy;
  SettlementInit init = town_hall();
  init.gold = 0;
  const SettlementId id = economy.create(init);
  economy.start(world);

  CHECK(economy.deliver_spoils(id, 3) == 300);
  CHECK(economy.resource(id, Resource::gold) == 300);
}

// --------------------------------------------------------------------------
// determinism
// --------------------------------------------------------------------------

namespace {

/// A world with one village supplying one town hall: production, growth,
/// starvation, repair, sentries, loyalty and the wagon cycle all live at once,
/// so a partition that disturbs any of their orderings shows up in the hash.
std::uint64_t run_scenario(const std::vector<std::int32_t>& turns) {
  World world;
  EconomySystem economy;
  const ObjectId anchor = world.spawn(NativeClass::town_hall, nullptr);
  world.set_health(anchor, 1200);

  SettlementInit hall = town_hall();
  hall.anchor = anchor;
  hall.anchor_max_health = 5000;
  const SettlementId town = economy.create(world, hall);

  const SettlementId farm = economy.create(world, village());
  economy.set_supplied(farm, town);
  economy.set_owner(farm, economy.find(town)->owner);
  economy.garrison_add(town, 1);
  economy.start(world);

  for (std::int32_t length : turns) {
    world.advance(length);
    economy.advance(world, world.turn());
  }
  return economy_hash(economy);
}

}  // namespace

/// Two identical worlds produce identical resources. The weakest of the three
/// properties, and the one that fails first when something reads a pointer.
TEST(economy_identical_worlds_agree) {
  const std::vector<std::int32_t> turns(200, 400);
  CHECK(run_scenario(turns) == run_scenario(turns));
}

/// **The one that matters.** The turn length is renegotiated mid-session, so any
/// partition of the same interval has to leave the economy in the same state.
/// A per-turn accumulator instead of the event-stepped loop fails this.
TEST(economy_agrees_under_any_turn_partition) {
  const std::uint64_t single = run_scenario({120000});

  std::vector<std::int32_t> uniform(300, 400);
  CHECK(run_scenario(uniform) == single);

  // The four lengths the nine desync dumps record, cycled.
  std::vector<std::int32_t> mixed;
  std::int32_t total = 0;
  const std::int32_t pattern[] = {400, 800, 200, 799};
  for (int i = 0; total < 120000; ++i) {
    std::int32_t next = pattern[i % 4];
    if (total + next > 120000) next = 120000 - total;
    mixed.push_back(next);
    total += next;
  }
  CHECK(run_scenario(mixed) == single);

  // And the degenerate partition: one unit at a time, over one growth interval.
  std::vector<std::int32_t> single_units(20000, 1);
  CHECK(run_scenario(single_units) == run_scenario({20000}));
}

// --------------------------------------------------------------------------
// host functions
// --------------------------------------------------------------------------

TEST(economy_host_entries_are_all_defined) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  const std::size_t before = registry.implemented();
  CHECK(register_economy_hosts(registry) == economy_host_entry_count());
  CHECK(registry.implemented() == before + economy_host_entry_count());
  CHECK(economy_host_entry_count() >= 35);
}

namespace {

/// Call one host entry point directly, the way the VM would.
script::HostOutcome call_host(script::HostRegistry& registry, World& world,
                              script::CallKind kind, const char* name, std::uint16_t arity,
                              std::vector<script::Value> arguments) {
  const std::uint32_t index = registry.find(kind, name, arity);
  if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
  const script::HostEntry& entry = registry.entry(index);
  if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
  script::CallContext ctx;
  // `CallContext::user` is a `HostContext*` for every domain; see
  // sim/host_context.hpp.
  sim::HostContext state;
  state.world = &world;
  ctx.arguments = arguments;
  ctx.user = &state;
  ctx.name = name;
  ctx.kind = kind;
  return entry.fn(ctx);
}

}  // namespace

/// `Building::settlement` is the object model's -- 432 call sites, resolved
/// from `WorldObject::settlement` -- and every settlement member the economy
/// owns is reached through it. That the two agree on the handle representation
/// is what this checks: `(kTypeSettlement, settlement object id)` on both sides.
TEST(economy_host_resolves_a_building_to_its_settlement) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_economy_hosts(registry);

  World world;
  EconomySystem economy;
  REQUIRE(world.add_system(&economy));
  const ObjectId anchor = world.spawn(NativeClass::town_hall, nullptr);
  SettlementInit init = town_hall();
  init.anchor = anchor;
  const SettlementId id = economy.create(world, init);
  economy.start(world);
  REQUIRE(economy_of(world) == &economy);

  const script::HostOutcome resolved =
      call_host(registry, world, script::CallKind::member, "settlement", 0,
                {script::Value::object(kTypeObj, anchor)});
  REQUIRE(resolved.status == script::HostStatus::ok);
  REQUIRE(resolved.value.is_object());
  CHECK(resolved.value.as_object().type == kTypeSettlement);
  CHECK(resolved.value.as_object().id == economy.find(id)->object);

  const script::HostOutcome gold =
      call_host(registry, world, script::CallKind::member, "gold", 0, {resolved.value});
  REQUIRE(gold.status == script::HostStatus::ok);
  CHECK(gold.value.as_integer() == 2500);  // BaseTownhall's settlement_gold

  const script::HostOutcome population =
      call_host(registry, world, script::CallKind::member, "population", 0, {resolved.value});
  CHECK(population.value.as_integer() == 40);

  const script::HostOutcome central = call_host(
      registry, world, script::CallKind::member, "GetCentralBuilding", 0, {resolved.value});
  REQUIRE(central.value.is_object());
  CHECK(central.value.as_object().id == anchor);

  // And the same members answer for a plain object handle to one of the
  // settlement's buildings, which is how a behaviour script written against
  // `This` reaches them.
  const script::HostOutcome direct =
      call_host(registry, world, script::CallKind::member, "gold", 0,
                {script::Value::object(kTypeObj, anchor)});
  CHECK(direct.value.as_integer() == 2500);
}

/// `GetSettlement(name)` -- 118 sites, and the opening line of two of the four
/// AI helpers.
TEST(a_settlement_is_found_by_its_map_name_exactly_or_not_at_all) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_economy_hosts(registry);

  World world;
  EconomySystem economy;
  REQUIRE(world.add_system(&economy));

  SettlementInit named = town_hall();
  named.anchor = world.spawn(NativeClass::town_hall, nullptr);
  named.name = "S_Utica";
  const SettlementId utica = economy.create(world, named);

  // A second settlement with no name at all. 496 of the installation's 684
  // ship this way, and the empty key must not match them together.
  SettlementInit unnamed = town_hall();
  unnamed.anchor = world.spawn(NativeClass::town_hall, nullptr);
  economy.create(world, unnamed);
  economy.start(world);

  const auto get = [&](const char* name) {
    return call_host(registry, world, script::CallKind::free_function, "GetSettlement", 1,
                     {script::Value::string(name)});
  };

  const script::HostOutcome hit = get("S_Utica");
  REQUIRE(hit.status == script::HostStatus::ok);
  REQUIRE(hit.value.is_object());
  CHECK(hit.value.as_object().type == kTypeSettlement);
  CHECK(hit.value.as_object().id == economy.find(utica)->object);

  // Byte-exact: 0x005c3b63 compares the `std::string` at `settlement+0xcc`
  // against the argument with its own length, so neither case nor a prefix
  // matches.
  const auto misses = [&](const char* name) {
    const script::HostOutcome out = get(name);
    return out.status == script::HostStatus::ok && out.value.is_object() &&
           out.value.as_object().type == script::kNoType;
  };
  CHECK(misses("s_utica"));
  CHECK(misses("S_Utic"));
  CHECK(misses("S_Utica "));
  CHECK(misses(""));  // and the empty name reaches the unnamed settlement no more

  // **A miss is not a refusal.** 0x005c3bad diagnoses and pushes the 0xffff
  // sentinel, and `SIEGE.VS` opens `set = GetSettlement(Target); if
  // (!set.IsValid) return;` -- a trap here would kill a helper the original
  // exits cleanly. So the miss has to be `ok` with an invalid handle, which is
  // what `misses` asserts, and a member call on it has to be survivable too.
  const script::HostOutcome gold =
      call_host(registry, world, script::CallKind::member, "gold", 0, {get("nobody").value});
  CHECK(zero_answer(gold));  // answered zero, not crashed
}

/// `units/0` -- and the property the whole entry point turns on: the
/// settlement's roster **is** the garrison, not a copy of it.
///
/// `gbr.exe` 0x005c3c70 allocates nothing on its hit path; it points at the
/// list embedded at `holder + 0x28` and bumps its refcount. Every assertion
/// below is one consequence of that, and a snapshotting implementation fails
/// each one -- which matters, because snapshotting is the reading that makes
/// `TOWNHALL_BEHAVIOR_GUARD.VS` do what its author meant, and is therefore the
/// tempting one.
TEST(a_settlements_unit_list_is_the_garrison_rather_than_a_copy_of_it) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_economy_hosts(registry);
  register_objlist_host(registry);

  World world;
  EconomySystem economy;
  REQUIRE(world.add_system(&economy));

  SettlementInit init = town_hall();
  init.anchor = world.spawn(NativeClass::town_hall, nullptr);
  const SettlementId town = economy.create(world, init);
  // `start` is what installs the pool's alias resolver. Without it an aliased
  // handle reads as empty, which is the degradation a world with no economy
  // behind it should show -- and is asserted at the end.
  economy.start(world);

  const ObjectId a = world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = world.spawn(NativeClass::unit, nullptr);
  REQUIRE(economy.garrison_add(town, a));
  REQUIRE(economy.garrison_add(town, b));

  const script::Value handle =
      script::Value::object(kTypeSettlement, economy.find(town)->object);
  const auto units = [&]() {
    return call_host(registry, world, script::CallKind::member, "Units", 0, {handle});
  };
  const auto count_of = [&](const script::Value& list) {
    return call_host(registry, world, script::CallKind::member, "count", 0, {list})
        .value.as_integer();
  };

  const script::HostOutcome first = units();
  REQUIRE(first.status == script::HostStatus::ok);
  REQUIRE(is_objlist(first.value));
  CHECK(count_of(first.value) == 2);
  // The same number `UnitsCount` gives, because 0x005c1d80 reads `holder+0x40`
  // and that is this very list's size member.
  CHECK(call_host(registry, world, script::CallKind::member, "UnitsCount", 0, {handle})
            .value.as_integer() == 2);

  // **The list follows the garrison.** A snapshot taken before the change
  // would still say two.
  const ObjectId c = world.spawn(NativeClass::unit, nullptr);
  REQUIRE(economy.garrison_add(town, c));
  CHECK(count_of(first.value) == 3);

  // **And the garrison follows the list**, which is the half that empties a
  // town hall in `TOWNHALL_BEHAVIOR_GUARD.VS`.
  const script::HostOutcome second = units();
  REQUIRE(is_objlist(second.value));
  CHECK(call_host(registry, world, script::CallKind::member, "Clear", 0, {second.value}).status ==
        script::HostStatus::ok);
  CHECK(economy.find(town)->holder.count() == 0);
  // Two handles over one list: the first one sees it too.
  CHECK(count_of(first.value) == 0);
  CHECK(call_host(registry, world, script::CallKind::member, "UnitsCount", 0, {handle})
            .value.as_integer() == 0);

  // A settlement that does not resolve is an empty list, never a refusal:
  // 0x005c3c99 diagnoses through a sink that is a bare `ret` in retail and then
  // allocates a fresh empty one.
  const script::HostOutcome missed =
      call_host(registry, world, script::CallKind::member, "Units", 0,
                {script::Value::object(kTypeSettlement, 9999)});
  CHECK(missed.status == script::HostStatus::ok);
  REQUIRE(is_objlist(missed.value));
  CHECK(count_of(missed.value) == 0);
  // ...and it is a real list of its own, which the original's 0x2c-byte
  // allocation makes it. Asserted by **writing to it**: an alias that resolves
  // to nothing is empty too, and reads exactly the same, but silently swallows
  // an `Add`. That is the difference this case exists for.
  CHECK(objlist_pool_of(world).alias_of(objlist_of(missed.value)) == kNoObject);
  CHECK(call_host(registry, world, script::CallKind::member, "Add", 1,
                  {missed.value, script::Value::object(kTypeObj, a)})
            .status == script::HostStatus::ok);
  CHECK(count_of(missed.value) == 1);
  // ...and it reached no garrison on the way.
  CHECK(economy.find(town)->holder.count() == 0);

  // **The resolver answers for the holder handle and for nothing else.**
  // `SettlementStore::for_object` maps all four of a settlement's handles --
  // settlement, holder, warehouse, anchor -- back to the same row, so without
  // the identity check an alias named on any of them would hand out the
  // garrison. Nothing in the entry point above can construct that, which is
  // exactly why it is asserted here rather than left to two checks and no
  // test.
  REQUIRE(economy.garrison_add(town, a));
  const Settlement* row = economy.find(town);
  ObjListPool& pool = objlist_pool_of(world);
  CHECK(pool.items(pool.acquire_alias(script::kNoScript, row->holder.object)).size() == 1);
  CHECK(pool.items(pool.acquire_alias(script::kNoScript, row->object)).empty());
  CHECK(pool.items(pool.acquire_alias(script::kNoScript, row->warehouse.object)).empty());
  CHECK(pool.items(pool.acquire_alias(script::kNoScript, row->anchor)).empty());

  // **And the resolver survives a load of the world.** The pool's entries are
  // in the world section; the resolver is a seam `EconomySystem::start`
  // installed on the live pool, and `World::deserialize` used to replace the
  // whole pool with the decoded one -- resolver and all, which the decoded one
  // did not have. Every alias handed out after a load then read as empty
  // while the garrison behind it was full: `UNIT_ENTER.VS`'s
  // `.settlement.Units().Contains(this)` answered false for a unit already
  // inside, and the unit walked back in. The save sweep's Balcans divergence
  // at turn 104, and nothing hashed it.
  {
    std::vector<std::byte> bytes;
    world.serialize(bytes);
    REQUIRE(world.deserialize(bytes).ok());
    ObjListPool& after = objlist_pool_of(world);
    CHECK(after.items(after.acquire_alias(script::kNoScript, row->holder.object)).size() == 1);
    CHECK(after.items(after.acquire_alias(script::kNoScript, row->object)).empty());
  }
}

/// `name/0` -- 61 sites, two live receivers, and one body, because this
/// registry keys on `(kind, name, arity)`.
///
/// The two are different *kinds* of answer, which is what makes the shared body
/// worth a test: a settlement's name is a field the map wrote, and an object's
/// is a reverse lookup in the named-object table. Getting the second one
/// forwards -- answering with the name the object *looks up* rather than the
/// name bound *to* it -- is the plausible mistake, and the fixture is built so
/// that it fails.
TEST(name_answers_a_settlements_own_and_an_objects_named_object_binding) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_economy_hosts(registry);

  World world;
  EconomySystem economy;
  REQUIRE(world.add_system(&economy));

  SettlementInit named = town_hall();
  named.anchor = world.spawn(NativeClass::town_hall, nullptr);
  named.name = "S_Utica";
  const SettlementId utica = economy.create(world, named);
  economy.start(world);

  const ObjectId hero = world.spawn(NativeClass::hero, nullptr);
  const ObjectId nobody = world.spawn(NativeClass::unit, nullptr);
  REQUIRE(world.named_objects().bind("NO_Scipio", hero));
  // A second name on the same object, alphabetically smaller. `gbr.exe`
  // 0x0054b3a0 walks a `std::map<std::string, ...>` with `++it` and returns the
  // first key whose value matches, so the smaller one wins -- and this table is
  // a vector in document order, which would otherwise answer with the earlier
  // declaration.
  REQUIRE(world.named_objects().bind("NO_Africanus", hero));

  const auto name_of = [&](script::Value receiver) {
    return call_host(registry, world, script::CallKind::member, "name", 0, {receiver});
  };
  const auto text = [&](const script::HostOutcome& out) {
    return out.status == script::HostStatus::ok && out.value.is_string()
               ? std::string(out.value.as_string())
               : std::string("<refused>");
  };

  CHECK(text(name_of(script::Value::object(kTypeSettlement,
                                           economy.find(utica)->object))) == "S_Utica");
  CHECK(text(name_of(script::Value::object(kTypeObj, hero))) == "NO_Africanus");
  // An object nobody named answers the empty string, not a refusal: both
  // bodies push the literal at 0x007ab85a on every miss.
  CHECK(text(name_of(script::Value::object(kTypeObj, nobody))).empty());
  CHECK(text(name_of(script::Value::object(kTypeObj, 9999))).empty());
  CHECK(text(name_of(script::Value::integer(3))).empty());
}

/// The settlement-unit family, through the host: three names, one body, and
/// **both argument forms**, which is the half that was missing.
///
/// `gbr.exe` registers each of `UnitsInSettlement`, `UnitsAroundSettlement` and
/// `UnitsGuardingSettlement` twice at arity 2 -- a `(str, str)` form and a
/// `(Settlement, str)` one -- and this engine's registry keys on
/// `(kind, name, arity)`, so one body has to take both. The string form is not
/// the rare one: 92 of the 93 shipped `UnitsInSettlement` sites pass a literal
/// settlement name, and `DATA\AI HELPERS\GUARD.VS` -- the only
/// `UnitsAroundSettlement` site in the installation -- declares `str Target`
/// and passes it straight through.
TEST(the_settlement_unit_query_takes_a_name_as_readily_as_a_handle) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_economy_hosts(registry);

  World world;
  EconomySystem economy;
  REQUIRE(world.add_system(&economy));

  SettlementInit named = town_hall();
  named.anchor = world.spawn(NativeClass::town_hall, nullptr);
  named.name = "S_Utica";
  const SettlementId utica = economy.create(world, named);
  economy.start(world);
  const ObjectId handle = economy.find(utica)->object;

  const auto ask = [&](const char* name, script::Value settlement) {
    return call_host(registry, world, script::CallKind::free_function, name, 2,
                     {settlement, script::Value::string("")});
  };
  // **By value.** `World::create_query` appends to a vector, so a pointer taken
  // before the next call is a pointer into the old allocation.
  const auto spec_of = [&](const script::HostOutcome& out) {
    QuerySpec copy;
    copy.subject = kNoObject;
    if (out.status != script::HostStatus::ok || !out.value.is_object()) return copy;
    const QuerySpec* live = world.query_spec(out.value.as_object().id);
    if (live != nullptr) copy = *live;
    return copy;
  };

  // Both forms reach the same settlement...
  const QuerySpec by_name = spec_of(ask("UnitsInSettlement", script::Value::string("S_Utica")));
  const QuerySpec by_handle =
      spec_of(ask("UnitsInSettlement", script::Value::object(kTypeSettlement, handle)));
  CHECK(by_name.subject == handle);
  CHECK(by_handle.subject == handle);

  // ...and the three names differ in exactly one field, which is the field the
  // original interns on (0x004fe0fb).
  CHECK(by_name.kind == QueryKind::units_in_settlement);
  CHECK(by_name.settlement_scope == SettlementScope::garrison);
  const QuerySpec around = spec_of(ask("UnitsAroundSettlement", script::Value::string("S_Utica")));
  CHECK(around.settlement_scope == SettlementScope::ring);
  CHECK(around.kind == QueryKind::units_in_settlement);

  // The third name is read and deliberately unbound: zero call sites, so
  // binding it would add an entry point the shipped inventory does not
  // declare. `SettlementScope::both` still exists and `test_sim_world.cpp`
  // exercises it; what is asserted here is that nothing registered the name.
  CHECK(registry.find(script::CallKind::free_function, "UnitsGuardingSettlement", 2) ==
        script::kUnresolvedHost);

  // A name that matches nothing is a diagnostic and an invalid handle, the
  // `GetSettlement/1` rule: 0x00574c69 prints "Could not find settlement named
  // '%s' in function 'UnitsInSettlement'. Check the spelling." and pushes the
  // 0xffff sentinel. Trapping would kill `GUARD.VS` at its first statement.
  const script::HostOutcome miss = ask("UnitsAroundSettlement", script::Value::string("s_utica"));
  CHECK(miss.status == script::HostStatus::ok);
  REQUIRE(miss.value.is_object());
  CHECK(miss.value.as_object().type == script::kNoType);
}

/// `unit.GetHolderSett` -- 34 sites. Not the settlement the unit belongs to.
TEST(the_holders_settlement_is_a_two_hop_walk_and_neither_hop_is_the_units_own) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_economy_hosts(registry);

  World world;
  EconomySystem economy;
  REQUIRE(world.add_system(&economy));
  SettlementInit init = town_hall();
  init.anchor = world.spawn(NativeClass::town_hall, nullptr);
  const SettlementId id = economy.create(world, init);
  economy.start(world);
  const Settlement* set = economy.find(id);
  REQUIRE(set != nullptr);
  REQUIRE(set->holder.object != kNoObject);

  const ObjectId crew = world.spawn(NativeClass::unit, nullptr);
  const auto holder_sett = [&](ObjectId who) {
    return call_host(registry, world, script::CallKind::member, "GetHolderSett", 0,
                     {script::Value::object(kTypeObj, who)});
  };

  // Out in the open: `unit+0x154` is null, 0x005d70d1 pushes 0xffff. Note this
  // is true even though the unit would answer `settlement` perfectly well if it
  // had one -- the two members walk different links, which is the whole reason
  // `GetHolderSett` exists.
  const script::HostOutcome loose = holder_sett(crew);
  REQUIRE(loose.status == script::HostStatus::ok);
  CHECK(loose.value.as_object().type == script::kNoType);

  // Inside the settlement's holder: holder -> settlement -> id.
  REQUIRE(world.put_in_holder(crew, set->holder.object));
  const script::HostOutcome held = holder_sett(crew);
  REQUIRE(held.status == script::HostStatus::ok);
  REQUIRE(held.value.is_object());
  CHECK(held.value.as_object().type == kTypeSettlement);
  CHECK(held.value.as_object().id == set->object);

  // Held by something that owns no settlement -- a ship, a transport -- stops
  // at the second hop rather than answering with the first thing it finds.
  REQUIRE(world.put_in_holder(crew, world.spawn(NativeClass::unit, nullptr)));
  CHECK(holder_sett(crew).value.as_object().type == script::kNoType);

  // **The null handle is not a settlement to search for.** A settlement built
  // without the allocator keeps `kNoObject` in all three of its handles --
  // `sim/settlement.hpp` says so, and `SettlementInit` defaults them that way
  // -- so a store holding one would match *every* unheld object if the null
  // check went. Two guards stand between here and that, one in `GetHolderSett`
  // and one in `for_object`, and this was written because removing either of
  // them on its own changed no test in the suite: the property was held up by
  // the pair and asserted by nothing.
  SettlementStore bare;
  SettlementInit unallocated = town_hall();  // no settlement_object, no holder
  REQUIRE(unallocated.holder_object == kNoObject);
  const SettlementId ghost = bare.create(unallocated);
  REQUIRE(bare.find(ghost) != nullptr);
  REQUIRE(bare.find(ghost)->holder.object == kNoObject);
  CHECK(bare.for_object(kNoObject) == nullptr);
  // ...and the name key has the same shape of hole, which is why `find_by_name`
  // refuses the empty string rather than scanning for it.
  CHECK(bare.find_by_name("") == nullptr);
}

TEST(economy_host_transfers_resources) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_economy_hosts(registry);

  World world;
  EconomySystem economy;
  REQUIRE(world.add_system(&economy));
  const SettlementId id = economy.create(world, town_hall());
  economy.start(world);
  const script::Value handle = script::Value::object(kTypeSettlement, economy.find(id)->object);

  // `WAGON_UNLOAD.VS`: `SetFood(settlement.food + amount)`.
  CHECK(call_host(registry, world, script::CallKind::member, "SetFood", 1,
                  {handle, script::Value::integer(750)})
            .status == script::HostStatus::ok);
  CHECK(economy.resource(id, Resource::food) == 750);

  // `TOWNHALL_ADDPOP.VS`: `AddToPopulation(10)`.
  CHECK(call_host(registry, world, script::CallKind::member, "AddToPopulation", 1,
                  {handle, script::Value::integer(10)})
            .status == script::HostStatus::ok);
  CHECK(economy.find(id)->population == 50);

  // `GetConst` used to live here and now lives in `sim/env.cpp`, which reads
  // the whole of `CONST.INI`'s `[GamePlay]` section rather than the economy's
  // ~40 keys. `economy_constant` is what is left of the economy's half, and it
  // is what env asks first so that `set_rules` stays authoritative; the host
  // entry point itself is tested in `test_env.cpp`.
  std::int32_t min_population = 0;
  REQUIRE(economy_constant(economy.rules(), "MinPopulation", min_population));
  CHECK(min_population == 10);
  std::int32_t not_ours = -1;
  CHECK(!economy_constant(economy.rules(), "CatapultBaseFireRate", not_ours));
  CHECK(not_ours == -1);  // untouched on a miss

  // And `set_rules` really is what it reports.
  EconomyRules retuned = economy.rules();
  retuned.min_population = 3;
  economy.set_rules(retuned);
  REQUIRE(economy_constant(economy.rules(), "MinPopulation", min_population));
  CHECK(min_population == 3);
  economy.set_rules(EconomyRules{});

  // `IdxToSet/1` used to be tested here, reading its argument as a
  // `SettlementId`. It belongs to `sim/objlist.cpp` now, and not because one
  // reading of the original beat the other: `MaxSetIdx` and `IdxToSet` have to
  // share one numbering, since every corpus site is
  // `for (i = 0; i < MaxSetIdx; i += 1) { s = IdxToSet(i); ... }`, and
  // `MaxSetIdx` counts settlement objects in world order. `test_objlist.cpp`
  // covers it; `sim/objlist.cpp` carries what is still unknown about the index
  // space.
}

/// A receiver that is not a settlement is **the type's zero, printed and
/// pushed** -- `Settlement::health` (0x005c1e63) and `IsFull` (0x005c1d43)
/// are the pattern, and `IsStronghold` (0x0042508b) pushes false without
/// even the print. This test used to assert an error here, on the argument
/// that a silent zero was a divergence no test could see; the first trap
/// sweep with the AI running saw the refusal instead, at the second line of
/// three AI scripts asking `g.settlement.IsStronghold` of a node with none.
TEST(economy_host_answers_zero_for_a_receiver_that_is_not_a_settlement) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_economy_hosts(registry);

  World world;
  EconomySystem economy;
  REQUIRE(world.add_system(&economy));
  economy.start(world);

  const script::HostOutcome gold = call_host(registry, world, script::CallKind::member, "gold", 0,
                                             {script::Value::object(kTypeObj, 4242)});
  CHECK(gold.status == script::HostStatus::ok);
  CHECK(gold.value.as_integer() == 0);
  const script::HostOutcome stronghold =
      call_host(registry, world, script::CallKind::member, "IsStronghold", 0,
                {script::Value::object(script::ObjectRef{script::kNoType, 0})});
  CHECK(stronghold.status == script::HostStatus::ok);
  CHECK(!stronghold.value.truthy_scalar());

  // And with no economy on the world at all, rather than a wrong number.
  World bare;
  CHECK(call_host(registry, bare, script::CallKind::free_function, "IdxToSet", 1,
                  {script::Value::integer(0)})
            .status == script::HostStatus::error);
}

// --------------------------------------------------------------------------
// the food sink
// --------------------------------------------------------------------------

/// The economy models no food sink on its own, and for a long time nothing else
/// did either: a stronghold's food could only climb. `FeederSystem` is the sink
/// (see `sim/feeder.hpp`), and these two tests are the economy's half of the
/// arithmetic that ties the two together.
///
/// A village is `population=12`, `produces_food="1"` and so `food_rate=24`:
/// `12 * 24 / 100 = 2` per `ProductionInterval = 2000`, which is **10 food per
/// ten seconds**. A feeding unit costs one food per `DropFoodByOneIvl = 10000`.
/// So one village sustains ten units, and the AI agrees from the other side --
/// `DATA\AI\ES_VILLAGE.VS` sizes its food target as `MilUnits(idPlayer) * 20`,
/// which is `UNIT.SC.XML`'s `max_food` per military unit.
///
/// The *transfer* is not the feeder's: `DATA\CLASSES\BASEVILLAGE.SC.XML` binds
/// `village_behavior_givefood.vs`, which tops up every friendly unit in sight
/// through `SetFood`. What is checked here is that the two rates match.
TEST(economy_village_production_matches_ten_units_of_feeding) {
  World world;
  EconomySystem economy;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  feeder.set_draws_from_warehouse(false);  // the transfer is script, not engine
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&feeder));

  SettlementInit init = village();
  // Growth is pinned off: it eats `foodperpop` every 20,000 ms and would
  // confound a rate measurement with a second, unrelated sink.
  init.max_population = init.population;
  const SettlementId id = economy.create(world, init);
  REQUIRE(economy.find(id)->food_rate == 24);
  REQUIRE(economy.find(id)->population == 12);

  for (ObjectId i = 0; i < 10; ++i) {
    FeedingUnit link;
    link.unit = 500 + i;
    link.food = 20;      // UNIT.SC.XML max_food="20"
    link.max_food = 20;
    link.max_health = 200;
    feeder.enrol(link);
  }
  economy.start(world);
  feeder.start(world);

  // `world.advance` drives both registered systems; the file's `run` helper
  // advances the economy a second time by hand, which is right for the tests
  // that never register it and wrong here.
  const std::int32_t before = economy.resource(id, Resource::food);
  for (int i = 0; i < 25; ++i) world.advance(400);  // 10,000 ms
  CHECK(economy.resource(id, Resource::food) - before == 10);

  std::int32_t eaten = 0;
  for (ObjectId i = 0; i < 10; ++i) eaten += 20 - feeder.food(500 + i);
  CHECK(eaten == 10);
}

/// A town hall is the opposite case and is why the reference dumps' stored food
/// does not simply climb. `DATA\CLASSES\BASETOWNHALL.SC.XML` ships
/// `produces_food="0"` -- the dumps' `capture/attack/gold/food` quad is
/// `1 1 24 0` on every one of the 33 town-hall settlements -- and
/// `max_units="10000"`, so its store is a pure sink against its garrison, fed
/// only by wagons.
TEST(economy_stronghold_food_is_a_sink_against_its_garrison) {
  World world;
  EconomySystem economy;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&feeder));

  SettlementInit init = town_hall();
  init.max_population = init.population;  // growth off, as above
  const SettlementId id = economy.create(world, init);
  REQUIRE(economy.find(id)->food_rate == 0);  // produces_food="0"

  const Settlement* s = economy.find(id);
  const ObjectId holder = s->holder.object;
  REQUIRE(holder != kNoObject);

  const ObjectId soldier = world.spawn(NativeClass::unit, nullptr);
  world.set_health(soldier, 200);
  REQUIRE(world.put_in_holder(soldier, holder));
  REQUIRE(economy.garrison_add(id, soldier));
  FeedingUnit link;
  link.unit = soldier;
  link.food = 0;  // hungry at once, so the draw is visible in one turn
  link.max_food = 20;
  link.max_health = 200;
  feeder.enrol(link);

  economy.start(world);
  feeder.start(world);

  const std::int32_t before = economy.resource(id, Resource::food);
  world.advance(200);
  // A full top-up out of a store nothing produces into.
  CHECK(economy.resource(id, Resource::food) == before - 20);
  CHECK(feeder.food(soldier) == 20);
}

// --------------------------------------------------------------------------
// SetPlayer: capture, and the defection that shares its name
// --------------------------------------------------------------------------

namespace {

std::span<const std::byte> xml_bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A stronghold with an anchor, two more buildings and a garrison of two.
///
/// The garrison matters more than it looks: `AddUnit` files units in the
/// **holder**, and `SetPlayer`'s sweep walks the *buildings* list, so a
/// captured settlement keeps somebody else's soldiers standing inside it.
/// Nothing else in this fixture can tell those two containers apart.
struct CaptureBench {
  World world;
  EconomySystem economy;
  CommandSystem commands;
  script::HostRegistry registry;
  SettlementId town = kNoSettlement;
  ObjectId anchor = kNoObject;
  ObjectId barracks = kNoObject;
  ObjectId tower = kNoObject;
  ObjectId garrison_a = kNoObject;
  ObjectId garrison_b = kNoObject;

  CaptureBench() {
    script::declare_shipped_surface(registry);
    (void)register_world_host(registry);
    (void)register_objlist_host(registry);
    (void)register_economy_hosts(registry);
    world.add_system(&economy);
    world.add_system(&commands);

    anchor = world.spawn(NativeClass::town_hall, nullptr);
    barracks = world.spawn(NativeClass::barrack, nullptr);
    tower = world.spawn(NativeClass::building, nullptr);
    garrison_a = world.spawn(NativeClass::unit, nullptr);
    garrison_b = world.spawn(NativeClass::unit, nullptr);
    for (const ObjectId id : {anchor, barracks, tower, garrison_a, garrison_b}) {
      world.set_owner(id, 0);
      world.set_health(id, 500);
    }

    SettlementInit init = town_hall();
    init.anchor = anchor;
    init.anchor_max_health = 5000;
    init.owner = 0;
    town = economy.create(world, init);
    Settlement* s = economy.find(town);
    s->buildings.push_back(SettlementBuilding{anchor, 5000});
    s->buildings.push_back(SettlementBuilding{barracks, 800});
    s->buildings.push_back(SettlementBuilding{tower, 400});
    s->loyalty = 40;
    (void)economy.garrison_force_add(town, garrison_a);
    (void)economy.garrison_force_add(town, garrison_b);
    // The back-link the object model resolves a settlement through, and the
    // one the building redirect keys on.
    for (const ObjectId id : {anchor, barracks, tower, garrison_a, garrison_b}) {
      world.find(id)->settlement = s->object;
    }
  }

  [[nodiscard]] script::Value settlement_handle() const {
    return script::Value::object(
        script::ObjectRef{kTypeSettlement, economy.find(town)->object});
  }
  [[nodiscard]] static script::Value obj(ObjectId id) {
    return script::Value::object(script::ObjectRef{kTypeObj, id});
  }
  script::HostOutcome set_player(script::Value receiver, std::int32_t player) {
    return call_host(registry, world, script::CallKind::member, "SetPlayer", 1,
                     {receiver, script::Value::integer(player)});
  }
  [[nodiscard]] PlayerId owner_of(ObjectId id) const { return world.find(id)->state.owner; }
};

}  // namespace

TEST(economy_capture_moves_the_settlement_and_its_buildings_but_not_its_garrison) {
  // The assertion that could only be written from the disassembly. The worker
  // at 0x005c4eb0 sweeps the settlement's **buildings** deque and gives each
  // one `CObject::SetPlayer`; `AddUnit` (0x005c1b20) files units in the
  // *holder*, a different container, which the sweep never reaches. So a
  // captured town comes with the previous owner's soldiers still inside it.
  CaptureBench b;
  CHECK(b.set_player(b.settlement_handle(), 3).status == script::HostStatus::ok);

  CHECK(b.economy.find(b.town)->owner == 2);  // 1-based in, 0-based stored
  CHECK(b.owner_of(b.anchor) == 2);
  CHECK(b.owner_of(b.barracks) == 2);
  CHECK(b.owner_of(b.tower) == 2);

  CHECK(b.owner_of(b.garrison_a) == 0);
  CHECK(b.owner_of(b.garrison_b) == 0);
  // And they are still garrisoned: capture does not empty the holder either.
  CHECK(b.economy.find(b.town)->holder.count() == 2);
}

TEST(economy_capture_leaves_loyalty_stock_and_population_alone) {
  // `SetPlayer` never touches loyalty -- which is why every shipped capture
  // site writes `SetLoyalty(11)` on the next line, and why routing this
  // through `EconomySystem::capture` would be wrong twice over: that function
  // *requires* loyalty 0 and then *writes* loyalty, and the entry point does
  // neither. Of the 109 `SetPlayer` sites in the installation only 5 carry a
  // loyalty test at all, and it is in the script.
  CaptureBench b;
  const Settlement before = *b.economy.find(b.town);
  CHECK(b.set_player(b.settlement_handle(), 2).status == script::HostStatus::ok);

  const Settlement* after = b.economy.find(b.town);
  CHECK(after->loyalty == before.loyalty);
  CHECK(after->gold() == before.gold());
  CHECK(after->food() == before.food());
  CHECK(after->population == before.population);
  CHECK(after->max_population == before.max_population);
  CHECK(after->sentries == before.sentries);
  // `can_be_captured` is not consulted either: the entry point has no such
  // test, so a settlement the class forbids capturing still changes hands
  // when a mission script says so.
  CHECK(after->owner == 1);
}

TEST(economy_capture_wipes_the_loan_and_setting_the_current_owner_does_not) {
  // The loan wipe is the only observable the "already owns it" early-out at
  // 0x005c4ec1 has. Without a loan on the books, re-capturing by the current
  // owner is indistinguishable from doing the work again.
  CaptureBench b;
  b.economy.find(b.town)->loan = 750;
  CHECK(b.set_player(b.settlement_handle(), 1).status == script::HostStatus::ok);
  CHECK(b.economy.find(b.town)->loan == 750);   // player 1 -> index 0, already the owner
  CHECK(b.economy.find(b.town)->owner == 0);

  CHECK(b.set_player(b.settlement_handle(), 4).status == script::HostStatus::ok);
  CHECK(b.economy.find(b.town)->loan == 0);
  CHECK(b.economy.find(b.town)->owner == 3);
}

TEST(economy_a_building_receiver_captures_its_whole_settlement) {
  // The dominant shipped path, and the one a settlement-handle-only reading
  // would miss. `Obj::SetPlayer`'s slot at `vtbl + 0xA0` is 0x004dbb00 for the
  // building family, whose whole body reads the object's settlement back-link
  // and redirects. Only 8 of the 116 sites pass a settlement handle;
  // `TTENT_BEHAVIOR.VS` and its eighteen per-map copies, `OUTPOST_BEHAVIOR.VS`
  // and every `NO_Village`/`NO_Tent` name capture through a building.
  CaptureBench b;
  CHECK(b.set_player(CaptureBench::obj(b.barracks), 3).status == script::HostStatus::ok);

  CHECK(b.economy.find(b.town)->owner == 2);
  CHECK(b.owner_of(b.anchor) == 2);
  CHECK(b.owner_of(b.tower) == 2);
  CHECK(b.owner_of(b.garrison_a) == 0);
}

TEST(economy_a_captured_settlement_answers_its_new_owner_to_its_own_members) {
  // `Settlement::player` (0x005c2340), `IsOwn` (0x004252c0), `IsEnemy` and
  // `IsAlly` all read `[settlement+0x90]`, which the capture worker writes at
  // 0x005c4f78. Here they read the settlement object's owner, so a capture
  // that moved the row alone left a captured village answering its previous
  // owner -- and `GS_CAPTURE.VS`, looping `while (set.IsEnemy(AIPlayer))`,
  // kept an army besieging a village its own side already held. Captured
  // through a building, the shipped path.
  CaptureBench b;
  (void)register_player_host(b.registry);
  const auto ask = [&](const char* name, std::uint16_t arity,
                       std::vector<script::Value> args) {
    const script::HostOutcome out =
        call_host(b.registry, b.world, script::CallKind::member, name, arity, std::move(args));
    CHECK(out.status == script::HostStatus::ok);
    return out.value;
  };
  REQUIRE(ask("player", 0, {b.settlement_handle()}).as_integer() == 1);
  CHECK(b.set_player(CaptureBench::obj(b.anchor), 3).status == script::HostStatus::ok);
  REQUIRE(b.economy.find(b.town)->owner == 2);

  CHECK(ask("player", 0, {b.settlement_handle()}).as_integer() == 3);
  CHECK(ask("IsOwn", 1, {b.settlement_handle(), script::Value::integer(3)}).truthy_scalar());
  CHECK(!ask("IsOwn", 1, {b.settlement_handle(), script::Value::integer(1)}).truthy_scalar());
  // The holder and the warehouse are still not touched as objects.
  const Settlement* s = b.economy.find(b.town);
  CHECK(b.owner_of(s->holder.object) == 0);
  CHECK(b.owner_of(s->warehouse.object) == 0);
}

TEST(economy_a_unit_receiver_defects_alone_and_drops_its_orders) {
  // The guard against reusing `settlement_at`, which maps *any* object with a
  // settlement back-link to its settlement -- and units carry that link here,
  // because `units_in_settlement` filters on it. Without the building test,
  // one soldier changing sides would hand over the town he is standing in.
  //
  // The command clear is the original's too: `CObject::SetPlayer` calls
  // `vtbl + 0xC0`, the same slot `Obj::ClearCommands` uses, before writing the
  // owner -- and only when the object already had one.
  CaptureBench b;
  CommandTable table;
  (void)table.merge(xml_bytes(R"(<commands>
<cmd name="advance" priority="0"><src obj="Unit" sticky="yes"/></cmd>
</commands>)"));
  b.commands.set_table(std::move(table));
  CHECK(b.commands.add_command(b.world, b.garrison_a, false, "advance", {}) != 0);
  CHECK(b.commands.add_command(b.world, b.garrison_a, false, "advance", {}) != 0);
  CHECK(b.commands.command_count(b.garrison_a) == 2);

  CHECK(b.set_player(CaptureBench::obj(b.garrison_a), 2).status == script::HostStatus::ok);
  CHECK(b.owner_of(b.garrison_a) == 1);
  // The tail *and* the runner: see the labelled note in `give_object_to`.
  // Queueing two is what makes this distinguish `ClearCommands` from the
  // whole-queue reading -- with one command the two are the same assertion.
  CHECK(b.commands.command_count(b.garrison_a) == 0);

  // His town did not change hands, and neither did anybody else.
  CHECK(b.economy.find(b.town)->owner == 0);
  CHECK(b.owner_of(b.garrison_b) == 0);
  CHECK(b.owner_of(b.anchor) == 0);
}

TEST(economy_an_unowned_object_keeps_its_orders_when_it_gains_an_owner) {
  // The clear is conditional on the object *already having* an owner -- there
  // is no previous owner whose orders they were -- and on it not being a spawn
  // template. Both conditions are the original's, and neither is observable
  // through the ordinary capture path, which is why a fault that cleared
  // unconditionally survived a first version of these tests.
  CaptureBench b;
  CommandTable table;
  (void)table.merge(xml_bytes(R"(<commands>
<cmd name="advance" priority="0"><src obj="Unit" sticky="yes"/></cmd>
</commands>)"));
  b.commands.set_table(std::move(table));

  const ObjectId stray = b.world.spawn(NativeClass::unit, nullptr);
  b.world.set_health(stray, 100);
  CHECK(b.world.find(stray)->state.owner == kNoPlayer);
  CHECK(b.commands.add_command(b.world, stray, false, "advance", {}) != 0);
  CHECK(b.commands.add_command(b.world, stray, false, "advance", {}) != 0);

  CHECK(b.set_player(CaptureBench::obj(stray), 2).status == script::HostStatus::ok);
  CHECK(b.owner_of(stray) == 1);
  CHECK(b.commands.command_count(stray) == 2);

  // And a spawn template is left alone even when it has an owner: the object
  // that is not in play yet has nothing to stop doing.
  const ObjectId templ = b.world.spawn(NativeClass::unit, nullptr);
  b.world.set_owner(templ, 0);
  b.world.set_health(templ, 100);
  b.world.find(templ)->state.flags.unspawned = true;
  CHECK(b.commands.add_command(b.world, templ, false, "advance", {}) != 0);
  CHECK(b.set_player(CaptureBench::obj(templ), 2).status == script::HostStatus::ok);
  CHECK(b.owner_of(templ) == 1);
  CHECK(b.commands.command_count(templ) == 1);
}

TEST(economy_set_player_never_traps_and_the_range_check_is_not_uniform) {
  // Every one of the four bodies returns after printing through a sink that is
  // a bare `ret` in the retail build, so a bad number is a silent no-op and
  // the script runs on -- the `GetSettlement/1` rule. The
  // `HERO_CAPTURE`/`UNIT_CAPTURE` loops that carry the campaign would die on a
  // refusal the original walks past.
  //
  // And the check is *not* uniform, which is transcribed rather than tidied:
  // `Obj` and `ObjList` test the number, `Query` and `Settlement` do not.
  CaptureBench b;
  for (const std::int32_t bad : {0, -1, 17, 99}) {
    const script::HostOutcome out = b.set_player(CaptureBench::obj(b.garrison_a), bad);
    CHECK(out.status == script::HostStatus::ok);
    CHECK(b.owner_of(b.garrison_a) == 0);
  }
  // A dead receiver, and a receiver that is not a handle at all.
  CHECK(b.set_player(CaptureBench::obj(9999), 2).status == script::HostStatus::ok);
  CHECK(b.set_player(script::Value::integer(3), 2).status == script::HostStatus::ok);

  // The settlement body has no range check, so the same 17 lands -- as
  // `kNoPlayer`, which is what a NULL owner pointer reads as here.
  CHECK(b.set_player(b.settlement_handle(), 17).status == script::HostStatus::ok);
  CHECK(b.economy.find(b.town)->owner == kNoPlayer);
}

// ==========================================================================
// the outpost descriptor
// ==========================================================================

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Five classes: two shipped outposts, their common base, something that is not
/// an outpost at all, and a root.
///
/// The numbers are `DATA\CLASSES\*OUTPOST.SC.XML` verbatim. `IOutpost` is the
/// only shipped class that declares a second slot; `ROutpost` declares one; and
/// `Outpost`, which all six descend from, declares neither -- which is what
/// `IsIndependentGuarded` already rests on. `Outpost` also declares no
/// `settlement_food`, and that absence is the one the resolver never
/// normalises.
struct OutpostGraph {
  ClassGraph graph;
  ClassIndex base = kNoClass;
  ClassIndex roman = kNoClass;
  ClassIndex iberian = kNoClass;
  ClassIndex barracks = kNoClass;
  ClassIndex sloppy = kNoClass;
  ClassIndex orphan = kNoClass;

  OutpostGraph() {
    const std::string docs[] = {
        R"(<class id="Building" cpp_class="CVXBuilding"><properties maxhealth="1000"/></class>)",
        R"(<class id="Outpost" parent="Building" cpp_class="CVXTownHall"><properties
             maxhealth="5000" defenders_out_1="3"/></class>)",
        R"(<class id="ROutpost" parent="Outpost" cpp_class="CVXOutpost"><properties
             settlement_food="0" settlement_gold="1000"
             defender_cls_1="RLiberatus" defenders_max_1="20" defenders_out_1="8"
             start_level_1="4" end_level_1="12"/></class>)",
        R"(<class id="IOutpost" parent="Outpost" cpp_class="CVXOutpost"><properties
             settlement_food="2000" settlement_gold="0"
             defender_cls_1="ISlinger" defenders_max_1="12" defenders_out_1="4"
             start_level_1="8" end_level_1="20"
             defender_cls_2="IDefender" defenders_max_2="10" defenders_out_2="10"
             start_level_2="8" end_level_2="8"/></class>)",
        R"(<class id="RBarracks" parent="Building" cpp_class="CVXBarrack"/>)",
        // A declared property that is not a whole decimal. Nothing ships like
        // this; it is here because the parse has to have an answer.
        R"(<class id="Sloppy" parent="Outpost" cpp_class="CVXOutpost"><properties
             defenders_max_1="lots" settlement_food="2000g"/></class>)",
        // No parent at all, which is the one shape the inheritance resolver
        // never touches. No shipped class is one.
        R"(<class id="Orphan" cpp_class="CVXOutpost"><properties maxhealth="5000"/></class>)",
    };
    const char* names[] = {"building.sc.xml", "outpost.sc.xml",   "routpost.sc.xml",
                           "ioutpost.sc.xml", "rbarracks.sc.xml", "sloppy.sc.xml",
                           "orphan.sc.xml"};
    for (int i = 0; i < 7; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    base = graph.find("Outpost");
    roman = graph.find("ROutpost");
    iberian = graph.find("IOutpost");
    barracks = graph.find("RBarracks");
    sloppy = graph.find("Sloppy");
    orphan = graph.find("Orphan");
  }
};

/// A world with that graph and the six getters registered.
///
/// `outpost()` builds what every shipped call site has: a building that is the
/// central building of a settlement whose kind is `outpost`. Without the
/// settlement the gate fails and all six answer zero, which is a case of its
/// own below rather than the default the other tests would silently be running.
struct OutpostBench {
  OutpostGraph classes;
  script::HostRegistry registry;
  World world;
  EconomySystem economy;

  OutpostBench() {
    script::declare_shipped_surface(registry);
    register_world_host(registry);
    register_economy_hosts(registry);
    world.set_class_graph(&classes.graph);
    REQUIRE(world.add_system(&economy));
    economy.start(world);
  }

  /// A building of `which`, anchoring an outpost settlement.
  ObjectId outpost(ClassIndex which) {
    const ObjectId anchor = world.spawn(NativeClass::building, nullptr, which);
    SettlementInit init;
    init.kind = SettlementKind::outpost;
    init.anchor = anchor;
    init.max_units = 10000;
    economy.create(world, init);
    return anchor;
  }

  /// A building of `which` anchoring a settlement of some other kind.
  ObjectId not_an_outpost(ClassIndex which) {
    const ObjectId anchor = world.spawn(NativeClass::building, nullptr, which);
    SettlementInit init;
    init.kind = SettlementKind::village;
    init.anchor = anchor;
    economy.create(world, init);
    return anchor;
  }

  /// A building of `which` that belongs to no settlement at all.
  ObjectId loose(ClassIndex which) {
    return world.spawn(NativeClass::building, nullptr, which);
  }

  script::HostOutcome get(const char* name, script::Value receiver, std::int32_t slot) {
    return call_host(registry, world, script::CallKind::member, name, 1,
                     {receiver, script::Value::integer(slot)});
  }
  script::HostOutcome food(script::Value receiver) {
    return call_host(registry, world, script::CallKind::member, "GetOutpostFood", 0,
                     {receiver});
  }
  static script::Value obj(ObjectId id) { return script::Value::object(kTypeObj, id); }

  // `CHECK` rather than `REQUIRE`: these return a value, and a `REQUIRE` that
  // fired would have to return one anyway. A wrong status is reported here and
  // the caller's own comparison then fails on the fallback.
  std::int32_t number(const char* name, ObjectId id, std::int32_t slot) {
    const script::HostOutcome out = get(name, obj(id), slot);
    CHECK(out.status == script::HostStatus::ok);
    CHECK(out.value.is_integer());
    return out.value.is_integer() ? out.value.as_integer() : 0x7f7f7f7f;
  }
  std::int32_t food_of(ObjectId id) {
    const script::HostOutcome out = food(obj(id));
    CHECK(out.status == script::HostStatus::ok);
    CHECK(out.value.is_integer());
    return out.value.is_integer() ? out.value.as_integer() : 0x7f7f7f7f;
  }
  std::string name_of(ObjectId id, std::int32_t slot) {
    const script::HostOutcome out = get("GetDefenderCls", obj(id), slot);
    CHECK(out.status == script::HostStatus::ok);
    CHECK(out.value.is_string());
    return out.value.is_string() ? out.value.as_string() : std::string("<not a string>");
  }
};

/// `0xff1b1e40`, the class-descriptor sentinel, as a signed int.
constexpr std::int32_t kUnresolved = -15000000;

}  // namespace

/// The five slotted getters read `<stem>_<slot>` off the receiver's own class.
TEST(the_outpost_getters_read_the_receivers_class_descriptor) {
  OutpostBench b;
  const ObjectId fort = b.outpost(b.classes.roman);
  CHECK(b.name_of(fort, 1) == "RLiberatus");
  CHECK(b.number("GetDefendersMax", fort, 1) == 20);
  CHECK(b.number("GetDefendersOut", fort, 1) == 8);
  CHECK(b.number("GetStartLevel", fort, 1) == 4);
  CHECK(b.number("GetEndLevel", fort, 1) == 12);

  const ObjectId post = b.outpost(b.classes.iberian);
  CHECK(b.name_of(post, 2) == "IDefender");
  CHECK(b.number("GetDefendersMax", post, 2) == 10);
  CHECK(b.number("GetDefendersOut", post, 2) == 10);
  CHECK(b.number("GetStartLevel", post, 2) == 8);
  CHECK(b.number("GetEndLevel", post, 2) == 8);

  // And nothing leaks sideways between two classes with the same parent.
  CHECK(b.number("GetDefendersMax", fort, 1) == 20);
  CHECK(b.number("GetDefendersMax", post, 1) == 12);
}

/// `GetOutpostFood` is `settlement_food`, and it takes no slot.
TEST(get_outpost_food_is_the_settlement_food_property) {
  OutpostBench b;
  // `ROutpost` ships `settlement_food="0"`, which is a *declared* zero.
  CHECK(b.food_of(b.outpost(b.classes.roman)) == 0);
  CHECK(b.food_of(b.outpost(b.classes.iberian)) == 2000);
}

/// An undeclared slot answers `0`, and an undeclared defender class answers
/// `""`, because the inheritance resolver normalises both.
///
/// This is what makes `OUTPOST_BEHAVIOR.VS` right on the five outposts that
/// declare one slot: `if (sDefenderCls2 != "")` is **false**, so the
/// second-slot spawn loop never runs. The class descriptor is *born* holding
/// `"**Invalid**"` and `0xff1b1e40` in these slots -- returning either of those
/// would take the other branch of that guard and put twenty units of a class
/// nothing can place on the field, or none, depending on which sentinel
/// survived. The resolver's second pass is the whole reason it does not.
TEST(an_undeclared_outpost_slot_is_normalised_to_zero_and_the_empty_string) {
  OutpostBench b;
  const ObjectId fort = b.outpost(b.classes.roman);  // slot 1 only
  CHECK(b.name_of(fort, 2) == "");
  CHECK(b.number("GetDefendersMax", fort, 2) == 0);
  CHECK(b.number("GetStartLevel", fort, 2) == 0);
  CHECK(b.number("GetEndLevel", fort, 2) == 0);

  // And the two the script compares are equal, so its `interval_2` is zero and
  // the promotion branch never runs either.
  CHECK(b.number("GetStartLevel", fort, 2) == b.number("GetEndLevel", fort, 2));

  // Nothing here is the sentinel.
  CHECK(b.number("GetDefendersMax", fort, 2) != kUnresolved);
  CHECK(b.name_of(fort, 2) != "**Invalid**");
}

/// `settlement_food` is the one field in the block the normalise pass misses,
/// so an outpost whose class tree never declares it answers **-15,000,000**.
///
/// `ResolveInheritance` normalises `+0xa4c .. +0xa68` -- the eight outpost
/// numbers -- and `settlement_food` is at `+0xa8c`. Unobservable on shipped
/// data, because all six race outposts declare it and only their common base
/// does not; asserted anyway, because a uniform implementation is a guess and
/// this one is a transcription. `OUTPOST_BEHAVIOR.VS` feeds the result straight
/// into `.settlement.SetFood`.
TEST(an_undeclared_settlement_food_escapes_the_normaliser_and_stays_the_sentinel) {
  OutpostBench b;
  // `Outpost` itself declares no `settlement_food`, and neither does anything
  // above it.
  CHECK(b.food_of(b.outpost(b.classes.base)) == kUnresolved);
  // While its slotted neighbours, undeclared in exactly the same way, are zero.
  CHECK(b.number("GetDefendersMax", b.outpost(b.classes.base), 1) == 0);
  CHECK(b.number("GetEndLevel", b.outpost(b.classes.base), 2) == 0);
}

/// A root class keeps every sentinel: the resolver's two passes are both inside
/// its `if (parent != null)` branch.
///
/// No shipped class is a root here -- `Outpost` descends from `Building` -- so
/// this is transcribed rather than observed, and it is the only way the
/// `"**Invalid**"` literal can reach a script at all.
TEST(a_root_outpost_class_keeps_the_sentinels_the_resolver_never_reaches) {
  OutpostBench b;
  const ObjectId orphan = b.outpost(b.classes.orphan);
  CHECK(b.number("GetDefendersMax", orphan, 1) == kUnresolved);
  CHECK(b.number("GetStartLevel", orphan, 2) == kUnresolved);
  CHECK(b.name_of(orphan, 1) == "**Invalid**");
  CHECK(b.food_of(orphan) == kUnresolved);

  // A bad slot is still zero even here: the dispatch never reaches a field, so
  // it cannot reach a field's fallback either.
  CHECK(b.number("GetDefendersMax", orphan, 3) == 0);
  CHECK(b.name_of(orphan, 0) == "");
}

/// The slot is two literal comparisons, not an index. Anything but 1 or 2 falls
/// through to the result register's pre-seeded zero.
TEST(an_out_of_range_outpost_slot_falls_through_rather_than_reading_past_the_end) {
  OutpostBench b;
  const ObjectId post = b.outpost(b.classes.iberian);
  for (const std::int32_t slot : {0, 3, -1, 99, 2147483647, -2147483647 - 1}) {
    CHECK(b.number("GetDefendersMax", post, slot) == 0);
    CHECK(b.number("GetEndLevel", post, slot) == 0);
    CHECK(b.name_of(post, slot) == "");
  }
  // The two that do exist still do.
  CHECK(b.number("GetDefendersMax", post, 1) == 12);
  CHECK(b.number("GetDefendersMax", post, 2) == 10);
}

/// The gate is about the settlement's central building, not about the receiver,
/// and a failed gate is **zero for every one of the six** -- including
/// `GetOutpostFood`, which answers the sentinel only when the gate *passes* and
/// the property is missing.
///
/// That asymmetry is the whole reason the gate has a test of its own: a
/// `GetOutpostFood` that returned the sentinel on a failed gate would be
/// indistinguishable on the five outposts that declare the property, and would
/// hand `SetFood` a negative fifteen million on every barracks in the game.
TEST(the_outpost_gate_asks_the_settlement_and_a_failed_gate_is_zero) {
  OutpostBench b;

  // A settlement that is not an outpost, anchored by an outpost class: the
  // receiver could answer, and the gate says no.
  const ObjectId village = b.not_an_outpost(b.classes.iberian);
  CHECK(b.number("GetDefendersMax", village, 1) == 0);
  CHECK(b.number("GetStartLevel", village, 1) == 0);
  CHECK(b.name_of(village, 1) == "");
  CHECK(b.food_of(village) == 0);
  CHECK(b.food_of(village) != kUnresolved);

  // A building that belongs to no settlement at all.
  const ObjectId loose = b.loose(b.classes.iberian);
  CHECK(b.number("GetDefendersMax", loose, 1) == 0);
  CHECK(b.food_of(loose) == 0);

  // And the same class *with* an outpost settlement answers, so the difference
  // above is the gate and nothing else.
  const ObjectId post = b.outpost(b.classes.iberian);
  CHECK(b.number("GetDefendersMax", post, 1) == 12);
  CHECK(b.food_of(post) == 2000);
}

/// A receiver that resolves to nothing is an answer, not a refusal.
///
/// Nothing traps: not one of the six bodies reaches the diagnostic sink, which
/// is a bare `ret` in the retail build anyway. A script that asks a barracks
/// for its defenders runs on with a zero that stops every loop it feeds.
TEST(the_outpost_getters_never_refuse_a_receiver_they_cannot_resolve) {
  OutpostBench b;
  const auto zero = [&](script::Value receiver) {
    const script::HostOutcome out = b.get("GetDefendersMax", receiver, 1);
    return out.status == script::HostStatus::ok && out.value.is_integer() &&
           out.value.as_integer() == 0;
  };
  CHECK(zero(OutpostBench::obj(9999)));                          // a dead id
  CHECK(zero(script::Value::object(kTypeSettlement, 1)));        // the wrong handle type
  CHECK(zero(script::Value::integer(7)));                        // not a handle
  CHECK(zero(script::Value::string("ROutpost")));                // not a handle either

  // An object with no class index at all, in an outpost settlement.
  const ObjectId classless = b.world.spawn(NativeClass::building, nullptr);
  SettlementInit init;
  init.kind = SettlementKind::outpost;
  init.anchor = classless;
  b.economy.create(b.world, init);
  CHECK(b.number("GetStartLevel", classless, 1) == 0);
  CHECK(b.name_of(classless, 1) == "");

  // And with no world behind the call at all, which is how every host entry
  // point in this tree is asked to prove it does not dereference.
  script::CallContext bare;
  std::vector<script::Value> args{OutpostBench::obj(1), script::Value::integer(1)};
  bare.arguments = args;
  bare.user = nullptr;
  const std::uint32_t index =
      b.registry.find(script::CallKind::member, "GetDefendersMax", 1);
  REQUIRE(index != script::kUnresolvedHost);
  const script::HostOutcome out = b.registry.entry(index).fn(bare);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.is_integer());
  CHECK(out.value.as_integer() == 0);
}

/// A declared property that is not a whole decimal reads as absent.
///
/// The original parses with `atoi`, which stops at the first non-digit and
/// keeps what it read; `parse_int` refuses the value entire. The difference
/// reaches a script only through a class property that is not a number, of
/// which the installation has none -- and refusing is what every other place in
/// this tree does with one, for the reason `ProductionInterval` was once
/// recorded as 20 when it is 2000.
TEST(an_unparsable_outpost_property_reads_as_absent) {
  OutpostBench b;
  const ObjectId odd = b.outpost(b.classes.sloppy);
  // `defenders_max_1="lots"` -- absent, so the resolver's zero.
  CHECK(b.number("GetDefendersMax", odd, 1) == 0);
  // `settlement_food="2000g"` -- absent, and *this* field's absence is the
  // sentinel. Not 2000.
  CHECK(b.food_of(odd) == kUnresolved);
}

/// A property declared on a parent resolves for the child, which is the whole
/// of what the resolver's first pass does.
TEST(an_outpost_property_declared_on_a_parent_resolves_for_the_child) {
  OutpostBench b;
  // `Outpost` declares `defenders_out_1="3"` and `Sloppy` declares no slot at
  // all, so `Sloppy` inherits the three rather than normalising to zero.
  CHECK(b.number("GetDefendersOut", b.outpost(b.classes.sloppy), 1) == 3);
  // And a child that declares its own wins: `ROutpost` says eight.
  CHECK(b.number("GetDefendersOut", b.outpost(b.classes.roman), 1) == 8);
  // The slot the parent does not declare is still zero.
  CHECK(b.number("GetDefendersOut", b.outpost(b.classes.sloppy), 2) == 0);
}

/// The one divergence, named where it is taken: a **non-central** building of
/// an outpost settlement.
///
/// It passes the gate in the original -- the gate asks about the settlement's
/// central building, not about the receiver -- and then reads `+0x240` off an
/// object that is not a `CVXOutpost` at all. That is uninitialised memory and a
/// fault, not a behaviour, so this reads the receiver's own class and answers
/// what that class declares. No shipped script constructs the case: the
/// receiver is always the outpost tent.
TEST(a_non_central_building_of_an_outpost_settlement_answers_from_its_own_class) {
  OutpostBench b;
  const ObjectId anchor = b.outpost(b.classes.iberian);
  const Settlement* s = b.economy.settlements().for_object(anchor);
  REQUIRE(s != nullptr);

  const ObjectId shed = b.world.spawn(NativeClass::building, nullptr, b.classes.barracks);
  REQUIRE(b.economy.add_building(b.world, s->id, shed, 3000));

  // Its settlement is the outpost's, so the gate passes...
  const script::HostOutcome gate = call_host(b.registry, b.world, script::CallKind::member,
                                             "IsOutpost", 0, {OutpostBench::obj(shed)});
  REQUIRE(gate.status == script::HostStatus::ok);
  CHECK(gate.value.as_integer() != 0);
  // ...and the answer is the barracks' own class, which declares none of it.
  CHECK(b.number("GetDefendersMax", shed, 1) == 0);
  CHECK(b.name_of(shed, 1) == "");
  // Not the anchor's twelve, which is what a settlement-rooted lookup would
  // have said.
  CHECK(b.number("GetDefendersMax", shed, 1) != 12);
}

// ==========================================================================
// the two collection members: Buildings and ObjectsAround
// ==========================================================================

namespace {

/// A settlement with a roll of buildings and a ring of loose objects around
/// them, which is what both members below read.
///
/// The class graph gives the tower a `sight` so that the ring is a real circle
/// and not a point, and gives `Military` a branch of its own so that a class
/// filter has something to reject.
struct AroundGraph {
  ClassGraph graph;
  ClassIndex object = kNoClass;
  ClassIndex tower = kNoClass;
  ClassIndex military = kNoClass;
  ClassIndex peasant = kNoClass;

  AroundGraph() {
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
        R"(<class id="Tower" parent="Object" cpp_class="CVXBuilding">
             <properties sight="500" maxhealth="4000"/></class>)",
        R"(<class id="Military" parent="Object" cpp_class="CVXUnit">
             <properties sight="200" maxhealth="80"/></class>)",
        R"(<class id="Peasant" parent="Object" cpp_class="CVXUnit">
             <properties sight="120" maxhealth="40"/></class>)",
    };
    const char* names[] = {"object.sc.xml", "tower.sc.xml", "military.sc.xml", "peasant.sc.xml"};
    for (int i = 0; i < 4; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    object = graph.find("Object");
    tower = graph.find("Tower");
    military = graph.find("Military");
    peasant = graph.find("Peasant");
  }
};

struct AroundBench {
  AroundGraph classes;
  script::HostRegistry registry;
  World world;
  EconomySystem economy;
  ObjectId anchor = kNoObject;
  ObjectId settlement_object = kNoObject;
  SettlementId town = kNoSettlement;

  AroundBench() {
    script::declare_shipped_surface(registry);
    register_world_host(registry);
    register_objlist_host(registry);
    register_economy_hosts(registry);
    world.set_class_graph(&classes.graph);
    REQUIRE(world.add_system(&economy));
    economy.start(world);

    anchor = world.spawn(NativeClass::building, nullptr, classes.tower);
    world.set_position(anchor, Point{1000, 1000});
    SettlementInit init;
    init.kind = SettlementKind::village;
    init.anchor = anchor;
    init.max_units = 10000;
    init.anchor_max_health = 4000;
    economy.create(world, init);
    const Settlement* s = economy.settlements().for_object(anchor);
    REQUIRE(s != nullptr);
    town = s->id;
    settlement_object = s->object;
    // `create` puts the anchor on the roll itself, but nothing writes the
    // back-link for it, and the back-link is what both members read.
    if (WorldObject* wo = world.find(anchor)) wo->settlement = settlement_object;
  }

  ObjectId building_at(Point where) {
    const ObjectId id = world.spawn(NativeClass::building, nullptr, classes.tower);
    world.set_position(id, where);
    world.set_health(id, 4000);
    CHECK(economy.add_building(world, town, id, 4000));
    return id;
  }

  ObjectId unit_at(ClassIndex which, Point where) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, which);
    world.set_position(id, where);
    world.set_health(id, 50);
    return id;
  }

  script::Value settlement_value() const {
    return script::Value::object(kTypeSettlement, settlement_object);
  }

  std::vector<ObjectId> members(const char* name, std::vector<script::Value> args) {
    const std::uint16_t arity = static_cast<std::uint16_t>(args.size() - 1);
    const script::HostOutcome out =
        call_host(registry, world, script::CallKind::member, name, arity, std::move(args));
    CHECK(out.status == script::HostStatus::ok);
    if (out.status != script::HostStatus::ok || !is_objlist(out.value)) return {};
    const std::span<const ObjectId> items = objlist_pool_of(world).items(objlist_of(out.value));
    return std::vector<ObjectId>(items.begin(), items.end());
  }
};

}  // namespace

/// `set.Buildings()` is the settlement's buildings and nothing else.
TEST(economy_buildings_lists_the_settlements_buildings_and_no_units) {
  AroundBench b;
  const ObjectId shed = b.building_at(Point{1200, 1000});
  const ObjectId soldier = b.unit_at(b.classes.military, Point{1050, 1000});
  // A building of another settlement entirely, standing in the same place.
  const ObjectId elsewhere = b.world.spawn(NativeClass::building, nullptr, b.classes.tower);
  b.world.set_position(elsewhere, Point{1100, 1000});

  const std::vector<ObjectId> got = b.members("Buildings", {b.settlement_value()});
  REQUIRE(got.size() == 2);
  CHECK(got[0] == b.anchor);
  CHECK(got[1] == shed);
  CHECK(std::find(got.begin(), got.end(), soldier) == got.end());
  CHECK(std::find(got.begin(), got.end(), elsewhere) == got.end());
}

/// The list is a **snapshot**, which is where this engine parts company with
/// the original -- and it is mutable, which is what makes the divergence safe
/// to have: clearing it leaves the settlement's own roll alone.
TEST(economy_buildings_hands_back_a_list_the_script_owns) {
  AroundBench b;
  const ObjectId shed = b.building_at(Point{1200, 1000});

  const script::HostOutcome out = call_host(b.registry, b.world, script::CallKind::member,
                                            "Buildings", 0, {b.settlement_value()});
  REQUIRE(out.status == script::HostStatus::ok);
  REQUIRE(is_objlist(out.value));
  const ObjListId id = objlist_of(out.value);
  REQUIRE(objlist_pool_of(b.world).items(id).size() == 2);
  // No alias: the pool owns this storage. `Units` above answers `holder` here.
  CHECK(objlist_pool_of(b.world).alias_of(id) == kNoObject);

  objlist_pool_of(b.world).mutable_items(id)->clear();
  const Settlement* s = b.economy.settlements().find(b.town);
  REQUIRE(s != nullptr);
  CHECK(s->buildings.size() == 2);
  // And a second call is a second list, seeing the roll unharmed.
  CHECK(b.members("Buildings", {b.settlement_value()}).size() == 2);
  CHECK(shed != kNoObject);
}

/// A settlement handle that resolves to nothing is an **empty list**, not a
/// refusal: both bodies print through a sink that is a bare `ret` in retail and
/// push a list either way.
TEST(economy_buildings_and_objects_around_answer_empty_for_a_dead_settlement) {
  AroundBench b;
  const script::Value nowhere = script::Value::object(kTypeSettlement, 999999);

  const script::HostOutcome listed = call_host(b.registry, b.world, script::CallKind::member,
                                               "Buildings", 0, {nowhere});
  REQUIRE(listed.status == script::HostStatus::ok);
  REQUIRE(is_objlist(listed.value));
  CHECK(objlist_pool_of(b.world).items(objlist_of(listed.value)).empty());

  const script::HostOutcome around =
      call_host(b.registry, b.world, script::CallKind::member, "ObjectsAround", 1,
                {nowhere, script::Value::string(std::string("Military"))});
  REQUIRE(around.status == script::HostStatus::ok);
  REQUIRE(is_objlist(around.value));
  CHECK(objlist_pool_of(b.world).items(objlist_of(around.value)).empty());
}

/// `set.ObjectsAround(cls)` runs **both** halves of the settlement collector --
/// the garrison and the ring -- which is `SettlementScope::both`, the mode no
/// shipped script reached until this entry point was bound.
TEST(economy_objects_around_takes_the_garrison_and_the_ring) {
  AroundBench b;
  // In the settlement (the back-link the garrison half walks), but standing
  // far outside every tower's sight.
  const ObjectId garrisoned = b.unit_at(b.classes.military, Point{40'000, 40'000});
  REQUIRE(b.world.find(garrisoned) != nullptr);
  b.world.find(garrisoned)->settlement = b.settlement_object;

  // Outside the settlement, inside the anchor tower's 500 of sight.
  const ObjectId neighbour = b.unit_at(b.classes.military, Point{1400, 1000});
  // Outside both.
  const ObjectId stranger = b.unit_at(b.classes.military, Point{9000, 9000});

  const std::vector<ObjectId> got = b.members(
      "ObjectsAround", {b.settlement_value(), script::Value::string(std::string("Military"))});
  CHECK(std::find(got.begin(), got.end(), garrisoned) != got.end());
  CHECK(std::find(got.begin(), got.end(), neighbour) != got.end());
  CHECK(std::find(got.begin(), got.end(), stranger) == got.end());

  // Sorted and unique, because the ring ran: `0x005c5ac3` sorts and uniques the
  // whole list once both halves have appended, so an object inside two towers'
  // sight appears once.
  CHECK(std::is_sorted(got.begin(), got.end()));
  CHECK(std::adjacent_find(got.begin(), got.end()) == got.end());
}

/// One object inside two towers' sight comes back once, not twice.
TEST(economy_objects_around_deduplicates_across_overlapping_towers) {
  AroundBench b;
  const ObjectId second_tower = b.building_at(Point{1300, 1000});
  const ObjectId between = b.unit_at(b.classes.military, Point{1150, 1000});

  const std::vector<ObjectId> got = b.members(
      "ObjectsAround", {b.settlement_value(), script::Value::string(std::string("Military"))});
  CHECK(std::count(got.begin(), got.end(), between) == 1);
  CHECK(second_tower != kNoObject);
}

/// The class argument filters on the class **tree**, and a name the graph does
/// not know matches nothing rather than everything.
TEST(economy_objects_around_filters_on_the_class_tree) {
  AroundBench b;
  const ObjectId soldier = b.unit_at(b.classes.military, Point{1200, 1000});
  const ObjectId villager = b.unit_at(b.classes.peasant, Point{1200, 1000});

  const std::vector<ObjectId> soldiers = b.members(
      "ObjectsAround", {b.settlement_value(), script::Value::string(std::string("Military"))});
  CHECK(std::find(soldiers.begin(), soldiers.end(), soldier) != soldiers.end());
  CHECK(std::find(soldiers.begin(), soldiers.end(), villager) == soldiers.end());

  // The common ancestor takes both.
  const std::vector<ObjectId> everything = b.members(
      "ObjectsAround", {b.settlement_value(), script::Value::string(std::string("Object"))});
  CHECK(std::find(everything.begin(), everything.end(), soldier) != everything.end());
  CHECK(std::find(everything.begin(), everything.end(), villager) != everything.end());

  // A name nothing resolves to. `ClassFilter::parse` yields a filter that names
  // classes and resolved none, which matches nothing -- a divergence from the
  // original, which refuses the whole query, and one no shipped site can see.
  CHECK(b.members("ObjectsAround",
                  {b.settlement_value(), script::Value::string(std::string("Dragon"))})
            .empty());
}

/// The list a script gets back is its own, which is what
/// `TOWNHALL_HEALING.VS`'s next line needs: `l = .ObjectsAround("Military");
/// l.AddList(.ObjectsAround("BaseMage"));`.
TEST(economy_objects_around_hands_back_a_list_and_not_a_query) {
  AroundBench b;
  const ObjectId soldier = b.unit_at(b.classes.military, Point{1200, 1000});

  const script::HostOutcome out =
      call_host(b.registry, b.world, script::CallKind::member, "ObjectsAround", 1,
                {b.settlement_value(), script::Value::string(std::string("Military"))});
  REQUIRE(out.status == script::HostStatus::ok);
  REQUIRE(is_objlist(out.value));
  CHECK(out.value.as_object().type != kTypeQuery);
  const ObjListId id = objlist_of(out.value);
  CHECK(objlist_pool_of(b.world).alias_of(id) == kNoObject);
  REQUIRE(objlist_pool_of(b.world).items(id).size() == 1);
  CHECK(objlist_pool_of(b.world).items(id)[0] == soldier);
  // Mutable, and mutating it does not move the world.
  objlist_pool_of(b.world).mutable_items(id)->push_back(b.anchor);
  CHECK(b.members("ObjectsAround",
                  {b.settlement_value(), script::Value::string(std::string("Military"))})
            .size() == 1);
}

// ==========================================================================
// what a settlement can pay for, and where it researches
// ==========================================================================

namespace {

constexpr std::string_view kResearchXml = R"(<commands>
<cmd name="Fights" priority="2" costgold="2000" costfood="0" execdelay="30000"
     method="research" researchcommand="yes"
     param="SetsSet, levels/GSwordsman, 4, NameSet, Fights, default,">
  <src obj="GArena1"/>
  <src obj="GArena2"/>
</cmd>
<cmd name="Feast" costgold="100" costfood="400" costpop="3" execdelay="9000"
     method="research" param="NameSet, Feast, default">
  <src obj="Tavern"/>
</cmd>
<cmd name="Free Idea" method="research" param="NameSet, Free, default">
  <src obj="GArena1"/>
</cmd>
<cmd name="Nowhere" costgold="1" method="research" param="NameSet, Nowhere, default"/>
<cmd name="repair" costgold="50" execdelay="5000"/>
<cmd name="hireheroR" costgold="600" execdelay="20000" method="hirehero" param="RomanHero">
  <src obj="RTemple"/>
</cmd>
<cmd name="Barrack Level 1" costgold="400" execdelay="12000" method="research"
     param="NameSet, Barrack Level 1, , SetsSet, DrillSpeed, 40"/>
<cmd name="Barrack Level 2" costgold="800" execdelay="12000" method="research"
     param="ReqSet, Barrack Level 1, , NameSet, Barrack Level 2, , SetsSet, DrillSpeed, 60"/>
<cmd name="Barrack Level 3" costgold="1600" execdelay="12000" method="research"
     param="ReqSet, Barrack Level 2, , NameSet, Barrack Level 3, default, SetsSet, DrillSpeed, 80"/>
<cmd name="GBarrack Level 3" costgold="1600" execdelay="12000" method="research"
     param="ReqSet, Barrack Level 2, , ReqSet, Fights, default, NameSet, Barrack Level 3, default, SetsSet, DrillSpeed, 80"/>
<cmd name="MBarrack Level 3" costgold="1600" execdelay="12000" method="research"
     param="ReqSet, Barrack Level 2, , ReqSet, Shows, default, NameSet, Barrack Level 3, default, SetsSet, DrillSpeed, 80"/>
</commands>)";

namespace {

/// A stronghold class and a village class, each declaring both ceilings, plus
/// a wall that declares neither -- so the anchor rule is measurable as well as
/// the values.
constexpr std::string_view kCeilingClasses[] = {
    R"(<class id="Building" cpp_class="CVXBuilding"><properties maxhealth="1000"/></class>)",
    R"(<class id="BaseTownhall" parent="Building" cpp_class="CVXTownHall"><properties
         max_population="100" max_units="10000" maxhealth="5000" population="40"
         settlement_gold="2500" settlement_food="200" settlement_maxgold="100000000"
         settlement_maxfood="100000000"/></class>)",
    R"(<class id="CTownhall" parent="BaseTownhall" cpp_class="CVXTownHall"/>)",
    R"(<class id="BaseVillage" parent="Building" cpp_class="CVXTownHall"><properties
         max_population="20" max_units="0" maxhealth="2000"/></class>)",
    R"(<class id="CVillage" parent="BaseVillage" cpp_class="CVXTownHall"/>)",
    R"(<class id="CWall" parent="Building" cpp_class="CVXBuilding"/>)",
    R"(<class id="Crops" cpp_class="CVXDecor"/>)",
};

constexpr std::string_view kCeilingMap = R"(<mapobject>
<settlement id="0" player="1" classoffirstbuilding="CTownhall" name="S_Town"
  maxpopulation="70" population="35" gold="1000" food="50" maxgold="5000" maxfood="6000"
  extrasentries="2">
<scriptobj class="CTownhall" num="0" player="1" x="1000" y="1000" flags="0x80800008"/>
<scriptobj class="CWall" num="1" player="1" x="1100" y="1000" flags="0x80800008"/>
<scriptobj class="Crops" num="4" x="1200" y="1000" flags="0x80000000"/>
</settlement>
<settlement id="1" player="1" classoffirstbuilding="CVillage" name="S_Hamlet">
<scriptobj class="CVillage" num="2" player="1" x="3000" y="3000" flags="0x80800008"/>
</settlement>
<settlement id="2" player="2" classoffirstbuilding="CTownhall" name="S_Bare">
<scriptobj class="CTownhall" num="3" player="2" x="6000" y="6000" flags="0x80800008"/>
</settlement>
</mapobject>)";

}  // namespace

/// The two ceilings come off the anchor's class, and until this test nothing
/// carried either.
///
/// `sim/settlement.hpp` documented `max_units` as coming "from the class graph:
/// 10,000 on a town hall, 0 on a village" and `session.cpp` never read it, so
/// every settlement on every retail map ran with both at zero. That is not a
/// neutral default in either direction: a `max_units` of 0 makes
/// `Holder::full()` true before the first unit walks in, and a
/// `max_population` of 0 switches growth off and -- since `SetPopulation` caps
/// -- pins the population at zero for good.
///
/// The tests above build their settlements through `EconomySystem::create` with
/// a hand-filled `SettlementInit`, so none of them could see it. This one goes
/// through `GameSession::create`, which is the only path a shipped map takes.
TEST(economy_a_loaded_settlement_takes_both_ceilings_from_its_anchors_class) {
  ClassGraph graph;
  const char* names[] = {"building.sc.xml", "basetownhall.sc.xml", "ctownhall.sc.xml",
                         "basevillage.sc.xml", "cvillage.sc.xml", "cwall.sc.xml", "crops.sc.xml"};
  for (int i = 0; i < 7; ++i) REQUIRE(graph.add(bytes_of(kCeilingClasses[i]), names[i]).ok());
  graph.link();

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.map_objects = bytes_of(kCeilingMap);

  auto session = GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(session.ok());
  const EconomySystem* economy = economy_of(session.value()->world());
  REQUIRE(economy != nullptr);

  const Settlement* town = economy->settlements().find_by_name("S_Town");
  REQUIRE(town != nullptr);
  CHECK(town->kind == SettlementKind::stronghold);
  CHECK(town->holder.max_units == 10000);
  // The point of a ceiling: an empty garrison is not full.
  CHECK(!town->holder.full());
  // **The map's `<settlement>` numbers over the class's** -- the settlement
  // constructor's own rule (0x005c3e60): an argument of -1 means the class,
  // anything else is taken as written. `S_Town` declares all seven.
  CHECK(town->max_population == 70);
  CHECK(town->population == 35);
  CHECK(town->warehouse.gold == 1000);
  CHECK(town->warehouse.food == 50);
  CHECK(town->warehouse.max_gold == 5000);
  CHECK(town->warehouse.max_food == 6000);
  CHECK(town->max_sentries >= 2);
  // **Every building the map put inside the element is on the settlement's
  // roll**, not the first alone -- the deque `BestBarrack` and `RepairAll`
  // walk. The wall is the second entry, in document order; the crops inside
  // the element are decor, not a building, and stay off it.
  REQUIRE(town->buildings.size() == 2);
  CHECK(town->buildings[0].object == town->anchor);
  CHECK(town->buildings[1].object == town->anchor + 1);
  CHECK(town->buildings[1].max_health == 1000);

  // `S_Bare` declares none of the seven, and gets the class's: `population`
  // 40, `settlement_gold` 2,500, `settlement_food` 200, the two ceilings
  // 100,000,000, `max_population` 100.
  const Settlement* bare = economy->settlements().find_by_name("S_Bare");
  REQUIRE(bare != nullptr);
  CHECK(bare->max_population == 100);
  CHECK(bare->population == 40);
  CHECK(bare->warehouse.gold == 2500);
  CHECK(bare->warehouse.food == 200);
  CHECK(bare->warehouse.max_gold == 100000000);
  CHECK(bare->warehouse.max_food == 100000000);
  REQUIRE(bare->buildings.size() == 1);

  // A village declares a population ceiling and no garrison at all, which is
  // what separates the two numbers -- one value could not stand for both.
  const Settlement* hamlet = economy->settlements().find_by_name("S_Hamlet");
  REQUIRE(hamlet != nullptr);
  CHECK(hamlet->kind == SettlementKind::village);
  CHECK(hamlet->max_population == 20);
  CHECK(hamlet->holder.max_units == 0);
  CHECK(hamlet->holder.full());
}

/// Two arena classes, a tavern and a barracks that offers nothing.
/// The barracks family's classes.
///
/// `RTemple` is the whole point of this graph. `gbr.exe`'s barrack test is
/// eight `IsHeirOf` calls against eight class *ids*, not a `cpp_class` test,
/// and the shipped content puts `cpp_class="CVXBarrack"` on every temple, the
/// druid house and two monuments as well. A graph with only real barracks in it
/// would pass under either reading.
struct BarrackGraph {
  ClassGraph graph;
  ClassIndex plain = kNoClass;
  ClassIndex rbarracks = kNoClass;
  ClassIndex gbarracks = kNoClass;
  ClassIndex ebarracks = kNoClass;
  ClassIndex mbarracks = kNoClass;
  ClassIndex winter = kNoClass;
  ClassIndex temple = kNoClass;
  ClassIndex shed = kNoClass;

  BarrackGraph() {
    const std::string docs[] = {
        R"(<class id="Building" cpp_class="CVXBuilding"><properties maxhealth="3000"/></class>)",
        R"(<class id="BaseBarracks" parent="Building" cpp_class="CVXBarrack"/>)",
        R"(<class id="RBarracks" parent="BaseBarracks" cpp_class="CVXBarrack"/>)",
        // The Gaul barracks answers to `Barrack`, which is the name the
        // executable's list actually carries.
        R"(<class id="GBarracks" altid="Barrack" parent="BaseBarracks" cpp_class="CVXBarrack">
             <properties race="Gaul"/></class>)",
        // A race whose level-3 row is spelt bare, for `UpgradeBestBarrack`.
        // `RBarracks` above declares no race on purpose: a raceless barrack
        // has no level 3 at all.
        R"(<class id="EBarracks" parent="BaseBarracks" cpp_class="CVXBarrack">
             <properties race="Egypt"/></class>)",
        // Race 4, the third of the three the switch spells with a prefix.
        R"(<class id="MBarracks" parent="BaseBarracks" cpp_class="CVXBarrack">
             <properties race="ImperialRome"/></class>)",
        // A descendant of a listed name, so that descent is measurable against
        // an exact-name match.
        R"(<class id="RBarracksWinter" parent="RBarracks" cpp_class="CVXBarrack"/>)",
        // Same C++ class, same parent, **not** one of the eight names.
        R"(<class id="RTemple" parent="BaseBarracks" cpp_class="CVXBarrack"/>)",
        // The one class here that binds `repair`, for `RepairAll`.
        R"(<class id="RShed" parent="Building" cpp_class="CVXBuilding">
             <method sig="repair" vs="data/subai/building_repair.vs"/>
             <method sig="idle" vs="data/subai/building_idle.vs"/></class>)",
    };
    const char* names[] = {"building.sc.xml", "basebarracks.sc.xml", "rbarracks.sc.xml",
                           "gbarracks.sc.xml", "ebarracks.sc.xml",   "mbarracks.sc.xml",
                           "rbarracksw.sc.xml", "rtemple.sc.xml",    "rshed.sc.xml"};
    for (int i = 0; i < 9; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    plain = graph.find("Building");
    rbarracks = graph.find("RBarracks");
    gbarracks = graph.find("GBarracks");
    ebarracks = graph.find("EBarracks");
    mbarracks = graph.find("MBarracks");
    winter = graph.find("RBarracksWinter");
    temple = graph.find("RTemple");
    shed = graph.find("RShed");
  }
};

struct BarrackBench {
  BarrackGraph classes;
  script::HostRegistry registry;
  World world;
  EconomySystem economy;
  CommandSystem commands;
  EnvSystem env;
  ObjectId anchor = kNoObject;
  ObjectId settlement_object = kNoObject;
  SettlementId town = kNoSettlement;

  BarrackBench() {
    script::declare_shipped_surface(registry);
    register_world_host(registry);
    register_command_host(registry);
    register_env_host(registry);
    register_economy_hosts(registry);
    world.set_class_graph(&classes.graph);
    CommandTable table;
    CHECK(table.merge(bytes_of(kResearchXml)).ok());
    commands.set_table(std::move(table));
    REQUIRE(world.add_system(&commands));
    REQUIRE(world.add_system(&env));
    REQUIRE(world.add_system(&economy));
    economy.start(world);
    env.start(world);

    anchor = world.spawn(NativeClass::building, nullptr, classes.plain);
    world.set_position(anchor, Point{1000, 1000});
    SettlementInit init;
    init.kind = SettlementKind::stronghold;
    init.anchor = anchor;
    init.owner = 2;
    init.max_units = 10000;
    economy.create(world, init);
    const Settlement* s = economy.settlements().for_object(anchor);
    REQUIRE(s != nullptr);
    town = s->id;
    settlement_object = s->object;
  }

  /// A member building of `which`, with `queued` commands already on it --
  /// training orders, which an order through the order core waits behind,
  /// where it would end a resting `idle` (`CommandSystem::append_order`).
  ObjectId add(ClassIndex which, std::size_t queued = 0,
               NativeClass native = NativeClass::barrack) {
    const ObjectId id = world.spawn(native, nullptr, which);
    world.set_position(id, Point{1100, 1000});
    world.set_health(id, 3000);
    CHECK(economy.add_building(world, town, id, 3000));
    for (std::size_t i = 0; i < queued; ++i) {
      (void)commands.add_command(world, id, false, "train", Command{});
    }
    CHECK(commands.command_count(id) == queued);
    return id;
  }

  script::Value settlement_value() const {
    return script::Value::object(kTypeSettlement, settlement_object);
  }

  script::HostOutcome call(const char* name, std::uint16_t arity,
                           std::vector<script::Value> args) {
    return call_host(registry, world, script::CallKind::member, name, arity, std::move(args));
  }

  void set_store(std::int32_t gold, std::int32_t food, std::int32_t population) {
    Settlement* s = economy.settlements().find(town);
    REQUIRE(s != nullptr);
    s->warehouse.max_gold = 1000000;
    s->warehouse.max_food = 1000000;
    s->warehouse.set(Resource::gold, gold);
    s->warehouse.set(Resource::food, food);
    s->population = population;
  }

  /// A call that answers an int, or -1 when it refused.
  std::int32_t count(const char* name, std::vector<script::Value> args) {
    const std::uint16_t arity = static_cast<std::uint16_t>(args.size() - 1);
    const script::HostOutcome out = call(name, arity, std::move(args));
    if (out.status != script::HostStatus::ok || !out.value.is_integer()) return -1;
    return out.value.as_integer();
  }

  /// A call that answers a bool, tri-state so a refusal is not a `false`.
  int truth(const char* name, std::vector<script::Value> args) {
    const std::int32_t got = count(name, std::move(args));
    return got < 0 ? -1 : (got != 0 ? 1 : 0);
  }

  ObjectId best(std::int32_t min_queue) {
    const script::HostOutcome out =
        call("BestBarrack", 1, {settlement_value(), script::Value::integer(min_queue)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() ? out.value.as_object().id : kNoObject;
  }

  /// `UpgradeBestBarrack(min_queue)`, tri-state like `truth`.
  int upgrade(std::int32_t min_queue) {
    return truth("UpgradeBestBarrack", {settlement_value(), script::Value::integer(min_queue)});
  }

  /// The verb at `index` on `id`'s queue, or empty.
  std::string verb_at(ObjectId id, std::size_t index = 0) const {
    return std::string(commands.command_name(id, index));
  }

  /// The `research` order queued last on `id`, or null.
  const Command* last_research(ObjectId id) const {
    const CommandQueue* q = commands.find(id);
    if (q == nullptr) return nullptr;
    for (std::size_t i = q->entries.size(); i > 0; --i) {
      if (q->entries[i - 1].verb == "research") return &q->entries[i - 1];
    }
    return nullptr;
  }

  void researched(const char* name) {
    env.env().write_string(EnvScope::for_settlement(town), name, "researched");
  }
};

struct LabGraph {
  ClassGraph graph;
  ClassIndex arena1 = kNoClass;
  ClassIndex arena2 = kNoClass;
  ClassIndex tavern = kNoClass;
  ClassIndex barracks = kNoClass;

  LabGraph() {
    const std::string docs[] = {
        R"(<class id="Building" cpp_class="CVXBuilding"><properties maxhealth="3000"/></class>)",
        R"(<class id="GArena1" parent="Building" cpp_class="CVXBuilding">
             <method sig="research" vs="data/subai/research.vs"/>
             <method sig="idle" vs="data/subai/building_idle.vs"/></class>)",
        // A descendant, so that `<src obj="GArena1"/>` matching the *tree* is
        // measurable against matching the leaf.
        R"(<class id="GArena1Winter" parent="GArena1" cpp_class="CVXBuilding"/>)",
        R"(<class id="GArena2" parent="Building" cpp_class="CVXBuilding">
             <method sig="research" vs="data/subai/research.vs"/>
             <method sig="idle" vs="data/subai/building_idle.vs"/></class>)",
        R"(<class id="Tavern" parent="Building" cpp_class="CVXBuilding">
             <method sig="research" vs="data/subai/research.vs"/>
             <method sig="idle" vs="data/subai/building_idle.vs"/></class>)",
        R"(<class id="Barracks" parent="Building" cpp_class="CVXBuilding">
             <method sig="idle" vs="data/subai/building_idle.vs"/></class>)",
    };
    const char* names[] = {"building.sc.xml", "arena1.sc.xml", "arena1w.sc.xml",
                           "arena2.sc.xml",   "tavern.sc.xml", "barracks.sc.xml"};
    for (int i = 0; i < 6; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    arena1 = graph.find("GArena1");
    arena2 = graph.find("GArena2");
    tavern = graph.find("Tavern");
    barracks = graph.find("Barracks");
  }
};

struct LabBench {
  LabGraph classes;
  script::HostRegistry registry;
  World world;
  EconomySystem economy;
  CommandSystem commands;
  ObjectId anchor = kNoObject;
  ObjectId settlement_object = kNoObject;
  SettlementId town = kNoSettlement;

  LabBench() {
    script::declare_shipped_surface(registry);
    register_world_host(registry);
    register_command_host(registry);
    register_economy_hosts(registry);
    world.set_class_graph(&classes.graph);
    CommandTable table;
    CHECK(table.merge(bytes_of(kResearchXml)).ok());
    commands.set_table(std::move(table));
    REQUIRE(world.add_system(&commands));
    REQUIRE(world.add_system(&economy));
    economy.start(world);

    anchor = world.spawn(NativeClass::building, nullptr, classes.barracks);
    world.set_position(anchor, Point{1000, 1000});
    SettlementInit init;
    init.kind = SettlementKind::stronghold;
    init.anchor = anchor;
    init.max_units = 10000;
    economy.create(world, init);
    const Settlement* s = economy.settlements().for_object(anchor);
    REQUIRE(s != nullptr);
    town = s->id;
    settlement_object = s->object;
    if (WorldObject* wo = world.find(anchor)) wo->settlement = settlement_object;
    set_store(1000, 1000, 50);
  }

  void set_store(std::int32_t gold, std::int32_t food, std::int32_t population) {
    Settlement* s = economy.settlements().find(town);
    CHECK(s != nullptr);
    if (s == nullptr) return;
    s->warehouse.max_gold = 1000000;
    s->warehouse.max_food = 1000000;
    s->warehouse.set(Resource::gold, gold);
    s->warehouse.set(Resource::food, food);
    s->population = population;
  }

  ObjectId add_building(ClassIndex which) {
    const ObjectId id = world.spawn(NativeClass::building, nullptr, which);
    world.set_position(id, Point{1100, 1000});
    world.set_health(id, 3000);
    CHECK(economy.add_building(world, town, id, 3000));
    return id;
  }

  script::Value settlement_value() const {
    return script::Value::object(kTypeSettlement, settlement_object);
  }

  script::HostOutcome call(const char* name, std::uint16_t arity,
                           std::vector<script::Value> args) {
    return call_host(registry, world, script::CallKind::member, name, arity, std::move(args));
  }

  bool can_afford(std::vector<script::Value> args) {
    const std::uint16_t arity = static_cast<std::uint16_t>(args.size() - 1);
    const script::HostOutcome out = call("CanAfford", arity, std::move(args));
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  }
};

}  // namespace

// --------------------------------------------------------------------------
// the AI's own bookkeeping, and the barracks walk
// --------------------------------------------------------------------------

/// **`BestBarrack` is an argmin with a cut-off, and the cut-off is the
/// argument.**
///
/// The two values the corpus passes are 100 ("just give me one") and 5 ("only
/// if it is not already backed up"), so both readings of the argument -- a
/// limit and a seed for the running best -- have to agree at those, and they
/// do: `0x004281a0` seeds the best from the argument itself, which makes them
/// the same thing.
TEST(best_barrack_is_the_shortest_queue_below_the_argument) {
  BarrackBench b;
  const ObjectId busy = b.add(b.classes.rbarracks, 7);
  const ObjectId quiet = b.add(b.classes.gbarracks, 2);
  const ObjectId middling = b.add(b.classes.winter, 4);

  // "Any barrack": the fewest queued wins outright.
  CHECK(b.best(100) == quiet);
  // Below 4 only `quiet` qualifies; below 3 still only `quiet`; below 2
  // nothing does, because the test is strictly less-than.
  CHECK(b.best(4) == quiet);
  CHECK(b.best(3) == quiet);
  CHECK(b.best(2) == kNoObject);
  // Between: `middling` is under the cut-off and `busy` is not.
  CHECK(b.best(5) == quiet);
  (void)middling;
  (void)busy;

  // Ties go to the earlier building -- strictly less-than over a forward walk.
  const ObjectId first = b.add(b.classes.rbarracks, 1);
  const ObjectId second = b.add(b.classes.rbarracks, 1);
  CHECK(b.best(100) == first);
  CHECK(second != first);
}

/// **The candidate test is eight class *names*, not the C++ class**, and this
/// is the case that tells the two apart.
TEST(best_barrack_takes_a_barracks_descendant_and_not_a_temple) {
  BarrackBench b;
  // Same `cpp_class`, same parent, a name that is not one of the eight.
  //
  // This also pins the half of `is_barrack_class` that has no guard in it: six
  // of the eight names are absent from this graph, and an absent name has to
  // match *nothing*. `ClassFilter::parse` gives it `count == 0` with
  // `match_all` false, which `World::matches_filter` reads as matching nothing;
  // the other reading would make every building a barracks here.
  const ObjectId temple = b.add(b.classes.temple, 0);
  CHECK(b.best(100) == kNoObject);

  // A descendant of a listed name qualifies: the original's test is `IsHeirOf`,
  // so a per-map winter variant of `RBarracks` is still a barracks.
  const ObjectId winter = b.add(b.classes.winter, 3);
  CHECK(b.best(100) == winter);

  // And the Gaul barracks answers to its `altid`, which is the name the
  // executable's list carries rather than the class id.
  const ObjectId gaul = b.add(b.classes.gbarracks, 1);
  CHECK(b.best(100) == gaul);
  CHECK(gaul != temple);

  // A ruin is out. `[b+0x204] == 3` is `Building::IsBroken`, the same stored
  // tier, so this is the predicate already written read from the other side.
  WorldObject* slot = b.world.find(gaul);
  REQUIRE(slot != nullptr);
  slot->state.damage_state = 3;
  CHECK(b.best(100) == winter);

  // A receiver that names no settlement answers the invalid handle rather than
  // refusing: `if (barrack.IsValid)` is what every shipped site is written for.
  const script::HostOutcome stray =
      b.call("BestBarrack", 1, {script::Value::integer(0), script::Value::integer(100)});
  CHECK(stray.status == script::HostStatus::ok);
  CHECK(!stray.value.as_object().valid());
}

/// **`UpgradeBestBarrack` keeps the busiest barrack it can upgrade, and a
/// level-1 upgrade is taken outright.**
///
/// 0x00437030: three rows tried in order per barrack, the first taken whatever
/// its queue, the other two only when the queue is strictly longer than the
/// running best the argument seeds. The winner gets `research` appended, costs
/// and delay riding along, nothing charged.
TEST(upgrade_best_barrack_takes_level_one_outright_and_the_busiest_above_it) {
  BarrackBench b;
  b.set_store(100000, 0, 50);
  const ObjectId busy = b.add(b.classes.rbarracks, 5);
  const ObjectId quiet = b.add(b.classes.gbarracks, 0);

  // Level 1 for everyone: the last barrack in the walk wins, queue or no queue.
  CHECK(b.upgrade(0) == 1);
  CHECK(b.commands.command_count(quiet) == 1);
  CHECK(b.commands.command_count(busy) == 5);
  const Command* order = b.last_research(quiet);
  REQUIRE(order != nullptr);
  CHECK(order->param == "NameSet, Barrack Level 1, , SetsSet, DrillSpeed, 40");
  CHECK(order->cost_gold == 400);
  CHECK(order->delay == 12000);
  const Settlement* s = b.economy.settlements().find(b.town);
  REQUIRE(s != nullptr);
  // Charged when queued -- the ASUS tick-2 dump has every queued row's
  // `costgold` already off its town's gold -- and this used to say the method
  // script pays, which no method script does.
  CHECK(s->warehouse.gold == 100000 - 400);

  // Appended, not replacing: the busy barrack keeps its five and gets a sixth
  // behind them once it is the last eligible one.
  b.env.env().write_string(EnvScope::for_building(quiet), "researching", "yes");
  CHECK(b.upgrade(0) == 1);
  CHECK(b.commands.command_count(busy) == 6);
  CHECK(b.verb_at(busy, 0) == "train");
  CHECK(b.verb_at(busy, 5) == "research");
  CHECK(b.commands.command_count(quiet) == 1);

  // Level 2 needs the queue *strictly* above the argument. With level 1 in
  // the ledger, `busy` has six: `UpgradeBestBarrack(6)` finds nothing,
  // `UpgradeBestBarrack(5)` finds it.
  b.env.env().write_string(EnvScope::for_building(quiet), "researching", "");
  b.researched("Barrack Level 1");
  b.world.clock().advance(600000);  // past every gate
  CHECK(b.upgrade(6) == 0);
  CHECK(b.commands.command_count(busy) == 6);
  CHECK(b.upgrade(5) == 1);
  CHECK(b.commands.command_count(busy) == 7);
  order = b.last_research(busy);
  REQUIRE(order != nullptr);
  CHECK(order->cost_gold == 800);
  CHECK(b.commands.command_count(quiet) == 1);

  // And the busiest of two level-2 candidates, which is `busy` at seven
  // against `quiet` at one; `UpgradeBestBarrack(0)` seeds the best at zero so
  // both qualify and the longer queue wins.
  CHECK(b.upgrade(0) == 1);
  CHECK(b.commands.command_count(busy) == 8);
  CHECK(b.commands.command_count(quiet) == 1);

  // A raceless barrack has no level 3: with level 2 in the ledger the Roman
  // one is out of rows, and the Gaul one wants `GBarrack Level 3`, whose
  // `Fights` requirement is unmet.
  b.researched("Barrack Level 2");
  CHECK(b.upgrade(0) == 0);
  b.researched("Fights");
  CHECK(b.upgrade(0) == 1);
  CHECK(b.commands.command_count(quiet) == 2);
  order = b.last_research(quiet);
  REQUIRE(order != nullptr);
  CHECK(order->cost_gold == 1600);
  CHECK(order->param.find("ReqSet, Fights") != std::string::npos);
  CHECK(b.commands.command_count(busy) == 8);
}

/// **The level-3 row is spelt with the race letter for Gaul, Rome and Imperial
/// Rome, and bare for the other five.**
///
/// The switch at 0x00437420 routes races 0, 1 and 4 through
/// `%sBarrack Level 3` and the rest through the literal. An Egyptian barrack
/// therefore wants the bare row and never the `G` one.
TEST(upgrade_best_barrack_spells_level_three_by_race) {
  BarrackBench b;
  b.set_store(100000, 0, 50);
  b.researched("Barrack Level 1");
  b.researched("Barrack Level 2");
  const ObjectId egypt = b.add(b.classes.ebarracks, 1);
  const ObjectId gaul = b.add(b.classes.gbarracks, 2);
  const ObjectId imperial = b.add(b.classes.mbarracks, 3);

  // `Fights` and `Shows` unmet: the Gaul and Imperial rows are
  // unavailable, the bare row is neither's, so only Egypt qualifies -- at
  // queue 1, above zero -- for all that the other two are busier.
  CHECK(b.upgrade(0) == 1);
  CHECK(b.commands.command_count(egypt) == 2);
  CHECK(b.commands.command_count(gaul) == 2);
  CHECK(b.commands.command_count(imperial) == 3);
  const Command* order = b.last_research(egypt);
  REQUIRE(order != nullptr);
  CHECK(order->param.find("Fights") == std::string::npos);

  // Imperial Rome's letter is `M`, and its row wants the shows.
  b.researched("Shows");
  CHECK(b.upgrade(0) == 1);
  CHECK(b.commands.command_count(imperial) == 4);
  CHECK(b.commands.command_count(egypt) == 2);
  order = b.last_research(imperial);
  REQUIRE(order != nullptr);
  CHECK(order->param.find("ReqSet, Shows") != std::string::npos);
  b.env.env().write_string(EnvScope::for_building(imperial), "researching", "yes");

  // With `Fights` met and Egypt busy researching, Gaul gets its own row --
  // the one carrying the `Fights` requirement, not the bare one.
  b.researched("Fights");
  b.env.env().write_string(EnvScope::for_building(egypt), "researching", "yes");
  CHECK(b.upgrade(0) == 1);
  CHECK(b.commands.command_count(gaul) == 3);
  CHECK(b.commands.command_count(egypt) == 2);
  order = b.last_research(gaul);
  REQUIRE(order != nullptr);
  CHECK(order->param.find("ReqSet, Fights") != std::string::npos);

  // Both rows record the same ledger name, `Barrack Level 3`, so once it reads
  // `researched` for the settlement neither barrack has a level 3 left.
  b.env.env().write_string(EnvScope::for_building(egypt), "researching", "");
  b.env.env().write_string(EnvScope::for_building(imperial), "researching", "");
  b.researched("Barrack Level 3");
  CHECK(b.upgrade(0) == 0);
  CHECK(b.commands.command_count(gaul) == 3);
  CHECK(b.commands.command_count(egypt) == 2);
  CHECK(b.commands.command_count(imperial) == 4);
}

/// **A non-zero argument gates on the clock: nothing in the first minute,
/// level 2 from five minutes, level 3 from ten -- and each gate is at the
/// exact boundary the executable compares against.**
///
/// 0x00437103 `jbe` 60,000; 0x0043720e `jb` 300,000 (skipped for an argument
/// `<= 0`); 0x00437254 `jb` 600,000 (skipped for an argument of exactly zero).
TEST(upgrade_best_barrack_gates_on_the_clock_only_for_a_nonzero_argument) {
  BarrackBench b;
  b.set_store(100000, 0, 50);
  const ObjectId barrack = b.add(b.classes.gbarracks, 9);
  // Walked *after* `barrack`, and what the first gate has to skip past rather
  // than stop at: a `continue` and a `return` differ only here.
  const ObjectId late = b.add(b.classes.gbarracks, 0);
  std::size_t queued = 9;

  // 60,000 is still "the first minute" for any non-zero argument, negative
  // included; 60,001 is not.
  b.world.clock().advance(60000);
  CHECK(b.upgrade(8) == 0);
  CHECK(b.upgrade(-1) == 0);
  CHECK(b.commands.command_count(barrack) == queued);
  CHECK(b.commands.command_count(late) == 0);
  b.world.clock().advance(1);
  // Level 1 is taken outright, so the last barrack in the walk gets it
  // however short its queue -- and a negative argument is no bar to that.
  CHECK(b.upgrade(8) == 1);
  CHECK(b.upgrade(-1) == 1);
  CHECK(b.commands.command_count(late) == 2);
  CHECK(b.commands.command_count(barrack) == queued);
  // With `late` researching, the walk ends on `barrack`.
  b.env.env().write_string(EnvScope::for_building(late), "researching", "yes");
  CHECK(b.upgrade(8) == 1);
  CHECK(b.commands.command_count(barrack) == ++queued);

  // Level 2 waits for 300,000 when the argument is positive, and does not
  // when it is zero. The researching barrack ahead of `barrack` in the walk
  // is gated -- and skipped, not returned on -- before `barrack` is reached.
  b.researched("Barrack Level 1");
  b.env.env().write_string(EnvScope::for_building(late), "researching", "");
  b.env.env().write_string(EnvScope::for_building(barrack), "researching", "yes");
  b.world.clock().advance(299999 - 60001);
  CHECK(b.upgrade(8) == 0);
  CHECK(b.upgrade(0) == 1);  // `late`'s two beat a seed of zero, not of eight
  CHECK(b.commands.command_count(late) == 3);
  b.env.env().write_string(EnvScope::for_building(barrack), "researching", "");
  b.env.env().write_string(EnvScope::for_building(late), "researching", "yes");
  CHECK(b.upgrade(8) == 0);
  CHECK(b.upgrade(0) == 1);
  CHECK(b.commands.command_count(barrack) == ++queued);
  b.world.clock().advance(1);
  CHECK(b.upgrade(8) == 1);
  CHECK(b.commands.command_count(barrack) == ++queued);
  CHECK(b.commands.command_count(late) == 3);

  // Level 3 waits for 600,000. A *negative* argument is gated here where it
  // was not at level 2 -- but a negative seed is `0xFFFFFFFF` to the unsigned
  // compare, so nothing ever beats it and the answer is no on either side of
  // the gate; what the gate changes is invisible, and this pins the boundary.
  b.researched("Barrack Level 2");
  b.researched("Fights");
  b.world.clock().advance(599999 - 300000);
  CHECK(b.upgrade(8) == 0);
  CHECK(b.upgrade(-1) == 0);
  CHECK(b.upgrade(0) == 1);
  CHECK(b.commands.command_count(barrack) == ++queued);
  b.world.clock().advance(1);
  CHECK(b.upgrade(-1) == 0);
  CHECK(b.upgrade(8) == 1);
  CHECK(b.commands.command_count(barrack) == ++queued);
  b.researched("Barrack Level 3");  // the Gaul row records the bare name
  CHECK(b.upgrade(0) == 0);
  CHECK(b.commands.command_count(barrack) == queued);
}

/// **A ruin is not skipped, a building that is already researching is, and
/// the two refusals.**
///
/// `BestBarrack` tests `[b+0x204]` and this walk does not; the building-side
/// `researching` flag is `VERIFY_RESEARCH.VS`'s own test, and it takes every
/// level with it because level 2 requires level 1 in the ledger. A receiver
/// that is no settlement prints and answers false; an empty settlement is a
/// false without a diagnostic.
TEST(upgrade_best_barrack_takes_a_ruin_and_leaves_a_researching_barrack) {
  BarrackBench b;
  b.set_store(100000, 0, 50);
  CHECK(b.upgrade(0) == 0);  // nothing to walk
  const ObjectId temple = b.add(b.classes.temple, 0);
  CHECK(b.upgrade(0) == 0);  // not a barrack by name
  CHECK(b.commands.command_count(temple) == 0);

  const ObjectId ruin = b.add(b.classes.winter, 3);
  WorldObject* slot = b.world.find(ruin);
  REQUIRE(slot != nullptr);
  slot->state.damage_state = 3;
  CHECK(b.best(100) == kNoObject);  // the walk next door skips it
  CHECK(b.upgrade(0) == 1);         // this one does not
  CHECK(b.commands.command_count(ruin) == 4);

  b.env.env().write_string(EnvScope::for_building(ruin), "researching", "yes");
  CHECK(b.upgrade(0) == 0);
  CHECK(b.commands.command_count(ruin) == 4);
  // A different building's flag is a different building's business.
  const ObjectId other = b.add(b.classes.rbarracks, 0);
  CHECK(b.upgrade(0) == 1);
  CHECK(b.commands.command_count(other) == 1);
  CHECK(b.commands.command_count(ruin) == 4);

  CHECK(zero_answer(b.call("UpgradeBestBarrack", 1, {script::Value::integer(0), script::Value::integer(0)})));
}

/// **A barracks resting on `idle` researches instead of resting.** The upgrade
/// goes through the order core, as `ExecCmd` does, and `BARRACK_IDLE.VS` never
/// returns: appended behind it, `Barrack Level 1` would have been charged and
/// never started. `CommandSystem::append_order` is the reading.
TEST(upgrade_best_barrack_ends_a_resting_idle_rather_than_waiting_behind_it) {
  BarrackBench b;
  b.set_store(100000, 0, 50);
  const ObjectId resting = b.add(b.classes.gbarracks, 0);
  // Priced, which no shipped `idle` is, so that the refund is visible: the
  // `idle` is ended the way `KillCommand` ends one, cost and all.
  Command priced;
  priced.cost_gold = 40;
  (void)b.commands.set_command(b.world, resting, "idle", priced);
  const Settlement* s = b.economy.settlements().find(b.town);
  REQUIRE(s != nullptr);
  CHECK(s->warehouse.gold == 100000 - 40);
  CHECK(b.upgrade(0) == 1);
  CHECK(b.commands.command_count(resting) == 1);
  CHECK(b.verb_at(resting, 0) == "research");
  CHECK(s->warehouse.gold == 100000 - 400);
}

/// **A building refuses to queue what its settlement cannot pay for, so a
/// cancel can never give back more than was taken.** Playtest #14's residue:
/// this engine charged what there was and refunded the full cost, so ordering
/// a training below its price and cancelling it made gold. Every insert asks
/// the accept test first (0x005b1760), which on a building is the payment
/// 0x004df070: gold below `costgold`, food below `costfood`, or population
/// below `costpop + MinPopulation` refuses, nothing is taken and nothing
/// queued; the refused command has still used its id.
TEST(a_building_refuses_to_queue_what_its_settlement_cannot_pay_for) {
  BarrackBench b;
  b.set_store(250, 0, 50);
  const ObjectId barracks = b.add(b.classes.rbarracks);
  const Settlement* s = b.economy.settlements().find(b.town);
  REQUIRE(s != nullptr);
  CommandDef train;
  train.name = "trainRHastatus";
  train.method = "train";
  train.train_command = true;
  train.cost_gold = 100;
  train.cost_pop = 1;

  // Two are paid for; the third finds 50 gold and is turned away whole.
  const std::uint32_t first = issue_order(b.world, barracks, train, OrderTarget{}, OrderMode::replace);
  const std::uint32_t second = issue_order(b.world, barracks, train, OrderTarget{}, OrderMode::replace);
  CHECK(first != 0);
  CHECK(second != 0);
  const std::uint32_t seed = b.world.command_id_seed();
  CHECK(issue_order(b.world, barracks, train, OrderTarget{}, OrderMode::replace) == 0);
  CHECK(b.world.command_id_seed() == seed + 1);  // made, and then refused
  CHECK(b.commands.command_count(barracks) == 2);
  CHECK(s->warehouse.gold == 50);
  CHECK(s->population == 48);

  // Cancelling both gives back exactly what was taken: the town is as it was.
  CHECK(b.commands.cancel_command(b.world, barracks, second));
  CHECK(b.commands.cancel_command(b.world, barracks, first));
  CHECK(b.commands.command_count(barracks) == 0);
  CHECK(s->warehouse.gold == 250);
  CHECK(s->population == 50);

  // A Ctrl press's five repeats are five executions, each paid on its own:
  // two queue and three are refused.
  std::size_t queued = 0;
  for (int r = 0; r < 5; ++r) {
    if (issue_order(b.world, barracks, train, OrderTarget{}, OrderMode::append) != 0) ++queued;
  }
  CHECK(queued == 2);
  CHECK(b.commands.command_count(barracks) == 2);
  CHECK(s->warehouse.gold == 50);
  CHECK(b.commands.clear_commands(b.world, barracks) == 1);
  CHECK(b.commands.kill_command(b.world, barracks));
  CHECK(s->warehouse.gold == 250);

  // Population: `costpop + MinPopulation` (10) is the floor, and reaching it
  // exactly is enough.
  b.set_store(1000, 0, 11);
  CHECK(issue_order(b.world, barracks, train, OrderTarget{}, OrderMode::append) != 0);
  CHECK(s->population == 10);
  CHECK(issue_order(b.world, barracks, train, OrderTarget{}, OrderMode::append) == 0);
  CHECK(s->warehouse.gold == 900);
  (void)b.commands.kill_command(b.world, barracks);

  // Food the same way.
  CommandDef feast = train;
  feast.cost_gold = 0;
  feast.cost_pop = 0;
  feast.cost_food = 400;
  b.set_store(1000, 399, 50);
  CHECK(issue_order(b.world, barracks, feast, OrderTarget{}, OrderMode::append) == 0);
  CHECK(s->warehouse.food == 399);
  CHECK(b.commands.command_count(barracks) == 0);
}

/// **A refused order changes nothing it would have replaced or ended.** The
/// insert pays before it clears a replaced queue or ends a resting `idle`
/// (0x005b4e90), so a replace cannot spend what the queue it clears is
/// holding, and a refusal leaves both where they were.
TEST(a_refused_order_leaves_the_queue_it_would_have_replaced) {
  BarrackBench b;
  b.set_store(100, 0, 50);
  const ObjectId barracks = b.add(b.classes.rbarracks);
  const Settlement* s = b.economy.settlements().find(b.town);
  REQUIRE(s != nullptr);
  Command priced;
  priced.cost_gold = 100;
  const std::uint32_t held = b.commands.add_command(b.world, barracks, false, "train", priced);
  REQUIRE(held != 0);
  CHECK(s->warehouse.gold == 0);
  // The refund of the replaced one would have paid for it; it is not counted.
  CHECK(b.commands.set_command(b.world, barracks, "research", priced) == 0);
  CHECK(b.commands.command_count(barracks) == 1);
  CHECK(b.verb_at(barracks, 0) == "train");
  CHECK(s->warehouse.gold == 0);
  CHECK(b.commands.add_command(b.world, barracks, true, "train", priced) == 0);
  CHECK(b.commands.command_count(barracks) == 1);

  // A resting `idle` is ended only for an order that is taken.
  (void)b.commands.set_command(b.world, barracks, "idle", Command{});
  CHECK(s->warehouse.gold == 100);
  b.set_store(99, 0, 50);
  CHECK(b.commands.append_order(b.world, barracks, "train", priced) == 0);
  CHECK(b.commands.command_count(barracks) == 1);
  CHECK(b.verb_at(barracks, 0) == "idle");
  b.set_store(100, 0, 50);
  CHECK(b.commands.append_order(b.world, barracks, "train", priced) != 0);
  CHECK(b.commands.command_count(barracks) == 1);
  CHECK(b.verb_at(barracks, 0) == "train");
  CHECK(s->warehouse.gold == 0);
}

/// **`GetTrainGold` is the row's `costgold`, and nothing for a name no row
/// carries.** 0x004229f0 through the command table's own lookup.
TEST(get_train_gold_is_the_rows_gold_cost) {
  BarrackBench b;
  const auto gold = [&](script::Value name) {
    const script::HostOutcome out =
        call_host(b.registry, b.world, script::CallKind::free_function, "GetTrainGold", 1, {name});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  CHECK(gold(script::Value::string("Barrack Level 1")) == 400);
  CHECK(gold(script::Value::string("barrack level 2")) == 800);  // the table's compare is case-blind
  CHECK(gold(script::Value::string("Nowhere")) == 1);
  CHECK(gold(script::Value::string("no such row")) == 0);
  CHECK(gold(script::Value::integer(7)) == 0);
}

namespace {

/// Two races, two unit types each, and a counter table over them: what
/// `GetCounterUnits`'s census reads. Costs are the fixture's own.
constexpr std::string_view kTrainXml = R"(<commands>
<cmd name="trainswordsman" costgold="70" costfood="10" method="train"/>
<cmd name="trainspearman" costgold="90" costfood="40" method="train"/>
<cmd name="trainhastatus" costgold="60" costfood="20" method="train"/>
<cmd name="trainprinciple" costgold="100" costfood="50" method="train"/>
</commands>)";

struct CounterBench {
  ClassGraph graph;
  script::HostRegistry registry;
  World world;
  EconomySystem economy;
  CommandSystem commands;
  EnvSystem env;
  CombatSystem combat;
  ClassIndex swordsman = kNoClass;
  ClassIndex spearman = kNoClass;
  ClassIndex hastatus = kNoClass;
  ClassIndex principle = kNoClass;
  ObjectId anchor = kNoObject;
  ObjectId settlement_object = kNoObject;
  SettlementId town = kNoSettlement;
  ScriptArrayId weights = kNoScriptArray;

  CounterBench() {
    const std::string docs[] = {
        R"(<class id="Building" cpp_class="CVXBuilding"><properties maxhealth="3000"/></class>)",
        R"(<class id="GSwordsman" cpp_class="CVXUnit"><properties maxhealth="100"/></class>)",
        R"(<class id="GSpearman" cpp_class="CVXUnit"><properties maxhealth="100"/></class>)",
        R"(<class id="RHastatus" cpp_class="CVXUnit"><properties maxhealth="100"/></class>)",
        R"(<class id="RPrinciple" cpp_class="CVXUnit"><properties maxhealth="100"/></class>)",
    };
    const char* names[] = {"building.sc.xml", "gsword.sc.xml", "gspear.sc.xml", "rhast.sc.xml",
                           "rprinc.sc.xml"};
    for (int i = 0; i < 5; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    swordsman = graph.find("GSwordsman");
    spearman = graph.find("GSpearman");
    hastatus = graph.find("RHastatus");
    principle = graph.find("RPrinciple");

    script::declare_shipped_surface(registry);
    register_world_host(registry);
    register_command_host(registry);
    register_env_host(registry);
    register_economy_hosts(registry);
    world.set_class_graph(&graph);
    CommandTable table;
    CHECK(table.merge(bytes_of(kTrainXml)).ok());
    commands.set_table(std::move(table));
    REQUIRE(world.add_system(&commands));
    REQUIRE(world.add_system(&env));
    REQUIRE(world.add_system(&economy));
    REQUIRE(world.add_system(&combat));
    economy.start(world);
    env.start(world);

    // Race 0 is Gaul and race 1 Rome, as the shipped table numbers them.
    std::vector<RaceUnits> races(2);
    races[0].name = "Gaul";
    races[0].rows = {UnitRow{"GSwordsman", "", "trainswordsman"},
                     UnitRow{"GSpearman", "", "trainspearman"}};
    races[1].name = "RepublicanRome";
    races[1].rows = {UnitRow{"RHastatus", "", "trainhastatus"},
                     UnitRow{"RPrinciple", "", "trainprinciple"}};
    env.units().set_races(std::move(races));

    CounterTable counters;
    counters.add(swordsman, hastatus, 33);
    counters.add(spearman, hastatus, 63);
    counters.add(principle, swordsman, 50);
    counters.add(hastatus, spearman, 40);
    combat.set_counter_table(std::move(counters));

    for (PlayerId p = 2; p <= 5; ++p) {
      world.players().setup(p).control = PlayerControl::computer;
      world.players().setup(p).race = p == 2 || p == 5 ? "Gaul" : "RepublicanRome";
    }
    world.players().setup(5).control = PlayerControl::disabled;

    anchor = world.spawn(NativeClass::building, nullptr, graph.find("Building"));
    world.set_position(anchor, Point{1000, 1000});
    SettlementInit init;
    init.kind = SettlementKind::stronghold;
    init.anchor = anchor;
    init.owner = 2;
    init.max_units = 10000;
    economy.create(world, init);
    const Settlement* s = economy.settlements().for_object(anchor);
    REQUIRE(s != nullptr);
    town = s->id;
    settlement_object = s->object;
    weights = world.arrays().acquire(7, 0, false);
  }

  void units(ClassIndex cls, PlayerId owner, int n) {
    for (int i = 0; i < n; ++i) {
      const ObjectId id = world.spawn(NativeClass::unit, nullptr, cls);
      world.set_position(id, Point{2000 + i, 2000});
      world.set_owner(id, owner);
      world.set_health(id, 100);
    }
  }

  script::HostOutcome ask(script::Value receiver) {
    return call_host(registry, world, script::CallKind::free_function, "GetCounterUnits", 2,
                     {receiver, make_array_value(kTypeIntArray, weights)});
  }

  std::int32_t weight(int k) {
    const script::Value v = world.arrays().get(weights, k);
    return v.is_integer() ? v.as_integer() : -1;
  }
};

}  // namespace

/// **`GetCounterUnits` weights the owner's unit types by what the enemies
/// field and the owner does not yet counter.** The census fallback of
/// 0x0042eef0: threshold `2000 / weight`, the owner's own weighted counters
/// subtracted, the remainder over 1,024, times the table's coefficient.
TEST(get_counter_units_weights_the_uncovered_enemy_mass) {
  CounterBench b;
  const script::Value set = script::Value::object(kTypeSettlement, b.settlement_object);
  // Nobody about: nothing, and the array stays as the script declared it.
  CHECK(b.ask(set).status == script::HostStatus::ok);
  CHECK(b.world.arrays().size(b.weights) == 0);

  // Thirty hastati (weight 80, threshold 25) against five swordsmen (400 of
  // cover): (2400 - 400) >> 10 = 1, so 33 for the swordsman and 63 for the
  // spearman, the two Gaul types that counter a hastatus.
  b.units(b.hastatus, 3, 30);
  b.units(b.swordsman, 2, 5);
  CHECK(b.ask(set).status == script::HostStatus::ok);
  CHECK(b.weight(0) == 33);
  CHECK(b.weight(1) == 63);
  // Added onto, never zeroed: a second ask doubles it.
  CHECK(b.ask(set).status == script::HostStatus::ok);
  CHECK(b.weight(0) == 66);
  CHECK(b.weight(1) == 126);

  // Fully covered: forty more swordsmen make 3600 of cover against 2400.
  b.units(b.swordsman, 2, 40);
  CHECK(b.ask(set).status == script::HostStatus::ok);
  CHECK(b.weight(0) == 66);
  CHECK(b.weight(1) == 126);
}

TEST(get_counter_units_counts_only_active_enemies_above_the_threshold) {
  CounterBench b;
  const script::Value set = script::Value::object(kTypeSettlement, b.settlement_object);
  // Twenty-four hastati are under the threshold of 25.
  b.units(b.hastatus, 3, 24);
  CHECK(b.ask(set).status == script::HostStatus::ok);
  CHECK(b.world.arrays().size(b.weights) == 0);
  // A disabled player's units are not counted, however many.
  b.units(b.hastatus, 5, 40);
  CHECK(b.ask(set).status == script::HostStatus::ok);
  CHECK(b.world.arrays().size(b.weights) == 0);
  // Nor a friend's: player 4 under a ceasefire from the owner's row.
  b.world.players().set(2, 4, Relation::ceasefire, true);
  b.units(b.hastatus, 4, 40);
  CHECK(b.ask(set).status == script::HostStatus::ok);
  CHECK(b.world.arrays().size(b.weights) == 0);
  // One more hastatus for player 3 crosses the threshold: 25 * 80 = 2000,
  // no cover, >> 10 is 1.
  b.units(b.hastatus, 3, 1);
  CHECK(b.ask(set).status == script::HostStatus::ok);
  CHECK(b.weight(0) == 33);
  CHECK(b.weight(1) == 63);
  // Dead units are not standing.
  for (const WorldObject& slot : b.world.objects()) {
    if (slot.state.owner == 3) b.world.set_health(slot.id, 0);
  }
  CHECK(b.ask(set).status == script::HostStatus::ok);
  CHECK(b.weight(0) == 33);

  // A receiver that is no settlement prints and leaves the array alone; an
  // owner with no race in the catalog gets nothing.
  CHECK(zero_answer(b.ask(script::Value::integer(0))));
  CHECK(b.weight(0) == 33);
  b.world.players().setup(2).race = "None";
  b.units(b.hastatus, 3, 40);
  CHECK(b.ask(set).status == script::HostStatus::ok);
  CHECK(b.weight(0) == 33);
}

namespace {

/// Ruins for `FindRuins`: the class the executable tests by descent, an heir,
/// and a building that is neither. The item catalogue is one entry, which is
/// all a ruin needs to hold something.
constexpr std::string_view kRuinItemsXml = R"(<items>
  <item id="Rusty ring" level="0" name="Rusty ring" important="no">
    <bonus health="0" damage="1" armor_slash="0" armor_pierce="0" level="0" experience="0"/>
  </item>
</items>)";

struct RuinBench {
  ClassGraph graph;
  ItemCatalog catalog;
  script::HostRegistry registry;
  World world;
  EconomySystem economy;
  EnvSystem env;
  HeroSystem heroes;
  ClassIndex base = kNoClass;
  ClassIndex gaul = kNoClass;
  ClassIndex fort = kNoClass;

  RuinBench() {
    const std::string docs[] = {
        R"(<class id="Building" cpp_class="CVXBuilding"><properties maxhealth="3000"/></class>)",
        R"(<class id="BaseRuins" parent="Building" cpp_class="CVXBuilding"><properties minlevel="4"/></class>)",
        R"(<class id="GRuins" parent="BaseRuins" cpp_class="CVXBuilding"/>)",
        R"(<class id="Fort" parent="Building" cpp_class="CVXBuilding"/>)",
    };
    const char* names[] = {"building.sc.xml", "baseruins.sc.xml", "gruins.sc.xml", "fort.sc.xml"};
    for (int i = 0; i < 4; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    base = graph.find("BaseRuins");
    gaul = graph.find("GRuins");
    fort = graph.find("Fort");
    REQUIRE(catalog.load(bytes_of(kRuinItemsXml)).ok());
    script::declare_shipped_surface(registry);
    register_world_host(registry);
    register_env_host(registry);
    register_economy_hosts(registry);
    world.set_class_graph(&graph);
    REQUIRE(world.add_system(&env));
    REQUIRE(world.add_system(&economy));
    REQUIRE(world.add_system(&heroes));
    economy.start(world);
    env.start(world);
    heroes.items().set_catalog(&catalog);
  }

  /// A settlement anchored on a building of `which` at `at`, with `minlevel`
  /// in its building scope and `items` rusty rings inside.
  ObjectId ruin(ClassIndex which, Point at, std::int32_t minlevel, int items) {
    const ObjectId id = world.spawn(NativeClass::building, nullptr, which);
    world.set_position(id, at);
    world.set_health(id, 3000);
    SettlementInit init;
    init.kind = SettlementKind::stronghold;
    init.anchor = id;
    init.owner = kNoPlayer;
    init.max_units = 10;
    CHECK(economy.create(world, init) != kNoSettlement);
    env.env().write_int(EnvScope::for_building(id), "minlevel", minlevel);
    for (int i = 0; i < items; ++i) {
      CHECK(heroes.items().add(world, id, "Rusty ring") != kNoObject);
    }
    return id;
  }

  ObjectId find(Point around, std::int32_t dist, std::int32_t level) {
    const script::HostOutcome out = call_host(
        registry, world, script::CallKind::free_function, "FindRuins", 3,
        {pack_point(around), script::Value::integer(dist), script::Value::integer(level)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() && out.value.as_object().valid() ? out.value.as_object().id
                                                                  : kNoObject;
  }

  /// What the next call will answer, by walking the same roll with a copy of
  /// the generator: one draw per candidate that reaches the coin.
  ObjectId expect(Point around, std::int32_t dist, std::int32_t level,
                  std::initializer_list<ObjectId> reaching) {
    Rng probe = world.rng();
    ObjectId found = kNoObject;
    for (const ObjectId id : reaching) {
      if (probe.between(0, 1) == 1) found = id;
    }
    (void)around;
    (void)dist;
    (void)level;
    return found;
  }
};

}  // namespace

/// **`FindRuins` is the last ruin within reach that a hero of the level may
/// loot, still holding something, on this side of the water, and lucky.**
/// 0x0042cba0's six tests in its order, one coin per survivor.
TEST(find_ruins_walks_the_settlements_and_flips_a_coin_per_candidate) {
  RuinBench b;
  const Point here{1000, 1000};
  // Reaching the coin: an heir with items and a low enough bar ...
  const ObjectId near_ruin = b.ruin(b.gaul, Point{1300, 1000}, 3, 1);
  // ... and a base-class ruin, later in the roll, which wins when both are lucky.
  const ObjectId later_ruin = b.ruin(b.base, Point{1000, 1400}, 5, 2);
  // Never reaching it: too far, the wrong class, too high a bar, or empty.
  const ObjectId far_ruin = b.ruin(b.gaul, Point{1000, 1600}, 1, 1);
  const ObjectId fort = b.ruin(b.fort, Point{1050, 1000}, 0, 1);
  const ObjectId proud = b.ruin(b.gaul, Point{1000, 900}, 6, 1);
  const ObjectId empty = b.ruin(b.gaul, Point{900, 1000}, 1, 0);

  b.world.rng().seed(0x1234);
  int lucky = 0;
  for (int i = 0; i < 24; ++i) {
    const ObjectId expected = b.expect(here, 500, 5, {near_ruin, later_ruin});
    const ObjectId got = b.find(here, 500, 5);
    CHECK(got == expected);
    if (got != kNoObject) ++lucky;
    CHECK(got != far_ruin);
    CHECK(got != fort);
    CHECK(got != proud);
    CHECK(got != empty);
  }
  CHECK(lucky > 0);
  CHECK(lucky < 24);

  // Reach is strict: at exactly 300 the near ruin is out -- and the later one,
  // 400 away, never in -- so nothing reaches the coin; 301 lets the near one in.
  b.world.rng().seed(7);
  CHECK(b.find(here, 300, 5) == kNoObject);
  CHECK(b.world.rng().state() == Rng(7).state());  // no candidate, no draw
  b.world.rng().seed(7);
  ObjectId expected = b.expect(here, 301, 5, {near_ruin});
  CHECK(b.find(here, 301, 5) == expected);
  // The bar is the building scope's `minlevel`, and a level below it is out.
  b.world.rng().seed(7);
  expected = b.expect(here, 500, 4, {near_ruin});
  CHECK(b.find(here, 500, 4) == expected);
  b.world.rng().seed(7);
  CHECK(b.find(here, 500, 2) == kNoObject);
  // Looted empty, it drops out; refilled, it is back.
  REQUIRE(b.heroes.items().remove_all_of_type(b.world, near_ruin, "Rusty ring") == 1);
  b.world.rng().seed(7);
  CHECK(b.find(here, 500, 4) == kNoObject);
  // Not a point: the invalid handle, no draw.
  const script::HostOutcome bad = call_host(b.registry, b.world, script::CallKind::free_function,
                                            "FindRuins", 3,
                                            {script::Value::integer(1), script::Value::integer(500),
                                             script::Value::integer(5)});
  CHECK(bad.status == script::HostStatus::ok);
  CHECK(!bad.value.as_object().valid());
}

namespace {

/// The independents' four outposts and their defenders, for
/// `AttackSetForTraining`. `range` is the sweep around a building whose
/// holder is short of defenders.
struct TrainingGroundBench {
  ClassGraph graph;
  script::HostRegistry registry;
  World world;
  EconomySystem economy;
  CommandSystem commands;
  HeroSystem heroes;

  TrainingGroundBench() {
    const std::string docs[] = {
        R"(<class id="Building" cpp_class="CVXBuilding"><properties maxhealth="3000" range="150"/></class>)",
        R"(<class id="COutpost" parent="Building" cpp_class="CVXBuilding"/>)",
        R"(<class id="IOutpost" parent="Building" cpp_class="CVXBuilding"/>)",
        R"(<class id="TTent" parent="Building" cpp_class="CVXBuilding"/>)",
        R"(<class id="TOutpost" parent="Building" cpp_class="CVXBuilding"/>)",
        R"(<class id="Fort" parent="Building" cpp_class="CVXBuilding"/>)",
        R"(<class id="Unit" cpp_class="CVXUnit"><properties maxhealth="100"/></class>)",
        R"(<class id="CMaceman" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="ISlinger" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="TTeutonArcher" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="TTeutonRider" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="TValkyrie" parent="Unit" cpp_class="CVXUnit"/>)",
    };
    const char* names[] = {"building.sc.xml", "coutpost.sc.xml", "ioutpost.sc.xml", "ttent.sc.xml",
                           "toutpost.sc.xml", "fort.sc.xml",     "unit.sc.xml",     "cmaceman.sc.xml",
                           "islinger.sc.xml", "tarcher.sc.xml",  "trider.sc.xml",   "tvalkyrie.sc.xml"};
    for (int i = 0; i < 12; ++i) graph.add(bytes_of(docs[i]), names[i]);
    graph.link();
    script::declare_shipped_surface(registry);
    register_world_host(registry);
    register_command_host(registry);
    register_economy_hosts(registry);
    world.set_class_graph(&graph);
    REQUIRE(world.add_system(&commands));
    REQUIRE(world.add_system(&economy));
    REQUIRE(world.add_system(&heroes));
    economy.start(world);
  }

  ObjectId unit_of(const char* cls, Point at, PlayerId owner) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, graph.find(cls));
    world.set_position(id, at);
    world.set_owner(id, owner);
    world.set_health(id, 100);
    heroes.register_unit(world, id);  // a level record, which the bars read
    return id;
  }

  /// An outpost of `cls` at `at`, owned by `owner`, garrisoned with `inside`.
  std::pair<ObjectId, SettlementId> outpost(const char* cls, Point at, PlayerId owner,
                                            std::initializer_list<ObjectId> inside) {
    const ObjectId id = world.spawn(NativeClass::building, nullptr, graph.find(cls));
    world.set_position(id, at);
    world.set_owner(id, owner);
    world.set_health(id, 3000);
    SettlementInit init;
    init.kind = SettlementKind::stronghold;
    init.anchor = id;
    init.owner = owner;
    init.max_units = 10;
    const SettlementId town = economy.create(world, init);
    CHECK(town != kNoSettlement);
    for (const ObjectId member : inside) CHECK(economy.garrison_add(town, member));
    return {id, town};
  }

  int ask(ObjectId unit, Point centre, std::int32_t radius) {
    const script::HostOutcome out = call_host(
        registry, world, script::CallKind::free_function, "AttackSetForTraining", 3,
        {script::Value::object(kTypeObj, unit), pack_point(centre), script::Value::integer(radius)});
    CHECK(out.status == script::HostStatus::ok);
    return out.status == script::HostStatus::ok ? (out.value.truthy_scalar() ? 1 : 0) : -1;
  }

  std::vector<std::string> verbs(ObjectId id) const {
    std::vector<std::string> out;
    const CommandQueue* q = commands.find(id);
    if (q == nullptr) return out;
    for (const Command& command : q->entries) out.emplace_back(command.verb);
    return out;
  }
  ObjectId target(ObjectId id, std::size_t index) const {
    const CommandQueue* q = commands.find(id);
    if (q == nullptr || index >= q->entries.size()) return kNoObject;
    return q->entries[index].arg_kind == CommandArgKind::object ? q->entries[index].object
                                                                 : kNoObject;
  }
};

}  // namespace

/// **`AttackSetForTraining` sends a warrior at the best-ranked independents'
/// outpost in the area that has two defenders of its own kind, and nothing
/// else.** 0x004397d0's walk, ranks, level bars, the two-defender rule inside
/// the holder or around the building, and the two commands it issues.
TEST(attack_set_for_training_picks_the_best_defended_outpost_and_engages) {
  TrainingGroundBench b;
  const Point centre{5000, 5000};
  const ObjectId warrior = b.unit_of("Unit", Point{5000, 4000}, 1);
  // Nothing around: false and nothing queued.
  CHECK(b.ask(warrior, centre, 3000) == 0);
  CHECK(b.verbs(warrior).empty());

  // A Carthaginian outpost with two macemen inside: approach it, then engage.
  const ObjectId m1 = b.unit_of("CMaceman", Point{5500, 5000}, 14);
  const ObjectId m2 = b.unit_of("CMaceman", Point{5500, 5000}, 14);
  const auto [carthage, carthage_town] = b.outpost("COutpost", Point{5500, 5000}, 14, {m1, m2});
  CHECK(b.ask(warrior, centre, 3000) == 1);
  CHECK(b.verbs(warrior) == (std::vector<std::string>{"approach", "engage"}));
  CHECK(b.target(warrior, 0) == carthage);
  CHECK(b.target(warrior, 1) == kNoObject);
  // Out of the area, or not the independents': nothing.
  CHECK(b.ask(warrior, Point{9000, 9000}, 500) == 0);
  b.world.set_owner(carthage, 3);
  {
    Settlement* s = b.economy.settlements().find(carthage_town);
    REQUIRE(s != nullptr);
    s->owner = 3;
  }
  CHECK(b.ask(warrior, centre, 3000) == 0);
  {
    Settlement* s = b.economy.settlements().find(carthage_town);
    s->owner = 14;
  }
  b.world.set_owner(carthage, 14);

  // An Iberian outpost, nearer, with two slingers: outranked by Carthage's
  // 10 over 9 -- and taken, with the exclusive engage at the last slinger
  // met, once Carthage is out of the area.
  const ObjectId s1 = b.unit_of("ISlinger", Point{5000, 4300}, 14);
  const ObjectId s2 = b.unit_of("ISlinger", Point{5000, 4300}, 14);
  const auto [iberia, iberia_town] = b.outpost("IOutpost", Point{5000, 4300}, 14, {s1, s2});
  CHECK(b.ask(warrior, centre, 3000) == 1);
  CHECK(b.target(warrior, 0) == carthage);
  CHECK(b.ask(warrior, Point{5000, 4300}, 400) == 1);
  CHECK(b.verbs(warrior) == (std::vector<std::string>{"approach", "engage_unit_type_exclusive"}));
  CHECK(b.target(warrior, 0) == iberia);
  CHECK(b.target(warrior, 1) == s2);
  (void)iberia_town;

  // Equal rank: the outpost nearer the *unit* wins, whatever the roll order.
  const ObjectId m3 = b.unit_of("CMaceman", Point{5000, 4100}, 14);
  const ObjectId m4 = b.unit_of("CMaceman", Point{5000, 4100}, 14);
  const auto [near_carthage, near_town] = b.outpost("COutpost", Point{5000, 4100}, 14, {m3, m4});
  CHECK(b.ask(warrior, centre, 3000) == 1);
  CHECK(b.target(warrior, 0) == near_carthage);
  (void)near_town;

  // Defenders are counted inside the holder and, short of two, within the
  // building's range around it; one alone is not enough.
  const ObjectId v1 = b.unit_of("TValkyrie", Point{3000, 5000}, 14);
  const auto [teuton, teuton_town] = b.outpost("TOutpost", Point{3000, 5000}, 14, {v1});
  b.heroes.set_level(warrior, 9);  // above the Teuton outpost's bar of 8
  CHECK(b.ask(warrior, Point{3000, 5000}, 300) == 0);
  const ObjectId v2 = b.unit_of("TValkyrie", Point{3100, 5000}, 14);  // within range 150
  CHECK(b.ask(warrior, Point{3000, 5000}, 300) == 1);
  CHECK(b.target(warrior, 0) == teuton);
  (void)v2;
  (void)teuton_town;
  // The level bar: at 8 the Teuton outpost is out; the tent's is 4, and its
  // archers and riders count together.
  b.heroes.set_level(warrior, 8);
  CHECK(b.ask(warrior, Point{3000, 5000}, 300) == 0);
  const ObjectId a1 = b.unit_of("TTeutonArcher", Point{7000, 5000}, 14);
  const ObjectId r1 = b.unit_of("TTeutonRider", Point{7000, 5000}, 14);
  const auto [tent, tent_town] = b.outpost("TTent", Point{7000, 5000}, 14, {a1, r1});
  b.heroes.set_level(warrior, 4);
  CHECK(b.ask(warrior, Point{7000, 5000}, 300) == 0);
  b.heroes.set_level(warrior, 5);
  CHECK(b.ask(warrior, Point{7000, 5000}, 300) == 1);
  CHECK(b.target(warrior, 0) == tent);
  (void)tent_town;
  // The area's edge is inclusive: an outpost exactly `radius` from the centre
  // is in, one unit further out is not.
  const ObjectId e1 = b.unit_of("CMaceman", Point{9000, 5000}, 14);
  const ObjectId e2 = b.unit_of("CMaceman", Point{9000, 5000}, 14);
  const auto [edge, edge_town] = b.outpost("COutpost", Point{9000, 5000}, 14, {e1, e2});
  CHECK(b.ask(warrior, Point{8700, 5000}, 300) == 1);
  CHECK(b.target(warrior, 0) == edge);
  CHECK(b.ask(warrior, Point{8699, 5000}, 300) == 0);
  (void)edge_town;
  // An equal rank at an equal distance from the unit keeps the settlement
  // later in the roll: the walk runs from the last to the first, and the
  // earlier one, met second, is no nearer.
  const ObjectId t1 = b.unit_of("CMaceman", Point{10000, 5300}, 14);
  const ObjectId t2 = b.unit_of("CMaceman", Point{10000, 5300}, 14);
  const auto [first_twin, first_twin_town] = b.outpost("COutpost", Point{10000, 5300}, 14, {t1, t2});
  const ObjectId t3 = b.unit_of("CMaceman", Point{10000, 4700}, 14);
  const ObjectId t4 = b.unit_of("CMaceman", Point{10000, 4700}, 14);
  const auto [later_twin, later_twin_town] = b.outpost("COutpost", Point{10000, 4700}, 14, {t3, t4});
  REQUIRE(b.world.set_position(warrior, Point{10000, 5000}));
  CHECK(b.ask(warrior, Point{10000, 5000}, 400) == 1);
  CHECK(b.target(warrior, 0) == later_twin);
  CHECK(first_twin != later_twin);
  (void)first_twin_town;
  (void)later_twin_town;
  REQUIRE(b.world.set_position(warrior, Point{5000, 4000}));
  // A building that is none of the four, however garrisoned: nothing.
  const ObjectId f1 = b.unit_of("CMaceman", Point{5000, 7000}, 14);
  const ObjectId f2 = b.unit_of("CMaceman", Point{5000, 7000}, 14);
  (void)b.outpost("Fort", Point{5000, 7000}, 14, {f1, f2});
  CHECK(b.ask(warrior, Point{5000, 7000}, 300) == 0);
  // A unit that does not resolve is false.
  CHECK(b.ask(999999, centre, 3000) == 0);
}

/// **`TSGetAllBarracks` and `BestBarrack` disagree, and the disagreement is
/// the finding.** The first keeps ruins and skips the C++ downcast; the second
/// does the opposite. Both run the same eight-name test, so a temple is in
/// neither.
TEST(ts_get_all_barracks_keeps_the_ruins_best_barrack_skips) {
  BarrackBench b;
  const ObjectId good = b.add(b.classes.rbarracks, 1);
  const ObjectId ruin = b.add(b.classes.gbarracks, 0);
  (void)b.add(b.classes.temple, 0);
  WorldObject* slot = b.world.find(ruin);
  REQUIRE(slot != nullptr);
  slot->state.damage_state = 3;

  const script::HostOutcome out = b.call("TSGetAllBarracks", 0, {b.settlement_value()});
  CHECK(out.status == script::HostStatus::ok);
  REQUIRE(is_objlist(out.value));
  const std::span<const ObjectId> items =
      objlist_pool_of(b.world).items(objlist_of(out.value));
  std::vector<ObjectId> ids(items.begin(), items.end());
  CHECK(ids.size() == 2);
  CHECK(std::find(ids.begin(), ids.end(), good) != ids.end());
  CHECK(std::find(ids.begin(), ids.end(), ruin) != ids.end());
  // The ruin is in the list and is not the answer to the other question.
  CHECK(b.best(100) == good);

  // A receiver that names no settlement still answers a list, and an empty one.
  const script::HostOutcome stray =
      b.call("TSGetAllBarracks", 0, {script::Value::integer(0)});
  CHECK(stray.status == script::HostStatus::ok);
  REQUIRE(is_objlist(stray.value));
  CHECK(objlist_pool_of(b.world).items(objlist_of(stray.value)).empty());
}

/// **`SpentGoldOnArmy` and `SpentGoldOnTech` are `Env` counters**, keyed by the
/// settlement and by its owner, and they add rather than replace.
///
/// Seventeen shipped writes and **no shipped read**, in either spelling: what
/// reads them is `Settlement::GoldSpentOnArmy` / `GoldSpentOnTech`, which
/// `gbr.exe` registers beside these two and no script calls. So the counter is
/// stored on `Unit::user`'s test, and it costs nothing to store because `Env`
/// is already saved and hashed.
TEST(spent_gold_accumulates_into_the_settlements_env_under_its_owner) {
  BarrackBench b;
  const EnvScope scope = EnvScope::for_settlement(b.town);
  const EnvStore& store = b.env.env();

  CHECK(b.call("SpentGoldOnArmy", 1, {b.settlement_value(), script::Value::integer(120)})
            .status == script::HostStatus::ok);
  CHECK(b.call("SpentGoldOnArmy", 1, {b.settlement_value(), script::Value::integer(30)})
            .status == script::HostStatus::ok);
  CHECK(b.call("SpentGoldOnTech", 1, {b.settlement_value(), script::Value::integer(7)})
            .status == script::HostStatus::ok);

  // The owner is 2, and the suffix is the **zero-based** index -- the field
  // `Obj::player` reads and then adds one to, without the add.
  CHECK(store.read_int(scope, "GoldSpentOnArmy2") == 150);
  CHECK(store.read_int(scope, "GoldSpentOnTech2") == 7);
  // Two counters, not one.
  CHECK(store.read_int(scope, "GoldSpentOnArmy0") == 0);

  // It records intent and spends nothing: the warehouse is untouched.
  const Settlement* s = b.economy.settlements().find(b.town);
  REQUIRE(s != nullptr);
  const std::int32_t gold = s->warehouse.gold;
  CHECK(b.call("SpentGoldOnArmy", 1, {b.settlement_value(), script::Value::integer(9999)})
            .status == script::HostStatus::ok);
  CHECK(b.economy.settlements().find(b.town)->warehouse.gold == gold);

  // No clamp: a negative argument subtracts, because the body is one `add`.
  CHECK(b.call("SpentGoldOnTech", 1, {b.settlement_value(), script::Value::integer(-20)})
            .status == script::HostStatus::ok);
  CHECK(store.read_int(scope, "GoldSpentOnTech2") == -13);

  // A receiver that names no settlement writes nothing and refuses, which is
  // every other settlement member's rule in this file.
  CHECK(zero_answer(b.call("SpentGoldOnArmy", 1, {script::Value::integer(0), script::Value::integer(5)})));
}



/// `StopReserving` writes one `Env` int and nothing else.
///
/// 0x00425980 formats `/<root>/Settlement<id>/Reserve<owner>` and stores 0. It
/// is the clear half of a food reserve whose arithmetic this file already
/// documents at `affordable_count`:
/// `Reserve > 0 ? ((GoldSpentOnArmy - ReservedAt) * Reserve) / 100 : 0`, all
/// three `Env` ints under the same scope with the same zero-based owner suffix.
/// With `ReserveFor/2` unimplemented the percentage is never anything but zero,
/// so the value written here is one this engine cannot yet make non-zero -- the
/// test sets it by hand for that reason.
TEST(stop_reserving_zeroes_the_settlements_reserve_percentage) {
  BarrackBench b;
  const EnvScope scope = EnvScope::for_settlement(b.town);
  EnvStore& store = b.env.env();

  // What `ReserveFor` would have written. The suffix is the owner, zero-based,
  // exactly as `SpentGoldOnArmy` above.
  store.write_int(scope, "Reserve2", 40);
  store.write_int(scope, "ReservedAt2", 100);
  store.write_int(scope, "GoldSpentOnArmy2", 900);

  CHECK(b.call("StopReserving", 0, {b.settlement_value()}).status == script::HostStatus::ok);
  CHECK(store.read_int(scope, "Reserve2") == 0);
  // Only the percentage: the two counters the formula reads are left alone, so
  // a reservation that restarts measures from where the spending actually is.
  CHECK(store.read_int(scope, "ReservedAt2") == 100);
  CHECK(store.read_int(scope, "GoldSpentOnArmy2") == 900);
  // And not another player's row.
  CHECK(store.read_int(scope, "Reserve0") == 0);
  store.write_int(scope, "Reserve0", 15);
  CHECK(b.call("StopReserving", 0, {b.settlement_value()}).status == script::HostStatus::ok);
  CHECK(store.read_int(scope, "Reserve0") == 15);

  CHECK(zero_answer(b.call("StopReserving", 0, {script::Value::integer(0)})));
}

/// **`ReserveFor` writes the three slots, and answers differently to the same
/// research and to the same percentage.**
///
/// 0x0042cd60: an unknown row is false; with a reserve running, the same
/// research is false and the same percentage is true *without a write*;
/// anything else writes `Reserve`, `ReserveFor` and `ReservedAt` (the current
/// `GoldSpentOnArmy`) and is true.
TEST(reserve_for_writes_three_slots_and_refuses_the_running_research) {
  BarrackBench b;
  const EnvScope scope = EnvScope::for_settlement(b.town);
  EnvStore& store = b.env.env();
  const auto reserve = [&](const char* tech, std::int32_t percent) {
    return b.truth("ReserveFor", {b.settlement_value(), script::Value::string(std::string(tech)),
                                  script::Value::integer(percent)});
  };
  store.write_int(scope, "GoldSpentOnArmy2", 500);

  CHECK(reserve("No Such Tech", 30) == 0);
  CHECK(store.find(scope, "Reserve2") == nullptr);

  CHECK(reserve("Feast", 30) == 1);
  CHECK(store.read_int(scope, "Reserve2") == 30);
  CHECK(store.read_string(scope, "ReserveFor2") == "Feast");
  CHECK(store.read_int(scope, "ReservedAt2") == 500);

  // The same research, any percentage: false, and nothing moves.
  store.write_int(scope, "GoldSpentOnArmy2", 800);
  CHECK(reserve("Feast", 45) == 0);
  CHECK(reserve("feast", 30) == 0);  // the compare is the table's, case-blind
  CHECK(store.read_int(scope, "Reserve2") == 30);
  CHECK(store.read_int(scope, "ReservedAt2") == 500);

  // Another research at the running percentage: true, and still nothing moves.
  CHECK(reserve("Fights", 30) == 1);
  CHECK(store.read_string(scope, "ReserveFor2") == "Feast");
  CHECK(store.read_int(scope, "ReservedAt2") == 500);

  // Another research at another percentage restarts the measure.
  CHECK(reserve("Fights", 45) == 1);
  CHECK(store.read_int(scope, "Reserve2") == 45);
  CHECK(store.read_string(scope, "ReserveFor2") == "Fights");
  CHECK(store.read_int(scope, "ReservedAt2") == 800);

  // `StopReserving` clears the switch, so the same research reserves again,
  // and the spelling stored is the script's, which is what it reads back.
  CHECK(b.call("StopReserving", 0, {b.settlement_value()}).status == script::HostStatus::ok);
  CHECK(reserve("fights", 10) == 1);
  CHECK(store.read_int(scope, "Reserve2") == 10);
  CHECK(store.read_string(scope, "ReserveFor2") == "fights");
  // The switch is `Reserve > 0`: a negative percentage is no reserve running,
  // so the same research reserves again rather than being refused.
  store.write_int(scope, "Reserve2", -5);
  CHECK(reserve("Fights", 10) == 1);
  CHECK(store.read_int(scope, "Reserve2") == 10);

  // The owner suffix is the settlement's, not a stray player's.
  CHECK(store.find(scope, "Reserve0") == nullptr);
  CHECK(zero_answer(b.call("ReserveFor", 2, {script::Value::integer(0), script::Value::string("Feast"),
                                  script::Value::integer(10)})));
}

/// **The reserve comes off gold, and the research it is held for is exempt.**
///
/// The count core (0x00425a10) divides `warehouse+0x18 - reserve` by the row's
/// `+0x1e8`, and `+0x1e8` is the field `CheckTechBudget` adds to
/// `GoldSpentOnTech` -- gold. An earlier note in this file had the reserve on
/// food; the test pins the correction. `MaxAffordCount` and both command forms
/// of `CanAfford` apply it unless `ReserveFor` names this command; the
/// `(gold, food)` form never applies it.
TEST(max_afford_count_and_can_afford_hold_the_reserve_back_from_gold) {
  BarrackBench b;
  const EnvScope scope = EnvScope::for_settlement(b.town);
  EnvStore& store = b.env.env();
  const auto max_count = [&](const char* cmd) {
    return b.count("MaxAffordCount", {b.settlement_value(), script::Value::string(std::string(cmd))});
  };
  const auto afford = [&](const char* cmd, std::int32_t n) {
    return b.truth("CanAfford", {b.settlement_value(), script::Value::string(std::string(cmd)),
                                 script::Value::integer(n)});
  };
  // Feast: 100 gold, 400 food, 3 heads. Gold is the binding term below.
  b.set_store(1000, 100000, 50);
  CHECK(max_count("Feast") == 10);
  CHECK(max_count("No Such Command") == 0);
  CHECK(max_count("Fights") == 0);            // 2000 gold each
  CHECK(max_count("Free Idea") == 0x7FFFFFFF);  // no costs: the seed falls through

  // (900 - 100) * 50 / 100 = 400 held back -> 600 / 100.
  store.write_int(scope, "Reserve2", 50);
  store.write_int(scope, "ReservedAt2", 100);
  store.write_int(scope, "GoldSpentOnArmy2", 900);
  CHECK(max_count("Feast") == 6);
  CHECK(afford("Feast", 6) == 1);
  CHECK(afford("Feast", 7) == 0);
  CHECK(b.truth("CanAfford", {b.settlement_value(), script::Value::string("Feast")}) == 1);
  // The `(gold, food)` form passes the flag 0: the whole store counts.
  CHECK(b.truth("CanAfford", {b.settlement_value(), script::Value::integer(700),
                              script::Value::integer(0)}) == 1);
  // And food is untouched by it: 100000 / 400 = 250 > 6, still gold-bound;
  // a food store that would bind at 5 binds at 5.
  b.set_store(1000, 2000, 50);
  CHECK(max_count("Feast") == 5);
  b.set_store(1000, 100000, 50);

  // Held for this very command: nothing is held back from it.
  store.write_string(scope, "ReserveFor2", "feast");
  CHECK(max_count("Feast") == 10);
  CHECK(afford("Feast", 10) == 1);
  CHECK(afford("Feast", 11) == 0);
  // But everything is held back from the others: 2300 - 400 < 2000.
  b.set_store(2300, 100000, 50);
  CHECK(max_count("Fights") == 0);
  CHECK(afford("Fights", 1) == 0);
  CHECK(b.truth("CanAfford", {b.settlement_value(), script::Value::string("Fights")}) == 0);
  store.write_string(scope, "ReserveFor2", "Fights");
  CHECK(max_count("Fights") == 1);
  CHECK(afford("Fights", 1) == 1);
  CHECK(b.truth("CanAfford", {b.settlement_value(), script::Value::string("Fights")}) == 1);
  store.write_string(scope, "ReserveFor2", "Feast");
  CHECK(max_count("Fights") == 0);

  // A reserve above the store is a count of zero, not a negative one.
  b.set_store(300, 100000, 50);
  CHECK(max_count("Fights") == 0);
  CHECK(max_count("Feast") == 3);  // exempt
  store.write_string(scope, "ReserveFor2", "Fights");
  CHECK(max_count("Feast") == 0);

  // No reserve, no percentage: `Reserve <= 0` is the switch, not the counters.
  store.write_int(scope, "Reserve2", 0);
  b.set_store(1000, 100000, 50);
  CHECK(max_count("Feast") == 10);
  store.write_int(scope, "Reserve2", -5);
  CHECK(max_count("Feast") == 10);
  // Spending below the mark holds nothing back either.
  store.write_int(scope, "Reserve2", 50);
  store.write_int(scope, "ReservedAt2", 900);
  CHECK(max_count("Feast") == 10);
  store.write_int(scope, "ReservedAt2", 300);
  CHECK(max_count("Feast") == 7);

  // A receiver that is no settlement, and a name that is no string.
  CHECK(b.count("MaxAffordCount", {script::Value::integer(0), script::Value::string("Feast")}) == 0);
  CHECK(max_count("") == 0);
  CHECK(b.count("MaxAffordCount", {b.settlement_value(), script::Value::integer(3)}) == 0);
}

/// **`CheckTechBudget` measures technology against everything spent or in
/// hand, and the reserve counts against the store and for its research.**
///
/// 0x00428ef0: `tech = GoldSpentOnTech + cost`; `total = GoldSpentOnArmy +
/// (gold - reserved) + tech`; true when `tech <= total * percent / 100`. When
/// the reserve is held for this research, a cost within it is true outright
/// and a cost above it is charged only for the excess.
TEST(check_tech_budget_is_a_share_of_army_store_and_technology) {
  BarrackBench b;
  const EnvScope scope = EnvScope::for_settlement(b.town);
  EnvStore& store = b.env.env();
  const auto within = [&](const char* tech, std::int32_t percent) {
    return b.truth("CheckTechBudget", {b.settlement_value(),
                                       script::Value::string(std::string(tech)),
                                       script::Value::integer(percent)});
  };
  b.set_store(1000, 100000, 50);
  store.write_int(scope, "GoldSpentOnArmy2", 900);
  store.write_int(scope, "GoldSpentOnTech2", 100);

  // Feast: tech 200 of total 2100. 20% is 420: yes. 9% is 189: no.
  CHECK(within("Feast", 20) == 1);
  CHECK(within("Feast", 9) == 0);
  // The boundary is `<=`: 2100 * 10 / 100 = 210 >= 200.
  CHECK(within("Feast", 10) == 1);
  // Fights: tech 2100 of total 4000. 52% is 2080: no. 53% is 2120: yes.
  CHECK(within("Fights", 52) == 0);
  CHECK(within("Fights", 53) == 1);
  CHECK(within("No Such Tech", 100) == 0);
  CHECK(within("Free Idea", 0) == 0);  // tech 100 > 0

  // A reserve of 400, held for Fights: cost 2000 -> 1600, store 1000 -> 600.
  // tech 1700 of total 900 + 600 + 1700 = 3200: 53% is 1696, no; 54% is
  // 1728, yes.
  store.write_int(scope, "Reserve2", 50);
  store.write_int(scope, "ReservedAt2", 100);
  store.write_string(scope, "ReserveFor2", "fights");
  CHECK(within("Fights", 53) == 0);
  CHECK(within("Fights", 54) == 1);
  // Feast is not the reserved research: full cost, reduced store.
  // tech 200 of 900 + 600 + 200 = 1700: 11% is 187, no; 12% is 204, yes.
  CHECK(within("Feast", 11) == 0);
  CHECK(within("Feast", 12) == 1);
  // Held for Feast, whose cost is within the reserve: true at any share.
  store.write_string(scope, "ReserveFor2", "Feast");
  CHECK(within("Feast", 0) == 1);
  // A reserve exactly the cost is still within it.
  store.write_int(scope, "ReservedAt2", 700);  // (900 - 700) * 50 / 100 = 100
  CHECK(within("Feast", 0) == 1);
  store.write_int(scope, "ReservedAt2", 702);  // 99: one over
  CHECK(within("Feast", 0) == 0);
  // Only the excess is charged: tech 101 of 900 + 901 + 101 = 1902, so 5% is
  // 95, no, and 6% is 114, yes.
  CHECK(within("Feast", 5) == 0);
  CHECK(within("Feast", 6) == 1);

  // The comparison is `<=`: with 900 in store, tech 200 of total 2000 is
  // exactly 10%.
  store.write_int(scope, "Reserve2", 0);
  b.set_store(900, 100000, 50);
  CHECK(within("Feast", 10) == 1);
  CHECK(within("Feast", 9) == 0);

  CHECK(zero_answer(b.call("CheckTechBudget", 2, {script::Value::integer(0), script::Value::string("Feast"),
                                       script::Value::integer(50)})));
  CHECK(b.truth("CheckTechBudget", {b.settlement_value(), script::Value::integer(1),
                                    script::Value::integer(50)}) == 0);
}

/// **`RepairAll` queues `repair` on every ruin whose class binds it, when the
/// row is affordable, and charges nothing.**
///
/// 0x0042dec0: damage tier 3, a class command list with a row named `repair`,
/// the count core with the reserve on gold and no exemption, then the
/// `ExecDefaultCmd` core with the replace flag set.
TEST(repair_all_queues_repair_on_the_affordable_ruins_that_bind_it) {
  BarrackBench b;
  const EnvScope scope = EnvScope::for_settlement(b.town);
  EnvStore& store = b.env.env();
  const auto ruin = [&](ObjectId id) {
    WorldObject* slot = b.world.find(id);
    REQUIRE(slot != nullptr);
    slot->state.damage_state = 3;
  };
  const auto verb = [&](ObjectId id) -> std::string {
    const CommandQueue* q = b.commands.find(id);
    const Command* running = q == nullptr ? nullptr : q->running();
    return running == nullptr ? std::string() : std::string(running->verb);
  };
  const auto repair_all = [&] {
    return b.call("RepairAll", 0, {b.settlement_value()}).status == script::HostStatus::ok;
  };

  const ObjectId first = b.add(b.classes.shed, 1, NativeClass::building);
  const ObjectId second = b.add(b.classes.shed, 0, NativeClass::building);
  const ObjectId whole = b.add(b.classes.shed, 0, NativeClass::building);
  const ObjectId temple = b.add(b.classes.temple, 0);
  ruin(first);
  ruin(second);
  ruin(temple);
  WorldObject* slot = b.world.find(whole);
  REQUIRE(slot != nullptr);
  slot->state.damage_state = 2;  // damaged, not broken

  // 40 gold against a 50-gold row: nothing.
  b.set_store(40, 0, 50);
  CHECK(repair_all());
  CHECK(verb(first) == "train");
  CHECK(verb(second).empty());

  // 50 gold: the first ruin, and **the cost is charged when the command is
  // queued**, so the second no longer counts -- 0x0042dec0 runs the
  // affordability count inline per building, against what is left. The
  // queued order is replaced, not appended to. (This used to read "the cost
  // is not charged, so the second still counts": the ASUS tick-2 dump has
  // every queued row's `costgold` off its town's gold 200 ms in.)
  b.set_store(50, 0, 50);
  CHECK(repair_all());
  CHECK(verb(first) == "repair");
  CHECK(b.commands.command_count(first) == 1);
  CHECK(verb(second).empty());
  CHECK(verb(whole).empty());
  CHECK(verb(temple).empty());
  const Settlement* s = b.economy.settlements().find(b.town);
  REQUIRE(s != nullptr);
  CHECK(s->warehouse.gold == 0);
  // With enough for both, both.
  b.commands.kill_command(b.world, first);
  b.set_store(100, 0, 50);
  CHECK(repair_all());
  CHECK(verb(first) == "repair");
  CHECK(verb(second) == "repair");
  CHECK(s->warehouse.gold == 0);
  // The row's costs and delay ride along, the way `ExecCmd` sends them.
  const CommandQueue* q = b.commands.find(first);
  REQUIRE(q != nullptr && q->running() != nullptr);
  CHECK(q->running()->cost_gold == 50);
  CHECK(q->running()->delay == 5000);

  // The reserve holds gold back here too, and `repair` gets no exemption
  // even when it is the reserved name.
  b.commands.kill_command(b.world, first);
  b.commands.kill_command(b.world, second);
  b.set_store(100, 0, 50);
  store.write_int(scope, "Reserve2", 50);
  store.write_int(scope, "ReservedAt2", 0);
  store.write_int(scope, "GoldSpentOnArmy2", 120);  // 60 held: 40 left
  store.write_string(scope, "ReserveFor2", "repair");
  CHECK(repair_all());
  CHECK(verb(first) != "repair");
  CHECK(verb(second) != "repair");
  store.write_int(scope, "GoldSpentOnArmy2", 100);  // 50 held: exactly enough for one
  CHECK(repair_all());
  CHECK(verb(first) == "repair");
  CHECK(verb(second) != "repair");

  // A settlement's last head is not spent: a row with a head cost needs two.
  CHECK(zero_answer(b.call("RepairAll", 0, {script::Value::integer(0)})));
}

/// `NearestStronghold` filters on how the caller sees each owner, and its
/// player argument is 1-based with an unconditional decrement.
///
/// 0x0042da70 walks the settlement array, keeps the ones whose central building
/// is a `BaseTownhall`, intersects a relation code against the filter mask, and
/// returns the nearest by `isqrt` of the squared distance. The relation code
/// (0x0044e250) is one of `AI_OWN` 1, `AI_ALLY` 2, `AI_ENEMY` 4 -- never a mask
/// -- and it reads the caller's own diplomacy row, not the transpose.
TEST(nearest_stronghold_filters_by_relation_and_takes_a_one_based_player) {
  World world;
  EconomySystem economy;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  REQUIRE(world.add_system(&economy));
  economy.start(world);
  HostContext context;
  context.world = &world;

  const auto place = [&](SettlementInit init, PlayerId owner, Point at) -> SettlementId {
    const ObjectId anchor = world.spawn(NativeClass::town_hall, nullptr);
    CHECK(world.set_position(anchor, at));
    init.owner = owner;
    init.anchor = anchor;
    return economy.create(world, init);
  };
  // Ascending distance from the origin, so "nearest" and "first in the store"
  // are different orders and the search has to be doing the arithmetic.
  const SettlementId far_mine = place(town_hall(), 1, Point{9000, 0});
  const SettlementId enemy_close = place(town_hall(), 3, Point{100, 0});
  const SettlementId ally_mid = place(town_hall(), 2, Point{400, 0});
  const SettlementId my_own = place(town_hall(), 1, Point{1000, 0});
  const SettlementId hamlet = place(village(), 1, Point{50, 0});  // nearer, not a stronghold
  // A tie, which only exists because the distance is truncated: from
  // (5010, 0) these are 100 and 101 squared units away and both `isqrt` to 10.
  const SettlementId tie_first = place(town_hall(), 1, Point{5000, 0});
  const SettlementId tie_second = place(town_hall(), 1, Point{5000, 1});
  (void)far_mine;
  (void)hamlet;
  (void)tie_second;
  world.players().set(1, 2, Relation::allied, true);

  const auto call = [&](std::vector<script::Value> args) -> SettlementId {
    const std::uint32_t index = registry.find(script::CallKind::free_function,
                                              "NearestStronghold",
                                              static_cast<std::uint16_t>(args.size()));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return kNoSettlement;
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "NearestStronghold";
    ctx.kind = script::CallKind::free_function;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    if (!out.value.is_object() || out.value.as_object().type == script::kNoType) {
      return kNoSettlement;
    }
    const Settlement* s = economy.settlements().for_object(out.value.as_object().id);
    return s == nullptr ? kNoSettlement : s->id;
  };
  const auto pt = [](std::int32_t x, std::int32_t y) {
    return pack_point(Point{x, y});
  };

  // Two arguments filters on `AI_OWN`: my nearest stronghold, not the enemy's
  // at a tenth the distance and not my own village nearer still.
  CHECK(call({pt(0, 0), script::Value::integer(2)}) == my_own);
  // `AI_ALLY` is a different question and a different answer.
  CHECK(call({pt(0, 0), script::Value::integer(2), script::Value::integer(2)}) == ally_mid);
  // `AI_ENEMY`, and the alliance is one-directional: player 2 grants nothing
  // back, so from player 3's side player 1 is an enemy too.
  CHECK(call({pt(0, 0), script::Value::integer(2), script::Value::integer(4)}) == enemy_close);
  // A mask with two bits takes whichever is nearest.
  CHECK(call({pt(0, 0), script::Value::integer(2), script::Value::integer(1 | 2)}) == ally_mid);

  // **Zero decrements to -1, which disables the filter entirely.** That is the
  // executable's own one-argument form written out, and it is why the
  // decrement has to be unconditional.
  CHECK(call({pt(0, 0), script::Value::integer(0)}) == enemy_close);
  // And a player past the table matches nothing at all.
  CHECK(call({pt(0, 0), script::Value::integer(99)}) == kNoSettlement);

  // Measured from the central building, so moving the point moves the answer.
  CHECK(call({pt(9000, 0), script::Value::integer(2)}) == far_mine);

  // The truncation is load bearing: 100 and 101 squared units are the same
  // distance once `isqrt` has had them, and a tie keeps the settlement the
  // store holds first. Comparing squared distances instead would pick the
  // other one and nothing else in this test would notice.
  CHECK(call({pt(5010, 0), script::Value::integer(2)}) == tie_first);
}

/// The affordability core answers a **count**, and the three shapes read it
/// against 1 or against their own number.
TEST(economy_can_afford_is_the_smallest_of_three_quotients) {
  LabBench b;
  b.set_store(/*gold=*/1000, /*food=*/400, /*population=*/50);

  const auto pair = [&](std::int32_t gold, std::int32_t food) {
    return b.can_afford({b.settlement_value(), script::Value::integer(gold),
                         script::Value::integer(food)});
  };
  CHECK(pair(1000, 400));
  CHECK(!pair(1001, 0));
  CHECK(!pair(0, 401));
  // A cost of zero is not a cost: the term is skipped, not divided by.
  CHECK(pair(0, 0));

  // The command forms. `Feast` costs 100 gold, 400 food and 3 population.
  CHECK(b.can_afford({b.settlement_value(), script::Value::string("Feast")}));
  // Two of them, at 200 gold and 800 food, is one food short.
  CHECK(!b.can_afford({b.settlement_value(), script::Value::string("Feast"),
                       script::Value::integer(2)}));
  b.set_store(1000, 800, 50);
  CHECK(b.can_afford({b.settlement_value(), script::Value::string("Feast"),
                      script::Value::integer(2)}));

  // **The population term is `population - 1`**, and `Feast` costs 3 a head.
  // Four heads buys one; three buys none.
  b.set_store(1000, 800, 4);
  CHECK(b.can_afford({b.settlement_value(), script::Value::string("Feast")}));
  b.set_store(1000, 800, 3);
  CHECK(!b.can_afford({b.settlement_value(), script::Value::string("Feast")}));

  // A name the table does not have is false, not a refusal -- **at both
  // arities**, which are two separate bodies reached by the same name.
  CHECK(!b.can_afford({b.settlement_value(), script::Value::string("Chariots")}));
  CHECK(!b.can_afford({b.settlement_value(), script::Value::string("Chariots"),
                       script::Value::integer(1)}));
}

/// `FindResearchLab` matches `<src obj>` against the class **tree**, and
/// answers the invalid handle when the settlement has no building that offers
/// the command.
TEST(economy_find_research_lab_matches_the_source_classes) {
  LabBench b;
  const ObjectId arena = b.add_building(b.classes.arena1);
  const ObjectId tavern = b.add_building(b.classes.tavern);

  const auto find = [&](const char* tech) {
    const script::HostOutcome out =
        b.call("FindResearchLab", 1, {b.settlement_value(), script::Value::string(tech)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() ? out.value.as_object().id : kNoObject;
  };

  CHECK(find("Fights") == arena);
  CHECK(find("Feast") == tavern);
  // `Nowhere` has no `<src>` at all, and `Chariots` is not a row.
  CHECK(find("Nowhere") == 0);
  CHECK(find("Chariots") == 0);

  // The **tree**, not the leaf: a `GArena1Winter` offers everything a `GArena1`
  // does. Asserted on a settlement whose only arena is the descendant.
  LabBench winter;
  const ObjectId cold = winter.add_building(winter.classes.graph.find("GArena1Winter"));
  const script::HostOutcome out = winter.call(
      "FindResearchLab", 1, {winter.settlement_value(), script::Value::string("Fights")});
  REQUIRE(out.value.is_object());
  CHECK(out.value.as_object().id == cold);
}

/// `Research` finds an idle lab, checks it can be paid for, and queues the
/// command there.
TEST(economy_research_queues_the_command_on_the_lab_that_offers_it) {
  LabBench b;
  const ObjectId arena = b.add_building(b.classes.arena1);
  b.add_building(b.classes.barracks);
  b.set_store(/*gold=*/9000, /*food=*/9000, /*population=*/50);

  const auto research = [&](const char* tech) {
    const script::HostOutcome out =
        b.call("Research", 1, {b.settlement_value(), script::Value::string(tech)});
    CHECK(out.status == script::HostStatus::ok);
  };

  research("Fights");
  const CommandQueue* queue = b.commands.find(arena);
  REQUIRE(queue != nullptr);
  REQUIRE(queue->entries.size() == 1);
  // The `<cmd method>` is what gets queued, and the costs and the delay travel
  // with it so that `cmdcost_gold` and `.cmddelay` answer inside the script.
  CHECK(queue->entries.front().verb == "research");
  CHECK(queue->entries.front().cost_gold == 2000);
  CHECK(queue->entries.front().delay == 30000);
  CHECK(queue->entries.front().param == "SetsSet, levels/GSwordsman, 4, NameSet, Fights, default,");
  // And nowhere else: the barracks offers nothing.
  CHECK(b.commands.find(arena) != nullptr);
}

TEST(economy_research_refuses_quietly_and_never_traps) {
  LabBench b;
  const ObjectId arena = b.add_building(b.classes.arena1);

  const auto research = [&](const char* tech) {
    const script::HostOutcome out =
        b.call("Research", 1, {b.settlement_value(), script::Value::string(tech)});
    CHECK(out.status == script::HostStatus::ok);
  };
  const auto queued = [&] {
    const CommandQueue* q = b.commands.find(arena);
    return q == nullptr ? std::size_t{0} : q->entries.size();
  };

  // Unknown upgrade: *"No such upgrade %s"* into the discard sink, and nothing
  // queued.
  research("Chariots");
  CHECK(queued() == 0);

  // **Unaffordable.** `Fights` costs 2000 gold and the store holds 1000.
  research("Fights");
  CHECK(queued() == 0);

  // Affordable, and it lands.
  b.set_store(2000, 0, 50);
  research("Fights");
  CHECK(queued() == 1);

  // A settlement with no lab for the row queues nothing, and the receiver that
  // resolves to nothing does not trap.
  LabBench bare;
  bare.set_store(9000, 9000, 50);
  const script::HostOutcome nowhere =
      bare.call("Research", 1, {bare.settlement_value(), script::Value::string("Fights")});
  CHECK(nowhere.status == script::HostStatus::ok);
  const script::HostOutcome dead =
      bare.call("Research", 1, {script::Value::object(kTypeSettlement, 999999),
                                script::Value::string("Fights")});
  CHECK(dead.status == script::HostStatus::ok);
}

/// **`Research` wants an idle lab and `FindResearchLab` does not**, which is
/// the flag the two callers pass to one finder.
TEST(economy_research_skips_a_busy_lab_and_the_finder_does_not) {
  LabBench b;
  const ObjectId arena = b.add_building(b.classes.arena1);
  b.set_store(9000, 9000, 50);

  Command busy;
  busy.verb = "research";
  b.commands.set_command(b.world, arena, "research", busy);
  REQUIRE(b.commands.find(arena) != nullptr);
  REQUIRE(b.commands.find(arena)->running() != nullptr);

  // The finder still names it -- 0x0042d010 is called with the flag clear.
  const script::HostOutcome found =
      b.call("FindResearchLab", 1, {b.settlement_value(), script::Value::string("Fights")});
  REQUIRE(found.value.is_object());
  CHECK(found.value.as_object().id == arena);

  // `Research` does not queue behind it.
  const std::size_t before = b.commands.find(arena)->entries.size();
  b.call("Research", 1, {b.settlement_value(), script::Value::string("Fights")});
  CHECK(b.commands.find(arena)->entries.size() == before);
}

/// `CanResearch` is a settlement's question before it is a ledger's:
/// 0x00434870 wants the row, then **an idle building of the settlement that
/// offers it**, and only then reads `researched`/`researching` -- and only for
/// a row that names a ledger key. `hireheroR` names none, so it is
/// researchable exactly when its temple is idle. This used to answer "no
/// such upgrade" for every row outside the research catalog, which is every
/// hire and every purchase the tactic scripts open with.
TEST(can_research_wants_an_idle_lab_and_skips_the_ledger_for_a_hire) {
  BarrackBench b;
  const auto can = [&](const char* name) {
    return b.truth("CanResearch", {b.settlement_value(), script::Value::string(name)});
  };
  // No temple: nothing offers the row.
  CHECK(can("hireheroR") == 0);
  const ObjectId temple = b.add(b.classes.temple, 0, NativeClass::building);
  CHECK(can("hireheroR") == 1);
  // A temple already hiring is not idle.
  Command busy;
  busy.verb = "hirehero";
  b.commands.set_command(b.world, temple, "hirehero", busy);
  b.commands.find(temple)->entries.front().started = true;
  CHECK(can("hireheroR") == 0);
  // Its resting `idle` is what an idle lab runs.
  Command idle;
  idle.verb = "idle";
  b.commands.set_command(b.world, temple, "idle", idle);
  CHECK(can("hireheroR") == 1);
  // A research row is still refused once its ledger says so.
  b.researched("hireheroR");  // no `NameSet`: the ledger is not consulted
  CHECK(can("hireheroR") == 1);
  // And a row nothing in the settlement offers -- the test table's
  // `Barrack Level 1` carries no `<src>` -- is not researchable anywhere,
  // whatever the ledger says.
  CHECK(can("Barrack Level 1") == 0);
  // A name the table does not carry is "no such upgrade".
  CHECK(can("Chariots") == 0);
}

/// **`Research` replaces what the lab is running**, the way 0x0042d2b5 issues
/// it -- through 0x004efc00 with the replace flag set -- and does not queue
/// behind it. A lab's running command is its `idle`, which never returns, so
/// an appended research never started: this is the shape that kept every
/// AI upgrade "queued" forever.
TEST(economy_research_replaces_the_labs_running_idle) {
  LabBench b;
  const ObjectId arena = b.add_building(b.classes.arena1);
  b.set_store(9000, 9000, 50);
  Command idle;
  idle.verb = "idle";
  b.commands.set_command(b.world, arena, "idle", idle);
  // An idle that has started is what a loaded lab is running; `Research`
  // still wants an idle lab, and *idle* is the one verb it waves through.
  REQUIRE(b.commands.find(arena) != nullptr);
  REQUIRE(b.commands.find(arena)->entries.size() == 1);

  b.call("Research", 1, {b.settlement_value(), script::Value::string("Fights")});
  const CommandQueue* q = b.commands.find(arena);
  REQUIRE(q != nullptr);
  REQUIRE(q->entries.size() == 1);
  CHECK(q->entries.front().verb == "research");
}

/// **A command's cost is charged when it is queued and returned when it is
/// cancelled.** The ASUS tick-2 dump: 200 ms in, every queued row's
/// `costgold` is already off its town's gold. `SpentGoldOnArmy` is a counter
/// and no method script pays, so the command core is what charges.
TEST(economy_a_queued_command_is_charged_and_a_cancelled_one_refunded) {
  LabBench b;
  const ObjectId arena = b.add_building(b.classes.arena1);
  const ObjectId tavern = b.add_building(b.classes.tavern);
  b.set_store(9000, 9000, 50);
  const Settlement* s = b.economy.settlements().find(b.town);
  REQUIRE(s != nullptr);

  // `Fights`: 2000 gold. `Feast`: 100 gold, 400 food, 3 heads.
  b.call("Research", 1, {b.settlement_value(), script::Value::string("Fights")});
  CHECK(s->warehouse.gold == 7000);
  b.call("Research", 1, {b.settlement_value(), script::Value::string("Feast")});
  CHECK(s->warehouse.gold == 6900);
  CHECK(s->warehouse.food == 8600);
  CHECK(s->population == 47);

  // Killing the running research puts it back; replacing it does too.
  CHECK(b.commands.kill_command(b.world, tavern));
  CHECK(s->warehouse.gold == 7000);
  CHECK(s->warehouse.food == 9000);
  CHECK(s->population == 50);
  Command idle;
  idle.verb = "idle";
  b.commands.set_command(b.world, arena, "idle", idle);
  CHECK(s->warehouse.gold == 9000);

  // And a cost-free order -- the whole `SetCommand("move")` family -- moves
  // nothing.
  b.commands.set_command(b.world, arena, "idle", idle);
  CHECK(s->warehouse.gold == 9000);
  // Clearing the tail refunds the tail and leaves the runner charged.
  b.call("Research", 1, {b.settlement_value(), script::Value::string("Feast")});
  CHECK(s->warehouse.gold == 8900);
  Command more = b.commands.find(tavern)->entries.front();
  (void)b.commands.add_command(b.world, tavern, false, "research", more);
  CHECK(s->warehouse.gold == 8800);
  CHECK(b.commands.clear_commands(b.world, tavern) == 1);
  CHECK(s->warehouse.gold == 8900);
}

/// **A queued command is cancelled by its id, and refunded wherever it
/// stands.** `CVXCmdCancelCmd` (0x004e63c0) finds the command by the id the
/// strip kept (0x005ae170) and removes it through 0x005b07d0, which puts a
/// row's gold, food and population back first (0x005b18d0, a building's
/// 0x004df400) and then ends index 0 as `KillCommand` does, or erases any
/// other unrun.
TEST(economy_a_cancelled_queue_entry_is_refunded_and_the_rest_keep_their_place) {
  LabBench b;
  const ObjectId tavern = b.add_building(b.classes.tavern);
  b.set_store(9000, 9000, 50);
  const Settlement* s = b.economy.settlements().find(b.town);
  REQUIRE(s != nullptr);

  // Three feasts: 100 gold, 400 food, 3 heads each.
  b.call("Research", 1, {b.settlement_value(), script::Value::string("Feast")});
  Command feast = b.commands.find(tavern)->entries.front();
  (void)b.commands.add_command(b.world, tavern, false, "research", feast);
  (void)b.commands.add_command(b.world, tavern, false, "research", feast);
  const CommandQueue* q = b.commands.find(tavern);
  REQUIRE(q != nullptr);
  REQUIRE(q->entries.size() == 3);
  CHECK(s->warehouse.gold == 8700);
  CHECK(s->warehouse.food == 7800);
  CHECK(s->population == 41);
  const std::uint32_t head = q->entries[0].id;
  const std::uint32_t middle = q->entries[1].id;
  const std::uint32_t last = q->entries[2].id;

  // The middle one: its cost comes back and the other two stay in order.
  CHECK(b.commands.cancel_command(b.world, tavern, middle));
  REQUIRE(q->entries.size() == 2);
  CHECK(q->entries[0].id == head);
  CHECK(q->entries[1].id == last);
  CHECK(s->warehouse.gold == 8800);
  CHECK(s->warehouse.food == 8200);
  CHECK(s->population == 44);
  // Gone is gone: a second cancel of it finds nothing and refunds nothing.
  CHECK(!b.commands.cancel_command(b.world, tavern, middle));
  CHECK(s->warehouse.gold == 8800);
  // Nor does an id no command ever had, or none at all.
  CHECK(!b.commands.cancel_command(b.world, tavern, 0));
  CHECK(!b.commands.cancel_command(b.world, tavern, last + 1000));
  CHECK(!b.commands.cancel_command(b.world, tavern + 1000, last));

  // The running one is refunded too, and the next takes its place.
  CHECK(b.commands.cancel_command(b.world, tavern, head));
  REQUIRE(!q->entries.empty());
  CHECK(q->entries[0].id == last);
  CHECK(s->warehouse.gold == 8900);
  CHECK(s->warehouse.food == 8600);
  CHECK(s->population == 47);

  // Through the stream: an order every peer applies, filtered as every
  // order is by whether the issuer may command the building.
  (void)b.world.set_owner(tavern, 2);
  b.world.players().set_relation_word(2, 2, 0x35);
  NetTurn turn;
  NetOrder cancel;
  cancel.kind = NetOrderKind::cancel_command;
  cancel.issuer = 5;  // not the owner, and granted nothing
  cancel.target.object = tavern;
  cancel.command_id = last;
  turn.orders = {cancel};
  NetTurnReport report = apply_turn(b.world, turn);
  CHECK(report.cancels == 0);
  CHECK(report.unapplied == 1);
  CHECK(q->entries[0].id == last);
  CHECK(s->warehouse.gold == 8900);
  turn.orders[0].issuer = 2;
  report = apply_turn(b.world, turn);
  CHECK(report.cancels == 1);
  CHECK(report.unapplied == 0);
  CHECK(s->warehouse.gold == 9000);
  CHECK(s->warehouse.food == 9000);
  CHECK(s->population == 50);
  CHECK(std::none_of(q->entries.begin(), q->entries.end(),
                     [last](const Command& c) { return c.id == last; }));
  // Applied twice -- a replay, a duplicate -- it finds nothing the second time.
  report = apply_turn(b.world, turn);
  CHECK(report.cancels == 0);
  CHECK(s->warehouse.gold == 9000);
}

/// **A row's `onaddremovescript` runs after the payment and before the
/// refund, and a refusal gives the payment back.** The accept test (0x005b1760)
/// takes the cost through the building's payment (`vtbl+0x84`, 0x004df070),
/// then runs the row's script with `bAdd` true, and on a false answer calls
/// the refund (`vtbl+0x88`, 0x004df400) and refuses the insert. A cancel goes
/// through `vtbl+0x90` (0x005b18d0), which refunds first and runs the script
/// with `bAdd` false after. `set.Research` is the route `ESH_ARENAUNITS.VS`
/// hires by, so the command it queues has to carry its row for the queue to
/// find the script at all. The hook here is written for the test: it writes
/// the gold it sees into the tavern's `user` word, and refuses the first add.
TEST(economy_a_rows_onaddremovescript_runs_between_the_payment_and_the_refund) {
  LabBench b;
  const ObjectId tavern = b.add_building(b.classes.tavern);
  b.set_store(9000, 9000, 50);
  const Settlement* s = b.economy.settlements().find(b.town);
  REQUIRE(s != nullptr);

  script::Scheduler scheduler;
  WorldHost host{b.world};
  HostContext context;
  context.world = &b.world;
  script::register_scheduler_builtins(b.registry);
  scheduler.set_registry(&b.registry);
  scheduler.set_host(&host);
  scheduler.set_user(&context);
  b.commands.set_scheduler(&scheduler);
  const std::string_view source =
      "// bool, Obj This, bool bAdd\n"
      "Building bld; int seen; bool first;\n"
      "bld = This.AsBuilding;\n"
      "seen = bld.settlement.gold;\n"
      "first = This.user == 0;\n"
      "This.SetUser(seen);\n"
      "if (first) return false;\n"
      "return true;\n";
  script::Diagnostic diagnostic;
  const auto parsed = script::parse(bytes_of(source), "data/subai/seen_gold.vs", &diagnostic);
  REQUIRE(parsed.ok());
  script::CompileError error;
  auto chunk = script::compile(parsed.value(), &b.registry, &error);
  REQUIRE(chunk.ok());
  REQUIRE(scheduler.add_chunk(std::move(chunk.value())) != script::kNoChunk);
  const CommandDef* feast = b.commands.table().find("Feast");
  REQUIRE(feast != nullptr);
  CommandDef hooked = *feast;
  hooked.on_add_remove = "data/subai/seen_gold.vs";
  b.commands.mutable_table().set(hooked);

  // Refused: the script saw the gold with the 100 already taken, and the 100
  // came back; nothing was queued.
  b.call("Research", 1, {b.settlement_value(), script::Value::string("Feast")});
  CHECK(b.world.find(tavern)->state.user == 8900);
  CHECK(s->warehouse.gold == 9000);
  CHECK(s->warehouse.food == 9000);
  CHECK(s->population == 50);
  CHECK(b.commands.command_count(tavern, "research") == 0);

  // Accepted: paid, and queued.
  b.call("Research", 1, {b.settlement_value(), script::Value::string("Feast")});
  CHECK(b.world.find(tavern)->state.user == 8900);
  CHECK(s->warehouse.gold == 8900);
  REQUIRE(b.commands.command_count(tavern, "research") == 1);

  // Cancelled: refunded first, then the script told.
  const std::uint32_t id = b.commands.find(tavern)->entries.front().id;
  CHECK(b.commands.cancel_command(b.world, tavern, id));
  CHECK(s->warehouse.gold == 9000);
  CHECK(b.world.find(tavern)->state.user == 9000);
}

/// `AllowCapture` writes the field `CanBeCaptured` reads. One word, a setter
/// and a getter, and no new state -- which is the whole finding.
TEST(allow_capture_writes_the_field_can_be_captured_reads) {
  LabBench b;
  Settlement* town = b.economy.settlements().find(b.town);
  REQUIRE(town != nullptr);
  town->can_be_captured = true;
  town->capture_health_percent = 100;

  const auto readable = [&] {
    return b.call("CanBeCaptured", 0, {b.settlement_value()}).value.as_integer() != 0;
  };
  CHECK(readable());
  CHECK(b.economy.is_valid_capture_target(b.world, b.town));

  // An adventure script locking a settlement: `1_Great_Losses_Rome:seq15.vs`
  // closes `S_TraitorTown` and reopens it two minutes and a conversation later.
  CHECK(b.call("AllowCapture", 1, {b.settlement_value(), script::Value::boolean(false)})
            .status == script::HostStatus::ok);
  CHECK(!readable());
  CHECK(!town->can_be_captured);
  // And the point of writing it: the capture rule answers differently at once.
  CHECK(!b.economy.is_valid_capture_target(b.world, b.town));

  CHECK(b.call("AllowCapture", 1, {b.settlement_value(), script::Value::boolean(true)})
            .status == script::HostStatus::ok);
  CHECK(readable());
  CHECK(b.economy.is_valid_capture_target(b.world, b.town));

  // An unresolvable receiver writes nothing, and does not reach through to some
  // other settlement.
  CHECK(zero_answer(b.call("AllowCapture", 1, {script::Value::object(kTypeSettlement, 999999),
                                   script::Value::boolean(false)})));
  CHECK(town->can_be_captured);
}

/// `GetSettlements(class, player)` -- the string is a class and the list holds
/// central buildings, neither of which the name says.
TEST(get_settlements_filters_by_class_and_treats_player_zero_as_a_wildcard) {
  LabBench b;
  script::HostRegistry& registry = b.registry;
  (void)register_player_host(registry);
  (void)register_objlist_host(registry);

  Settlement* town = b.economy.settlements().find(b.town);
  REQUIRE(town != nullptr);
  town->owner = 0;  // player 1 on the wire

  const auto list_of = [&](const char* klass, std::int64_t player) {
    const script::HostOutcome out =
        call_host(registry, b.world, script::CallKind::free_function, "GetSettlements", 2,
                  {script::Value::string(klass), script::Value::integer(player)});
    CHECK(out.status == script::HostStatus::ok);
    std::vector<ObjectId> ids;
    if (!is_objlist(out.value)) return ids;
    const ObjListPool& pool = objlist_pool_of(b.world);
    const std::span<const ObjectId> items = pool.items(objlist_of(out.value));
    ids.assign(items.begin(), items.end());
    return ids;
  };

  // The anchor's class is what the filter is applied to, and the anchor is what
  // comes back -- not the settlement handle.
  const std::vector<ObjectId> mine = list_of("Barracks", 1);
  REQUIRE(mine.size() == 1);
  CHECK(mine[0] == town->anchor);

  // **Player 0 is a wildcard**, read off the body rather than guessed: it
  // decrements the argument and passes a placeholder, so zero becomes "no
  // player filter" and not "player minus one".
  CHECK(list_of("Barracks", 0).size() == 1);
  // And the filter is a *tree* test, as everywhere else: the anchor's parent
  // class matches it too.
  CHECK(list_of("Building", 0).size() == 1);
  // Another player's number matches nothing.
  CHECK(list_of("Barracks", 2).empty());
  // A class the settlement's anchor does not descend from matches nothing, and
  // a class the graph cannot resolve does not silently widen to everything.
  CHECK(list_of("Tavern", 1).empty());
  CHECK(list_of("NoSuchClassAnywhere", 0).empty());
}

/// `set.IsCity()` is two class names, and `set.PopulationDied()` is a zero.
///
/// 0x005c2890 compares the central building's own class name against
/// `"GTownhall"` and `"RTownhall"` -- ten bytes each, so name equality and not
/// ancestry, and the settlement itself is never looked at. 0x005c2930 reads
/// `[settlement + 0xf0]`, which nothing in the image writes.
TEST(is_city_is_two_class_names_and_population_died_is_always_zero) {
  ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor" parent=""/>)",
      R"(<class id="BaseTownhall" cpp_class="CVXTownHall" parent="Object"/>)",
      R"(<class id="RTownhall" cpp_class="CVXTownHall" parent="BaseTownhall"/>)",
      R"(<class id="GTownhall" cpp_class="CVXTownHall" parent="BaseTownhall"/>)",
      R"(<class id="ITownhall" cpp_class="CVXTownHall" parent="BaseTownhall"/>)",
      // A subclass of a city's class, which is the case that separates name
      // equality from descent: the original compares the name and this is not
      // a city.
      R"(<class id="RTownhall2" cpp_class="CVXTownHall" parent="RTownhall"/>)",
  };
  const char* names[] = {"object.sc.xml",    "basetownhall.sc.xml", "rtownhall.sc.xml",
                         "gtownhall.sc.xml", "itownhall.sc.xml",    "rtownhall2.sc.xml"};
  for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    REQUIRE(graph.add(bytes_of(docs[i]), names[i]).ok());
  }
  graph.link();

  World world;
  EconomySystem economy;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  world.set_class_graph(&graph);
  REQUIRE(world.add_system(&economy));
  economy.start(world);
  HostContext context;
  context.world = &world;

  const auto place = [&](const char* class_name) -> SettlementId {
    SettlementInit init = town_hall();
    init.owner = 1;
    init.anchor = world.spawn(NativeClass::town_hall, nullptr, graph.find(class_name));
    return economy.create(world, init);
  };
  const auto ask = [&](const char* name, SettlementId id) {
    const std::uint32_t index = registry.find(script::CallKind::member, name, 0);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const Settlement* s = economy.settlements().find(id);
    CHECK(s != nullptr);
    std::vector<script::Value> args{
        script::Value::object(kTypeObj, s == nullptr ? kNoObject : s->object)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };

  const SettlementId roman = place("RTownhall");
  const SettlementId gaul = place("GTownhall");
  const SettlementId iberian = place("ITownhall");
  const SettlementId base = place("BaseTownhall");
  const SettlementId roman_sub = place("RTownhall2");

  CHECK(ask("IsCity", roman).value.truthy_scalar());
  CHECK(ask("IsCity", gaul).value.truthy_scalar());
  CHECK(!ask("IsCity", iberian).value.truthy_scalar());
  CHECK(!ask("IsCity", base).value.truthy_scalar());
  // Descent is not enough, and this is the assertion that says so.
  CHECK(!ask("IsCity", roman_sub).value.truthy_scalar());

  // Nothing about the settlement changes the answer -- it is a class question.
  CHECK(economy.find(roman) != nullptr);
  if (Settlement* s = economy.find(roman); s != nullptr) {
    s->kind = SettlementKind::village;
    s->population = 0;
    s->owner = 7;
  }
  CHECK(ask("IsCity", roman).value.truthy_scalar());

  // And the counter that has no writer.
  for (const SettlementId id : {roman, gaul, iberian, base}) {
    CHECK(ask("PopulationDied", id).value.as_integer() == 0);
  }
}

/// `set.SetFoodProduction(n)` writes the rate the *Food Tax* research buys.
///
/// `Settlement::food_rate` has carried the note that this entry point is what
/// makes it mutable since the production block was written; this is the entry
/// point arriving.
TEST(set_food_production_replaces_the_rate_and_starts_a_settlement_producing) {
  World world;
  EconomySystem economy;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  REQUIRE(world.add_system(&economy));
  economy.start(world);
  HostContext context;
  context.world = &world;

  SettlementInit init = town_hall();  // `produces_gold`, so `food_rate` is 0
  init.owner = 1;
  init.anchor = world.spawn(NativeClass::town_hall, nullptr);
  const SettlementId id = economy.create(world, init);
  Settlement* s = economy.find(id);
  REQUIRE(s != nullptr);
  CHECK(s->food_rate == 0);
  const std::int32_t gold_before = s->gold_rate;

  const auto set = [&](std::int32_t n) {
    const std::uint32_t index =
        registry.find(script::CallKind::member, "SetFoodProduction", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    std::vector<script::Value> args{script::Value::object(kTypeObj, s->object),
                                    script::Value::integer(n)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "SetFoodProduction";
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };

  CHECK(set(24).status == script::HostStatus::ok);
  CHECK(s->food_rate == 24);
  // The *food* rate and not the gold one: the two live one field apart and the
  // research names only one of them.
  CHECK(s->gold_rate == gold_before);
  // It replaces rather than adds, which is what "production" means here.
  CHECK(set(9).status == script::HostStatus::ok);
  CHECK(s->food_rate == 9);
}

/// The mule's load lives on the **object**, not in the shipment table.
///
/// `Wagon::amount` and `Wagon::restype` are plain reads of `[obj+0x1cc]` and
/// `[obj+0x1d4]`; `LoadGold`/`LoadFood` are the only script writers, and the
/// four steps they take are in the note at their registrations.
TEST(a_mule_loads_from_the_settlement_it_stands_in_and_carries_one_kind) {
  ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor" parent=""/>)",
      R"(<class id="Wagon" cpp_class="CVXWagon" parent="Object"><properties max_load="1000"/></class>)",
      // Inherits the capacity, which is how the shipped tree does it.
      R"(<class id="Camel" cpp_class="CVXWagon" parent="Wagon"/>)",
  };
  const char* names[] = {"object.sc.xml", "wagon.sc.xml", "camel.sc.xml"};
  for (std::size_t i = 0; i < 3; ++i) REQUIRE(graph.add(bytes_of(docs[i]), names[i]).ok());
  graph.link();

  World world;
  EconomySystem economy;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  world.set_class_graph(&graph);
  REQUIRE(world.add_system(&economy));
  economy.start(world);
  HostContext context;
  context.world = &world;

  SettlementInit init = town_hall();
  init.owner = 1;
  init.anchor = world.spawn(NativeClass::town_hall, nullptr);
  const SettlementId id = economy.create(world, init);
  Settlement* town = economy.find(id);
  REQUIRE(town != nullptr);
  town->warehouse.set(Resource::gold, 5000);
  town->warehouse.set(Resource::food, 300);

  const ObjectId mule = world.spawn(NativeClass::wagon, nullptr, graph.find("Camel"));
  const auto call = [&](const char* name, std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(
        script::CallKind::member, name, static_cast<std::uint16_t>(args.size() - 1));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };
  const script::Value self = script::Value::object(kTypeObj, mule);
  const auto amount = [&] { return call("amount", {self}).value.as_integer(); };
  const auto restype = [&] { return call("restype", {self}).value.as_integer(); };

  CHECK(amount() == 0);
  CHECK(restype() == 0);

  // 1. Standing nowhere, it loads nothing -- the settlement comes from the
  //    holder, not from ownership or from the nearest town.
  CHECK(call("LoadGold", {self, script::Value::integer(500)}).status ==
        script::HostStatus::ok);
  CHECK(amount() == 0);
  CHECK(town->warehouse.gold == 5000);

  REQUIRE(world.put_in_holder(mule, town->holder.object));
  CHECK(call("LoadGold", {self, script::Value::integer(500)}).status ==
        script::HostStatus::ok);
  CHECK(amount() == 500);
  CHECK(restype() == static_cast<std::int32_t>(Resource::gold));
  CHECK(town->warehouse.gold == 4500);

  // 2. A part-loaded gold mule refuses food, and the refusal changes nothing.
  CHECK(call("LoadFood", {self, script::Value::integer(100)}).status ==
        script::HostStatus::ok);
  CHECK(amount() == 500);
  CHECK(restype() == static_cast<std::int32_t>(Resource::gold));
  CHECK(town->warehouse.food == 300);

  // 3. Clamped to what is left of `max_load` -- 1000 here, inherited from the
  //    parent class -- and a negative request loads nothing rather than
  //    unloading.
  CHECK(call("LoadGold", {self, script::Value::integer(4000)}).status ==
        script::HostStatus::ok);
  CHECK(amount() == 1000);
  CHECK(town->warehouse.gold == 4000);
  CHECK(call("LoadGold", {self, script::Value::integer(-200)}).status ==
        script::HostStatus::ok);
  CHECK(amount() == 1000);
  CHECK(town->warehouse.gold == 4000);

  // 4. Only what the warehouse can pay. Emptied, a full-capacity request takes
  //    what is there and is not an error.
  world.find(mule)->state.cargo = 0;
  town->warehouse.set(Resource::gold, 120);
  CHECK(call("LoadGold", {self, script::Value::integer(1000)}).status ==
        script::HostStatus::ok);
  CHECK(amount() == 120);
  CHECK(town->warehouse.gold == 0);

  // The kind lands even when nothing does, which is the original's order and
  // is only visible on an **empty warehouse**: the field is written before the
  // warehouse is asked, so a mule that asked for food and got none still
  // reports food.
  world.find(mule)->state.cargo = 0;
  world.find(mule)->state.cargo_resource = static_cast<std::int32_t>(Resource::gold);
  town->warehouse.set(Resource::food, 0);
  CHECK(call("LoadFood", {self, script::Value::integer(50)}).status ==
        script::HostStatus::ok);
  CHECK(amount() == 0);
  CHECK(restype() == static_cast<std::int32_t>(Resource::food));

  town->warehouse.set(Resource::food, 300);
  CHECK(call("LoadFood", {self, script::Value::integer(50)}).status ==
        script::HostStatus::ok);
  CHECK(restype() == static_cast<std::int32_t>(Resource::food));
  CHECK(amount() == 50);
  CHECK(town->warehouse.food == 250);

  // ...and again on a class that declares **no** `max_load` at all, where the
  // clamp leaves nothing to load and the kind is the only thing that moves.
  // That is the one arrangement in which the order of those two steps is
  // visible, so it is the one this asserts.
  const ObjectId cartless = world.spawn(NativeClass::wagon, nullptr, graph.find("Object"));
  REQUIRE(world.put_in_holder(cartless, town->holder.object));
  world.find(cartless)->state.cargo_resource = static_cast<std::int32_t>(Resource::gold);
  const script::Value other = script::Value::object(kTypeObj, cartless);
  CHECK(call("LoadFood", {other, script::Value::integer(50)}).status ==
        script::HostStatus::ok);
  CHECK(call("amount", {other}).value.as_integer() == 0);
  CHECK(call("restype", {other}).value.as_integer() ==
        static_cast<std::int32_t>(Resource::food));

  // A receiver naming no object reads zero and loads nothing.
  const script::Value nobody = script::Value::object(kTypeObj, 9999);
  CHECK(call("amount", {nobody}).value.as_integer() == 0);
  CHECK(call("restype", {nobody}).value.as_integer() == 0);
  CHECK(call("LoadGold", {nobody, script::Value::integer(10)}).status ==
        script::HostStatus::ok);
}

/// The cargo is state: two peers that disagree about a mule's load sell
/// different amounts, so it moves the hash and survives a save.
TEST(the_mules_cargo_is_hashed_and_round_trips) {
  World world;
  const ObjectId mule = world.spawn(NativeClass::wagon, nullptr);
  const std::uint64_t empty = world.state_hash();
  world.find(mule)->state.cargo = 700;
  const std::uint64_t loaded = world.state_hash();
  CHECK(loaded != empty);
  world.find(mule)->state.cargo_resource = 1;
  CHECK(world.state_hash() != loaded);

  std::vector<std::byte> bytes;
  world.serialize(bytes);
  World back;
  REQUIRE(back.deserialize(bytes).ok());
  REQUIRE(back.find(mule) != nullptr);
  CHECK(back.find(mule)->state.cargo == 700);
  CHECK(back.find(mule)->state.cargo_resource == 1);
  CHECK(back.state_hash() == world.state_hash());
}


/// `Settlement::EvalSentries`, which answers a **strength** and not a count,
/// and which computes the class of the sentries it is evaluating rather than
/// reading it anywhere.
///
/// `GS_SIEGE.VS` adds it to `UnitsInHolderEval()` and weighs the sum against
/// the enemy strength the census reports, which is the whole reason it has to
/// be a strength.
TEST(eval_sentries_multiplies_the_roster_by_a_computed_sentry_class) {
  ClassGraph graph;
  const auto add = [&](std::string_view xml, const char* source) {
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(xml.data()), xml.size());
    CHECK(graph.add(bytes, source).ok());
  };
  add(R"(<class id="Building" cpp_class="CVXBuilding" parent=""/>)", "b.sc.xml");
  // A Roman town hall, which is what makes the sentry class `RSentry`: the
  // letter comes from the central building's `race`, and nothing else.
  add(R"(<class id="RTownhall" cpp_class="CVXTownHall" parent="Building">
      <properties race="RepublicanRome"/>
    </class>)",
      "rth.sc.xml");
  add(R"(<class id="RSentry" cpp_class="CVXUnit" parent="">
      <properties maxhealth="100" damage="20" armor_slash="10"/>
    </class>)",
      "rs.sc.xml");
  // A second nation's, so that "it picked the right letter" is a claim a test
  // can fail rather than the only class in the graph.
  add(R"(<class id="TTownhall" cpp_class="CVXTownHall" parent="Building">
      <properties race="German"/>
    </class>)",
      "tth.sc.xml");
  add(R"(<class id="TSentry" cpp_class="CVXUnit" parent="">
      <properties maxhealth="200" damage="20" armor_slash="10"/>
    </class>)",
      "ts.sc.xml");
  // **The raceless base the eight descend from**, which is in the graph so
  // that "no race means no sentry class" is a claim a test can fail: without
  // it, a building with no race would look up `"" + "Sentry"` and find nothing
  // for the wrong reason.
  add(R"(<class id="Sentry" cpp_class="CVXUnit" parent="">
      <properties maxhealth="900" damage="900" armor_slash="900"/>
    </class>)",
      "sent.sc.xml");
  graph.link();

  World world;
  EconomySystem economy;
  EnvSystem env;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  world.set_class_graph(&graph);
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&env));
  economy.start(world);
  HostContext context;
  context.world = &world;

  const auto plant = [&](const char* anchor_class) {
    SettlementInit init = town_hall();
    init.owner = 1;
    init.anchor = world.spawn(NativeClass::town_hall, nullptr, graph.find(anchor_class));
    return economy.create(world, init);
  };
  const auto ask = [&](SettlementId id) {
    Settlement* s = economy.find(id);
    CHECK(s != nullptr);
    if (s == nullptr) return 0;
    const std::uint32_t index =
        registry.find(script::CallKind::member, "EvalSentries", 0);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return 0;
    std::vector<script::Value> args{script::Value::object(kTypeObj, s->object)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "EvalSentries";
    ctx.kind = script::CallKind::member;
    return static_cast<int>(registry.entry(index).fn(ctx).value.as_integer());
  };

  const SettlementId roman = plant("RTownhall");
  Settlement* town = economy.find(roman);
  REQUIRE(town != nullptr);

  // An empty roster is nothing, before any class is looked at.
  CHECK(town->sentries == 0);
  CHECK(ask(roman) == 0);

  // `RSentry`: quarter = (100*5)/4 = 125, power = 20+10 = 30, and the level is
  // unset so the factor is 13. per = (30*13*125)/1000 + 1 = 48 + 1 = 49.
  town->sentries = 4;
  CHECK(ask(roman) == 49 * 4);

  // **The roster, not the wall.** `sentries_ready` is how many of the roster
  // are actually standing in the wall, and the cached strength at `[+0xf8]`
  // uses that one; this entry point does not.
  town->sentries_ready = 1;
  CHECK(ask(roman) == 49 * 4);
  town->sentries_ready = 0;
  CHECK(ask(roman) == 49 * 4);

  // **The level is an environment integer in the settlement's own scope**, and
  // it moves the factor. 13 + 2 = 15: (30*15*125)/1000 + 1 = 56 + 1 = 57.
  env.env().write_int(EnvScope::for_settlement(roman), "SentriesLevel", 2);
  CHECK(ask(roman) == 57 * 4);
  // It is per settlement, so a second one is unaffected.
  const SettlementId german = plant("TTownhall");
  Settlement* other = economy.find(german);
  REQUIRE(other != nullptr);
  other->sentries = 1;
  // `TSentry`: quarter = (200*5)/4 = 250, power = 30, factor 13.
  // per = (30*13*250)/1000 + 1 = 97 + 1 = 98.
  CHECK(ask(german) == 98);
  env.env().write_int(EnvScope::for_settlement(roman), "SentriesLevel", 0);
  town = economy.find(roman);
  REQUIRE(town != nullptr);

  // **The letter is the central building's race**, and `German` is the one
  // demonym in the retail install -- `TOutpost` writes it. A table that did not
  // know the spelling would give this settlement no race, no letter and no
  // sentry class, and it would answer zero.
  CHECK(ask(german) == 98);

  // A settlement whose race names no sentry class answers zero rather than
  // dereferencing what the registry did not find, which is what the original
  // does there.
  {
    add(R"(<class id="ETownhall" cpp_class="CVXTownHall" parent="Building">
        <properties race="Egypt"/>
      </class>)",
        "eth.sc.xml");
    graph.link();
    const SettlementId egyptian = plant("ETownhall");
    Settlement* nile = economy.find(egyptian);
    REQUIRE(nile != nullptr);
    nile->sentries = 5;
    CHECK(ask(egyptian) == 0);  // no `ESentry` in this graph
  }

  // And a settlement whose central building declares no race at all -- which
  // answers zero and **not** the raceless `Sentry` class, whose numbers are in
  // this graph precisely so that picking it up would show.
  {
    const SettlementId plain = plant("Building");
    Settlement* s = economy.find(plain);
    REQUIRE(s != nullptr);
    s->sentries = 5;
    CHECK(ask(plain) == 0);
  }

  // A roster below zero contributes nothing rather than subtracting: the
  // original's gate is a signed `jle`, and the multiply that follows it would
  // otherwise hand back a negative strength.
  //
  // **Re-found rather than reused**: `SettlementStore` keeps its settlements in
  // a vector, and the three planted since `town` was taken can have moved them.
  Settlement* again = economy.find(roman);
  REQUIRE(again != nullptr);
  again->sentries = -3;
  CHECK(ask(roman) == 0);
  again->sentries = 4;
  CHECK(ask(roman) == 49 * 4);
}

/// The wall pool is a fourth sentry number, and only two of the four are
/// readable from a script.
TEST(get_sentry_takes_one_out_of_the_wall_and_put_sentry_puts_one_back) {
  World world;
  EconomySystem economy;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  REQUIRE(world.add_system(&economy));
  economy.start(world);
  HostContext context;
  context.world = &world;

  SettlementInit init = town_hall();
  init.owner = 1;
  init.anchor = world.spawn(NativeClass::town_hall, nullptr);
  const SettlementId id = economy.create(world, init);
  Settlement* town = economy.find(id);
  REQUIRE(town != nullptr);

  const auto call = [&](const char* name) {
    const std::uint32_t index = registry.find(script::CallKind::member, name, 0);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    std::vector<script::Value> args{script::Value::object(kTypeObj, town->object)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };
  const auto took = [&] { return call("GetSentry").value.truthy_scalar(); };
  const auto roster = [&] { return call("GetNumSentries").value.as_integer(); };

  // An empty wall answers no and changes nothing.
  town->sentries = 0;
  town->sentries_ready = 0;
  CHECK(!took());
  CHECK(town->sentries_ready == 0);
  CHECK(roster() == 0);

  // `AddSentries` raises **both** the roster and the wall, so they start equal.
  REQUIRE(economy.add_sentries(id, 3));
  CHECK(town->sentries == 3);
  CHECK(town->sentries_ready == 3);
  CHECK(roster() == 3);

  // Taking one out lowers the wall and **not** the roster: the sentry still
  // belongs to the settlement, it is just standing outside it.
  CHECK(took());
  CHECK(town->sentries_ready == 2);
  CHECK(town->sentries == 3);
  CHECK(roster() == 3);
  CHECK(took());
  CHECK(took());
  CHECK(town->sentries_ready == 0);
  CHECK(town->sentries == 3);
  // ...and the fourth ask fails, because the wall is empty however full the
  // roster is.
  CHECK(!took());
  CHECK(town->sentries == 3);

  // Putting one back raises the wall, and it asks about neither the roster nor
  // the ceiling -- a wall can hold more than it started with.
  CHECK(call("PutSentry").status == script::HostStatus::ok);
  CHECK(town->sentries_ready == 1);
  CHECK(town->sentries == 3);
  town->sentries_ready = 99;
  CHECK(call("PutSentry").status == script::HostStatus::ok);
  CHECK(town->sentries_ready == 100);
  CHECK(town->max_sentries < 100);

  // `DelSentry` runs and changes nothing a script can read.
  const std::int32_t before_roster = town->sentries;
  const std::int32_t before_wall = town->sentries_ready;
  for (int i = 0; i < 5; ++i) {
    CHECK(call("DelSentry").status == script::HostStatus::ok);
  }
  CHECK(town->sentries == before_roster);
  CHECK(town->sentries_ready == before_wall);

  // The wall pool is state: it decides whether a gate can post a guard.
  const std::uint64_t hashed = economy.settlements().hash();
  town->sentries_ready += 1;
  CHECK(economy.settlements().hash() != hashed);
}

/// `set.GoldSpent(n)` adds to the **settlement owner's** cumulative gold spent,
/// which is the counter `GetTeamOverallScore` has been waiting for.
TEST(gold_spent_credits_the_settlements_own_owner) {
  World world;
  EconomySystem economy;
  MatchSystem match;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&match));
  economy.start(world);
  HostContext context;
  context.world = &world;

  // **Ids, not pointers.** The settlement store is a vector and `create` moves
  // every row it holds, so a `Settlement*` taken before the second `create`
  // dangles -- which is the same reason `ObjListPool`'s alias resolves through
  // a hook rather than caching one.
  const auto place = [&](PlayerId owner) {
    SettlementInit init = town_hall();
    init.owner = owner;
    init.anchor = world.spawn(NativeClass::town_hall, nullptr);
    return economy.create(world, init);
  };
  const SettlementId mine = place(1);
  const SettlementId theirs = place(4);
  REQUIRE(economy.find(mine) != nullptr);
  REQUIRE(economy.find(theirs) != nullptr);

  const auto spend = [&](SettlementId id, std::int32_t n) {
    const std::uint32_t index = registry.find(script::CallKind::member, "GoldSpent", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const Settlement* s = economy.find(id);
    CHECK(s != nullptr);
    std::vector<script::Value> args{
        script::Value::object(kTypeObj, s == nullptr ? kNoObject : s->object),
        script::Value::integer(n)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "GoldSpent";
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  };

  CHECK(match.score(1).gold == 0);
  CHECK(spend(mine, 250).status == script::HostStatus::ok);
  CHECK(match.score(1).gold == 250);
  // It **adds**, and it credits the settlement's owner and nobody else.
  CHECK(spend(mine, 100).status == script::HostStatus::ok);
  CHECK(match.score(1).gold == 350);
  CHECK(match.score(4).gold == 0);
  CHECK(spend(theirs, 7).status == script::HostStatus::ok);
  CHECK(match.score(4).gold == 7);
  CHECK(match.score(1).gold == 350);

  // No sign check, in either engine: a script that says it spent -50 gets -50.
  CHECK(spend(mine, -50).status == script::HostStatus::ok);
  CHECK(match.score(1).gold == 300);

  // It follows the owner rather than a remembered player: a captured
  // settlement credits whoever holds it now.
  economy.find(mine)->owner = 4;
  CHECK(spend(mine, 5).status == script::HostStatus::ok);
  CHECK(match.score(4).gold == 12);
  CHECK(match.score(1).gold == 300);
}

/// `set.CreateBoatFood(n)` / `CreateBoatGold(n)` -- two clamps, a spawn, and
/// nothing at all when the warehouse is empty.
TEST(create_boat_loads_from_the_warehouse_and_parks_the_boat_in_the_holder) {
  ClassGraph graph;
  const std::string docs[] = {
      R"(<class id="Object" cpp_class="CVXDecor" parent=""/>)",
      R"(<class id="Wagon" cpp_class="CVXWagon" parent="Object"><properties max_load="1000"/></class>)",
      // `ShipS` is the class both entry points always spawn, and it declares
      // its own `max_load` in the shipped tree.
      R"(<class id="ShipS" cpp_class="CVXWagon" parent="Wagon"><properties max_load="1000"/></class>)",
  };
  const char* names[] = {"object.sc.xml", "wagon.sc.xml", "ships.sc.xml"};
  for (std::size_t i = 0; i < 3; ++i) REQUIRE(graph.add(bytes_of(docs[i]), names[i]).ok());
  graph.link();

  World world;
  EconomySystem economy;
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  world.set_class_graph(&graph);
  REQUIRE(world.add_system(&economy));
  economy.start(world);
  HostContext context;
  context.world = &world;

  SettlementInit init = town_hall();
  init.owner = 3;
  init.anchor = world.spawn(NativeClass::town_hall, nullptr);
  const SettlementId id = economy.create(world, init);
  REQUIRE(economy.find(id) != nullptr);
  economy.find(id)->warehouse.set(Resource::gold, 4000);
  economy.find(id)->warehouse.set(Resource::food, 120);

  const auto make = [&](const char* name, std::int32_t n) {
    const std::uint32_t index = registry.find(script::CallKind::member, name, 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return sim::kNoObject;
    std::vector<script::Value> args{
        script::Value::object(kTypeObj, economy.find(id)->object),
        script::Value::integer(n)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() ? static_cast<ObjectId>(out.value.as_object().id)
                                 : sim::kNoObject;
  };

  // A full load: clamped by `max_load` and paid for out of the warehouse.
  const ObjectId big = make("CreateBoatGold", 1000);
  REQUIRE(big != sim::kNoObject);
  CHECK(world.find(big)->state.cargo == 1000);
  CHECK(world.find(big)->state.cargo_resource == static_cast<std::int32_t>(Resource::gold));
  CHECK(economy.find(id)->warehouse.gold == 3000);
  // The settlement's own player, and inside the settlement's holder.
  CHECK(world.find(big)->state.owner == 3);
  CHECK(world.find(big)->state.holder == economy.find(id)->holder.object);
  CHECK(world.find(big)->class_index == graph.find("ShipS"));

  // Asking for more than the class can carry loads the class maximum.
  const ObjectId capped = make("CreateBoatGold", 5000);
  REQUIRE(capped != sim::kNoObject);
  CHECK(world.find(capped)->state.cargo == 1000);
  CHECK(economy.find(id)->warehouse.gold == 2000);

  // Asking for more than the warehouse holds loads what is there.
  const ObjectId short_load = make("CreateBoatFood", 1000);
  REQUIRE(short_load != sim::kNoObject);
  CHECK(world.find(short_load)->state.cargo == 120);
  CHECK(world.find(short_load)->state.cargo_resource ==
        static_cast<std::int32_t>(Resource::food));
  CHECK(economy.find(id)->warehouse.food == 0);

  // **Nothing loaded, no boat**, which is the test the shipped script makes
  // before it does anything else -- and the warehouse is not touched.
  CHECK(make("CreateBoatFood", 500) == sim::kNoObject);
  CHECK(economy.find(id)->warehouse.food == 0);
  CHECK(make("CreateBoatGold", 0) == sim::kNoObject);
  CHECK(economy.find(id)->warehouse.gold == 2000);
  CHECK(make("CreateBoatGold", -100) == sim::kNoObject);
  CHECK(economy.find(id)->warehouse.gold == 2000);

  // A receiver that is not a settlement answers the invalid handle.
  const std::uint32_t index = registry.find(script::CallKind::member, "CreateBoatGold", 1);
  REQUIRE(index != script::kUnresolvedHost);
  std::vector<script::Value> args{script::Value::object(kTypeObj, 9999),
                                  script::Value::integer(10)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "CreateBoatGold";
  ctx.kind = script::CallKind::member;
  const script::HostOutcome missed = registry.entry(index).fn(ctx);
  CHECK(missed.status == script::HostStatus::ok);
  CHECK(!missed.value.as_object().valid());
}

// --------------------------------------------------------------------------
// the gates
// --------------------------------------------------------------------------

/// `NumGates`, `BestGate`, `OpenAllGates` and `IdleAllGates`, on a town with
/// two gates and a barrack that is not one.
TEST(economy_gates_are_counted_ranked_opened_and_idled) {
  BarrackBench b;
  const ObjectId near = b.add(b.classes.plain, 0, NativeClass::gate);
  const ObjectId far = b.add(b.classes.plain, 0, NativeClass::gate);
  const ObjectId barrack = b.add(b.classes.plain, 0, NativeClass::barrack);
  b.world.set_position(near, Point{1100, 1000});
  b.world.set_position(far, Point{1590, 1000});
  b.world.set_health(near, 3000);
  b.world.set_health(far, 3000);
  (void)barrack;

  const auto number = [&](const script::HostOutcome& out) {
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  const auto handle = [&](const script::HostOutcome& out) {
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() && out.value.as_object().type != script::kNoType
               ? out.value.as_object().id
               : kNoObject;
  };

  CHECK(number(b.call("NumGates", 0, {b.settlement_value()})) == 2);

  // The score is distance plus a fifth of the health. From the west, both
  // sound: 100 + 600 against 590 + 600, the near gate. Breach the far gate to
  // nothing: 700 against 590, and the far one is now the better target.
  const script::Value west = pack_point(Point{1000, 1000});
  CHECK(handle(b.call("BestGate", 1, {b.settlement_value(), west})) == near);
  b.world.set_health(far, 0);
  CHECK(handle(b.call("BestGate", 1, {b.settlement_value(), west})) == far);
  // A fifth, not a quarter: the near gate at 2400 scores 100 + 480 = 580,
  // ten under the far gate's 590; a quarter would put it at 700.
  b.world.set_health(near, 2400);
  CHECK(handle(b.call("BestGate", 1, {b.settlement_value(), west})) == near);
  // Standing at the far gate: 0 against 490 + 480.
  CHECK(handle(b.call("BestGate", 1, {b.settlement_value(), pack_point(Point{1590, 1000})})) ==
        far);
  // A tie keeps the earlier: both sound, the point half way.
  b.world.set_health(near, 3000);
  b.world.set_health(far, 3000);
  CHECK(handle(b.call("BestGate", 1, {b.settlement_value(), pack_point(Point{1345, 1000})})) ==
        near);
  // Not a point: nothing.
  CHECK(handle(b.call("BestGate", 1, {b.settlement_value(), script::Value::integer(3)})) ==
        kNoObject);

  // OpenAllGates orders every gate that is not open, and clears what it was
  // doing first; the barrack is not a gate and keeps its queue.
  (void)b.commands.add_command(b.world, near, false, "idle", Command{});
  (void)b.commands.add_command(b.world, far, false, "idle", Command{});
  (void)b.commands.add_command(b.world, barrack, false, "idle", Command{});
  b.world.mutable_state(far)->flags.gate_open = true;
  CHECK(b.call("OpenAllGates", 0, {b.settlement_value()}).status == script::HostStatus::ok);
  CHECK(b.commands.command_count(near) == 1);
  CHECK(b.commands.command_name(near, 0) == "opengate");
  CHECK(b.commands.command_name(far, 0) == "idle");
  CHECK(b.commands.command_name(barrack, 0) == "idle");

  // IdleAllGates ends what every gate that is not idling was doing -- the
  // runner too, so `opengate` is gone -- and leaves one that is.
  (void)b.commands.add_command(b.world, near, false, "idle", Command{});
  CHECK(b.commands.command_count(near) == 2);
  CHECK(b.call("IdleAllGates", 0, {b.settlement_value()}).status == script::HostStatus::ok);
  CHECK(b.commands.command_count(near, "opengate") == 0);
  CHECK(b.commands.command_count(near) <= 1);
  CHECK(b.commands.command_count(far) == 1);
  CHECK(b.commands.command_name(far, 0) == "idle");
  CHECK(b.commands.command_count(barrack) == 1);

  // No settlement: the counted answer is zero and the rest are quiet.
  CHECK(number(b.call("NumGates", 0, {script::Value::object(kTypeSettlement, 9999)})) == 0);
  CHECK(b.call("OpenAllGates", 0, {script::Value::object(kTypeSettlement, 9999)}).status ==
        script::HostStatus::ok);
}

// --------------------------------------------------------------------------
// damage taken, and the settlement census
// --------------------------------------------------------------------------

/// An entity with one enter/exit door, so `MostDamagedBuilding` has something
/// it will consider.
constexpr std::string_view kDoorEntity =
    "<entity name=\"hall\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points><point idx=\"0\" type=\"12\" x=\"64\" y=\"0\"/></points></entity>";

/// `MostDamagedBuilding` ranks by the counter among buildings with a door;
/// `ClearDamageTaken` zeroes one building or a whole settlement.
TEST(economy_most_damaged_building_ranks_by_damage_taken_among_the_enterable) {
  BarrackBench b;
  const Result<Entity> door = Entity::parse(bytes_of(kDoorEntity));
  REQUIRE(door.ok());
  const auto building = [&](const Entity* entity) {
    const ObjectId id = b.world.spawn(NativeClass::building, entity, b.classes.plain);
    b.world.set_position(id, Point{1100, 1000});
    b.world.set_health(id, 3000);
    CHECK(b.economy.add_building(b.world, b.town, id, 3000));
    return id;
  };
  const ObjectId hall = building(&door.value());
  const ObjectId keep = building(&door.value());
  const ObjectId shed = building(nullptr);
  b.world.mutable_state(hall)->damage_taken = 40;
  b.world.mutable_state(keep)->damage_taken = 90;
  b.world.mutable_state(shed)->damage_taken = 500;

  const auto handle = [&](const script::HostOutcome& out) {
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() && out.value.as_object().type != script::kNoType
               ? out.value.as_object().id
               : kNoObject;
  };
  // The shed has taken the most and has no door: the keep wins.
  CHECK(handle(b.call("MostDamagedBuilding", 0, {b.settlement_value()})) == keep);
  // Strictly more: two at 90 keep the earlier.
  b.world.mutable_state(hall)->damage_taken = 90;
  CHECK(handle(b.call("MostDamagedBuilding", 0, {b.settlement_value()})) == hall);

  // One building cleared, by its own handle.
  CHECK(b.call("ClearDamageTaken", 0, {script::Value::object(kTypeObj, hall)}).status ==
        script::HostStatus::ok);
  CHECK(b.world.state(hall)->damage_taken == 0);
  CHECK(b.world.state(keep)->damage_taken == 90);
  CHECK(handle(b.call("MostDamagedBuilding", 0, {b.settlement_value()})) == keep);
  // The whole settlement cleared: nothing is damaged, so nothing is answered.
  CHECK(b.call("ClearDamageTaken", 0, {b.settlement_value()}).status == script::HostStatus::ok);
  CHECK(b.world.state(keep)->damage_taken == 0);
  CHECK(b.world.state(shed)->damage_taken == 0);
  CHECK(handle(b.call("MostDamagedBuilding", 0, {b.settlement_value()})) == kNoObject);
  CHECK(handle(b.call("MostDamagedBuilding", 0, {script::Value::object(kTypeSettlement, 9999)})) ==
        kNoObject);
}

/// `SettlementCount(around, dist, class)`: strictly within, measured to the
/// central building, and an is-a test on that building's class.
TEST(economy_settlement_count_is_strictly_within_and_by_descent) {
  BarrackBench b;
  const auto plant = [&](Point at, ClassIndex cls) {
    const ObjectId anchor = b.world.spawn(NativeClass::building, nullptr, cls);
    b.world.set_position(anchor, at);
    SettlementInit init;
    init.kind = SettlementKind::village;
    init.anchor = anchor;
    init.owner = 3;
    init.max_units = 10;
    (void)b.economy.create(b.world, init);
  };
  // The bench's own town is at (1000, 1000) on the plain `Building` class.
  plant(Point{1500, 1000}, b.classes.rbarracks);   // 500 away, a barrack
  plant(Point{1000, 1700}, b.classes.plain);       // 700 away
  plant(Point{3000, 1000}, b.classes.gbarracks);   // 2000 away

  const auto count = [&](Point around, std::int32_t dist, const char* cls) {
    const script::HostOutcome out =
        call_host(b.registry, b.world, script::CallKind::free_function, "SettlementCount", 3,
                  {pack_point(around), script::Value::integer(dist), script::Value::string(cls)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  CHECK(count(Point{1000, 1000}, 800, "Building") == 3);      // the town, the barrack, the village
  CHECK(count(Point{1000, 1000}, 800, "BaseBarracks") == 1);  // descent, not name
  CHECK(count(Point{1000, 1000}, 2001, "BaseBarracks") == 2);
  CHECK(count(Point{1000, 1000}, 2000, "BaseBarracks") == 1);  // strictly within
  CHECK(count(Point{1000, 1000}, 800, "Nonesuch") == 0);
  CHECK(count(Point{1000, 1000}, 1, "Building") == 1);        // the town itself, at 0
}

/// `set.CreateShip(class)`: a ship of the class, owned by the settlement,
/// berthed at the central building's first water door and garrisoned in the
/// settlement's holder; the invalid handle for a receiver naming nothing, a
/// class nothing declares, or a class that is not a ship.
TEST(create_ship_spawns_the_class_into_the_settlements_holder) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_economy_hosts(registry);

  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)"),
            "object.sc.xml");
  graph.add(bytes_of(R"(<class id="Shipyard" parent="Object" cpp_class="CVXBuilding"><properties sight="200" maxhealth="900"/></class>)"),
            "shipyard.sc.xml");
  graph.add(bytes_of(R"(<class id="Trireme" parent="Object" cpp_class="CVXShip"><properties sight="200" maxhealth="300"/></class>)"),
            "trireme.sc.xml");
  graph.add(bytes_of(R"(<class id="Peasant" parent="Object" cpp_class="CVXUnit"><properties sight="100" maxhealth="50"/></class>)"),
            "peasant.sc.xml");
  graph.link();

  World world;
  world.set_class_graph(&graph);
  EconomySystem economy;
  REQUIRE(world.add_system(&economy));
  const World::SettlementIds ids = world.spawn_settlement(2);
  const ObjectId yard = world.spawn(NativeClass::building, nullptr, graph.find("Shipyard"));
  world.set_position(yard, Point{3000, 3000});
  world.set_owner(yard, 2);
  SettlementInit init = town_hall();
  init.settlement_object = ids.settlement;
  init.holder_object = ids.holder;
  init.warehouse_object = ids.warehouse;
  init.anchor = yard;
  init.owner = 2;
  const SettlementId id = economy.create(world, init);
  REQUIRE(id != kNoSettlement);
  economy.start(world);

  const auto create = [&](const char* cls) -> ObjectId {
    const script::HostOutcome out =
        call_host(registry, world, script::CallKind::member, "CreateShip", 1,
                  {script::Value::object(kTypeSettlement, ids.settlement), script::Value::string(cls)});
    CHECK(out.status == script::HostStatus::ok);
    CHECK(out.value.is_object());
    if (!out.value.is_object()) return kNoObject;
    return out.value.as_object().type == kTypeObj ? out.value.as_object().id : kNoObject;
  };
  const ObjectId ship = create("Trireme");
  REQUIRE(ship != kNoObject);
  const WorldObject* slot = world.find(ship);
  REQUIRE(slot != nullptr);
  CHECK(slot->class_index == graph.find("Trireme"));
  CHECK(slot->state.owner == 2);
  // Born inside: on the settlement's roster and in its holder, at the held
  // sentinel, as `AddUnit` leaves a unit (`garrison_enter`).
  CHECK(slot->state.position == kHeldPosition);
  CHECK(slot->state.holder == ids.holder);
  const Settlement* set = economy.settlements().find(id);
  REQUIRE(set != nullptr);
  CHECK(set->holder.contains(ship));

  CHECK(create("Peasant") == kNoObject);
  CHECK(create("NoSuchClass") == kNoObject);
  const script::HostOutcome bad =
      call_host(registry, world, script::CallKind::member, "CreateShip", 1,
                {script::Value::object(kTypeSettlement, 999999), script::Value::string("Trireme")});
  CHECK(bad.status == script::HostStatus::ok);
  CHECK(bad.value.is_object() && bad.value.as_object().type == script::kNoType);
}
