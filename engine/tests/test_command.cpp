// The command queue and the cross-domain orders: sim/command.hpp.
//
// No game data anywhere in this file. Every `<commands>` document and every
// class is a string literal written here, and the queue shapes asserted below
// are transcriptions of shipped `.vs` code -- `HERO_AI_KILLALL.VS`,
// `WALL_PATROL.VS`, `AI HELPERS\GUARD.VS`, `CATAPULT_DISBAND.VS`,
// `ESH_FOODTRADE.VS` -- rather than shapes invented to match the code.
//
// The cases that carry the reading are:
//
//   * **`AddCommand(true, ...)` inserts at index 1, not index 0.** The queue
//     `HERO_AI_KILLALL.VS` builds only matches the comment it ends with --
//     `// else commands "ai_killall" / "advance" cycle` -- under that reading.
//   * **`KillCommand` ends the command, not the object.** The wagon in
//     `ESH_FOODTRADE.VS` has to survive it, carrying its gold.
//   * **A queue is never empty.** `GUARD.VS` reads `o.command == "idle"` as a
//     resting state, so a drained queue refills.
//
// The null-`user` sweep at the bottom is not defensive tidying: a host function
// that dereferenced `CallContext::user` without checking would take the process
// down on the first script that ran outside an embedder.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/globals.hpp"
#include "imperivm/core/sim/text.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;
using namespace imperivm::core::script;

namespace {

std::span<const std::byte> bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// --------------------------------------------------------------------------
// fixtures
// --------------------------------------------------------------------------

/// Six `<cmd>` rows in the shape `DATA\COMMANDS\*.XML` writes them, attributes
/// and spacing included: `coststamina = "1"` really does ship with spaces
/// around its equals sign, which is why the attribute census missed it once.
constexpr std::string_view kCommandsXml = R"(<commands>
<cmd name="move" priority="0" offset="1" button="Actions/Move.bmp" key="m">
  <src obj="Unit" sticky="yes"/>
</cmd>
<cmd name="stand_ground" priority="3" method="stand_position" key="s"/>
<cmd name="heal" priority="11" cursor="do_something"
  coststamina = "1"
  >
  <src obj="GDruid" sticky="yes"/>
</cmd>
<cmd name="trainhastatus" costgold="60" costfood="20" costpop="1" execdelay="9000"
  method="train" param="RHastatus" traincommand="1"/>
<cmd name="repair village" costgold="100" costfood="0"/>
<cmd name="immediate_thing" immediate="1"/>
</commands>)";

/// Two unit classes and a hero, each binding the handful of `<method sig>`
/// rows the tests queue. `Warrior` inherits `Unit`'s, which is what the 134
/// unit classes in the shipped tree do.
ClassGraph fixture_graph() {
  ClassGraph graph;
  graph.add(bytes(R"(<class id="Unit" cpp_class="CVXUnit" parent="">
      <properties maxhealth="200" speed="50" sight="500" formation_radius="26" radius="15"/>
      <method sig="idle"    vs="data/subai/unit_idle.vs"/>
      <method sig="move"    vs="data/subai/unit_move.vs"/>
      <method sig="engage"  vs="data/subai/unit_engage.vs"/>
      <method sig="advance" vs="data/subai/unit_advance.vs"/>
      <method sig="goto"    vs="data/subai/unit_move.vs"/>
      <method sig="guard"   vs="data/subai/unit_guard.vs"/>
      <method sig="unload"  vs="data/subai/wagon_unload.vs"/>
      <method sig="train"   vs="data/subai/barrack_train.vs"/>
      <method sig="research" vs="data/subai/research.vs" onfinish="data/subai/onfinish_research.vs"/>
    </class>)"),
            "unit.sc.xml");
  graph.add(bytes(R"(<class id="Warrior" cpp_class="CVXUnit" parent="Unit">
      <properties maxhealth="240"/>
    </class>)"),
            "warrior.sc.xml");
  graph.add(bytes(R"(<class id="Hero" cpp_class="CVXHero" parent="Unit">
      <properties max_army="50"/>
      <method sig="ai_killall" vs="data/subai/hero_ai_killall.vs"/>
    </class>)"),
            "hero.sc.xml");
  // A class that binds nothing at all: a command queued on it can never start,
  // and must be retired rather than block the queue forever.
  graph.add(bytes(R"(<class id="Rock" cpp_class="CVXDecor" parent=""/>)"), "rock.sc.xml");
  graph.link();
  return graph;
}

/// A world with a command system on it and nothing else. Enough for every
/// queue-shape case: with no scheduler a command is queued, marked running and
/// never retires, which is exactly what a test of queue mechanics wants.
struct Fixture {
  ClassGraph graph = fixture_graph();
  World world;
  CommandSystem commands;
  ClassIndex unit_class = kNoClass;
  ClassIndex warrior_class = kNoClass;
  ClassIndex hero_class = kNoClass;
  ClassIndex rock_class = kNoClass;

  Fixture() {
    world.set_class_graph(&graph);
    unit_class = graph.find("Unit");
    warrior_class = graph.find("Warrior");
    hero_class = graph.find("Hero");
    rock_class = graph.find("Rock");
    CommandTable table;
    (void)table.merge(bytes(kCommandsXml));
    commands.set_table(std::move(table));
    world.add_system(&commands);
  }

  ObjectId spawn(ClassIndex cls, Point at = Point{100, 100}, PlayerId owner = 1) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, cls);
    world.set_position(id, at);
    world.set_owner(id, owner);
    world.set_health(id, 100);
    return id;
  }

  /// The verbs of the whole queue, head first. What `command(i)` walks.
  std::vector<std::string> verbs(ObjectId id) const {
    std::vector<std::string> out;
    const CommandQueue* q = commands.find(id);
    if (q == nullptr) return out;
    for (const Command& command : q->entries) out.emplace_back(command.verb);
    return out;
  }

  std::uint32_t order(ObjectId id, std::string_view verb) {
    return commands.set_command(world, id, verb, Command{});
  }
  std::uint32_t add(ObjectId id, bool front, std::string_view verb) {
    return commands.add_command(world, id, front, verb, Command{});
  }
};

// -- calling into the host --------------------------------------------------

struct HostCall {
  std::vector<Value> arguments;
  CallContext context;
  imperivm::core::sim::HostContext context_state;

  HostCall(World& world, std::initializer_list<Value> args, bool bind = true)
      : arguments(args) {
    context_state.world = &world;
    context.arguments = arguments;
    context.user = bind ? &context_state : nullptr;
  }
};

HostOutcome invoke(const HostRegistry& registry, CallKind kind, std::string_view name,
                   std::uint16_t arity, HostCall& call) {
  const std::uint32_t index = registry.find(kind, name, arity);
  if (index == kUnresolvedHost) return HostOutcome::failed("not registered");
  const HostEntry& entry = registry.entry(index);
  if (entry.fn == nullptr) return HostOutcome::failed("not implemented");
  return entry.fn(call.context);
}

Value obj(ObjectId id) { return Value::object(ObjectRef{kTypeObj, id}); }

/// Parse and compile one script into `scheduler`, printing the reason on
/// failure -- a silently unbuilt chunk would make the launch cases pass by
/// never running anything.
std::uint32_t build(Scheduler& scheduler, const HostRegistry& registry, std::string_view source,
                    const char* name) {
  Diagnostic diagnostic;
  const auto parsed = parse(bytes(source), name, &diagnostic);
  if (!parsed.ok()) {
    std::printf("  parse %s:%u: %.*s\n", name, diagnostic.line,
                static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
    return kNoChunk;
  }
  CompileError error;
  auto chunk = compile(parsed.value(), &registry, &error);
  if (!chunk.ok()) {
    std::printf("  compile %s:%u: %s\n", name, error.line, error.message.c_str());
    return kNoChunk;
  }
  return scheduler.add_chunk(std::move(chunk.value()));
}

}  // namespace

// --------------------------------------------------------------------------
// the command table
// --------------------------------------------------------------------------

TEST(the_command_table_reads_every_attribute_the_shipped_files_carry) {
  CommandTable table;
  CHECK(table.merge(bytes(kCommandsXml)).ok());
  CHECK(table.size() == 6);

  const CommandDef* move = table.find("move");
  REQUIRE(move != nullptr);
  // No `method=`, so the row's own name is the `<method sig>` it queues.
  CHECK(move->method == "move");
  CHECK(move->cost_gold == 0);

  const CommandDef* stand = table.find("stand_ground");
  REQUIRE(stand != nullptr);
  // `HERO.XML` really does declare `stand_ground` with `method="stand_position"`:
  // the player-facing name and the class method are two name spaces.
  CHECK(stand->method == "stand_position");
  CHECK(stand->priority == 3);

  const CommandDef* heal = table.find("heal");
  REQUIRE(heal != nullptr);
  CHECK(heal->cost_stamina == 1);

  const CommandDef* train = table.find("trainhastatus");
  REQUIRE(train != nullptr);
  CHECK(train->cost_gold == 60);
  CHECK(train->cost_food == 20);
  CHECK(train->cost_pop == 1);
  CHECK(train->exec_delay == 9000);
  CHECK(train->param == "RHastatus");
  CHECK(train->method == "train");

  CHECK(table.find("immediate_thing")->immediate);
  // Names with spaces occur -- `GetCmdCost("repair village", gold, food)`.
  CHECK(table.find("repair village") != nullptr);
  // Case-insensitive: `cmdparam`-built names and script literals disagree.
  CHECK(table.find("TRAINHASTATUS") == train);
  CHECK(table.find("nosuchcommand") == nullptr);
}

TEST(the_command_table_refuses_a_document_that_is_not_a_command_list) {
  // Handing it a class file must fail loudly rather than produce an empty
  // table and a mystery three systems away.
  CommandTable table;
  CHECK(!table.merge(bytes(R"(<class id="Unit" cpp_class="CVXUnit" parent=""/>)")).ok());
  CHECK(table.empty());
  CHECK(!table.merge(bytes("not xml at all")).ok());
}

TEST(merging_a_second_command_file_keeps_one_sorted_table) {
  CommandTable table;
  CHECK(table.merge(bytes(kCommandsXml)).ok());
  CHECK(table.merge(bytes(R"(<commands><cmd name="unload" costgold="0"/></commands>)")).ok());
  CHECK(table.size() == 7);
  CHECK(table.find("unload") != nullptr);
  CHECK(table.find("move") != nullptr);
  // Sorted, so lookup is a binary search and iteration does not depend on
  // which file was read first.
  const std::span<const CommandDef> all = table.commands();
  for (std::size_t i = 1; i < all.size(); ++i) CHECK(all[i - 1].name < all[i].name);
}

// --------------------------------------------------------------------------
// queue shapes, transcribed from shipped scripts
// --------------------------------------------------------------------------

TEST(set_command_replaces_the_whole_queue_and_add_command_inserts) {
  Fixture f;
  const ObjectId u = f.spawn(f.unit_class);

  f.order(u, "move");
  CHECK(f.verbs(u) == std::vector<std::string>{"move"});

  // `false` appends: `TS_ATTACKATWILL.VS` writes
  // `u.SetCommand("approach", hall); u.AddCommand(false, "train");`
  f.add(u, false, "train");
  CHECK(f.verbs(u) == (std::vector<std::string>{"move", "train"}));

  // `true` inserts behind the runner, ahead of everything else.
  f.add(u, true, "engage");
  CHECK(f.verbs(u) == (std::vector<std::string>{"move", "engage", "train"}));

  // And `SetCommand` clears the lot, running command included. `GUARD.VS`
  // issues it to a unit whose runner is `idle`, and `UNIT_IDLE.VS` is a
  // `while(1)` that never returns on its own -- so it has to abort.
  f.order(u, "guard");
  CHECK(f.verbs(u) == std::vector<std::string>{"guard"});
}

TEST(add_command_to_an_empty_queue_lands_at_the_head_either_way) {
  Fixture f;
  const ObjectId a = f.spawn(f.unit_class);
  const ObjectId b = f.spawn(f.unit_class);
  f.add(a, true, "move");
  f.add(b, false, "move");
  CHECK(f.verbs(a) == std::vector<std::string>{"move"});
  CHECK(f.verbs(b) == std::vector<std::string>{"move"});
}

TEST(the_hero_ai_killall_queue_comes_out_in_the_order_its_comment_claims) {
  // DATA\SUBAI\HERO_AI_KILLALL.VS, running `ai_killall`, on the branch that
  // has no enemy in sight:
  //
  //     .AddCommand(true, "ai_killall");
  //     .AddCommand(true, "advance", set.GetCentralBuilding.pos);
  //     break;
  //
  // and the file's last line is `Sleep(500); // else commands "ai_killall" /
  // "advance" cycle`. The cycle it describes only exists if `advance` runs
  // first and `ai_killall` after it -- which is what inserting each at index 1
  // gives, and the reverse of what appending would.
  Fixture f;
  const ObjectId hero = f.spawn(f.hero_class);
  f.order(hero, "ai_killall");  // the command whose script is talking

  f.add(hero, true, "ai_killall");
  f.add(hero, true, "advance");
  CHECK(f.verbs(hero) == (std::vector<std::string>{"ai_killall", "advance", "ai_killall"}));

  // The script returns; the runner retires.
  CHECK(f.commands.kill_command(f.world, hero));
  CHECK(f.verbs(hero) == (std::vector<std::string>{"advance", "ai_killall"}));
}

TEST(the_unit_advance_queue_engages_now_and_resumes_advancing) {
  // DATA\SUBAI\UNIT_ADVANCE.VS: `.AddCommand(true, "advance", pt);
  // .AddCommand(true, "engage"); return;` -- engage the enemy it just spotted,
  // then go back to advancing on the point it was given.
  Fixture f;
  const ObjectId u = f.spawn(f.unit_class);
  f.order(u, "advance");

  Command with_point;
  with_point.arg_kind = CommandArgKind::point;
  with_point.point = Point{700, 300};
  f.commands.add_command(f.world, u, true, "advance", with_point);
  f.add(u, true, "engage");
  CHECK(f.verbs(u) == (std::vector<std::string>{"advance", "engage", "advance"}));

  f.commands.kill_command(f.world, u);
  CHECK(f.verbs(u) == (std::vector<std::string>{"engage", "advance"}));
  // The argument survives the insert: the unit resumes on the point it was
  // given, not on wherever it happens to be standing.
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr);
  CHECK(q->entries[1].arg_kind == CommandArgKind::point);
  CHECK(q->entries[1].point == (Point{700, 300}));
}

TEST(wall_patrol_walks_its_own_queue_by_index) {
  // DATA\SUBAI\WALL_PATROL.VS builds the queue and then reads it back:
  //
  //     s1.SetCommand("goto", A);
  //     s1.AddCommand(false, "goto", B);
  //     s1.AddCommand(false, "guard", This);
  //     i = 0; while (s1.command(i) == "goto") i += 1;
  //     if (s1.command(i) != "guard") { ...rebuild... }
  //
  // This is the case that fixes `command(0)` as the *running* command rather
  // than the next pending one, and `command()` as `command(0)`.
  Fixture f;
  const ObjectId sentry = f.spawn(f.unit_class);
  f.order(sentry, "goto");
  f.add(sentry, false, "goto");
  f.add(sentry, false, "guard");

  std::size_t i = 0;
  while (f.commands.command_name(sentry, i) == "goto") ++i;
  CHECK(i == 2);
  CHECK(f.commands.command_name(sentry, i) == "guard");
  CHECK(f.commands.command_name(sentry) == "goto");
  CHECK(f.commands.command_name(sentry, 3).empty());
  CHECK(f.commands.command_count(sentry) == 3);
}

TEST(kill_command_ends_the_command_and_leaves_the_object_alone) {
  // DATA\AI\ESH_FOODTRADE.VS:
  //
  //     wagon.AddCommand(false, "unload", sSell.GetCentralBuilding);
  //     wagon.KillCommand();
  //
  // A `KillCommand` that killed the *object* would destroy the mule and the
  // gold it was just loaded with, and the whole trade script would be a no-op.
  // The wagon has to survive, with `unload` at the head.
  Fixture f;
  const ObjectId wagon = f.spawn(f.unit_class);
  f.order(wagon, "idle");  // what a freshly created object is doing
  f.add(wagon, false, "unload");
  CHECK(f.commands.kill_command(f.world, wagon));

  CHECK(f.world.find(wagon) != nullptr);
  CHECK(f.world.state(wagon)->health == 100);
  CHECK(f.commands.command_name(wagon) == "unload");
  CHECK(f.commands.command_count(wagon) == 1);

  // Killing the last one empties the queue; the per-turn refill puts the
  // default back, and that is tested separately.
  CHECK(f.commands.kill_command(f.world, wagon));
  CHECK(f.commands.command_count(wagon) == 0);
  CHECK(!f.commands.kill_command(f.world, wagon));
}

TEST(clear_commands_drops_the_tail_and_keeps_the_runner) {
  // DATA\SUBAI\CATAPULT_DISBAND.VS:
  //
  //     units[i].ClearCommands();
  //     units[i].AddCommand(false, "move", pt + .pos());
  //     units[i].KillCommand();
  //
  // Append-then-kill only reaches the appended command if the runner survived
  // the clear -- otherwise `KillCommand` would end the `move` that was just
  // queued and the crew would never scatter.
  Fixture f;
  const ObjectId u = f.spawn(f.unit_class);
  f.order(u, "idle");
  f.add(u, false, "engage");
  f.add(u, false, "guard");
  CHECK(f.commands.clear_commands(f.world, u) == 2);
  CHECK(f.verbs(u) == std::vector<std::string>{"idle"});

  f.add(u, false, "move");
  f.commands.kill_command(f.world, u);
  CHECK(f.verbs(u) == std::vector<std::string>{"move"});
}

TEST(cmd_count_counts_the_runner_and_counts_by_verb) {
  // `UNIT_STANDSTILL.VS` tests `.CmdCount != 1` and `HERO_MOVE.VS`
  // `.CmdCount == 1`: one command is the resting count, so the runner is in it.
  // `CmdCount(cmd)` is the barracks form -- `num -= barracks[i].CmdCount(cmd)`.
  Fixture f;
  const ObjectId b = f.spawn(f.unit_class);
  CHECK(f.commands.command_count(b) == 0);
  f.order(b, "train");
  CHECK(f.commands.command_count(b) == 1);
  f.add(b, false, "train");
  f.add(b, false, "move");
  CHECK(f.commands.command_count(b) == 3);
  CHECK(f.commands.command_count(b, "train") == 2);
  CHECK(f.commands.command_count(b, "TRAIN") == 2);
  CHECK(f.commands.command_count(b, "engage") == 0);
}

TEST(command_ids_come_from_the_world_counter_that_reproduces_cmdidseed) {
  Fixture f;
  const ObjectId a = f.spawn(f.unit_class);
  const ObjectId b = f.spawn(f.unit_class);
  f.world.set_command_id_seed(4000);

  const std::uint32_t first = f.order(a, "move");
  const std::uint32_t second = f.add(a, false, "engage");
  const std::uint32_t third = f.order(b, "guard");
  CHECK(first == 4000);
  CHECK(second == 4001);
  CHECK(third == 4002);
  // Monotone and never reused: killing one does not hand its number back.
  f.commands.kill_command(f.world, a);
  CHECK(f.add(a, false, "move") == 4003);
  CHECK(f.world.command_id_seed() == 4004);

  // An order to an object that does not exist issues nothing and consumes no
  // id, so the counter stays in step with what actually happened.
  CHECK(f.order(9999, "move") == 0);
  CHECK(f.world.command_id_seed() == 4004);
}

// --------------------------------------------------------------------------
// the host slice
// --------------------------------------------------------------------------

/// **`SneakCommand` runs the verb now and picks the old order up after.**
/// 0x005b0930: the running command cloned to the front, the new one pushed
/// ahead of it, the runner ended -- so `[run, rest]` becomes `[verb, run',
/// rest]`, the clone keeping the runner's argument.
TEST(sneak_command_interrupts_the_runner_and_resumes_it_after) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);
  HostCall set(f.world, {obj(u), Value::string("move"), pack_point(Point{640, 480})});
  CHECK(invoke(registry, CallKind::member, "SetCommand", 2, set).status == HostStatus::ok);
  (void)f.add(u, false, "guard");
  CHECK(f.verbs(u) == (std::vector<std::string>{"move", "guard"}));

  HostCall sneak(f.world, {obj(u), Value::string("engage")});
  CHECK(invoke(registry, CallKind::member, "SneakCommand", 1, sneak).status == HostStatus::ok);
  CHECK(f.verbs(u) == (std::vector<std::string>{"engage", "move", "guard"}));
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr && q->entries.size() == 3);
  // The resumed march is the old one, argument and all, under a fresh id.
  CHECK(q->entries[1].arg_kind == CommandArgKind::point);
  CHECK(q->entries[1].point == (Point{640, 480}));
  CHECK(q->entries[1].id != q->entries[0].id);
  CHECK(q->entries[0].arg_kind == CommandArgKind::none);

  // A verb the class does not bind prints and adds nothing.
  HostCall unbound(f.world, {obj(u), Value::string("fly")});
  CHECK(invoke(registry, CallKind::member, "SneakCommand", 1, unbound).status == HostStatus::ok);
  CHECK(f.verbs(u) == (std::vector<std::string>{"engage", "move", "guard"}));

  // An empty queue is left empty: the original tests `[obj+0x98]` first.
  const ObjectId idle = f.spawn(f.unit_class);
  HostCall nothing(f.world, {obj(idle), Value::string("engage")});
  CHECK(invoke(registry, CallKind::member, "SneakCommand", 1, nothing).status == HostStatus::ok);
  CHECK(f.verbs(idle).empty());

  // A receiver that resolves to nothing is an empty receiver set, as it is
  // for the whole family (`receiver_departed`): the call answers and orders
  // nobody. A point is still the refusal.
  HostCall gone(f.world, {obj(999999), Value::string("engage")});
  CHECK(invoke(registry, CallKind::member, "SneakCommand", 1, gone).status == HostStatus::ok);
  HostCall point(f.world, {pack_point(Point{1, 1}), Value::string("engage")});
  CHECK(invoke(registry, CallKind::member, "SneakCommand", 1, point).status != HostStatus::ok);
}

TEST(register_command_host_defines_exactly_what_it_says_it_does) {
  HostRegistry registry;
  declare_shipped_surface(registry);
  const std::size_t before = registry.implemented();
  const std::size_t defined = register_command_host(registry);
  CHECK(defined == command_host_entry_count());
  CHECK(registry.implemented() - before == defined);

  // Every one of them was already in the declared surface, so nothing here
  // widened it. `find` returning `kUnresolvedHost` would mean a name or arity
  // the corpus never calls.
  const auto bound = [&registry](CallKind kind, std::string_view name, std::uint16_t arity) {
    const std::uint32_t index = registry.find(kind, name, arity);
    return index != kUnresolvedHost && registry.entry(index).fn != nullptr;
  };
  CHECK(bound(CallKind::member, "AddCommand", 2));
  CHECK(bound(CallKind::member, "AddCommand", 3));
  CHECK(bound(CallKind::member, "SetCommand", 1));
  CHECK(bound(CallKind::member, "SetCommand", 2));
  CHECK(bound(CallKind::member, "command", 0));
  CHECK(bound(CallKind::member, "command", 1));
  CHECK(bound(CallKind::member, "Idle", 0));
  CHECK(bound(CallKind::member, "Idle", 1));
  CHECK(bound(CallKind::member, "GotoAttack", 3));
  CHECK(bound(CallKind::member, "GotoAttack", 4));
  CHECK(bound(CallKind::member, "GotoEnter", 5));
  CHECK(bound(CallKind::member, "FormSetupAndMoveTo", 4));
  CHECK(bound(CallKind::member, "FormKeepMoving", 1));
  CHECK(bound(CallKind::member, "KillCommand", 0));
  CHECK(bound(CallKind::free_function, "GetCmdCost", 3));
  CHECK(bound(CallKind::free_function, "GetCmdStaminaCost", 1));
  // Member lookup is case-insensitive, so `Command` and `command` are one
  // entry point -- which is what the corpus's `.command` spelling relies on.
  CHECK(bound(CallKind::member, "Command", 0));

  // Deliberately still unimplemented; see the note at the end of
  // `register_command_host`.
  CHECK(!bound(CallKind::member, "SetCmd", 4));
  CHECK(!bound(CallKind::member, "AIDest", 0));
  // `ForceIdle` was on that list until `Idle` gained a neighbour that resolves
  // its receiver and stores nothing; see its body for why storing nothing is
  // the honest answer while `Idle` does not animate.
  CHECK(bound(CallKind::member, "ForceIdle", 0));
}

TEST(the_host_add_and_set_command_reach_the_queue) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);

  HostCall set(f.world, {obj(u), Value::string("move"), pack_point(Point{640, 480})});
  CHECK(invoke(registry, CallKind::member, "SetCommand", 2, set).status == HostStatus::ok);
  CHECK(f.verbs(u) == std::vector<std::string>{"move"});
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr);
  CHECK(q->entries[0].arg_kind == CommandArgKind::point);
  CHECK(q->entries[0].point == (Point{640, 480}));

  HostCall add(f.world, {obj(u), Value::boolean(true), Value::string("engage")});
  CHECK(invoke(registry, CallKind::member, "AddCommand", 2, add).status == HostStatus::ok);
  CHECK(f.verbs(u) == (std::vector<std::string>{"move", "engage"}));

  // An object argument, as `AddCommand(true, "enter", bldEnter)` passes.
  const ObjectId hall = f.spawn(f.unit_class, Point{900, 900});
  HostCall enter(f.world, {obj(u), Value::boolean(false), Value::string("guard"), obj(hall)});
  CHECK(invoke(registry, CallKind::member, "AddCommand", 3, enter).status == HostStatus::ok);
  CHECK(f.commands.find(u)->entries[2].arg_kind == CommandArgKind::object);
  CHECK(f.commands.find(u)->entries[2].object == hall);

  // `command()` reports the running command's *name*: 122 of its 137 sites
  // compare it against a string literal.
  HostCall name(f.world, {obj(u)});
  const HostOutcome running = invoke(registry, CallKind::member, "command", 0, name);
  CHECK(running.value.is_string());
  CHECK(running.value.as_string() == "move");
  HostCall indexed(f.world, {obj(u), Value::integer(2)});
  CHECK(invoke(registry, CallKind::member, "command", 1, indexed).value.as_string() == "guard");
  HostCall past(f.world, {obj(u), Value::integer(9)});
  CHECK(invoke(registry, CallKind::member, "command", 1, past).value.as_string().empty());

  HostCall count(f.world, {obj(u)});
  CHECK(invoke(registry, CallKind::member, "CmdCount", 0, count).value.as_integer() == 3);
}

TEST(an_object_nobody_has_ordered_is_running_idle) {
  // The original keeps the running command's name on the object (`obj+0x10c`,
  // `WaitIdle`'s compare at 0x005ece5a), and an object never ordered is
  // running its `idle`. Here that first `idle` is started outside any queue,
  // so the queue is empty -- and `ES_STRONGHOLD.VS`'s `barrack.command ==
  // "idle"` has to be true of it, or no barracks trains until something else
  // has ordered it once.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId barracks = f.spawn(f.unit_class);
  const auto name = [&](std::initializer_list<Value> args, std::uint16_t arity) {
    HostCall call(f.world, args);
    return invoke(registry, CallKind::member, "command", arity, call).value.as_string();
  };
  CHECK(name({obj(barracks)}, 0) == "idle");
  CHECK(name({obj(barracks), Value::integer(0)}, 1) == "idle");
  // Only the running slot: nothing is queued behind it.
  CHECK(name({obj(barracks), Value::integer(1)}, 1).empty());
  // And the queue is still empty -- `CmdCount` counts what is queued, and
  // `barrack.CmdCount < 5` reads the same either way.
  HostCall count(f.world, {obj(barracks)});
  CHECK(invoke(registry, CallKind::member, "CmdCount", 0, count).value.as_integer() == 0);
  CHECK(f.commands.find(barracks) == nullptr || f.commands.find(barracks)->empty());

  // A queue that drained to empty reads the same.
  f.order(barracks, "move");
  CHECK(name({obj(barracks)}, 0) == "move");
  CHECK(f.commands.kill_command(f.world, barracks));
  CHECK(name({obj(barracks)}, 0) == "idle");

  // It is the system's resting verb, not a spelling: with the refill disabled
  // an empty queue names nothing, as before.
  f.commands.set_default_verb("");
  CHECK(name({obj(barracks)}, 0).empty());
  f.commands.set_default_verb(kDefaultCommandVerb);
}

TEST(an_objlist_receiver_orders_every_member_in_list_order) {
  // `sq.Units.SetCommand("advance", pt)`, `hero.army.SetCommand("idle")`,
  // `ol.AddCommand(true, "advanceenter", oTarget)` -- the group form is
  // ordinary, not exotic.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId a = f.spawn(f.unit_class);
  const ObjectId b = f.spawn(f.unit_class);
  const ObjectId c = f.spawn(f.unit_class);

  ObjListPool& pool = objlist_pool_of(f.world);
  const ObjListId list = pool.acquire(1, 0);
  std::vector<ObjectId>* items = pool.mutable_items(list);
  REQUIRE(items != nullptr);
  *items = {a, b, c};

  HostCall set(f.world, {make_objlist_value(list), Value::string("advance")});
  CHECK(invoke(registry, CallKind::member, "SetCommand", 1, set).status == HostStatus::ok);
  CHECK(f.verbs(a) == std::vector<std::string>{"advance"});
  CHECK(f.verbs(b) == std::vector<std::string>{"advance"});
  CHECK(f.verbs(c) == std::vector<std::string>{"advance"});
  // Issued in list order, so the ids they take are in list order too.
  CHECK(f.commands.find(a)->entries[0].id < f.commands.find(b)->entries[0].id);
  CHECK(f.commands.find(b)->entries[0].id < f.commands.find(c)->entries[0].id);

  HostCall kill(f.world, {make_objlist_value(list)});
  CHECK(invoke(registry, CallKind::member, "KillCommand", 0, kill).status == HostStatus::ok);
  CHECK(f.commands.command_count(a) == 0);
  CHECK(f.commands.command_count(c) == 0);
  // And every one of them is still alive.
  CHECK(f.world.find(a) != nullptr);
  CHECK(f.world.find(c) != nullptr);
}

TEST(set_command_offset_spreads_a_group_around_the_point) {
  // `WOLF_ENGAGE.VS`: `ol.SetCommandOffset("move", ptCenter);
  // .AddCommand(true, "move", ptCenter);` -- the pack retreats to one point
  // without stacking on one cell.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId a = f.spawn(f.unit_class, Point{100, 100});
  const ObjectId b = f.spawn(f.unit_class, Point{300, 100});

  ObjListPool& pool = objlist_pool_of(f.world);
  const ObjListId list = pool.acquire(1, 0);
  *pool.mutable_items(list) = {a, b};

  HostCall call(f.world,
                {make_objlist_value(list), Value::string("move"), pack_point(Point{1000, 1000})});
  CHECK(invoke(registry, CallKind::member, "SetCommandOffset", 2, call).status ==
        HostStatus::ok);
  // The centroid is (200, 100); each keeps its offset from it.
  CHECK(f.commands.find(a)->entries[0].point == (Point{900, 1000}));
  CHECK(f.commands.find(b)->entries[0].point == (Point{1100, 1000}));
}

/// `AddCommandOffset` is the same spread, queued instead of replacing.
///
/// `6_Great_loses_Boudicca` map 1 sequence 16 is the only caller and it lays a
/// three-leg advance out one call at a time: a `SetCommandOffset` and then two
/// `AddCommandOffset(true, ...)`. So the thing to pin is that the spread is
/// identical and the queue is not cleared -- and that the centroid is taken
/// from where the group *is*, not from where its last order sent it.
TEST(add_command_offset_queues_the_same_spread_it_would_have_set) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId a = f.spawn(f.unit_class, Point{100, 100});
  const ObjectId b = f.spawn(f.unit_class, Point{300, 100});

  ObjListPool& pool = objlist_pool_of(f.world);
  const ObjListId list = pool.acquire(1, 0);
  *pool.mutable_items(list) = {a, b};

  HostCall first(f.world,
                 {make_objlist_value(list), Value::string("move"), pack_point(Point{1000, 1000})});
  REQUIRE(invoke(registry, CallKind::member, "SetCommandOffset", 2, first).status ==
          HostStatus::ok);

  // At the front, which is what every shipped site passes. `front` is index
  // **1** -- behind the running command and ahead of everything else -- which
  // is `AddCommand`'s rule and is inherited rather than restated.
  HostCall ahead(f.world, {make_objlist_value(list), Value::boolean(true),
                           Value::string("advance"), pack_point(Point{2000, 2000})});
  CHECK(invoke(registry, CallKind::member, "AddCommandOffset", 3, ahead).status ==
        HostStatus::ok);
  REQUIRE(f.commands.find(a)->entries.size() == 2);
  CHECK(f.commands.find(a)->entries[0].point == (Point{900, 1000}));   // still running
  CHECK(f.commands.find(a)->entries[1].point == (Point{1900, 2000}));  // jumped the queue
  CHECK(f.commands.find(b)->entries[1].point == (Point{2100, 2000}));

  // And at the back. **Both flags need a queue with a tail to be visible at
  // all**: with one entry, index 1 and append are the same slot, so a body that
  // ignored the flag either way would pass a shorter version of this.
  HostCall behind(f.world, {make_objlist_value(list), Value::boolean(false),
                            Value::string("guard"), pack_point(Point{5000, 1000})});
  CHECK(invoke(registry, CallKind::member, "AddCommandOffset", 3, behind).status ==
        HostStatus::ok);
  HostCall jumped(f.world, {make_objlist_value(list), Value::boolean(true),
                            Value::string("siege"), pack_point(Point{7000, 1000})});
  CHECK(invoke(registry, CallKind::member, "AddCommandOffset", 3, jumped).status ==
        HostStatus::ok);

  REQUIRE(f.commands.find(a)->entries.size() == 4);
  CHECK(f.verbs(a)[0] == "move");    // still running, never displaced
  CHECK(f.verbs(a)[1] == "siege");   // the front insert, ahead of both
  CHECK(f.verbs(a)[2] == "advance");
  CHECK(f.verbs(a)[3] == "guard");   // the back insert, still last
  CHECK(f.commands.find(a)->entries[1].point == (Point{6900, 1000}));
  CHECK(f.commands.find(a)->entries[3].point == (Point{4900, 1000}));

  // The spread is measured from where the units stand, not from the order they
  // are carrying: nothing has moved, so the centroid is still (200, 100).
  CHECK(f.commands.find(b)->entries[3].point == (Point{5100, 1000}));
}

TEST(get_cmd_cost_writes_through_its_arguments_and_reports_existence) {
  // 22 sites, every one `if (GetCmdCost(cmd, gold, food)) if (set.CanAfford(...))`,
  // which is what fixes the return as "the command exists" and not "it is
  // affordable".
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);

  HostCall known(f.world,
                 {Value::string("trainhastatus"), Value::integer(-1), Value::integer(-1)});
  const HostOutcome hit = invoke(registry, CallKind::free_function, "GetCmdCost", 3, known);
  CHECK(hit.value.as_integer() == 1);
  CHECK(known.context.arg(1).as_integer() == 60);
  CHECK(known.context.arg(2).as_integer() == 20);

  HostCall unknown(f.world,
                   {Value::string("no_such_command"), Value::integer(-1), Value::integer(-1)});
  const HostOutcome miss = invoke(registry, CallKind::free_function, "GetCmdCost", 3, unknown);
  CHECK(miss.value.as_integer() == 0);
  CHECK(unknown.context.arg(1).as_integer() == 0);

  HostCall stamina(f.world, {Value::string("heal")});
  CHECK(invoke(registry, CallKind::free_function, "GetCmdStaminaCost", 1, stamina)
            .value.as_integer() == 1);
  HostCall none(f.world, {Value::string("move")});
  CHECK(invoke(registry, CallKind::free_function, "GetCmdStaminaCost", 1, none)
            .value.as_integer() == 0);
}

TEST(exec_cmd_appends_so_a_barracks_queue_counts_up) {
  // `TSH_RECRUITARMY.VS` queues `n` training orders in a loop and the tactic
  // scripts subtract `barracks[i].CmdCount(cmd)` to decide how many more to
  // add. That only works if `ExecCmd(..., false)` appends.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId barracks = f.spawn(f.unit_class);
  f.order(barracks, "idle");

  for (int i = 0; i < 3; ++i) {
    HostCall call(f.world, {obj(barracks), Value::string("trainhastatus"), Value::nil(),
                            Value::nil(), Value::boolean(false)});
    CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, call).value.as_integer() == 1);
  }
  // Queued under the *method*, which is what `AddCommand` would have named --
  // and the resting `idle` is gone: an order does not wait behind it
  // (`append_order`; the case below has the evidence).
  CHECK(f.commands.command_count(barracks) == 3);
  CHECK(f.commands.command_count(barracks, "train") == 3);
  CHECK((f.verbs(barracks) == std::vector<std::string>{"train", "train", "train"}));

  // The row's metadata rides along: `.cmddelay` reports the running command's
  // `execdelay`, and the costs are carried for whoever binds `cmdcost_gold`.
  f.commands.kill_command(f.world, barracks);
  HostCall delay(f.world, {obj(barracks)});
  CHECK(invoke(registry, CallKind::member, "cmddelay", 0, delay).value.as_integer() == 9000);
  CHECK(f.commands.find(barracks)->entries[0].param == "RHastatus");
  CHECK(f.commands.find(barracks)->entries[0].cost_gold == 60);

  // A name the table does not carry is reported, not queued.
  HostCall bogus(f.world, {obj(barracks), Value::string("nope"), Value::nil(), Value::nil(),
                           Value::boolean(false)});
  CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, bogus).value.as_integer() == 0);
  CHECK(f.commands.command_count(barracks) == 2);

  // `replace` is the other half of the argument `GROUP_UNITSOUT.VS` names
  // `bReplace`.
  HostCall replace(f.world, {obj(barracks), Value::string("move"), pack_point(Point{5, 6}),
                             Value::nil(), Value::boolean(true)});
  CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, replace).value.as_integer() == 1);
  CHECK(f.verbs(barracks) == std::vector<std::string>{"move"});
}

TEST(exec_cmd_never_replaces_with_a_train_row) {
  // Both `ExecCmd` bodies reach the per-object issue 0x004ef3d0, which hands
  // the object's insert `replace && !traincommand` (0x004efbbb). No shipped
  // site passes true, but a train row told to replace still queues behind.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId barracks = f.spawn(f.unit_class);
  f.order(barracks, "move");
  HostCall call(f.world, {obj(barracks), Value::string("trainhastatus"), Value::nil(),
                          Value::nil(), Value::boolean(true)});
  CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, call).value.as_integer() == 1);
  CHECK((f.verbs(barracks) == std::vector<std::string>{"move", "train"}));
  // A row without the flag replaces, as before.
  HostCall replace(f.world, {obj(barracks), Value::string("repair village"), Value::nil(),
                             Value::nil(), Value::boolean(true)});
  CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, replace).value.as_integer() == 1);
  CHECK(f.commands.command_count(barracks) == 1);
}

TEST(an_order_ends_a_resting_idle_and_waits_behind_anything_else) {
  // `ES_STRONGHOLD.VS` trains an army with `barrack.ExecCmd(cmd, pt, obj,
  // false)`, up to five times, on a barracks whose `idle` is `BARRACK_IDLE.VS`
  // -- a `while (1) Sleep(...)` that never returns. Queued behind it, the
  // training never started: every computer player on Crossroads raised one
  // unit in 15,000 turns. An order through the order core therefore ends a
  // resting `idle` (a reading; `CommandSystem::append_order` says why).
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const auto exec = [&](ObjectId who) {
    HostCall call(f.world, {obj(who), Value::string("trainhastatus"), Value::nil(), Value::nil(),
                            Value::boolean(false)});
    return invoke(registry, CallKind::member, "ExecCmd", 4, call).value.as_integer();
  };

  const ObjectId barracks = f.spawn(f.unit_class);
  const std::uint32_t resting = f.order(barracks, "idle");
  CHECK(exec(barracks) == 1);
  CHECK((f.verbs(barracks) == std::vector<std::string>{"train"}));
  CHECK(f.commands.find(barracks)->entries[0].id != resting);

  // A second order finds `train` running, and `train` is not resting: it
  // queues behind it, which is what lets `CmdCount(cmd)` count the five.
  CHECK(exec(barracks) == 1);
  CHECK((f.verbs(barracks) == std::vector<std::string>{"train", "train"}));

  // Only the *head* is asked. An `idle` somewhere behind a runner -- which
  // `AddCommand` can put there -- is not what is running and stays.
  const ObjectId busy = f.spawn(f.unit_class);
  f.order(busy, "move");
  f.add(busy, false, "idle");
  CHECK(exec(busy) == 1);
  CHECK((f.verbs(busy) == std::vector<std::string>{"move", "idle", "train"}));

  // The verb is matched case-blind, as every other verb here is.
  const ObjectId shouting = f.spawn(f.unit_class);
  f.order(shouting, "IDLE");
  CHECK(exec(shouting) == 1);
  CHECK((f.verbs(shouting) == std::vector<std::string>{"train"}));

  // The verb asked about is the system's default rather than a spelling: with
  // the refill disabled, nothing counts as resting.
  f.commands.set_default_verb("");
  const ObjectId plain = f.spawn(f.unit_class);
  f.order(plain, "idle");
  CHECK(exec(plain) == 1);
  CHECK((f.verbs(plain) == std::vector<std::string>{"idle", "train"}));
  f.commands.set_default_verb(kDefaultCommandVerb);

  // An object the world does not hold gets nothing.
  CHECK(f.commands.append_order(f.world, ObjectId{99999}, "train", Command{}) == 0);
}

TEST(a_scripts_add_command_still_leaves_a_resting_idle_for_kill_command) {
  // The other half of the reading, and the reason it lives on the order path
  // only. `ESH_FOODTRADE.VS`, `ES_OUTPOSTSELLGOLD.VS`, `CATAPULT_DISBAND.VS`
  // and the `CREATE_*_MULE_*.VS` scripts write
  //
  //     wagon.AddCommand(false, "unload", hall);
  //     wagon.KillCommand();
  //
  // on an object running `idle`. If the append ended the `idle`, the kill
  // would take the `unload` and the mule would never deliver.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId wagon = f.spawn(f.unit_class);
  const ObjectId hall = f.spawn(f.unit_class, Point{900, 900});
  f.order(wagon, "idle");
  HostCall add(f.world, {obj(wagon), Value::boolean(false), Value::string("unload"), obj(hall)});
  CHECK(invoke(registry, CallKind::member, "AddCommand", 3, add).status == HostStatus::ok);
  CHECK((f.verbs(wagon) == std::vector<std::string>{"idle", "unload"}));
  HostCall kill(f.world, {obj(wagon)});
  CHECK(invoke(registry, CallKind::member, "KillCommand", 0, kill).status == HostStatus::ok);
  CHECK((f.verbs(wagon) == std::vector<std::string>{"unload"}));
}

TEST(get_can_exec_cmd_wants_both_a_row_and_a_bound_method) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);
  const ObjectId rock = f.spawn(f.rock_class);

  HostCall yes(f.world, {obj(u), Value::string("move")});
  CHECK(invoke(registry, CallKind::member, "GetCanExecCmd", 1, yes).value.as_integer() == 1);
  // The row exists but `Rock` binds no `<method sig="move">`.
  HostCall no_method(f.world, {obj(rock), Value::string("move")});
  CHECK(invoke(registry, CallKind::member, "GetCanExecCmd", 1, no_method).value.as_integer() ==
        0);
  // The method exists but there is no `<cmd>` row for it.
  HostCall no_row(f.world, {obj(u), Value::string("engage")});
  CHECK(invoke(registry, CallKind::member, "GetCanExecCmd", 1, no_row).value.as_integer() == 0);
}

TEST(cmd_disable_takes_one_command_and_cmd_enable_gives_it_back) {
  // `3_Great_Losses_Egypt`'s `seq2.vs` opens with two of these:
  // `EMarket.obj.CmdDisable("BuySlaves")`, `ETemple.obj.CmdDisable("Chariot of
  // Osiris")` -- a mission taking two purchases off two buildings.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);

  CHECK(f.commands.command_enabled(f.world, u, "move"));
  CHECK(f.commands.disabled_commands(u).empty());

  HostCall off(f.world, {obj(u), Value::string("move")});
  CHECK(invoke(registry, CallKind::member, "CmdDisable", 1, off).status == HostStatus::ok);
  CHECK(!f.commands.command_enabled(f.world, u, "move"));
  REQUIRE(f.commands.disabled_commands(u).size() == 1);
  CHECK(f.commands.disabled_commands(u)[0] == "move");

  // One command, not the object: everything else it offers still stands. That
  // is the whole difference between this and `SetCmdEnable(false)`.
  CHECK(f.commands.command_enabled(f.world, u, "stand_ground"));

  // And `GetCanExecCmd` is the reader -- 0x00560461 calls the same predicate.
  HostCall asked(f.world, {obj(u), Value::string("move")});
  CHECK(invoke(registry, CallKind::member, "GetCanExecCmd", 1, asked).value.as_integer() == 0);

  // The other half of the pair, which no shipped script calls.
  CHECK(f.commands.enable_command(u, "move"));
  CHECK(f.commands.command_enabled(f.world, u, "move"));
  // An empty set is no set.
  CHECK(f.commands.disabled_commands(u).empty());
  // And a second `CmdEnable` has nothing to give back.
  CHECK(!f.commands.enable_command(u, "move"));
}

TEST(cmd_disable_is_case_insensitive_and_idempotent_and_ignores_unknown_names) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);

  // `CommandTable::find` folds, and the corpus spells command names both ways
  // -- `Obj.ExecCmd` is called with each. What gets *stored* is the table's own
  // spelling, so the save does not carry a script's capitalisation.
  HostCall shouted(f.world, {obj(u), Value::string("MOVE")});
  CHECK(invoke(registry, CallKind::member, "CmdDisable", 1, shouted).status == HostStatus::ok);
  REQUIRE(f.commands.disabled_commands(u).size() == 1);
  CHECK(f.commands.disabled_commands(u)[0] == "move");
  CHECK(!f.commands.command_enabled(f.world, u, "move"));

  // Twice is once: the original's own guard is the `vtbl+0xd8` predicate, which
  // is already false the second time round.
  HostCall again(f.world, {obj(u), Value::string("move")});
  CHECK(invoke(registry, CallKind::member, "CmdDisable", 1, again).status == HostStatus::ok);
  CHECK(f.commands.disabled_commands(u).size() == 1);

  // A name the table does not carry writes nothing: 0x005ab7dc jumps past the
  // append when the registry answers a negative index.
  HostCall unknown(f.world, {obj(u), Value::string("no_such_command")});
  CHECK(invoke(registry, CallKind::member, "CmdDisable", 1, unknown).status == HostStatus::ok);
  CHECK(f.commands.disabled_commands(u).size() == 1);
  CHECK(!f.commands.command_enabled(f.world, u, "no_such_command"));

  // **The membership test folds, and that is reachable.** `CommandTable::set`
  // overwrites a row whose folded name matches and keeps the *new* spelling, so
  // a second `DATA/COMMANDS/*.XML` that spells the row differently leaves the
  // stored name and the table's name disagreeing in case. The command stays
  // taken away.
  CommandDef restyled;
  restyled.name = "MoVe";
  restyled.method = "move";
  f.commands.mutable_table().set(restyled);
  CHECK(!f.commands.command_enabled(f.world, u, "move"));
  REQUIRE(!f.commands.disabled_commands(u).empty());
  CHECK(f.commands.disabled_commands(u)[0] == "move");

  // Sorted, and by the folded name. Nothing enumerates the set, so this is
  // about what gets written down rather than about what gets answered.
  CHECK(f.commands.disable_command(f.world, u, "heal"));
  CHECK(f.commands.disable_command(f.world, u, "stand_ground"));
  const std::span<const std::string> set = f.commands.disabled_commands(u);
  CHECK(std::vector<std::string>(set.begin(), set.end()) ==
        (std::vector<std::string>{"heal", "move", "stand_ground"}));
}

TEST(cmd_disable_writes_nothing_while_the_object_is_silenced_altogether) {
  // The gate is the finding. `CmdDisable` appends only when `vtbl+0xd8` passes,
  // and that predicate reads `[obj+0x9c]` -- `SetCmdEnable(false)` -- before it
  // walks the list. So a script that silences an object and *then* takes one of
  // its commands away has taken nothing: lifting the flag brings the command
  // back.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);

  WorldObject* slot = f.world.find(u);
  slot->state.flags.commands_disabled = true;
  CHECK(!f.commands.command_enabled(f.world, u, "move"));

  HostCall off(f.world, {obj(u), Value::string("move")});
  CHECK(invoke(registry, CallKind::member, "CmdDisable", 1, off).status == HostStatus::ok);
  CHECK(f.commands.disabled_commands(u).empty());

  slot->state.flags.commands_disabled = false;
  CHECK(f.commands.command_enabled(f.world, u, "move"));

  // With the flag down, the same call lands, and now the flag is the thing that
  // does not matter: raising it again cannot restore what the list holds.
  CHECK(invoke(registry, CallKind::member, "CmdDisable", 1, off).status == HostStatus::ok);
  slot->state.flags.commands_disabled = true;
  slot->state.flags.commands_disabled = false;
  CHECK(!f.commands.command_enabled(f.world, u, "move"));
}

TEST(cmd_disable_neither_refuses_a_dead_receiver_nor_survives_it) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);

  // Unlike `SetCmdEnable`, which prints and returns for a dead receiver, this
  // family has no `IsAlive` test in either the entry point or the virtual.
  f.world.find(u)->state.health = 0;
  HostCall off(f.world, {obj(u), Value::string("move")});
  CHECK(invoke(registry, CallKind::member, "CmdDisable", 1, off).status == HostStatus::ok);
  CHECK(f.commands.disabled_commands(u).size() == 1);

  // But the set goes when the object does, so a long game does not accumulate
  // one row per dead building.
  f.commands.forget(u);
  CHECK(f.commands.disabled_commands(u).empty());

  // A handle that resolves to no object is **not** this family's refusal, and
  // that is the original rather than a shortcut: 0x005ab7c2 jumps past the
  // whole body when the object table answers null, and lands on the same
  // cleanup-and-return the unknown-name path uses. Nothing prints, nothing is
  // written, and the script carries on -- where `SneakCommand` two tests above
  // traps on the same argument.
  HostCall gone(f.world, {obj(999999), Value::string("move")});
  CHECK(invoke(registry, CallKind::member, "CmdDisable", 1, gone).status == HostStatus::ok);
  CHECK(f.commands.disabled_commands(999999).empty());
  // The predicate answers false for it rather than reaching through a null
  // slot, which is what makes the write above a no-op.
  CHECK(!f.commands.command_enabled(f.world, 999999, "move"));
}

TEST(get_and_set_commanded_ride_on_the_running_command) {
  // `TOWNHALL_AUTOTRAIN.VS` annotates the setter itself:
  // `u.SetCommanded(false); /// clear user commanded flag`, and reads it to
  // decide whether it may take a unit over.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);

  HostCall none(f.world, {obj(u)});
  CHECK(invoke(registry, CallKind::member, "GetCommanded", 0, none).value.as_integer() == 0);

  Command player_order;
  player_order.user = true;
  f.commands.set_command(f.world, u, "move", player_order);
  HostCall set(f.world, {obj(u)});
  CHECK(invoke(registry, CallKind::member, "GetCommanded", 0, set).value.as_integer() == 1);

  HostCall clear(f.world, {obj(u), Value::boolean(false)});
  CHECK(invoke(registry, CallKind::member, "SetCommanded", 1, clear).status == HostStatus::ok);
  HostCall after(f.world, {obj(u)});
  CHECK(invoke(registry, CallKind::member, "GetCommanded", 0, after).value.as_integer() == 0);
}

TEST(get_and_set_commanded_answer_false_for_a_handle_that_names_nothing) {
  // 0x005d81e0 and 0x005d8180: an invalid receiver prints into the bare-`ret`
  // sink and answers false, or does nothing. `TOWNHALL_AUTOTRAIN.VS:182-184`
  // gets there with `Sleep(1); u = olOutside[i].AsUnit; if (u.GetCommanded)`
  // when the unit dies during the sleep, and `AsUnit` hands back the invalid
  // handle. It used to trap here, once on Balcans in 4,000 turns.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);
  Command player_order;
  player_order.user = true;
  f.commands.set_command(f.world, u, "move", player_order);

  const Value invalid = Value::object(ObjectRef{script::kNoType, 0});
  HostCall get(f.world, {invalid});
  const HostOutcome read = invoke(registry, CallKind::member, "GetCommanded", 0, get);
  CHECK(read.status == HostStatus::ok);
  CHECK(read.value.is_integer() && read.value.as_integer() == 0);

  HostCall set(f.world, {invalid, Value::boolean(false)});
  CHECK(invoke(registry, CallKind::member, "SetCommanded", 1, set).status == HostStatus::ok);
  // Nobody else's flag moved.
  HostCall still(f.world, {obj(u)});
  CHECK(invoke(registry, CallKind::member, "GetCommanded", 0, still).value.as_integer() == 1);
  HostCall erased(f.world, {obj(999999)});
  const HostOutcome gone = invoke(registry, CallKind::member, "GetCommanded", 0, erased);
  CHECK(gone.status == HostStatus::ok && gone.value.as_integer() == 0);
}

TEST(idle_is_an_order_that_costs_game_time) {
  // 63 sites, not one of them a condition and not one assigned: it is an order.
  // The suspension is the only behaviour the corpus proves, and it proves it --
  // `SHIP_IDLE.VS`'s `while(1) { ... if (.Stop(200)) .Idle(); }` spins the
  // scheduler if `Idle` costs nothing.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);

  HostCall timed(f.world, {obj(u), Value::integer(1900)});
  const HostOutcome slept = invoke(registry, CallKind::member, "Idle", 1, timed);
  CHECK(slept.status == HostStatus::suspend);
  CHECK(slept.suspend_for == 1900);

  HostCall bare(f.world, {obj(u)});
  const HostOutcome fallback = invoke(registry, CallKind::member, "Idle", 0, bare);
  CHECK(fallback.status == HostStatus::suspend);
  CHECK(fallback.suspend_for == kDefaultIdleSlice);
  CHECK(fallback.suspend_for > 0);  // or SHIP_IDLE.VS spins

  // It does *not* touch the queue: idling is what a command does, not a
  // command in itself.
  CHECK(f.commands.command_count(u) == 0);
}

/// `Taunt` yields where it can taunt and sleeps where it cannot.
///
/// `Unit::Taunt` and `Unit::Idle` are the same function in `gbr.exe`, one
/// constant apart, and the branch they share is on the receiver's *position*:
/// `(-1, -1)` -- a unit inside a holder, whose location is its holder's -- takes
/// the argument as a sleep and does nothing else, and anything with a real
/// position drops its combat target and ends the slice. The 2,000 every shipped
/// site passes is therefore only ever consumed by the held branch.
TEST(taunt_drops_the_target_on_the_map_and_waits_in_a_holder) {
  Fixture f;
  CombatSystem combat;
  REQUIRE(f.world.add_system(&combat));
  HostRegistry registry;
  register_command_host(registry);

  const ObjectId u = f.spawn(f.unit_class, Point{100, 100});
  const ObjectId foe = f.spawn(f.unit_class, Point{140, 100}, /*owner=*/2);
  Combatant a;
  a.id = u;
  a.owner = 1;
  a.health = 100;
  a.position = Point{100, 100};
  a.target = foe;
  a.attacks = 3;
  a.action = Action::engaging;
  combat.add(a);

  HostCall standing(f.world, {obj(u), Value::integer(2000)});
  const HostOutcome yielded = invoke(registry, CallKind::member, "Taunt", 1, standing);
  // Suspended with no timed wake: one scheduler step, not two seconds.
  CHECK(yielded.status == HostStatus::suspend);
  CHECK(yielded.suspend_for == 0);
  // "A new order supersedes combat" -- the prologue this shares with `Stop`,
  // both `Goto`s, `Attack` and `AttackEveryone`.
  REQUIRE(combat.find(u) != nullptr);
  CHECK(combat.find(u)->target == kNoObject);
  CHECK(combat.find(u)->attacks == 0);

  // Inside a holder there is no position to taunt from, so the argument
  // becomes a wait and nothing is dropped.
  const ObjectId inside = f.spawn(f.unit_class, Point{200, 200});
  const World::SettlementIds town = f.world.spawn_settlement(1);
  REQUIRE(f.world.put_in_holder(inside, town.holder));
  Combatant held;
  held.id = inside;
  held.owner = 1;
  held.health = 100;
  held.target = foe;
  held.attacks = 2;
  combat.add(held);

  HostCall boxed(f.world, {obj(inside), Value::integer(2000)});
  const HostOutcome waited = invoke(registry, CallKind::member, "Taunt", 1, boxed);
  CHECK(waited.status == HostStatus::suspend);
  CHECK(waited.suspend_for == 2000);
  CHECK(combat.find(inside)->target == foe);   // untouched
  CHECK(combat.find(inside)->attacks == 2);

  // A held receiver with a non-positive argument has nothing to wait for.
  HostCall zero(f.world, {obj(inside), Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "Taunt", 1, zero).status == HostStatus::ok);

  // It touches the queue no more than `Idle` does.
  CHECK(f.commands.command_count(u) == 0);
}

/// A world with no combat system taunts without falling over, and the two ways
/// of naming nothing part company.
TEST(taunt_without_a_combat_system_still_yields) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);

  HostCall standing(f.world, {obj(u), Value::integer(2000)});
  CHECK(invoke(registry, CallKind::member, "Taunt", 1, standing).status == HostStatus::suspend);

  // No receiver at all refuses, which is `resolve`'s answer and `Idle`'s too.
  HostCall nobody(f.world, {obj(kNoObject), Value::integer(2000)});
  CHECK(invoke(registry, CallKind::member, "Taunt", 1, nobody).status == HostStatus::error);

  // A handle that is a number but names no object gets past `resolve` and is
  // caught here instead: this body reads the position and the original
  // dereferences null in the same place, which is a fault rather than a
  // behaviour.
  HostCall stale(f.world, {obj(static_cast<ObjectId>(9999)), Value::integer(2000)});
  CHECK(invoke(registry, CallKind::member, "Taunt", 1, stale).status == HostStatus::ok);
}

// --------------------------------------------------------------------------
// running a command's script
// --------------------------------------------------------------------------

namespace {

/// A world with a command system, a scheduler, and three tiny scripts standing
/// in for `DATA\SUBAI`'s.
struct RunFixture : Fixture {
  HostRegistry registry;
  WorldHost host{world};
  Scheduler scheduler;
  imperivm::core::sim::HostContext context;

  RunFixture() {
    declare_shipped_surface(registry);
    (void)register_world_host(registry);
    register_scheduler_builtins(registry);
    (void)register_command_host(registry);
    context.world = &world;
    scheduler.set_registry(&registry);
    scheduler.set_host(&host);
    scheduler.set_user(&context);
    commands.set_scheduler(&scheduler);
  }

  bool add_script(std::string_view source, const char* path) {
    return build(scheduler, registry, source, path) != kNoChunk;
  }
};

}  // namespace

/// **A command's script is compiled on first use, and before this nothing
/// compiled it.**
///
/// `Scheduler::spawn_by_name` is a lookup in the already-loaded library, and
/// the only files ever preloaded are `AI.INI`'s `[Scripts]` manifest and the
/// per-object `idle` methods. A class `<method>` like `move` is named by
/// neither -- so every order on every map resolved to a real path, failed to
/// spawn, and was retired by `service` without a word. Every test in this file
/// built its scripts into the scheduler by hand first, which is exactly why
/// none of them could notice.
TEST(a_command_compiles_its_script_through_the_library_and_counts_the_misses) {
  /// Compiles from a fixed table of sources, the way the session compiles from
  /// the installation.
  struct TableLibrary final : imperivm::core::sim::ScriptLibrary {
    Scheduler* scheduler = nullptr;
    const HostRegistry* registry = nullptr;
    std::vector<std::pair<std::string, std::string>> sources;
    std::size_t asked = 0;

    std::uint32_t chunk_for(std::string_view path) override {
      ++asked;
      for (const auto& [name, source] : sources) {
        if (name != path) continue;
        return build(*scheduler, *registry, source, name.c_str());
      }
      return kNoChunk;
    }
  };

  RunFixture f;
  TableLibrary library;
  library.scheduler = &f.scheduler;
  library.registry = &f.registry;
  library.sources.emplace_back("data/subai/unit_move.vs",
                               "// void, Obj This, point pt\nSleep(500);\n");
  f.commands.set_library(&library);

  // **Not pre-built.** The scheduler has never heard of it.
  CHECK(f.scheduler.find_chunk("data/subai/unit_move.vs") == kNoChunk);

  const ObjectId u = f.spawn(f.unit_class);
  Command order;
  order.arg_kind = CommandArgKind::point;
  order.point = Point{300, 300};
  f.commands.set_command(f.world, u, "move", order);
  f.world.advance(100);

  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr);
  REQUIRE(q->running() != nullptr);
  CHECK(q->running()->verb == "move");
  CHECK(q->running()->script != kNoScript);
  CHECK(f.scheduler.alive(q->running()->script));
  CHECK(library.asked == 1);
  // Compiled once: the second order finds it in the scheduler and the library
  // is not asked again... which is the library's own business, so what is
  // asserted here is only that the command still launches.
  CHECK(f.scheduler.find_chunk("data/subai/unit_move.vs") != kNoChunk);
  CHECK(f.commands.launch_failures().empty());
}

TEST(an_order_behind_a_never_returning_idle_runs_and_the_idle_script_ends) {
  // The shape `ES_STRONGHOLD.VS` met on every barracks: the refill put
  // `BARRACK_IDLE.VS` -- `while (1) Sleep(...)` -- at the head, and a training
  // order appended behind it waited forever. Through the order core it runs,
  // and the `idle` it displaced stops running rather than lingering as a
  // coroutine nobody's queue owns.
  RunFixture f;
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n", "data/subai/unit_idle.vs"));
  REQUIRE(f.add_script("// void, Obj me\nSleep(500);\n", "data/subai/barrack_train.vs"));

  const ObjectId barracks = f.spawn(f.unit_class);
  // An order whose script is missing retires, and the queue refills with a
  // running `idle`, which is how a barracks comes to rest on one.
  f.commands.set_command(f.world, barracks, "guard", Command{});
  f.world.advance(100);
  const CommandQueue* q = f.commands.find(barracks);
  REQUIRE(q != nullptr);
  REQUIRE(q->running() != nullptr);
  REQUIRE(q->running()->verb == "idle");
  const ScriptId resting = q->running()->script;
  REQUIRE(f.scheduler.alive(resting));

  CHECK(f.commands.append_order(f.world, barracks, "train", Command{}) != 0);
  CHECK(!f.scheduler.alive(resting));
  f.world.advance(100);
  REQUIRE(q->running() != nullptr);
  CHECK(q->running()->verb == "train");
  CHECK(f.scheduler.alive(q->running()->script));

  // Once `train` returns the barracks goes back to resting, which is the
  // state the next pass of `ES_STRONGHOLD.VS` asks `command == "idle"` of.
  for (int i = 0; i < 10; ++i) {
    f.scheduler.advance(100);
    f.world.advance(100);
  }
  REQUIRE(q->running() != nullptr);
  CHECK(q->running()->verb == "idle");
}

TEST(a_command_whose_script_will_not_compile_is_counted_rather_than_vanishing) {
  // The failure used to be invisible: the command was erased and the queue
  // refilled with `idle`, which looks exactly like a unit that was never
  // ordered. That is the shape the whole family of these bugs takes.
  RunFixture f;
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n",
                       "data/subai/unit_idle.vs"));

  const ObjectId u = f.spawn(f.unit_class);
  Command order;
  order.arg_kind = CommandArgKind::point;
  order.point = Point{300, 300};
  f.commands.set_command(f.world, u, "move", order);
  f.world.advance(100);

  // No library and no pre-built chunk, so `move` cannot start. It retires and
  // the queue falls back to `idle` -- and the miss is now on the record.
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr);
  REQUIRE(q->running() != nullptr);
  CHECK(q->running()->verb == "idle");
  REQUIRE(f.commands.launch_failures().size() == 1);
  CHECK(f.commands.launch_failures().begin()->first == "move");
  CHECK(f.commands.launch_failures().begin()->second == 1);
}

TEST(a_command_runs_its_class_script_and_retires_when_the_script_returns) {
  RunFixture f;
  // `UNIT_MOVE.VS` opens `// void, Obj This, point pt`, so the receiver is the
  // first argument and the command's own argument the second. This one records
  // both and returns.
  REQUIRE(f.add_script("// void, Obj This, point pt\nSleep(500);\n",
                       "data/subai/unit_move.vs"));
  REQUIRE(f.add_script("// void, Obj me\nSleep(100);\n", "data/subai/unit_engage.vs"));
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n",
                       "data/subai/unit_idle.vs"));

  const ObjectId u = f.spawn(f.unit_class);
  Command with_point;
  with_point.arg_kind = CommandArgKind::point;
  with_point.point = Point{300, 300};
  f.commands.set_command(f.world, u, "move", with_point);
  f.commands.add_command(f.world, u, false, "engage", Command{});

  // The turn loop starts the head.
  f.world.advance(100);
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr);
  CHECK(q->running() != nullptr);
  CHECK(q->running()->script != kNoScript);
  CHECK(f.scheduler.alive(q->running()->script));
  // And the command knows which coroutine is its own, which is what the
  // ambient `cmdparam` globals will need.
  CHECK(f.commands.command_of_script(q->running()->script) == q->running());

  // `move` sleeps 500 and returns; `engage` takes over. The two clocks are
  // driven together, which is what an embedder does: the scheduler resumes
  // coroutines and the turn loop then notices which of them ended.
  const auto step = [&f](std::int32_t ms) {
    f.scheduler.advance(ms);
    f.world.advance(ms);
  };
  for (int i = 0; i < 10 && f.commands.command_name(u) == "move"; ++i) step(200);
  CHECK(f.commands.command_name(u) == "engage");
  CHECK(f.commands.command_count(u) == 1);

  // `engage` returns too, and the queue refills with the class's `idle` --
  // `GUARD.VS` reads exactly that as an ordinary resting state.
  for (int i = 0; i < 10 && f.commands.command_name(u) == "engage"; ++i) step(200);
  CHECK(f.commands.command_name(u) == "idle");
  CHECK(f.commands.command_count(u) == 1);
  // `UNIT_IDLE.VS` never returns, so the resting state is stable.
  for (int i = 0; i < 20; ++i) step(1000);
  CHECK(f.commands.command_name(u) == "idle");
  CHECK(f.commands.command_count(u) == 1);
}

/// **A method's `onfinish` runs when its command is over**, with `(This,
/// bCanceled)`: false when the script returned, true when the command was
/// killed or replaced under it. It sees the command it is finishing --
/// `cmdparam` still answers -- which is how `ONFINISH_RESEARCH.VS` knows which
/// ledger key to write `researched` into. Without it every upgrade any player
/// started stayed `researching` for the rest of the match.
TEST(a_commands_onfinish_runs_with_the_cancel_flag_and_the_commands_param) {
  RunFixture f;
  (void)register_global_hosts(f.registry);
  (void)register_text_host(f.registry);
  REQUIRE(f.add_script("// void, Obj This\nSleep(500);\n", "data/subai/research.vs"));
  // Records what it was told through `SetUser`: 100 + the param's length,
  // plus 1000 when cancelled.
  REQUIRE(f.add_script(
      "// void, Obj This, bool bCanceled\nstr p; p = cmdparam;\n"
      "if (bCanceled) This.SetUser(1100 + StrLen(p)); else This.SetUser(100 + StrLen(p));\n",
      "data/subai/onfinish_research.vs"));
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n",
                       "data/subai/unit_idle.vs"));
  const auto step = [&f](std::int32_t ms) {
    f.scheduler.advance(ms);
    f.world.advance(ms);
  };

  // Runs to its end: `bCanceled` false.
  const ObjectId u = f.spawn(f.unit_class);
  Command order;
  order.param = "NameSet, Fights";  // 15 characters
  f.commands.set_command(f.world, u, "research", order);
  for (int i = 0; i < 10 && f.commands.command_name(u) == "research"; ++i) step(200);
  CHECK(f.commands.command_name(u) == "idle");
  step(200);  // the onfinish spawned at the turn's end runs on the next pass
  CHECK(f.world.find(u)->state.user == 115);

  // Replaced while running: `bCanceled` true.
  f.commands.set_command(f.world, u, "research", order);
  step(200);
  REQUIRE(f.commands.command_name(u) == "research");
  Command idle;
  f.commands.set_command(f.world, u, "idle", idle);
  step(200);
  CHECK(f.world.find(u)->state.user == 1115);

  // Killed while running: `bCanceled` true too.
  f.world.find(u)->state.user = 0;
  f.commands.set_command(f.world, u, "research", order);
  step(200);
  REQUIRE(f.commands.command_name(u) == "research");
  CHECK(f.commands.kill_command(f.world, u));
  step(200);
  CHECK(f.world.find(u)->state.user == 1115);

  // A command that never started has no `onfinish`: two researches queued
  // and replaced before the turn loop launched either.
  f.world.find(u)->state.user = 0;
  f.commands.set_command(f.world, u, "idle", idle);
  step(200);
  f.world.find(u)->state.user = 0;
  f.commands.set_command(f.world, u, "research", order);
  f.commands.add_command(f.world, u, false, "research", order);
  f.commands.set_command(f.world, u, "idle", idle);
  step(200);
  CHECK(f.world.find(u)->state.user == 0);

  // A resting head that an order displaces (`append_order`) is cancelled, the
  // way `KillCommand` cancels. No shipped `idle` binds an `onfinish`, so the
  // resting verb is pointed at `research` for long enough to hear it.
  f.commands.set_default_verb("research");
  f.commands.set_command(f.world, u, "research", order);
  step(200);
  REQUIRE(f.commands.command_name(u) == "research");
  CHECK(f.commands.append_order(f.world, u, "idle", idle) != 0);
  step(200);
  CHECK(f.world.find(u)->state.user == 1115);
  f.commands.set_default_verb(kDefaultCommandVerb);
}

/// **A parameter the caller never passed is its type's default, not nil.**
/// `HERO_STAND_GROUND.VS` is declared `(Obj me, point pt)`, re-issues itself
/// as `SetCommand("stand_position")` with no point, and reads `pt` on the next
/// pass: nil there trapped `GetVec` on every hero told to stand its ground,
/// nine times over the map sweep. A frame's slots start zeroed in the original,
/// so `pt` is `(0, 0)`, an unpassed `int` is 0 and an unpassed handle invalid.
TEST(a_missing_script_argument_is_its_declared_types_default) {
  RunFixture f;
  // `move` shows what it was given through how long it sleeps: `pt.x + n +
  // 300`, which is only well-formed when `pt` is a point and `n` a number, and
  // is 300 exactly when both are their defaults; an unpassed handle is
  // invalid, so the branch that would sleep for ever is not taken.
  REQUIRE(f.add_script("// void, Obj This, point pt, int n, Obj other\n"
                       "if (other.IsValid) while (1) Sleep(1000);\n"
                       "Sleep(pt.x + n + 300);\n",
                       "data/subai/unit_move.vs"));
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n",
                       "data/subai/unit_idle.vs"));
  const ObjectId u = f.spawn(f.unit_class);
  // A plain `move` with no argument at all: one argument pushed, three left.
  f.commands.set_command(f.world, u, "move", Command{});
  const auto step = [&f](std::int32_t ms) {
    f.scheduler.advance(ms);
    f.world.advance(ms);
  };
  step(100);
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr && q->running() != nullptr);
  // Not trapped, and asleep for 300 from its start at 100: still `move` at
  // 250, `idle` by 550.
  CHECK(f.scheduler.alive(q->running()->script));
  step(150);
  CHECK(f.commands.command_name(u) == "move");
  step(300);
  CHECK(f.commands.command_name(u) == "idle");
}

TEST(a_command_whose_class_binds_no_script_is_retired_rather_than_blocking) {
  RunFixture f;
  REQUIRE(f.add_script("// void, Obj me\nSleep(50);\n", "data/subai/unit_engage.vs"));
  const ObjectId rock = f.spawn(f.rock_class);
  f.commands.set_command(f.world, rock, "engage", Command{});
  // `Rock` binds nothing, and the default verb is not bound either, so the
  // queue drains rather than looping on a command that can never start.
  f.world.advance(100);
  CHECK(f.commands.command_count(rock) == 0);
  f.world.advance(100);
  CHECK(f.commands.command_count(rock) == 0);
}

TEST(with_no_scheduler_a_queue_is_pure_mechanics) {
  // The configuration every queue-shape case above runs in, asserted rather
  // than assumed: nothing retires, so `advance` cannot quietly drain a queue a
  // test just built.
  Fixture f;
  const ObjectId u = f.spawn(f.unit_class);
  f.order(u, "move");
  f.add(u, false, "engage");
  for (int i = 0; i < 5; ++i) f.world.advance(400);
  CHECK(f.verbs(u) == (std::vector<std::string>{"move", "engage"}));
}

TEST(the_class_method_table_resolves_through_inheritance) {
  Fixture f;
  const ObjectId warrior = f.spawn(f.warrior_class);
  // `Warrior` declares no methods of its own; all eight come from `Unit`,
  // which is what 134 shipped unit classes do.
  CHECK(f.commands.script_for(f.world, warrior, "move") == "data/subai/unit_move.vs");
  CHECK(f.commands.script_for(f.world, warrior, "MOVE") == "data/subai/unit_move.vs");
  CHECK(f.commands.script_for(f.world, warrior, "ai_killall").empty());
  const ObjectId hero = f.spawn(f.hero_class);
  CHECK(f.commands.script_for(f.world, hero, "ai_killall") == "data/subai/hero_ai_killall.vs");
  CHECK(f.commands.script_for(f.world, hero, "idle") == "data/subai/unit_idle.vs");
}

// --------------------------------------------------------------------------
// the cross-domain orders
// --------------------------------------------------------------------------

TEST(goto_attack_arrives_exactly_when_the_attack_would_connect) {
  // `UNIT_ENGAGE.VS`: `while (!.GotoAttack(u, 1500, true, 15000)) {...}` and
  // then `while (.Attack(u));`. So the truth of `GotoAttack` has to be combat's
  // own reach test and not a number chosen here.
  Fixture f;
  MovementSystem movement;
  CombatSystem combat;
  movement.set_grid(ObstructionGrid(64, 64));
  f.world.add_system(&movement);
  f.world.add_system(&combat);

  HostRegistry registry;
  register_movement_host(registry);
  register_command_host(registry);

  const ObjectId archer = f.spawn(f.unit_class, Point{100, 100}, 1);
  const ObjectId target = f.spawn(f.unit_class, Point{600, 100}, 2);
  movement.state(archer).speed = 100;

  CombatProfile profile;
  profile.range = 300;
  profile.radius = 15;
  combat.set_profile(f.unit_class, profile);
  Combatant a;
  a.id = archer;
  a.class_index = f.unit_class;
  a.owner = 1;
  a.position = Point{100, 100};
  a.health = 100;
  combat.add(a);
  Combatant d = a;
  d.id = target;
  d.owner = 2;
  d.position = Point{600, 100};
  combat.add(d);

  HostCall call(f.world, {obj(archer), obj(target), Value::integer(1500), Value::boolean(true),
                          Value::integer(15000)});
  const HostOutcome first = invoke(registry, CallKind::member, "GotoAttack", 4, call);
  CHECK(first.value.as_integer() == 0);  // 500 apart, reach is 330
  CHECK(first.status == HostStatus::suspend);
  // The order it laid stops at the weapon's reach, not on top of the target.
  const MoveState* move = movement.find(archer);
  REQUIRE(move != nullptr);
  CHECK(move->range == profile.range + 2 * profile.radius);
  CHECK(move->min_range == profile.min_range);

  // Walk until it reports true, then check the distance really is inside reach.
  bool arrived = false;
  for (int i = 0; i < 40 && !arrived; ++i) {
    f.world.advance(400);
    HostCall again(f.world, {obj(archer), obj(target), Value::integer(1500),
                             Value::boolean(true), Value::integer(15000)});
    arrived = invoke(registry, CallKind::member, "GotoAttack", 4, again).value.as_integer() != 0;
  }
  CHECK(arrived);
  CHECK(distance(f.world.resolve_position(archer), f.world.resolve_position(target)) <= 330);
}

TEST(goto_attack_without_a_combat_system_walks_onto_its_target) {
  // Degrading to `Goto(target, 0, ...)` rather than refusing: a world with no
  // combat registered is a synthetic one, and a trap there would be noise.
  Fixture f;
  MovementSystem movement;
  movement.set_grid(ObstructionGrid(64, 64));
  f.world.add_system(&movement);
  HostRegistry registry;
  register_command_host(registry);

  const ObjectId a = f.spawn(f.unit_class, Point{100, 100});
  const ObjectId b = f.spawn(f.unit_class, Point{200, 100});
  movement.state(a).speed = 100;
  HostCall call(f.world,
                {obj(a), obj(b), Value::integer(1000), Value::boolean(true), Value::integer(0)});
  const HostOutcome outcome = invoke(registry, CallKind::member, "GotoAttack", 4, call);
  CHECK(outcome.value.as_integer() == 0);
  CHECK(movement.find(a)->range == 0);
}

TEST(goto_enter_accepts_the_end_of_a_partial_route) {
  // `UNIT_ENTER.VS` puts `Goto` and `GotoEnter` in the arms of one `if` with
  // identical arguments, and the destination is a building's `GetEnterPoint` --
  // a doorway that the building's own footprint may block. `Goto` keeps
  // searching; `GotoEnter` takes what it can reach.
  Fixture f;
  MovementSystem movement;
  ObstructionGrid grid(64, 64);
  // A sealed room with the destination inside it.
  for (std::int32_t d = -2; d <= 2; ++d) {
    grid.set_cell(30 + d, 28, true);
    grid.set_cell(30 + d, 32, true);
    grid.set_cell(28, 30 + d, true);
    grid.set_cell(32, 30 + d, true);
  }
  movement.set_grid(std::move(grid));
  f.world.add_system(&movement);
  HostRegistry registry;
  register_movement_host(registry);
  register_command_host(registry);

  const ObjectId u = f.spawn(f.unit_class, Point{100, 100});
  movement.state(u).speed = 400;
  const Value door = pack_point(Point{30 * 16 + 8, 30 * 16 + 8});

  bool entered = false;
  for (int i = 0; i < 60 && !entered; ++i) {
    HostCall call(f.world, {obj(u), door, Value::integer(0), Value::integer(1000),
                            Value::boolean(true), Value::integer(20000)});
    entered = invoke(registry, CallKind::member, "GotoEnter", 5, call).value.as_integer() != 0;
    f.world.advance(500);
  }
  CHECK(entered);
  // It stopped outside the wall, which is the whole point: it got as close as
  // the obstruction allows and called that arrival.
  CHECK(f.world.resolve_position(u) != (Point{30 * 16 + 8, 30 * 16 + 8}));

  // Plain `Goto` to the same sealed point never reports arrival.
  const ObjectId v = f.spawn(f.unit_class, Point{100, 100});
  movement.state(v).speed = 400;
  bool reached = false;
  for (int i = 0; i < 60 && !reached; ++i) {
    HostCall call(f.world, {obj(v), door, Value::integer(0), Value::integer(1000),
                            Value::boolean(true), Value::integer(0)});
    reached = invoke(registry, CallKind::member, "Goto", 5, call).value.as_integer() != 0;
    f.world.advance(500);
  }
  CHECK(!reached);
}

TEST(form_setup_moves_the_hero_and_keep_moving_carries_the_army) {
  // `HERO_MOVE.VS` is the whole protocol:
  //     .FormSetupAndMoveTo(pt, 0, 0, true);
  //     while (.HasPath()) .FormKeepMoving(1500);
  Fixture f;
  MovementSystem movement;
  HeroSystem heroes;
  movement.set_grid(ObstructionGrid(128, 128));
  Result<FormationTable> formations = FormationTable::parse(bytes(R"(<Formations>
      <Default Name="Front"/>
      <FormationClass Name="Front" Width="2" Height="1" OffsetFrontLineByY="60"
        OffsetWingsByX="80" OffsetWingsByY="-40">
        <Class Name="Unit" CentralBlock="1"/>
      </FormationClass>
    </Formations>)"));
  REQUIRE(formations.ok());
  movement.set_formations(std::move(formations.value()));
  f.world.add_system(&movement);
  f.world.add_system(&heroes);

  HostRegistry registry;
  register_movement_host(registry);
  register_command_host(registry);

  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr, f.hero_class);
  f.world.set_position(hero, Point{200, 200});
  f.world.set_owner(hero, 1);
  f.world.set_health(hero, 1000);
  heroes.register_hero(f.world, hero);
  movement.state(hero).speed = 100;

  std::vector<ObjectId> army;
  for (int i = 0; i < 4; ++i) {
    const ObjectId w = f.world.spawn(NativeClass::unit, nullptr, f.warrior_class);
    f.world.set_position(w, Point{200 + 10 * i, 200});
    f.world.set_owner(w, 1);
    f.world.set_health(w, 200);
    heroes.register_unit(f.world, w);
    CHECK(heroes.attach(f.world, w, hero));
    movement.state(w).speed = 140;
    army.push_back(w);
  }

  HostCall setup(f.world, {obj(hero), pack_point(Point{2000, 200}), Value::integer(0),
                           Value::integer(0), Value::boolean(true)});
  CHECK(invoke(registry, CallKind::member, "FormSetupAndMoveTo", 4, setup).status ==
        HostStatus::ok);
  CHECK(f.world.state(hero)->flags.has_active_path);
  // Every warrior got a station of its own, so the army does not stack on the
  // hero's own cell.
  for (const ObjectId w : army) {
    const MoveState* member = movement.find(w);
    REQUIRE(member != nullptr);
    CHECK(member->goto_active);
  }
  bool distinct = true;
  for (std::size_t i = 1; i < army.size(); ++i) {
    if (movement.find(army[i])->target == movement.find(army[0])->target) distinct = false;
  }
  CHECK(distinct);
  // And the march is one party -- the hero and every member carry the hero's
  // id -- so their cooperative steps do not block one another
  // (`sim/movement.hpp`, inference 3).
  CHECK(movement.find(hero)->party == hero);
  for (const ObjectId w : army) CHECK(movement.find(w)->party == hero);

  // The march: `FormKeepMoving` suspends and the loop's exit is `.HasPath`.
  int calls = 0;
  while (calls < 200) {
    HostCall has_path(f.world, {obj(hero)});
    if (invoke(registry, CallKind::member, "HasPath", 0, has_path).value.as_integer() == 0) {
      break;
    }
    HostCall keep(f.world, {obj(hero), Value::integer(1500)});
    const HostOutcome outcome = invoke(registry, CallKind::member, "FormKeepMoving", 1, keep);
    CHECK(outcome.value.is_nil());
    const std::int64_t wait = outcome.status == HostStatus::suspend ? outcome.suspend_for : 400;
    f.world.advance(static_cast<std::int32_t>(wait > 0 ? wait : 400));
    ++calls;
  }
  CHECK(calls < 200);
  CHECK(f.world.resolve_position(hero) == (Point{2000, 200}));
  // The army followed rather than walking to the hero's original station.
  for (const ObjectId w : army) {
    CHECK(distance(f.world.resolve_position(w), Point{2000, 200}) < 600);
  }
}

TEST(form_keep_moving_waits_for_its_whole_argument_and_never_for_the_eta) {
  // `FormKeepMoving(ms)` used to clamp its wait to `movement->eta(id)` and
  // return the shorter of the two, which reads well against `HERO_MOVE.VS`'s
  // `while (.HasPath()) .FormKeepMoving(1500);` -- the last slice lands exactly
  // on arrival instead of overshooting.
  //
  // 0x0052eaf0 does not do that, and there is no distance in the body to do it
  // with. First entry raises bit 0x10000 on the hero, writes the `ms` argument
  // to the scheduler's wait global at `[0xa77eac]` and returns **1**; re-entry
  // clears the bit and returns 0. The march above it is unaffected -- the loop's
  // exit is `.HasPath`, not this call's wait -- so the only thing the clamp
  // bought was a shorter final slice, and it cost a divergence on every call
  // whose ETA is under its argument.
  Fixture f;
  MovementSystem movement;
  movement.set_grid(ObstructionGrid(64, 64));
  f.world.add_system(&movement);
  // `FormSetupAndMoveTo` places the army and refuses without a hero system.
  HeroSystem heroes;
  f.world.add_system(&heroes);
  HostRegistry registry;
  register_movement_host(registry);
  register_command_host(registry);

  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr, f.hero_class);
  f.world.set_position(hero, Point{100, 100});
  f.world.set_owner(hero, 1);
  f.world.set_health(hero, 1000);
  heroes.register_hero(f.world, hero);
  movement.state(hero).speed = 100;

  // A march short enough that its ETA is well under the wait asked for. This is
  // the discriminating case: the two readings agree whenever the ETA is longer.
  HostCall setup(f.world, {obj(hero), pack_point(Point{140, 100}), Value::integer(0),
                           Value::integer(0), Value::boolean(true)});
  CHECK(invoke(registry, CallKind::member, "FormSetupAndMoveTo", 4, setup).status ==
        HostStatus::ok);
  const std::int64_t eta = movement.eta(hero);
  {
    // Measured, not inferred, and kept because it is what makes the *other*
    // reading of this entry point wrong. `HERO_IDLE.VS` calls
    // `.FormSetupAndMoveTo(.pos, ...)` -- a goto to the position the object
    // already occupies -- and it is tempting to say the old ETA clamp fired
    // there and turned that arm of its `while(1)` into a non-yielding loop.
    // It did not: a goto to self leaves no path at all, `eta` answers **-1**
    // for that, and `eta >= 0` was never true. Whatever spins in `hero_idle.vs`
    // is not this.
    Fixture g;
    MovementSystem m2;
    m2.set_grid(ObstructionGrid(64, 64));
    g.world.add_system(&m2);
    HeroSystem h2;
    g.world.add_system(&h2);
    const ObjectId ho = g.world.spawn(NativeClass::hero, nullptr, g.hero_class);
    g.world.set_position(ho, Point{100, 100});
    g.world.set_owner(ho, 1);
    g.world.set_health(ho, 1000);
    h2.register_hero(g.world, ho);
    m2.state(ho).speed = 100;
    HostCall self_goto(g.world, {obj(ho), pack_point(Point{100, 100}), Value::integer(0),
                                 Value::integer(0), Value::boolean(true)});
    CHECK(invoke(registry, CallKind::member, "FormSetupAndMoveTo", 4, self_goto).status ==
          HostStatus::ok);
    REQUIRE(m2.find(ho) != nullptr);
    CHECK(!m2.find(ho)->has_path);
    CHECK(m2.eta(ho) == -1);
  }
  REQUIRE(eta > 0);
  REQUIRE(eta < 5000);

  HostCall keep(f.world, {obj(hero), Value::integer(5000)});
  const HostOutcome outcome = invoke(registry, CallKind::member, "FormKeepMoving", 1, keep);
  CHECK(outcome.status == HostStatus::suspend);
  CHECK(outcome.suspend_for == 5000);  // not `eta`

  // A zero or absent duration is the one case that does not suspend: there is
  // nothing to wait for, and a zero-length suspension is a yield the original
  // does not make.
  HostCall zero(f.world, {obj(hero), Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "FormKeepMoving", 1, zero).status == HostStatus::ok);
}

TEST(form_path_left_is_a_distance) {
  // Both sites compare it against a radius --
  // `if (.FormPathLeft() < .FormRadius() + s.radius) break;`
  Fixture f;
  MovementSystem movement;
  movement.set_grid(ObstructionGrid(64, 64));
  f.world.add_system(&movement);
  HostRegistry registry;
  register_movement_host(registry);
  register_command_host(registry);

  const ObjectId hero = f.spawn(f.hero_class, Point{100, 100});
  movement.state(hero).speed = 100;
  HostCall idle(f.world, {obj(hero)});
  CHECK(invoke(registry, CallKind::member, "FormPathLeft", 0, idle).value.as_integer() == 0);

  movement.order_goto(f.world, hero, Point{900, 100}, 0);
  HostCall walking(f.world, {obj(hero)});
  const std::int32_t left =
      invoke(registry, CallKind::member, "FormPathLeft", 0, walking).value.as_integer();
  CHECK(left > 700);
  CHECK(left < 900);
}

// --------------------------------------------------------------------------
// the null-user sweep
// --------------------------------------------------------------------------

TEST(every_command_entry_point_refuses_a_null_user_rather_than_dereferencing) {
  // `CallContext::user` is null whenever a script runs outside an embedder, and
  // a host function that dereferenced it would take the process down. Asserted
  // over the whole slice rather than sampled, because the one that is missed is
  // the one that gets called.
  HostRegistry registry;
  register_command_host(registry);
  World world;

  struct Case {
    CallKind kind;
    const char* name;
    std::uint16_t arity;
    std::size_t args;
  };
  constexpr Case cases[] = {
      {CallKind::member, "AddCommand", 2, 3},   {CallKind::member, "AddCommand", 3, 4},
      {CallKind::member, "SneakCommand", 1, 2},
      {CallKind::member, "SetCommand", 1, 2},   {CallKind::member, "SetCommand", 2, 3},
      {CallKind::member, "command", 0, 1},      {CallKind::member, "command", 1, 2},
      {CallKind::member, "Idle", 0, 1},         {CallKind::member, "Idle", 1, 2},
      {CallKind::member, "ForceIdle", 0, 1},   {CallKind::member, "Taunt", 1, 2},
      {CallKind::member, "GotoAttack", 3, 4},   {CallKind::member, "GotoAttack", 4, 5},
      {CallKind::member, "KillCommand", 0, 1},  {CallKind::member, "CmdCount", 0, 1},
      {CallKind::member, "CmdCount", 1, 2},     {CallKind::member, "FormKeepMoving", 1, 2},
      {CallKind::member, "FormSetupAndMoveTo", 4, 5},
      {CallKind::member, "FormAcceptMove", 0, 1},
      {CallKind::member, "GotoEnter", 5, 6},    {CallKind::member, "ExecCmd", 4, 5},
      {CallKind::member, "GetCommanded", 0, 1}, {CallKind::member, "cmddelay", 0, 1},
      {CallKind::member, "Progress", 0, 1},     {CallKind::member, "Progress", 1, 2},
      {CallKind::member, "SetCommandOffset", 2, 3},
      {CallKind::member, "AddCommandOffset", 3, 4},
      {CallKind::member, "ClearCommands", 0, 1},
      {CallKind::member, "SetCommanded", 1, 2}, {CallKind::member, "FormPathLeft", 0, 1},
      {CallKind::member, "GetCanExecCmd", 1, 2},
      {CallKind::member, "CmdDisable", 1, 2},
      {CallKind::free_function, "GetCmdStaminaCost", 1, 1},
      {CallKind::free_function, "GetCmdCost", 3, 3},
  };
  // The sweep covers the whole slice, so a new entry point that forgets the
  // check also fails this count.
  CHECK(std::size(cases) == command_host_entry_count());

  for (const Case& c : cases) {
    std::vector<Value> arguments(c.args, Value::integer(0));
    arguments[0] = Value::object(ObjectRef{kTypeObj, 1});
    CallContext context;
    context.arguments = arguments;
    context.user = nullptr;
    const std::uint32_t index = registry.find(c.kind, c.name, c.arity);
    REQUIRE(index != kUnresolvedHost);
    const HostEntry& entry = registry.entry(index);
    REQUIRE(entry.fn != nullptr);
    const HostOutcome outcome = entry.fn(context);
    if (outcome.status != HostStatus::error) {
      std::printf("  %s/%u did not refuse a null user\n", c.name, c.arity);
    }
    CHECK(outcome.status == HostStatus::error);
    CHECK(outcome.error != nullptr);
  }

  // And a receiver that is not an object at all is refused too, rather than
  // read as object zero.
  HostCall bad(world, {Value::integer(7), Value::boolean(true), Value::string("move")});
  CHECK(invoke(registry, CallKind::member, "AddCommand", 2, bad).status == HostStatus::error);
}

// --------------------------------------------------------------------------
// the receivers the command family was refusing
// --------------------------------------------------------------------------

TEST(a_query_receiver_orders_every_member_in_group_order) {
  // `gbr.exe` registers `SetCommand` three times -- on `Obj` (0x005b7568), on
  // `ObjList` (0x00563659) and on `Query` (0x0057af1a) -- each with its own
  // body, and the `Query` body walks the query's members and applies the verb
  // once per member. This engine keys its registry on (kind, name, arity), so
  // one body answers all three, and the receiver expansion is the part that
  // has to know about it.
  //
  // It did not. `receivers_of` took `Obj` and `ObjList` only, while
  // `membership_receivers` eight files away took all four types, so a bare
  // `<group>` name -- which resolves to a `Query` -- reached `AddToGroup` and
  // was refused by `SetCommand`. 545 shipped command-family sites.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId a = f.spawn(f.unit_class);
  const ObjectId b = f.spawn(f.unit_class);
  const ObjectId c = f.spawn(f.unit_class);

  // Added in an order that is *not* ascending id order, which is the shape
  // `mediterranean` map 4's `Q_Interceptors1..4.AddToGroup("Q_Slingers")` has
  // and the reason `GroupTable` keeps first-add order at all.
  const std::int32_t group = f.world.groups().intern("Q_Slingers");
  CHECK(f.world.groups().add(group, c));
  CHECK(f.world.groups().add(group, a));
  CHECK(f.world.groups().add(group, b));
  const ObjectId query = f.world.create_query(group_query(group));

  HostCall set(f.world, {Value::object(ObjectRef{kTypeQuery, query}), Value::string("advance")});
  CHECK(invoke(registry, CallKind::member, "SetCommand", 1, set).status == HostStatus::ok);
  CHECK(f.verbs(a) == std::vector<std::string>{"advance"});
  CHECK(f.verbs(b) == std::vector<std::string>{"advance"});
  CHECK(f.verbs(c) == std::vector<std::string>{"advance"});

  // And in *group* order, not id order. Without this half the test passes on
  // an implementation that sorts, which is the exact defect the group table
  // was carrying two commits ago.
  REQUIRE(f.commands.find(c) != nullptr);
  REQUIRE(f.commands.find(a) != nullptr);
  REQUIRE(f.commands.find(b) != nullptr);
  CHECK(f.commands.find(c)->entries[0].id < f.commands.find(a)->entries[0].id);
  CHECK(f.commands.find(a)->entries[0].id < f.commands.find(b)->entries[0].id);
}

TEST(a_query_that_selects_nothing_is_a_receiver_and_not_a_refusal) {
  // The two cases that used to be the same empty vector and mean opposite
  // things: a live query with no members is a receiver whose call applies to
  // nobody, exactly as an empty `ObjList` receiver is; a point is not a
  // receiver at all and refuses by name.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const std::int32_t group = f.world.groups().intern("Empty");
  const ObjectId query = f.world.create_query(group_query(group));

  HostCall empty(f.world, {Value::object(ObjectRef{kTypeQuery, query}), Value::string("advance")});
  CHECK(invoke(registry, CallKind::member, "SetCommand", 1, empty).status == HostStatus::ok);

  HostCall point(f.world, {pack_point(Point{5, 5}), Value::string("advance")});
  CHECK(invoke(registry, CallKind::member, "SetCommand", 1, point).status == HostStatus::error);
  HostCall number(f.world, {Value::integer(3), Value::string("advance")});
  CHECK(invoke(registry, CallKind::member, "SetCommand", 1, number).status == HostStatus::error);
}

TEST(a_named_object_receiver_orders_the_one_object_it_names) {
  // `NO_Hero1.SetCommand("stand_position")` ships, in `Maps/10/Sequences/
  // seq0.vs`, on a name its own `map.obj.xml` declares `<group type="0">`.
  // There is no `NamedObj::SetCommand` registration in `gbr.exe` and none is
  // needed: `NamedObj` is a registered *subtype* of `Query` (0x005b6d96), so
  // the compiler upcasts at zero cost and the receiver is a one-member query.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId hero = f.spawn(f.hero_class);
  CHECK(f.world.named_objects().bind("NO_Hero1", hero));
  const std::int32_t index = f.world.named_objects().find("NO_Hero1");
  REQUIRE(index >= 0);
  const Value named = Value::object(ObjectRef{kTypeNamedObj, static_cast<std::uint32_t>(index)});

  HostCall set(f.world, {named, Value::string("advance")});
  CHECK(invoke(registry, CallKind::member, "SetCommand", 1, set).status == HostStatus::ok);
  CHECK(f.verbs(hero) == std::vector<std::string>{"advance"});

  // A dead binding is an empty query rather than an invalid receiver -- the
  // original's `CVXNamedObjQuery` goes empty, and the name stays bound to a
  // handle that is never reissued -- and the order is a no-op, not a
  // refusal: `Obj::SetCommand` (0x005b0a60) prints "called for invalid
  // object" into a sink that is a bare `ret` and returns 0. Great Losses
  // Rome's `seq7.vs` orders `NO_Hero2` twelve seconds after the hero can have
  // died, and the rest of the sequence has to run.
  CHECK(f.world.despawn(hero));
  HostCall again(f.world, {named, Value::string("idle")});
  const HostOutcome out = invoke(registry, CallKind::member, "SetCommand", 1, again);
  CHECK(out.status == HostStatus::ok);
  // The invalid handle itself, likewise.
  HostCall nobody(f.world, {Value::object(ObjectRef{script::kNoType, 0}), Value::string("idle")});
  CHECK(invoke(registry, CallKind::member, "SetCommand", 1, nobody).status == HostStatus::ok);
}

TEST(exec_cmd_refuses_the_query_receiver_the_others_take) {
  // The one entry point in the family that is genuinely narrower: `gbr.exe`
  // registers `ExecCmd` on `Obj` and `ObjList` and **not** on `Query`, where
  // it registers `SetCommand`, `AddCommand`, `KillCommand` and `ClearCommands`
  // on all three. No shipped site passes `ExecCmd` a query, so this is
  // unobservable on retail data -- which is exactly why it is a stated choice
  // with a test rather than a widening nobody would notice.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId a = f.spawn(f.unit_class);
  const std::int32_t group = f.world.groups().intern("Q_Slingers");
  CHECK(f.world.groups().add(group, a));
  const ObjectId query = f.world.create_query(group_query(group));
  const Value q = Value::object(ObjectRef{kTypeQuery, query});

  HostCall set(f.world, {q, Value::string("advance")});
  CHECK(invoke(registry, CallKind::member, "SetCommand", 1, set).status == HostStatus::ok);

  // Arity 4 as a member, which means five values with the receiver. A first
  // version of this test passed arity 3, resolved nothing, and read the
  // registry's own "not registered" as the refusal it was looking for --
  // passing for the wrong reason, and the injected fault said so by surviving.
  // The `Obj` receiver beside it is what proves the entry point is reachable
  // at this arity at all.
  HostCall exec(f.world,
                {q, Value::string("advance"), pack_point(Point{9, 9}), Value::integer(0),
                 Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, exec).status == HostStatus::error);

  HostCall direct(f.world,
                  {Value::object(ObjectRef{kTypeObj, a}), Value::string("advance"),
                   pack_point(Point{9, 9}), Value::integer(0), Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, direct).status == HostStatus::ok);
}

// --------------------------------------------------------------------------
// Progress
// --------------------------------------------------------------------------

/// `.Progress()` suspends for the running command's `execdelay`, and
/// `.Progress(ms)` for what it is handed.
TEST(command_progress_suspends_for_the_commands_own_delay) {
  RunFixture f;
  REQUIRE(f.add_script("// void, Obj me\nme.Progress();\n", "data/subai/barrack_train.vs"));
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n",
                       "data/subai/unit_idle.vs"));

  // `Command::delay` is `<cmd execdelay>`, which is what `.cmddelay` reports
  // and what the no-argument `Progress` waits out.
  const ObjectId u = f.spawn(f.unit_class);
  Command order;
  order.delay = 9000;
  f.commands.set_command(f.world, u, "train", order);
  f.world.advance(100);
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr);
  REQUIRE(q->running() != nullptr);
  const script::ScriptId id = q->running()->script;
  REQUIRE(q->running()->delay == 9000);

  // `Scheduler::advance` moves its clock and *then* runs, so this is the tick
  // the script first runs on. A trap here would also leave it dead further
  // down, so the report is checked rather than the liveness alone.
  const script::RunReport started = f.scheduler.advance(1);
  CHECK(started.traps.empty());
  CHECK(started.failed == 0);
  REQUIRE(f.scheduler.alive(id));

  // One millisecond short of the delay, still asleep.
  f.scheduler.advance(8999);
  CHECK(f.scheduler.alive(id));
  // And on the tick that reaches it, the script runs off the end.
  f.scheduler.advance(2);
  CHECK(!f.scheduler.alive(id));
}

/// The argument form takes its duration from the argument, and the command's
/// own delay is nine thousand -- so a body that read the wrong one is asleep
/// where this one has finished, and awake where it has not.
TEST(command_progress_with_an_argument_uses_the_argument) {
  RunFixture f;
  REQUIRE(f.add_script("// void, Obj me\nme.Progress(500);\n", "data/subai/barrack_train.vs"));
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n",
                       "data/subai/unit_idle.vs"));

  const ObjectId u = f.spawn(f.unit_class);
  Command order;
  order.delay = 9000;
  f.commands.set_command(f.world, u, "train", order);
  f.world.advance(100);
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr);
  REQUIRE(q->running() != nullptr);
  const script::ScriptId id = q->running()->script;
  REQUIRE(q->running()->delay == 9000);

  const script::RunReport started = f.scheduler.advance(1);
  CHECK(started.traps.empty());
  REQUIRE(f.scheduler.alive(id));
  f.scheduler.advance(499);
  CHECK(f.scheduler.alive(id));
  f.scheduler.advance(2);
  CHECK(!f.scheduler.alive(id));
}

/// **`Progress(0)` still suspends.** The argument form has no zero guard --
/// 0x005ac578 stamps and returns 1 whatever it was handed -- and that is the
/// only thing separating the two forms once the duration is in hand. A body
/// that shared the no-argument form's guard finishes this script one pass
/// earlier.
TEST(command_progress_with_a_zero_argument_still_yields) {
  RunFixture f;
  REQUIRE(f.add_script("// void, Obj me\nme.Progress(0);\n", "data/subai/unit_move.vs"));
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n",
                       "data/subai/unit_idle.vs"));

  const ObjectId u = f.spawn(f.unit_class);
  f.commands.set_command(f.world, u, "move", Command{});
  f.world.advance(100);
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr);
  REQUIRE(q->running() != nullptr);
  const script::ScriptId id = q->running()->script;

  // Pass one runs the script as far as the yield and no further.
  const script::RunReport first = f.scheduler.advance(1);
  CHECK(first.traps.empty());
  CHECK(first.completed == 0);
  CHECK(f.scheduler.alive(id));

  // Pass two resumes it, and it runs off the end.
  const script::RunReport second = f.scheduler.advance(1);
  CHECK(second.traps.empty());
  CHECK(second.completed == 1);
  CHECK(!f.scheduler.alive(id));
}

/// A command with **no** delay: `.Progress()` does not suspend at all, and the
/// script runs straight through. 0x005ae952 is the guard, and it is the reason
/// the no-argument form exists separately.
TEST(command_progress_does_not_suspend_when_the_command_has_no_delay) {
  RunFixture f;
  REQUIRE(f.add_script("// void, Obj me\nme.Progress();\n", "data/subai/unit_engage.vs"));
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n",
                       "data/subai/unit_idle.vs"));

  const ObjectId u = f.spawn(f.unit_class);
  f.commands.add_command(f.world, u, false, "engage", Command{});
  f.world.advance(100);
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr);
  REQUIRE(q->running() != nullptr);
  REQUIRE(q->running()->delay == 0);
  const script::ScriptId id = q->running()->script;

  // The tick the script runs on is the tick it finishes on -- and it finished
  // rather than trapped, which is the half `!alive` alone cannot tell.
  const script::RunReport ran = f.scheduler.advance(1);
  CHECK(ran.traps.empty());
  CHECK(ran.completed == 1);
  CHECK(!f.scheduler.alive(id));
}

// --------------------------------------------------------------------------
// KillScript
// --------------------------------------------------------------------------

/// `KillScript()` ends the coroutine that called it, and nothing after it runs.
///
/// `gbr.exe` 0x0061e730 is `mov eax, 2; ret` -- status 2 is *stop this
/// coroutine*, beside 0 for carry on and 1 for suspend. Its eight scripts use
/// it as an early return out of a nested loop.
TEST(command_kill_script_ends_the_calling_coroutine) {
  RunFixture f;
  // The `Sleep` after it must never run: if `KillScript` were a no-op the
  // script would still be alive on the next tick, and if it were a trap the
  // report would say so.
  REQUIRE(f.add_script("// void, Obj me\nKillScript();\nSleep(100000);\n",
                       "data/subai/unit_move.vs"));
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n",
                       "data/subai/unit_idle.vs"));

  const ObjectId u = f.spawn(f.unit_class);
  f.commands.set_command(f.world, u, "move", Command{});
  f.world.advance(100);
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr);
  REQUIRE(q->running() != nullptr);
  const script::ScriptId id = q->running()->script;

  const script::RunReport ran = f.scheduler.advance(1);
  CHECK(ran.traps.empty());
  CHECK(ran.failed == 0);
  CHECK(!f.scheduler.alive(id));

  // And it stays dead: a long sleep the script never reached would have it
  // waking up here.
  f.scheduler.advance(200000);
  CHECK(!f.scheduler.alive(id));
}

TEST(form_accept_move_finishes_at_once_because_the_station_is_already_ordered) {
  // A member that reaches `UNIT_FORM_MOVE.VS` on this engine was ordered to
  // its station by `place_army` when its hero moved, so the original's
  // "nothing pending" exit is the answer: finish, do not suspend, touch
  // nothing -- and the same for a handle naming nothing.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId unit = f.spawn(f.warrior_class);
  (void)f.order(unit, "form_move");
  const std::vector<std::string> before = f.verbs(unit);
  const std::uint64_t hash_before = f.world.state_hash();

  HostCall live(f.world, {obj(unit)});
  const HostOutcome out = invoke(registry, CallKind::member, "FormAcceptMove", 0, live);
  CHECK(out.status == HostStatus::ok);
  CHECK(f.verbs(unit) == before);
  CHECK(f.world.state_hash() == hash_before);

  HostCall stale(f.world, {obj(999999)});
  CHECK(invoke(registry, CallKind::member, "FormAcceptMove", 0, stale).status == HostStatus::ok);
}
