// `Unit::CanAttack`: which units may strike which things at all -- and, last,
// `InRange`, whether one is near enough to, measured where the two stand now.
// sim/combat.hpp's `CombatSystem::unit_can_attack` has the rule and its
// addresses; playtest #24 (docs/plan.html) was a swordsman hitting a fort.
//
// The classes below are written for the rule, not copied: each declares only
// the property the rule reads (`damage_type`, `projectile_class`) and its
// place under the class names `gbr.exe` tests by name (`RamUnit`, `Sentry`,
// `FakeTower`, `Gate`, `Catapult`, `Outpost`, `Wolf`, `Fish`).

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "fight.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Player 1 attacks, player 2 defends.
constexpr PlayerId kUs = 1;
constexpr PlayerId kThem = 2;

/// A world with combat, bound and started, and -- when asked -- the economy
/// that settles buildings into towns.
struct Bench {
  ClassGraph graph;
  World world;
  EconomySystem economy;
  CombatSystem combat;
  script::HostRegistry registry;
  HostContext context;

  explicit Bench(bool with_settlements) {
    const char* docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties maxhealth="100"/></class>)",
        R"(<class id="Unit" parent="Object" cpp_class="CVXUnit"><properties maxhealth="200" damage="10" damage_type="slash" range="17" min_range="2" radius="15" sight="400" attack_delay="400"/></class>)",
        // A sword, which neither shoots nor deals siege damage.
        R"(<class id="Swordsman" parent="Unit" cpp_class="CVXUnit"><properties damage="30" damage_type="slash"/></class>)",
        R"(<class id="Archer" parent="Unit" cpp_class="CVXUnit"><properties damage="20" damage_type="pierce" range="500" projectile_class="Arrow"/></class>)",
        // `slash`, and a `projectile_class` all the same: `BritonHero`'s case.
        R"(<class id="Thrower" parent="Unit" cpp_class="CVXUnit"><properties damage="25" damage_type="slash" range="300" projectile_class="Arrow"/></class>)",
        R"(<class id="RamUnit" parent="Unit" cpp_class="CVXUnit"><properties damage="300" damage_type="siege"/></class>)",
        R"(<class id="BRam" parent="RamUnit" cpp_class="CVXUnit"/>)",
        R"(<class id="Peasant" parent="Unit" cpp_class="CVXUnit"><properties damage="0" damage_type="none"/></class>)",
        R"(<class id="Sentry" parent="Unit" cpp_class="CVXUnit"/>)",
        R"(<class id="Wolf" parent="Unit" cpp_class="CVXUnit"><properties damage="15" damage_type="slash"/></class>)",
        R"(<class id="Fish" parent="Unit" cpp_class="CVXUnit"/>)",
        // A water unit's class (`water_unit`, `[class+0xb30]`), melee.
        R"(<class id="Boat" parent="Unit" cpp_class="CVXUnit"><properties water_unit="1"/></class>)",
        // A wolf's heir: the constructor's beast test walks the ancestry.
        R"(<class id="GreyWolf" parent="Wolf" cpp_class="CVXUnit"/>)",
        // A settlement whose central class may not be attacked.
        R"(<class id="Shrine" parent="Building" cpp_class="CVXTownHall"><properties is_central_building="1" can_be_attacked="0"/></class>)",
        R"(<class id="Pierce" parent="FakeTower" cpp_class="CVXBuilding"><properties damage="40" damage_type="pierce" range="600" projectile_class="Arrow"/></class>)",
        R"(<class id="Arrow" parent="Object" cpp_class="CVXCatapultShot"/>)",
        R"(<class id="Building" parent="Object" cpp_class="CVXBuilding"><properties maxhealth="3000" radius="150" can_be_attacked="1"/></class>)",
        R"(<class id="House" parent="Building" cpp_class="CVXBuilding"/>)",
        R"(<class id="TownHall" parent="Building" cpp_class="CVXTownHall"><properties is_central_building="1"/></class>)",
        R"(<class id="Outpost" parent="Building" cpp_class="CVXTownHall"><properties is_central_building="1"/></class>)",
        R"(<class id="Gate" parent="Building" cpp_class="CVXGate"/>)",
        R"(<class id="FakeTower" parent="Building" cpp_class="CVXBuilding"/>)",
        R"(<class id="Catapult" parent="Building" cpp_class="CVXCatapult"><properties damage="120" damage_type="siege" range="800" min_range="301" radius="50" splash_radius="100"/></class>)",
    };
    int n = 0;
    for (const char* doc : docs) graph.add(bytes_of(doc), "class" + std::to_string(n++) + ".sc.xml");
    graph.link();
    world.set_class_graph(&graph);
    combat.set_class_graph(&graph);
    combat.set_world_bound(true);
    REQUIRE(world.add_system(&combat));
    if (with_settlements) REQUIRE(world.add_system(&economy));
    define_combat_host(registry);
    context.world = &world;
  }

  ObjectId spawn(NativeClass native, const char* cls, Point at, PlayerId owner,
                 std::int32_t health) {
    const ObjectId id = world.spawn(native, nullptr, graph.find(cls));
    world.set_position(id, at);
    world.set_owner(id, owner);
    world.set_health(id, health);
    return id;
  }
  ObjectId unit(const char* cls, Point at, PlayerId owner = kUs) {
    return spawn(NativeClass::unit, cls, at, owner, 200);
  }

  /// A settlement of player 2 centred on `anchor`, with `others` in it.
  void settle(ObjectId anchor, SettlementKind kind, std::vector<ObjectId> others = {},
              bool can_be_attacked = true) {
    SettlementInit init;
    init.kind = kind;
    init.anchor = anchor;
    init.owner = kThem;
    init.max_units = 10;
    init.can_be_attacked = can_be_attacked;
    init.anchor_max_health = 3000;
    const SettlementId id = economy.create(world, init);
    REQUIRE(id != kNoSettlement);
    (void)economy.add_building(world, id, anchor, 3000);
    for (const ObjectId other : others) (void)economy.add_building(world, id, other, 3000);
  }

  bool may(ObjectId attacker, ObjectId target) const {
    const Combatant* a = combat.find(attacker);
    const Combatant* d = combat.find(target);
    CHECK(a != nullptr && d != nullptr);
    return a != nullptr && d != nullptr && combat.unit_can_attack(*a, *d);
  }

  script::HostOutcome call(const char* name, ObjectId self, ObjectId other) {
    const std::uint32_t index = registry.find(script::CallKind::member, name, 1);
    const bool defined = index != 0xFFFFFFFFu && registry.entry(index).fn != nullptr;
    CHECK(defined);
    if (!defined) return script::HostOutcome::failed("not defined");
    std::vector<script::Value> args{script::Value::object(1, self),
                                    script::Value::object(1, other)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  }

  /// Advance `turns` turns of 400 ms.
  /// `Catapult::Attack(point)`: the one entry point that takes a point.
  script::HostOutcome attack_point(ObjectId self, Point at, std::int64_t now) {
    const std::uint32_t index = registry.find(script::CallKind::member, "Attack", 1);
    std::vector<script::Value> args{script::Value::object(1, self), pack_point(at)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "Attack";
    ctx.kind = script::CallKind::member;
    ctx.now = now;
    return registry.entry(index).fn(ctx);
  }

  void run(int turns) {
    for (int i = 0; i < turns; ++i) world.advance(400);
  }
};

}  // namespace

/// **A swordsman may not attack a building, and does not hurt one.** Playtest
/// #24: ordered onto an enemy fort he hit it and its health fell. In the
/// original `Unit::Attack` (0x005ddb50) refuses him at 0x005ddbd2 without
/// suspending, `IsValidTarget` and `CanAttack` answer false, and the target
/// sweep (0x005db650) never offers him one -- so a hall beside him stays whole.
TEST(a_swordsman_may_not_attack_a_building_and_leaves_one_whole) {
  Bench b(false);
  const ObjectId sword = b.unit("Swordsman", Point{1000, 1000});
  // 180 from the centre: 15 between the edges, inside his band of 2 to 17,
  // so reach is not what stops him.
  const ObjectId hall = b.spawn(NativeClass::building, "House", Point{1180, 1000}, kThem, 3000);
  b.world.start();
  REQUIRE(b.combat.find(hall) != nullptr);
  REQUIRE(b.combat.in_attack_range(*b.combat.find(sword), *b.combat.find(hall)));

  CHECK(!b.may(sword, hall));
  CHECK(!b.combat.order_attack(sword, hall));
  CHECK(b.combat.find(sword)->target == kNoObject);
  CHECK(b.combat.best_target(sword) == kNoObject);

  const script::HostOutcome attack = b.call("Attack", sword, hall);
  CHECK(attack.status == script::HostStatus::ok);  // refused: no suspension
  CHECK(attack.value.as_integer() == 0);
  CHECK(b.call("IsValidTarget", sword, hall).value.as_integer() == 0);
  CHECK(b.call("CanAttack", sword, hall).value.as_integer() == 0);

  b.run(30);
  CHECK(b.combat.health(hall) == 3000);

  // He is not disarmed: an enemy soldier at the same spot is a target.
  const ObjectId foe = b.unit("Swordsman", Point{1040, 1000}, kThem);
  b.run(1);
  CHECK(b.may(sword, foe));
  CHECK(b.combat.best_target(sword) == foe);
}

/// **An archer may, and does.** `slash` and `pierce` accept a building when the
/// attacker has a `projectile_class` (0x0053f5b0); the hit is not reduced for
/// being non-siege (inferred: no reader of `damage_type` is in the damage
/// routine).
TEST(an_archer_may_attack_a_building_and_damages_it) {
  Bench b(false);
  const ObjectId archer = b.unit("Archer", Point{1000, 1000});
  const ObjectId thrower = b.unit("Thrower", Point{1000, 1100});
  const ObjectId hall = b.spawn(NativeClass::building, "House", Point{1300, 1000}, kThem, 3000);
  b.world.start();

  CHECK(b.may(archer, hall));
  CHECK(b.may(thrower, hall));
  CHECK(b.call("IsValidTarget", archer, hall).value.as_integer() == 1);
  CHECK(b.call("CanAttack", archer, hall).value.as_integer() == 1);
  const script::HostOutcome attack = b.call("Attack", archer, hall);
  CHECK(attack.status == script::HostStatus::suspend);
  CHECK(b.combat.find(archer)->target == hall);

  b.run(30);
  CHECK(b.combat.health(hall) < 3000);
}

/// **A catapult and a siege-damage unit hurt a building.** The engine is a
/// building, whose own `Catapult::Attack` (0x004e2d10) this rule does not
/// gate -- it throws at a point, and the ground shot hurts what stands there
/// (`docs/formats/siege-fire.md`); `siege` damage accepts anything.
TEST(a_catapult_and_a_ram_damage_a_building) {
  Bench b(false);
  const ObjectId engine =
      b.spawn(NativeClass::catapult, "Catapult", Point{1000, 1000}, kUs, 1000);
  b.world.find(engine)->state.flags.built = true;
  const ObjectId target = b.spawn(NativeClass::building, "House", Point{1500, 1000}, kThem, 3000);
  // A ram at a building outside any settlement: the ram's own clause asks for
  // the settlement's centre, and with none `siege` decides.
  const ObjectId ram = b.unit("BRam", Point{2000, 2000});
  const ObjectId shed = b.spawn(NativeClass::building, "House", Point{2180, 2000}, kThem, 3000);
  b.world.start();

  CHECK(b.attack_point(engine, Point{1500, 1000}, 0).status == script::HostStatus::suspend);
  CHECK(b.may(ram, shed));
  CHECK(imperivm::test::fight(b.world, b.combat, ram, shed, 30) > 0);
  CHECK(b.combat.health(target) < 3000);
  CHECK(b.combat.health(shed) < 3000);
}

/// The whole table, against towns. `RamUnit`: any building of a settlement
/// centred on an `Outpost` or `BaseShipyard`, any `Gate`, nothing else and no
/// unit. `FakeTower`: never. `Catapult`: anyone but the five beasts. `Sentry`:
/// only a unit that shoots. `Fish`: never. `none`: nothing.
TEST(unit_can_attack_follows_the_originals_table) {
  Bench b(true);
  const ObjectId sword = b.unit("Swordsman", Point{100, 100});
  const ObjectId archer = b.unit("Archer", Point{120, 100});
  const ObjectId thrower = b.unit("Thrower", Point{140, 100});
  const ObjectId ram = b.unit("BRam", Point{160, 100});
  const ObjectId peasant = b.unit("Peasant", Point{180, 100});
  const ObjectId wolf = b.unit("Wolf", Point{200, 100});

  const ObjectId hall = b.spawn(NativeClass::town_hall, "TownHall", Point{3000, 3000}, kThem, 3000);
  const ObjectId gate = b.spawn(NativeClass::gate, "Gate", Point{3600, 3000}, kThem, 2000);
  const ObjectId house = b.spawn(NativeClass::building, "House", Point{3000, 3400}, kThem, 1000);
  const ObjectId tower = b.spawn(NativeClass::building, "FakeTower", Point{2600, 3000}, kThem, 1000);
  b.settle(hall, SettlementKind::stronghold, {gate, house, tower});
  const ObjectId outpost = b.spawn(NativeClass::outpost, "Outpost", Point{6000, 6000}, kThem, 3000);
  const ObjectId hut = b.spawn(NativeClass::building, "House", Point{6000, 6400}, kThem, 1000);
  b.settle(outpost, SettlementKind::outpost, {hut});
  const ObjectId engine = b.spawn(NativeClass::catapult, "Catapult", Point{4000, 4000}, kThem, 1000);
  const ObjectId foe = b.unit("Swordsman", Point{200, 200}, kThem);
  const ObjectId sentry = b.unit("Sentry", Point{220, 200}, kThem);
  const ObjectId fish = b.unit("Fish", Point{240, 200}, kThem);
  b.world.start();
  REQUIRE(b.world.find(gate)->settlement != kNoObject);
  REQUIRE(b.world.find(hut)->settlement != kNoObject);

  // The swordsman: units, and the catapult, and no building.
  CHECK(b.may(sword, foe));
  CHECK(!b.may(sword, hall));
  CHECK(!b.may(sword, gate));
  CHECK(!b.may(sword, house));
  CHECK(!b.may(sword, outpost));
  CHECK(!b.may(sword, hut));
  CHECK(b.may(sword, engine));
  CHECK(!b.may(sword, sentry));
  CHECK(!b.may(sword, fish));

  // The archer and the thrower: every building but the fake tower.
  for (const ObjectId shooter : {archer, thrower}) {
    CHECK(b.may(shooter, foe));
    CHECK(b.may(shooter, hall));
    CHECK(b.may(shooter, gate));
    CHECK(b.may(shooter, house));
    CHECK(b.may(shooter, outpost));
    CHECK(b.may(shooter, hut));
    CHECK(!b.may(shooter, tower));
    CHECK(b.may(shooter, engine));
    CHECK(b.may(shooter, sentry));
    CHECK(!b.may(shooter, fish));
  }

  // The ram: the gate, and the outpost's town, and nothing else.
  CHECK(b.may(ram, gate));
  CHECK(!b.may(ram, hall));
  CHECK(!b.may(ram, house));
  CHECK(!b.may(ram, tower));
  CHECK(b.may(ram, outpost));
  CHECK(b.may(ram, hut));
  CHECK(!b.may(ram, foe));
  CHECK(b.combat.order_attack(ram, gate));
  CHECK(!b.combat.order_attack(ram, hall));

  // `none` meets nothing; a wolf may not touch a catapult.
  CHECK(!b.may(peasant, foe));
  CHECK(!b.may(peasant, hall));
  CHECK(!b.may(wolf, engine));
  CHECK(b.may(wolf, foe));

  // A building attacker is not this rule's: the engine may still be aimed.
  CHECK(b.combat.order_attack(engine, sword));
}

/// **`InRange` measures where the two stand now**, as `GotoAttack` does. A
/// combatant's `position` is copied from the world once a turn, and a script
/// runs after movement has stepped the units; measuring the copy, a priest on
/// Balcans was told by `GotoAttack` that it had arrived and by `InRange` that
/// it was out of range, and `EPRIEST_IDLE.VS` looped between the two until its
/// instruction budget ran out (the seventh round's war pass).
TEST(in_range_measures_where_the_two_stand_now) {
  Bench b(false);
  const ObjectId archer = b.unit("Archer", Point{1000, 1000});
  const ObjectId foe = b.unit("Swordsman", Point{1300, 1000}, kThem);
  b.world.start();
  b.run(1);
  REQUIRE(b.combat.find(foe) != nullptr);
  CHECK(b.call("InRange", archer, foe).value.as_integer() == 1);

  // Out of reach (500 + 15 + 15) since the turn began; combat's copy is not.
  b.world.set_position(foe, Point{2000, 1000});
  REQUIRE(b.combat.find(foe)->position.x == 1300);
  CHECK(b.call("InRange", archer, foe).value.as_integer() == 0);

  // And back within it, the copy still where the turn left it.
  b.run(1);
  b.world.set_position(foe, Point{1300, 1000});
  REQUIRE(b.combat.find(foe)->position.x == 2000);
  CHECK(b.call("InRange", archer, foe).value.as_integer() == 1);
}

/// **The virtual reads what the object carries, in the original's order**
/// (0x005b1da0): a hidden target only through the side mask, one in the air
/// or of a water unit's class only to a receiver that shoots, a messenger on
/// either side never.
TEST(the_can_attack_virtual_reads_the_objects_flags) {
  Bench b(false);
  const ObjectId sword = b.unit("Swordsman", Point{100, 100});
  const ObjectId archer = b.unit("Archer", Point{120, 100});
  const ObjectId foe = b.unit("Swordsman", Point{200, 200}, kThem);
  const ObjectId boat = b.unit("Boat", Point{240, 200}, kThem);
  const ObjectId oar = b.unit("Boat", Point{260, 200});
  b.world.start();
  CHECK(b.may(sword, foe));

  // Hidden (bit 21 of `[obj+0x2c]`): refused unless the target's owner shares
  // its view with the receiver's -- the receiver's `seeflags`, not its own
  // view of the target's player.
  b.world.mutable_state(foe)->flags.hidden = true;
  CHECK(!b.may(sword, foe));
  CHECK(!b.may(archer, foe));
  CHECK(b.call("IsValidTarget", sword, foe).value.as_integer() == 0);
  b.world.players().set(kUs, kThem, Relation::share_view, true);
  CHECK(!b.may(sword, foe));
  b.world.players().set(kThem, kUs, Relation::share_view, true);
  CHECK(b.may(sword, foe));
  b.world.mutable_state(foe)->flags.hidden = false;

  // In the air (bit 22 of `[unit+0x194]`): only a receiver that shoots.
  b.world.mutable_state(foe)->flags.in_air = true;
  CHECK(!b.may(sword, foe));
  CHECK(b.may(archer, foe));
  b.world.mutable_state(foe)->flags.in_air = false;

  // A water unit's class: only to one that shoots, or is a water unit too.
  CHECK(!b.may(sword, boat));
  CHECK(b.may(archer, boat));
  CHECK(b.may(oar, boat));

  // A messenger (bit 26 of `[obj+0x2c]`), as target or as receiver.
  b.world.mutable_state(foe)->flags.messenger = true;
  CHECK(!b.may(sword, foe));
  CHECK(!b.may(archer, foe));
  b.world.mutable_state(foe)->flags.messenger = false;
  b.world.mutable_state(sword)->flags.messenger = true;
  CHECK(!b.may(sword, foe));
  b.world.mutable_state(sword)->flags.messenger = false;
  CHECK(b.may(sword, foe));
}

/// **The side test, and the beast that skips it** (0x005b1e19 .. 0x005b1e56).
/// A non-enemy passes only training or as a building with health -- so the
/// virtual alone lets an archer aim at its own house -- and a unit carrying
/// the beast's bit passes whatever the diplomacy. The constructor raises the
/// bit on every heir of the five beasts and on nothing else.
TEST(the_side_test_passes_a_beast_a_trainee_and_a_standing_building) {
  Bench b(false);
  const ObjectId sword = b.unit("Swordsman", Point{100, 100});
  const ObjectId archer = b.unit("Archer", Point{120, 100});
  const ObjectId friend_ = b.unit("Swordsman", Point{140, 100});
  const ObjectId wolf = b.unit("GreyWolf", Point{160, 100});
  const ObjectId house = b.spawn(NativeClass::building, "House", Point{3000, 3000}, kUs, 1000);
  b.world.start();
  CHECK(b.world.find(wolf)->state.flags.strikes_anyone);
  CHECK(!b.world.find(sword)->state.flags.strikes_anyone);

  CHECK(!b.may(sword, friend_));
  CHECK(b.may(wolf, friend_));
  CHECK(b.may(wolf, sword));
  b.world.mutable_state(friend_)->flags.training = true;
  CHECK(b.may(sword, friend_));
  b.world.mutable_state(friend_)->flags.training = false;

  CHECK(b.may(archer, house));
  CHECK(!b.may(sword, house));  // the side test passes; the sword's damage type does not
  b.world.set_health(house, 0);
  CHECK(!b.may(archer, house));
}

/// **A building through its settlement** (0x005b1f51 .. 0x005b1fbc): at no
/// health only in a garrisoned outpost's or shipyard's settlement, and never
/// in a settlement whose `canbeattacked` (`[settlement+0x40]`) is clear.
TEST(a_building_is_judged_by_its_settlement) {
  Bench b(true);
  const ObjectId archer = b.unit("Archer", Point{100, 100});
  const ObjectId shrine = b.spawn(NativeClass::town_hall, "Shrine", Point{3000, 3000}, kThem, 3000);
  const ObjectId porch = b.spawn(NativeClass::building, "House", Point{3000, 3400}, kThem, 1000);
  b.settle(shrine, SettlementKind::other, {porch}, false);
  const ObjectId outpost = b.spawn(NativeClass::outpost, "Outpost", Point{6000, 6000}, kThem, 3000);
  const ObjectId hall = b.spawn(NativeClass::town_hall, "TownHall", Point{9000, 9000}, kThem, 3000);
  b.settle(outpost, SettlementKind::outpost);
  b.settle(hall, SettlementKind::stronghold);
  const ObjectId guard = b.unit("Swordsman", Point{6100, 6100}, kThem);
  b.world.start();

  CHECK(!b.may(archer, shrine));
  CHECK(!b.may(archer, porch));

  CHECK(b.may(archer, outpost));
  b.world.set_health(outpost, 0);
  b.world.set_health(hall, 0);
  CHECK(!b.may(archer, outpost));  // nobody in its holder
  CHECK(!b.may(archer, hall));
  Settlement* camp = b.economy.settlements().for_object(b.world.find(outpost)->settlement);
  Settlement* town = b.economy.settlements().for_object(b.world.find(hall)->settlement);
  REQUIRE(camp != nullptr && town != nullptr);
  REQUIRE(camp->holder.force_add(guard));
  REQUIRE(town->holder.force_add(guard));
  CHECK(b.may(archer, outpost));
  CHECK(!b.may(archer, hall));  // a town hall's is no outpost

  // One's own garrisoned outpost at no health fails the side test, which
  // passes a non-enemy building only with health (0x005b1e4e).
  const ObjectId ours = b.spawn(NativeClass::outpost, "Outpost", Point{12000, 6000}, kUs, 3000);
  b.settle(ours, SettlementKind::outpost);
  b.run(1);  // into combat's roster
  Settlement* own = b.economy.settlements().for_object(b.world.find(ours)->settlement);
  REQUIRE(own != nullptr);
  REQUIRE(own->holder.force_add(guard));
  CHECK(b.may(archer, ours));
  b.world.set_health(ours, 0);
  CHECK(!b.may(archer, ours));
}

/// **A tower asks the same virtual** -- every object family's table carries
/// 0x005b1da0 at `+0xb4` -- from the receiver's side: the side test, and its
/// class's damage type, with none of the unit-receiver clauses.
TEST(a_tower_asks_the_same_virtual_as_a_soldier) {
  Bench b(false);
  const ObjectId tower = b.spawn(NativeClass::building, "Pierce", Point{100, 100}, kUs, 1000);
  const ObjectId foe = b.unit("Swordsman", Point{300, 100}, kThem);
  const ObjectId friend_ = b.unit("Swordsman", Point{320, 100});
  const ObjectId sentry = b.unit("Sentry", Point{340, 100}, kThem);
  b.world.start();
  CHECK(b.may(tower, foe));
  CHECK(!b.may(tower, friend_));
  CHECK(b.may(tower, sentry));  // it shoots
  b.world.mutable_state(foe)->flags.in_air = true;
  CHECK(b.may(tower, foe));
}

/// **A beast's blow on a non-enemy is a full blow** (0x0051163f): the
/// training blow between two units that are not enemies is skipped for an
/// attacker carrying `strikes_anyone`, so a wolf takes a neighbour's soldier
/// below the tenth of his health a training blow would leave him.
TEST(a_beasts_blow_on_a_non_enemy_is_not_a_training_blow) {
  Bench b(false);
  const ObjectId wolf = b.unit("Wolf", Point{1000, 1000});
  const ObjectId sword = b.unit("Swordsman", Point{1000, 1000});
  const ObjectId mate = b.unit("Swordsman", Point{1040, 1000});
  b.world.set_health(mate, 21);
  b.world.start();
  REQUIRE(b.world.find(wolf)->state.flags.strikes_anyone);
  CHECK(imperivm::test::fight(b.world, b.combat, wolf, mate, 10) > 0);
  CHECK(b.combat.health(mate) < 20);  // a training blow stops at 20 (a tenth of 200)

  // And a swordsman training on the same mate stops at the tenth.
  const ObjectId other = b.unit("Swordsman", Point{2040, 1000});
  b.world.set_health(other, 60);
  b.world.mutable_state(other)->flags.training = true;
  b.world.set_position(sword, Point{2000, 1000});
  b.run(1);
  CHECK(imperivm::test::fight(b.world, b.combat, sword, other, 20) > 0);
  CHECK(b.combat.health(other) == 20);
}
