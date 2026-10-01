// A settlement's garrison is its holder: in by `AddUnit`/`ForceAddUnit`, off
// the map while inside, out by `Goto` and friends, off the roster when dead.
//
// The classes and numbers are invented. What they pin is the shape of
// `gbr.exe`'s holder entry (0x005d3e10) and exit (0x005d3f20), and the one
// thing the outposts depend on: `UnitsCount` falls to zero when the last
// defender has gone, so `OUTPOST_BEHAVIOR.VS` can hand the outpost over.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

ClassGraph garrison_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)"),
            "test_garrison.cpp");
  graph.add(bytes_of(R"(<class id="Town" parent="Object" cpp_class="CVXBuilding">
      <properties maxhealth="1000" radius="40" sight="600"/>
    </class>)"),
            "test_garrison.cpp");
  graph.add(bytes_of(R"(<class id="Soldier" parent="Object" cpp_class="CVXUnit">
      <properties maxhealth="200" maxstamina="10" damage="40" damage_type="slash" range="17"
                  min_range="2" radius="15" sight="500" attack_delay="400" max_food="100"/>
    </class>)"),
            "test_garrison.cpp");
  graph.add(bytes_of(R"(<class id="Leader" parent="Object" cpp_class="CVXHero">
      <properties maxhealth="1000" sight="500" radius="15" max_army="10"/>
    </class>)"),
            "test_garrison.cpp");
  graph.link();
  return graph;
}

/// One town of player 0 with room for two, one of its soldiers beside it and
/// one of player 1's close enough to fight.
struct Garrison {
  ClassGraph graph = garrison_graph();
  World world;
  EconomySystem economy;
  CombatSystem combat;
  FeederSystem feeder;
  World::SettlementIds ids{};
  SettlementId town_id = kNoSettlement;
  ObjectId town = kNoObject;
  ObjectId mine = kNoObject;
  ObjectId theirs = kNoObject;

  Garrison() {
    world.set_class_graph(&graph);
    combat.set_class_graph(&graph);
    combat.set_world_bound(true);
    REQUIRE(world.add_system(&combat));
    REQUIRE(world.add_system(&economy));
    REQUIRE(world.add_system(&feeder));
    ids = world.spawn_settlement(0);
    town = world.spawn(NativeClass::building, nullptr, graph.find("Town"));
    world.set_position(town, Point{3000, 3000});
    world.set_owner(town, 0);
    SettlementInit init;
    init.kind = SettlementKind::stronghold;
    init.settlement_object = ids.settlement;
    init.holder_object = ids.holder;
    init.warehouse_object = ids.warehouse;
    init.anchor = town;
    init.owner = 0;
    init.max_units = 2;
    init.food = 1000;
    init.max_food = 5000;
    town_id = economy.create(world, init);
    mine = world.spawn(NativeClass::unit, nullptr, graph.find("Soldier"));
    world.set_position(mine, Point{3100, 3000});
    world.set_owner(mine, 0);
    world.set_health(mine, 200);
    theirs = world.spawn(NativeClass::unit, nullptr, graph.find("Soldier"));
    world.set_position(theirs, Point{3130, 3000});
    world.set_owner(theirs, 1);
    world.set_health(theirs, 200);
    world.start();
    const std::int32_t turn[] = {100};
    world.advance(std::span<const std::int32_t>(turn));
  }

  [[nodiscard]] const Settlement& town_row() const { return *economy.settlements().find(town_id); }
};

/// A host entry point called the way the VM calls it, over `world`.
script::HostOutcome call(const script::HostRegistry& registry, World& world,
                         std::string_view name, std::uint16_t arity,
                         std::vector<script::Value> arguments) {
  const std::uint32_t index = registry.find(script::CallKind::member, name, arity);
  if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not registered");
  const script::HostEntry& entry = registry.entry(index);
  if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
  HostContext state;
  state.world = &world;
  script::CallContext ctx;
  ctx.arguments = arguments;
  ctx.user = &state;
  ctx.name = name;
  ctx.kind = script::CallKind::member;
  return entry.fn(ctx);
}

script::Value obj(ObjectId id) { return script::Value::object(script::ObjectRef{kTypeObj, id}); }

}  // namespace

TEST(a_garrisoned_unit_is_inside_the_holder_and_off_the_map) {
  Garrison g;
  REQUIRE(g.combat.find(g.mine) != nullptr);
  // In the open, the two would fight: the control for everything below.
  REQUIRE(g.combat.best_target(g.theirs) == g.mine);
  REQUIRE(g.combat.best_target(g.mine) == g.theirs);
  const std::int32_t loyalty = g.town_row().loyalty;
  REQUIRE(garrison_enter(g.world, g.town_id, g.mine, /*force=*/false));
  const WorldObject* slot = g.world.find(g.mine);
  REQUIRE(slot != nullptr);
  CHECK(slot->state.holder == g.ids.holder);
  CHECK(slot->state.position == kHeldPosition);
  CHECK(g.town_row().holder.contains(g.mine));
  CHECK(g.town_row().loyalty == loyalty + g.economy.rules().loyalty_increase_per_unit);
  // Combat sees it where the world does at once, not at the top of its turn.
  CHECK(g.combat.find(g.mine)->position == kHeldPosition);

  // Nobody strikes it and it strikes nobody, though the enemy stood beside it.
  CHECK(g.combat.best_target(g.theirs) == kNoObject);
  CHECK(g.combat.best_target(g.mine) == kNoObject);
  CHECK(!g.combat.order_attack(g.theirs, g.mine));
  CHECK(!g.combat.order_attack(g.mine, g.theirs));
  // Not even another garrison: every held object stands at the one sentinel,
  // and two enemies held there are nothing to each other.
  REQUIRE(garrison_enter(g.world, g.town_id, g.theirs, /*force=*/true));
  CHECK(g.combat.best_target(g.theirs) == kNoObject);
  CHECK(g.combat.best_target(g.mine) == kNoObject);
  CHECK(!g.combat.order_attack(g.theirs, g.mine));
  CHECK(!g.combat.order_attack(g.mine, g.theirs));
  CHECK(garrison_forget(g.world, g.theirs));
  // And the sentinel is a point on no map, not a corner of this one: an enemy
  // standing beside (-1, -1) neither finds the garrison nor is found by it.
  REQUIRE(g.world.remove_from_holder(g.theirs, Point{20, 20}));
  g.combat.set_position(g.theirs, Point{20, 20});
  CHECK(g.combat.best_target(g.theirs) == kNoObject);
  CHECK(g.combat.best_target(g.mine) == kNoObject);

  // Full is full for `AddUnit`, and a refusal leaves the unit where it was.
  const ObjectId third = g.world.spawn(NativeClass::unit, nullptr, g.graph.find("Soldier"));
  const ObjectId fourth = g.world.spawn(NativeClass::unit, nullptr, g.graph.find("Soldier"));
  g.world.set_position(third, Point{3200, 3000});
  g.world.set_position(fourth, Point{3300, 3000});
  REQUIRE(garrison_enter(g.world, g.town_id, third, /*force=*/false));
  CHECK(!garrison_enter(g.world, g.town_id, fourth, /*force=*/false));
  CHECK((g.world.find(fourth)->state.position == Point{3300, 3000}));
  CHECK(!g.world.find(fourth)->state.is_held());
  // `ForceAddUnit` is admitted past the cap.
  CHECK(garrison_enter(g.world, g.town_id, fourth, /*force=*/true));
  CHECK(g.town_row().holder.count() == 3);
  // Entering again is not a second place on the roster.
  CHECK(garrison_enter(g.world, g.town_id, fourth, /*force=*/true));
  CHECK(g.town_row().holder.count() == 3);
}

TEST(a_garrisoned_unit_steps_out_one_per_exit_interval_with_its_food) {
  Garrison g;
  (void)g.feeder.enrol(g.world, g.mine);
  REQUIRE(g.feeder.max_food(g.mine) == 100);
  REQUIRE(g.feeder.set_food(g.mine, 30));
  REQUIRE(garrison_enter(g.world, g.town_id, g.mine, /*force=*/true));
  const ObjectId second = g.world.spawn(NativeClass::unit, nullptr, g.graph.find("Soldier"));
  REQUIRE(garrison_enter(g.world, g.town_id, second, /*force=*/true));
  const std::int32_t loyalty = g.town_row().loyalty;
  const std::int32_t stored = g.town_row().warehouse.food;

  // The first goes at once, from the central building's door -- this class
  // has none, so the building's own point -- with its food made up from the
  // town's store.
  const GameTime now = 5000;
  CHECK(garrison_exit(g.world, g.mine, Point{4000, 3000}, now) == 0);
  const WorldObject* out = g.world.find(g.mine);
  CHECK(!out->state.is_held());
  CHECK((out->state.position == Point{3000, 3000}));
  CHECK((g.combat.find(g.mine)->position == Point{3000, 3000}));
  CHECK(!g.town_row().holder.contains(g.mine));
  CHECK(g.town_row().loyalty == loyalty - g.economy.rules().loyalty_increase_per_unit);
  CHECK(g.feeder.food(g.mine) == 100);
  CHECK(g.town_row().warehouse.food == stored - 70);
  CHECK(g.town_row().last_unit_exit_time == now);

  // The second, in the same instant, waits: the interval less the last exit
  // plus the time, the original's arithmetic (0x005d3f60).
  const std::int32_t interval = g.town_row().exit_interval;
  REQUIRE(interval > 0);
  CHECK(garrison_exit(g.world, second, Point{4000, 3000}, now) == interval);
  CHECK(g.world.find(second)->state.is_held());
  CHECK(garrison_exit(g.world, second, Point{4000, 3000}, now + interval - 1) ==
        interval + interval - 1);
  CHECK(garrison_exit(g.world, second, Point{4000, 3000}, now + interval) == 0);
  CHECK(!g.world.find(second)->state.is_held());
  CHECK(g.town_row().holder.count() == 0);

  // Not in a settlement's holder: nothing to do and nothing done.
  CHECK(garrison_exit(g.world, g.theirs, Point{0, 0}, now + 1000) == 0);
  CHECK((g.world.find(g.theirs)->state.position == Point{3130, 3000}));
  // Held by something that is not the settlement's holder -- here its central
  // building, which the settlement lookup would also answer to -- the same.
  REQUIRE(g.world.put_in_holder(g.theirs, g.town));
  CHECK(garrison_exit(g.world, g.theirs, Point{0, 0}, now + 1000) == 0);
  CHECK(g.world.find(g.theirs)->state.holder == g.town);
}

TEST(goto_erase_and_exitholder_take_a_unit_out_of_the_garrison) {
  Garrison g;
  MovementSystem movement;
  movement.set_grid(ObstructionGrid(512, 512));
  REQUIRE(g.world.add_system(&movement));
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  register_movement_host(registry);

  // `Goto` from inside: out first, then routed.
  REQUIRE(garrison_enter(g.world, g.town_id, g.mine, /*force=*/true));
  const script::HostOutcome walked =
      call(registry, g.world, "Goto", 5,
           {obj(g.mine), pack_point(Point{3600, 3000}), script::Value::integer(0),
            script::Value::integer(1000), script::Value::boolean(true), script::Value::integer(0)});
  CHECK(walked.status != script::HostStatus::error);
  CHECK(!g.world.find(g.mine)->state.is_held());
  CHECK(!g.town_row().holder.contains(g.mine));
  CHECK(movement.find(g.mine) != nullptr && movement.find(g.mine)->goto_active);

  // `ExitHolder` is the holder's removal too: off the roster.
  REQUIRE(garrison_enter(g.world, g.town_id, g.theirs, /*force=*/true));
  CHECK(call(registry, g.world, "ExitHolder", 1, {obj(g.theirs), pack_point(Point{2500, 2500})})
            .status == script::HostStatus::ok);
  CHECK(!g.world.find(g.theirs)->state.is_held());
  CHECK(!g.town_row().holder.contains(g.theirs));

  // `Erase` on a garrisoned unit empties its place.
  REQUIRE(garrison_enter(g.world, g.town_id, g.theirs, /*force=*/true));
  REQUIRE(g.town_row().holder.contains(g.theirs));
  CHECK(call(registry, g.world, "Erase", 0, {obj(g.theirs)}).status == script::HostStatus::ok);
  CHECK(g.world.find(g.theirs) == nullptr);
  CHECK(!g.town_row().holder.contains(g.theirs));
}

TEST(a_hero_and_his_army_march_out_of_the_garrison_together) {
  Garrison g;
  MovementSystem movement;
  movement.set_grid(ObstructionGrid(512, 512));
  Result<FormationTable> formations = FormationTable::parse(bytes_of(R"(<Formations>
      <Default Name="Front"/>
      <FormationClass Name="Front" Width="2" Height="1" OffsetFrontLineByY="60"
        OffsetWingsByX="80" OffsetWingsByY="-40">
        <Class Name="Soldier" CentralBlock="1"/>
      </FormationClass>
    </Formations>)"));
  REQUIRE(formations.ok());
  movement.set_formations(std::move(formations.value()));
  HeroSystem heroes;
  REQUIRE(g.world.add_system(&movement));
  REQUIRE(g.world.add_system(&heroes));
  script::HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_world_host(registry);
  register_movement_host(registry);
  (void)register_command_host(registry);

  const ObjectId hero = g.world.spawn(NativeClass::hero, nullptr, g.graph.find("Leader"));
  g.world.set_position(hero, Point{3050, 3000});
  g.world.set_owner(hero, 0);
  g.world.set_health(hero, 1000);
  heroes.register_hero(g.world, hero);
  heroes.register_unit(g.world, g.mine);
  REQUIRE(heroes.attach(g.world, g.mine, hero));
  REQUIRE(garrison_enter(g.world, g.town_id, hero, /*force=*/true));
  REQUIRE(garrison_enter(g.world, g.town_id, g.mine, /*force=*/true));

  const script::HostOutcome setup =
      call(registry, g.world, "FormSetupAndMoveTo", 4,
           {obj(hero), pack_point(Point{3800, 3000}), script::Value::integer(0),
            script::Value::integer(0), script::Value::boolean(true)});
  CHECK(setup.status == script::HostStatus::ok);
  CHECK(!g.world.find(hero)->state.is_held());
  CHECK(!g.world.find(g.mine)->state.is_held());
  CHECK(g.town_row().holder.count() == 0);
}

TEST(the_dead_and_the_departed_leave_the_garrison) {
  Garrison g;
  REQUIRE(garrison_enter(g.world, g.town_id, g.mine, /*force=*/true));
  REQUIRE(garrison_enter(g.world, g.town_id, g.theirs, /*force=*/true));
  REQUIRE(g.town_row().holder.count() == 2);

  script::Scheduler scheduler;
  // Still there, still counted.
  (void)reap_departed(scheduler, g.world);
  CHECK(g.town_row().holder.count() == 2);
  // Gone from the world: off the roster on the next reap, and only that one.
  REQUIRE(g.world.despawn(g.theirs));
  (void)reap_departed(scheduler, g.world);
  CHECK(g.town_row().holder.count() == 1);
  CHECK(g.town_row().holder.contains(g.mine));

  // `garrison_forget` is the same removal, for any roster that has it.
  CHECK(garrison_forget(g.world, g.mine));
  CHECK(g.town_row().holder.count() == 0);
  CHECK(!garrison_forget(g.world, g.mine));
}
