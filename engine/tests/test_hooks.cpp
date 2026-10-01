// The class script hooks: `ondie`, `onkill`, `onenter`.
//
// These go through `GameSession::create` and `advance` rather than through a
// bare `World`, and that is the point of the file rather than a convenience.
// The thread this closes was open because the *seam* was missing, not because
// the lookup was hard: a hook has to fire from `CombatSystem::advance`, which
// holds a world and no scheduler, as well as from `Erase`, which is a host
// function and has both. A test that reached into the runner directly would
// assert the half that was never in doubt.
//
// The class shapes below are the shipped ones. `Military` binds `ondie` --
// fourteen classes do, and it is the base of every soldier -- while `Unit`
// binds `onkill` and `onenter`. Nothing here binds a hook on the leaf class,
// so every assertion also exercises `ClassGraph::resolved_methods`, which is
// where the original's own lookup ends up.
//
// The `map.obj.xml` fragment's element names, attribute spellings and flag
// words are transcribed from the shipped `Adventures\Tutorial.BFHP`
// `Maps\1\map.obj.xml`, as `test_group.cpp`'s is.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/hooks.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;

namespace script = imperivm::core::script;

using imperivm::core::ClassGraph;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Two soldiers of opposing players, close enough to reach each other, and a
/// third that is only ever a bystander.
constexpr std::string_view kMapDocument = R"(<mapobject>
	<scriptobj
		class="RHastatus"
		num="0"
	player="1"
	x="1000"
	y="1000"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="GWarrior"
		num="1"
	player="2"
	x="1010"
	y="1000"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
	<scriptobj
		class="BWarrior"
		num="2"
	player="1"
	x="4000"
	y="4000"
	flags="0xA0400002"
	dir.x="0"
	dir.y="1"/>
</mapobject>
)";

/// `Object` -> `Unit` (`onkill`, `onenter`) -> `Military` (`ondie`) -> the
/// leaves, which bind nothing of their own and differ only in how hard they
/// hit. `BWarrior` is deliberately outside `Military`, so one class has no
/// death hook at all and the tests can tell "fired for the right object" from
/// "fired".
ClassGraph hook_graph() {
  ClassGraph graph;
  graph.add(bytes_of(R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)"),
            "test_hooks.cpp");
  graph.add(bytes_of(R"(<class id="Unit" parent="Object" cpp_class="CVXUnit">
      <properties sight="400" maxhealth="200" maxstamina="10" damage="40" range="40"
                  radius="10" selection_radius="10"/>
      <method sig="onkill" vs="onkill.vs"/>
      <method sig="onenter" vs="onenter.vs"/>
    </class>)"),
            "test_hooks.cpp");
  graph.add(bytes_of(R"(<class id="Military" parent="Unit" cpp_class="CVXUnit">
      <method sig="ondie" vs="ondie.vs"/>
    </class>)"),
            "test_hooks.cpp");
  graph.add(bytes_of(R"(<class id="RHastatus" parent="Military" cpp_class="CVXUnit"/>)"),
            "test_hooks.cpp");
  // Twice the damage and the same health, so the fight has one outcome and the
  // test can name the killer rather than assert whichever way it fell.
  graph.add(bytes_of(R"(<class id="GWarrior" parent="Military" cpp_class="CVXUnit">
      <properties damage="80"/>
    </class>)"),
            "test_hooks.cpp");
  graph.add(bytes_of(R"(<class id="BWarrior" parent="Unit" cpp_class="CVXUnit"/>)"),
            "test_hooks.cpp");
  // A leader to be paid, and a soldier whose own `ondie` pays it -- the shape
  // of `CMercenary`, which binds its hook on its own class and so overrides
  // `Military`'s. Neither is on the map; the tests that need them place them.
  graph.add(bytes_of(R"(<class id="Leader" parent="Unit" cpp_class="CVXHero">
      <properties maxhealth="1000" inventory_size="8" max_army="50"/>
    </class>)"),
            "test_hooks.cpp");
  graph.add(bytes_of(R"(<class id="Hireling" parent="Military" cpp_class="CVXUnit">
      <method sig="ondie" vs="hireling_ondie.vs"/>
    </class>)"),
            "test_hooks.cpp");
  graph.link();
  return graph;
}

/// A `ScriptResolver` over an in-memory table.
class Scripts final : public ScriptResolver {
 public:
  /// Replaces, so a test can hand one file a different body than
  /// `add_hook_scripts` gave it. Appending instead would leave the first
  /// version answering and the override silently unread, which is a fixture
  /// asserting the wrong thing.
  void add(std::string path, std::string text) {
    for (auto& [name, body] : files_) {
      if (name == path) {
        body = std::move(text);
        return;
      }
    }
    files_.emplace_back(std::move(path), std::move(text));
  }
  std::span<const std::byte> source(std::string_view path) override {
    for (const auto& [name, text] : files_) {
      if (name == path) return bytes_of(text);
    }
    return {};
  }

 private:
  std::vector<std::pair<std::string, std::string>> files_;
};

/// The three hook scripts, each recording enough for a test to tell which
/// object it ran on. `.player` is the ambient receiver -- the leading dot binds
/// to the variable spelled exactly `this`, which is the first parameter here as
/// it is in `UNIT_ON_KILL.VS`.
void add_hook_scripts(Scripts& scripts) {
  scripts.add("ondie.vs",
              "//void, Obj this\n"
              "EnvWriteInt(\"died\", EnvReadInt(\"died\") + 1);\n"
              "EnvWriteInt(\"diedplayer\", .player);\n"
              "EnvWriteInt(\"diedhealth\", .health);\n");
  scripts.add("onkill.vs",
              "//void, Obj this, Obj Victim\n"
              "EnvWriteInt(\"killed\", EnvReadInt(\"killed\") + 1);\n"
              "EnvWriteInt(\"killerplayer\", .player);\n"
              "EnvWriteInt(\"victimplayer\", Victim.player);\n");
  scripts.add("onenter.vs",
              "//void, Obj this, Settlement sett\n"
              "EnvWriteInt(\"entered\", EnvReadInt(\"entered\") + 1);\n"
              "EnvWriteInt(\"enteredgold\", sett.gold);\n");
  // What `CMERCENARY_ONDIE.VS` does, written here from its behaviour rather
  // than copied: with the pact researched, find the dying unit's hero through
  // `.hero` and add `MercenaryPactGold` to the hero's gold item, minting one
  // if it has none. A hero that is no longer there pays nothing -- which is
  // exactly what a detach that ran before the hook would produce.
  scripts.add("hireling_ondie.vs",
              "//void, Obj this\n"
              "Unit me;\n"
              "Hero boss;\n"
              "Item purse;\n"
              "me = this.AsUnit;\n"
              "if (!me.IsValid) return;\n"
              "if (EnvReadString(me.player, \"Mercenary pact\") != \"researched\") return;\n"
              "boss = me.hero;\n"
              "if (!boss.IsValid) return;\n"
              "purse = boss.FindItem(\"Gold\");\n"
              "if (!purse.IsValid) {\n"
              "  if (!boss.AddItem(\"Gold\")) return;\n"
              "  purse = boss.FindItem(\"Gold\");\n"
              "}\n"
              "purse.SetUseCount(purse.use_count + GetConst(\"MercenaryPactGold\"));\n");
}

/// One item the hireling's hook can mint. Authored, not the shipped catalogue.
constexpr std::string_view kPurseItems = R"(<items>
  <item id="Gold" name="Gold" level="0" usecount="0" important="no"/>
</items>
)";

[[nodiscard]] std::int32_t env_int(World& world, std::string_view key) {
  EnvSystem* env = env_of(world);
  return env == nullptr ? -1 : env->env().read_int(EnvScope::root(), key);
}

/// A session over `kMapDocument` and `hook_graph`, with the three hook scripts
/// in the library. Object ids are 1, 2 and 3 in document order.
struct Fixture {
  script::HostRegistry registry;
  ClassGraph graph = hook_graph();
  Scripts scripts;
  std::unique_ptr<GameSession> session;

  Fixture() {
    register_all_hosts(registry);
    add_hook_scripts(scripts);
    SessionInputs inputs;
    inputs.classes = &graph;
    inputs.scripts = &scripts;
    inputs.map_objects = bytes_of(kMapDocument);
    inputs.items = bytes_of(kPurseItems);
    imperivm::core::Result<std::unique_ptr<GameSession>> made =
        GameSession::create(registry, inputs, /*seed=*/1);
    if (made.ok()) session = std::move(made.value());
  }

  [[nodiscard]] World& world() { return session->world(); }
  [[nodiscard]] CombatSystem& combat() { return *combat_system_of(world()); }
  [[nodiscard]] HeroSystem& heroes() { return *hero_system_of(world()); }
};

/// The pact's payout, in `CONST.INI`'s `MercenaryPactGold` shape. Not the
/// shipped figure, so a pass cannot be a coincidence with the data.
constexpr std::int32_t kPactGold = 37;

/// A `Leader` with a `Hireling` in its army, both owned by player 1 and placed
/// far from the fight on the map, with the pact researched and its payout
/// declared. Combat is told about both, so a blow can kill the hireling.
struct PactScene {
  ObjectId leader = kNoObject;
  ObjectId hireling = kNoObject;
};

PactScene hire(Fixture& f) {
  PactScene scene;
  const ClassGraph& graph = f.graph;
  scene.leader = f.world().spawn(imperivm::core::NativeClass::hero, nullptr, graph.find("Leader"));
  scene.hireling =
      f.world().spawn(imperivm::core::NativeClass::unit, nullptr, graph.find("Hireling"));
  for (const ObjectId id : {scene.leader, scene.hireling}) {
    f.world().set_owner(id, 0);
    (void)f.world().set_position(id, Point{6000, 6000});
  }
  f.world().set_health(scene.leader, 1000);
  f.world().set_health(scene.hireling, 200);
  (void)f.heroes().attach(f.world(), scene.hireling, scene.leader);
  (void)f.combat().reconcile(f.world());
  // `GetConst` answers an economy constant from the economy's rules before it
  // asks the environment, and `MercenaryPactGold` is one of those.
  if (EconomySystem* economy = economy_of(f.world()); economy != nullptr) {
    EconomyRules rules = economy->rules();
    rules.mercenary_pact_gold = kPactGold;
    economy->set_rules(rules);
  }
  EnvSystem* env = env_of(f.world());
  if (env != nullptr) {
    // `.player` is the owner plus one, and `EnvReadString(player, ...)` keys
    // the player scope by the number the script hands it.
    env->env().write_string(EnvScope::for_player(/*owner 0, plus one=*/1), "Mercenary pact",
                            "researched");
  }
  return scene;
}

[[nodiscard]] std::int32_t purse_of(Fixture& f, ObjectId hero) {
  ItemStore& items = f.heroes().items();
  const ObjectId purse = items.find_of_type(hero, "Gold");
  return purse == kNoObject ? 0 : items.use_count(purse);
}

constexpr ObjectId kHastatus = 1;  // player 1, binds `ondie` through `Military`
constexpr ObjectId kWarrior = 2;   // player 2, binds `ondie` too, and hits harder
constexpr ObjectId kHookless = 3;  // player 1, far away, binds no `ondie` at all

// The map's `player="1"` is owner 0, and `Obj::player` hands a script the owner
// plus one -- scripts count players from 1. So the two fighters read back as 1
// and 2 in that order.
constexpr std::int32_t kHastatusPlayer = 1;
constexpr std::int32_t kWarriorPlayer = 2;

[[nodiscard]] script::Value handle(ObjectId id) {
  return script::Value::object(script::ObjectRef{kTypeObj, id});
}

/// One member entry point, against the session's own world and scheduler --
/// which is what carries the hook runner, so the call fires hooks exactly as a
/// script would, and `Erase` performs its coroutine sweep for real.
script::HostOutcome call_member(script::HostRegistry& registry, GameSession& run,
                                const char* name, std::uint16_t arity,
                                std::vector<script::Value> arguments) {
  const std::uint32_t index = registry.find(script::CallKind::member, name, arity);
  if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
  const script::HostEntry& entry = registry.entry(index);
  if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
  HostContext state;
  state.world = &run.world();
  script::CallContext ctx;
  ctx.arguments = arguments;
  ctx.user = &state;
  ctx.name = name;
  ctx.kind = script::CallKind::member;
  ctx.scheduler = &run.scheduler();
  return entry.fn(ctx);
}

script::HostOutcome call_member(Fixture& f, const char* name, std::uint16_t arity,
                                std::vector<script::Value> arguments) {
  return call_member(f.registry, *f.session, name, arity, std::move(arguments));
}

/// `Obj::Erase`, through the entry point rather than through the routine behind
/// it, so the deferral latch, the coroutine sweep and the hook all run in the
/// order a script would see.
[[nodiscard]] bool erase(script::HostRegistry& registry, GameSession& run, ObjectId id) {
  return call_member(registry, run, "Erase", 0, {handle(id)}).status == script::HostStatus::ok;
}

[[nodiscard]] bool erase(Fixture& f, ObjectId id) {
  return erase(f.registry, *f.session, id);
}

/// A settlement composite with a garrison that holds `max_units` and a
/// warehouse holding `gold`. The gold is a value only this settlement has, so a
/// hook handed anything else reads zero.
SettlementId make_town(Fixture& f, EconomySystem& economy, std::int32_t max_units,
                       std::int32_t gold) {
  const World::SettlementIds ids = f.world().spawn_settlement(/*owner=*/0);
  SettlementInit init;
  init.settlement_object = ids.settlement;
  init.holder_object = ids.holder;
  init.warehouse_object = ids.warehouse;
  init.owner = 0;
  // A village ships `max_units="0"` and a town hall 10,000; a garrison with no
  // room refuses the unit, and the original fires nothing when it does.
  init.max_units = max_units;
  const SettlementId id = economy.settlements().create(init);
  if (Settlement* s = economy.settlements().find(id); s != nullptr) s->warehouse.gold = gold;
  return id;
}

[[nodiscard]] ObjectId town_object(EconomySystem& economy, SettlementId id) {
  const Settlement* s = economy.settlements().find(id);
  return s == nullptr ? kNoObject : s->object;
}

/// `Settlement::AddUnit`, which is what a script calls.
[[nodiscard]] bool add_unit(Fixture& f, ObjectId settlement, ObjectId unit) {
  const script::HostOutcome out =
      call_member(f, "AddUnit", 1, {handle(settlement), handle(unit)});
  return out.status == script::HostStatus::ok && out.value.as_integer() == 1;
}

}  // namespace

// ---------------------------------------------------------------------------
// ondie
// ---------------------------------------------------------------------------

TEST(a_script_that_damages_a_unit_to_death_runs_its_class_ondie) {
  // `Obj::Damage(n)` is the head of the original's
  // `Damage -> SetHealth -> IsDead -> vtbl[0xB0]` chain, and the death virtual
  // is where the hook fires. A payout that only happened when a spear did the
  // killing would leave `GHOST_SAC_IDLE.VS` -- which damages its ghost to death
  // and nothing else -- outside the mechanism.
  Fixture f;
  REQUIRE(f.session != nullptr);
  CHECK(env_int(f.world(), "died") == 0);

  f.combat().apply_damage(f.world(), kHastatus, f.combat().health(kHastatus));
  CHECK(!f.combat().is_alive(kHastatus));
  // Run, not spawned: 0x0069dca0 runs the hook to its end inside the death,
  // so it has written before the scheduler has had a pass.
  CHECK(env_int(f.world(), "died") == 1);
  CHECK(env_int(f.world(), "diedplayer") == kHastatusPlayer);
  // And it read the world's health as the death virtual left it: `SetHealth(0)`
  // comes before the hook, not at the end of the turn with everybody else's.
  CHECK(env_int(f.world(), "diedhealth") == 0);
  // And it is not also sitting in the scheduler to run a second time.
  f.session->advance(1, 400);
  CHECK(env_int(f.world(), "died") == 1);
}

TEST(a_class_with_no_ondie_binding_starts_nothing) {
  // `BWarrior` descends from `Unit` and not from `Military`, so its resolved
  // method table has `onkill` and `onenter` in it and no `ondie`. The original
  // compares its map find against `end()` and returns, with no diagnostic: 203
  // of the installation's 845 classes resolve an `ondie` and the other 642 are
  // this case, so a miss is an answer rather than a fault.
  Fixture f;
  REQUIRE(f.session != nullptr);

  f.combat().apply_damage(f.world(), kHookless, f.combat().health(kHookless));
  CHECK(!f.combat().is_alive(kHookless));
  f.session->advance(1, 400);
  CHECK(env_int(f.world(), "died") == 0);
}

TEST(ondie_fires_once_however_many_paths_reach_the_object) {
  // `0x005141b0` refuses when `[obj+0xbc]` is set and writes 1 to it on the way
  // out, so it runs at most once per object ever. That matters here because
  // this engine leaves a corpse standing for the length of its death animation
  // and a script can `Erase` one inside that window -- without the latch,
  // `MILITARY_ONDIE.VS` would pay Warrior Tales twice for one casualty.
  Fixture f;
  REQUIRE(f.session != nullptr);

  f.combat().apply_damage(f.world(), kHastatus, f.combat().health(kHastatus));
  CHECK(f.world().state(kHastatus)->flags.ondie_fired);

  // The corpse is still in the world: the drain that removes it is
  // `death_duration_` away.
  REQUIRE(f.world().find(kHastatus) != nullptr);
  REQUIRE(erase(f, kHastatus));

  f.session->advance(1, 400);
  CHECK(env_int(f.world(), "died") == 1);
}

TEST(erase_alone_runs_ondie_too) {
  // The other half of the shared routine, and the half that made this a thread
  // rather than a patch: `Erase` and the death virtual both reach it, and an
  // implementation on one of them is worse than none.
  Fixture f;
  REQUIRE(f.session != nullptr);

  REQUIRE(erase(f, kHastatus));
  CHECK(f.world().find(kHastatus) == nullptr);
  CHECK(env_int(f.world(), "died") == 1);
  // And it ran against the object while it was still whole: the hook is the
  // first thing the erase does and it runs to its end there, so `.player`
  // resolves. This used to assert -1, on the reading that the hook ran on a
  // later pass against a freed handle; that reading was this engine's, not
  // the original's.
  CHECK(env_int(f.world(), "diedplayer") == kHastatusPlayer);
  f.session->advance(1, 400);
  CHECK(env_int(f.world(), "died") == 1);
}

TEST(a_hook_that_sleeps_is_abandoned_where_it_sleeps) {
  // 0x0069dca0 deletes the context the moment the interpreter returns, and a
  // host call answering "suspend" is one of the ways it returns -- so what a
  // hook does before a `Sleep` happens and what it does after never does, and
  // nothing is left in the scheduler to wake. INFERRED from the disassembly;
  // none of the ten shipped hook scripts sleeps, so no shipped behaviour can
  // tell. Asserted so that the choice is a choice and not an accident.
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = hook_graph();
  Scripts scripts;
  add_hook_scripts(scripts);
  scripts.add("ondie.vs",
              "//void, Obj this\n"
              "EnvWriteInt(\"before\", 1);\n"
              "Sleep(1000);\n"
              "EnvWriteInt(\"died\", EnvReadInt(\"died\") + 1);\n");

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.map_objects = bytes_of(kMapDocument);
  imperivm::core::Result<std::unique_ptr<GameSession>> made =
      GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(made.ok());
  GameSession& run = *made.value();

  const std::size_t live = run.scheduler().live_count();
  REQUIRE(erase(registry, run, kHastatus));
  CHECK(env_int(run.world(), "before") == 1);
  CHECK(run.scheduler().live_count() == live);
  run.advance(4, 400);
  CHECK(env_int(run.world(), "died") == 0);
}

TEST(a_script_owned_by_an_object_that_left_the_world_is_reaped_before_it_resumes) {
  // The death path: `CombatSystem::advance` despawns the corpse at the end of
  // its dying state and holds no scheduler, so it cannot do what `Erase`
  // does. `reap_departed` runs once per turn before the scripts, and a
  // sleeping `idle`-shaped coroutine whose owner is gone never wakes -- the
  // original cancels the object's pending messages when it frees the corpse
  // (0x00687c80). `UNIT_AI_KILLALL.VS:24` resumed on the freed handle and
  // its `.AddCommand` trapped, five times over the 300-turn sweep.
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = hook_graph();
  Scripts scripts;
  add_hook_scripts(scripts);
  // The library compiles what the class tree binds, so the owned script wears
  // `ondie.vs`'s name; nothing here dies through the hook path, so it is only
  // ever started by hand below.
  scripts.add("ondie.vs",
              "//void, Obj this\n"
              "Sleep(1000);\n"
              "EnvWriteInt(\"woke\", EnvReadInt(\"woke\") + 1);\n");

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.map_objects = bytes_of(kMapDocument);
  imperivm::core::Result<std::unique_ptr<GameSession>> made =
      GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(made.ok());
  GameSession& run = *made.value();

  // Compiled on demand, as a hook or a command method would have it.
  REQUIRE(run.host_context().library != nullptr);
  const std::uint32_t chunk = run.host_context().library->chunk_for("ondie.vs");
  REQUIRE(chunk != script::kNoChunk);
  const script::Value self = handle(kHastatus);
  const script::ScriptId owned = run.scheduler().spawn(
      chunk, std::span<const script::Value>{&self, 1}, script::ObjectRef{kTypeObj, kHastatus});
  REQUIRE(owned != script::kNoScript);
  // A peer that is not owned by the object keeps running: the reaper reads
  // the owner, not the argument.
  const script::ScriptId peer =
      run.scheduler().spawn(chunk, std::span<const script::Value>{&self, 1});
  REQUIRE(peer != script::kNoScript);

  run.advance(1, 400);  // both asleep
  REQUIRE(run.world().despawn(kHastatus));
  CHECK(reap_departed(run.scheduler(), run.world()) == 1);
  CHECK(reap_departed(run.scheduler(), run.world()) == 0);  // idempotent
  run.advance(3, 400);
  CHECK(env_int(run.world(), "woke") == 1);

  // And the turn does it on its own: a second owned script, a despawn between
  // turns, and the sleep never ends.
  const script::Value other = handle(kHookless);
  REQUIRE(run.scheduler().spawn(chunk, std::span<const script::Value>{&other, 1},
                                script::ObjectRef{kTypeObj, kHookless}) != script::kNoScript);
  run.advance(1, 400);
  REQUIRE(run.world().despawn(kHookless));
  run.advance(3, 400);
  CHECK(env_int(run.world(), "woke") == 1);
}

TEST(a_corpse_s_own_scripts_end_at_death_and_its_ondie_hook_does_not) {
  // All 15 dying objects in the nine dumps print an empty `method=` and an
  // empty command queue. The dying state now lasts the whole death animation,
  // so a corpse whose behaviour kept running would go on ordering itself
  // about for sixteen seconds. The object is still in the world -- only its
  // own coroutines go, at the first reap after it died, and the `ondie` hook,
  // spawned detached precisely so that it outlives its object, runs.
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = hook_graph();
  Scripts scripts;
  add_hook_scripts(scripts);
  // The owned script wears `onenter.vs`'s name, which the library compiles
  // because the class tree binds it; nothing here enters a settlement.
  scripts.add("onenter.vs",
              "//void, Obj this\n"
              "Sleep(1000);\n"
              "EnvWriteInt(\"woke\", EnvReadInt(\"woke\") + 1);\n");

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.map_objects = bytes_of(kMapDocument);
  imperivm::core::Result<std::unique_ptr<GameSession>> made =
      GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(made.ok());
  GameSession& run = *made.value();

  REQUIRE(run.host_context().library != nullptr);
  const std::uint32_t chunk = run.host_context().library->chunk_for("onenter.vs");
  REQUIRE(chunk != script::kNoChunk);
  const script::Value self = handle(kHastatus);
  REQUIRE(run.scheduler().spawn(chunk, std::span<const script::Value>{&self, 1},
                                script::ObjectRef{kTypeObj, kHastatus}) != script::kNoScript);
  run.advance(1, 400);  // asleep

  CombatSystem& combat = *combat_system_of(run.world());
  combat.apply_damage(run.world(), kHastatus, combat.health(kHastatus));
  REQUIRE(combat.is_dying(kHastatus));
  REQUIRE(run.world().find(kHastatus) != nullptr);  // a corpse, not gone
  CHECK(reap_departed(run.scheduler(), run.world()) == 1);
  CHECK(reap_departed(run.scheduler(), run.world()) == 0);

  // The owned script never wakes; the hook runs, once.
  run.advance(4, 400);
  CHECK(env_int(run.world(), "woke") == 0);
  CHECK(env_int(run.world(), "died") == 1);
}

TEST(a_world_with_no_runner_takes_the_latch_and_starts_nothing) {
  // Which is the original's third guard: `[0x9c0824]` non-zero skips the launch
  // and the routine still writes its latch. Every synthetic test in this suite
  // is that case, so it has to be the quiet one.
  World world;
  const ObjectId id = world.spawn(imperivm::core::NativeClass::unit, nullptr);
  REQUIRE(id != kNoObject);
  CHECK(!fire_class_hook(world, ClassHook::on_die, id));
  CHECK(world.state(id)->flags.ondie_fired);
  // And a second pass is refused on the latch rather than on the missing
  // runner, which is what keeps the two guards independent.
  CHECK(!fire_class_hook(world, ClassHook::on_die, id));

  // A handle that resolves to nothing is the first guard and touches neither.
  CHECK(!fire_class_hook(world, ClassHook::on_die, id + 1000));
}

TEST(the_ondie_latch_survives_a_save) {
  // Fifteen objects sit in the dying state across the nine dumps, so a save can
  // be taken between the hook firing and the corpse leaving the world. A reload
  // that forgot would run the payout a second time.
  World world;
  const ObjectId id = world.spawn(imperivm::core::NativeClass::unit, nullptr);
  REQUIRE(id != kNoObject);
  (void)fire_class_hook(world, ClassHook::on_die, id);

  std::vector<std::byte> bytes;
  world.serialize(bytes);

  World restored;
  REQUIRE(restored.deserialize(bytes).ok());
  REQUIRE(restored.state(id) != nullptr);
  CHECK(restored.state(id)->flags.ondie_fired);
  CHECK(!fire_class_hook(restored, ClassHook::on_die, id));
}

// ---------------------------------------------------------------------------
// onkill
// ---------------------------------------------------------------------------

TEST(a_killing_blow_runs_the_killers_onkill_with_the_victim) {
  // `0x005119ee` fires it on the attacker, from the block that pays kill
  // experience, with the victim as the second argument. Both halves are
  // asserted: the two fighters have different owners, so a hook that ran on the
  // wrong object or read the wrong argument reports the wrong pair.
  Fixture f;
  REQUIRE(f.session != nullptr);

  REQUIRE(f.combat().order_attack(kWarrior, kHastatus));
  // Long enough for the swings to land: 200 health against 80 damage a blow.
  // Both fight back, and the harder hitter is the one still standing.
  f.session->advance(40, 400);

  CHECK(!f.combat().is_alive(kHastatus));
  CHECK(f.combat().is_alive(kWarrior));
  CHECK(env_int(f.world(), "killed") == 1);
  CHECK(env_int(f.world(), "killerplayer") == kWarriorPlayer);
  CHECK(env_int(f.world(), "victimplayer") == kHastatusPlayer);
  // And the victim's own `ondie` ran on the same blow. Exactly one of each:
  // the fight is one death, and the two hooks are on opposite ends of it.
  CHECK(env_int(f.world(), "died") == 1);
  CHECK(env_int(f.world(), "diedplayer") == kHastatusPlayer);
}

TEST(damage_from_nowhere_runs_no_onkill) {
  // `record_score` and `record_army_attacked` are `hit`-only for the same
  // reason and say so: a script's `Obj::Damage(n)` has no attacker. Here the
  // reason is sharper than a policy -- the hook's *receiver* is the killer, so
  // there is nobody to run it on.
  Fixture f;
  REQUIRE(f.session != nullptr);

  f.combat().apply_damage(f.world(), kHastatus, f.combat().health(kHastatus));
  f.session->advance(1, 400);

  CHECK(env_int(f.world(), "died") == 1);
  CHECK(env_int(f.world(), "killed") == 0);
}

// ---------------------------------------------------------------------------
// onenter
// ---------------------------------------------------------------------------

TEST(a_unit_added_to_a_garrison_runs_its_onenter_with_that_settlement) {
  // The argument is the settlement, not the holder: the original resolves the
  // handle at `[holder+0xc]`, and the script's parameter is typed
  // `Settlement sett` and reads `sett.gold` off it.
  //
  // **This cannot tell the settlement from the holder, and that is a fact
  // about the engine rather than a gap in the test.**
  // `SettlementStore::for_object` maps all four handles of a composite to one
  // row, so a script handed the holder would read the same `gold`. The
  // settlement's own handle is passed because that is what the original
  // resolves; nothing in the corpus can observe the difference.
  Fixture f;
  REQUIRE(f.session != nullptr);
  EconomySystem* economy = economy_of(f.world());
  REQUIRE(economy != nullptr);
  const SettlementId town = make_town(f, *economy, /*max_units=*/10, /*gold=*/4242);

  // Driven through the entry point rather than through `garrison_add`, because
  // that is the distinction the fire is placed on: an entry is an event, and a
  // roster that gains a member some other way -- a save being restored -- is
  // not one.
  REQUIRE(add_unit(f, town_object(*economy, town), kHastatus));

  f.session->advance(1, 400);
  CHECK(env_int(f.world(), "entered") == 1);
  CHECK(env_int(f.world(), "enteredgold") == 4242);
  // On the unit, not on the settlement: `Unit` is what binds `onenter`, and a
  // settlement object binds nothing at all.
  CHECK(env_int(f.world(), "died") == 0);
}

TEST(a_garrison_restored_rather_than_entered_runs_nothing) {
  // `garrison_add` is the roster mutation and carries no hook. This is the case
  // that decided where the fire goes: `UNIT_ON_ENTER.VS` converts the unit's
  // Spoils of War into settlement gold, and a load that replayed every
  // garrison membership as an entry would pay every one of them again.
  Fixture f;
  REQUIRE(f.session != nullptr);
  EconomySystem* economy = economy_of(f.world());
  REQUIRE(economy != nullptr);
  const SettlementId town = make_town(f, *economy, /*max_units=*/10, /*gold=*/7);

  REQUIRE(economy->garrison_add(town, kHookless));
  f.session->advance(1, 400);
  CHECK(env_int(f.world(), "entered") == 0);
}

TEST(the_latch_governs_only_ondie) {
  // `0x005141b0` is the `ondie` routine and the latch is *its* re-entry guard.
  // `onkill` and `onenter` are fired from two other routines that have none, so
  // one object can legitimately run either of them many times -- a soldier that
  // kills twice books two kills, and a unit that leaves a garrison and comes
  // back enters twice. A latch over the whole family would silently make every
  // one of those the first one only.
  Fixture f;
  REQUIRE(f.session != nullptr);
  EconomySystem* economy = economy_of(f.world());
  REQUIRE(economy != nullptr);
  const SettlementId town = make_town(f, *economy, /*max_units=*/10, /*gold=*/7);

  REQUIRE(add_unit(f, town_object(*economy, town), kHastatus));
  REQUIRE(economy->garrison_remove(town, kHastatus));
  REQUIRE(add_unit(f, town_object(*economy, town), kHastatus));

  f.session->advance(1, 400);
  CHECK(env_int(f.world(), "entered") == 2);
}

TEST(a_full_garrison_refuses_the_unit_and_fires_nothing) {
  // `0x005d3e10` calls the holder's own add and returns without touching the
  // holder handle, the position or the hook when it comes back zero. Firing
  // first and asking afterwards would pay `UNIT_ON_ENTER.VS`'s Spoils of War
  // conversion for a unit that never got in.
  Fixture f;
  REQUIRE(f.session != nullptr);
  EconomySystem* economy = economy_of(f.world());
  REQUIRE(economy != nullptr);
  const SettlementId town = make_town(f, *economy, /*max_units=*/0, /*gold=*/7);

  CHECK(!add_unit(f, town_object(*economy, town), kHastatus));
  f.session->advance(1, 400);
  CHECK(env_int(f.world(), "entered") == 0);
}

TEST(a_save_taken_after_a_sleeping_hook_reloads_when_its_class_has_no_instances_left) {
  // This used to be a save taken *while* a hook slept, and the concern was
  // that nothing but the save itself named `ondie.vs` for the load to compile.
  // A hook runs to completion now, and one that sleeps is abandoned there (see
  // above), so no hook coroutine survives into any save -- which is the
  // stronger form of the same guarantee, kept as a round trip because
  // `GameSession::prime_library` and `Scheduler::deserialize`'s refusal of an
  // unknown name are exactly what would catch a regression to spawning.
  const auto build = [](script::HostRegistry& registry, ClassGraph& graph, Scripts& scripts) {
    SessionInputs inputs;
    inputs.classes = &graph;
    inputs.scripts = &scripts;
    inputs.map_objects = bytes_of(kMapDocument);
    return GameSession::create(registry, inputs, /*seed=*/1);
  };

  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = hook_graph();
  Scripts scripts;
  add_hook_scripts(scripts);
  // Long enough that the hook is still suspended when the save is taken.
  scripts.add("ondie.vs",
              "//void, Obj this\n"
              "Sleep(100000);\n"
              "EnvWriteInt(\"died\", EnvReadInt(\"died\") + 1);\n");

  imperivm::core::Result<std::unique_ptr<GameSession>> made = build(registry, graph, scripts);
  REQUIRE(made.ok());
  GameSession& run = *made.value();

  // Both `Military` leaves, so nothing left in the world binds `ondie`.
  REQUIRE(erase(registry, run, kHastatus));
  REQUIRE(erase(registry, run, kWarrior));
  run.advance(1, 400);
  CHECK(run.world().find(kHastatus) == nullptr);
  CHECK(run.world().find(kWarrior) == nullptr);

  const imperivm::core::Result<std::vector<std::byte>> bytes = run.save();
  REQUIRE(bytes.ok());

  imperivm::core::Result<std::unique_ptr<GameSession>> reloaded = build(registry, graph, scripts);
  REQUIRE(reloaded.ok());
  CHECK(reloaded.value()->load(bytes.value()).ok());
}

// ---------------------------------------------------------------------------
// the death order: the hook runs, then the detaches
// ---------------------------------------------------------------------------

TEST(a_hireling_killed_with_the_pact_researched_pays_its_hero_in_the_same_instant) {
  // `CMERCENARY_ONDIE.VS` reads `.hero` (0x005d7380, `[unit+0x170]`) and pays
  // `MercenaryPactGold` into it. Two orders get this wrong and this catches
  // both: a detach that ran before the hook leaves `.hero` empty and pays
  // nothing, and a hook deferred to the scheduler pays after the death rather
  // than inside it.
  Fixture f;
  REQUIRE(f.session != nullptr);
  const PactScene scene = hire(f);
  REQUIRE(f.heroes().hero_of(scene.hireling) == scene.leader);
  REQUIRE(purse_of(f, scene.leader) == 0);

  f.combat().apply_damage(f.world(), scene.hireling, f.combat().health(scene.hireling));
  REQUIRE(f.combat().is_dying(scene.hireling));
  CHECK(purse_of(f, scene.leader) == kPactGold);
  // And the detach did come after it, in the same instant.
  CHECK(f.heroes().hero_of(scene.hireling) == kNoObject);
  CHECK(f.heroes().army_size(scene.leader) == 0);

  f.session->advance(1, 400);
  CHECK(purse_of(f, scene.leader) == kPactGold);  // paid once
}

TEST(a_unit_a_script_kills_loses_its_scripts_before_they_run_again_that_pass) {
  // The turn's reap covers what combat killed; this is what a *script* kills.
  // `WALL_PATROL.VS` puts its sentries down with `Damage(10000)` when its wall
  // is very broken or changes hands, and a sentry in `SENTRY_GUARD.VS` whose
  // target was still valid and in range then ran its instruction budget out:
  // its `Attack` was refused, it being a corpse, and a refused `Attack` does
  // not suspend. The scheduler's step hook reaps after the killing slice, so
  // the corpse's own coroutine never resumes -- not even later in that pass.
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = hook_graph();
  Scripts scripts;
  add_hook_scripts(scripts);
  // The killer wears `onkill.vs`'s name and the victim's own script
  // `onenter.vs`'s: the library compiles what the class tree binds, and
  // neither hook fires here (a script's `Damage` has no killer).
  scripts.add("onkill.vs",
              "//void, Obj victim\n"
              "Sleep(1000);\n"
              "victim.Damage(10000);\n");
  scripts.add("onenter.vs",
              "//void, Obj this\n"
              "Sleep(1000);\n"
              "EnvWriteInt(\"woke\", EnvReadInt(\"woke\") + 1);\n");

  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.map_objects = bytes_of(kMapDocument);
  imperivm::core::Result<std::unique_ptr<GameSession>> made =
      GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(made.ok());
  GameSession& run = *made.value();

  REQUIRE(run.host_context().library != nullptr);
  const std::uint32_t killer = run.host_context().library->chunk_for("onkill.vs");
  const std::uint32_t own = run.host_context().library->chunk_for("onenter.vs");
  REQUIRE(killer != script::kNoChunk);
  REQUIRE(own != script::kNoChunk);
  const script::Value victim = handle(kHastatus);
  // Spawned first, so it runs first in the pass both wake in.
  REQUIRE(run.scheduler().spawn(killer, std::span<const script::Value>{&victim, 1}) !=
          script::kNoScript);
  REQUIRE(run.scheduler().spawn(own, std::span<const script::Value>{&victim, 1},
                                script::ObjectRef{kTypeObj, kHastatus}) != script::kNoScript);
  // A peer owned by somebody else, waking in the same pass, is untouched.
  const script::Value other = handle(kHookless);
  REQUIRE(run.scheduler().spawn(own, std::span<const script::Value>{&other, 1},
                                script::ObjectRef{kTypeObj, kHookless}) != script::kNoScript);

  run.advance(4, 400);
  CombatSystem& combat = *combat_system_of(run.world());
  CHECK(combat.is_dying(kHastatus));
  CHECK(env_int(run.world(), "died") == 1);
  CHECK(env_int(run.world(), "woke") == 1);  // the peer's, and not the corpse's
  CHECK(!combat.has_fresh_deaths());
}

TEST(erasing_a_corpse_in_its_dying_window_runs_no_second_hook_and_detaches_nothing_more) {
  // 0x005db230 runs 0x005141b0 again, which the latch refuses, and then the
  // same detaches over a unit they already emptied.
  Fixture f;
  REQUIRE(f.session != nullptr);
  const PactScene scene = hire(f);
  const SquadKey leader_squad = f.heroes().squad_of(scene.leader);
  REQUIRE(leader_squad.valid());

  f.combat().apply_damage(f.world(), scene.hireling, f.combat().health(scene.hireling));
  REQUIRE(purse_of(f, scene.leader) == kPactGold);
  REQUIRE(f.world().find(scene.hireling) != nullptr);  // a corpse

  REQUIRE(erase(f, scene.hireling));
  CHECK(f.world().find(scene.hireling) == nullptr);
  CHECK(purse_of(f, scene.leader) == kPactGold);
  // The leader is untouched: still its own squad's only member.
  CHECK(f.heroes().squad_of(scene.leader) == leader_squad);
  const Squad* squad = f.heroes().squads().find(leader_squad);
  REQUIRE(squad != nullptr);
  CHECK(squad->size() == 1);
  CHECK(f.heroes().squad_of(scene.hireling) == kNoSquad);
}

TEST(erasing_a_live_hireling_runs_its_hook_while_it_still_has_a_hero) {
  // The erase path's order: the hook first, then the detaches. The hook reads
  // `.hero`, so a detach ahead of it would pay nothing.
  Fixture f;
  REQUIRE(f.session != nullptr);
  const PactScene scene = hire(f);

  REQUIRE(erase(f, scene.hireling));
  CHECK(purse_of(f, scene.leader) == kPactGold);
  CHECK(f.heroes().hero_of(scene.hireling) == kNoObject);
  CHECK(f.heroes().army_size(scene.leader) == 0);
}

TEST(a_hook_fired_from_inside_a_scripts_host_call_runs_before_the_call_returns) {
  // The original's interpreter saves and restores its current-context globals
  // so that 0x0069dca0 can run a hook *inside* a host call of a script that is
  // itself running. Here a script kills the hireling with `Obj::Damage` and
  // reads the leader's purse on the very next statement: the hook has to have
  // run, whole, in between -- and the outer script has to carry on afterwards
  // as though nothing had been nested inside it.
  Fixture f;
  REQUIRE(f.session != nullptr);
  const PactScene scene = hire(f);

  script::Scheduler& scheduler = f.session->scheduler();
  REQUIRE(f.session->host_context().library != nullptr);
  f.scripts.add("killer.vs",
                "//void, Obj victim, Obj boss\n"
                "Item purse;\n"
                "victim.Damage(victim.health);\n"
                "purse = boss.AsHero.FindItem(\"Gold\");\n"
                "EnvWriteInt(\"seen\", purse.use_count);\n"
                "EnvWriteInt(\"after\", 1);\n");
  const std::uint32_t chunk = f.session->host_context().library->chunk_for("killer.vs");
  REQUIRE(chunk != script::kNoChunk);
  const script::Value args[] = {handle(scene.hireling), handle(scene.leader)};
  const script::ScriptId outer = scheduler.spawn(chunk, args);
  REQUIRE(outer != script::kNoScript);

  f.session->advance(1, 400);
  CHECK(env_int(f.world(), "seen") == kPactGold);
  CHECK(env_int(f.world(), "after") == 1);
  CHECK(!scheduler.alive(outer));  // finished, and not trapped along the way
  CHECK(f.session->report().traps.empty());
}

TEST(onenter_runs_inside_the_call_that_garrisons_the_unit) {
  // `0x0059b330` reaches 0x006a0360 like `ondie` does, so the entry hook has
  // run before `AddUnit` returns to the script that called it.
  Fixture f;
  REQUIRE(f.session != nullptr);
  EconomySystem* economy = economy_of(f.world());
  REQUIRE(economy != nullptr);
  const SettlementId town = make_town(f, *economy, /*max_units=*/10, /*gold=*/99);
  REQUIRE(add_unit(f, town_object(*economy, town), kHastatus));
  CHECK(env_int(f.world(), "entered") == 1);
  CHECK(env_int(f.world(), "enteredgold") == 99);
}

TEST(onkill_runs_inside_the_turn_that_lands_the_blow) {
  // And `0x0059b1c0` likewise: the killer's hook has run when the systems
  // hand back, with no scheduler pass at all -- only the world is advanced.
  Fixture f;
  REQUIRE(f.session != nullptr);
  REQUIRE(f.combat().order_attack(kWarrior, kHastatus));
  for (int i = 0; i < 40 && f.combat().is_alive(kHastatus); ++i) f.world().advance(400);
  REQUIRE(!f.combat().is_alive(kHastatus));
  CHECK(env_int(f.world(), "killed") == 1);
  CHECK(env_int(f.world(), "killerplayer") == kWarriorPlayer);
  CHECK(env_int(f.world(), "died") == 1);
}

TEST(a_hook_s_pooled_objlist_goes_with_it) {
  // `MILITARY_ONDIE.VS` builds an `ObjList` from `ObjsInCircle`, and a pooled
  // list is keyed by the script that made it. The synchronous call has a real
  // id and fires the teardown for it before it returns -- without that the
  // entry would belong to a script that no longer exists, which the sweep
  // leaves alone by design, and the pool would grow by one list per death.
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = hook_graph();
  Scripts scripts;
  add_hook_scripts(scripts);
  scripts.add("ondie.vs",
              "//void, Obj this\n"
              "ObjList around;\n"
              "around = ObjsInCircle(.pos, 500, \"Unit\").GetObjList();\n"
              "EnvWriteInt(\"around\", around.count);\n");
  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.map_objects = bytes_of(kMapDocument);
  imperivm::core::Result<std::unique_ptr<GameSession>> made =
      GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(made.ok());
  GameSession& run = *made.value();

  const std::size_t pooled = objlist_pool_of(run.world()).size();
  CombatSystem& combat = *combat_system_of(run.world());
  combat.apply_damage(run.world(), kHastatus, combat.health(kHastatus));
  REQUIRE(env_int(run.world(), "around") > 0);  // the list was really built
  CHECK(objlist_pool_of(run.world()).size() == pooled);
}

TEST(a_hook_that_traps_is_reported_like_any_other_script) {
  // A hook is no longer a record the scheduler's pass collects traps from, so
  // the session tallies it itself; a hook that fails silently would be a gap
  // nobody could see.
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = hook_graph();
  Scripts scripts;
  add_hook_scripts(scripts);
  scripts.add("ondie.vs",
              "//void, Obj this\n"
              "int zero;\n"
              "EnvWriteInt(\"died\", 1 / zero);\n");
  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.map_objects = bytes_of(kMapDocument);
  imperivm::core::Result<std::unique_ptr<GameSession>> made =
      GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(made.ok());
  GameSession& run = *made.value();

  CHECK(run.report().traps.empty());
  CombatSystem& combat = *combat_system_of(run.world());
  combat.apply_damage(run.world(), kHastatus, combat.health(kHastatus));
  const SessionReport report = run.report();
  REQUIRE(report.traps.size() == 1);
  CHECK(report.traps.front().message.find("ondie.vs") != std::string::npos);
}

TEST(a_hook_runs_to_its_end_past_what_one_slice_allows) {
  // The synchronous launcher hands the interpreter `0x0fffffff`, not the
  // scheduler slice's 15000: a hook is not cut off where a coroutine would be
  // suspended, because it has nowhere to be resumed from.
  script::HostRegistry registry;
  register_all_hosts(registry);
  ClassGraph graph = hook_graph();
  Scripts scripts;
  add_hook_scripts(scripts);
  scripts.add("ondie.vs",
              "//void, Obj this\n"
              "int i;\n"
              "int sum;\n"
              "for (i = 0; i < 20000; i += 1) sum += 1;\n"
              "EnvWriteInt(\"sum\", sum);\n");
  SessionInputs inputs;
  inputs.classes = &graph;
  inputs.scripts = &scripts;
  inputs.map_objects = bytes_of(kMapDocument);
  imperivm::core::Result<std::unique_ptr<GameSession>> made =
      GameSession::create(registry, inputs, /*seed=*/1);
  REQUIRE(made.ok());
  GameSession& run = *made.value();

  CombatSystem& combat = *combat_system_of(run.world());
  combat.apply_damage(run.world(), kHastatus, combat.health(kHastatus));
  CHECK(env_int(run.world(), "sum") == 20000);
  CHECK(run.report().traps.empty());
}
