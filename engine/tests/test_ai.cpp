// The AI bootstrap: sim/ai.hpp.
//
// Four of these cases are about a single question -- *is the AI actually
// started, and does everything it starts know whose AI it is* -- because that
// is the whole of what was missing. `MAIN.VS` is six `AIRun` calls and a
// return, and the six monitors it starts each ask `AIGetPlayer` before they do
// anything at all. Four of the six `Sleep` first, so by the time they ask,
// `Main.vs` has finished and been compacted out of the scheduler: the parent
// link they would resolve through is gone. `ai_get_player_survives_a_dead_main`
// pins that, and `ai_get_player_survives_a_prune` pins the other half of it:
// that a prune of the ownership table never throws a root away. Both were
// checked by breaking them on purpose.
//
// The rest fence the two refusals that matter -- an unknown profile is refused
// the way `gbr.exe` refuses it, and this domain's registration overwrites
// nothing another domain already implemented.

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/script/ast.hpp"
#include "imperivm/core/script/bytecode.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/array.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/gaika.hpp"
#include "imperivm/core/sim/gaika_table.hpp"
#include "imperivm/core/sim/tick.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "domains.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;

namespace script = imperivm::core::script;

namespace {

/// The smallest `AI.INI` that declares what these tests read: one economy
/// script, one tactic script, and the `[Scripts]` table that names their files.
constexpr std::string_view kProfileIni = R"([SquadStates]
SS_Approach

[GAIKAStrat]
GS_Guard

[EconomyScripts]
ES_Stronghold
ES_Village

[TacticScripts]
TS_AttackAtWill

[Scripts]
Main.vs = void
GetEconomyScript.vs = int, Settlement set, int idPlayer
ES_Stronghold.vs = void, Settlement set
ES_Village.vs = void, Settlement set

[Vars.All]
AIV_Sleep_ES = 3000
)";

[[nodiscard]] std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Parse and compile one script into `scheduler`, printing why if it fails.
/// A silently unbuilt chunk would make every case below pass by never running.
std::uint32_t build(script::Scheduler& scheduler, const script::HostRegistry& registry,
                    std::string_view source, const char* name) {
  script::Diagnostic diagnostic;
  const auto parsed = script::parse(bytes_of(source), name, &diagnostic);
  if (!parsed.ok()) {
    std::printf("  parse %s:%u: %.*s\n", name, diagnostic.line,
                static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
    return script::kNoChunk;
  }
  script::CompileError error;
  auto chunk = script::compile(parsed.value(), &registry, &error);
  if (!chunk.ok()) {
    std::printf("  compile %s:%u: %s\n", name, error.line, error.message.c_str());
    return script::kNoChunk;
  }
  return scheduler.add_chunk(std::move(chunk.value()));
}

/// A world with an economy and an AI on it, one registry with the whole host
/// surface, and a scheduler wired to both.
struct Fixture {
  World world;
  EconomySystem economy;
  HeroSystem heroes;
  // The `AreaAI*` writers resolve their first argument through `area_named`,
  // which needs the table this system owns. Nothing else in this file does.
  AreaSystem areas;
  AiSystem ai;
  script::HostRegistry registry;
  HostContext context;
  WorldHost host{world};
  script::Scheduler scheduler;
  AiProfile profile;

  Fixture() {
    world.add_system(&economy);
    // `Hero::TSAdvHeroSkills` runs a script that reads and writes a hero's
    // skills, so this domain's one hero-shaped entry point needs the system
    // that owns them. Nothing else in this file does.
    world.add_system(&heroes);
    world.add_system(&areas);
    world.add_system(&ai);

    (void)register_all_hosts(registry);
    // Not yet in `host_domains()` -- that manifest is not this file's. Running
    // it *after* the full surface is also the collision guard: see
    // `ai_registration_overwrites_nothing`.
    (void)register_ai_host(registry);

    context.world = &world;
    context.object_type = kTypeObj;
    scheduler.set_registry(&registry);
    scheduler.set_host(&host);
    scheduler.set_user(&context);

    // The pooled-collection teardown, which a real session installs and which
    // `run_script_now` runs by hand for its synthetic id. Without it a
    // synchronously-called script's `ObjList`s would outlive it here and the
    // recruiter cases below would be measuring the wrong pool.
    install_objlist_teardown(scheduler);

    const auto parsed = AiProfile::parse(bytes_of(kProfileIni));
    CHECK(parsed.ok());
    if (parsed.ok()) profile = parsed.value();
    ai.add_profile("", &profile);
  }

  void make_computer(PlayerId player, std::string_view ai_name = {},
                     std::int32_t difficulty = 0) {
    PlayerSetup& setup = world.players().setup(player);
    setup.control = PlayerControl::computer;
    setup.ai_script = std::string(ai_name);
    setup.difficulty = difficulty;
  }

  /// A settlement with a real object behind it, because the handle a script
  /// carries is `(kTypeSettlement, settlement object id)` and a settlement with
  /// no object resolves to nothing.
  SettlementId make_settlement(PlayerId owner) {
    const ObjectId object = world.spawn(imperivm::core::NativeClass::building, nullptr);
    SettlementInit init;
    init.settlement_object = object;
    init.anchor = object;
    init.owner = owner;
    init.kind = SettlementKind::stronghold;
    init.can_be_captured = true;
    init.can_be_attacked = true;
    return economy.settlements().create(init);
  }

  /// A hero owned by `owner`, alive, with `points` unspent skill points:
  /// the balance derives from the level, one point per level, over the
  /// skills the record offers.
  ObjectId make_hero(PlayerId owner, std::int32_t points) {
    const ObjectId id = world.spawn(imperivm::core::NativeClass::hero, nullptr);
    world.set_owner(id, owner);
    world.set_health(id, 1000);
    HeroRecord& record = heroes.register_hero(world, id);
    for (bool& offered : record.offered) offered = true;
    (void)heroes.set_level(id, points);
    return id;
  }

  script::Value settlement_value(SettlementId id) {
    const Settlement* set = economy.settlements().find(id);
    CHECK(set != nullptr);
    if (set == nullptr) return script::Value::object(script::ObjectRef{});
    return script::Value::object(kTypeSettlement, set->object);
  }

  /// Call one entry point with an explicit running-script id, which is what
  /// `AIGetPlayer` and everything downstream of it resolve through.
  script::HostOutcome call(script::CallKind kind, std::string_view name,
                           std::vector<script::Value> args, script::ScriptId from) {
    const std::uint16_t arity =
        static_cast<std::uint16_t>(args.size() - (kind == script::CallKind::member ? 1 : 0));
    const std::uint32_t index = registry.find(kind, name, arity);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostEntry& entry = registry.entry(index);
    CHECK(entry.fn != nullptr);
    if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.host = &host;
    ctx.scheduler = &scheduler;
    ctx.script = from;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    return entry.fn(ctx);
  }
};

}  // namespace

// --------------------------------------------------------------------------
// AIStart, as gbr.exe defines it
// --------------------------------------------------------------------------

TEST(ai_start_runs_main_vs_and_nothing_else) {
  Fixture f;
  const std::uint32_t main_chunk = build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS");
  REQUIRE(main_chunk != script::kNoChunk);

  CHECK(f.ai.active_count() == 0);
  CHECK(f.ai.start(2, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);

  // `CVXAI::Start` runs exactly one script. One coroutine, and it is `Main.vs`.
  CHECK(f.scheduler.live_count() == 1);
  const AiPlayer* ai = f.ai.player_ai(2);
  REQUIRE(ai != nullptr);
  CHECK(ai->active);
  CHECK(ai->root != script::kNoScript);
  CHECK(ai->difficulty == AiDifficulty::normal);
  CHECK(f.ai.active_count() == 1);

  const script::ScriptRecord* record = f.scheduler.find(ai->root);
  REQUIRE(record != nullptr);
  CHECK(f.scheduler.chunk(record->chunk_index).source_name == "DATA/AI/MAIN.VS");
  // The root of a player's AI has no parent: it is the thing everything else
  // resolves *to*.
  CHECK(record->parent == script::kNoScript);
}

/// **The pathfinder bootstrap has nothing to build here: every `_Quant` answers
/// false and every stage finishes at once**, which is the script's shape run
/// through in one pass. Each refuses without a world, as the domain does.
TEST(the_pathfinder_bootstrap_has_nothing_left_to_build) {
  Fixture f;
  const char* quants[] = {"SPFFindAreas_Quant", "SPFCalcConnectHighRes_Quant",
                          "SPFIncreaseConnect_Quant"};
  const char* steps[] = {"SPFInitData",        "SPFInitDirectionData", "SPFInitPointToAreaData",
                         "SPFInitConnectData", "SPFIncreaseConnect",   "SPFDone"};
  const auto call = [&](const char* name, bool with_world) -> script::HostOutcome {
    const std::uint32_t index = f.registry.find(script::CallKind::free_function, name, 0);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    script::CallContext ctx;
    ctx.user = with_world ? &f.context : nullptr;
    ctx.name = name;
    ctx.kind = script::CallKind::free_function;
    return f.registry.entry(index).fn(ctx);
  };
  for (const char* name : quants) {
    const script::HostOutcome out = call(name, true);
    CHECK(out.status == script::HostStatus::ok);
    CHECK(out.value.is_integer() && out.value.as_integer() == 0);
    CHECK(call(name, false).status != script::HostStatus::ok);
  }
  for (const char* name : steps) {
    CHECK(call(name, true).status == script::HostStatus::ok);
    CHECK(call(name, false).status != script::HostStatus::ok);
  }
}

/// **`AIStop` through the host is `stop` for the script's player number, and
/// an out-of-range number is refused quietly rather than read off the end of
/// the table.**
TEST(ai_stop_host_kills_the_players_ai_and_refuses_a_bad_number_quietly) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  REQUIRE(f.ai.start(2, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  CHECK(f.ai.active_count() == 1);
  CHECK(f.scheduler.live_count() == 1);

  const auto stop = [&](script::Value who, bool with_scheduler) -> script::HostOutcome {
    const std::uint32_t index = f.registry.find(script::CallKind::free_function, "AIStop", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    std::vector<script::Value> args{who};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &f.context;
    ctx.scheduler = with_scheduler ? &f.scheduler : nullptr;
    ctx.name = "AIStop";
    ctx.kind = script::CallKind::free_function;
    return f.registry.entry(index).fn(ctx);
  };
  // Player numbers are 1-based on the script side: `AIStop(3)` is player 2.
  CHECK(stop(script::Value::integer(4), true).status == script::HostStatus::ok);
  CHECK(f.ai.active_count() == 1);  // nobody's AI, nothing stopped
  CHECK(stop(script::Value::integer(0), true).status == script::HostStatus::ok);
  CHECK(stop(script::Value::integer(17), true).status == script::HostStatus::ok);
  CHECK(f.ai.active_count() == 1);
  CHECK(stop(script::Value::integer(3), true).status == script::HostStatus::ok);
  CHECK(f.ai.active_count() == 0);
  CHECK(f.scheduler.live_count() == 0);
  // Without a scheduler there is nothing to kill: a refusal, as `AIStart`.
  CHECK(stop(script::Value::integer(3), false).status != script::HostStatus::ok);
}

TEST(ai_start_refuses_a_profile_it_was_never_given) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);

  // `AIStart: invalid AI profile`, and the AI is *not* started -- 0x00435085
  // jumps straight to the error print without touching the core.
  CHECK(f.ai.start(0, "CHAOTIC", AiDifficulty::easy, f.scheduler) ==
        AiStartStatus::unknown_profile);
  CHECK(f.ai.active_count() == 0);
  CHECK(f.scheduler.live_count() == 0);

  // The name is uppercased before the lookup, so the root profile answers to
  // any casing of its (empty) name and a registered one answers to any casing.
  f.ai.add_profile("Defensive", &f.profile);
  CHECK(f.ai.has_profile("DEFENSIVE"));
  CHECK(f.ai.has_profile("defensive"));
  CHECK(f.ai.start(0, "defensive", AiDifficulty::easy, f.scheduler) == AiStartStatus::ok);
}

TEST(ai_start_twice_restarts_rather_than_stacking) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);

  CHECK(f.ai.start(1, "", AiDifficulty::easy, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId first = f.ai.player_ai(1)->root;
  CHECK(f.ai.start(1, "", AiDifficulty::hard, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId second = f.ai.player_ai(1)->root;

  // The core destroys the previous AI object before allocating the new one.
  CHECK(first != second);
  CHECK(!f.scheduler.alive(first));
  CHECK(f.scheduler.alive(second));
  CHECK(f.ai.player_ai(1)->difficulty == AiDifficulty::hard);
  CHECK(f.ai.active_count() == 1);
}

TEST(ai_stop_kills_everything_under_the_player) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  const std::uint32_t monitor =
      build(f.scheduler, f.registry, "// void\nwhile (1) Sleep(1000);\n", "DATA/AI/MON.VS");
  REQUIRE(monitor != script::kNoChunk);

  CHECK(f.ai.start(3, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(3)->root;
  const script::ScriptId child = f.scheduler.spawn(monitor, {}, script::ObjectRef{}, root);
  CHECK(f.ai.script_player(child, f.scheduler) == 4);  // 1-based, as AIGetPlayer returns

  CHECK(f.ai.stop(3, f.scheduler));
  CHECK(!f.scheduler.alive(root));
  CHECK(!f.scheduler.alive(child));
  CHECK(!f.ai.player_ai(3)->active);
  CHECK(!f.ai.stop(3, f.scheduler));  // idempotent: nothing to stop twice
}

// --------------------------------------------------------------------------
// AIGetPlayer
// --------------------------------------------------------------------------

TEST(ai_get_player_survives_a_dead_main) {
  // The case the design turns on. `Main.vs` spawns its monitors and returns on
  // the same tick; `GAIKAMonitor`, `EconomyMonitor`, `TacticMonitor` and
  // `SquadMonitor` all `Sleep` before their first `AIGetPlayer`, by which time
  // the parent record is gone from the scheduler. Not recording the root in
  // `AiSystem::start`, or looking the parent up in the scheduler before this
  // table, makes this fail -- the first of those was introduced deliberately to
  // check that it does. The prune's own root exemption is fenced separately, by
  // `ai_get_player_survives_a_prune`.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry,
                "// void\nAIRun('DATA/AI/MON.VS');\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(500);\nSleep(100000);\n",
                "DATA/AI/MON.VS") != script::kNoChunk);

  CHECK(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(0)->root;

  // One pass: `Main.vs` spawns the monitor and finishes. The monitor sleeps
  // without asking anything, so nothing has attributed it yet.
  f.scheduler.run_ready();
  CHECK(!f.scheduler.alive(root));
  REQUIRE(f.scheduler.live_count() == 1);
  const script::ScriptId monitor = f.scheduler.scripts().front().id;
  CHECK(monitor != root);
  // The parent is genuinely unreachable: this is what makes the case a test.
  CHECK(f.scheduler.find(root) == nullptr);
  CHECK(f.scheduler.find(monitor)->parent == root);

  const script::HostOutcome out = f.call(script::CallKind::free_function, "AIGetPlayer", {},
                                         monitor);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.is_integer());
  CHECK(out.value.as_integer() == 1);  // player 0, 1-based
}

TEST(ai_get_player_survives_a_prune) {
  // `prune_owners` drops entries whose coroutine is gone, and **exempts the
  // roots**. Without that exemption the table is correct only until the 4,096th
  // resolution, at which point every monitor in the game silently starts
  // answering -1. Deleting the `owned.root ||` clause makes this case fail and
  // nothing else in the suite notice.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nAIRun('DATA/AI/MON.VS');\n",
                "DATA/AI/MAIN.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MON.VS") !=
          script::kNoChunk);

  CHECK(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  f.scheduler.run_ready();
  REQUIRE(f.scheduler.live_count() == 1);
  const script::ScriptId monitor = f.scheduler.scripts().front().id;

  // Past the prune threshold, several times over. Accumulated rather than
  // checked per iteration so the case costs the suite one assertion, not 20,000.
  bool always_one = true;
  for (int i = 0; i < 20000; ++i) {
    if (f.ai.script_player(monitor, f.scheduler) != 1) always_one = false;
  }
  CHECK(always_one);
  // A script the table has never seen still resolves through the root, which is
  // the thing a prune must not throw away.
  f.ai.forget(monitor);
  CHECK(f.ai.script_player(monitor, f.scheduler) == 1);
}

TEST(ai_get_player_answers_minus_one_outside_any_ai) {
  Fixture f;
  const std::uint32_t chunk =
      build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/SUBAI/IDLE.VS");
  REQUIRE(chunk != script::kNoChunk);
  const script::ScriptId loose = f.scheduler.spawn(chunk);

  const script::HostOutcome out =
      f.call(script::CallKind::free_function, "AIGetPlayer", {}, loose);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.as_integer() == -1);
  // And with no running script at all, which is the case 0x0043caa0 answers -1
  // for by name.
  CHECK(f.ai.script_player(script::kNoScript, f.scheduler) == -1);
}

TEST(ai_get_player_walks_a_chain_and_caches_it) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  const std::uint32_t chunk =
      build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/X.VS");
  REQUIRE(chunk != script::kNoChunk);

  CHECK(f.ai.start(5, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  script::ScriptId parent = f.ai.player_ai(5)->root;
  const std::size_t before = f.ai.owner_entries();
  for (int depth = 0; depth < 4; ++depth) {
    parent = f.scheduler.spawn(chunk, {}, script::ObjectRef{}, parent);
  }
  CHECK(f.ai.owner_entries() == before);  // nothing attributed until it is asked
  CHECK(f.ai.script_player(parent, f.scheduler) == 6);
  // The whole chain is adopted in one walk, not one level per question.
  CHECK(f.ai.owner_entries() == before + 4);
}

// --------------------------------------------------------------------------
// GetGAIKA
// --------------------------------------------------------------------------

TEST(get_gaika_is_the_identity) {
  // Not a simplification: `GetGAIKA` (arity 1) and `GAIKA::ID` share the
  // function pointer 0x00746ea0, whose body is `xor eax,eax; ret` -- it leaves
  // the VM stack untouched, so the argument *is* the result, with no bounds
  // check on either side.
  Fixture f;
  for (const std::int32_t id : {1, 2, 17, 4095}) {
    const script::HostOutcome out = f.call(script::CallKind::free_function, "GetGAIKA",
                                           {script::Value::integer(id)}, 1);
    CHECK(out.status == script::HostStatus::ok);
    CHECK(out.value.as_integer() == id);
  }
  // Zero is `kNoGaika` and stays zero, as does anything below it: `sim/gaika.hpp`
  // fixes that mapping and this entry point does not get to disagree with it.
  const script::HostOutcome zero =
      f.call(script::CallKind::free_function, "GetGAIKA", {script::Value::integer(0)}, 1);
  CHECK(zero.value.as_integer() == 0);
}

/// `obj.AI` -- 25 sites and 36,957 of the installation's 38,303 trap hits.
TEST(ai_is_a_computer_player_and_a_squad_that_has_not_been_pinned) {
  // 0x004254b0 asks two questions in order and this pins both, because either
  // one alone is a plausible reading of the name and each is wrong on its own.
  Fixture f;
  HeroSystem& heroes = f.heroes;

  const ObjectId soldier = f.world.spawn(imperivm::core::NativeClass::unit, nullptr);
  f.world.set_owner(soldier, 1);
  f.world.set_health(soldier, 200);
  const script::Value who = script::Value::object(kTypeObj, soldier);
  const auto ai_of = [&](script::Value v) {
    const script::HostOutcome out = f.call(script::CallKind::member, "AI", {v}, 0);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.as_integer() != 0;
  };

  // **A unit whose player has no AI is not AI-controlled**, whatever its squad
  // says. `[player+0x88]` is null for a human and 0x004254e6 pushes false.
  //
  // This is the check that caught the obvious mistranslation: `player_ai(p)`
  // indexes a dense array and is non-null for every player in range, so testing
  // it for null the way the original tests its pointer answers **true for every
  // unit on the map**. The slot always exists; `AiPlayer::active` is the AI.
  REQUIRE(f.ai.player_ai(1) != nullptr);
  REQUIRE(!f.ai.player_ai(1)->active);
  CHECK(!ai_of(who));

  f.make_computer(1);
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  REQUIRE(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  REQUIRE(f.ai.player_ai(1) != nullptr);
  CHECK(ai_of(who));

  // **And a unit in no squad is not pinned.** The original does not special-case
  // squad index 0: it indexes the vector at 0 and reads the reserved slot,
  // whose flags are zero. So step 2 alone decides this one.
  REQUIRE(heroes.squad_of(soldier) == kNoSquad);
  CHECK(ai_of(who));

  // Now give it a squad and pin it. `SF_NOAI` is bit 0 of the squad flags word,
  // and `AI` is its **negation** -- 0x0042550f is `notb` then `andb $1`.
  const ObjectId hero = f.world.spawn(imperivm::core::NativeClass::hero, nullptr);
  f.world.set_owner(hero, 1);
  f.world.set_health(hero, 1000);
  heroes.register_hero(f.world, hero);
  heroes.register_unit(f.world, soldier);
  REQUIRE(heroes.attach(f.world, soldier, hero));
  const SquadKey key = heroes.squad_of(soldier);
  REQUIRE(key != kNoSquad);
  Squad* squad = heroes.squads().find(key);
  REQUIRE(squad != nullptr);

  CHECK(ai_of(who));  // squad, but no flag
  squad->flags |= kSquadFlagNoAi;
  CHECK(!ai_of(who));
  // A *different* bit does not pin it. `SF_PEACEFUL` is 4, and reading the word
  // as "non-zero means pinned" would make every peaceful squad AI-less.
  squad->flags = 4;
  CHECK(ai_of(who));
  squad->flags = 4 | kSquadFlagNoAi;
  CHECK(!ai_of(who));

  // The hero shares the squad, so it is pinned too -- that is what makes a
  // squad flag a squad flag rather than a per-unit one.
  CHECK(!ai_of(script::Value::object(kTypeObj, hero)));

  // A handle to nothing is false, not a trap: `UNIT_IDLE.VS` runs this on every
  // pass and a dead receiver must not kill the script.
  CHECK(!ai_of(script::Value::object(kTypeObj, 4242)));
  CHECK(!ai_of(script::Value::integer(3)));

  // **And an object nobody owns is not AI-controlled.** Wildlife and map
  // garrisons are spawned unowned, and `UNIT_IDLE.VS` runs on them too. Two
  // things stand between here and a wrong answer -- the `kNoPlayer` guard in
  // `m_ai` and the bounds check inside `AiSystem::player_ai` -- and neither is
  // load-bearing alone, because `kNoPlayer` is 255 and the table holds 16. The
  // property is what matters, so the property is what is asserted.
  const ObjectId stray = f.world.spawn(imperivm::core::NativeClass::unit, nullptr);
  f.world.set_health(stray, 200);
  REQUIRE(f.world.state(stray)->owner == kNoPlayer);
  CHECK(!ai_of(script::Value::object(kTypeObj, stray)));
  CHECK(f.ai.player_ai(kNoPlayer) == nullptr);
}

// --------------------------------------------------------------------------
// the per-settlement script slots
// --------------------------------------------------------------------------

TEST(run_economy_script_spawns_the_file_the_profile_names) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, "// void, Settlement set\nSleep(100000);\n",
                "DATA/AI/ES_VILLAGE.VS") != script::kNoChunk);

  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const SettlementId set = f.make_settlement(1);
  const script::Value handle = f.settlement_value(set);

  // `ES_Village` is the second declared entry, and the sentinel is 0, so it is
  // 2. Reading it back off the profile rather than writing 2 here keeps the
  // case honest if the numbering ever changes.
  const std::int32_t es_village = f.profile.constant(AiEnum::economy_script, "ES_Village");
  CHECK(es_village == 2);

  CHECK(f.ai.economy_script(set, f.scheduler) == 0);  // ES_NONE before anything runs
  const script::HostOutcome ran =
      f.call(script::CallKind::member, "RunEconomyScript",
             {handle, script::Value::integer(es_village)}, root);
  CHECK(ran.status == script::HostStatus::ok);

  CHECK(f.scheduler.live_count() == 2);
  const script::HostOutcome slot =
      f.call(script::CallKind::member, "EconomyScript", {handle}, root);
  CHECK(slot.status == script::HostStatus::ok);
  CHECK(slot.value.as_integer() == es_village);

  // The spawned script got the settlement as its one parameter and is under
  // the same AI, which is what `ES_Village.vs = void, Settlement set` and its
  // own `AIGetPlayer` need.
  const script::ScriptId spawned = f.scheduler.scripts().back().id;
  CHECK(f.ai.script_player(spawned, f.scheduler) == 2);

  // ...and the slot clears itself when the script goes, so the monitor picks
  // the settlement up again on its next sweep.
  f.scheduler.kill(spawned);
  f.scheduler.run_ready();
  const script::HostOutcome cleared =
      f.call(script::CallKind::member, "EconomyScript", {handle}, root);
  CHECK(cleared.value.as_integer() == 0);
}

TEST(run_economy_script_refuses_an_id_the_profile_does_not_declare) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const script::Value handle = f.settlement_value(f.make_settlement(1));

  // 99 names nothing. Refusing beats spawning something plausible.
  const script::HostOutcome out = f.call(script::CallKind::member, "RunEconomyScript",
                                         {handle, script::Value::integer(99)}, root);
  CHECK(out.status == script::HostStatus::error);
  CHECK(f.scheduler.live_count() == 1);

  // The sentinel is a no-op rather than an error: `ES_NONE` means "nothing to
  // run", and the corpus passes whatever came back.
  const script::HostOutcome none = f.call(script::CallKind::member, "RunEconomyScript",
                                          {handle, script::Value::integer(0)}, root);
  CHECK(none.status == script::HostStatus::ok);
  CHECK(f.scheduler.live_count() == 1);
}

TEST(get_economy_script_runs_the_script_and_returns_its_value) {
  // `Settlement::GetEconomyScript` is one synchronous call into
  // `GetEconomyScript.vs`, whose declared return type is `int`. `AIRun` cannot
  // express that -- it spawns a peer and yields a handle -- so this is the
  // capability, not a convenience.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// int, Settlement set, int idPlayer\nif (set.IsStronghold) return "
                "ES_Stronghold;\nreturn ES_NONE;\n",
                "DATA/AI/GETECONOMYSCRIPT.VS") != script::kNoChunk);
  f.host.set_ai_profile(&f.profile);

  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const script::Value handle = f.settlement_value(f.make_settlement(1));

  const script::HostOutcome out =
      f.call(script::CallKind::member, "GetEconomyScript",
             {handle, script::Value::integer(2)}, root);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.is_integer());
  CHECK(out.value.as_integer() == f.profile.constant(AiEnum::economy_script, "ES_Stronghold"));
  // Synchronous: nothing was left behind in the scheduler.
  CHECK(f.scheduler.live_count() == 1);
}

TEST(get_economy_script_refuses_a_script_that_sleeps) {
  // A `Sleep` inside a synchronous call has nowhere to suspend to, and quietly
  // dropping the half-run `Execution` would leave its side effects applied.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// int, Settlement set, int idPlayer\nSleep(10);\nreturn 1;\n",
                "DATA/AI/GETECONOMYSCRIPT.VS") != script::kNoChunk);

  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const script::Value handle = f.settlement_value(f.make_settlement(1));

  const script::HostOutcome out =
      f.call(script::CallKind::member, "GetEconomyScript",
             {handle, script::Value::integer(2)}, root);
  CHECK(out.status == script::HostStatus::error);
}

// --------------------------------------------------------------------------
// Hero::TSAdvHeroSkills, the third trampoline
// --------------------------------------------------------------------------

/// **68 call sites and no native behaviour behind any of them.**
///
/// `Hero::TSAdvHeroSkills` (0x00426c00) is the same shape as
/// `GetEconomyScript`: resolve a `.vs` file on a player's AI search path, run
/// it synchronously against the arguments already on the stack. What is under
/// test here is that trampoline -- the file is found under the *hero's owner*
/// rather than the caller, the three arguments arrive as the helper's three
/// parameters, an array handle survives the crossing, and the writes the
/// callee makes land on the world.
///
/// **The helper built here is a stand-in, not the shipped one.** Its name and
/// signature are the shipped ones, because those are what the lookup and the
/// argument shuffle are made of; its body is written for this test. The real
/// `TSH_HEROSKILLS.VS` is game data and this project does not carry game data,
/// as a test fixture or otherwise -- what it does is described in prose beside
/// `m_ts_adv_hero_skills` and implemented by nothing here.
///
/// The second `SetSkill` writes `aSkills.size()` deliberately: that is the
/// call that decided whether this entry point could be bound at all. Until
/// `size/0` grew its array branch it answered 0 on an `IntArray`, the shipped
/// helper's own length guard would have fired on its first line, and binding
/// this name would have cleared 68 sites while no hero ever gained a skill.
TEST(ts_adv_hero_skills_runs_the_helper_under_the_heros_owner) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Hero hero, IntArray* aSkills, IntArray* aSkillLevels\n"
                "if (!hero.IsValid) return;\n"
                "if (aSkills.size() == 0) return;\n"
                "hero.SetSkill(aSkills[0], aSkillLevels[0]);\n"
                "hero.SetSkill(aSkills[1], aSkills.size());\n",
                "DATA/AI/TSH_HEROSKILLS.VS") != script::kNoChunk);

  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const ObjectId hero = f.make_hero(1, 10);

  const ScriptArrayId skills = f.world.arrays().acquire(root, 0, false);
  const ScriptArrayId levels = f.world.arrays().acquire(root, 1, false);
  CHECK(f.world.arrays().set(skills, 0, script::Value::integer(3)));
  CHECK(f.world.arrays().set(skills, 1, script::Value::integer(7)));
  CHECK(f.world.arrays().set(levels, 0, script::Value::integer(2)));

  const script::HostOutcome out =
      f.call(script::CallKind::member, "TSAdvHeroSkills",
             {script::Value::object(kTypeObj, hero), make_array_value(kTypeIntArray, skills),
              make_array_value(kTypeIntArray, levels)},
             root);
  CHECK(out.status == script::HostStatus::ok);
  // Skill 3 got `aSkillLevels[0]`; skill 7 got the array's length, which is 2
  // and would have been 0 before `size/0` learned about arrays.
  CHECK(f.heroes.skill(hero, 3) == 2);
  CHECK(f.heroes.skill(hero, 7) == 2);
  // Four points spent out of ten, by `HeroSystem::set_skill` rather than by
  // anything here -- the helper never touches the counter.
  CHECK(f.heroes.available_skill_points(hero) == 6);
  // Synchronous: `Main.vs` is still the only live script.
  CHECK(f.scheduler.live_count() == 1);
}

/// **The player is the hero's owner, not the caller's**, and this is the only
/// case that can tell the two apart.
///
/// The original reads the player off the hero's own player pointer;
/// `GetTacticScript` beside it reads the running script's. The difference is
/// invisible at every shipped site -- a tactic script only ever advances its
/// own player's heroes -- so it takes two profiles carrying two copies of the
/// same file to make it visible at all. It is still the original's rule, and a
/// reimplementation that quietly used the caller's would pick the wrong
/// personality's skill priorities for any hero handed between players.
TEST(ts_adv_hero_skills_uses_the_heros_profile_and_not_the_callers) {
  Fixture f;
  AiProfile defensive;
  const auto parsed = AiProfile::parse(bytes_of(kProfileIni));
  REQUIRE(parsed.ok());
  defensive = parsed.value();
  f.ai.add_profile("DEFENSIVE", &defensive, "DATA\\AI\\DEFENSIVE\\");

  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n",
                "DATA/AI/DEFENSIVE/MAIN.VS") != script::kNoChunk);
  // Two copies of one file, one per profile, differing only in what they write.
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Hero hero, IntArray* aSkills, IntArray* aSkillLevels\n"
                "hero.SetSkill(0, 1);\n",
                "DATA/AI/TSH_HEROSKILLS.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Hero hero, IntArray* aSkills, IntArray* aSkillLevels\n"
                "hero.SetSkill(0, 2);\n",
                "DATA/AI/DEFENSIVE/TSH_HEROSKILLS.VS") != script::kNoChunk);

  CHECK(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  CHECK(f.ai.start(1, "DEFENSIVE", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  // The caller is player 0's root script; the hero belongs to player 1.
  const script::ScriptId caller = f.ai.player_ai(0)->root;
  const ObjectId hero = f.make_hero(1, 5);
  const ScriptArrayId empty = f.world.arrays().acquire(caller, 0, false);

  const script::HostOutcome out =
      f.call(script::CallKind::member, "TSAdvHeroSkills",
             {script::Value::object(kTypeObj, hero), make_array_value(kTypeIntArray, empty),
              make_array_value(kTypeIntArray, empty)},
             caller);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(f.heroes.skill(hero, 0) == 2);  // the overlay's, not the root's

  // And the same call for a hero of player 0 takes the root copy, so the two
  // readings really do disagree here rather than the overlay simply winning.
  const ObjectId theirs = f.make_hero(0, 5);
  CHECK(f.call(script::CallKind::member, "TSAdvHeroSkills",
               {script::Value::object(kTypeObj, theirs), make_array_value(kTypeIntArray, empty),
                make_array_value(kTypeIntArray, empty)},
               caller)
            .status == script::HostStatus::ok);
  CHECK(f.heroes.skill(theirs, 0) == 1);
}

TEST(ts_adv_hero_skills_answers_a_receiver_that_names_nothing_without_running) {
  // The original prints `The function 'TSHeroSkills' called for an
  // uninitialized or invalid object.` and returns -- it does not run the
  // helper and it does not fail the caller. All 68 shipped sites are bare
  // statements behind `if (hero.IsAlive)`, so a refusal here would kill a
  // tactic script on the one pass its hero had just died.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Hero hero, IntArray* aSkills, IntArray* aSkillLevels\n"
                "hero.SetSkill(0, 5);\n",
                "DATA/AI/TSH_HEROSKILLS.VS") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const ScriptArrayId empty = f.world.arrays().acquire(root, 0, false);

  const script::HostOutcome out =
      f.call(script::CallKind::member, "TSAdvHeroSkills",
             {script::Value::integer(0), make_array_value(kTypeIntArray, empty),
              make_array_value(kTypeIntArray, empty)},
             root);
  CHECK(out.status == script::HostStatus::ok);

  // A handle that *is* an object but names no live one is a different case: it
  // reaches the helper, whose own validity guard answers it. That guard is the
  // shipped script's and not this engine's, so what is asserted is only that
  // the call completes.
  const script::HostOutcome stale =
      f.call(script::CallKind::member, "TSAdvHeroSkills",
             {script::Value::object(kTypeObj, ObjectId{0x7fffffff}),
              make_array_value(kTypeIntArray, empty), make_array_value(kTypeIntArray, empty)},
             root);
  CHECK(stale.status == script::HostStatus::ok);
}

TEST(ts_adv_hero_skills_refuses_when_the_helper_is_not_loaded) {
  // `run_script_now`'s rule, and `GetEconomyScript`'s: a named file that is not
  // in the scheduler is a broken installation rather than a stale handle, and
  // the name in the report is the name of the entry point that wanted it.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const ObjectId hero = f.make_hero(1, 4);
  const ScriptArrayId empty = f.world.arrays().acquire(root, 0, false);

  const script::HostOutcome out =
      f.call(script::CallKind::member, "TSAdvHeroSkills",
             {script::Value::object(kTypeObj, hero), make_array_value(kTypeIntArray, empty),
              make_array_value(kTypeIntArray, empty)},
             root);
  CHECK(out.status == script::HostStatus::error);
}

// --------------------------------------------------------------------------
// Hero::AIGetSkillToDevelop, a weighted draw over one script run per skill
// --------------------------------------------------------------------------

/// **One call site, and it is the sole blocker of `HERO_SKILL_BEHAVIOUR.VS`.**
///
/// `Hero::AIGetSkillToDevelop` (0x00426680) runs `HeroSkill <name>.vs` -- or
/// `HEROSKILL DEFAULT.VS` where no such file exists -- once per developable
/// skill on the hero's owner's AI search path, then draws one skill by the
/// weights those runs left behind. What is under test is the whole of that:
/// which skills get a run, what the run is handed, what of its answer counts,
/// how the draw is taken, and what comes back through the two out-parameters.
///
/// **The scripts built here are stand-ins.** The shipped default is game data
/// and this project does not carry game data; its one line of behaviour is
/// described beside `m_ai_get_skill_to_develop`. These share its declared
/// parameter list, because that list is the contract the read-back is made
/// of, and their bodies are written for the case at hand.
namespace {

constexpr const char* kSkillScriptHeader =
    "// void, Hero hero, int skill, int *skill_points, int *weight\n";

std::string skill_script(std::string_view body) {
  return std::string(kSkillScriptHeader) + std::string(body);
}

/// Call with the argument window exposed, which is how the two out-parameters
/// are read back. `Fixture::call` takes its vector by value and so cannot show
/// a write-through.
script::HostOutcome call_into(Fixture& f, std::string_view name, std::vector<script::Value>& args,
                              script::ScriptId from) {
  const std::uint32_t index = f.registry.find(script::CallKind::member, name,
                                              static_cast<std::uint16_t>(args.size() - 1));
  CHECK(index != script::kUnresolvedHost);
  if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
  const script::HostEntry& entry = f.registry.entry(index);
  CHECK(entry.fn != nullptr);
  if (entry.fn == nullptr) return script::HostOutcome::failed("not implemented");
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.host = &f.host;
  ctx.scheduler = &f.scheduler;
  ctx.script = from;
  ctx.user = &f.context;
  ctx.name = name;
  ctx.kind = script::CallKind::member;
  return entry.fn(ctx);
}

/// `hero.AIGetSkillToDevelop(skill, level)` with both out-parameters seeded to
/// a value the entry point must overwrite, so a body that wrote nothing would
/// be told apart from one that wrote -1.
struct SkillPick {
  script::HostStatus status = script::HostStatus::error;
  std::int32_t skill = 99;
  std::int32_t level = 99;
};

SkillPick pick_skill(Fixture& f, ObjectId hero, script::ScriptId from) {
  std::vector<script::Value> args = {script::Value::object(kTypeObj, hero),
                                     script::Value::integer(99), script::Value::integer(99)};
  SkillPick out;
  out.status = call_into(f, "AIGetSkillToDevelop", args, from).status;
  CHECK(args[1].is_integer());
  CHECK(args[2].is_integer());
  if (args[1].is_integer()) out.skill = args[1].as_integer();
  if (args[2].is_integer()) out.level = args[2].as_integer();
  return out;
}

}  // namespace

TEST(ai_get_skill_to_develop_runs_the_per_skill_file_and_the_default_for_the_rest) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  // The default holds every skill back; the one per-skill file that exists
  // names a goal and keeps its weight of 100. Battle cry is skill 8, and the
  // file is found under the name the executable formats: `HeroSkill %s.vs`
  // with the `SKILLS.INI` section name.
  REQUIRE(build(f.scheduler, f.registry, skill_script("weight = 0;\n").c_str(),
                "DATA/AI/HEROSKILL DEFAULT.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, skill_script("skill_points = 3;\n").c_str(),
                "DATA/AI/HeroSkill Battle cry.vs") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const ObjectId hero = f.make_hero(1, 10);

  // One draw and exactly one: `rand(1, 100)` on a total of 100.
  Rng model = f.world.rng();
  (void)model.between(1, 100);

  const SkillPick pick = pick_skill(f, hero, root);
  CHECK(pick.status == script::HostStatus::ok);
  CHECK(pick.skill == 8);
  CHECK(pick.level == 3);
  CHECK(f.world.rng().state() == model.state());
  // Synchronous, all twenty-five of them: `Main.vs` is still the only live script.
  CHECK(f.scheduler.live_count() == 1);
  // And nothing here spends: the script names a goal, the caller pays for it.
  CHECK(f.heroes.available_skill_points(hero) == 10);
  CHECK(f.heroes.skill(hero, 8) == 0);
}

TEST(ai_get_skill_to_develop_hands_the_script_the_skill_id_and_its_current_points) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  // Only skill 5 is allowed to stay in the draw, and its goal is built from
  // both inputs: the current points plus the skill's own id.
  REQUIRE(build(f.scheduler, f.registry,
                skill_script("if (skill != 5) weight = 0;\n"
                             "skill_points = skill_points + skill;\n")
                    .c_str(),
                "DATA/AI/HEROSKILL DEFAULT.VS") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const ObjectId hero = f.make_hero(1, 10);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::discipline, 2));

  const SkillPick pick = pick_skill(f, hero, root);
  CHECK(pick.status == script::HostStatus::ok);
  CHECK(pick.skill == 5);
  CHECK(pick.level == 7);
}

TEST(ai_get_skill_to_develop_draws_by_weight_in_table_order) {
  // Two skills survive with weights 100 and 300. The draw is `rand(1, 400)`,
  // inclusive at both ends, and the pick is the first skill in table order at
  // which subtracting weights takes it to zero or below: 1..100 is skill 2,
  // 101..400 is skill 9. Checked against the RNG's own stream over enough
  // seeds that both answers occur, so an off-by-one in either the range or
  // the walk shows up as a disagreement on some seed.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                skill_script("if (skill == 2) { skill_points = 1; }\n"
                             "else if (skill == 9) { skill_points = 1; weight = weight * 3; }\n"
                             "else weight = 0;\n")
                    .c_str(),
                "DATA/AI/HEROSKILL DEFAULT.VS") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const ObjectId hero = f.make_hero(1, 10);

  int picked_two = 0;
  int picked_nine = 0;
  for (std::uint32_t seed = 1; seed <= 40; ++seed) {
    f.world.rng().seed(seed);
    Rng model = f.world.rng();
    const std::int32_t draw = model.between(1, 400);
    const std::int32_t expected = draw <= 100 ? 2 : 9;
    const SkillPick pick = pick_skill(f, hero, root);
    CHECK(pick.status == script::HostStatus::ok);
    CHECK(pick.skill == expected);
    CHECK(pick.level == 1);
    CHECK(f.world.rng().state() == model.state());
    if (pick.skill == 2) ++picked_two;
    if (pick.skill == 9) ++picked_nine;
  }
  CHECK(picked_two > 0);
  CHECK(picked_nine > 0);
  CHECK(picked_two + picked_nine == 40);

  // And the edge itself, which forty seeds will not land on by chance: a seed
  // whose draw is exactly 100 picks skill 2 (the subtraction reaches zero, and
  // zero is a pick), and a seed whose draw is exactly 101 picks skill 9.
  for (const std::int32_t edge : {100, 101}) {
    std::uint32_t seed = 1;
    for (; seed < 200000; ++seed) {
      Rng probe(seed);
      if (probe.between(1, 400) == edge) break;
    }
    REQUIRE(seed < 200000);
    f.world.rng().seed(seed);
    const SkillPick pick = pick_skill(f, hero, root);
    CHECK(pick.status == script::HostStatus::ok);
    CHECK(pick.skill == (edge == 100 ? 2 : 9));
  }
}

TEST(ai_get_skill_to_develop_draws_the_exact_value_over_twenty_five_equal_weights) {
  // Every skill survives with weight 1, so the total is 25 and the pick is the
  // draw itself, less one: `rand(1, 25)` is 1..25 inclusive and skill k is
  // reached when the running subtraction hits zero at the k+1th weight. A
  // draw taken as `rand(1, 24)` or `rand(0, 25)` disagrees with this on about
  // half of all seeds, where the two-weight case above only sees the edge.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                skill_script("skill_points = skill_points + 1;\nweight = 1;\n").c_str(),
                "DATA/AI/HEROSKILL DEFAULT.VS") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const ObjectId hero = f.make_hero(1, 10);

  bool saw_first = false;
  bool saw_last = false;
  for (std::uint32_t seed = 1; seed <= 60; ++seed) {
    f.world.rng().seed(seed);
    Rng model = f.world.rng();
    const std::int32_t draw = model.between(1, 25);
    const SkillPick pick = pick_skill(f, hero, root);
    CHECK(pick.status == script::HostStatus::ok);
    CHECK(pick.skill == draw - 1);
    CHECK(pick.level == 1);
    if (pick.skill == 0) saw_first = true;
    if (pick.skill == 24) saw_last = true;
  }
  CHECK(saw_first);
  CHECK(saw_last);
}

TEST(ai_get_skill_to_develop_counts_a_file_with_the_wrong_parameter_list_as_no_weight) {
  // The read-back is positional over the shipped default's declaration. A
  // per-skill file that declares fewer parameters is the wrong file: its run
  // leaves no slot to read, and the skill drops out of the draw rather than
  // reading whatever happens to sit at that index.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                skill_script("if (skill == 7) skill_points = 2; else weight = 0;\n").c_str(),
                "DATA/AI/HEROSKILL DEFAULT.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Hero hero, int skill, int *skill_points\nskill_points = 9;\n",
                "DATA/AI/HeroSkill Rush.vs") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const SkillPick pick = pick_skill(f, f.make_hero(1, 10), root);
  CHECK(pick.status == script::HostStatus::ok);
  CHECK(pick.skill == 7);
  CHECK(pick.level == 2);
}

TEST(ai_get_skill_to_develop_skips_a_maxed_or_unoffered_skill_without_running_its_script) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  // A run for skill 0 or 1 leaves a mark on the world; every other run holds
  // its skill back except 2, which is the pick.
  REQUIRE(build(f.scheduler, f.registry,
                skill_script("if (skill == 0 || skill == 1) hero.SetSkill(24, 1);\n"
                             "if (skill == 2) skill_points = 4; else weight = 0;\n")
                    .c_str(),
                "DATA/AI/HEROSKILL DEFAULT.VS") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const ObjectId hero = f.make_hero(1, 20);
  // Skill 0 is at the cap; skill 1 is one the hero's class does not offer.
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::administration, 10));
  f.heroes.hero(hero)->offered[1] = false;

  const SkillPick pick = pick_skill(f, hero, root);
  CHECK(pick.status == script::HostStatus::ok);
  CHECK(pick.skill == 2);
  CHECK(pick.level == 4);
  CHECK(f.heroes.skill(hero, 24) == 0);

  // And the cap is the same one `set_skill` clamps against, so a skill parked
  // one under it is still developable.
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::administration, 9));
  f.heroes.hero(hero)->offered[1] = true;
  (void)pick_skill(f, hero, root);
  CHECK(f.heroes.skill(hero, 24) == 1);
}

TEST(ai_get_skill_to_develop_answers_minus_one_and_draws_nothing_when_no_skill_qualifies) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  // Half the skills answer with weight 0, the other half with a goal that is
  // not above the current points; neither counts, and with a total of 0 the
  // RNG is not touched.
  REQUIRE(build(f.scheduler, f.registry,
                skill_script("if (skill < 12) weight = 0; else skill_points = skill_points;\n")
                    .c_str(),
                "DATA/AI/HEROSKILL DEFAULT.VS") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const ObjectId hero = f.make_hero(1, 10);
  REQUIRE(f.heroes.set_skill(hero, HeroSkill::scout, 3));

  const std::uint32_t before = f.world.rng().state();
  const SkillPick pick = pick_skill(f, hero, root);
  CHECK(pick.status == script::HostStatus::ok);
  CHECK(pick.skill == -1);
  CHECK(pick.level == -1);
  CHECK(f.world.rng().state() == before);
}

TEST(ai_get_skill_to_develop_clamps_the_goal_to_the_cap) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                skill_script("if (skill == 3) skill_points = 50; else weight = 0;\n").c_str(),
                "DATA/AI/HEROSKILL DEFAULT.VS") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const ObjectId hero = f.make_hero(1, 10);

  const SkillPick pick = pick_skill(f, hero, root);
  CHECK(pick.status == script::HostStatus::ok);
  CHECK(pick.skill == 3);
  CHECK(pick.level == 10);
}

TEST(ai_get_skill_to_develop_counts_a_failed_run_as_no_weight) {
  // A per-skill file that sleeps has nowhere to suspend to inside a
  // synchronous call and is refused by `run_script_now`; the original's
  // runner reports the same failure by its return value, and the body turns
  // it into a weight of 0 rather than into an error for the caller.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                skill_script("if (skill == 7) skill_points = 2; else weight = 0;\n").c_str(),
                "DATA/AI/HEROSKILL DEFAULT.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                skill_script("skill_points = 9;\nSleep(10);\n").c_str(),
                "DATA/AI/HeroSkill Administration.vs") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const ObjectId hero = f.make_hero(1, 10);

  const SkillPick pick = pick_skill(f, hero, root);
  CHECK(pick.status == script::HostStatus::ok);
  CHECK(pick.skill == 7);
  CHECK(pick.level == 2);
  // The half-run file left nothing behind.
  CHECK(f.scheduler.live_count() == 1);
}

TEST(ai_get_skill_to_develop_resolves_the_files_under_the_heros_owner) {
  // The original reads the player off the hero's own player pointer, as
  // `TSAdvHeroSkills` does, and the search path is that profile's: an overlay
  // copy of a per-skill file wins for the overlay's hero and not for another
  // player's, whichever script is asking.
  Fixture f;
  AiProfile defensive;
  const auto parsed = AiProfile::parse(bytes_of(kProfileIni));
  REQUIRE(parsed.ok());
  defensive = parsed.value();
  f.ai.add_profile("DEFENSIVE", &defensive, "DATA\\AI\\DEFENSIVE\\");

  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n",
                "DATA/AI/DEFENSIVE/MAIN.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, skill_script("weight = 0;\n").c_str(),
                "DATA/AI/HEROSKILL DEFAULT.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, skill_script("skill_points = 3;\n").c_str(),
                "DATA/AI/HeroSkill Wisdom.vs") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, skill_script("skill_points = 7;\n").c_str(),
                "DATA/AI/DEFENSIVE/HeroSkill Wisdom.vs") != script::kNoChunk);

  CHECK(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  CHECK(f.ai.start(1, "DEFENSIVE", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId caller = f.ai.player_ai(0)->root;

  const SkillPick theirs = pick_skill(f, f.make_hero(1, 10), caller);
  CHECK(theirs.skill == 15);
  CHECK(theirs.level == 7);  // the overlay's copy
  const SkillPick ours = pick_skill(f, f.make_hero(0, 10), caller);
  CHECK(ours.skill == 15);
  CHECK(ours.level == 3);  // the root's
}

TEST(ai_get_skill_to_develop_answers_a_receiver_that_names_nothing_without_running) {
  // `The function 'Hero::AIGetSkillToDevelop' called for an uninitialized or
  // invalid object.` -- printed, both out-parameters already -1, no script run
  // and no draw. The shipped caller tests `nSkillDevelop < 0` and breaks out
  // of its loop, so a refusal here would kill the idle behaviour of every
  // hero whose handle went stale between two polls.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, skill_script("hero.SetSkill(0, 1);\n").c_str(),
                "DATA/AI/HEROSKILL DEFAULT.VS") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const std::uint32_t before = f.world.rng().state();

  std::vector<script::Value> args = {script::Value::object(script::kNoType, 0),
                                     script::Value::integer(99), script::Value::integer(99)};
  CHECK(call_into(f, "AIGetSkillToDevelop", args, root).status == script::HostStatus::ok);
  CHECK(args[1].is_integer() && args[1].as_integer() == -1);
  CHECK(args[2].is_integer() && args[2].as_integer() == -1);
  // And a receiver that is not an object handle at all is the same answer,
  // not a failure of the caller.
  args = {script::Value::integer(5), script::Value::integer(99), script::Value::integer(99)};
  CHECK(call_into(f, "AIGetSkillToDevelop", args, root).status == script::HostStatus::ok);
  CHECK(args[1].is_integer() && args[1].as_integer() == -1);
  CHECK(args[2].is_integer() && args[2].as_integer() == -1);

  // A handle to something that is not a hero has no skill table and is the
  // same answer.
  const ObjectId building = f.world.spawn(imperivm::core::NativeClass::building, nullptr);
  f.world.set_owner(building, 1);
  const SkillPick pick = pick_skill(f, building, root);
  CHECK(pick.status == script::HostStatus::ok);
  CHECK(pick.skill == -1);
  CHECK(pick.level == -1);
  CHECK(f.world.rng().state() == before);
}

TEST(ai_get_skill_to_develop_answers_minus_one_when_no_skill_file_is_loaded) {
  // Neither a per-skill file nor the default: every skill is a run that could
  // not start, which is weight 0, and the answer is the same -1 pair rather
  // than an error -- the original's runner fails per skill and the body never
  // looks at why.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const SkillPick pick = pick_skill(f, f.make_hero(1, 10), root);
  CHECK(pick.status == script::HostStatus::ok);
  CHECK(pick.skill == -1);
  CHECK(pick.level == -1);
}

// --------------------------------------------------------------------------
// the recruiter family, the fourth trampoline and the largest
// --------------------------------------------------------------------------

/// **59 call sites across four names, and no native behaviour behind any.**
///
/// `Settlement::TSRecruitArmy` (0x00431b30) and its three siblings are
/// `TSAdvHeroSkills`'s shape: resolve a `.vs` file on a player's AI search
/// path, run it synchronously, hand back what it filled. What is under test
/// here is the trampoline -- the list is minted by the entry point and comes
/// back through the helper's last parameter, the file is found under the
/// *settlement's* owner, and the class and count arguments arrive where the
/// helper declares them.
///
/// **The helpers built here are stand-ins, not the shipped ones.** Their names
/// and signatures are the shipped ones, because those are what the lookup and
/// the argument shuffle are made of; the bodies are written for this test. The
/// real `TSH_RECRUITARMY.VS` is game data and this project does not carry game
/// data, as a test fixture or otherwise -- what it does is described in prose
/// beside `recruit_into_list`.
///
/// The declaration of `ObjList ol2;` in the stand-in is deliberate and is half
/// the point: `run_script_now` used to refuse any callee that declared a
/// pooled type, which is what kept all four of these names unbound.
TEST(ts_recruit_army_hands_back_the_list_its_helper_filled) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Settlement set, str class, int num, ObjList ol\n"
                "ObjList ol2;\n"
                "if (class == \"Legionary\")\n"
                "  if (num == 3)\n"
                "    ol2.Add(set.GetCentralBuilding);\n"
                "ol.AddList(ol2);\n",
                "DATA/AI/TSH_RECRUITARMY.VS") != script::kNoChunk);

  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const SettlementId id = f.make_settlement(1);
  const Settlement* set = f.economy.settlements().find(id);
  REQUIRE(set != nullptr);

  const script::HostOutcome out = f.call(
      script::CallKind::member, "TSRecruitArmy",
      {f.settlement_value(id), script::Value::string("Legionary"), script::Value::integer(3)},
      root);
  CHECK(out.status == script::HostStatus::ok);
  REQUIRE(is_objlist(out.value));
  // Both arguments crossed -- the helper only adds when it sees both -- and the
  // `AddList` through the caller's handle landed on the list this entry point
  // minted, which is the whole out-parameter convention.
  const std::span<const ObjectId> items = f.world.objlists().items(objlist_of(out.value));
  REQUIRE(items.size() == 1);
  CHECK(items[0] == set->anchor);
  // Synchronous: `Main.vs` is still the only live script.
  CHECK(f.scheduler.live_count() == 1);
}

TEST(ts_recruit_army_hands_back_an_empty_list_when_the_helper_adds_nothing) {
  // The same helper, called with a class it does not recognise. Every shipped
  // caller walks the result with `for (i = 0; i < oll.count; ...)`, so an empty
  // list is an ordinary answer and not a failure.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Settlement set, str class, int num, ObjList ol\n"
                "ObjList ol2;\n"
                "if (class == \"Legionary\")\n"
                "  ol2.Add(set.GetCentralBuilding);\n"
                "ol.AddList(ol2);\n",
                "DATA/AI/TSH_RECRUITARMY.VS") != script::kNoChunk);

  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const script::HostOutcome out =
      f.call(script::CallKind::member, "TSRecruitArmy",
             {f.settlement_value(f.make_settlement(1)), script::Value::string("Praetorian"),
              script::Value::integer(3)},
             root);
  CHECK(out.status == script::HostStatus::ok);
  REQUIRE(is_objlist(out.value));
  CHECK(f.world.objlists().items(objlist_of(out.value)).empty());
}

/// A receiver naming no settlement returns the empty list without running the
/// helper, which is what the original does: 0x00431bd7 jumps past the script
/// call to the return, printing `The function 'TSRecruitArmy' called for an
/// uninitialized or invalid object.` through a sink that is a bare `ret` in
/// retail. A refusal here would trap the calling tactic script instead.
TEST(ts_recruit_army_answers_a_receiver_that_names_nothing_without_running) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Settlement set, str class, int num, ObjList ol\n"
                "ol.Add(set.GetCentralBuilding);\n",
                "DATA/AI/TSH_RECRUITARMY.VS") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;

  const script::HostOutcome out =
      f.call(script::CallKind::member, "TSRecruitArmy",
             {script::Value::integer(0), script::Value::string("Legionary"),
              script::Value::integer(1)},
             root);
  CHECK(out.status == script::HostStatus::ok);
  REQUIRE(is_objlist(out.value));
  CHECK(f.world.objlists().items(objlist_of(out.value)).empty());
}

/// **The file is resolved under the settlement's owner, not the caller's.**
///
/// Measured rather than assumed: 0x00431be8 loads the settlement's player
/// through `[settlement+0x90]` and hands it to the resolver as its first
/// argument, so the search path is the settlement's AI profile. The two agree
/// at every shipped site -- a tactic script only ever recruits for its own
/// player -- so it takes two profiles carrying two copies of one file to tell
/// them apart at all, exactly as it does for `TSAdvHeroSkills`.
TEST(ts_recruit_army_uses_the_settlements_profile_and_not_the_callers) {
  Fixture f;
  AiProfile defensive;
  const auto parsed = AiProfile::parse(bytes_of(kProfileIni));
  REQUIRE(parsed.ok());
  defensive = parsed.value();
  f.ai.add_profile("DEFENSIVE", &defensive, "DATA\\AI\\DEFENSIVE\\");

  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n",
                "DATA/AI/DEFENSIVE/MAIN.VS") != script::kNoChunk);
  // Two copies of one file, one per profile. The root's adds nothing; the
  // overlay's adds the anchor, so which one ran is visible in the count.
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Settlement set, str class, int num, ObjList ol\n"
                "int i;\n"
                "i = 0;\n",
                "DATA/AI/TSH_RECRUITARMY.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Settlement set, str class, int num, ObjList ol\n"
                "ol.Add(set.GetCentralBuilding);\n",
                "DATA/AI/DEFENSIVE/TSH_RECRUITARMY.VS") != script::kNoChunk);

  CHECK(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  CHECK(f.ai.start(1, "DEFENSIVE", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  // The caller is player 0's root script; the settlement belongs to player 1.
  const script::ScriptId caller = f.ai.player_ai(0)->root;

  const script::HostOutcome theirs =
      f.call(script::CallKind::member, "TSRecruitArmy",
             {f.settlement_value(f.make_settlement(1)), script::Value::string("Legionary"),
              script::Value::integer(1)},
             caller);
  CHECK(theirs.status == script::HostStatus::ok);
  CHECK(f.world.objlists().items(objlist_of(theirs.value)).size() == 1);

  // And the same call for a settlement of player 0 takes the root copy, so the
  // two readings really do disagree here rather than the overlay simply winning.
  const script::HostOutcome ours =
      f.call(script::CallKind::member, "TSRecruitArmy",
             {f.settlement_value(f.make_settlement(0)), script::Value::string("Legionary"),
              script::Value::integer(1)},
             caller);
  CHECK(ours.status == script::HostStatus::ok);
  CHECK(f.world.objlists().items(objlist_of(ours.value)).empty());
}

TEST(ts_arena_and_temple_recruit_are_the_same_trampoline_on_their_own_files) {
  // Two names, one shape, and the only thing that distinguishes them is which
  // file they resolve. Each is checked against a stand-in that could only have
  // been reached through its own name.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Settlement set, int num, ObjList ol\n"
                "ObjList ol2;\n"
                "if (num == 7)\n"
                "  ol2.Add(set.GetCentralBuilding);\n"
                "ol.AddList(ol2);\n",
                "DATA/AI/TSH_ARENARECRUIT.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Settlement set, int num, ObjList ol\n"
                "ObjList ol2;\n"
                "if (num == 9)\n"
                "  ol2.Add(set.GetCentralBuilding);\n"
                "ol.AddList(ol2);\n",
                "DATA/AI/TSH_TEMPLERECRUIT.VS") != script::kNoChunk);

  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const script::Value handle = f.settlement_value(f.make_settlement(1));

  const script::HostOutcome arena =
      f.call(script::CallKind::member, "TSArenaRecruit", {handle, script::Value::integer(7)}, root);
  CHECK(arena.status == script::HostStatus::ok);
  CHECK(f.world.objlists().items(objlist_of(arena.value)).size() == 1);

  const script::HostOutcome temple = f.call(script::CallKind::member, "TSTempleRecruit",
                                            {handle, script::Value::integer(9)}, root);
  CHECK(temple.status == script::HostStatus::ok);
  CHECK(f.world.objlists().items(objlist_of(temple.value)).size() == 1);

  // And crossed: the arena's count reaches only the arena's file.
  const script::HostOutcome crossed = f.call(script::CallKind::member, "TSArenaRecruit",
                                             {handle, script::Value::integer(9)}, root);
  CHECK(crossed.status == script::HostStatus::ok);
  CHECK(f.world.objlists().items(objlist_of(crossed.value)).empty());
}

/// `TSRecruitHero` is the one that is not a list, and the one real
/// by-reference parameter in the family: `TSH_HeroRecruit.vs` is declared
/// `void, Settlement set, Hero *h` and assigns `h = hh`. The entry point
/// passes an unbound handle in and reads slot 1 back out of the finished
/// frame.
///
/// The settlement here is anchored on a hero so the stand-in has something to
/// name; the shipped helper finds one through `GetAIControlledUnits` instead.
TEST(ts_recruit_hero_reads_back_the_by_reference_hero) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Settlement set, Hero *h\n"
                "h = set.GetCentralBuilding.AsHero;\n",
                "DATA/AI/TSH_HERORECRUIT.VS") != script::kNoChunk);

  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;

  const ObjectId hero = f.make_hero(1, 0);
  SettlementInit init;
  init.settlement_object = hero;
  init.anchor = hero;
  init.owner = 1;
  init.kind = SettlementKind::stronghold;
  const SettlementId id = f.economy.settlements().create(init);

  const script::HostOutcome out =
      f.call(script::CallKind::member, "TSRecruitHero", {f.settlement_value(id)}, root);
  CHECK(out.status == script::HostStatus::ok);
  REQUIRE(out.value.is_object());
  CHECK(out.value.as_object().id == hero);
}

TEST(ts_recruit_hero_hands_back_an_unbound_handle_when_the_helper_assigns_nothing) {
  // Every shipped caller writes `if (!h.IsAlive())` immediately afterwards, so
  // "found nobody" has to come back as a handle that names nothing rather than
  // as a failure.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Settlement set, Hero *h\n"
                "int i;\n"
                "i = 1;\n",
                "DATA/AI/TSH_HERORECRUIT.VS") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;

  const script::HostOutcome out = f.call(script::CallKind::member, "TSRecruitHero",
                                         {f.settlement_value(f.make_settlement(1))}, root);
  CHECK(out.status == script::HostStatus::ok);
  REQUIRE(out.value.is_object());
  CHECK(f.world.find(out.value.as_object().id) == nullptr);
}

/// **The synchronous callee's pooled state is released when the call ends, and
/// nothing used to release it.**
///
/// `run_script_now` builds a synthetic script id and never enters the
/// scheduler's lifecycle, so the scheduler's teardown -- the one thing that
/// frees a dead script's `ObjList`s, arrays, squad lists and conversations --
/// never ran for it. Every synchronous call leaked one pool entry per pooled
/// local, for the whole session. Two locals and three calls make the leak six
/// entries and the fix three, which is the difference this measures.
TEST(a_synchronous_call_leaves_no_pooled_state_behind) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// void, Settlement set, int num, ObjList ol\n"
                "ObjList ol2;\n"
                "ObjList oll;\n"
                "ol2.Add(set.GetCentralBuilding);\n"
                "oll.Add(set.GetCentralBuilding);\n"
                "ol.AddList(ol2);\n",
                "DATA/AI/TSH_ARENARECRUIT.VS") != script::kNoChunk);

  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  const script::Value handle = f.settlement_value(f.make_settlement(1));

  for (int i = 0; i < 3; ++i) {
    const script::HostOutcome out = f.call(script::CallKind::member, "TSArenaRecruit",
                                           {handle, script::Value::integer(1)}, root);
    CHECK(out.status == script::HostStatus::ok);
    CHECK(f.world.objlists().items(objlist_of(out.value)).size() == 1);
  }
  // Three live entries: one returned list per call, owned by the caller and
  // still reachable. The callee's two locals are gone from all three.
  CHECK(f.world.objlists().size() == 3);
}

// --------------------------------------------------------------------------
// the per-player node view
// --------------------------------------------------------------------------

/// A fresh view is the identity permutation with `Optimism` 100 and everything
/// else zero -- the original's constructor, which sizes both arrays to the
/// whole node vector rather than growing them as nodes are discovered.
TEST(a_players_node_view_starts_as_the_identity_with_optimism_a_hundred) {
  Fixture f;
  f.ai.seed_gaika_view(1, 4);
  const GaikaView* view = f.ai.gaika_view(1);
  REQUIRE(view != nullptr);
  // Five records for four nodes: index 0 is the reserved one, which is what
  // makes `GAIKACount()` one more than the table's own count.
  CHECK(view->size() == 5);
  for (std::int32_t g = 0; g <= 4; ++g) {
    CHECK(view->ranked(g) == g);
  }
  for (std::int32_t g = 1; g <= 4; ++g) {
    REQUIRE(view->find(g) != nullptr);
    CHECK(view->find(g)->gaika == g);
    CHECK(view->find(g)->optimism == kDefaultGaikaOptimism);
    CHECK(view->find(g)->priority == 0);
    CHECK(view->find(g)->flags == 0);
  }
  // Node 0 is not a place, and neither is one past the end.
  CHECK(view->find(0) == nullptr);
  CHECK(view->find(5) == nullptr);
  CHECK(view->find(-1) == nullptr);

  // A player with no view answers nothing, which is every accessor's own
  // "this player has no AI" arm.
  const GaikaView* other = f.ai.gaika_view(2);
  REQUIRE(other != nullptr);
  CHECK(other->size() == 0);
  CHECK(other->find(1) == nullptr);
  CHECK(other->ranked(0) == kNoGaika);
}

/// **`SetPriority` reorders the view**, and the order is what `LAIKA(p, i)`
/// reads. Descending, stable, and the slot map stays in step at every swap.
TEST(set_priority_sorts_the_view_and_keeps_the_map_in_step) {
  Fixture f;
  f.ai.seed_gaika_view(1, 4);
  GaikaView* view = f.ai.gaika_view(1);
  REQUIRE(view != nullptr);
  const auto ranking = [&] {
    std::vector<GaikaId> out;
    for (std::int32_t i = 0; i < view->size(); ++i) out.push_back(view->ranked(i));
    return out;
  };
  // The invariant the map exists for: `records[map[g]].gaika == g`.
  const auto consistent = [&] {
    for (std::int32_t g = 1; g < view->size(); ++g) {
      const Laika* record = view->find(g);
      if (record == nullptr || record->gaika != g) return false;
    }
    return true;
  };

  view->set_priority(2, 50);
  CHECK(ranking() == (std::vector<GaikaId>{0, 2, 1, 3, 4}));
  CHECK(consistent());

  view->set_priority(4, 70);
  CHECK(ranking() == (std::vector<GaikaId>{0, 4, 2, 1, 3}));
  CHECK(consistent());
  CHECK(view->find(4)->priority == 70);

  // Lowering sinks it, and it stops as soon as the next one is not higher.
  view->set_priority(4, 10);
  CHECK(ranking() == (std::vector<GaikaId>{0, 2, 4, 1, 3}));
  CHECK(consistent());

  // **Stable**: node 1 rises to equal node 2's priority and stops *behind* it,
  // because both loops halt on equality.
  view->set_priority(1, 50);
  CHECK(ranking() == (std::vector<GaikaId>{0, 2, 1, 4, 3}));
  CHECK(consistent());

  // Slot 0 never moves, whatever anyone does to it.
  view->set_priority(0, 1000);
  CHECK(ranking()[0] == 0);
  CHECK(view->find(0) == nullptr);
}

/// **The sink loop stops on equality too**, which the rising case above cannot
/// show: it takes a record falling *past* two equal priorities to tell a stable
/// insertion from an unstable one.
TEST(lowering_a_priority_stops_at_the_first_equal_neighbour) {
  Fixture f;
  f.ai.seed_gaika_view(1, 3);
  GaikaView* view = f.ai.gaika_view(1);
  REQUIRE(view != nullptr);
  const auto ranking = [&] {
    std::vector<GaikaId> out;
    for (std::int32_t i = 1; i < view->size(); ++i) out.push_back(view->ranked(i));
    return out;
  };

  // Build [1 @ 60, 2 @ 50, 3 @ 50] -- two equal priorities behind a higher one.
  view->set_priority(3, 50);
  view->set_priority(2, 50);
  view->set_priority(1, 60);
  CHECK(ranking() == (std::vector<GaikaId>{1, 3, 2}));

  // Now drop node 1 to the same 50. A stable sink stops in front of both; an
  // unstable one falls to the back.
  view->set_priority(1, 50);
  CHECK(ranking() == (std::vector<GaikaId>{1, 3, 2}));
}

/// `Prioritized` means "`SetPriority` ran with a value that **differed**",
/// which is what makes `GAIKAMONITOR.VS`'s wait terminate on a real pass rather
/// than on the first call.
TEST(prioritized_is_set_only_by_a_priority_that_changed) {
  Fixture f;
  f.ai.seed_gaika_view(1, 3);
  GaikaView* view = f.ai.gaika_view(1);
  REQUIRE(view != nullptr);

  // Setting the value it already has changes nothing at all.
  view->set_priority(1, 0);
  CHECK((view->find(1)->flags & kLaikaPrioritized) == 0);
  view->set_priority(1, 7);
  CHECK((view->find(1)->flags & kLaikaPrioritized) != 0);
  // And it is never cleared.
  view->set_priority(1, 0);
  CHECK((view->find(1)->flags & kLaikaPrioritized) != 0);
}

/// The entry points, through the registry, with the lookup they all share:
/// **every failure is the type's zero and none of them is an error**.
TEST(the_node_view_entry_points_read_and_write_the_players_own_records) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  f.ai.seed_gaika_view(1, 4);

  const auto member = [&](const char* name, std::vector<script::Value> args) {
    return f.call(script::CallKind::member, name, std::move(args), root);
  };
  const auto node = [](GaikaId g) { return script::Value::integer(g); };
  // Player 2 in the script's 1-based numbering is player 1 here.
  const script::Value me = script::Value::integer(2);
  const script::Value nobody = script::Value::integer(3);

  // Priority, read and written through the entry points.
  CHECK(member("GetPriority", {node(1), me}).value.as_integer() == 0);
  CHECK(member("SetPriority", {node(1), me, script::Value::integer(42)}).status ==
        script::HostStatus::ok);
  CHECK(member("GetPriority", {node(1), me}).value.as_integer() == 42);
  CHECK(member("Prioritized", {node(1), me}).value.truthy_scalar());
  CHECK(!member("Prioritized", {node(2), me}).value.truthy_scalar());

  // `LAIKA` is the ranked accessor, and node 1 has just been sorted to the top.
  const auto laika = [&](std::int32_t index) {
    return f.call(script::CallKind::free_function, "LAIKA",
                  {me, script::Value::integer(index)}, root)
        .value.as_integer();
  };
  CHECK(laika(1) == 1);
  CHECK(laika(0) == 0);  // the reserved node
  CHECK(laika(99) == kNoGaika);

  // The flags. `SetControlFlag` is the only writer among them with a shipped
  // site, and its reader is in the same profile.
  CHECK(!member("GetControlFlag", {node(3), me}).value.truthy_scalar());
  CHECK(member("SetControlFlag", {node(3), me, script::Value::integer(1)}).status ==
        script::HostStatus::ok);
  CHECK(member("GetControlFlag", {node(3), me}).value.truthy_scalar());
  CHECK(member("SetControlFlag", {node(3), me, script::Value::integer(0)}).status ==
        script::HostStatus::ok);
  CHECK(!member("GetControlFlag", {node(3), me}).value.truthy_scalar());
  // Each bit is its own: setting the control flag moves none of the others.
  CHECK(member("SetControlFlag", {node(3), me, script::Value::integer(1)}).status ==
        script::HostStatus::ok);
  CHECK(!member("NoAttack", {node(3), me}).value.truthy_scalar());
  CHECK(!member("NoRecruit", {node(3), me}).value.truthy_scalar());
  CHECK(!member("Revealed", {node(3), me}).value.truthy_scalar());
  CHECK(!member("Explored", {node(3), me}).value.truthy_scalar());

  // Set from the C++ side, since the shipped corpus has no writer for these.
  GaikaView* view = f.ai.gaika_view(1);
  REQUIRE(view != nullptr);
  view->find(3)->flags |= kLaikaNoAttack;
  view->find(3)->flags |= kLaikaExplored;
  view->find(3)->last_seen = 12345;
  CHECK(member("NoAttack", {node(3), me}).value.truthy_scalar());
  CHECK(member("Explored", {node(3), me}).value.truthy_scalar());
  CHECK(!member("NoRecruit", {node(3), me}).value.truthy_scalar());
  CHECK(member("LastSeen", {node(3), me}).value.as_integer() == 12345);

  // **Every failure is a zero, and none is an error.**
  CHECK(member("GetPriority", {node(0), me}).value.as_integer() == 0);   // the reserved node
  CHECK(member("GetPriority", {node(9), me}).value.as_integer() == 0);   // past the end
  CHECK(member("GetPriority", {node(1), nobody}).value.as_integer() == 0);  // a player with none
  CHECK(member("GetPriority", {node(1), script::Value::integer(0)}).value.as_integer() == 0);
  CHECK(member("GetPriority", {node(1), script::Value::integer(17)}).value.as_integer() == 0);
  // **A player argument that wraps.** `PlayerId` is a byte, so `-255` would
  // decrement to `-256` and cast to player 0 -- somebody else's records -- if
  // the 1..16 test were not applied before the cast. Player 0 is given a view
  // with a value nothing else uses, so the wrong answer would be visible.
  f.ai.seed_gaika_view(0, 4);
  f.ai.gaika_view(0)->set_priority(1, 999);
  CHECK(member("GetPriority", {node(1), script::Value::integer(-255)}).value.as_integer() == 0);
  CHECK(member("GetPriority", {node(1), script::Value::integer(1)}).value.as_integer() == 999);
  CHECK(member("GetPriority", {node(1), me}).status == script::HostStatus::ok);
  CHECK(member("GetPriority", {node(1), nobody}).status == script::HostStatus::ok);
  CHECK(!member("Prioritized", {node(0), me}).value.truthy_scalar());
  // And a write through a bad lookup does nothing rather than refusing.
  CHECK(member("SetPriority", {node(9), me, script::Value::integer(5)}).status ==
        script::HostStatus::ok);
  CHECK(member("SetControlFlag", {node(1), nobody, script::Value::integer(1)}).status ==
        script::HostStatus::ok);
  CHECK(!member("GetControlFlag", {node(1), me}).value.truthy_scalar());
}

/// **`AIStart` seeds the view and `AIStop` drops it**, which is the original's
/// lifetime: the arrays are constructed with the AI object and destroyed with
/// it, and a restart rebuilds them.
TEST(starting_an_ai_gives_it_a_view_of_every_node_and_stopping_takes_it_back) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  // Two settlements, so the node table has two nodes and the view three
  // records -- the reserved one and one per node.
  (void)f.make_settlement(1);
  (void)f.make_settlement(1);
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  REQUIRE(f.world.gaika().count() == 2);

  REQUIRE(f.ai.gaika_view(1) != nullptr);
  CHECK(f.ai.gaika_view(1)->size() == 0);
  CHECK(ai_start(f.world, 1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  CHECK(f.ai.gaika_view(1)->size() == 3);
  CHECK(f.ai.gaika_view(1)->find(2) != nullptr);

  CHECK(f.ai.stop(1, f.scheduler));
  CHECK(f.ai.gaika_view(1)->size() == 0);
  CHECK(f.ai.gaika_view(1)->find(1) == nullptr);
}

/// The view is world state and rides in the save -- both arrays, because the
/// permutation *is* the behaviour.
TEST(a_node_view_round_trips_through_the_save_with_its_ranking) {
  Fixture f;
  f.ai.seed_gaika_view(1, 4);
  GaikaView* view = f.ai.gaika_view(1);
  REQUIRE(view != nullptr);
  view->set_priority(3, 90);
  view->set_priority(2, 40);
  view->find(3)->flags |= kLaikaExplored;
  view->find(3)->last_seen = 777;
  view->find(2)->optimism = 300;
  view->find(2)->strat = 5;

  std::vector<std::byte> bytes;
  f.ai.serialize(bytes);
  AiSystem loaded;
  REQUIRE(loaded.deserialize(bytes).ok());
  const GaikaView* back = loaded.gaika_view(1);
  REQUIRE(back != nullptr);
  CHECK(back->size() == 5);
  CHECK(back->ranked(1) == 3);
  CHECK(back->ranked(2) == 2);
  REQUIRE(back->find(3) != nullptr);
  CHECK(back->find(3)->priority == 90);
  CHECK((back->find(3)->flags & kLaikaExplored) != 0);
  CHECK(back->find(3)->last_seen == 777);
  REQUIRE(back->find(2) != nullptr);
  CHECK(back->find(2)->optimism == 300);
  CHECK(back->find(2)->strat == 5);
  // The map came back with it rather than being rebuilt, which is what keeps
  // the ranking rather than only the values.
  CHECK(back->slots().size() == 5);
}

/// **`StratRunning` is the field, `GetStrat` is the script, `RunStrat` is the
/// spawn** -- and `GAIKAMONITOR.VS` is all three in four lines.
///
/// The stand-in `GetGAIKAStrat.vs` here is written for this test; its name and
/// signature are the shipped ones because those are what the lookup and the
/// argument shuffle are made of, and the real file is game data this project
/// does not carry.
TEST(the_strategy_trio_asks_a_script_reads_a_field_and_spawns_a_coroutine) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  // Its declared signature is the shipped one: the GAIKA first, the player
  // second -- which is the opposite of `CalcGAIKAPriority.vs`.
  REQUIRE(build(f.scheduler, f.registry,
                "// int, GAIKA gaika, int idPlayer\n"
                "if (idPlayer != 2) return -5;\n"
                "if (gaika == 3) return 4;\n"
                "return 0;\n",
                "DATA/AI/GETGAIKASTRAT.VS") != script::kNoChunk);
  // The stand-in records **which node it was handed**, so a spawn that passed
  // the player instead would mark a different record.
  REQUIRE(build(f.scheduler, f.registry,
                "// void, GAIKA gaika\n"
                "gaika.SetPriority(AIGetPlayer, 77);\n"
                "Sleep(100000);\n",
                "DATA/AI/GS_GUARD.VS") != script::kNoChunk);

  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  f.ai.seed_gaika_view(1, 4);
  const auto member = [&](const char* name, std::vector<script::Value> args) {
    return f.call(script::CallKind::member, name, std::move(args), root);
  };
  const script::Value me = script::Value::integer(2);

  // `GetStrat` runs the file and hands back what it returned.
  CHECK(member("GetStrat", {script::Value::integer(3), me}).value.as_integer() == 4);
  CHECK(member("GetStrat", {script::Value::integer(1), me}).value.as_integer() == 0);
  // **A negative return is clamped to zero**, which is the original's own
  // handling and what makes `if (Strat != 0)` safe.
  CHECK(member("GetStrat", {script::Value::integer(3), script::Value::integer(4)})
            .value.as_integer() == 0);
  // Synchronous: `Main.vs` is still the only live script.
  CHECK(f.scheduler.live_count() == 1);

  // Nothing is running yet, and `StratRunning` says so.
  CHECK(member("StratRunning", {script::Value::integer(3), me}).value.as_integer() == 0);

  // `RunStrat` writes the field and spawns the profile's file for that id.
  const std::int32_t guard = f.profile.constant(AiEnum::gaika_strategy, "GS_Guard");
  REQUIRE(guard != 0);
  CHECK(member("RunStrat", {script::Value::integer(3), me, script::Value::integer(guard)})
            .status == script::HostStatus::ok);
  CHECK(member("StratRunning", {script::Value::integer(3), me}).value.as_integer() == guard);
  CHECK(f.scheduler.live_count() == 2);
  const script::ScriptId spawned = f.ai.gaika_view(1)->find(3)->strat_script;
  CHECK(spawned != script::kNoScript);
  // The spawn is adopted, so `AIGetPlayer` inside `GS_Guard.vs` resolves.
  CHECK(f.ai.script_player(spawned, f.scheduler) == 2);
  // And it was handed **the node**, which the stand-in marks by priority.
  f.scheduler.run_ready();
  CHECK(f.ai.gaika_view(1)->find(3)->priority == 77);
  CHECK(f.ai.gaika_view(1)->find(2)->priority == 0);

  // **The field is self-cleaning**: kill the coroutine and the next read is 0.
  f.scheduler.kill(spawned);
  CHECK(member("StratRunning", {script::Value::integer(3), me}).value.as_integer() == 0);
  CHECK(f.ai.gaika_view(1)->find(3)->strat == 0);

  // Running one over another kills the first.
  CHECK(member("RunStrat", {script::Value::integer(3), me, script::Value::integer(guard)})
            .status == script::HostStatus::ok);
  const script::ScriptId first = f.ai.gaika_view(1)->find(3)->strat_script;
  CHECK(member("RunStrat", {script::Value::integer(3), me, script::Value::integer(guard)})
            .status == script::HostStatus::ok);
  CHECK(!f.scheduler.alive(first));
  CHECK(f.ai.gaika_view(1)->find(3)->strat_script != first);

  // **The spawn belongs to the argument's player, not the caller's.** Every
  // shipped site passes `AIPlayer` so the two agree there, but the argument is
  // explicit and the original puts the coroutine on *that* player's AI object.
  CHECK(f.ai.start(2, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  f.ai.seed_gaika_view(2, 4);
  CHECK(f.call(script::CallKind::member, "RunStrat",
               {script::Value::integer(3), script::Value::integer(3),
                script::Value::integer(guard)},
               root)
            .status == script::HostStatus::ok);
  const script::ScriptId theirs = f.ai.gaika_view(2)->find(3)->strat_script;
  CHECK(theirs != script::kNoScript);
  CHECK(f.ai.script_player(theirs, f.scheduler) == 3);
  f.scheduler.kill(theirs);

  // **Zero is how a caller stops one**, and it is written unconditionally.
  CHECK(member("RunStrat", {script::Value::integer(3), me, script::Value::integer(0)}).status ==
        script::HostStatus::ok);
  CHECK(f.ai.gaika_view(1)->find(3)->strat == 0);
  // Two `Main.vs` coroutines and nothing else: the second player's AI was
  // started above and its own strategy killed.
  CHECK(f.scheduler.live_count() == 2);
}

/// **The script is found on the argument player's search path, not the
/// caller's** -- the same rule `TSAdvHeroSkills` follows for the hero's owner,
/// and it takes two profiles carrying two copies of one file to see.
TEST(get_strat_runs_the_argument_players_copy_of_the_file) {
  Fixture f;
  AiProfile defensive;
  const auto parsed = AiProfile::parse(bytes_of(kProfileIni));
  REQUIRE(parsed.ok());
  defensive = parsed.value();
  f.ai.add_profile("DEFENSIVE", &defensive, "DATA\\AI\\DEFENSIVE\\");

  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n",
                "DATA/AI/DEFENSIVE/MAIN.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, "// int, GAIKA gaika, int idPlayer\nreturn 1;\n",
                "DATA/AI/GETGAIKASTRAT.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry, "// int, GAIKA gaika, int idPlayer\nreturn 2;\n",
                "DATA/AI/DEFENSIVE/GETGAIKASTRAT.VS") != script::kNoChunk);

  CHECK(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  CHECK(f.ai.start(1, "DEFENSIVE", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  f.ai.seed_gaika_view(0, 4);
  f.ai.seed_gaika_view(1, 4);
  // The caller is player 0's root throughout.
  const script::ScriptId caller = f.ai.player_ai(0)->root;

  CHECK(f.call(script::CallKind::member, "GetStrat",
               {script::Value::integer(1), script::Value::integer(1)}, caller)
            .value.as_integer() == 1);
  CHECK(f.call(script::CallKind::member, "GetStrat",
               {script::Value::integer(1), script::Value::integer(2)}, caller)
            .value.as_integer() == 2);
}

/// `CalcPriority` is the same trampoline with **its arguments the other way
/// round**, which is the corpus's own asymmetry: `CALCGAIKAPRIORITY.VS` is
/// declared `int, int idPlayer, GAIKA g` and `GETGAIKASTRAT.VS` is declared
/// `int, GAIKA gaika, int idPlayer`.
TEST(calc_priority_marshals_the_player_first) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// int, int idPlayer, GAIKA g\n"
                "return idPlayer * 1000 + g;\n",
                "DATA/AI/CALCGAIKAPRIORITY.VS") != script::kNoChunk);
  CHECK(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(1)->root;
  f.ai.seed_gaika_view(1, 4);

  // Player 2 and node 3: a body that swapped them would answer 3002.
  CHECK(f.call(script::CallKind::member, "CalcPriority",
               {script::Value::integer(3), script::Value::integer(2)}, root)
            .value.as_integer() == 2003);
  // And it keeps the full 32 bits where `GetStrat` truncates.
  CHECK(f.call(script::CallKind::member, "CalcPriority",
               {script::Value::integer(4), script::Value::integer(9)}, root)
            .value.as_integer() == 9004);
}

/// The `AreaAI*` writers: **a named map area, and one write per node whose
/// centre falls inside it.**
TEST(area_ai_writes_reach_the_nodes_inside_the_named_shape_and_no_others) {
  Fixture f;
  // Two settlements far apart, so the node table has two nodes with two
  // centres and a small circle can hold one of them.
  const SettlementId near = f.make_settlement(1);
  const SettlementId away = f.make_settlement(1);
  // **Captured by value.** `SettlementStore` holds its rows in a vector, so
  // creating the third settlement below moves the first two.
  ObjectId a_object = kNoObject;
  ObjectId b_object = kNoObject;
  {
    const Settlement* a = f.economy.settlements().find(near);
    const Settlement* b = f.economy.settlements().find(away);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    a_object = a->object;
    b_object = b->object;
    CHECK(f.world.set_position(a->anchor, Point{1000, 1000}));
    CHECK(f.world.set_position(b->anchor, Point{9000, 9000}));
  }
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  REQUIRE(f.world.gaika().count() == 2);
  const GaikaId inside = f.world.gaika().for_settlement(a_object);
  const GaikaId outside = f.world.gaika().for_settlement(b_object);
  REQUIRE(inside != kNoGaika);
  REQUIRE(outside != kNoGaika);

  const ObjectId marker = f.world.spawn(imperivm::core::NativeClass::area, nullptr);
  CHECK(f.world.set_position(marker, Point{1000, 1000}));
  CHECK(f.world.named_objects().bind("A_NearBrit3", marker));
  CHECK(f.areas.areas().bind(marker, AreaShape::of_circle(Point{1000, 1000}, 900)));

  f.ai.seed_gaika_view(1, f.world.gaika().count());
  const GaikaView* view = f.ai.gaika_view(1);
  REQUIRE(view != nullptr);
  const auto call = [&](const char* name, std::int32_t value) {
    return f.call(script::CallKind::free_function, name,
                  {script::Value::string("A_NearBrit3"), script::Value::integer(2),
                   script::Value::integer(value)},
                  script::kNoScript);
  };

  CHECK(call("AreaAINoRecruit", 1).status == script::HostStatus::ok);
  CHECK((view->find(inside)->flags & kLaikaNoRecruit) != 0);
  CHECK((view->find(outside)->flags & kLaikaNoRecruit) == 0);
  // And it clears, which is what the boolean is for.
  CHECK(call("AreaAINoRecruit", 0).status == script::HostStatus::ok);
  CHECK((view->find(inside)->flags & kLaikaNoRecruit) == 0);

  // **`AreaAIMaxPriority` does not write the boolean.** True is 100 --
  // `MaxGAIKAPriority`, the value `PRIORITIZE.VS` refuses to lower a node
  // from -- and false is half of it. It goes through `SetPriority`, so the
  // view re-sorts and `Prioritized` is set.
  CHECK(call("AreaAIMaxPriority", 1).status == script::HostStatus::ok);
  CHECK(view->find(inside)->priority == kMaxGaikaPriority);
  CHECK((view->find(inside)->flags & kLaikaPrioritized) != 0);
  CHECK(view->ranked(1) == inside);
  CHECK(view->find(outside)->priority == 0);
  CHECK(call("AreaAIMaxPriority", 0).status == script::HostStatus::ok);
  CHECK(view->find(inside)->priority == kMaxGaikaPriority / 2);

  // `AreaAISetAttackOptimism` writes the number it is given, over the 100 a
  // fresh record starts at.
  CHECK(view->find(inside)->optimism == kDefaultGaikaOptimism);
  CHECK(call("AreaAISetAttackOptimism", 300).status == script::HostStatus::ok);
  CHECK(view->find(inside)->optimism == 300);
  CHECK(view->find(outside)->optimism == kDefaultGaikaOptimism);

  // **The circle test is the query rule and not the sampler's.** A node at
  // (540, 721) from the centre is 811,441 squared units away, which is more
  // than `r * r` (810,000) and less than `(r + 1) * (r + 1)` (811,801) -- the
  // one-unit shell where the two rules disagree. The query rule excludes it.
  const SettlementId rim = f.make_settlement(1);
  ObjectId c_object = kNoObject;
  {
    const Settlement* c = f.economy.settlements().find(rim);
    REQUIRE(c != nullptr);
    c_object = c->object;
    CHECK(f.world.set_position(c->anchor, Point{1000 + 540, 1000 + 721}));
  }
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  f.ai.seed_gaika_view(1, f.world.gaika().count());
  const GaikaId shell = f.world.gaika().for_settlement(c_object);
  REQUIRE(shell != kNoGaika);
  REQUIRE(f.ai.gaika_view(1)->find(shell) != nullptr);
  CHECK(call("AreaAISetAttackOptimism", 250).status == script::HostStatus::ok);
  CHECK(f.ai.gaika_view(1)->find(shell)->optimism == kDefaultGaikaOptimism);
  CHECK(f.ai.gaika_view(1)->find(f.world.gaika().for_settlement(a_object))->optimism == 250);

  // A name that matches nothing does nothing, and is not a refusal -- the map
  // sequence that calls these has no way to test the result.
  const script::HostOutcome miss =
      f.call(script::CallKind::free_function, "AreaAINoRecruit",
             {script::Value::string("A_Nowhere"), script::Value::integer(2),
              script::Value::integer(1)},
             script::kNoScript);
  CHECK(miss.status == script::HostStatus::ok);
  CHECK((view->find(inside)->flags & kLaikaNoRecruit) == 0);

  // And so does a player with no view of its own.
  CHECK(f.call(script::CallKind::free_function, "AreaAINoRecruit",
               {script::Value::string("A_NearBrit3"), script::Value::integer(4),
                script::Value::integer(1)},
               script::kNoScript)
            .status == script::HostStatus::ok);
  CHECK((view->find(inside)->flags & kLaikaNoRecruit) == 0);
}

// --------------------------------------------------------------------------
// the search path, the difficulty argument, and the table discipline
// --------------------------------------------------------------------------

TEST(find_script_prefers_the_players_own_profile_directory) {
  // `Scheduler::find_chunk` documents that a bare `Main.vs` is ambiguous
  // between the four copies that ship and says the fix is a per-player search
  // path rooted at the personality directory. This is that fix.
  Fixture f;
  AiProfile defensive;
  const auto parsed = AiProfile::parse(bytes_of(kProfileIni));
  REQUIRE(parsed.ok());
  defensive = parsed.value();
  f.ai.add_profile("DEFENSIVE", &defensive, "DATA\\AI\\DEFENSIVE\\");

  const std::uint32_t root_main =
      build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS");
  const std::uint32_t overlay_main =
      build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/DEFENSIVE/MAIN.VS");
  REQUIRE(root_main != script::kNoChunk);
  REQUIRE(overlay_main != script::kNoChunk);

  CHECK(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  CHECK(f.ai.find_script(f.scheduler, 0, "Main.vs") == root_main);

  CHECK(f.ai.start(1, "DEFENSIVE", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  CHECK(f.ai.find_script(f.scheduler, 1, "Main.vs") == overlay_main);
  // ...and a file the overlay does not carry still resolves, to the root copy.
  CHECK(f.ai.find_script(f.scheduler, 1, "MissingFromOverlay.vs") == script::kNoChunk);
}

TEST(ai_start_difficulty_is_the_argument_space_not_the_attribute_space) {
  // The core does `inc ebx` and *then* validates 1..3, so the argument runs
  // 0..2. `playerdata/@difficulty` reaches 3, so it cannot be the same number,
  // and conflating them would silently shift every AI variable one overlay.
  CHECK(ai_start_difficulty(0) == AiDifficulty::easy);
  CHECK(ai_start_difficulty(1) == AiDifficulty::normal);
  CHECK(ai_start_difficulty(2) == AiDifficulty::hard);
  CHECK(ai_start_difficulty(3) == AiDifficulty::none);
  CHECK(ai_start_difficulty(-1) == AiDifficulty::none);

  CHECK(ai_difficulty_overlay(0) == AiDifficulty::none);
  CHECK(ai_difficulty_overlay(3) == AiDifficulty::hard);
}

TEST(ai_start_players_starts_only_computer_slots) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  f.world.players().setup(0).control = PlayerControl::human;
  f.make_computer(1);
  f.make_computer(2, "MISSING");  // a profile nobody registered
  f.world.players().setup(3).control = PlayerControl::both;

  // The manager does not exist until the bootstrap runs -- and then it does
  // whether or not a player starts. See `AiSystem::manager_started`.
  CHECK(!f.ai.manager_started());
  std::vector<std::pair<PlayerId, AiStartStatus>> refused;
  CHECK(ai_start_players(f.world, f.scheduler, &refused) == 1);
  CHECK(f.ai.manager_started());
  CHECK(f.ai.player_ai(1)->active);
  CHECK(!f.ai.player_ai(0)->active);
  CHECK(!f.ai.player_ai(3)->active);  // `Both` is decided at match setup, not here
  REQUIRE(refused.size() == 1);
  CHECK(refused[0].first == 2);
  CHECK(refused[0].second == AiStartStatus::unknown_profile);

  // Idempotent: a second sweep does not restart the one already running.
  CHECK(ai_start_players(f.world, f.scheduler) == 0);
}

TEST(ai_contributes_nothing_to_the_world_hash) {
  // `aihash` is zero in all nine desync dumps: the shipped build kept the AI
  // out of its determinism contract. An `AiSystem` that started folding state
  // in would desynchronise against the original rather than agree with it.
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  std::uint64_t accumulator = 0x1234'5678'9abc'def0ull;
  const std::uint64_t before = accumulator;
  f.ai.hash(accumulator);
  CHECK(accumulator == before);

  CHECK(f.ai.start(1, "", AiDifficulty::hard, f.scheduler) == AiStartStatus::ok);
  f.ai.hash(accumulator);
  CHECK(accumulator == before);
}

// --------------------------------------------------------------------------
// the AI-helper runner
// --------------------------------------------------------------------------

namespace {

/// A `ScriptLibrary` that compiles from a fixed table of sources, so a test can
/// exercise the compile-on-demand path without an installation behind it.
///
/// This is the seam `RunAIHelper` needs and nothing else in the engine does:
/// its file name comes from a running script, so there is no manifest to prime
/// from. See `sim/host_context.hpp`.
struct FakeLibrary : ScriptLibrary {
  script::Scheduler* scheduler = nullptr;
  const script::HostRegistry* registry = nullptr;
  std::vector<std::pair<std::string, std::string>> sources;
  /// Every path that was asked for, in order, so a test can assert *which*
  /// file a call went looking for rather than only that it found one.
  std::vector<std::string> asked;

  std::uint32_t chunk_for(std::string_view path) override {
    asked.emplace_back(path);
    const std::uint32_t existing = scheduler->find_chunk_exact(path);
    if (existing != script::kNoChunk) return existing;
    for (const auto& [name, source] : sources) {
      // Case and separator folded, the way both real resolvers fold them.
      if (name.size() != path.size()) continue;
      bool same = true;
      for (std::size_t i = 0; i < name.size(); ++i) {
        const auto fold = [](char c) {
          if (c == '\\') return '/';
          return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
        };
        if (fold(name[i]) != fold(path[i])) { same = false; break; }
      }
      if (!same) continue;
      return build(*scheduler, *registry, source, name.c_str());
    }
    return script::kNoChunk;
  }
};

/// A helper that never returns on its own, so "still running" is a real state
/// rather than a race with the first pass.
constexpr std::string_view kForeverHelper =
    "// void, str GroupName, str Target\n"
    "while (1) { Sleep(1000); }\n";

/// The same, but it only settles in if it was actually handed its parameters.
///
/// Without this a body that dropped arguments 2..N entirely passes every other
/// case here: all four shipped helpers take `GroupName` and `Target`, and a
/// helper that never reads them behaves identically either way.
constexpr std::string_view kPickyHelper =
    "// void, str GroupName, str Target\n"
    "if (Target != \"S_Utica\") return;\n"
    "while (1) { Sleep(1000); }\n";

}  // namespace

/// `RunAIHelper` finds its file by the path `gbr.exe` formats, compiles it on
/// demand, and records the coroutine under the caller's own key.
///
/// The three things this is built to catch, each of which was a plausible way
/// to write it wrong:
///
///   1. keying the table on the *file* rather than on argument 1 -- which would
///      make two helpers of one file collide, and 275 of the 285 shipped sites
///      pass a distinct name for exactly that reason;
///   2. building the path with the pack's convention (`DATA\AI HELPERS\...`)
///      rather than the executable's (`data/ai helpers/...`); and
///   3. resolving through `Scheduler::find_chunk`, whose basename fallback
///      would match `DATA\SUBAI\TOWNHALL_BEHAVIOR_GUARD.VS` for `"guard"`.
TEST(run_ai_helper_compiles_by_path_and_keys_by_the_callers_name) {
  Fixture f;
  FakeLibrary library;
  library.scheduler = &f.scheduler;
  library.registry = &f.registry;
  library.sources.emplace_back("DATA\\AI HELPERS\\GUARD.VS", std::string(kForeverHelper));
  // The decoy: same basename, different directory. A basename fallback would
  // find this one, and it is what ten files in `data.pak` look like.
  library.sources.emplace_back("DATA\\SUBAI\\TOWNHALL_BEHAVIOR_GUARD.VS",
                               std::string("// void\n"));
  f.context.library = &library;

  CHECK(f.ai.helpers().empty());
  const script::HostOutcome out =
      f.call(script::CallKind::free_function, "RunAIHelper",
             {script::Value::string("GUARD_Utica"), script::Value::string("guard"),
              script::Value::string("G_Defenders"), script::Value::string("S_Utica")},
             script::kNoScript);
  CHECK(out.status == script::HostStatus::ok);

  // The path is the executable's format string, character for character.
  REQUIRE(library.asked.size() == 1);
  CHECK(library.asked[0] == "data/ai helpers/guard.vs");

  // One entry, under the caller's key and not under the file's.
  REQUIRE(f.ai.helpers().size() == 1);
  CHECK(f.ai.helpers()[0].name == "GUARD_Utica");
  CHECK(f.ai.helper("GUARD_Utica") != script::kNoScript);
  CHECK(f.ai.helper("guard") == script::kNoScript);
  CHECK(f.scheduler.alive(f.ai.helper("GUARD_Utica")));
  // And the decoy was not what ran.
  CHECK(f.scheduler.live_count() == 1);
}

/// The two predicates, and the branch that is not there.
///
/// `IsAIHelperRunning` consults the table and only the table (0x004d1cd0 is a
/// `find` and a comparison against `end()`), and `RunAIHelper` on a live key
/// has **no** already-running branch: 0x004d3268 stores over whatever
/// `operator[]` returned. So the second call starts a second coroutine and the
/// first becomes unreachable by name while staying alive.
TEST(a_second_run_under_one_name_orphans_the_first_helper) {
  Fixture f;
  FakeLibrary library;
  library.scheduler = &f.scheduler;
  library.registry = &f.registry;
  library.sources.emplace_back("DATA\\AI HELPERS\\GUARD.VS", std::string(kForeverHelper));
  f.context.library = &library;

  const auto run = [&]() {
    return f.call(script::CallKind::free_function, "RunAIHelper",
                  {script::Value::string("G"), script::Value::string("guard"),
                   script::Value::string("a"), script::Value::string("b")},
                  script::kNoScript);
  };
  const auto running = [&](const char* name) {
    return f.call(script::CallKind::free_function, "IsAIHelperRunning",
                  {script::Value::string(name)}, script::kNoScript)
               .value.as_integer() != 0;
  };

  CHECK(!running("G"));
  CHECK(!running("never-inserted"));  // an unknown key is false, not a refusal
  (void)run();
  CHECK(running("G"));
  const script::ScriptId first = f.ai.helper("G");

  (void)run();
  const script::ScriptId second = f.ai.helper("G");
  CHECK(second != first);
  CHECK(f.ai.helpers().size() == 1);        // one slot...
  CHECK(f.scheduler.live_count() == 2);     // ...and two coroutines
  CHECK(f.scheduler.alive(first));          // the orphan is still running
}

/// `StopAIHelper` kills the coroutine and drops the entry; an unknown key is a
/// complete no-op, with no diagnostic and no trap (0x004d3375).
TEST(stop_ai_helper_kills_the_named_one_and_ignores_the_rest) {
  Fixture f;
  FakeLibrary library;
  library.scheduler = &f.scheduler;
  library.registry = &f.registry;
  library.sources.emplace_back("DATA\\AI HELPERS\\GUARD.VS", std::string(kForeverHelper));
  f.context.library = &library;

  const auto run = [&](const char* key) {
    return f.call(script::CallKind::free_function, "RunAIHelper",
                  {script::Value::string(key), script::Value::string("guard"),
                   script::Value::string("a"), script::Value::string("b")},
                  script::kNoScript);
  };
  const auto stop = [&](const char* key) {
    return f.call(script::CallKind::free_function, "StopAIHelper",
                  {script::Value::string(key)}, script::kNoScript);
  };

  (void)run("A");
  (void)run("B");
  const script::ScriptId a = f.ai.helper("A");
  const script::ScriptId b = f.ai.helper("B");
  REQUIRE(a != script::kNoScript && b != script::kNoScript);

  // An unknown key does nothing at all -- not to the table, not to anybody
  // else's coroutine.
  CHECK(stop("C").status == script::HostStatus::ok);
  CHECK(f.ai.helpers().size() == 2);
  CHECK(f.scheduler.alive(a) && f.scheduler.alive(b));

  CHECK(stop("A").status == script::HostStatus::ok);
  CHECK(!f.scheduler.alive(a));
  CHECK(f.scheduler.alive(b));
  CHECK(f.ai.helper("A") == script::kNoScript);
  REQUIRE(f.ai.helpers().size() == 1);
  CHECK(f.ai.helpers()[0].name == "B");
}

/// A helper that ends on its own leaves the table only when the reap runs.
///
/// This is the half that cannot be folded into `IsAIHelperRunning`: the
/// original erases from a completion hook (0x004d2160), so between a helper's
/// last statement and the next erase there is no window in which the predicate
/// answers true for a dead one. Polling reproduces that as long as the poll is
/// once a turn -- and this is the case that would pass just as well if
/// `IsAIHelperRunning` asked the scheduler instead, which is why it asserts the
/// table directly.
TEST(the_helper_table_is_reaped_when_a_helper_ends) {
  Fixture f;
  FakeLibrary library;
  library.scheduler = &f.scheduler;
  library.registry = &f.registry;
  library.sources.emplace_back("DATA\\AI HELPERS\\GUARD.VS", std::string(kForeverHelper));
  // One that returns immediately.
  library.sources.emplace_back("DATA\\AI HELPERS\\ONCE.VS",
                               std::string("// void, str GroupName, str Target\nreturn;\n"));
  f.context.library = &library;

  const auto run = [&](const char* key, const char* file) {
    return f.call(script::CallKind::free_function, "RunAIHelper",
                  {script::Value::string(key), script::Value::string(file),
                   script::Value::string("a"), script::Value::string("b")},
                  script::kNoScript);
  };
  (void)run("forever", "guard");
  (void)run("brief", "once");
  REQUIRE(f.ai.helpers().size() == 2);

  const auto running = [&](const char* name) {
    return f.call(script::CallKind::free_function, "IsAIHelperRunning",
                  {script::Value::string(name)}, script::kNoScript)
               .value.as_integer() != 0;
  };

  (void)f.scheduler.advance(100);
  // Still there: the reap has not run. A `reap_helpers` folded into the
  // predicate would already have dropped it.
  CHECK(f.ai.helpers().size() == 2);
  // **And the predicate still says true**, which is the whole of why it must
  // read the table rather than ask the scheduler. `brief` has ended; its entry
  // has not been erased yet; 0x004d1cd0 is a `find` and a comparison against
  // `end()` and nothing else, so between the last statement of a helper and
  // the erase that follows it the original answers true. Asking
  // `Scheduler::alive` here would agree with the table on every other case in
  // this file and disagree on exactly this one.
  CHECK(running("brief"));
  CHECK(running("forever"));

  f.ai.reap_helpers(f.scheduler);
  REQUIRE(f.ai.helpers().size() == 1);
  CHECK(f.ai.helpers()[0].name == "forever");
  CHECK(!running("brief"));
  CHECK(running("forever"));
  // And the live one survives the reap, which is the assertion the sequence
  // runner's own reap test was missing for a while.
  CHECK(f.scheduler.alive(f.ai.helper("forever")));
}

/// The helper's parameters reach it. Arguments 2..N of `RunAIHelper` are the
/// spawned script's own, in order (0x004d309e for the arity-4 case).
TEST(a_helper_is_handed_the_arguments_after_its_name_and_file) {
  Fixture f;
  FakeLibrary library;
  library.scheduler = &f.scheduler;
  library.registry = &f.registry;
  library.sources.emplace_back("DATA\\AI HELPERS\\PICKY.VS", std::string(kPickyHelper));
  f.context.library = &library;

  const auto run = [&](const char* key, const char* group, const char* target) {
    return f.call(script::CallKind::free_function, "RunAIHelper",
                  {script::Value::string(key), script::Value::string("picky"),
                   script::Value::string(group), script::Value::string(target)},
                  script::kNoScript);
  };

  (void)run("wanted", "G", "S_Utica");
  (void)run("other", "G", "S_Zama");
  (void)f.scheduler.advance(10);
  // The one whose fourth argument matched is still going; the one whose did
  // not returned at its first statement. A body that dropped the parameters
  // would leave both of them returning.
  CHECK(f.scheduler.alive(f.ai.helper("wanted")));
  CHECK(!f.scheduler.alive(f.ai.helper("other")));
}

/// A missing helper file is silent: nothing spawns, nothing is recorded, and
/// the calling script runs on. `gbr.exe` formats "AI helper %s not found"
/// (0x007b9074) through 0x00686eb0, which is a bare `ret` in retail.
TEST(a_missing_helper_file_records_nothing_and_refuses_nothing) {
  Fixture f;
  FakeLibrary library;
  library.scheduler = &f.scheduler;
  library.registry = &f.registry;
  f.context.library = &library;

  const script::HostOutcome out =
      f.call(script::CallKind::free_function, "RunAIHelper",
             {script::Value::string("G"), script::Value::string("nosuchfile"),
              script::Value::string("a"), script::Value::string("b")},
             script::kNoScript);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(f.ai.helpers().empty());
  CHECK(f.scheduler.live_count() == 0);
  CHECK(library.asked.size() == 1 && library.asked[0] == "data/ai helpers/nosuchfile.vs");

  // With no library at all -- a session that never wired one -- the same call
  // has to degrade rather than dereference.
  f.context.library = nullptr;
  CHECK(f.call(script::CallKind::free_function, "RunAIHelper",
               {script::Value::string("G"), script::Value::string("guard"),
                script::Value::string("a"), script::Value::string("b")},
               script::kNoScript)
            .status == script::HostStatus::ok);
  CHECK(f.ai.helpers().empty());
}

/// The table is saved. `AiSystem::hash` is a no-op -- `aihash` is zero in all
/// nine dumps and the standing decision is to respect that -- so nothing else
/// would notice this section going missing.
TEST(the_helper_table_survives_a_save) {
  Fixture f;
  f.ai.set_helper("zulu", 7);
  f.ai.set_helper("alpha", 3);
  f.ai.set_helper("mike", 5);
  // Ascending by name, which is `std::map`'s order and what `helper`
  // binary-searches on.
  REQUIRE(f.ai.helpers().size() == 3);
  CHECK(f.ai.helpers()[0].name == "alpha");
  CHECK(f.ai.helpers()[2].name == "zulu");
  // Overwrite, no branch: 0x004d3268 stores over what was there.
  f.ai.set_helper("mike", 11);
  CHECK(f.ai.helpers().size() == 3);
  CHECK(f.ai.helper("mike") == 11);

  std::vector<std::byte> bytes;
  f.ai.serialize(bytes);
  AiSystem loaded;
  REQUIRE(loaded.deserialize(bytes).ok());
  REQUIRE(loaded.helpers().size() == 3);
  CHECK(loaded.helper("alpha") == 3);
  CHECK(loaded.helper("mike") == 11);
  CHECK(loaded.helper("zulu") == 7);
  CHECK(loaded.helper("nobody") == script::kNoScript);
}

TEST(ai_registration_overwrites_nothing) {
  // `HostRegistry::define` replaces silently, so a domain added after the
  // manifest can take an entry point away from one already in it and every
  // existing test keeps passing. Measured the way `test_host_setup.cpp`
  // measures it: alone, and as a delta on the full surface.
  script::HostRegistry alone;
  script::declare_shipped_surface(alone);
  const std::size_t solo = register_ai_host(alone);

  script::HostRegistry full;
  imperivm::test::define_all_except("ai", full);
  const std::size_t everyone = full.implemented();
  const std::size_t with_ai = register_ai_host(full);
  // 14 + the seven node-table readers the partition decision unblocked, less
  // `GetGAIKA/1`, which was already one of the 14 and is now the same body's
  // integer arm rather than a second registration, + the four recruiter
  // trampolines, + the eleven readers and writers of the per-player view, +
  // the strategy trio, `CalcPriority`, the three `AreaAI*` writers,
  // `GetDistToPlayers/4`, `BestTargetInGAIKA/0`, `Empty/0` and the
  // `MinNeed`/`MaxNeed` pair, `SS_STR/1` and `SetMAIKA/3`, and
  // `AIGetSkillToDevelop/2`, the fifth trampoline, the two spell-siting
  // searches `BestMDPos/5` and `BestProtPos/4`, `ControlledNeighbors/1`,
  // `CanExplore/1` and `Recruit/4`, `GetDestPoint/1` and
  // `PrepareAiTransportShip/5`.
  CHECK(solo == 77);
  CHECK(with_ai == solo);
  CHECK(everyone > 0);

  // And every name it claims was declared by the shipped inventory rather than
  // introduced here. `AIStart/3` and `AIStop/1` are part of that inventory --
  // they were thought absent while the inventory covered only `data.pak`, and
  // the containers call them 20 and 1 times respectively.
  CHECK(alone.find(script::CallKind::free_function, "AIStart", 3) != script::kUnresolvedHost);
  CHECK(alone.find(script::CallKind::free_function, "AIStop", 1) != script::kUnresolvedHost);
  // `AIStart/3` is this domain's now -- 20 container sites is a call site count
  // like any other, and the bootstrap it needs has been here since the domain
  // landed. `AIStop/1` is this domain's too, one container site and the trap
  // message it used to print.
  CHECK(alone.entry(alone.find(script::CallKind::free_function, "AIStart", 3)).fn != nullptr);
  CHECK(alone.entry(alone.find(script::CallKind::free_function, "AIStop", 1)).fn != nullptr);
  // `GAIKACount` **is** this domain's now. It was withheld while a count of
  // zero would have spun `PRIORITIZE.VS`; there is a table to count since
  // `sim/gaika.hpp`'s partition decision.
  const std::uint32_t count = full.find(script::CallKind::free_function, "GAIKACount", 0);
  REQUIRE(count != script::kUnresolvedHost);
  CHECK(full.entry(count).fn != nullptr);
  // `LAIKA/2` is this domain's now: the per-player node state it reads is
  // `AiSystem`'s, and `AiPlayer` holds it.
  const std::uint32_t laika = full.find(script::CallKind::free_function, "LAIKA", 2);
  REQUIRE(laika != script::kUnresolvedHost);
  CHECK(full.entry(laika).fn != nullptr);
}

// --------------------------------------------------------------------------
// AIStart, from a script
// --------------------------------------------------------------------------

/// The host entry point, which is the bootstrap above reached the other way.
///
/// Twenty container sites, all three-argument, all naming `"DEFAULT"`, and
/// eleven of them passing `GetDifficulty()` rather than a literal.
TEST(ai_start_host_entry_starts_the_named_player) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  f.ai.add_profile("DEFAULT", &f.profile);

  // `AIStart(5, "DEFAULT", 2)`: player 5 is **1-based**, so slot 4, and
  // difficulty 2 is Hard.
  const script::HostOutcome out =
      f.call(script::CallKind::free_function, "AIStart",
             {script::Value::integer(5), script::Value::string("DEFAULT"),
              script::Value::integer(2)},
             script::kNoScript);
  CHECK(out.status == script::HostStatus::ok);
  const AiPlayer* ai = f.ai.player_ai(4);
  REQUIRE(ai != nullptr);
  CHECK(ai->active);
  CHECK(ai->difficulty == AiDifficulty::hard);
  CHECK(f.ai.active_count() == 1);
  // Nobody else. A body that ignored the argument and started everyone, or
  // that read the player 0-based, fails here.
  CHECK(f.ai.player_ai(5) == nullptr || !f.ai.player_ai(5)->active);
  CHECK(f.ai.player_ai(0) == nullptr || !f.ai.player_ai(0)->active);

  // The name is folded on the way in, so any casing of a registered profile
  // reaches it -- and an unregistered one starts nothing and does not trap.
  const script::HostOutcome miss =
      f.call(script::CallKind::free_function, "AIStart",
             {script::Value::integer(6), script::Value::string("CHAOTIC"),
              script::Value::integer(1)},
             script::kNoScript);
  CHECK(miss.status == script::HostStatus::ok);
  CHECK(f.ai.active_count() == 1);
}

/// Every refusal the original makes is a **return**, not a trap: a mission that
/// names a player outside 1..16 or a difficulty outside 0..2 keeps running with
/// one fewer AI.
TEST(ai_start_host_entry_refuses_out_of_range_arguments_without_trapping) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  f.ai.add_profile("DEFAULT", &f.profile);

  const auto start = [&](std::int32_t player, const char* profile, std::int32_t difficulty) {
    return f.call(script::CallKind::free_function, "AIStart",
                  {script::Value::integer(player), script::Value::string(profile),
                   script::Value::integer(difficulty)},
                  script::kNoScript);
  };

  // `Function "AIStart": Player number should be between 1 and 16`.
  CHECK(start(0, "DEFAULT", 1).status == script::HostStatus::ok);
  CHECK(start(17, "DEFAULT", 1).status == script::HostStatus::ok);
  // `AIStart: invalid diffivulty` -- the core increments before validating
  // against 1..3, so 3 and -1 are both out.
  CHECK(start(1, "DEFAULT", 3).status == script::HostStatus::ok);
  CHECK(start(1, "DEFAULT", -1).status == script::HostStatus::ok);
  CHECK(f.ai.active_count() == 0);

  // And the three that are in range do start, so the four above are refusals
  // and not a fixture that could never have started anything.
  CHECK(start(1, "DEFAULT", 0).status == script::HostStatus::ok);
  CHECK(start(2, "DEFAULT", 1).status == script::HostStatus::ok);
  CHECK(start(16, "DEFAULT", 2).status == script::HostStatus::ok);
  CHECK(f.ai.active_count() == 3);
  REQUIRE(f.ai.player_ai(0) != nullptr);
  CHECK(f.ai.player_ai(0)->difficulty == AiDifficulty::easy);
  REQUIRE(f.ai.player_ai(15) != nullptr);
  CHECK(f.ai.player_ai(15)->difficulty == AiDifficulty::hard);
}

/// **The empty profile is refused by the script's entry point**, though the
/// fixture registers the root under `""` exactly as a session does: the
/// original's list, filled from `data/ai/`'s subdirectories, never holds an
/// empty name (0x00443190), and the core path that the root *does* reach --
/// a map's own `@AI`, a dropped seat -- does not go through this check.
TEST(ai_start_host_entry_refuses_the_empty_profile_the_core_path_accepts) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  const script::HostOutcome out =
      f.call(script::CallKind::free_function, "AIStart",
             {script::Value::integer(3), script::Value::string(""), script::Value::integer(1)},
             script::kNoScript);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(f.ai.active_count() == 0);
  CHECK(f.scheduler.live_count() == 0);
  // The same name through the core is the root profile, and starts.
  CHECK(ai_start(f.world, 2, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  CHECK(f.ai.active_count() == 1);
}

/// **A seat that already has an AI gets a new one** (0x00434cd0 destroys the
/// old object first), and a call the wrapper refuses leaves the old one
/// running, because the three checks come before the core.
TEST(ai_start_host_entry_restarts_a_seats_ai_and_a_refusal_keeps_it) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  f.ai.add_profile("DEFAULT", &f.profile);
  const auto start = [&](const char* profile, std::int32_t difficulty) {
    return f.call(script::CallKind::free_function, "AIStart",
                  {script::Value::integer(4), script::Value::string(profile),
                   script::Value::integer(difficulty)},
                  script::kNoScript);
  };
  CHECK(start("DEFAULT", 0).status == script::HostStatus::ok);
  const AiPlayer* ai = f.ai.player_ai(3);
  REQUIRE(ai != nullptr);
  const script::ScriptId first = ai->root;
  REQUIRE(first != script::kNoScript);
  CHECK(ai->difficulty == AiDifficulty::easy);

  // Refused three ways: the first AI is untouched, root and all.
  CHECK(start("CHAOTIC", 2).status == script::HostStatus::ok);
  CHECK(start("", 2).status == script::HostStatus::ok);
  CHECK(start("DEFAULT", 3).status == script::HostStatus::ok);
  CHECK(f.ai.player_ai(3)->root == first);
  CHECK(f.ai.player_ai(3)->difficulty == AiDifficulty::easy);
  CHECK(f.scheduler.alive(first));

  // Accepted: a new root, the old one gone, one AI still.
  CHECK(start("default", 2).status == script::HostStatus::ok);
  const AiPlayer* again = f.ai.player_ai(3);
  REQUIRE(again != nullptr);
  CHECK(again->active);
  CHECK(again->root != first);
  CHECK(again->difficulty == AiDifficulty::hard);
  CHECK(!f.scheduler.alive(first));
  CHECK(f.ai.active_count() == 1);
  CHECK(f.scheduler.live_count() == 1);
}

/// `sq.SendTo(g, n)` is the writer `Squad::ai_dest` never had.
///
/// It lives here rather than in `test_squad.cpp` because the entry point's
/// first refusal is *the player has no AI*, and only this file has a fixture
/// that can start one.
TEST(send_to_sets_both_destinations_and_refuses_a_squad_no_ai_can_move) {
  Fixture f;
  const std::uint32_t main_chunk =
      build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS");
  REQUIRE(main_chunk != script::kNoChunk);

  const SquadKey key = f.heroes.squads().create(1);
  Squad* squad = f.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  CHECK(squad->ai_dest == kNoGaika);
  CHECK(squad->order_dest == kNoGaika);

  const std::uint32_t index = f.registry.find(script::CallKind::member, "SendTo", 2);
  REQUIRE(index != script::kUnresolvedHost);
  REQUIRE(f.registry.entry(index).fn != nullptr);
  const auto send = [&](GaikaId node) {
    std::vector<script::Value> args{pack_squad(key), script::Value::integer(node),
                                    script::Value::integer(1)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &f.context;
    ctx.name = "SendTo";
    ctx.kind = script::CallKind::member;
    return f.registry.entry(index).fn(ctx).status;
  };

  // **A player whose AI is not running is not sent anywhere.** The slot exists
  // for every player in range either way -- that is the trap `Unit::AI` fell
  // into once -- so what the original's `[record+0x88]` means here is `active`.
  REQUIRE(f.ai.player_ai(1) != nullptr);
  CHECK(!f.ai.player_ai(1)->active);
  CHECK(send(5) == script::HostStatus::ok);
  CHECK(squad->ai_dest == kNoGaika);
  CHECK(squad->order_dest == kNoGaika);

  REQUIRE(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  REQUIRE(f.ai.player_ai(1)->active);

  // Both fields, and it replaces rather than accumulating.
  CHECK(send(5) == script::HostStatus::ok);
  CHECK(squad->ai_dest == 5);
  CHECK(squad->order_dest == 5);
  CHECK(send(7) == script::HostStatus::ok);
  CHECK(squad->ai_dest == 7);
  CHECK(squad->order_dest == 7);

  // Where the squad *is* is not this entry point's to say, and a node the
  // table does not have (this world has no nodes at all) moves nobody and
  // leaves its own destination alone -- the case below has a node.
  squad->gaika_in = 2;
  squad->dest_gaika = 3;
  CHECK(send(9) == script::HostStatus::ok);
  CHECK(squad->gaika_in == 2);
  CHECK(squad->dest_gaika == 3);

  // `SF_NOAI`: a squad a mission has pinned is not the AI's to move.
  squad->flags = static_cast<std::uint16_t>(squad->flags | kSquadFlagNoAi);
  CHECK(send(11) == script::HostStatus::ok);
  CHECK(squad->ai_dest == 9);
  CHECK(squad->order_dest == 9);
  squad->flags = static_cast<std::uint16_t>(squad->flags & ~kSquadFlagNoAi);
  CHECK(send(11) == script::HostStatus::ok);
  CHECK(squad->ai_dest == 11);

  // A handle naming no squad runs and writes nothing, rather than refusing.
  std::vector<script::Value> gone{pack_squad(SquadKey{99, 1}), script::Value::integer(3),
                                  script::Value::integer(1)};
  script::CallContext ctx;
  ctx.arguments = gone;
  ctx.user = &f.context;
  ctx.name = "SendTo";
  ctx.kind = script::CallKind::member;
  CHECK(f.registry.entry(index).fn(ctx).status == script::HostStatus::ok);
}

namespace {

/// A world with two settlements, their nodes built, a running AI for player 1
/// and one squad of three of its soldiers -- the ground `SendTo`'s drain is
/// tested on.
struct SendToBench {
  Fixture f;
  CommandSystem commands;
  GaikaId home = kNoGaika;
  GaikaId dest = kNoGaika;
  Point centre{};
  SquadKey key{};
  std::vector<ObjectId> soldiers;

  SendToBench() {
    REQUIRE(f.world.add_system(&commands));
    REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
    REQUIRE(f.ai.start(1, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
    const SettlementId near = f.make_settlement(1);
    const SettlementId far = f.make_settlement(2);
    const Settlement* a = f.economy.settlements().find(near);
    const Settlement* b = f.economy.settlements().find(far);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    const ObjectId mine = a->object;
    const ObjectId theirs = b->object;
    CHECK(f.world.set_position(a->anchor, Point{1000, 1000}));
    CHECK(f.world.set_position(b->anchor, Point{6000, 4000}));
    f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
    home = f.world.gaika().for_settlement(mine);
    dest = f.world.gaika().for_settlement(theirs);
    REQUIRE(home != kNoGaika);
    REQUIRE(dest != kNoGaika);
    centre = f.world.gaika().find(dest)->center;

    key = f.heroes.squads().create(1);
    for (int i = 0; i < 3; ++i) {
      const ObjectId id = f.world.spawn(imperivm::core::NativeClass::unit, nullptr);
      f.world.set_owner(id, 1);
      f.world.set_health(id, 100);
      CHECK(f.world.set_position(id, Point{1000 + 30 * i, 1100}));
      REQUIRE(f.heroes.squads().join(key, id));
      soldiers.push_back(id);
      (void)commands.set_command(f.world, id, "guard", Command{});
    }
    Squad* squad = f.heroes.squads().find(key);
    REQUIRE(squad != nullptr);
    squad->state = 7;
    squad->flags = kSquadFlagAdvChooser;
    squad->gaika_in = home;
  }

  script::HostStatus send(GaikaId node, std::int32_t priority = 1) {
    return send_squad(key, node, priority);
  }
  script::HostStatus send_squad(SquadKey which, GaikaId node, std::int32_t priority) {
    const script::HostOutcome out =
        f.call(script::CallKind::member, "SendTo",
               {pack_squad(which), gaika_value(node), script::Value::integer(priority)},
               script::kNoScript);
    if (out.status != script::HostStatus::ok && out.error != nullptr) std::printf("  %s\n", out.error);
    return out.status;
  }
  Squad& squad() { return *f.heroes.squads().find(key); }

  /// One more squad of player 1: a single guarding soldier at `at`, filed
  /// under `home`.
  SquadKey squad_at(Point at) {
    const SquadKey made = f.heroes.squads().create(1);
    const ObjectId id = f.world.spawn(imperivm::core::NativeClass::unit, nullptr);
    f.world.set_owner(id, 1);
    f.world.set_health(id, 100);
    CHECK(f.world.set_position(id, at));
    CHECK(f.heroes.squads().join(made, id));
    (void)commands.set_command(f.world, id, "guard", Command{});
    if (Squad* found = f.heroes.squads().find(made)) found->gaika_in = home;
    return made;
  }
  [[nodiscard]] std::string verb_of(SquadKey which) {
    const Squad* found = f.heroes.squads().find(which);
    if (found == nullptr || found->members.empty()) return {};
    return std::string(commands.command_name(found->members.front()));
  }

  /// The timer, at `now`; and the scheduler's pass the runner it spawns waits
  /// for. Driven by hand, so nothing here depends on when the AI acts.
  std::size_t drain(GameTime now) { return run_ai_orders(f.world, f.scheduler, nullptr, now); }
  void run_scripts() { (void)f.scheduler.run_ready(); }
  [[nodiscard]] const AiOrderQueue& queue() { return *f.heroes.squads().orders(1); }
  [[nodiscard]] const AiOrder& record_of(SquadKey which) {
    return queue().todo[static_cast<std::size_t>(f.heroes.squads().find(which)->order)];
  }
};

/// The two branches of the shipped `AIOSENDSQUAD.VS` that decide who moves --
/// a squad in the node is cleared, one elsewhere approaches -- with the state
/// and point written as literals, since a core test has no installation.
constexpr std::string_view kSendSquadStandIn =
    "// void, SquadList l, GAIKA g\n"
    "if (!l.Rewind()) return;\n"
    "l.Lock;\n"
    "while (true)\n"
    "{\n"
    "  if (l.Cur.GAIKAIn == g) l.Cur.ClrCmd(0, 0, 2);\n"
    "  else l.Cur.SetCmd(1, 0, 2, \"advance\", g.Center);\n"
    "  if (!l.Next()) break;\n"
    "}\n"
    "l.Unlock;\n";

}  // namespace

/// **`SendTo` posts; the AI's order drain carries it out**, by spawning
/// `data/ai/AIOSendSquad.vs` (0x00448b60, verb 1) with a `SquadList` of the
/// squad and the node. Until the timer drains it nobody moves; the drain takes
/// the record (priority 0), writes the squad's `SrcGAIKA` from its `GAIKAIn`,
/// and the runner it spawns -- the AI's, so `AIGetPlayer` answers its player --
/// moves the squad in the pass that follows. The squad's own `DestGAIKA` is
/// untouched, and the order keeps answering `OrderDest` after it is done.
TEST(send_to_posts_an_order_and_the_drain_runs_aio_send_squad_with_the_squad_and_the_node) {
  SendToBench bench;
  REQUIRE(build(bench.f.scheduler, bench.f.registry, kSendSquadStandIn,
                "DATA\\AI\\AIOSENDSQUAD.VS") != script::kNoChunk);
  bench.squad().src_gaika = 99;
  CHECK(bench.send(bench.dest, 30) == script::HostStatus::ok);
  {
    Squad& squad = bench.squad();
    CHECK(squad.order_dest == bench.dest);
    CHECK(squad.ai_dest == bench.dest);
    CHECK(squad.dest_gaika == kNoGaika);
    CHECK(squad_ai_dest(squad) == bench.dest);
    CHECK(squad.state == 7);
    for (const ObjectId id : bench.soldiers) CHECK(bench.commands.command_name(id) == "guard");
    const AiOrder& record = bench.record_of(bench.key);
    CHECK(record.verb == 1);
    CHECK(record.squad == bench.key.index);
    CHECK(record.node == bench.dest);
    CHECK(record.priority == 30);
  }

  CHECK(bench.drain(0) == 1);
  CHECK(bench.record_of(bench.key).priority == 0);
  const script::ScriptId runner = bench.queue().runner;
  REQUIRE(runner != script::kNoScript);
  CHECK(bench.f.ai.script_player(runner, bench.f.scheduler) == 2);
  CHECK(bench.squad().src_gaika == bench.home);
  CHECK(bench.squad().state == 7);  // spawned, not yet run

  bench.run_scripts();
  Squad& squad = bench.squad();
  CHECK(squad.state == 1);
  CHECK((squad.flags & kSquadFlagAdvChooser) == 0);
  for (const ObjectId id : bench.soldiers) {
    const CommandQueue* q = bench.commands.find(id);
    REQUIRE(q != nullptr);
    REQUIRE(q->size() == 1);
    CHECK(q->entries[0].verb == "advance");
    CHECK(q->entries[0].point == bench.centre);
  }
  CHECK(!bench.f.scheduler.alive(runner));
  CHECK(squad.order_dest == bench.dest);
  CHECK(squad.dest_gaika == kNoGaika);
}

/// **A squad sent to the node it already stands in moves nobody**: the script's
/// first branch is `ClrCmd(SS_IDLE, 0, SF_ADVCHOOSER)`, which ends what every
/// member is doing. This is the case that put a town's sentries on its town
/// hall when `SendTo` walked every member to the node's centre itself.
TEST(send_to_a_squad_already_in_the_node_stops_it_where_it_stands) {
  SendToBench bench;
  REQUIRE(build(bench.f.scheduler, bench.f.registry, kSendSquadStandIn,
                "DATA\\AI\\AIOSENDSQUAD.VS") != script::kNoChunk);
  CHECK(bench.send(bench.home) == script::HostStatus::ok);
  CHECK(bench.drain(0) == 1);
  bench.run_scripts();
  Squad& squad = bench.squad();
  CHECK(squad.order_dest == bench.home);
  CHECK(squad.state == 0);
  CHECK((squad.flags & kSquadFlagAdvChooser) == 0);
  for (const ObjectId id : bench.soldiers) {
    CHECK(bench.commands.command_name(id) != "guard");
    CHECK(bench.commands.command_name(id) != "advance");
  }
}

/// **With no such script the order is taken and nobody moves**: the movement
/// is the script's, so a world without the file has a destination and no
/// journey -- and the record is spent, as a spawn of a missing file spends it.
TEST(send_to_without_the_order_script_takes_the_order_and_moves_nobody) {
  SendToBench bench;
  CHECK(bench.send(bench.dest) == script::HostStatus::ok);
  CHECK(bench.drain(0) == 0);
  CHECK(bench.record_of(bench.key).priority == 0);
  CHECK(bench.queue().runner == script::kNoScript);
  bench.run_scripts();
  Squad& squad = bench.squad();
  CHECK(squad.order_dest == bench.dest);
  CHECK(squad.ai_dest == bench.dest);
  CHECK(squad.dest_gaika == kNoGaika);
  CHECK(squad.state == 7);
  for (const ObjectId id : bench.soldiers) CHECK(bench.commands.command_name(id) == "guard");
}

/// **The post** (0x004494c0): a squad sent again frees its old record first and
/// so takes the same slot back; another squad's order takes the next one; the
/// priority is the script's `n` in sixteen bits.
TEST(send_to_reposts_into_the_squads_own_slot_and_keeps_sixteen_bits_of_priority) {
  SendToBench bench;
  const SquadKey other = bench.squad_at(Point{4000, 1100});
  CHECK(bench.send(bench.dest, 5) == script::HostStatus::ok);
  CHECK(bench.send_squad(other, bench.dest, 5) == script::HostStatus::ok);
  CHECK(bench.squad().order == 0);
  CHECK(bench.f.heroes.squads().find(other)->order == 1);
  CHECK(bench.send(bench.home, 70000) == script::HostStatus::ok);
  CHECK(bench.squad().order == 0);
  CHECK(bench.queue().todo.size() == 2);
  CHECK(bench.record_of(bench.key).node == bench.home);
  CHECK(bench.record_of(bench.key).priority == static_cast<std::int16_t>(70000 - 65536));
  CHECK(bench.queue().first_free == -1);
}

/// **The timer** (0x0041d790): armed at no delay when the AI starts, it drains
/// one order whenever it is due and re-arms itself 500 from the moment it
/// fired. Two orders far apart are two drains, half a second apart.
TEST(the_order_drain_fires_when_due_and_rearms_half_a_second_after) {
  SendToBench bench;
  REQUIRE(build(bench.f.scheduler, bench.f.registry, kSendSquadStandIn,
                "DATA\\AI\\AIOSENDSQUAD.VS") != script::kNoChunk);
  const SquadKey other = bench.squad_at(Point{4000, 1100});
  CHECK(bench.send(bench.dest) == script::HostStatus::ok);
  CHECK(bench.send_squad(other, bench.dest, 1) == script::HostStatus::ok);
  CHECK(bench.queue().due == 0);

  CHECK(bench.drain(100) == 1);
  CHECK(bench.record_of(bench.key).priority == 0);
  CHECK(bench.record_of(other).priority == 1);
  CHECK(bench.queue().due == 600);
  bench.run_scripts();

  CHECK(bench.drain(599) == 0);
  CHECK(bench.record_of(other).priority == 1);
  CHECK(bench.queue().due == 600);

  CHECK(bench.drain(600) == 1);
  CHECK(bench.record_of(other).priority == 0);
  CHECK(bench.queue().due == 1100);
  bench.run_scripts();
  CHECK(bench.verb_of(other) == "advance");
}

/// **Slot 2** (0x0069f950): while the runner the last drain spawned is still
/// alive, the drain executes nothing -- the record it picked keeps waiting --
/// and the timer re-arms all the same.
TEST(the_order_drain_takes_nothing_while_the_last_runner_still_runs) {
  SendToBench bench;
  REQUIRE(build(bench.f.scheduler, bench.f.registry, kSendSquadStandIn,
                "DATA\\AI\\AIOSENDSQUAD.VS") != script::kNoChunk);
  const SquadKey other = bench.squad_at(Point{4000, 1100});
  CHECK(bench.send(bench.dest) == script::HostStatus::ok);
  CHECK(bench.send_squad(other, bench.dest, 1) == script::HostStatus::ok);

  CHECK(bench.drain(0) == 1);
  const script::ScriptId first = bench.queue().runner;
  REQUIRE(bench.f.scheduler.alive(first));
  CHECK(bench.drain(500) == 0);
  CHECK(bench.record_of(other).priority == 1);
  CHECK(bench.queue().runner == first);
  CHECK(bench.queue().due == 1000);

  bench.run_scripts();
  CHECK(bench.drain(1000) == 1);
  CHECK(bench.record_of(other).priority == 0);
  CHECK(bench.queue().runner != first);
}

/// **The pick** (0x00448ad0 with 0x004488e0): the larger `priority / 5` first,
/// and within one band the squad nearer `View`, which nothing moves from
/// (-1, -1) -- so a 5 near the corner goes before a 9 further out, and a 4 goes
/// last whatever its place.
TEST(the_order_drain_takes_the_larger_fifth_first_and_the_corner_within_one) {
  SendToBench bench;
  REQUIRE(build(bench.f.scheduler, bench.f.registry, kSendSquadStandIn,
                "DATA\\AI\\AIOSENDSQUAD.VS") != script::kNoChunk);
  const SquadKey near_corner = bench.squad_at(Point{500, 500});
  const SquadKey corner = bench.squad_at(Point{200, 200});
  CHECK(bench.send(bench.dest, 9) == script::HostStatus::ok);  // record 0, at (1000, 1100)
  CHECK(bench.send_squad(near_corner, bench.dest, 5) == script::HostStatus::ok);
  CHECK(bench.send_squad(corner, bench.dest, 4) == script::HostStatus::ok);

  CHECK(bench.drain(0) == 1);
  CHECK(bench.record_of(near_corner).priority == 0);
  CHECK(bench.record_of(bench.key).priority == 8);  // record 0 aged, band unchanged
  CHECK(bench.record_of(corner).priority == 4);
  bench.run_scripts();

  CHECK(bench.drain(500) == 1);
  CHECK(bench.record_of(bench.key).priority == 0);
  CHECK(bench.record_of(corner).priority == 4);
  bench.run_scripts();

  CHECK(bench.drain(1000) == 1);
  CHECK(bench.record_of(corner).priority == 0);
}

/// A tie in band and in distance keeps the record the walk met first: the
/// comparison is strict.
TEST(the_order_drain_breaks_a_full_tie_by_the_lower_slot) {
  SendToBench bench;
  REQUIRE(build(bench.f.scheduler, bench.f.registry, kSendSquadStandIn,
                "DATA\\AI\\AIOSENDSQUAD.VS") != script::kNoChunk);
  const SquadKey first = bench.squad_at(Point{3000, 3000});
  const SquadKey second = bench.squad_at(Point{3000, 3000});
  // Two nodes, so the second is not swept into the first's list.
  CHECK(bench.send_squad(first, bench.dest, 1) == script::HostStatus::ok);
  CHECK(bench.send_squad(second, bench.home, 1) == script::HostStatus::ok);
  CHECK(bench.drain(0) == 1);
  CHECK(bench.record_of(first).priority == 0);
  CHECK(bench.record_of(second).priority == 1);
}

/// **The batch** (0x00448be9..0x00448cbc): every other order still waiting for
/// the same node whose squad's front member stands within 240 of this one's
/// goes in the same list, taken the same way -- 239 is in and 240 is out, and
/// an order for another node is out at any distance.
TEST(the_order_drain_sends_the_squads_bound_for_one_node_within_240_in_one_list) {
  SendToBench bench;
  REQUIRE(build(bench.f.scheduler, bench.f.registry, kSendSquadStandIn,
                "DATA\\AI\\AIOSENDSQUAD.VS") != script::kNoChunk);
  // The bench squad's front member stands at (1000, 1100).
  const SquadKey within = bench.squad_at(Point{1239, 1100});
  const SquadKey edge = bench.squad_at(Point{1000, 1340});
  const SquadKey elsewhere = bench.squad_at(Point{1010, 1100});
  bench.f.heroes.squads().find(edge)->src_gaika = 77;
  CHECK(bench.send(bench.dest) == script::HostStatus::ok);
  CHECK(bench.send_squad(within, bench.dest, 1) == script::HostStatus::ok);
  CHECK(bench.send_squad(edge, bench.dest, 1) == script::HostStatus::ok);
  CHECK(bench.send_squad(elsewhere, bench.home, 1) == script::HostStatus::ok);

  CHECK(bench.drain(0) == 1);
  CHECK(bench.record_of(bench.key).priority == 0);
  CHECK(bench.record_of(within).priority == 0);
  CHECK(bench.record_of(edge).priority == 1);
  CHECK(bench.record_of(elsewhere).priority == 1);
  CHECK(bench.f.heroes.squads().find(within)->src_gaika == bench.home);
  CHECK(bench.f.heroes.squads().find(edge)->src_gaika == 77);

  bench.run_scripts();
  CHECK(bench.verb_of(bench.key) == "advance");
  CHECK(bench.verb_of(within) == "advance");
  CHECK(bench.verb_of(edge) == "guard");
  CHECK(bench.verb_of(elsewhere) == "guard");
}

/// **The ageing** (0x004489a0 with 0x004487d0) was meant to walk every record
/// and stops after the first, because its callback answers 0: record 0 alone
/// loses a point a drain, never below 1, whether or not anything ran.
TEST(the_order_drain_ages_only_the_first_record) {
  SendToBench bench;
  REQUIRE(build(bench.f.scheduler, bench.f.registry, kSendSquadStandIn,
                "DATA\\AI\\AIOSENDSQUAD.VS") != script::kNoChunk);
  const SquadKey urgent = bench.squad_at(Point{4000, 1100});
  const SquadKey middling = bench.squad_at(Point{5000, 1100});
  CHECK(bench.send(bench.dest, 3) == script::HostStatus::ok);
  CHECK(bench.send_squad(urgent, bench.dest, 100) == script::HostStatus::ok);
  CHECK(bench.send_squad(middling, bench.dest, 50) == script::HostStatus::ok);

  CHECK(bench.drain(0) == 1);  // `urgent`; its runner is left alive
  CHECK(bench.record_of(urgent).priority == 0);
  CHECK(bench.record_of(bench.key).priority == 2);
  CHECK(bench.record_of(middling).priority == 50);
  CHECK(bench.drain(500) == 0);  // `middling` picked, slot 2 busy
  CHECK(bench.record_of(bench.key).priority == 1);
  CHECK(bench.record_of(middling).priority == 50);
  CHECK(bench.drain(1000) == 0);
  CHECK(bench.record_of(bench.key).priority == 1);
}

/// **The regroup re-posts the order** (0x00447527..0x0044755d): a unit taken
/// out of its squad into a fresh one carries the old order's node and its
/// priority as it stands. The old squad emptied frees its slot first, so the
/// fresh squad's record takes it back. An order still waiting is then drained
/// for the fresh squad; one already carried out comes back at 0 and is never
/// carried out again -- the fresh squad answers `OrderDest` and nobody is sent.
TEST(a_regrouped_unit_takes_its_squads_order_to_the_fresh_squad) {
  SendToBench bench;
  REQUIRE(build(bench.f.scheduler, bench.f.registry, kSendSquadStandIn,
                "DATA\\AI\\AIOSENDSQUAD.VS") != script::kNoChunk);
  const SquadKey done = bench.squad_at(Point{4000, 1100});
  const SquadKey waiting = bench.squad_at(Point{5000, 1100});
  CHECK(bench.send_squad(done, bench.dest, 1) == script::HostStatus::ok);
  CHECK(bench.drain(0) == 1);
  bench.run_scripts();
  REQUIRE(bench.record_of(done).priority == 0);
  CHECK(bench.send_squad(waiting, bench.home, 100) == script::HostStatus::ok);
  const std::int32_t done_slot = bench.f.heroes.squads().find(done)->order;
  const std::int32_t waiting_slot = bench.f.heroes.squads().find(waiting)->order;
  const ObjectId done_unit = bench.f.heroes.squads().find(done)->members.front();
  const ObjectId waiting_unit = bench.f.heroes.squads().find(waiting)->members.front();

  const ObjectId units[2] = {done_unit, waiting_unit};
  regroup_into_fresh_squads(bench.f.world, bench.f.heroes, units, 5, 0);
  SquadTable& squads = bench.f.heroes.squads();
  const SquadKey fresh_done = squads.squad_of(done_unit);
  const SquadKey fresh_waiting = squads.squad_of(waiting_unit);
  REQUIRE(fresh_done.valid());
  REQUIRE(fresh_waiting.valid());
  CHECK(squads.find(fresh_done)->order == done_slot);
  CHECK(squads.find(fresh_waiting)->order == waiting_slot);
  CHECK(squads.find(fresh_done)->order_dest == bench.dest);
  CHECK(squads.find(fresh_waiting)->order_dest == bench.home);
  CHECK(bench.record_of(fresh_done).priority == 0);
  CHECK(bench.record_of(fresh_waiting).priority == 100);
  CHECK(bench.record_of(fresh_waiting).squad == fresh_waiting.index);

  CHECK(bench.drain(500) == 1);
  CHECK(bench.record_of(fresh_waiting).priority == 0);
  bench.run_scripts();
  // A fresh squad is filed under no node until the next turn's revaluation,
  // so the stand-in sends it on (state 1). The other keeps the regroup's
  // state and its unit's command, sent nowhere.
  CHECK(squads.find(fresh_waiting)->state == 1);
  CHECK(bench.verb_of(fresh_waiting) == "advance");
  CHECK(squads.find(fresh_done)->state == 5);
  CHECK(bench.drain(1000) == 0);
}

/// **`AIStop` takes the queue with the AI** (0x00448e90): every record freed,
/// every squad of the player answering no `OrderDest`, and the runner killed
/// with the AI's other scripts.
TEST(ai_stop_takes_the_order_queue_and_its_runner_with_the_ai) {
  SendToBench bench;
  REQUIRE(build(bench.f.scheduler, bench.f.registry, kSendSquadStandIn,
                "DATA\\AI\\AIOSENDSQUAD.VS") != script::kNoChunk);
  const SquadKey other = bench.squad_at(Point{4000, 1100});
  CHECK(bench.send(bench.dest) == script::HostStatus::ok);
  CHECK(bench.send_squad(other, bench.dest, 1) == script::HostStatus::ok);
  CHECK(bench.drain(0) == 1);
  const script::ScriptId runner = bench.queue().runner;
  REQUIRE(bench.f.scheduler.alive(runner));

  CHECK(ai_stop(bench.f.world, 1, bench.f.scheduler));
  CHECK(!bench.f.scheduler.alive(runner));
  CHECK(bench.queue() == AiOrderQueue{});
  CHECK(bench.squad().order == -1);
  CHECK(bench.squad().order_dest == kNoGaika);
  CHECK(bench.f.heroes.squads().find(other)->order == -1);
  CHECK(bench.f.heroes.squads().find(other)->ai_dest == kNoGaika);
  // And a stopped AI's timer does not fire.
  CHECK(bench.drain(500) == 0);
}

/// `g.MinNeed(...)` and `g.MaxNeed(...)` **run a script** and then divide by
/// optimism.
///
/// The stand-in below returns its own arguments folded into one number, so the
/// test can assert both that the six reach the script in the right order and
/// what the entry point does to the answer afterwards.
TEST(min_need_and_max_need_run_get_army_need_and_scale_by_optimism) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\nSleep(100000);\n", "DATA/AI/MAIN.VS") !=
          script::kNoChunk);
  // `int, GAIKA g, int idPlayer, bool bMin, int own, int ally, int enemy` --
  // the shipped signature. `bMin` is the only thing that separates the two
  // entry points, so it is what the stand-in reports.
  REQUIRE(build(f.scheduler, f.registry,
                "// int, GAIKA g, int idPlayer, bool bMin, int own, int ally, int enemy\n"
                "if (bMin) return 1000000 + g.ID * 10000 + idPlayer * 1000 + own * 100 + "
                "ally * 10 + enemy;\n"
                "return 2000000 + g.ID * 10000 + idPlayer * 1000 + own * 100 + ally * 10 + "
                "enemy;\n",
                "DATA/AI/GETARMYNEED.VS") != script::kNoChunk);

  // **Engine player 0 is script player 1**, which is the numbering the entry
  // point converts between and the one thing this test must not blur.
  CHECK(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(0)->root;
  f.ai.seed_gaika_view(0, 8);

  const auto need = [&](const char* name, GaikaId node, std::int32_t player) {
    const script::HostOutcome out =
        f.call(script::CallKind::member, name,
               {script::Value::integer(node), script::Value::integer(player),
                script::Value::integer(2), script::Value::integer(3),
                script::Value::integer(4)},
               root);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };

  // The three integers, folded: `own * 100 + ally * 10 + enemy` with 2, 3, 4.
  constexpr std::int32_t kThree = 2 * 100 + 3 * 10 + 4;

  // With no record for the node -- id 0 is the reserved one and has none --
  // there is no divisor, and the script's number comes back untouched. That
  // pins the argument order: `bMin`, then the node, the player and the three.
  CHECK(need("MinNeed", 0, 1) == 1000000 + 1000 + kThree);
  CHECK(need("MaxNeed", 0, 1) == 2000000 + 1000 + kThree);
  CHECK(need("MinNeed", 0, 2) == 1000000 + 2000 + kThree);

  // A node with a record divides by that record's optimism, as a percentage.
  GaikaView* view = f.ai.gaika_view(0);
  REQUIRE(view != nullptr);
  Laika* record = view->find(3);
  REQUIRE(record != nullptr);
  const std::int32_t raw = 1000000 + 3 * 10000 + 1000 + kThree;
  record->optimism = 100;
  CHECK(need("MinNeed", 3, 1) == raw);  // 100% is the identity
  record->optimism = 200;
  CHECK(need("MinNeed", 3, 1) == raw * 100 / 200);
  record->optimism = 50;
  CHECK(need("MinNeed", 3, 1) == raw * 100 / 50);

  // **Floored at 1**, so a real need never rounds away to nothing.
  record->optimism = 1000000000;
  CHECK(need("MinNeed", 3, 1) == 1);

  // Optimism of zero leaves the raw number rather than dividing by it.
  record->optimism = 0;
  CHECK(need("MinNeed", 3, 1) == raw);
  record->optimism = -5;
  CHECK(need("MinNeed", 3, 1) == raw);

  // A player outside 1..16 has no view to divide by, and the script still runs.
  CHECK(need("MinNeed", 3, 0) == 1000000 + 3 * 10000 + kThree);
  CHECK(need("MinNeed", 3, 17) == 1000000 + 3 * 10000 + 17000 + kThree);
}

/// `SS_STR(state)` is the inverse of the `SS_*` constants, and the names are
/// the **profile's** rather than the engine's.
TEST(ss_str_names_a_squad_state_out_of_the_root_profile) {
  Fixture f;
  // `[SquadStates]` in declaration order, sentinel first -- the same shape
  // `AiProfile::parse` reads out of `AI.INI`.
  const std::string ini =
      "[SquadStates]\n"
      "SS_Wait\n"
      "SS_Flee\n"
      "SS_Siege\n";
  imperivm::core::Result<AiProfile> parsed = AiProfile::parse(bytes_of(ini));
  REQUIRE(parsed.ok());
  f.ai.add_profile({}, &parsed.value());

  const auto name = [&](std::int32_t state) {
    const script::HostOutcome out = f.call(script::CallKind::free_function, "SS_STR",
                                           {script::Value::integer(state)}, script::kNoScript);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_string() ? std::string(out.value.as_string()) : std::string("<no>");
  };

  // Zero -- and anything below it -- is the engine's own sentinel, which is
  // the one name of the family that appears in `gbr.exe` at all.
  CHECK(name(0) == "SS_IDLE");
  CHECK(name(-3) == "SS_IDLE");
  // The declared names follow it, in file order.
  CHECK(name(1) == "SS_Wait");
  CHECK(name(2) == "SS_Flee");
  CHECK(name(3) == "SS_Siege");
  // A number the profile does not declare is the **empty string**, not the
  // sentinel: `SS_IDLE` is what zero means, and 99 is not zero.
  CHECK(name(99).empty());
}

/// `g.Recruit(player, min, max, avail)` is the AI's army dispatch, and it is
/// **three scripts and a budget**. The stand-ins below are the shipped ones cut
/// down to what the entry point reads back: a per-squad score with an optional
/// dependency, a per-node take budget, and a send that does what the real
/// `SendSquad.vs` does -- `sq.SendTo(g, 100)` -- so "was this squad dispatched"
/// is a question the squad table can answer.
TEST(recruit_scores_ranks_and_dispatches_squads_through_three_scripts) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  // `int, Squad sq, GAIKA g, GAIKA *gDepend`. Squad 2 depends on node 2; the
  // other two are free. The score is the squad number, so the ranking below is
  // 3, 2, 1 and the visiting order in the first pass is 1, 2, 3.
  REQUIRE(build(f.scheduler, f.registry,
                "// int, Squad sq, GAIKA g, GAIKA *gDepend\n"
                "if (sq.No == 2) gDepend = 2;\n"
                "return sq.No;\n",
                "DATA/AI/EVALRECRUIT.VS") != script::kNoChunk);
  // `int, int idPlayer, GAIKA g, int *pOverneed`: `own - MinNeed` and
  // `own - MaxNeed`. Generous enough that nothing is refused for want of it.
  REQUIRE(build(f.scheduler, f.registry,
                "// int, int idPlayer, GAIKA g, int *pOverneed\n"
                "pOverneed = 1000;\n"
                "return 1000;\n",
                "DATA/AI/CALCMAXTAKE.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// bool, Squad sq, GAIKA g\n"
                "sq.SendTo(g, 100);\n"
                "return true;\n",
                "DATA/AI/SENDSQUAD.VS") != script::kNoChunk);

  (void)f.make_settlement(0);
  (void)f.make_settlement(0);
  (void)f.make_settlement(0);
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  REQUIRE(f.world.gaika().count() >= 3);

  // Engine player 0 is script player 1.
  REQUIRE(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(0)->root;

  std::vector<SquadKey> keys;
  for (std::int32_t i = 0; i < 3; ++i) {
    const SquadKey key = f.heroes.squads().create(0);
    Squad* record = f.heroes.squads().find(key);
    REQUIRE(record != nullptr);
    // `Squad::Eval` is the unit of account -- `nAvail`, the budget and the
    // answer are all denominated in it. Nothing in this engine writes it, so
    // the test does; see the body's last paragraph.
    record->eval = 10;
    keys.push_back(key);
  }

  /// `Recruit` writes its fourth argument, so the call needs the window back.
  const auto recruit = [&](GaikaId node, std::int32_t player, std::int32_t min_need,
                           std::int32_t max_need, std::int32_t avail,
                           std::int32_t* left) {
    std::vector<script::Value> args{
        script::Value::integer(node),     script::Value::integer(player),
        script::Value::integer(min_need), script::Value::integer(max_need),
        script::Value::integer(avail)};
    const std::uint32_t index = f.registry.find(script::CallKind::member, "Recruit", 4);
    CHECK(index != script::kUnresolvedHost);
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.host = &f.host;
    ctx.scheduler = &f.scheduler;
    ctx.script = root;
    ctx.user = &f.context;
    ctx.name = "Recruit";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = f.registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    if (left != nullptr) *left = args[4].is_integer() ? args[4].as_integer() : -1;
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  const auto clear_destinations = [&] {
    for (const SquadKey key : keys) {
      Squad* record = f.heroes.squads().find(key);
      REQUIRE(record != nullptr);
      record->ai_dest = kNoGaika;
      record->order_dest = kNoGaika;
    }
  };
  const auto destination = [&](std::size_t which) {
    const Squad* record = f.heroes.squads().find(keys[which]);
    return record == nullptr ? kNoGaika : record->ai_dest;
  };

  // **`nAvail < nMin` gives up before anything runs**, and leaves `nAvail`
  // alone -- the recruiter's own "no chance" filter reaches this first.
  std::int32_t left = -1;
  CHECK(recruit(1, 1, 100, 200, 20, &left) == 0);
  CHECK(left == 20);
  CHECK(destination(0) == kNoGaika);

  // Everything fits: three squads worth 10 each, a maximum of 100. Squad 2
  // depends on node 2 and so waits for the second send pass, which only runs
  // while the total is short of the minimum -- and 30 clears a minimum of 5 on
  // the free two alone, so squad 2 is *not* sent.
  CHECK(recruit(1, 1, 5, 100, 50, &left) == 20);
  CHECK(left == 30);
  CHECK(destination(0) == 1);
  CHECK(destination(2) == 1);
  CHECK(destination(1) == kNoGaika);

  // `nMax` stops the free pass, and the ranking is best-first: the score is the
  // squad number, so squad 3 goes and squad 1 does not.
  clear_destinations();
  CHECK(recruit(1, 1, 5, 10, 50, &left) == 10);
  CHECK(left == 40);
  CHECK(destination(2) == 1);
  CHECK(destination(0) == kNoGaika);
  CHECK(destination(1) == kNoGaika);

  // A minimum the free squads cannot reach **does** rob the dependent one, and
  // that second pass stops the moment the minimum is met rather than at `nMax`.
  clear_destinations();
  CHECK(recruit(1, 1, 25, 100, 50, &left) == 30);
  CHECK(left == 20);
  CHECK(destination(0) == 1);
  CHECK(destination(1) == 1);
  CHECK(destination(2) == 1);

  // `nAvail` floors at zero rather than going negative.
  clear_destinations();
  CHECK(recruit(1, 1, 25, 100, 25, &left) == 30);
  CHECK(left == 0);

  // A player outside 1..16 is refused, and `nAvail` comes back untouched.
  clear_destinations();
  CHECK(recruit(1, 0, 5, 100, 50, &left) == 0);
  CHECK(left == 50);
  CHECK(recruit(1, 17, 5, 100, 50, &left) == 0);
  CHECK(left == 50);
  CHECK(destination(0) == kNoGaika);

  // **A player whose AI is not running is refused too**, and the slot existing
  // is not the same as the AI running -- these squads are player 1's, they are
  // the same squads, and only the flag differs. It gates here for the reason it
  // gates in `Squad::SendTo`: `SendSquad.vs` calls that, and an inactive AI
  // would refuse every send after this had already counted them.
  REQUIRE(f.ai.player_ai(0) != nullptr);
  CHECK(f.ai.stop(0, f.scheduler));
  CHECK(f.ai.player_ai(0) != nullptr);
  CHECK(!f.ai.player_ai(0)->active);
  CHECK(recruit(1, 1, 5, 100, 50, &left) == 0);
  CHECK(left == 50);
  CHECK(destination(0) == kNoGaika);
  CHECK(destination(2) == kNoGaika);
}

/// The budget, which is the only thing in `Recruit` that is not a script's
/// answer taken at face value: what a donor node can give up, spent best-first
/// once anything has overflowed, and dropped entirely when the take leaves the
/// donor above its *maximum* need.
TEST(recruit_spends_each_donor_nodes_budget_best_first) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  // Every squad depends on node 2, and the score is the squad number.
  REQUIRE(build(f.scheduler, f.registry,
                "// int, Squad sq, GAIKA g, GAIKA *gDepend\n"
                "gDepend = 2;\n"
                "return sq.No;\n",
                "DATA/AI/EVALRECRUIT.VS") != script::kNoChunk);
  // 25 to give up, and nothing to spare: `pOverneed` of 0 means no take is
  // free, so a dependency is never cleared and every squad waits for the
  // second send pass.
  REQUIRE(build(f.scheduler, f.registry,
                "// int, int idPlayer, GAIKA g, int *pOverneed\n"
                "pOverneed = 0;\n"
                "return 25;\n",
                "DATA/AI/CALCMAXTAKE.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// bool, Squad sq, GAIKA g\n"
                "sq.SendTo(g, 100);\n"
                "return true;\n",
                "DATA/AI/SENDSQUAD.VS") != script::kNoChunk);

  (void)f.make_settlement(0);
  (void)f.make_settlement(0);
  (void)f.make_settlement(0);
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  REQUIRE(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(0)->root;

  std::vector<SquadKey> keys;
  for (std::int32_t i = 0; i < 3; ++i) {
    const SquadKey key = f.heroes.squads().create(0);
    Squad* record = f.heroes.squads().find(key);
    REQUIRE(record != nullptr);
    record->eval = 10;
    keys.push_back(key);
  }

  const auto recruit = [&](std::int32_t min_need, std::int32_t max_need) {
    std::vector<script::Value> args{
        script::Value::integer(1),        script::Value::integer(1),
        script::Value::integer(min_need), script::Value::integer(max_need),
        script::Value::integer(1000)};
    const std::uint32_t index = f.registry.find(script::CallKind::member, "Recruit", 4);
    CHECK(index != script::kUnresolvedHost);
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.host = &f.host;
    ctx.scheduler = &f.scheduler;
    ctx.script = root;
    ctx.user = &f.context;
    ctx.name = "Recruit";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = f.registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };

  // 30 of strength wants to leave a node that can only give up 25, so the
  // third squad to be counted overflows by 5. That triggers the re-run, which
  // spends the same 25 **best-first** -- squads 3 and 2 fit, squad 1 does not
  // and is dropped. 20 is what is left, and it clears a minimum of 15.
  CHECK(recruit(15, 1000) == 20);
  const auto sent = [&](std::size_t which) {
    const Squad* record = f.heroes.squads().find(keys[which]);
    return record != nullptr && record->ai_dest == 1;
  };
  CHECK(sent(2));
  CHECK(sent(1));
  CHECK(!sent(0));

  // And the same 20 does not clear a minimum of 25: the recruit is abandoned
  // whole rather than sending what it has.
  for (const SquadKey key : keys) {
    Squad* record = f.heroes.squads().find(key);
    REQUIRE(record != nullptr);
    record->ai_dest = kNoGaika;
    record->order_dest = kNoGaika;
  }
  CHECK(recruit(25, 1000) == 0);
  CHECK(!sent(0));
  CHECK(!sent(1));
  CHECK(!sent(2));
}

/// `Overneed`, which is the half of `CalcMaxTake.vs` that decides **which send
/// pass a squad lands in**. A take that leaves the donor above its *maximum*
/// need costs it nothing, so the re-run drops the dependency, and a squad with
/// no dependency goes in the first pass -- which runs to `nMax` rather than
/// stopping at `nMin`.
TEST(recruit_clears_a_dependency_the_donor_does_not_feel) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// int, Squad sq, GAIKA g, GAIKA *gDepend\n"
                "gDepend = 2;\n"
                "return sq.No;\n",
                "DATA/AI/EVALRECRUIT.VS") != script::kNoChunk);
  // 25 to give up, and **all** of it above the maximum need.
  REQUIRE(build(f.scheduler, f.registry,
                "// int, int idPlayer, GAIKA g, int *pOverneed\n"
                "pOverneed = 25;\n"
                "return 25;\n",
                "DATA/AI/CALCMAXTAKE.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// bool, Squad sq, GAIKA g\n"
                "sq.SendTo(g, 100);\n"
                "return true;\n",
                "DATA/AI/SENDSQUAD.VS") != script::kNoChunk);

  (void)f.make_settlement(0);
  (void)f.make_settlement(0);
  (void)f.make_settlement(0);
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  REQUIRE(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(0)->root;

  std::vector<SquadKey> keys;
  for (std::int32_t i = 0; i < 4; ++i) {
    const SquadKey key = f.heroes.squads().create(0);
    Squad* record = f.heroes.squads().find(key);
    REQUIRE(record != nullptr);
    record->eval = 10;
    keys.push_back(key);
  }

  // Four squads worth 10 each against a budget of 25: the last two to be
  // counted overflow, so the re-run happens. Best-first it admits squads 4 and
  // 3 -- both inside the 25 of `Overneed`, so **both lose their dependency** --
  // and drops 2 and 1 for want of budget. A minimum of 5 is met by the free
  // pass alone, which then keeps going to `nMax` and sends both.
  std::vector<script::Value> args{
      script::Value::integer(1), script::Value::integer(1), script::Value::integer(5),
      script::Value::integer(1000), script::Value::integer(1000)};
  const std::uint32_t index = f.registry.find(script::CallKind::member, "Recruit", 4);
  REQUIRE(index != script::kUnresolvedHost);
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.host = &f.host;
  ctx.scheduler = &f.scheduler;
  ctx.script = root;
  ctx.user = &f.context;
  ctx.name = "Recruit";
  ctx.kind = script::CallKind::member;
  const script::HostOutcome out = f.registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  // 20, not 10: with the dependency kept, the second pass would stop at the
  // minimum after one squad.
  CHECK(out.value.is_integer() && out.value.as_integer() == 20);
  const auto sent = [&](std::size_t which) {
    const Squad* record = f.heroes.squads().find(keys[which]);
    return record != nullptr && record->ai_dest == 1;
  };
  CHECK(sent(3));
  CHECK(sent(2));
  CHECK(!sent(1));
  CHECK(!sent(0));
}

/// A squad heavier than the whole of its donor's budget is refused **in the
/// first pass**, before it is counted -- and the difference that makes is not
/// in the arithmetic, which the re-run would repair, but in whether the re-run
/// happens at all. Admitting it overflows the budget, the overflow forces the
/// re-run, and the re-run is what clears the *other* squad's dependency and
/// moves it into the free send pass.
TEST(recruit_refuses_a_squad_heavier_than_its_donors_whole_budget) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// int, Squad sq, GAIKA g, GAIKA *gDepend\n"
                "gDepend = 2;\n"
                "return sq.No;\n",
                "DATA/AI/EVALRECRUIT.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// int, int idPlayer, GAIKA g, int *pOverneed\n"
                "pOverneed = 15;\n"
                "return 25;\n",
                "DATA/AI/CALCMAXTAKE.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// bool, Squad sq, GAIKA g\n"
                "sq.SendTo(g, 100);\n"
                "return true;\n",
                "DATA/AI/SENDSQUAD.VS") != script::kNoChunk);

  (void)f.make_settlement(0);
  (void)f.make_settlement(0);
  (void)f.make_settlement(0);
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  REQUIRE(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(0)->root;

  // Squad 1 is worth 30 against a budget of 25 and is counted first; squad 2 is
  // worth 10 and scores higher.
  const SquadKey heavy = f.heroes.squads().create(0);
  const SquadKey light = f.heroes.squads().create(0);
  REQUIRE(f.heroes.squads().find(heavy) != nullptr);
  REQUIRE(f.heroes.squads().find(light) != nullptr);
  f.heroes.squads().find(heavy)->eval = 30;
  f.heroes.squads().find(light)->eval = 10;

  std::vector<script::Value> args{
      script::Value::integer(1), script::Value::integer(1), script::Value::integer(0),
      script::Value::integer(1000), script::Value::integer(1000)};
  const std::uint32_t index = f.registry.find(script::CallKind::member, "Recruit", 4);
  REQUIRE(index != script::kUnresolvedHost);
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.host = &f.host;
  ctx.scheduler = &f.scheduler;
  ctx.script = root;
  ctx.user = &f.context;
  ctx.name = "Recruit";
  ctx.kind = script::CallKind::member;
  const script::HostOutcome out = f.registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  // Nothing overflows once the heavy squad is out, so no re-run, so squad 2
  // keeps the dependency it was given -- and a minimum of zero means the
  // second send pass never opens. Admitting the heavy squad would have sent it.
  CHECK(out.value.is_integer() && out.value.as_integer() == 0);
  CHECK(f.heroes.squads().find(heavy)->ai_dest == kNoGaika);
  CHECK(f.heroes.squads().find(light)->ai_dest == kNoGaika);
}

/// The three refusals that never reach a script: a squad worth nothing, a
/// squad a mission has pinned, and a player with no squads at all.
TEST(recruit_passes_over_squads_no_script_should_ever_see) {
  Fixture f;
  REQUIRE(build(f.scheduler, f.registry, "// void\n", "DATA/AI/MAIN.VS") != script::kNoChunk);
  // The stand-in **marks the squad it is asked about**, so that "no script ever
  // saw this squad" is a thing the squad table can be asked rather than an
  // absence inferred from the answer.
  REQUIRE(build(f.scheduler, f.registry,
                "// int, Squad sq, GAIKA g, GAIKA *gDepend\n"
                "sq.SendTo(2, 100);\n"
                "return 100;\n",
                "DATA/AI/EVALRECRUIT.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// int, int idPlayer, GAIKA g, int *pOverneed\n"
                "return 0;\n",
                "DATA/AI/CALCMAXTAKE.VS") != script::kNoChunk);
  REQUIRE(build(f.scheduler, f.registry,
                "// bool, Squad sq, GAIKA g\n"
                "sq.SendTo(g, 100);\n"
                "return true;\n",
                "DATA/AI/SENDSQUAD.VS") != script::kNoChunk);
  (void)f.make_settlement(0);
  (void)f.make_settlement(0);
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  REQUIRE(f.ai.start(0, "", AiDifficulty::normal, f.scheduler) == AiStartStatus::ok);
  const script::ScriptId root = f.ai.player_ai(0)->root;

  const auto recruit = [&](std::int32_t min_need) {
    std::vector<script::Value> args{
        script::Value::integer(1),        script::Value::integer(1),
        script::Value::integer(min_need), script::Value::integer(1000),
        script::Value::integer(1000)};
    const std::uint32_t index = f.registry.find(script::CallKind::member, "Recruit", 4);
    CHECK(index != script::kUnresolvedHost);
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.host = &f.host;
    ctx.scheduler = &f.scheduler;
    ctx.script = root;
    ctx.user = &f.context;
    ctx.name = "Recruit";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = f.registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };

  // No squads at all -- the original's "fewer than two entries in the list",
  // slot 0 being reserved.
  CHECK(recruit(1) == 0);

  // **A squad worth nothing is refused without asking.** This is the standing
  // gap made visible: `Squad::Eval` is zero everywhere in this engine, so this
  // is the answer `RECRUITER.VS` gets on every shipped map today.
  const SquadKey idle = f.heroes.squads().create(0);
  REQUIRE(f.heroes.squads().find(idle) != nullptr);
  CHECK(recruit(1) == 0);
  // Untouched: `EvalRecruit.vs` marks everything it is handed, so this is the
  // short circuit itself and not the empty answer that would follow it anyway.
  CHECK(f.heroes.squads().find(idle)->ai_dest == kNoGaika);

  // The three flags `MilEval` leaves out are the three this leaves out.
  Squad* pinned = f.heroes.squads().find(idle);
  REQUIRE(pinned != nullptr);
  pinned->eval = 40;
  for (const std::uint16_t flag :
       {kSquadFlagNoAi, kSquadFlagPeaceful, kSquadFlagSentries}) {
    pinned->flags = flag;
    pinned->ai_dest = kNoGaika;
    CHECK(recruit(1) == 0);
    CHECK(pinned->ai_dest == kNoGaika);
  }
  // And with none of them set it goes.
  pinned->flags = 0;
  CHECK(recruit(1) == 40);
  CHECK(pinned->ai_dest == 1);
}

/// `SetMAIKA` runs and stores nothing, on `GlobalSpellStart`'s footing -- and
/// the assertion is that it *runs*, because the alternative to a body is
/// `CHECKMAIKA.VS` stopping.
///
/// The original's seven refusals are written out on the body and not
/// reproduced: with nothing stored none of them can change an answer, and this
/// test is where that would show if one ever could.
TEST(set_maika_runs_and_stores_nothing) {
  Fixture f;
  (void)f.make_settlement(1);
  (void)f.make_settlement(1);
  (void)f.make_settlement(1);
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  REQUIRE(f.world.gaika().count() > 0);
  const std::uint64_t before = f.world.state_hash();
  const auto set = [&](std::int32_t a, std::int32_t b, std::int32_t c) {
    return f.call(script::CallKind::free_function, "SetMAIKA",
                  {script::Value::integer(a), script::Value::integer(b),
                   script::Value::integer(c)},
                  script::kNoScript)
        .status;
  };
  CHECK(set(1, 2, 3) == script::HostStatus::ok);
  // The triples the original refuses run here too, and none of them is an
  // error either way.
  CHECK(set(0, 2, 3) == script::HostStatus::ok);
  CHECK(set(1, 0, 3) == script::HostStatus::ok);
  CHECK(set(1, 2, 0) == script::HostStatus::ok);
  CHECK(set(1, 1, 3) == script::HostStatus::ok);
  CHECK(set(1, 2, 1) == script::HostStatus::ok);
  CHECK(set(1, 2, 2) == script::HostStatus::ok);
  CHECK(f.world.state_hash() == before);
}

// --------------------------------------------------------------------------
// ship needs
// --------------------------------------------------------------------------

/// The three counters are one table keyed by `(player, lsa)`, the census is a
/// walk over the world, and the table rides in the save.
TEST(ai_ship_needs_count_per_player_and_area_and_ships_are_a_census) {
  Fixture f;
  const script::CallKind kFree = script::CallKind::free_function;
  const auto needs = [&](std::int32_t player, std::int32_t lsa) {
    const script::HostOutcome out = f.call(
        kFree, "ShipNeeds", {script::Value::integer(player), script::Value::integer(lsa)}, 1);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  const auto ships = [&](std::int32_t player, std::int32_t lsa) {
    const script::HostOutcome out =
        f.call(kFree, "Ships", {script::Value::integer(player), script::Value::integer(lsa)}, 1);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };

  CHECK(needs(2, 7) == 0);
  f.call(kFree, "IncShipNeeds", {script::Value::integer(2), script::Value::integer(7)}, 1);
  f.call(kFree, "IncShipNeeds", {script::Value::integer(2), script::Value::integer(7)}, 1);
  f.call(kFree, "IncShipNeeds", {script::Value::integer(2), script::Value::integer(3)}, 1);
  f.call(kFree, "IncShipNeeds", {script::Value::integer(1), script::Value::integer(7)}, 1);
  CHECK(needs(2, 7) == 2);
  CHECK(needs(2, 3) == 1);
  CHECK(needs(1, 7) == 1);
  CHECK(needs(3, 7) == 0);
  f.call(kFree, "ClrShipNeeds", {script::Value::integer(2), script::Value::integer(7)}, 1);
  CHECK(needs(2, 7) == 0);
  CHECK(needs(2, 3) == 1);
  // Rows are `(player, lsa)` ascending, and a cleared row is kept at zero.
  REQUIRE(f.ai.ship_need_rows().size() == 3);
  CHECK(f.ai.ship_need_rows()[0].player == 0);
  CHECK(f.ai.ship_need_rows()[1].lsa == 3);
  CHECK(f.ai.ship_need_rows()[2].lsa == 7);
  CHECK(f.ai.ship_need_rows()[2].count == 0);
  // A player outside the table is nothing, and changes nothing.
  f.call(kFree, "IncShipNeeds", {script::Value::integer(40), script::Value::integer(7)}, 1);
  CHECK(needs(40, 7) == 0);
  CHECK(f.ai.ship_need_rows().size() == 3);

  // The save carries the rows.
  std::vector<std::byte> bytes;
  f.ai.serialize(bytes);
  AiSystem loaded;
  REQUIRE(loaded.deserialize(bytes).ok());
  CHECK(loaded.ship_needs(1, 3) == 1);
  CHECK(loaded.ship_needs(1, 7) == 0);
  CHECK(loaded.ship_needs(0, 7) == 1);
  CHECK(loaded.ship_need_rows().size() == 3);
  // And refuses rows out of order.
  AiSystem refusing;
  CHECK(!refusing.adopt_ship_needs({AiSystem::ShipNeed{1, 7, 1}, AiSystem::ShipNeed{1, 3, 1}}));
  CHECK(refusing.adopt_ship_needs({AiSystem::ShipNeed{1, 3, 1}, AiSystem::ShipNeed{1, 7, 1}}));

  // Ships: the player's living ships whose position falls in the area. This
  // world has no partition, so every point is in area 0 and no other.
  const World::ShipIds mine = f.world.spawn_ship(nullptr, imperivm::core::kNoClass);
  const World::ShipIds theirs = f.world.spawn_ship(nullptr, imperivm::core::kNoClass);
  const World::ShipIds wreck = f.world.spawn_ship(nullptr, imperivm::core::kNoClass);
  for (const World::ShipIds& ship : {mine, theirs, wreck}) {
    f.world.set_position(ship.ship, Point{100, 100});
    f.world.set_health(ship.ship, 100);
  }
  f.world.set_owner(mine.ship, 1);
  f.world.set_owner(wreck.ship, 1);
  f.world.set_owner(theirs.ship, 2);
  f.world.set_health(wreck.ship, 0);
  CHECK(ships(2, 0) == 1);
  CHECK(ships(3, 0) == 1);
  CHECK(ships(2, 5) == 0);
  CHECK(ships(40, 0) == 0);
}

// --------------------------------------------------------------------------
// the two evaluations
// --------------------------------------------------------------------------

namespace {

/// A call whose argument window is handed back, so out-parameters can be read.
std::vector<script::Value> call_with_outs(Fixture& f, const char* name,
                                          std::vector<script::Value> args) {
  const std::uint32_t index =
      f.registry.find(script::CallKind::free_function, name, static_cast<std::uint16_t>(args.size()));
  CHECK(index != script::kUnresolvedHost);
  if (index == script::kUnresolvedHost) return args;
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.host = &f.host;
  ctx.scheduler = &f.scheduler;
  ctx.script = 1;
  ctx.user = &f.context;
  const script::HostOutcome out = f.registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  return args;
}

}  // namespace

/// `Eval` sums the census valuation of the standing units within range into
/// three buckets by the owner's relation to the player; the edge counts, a
/// garrisoned unit does not.
TEST(ai_eval_buckets_the_units_in_range_by_relation) {
  Fixture f;
  // Player 0 is at peace with 1 and at war with 2 (a fresh table is a
  // free-for-all, so the peace is declared).
  f.world.players().set_relation_word(0, 1, 1);
  const auto soldier = [&](PlayerId owner, Point at) {
    const ObjectId id = f.world.spawn(imperivm::core::NativeClass::unit, nullptr);
    f.world.set_owner(id, owner);
    f.world.set_health(id, 100);
    f.world.set_position(id, at);
    return id;
  };
  const ObjectId mine = soldier(0, Point{100, 100});
  const ObjectId friendly = soldier(1, Point{150, 100});
  const ObjectId foe = soldier(2, Point{100, 150});
  const ObjectId far = soldier(2, Point{100, 1000});
  const ObjectId edge = soldier(2, Point{300, 100});
  const ObjectId inside = soldier(2, Point{100, 100});
  const World::SettlementIds town = f.world.spawn_settlement(2);
  REQUIRE(f.world.put_in_holder(inside, town.holder));
  (void)far;

  // Every unit here is worth 1: no combatant, no class, so the valuation is
  // its `+ 1`. That makes the buckets counts.
  std::vector<script::Value> out = call_with_outs(
      f, "Eval",
      {script::Value::integer(1), pack_point(Point{100, 100}), script::Value::integer(200),
       script::Value::integer(0), script::Value::integer(0), script::Value::integer(0)});
  CHECK(out[3].as_integer() == 1);   // mine
  CHECK(out[4].as_integer() == 1);   // friendly
  CHECK(out[5].as_integer() == 2);   // foe, and edge at exactly 200
  (void)mine; (void)friendly; (void)foe; (void)edge;

  // A player outside the table leaves every bucket at zero.
  out = call_with_outs(f, "Eval",
                       {script::Value::integer(40), pack_point(Point{100, 100}),
                        script::Value::integer(200), script::Value::integer(7),
                        script::Value::integer(7), script::Value::integer(7)});
  CHECK(out[3].as_integer() == 0);
  CHECK(out[4].as_integer() == 0);
  CHECK(out[5].as_integer() == 0);
}

/// `EnemyPlayersEval`: per enemy, the sum of its squads' `eval` less the
/// no-AI, peaceful and sentry ones; least, most and mean out, total back.
TEST(ai_enemy_players_eval_ranges_over_the_enemies_squads) {
  Fixture f;
  f.world.players().set_relation_word(0, 1, 1);  // 1 is a friend
  const auto squad = [&](PlayerId owner, std::int32_t eval, std::uint16_t flags) {
    const SquadKey key = f.heroes.squads().create(owner);
    Squad* record = f.heroes.squads().find(key);
    REQUIRE(record != nullptr);
    record->eval = eval;
    record->flags = flags;
  };
  squad(1, 500, 0);                     // a friend's, not counted
  squad(2, 30, 0);
  squad(2, 20, 0);
  squad(2, 900, kSquadFlagPeaceful);    // skipped
  squad(2, 900, kSquadFlagSentries);    // skipped
  squad(3, 10, 0);
  squad(3, 900, kSquadFlagNoAi);        // skipped

  std::vector<script::Value> out =
      call_with_outs(f, "EnemyPlayersEval",
                     {script::Value::integer(1), script::Value::integer(0),
                      script::Value::integer(0), script::Value::integer(0)});
  // Enemies: 2 at 50, 3 at 10, and every other table player at 0.
  CHECK(out[1].as_integer() == 0);   // least is 0 and zero is not written
  CHECK(out[2].as_integer() == 50);
  CHECK(out[3].as_integer() == 60 / 14);

  // Make everyone but 2 and 3 a friend: least is 10 and the mean is 30.
  for (PlayerId other = 4; other < kPlayerCount; ++other) {
    f.world.players().set_relation_word(0, other, 1);
  }
  out = call_with_outs(f, "EnemyPlayersEval",
                       {script::Value::integer(1), script::Value::integer(0),
                        script::Value::integer(0), script::Value::integer(0)});
  CHECK(out[1].as_integer() == 10);
  CHECK(out[2].as_integer() == 50);
  CHECK(out[3].as_integer() == 30);
}

/// **A node is `Explored` for a player once the exploration map shows its
/// centre explored for that player** -- the `Explored` half of the original's
/// per-node visibility sweep, which `AiSystem::advance` runs. Without it
/// `RECRUITER.VS:104-116` skipped every node but home: `Explored` was false
/// everywhere, and so was `CanExplore`, which asks it of the neighbours.
TEST(the_ai_sweep_marks_a_node_explored_once_its_centre_is_and_stamps_last_seen) {
  Fixture f;
  FogSystem fog;
  // Registered after the AI: the order the session registers them in, so
  // this turn's sweep reads the map as the previous turn left it.
  REQUIRE(f.world.add_system(&fog));
  fog.map().resize(16384);

  const SettlementId near = f.make_settlement(1);
  const SettlementId away = f.make_settlement(1);
  ObjectId a_object = kNoObject;
  ObjectId b_object = kNoObject;
  {
    const Settlement* a = f.economy.settlements().find(near);
    const Settlement* b = f.economy.settlements().find(away);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    a_object = a->object;
    b_object = b->object;
    CHECK(f.world.set_position(a->anchor, Point{1000, 1000}));
    CHECK(f.world.set_position(b->anchor, Point{9000, 9000}));
  }
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  REQUIRE(f.world.gaika().count() == 2);
  const GaikaId seen = f.world.gaika().for_settlement(a_object);
  const GaikaId unseen = f.world.gaika().for_settlement(b_object);
  f.ai.seed_gaika_view(1, f.world.gaika().count());
  f.ai.seed_gaika_view(2, f.world.gaika().count());
  // A slot past the eighth: the map answers true for it everywhere, which is
  // the original's own short circuit (0x00515958).
  f.ai.seed_gaika_view(9, f.world.gaika().count());
  const GaikaView* one = f.ai.gaika_view(1);
  const GaikaView* two = f.ai.gaika_view(2);
  const GaikaView* ten = f.ai.gaika_view(9);
  REQUIRE(one != nullptr);
  REQUIRE(two != nullptr);
  REQUIRE(ten != nullptr);
  const auto explored = [](const GaikaView* view, GaikaId id) {
    return (view->find(id)->flags & kLaikaExplored) != 0;
  };
  const auto sweep = [&](GameTime time) {
    Turn turn;
    turn.time = time;
    f.ai.advance(f.world, turn);
  };

  // Nothing seen yet: nothing explored, nothing stamped.
  sweep(1000);
  CHECK(!explored(one, seen));
  CHECK(!explored(one, unseen));
  CHECK(one->find(seen)->last_seen == 0);
  CHECK(explored(ten, seen));
  CHECK(explored(ten, unseen));

  // Player 1 sees the first node's centre. Flags already on the record stay:
  // the sweep sets a bit, it does not write the word.
  f.ai.gaika_view(1)->find(seen)->flags |= kLaikaNoRecruit;
  fog.map().explore(Point{1000, 1000}, 1);
  sweep(5000);
  CHECK(explored(one, seen));
  CHECK((one->find(seen)->flags & kLaikaNoRecruit) != 0);
  CHECK(one->find(seen)->last_seen == 5000);
  CHECK(!explored(one, unseen));
  CHECK(one->find(unseen)->last_seen == 0);
  // Another player's slot is another player's.
  CHECK(!explored(two, seen));
  CHECK(two->find(seen)->last_seen == 0);

  // `LastSeen` follows the sweep for as long as the node is explored, which
  // is for good: the map only ever gains ground.
  sweep(7000);
  CHECK(one->find(seen)->last_seen == 7000);

  // **The centre, not any cell.** A cell beside the second node's centre --
  // 1,100 units off, in the next 1,024-unit fog cell -- is not its centre.
  fog.map().explore(Point{9000 - 1100, 9000}, 1);
  sweep(8000);
  CHECK(!explored(one, unseen));
  fog.map().explore(Point{9000, 9000}, 1);
  sweep(9000);
  CHECK(explored(one, unseen));

  // And the script reads it: `g.Explored(2)` is player 1's bit.
  const script::HostOutcome asked =
      f.call(script::CallKind::member, "Explored", {gaika_value(seen), script::Value::integer(2)},
             script::kNoScript);
  CHECK(asked.status == script::HostStatus::ok);
  CHECK(asked.value.truthy_scalar());
}

TEST(the_ai_sweep_does_nothing_without_an_exploration_map) {
  // A world with no fog system -- every synthetic test in this file -- is not
  // a world where everything is explored.
  Fixture f;
  const SettlementId only = f.make_settlement(1);
  {
    const Settlement* s = f.economy.settlements().find(only);
    REQUIRE(s != nullptr);
    CHECK(f.world.set_position(s->anchor, Point{1000, 1000}));
  }
  f.world.mutable_gaika().build(f.world, f.world.lsa(), f.economy.settlements());
  f.ai.seed_gaika_view(1, f.world.gaika().count());
  Turn turn;
  turn.time = 4000;
  f.ai.advance(f.world, turn);
  const Laika* record = f.ai.gaika_view(1)->find(1);
  REQUIRE(record != nullptr);
  CHECK((record->flags & kLaikaExplored) == 0);
  CHECK(record->last_seen == 0);
}
