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

  /// The call's word of state (`CallContext::scratch`), as the VM keeps it.
  std::int32_t scratch = 0;

  HostCall(World& world, std::initializer_list<Value> args, bool bind = true)
      : arguments(args) {
    context_state.world = &world;
    context.arguments = arguments;
    context.user = bind ? &context_state : nullptr;
    context.scratch = &scratch;
  }

  /// Run the same call again after its wait, as the VM does after a `retry`.
  HostCall& reenter() {
    context.first_call = false;
    return *this;
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

/// `Unit::Disappear` (0x005d86d0) leaves the command queue alone. It idles the
/// unit's activity (`[obj+0x130]`), drops its path, stops its animation and
/// stands it at `(-1, -1)` -- and nothing else. The ambient villager depends on
/// that: `SETTLEMENT_BEHAVIOR_AMBIENT.VS` queues `moveenter`, `disappear`,
/// `grab_water`, `moveenter` home, and a disappear that emptied the queue left
/// the villager at the well forever.
TEST(disappear_keeps_the_commands_queued_behind_it) {
  Fixture f;
  HostRegistry hosts;
  (void)imperivm::core::sim::register_world_host(hosts);
  const ObjectId u = f.spawn(f.unit_class);
  f.order(u, "idle");
  f.add(u, false, "engage");
  f.add(u, false, "guard");
  HostCall call(f.world, {obj(u)});
  CHECK(invoke(hosts, CallKind::member, "Disappear", 0, call).status == HostStatus::ok);
  CHECK(f.world.resolve_position(u) == imperivm::core::sim::kHeldPosition);
  CHECK(f.verbs(u) == (std::vector<std::string>{"idle", "engage", "guard"}));
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

  const std::uint32_t runner = f.commands.find(u)->entries.front().id;
  const std::uint32_t next = f.world.next_command_id();
  HostCall sneak(f.world, {obj(u), Value::string("engage")});
  CHECK(invoke(registry, CallKind::member, "SneakCommand", 1, sneak).status == HostStatus::ok);
  CHECK(f.verbs(u) == (std::vector<std::string>{"engage", "move", "guard"}));
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr && q->entries.size() == 3);
  // The resumed march is the old one, argument, row and id: the clone draws
  // an id in its constructor and then takes the runner's (0x00599cdb). The
  // new command drew the id after the one the probe above took, the clone
  // the one after that.
  CHECK(q->entries[1].arg_kind == CommandArgKind::point);
  CHECK(q->entries[1].point == (Point{640, 480}));
  CHECK(q->entries[1].id == runner);
  CHECK(q->entries[0].id == next + 1);
  CHECK(f.world.next_command_id() == next + 3);
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

TEST(the_bare_command_of_an_empty_queue_is_idle) {
  // The original's bare `command` (0x005abe40) reads the name of the command
  // the object last started, a string on the object; the indexed form
  // (0x005ae620) reads the queue, `""` past its end. A started object's queue
  // holds its running `idle` (`CommandSystem::start_slot`); a queue the
  // fixture never started is empty, and the bare form answers the resting
  // verb for it (the labelled reading at `command_impl`) --
  // `ES_STRONGHOLD.VS`'s `barrack.command == "idle"`.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId barracks = f.spawn(f.unit_class);
  const auto name = [&](std::initializer_list<Value> args, std::uint16_t arity) {
    HostCall call(f.world, args);
    return invoke(registry, CallKind::member, "command", arity, call).value.as_string();
  };
  CHECK(name({obj(barracks)}, 0) == "idle");
  // The indexed form reads the queue, which holds nothing.
  CHECK(name({obj(barracks), Value::integer(0)}, 1).empty());
  CHECK(name({obj(barracks), Value::integer(1)}, 1).empty());
  // And the queue is still empty: `CmdCount` counts what is queued.
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
                            Value::string("goto"), pack_point(Point{7000, 1000})});
  CHECK(invoke(registry, CallKind::member, "AddCommandOffset", 3, jumped).status ==
        HostStatus::ok);

  REQUIRE(f.commands.find(a)->entries.size() == 4);
  CHECK(f.verbs(a)[0] == "move");    // still running, never displaced
  CHECK(f.verbs(a)[1] == "goto");    // the front insert, ahead of both
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
  // A miss writes neither argument (0x004256b8): they keep what was in them.
  CHECK(unknown.context.arg(1).as_integer() == -1);
  CHECK(unknown.context.arg(2).as_integer() == -1);

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
    CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, call).status == HostStatus::ok);
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

  // A name the table does not carry queues nothing ("Invalid command",
  // 0x005b5554), and `ExecCmd` is void: there is no answer to read.
  HostCall bogus(f.world, {obj(barracks), Value::string("nope"), Value::nil(), Value::nil(),
                           Value::boolean(false)});
  CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, bogus).status == HostStatus::ok);
  CHECK(f.commands.command_count(barracks) == 2);

  // `replace` is the other half of the argument `GROUP_UNITSOUT.VS` names
  // `bReplace`.
  HostCall replace(f.world, {obj(barracks), Value::string("move"), pack_point(Point{5, 6}),
                             Value::nil(), Value::boolean(true)});
  CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, replace).status == HostStatus::ok);
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
  CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, call).status == HostStatus::ok);
  CHECK((f.verbs(barracks) == std::vector<std::string>{"move", "train"}));
  // A row without the flag replaces, as before.
  HostCall replace(f.world, {obj(barracks), Value::string("move"), pack_point(Point{5, 6}),
                             Value::nil(), Value::boolean(true)});
  CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, replace).status == HostStatus::ok);
  CHECK(f.commands.command_count(barracks) == 1);
  // And a row whose method the class does not bind issues nothing ("Cmd Not
  // resolved to method", 0x004ef8a6): `repair village` is no `Unit` method.
  HostCall unbound(f.world, {obj(barracks), Value::string("repair village"), Value::nil(),
                             Value::nil(), Value::boolean(true)});
  CHECK(invoke(registry, CallKind::member, "ExecCmd", 4, unbound).status == HostStatus::ok);
  CHECK(f.verbs(barracks) == std::vector<std::string>{"move"});
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
    return invoke(registry, CallKind::member, "ExecCmd", 4, call).status == HostStatus::ok ? 1 : 0;
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

/// `ObjList::GetCanExecCmd(name)` (0x00560300) is a filter: a new list of the
/// members the row is enabled for (`vtbl+0xd8`). It does not ask whether the
/// class binds the method, and an unknown row gives an empty list.
TEST(get_can_exec_cmd_lists_the_members_the_row_is_enabled_for) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);
  const ObjectId v = f.spawn(f.unit_class, Point{300, 100});
  const ObjectId rock = f.spawn(f.rock_class, Point{500, 100});
  ObjListPool& pool = objlist_pool_of(f.world);
  const ObjListId list = pool.acquire(1, 0);
  *pool.mutable_items(list) = {u, v, rock};
  CHECK(f.commands.disable_command(f.world, v, "move"));

  const auto members = [&](std::string_view name) -> std::vector<ObjectId> {
    HostCall call(f.world, {make_objlist_value(list), Value::string(std::string(name))});
    const HostOutcome out = invoke(registry, CallKind::member, "GetCanExecCmd", 1, call);
    if (out.status != HostStatus::ok) return {};
    const std::span<const ObjectId> items = pool.items(objlist_of(out.value));
    return std::vector<ObjectId>(items.begin(), items.end());
  };
  // `Rock` binds no `move`, and is listed all the same; `v` has lost it.
  CHECK((members("move") == std::vector<ObjectId>{u, rock}));
  CHECK(members("no such row").empty());
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

TEST(get_and_set_commanded_are_the_units_own_flag_raised_by_every_issue) {
  // `TOWNHALL_AUTOTRAIN.VS` annotates the setter itself:
  // `u.SetCommanded(false); /// clear user commanded flag`, and reads it to
  // decide whether it may take a unit over. The flag is bit 0x80000 of
  // `[unit+0x194]` (0x005d81e0, 0x005d8180): the per-object issue raises it
  // on every unit an order reaches (0x004ef475), `ExecCmd` included, and it
  // outlives the order -- nothing but `SetCommanded` lowers it.
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);
  f.world.mutable_state(u)->flags.is_unit = true;

  HostCall none(f.world, {obj(u)});
  CHECK(invoke(registry, CallKind::member, "GetCommanded", 0, none).value.as_integer() == 0);

  // A script's own `SetCommand` does not raise it; an issued row does.
  f.commands.set_command(f.world, u, "move", Command{});
  HostCall plain(f.world, {obj(u)});
  CHECK(invoke(registry, CallKind::member, "GetCommanded", 0, plain).value.as_integer() == 0);
  HostCall exec(f.world, {obj(u), Value::string("move"), pack_point(Point{5, 6}), Value::nil(),
                          Value::boolean(false)});
  REQUIRE(invoke(registry, CallKind::member, "ExecCmd", 4, exec).status == HostStatus::ok);
  // And it stays up when the order is over.
  f.commands.kill_command(f.world, u);
  f.commands.set_command(f.world, u, "idle", Command{});
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
  f.commands.set_commanded(u, true);

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
  CHECK(slept.status == HostStatus::retry);  // the activity's steps are re-entries
  CHECK(slept.suspend_for == 1900);

  HostCall bare(f.world, {obj(u)});
  const HostOutcome fallback = invoke(registry, CallKind::member, "Idle", 0, bare);
  CHECK(fallback.status == HostStatus::retry);
  CHECK(fallback.suspend_for == kDefaultIdleSlice);
  CHECK(fallback.suspend_for > 0);  // or SHIP_IDLE.VS spins

  // It does *not* touch the queue: idling is what a command does, not a
  // command in itself.
  CHECK(f.commands.command_count(u) == 0);
}

/// `Unit::Idle` on the map drops the combat target and still costs its
/// argument; in a holder it only waits.
///
/// 0x005d6030 sets the idle activity, empties `[unit+0x1a8]` and returns 3,
/// which ends the slice with no timed wake (0x0069d5fb); the idle activity
/// (0x005d2ca0) spends the popped `ms` and then wakes the script. A held
/// receiver writes `ms` to the wait cell and returns 1 instead, and its target
/// is left alone.
TEST(idle_on_the_map_drops_the_target_and_still_waits_its_argument) {
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

  HostCall standing(f.world, {obj(u), Value::integer(1900)});
  const HostOutcome idled = invoke(registry, CallKind::member, "Idle", 1, standing);
  CHECK(idled.status == HostStatus::retry);
  CHECK(idled.suspend_for == 1900);
  REQUIRE(combat.find(u) != nullptr);
  CHECK(combat.find(u)->target == kNoObject);
  CHECK(combat.find(u)->attacks == 0);
  CHECK(combat.find(u)->action == Action::idle);

  // A non-positive argument still ends the slice, as every 3 does.
  combat.find(u)->target = foe;
  HostCall zero(f.world, {obj(u), Value::integer(0)});
  const HostOutcome yielded = invoke(registry, CallKind::member, "Idle", 1, zero);
  CHECK(yielded.status == HostStatus::suspend);
  CHECK(yielded.suspend_for == 0);
  CHECK(combat.find(u)->target == kNoObject);

  // Inside a holder: the argument as a wait, and the target kept.
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
  HostCall boxed(f.world, {obj(inside), Value::integer(1500)});
  const HostOutcome waited = invoke(registry, CallKind::member, "Idle", 1, boxed);
  CHECK(waited.status == HostStatus::suspend);
  CHECK(waited.suspend_for == 1500);
  CHECK(combat.find(inside)->target == foe);
  CHECK(combat.find(inside)->attacks == 2);
}

/// `Taunt` costs its argument wherever it is, and drops the target on the map.
///
/// `Unit::Taunt` and `Unit::Idle` are the same function in `gbr.exe`, one
/// constant apart, and the branch they share is on the receiver's *position*:
/// `(-1, -1)` -- a unit inside a holder, whose location is its holder's -- takes
/// the argument as a sleep and does nothing else, and anything with a real
/// position drops its combat target and returns 3, whose wake comes from the
/// taunt activity once it has spent the popped argument (0x005d2ca0). So the
/// 2,000 every shipped site passes is two seconds on the map too.
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
  // Two seconds of taunting, not one scheduler step: `UNIT_CAPTURE.VS` takes
  // a point of loyalty after each.
  CHECK(yielded.status == HostStatus::retry);
  CHECK(yielded.suspend_for == 2000);
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

  // A non-positive argument still suspends, for one pass: the held branch
  // writes it to the wait cell and returns 1, the other returns 3.
  HostCall zero(f.world, {obj(inside), Value::integer(0)});
  const HostOutcome brief = invoke(registry, CallKind::member, "Taunt", 1, zero);
  CHECK(brief.status == HostStatus::suspend);
  CHECK(brief.suspend_for == 0);

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
  CHECK(invoke(registry, CallKind::member, "Taunt", 1, standing).status == HostStatus::retry);

  // No receiver at all refuses, which is `resolve`'s answer and `Idle`'s too.
  HostCall nobody(f.world, {obj(kNoObject), Value::integer(2000)});
  CHECK(invoke(registry, CallKind::member, "Taunt", 1, nobody).status == HostStatus::error);

  // A handle that is a number but names no object gets past `resolve` and is
  // caught here instead: this body reads the position and the original
  // dereferences null in the same place, which is a fault rather than a
  // behaviour. It waits its argument, as an idle does, so that no loop spins
  // on it.
  HostCall stale(f.world, {obj(static_cast<ObjectId>(9999)), Value::integer(2000)});
  const HostOutcome gone = invoke(registry, CallKind::member, "Taunt", 1, stale);
  CHECK(gone.status == HostStatus::suspend);
  CHECK(gone.suspend_for == 2000);
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

/// **A `SetCommand` starts its command inside the call.** Ending the head
/// (0x005b07d0) starts the next command there and then (0x005b4f20), its script
/// a thread armed at the clock, so the order is running before the script that
/// gave it next wakes. `OUTPOST_BEHAVIOR.VS` depends on it: it orders a
/// garrisoned defender out, sleeps 200 ms and picks again from whoever is still
/// inside; started a pass later, the same defender was still there, was ordered
/// again -- cancelling the order that had not run -- and never left.
TEST(set_command_starts_the_command_inside_the_call) {
  RunFixture f;
  REQUIRE(f.add_script("// void, Obj This, point pt\nSleep(500);\n", "data/subai/unit_move.vs"));
  const ObjectId u = f.spawn(f.unit_class);
  Command order;
  order.arg_kind = CommandArgKind::point;
  order.point = Point{300, 300};
  f.commands.set_command(f.world, u, "move", order);

  // No turn has run: the command has started and its script is alive.
  const CommandQueue* q = f.commands.find(u);
  REQUIRE(q != nullptr);
  REQUIRE(q->running() != nullptr);
  CHECK(q->running()->verb == "move");
  CHECK(q->running()->script != kNoScript);
  CHECK(f.scheduler.alive(q->running()->script));

  // A verb with no script is left for the pass, which retires it.
  f.commands.set_command(f.world, u, "guard", Command{});
  q = f.commands.find(u);
  REQUIRE(q != nullptr);
  REQUIRE(!q->entries.empty());
  CHECK(q->entries.front().verb == "guard");
  CHECK(!q->entries.front().started);
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

TEST(the_end_of_a_command_clears_the_units_goto_failure_stamp) {
  // `[unit+0x150]`, `Goto`'s failure stamp, is cleared by slot 9 of the unit
  // vtables (0x005d2940) as its base pops the command queue's head, so each
  // command's give-up runs from its own first failure (`GotoOrder::give_up`).
  RunFixture f;
  MovementSystem movement;
  movement.set_grid(ObstructionGrid(64, 64));
  REQUIRE(f.world.add_system(&movement));
  REQUIRE(f.add_script("// void, Obj This, point pt\nSleep(500);\n",
                       "data/subai/unit_move.vs"));
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n",
                       "data/subai/unit_idle.vs"));

  const ObjectId u = f.spawn(f.unit_class);
  Command order;
  order.arg_kind = CommandArgKind::point;
  order.point = Point{300, 300};
  f.commands.set_command(f.world, u, "move", order);
  f.world.advance(100);
  REQUIRE(f.commands.command_name(u) == "move");
  movement.state(u).goto_failed_at = 50;

  // The script returns and the command is over.
  for (int i = 0; i < 10 && f.commands.command_name(u) == "move"; ++i) {
    f.scheduler.advance(200);
    f.world.advance(200);
  }
  REQUIRE(f.commands.command_name(u) == "idle");
  CHECK(movement.find(u)->goto_failed_at == kNoGotoFailure);

  // A command cancelled under it clears the stamp the same way.
  movement.state(u).goto_failed_at = 50;
  f.commands.set_command(f.world, u, "move", order);
  CHECK(movement.find(u)->goto_failed_at == kNoGotoFailure);
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
  // behind a running head and cleared with it before either reached the
  // front. (A `SetCommand` starts its command inside the call, as 0x005b07d0
  // does, so a research *set* and then replaced has started, and its
  // `onfinish` is told it was cancelled.)
  f.world.find(u)->state.user = 0;
  f.commands.set_command(f.world, u, "idle", idle);
  step(200);
  f.world.find(u)->state.user = 0;
  f.commands.add_command(f.world, u, false, "research", order);
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

/// **A row's `onaddremovescript` is asked on every add and told of every unrun
/// removal.** `gbr.exe` runs it from the accept test every insert opens with
/// (0x005b1760, `bAdd` true, a false answer refusing the insert) and from
/// 0x005b07d0 -- the routine a cancel, `KillCommand`, `ClearCommands` and a
/// replacing insert all end in -- through `vtbl+0x90` (0x005b18d0, `bAdd`
/// false), but only for a row with a cost. A command whose script returned is
/// popped by 0x005b5160, which runs nothing: `TRAIN_EX.VS` takes itself off the
/// count it keeps. The hook here is written for the test: it keeps a count in
/// the unit's `user` word, adding and taking the length of the ambient
/// `cmdparam` -- which must be the row's inside the hook -- and refuses an add
/// past 6, the shape of `TRAINEX_ONADDREMOVE.VS`'s cap.
TEST(a_rows_onaddremovescript_is_asked_on_every_add_and_told_of_every_unrun_removal) {
  RunFixture f;
  (void)register_global_hosts(f.registry);
  (void)register_text_host(f.registry);
  REQUIRE(f.add_script("// void, Obj This\nSleep(500);\n", "data/subai/barrack_train.vs"));
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n",
                       "data/subai/unit_idle.vs"));
  REQUIRE(f.add_script("// bool, Obj This, bool bAdd\n"
                       "int n; n = This.user;\n"
                       "if (bAdd) {\n"
                       "  if (n + StrLen(cmdparam) > 6) return false;\n"
                       "  This.SetUser(n + StrLen(cmdparam));\n"
                       "  return true;\n"
                       "}\n"
                       "This.SetUser(n - StrLen(cmdparam));\n"
                       "return true;\n",
                       "data/subai/count_queued.vs"));
  CommandDef hire;
  hire.name = "hire";
  hire.method = "train";
  hire.param = "abc";
  hire.cost_gold = 10;
  hire.train_command = true;
  hire.on_add_remove = "data/subai/count_queued.vs";
  f.commands.mutable_table().set(hire);
  CommandDef free_row = hire;
  free_row.name = "hire free";
  free_row.cost_gold = 0;
  f.commands.mutable_table().set(free_row);

  const auto step = [&f](std::int32_t ms) {
    f.scheduler.advance(ms);
    f.world.advance(ms);
  };
  const auto user = [&f](ObjectId id) { return f.world.find(id)->state.user; };
  Command order;
  order.name = "hire";
  order.param = "abc";
  order.cost_gold = 10;

  // Two adds, each asked first and counted; the third would pass 6 and is
  // refused: nothing queued, and the count as it was.
  const ObjectId u = f.spawn(f.unit_class);
  CHECK(f.commands.append_order(f.world, u, "train", order) != 0);
  CHECK(user(u) == 3);
  const std::uint32_t second = f.commands.append_order(f.world, u, "train", order);
  CHECK(second != 0);
  CHECK(user(u) == 6);
  CHECK(f.commands.append_order(f.world, u, "train", order) == 0);
  CHECK(f.commands.command_count(u) == 2);
  CHECK(user(u) == 6);
  CHECK(f.commands.take_hook_traps().empty());

  // A cancel of the pending one tells the row; the count comes back down.
  CHECK(f.commands.cancel_command(f.world, u, second));
  CHECK(user(u) == 3);
  CHECK(f.commands.command_count(u) == 1);

  // The running one runs to its end: no hook, the count stays.
  step(100);
  REQUIRE(f.commands.command_name(u) == "train");
  for (int i = 0; i < 10 && f.commands.command_name(u) == "train"; ++i) step(200);
  REQUIRE(f.commands.command_name(u) == "idle");
  CHECK(user(u) == 3);

  // `KillCommand` on a running one tells it.
  f.world.find(u)->state.user = 0;
  CHECK(f.commands.append_order(f.world, u, "train", order) != 0);
  step(100);
  REQUIRE(f.commands.command_name(u) == "train");
  CHECK(user(u) == 3);
  CHECK(f.commands.kill_command(f.world, u));
  CHECK(user(u) == 0);

  // `ClearCommands` tells every pending one, and a replacing `SetCommand`
  // every one it clears, the running one included.
  step(100);
  CHECK(f.commands.append_order(f.world, u, "train", order) != 0);
  CHECK(f.commands.append_order(f.world, u, "train", order) != 0);
  CHECK(user(u) == 6);
  step(100);
  REQUIRE(f.commands.command_name(u) == "train");
  CHECK(f.commands.clear_commands(f.world, u) == 1);
  CHECK(user(u) == 3);
  CHECK(f.commands.append_order(f.world, u, "train", order) != 0);
  CHECK(user(u) == 6);
  CHECK(f.commands.set_command(f.world, u, "idle", Command{}) != 0);
  CHECK(user(u) == 0);

  // A row with no cost is asked on the add and never told of a removal:
  // 0x005b07d0 calls `vtbl+0x90` only for a row with a cost.
  Command free_order = order;
  free_order.name = "hire free";
  free_order.cost_gold = 0;
  const std::uint32_t free_id = f.commands.append_order(f.world, u, "train", free_order);
  CHECK(free_id != 0);
  CHECK(user(u) == 3);
  CHECK(f.commands.cancel_command(f.world, u, free_id));
  CHECK(user(u) == 3);
}

/// **A hook that answers nothing, or traps, accepts.** 0x004e7600 starts the
/// answer at true and only a returned value overwrites it. A trap is not lost
/// either: it runs inside the insert, outside every scheduler pass, so the
/// queue keeps it for the session to report.
TEST(a_rows_onaddremovescript_that_answers_nothing_or_traps_accepts) {
  RunFixture f;
  (void)register_global_hosts(f.registry);
  REQUIRE(f.add_script("// void, Obj This\nSleep(500);\n", "data/subai/barrack_train.vs"));
  REQUIRE(f.add_script("// bool, Obj This, bool bAdd\nThis.SetUser(This.user + 1);\n",
                       "data/subai/silent.vs"));
  REQUIRE(f.add_script("// bool, Obj This, bool bAdd\nint n; n = 1 / (This.user - This.user);\nreturn false;\n",
                       "data/subai/trapping.vs"));
  CommandDef silent;
  silent.name = "silent";
  silent.method = "train";
  silent.on_add_remove = "data/subai/silent.vs";
  f.commands.mutable_table().set(silent);
  CommandDef trapping = silent;
  trapping.name = "trapping";
  trapping.on_add_remove = "data/subai/trapping.vs";
  f.commands.mutable_table().set(trapping);

  const ObjectId u = f.spawn(f.unit_class);
  Command order;
  order.name = "silent";
  CHECK(f.commands.append_order(f.world, u, "train", order) != 0);
  CHECK(f.world.find(u)->state.user == 1);
  order.name = "trapping";
  CHECK(f.commands.append_order(f.world, u, "train", order) != 0);
  CHECK(f.commands.command_count(u) == 2);
  const std::vector<FailedScript> traps = f.commands.take_hook_traps();
  REQUIRE(traps.size() == 1);
  CHECK(traps.front().source_name == "data/subai/trapping.vs");
  CHECK(f.commands.take_hook_traps().empty());
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
  CHECK(first.status == HostStatus::retry);
  // The combat target is the argument from the first run (0x005d6b10).
  REQUIRE(combat.find(archer) != nullptr);
  CHECK(combat.find(archer)->target == target);
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

TEST(goto_enter_does_not_call_the_end_of_a_partial_route_arrival) {
  // Playtest #19: builders entered a siege engine from where their walk gave
  // out. `GotoEnter` (0x005d6620) answers `IsArrived` (0x00417c10), which for
  // a point is the band and nothing else, so a unit stopped by a wall has not
  // arrived; and once no route is left it ends the script after `give_up`.
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

  // `UNIT_ENTER.VS`'s give-up, 5,000 ms. The unit walks to the wall, which
  // is not arrival; then it searches from there and finds nothing, and five
  // seconds of that ends the script. It never says it entered.
  bool entered = false;
  bool ended = false;
  GameTime last_route = -1;
  GameTime ended_at = -1;
  for (int i = 0; i < 100 && !entered && !ended; ++i) {
    HostCall call(f.world, {obj(u), door, Value::integer(0), Value::integer(1000),
                            Value::boolean(true), Value::integer(5000)});
    const HostOutcome out = invoke(registry, CallKind::member, "GotoEnter", 5, call);
    if (movement.find(u)->has_path) last_route = f.world.time();
    ended = out.status == HostStatus::finish;
    if (ended) ended_at = f.world.time();
    entered = out.status != HostStatus::finish && out.value.as_integer() != 0;
    f.world.advance(500);
  }
  CHECK(!entered);
  CHECK(ended);
  // Timed from the failure, not from the order: the walk to the wall came
  // first, and the five seconds after the last route was laid.
  CHECK(last_route > 0);
  CHECK(ended_at - last_route >= 5000);
  CHECK(ended_at - last_route <= 5500);
  CHECK(!movement.find(u)->goto_active);
  CHECK(distance(f.world.resolve_position(u), Point{30 * 16 + 8, 30 * 16 + 8}) > 32);

  // Plain `Goto` to the same sealed point never reports arrival either.
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

  // `UNIT_BUILD_CATAPULT.VS` passes 0: the first search that finds nothing
  // ends it. A unit walled into a single cell has no route anywhere.
  ObstructionGrid pocket(64, 64);
  for (std::int32_t dx = -1; dx <= 1; ++dx) {
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
      if (dx != 0 || dy != 0) pocket.set_cell(10 + dx, 10 + dy, true);
    }
  }
  movement.set_grid(std::move(pocket));
  const ObjectId walled = f.spawn(f.unit_class, Point{10 * 16 + 8, 10 * 16 + 8});
  movement.state(walled).speed = 400;
  const Value away = pack_point(Point{40 * 16 + 8, 40 * 16 + 8});
  HostCall at_once(f.world, {obj(walled), away, Value::integer(0), Value::integer(1000),
                             Value::boolean(false), Value::integer(0)});
  CHECK(invoke(registry, CallKind::member, "GotoEnter", 5, at_once).status ==
        HostStatus::finish);
  // A negative give-up never ends it: the call waits its slice and says no.
  for (int i = 0; i < 20; ++i) {
    HostCall never(f.world, {obj(walled), away, Value::integer(0), Value::integer(1000),
                             Value::boolean(false), Value::integer(-1)});
    const HostOutcome out = invoke(registry, CallKind::member, "GotoEnter", 5, never);
    CHECK(out.status == HostStatus::retry);
    CHECK(out.suspend_for == 1000);
    CHECK(out.value.as_integer() == 0);
    f.world.advance(1000);
  }
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
    const std::int64_t wait = outcome.status == HostStatus::retry ? outcome.suspend_for : 400;
    f.world.advance(static_cast<std::int32_t>(wait > 0 ? wait : 400));
    (void)invoke(registry, CallKind::member, "FormKeepMoving", 1, keep.reenter());
    ++calls;
  }
  CHECK(calls < 200);
  CHECK(f.world.resolve_position(hero) == (Point{2000, 200}));
  // The army followed rather than walking to the hero's original station.
  for (const ObjectId w : army) {
    CHECK(distance(f.world.resolve_position(w), Point{2000, 200}) < 600);
  }
}

TEST(a_fast_hero_marches_at_his_armys_pace_and_it_stands_formed_on_him_when_he_stops) {
  // Playtest #22: a mounted hero stood alone on open grass with his army
  // clumped some way off. The hero walked his own route at his own speed and
  // the army chased stations laid round where he had been, so when he stopped
  // `HERO_MOVE.VS` stood the army (`stand_position`, `Stop`) wherever it had
  // got to. In `gbr.exe` the hero's route is aimed at the formation like the
  // members' (0x0052e830): every unit walks at the formation's pace, timed to
  // the sample's deadline (0x00419e52), and the last sample is laid round the
  // destination.
  Fixture f;
  MovementSystem movement;
  HeroSystem heroes;
  movement.set_grid(ObstructionGrid(192, 64));
  Result<FormationTable> formations = FormationTable::parse(bytes(R"(<Formations>
      <Default Name="Block"/>
      <FormationClass Name="Block" OffsetFrontLineByY="0" OffsetWingsByX="0"
        OffsetWingsByY="0">
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

  const Point start{300, 500};
  const Point dest{2700, 500};
  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr, f.hero_class);
  f.world.set_position(hero, start);
  f.world.set_owner(hero, 1);
  f.world.set_health(hero, 1000);
  heroes.register_hero(f.world, hero);
  movement.state(hero).speed = 480;  // mounted, eight times his footmen

  std::vector<ObjectId> army;
  for (int i = 0; i < 6; ++i) {
    const ObjectId w = f.spawn(f.warrior_class, Point{start.x - 40 * (i % 3), start.y + 40 * (i / 3)});
    heroes.register_unit(f.world, w);
    CHECK(heroes.attach(f.world, w, hero));
    movement.state(w).speed = 60;
    (void)f.order(w, "idle");
    army.push_back(w);
  }

  HostCall setup(f.world, {obj(hero), pack_point(dest), Value::integer(0), Value::integer(0),
                           Value::boolean(true)});
  REQUIRE(invoke(registry, CallKind::member, "FormSetupAndMoveTo", 4, setup).status ==
          HostStatus::ok);
  // The formation's speed is the mean of all seven, (480 + 6 * 60) / 7 =
  // 120, held to one and a half times the slowest's (0x005f5260): 90. With
  // the army about as far from its stations as he is from his, the hero
  // walks about that rather than his own 480; every pace lies between half
  // and two and a half times it (0x005f2130).
  const std::int64_t formation = 90 * kDefaultSpeedFactor;
  CHECK(movement.find(hero)->march_rate >= formation * 4 / 5);
  CHECK(movement.find(hero)->march_rate <= formation * 6 / 5);
  for (const ObjectId w : army) {
    CHECK(movement.find(w)->march_rate >= formation / 2);
    CHECK(movement.find(w)->march_rate <= formation * 5 / 2);
  }

  // `HERO_MOVE.VS`: `while (.HasPath()) .FormKeepMoving(1500);`, the world
  // run in 100 ms turns in between.
  std::int32_t elapsed = 0;
  std::int64_t widest = 0;
  while (elapsed < 120000) {
    HostCall has_path(f.world, {obj(hero)});
    if (invoke(registry, CallKind::member, "HasPath", 0, has_path).value.as_integer() == 0) break;
    HostCall keep(f.world, {obj(hero), Value::integer(1500)});
    (void)invoke(registry, CallKind::member, "FormKeepMoving", 1, keep);
    for (int t = 0; t < 15; ++t) f.world.advance(100);
    elapsed += 1500;
    // The hero never leaves his army behind on the way.
    for (const ObjectId w : army) {
      widest = std::max(widest, isqrt(dist_sq(f.world.resolve_position(w),
                                              f.world.resolve_position(hero))));
    }
  }
  REQUIRE(elapsed < 120000);
  CHECK(f.world.resolve_position(hero) == dest);
  CHECK(widest < 400);
  // At his own 480 the 2,400 units take five seconds; at the formation's
  // pace they take more than twenty.
  CHECK(elapsed > 20000);

  // And when he stops, the army stands formed on him: the end of
  // `HERO_MOVE.VS` stops every member where it is (`stand_position`'s
  // `Stop`), and every one of them is already on its station round the
  // destination -- a block of three ranks of two, laid back from him.
  for (const ObjectId w : army) {
    CHECK(f.world.resolve_position(w) == movement.find(w)->target);
    CHECK(!movement.find(w)->has_path);
  }
  for (const ObjectId w : army) movement.stop(f.world, w);
  for (const ObjectId w : army) {
    CHECK(isqrt(dist_sq(f.world.resolve_position(w), dest)) <= 3 * 52);
    CHECK(movement.find(w)->march_rate == 0);
  }
  // An order that is not a march walks at the unit's own pace again.
  (void)movement.order_goto(f.world, hero, start, 0);
  CHECK(movement.find(hero)->march_rate == 0);
  CHECK(movement.find(hero)->rate() == movement.find(hero)->own_rate());
}

namespace {

/// A hero on open ground, a `Block` formation, and his army stood where the
/// case puts it: the bench the march's pace is read on.
struct MarchBench {
  Fixture f;
  MovementSystem movement;
  HeroSystem heroes;
  HostRegistry registry;
  ObjectId hero = kNoObject;
  std::vector<ObjectId> army;

  MarchBench(Point from, std::int32_t hero_speed, std::span<const Point> at, std::int32_t speed) {
    movement.set_grid(ObstructionGrid(448, 64));
    Result<FormationTable> formations = FormationTable::parse(bytes(R"(<Formations>
        <Default Name="Block"/>
        <FormationClass Name="Block" OffsetFrontLineByY="0" OffsetWingsByX="0"
          OffsetWingsByY="0">
          <Class Name="Unit" CentralBlock="1"/>
        </FormationClass>
      </Formations>)"));
    movement.set_formations(std::move(formations.value()));
    f.world.add_system(&movement);
    f.world.add_system(&heroes);
    register_movement_host(registry);
    register_command_host(registry);
    hero = f.world.spawn(NativeClass::hero, nullptr, f.hero_class);
    f.world.set_position(hero, from);
    f.world.set_owner(hero, 1);
    f.world.set_health(hero, 1000);
    heroes.register_hero(f.world, hero);
    movement.state(hero).speed = hero_speed;
    for (const Point p : at) {
      const ObjectId w = f.spawn(f.warrior_class, p);
      heroes.register_unit(f.world, w);
      (void)heroes.attach(f.world, w, hero);
      movement.state(w).speed = speed;
      army.push_back(w);
    }
  }

  /// `FormSetupAndMoveTo(dest, 0, 0, true)`, east along the row.
  void march(Point dest) {
    HostCall setup(f.world, {obj(hero), pack_point(dest), Value::integer(0), Value::integer(0),
                             Value::boolean(true)});
    REQUIRE(invoke(registry, CallKind::member, "FormSetupAndMoveTo", 4, setup).status ==
            HostStatus::ok);
  }
  [[nodiscard]] std::int64_t pace(ObjectId id) const { return movement.find(id)->march_rate; }
};

}  // namespace

TEST(a_marchs_pace_is_its_share_of_the_mean_deadline_short_of_133) {
  // One member, 200 east of the hero on his route. The centre stands 333
  // ahead of him (one 200-unit sample beyond the 133 a unit is timed short
  // of, 0x00419c78), facing the way it stepped, and a lone member of a block
  // stands at the front of its first ring, `15 + 2 * 26` = 67 further on
  // (0x005f04b0: the hero's `radius`, twice the member's `formation_radius`).
  // The deadline is the mean over both of the time to cover the straight
  // distance less 133 (0x005f5850): (200 + 67) / 2 at the formation's 100,
  // 1,335 ms. The hero's route to the centre less 133 is 200, the member's
  // to its station 67; each is paced at that over the deadline.
  const Point at[] = {Point{500, 500}};
  MarchBench b(Point{300, 500}, 100, at, 100);
  b.march(Point{4000, 500});
  CHECK(b.movement.find(b.army[0])->target == (Point{700, 500}));
  CHECK(b.pace(b.hero) == 200 * kSpeedScale / 1335);
  CHECK(b.pace(b.army[0]) == 67 * kSpeedScale / 1335);
}

TEST(the_last_sample_is_the_destination_and_nothing_is_taken_short_of_it) {
  // The destination 200 east of the hero, nearer than a sample: the centre is
  // the route's end, the formation's last sample (`[form+0x88]` cleared,
  // 0x005f5cff), and no 133 comes off either distance. The member's station
  // is 67 ahead of the destination, 285 from it; the deadline is
  // (2,000 + 2,850 + 1) / 2 ms at the formation's 100, and the hero's pace
  // his 200 over it.
  const Point at[] = {Point{300, 600}};
  MarchBench b(Point{300, 500}, 100, at, 100);
  b.march(Point{500, 500});
  CHECK(b.movement.find(b.army[0])->target == (Point{567, 500}));
  CHECK(b.pace(b.hero) == 200 * kSpeedScale / 2425);
}

TEST(a_straggler_is_hurried_and_the_hero_held_back_within_the_clamp) {
  // Two members on the hero and one 3,000 behind him. The formation's speed
  // is the mean of 400, 60, 60, 60 -- 145 -- held to 90, one and a half times
  // the slowest. The straggler's share of the deadline would have it run at
  // over five times that and the hero walk at a third of it; both are held
  // to the clamp (0x005f2130): the straggler at two and a half times the
  // formation's speed, the hero at half.
  const Point at[] = {Point{3400, 540}, Point{3400, 460}, Point{400, 500}};
  MarchBench b(Point{3400, 500}, 400, at, 60);
  b.march(Point{6400, 500});
  const std::int64_t formation = 90 * kDefaultSpeedFactor;
  CHECK(b.pace(b.hero) == formation / 2);
  CHECK(b.pace(b.army[2]) == formation * 5 / 2);
  CHECK(b.pace(b.army[0]) >= formation / 2);
  CHECK(b.pace(b.army[0]) <= formation * 5 / 2);
}

namespace {

[[nodiscard]] std::int64_t form_radius_of(MarchBench& b) {
  HostCall call(b.f.world, {obj(b.hero)});
  return invoke(b.registry, CallKind::member, "FormRadius", 0, call).value.as_integer();
}

}  // namespace

TEST(a_unit_that_joins_a_march_takes_its_place_at_the_next_layout) {
  // Joining only puts the unit on the formation's pending list and marks the
  // layout (0x005f47e0); `FormRadius` answers the last layout's radius until
  // the layout runs (0x005f2350), and that is in `FormKeepMoving`'s update
  // (0x005f5e30 -> 0x005f4390), which hands every unit it lays `form_move`.
  // The hero's class radius is 15 and the warriors' 26: two in a block stand
  // at the front and back of the first ring, 67 out; three are its short
  // ring, the front corners 94 out.
  const Point at[] = {Point{300, 540}, Point{300, 460}};
  MarchBench b(Point{300, 500}, 100, at, 100);
  CHECK(form_radius_of(b) == 15);
  b.march(Point{3000, 500});
  CHECK(form_radius_of(b) == 67);
  for (const ObjectId w : b.army) CHECK(b.f.verbs(w) == std::vector<std::string>{"form_move"});

  const ObjectId late = b.f.spawn(b.f.warrior_class, Point{200, 500});
  b.heroes.register_unit(b.f.world, late);
  REQUIRE(b.heroes.attach(b.f.world, late, b.hero));
  b.movement.state(late).speed = 100;
  (void)b.f.order(late, "idle");
  CHECK(form_radius_of(b) == 67);
  CHECK(b.f.verbs(late) == std::vector<std::string>{"idle"});

  b.f.world.advance(500);
  HostCall keep(b.f.world, {obj(b.hero), Value::integer(1500)});
  (void)invoke(b.registry, CallKind::member, "FormKeepMoving", 1, keep);
  CHECK(b.f.verbs(late) == std::vector<std::string>{"form_move"});
  CHECK(b.movement.find(late)->goto_active);
  CHECK(b.movement.find(late)->party == b.hero);
  CHECK(form_radius_of(b) == 94);
}

TEST(a_member_that_leaves_the_army_loses_its_route) {
  // The hero's `RemoveUnit` (0x0052db70 -> 0x005f3290) deletes the leaving
  // member's route (0x005d3830 with 0), and its slot goes with it.
  const Point at[] = {Point{300, 540}, Point{300, 460}};
  MarchBench b(Point{300, 500}, 100, at, 100);
  b.march(Point{3000, 500});
  REQUIRE(b.movement.find(b.army[0])->goto_active);
  REQUIRE(b.heroes.detach(b.f.world, b.army[0]));
  CHECK(!b.movement.find(b.army[0])->goto_active);
  CHECK(!b.movement.find(b.army[0])->has_path);
  CHECK(b.movement.formation(b.hero)->slots.size() == 1);
  CHECK(b.movement.formation(b.hero)->rebuild);
  CHECK(b.movement.find(b.army[1])->goto_active);
}

TEST(a_requested_orientation_is_settled_by_the_setup_and_faces_the_last_sample) {
  // `SetDest` (0x005f2cc0) moves a requested orientation into the settled
  // pair and clears the request; within 200 of the path's end the sample
  // faces it (0x005f5c7d). Two hundred east, the first sample is the last:
  // the lone member stands at the front of the block, 67 along the settled
  // orientation -- south -- and not along the march -- east.
  const Point at[] = {Point{300, 540}};
  MarchBench b(Point{300, 500}, 100, at, 100);
  HeroRecord* record = b.heroes.hero(b.hero);
  REQUIRE(record != nullptr);
  record->party_orientation_request = Point{0, 50};
  b.march(Point{500, 500});
  CHECK(record->party_orientation == (Point{0, 50}));
  CHECK(record->party_orientation_request == (Point{}));
  CHECK(b.movement.find(b.army[0])->target == (Point{500, 567}));
}

TEST(a_setup_to_where_the_hero_stands_ends_the_march) {
  // `HERO_IDLE.VS` after a fight: `.FormSetupAndMoveTo(.pos, 0, 0, true)`.
  // `SetDest` hands every member with a slot `form_move` (0x005f2b50), and
  // the first sample cannot step a centre that has nowhere to go: with no
  // sample laid, `CreateNextSample` ends the march (0x005f5ce5 -> 0x005f2c10)
  // -- each member loses its route and its `form_move`.
  const Point at[] = {Point{300, 540}, Point{300, 460}};
  MarchBench b(Point{300, 500}, 100, at, 100);
  b.march(Point{1000, 500});
  for (const ObjectId w : b.army) CHECK(b.f.verbs(w) == std::vector<std::string>{"form_move"});
  b.f.world.advance(500);
  b.march(b.f.world.resolve_position(b.hero));
  for (const ObjectId w : b.army) {
    CHECK(b.f.verbs(w).empty());
    CHECK(!b.movement.find(w)->goto_active);
  }
  CHECK(b.movement.formation(b.hero)->samples == 0);
}

TEST(form_setup_hands_the_army_form_move_and_the_march_moves_only_its_marchers) {
  // `CVXFormObj::SetDest` (0x005f2cc0) that lays a route gives every member
  // `form_move` with the replace flag (0x005f2b50 -> 0x005b4e90 with 1), so
  // whatever a member was running -- `UNIT_IDLE.VS`, whose `Stop(2000)` drops
  // a route every few seconds -- ends. The march then moves only the members
  // still running it: one given another command since keeps its own.
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
  for (int i = 0; i < 3; ++i) {
    const ObjectId w = f.spawn(f.warrior_class, Point{200 + 10 * i, 200});
    heroes.register_unit(f.world, w);
    CHECK(heroes.attach(f.world, w, hero));
    movement.state(w).speed = 140;
    (void)f.order(w, "idle");
    army.push_back(w);
  }

  HostCall setup(f.world, {obj(hero), pack_point(Point{2000, 200}), Value::integer(0),
                           Value::integer(0), Value::boolean(true)});
  REQUIRE(invoke(registry, CallKind::member, "FormSetupAndMoveTo", 4, setup).status ==
          HostStatus::ok);
  CHECK(movement.formation(hero)->should_lock);
  for (const ObjectId w : army) {
    CHECK(f.verbs(w) == std::vector<std::string>{"form_move"});
    CHECK(movement.find(w)->goto_active);
    CHECK(movement.find(w)->party == hero);
    // The formation's flag rides on the order; while the hero walks it is
    // not yet a lock (`0x004178d0`).
    CHECK(movement.find(w)->dest_lock);
    CHECK(!movement.lock_owner(w, *movement.find(w)));
  }

  // One member is given something else and stops; the march goes on.
  (void)f.order(army[0], "idle");
  movement.stop(f.world, army[0]);
  for (int i = 0; i < 4; ++i) f.world.advance(800);
  HostCall keep(f.world, {obj(hero), Value::integer(1500)});
  (void)invoke(registry, CallKind::member, "FormKeepMoving", 1, keep);
  CHECK(!movement.find(army[0])->goto_active);
  CHECK(movement.find(army[1])->goto_active);
  CHECK(movement.find(army[2])->goto_active);

  // A target that names nothing changes nothing (0x0052e890's parameter
  // check comes first): no route, no `form_move` handed out again.
  HostCall nothing(f.world, {obj(hero), obj(999999), Value::integer(0), Value::integer(0),
                             Value::boolean(true)});
  CHECK(invoke(registry, CallKind::member, "FormSetupAndMoveTo", 4, nothing).status ==
        HostStatus::ok);
  CHECK(f.verbs(army[0]) == std::vector<std::string>{"idle"});
  CHECK(movement.find(army[1])->goto_active);

  // A setup the hero cannot walk ends the march (0x005f2c10) and hands out
  // nothing: the marchers lose their routes and their `form_move` (its script
  // is stopped, 0x0069fee0), and the idle one keeps its idle.
  f.world.set_position(hero, Point{100 * 16 + 8, 100 * 16 + 8});
  for (std::int32_t y = 99; y <= 101; ++y) {
    for (std::int32_t x = 99; x <= 101; ++x) {
      if (x != 100 || y != 100) movement.mutable_grid().set_cell(x, y, true);
    }
  }
  HostCall walled(f.world, {obj(hero), pack_point(Point{20 * 16 + 8, 20 * 16 + 8}),
                            Value::integer(0), Value::integer(0), Value::boolean(true)});
  (void)invoke(registry, CallKind::member, "FormSetupAndMoveTo", 4, walled);
  REQUIRE(movement.find(hero)->last_outcome == MoveOutcome::blocked);
  CHECK(!movement.find(army[1])->goto_active);
  CHECK(f.verbs(army[1]).empty());
  CHECK(f.verbs(army[2]).empty());
  CHECK(f.verbs(army[0]) == std::vector<std::string>{"idle"});
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
  CHECK(outcome.status == HostStatus::retry);
  CHECK(outcome.suspend_for == 5000);  // not `eta`

  // A zero duration suspends too: the first run writes its argument to the
  // wait cell and returns 1 whatever it is, and the re-entry is what returns.
  HostCall zero(f.world, {obj(hero), Value::integer(0)});
  const HostOutcome none = invoke(registry, CallKind::member, "FormKeepMoving", 1, zero);
  CHECK(none.status == HostStatus::retry);
  CHECK(none.suspend_for == 0);
  CHECK(invoke(registry, CallKind::member, "FormKeepMoving", 1, zero.reenter()).status ==
        HostStatus::ok);
}

/// **The re-entry of `FormKeepMoving` drops an arrived route** (0x0052eb5b,
/// 0x005d7ab5: the route record asked `IsArrived`, and deleted when it is),
/// and steps nothing: `while (.HasPath()) .FormKeepMoving(1500);` ends on the
/// call that finds the hero there, not a slice later. A route not arrived is
/// kept.
TEST(form_keep_moving_drops_an_arrived_route_at_its_reentry) {
  Fixture f;
  MovementSystem movement;
  movement.set_grid(ObstructionGrid(64, 64));
  f.world.add_system(&movement);
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
  HostCall setup(f.world, {obj(hero), pack_point(Point{500, 100}), Value::integer(0),
                           Value::integer(0), Value::boolean(true)});
  REQUIRE(invoke(registry, CallKind::member, "FormSetupAndMoveTo", 4, setup).status ==
          HostStatus::ok);
  REQUIRE(movement.find(hero)->goto_active);

  HostCall keep(f.world, {obj(hero), Value::integer(1500)});
  REQUIRE(invoke(registry, CallKind::member, "FormKeepMoving", 1, keep).status ==
          HostStatus::retry);
  CHECK(invoke(registry, CallKind::member, "FormKeepMoving", 1, keep.reenter()).status ==
        HostStatus::ok);
  CHECK(movement.find(hero)->goto_active);  // not there: kept

  // Put him in the band by hand: the re-entry deletes the route.
  REQUIRE(f.world.set_position(hero, Point{500, 100}));
  HostCall last(f.world, {obj(hero), Value::integer(1500)});
  REQUIRE(invoke(registry, CallKind::member, "FormKeepMoving", 1, last).status ==
          HostStatus::retry);
  REQUIRE(f.world.set_position(hero, Point{500, 100}));
  CHECK(invoke(registry, CallKind::member, "FormKeepMoving", 1, last.reenter()).status ==
        HostStatus::ok);
  CHECK(!movement.find(hero)->goto_active);
}

/// `FormSetupAndMoveTo`'s route is laid by `SetDest` (0x005f2cc0 ->
/// 0x0041a4c0), which keeps at most 999 of the range.
TEST(form_setup_keeps_setdests_range_cap) {
  Fixture f;
  MovementSystem movement;
  movement.set_grid(ObstructionGrid(128, 128));
  f.world.add_system(&movement);
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
  HostCall setup(f.world, {obj(hero), pack_point(Point{1900, 100}), Value::integer(1500),
                           Value::integer(0), Value::boolean(true)});
  REQUIRE(invoke(registry, CallKind::member, "FormSetupAndMoveTo", 4, setup).status ==
          HostStatus::ok);
  CHECK(movement.find(hero)->range == 999);
}

TEST(a_members_form_keep_moving_drops_its_target_and_a_heros_does_not) {
  // `Unit::FormKeepMoving` (0x005d7a40) empties `[unit+0x1a8]` on its first
  // entry (0x005d7a7b); `Hero::FormKeepMoving` (0x0052eaf0) has no such write.
  Fixture f;
  MovementSystem movement;
  movement.set_grid(ObstructionGrid(64, 64));
  f.world.add_system(&movement);
  HeroSystem heroes;
  f.world.add_system(&heroes);
  CombatSystem combat;
  REQUIRE(f.world.add_system(&combat));
  HostRegistry registry;
  register_movement_host(registry);
  register_command_host(registry);

  const ObjectId hero = f.world.spawn(NativeClass::hero, nullptr, f.hero_class);
  f.world.set_position(hero, Point{100, 100});
  f.world.set_owner(hero, 1);
  f.world.set_health(hero, 1000);
  heroes.register_hero(f.world, hero);
  const ObjectId member = f.spawn(f.unit_class, Point{120, 100});
  const ObjectId foe = f.spawn(f.unit_class, Point{300, 100}, /*owner=*/2);
  for (const ObjectId id : {hero, member}) {
    Combatant c;
    c.id = id;
    c.owner = 1;
    c.health = 100;
    c.target = foe;
    c.attacks = 2;
    combat.add(c);
  }

  HostCall member_keep(f.world, {obj(member), Value::integer(1000)});
  const HostOutcome kept = invoke(registry, CallKind::member, "FormKeepMoving", 1, member_keep);
  CHECK(kept.status == HostStatus::retry);
  CHECK(kept.suspend_for == 1000);
  CHECK(combat.find(member)->target == kNoObject);
  CHECK(combat.find(member)->attacks == 0);

  HostCall hero_keep(f.world, {obj(hero), Value::integer(1500)});
  (void)invoke(registry, CallKind::member, "FormKeepMoving", 1, hero_keep);
  CHECK(combat.find(hero)->target == foe);
  CHECK(combat.find(hero)->attacks == 2);
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

  // A route that is not a march's is not the formation's: 0x0052ebc0
  // answers 0 while the formation has no route record (`[form+0x84]`).
  movement.order_goto(f.world, hero, Point{900, 100}, 0);
  HostCall alone(f.world, {obj(hero)});
  CHECK(invoke(registry, CallKind::member, "FormPathLeft", 0, alone).value.as_integer() == 0);

  // Leading a march, his route stands for the formation's.
  movement.formation_of(hero).path = true;
  movement.state(hero).party = hero;
  HostCall walking(f.world, {obj(hero)});
  const std::int32_t left =
      invoke(registry, CallKind::member, "FormPathLeft", 0, walking).value.as_integer();
  CHECK(left > 700);
  CHECK(left < 900);

  // **An incomplete path reads 32,000 longer** (0x004185a0): a route cut short
  // of its goal is never nearly there.
  MoveState& move = movement.state(hero);
  REQUIRE(move.has_path);
  move.path_complete = false;
  HostCall partial(f.world, {obj(hero)});
  CHECK(invoke(registry, CallKind::member, "FormPathLeft", 0, partial).value.as_integer() ==
        left + 32000);
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

// --------------------------------------------------------------------------
// the host audit: the queue members against 0x005aef20 and their three forms
// --------------------------------------------------------------------------

namespace {

/// Compiles method scripts from a fixed table, as the session compiles from
/// the installation, so that `CommandSystem::binds` can read their shapes.
struct ShapeLibrary final : imperivm::core::sim::ScriptLibrary {
  Scheduler* scheduler = nullptr;
  const HostRegistry* registry = nullptr;
  std::vector<std::pair<std::string, std::string>> sources;

  std::uint32_t chunk_for(std::string_view path) override {
    if (const std::uint32_t found = scheduler->find_chunk(path); found != kNoChunk) return found;
    for (const auto& [name, source] : sources) {
      if (name == path) return build(*scheduler, *registry, source, name.c_str());
    }
    return kNoChunk;
  }
};

struct ShapeFixture : RunFixture {
  ShapeLibrary library;
  ShapeFixture() {
    library.scheduler = &scheduler;
    library.registry = &registry;
    // `move` takes a point, `engage` an object, `guard` nothing -- the three
    // shapes 0x005aef20 asks for by kind 1, 2 and 0.
    library.sources.emplace_back("data/subai/unit_move.vs",
                                 "// void, Obj This, point pt\nSleep(500);\n");
    library.sources.emplace_back("data/subai/unit_engage.vs",
                                 "// void, Obj This, Obj target\nSleep(500);\n");
    library.sources.emplace_back("data/subai/unit_guard.vs", "// void, Obj This\nSleep(500);\n");
    commands.set_library(&library);
  }
};

}  // namespace

/// 0x005aef20 finds a method by name **and** by its script's signature, so a
/// verb given the wrong argument is not found: `AddCommand` queues nothing and,
/// building no command, draws no id. `GDRUID_IDLE.VS`'s
/// `AddCommand(true, "ghost", .pos)` against a `ghost` that takes no argument
/// is the shipped case.
TEST(add_command_queues_only_a_verb_bound_in_the_shape_it_is_given) {
  ShapeFixture f;
  const ObjectId u = f.spawn(f.unit_class);
  f.world.set_command_id_seed(500);

  HostCall bare(f.world, {obj(u), Value::boolean(true), Value::string("move")});
  REQUIRE(invoke(f.registry, CallKind::member, "AddCommand", 2, bare).status == HostStatus::ok);
  HostCall pointed(f.world, {obj(u), Value::boolean(true), Value::string("guard"),
                             pack_point(Point{300, 300})});
  REQUIRE(invoke(f.registry, CallKind::member, "AddCommand", 3, pointed).status ==
          HostStatus::ok);
  CHECK(f.commands.command_count(u) == 0);
  CHECK(f.world.command_id_seed() == 500);

  HostCall right(f.world, {obj(u), Value::boolean(true), Value::string("move"),
                           pack_point(Point{300, 300})});
  REQUIRE(invoke(f.registry, CallKind::member, "AddCommand", 3, right).status == HostStatus::ok);
  HostCall at(f.world, {obj(u), Value::boolean(false), Value::string("engage"), obj(u)});
  REQUIRE(invoke(f.registry, CallKind::member, "AddCommand", 3, at).status == HostStatus::ok);
  CHECK(f.verbs(u) == (std::vector<std::string>{"move", "engage"}));
  CHECK(f.world.command_id_seed() == 502);
}

/// A script's `SetCommand` clears the tail **before** it looks the verb up, and
/// on a miss the `Obj` form leaves the head running (0x005b0aec returns) while
/// the `ObjList` and `Query` forms end it anyway (0x00561d6a, 0x00577ef8).
TEST(a_set_command_that_finds_no_method_clears_the_tail_and_only_a_list_ends_the_head) {
  ShapeFixture f;
  const ObjectId a = f.spawn(f.unit_class, Point{100, 100});
  const ObjectId b = f.spawn(f.unit_class, Point{300, 100});
  for (const ObjectId id : {a, b}) {
    f.commands.set_command(f.world, id, "guard", Command{});
    f.commands.add_command(f.world, id, false, "goto", Command{});
  }

  HostCall single(f.world, {obj(a), Value::string("move")});
  REQUIRE(invoke(f.registry, CallKind::member, "SetCommand", 1, single).status == HostStatus::ok);
  CHECK(f.verbs(a) == (std::vector<std::string>{"guard"}));

  ObjListPool& pool = objlist_pool_of(f.world);
  const ObjListId list = pool.acquire(1, 0);
  *pool.mutable_items(list) = {b};
  HostCall group(f.world, {make_objlist_value(list), Value::string("move")});
  REQUIRE(invoke(f.registry, CallKind::member, "SetCommand", 1, group).status == HostStatus::ok);
  // The head ended, and the end refilled the queue with the class's `idle`
  // there and then (0x005b5160 -> 0x005b4f20).
  CHECK(f.verbs(b) == (std::vector<std::string>{"idle"}));
}

/// `Obj::KillCommand` and `Obj::ClearCommands` refuse a dead object
/// (0x005b0d30, 0x005aa250 ask `vtbl+0x50` first); the `ObjList` forms skip a
/// dead member of every queue call, `AddCommand` and `SetCommand` included,
/// while `Obj::AddCommand` has no such test.
TEST(the_queue_members_leave_a_dead_object_alone_as_each_form_does) {
  ShapeFixture f;
  const ObjectId corpse = f.spawn(f.unit_class);
  const ObjectId live = f.spawn(f.unit_class, Point{300, 100});
  for (const ObjectId id : {corpse, live}) {
    f.commands.set_command(f.world, id, "guard", Command{});
    f.commands.add_command(f.world, id, false, "goto", Command{});
  }
  f.world.set_health(corpse, 0);

  HostCall kill(f.world, {obj(corpse)});
  REQUIRE(invoke(f.registry, CallKind::member, "KillCommand", 0, kill).status == HostStatus::ok);
  HostCall clear(f.world, {obj(corpse)});
  REQUIRE(invoke(f.registry, CallKind::member, "ClearCommands", 0, clear).status ==
          HostStatus::ok);
  CHECK(f.verbs(corpse) == (std::vector<std::string>{"guard", "goto"}));

  ObjListPool& pool = objlist_pool_of(f.world);
  const ObjListId list = pool.acquire(1, 0);
  *pool.mutable_items(list) = {corpse, live};
  HostCall add(f.world, {make_objlist_value(list), Value::boolean(false), Value::string("guard")});
  REQUIRE(invoke(f.registry, CallKind::member, "AddCommand", 2, add).status == HostStatus::ok);
  CHECK(f.verbs(corpse) == (std::vector<std::string>{"guard", "goto"}));
  CHECK(f.verbs(live) == (std::vector<std::string>{"guard", "goto", "guard"}));

  HostCall listed_kill(f.world, {make_objlist_value(list)});
  REQUIRE(invoke(f.registry, CallKind::member, "KillCommand", 0, listed_kill).status ==
          HostStatus::ok);
  CHECK(f.verbs(corpse) == (std::vector<std::string>{"guard", "goto"}));
  CHECK(f.verbs(live) == (std::vector<std::string>{"goto", "guard"}));

  // The `Obj` form of `AddCommand` asks nothing about health.
  HostCall single(f.world, {obj(corpse), Value::boolean(false), Value::string("guard")});
  REQUIRE(invoke(f.registry, CallKind::member, "AddCommand", 2, single).status == HostStatus::ok);
  CHECK(f.verbs(corpse).size() == 3);
}

/// The offset forms hold each member's destination inside the map
/// (0x00562667..0x005626a7, the rectangle `ClipDestToMap` clamps to).
TEST(set_command_offset_holds_a_member_spread_off_the_map_inside_it) {
  ShapeFixture f;
  MovementSystem movement;
  movement.mutable_grid() = ObstructionGrid(64, 64);
  f.world.add_system(&movement);
  const ObjectId a = f.spawn(f.unit_class, Point{100, 100});
  const ObjectId b = f.spawn(f.unit_class, Point{500, 100});

  ObjListPool& pool = objlist_pool_of(f.world);
  const ObjListId list = pool.acquire(1, 0);
  *pool.mutable_items(list) = {a, b};
  HostCall call(f.world, {make_objlist_value(list), Value::string("move"), pack_point(Point{50, 50})});
  REQUIRE(invoke(f.registry, CallKind::member, "SetCommandOffset", 2, call).status ==
          HostStatus::ok);
  REQUIRE(f.commands.find(a) != nullptr);
  REQUIRE(f.commands.find(a)->entries.size() == 1);
  // The centre is (300, 100), so `a` is sent to (-150, 50): held at x = 0.
  CHECK(f.commands.find(a)->entries[0].point == (Point{0, 50}));
  CHECK(f.commands.find(b)->entries[0].point == (Point{250, 50}));
}

/// A command that ends **itself** is not a cancelled one. 0x005b07d0 ends
/// index 0 through 0x0069fee0, and when the script holding the command slot is
/// the one making the call it only raises that script's stop flag
/// (`[ctx+0x40]`); the script then ends the ordinary way and the head is popped
/// with the cancel flag (0x009bdb18) down, so `onfinish` is told `false`. A
/// `KillCommand` from any other script is a cancel, as before.
TEST(a_command_that_kills_itself_is_told_it_finished_and_not_that_it_was_cancelled) {
  RunFixture f;
  (void)register_global_hosts(f.registry);
  REQUIRE(f.add_script("// void, Obj This\nThis.KillCommand();\nSleep(100000);\n",
                       "data/subai/research.vs"));
  REQUIRE(f.add_script(
      "// void, Obj This, bool bCanceled\n"
      "if (bCanceled) This.SetUser(2); else This.SetUser(1);\n",
      "data/subai/onfinish_research.vs"));
  REQUIRE(f.add_script("// void, Obj me\nwhile (1) Sleep(1000);\n", "data/subai/unit_idle.vs"));
  const ObjectId u = f.spawn(f.unit_class);
  f.commands.set_command(f.world, u, "research", Command{});
  for (int i = 0; i < 4; ++i) {
    f.scheduler.advance(200);
    f.world.advance(200);
  }
  CHECK(f.commands.command_name(u) != "research");
  CHECK(f.world.find(u)->state.user == 1);
}

// --------------------------------------------------------------------------
// the command slot's end: 0x005b5160 -> 0x005b19f0 -> 0x005b4f20
// --------------------------------------------------------------------------

namespace {

/// A `RunFixture` with the session's step hook, which is where a command's
/// thread ending is told to its queue (0x0069f7a0 -> `vtbl+0x1c`), and the
/// four scripts the tests below queue. Each one leaves its mark in the unit's
/// `user` word: `move` adds 1 and returns, `guard` adds 10 and stays,
/// `research` waits, and `research`'s `onfinish` adds 1000 when it is told it
/// was cancelled and 100 when not.
struct EndFixture : RunFixture {
  EndFixture() {
    install_deferred_erase(scheduler);
    (void)register_global_hosts(registry);
    (void)add_script("// void, Obj This, point pt\nThis.SetUser(This.user + 1);\n",
                     "data/subai/unit_move.vs");
    (void)add_script("// void, Obj This\nThis.SetUser(This.user + 10);\nSleep(100000);\n",
                     "data/subai/unit_guard.vs");
    (void)add_script("// void, Obj This\nSleep(500);\n", "data/subai/research.vs");
    (void)add_script("// void, Obj me\nwhile (1) Sleep(1000);\n", "data/subai/unit_idle.vs");
  }
  void onfinish(std::string_view first_line) {
    std::string source = "// void, Obj This, bool bCanceled\n";
    source += first_line;
    source +=
        "if (bCanceled) This.SetUser(This.user + 1000); else This.SetUser(This.user + 100);\n";
    (void)add_script(source, "data/subai/onfinish_research.vs");
  }
  std::int32_t user(ObjectId id) { return world.find(id)->state.user; }
  const Command* head(ObjectId id) {
    const CommandQueue* q = commands.find(id);
    return q == nullptr ? nullptr : q->running();
  }
};

}  // namespace

/// **A command whose script returns starts the next one there and then**,
/// before any other script runs: 0x0069f7a0 calls the owner's `vtbl+0x1c` as
/// soon as the thread's slice is over and it has finished, and 0x005b5160
/// pops the head and starts the next through 0x005b4f20, whose thread is armed
/// at the clock and so runs in the same pass. It used to wait for the next
/// turn's command pass.
/// **A list-and-point command hands its script a list and a point**: kind 5,
/// `SHIP_UNBOARD.VS`'s `// void, Obj This, ObjList ol, point pt`, which the
/// issue's group-move set-up makes for a ship (0x004ef1c0). The list is the
/// script's temporary and goes with it.
TEST(a_list_and_point_command_hands_its_script_both) {
  EndFixture f;
  (void)register_objlist_host(f.registry);
  REQUIRE(f.add_script("// void, Obj This, ObjList ol, point pt\n"
                       "This.SetUser(ol.count * 1000 + pt.x);\n",
                       "data/subai/wagon_unload.vs"));
  const ObjectId u = f.spawn(f.unit_class);
  const ObjectId a = f.spawn(f.unit_class);
  const ObjectId b = f.spawn(f.unit_class);
  CHECK(f.commands.binds(f.world, u, "unload", CommandArgKind::list_point));
  CHECK(!f.commands.binds(f.world, u, "unload", CommandArgKind::point));
  Command unload;
  unload.arg_kind = CommandArgKind::list_point;
  unload.point = Point{321, 50};
  unload.list = {a, b};
  REQUIRE(f.commands.set_command(f.world, u, "unload", unload) != 0);
  f.scheduler.advance(100);
  CHECK(f.user(u) == 2321);
}

/// **A command whose script traps does not start the next inside the pass.**
/// An `idle` that queues a command and returns, over a command whose script
/// traps on its first slice, would otherwise end and start one another on
/// the same instant for ever (`EAGLE_IDLE.VS` over `EAGLE_MOVE.VS` in a
/// session with no entities, whose `GetAnimDuration(0)` divides by zero).
/// Bounded: the `idle` counts its runs and stops handing back at fifty, so
/// the unfixed loop fails here rather than hangs.
TEST(a_command_whose_script_traps_starts_the_next_at_the_pass_not_at_once) {
  RunFixture f;
  install_deferred_erase(f.scheduler);
  (void)register_global_hosts(f.registry);
  REQUIRE(f.add_script("// void, Obj This\n"
                       "This.SetUser(This.user + 1);\n"
                       "if (This.user > 50) { while (1) Sleep(100000); }\n"
                       "This.AddCommand(true, \"engage\");\n",
                       "data/subai/unit_idle.vs"));
  REQUIRE(f.add_script("// void, Obj This\nint z;\nz = 0;\nz = 1 / z;\n",
                       "data/subai/unit_engage.vs"));
  const ObjectId u = f.spawn(f.unit_class);
  REQUIRE(f.commands.start_slot(f.world, u));
  f.scheduler.advance(100);
  // The idle ran once and handed over; `engage` trapped and ended; the next
  // idle waits for the command pass.
  CHECK(f.world.find(u)->state.user == 1);
  // Each command pass starts the next idle, which hands over once more.
  for (int turn = 0; turn < 3; ++turn) {
    Turn pass;
    pass.time = f.scheduler.now();
    f.commands.advance(f.world, pass);
    f.scheduler.advance(100);
  }
  CHECK(f.world.find(u)->state.user == 4);
}

TEST(a_command_that_returns_starts_the_next_in_the_same_pass) {
  EndFixture f;
  f.onfinish("");
  const ObjectId u = f.spawn(f.unit_class);
  Command order;
  order.arg_kind = CommandArgKind::point;
  order.point = Point{300, 300};
  f.commands.set_command(f.world, u, "move", order);
  f.commands.add_command(f.world, u, false, "guard", Command{});
  // One scheduler pass and no command pass: `move` ran and returned, and
  // `guard` started at its end and ran in the same pass.
  f.scheduler.advance(100);
  REQUIRE(f.head(u) != nullptr);
  CHECK(f.head(u)->verb == "guard");
  CHECK(f.user(u) == 11);
  // And a queue that runs dry is given its `idle` the same way.
  f.commands.set_command(f.world, u, "move", order);
  f.scheduler.advance(100);
  REQUIRE(f.head(u) != nullptr);
  CHECK(f.head(u)->verb == "idle");
  CHECK(f.head(u)->started);
  CHECK(f.scheduler.alive(f.head(u)->script));
}

/// **Ended by another script, a command is told inside the call and the next
/// starts inside it too**: 0x005b07d0 deletes the thread through 0x0069fee0,
/// which calls `vtbl+0x1c` at once under the cancel flag -- the `onfinish`
/// runs to its end there (0x0059af00 through the synchronous launcher
/// 0x006a0360), the head is popped and the next started. The `onfinish` was
/// spawned and ran a pass late, and the next command waited for the pass.
TEST(a_command_ended_by_another_script_is_told_and_followed_inside_the_call) {
  EndFixture f;
  f.onfinish("");
  const ObjectId u = f.spawn(f.unit_class);
  f.commands.set_command(f.world, u, "research", Command{});
  f.commands.add_command(f.world, u, false, "guard", Command{});
  REQUIRE(f.head(u) != nullptr);
  REQUIRE(f.head(u)->verb == "research");
  REQUIRE(f.head(u)->started);

  CHECK(f.commands.kill_command(f.world, u));
  // No pass has run.
  CHECK(f.user(u) == 1000);
  REQUIRE(f.head(u) != nullptr);
  CHECK(f.head(u)->verb == "guard");
  CHECK(f.head(u)->started);
  CHECK(f.scheduler.alive(f.head(u)->script));
}

/// **A command ended by its own script stops at that call.** 0x0069fee0 finds
/// the thread it is to end is the one running and only raises its stop flag
/// (`[ctx+0x40]`); the interpreter reads it as the host call returns
/// (0x0069d4c0) and ends the script there, so nothing after the call runs.
/// The end then pops it with the cancel flag down and starts the next. Here
/// the script used to run on to its next suspension.
TEST(a_command_ending_itself_stops_at_the_call_and_the_next_starts_after_it) {
  EndFixture f;
  (void)f.add_script("// void, Obj This\nThis.KillCommand();\nThis.SetUser(This.user + 5);\n"
                     "Sleep(100000);\n",
                     "data/subai/barrack_train.vs");
  const ObjectId u = f.spawn(f.unit_class);
  f.commands.set_command(f.world, u, "train", Command{});
  f.commands.add_command(f.world, u, false, "guard", Command{});
  f.scheduler.advance(100);
  // Not 15: the line after `KillCommand` never ran; `guard` started at the
  // end and ran in the same pass.
  CHECK(f.user(u) == 10);
  REQUIRE(f.head(u) != nullptr);
  CHECK(f.head(u)->verb == "guard");
}

/// **While a command's `onfinish` runs, ending index 0 does nothing.** The
/// thread is deleted before `vtbl+0x1c` is called, and 0x005b07d0 does
/// nothing at all without a live thread in the command slot (0x0069f950), so
/// a `KillCommand` inside the `onfinish` leaves the head for the end to pop,
/// and the command behind it starts.
TEST(a_kill_command_inside_an_onfinish_leaves_the_queue_to_the_end) {
  EndFixture f;
  f.onfinish("This.KillCommand();\n");
  const ObjectId u = f.spawn(f.unit_class);
  f.commands.set_command(f.world, u, "research", Command{});
  f.commands.add_command(f.world, u, false, "guard", Command{});
  CHECK(f.commands.kill_command(f.world, u));
  CHECK(f.user(u) == 1000);
  REQUIRE(f.head(u) != nullptr);
  CHECK(f.head(u)->verb == "guard");
  CHECK(f.commands.command_count(u) == 1);
}

/// **A research that jumps a running order starts inside the issue**
/// (0x004efa82..0x004efaba: the runner's thread deleted, its `onfinish` told
/// it was cancelled, the research pushed to the front and started through
/// 0x005b4f20). It used to stand unstarted until the next pass.
TEST(a_research_that_jumps_the_queue_starts_inside_the_issue) {
  EndFixture f;
  f.onfinish("");
  CommandDef study;
  study.name = "researchArmour";
  study.method = "research";
  study.research_command = true;
  f.commands.mutable_table().set(study);
  CommandDef stand;
  stand.name = "guardHere";
  stand.method = "guard";
  f.commands.mutable_table().set(stand);
  const ObjectId u = f.spawn(f.unit_class);
  HostCall order(f.world, {obj(u), Value::string("guardHere"), Value::nil(), Value::nil(),
                           Value::boolean(true)});
  REQUIRE(invoke(f.registry, CallKind::member, "ExecCmd", 4, order).status == HostStatus::ok);
  f.commands.add_command(f.world, u, false, "guard", Command{});
  REQUIRE(f.head(u) != nullptr);
  REQUIRE(f.head(u)->verb == "guard");
  HostCall call(f.world, {obj(u), Value::string("researchArmour"), Value::nil(), Value::nil(),
                          Value::boolean(false)});
  REQUIRE(invoke(f.registry, CallKind::member, "ExecCmd", 4, call).status == HostStatus::ok);
  REQUIRE(f.head(u) != nullptr);
  CHECK(f.head(u)->verb == "research");
  CHECK(f.head(u)->started);
  CHECK(f.scheduler.alive(f.head(u)->script));
}

// --------------------------------------------------------------------------
// the host audit: ExecCmd through the per-object issue, 0x004ef3d0
// --------------------------------------------------------------------------

/// `CmdCount(name)` (0x005ae4d0) counts the queued commands that came in
/// through the row called `name` -- not the ones running its verb. A train
/// row's verb is `train`, so the AI's `barrack.CmdCount(cmd)` beside
/// `barrack.ExecCmd(cmd, ...)` read 0 for every barracks and never stopped
/// queueing.
TEST(cmd_count_with_a_name_counts_the_rows_commands_not_the_verbs) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId barracks = f.spawn(f.unit_class);
  for (int i = 0; i < 2; ++i) {
    HostCall call(f.world, {obj(barracks), Value::string("trainhastatus"), Value::nil(),
                            Value::nil(), Value::boolean(false)});
    REQUIRE(invoke(registry, CallKind::member, "ExecCmd", 4, call).status == HostStatus::ok);
  }
  f.add(barracks, false, "train");  // a script's own: no row, never counted
  const auto count = [&](std::string_view name) {
    HostCall call(f.world, {obj(barracks), Value::string(std::string(name))});
    return invoke(registry, CallKind::member, "CmdCount", 1, call).value.as_integer();
  };
  CHECK(count("trainhastatus") == 2);
  CHECK(count("train") == 0);
  CHECK(count("TrainHastatus") == 0);  // byte for byte
  HostCall all(f.world, {obj(barracks)});
  CHECK(invoke(registry, CallKind::member, "CmdCount", 0, all).value.as_integer() == 3);
}

/// The loader reads `traincommand`, `researchcommand`, `immediate` and
/// `plrresearch` as anything but `no` (0x004f3f80..0x004f405a), so the
/// shipped `immediate="yes"` is true, and a player research is a research.
TEST(the_command_tables_yes_no_flags_are_anything_but_no) {
  CommandTable table;
  REQUIRE(table.merge(bytes(R"(<commands>
<cmd name="a" immediate="yes" traincommand="no"/>
<cmd name="b" plrresearch="yes"/>
<cmd name="c" researchcommand="Yes" immediate="NO"/>
</commands>)"))
              .ok());
  CHECK(table.find("a")->immediate);
  CHECK(!table.find("a")->train_command);
  CHECK(table.find("b")->plr_research);
  CHECK(table.find("b")->research_command);
  CHECK(table.find("c")->research_command);
  CHECK(!table.find("c")->immediate);
}

/// A row with a `groupdispatch` is not queued: its dispatch runs once, to its
/// end, with the row's `cmdparam` and `cmdcost_*` ambient for the call
/// (0x004ef5c1..0x004ef632, cleared at 0x004ef6ad..0x004ef6f2). The AI's hero
/// autocast reaches `ACTIVATE_HERO_SKILL.VS` this way, which reads both, and
/// used to queue an unbound verb instead.
TEST(exec_cmd_runs_a_rows_group_dispatch_with_the_row_ambient) {
  ShapeFixture f;
  (void)register_global_hosts(f.registry);
  (void)register_text_host(f.registry);
  f.library.sources.emplace_back(
      "data/subai/activate_test.vs",
      "// void, ObjList objs, point pt, Obj obj, bool bReplace, bool bModifier, int player\n"
      "obj.SetUser(StrLen(cmdparam) * 100 + cmdcost_stamina * 10);\n");
  CommandDef skill;
  skill.name = "skill_battle_cry";
  skill.method = "skill_battle_cry";
  skill.param = "Battle cry";
  skill.cost_stamina = 6;
  skill.group_dispatch = "data/subai/activate_test.vs";
  f.commands.mutable_table().set(skill);

  const ObjectId hero = f.spawn(f.hero_class);
  HostCall call(f.world, {obj(hero), Value::string("skill_battle_cry"), pack_point(Point{}),
                          obj(hero), Value::boolean(false)});
  REQUIRE(invoke(f.registry, CallKind::member, "ExecCmd", 4, call).status == HostStatus::ok);
  CHECK(f.world.find(hero)->state.user == 1060);
  CHECK(f.commands.command_count(hero) == 0);
  // And the row is ambient no longer once the call is over.
  CHECK(f.commands.command_of_script(script::kNoScript) == nullptr);
}

/// An `immediate` row runs its method now, synchronously, with the row
/// ambient, and is never queued (0x004ef917..0x004ef9df).
TEST(exec_cmd_runs_an_immediate_rows_method_at_once_and_queues_nothing) {
  ShapeFixture f;
  (void)register_global_hosts(f.registry);
  (void)register_text_host(f.registry);
  f.library.sources.clear();
  f.library.sources.emplace_back("data/subai/unit_guard.vs",
                                 "// void, Obj This\nThis.SetUser(StrLen(cmdparam));\n");
  CommandDef now;
  now.name = "guard_now";
  now.method = "guard";
  now.param = "seven c";
  now.immediate = true;
  f.commands.mutable_table().set(now);

  const ObjectId u = f.spawn(f.unit_class);
  f.commands.set_command(f.world, u, "move", Command{});
  HostCall call(f.world, {obj(u), Value::string("guard_now"), Value::nil(), Value::nil(),
                          Value::boolean(false)});
  REQUIRE(invoke(f.registry, CallKind::member, "ExecCmd", 4, call).status == HostStatus::ok);
  CHECK(f.world.find(u)->state.user == 7);
  CHECK(f.verbs(u) == (std::vector<std::string>{"move"}));
}

/// A research row (0x004efa05..0x004efad5): an ordinary order on an object
/// with at most one command and that one `idle`; otherwise it jumps ahead of
/// a running command that came in through a non-research row, which goes
/// back to index 1 to start again; and it is dropped behind a research or a
/// row-less command.
TEST(exec_cmd_a_research_jumps_a_running_rows_command_and_never_queues_behind_one) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  CommandDef study;
  study.name = "researchArmour";
  study.method = "research";
  study.research_command = true;
  f.commands.mutable_table().set(study);
  const auto exec = [&](ObjectId who, std::string_view row) {
    HostCall call(f.world, {obj(who), Value::string(std::string(row)), Value::nil(), Value::nil(),
                            Value::boolean(false)});
    (void)invoke(registry, CallKind::member, "ExecCmd", 4, call);
  };

  const ObjectId busy = f.spawn(f.unit_class);
  exec(busy, "trainhastatus");
  REQUIRE(f.verbs(busy) == (std::vector<std::string>{"train"}));
  exec(busy, "researchArmour");
  CHECK(f.verbs(busy) == (std::vector<std::string>{"research", "train"}));
  CHECK(!f.commands.find(busy)->entries[1].started);
  // Behind a research it is dropped, though its command was made.
  const std::uint32_t seed = f.world.command_id_seed();
  exec(busy, "researchArmour");
  CHECK(f.verbs(busy) == (std::vector<std::string>{"research", "train"}));
  CHECK(f.world.command_id_seed() == seed + 1);

  // Resting on `idle`: an ordinary order, which ends the resting head.
  const ObjectId resting = f.spawn(f.unit_class);
  f.order(resting, "idle");
  exec(resting, "researchArmour");
  CHECK(f.verbs(resting) == (std::vector<std::string>{"research"}));

  // A row-less command running with something behind it: dropped.
  const ObjectId scripted = f.spawn(f.unit_class);
  f.order(scripted, "move");
  f.add(scripted, false, "guard");
  exec(scripted, "researchArmour");
  CHECK(f.verbs(scripted) == (std::vector<std::string>{"move", "guard"}));
}

/// A `plrresearch` row is held once per player (0x005b1883..0x005b18ad).
TEST(a_player_research_is_refused_while_another_of_the_players_objects_holds_it) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  CommandDef tribute;
  tribute.name = "RTribute";
  tribute.method = "research";
  tribute.research_command = true;
  tribute.plr_research = true;
  f.commands.mutable_table().set(tribute);
  const auto exec = [&](ObjectId who) {
    HostCall call(f.world, {obj(who), Value::string("RTribute"), Value::nil(), Value::nil(),
                            Value::boolean(false)});
    (void)invoke(registry, CallKind::member, "ExecCmd", 4, call);
  };
  const ObjectId first = f.spawn(f.unit_class, Point{100, 100}, 1);
  const ObjectId second = f.spawn(f.unit_class, Point{300, 100}, 1);
  const ObjectId rival = f.spawn(f.unit_class, Point{500, 100}, 2);
  exec(first);
  exec(second);
  exec(rival);
  CHECK(f.commands.command_count_named(first, "RTribute") == 1);
  CHECK(f.commands.command_count_named(second, "RTribute") == 0);
  CHECK(f.commands.command_count_named(rival, "RTribute") == 1);
  // Once the first lets go, the second may have it.
  CHECK(f.commands.kill_command(f.world, first));
  exec(second);
  CHECK(f.commands.command_count_named(second, "RTribute") == 1);
}

/// `ExecCmd` refuses a dead receiver (0x005b5520).
TEST(exec_cmd_gives_a_dead_object_nothing) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId barracks = f.spawn(f.unit_class);
  f.world.set_health(barracks, 0);
  HostCall call(f.world, {obj(barracks), Value::string("trainhastatus"), Value::nil(),
                          Value::nil(), Value::boolean(false)});
  REQUIRE(invoke(registry, CallKind::member, "ExecCmd", 4, call).status == HostStatus::ok);
  CHECK(f.commands.command_count(barracks) == 0);
}

/// `Progress()` (0x005ae890) refuses a dead receiver -- no bar, no suspend --
/// while `Progress(ms)` (0x005ac530) asks nothing about health.
TEST(command_progress_without_an_argument_does_not_suspend_a_dead_object) {
  Fixture f;
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId u = f.spawn(f.unit_class);
  Command slow;
  slow.delay = 9000;
  f.commands.set_command(f.world, u, "train", slow);
  f.world.set_health(u, 0);
  HostCall bare(f.world, {obj(u)});
  CHECK(invoke(registry, CallKind::member, "Progress", 0, bare).status == HostStatus::ok);
  HostCall timed(f.world, {obj(u), Value::integer(500)});
  CHECK(invoke(registry, CallKind::member, "Progress", 1, timed).status != HostStatus::ok);
}

/// `FormAcceptMove` on a unit with no hero, or no march, returns 2
/// (0x005d7a1c): the calling script ends there, so `UNIT_FORM_MOVE.VS` never
/// runs its `while (.HasPath())` or its closing `Face`.
TEST(form_accept_move_ends_the_script_of_a_unit_with_no_march) {
  Fixture f;
  HeroSystem heroes;
  MovementSystem movement;
  f.world.add_system(&heroes);
  f.world.add_system(&movement);
  HostRegistry registry;
  register_command_host(registry);
  const ObjectId loner = f.spawn(f.warrior_class);
  HostCall call(f.world, {obj(loner)});
  CHECK(invoke(registry, CallKind::member, "FormAcceptMove", 0, call).status ==
        HostStatus::finish);
}
