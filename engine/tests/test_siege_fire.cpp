// Siege fire: catapults and towers shoot when their scripts say so.
// `docs/formats/siege-fire.md` is the model and its addresses; the code is
// `sim/siege_fire.cpp` and the catapult and tower arms of the host entry
// points in `sim/combat.cpp` and `sim/world_host.cpp`.
//
// The classes and entities below are written for the rules, not copied: each
// declares only what a rule reads, under the class names `gbr.exe` tests by
// name (`Catapult`, `CatapultTower`, `FakeTower`, `Military`, `Wall`).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/siege_fire.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

/// `REQUIRE` for a function that returns a value, as `test_siege.cpp` has it.
#define SIEGE_FIRE_REQUIRE_RETURN(expr, value)                                             \
  do {                                                                                     \
    if (!::imperivm::test::check((expr), "REQUIRE(" #expr ")", __FILE__, __LINE__)) return value; \
  } while (0)
#define REQUIRE_RETURN SIEGE_FIRE_REQUIRE_RETURN

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

constexpr PlayerId kUs = 1;
constexpr PlayerId kThem = 2;

/// A catapult whose attack animation (`<anim idx="5">`) lasts 1,500 ms with
/// its action moment at 300, and **no state named `attack`** -- the shape
/// four of the five shipped firing catapults have.
constexpr std::string_view kEngineXml = R"(<?xml version="1.0"?>
<entity name="engine" type="vx/building" variations="8">
  <images>
    <image idx="1" file="cat.rle" drawmode="player_color" remaping="none" rows="3" columns="8"/>
  </images>
  <points>
    <point idx="1" type="3" x="0" y="-100"/>
  </points>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="0"/>
    <state idx="2" name="fight" image_idx="1" image_row="0"/>
  </states>
  <anims>
    <anim idx="5" name="attack" startstate="1" endstate="1" frames="3" duration="1500"
          default_duration="0" action_time="300" step="0">
      <replace layer="1" image="1"/>
      <frame idx="1" duration="500"/>
      <frame idx="2" duration="500"/>
      <frame idx="3" duration="500"/>
    </anim>
  </anims>
</entity>)";

/// The same, with one type-7 point: a Roman tower's.
constexpr std::string_view kTowerEngineXml = R"(<?xml version="1.0"?>
<entity name="rengine" type="vx/building" variations="1">
  <images>
    <image idx="1" file="rcat.rle" drawmode="player_color" remaping="none" rows="1" columns="1"/>
  </images>
  <points>
    <point idx="1" type="2" x="40" y="-28"/>
    <point idx="2" type="7" x="-1" y="-11"/>
  </points>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="0"/>
  </states>
</entity>)";

/// An outpost with two type-7 points, and a type-3 one that is not a site.
constexpr std::string_view kOutpostXml = R"(<?xml version="1.0"?>
<entity name="post" type="vx/building" variations="1">
  <images>
    <image idx="1" file="post.rle" drawmode="player_color" remaping="none" rows="1" columns="1"/>
  </images>
  <points>
    <point idx="1" type="7" x="-200" y="0"/>
    <point idx="2" type="3" x="0" y="50"/>
    <point idx="3" type="7" x="200" y="0"/>
  </points>
  <layers>
    <layer idx="1" name="body" image="1" z="1000"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="1" image_row="0"/>
  </states>
</entity>)";

struct Bench {
  ClassGraph graph;
  World world;
  EconomySystem economy;
  CombatSystem combat;
  MatchSystem match;
  script::HostRegistry registry;
  HostContext context;
  Entity engine_entity;
  Entity tower_engine_entity;
  Entity outpost_entity;

  Bench() {
    const char* docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties maxhealth="100"/></class>)",
        R"(<class id="Unit" parent="Object" cpp_class="CVXUnit"><properties maxhealth="200" damage="10" damage_type="slash" range="17" min_range="2" radius="15" sight="400"/></class>)",
        R"(<class id="Military" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="Swordsman" parent="Military" cpp_class="CVXUnit"/>)",
        R"(<class id="Peasant" parent="Unit" cpp_class="CVXUnit"><properties damage="0" damage_type="none"/></class>)",
        R"(<class id="Arrow" parent="Object" cpp_class="CVXCatapultShot"/>)",
        R"(<class id="Gule" parent="Object" cpp_class="CVXCatapultShot"/>)",
        R"(<class id="Building" parent="Object" cpp_class="CVXBuilding"><properties maxhealth="3000" radius="100" can_be_attacked="1" target_priority="100"/></class>)",
        R"(<class id="House" parent="Building" cpp_class="CVXBuilding"/>)",
        R"(<class id="Keep" parent="Building" cpp_class="CVXBuilding"><properties target_priority="150"/></class>)",
        R"(<class id="Wall" parent="Building" cpp_class="CVXBuilding"><properties target_priority="300"/></class>)",
        R"(<class id="Outpost" parent="Building" cpp_class="CVXTownHall"><properties is_central_building="1"/></class>)",
        R"(<class id="BOutpost" parent="Outpost" cpp_class="CVXOutpost"/>)",
        R"(<class id="FakeTower" parent="Building" cpp_class="CVXBuilding"><properties maxhealth="1500" radius="45" damage="40" damage_type="pierce" range="600" min_range="100" sight="600" attack_delay="400" projectile_class="Arrow" target_priority="150"/></class>)",
        R"(<class id="CatapultTower" parent="FakeTower" cpp_class="CVXBuilding"><properties range="800" min_range="150" sight="0"/></class>)",
        // A short-ranged tower, so that "in sight but out of range" exists.
        R"(<class id="WatchTower" parent="FakeTower" cpp_class="CVXBuilding"><properties range="300"/></class>)",
        R"(<class id="Catapult" parent="Building" cpp_class="CVXCatapult"><properties maxhealth="1000" radius="50" damage="120" damage_type="siege" range="800" min_range="301" sight="850" splash_radius="100" attack_delay="500" projectile_class="Gule" max_units="10" is_central_building="1"/></class>)",
        R"(<class id="GCatapult" parent="Catapult" cpp_class="CVXCatapult"/>)",
        R"(<class id="RCatapult" parent="Catapult" cpp_class="CVXCatapult"><properties range="700" min_range="201"/></class>)",
    };
    int n = 0;
    for (const char* doc : docs) graph.add(bytes_of(doc), "class" + std::to_string(n++) + ".sc.xml");
    graph.link();
    const auto parse = [](std::string_view xml) {
      Result<Entity> e = Entity::parse(bytes_of(xml));
      REQUIRE_RETURN(e.ok(), Entity{});
      return std::move(e.value());
    };
    engine_entity = parse(kEngineXml);
    tower_engine_entity = parse(kTowerEngineXml);
    outpost_entity = parse(kOutpostXml);

    world.set_class_graph(&graph);
    combat.set_class_graph(&graph);
    combat.set_world_bound(true);
    MatchRules rules;
    rules.map_size = 16384;
    match.configure(rules, kNoPlayer, false);
    REQUIRE(world.add_system(&combat));
    REQUIRE(world.add_system(&economy));
    REQUIRE(world.add_system(&match));
    (void)register_all_hosts(registry);
    context.world = &world;
    context.object_type = kTypeObj;
    economy.start(world);
  }

  ObjectId spawn(NativeClass native, const char* cls, Point at, PlayerId owner,
                 std::int32_t health, const Entity* entity = nullptr) {
    const ObjectId id = world.spawn(native, entity, graph.find(cls));
    world.set_position(id, at);
    world.set_owner(id, owner);
    world.set_health(id, health);
    return id;
  }
  ObjectId unit(const char* cls, Point at, PlayerId owner) {
    return spawn(NativeClass::unit, cls, at, owner, 200);
  }

  /// A settlement anchored on `anchor`, as the loader or `PlaceCatapult`
  /// makes one.
  SettlementId settle(ObjectId anchor, PlayerId owner) {
    const World::SettlementIds ids = world.spawn_settlement(owner);
    SettlementInit init;
    init.settlement_object = ids.settlement;
    init.holder_object = ids.holder;
    init.warehouse_object = ids.warehouse;
    init.anchor = anchor;
    init.owner = owner;
    init.max_units = 10000;
    const SettlementId id = economy.create(world, init);
    CHECK(id != kNoSettlement);
    return id;
  }

  ObjectId engine(const char* cls, Point at, PlayerId owner, const Entity* entity) {
    const ObjectId id = spawn(NativeClass::catapult, cls, at, owner, 1000, entity);
    world.find(id)->state.flags.built = true;
    (void)settle(id, owner);
    return id;
  }

  static script::Value obj(ObjectId id) { return script::Value::object(kTypeObj, id); }

  script::HostOutcome call(const char* name, std::vector<script::Value> args,
                           std::int64_t now = 0) {
    const std::uint32_t index = registry.find(script::CallKind::member, name,
                                              static_cast<std::uint16_t>(args.size() - 1));
    REQUIRE_RETURN(index != script::kUnresolvedHost, script::HostOutcome::failed("undeclared"));
    REQUIRE_RETURN(registry.entry(index).fn != nullptr, script::HostOutcome::failed("undefined"));
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    ctx.now = now;
    ctx.waiting_since = now;
    return registry.entry(index).fn(ctx);
  }

  ObjectId handle(const script::HostOutcome& out) const {
    return out.value.is_object() ? out.value.as_object().id : kNoObject;
  }

  void run(int turns) {
    for (int i = 0; i < turns; ++i) world.advance(400);
  }
};

}  // namespace

/// **A catapult's target is a slot of its own, and its towers' guns follow
/// it only when it has towers.** `Catapult::SetTarget` (0x004e3600) stores
/// the handle at `[cat+0x228]` unvalidated -- a friend is stored as readily
/// as an enemy -- and `GetCurrentTarget` (0x004e3500) reads it back; both
/// writers then copy it onto every `CatapultTower` of the engine's
/// settlement while `[cat+0x22c]`, `HasTowers`, is set. Nothing is engaged:
/// the engine shoots from its script.
TEST(a_catapults_target_is_its_own_slot_and_its_towers_follow_it) {
  Bench b;
  const ObjectId roman = b.engine("RCatapult", Point{2000, 2000}, kUs, &b.tower_engine_entity);
  const ObjectId gaul = b.engine("GCatapult", Point{6000, 2000}, kUs, &b.engine_entity);
  const ObjectId friend_ = b.unit("Swordsman", Point{2100, 2400}, kUs);
  b.world.start();

  // The tower placement (0x004e38d0): one `CatapultTower`, in the engine's
  // settlement, the engine's owner's, and `HasTowers` raised. Placed once.
  REQUIRE(place_fake_towers(b.world, roman) == 1);
  CHECK(place_fake_towers(b.world, roman) == 0);
  CHECK(b.world.find(roman)->state.flags.has_towers);
  const Settlement* own = siege_settlement_of(b.world, roman);
  REQUIRE(own != nullptr);
  ObjectId tower = kNoObject;
  for (const SettlementBuilding& member : own->buildings) {
    if (member.object != roman) tower = member.object;
  }
  REQUIRE(tower != kNoObject);
  CHECK(b.world.find(tower)->class_index == b.graph.find("CatapultTower"));
  CHECK(b.world.find(tower)->state.owner == kUs);
  // An engine whose entity has no type-7 point is posted and has none.
  CHECK(place_fake_towers(b.world, gaul) == 0);
  CHECK(!b.world.find(gaul)->state.flags.has_towers);

  CHECK(b.call("SetTarget", {Bench::obj(roman), Bench::obj(friend_)}).status ==
        script::HostStatus::ok);
  CHECK(b.world.state(roman)->catapult_target == friend_);
  CHECK(b.world.state(tower)->ui_target == friend_);
  CHECK(b.handle(b.call("GetCurrentTarget", {Bench::obj(roman)})) == friend_);
  CHECK(b.combat.find(roman)->target == kNoObject);

  CHECK(b.call("ClearTarget", {Bench::obj(roman)}).status == script::HostStatus::ok);
  CHECK(b.world.state(roman)->catapult_target == kNoObject);
  CHECK(b.world.state(tower)->ui_target == kNoObject);
  CHECK(b.handle(b.call("GetCurrentTarget", {Bench::obj(roman)})) == kNoObject);

  // A tower in an engine's settlement that has no `HasTowers` is not touched.
  const SettlementId gauls = siege_settlement_of(b.world, gaul)->id;
  const ObjectId stray = b.spawn(NativeClass::building, "CatapultTower", Point{6100, 2000}, kUs, 1500);
  REQUIRE(b.economy.add_building(b.world, gauls, stray, 1500));
  b.world.mutable_state(stray)->ui_target = roman;
  b.call("SetTarget", {Bench::obj(gaul), Bench::obj(friend_)});
  CHECK(b.world.state(gaul)->catapult_target == friend_);
  CHECK(b.world.state(stray)->ui_target == roman);
}

/// **One `Catapult::Attack(point)` is one throw at the ground** (0x004e2d10,
/// 0x004e22a0, and the impact 0x004e4520 -> 0x005a8e10). The script waits
/// out the attack animation (`<anim idx="5">`'s 1,500 ms here); the shot
/// leaves at its action moment and lands on the point, where everyone --
/// friend as well as foe -- takes the class damage `D` within one unit of
/// surface distance and `D * (R - d)^2 / R^2` out to `R = splash_radius`,
/// with no armour. Refused, with nothing thrown and nothing waited, for a
/// point on the engine itself and for an engine that has towers.
TEST(a_catapult_throws_at_a_point_and_the_ground_shot_hurts_whoever_stands_there) {
  Bench b;
  const ObjectId engine = b.engine("GCatapult", Point{2000, 2000}, kUs, &b.engine_entity);
  const Point at{2600, 2000};
  const ObjectId enemy = b.unit("Swordsman", at, kThem);
  // Surface 50 - 15 = 35 from the point: 120 * 65^2 / 100^2 = 50.
  const ObjectId friend_ = b.unit("Swordsman", Point{2650, 2000}, kUs);
  // Surface 200 - 15 = 185: outside the splash.
  const ObjectId far = b.unit("Swordsman", Point{2800, 2000}, kThem);
  b.world.start();
  b.run(1);

  const std::int64_t now = b.world.time();
  const script::HostOutcome thrown = b.call("Attack", {Bench::obj(engine), pack_point(at)}, now);
  CHECK(thrown.status == script::HostStatus::suspend);
  CHECK(thrown.suspend_for == 1500);
  REQUIRE(b.combat.projectiles().size() == 1);
  CHECK(b.combat.projectiles()[0].launched == now + 300);
  CHECK(b.combat.projectiles()[0].ground);
  b.run(10);
  CHECK(b.combat.projectiles().empty());
  CHECK(b.combat.health(enemy) == 80);
  CHECK(b.combat.health(friend_) == 150);
  CHECK(b.combat.health(far) == 200);

  // The engine's own spot: nothing.
  const script::HostOutcome own =
      b.call("Attack", {Bench::obj(engine), pack_point(b.world.resolve_position(engine))}, now);
  CHECK(own.status == script::HostStatus::ok);
  CHECK(b.combat.projectiles().empty());
  // An engine with towers never throws.
  b.world.mutable_state(engine)->flags.has_towers = true;
  const script::HostOutcome towered = b.call("Attack", {Bench::obj(engine), pack_point(at)}, now);
  CHECK(towered.status == script::HostStatus::ok);
  CHECK(b.combat.projectiles().empty());
}

/// **`AttackWait` takes off the attack animation's `duration`, not a state's
/// cycle** (0x004e32b0 -> 0x004e2030): the record at `[entity+0x147c]` is
/// `<anim idx="5">`, whose `+0x4c` is `duration`. An entity with no state
/// named `attack` -- most shipped catapults -- still has it. One crewman:
/// 600000 / 40 = 15000, less 1500.
TEST(attack_wait_takes_off_the_attack_animations_duration) {
  Bench b;
  const ObjectId engine = b.engine("GCatapult", Point{2000, 2000}, kUs, &b.engine_entity);
  REQUIRE(b.economy.garrison_add(siege_settlement_of(b.world, engine)->id,
                                 b.unit("Swordsman", Point{2000, 2000}, kUs)));
  const script::HostOutcome out = b.call("AttackWait", {Bench::obj(engine)});
  REQUIRE(out.value.is_integer());
  CHECK(out.value.as_integer() == 13500);
}

/// **`Catapult::InRange(point)`** (0x004e3230 -> 0x005a7940): the centre to
/// the point, less the engine's own radius, between `min_range` and `range`
/// inclusive. Radius 50, 301..800.
TEST(a_catapult_measures_a_point_from_its_edge) {
  Bench b;
  const ObjectId engine = b.engine("GCatapult", Point{2000, 2000}, kUs, &b.engine_entity);
  b.world.start();
  const auto in = [&](std::int32_t dx) {
    const script::HostOutcome out =
        b.call("InRange", {Bench::obj(engine), pack_point(Point{2000 + dx, 2000})});
    return out.value.is_integer() && out.value.as_integer() != 0;
  };
  CHECK(!in(350));
  CHECK(in(351));
  CHECK(in(850));
  CHECK(!in(851));
}

/// **`Catapult::BestTarget` wants a building** (0x004e2f60): the highest
/// `target_priority` among the buildings in range, never a `Wall`, a nearly
/// destroyed one spared; a unit only when nothing else is there **and** the
/// engine has towers.
TEST(a_catapult_picks_the_most_wanted_building_and_a_unit_only_with_towers) {
  Bench b;
  const ObjectId engine = b.engine("GCatapult", Point{4000, 4000}, kUs, &b.engine_entity);
  const ObjectId soldier = b.unit("Swordsman", Point{4500, 4000}, kThem);
  b.world.start();
  b.run(1);
  const auto best = [&] { return b.handle(b.call("BestTarget", {Bench::obj(engine)})); };
  // Only a unit: none, for an engine without towers; the unit with them.
  CHECK(best() == kNoObject);
  b.world.mutable_state(engine)->flags.has_towers = true;
  CHECK(best() == soldier);
  b.world.mutable_state(engine)->flags.has_towers = false;

  const ObjectId house = b.spawn(NativeClass::building, "House", Point{4000, 4600}, kThem, 3000);
  const ObjectId keep = b.spawn(NativeClass::building, "Keep", Point{3400, 4000}, kThem, 3000);
  (void)b.spawn(NativeClass::building, "Wall", Point{4000, 3500}, kThem, 3000);
  b.run(1);
  CHECK(best() == keep);
  // The keep, nearly destroyed (tier 3, under a twentieth): spared.
  // Written straight into the world, which is what the sweep reads.
  b.world.mutable_state(keep)->health = 100;
  b.world.mutable_state(keep)->damage_state = 3;
  CHECK(best() == house);
}

/// **`Building::Attack`** (0x004df650): no garrison, no fire; then one arrow
/// a call, the tower's own `attack_delay` and the settlement's `1000 / rate`
/// waited out by suspending and calling again. Four soldiers inside fire two
/// shots a second (`isqrt(4) * BaseFireRate / 100`), shared by every tower of
/// the outpost.
TEST(a_tower_fires_at_its_garrisons_rate_and_waits_out_both_clocks) {
  Bench b;
  const ObjectId post = b.spawn(NativeClass::outpost, "BOutpost", Point{8000, 8000}, kUs, 5000,
                                &b.outpost_entity);
  const SettlementId town = b.settle(post, kUs);
  const ObjectId enemy = b.unit("Swordsman", Point{8000, 8400}, kThem);
  b.world.start();
  b.run(1);  // the post: two `FakeTower`s, at the two type-7 points
  std::vector<ObjectId> towers;
  for (const SettlementBuilding& member : b.economy.settlements().find(town)->buildings) {
    if (member.object != post) towers.push_back(member.object);
  }
  REQUIRE(towers.size() == 2);
  CHECK(b.world.find(towers[0])->class_index == b.graph.find("FakeTower"));
  CHECK(b.world.state(towers[0])->position.x < 8000);
  CHECK(b.world.state(towers[1])->position.x > 8000);
  CHECK(b.world.state(post)->flags.towers_posted);

  const auto attack = [&](ObjectId tower, ObjectId target, std::int64_t now) {
    return b.call("Attack", {Bench::obj(tower), Bench::obj(target)}, now);
  };
  const auto refused = [](const script::HostOutcome& out) {
    return out.status == script::HostStatus::ok && out.value.is_integer() &&
           out.value.as_integer() == 0;
  };
  const auto fired = [](const script::HostOutcome& out) {
    return out.status == script::HostStatus::ok && out.value.is_integer() &&
           out.value.as_integer() == 1;
  };
  // Empty: a rate of 0.
  CHECK(refused(attack(towers[0], enemy, 10000)));
  CHECK(b.combat.projectiles().empty());

  for (int i = 0; i < 4; ++i) {
    REQUIRE(b.economy.garrison_add(town, b.unit("Swordsman", Point{8000, 8000}, kUs)));
  }
  CHECK(fired(attack(towers[0], enemy, 10000)));
  CHECK(b.combat.projectiles().size() == 1);
  CHECK(b.economy.settlements().find(town)->last_tower_fire_time == 10000);
  // The same tower 100 later: 300 of its 400 left.
  const script::HostOutcome again = attack(towers[0], enemy, 10100);
  CHECK(again.status == script::HostStatus::retry);
  CHECK(again.suspend_for == 300);
  // The other tower: free of its own delay, held by the settlement's 500.
  const script::HostOutcome other = attack(towers[1], enemy, 10100);
  CHECK(other.status == script::HostStatus::retry);
  CHECK(other.suspend_for == 400);
  CHECK(fired(attack(towers[1], enemy, 10500)));
  CHECK(b.combat.projectiles().size() == 2);

  // Out of range (600 from the edges): false, not a wait.
  const ObjectId distant = b.unit("Swordsman", Point{8000, 9200}, kThem);
  b.run(1);
  CHECK(refused(attack(towers[0], distant, 20000)));
}

/// **The fire rate counts soldiers whole and the rest up to ten**
/// (0x005c2fb0): `isqrt(military + min(others, 10)) * BaseFireRate / 100`.
TEST(a_settlements_fire_rate_counts_ten_civilians_at_most) {
  Bench b;
  const ObjectId post = b.spawn(NativeClass::outpost, "BOutpost", Point{8000, 8000}, kUs, 5000);
  const SettlementId town = b.settle(post, kUs);
  for (int i = 0; i < 30; ++i) {
    REQUIRE(b.economy.garrison_add(town, b.unit("Peasant", Point{8000, 8000}, kUs)));
  }
  const Settlement& s = *b.economy.settlements().find(town);
  CHECK(settlement_fire_rate(b.world, s, 100) == 3);  // isqrt(10)
  for (int i = 0; i < 6; ++i) {
    REQUIRE(b.economy.garrison_add(town, b.unit("Swordsman", Point{8000, 8000}, kUs)));
  }
  CHECK(settlement_fire_rate(b.world, *b.economy.settlements().find(town), 100) == 4);
  CHECK(settlement_fire_rate(b.world, *b.economy.settlements().find(town), 250) == 10);
}

/// **`Building::BestTarget`** (0x004dcc60 -> 0x004dbb20): in sweep order --
/// rows of 256-unit cells, then cells -- the first unit in sight is taken;
/// after it, a unit in `[min_range, range]` is taken and ends the sweep, and
/// one beyond range replaces the best only when it is no farther. A hidden
/// unit is never offered.
TEST(a_tower_takes_the_first_unit_in_range_after_the_first_it_sees) {
  Bench b;
  const ObjectId post = b.spawn(NativeClass::outpost, "BOutpost", Point{8000, 8000}, kUs, 5000);
  const SettlementId town = b.settle(post, kUs);
  const ObjectId tower = b.spawn(NativeClass::building, "WatchTower", Point{8000, 8000}, kUs, 1500);
  REQUIRE(b.economy.add_building(b.world, town, tower, 1500));
  // Swept first (the topmost row): beyond the 300 range, inside sight.
  const ObjectId first = b.unit("Swordsman", Point{8000, 7500}, kThem);
  // A hidden one, in range, before the rest.
  const ObjectId hidden = b.unit("Swordsman", Point{8000, 7700}, kThem);
  b.world.mutable_state(hidden)->flags.hidden = true;
  // In range, the row below; and nearer still, a row further down.
  const ObjectId in_range = b.unit("Swordsman", Point{8250, 7800}, kThem);
  const ObjectId nearer = b.unit("Swordsman", Point{8000, 8200}, kThem);
  b.world.start();
  b.run(1);
  const ObjectId best = b.handle(b.call("BestTarget", {Bench::obj(tower)}));
  CHECK(best == in_range);
  CHECK(best != nearer);
  CHECK(best != first);
}
