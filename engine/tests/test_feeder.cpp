// Unit feeding tests.
//
// No game data. Every number a fixture is built around is measured, and the
// comment beside it says from what: the `;Feeding Constants` and `;Unit
// Feeding` blocks of `DATA\CONST.INI` for the intervals, `DATA\CLASSES\*.SC.XML`
// for `max_food` and `feeds`, the shipped `DATA\SUBAI\*.vs` behaviours for the
// refill rules, and `docs/engine/state-vector.md` for the per-unit `food` field
// the dumps record.
//
// The test that matters most is `feeder_agrees_under_any_turn_partition`. Turn
// length is renegotiated mid-session -- 200, 400, 799 and 800 all occur -- so an
// interval cannot be a turn count, and a round-robin cursor over every unit is
// world state that must survive being chopped up.

#include <cstdint>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

/// `DATA\CLASSES\UNIT.SC.XML`: `max_food="20"`, `feeds="1"`, `maxhealth="200"`.
FeedingUnit unit(ObjectId id) {
  FeedingUnit link;
  link.unit = id;
  link.food = 20;
  link.max_food = 20;
  link.max_health = 200;
  link.feeds = true;
  return link;
}

/// `BaseTownhall`: `produces_gold=1`, **`produces_food=0`**, `population=40`,
/// `settlement_food=200`, `settlement_maxfood=100000000`, `max_units=10000`.
/// The whole point of this file is that the second of those makes a stronghold
/// store a sink.
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

/// Run `total` game-time units as turns of `length`, plus whatever is left.
/// The world drives its systems, so nothing here advances one by hand.
void run(World& world, std::int32_t length, std::int64_t total) {
  std::int64_t elapsed = 0;
  while (elapsed < total) {
    const std::int32_t step =
        static_cast<std::int32_t>(length < total - elapsed ? length : total - elapsed);
    world.advance(step);
    elapsed += step;
  }
}

std::uint64_t feeder_hash(const FeederSystem& feeder) {
  std::uint64_t h = 1469598103934665603ull;
  feeder.hash(h);
  return h;
}

script::HostOutcome call_host(script::HostRegistry& registry, World& world,
                              script::CallKind kind, const char* name, std::uint16_t arity,
                              std::vector<script::Value> arguments, bool with_user = true) {
  const std::uint32_t index = registry.find(kind, name, arity);
  if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
  const script::HostEntry& entry = registry.entry(index);
  if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
  script::CallContext ctx;
  sim::HostContext state;
  state.world = &world;
  ctx.arguments = arguments;
  ctx.user = with_user ? &state : nullptr;
  ctx.name = name;
  ctx.kind = kind;
  return entry.fn(ctx);
}

}  // namespace

// --------------------------------------------------------------------------
// the constants
// --------------------------------------------------------------------------

/// Every default is a line of `DATA\CONST.INI`, read whole. This project has
/// already been bitten twice by a value truncated mid-number --
/// `ProductionInterval` was recorded as 20 when the file says 2000 -- so the
/// digits are asserted rather than assumed.
TEST(feeder_constants_are_the_shipped_ones) {
  const FeederRules rules;
  CHECK(rules.feed_quant_interval == 100);          // FeedinQuantIvl = 100
  CHECK(rules.drop_food_by_one_interval == 10000);  // DropFoodByOneIvl = 10000
  CHECK(rules.health_drop_interval == 5000);        // HealthDropIvl = 5000
  CHECK(rules.health_decrease_step == 5);           // HealthDecreaseStep = 5
  CHECK(rules.health_drop_bound_percent == 10);     // HealthDropBoundPercent = 10
  CHECK(rules.food_search_freq == 1000);            // FoodSearchFreq = 1000
  CHECK(rules.food_search_freq_var == 1000);        // FoodSearchFreqVar = 1000
  CHECK(rules.fast_metabolism_speed == 5);          // FastMetabolismSpeed = 5
  CHECK(rules.default_max_food == 20);              // UNIT.SC.XML max_food="20"
  CHECK(rules.default_feeds);                       // UNIT.SC.XML feeds="1"

  // `feeder_constant` is the hook `sim/env.cpp` needs so that `set_rules` stays
  // authoritative for the feeder's keys, exactly as `economy_constant` is for
  // the economy's.
  std::int32_t value = 0;
  REQUIRE(feeder_constant(rules, "DropFoodByOneIvl", value));
  CHECK(value == 10000);
  std::int32_t not_ours = -1;
  CHECK(!feeder_constant(rules, "ProductionInterval", not_ours));
  CHECK(not_ours == -1);  // untouched on a miss

  FeederRules retuned = rules;
  retuned.drop_food_by_one_interval = 1234;
  REQUIRE(feeder_constant(retuned, "DropFoodByOneIvl", value));
  CHECK(value == 1234);
}

// --------------------------------------------------------------------------
// the food cursor
// --------------------------------------------------------------------------

/// One food per feeding unit per `DropFoodByOneIvl`, whatever the chain length.
/// That is the whole economic claim: `n` feeding units cost `n` food per ten
/// seconds, which a village's `12 * 24 / 100 = 2` per 2,000 ms sustains for
/// exactly ten of them.
TEST(feeder_drains_one_food_per_unit_per_interval) {
  for (const std::size_t population : {std::size_t{1}, std::size_t{7}, std::size_t{40}}) {
    World world;
    FeederSystem feeder;
    feeder.set_world_bound(false);  // ids of the test's own, not the world's
    REQUIRE(world.add_system(&feeder));
    for (std::size_t i = 0; i < population; ++i) {
      feeder.enrol(unit(static_cast<ObjectId>(100 + i)));
    }
    feeder.start(world);

    run(world, 800, 10000);  // one `DropFoodByOneIvl`
    for (std::size_t i = 0; i < population; ++i) {
      CHECK(feeder.food(static_cast<ObjectId>(100 + i)) == 19);
    }

    run(world, 800, 30000);  // three more
    for (std::size_t i = 0; i < population; ++i) {
      CHECK(feeder.food(static_cast<ObjectId>(100 + i)) == 16);
    }

    // Twenty intervals empties a `max_food="20"` store exactly, and it stops
    // there rather than going negative.
    run(world, 800, 200000);
    for (std::size_t i = 0; i < population; ++i) {
      CHECK(feeder.food(static_cast<ObjectId>(100 + i)) == 0);
    }
  }
}

/// `feeds="0"` is shipped by `SENTRY`, `RAMUNIT`, `WAGON`, `BASEANIMAL`,
/// `EAGLE`, `CROW`, `HEN2`, `IMOUNTAINEER`, `SHAMANGHOST` and `GGHOST`, and
/// `Unit::SetFeeding(false)` writes the same flag at runtime from five shipped
/// behaviours. Such a unit stays in the chain -- `TTENT_BEHAVIOR.VS` switches
/// its Teutons back on -- and is skipped by both cursors.
TEST(feeder_skips_a_unit_that_does_not_feed) {
  World world;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  REQUIRE(world.add_system(&feeder));

  feeder.enrol(unit(1));
  FeedingUnit sentry = unit(2);
  sentry.feeds = false;  // SENTRY.SC.XML: feeds="0", max_food="0"
  feeder.enrol(sentry);
  feeder.start(world);

  run(world, 400, 50000);
  CHECK(feeder.food(1) == 15);
  CHECK(feeder.food(2) == 20);  // untouched
  CHECK(feeder.total_unit_count() == 2);  // still in the chain

  // `SetFeeding(true)`, as `TTENT_BEHAVIOR.VS` does for its commanded Teutons.
  REQUIRE(feeder.set_feeding(2, true));
  run(world, 400, 50000);
  CHECK(feeder.food(1) == 10);
  CHECK(feeder.food(2) == 15);
}

// --------------------------------------------------------------------------
// the health cursor
// --------------------------------------------------------------------------

/// A starving unit loses `HealthDecreaseStep` every `HealthDropIvl`, and stops
/// at `HealthDropBoundPercent` of its maximum. Starvation is a debuff, not an
/// attrition kill -- which is what a bound percent in the feeder's own constant
/// cluster has to mean.
TEST(feeder_starves_a_unit_down_to_the_bound_and_no_further) {
  World world;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  REQUIRE(world.add_system(&feeder));

  const ObjectId id = world.spawn(NativeClass::unit, nullptr);
  world.set_health(id, 200);  // UNIT.SC.XML maxhealth="200"
  FeedingUnit link = unit(id);
  link.food = 0;  // already empty
  feeder.enrol(link);
  feeder.start(world);

  // One `HealthDropIvl`: one visit of the health cursor, one step of 5.
  run(world, 500, 5000);
  CHECK(world.state(id)->health == 195);

  run(world, 500, 5000);
  CHECK(world.state(id)->health == 190);

  // The floor is 200 * 10 / 100 = 20, and a long famine does not pass it.
  run(world, 799, 500000);
  CHECK(world.state(id)->health == 20);
}

/// A fed unit takes no starvation damage at all.
TEST(feeder_does_not_starve_a_fed_unit) {
  World world;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  REQUIRE(world.add_system(&feeder));

  const ObjectId id = world.spawn(NativeClass::unit, nullptr);
  world.set_health(id, 200);
  feeder.enrol(unit(id));
  feeder.start(world);

  // Nineteen intervals: food falls to 1 and health never moves.
  run(world, 800, 190000);
  CHECK(feeder.food(id) == 1);
  CHECK(world.state(id)->health == 200);
}

// --------------------------------------------------------------------------
// the food search
// --------------------------------------------------------------------------

/// The engine path: a hungry unit inside a settlement's holder draws from that
/// settlement's warehouse. This is what makes a stronghold's store a **sink** --
/// `BASETOWNHALL.SC.XML` ships `produces_food="0"` and `max_units="10000"`, so
/// the garrison eats a store with no production behind it, only wagons.
TEST(feeder_draws_a_garrisoned_unit_from_the_settlement_store) {
  World world;
  EconomySystem economy;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&feeder));

  const SettlementId id = economy.create(world, town_hall());
  const Settlement* s = economy.find(id);
  REQUIRE(s != nullptr);
  const ObjectId holder = s->holder.object;
  REQUIRE(holder != kNoObject);

  const ObjectId soldier = world.spawn(NativeClass::unit, nullptr);
  world.set_health(soldier, 200);
  REQUIRE(world.put_in_holder(soldier, holder));
  REQUIRE(economy.garrison_add(id, soldier));

  FeedingUnit link = unit(soldier);
  link.food = 0;  // hungry the instant the clock starts
  feeder.enrol(link);
  economy.start(world);
  feeder.start(world);

  const std::int32_t before = economy.resource(id, Resource::food);
  REQUIRE(before == 200);  // BaseTownhall's settlement_food

  // The search is due at once, and tops the unit up to `max_food` the way
  // `OUTPOST_BEHAVIOR.VS` and `TTENT_BEHAVIOR.VS` do explicitly.
  run(world, 200, 200);
  CHECK(feeder.food(soldier) == 20);
  CHECK(economy.resource(id, Resource::food) == before - 20);
  CHECK(world.state(soldier)->health == 200);  // fed before it could starve

  // And a town hall's store really does only fall. `produces_food="0"` means
  // production adds nothing to it, so over ten seconds the settlement pays the
  // one food the unit eats and nothing puts it back.
  const std::int32_t after_feeding = economy.resource(id, Resource::food);
  run(world, 800, 10000);
  CHECK(economy.resource(id, Resource::food) <= after_feeding);
}

/// A unit in the open is **not** fed by this system. The refill for a unit in
/// the field is `village_behavior_givefood.vs`, which
/// `DATA\CLASSES\BASEVILLAGE.SC.XML` binds as a behaviour and which reaches the
/// unit through `SetFood`; the original may also search a radius, but no
/// shipped file names one and inventing a radius would be inventing a balance
/// rule.
TEST(feeder_does_not_feed_a_unit_standing_in_the_open) {
  World world;
  EconomySystem economy;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&feeder));

  const SettlementId id = economy.create(world, town_hall());
  const ObjectId wanderer = world.spawn(NativeClass::unit, nullptr);
  world.set_health(wanderer, 200);
  FeedingUnit link = unit(wanderer);
  link.food = 0;
  feeder.enrol(link);
  economy.start(world);
  feeder.start(world);

  run(world, 400, 20000);
  CHECK(feeder.food(wanderer) == 0);
  CHECK(world.state(wanderer)->health < 200);  // it starved instead

  // The store is not asserted against a fixed number here: a town hall's food
  // moves on its own -- the economy's `PopulationDecreaseInterval` branch eats
  // `foodperpop` per head whether or not anything else does. What matters is
  // that no 20-food top-up ever left it, which the unit's own empty store shows
  // from the other side.
  (void)id;
}

/// A unit in the open **is** fed from a wagon its squad was sent, once the
/// wagon is built: `Squad::SendFoodWagon`'s mule follows the army there, and
/// here it is the larder `draw_from_larder` empties. The holder path still
/// comes first, and a unit in no squad, or in a squad the wagon does not
/// follow, gets nothing.
TEST(feeder_draws_a_units_food_from_the_wagon_its_squad_was_sent) {
  World world;
  EconomySystem economy;
  HeroSystem heroes;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&heroes));
  REQUIRE(world.add_system(&feeder));

  const SettlementId id = economy.create(world, town_hall());
  economy.set_resource(id, Resource::food, 1000);
  const ObjectId leader = world.spawn(NativeClass::unit, nullptr);
  const ObjectId mate = world.spawn(NativeClass::unit, nullptr);
  const ObjectId stranger = world.spawn(NativeClass::unit, nullptr);
  for (const ObjectId u : {leader, mate, stranger}) {
    world.set_health(u, 200);
    FeedingUnit link = unit(u);
    link.food = 0;
    feeder.enrol(link);
  }
  const SquadKey key = heroes.squads().create(1, leader);
  REQUIRE(heroes.squads().join(key, mate));
  economy.start(world);
  feeder.start(world);

  // Sent after the leader, worth 30: built at 7000, and not before.
  REQUIRE(economy.create_feeding_mule(id, leader, 30) != 0);
  run(world, 400, 6800);
  CHECK(feeder.food(leader) == 0);
  CHECK(feeder.food(mate) == 0);
  // Each unit's next search is due within two seconds of the build; whichever
  // of the two squad-mates searches first takes its 20 and the other the 10
  // left, and the stranger starves.
  run(world, 400, 3200);
  CHECK(feeder.food(leader) + feeder.food(mate) == 30);
  CHECK(feeder.food(leader) > 0);
  CHECK(feeder.food(mate) > 0);
  CHECK(feeder.food(stranger) == 0);
  CHECK(economy.wagons().empty());
}

/// The search delay is drawn from `World::rng()` and from nowhere else: one
/// generator, owned by the world, serialised as world state.
TEST(feeder_draws_its_search_jitter_from_the_world_generator) {
  World world;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  feeder.set_draws_from_warehouse(false);  // no economy: every search fails
  REQUIRE(world.add_system(&feeder));

  FeedingUnit link = unit(9);
  link.food = 0;
  feeder.enrol(link);
  feeder.start(world);

  const std::uint32_t seed_before = world.rng().state();
  run(world, 200, 3000);
  CHECK(world.rng().state() != seed_before);

  // And the delay really is `FoodSearchFreq + rand(FoodSearchFreqVar)`, so a
  // hungry unit searches at most once per 1,000 ms and at least once per
  // 2,000 -- never every frame, which the literal `Freq - Var` reading admits.
  const FeedingUnit* after = feeder.find(9);
  REQUIRE(after != nullptr);
  const GameTime now = world.time();
  CHECK(after->search_due > now);
  CHECK(after->search_due - now <= 2000);
}

/// **The hunger stamp rides in the save.** A build that dropped it from the
/// section would round-trip clean over a chain that never went hungry.
TEST(the_hunger_stamp_round_trips_through_the_section) {
  World world;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  feeder.set_draws_from_warehouse(false);
  REQUIRE(world.add_system(&feeder));
  FeedingUnit link = unit(4);
  link.food = 0;
  link.hungry_since = 1234;
  feeder.enrol(link);
  std::vector<std::byte> bytes;
  feeder.serialize(bytes);
  FeederSystem other;
  REQUIRE(other.deserialize(bytes).ok());
  const FeedingUnit* back = other.find(4);
  REQUIRE(back != nullptr);
  CHECK(back->hungry_since == 1234);
  // And it is in the hash, for the reason `search_due` is.
  std::uint64_t one = 0;
  std::uint64_t two = 0;
  feeder.hash(one);
  other.find(4);
  FeedingUnit* mutable_back = const_cast<FeedingUnit*>(back);
  mutable_back->hungry_since = 1235;
  other.hash(two);
  CHECK(one != two);
}

// --------------------------------------------------------------------------
// the world binding
// --------------------------------------------------------------------------

/// Enrolment is the world's unit set, in ascending id order, and a dead unit
/// leaves the chain.
TEST(feeder_enrols_and_retires_units_with_the_world) {
  World world;
  FeederSystem feeder;
  REQUIRE(world.add_system(&feeder));

  const ObjectId a = world.spawn(NativeClass::unit, nullptr);
  const ObjectId b = world.spawn(NativeClass::unit, nullptr);
  const ObjectId wall = world.spawn(NativeClass::building, nullptr);
  world.set_health(a, 200);
  world.set_health(b, 200);
  world.set_health(wall, 5000);
  feeder.start(world);

  REQUIRE(feeder.total_unit_count() == 2);  // the building is not a unit
  CHECK(feeder.chain()[0].unit == a);
  CHECK(feeder.chain()[1].unit == b);
  CHECK(feeder.food(a) == 20);  // a unit arrives fed

  // With no class graph the fallbacks are `UNIT.SC.XML`'s own numbers.
  CHECK(feeder.max_food(a) == 20);
  CHECK(feeder.feeding(a));

  const ObjectId c = world.spawn(NativeClass::unit, nullptr);
  world.set_health(c, 200);
  world.set_health(b, 0);
  world.advance(200);
  REQUIRE(feeder.total_unit_count() == 2);
  CHECK(feeder.chain()[0].unit == a);
  CHECK(feeder.chain()[1].unit == c);
  CHECK(feeder.find(b) == nullptr);
}

// --------------------------------------------------------------------------
// determinism
// --------------------------------------------------------------------------

/// **The one that matters.** The turn length is renegotiated mid-session and
/// takes at least four values in the retail dumps, so any partition of the same
/// span has to leave the feeder in bit-identical state -- chain, both cursors,
/// both residues and the tick remainder alike. A cursor that were an index
/// rather than an id, or a residue that were dropped at a turn boundary, would
/// fail here and nowhere else.
TEST(feeder_agrees_under_any_turn_partition) {
  const auto play = [](const std::vector<std::int32_t>& lengths) {
    World world;
    FeederSystem feeder;
    feeder.set_world_bound(false);
    world.add_system(&feeder);
    for (ObjectId i = 0; i < 13; ++i) feeder.enrol(unit(100 + i));
    // Three of them start hungry, so the health cursor and the search pass are
    // both live and both contribute to the hash.
    feeder.set_food(101, 0);
    feeder.set_food(105, 0);
    feeder.set_food(111, 0);
    feeder.start(world);

    std::int64_t elapsed = 0;
    const std::int64_t total = 47600;  // not a multiple of any interval
    std::size_t i = 0;
    while (elapsed < total) {
      const std::int32_t want = lengths[i++ % lengths.size()];
      const std::int32_t step =
          static_cast<std::int32_t>(want < total - elapsed ? want : total - elapsed);
      world.advance(step);
      elapsed += step;
    }
    return feeder_hash(feeder);
  };

  const std::uint64_t whole = play({47600});
  CHECK(play({400, 800, 200}) == whole);
  CHECK(play({799}) == whole);
  CHECK(play({100}) == whole);
  CHECK(play({1, 3, 7}) == whole);
}

/// The hash covers the cursors, not just the food. Two worlds with identical
/// per-unit food but different cursor positions are different worlds: the next
/// interval will take food off different units first, and a peer that hashed
/// only the stores would not notice until the divergence had spread.
TEST(feeder_hash_covers_the_cursors) {
  World a;
  FeederSystem fa;
  fa.set_world_bound(false);
  a.add_system(&fa);
  for (ObjectId i = 0; i < 5; ++i) fa.enrol(unit(10 + i));
  fa.start(a);

  World b;
  FeederSystem fb;
  fb.set_world_bound(false);
  b.add_system(&fb);
  for (ObjectId i = 0; i < 5; ++i) fb.enrol(unit(10 + i));
  fb.start(b);

  CHECK(feeder_hash(fa) == feeder_hash(fb));

  // A part-pass on one of them only. Five units over 1,500 ms is fifteen steps
  // and 7,500 unit-milliseconds, short of the 10,000 one drop costs, so every
  // store still reads 20: the two worlds differ in their cursors and residues
  // and in nothing else.
  a.advance(1500);
  for (ObjectId i = 0; i < 5; ++i) CHECK(fa.food(10 + i) == 20);
  CHECK(feeder_hash(fa) != feeder_hash(fb));
}

// --------------------------------------------------------------------------
// the host surface
// --------------------------------------------------------------------------

/// `maxfood/0` (11 sites) and `SetFeeding/1` (14 sites) are the feeder's own,
/// and neither name is defined by any other domain. `food/0` and `SetFood/1`
/// are **not** here: both are already the economy's for a settlement receiver,
/// and a second `define()` on one (kind, name, arity) replaces the first
/// silently, so `sim/economy.cpp` widens them instead.
TEST(feeder_host_defines_only_its_own_names) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  const std::size_t before = registry.implemented();
  const std::size_t defined = register_feeder_hosts(registry);
  CHECK(defined == feeder_host_entry_count());
  CHECK(defined == 2);
  // Nothing was overwritten: the count went up by exactly what was defined.
  CHECK(registry.implemented() == before + defined);
}

TEST(feeder_host_reads_and_writes_a_units_food) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_economy_hosts(registry);
  register_feeder_hosts(registry);

  World world;
  EconomySystem economy;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&feeder));

  const ObjectId soldier = world.spawn(NativeClass::unit, nullptr);
  world.set_health(soldier, 200);
  FeedingUnit link = unit(soldier);
  link.food = 6;
  feeder.enrol(link);
  economy.start(world);
  feeder.start(world);

  const script::Value handle = script::Value::object(kTypeObj, soldier);

  // `.AsUnit.food` -- `UNIT.SC.XML`'s own info bar reads it.
  script::HostOutcome food =
      call_host(registry, world, script::CallKind::member, "food", 0, {handle});
  REQUIRE(food.status == script::HostStatus::ok);
  CHECK(food.value.as_integer() == 6);

  // `.AsUnit.maxfood`, from the same line of the same file.
  const script::HostOutcome maxfood =
      call_host(registry, world, script::CallKind::member, "maxfood", 0, {handle});
  REQUIRE(maxfood.status == script::HostStatus::ok);
  CHECK(maxfood.value.as_integer() == 20);

  // `u.SetFood(u.food + give)` -- `VILLAGE_BEHAVIOR_GIVEFOOD.VS`, which is the
  // ordinary refill path and is script, not engine.
  CHECK(call_host(registry, world, script::CallKind::member, "SetFood", 1,
                  {handle, script::Value::integer(20)})
            .status == script::HostStatus::ok);
  CHECK(feeder.food(soldier) == 20);

  // Clamped to `max_food`: `TTENT_BEHAVIOR.VS` writes `u.SetFood(u.maxfood)`
  // and nothing may exceed it, which is why the dumps' `food` domain is 0..20.
  CHECK(call_host(registry, world, script::CallKind::member, "SetFood", 1,
                  {handle, script::Value::integer(9999)})
            .status == script::HostStatus::ok);
  CHECK(feeder.food(soldier) == 20);

  // `u.SetFood(0)` -- `ITAVERN_CALLTOARMS.VS` and
  // `SETTLEMENT_BEHAVIOR_AMBIENT.VS`.
  CHECK(call_host(registry, world, script::CallKind::member, "SetFood", 1,
                  {handle, script::Value::integer(-5)})
            .status == script::HostStatus::ok);
  CHECK(feeder.food(soldier) == 0);

  // `u1.SetFeeding(false)` -- five shipped behaviours, every call a literal
  // boolean, and `Value::boolean` is `Value::integer(v ? 1 : 0)`.
  CHECK(call_host(registry, world, script::CallKind::member, "SetFeeding", 1,
                  {handle, script::Value::boolean(false)})
            .status == script::HostStatus::ok);
  CHECK(!feeder.feeding(soldier));
  CHECK(call_host(registry, world, script::CallKind::member, "SetFeeding", 1,
                  {handle, script::Value::boolean(true)})
            .status == script::HostStatus::ok);
  CHECK(feeder.feeding(soldier));
}

/// The widened economy entry points still answer for a settlement. A unit
/// fallback that shadowed the settlement reading would be a silent regression
/// in 60 of `food`'s 109 call sites.
TEST(feeder_host_widening_leaves_the_settlement_reading_alone) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_world_host(registry);
  register_economy_hosts(registry);
  register_feeder_hosts(registry);

  World world;
  EconomySystem economy;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  REQUIRE(world.add_system(&economy));
  REQUIRE(world.add_system(&feeder));
  const SettlementId id = economy.create(world, town_hall());
  economy.start(world);
  feeder.start(world);

  const script::Value handle = script::Value::object(kTypeSettlement, economy.find(id)->object);
  const script::HostOutcome food =
      call_host(registry, world, script::CallKind::member, "food", 0, {handle});
  REQUIRE(food.status == script::HostStatus::ok);
  CHECK(food.value.as_integer() == 200);  // BaseTownhall's settlement_food

  CHECK(call_host(registry, world, script::CallKind::member, "SetFood", 1,
                  {handle, script::Value::integer(750)})
            .status == script::HostStatus::ok);
  CHECK(economy.resource(id, Resource::food) == 750);
}

/// A null `CallContext::user` must fail, never dereference. `engine/tests` runs
/// host functions that way on purpose.
TEST(feeder_host_refuses_a_null_context) {
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  register_economy_hosts(registry);
  register_feeder_hosts(registry);

  World world;
  FeederSystem feeder;
  feeder.set_world_bound(false);
  REQUIRE(world.add_system(&feeder));
  feeder.enrol(unit(7));
  feeder.start(world);

  const script::Value handle = script::Value::object(kTypeObj, 7);
  for (const char* name : {"maxfood", "food"}) {
    CHECK(call_host(registry, world, script::CallKind::member, name, 0, {handle}, false)
              .status == script::HostStatus::error);
  }
  for (const char* name : {"SetFeeding", "SetFood"}) {
    CHECK(call_host(registry, world, script::CallKind::member, name, 1,
                    {handle, script::Value::integer(0)}, false)
              .status == script::HostStatus::error);
  }

  // A receiver that is neither a settlement nor a unit in the chain is an
  // error, not a zero.
  CHECK(call_host(registry, world, script::CallKind::member, "maxfood", 0,
                  {script::Value::object(kTypeObj, 4242)})
            .status == script::HostStatus::error);
  // And with no feeder on the world at all.
  World bare;
  CHECK(call_host(registry, bare, script::CallKind::member, "maxfood", 0, {handle})
            .status == script::HostStatus::error);
}
