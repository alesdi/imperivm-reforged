// The VS compiler, virtual machine and scheduler.
//
// Every script here is written for the test. None of it is shipped data: the
// repository must never carry game assets, and a test that needs an
// installation is a test CI cannot run. The scripts are shaped like the retail
// ones on purpose -- a behaviour loop around a `Sleep`, a `for` that
// `continue`s, a host call that writes through an argument -- because those
// shapes are what the corpus is made of.

#include <cstdint>
#include <string>
#include <vector>

#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/script/vm.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::script;

namespace {

// -- a host to run against ---------------------------------------------------
//
// Small on purpose. It implements `point` as a handle into a side table, which
// is the shape Part 5 will need for the real thing: `point` is a value type in
// VS, so it clones on assignment, and it supports arithmetic that the VM itself
// must not know about.

constexpr TypeId kPoint = 1;
constexpr TypeId kList = 2;
constexpr TypeId kUnit = 3;
constexpr TypeId kArray = 4;

struct World {
  std::vector<std::int32_t> point_x;
  std::vector<std::int32_t> point_y;
  /// Every list in this world holds the same three units, which is enough to
  /// exercise `ol.count` and `ol[i]`.
  std::vector<std::vector<std::uint32_t>> lists;
  /// `IntArray`, which is the one container the language subscripts on both
  /// sides of an assignment.
  std::vector<std::vector<std::int32_t>> arrays;

  int ticks = 0;
  std::vector<std::string> log;
  /// How many times a blocking wait has polled, and what the last poll was told
  /// about its own elapsed time.
  int wait_polls = 0;
  std::int64_t last_elapsed = -1;
  /// Elapsed time as reported to every poll, in order. A *fresh* wait always
  /// reports 0 on its first poll, so the count of zeros is the count of waits
  /// that were given their own stopwatch.
  std::vector<std::int64_t> elapsed_log;

  std::uint32_t make_point(std::int32_t x, std::int32_t y) {
    point_x.push_back(x);
    point_y.push_back(y);
    return static_cast<std::uint32_t>(point_x.size() - 1);
  }
};

class TestHost : public Host {
 public:
  explicit TestHost(World& world) : world_(world) {}

  Value default_value(std::string_view type_name) override {
    if (type_name == "point") return Value::object(kPoint, world_.make_point(0, 0));
    if (type_name == "ObjList") {
      world_.lists.push_back({10, 11, 12});
      return Value::object(kList, static_cast<std::uint32_t>(world_.lists.size() - 1));
    }
    if (type_name == "IntArray") {
      world_.arrays.push_back(std::vector<std::int32_t>(4, 0));
      return Value::object(kArray, static_cast<std::uint32_t>(world_.arrays.size() - 1));
    }
    return Host::default_value(type_name);
  }

  /// `point` is a value type. Handles are not, so only points clone.
  Value clone_for_assign(const Value& value) override {
    if (value.is_object() && value.as_object().type == kPoint) {
      const std::uint32_t id = value.as_object().id;
      return Value::object(kPoint, world_.make_point(world_.point_x[id], world_.point_y[id]));
    }
    return value;
  }

  Result<Value> binary(BinaryOp op, const Value& lhs, const Value& rhs) override {
    const bool left_point = lhs.is_object() && lhs.as_object().type == kPoint;
    const bool right_point = rhs.is_object() && rhs.as_object().type == kPoint;
    if (left_point && right_point && (op == BinaryOp::add || op == BinaryOp::sub)) {
      const std::uint32_t a = lhs.as_object().id;
      const std::uint32_t b = rhs.as_object().id;
      const std::int32_t sign = op == BinaryOp::add ? 1 : -1;
      return Value::object(kPoint, world_.make_point(world_.point_x[a] + sign * world_.point_x[b],
                                                     world_.point_y[a] + sign * world_.point_y[b]));
    }
    if (left_point && rhs.is_integer() && (op == BinaryOp::mul || op == BinaryOp::div)) {
      const std::uint32_t a = lhs.as_object().id;
      const std::int32_t k = rhs.as_integer();
      if (op == BinaryOp::div && k == 0) return FormatError::malformed;
      return Value::object(kPoint,
                           world_.make_point(op == BinaryOp::mul ? world_.point_x[a] * k
                                                                 : world_.point_x[a] / k,
                                             op == BinaryOp::mul ? world_.point_y[a] * k
                                                                 : world_.point_y[a] / k));
    }
    return FormatError::unsupported;
  }

  Result<Value> index_get(const Value& container, const Value& key) override {
    if (!key.is_integer()) return FormatError::malformed;
    if (container.is_object() && container.as_object().type == kArray) {
      const std::vector<std::int32_t>& array = world_.arrays[container.as_object().id];
      const std::int32_t at = key.as_integer();
      if (at < 0 || static_cast<std::size_t>(at) >= array.size()) {
        return FormatError::out_of_range;
      }
      return Value::integer(array[static_cast<std::size_t>(at)]);
    }
    if (!container.is_object() || container.as_object().type != kList) {
      return FormatError::unsupported;
    }
    const std::vector<std::uint32_t>& list = world_.lists[container.as_object().id];
    const std::int32_t at = key.as_integer();
    if (at < 0 || static_cast<std::size_t>(at) >= list.size()) return FormatError::out_of_range;
    return Value::object(kUnit, list[static_cast<std::size_t>(at)]);
  }

  Status index_set(Value& container, const Value& key, const Value& value) override {
    if (!container.is_object() || container.as_object().type != kArray) {
      return FormatError::unsupported;
    }
    if (!key.is_integer() || !value.is_integer()) return FormatError::malformed;
    std::vector<std::int32_t>& array = world_.arrays[container.as_object().id];
    const std::int32_t at = key.as_integer();
    if (at < 0 || static_cast<std::size_t>(at) >= array.size()) return FormatError::out_of_range;
    array[static_cast<std::size_t>(at)] = value.as_integer();
    return Status();
  }

  Result<Value> global(std::string_view name) override {
    if (name == "AI_COMING") return Value::integer(1);
    if (name == "AI_STAYING") return Value::integer(2);
    if (name == "Carthage") return Value::integer(4);
    return FormatError::not_found;
  }

  Result<std::string> to_string(const Value& value) override {
    if (!value.is_object()) return FormatError::unsupported;
    return std::string("#") + to_decimal(static_cast<std::int32_t>(value.as_object().id));
  }

 private:
  World& world_;
};

World& world_of(CallContext& context) { return *static_cast<World*>(context.user); }

HostOutcome host_tick(CallContext& context) {
  ++world_of(context).ticks;
  return HostOutcome::ok_void();
}

HostOutcome host_note(CallContext& context) {
  World& world = world_of(context);
  if (context.arg(0).is_string()) {
    world.log.push_back(context.arg(0).as_string());
  } else {
    world.log.push_back(to_decimal(context.arg(0).as_integer()));
  }
  return HostOutcome::ok_void();
}

/// The out-parameter idiom, exactly as `ParseStr(s, OUT tail)` uses it: return
/// the head, write the tail through the second argument.
HostOutcome host_split(CallContext& context) {
  const std::string text = context.arg(0).as_string();
  const std::size_t space = text.find(' ');
  if (space == std::string::npos) {
    context.out(1) = Value::string(std::string());
    return HostOutcome::ok_with(Value::string(text));
  }
  context.out(1) = Value::string(text.substr(space + 1));
  return HostOutcome::ok_with(Value::string(text.substr(0, space)));
}

/// `g.Eval(flags, a, b)` -- a member call that writes through two arguments at
/// once, which is the shape `CALCMAXTAKE.VS` depends on.
HostOutcome host_eval(CallContext& context) {
  context.out(1) = Value::integer(context.arg(1).as_integer() + 10);
  context.out(2) = Value::integer(context.arg(2).as_integer() + 20);
  return HostOutcome::ok_void();
}

HostOutcome host_point(CallContext& context) {
  World& world = world_of(context);
  return HostOutcome::ok_with(Value::object(
      kPoint, world.make_point(context.arg(0).as_integer(), context.arg(1).as_integer())));
}

HostOutcome host_point_x(CallContext& context) {
  World& world = world_of(context);
  return HostOutcome::ok_with(Value::integer(world.point_x[context.arg(0).as_object().id]));
}

/// A mutating member on a value type: `pt.SetLen(15)` and friends.
HostOutcome host_point_scale(CallContext& context) {
  World& world = world_of(context);
  const std::uint32_t id = context.arg(0).as_object().id;
  world.point_x[id] *= context.arg(1).as_integer();
  world.point_y[id] *= context.arg(1).as_integer();
  return HostOutcome::ok_void();
}

HostOutcome host_count(CallContext& context) {
  World& world = world_of(context);
  const std::vector<std::uint32_t>& list = world.lists[context.arg(0).as_object().id];
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(list.size())));
}

HostOutcome host_size(CallContext& context) {
  World& world = world_of(context);
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(world.arrays[context.arg(0).as_object().id].size())));
}

HostOutcome host_boom(CallContext&) { return HostOutcome::failed("Boom always refuses"); }

/// A blocking wait that has to re-poll: it suspends *at* the call and runs
/// again on resume, which is what `WaitNonEmptyQuery` needs and what `Sleep`
/// does not.
HostOutcome host_wait_for_ticks(CallContext& context) {
  World& world = world_of(context);
  if (world.ticks >= context.arg(0).as_integer()) return HostOutcome::ok_void();
  HostOutcome outcome;
  outcome.status = HostStatus::retry;
  outcome.suspend_for = 10;
  return outcome;
}

/// A blocking wait with a **deadline**, which is the shape every `Wait*` entry
/// point has: poll a predicate, give up after N milliseconds, return whether it
/// held. It measures its own elapsed time from `CallContext::waiting_since`,
/// which is the only thing the runtime gives a retrying call that a plain call
/// does not have.
///
/// `WaitUntilTicks(target, timeout)` -> bool.
HostOutcome host_wait_until_ticks(CallContext& context) {
  World& world = world_of(context);
  world.wait_polls += 1;
  world.last_elapsed = context.now - context.waiting_since;
  world.elapsed_log.push_back(world.last_elapsed);
  if (world.ticks >= context.arg(0).as_integer()) {
    return HostOutcome::ok_with(Value::boolean(true));
  }
  const std::int64_t timeout = context.arg(1).as_integer();
  if (timeout >= 0 && context.now - context.waiting_since >= timeout) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  HostOutcome outcome;
  outcome.status = HostStatus::retry;
  outcome.suspend_for = 10;
  return outcome;
}

HostRegistry make_registry() {
  HostRegistry registry;
  declare_shipped_surface(registry);
  register_scheduler_builtins(registry);
  registry.define(CallKind::free_function, "Tick", 0, &host_tick);
  registry.define(CallKind::free_function, "Note", 1, &host_note);
  registry.define(CallKind::free_function, "Split", 2, &host_split);
  registry.define(CallKind::free_function, "Point", 2, &host_point);
  registry.define(CallKind::free_function, "Boom", 0, &host_boom);
  registry.define(CallKind::free_function, "WaitForTicks", 1, &host_wait_for_ticks);
  registry.define(CallKind::free_function, "WaitUntilTicks", 2, &host_wait_until_ticks);
  registry.define(CallKind::member, "Eval", 2, &host_eval);
  registry.define(CallKind::member, "x", 0, &host_point_x);
  registry.define(CallKind::member, "Scale", 1, &host_point_scale);
  registry.define(CallKind::member, "count", 0, &host_count);
  registry.define(CallKind::member, "size", 0, &host_size);
  return registry;
}

/// Source, AST and chunk in one place, because the AST borrows the source and
/// both have to outlive the compile.
struct Program {
  std::string source;
  std::string name;
  Chunk chunk;
  bool ok = false;
  std::string error;
};

Program build(std::string_view text, const char* name, const HostRegistry& registry) {
  Program program;
  program.source = std::string(text);
  program.name = name;
  std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(program.source.data()),
                                   program.source.size());
  Diagnostic diagnostic;
  Result<Script> script = parse(bytes, program.name, &diagnostic);
  if (!script.ok()) {
    program.error = "parse: " + std::string(diagnostic.message);
    return program;
  }
  CompileError compile_error;
  Result<Chunk> chunk = compile(script.value(), &registry, &compile_error);
  if (!chunk.ok()) {
    program.error = "compile: " + compile_error.message;
    return program;
  }
  program.chunk = std::move(chunk.value());
  program.ok = true;
  return program;
}

/// Run one program to completion outside the scheduler.
struct Runner {
  World world;
  TestHost host{world};
  HostRegistry registry = make_registry();

  VmEnv env() {
    VmEnv out;
    out.registry = &registry;
    out.host = &host;
    // This file's `World` is a fixture of its own, not `sim::World`, so `user`
    // here is the fixture -- the VM never interprets it.
    out.user = &world;
    return out;
  }
};

}  // namespace

TEST(vs_arithmetic_is_integer_only) {
  Runner runner;
  const Program program = build(
      "// int\n"
      "int a, b;\n"
      "a = 7;\n"
      "b = 2;\n"
      "return a * 100 + a / b * 10 + a % b;\n",
      "arith.vs", runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  CHECK(execution.result.is_integer());
  CHECK(execution.result.as_integer() == 731);
}

TEST(vs_division_by_zero_traps_rather_than_crashing) {
  Runner runner;
  const Program program = build("// int\nint z;\nz = 0;\nreturn 1 / z;\n", "div.vs",
                                runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::failed);
  CHECK(execution.trap.code == TrapCode::divide_by_zero);
  CHECK(execution.trap.line == 4);
  CHECK(execution.trap.source_name == "div.vs");
}

TEST(vs_comparison_and_boolean_coercion) {
  Runner runner;
  // `bool` is numerically usable: ENCHANTRESS_IDLE.VS adds a comparison result
  // straight into an int expression.
  const Program program = build(
      "// int\n"
      "int n;\n"
      "n = 0;\n"
      "n = n + (3 > 2) + (2 > 3) + (1 == 1) + (1 != 1);\n"
      "n = n + 10 * (true && false) + 100 * (false || true);\n"
      "return n;\n",
      "cmp.vs", runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 102);
}

TEST(vs_short_circuit_does_not_evaluate_the_right_hand_side) {
  Runner runner;
  // `Boom()` refuses if it is ever called, so reaching the end proves both
  // operators stopped early.
  const Program program = build(
      "// int\n"
      "int n;\n"
      "n = 0;\n"
      "if (n && Boom()) n = 1;\n"
      "if (n == 0 || Boom()) n = n + 5;\n"
      "return n;\n",
      "shortcircuit.vs", runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 5);
}

TEST(vs_strings_concatenate_and_coerce) {
  Runner runner;
  const Program program = build(
      "// str\n"
      "int n;\n"
      "n = 12;\n"
      "return \"unit \" + n + \" of \" + 3;\n",
      "str.vs", runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  REQUIRE(execution.result.is_string());
  CHECK(execution.result.as_string() == "unit 12 of 3");
}

// `EnvReadString(set, "NeedTechSucceed") != 0` (`ESH_BUILDARMY.VS:381`): a
// string against an integer compares as strings (INFERRED; see `compare`).
TEST(vs_a_string_compared_with_an_integer_compares_as_strings) {
  Runner runner;
  const Program program = build(
      "// int\n"
      "str s;\n"
      "s = \"0\";\n"
      "if (s != 0) return 1;\n"
      "s = \"1\";\n"
      "if (s == 1 && \"\" != 0 && 2 < \"3\") return 2;\n"
      "return 3;\n",
      "strint.vs", runner.registry);
  REQUIRE(program.ok);
  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 2);
}

TEST(vs_control_flow_covers_if_while_for_break_and_continue) {
  Runner runner;
  // The `continue` is the point: the front end normalises `for` into a `while`
  // with the step carried separately, and a `continue` that skipped the step
  // would spin here rather than finishing.
  const Program program = build(
      "// int\n"
      "int i, total;\n"
      "total = 0;\n"
      "for (i = 0; i < 10; i += 1) {\n"
      "  if (i % 2 == 0) continue;\n"
      "  if (i > 7) break;\n"
      "  total = total + i;\n"
      "}\n"
      "while (total < 100) total = total + 50;\n"
      "if (total == 116) return total;\n"
      "else return -1;\n",
      "flow.vs", runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  std::uint64_t steps = 0;
  REQUIRE(run(execution, program.chunk, runner.env(), &steps) == ExecStatus::finished);
  // 1 + 3 + 5 + 7 = 16, then one pass of the while loop.
  CHECK(execution.result.as_integer() == 116);
  CHECK(steps > 0);
}

TEST(vs_locals_shadow_in_nested_blocks) {
  Runner runner;
  // `This` and `this` are live at once in 204 shipped files, and `int i;` is
  // redeclared in nested blocks in dozens of them.
  const Program program = build(
      "// int, int This\n"
      "int this;\n"
      "this = 1;\n"
      "{\n"
      "  int this;\n"
      "  this = 40;\n"
      "  This = This + this;\n"
      "}\n"
      "return This + this;\n",
      "shadow.vs", runner.registry);
  REQUIRE(program.ok);

  const Value argument = Value::integer(1);
  Execution execution = start(program.chunk, std::span<const Value>(&argument, 1));
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 42);
}

TEST(vs_out_parameters_are_written_back_to_the_callers_locals) {
  Runner runner;
  // No syntax marks these. `Split` writes its second argument, `Eval` writes
  // its second and third, and the call sites look like any other.
  const Program program = build(
      "// str\n"
      "str head, tail;\n"
      "int own, ally;\n"
      "point g;\n"
      "head = Split(\"alpha beta gamma\", tail);\n"
      "own = 1;\n"
      "ally = 2;\n"
      "g.Eval(own, ally);\n"
      "return head + \"|\" + tail + \"|\" + own + \"|\" + ally;\n",
      "out.vs", runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  REQUIRE(execution.result.is_string());
  CHECK(execution.result.as_string() == "alpha|beta gamma|11|22");
}

TEST(vs_host_objects_carry_value_and_reference_semantics) {
  Runner runner;
  // `point` is a value type wearing a handle, so `b = a` must copy: mutating
  // `b` afterwards must leave `a` alone.
  const Program program = build(
      "// int\n"
      "point a, b;\n"
      "a = Point(3, 4);\n"
      "b = a;\n"
      "b.Scale(10);\n"
      "return a.x * 1000 + b.x;\n",
      "points.vs", runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 3030);
}

TEST(vs_host_owns_arithmetic_on_its_own_types) {
  Runner runner;
  const Program program = build(
      "// int\n"
      "point a, b, c;\n"
      "a = Point(10, 0);\n"
      "b = Point(4, 0);\n"
      "c = (a + b) * 2 / 4;\n"
      "return c.x;\n",
      "pointmath.vs", runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 7);
}

TEST(vs_subscripts_and_globals_reach_the_host) {
  Runner runner;
  const Program program = build(
      "// int\n"
      "ObjList ol;\n"
      "int n, i;\n"
      "n = 0;\n"
      "for (i = 0; i < ol.count; i += 1) n = n + 1;\n"
      "return n * 1000 + AI_COMING + AI_STAYING + Carthage;\n",
      "list.vs", runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 3007);
}

TEST(vs_an_expression_only_script_yields_its_value) {
  Runner runner;
  // The second entry mode: `DATA\SCDEBUG.XML` holds `.AsUnit.level`, a bare
  // expression with no `return` and no `;`. The host reads its value, so the
  // trailing expression must be returned rather than discarded.
  const Program program = build("40 + 2", "watch", runner.registry);
  REQUIRE(program.ok);
  CHECK(program.chunk.is_expression_only);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 42);
}

TEST(vs_a_chunk_disassembles_legibly) {
  Runner runner;
  const Program program = build("// void\nint i;\ni = 1;\nNote(i);\n", "dis.vs",
                                runner.registry);
  REQUIRE(program.ok);

  const std::string listing = disassemble(program.chunk);
  CHECK(listing.find("dis.vs") == 0);
  CHECK(listing.find("declare_local i") != std::string::npos);
  CHECK(listing.find("store_local i") != std::string::npos);
  CHECK(listing.find("call Note/1 writeback:1") != std::string::npos);
}

TEST(vs_a_bare_name_can_be_a_zero_argument_call) {
  Runner runner;
  // `rcMap = GetMapRect;` in DEER_IDLE.VS is a call, not a constant read, and
  // `GetTime`, `AIGetPlayer`, `GAIKACount`, `MapSize`, `MaxSetIdx` and
  // `Breakpoint` are each written both ways across the corpus. Seven of the 245
  // "globals" in the inventory are functions for exactly this reason, so a bare
  // name resolves local first, then zero-argument host function, then constant.
  runner.registry.define(CallKind::free_function, "GetTime", 0, [](CallContext&) {
    return HostOutcome::ok_with(Value::integer(4242));
  });
  const Program program = build(
      "// int\n"
      "int a, b;\n"
      "a = GetTime;\n"
      "b = GetTime();\n"
      "return a + b + AI_COMING;\n",
      "bare.vs", runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 8485);
}

TEST(vs_a_subscript_is_an_assignment_target) {
  Runner runner;
  // A subscript is one of only two assignable forms -- 197 sites across the
  // corpus. A member never is: all mutation goes through `SetXxx` methods.
  const Program program = build(
      "// int\n"
      "IntArray skills;\n"
      "int i, total;\n"
      "for (i = 0; i < skills.size; i += 1) skills[i] = i * 3;\n"
      "total = 0;\n"
      "for (i = 0; i < skills.size; i += 1) total = total + skills[i];\n"
      "return total;\n",
      "array.vs", runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 18);
}

TEST(vs_assigning_to_a_member_is_a_compile_error) {
  Runner runner;
  const Program program = build("// void\npoint pt;\npt.x = 3;\n", "member.vs",
                                runner.registry);
  CHECK(!program.ok);
  CHECK(program.error.find("neither a name nor a subscript") != std::string::npos);
}

TEST(vs_an_unknown_global_traps_by_name) {
  Runner runner;
  const Program program = build("// int\nreturn NoSuchConstant;\n", "global.vs",
                                runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::failed);
  CHECK(execution.trap.code == TrapCode::unknown_global);
  CHECK(execution.trap.detail.find("NoSuchConstant") != std::string::npos);
}

// -- the failure mode that matters -------------------------------------------

TEST(vs_an_unimplemented_host_call_fails_identifiably) {
  Runner runner;
  // Declared in the shipped inventory, nobody has written it. The trap has to
  // name it: silently returning zero would turn a missing host function into a
  // behavioural divergence a thousand ticks downstream.
  const Program program = build("// void\nSetPlayerStatus(1, 2, 3);\n", "unimpl.vs",
                                runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::failed);
  CHECK(execution.trap.code == TrapCode::host_not_implemented);
  CHECK(execution.trap.detail.find("SetPlayerStatus/3") != std::string::npos);
  CHECK(describe(execution.trap).find("unimpl.vs:2") != std::string::npos);
}

TEST(vs_a_host_call_nobody_declared_fails_differently) {
  Runner runner;
  const Program program = build("// void\nNotAnEngineFunction(1);\n", "unknown.vs",
                                runner.registry);
  REQUIRE(program.ok);

  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, runner.env()) == ExecStatus::failed);
  CHECK(execution.trap.code == TrapCode::host_not_registered);
  CHECK(execution.trap.detail.find("NotAnEngineFunction/1") != std::string::npos);
}

TEST(vs_arity_and_receiver_are_part_of_the_dispatch_key) {
  Runner runner;
  // `Tick` exists with arity 0 only, and `count` is a member, not a free
  // function. Neither near miss may resolve.
  const Program wrong_arity = build("// void\nTick(1);\n", "arity.vs", runner.registry);
  REQUIRE(wrong_arity.ok);
  Execution execution = start(wrong_arity.chunk);
  REQUIRE(run(execution, wrong_arity.chunk, runner.env()) == ExecStatus::failed);
  CHECK(execution.trap.code == TrapCode::host_not_registered);

  const Program wrong_kind = build("// void\ncount();\n", "kind.vs", runner.registry);
  REQUIRE(wrong_kind.ok);
  Execution second = start(wrong_kind.chunk);
  REQUIRE(run(second, wrong_kind.chunk, runner.env()) == ExecStatus::failed);
  CHECK(second.trap.code == TrapCode::host_not_registered);
}

TEST(vs_member_lookup_is_case_insensitive_and_free_lookup_is_not) {
  HostRegistry registry;
  registry.define(CallKind::member, "GetGAIKA", 0, &host_tick);
  registry.define(CallKind::free_function, "GetTime", 0, &host_tick);

  // The corpus spells the same member `GetGAIKA` on `point` and `GetGaika` on
  // `Settlement`, so members are matched case-insensitively.
  CHECK(registry.find(CallKind::member, "GetGaika", 0) != kUnresolvedHost);
  CHECK(registry.find(CallKind::member, "getgaika", 0) != kUnresolvedHost);
  // Locals and free functions are case sensitive: OUTPOST_IDLE.VS keeps `This`
  // and `this` live at the same time.
  CHECK(registry.find(CallKind::free_function, "gettime", 0) == kUnresolvedHost);
  CHECK(registry.find(CallKind::free_function, "GetTime", 0) != kUnresolvedHost);
  CHECK(registry.find(CallKind::member, "GetGAIKA", 1) == kUnresolvedHost);
}

TEST(vs_the_shipped_surface_is_wired_even_though_it_is_not_written) {
  HostRegistry registry;
  declare_shipped_surface(registry);
  // 271 free-function entry points and 555 member entry points from the
  // inventory over all 885 shipped `.vs` files, less the four member pairs that
  // differ only in case (`GetGAIKA` / `GetGaika` and friends) and therefore
  // share one entry.
  //
  // It was 743 while the inventory covered only the 577 scripts in `data.pak`.
  // The 308 inside the 24 `.bfhp` containers add 66 free and 13 member entry
  // points that appear nowhere in the packs -- the campaign layer -- and
  // `host_surface.cpp` explains how that was proved, and against what control.
  // Plus one: `selu`, the console global one map reads as a bare name, which
  // the inventory counts as an identifier and the executable registers as a
  // zero-argument accessor.
  CHECK(registry.size() == 823);
  CHECK(registry.implemented() == 0);
  CHECK(registry.find(CallKind::free_function, "Sleep", 1) != kUnresolvedHost);
  CHECK(registry.find(CallKind::member, "IsValid", 0) != kUnresolvedHost);
  CHECK(registry.find(CallKind::member, "AddCommand", 3) != kUnresolvedHost);
  CHECK(shipped_global_names().size() == 245);

  // The runtime implements fourteen of them itself: `Sleep`, `AIBreakScript`,
  // `KillScript`, `StartPlayerScript`, `Run`, and `AIRun` in five free and five member
  // arities.
  register_scheduler_builtins(registry);
  CHECK(registry.implemented() == 15);
}

TEST(vs_a_runaway_loop_traps_instead_of_hanging) {
  Runner runner;
  const Program program = build("// void\nwhile (1) { }\n", "runaway.vs", runner.registry);
  REQUIRE(program.ok);

  VmEnv env = runner.env();
  env.instruction_budget = 1000;
  Execution execution = start(program.chunk);
  REQUIRE(run(execution, program.chunk, env) == ExecStatus::failed);
  CHECK(execution.trap.code == TrapCode::budget_exceeded);
}

// -- suspension --------------------------------------------------------------

TEST(vs_sleep_suspends_and_resumes_across_ticks) {
  Runner runner;
  const Program program = build(
      "// void\n"
      "int i;\n"
      "i = 0;\n"
      "while (i < 3) {\n"
      "  Tick();\n"
      "  Sleep(100);\n"
      "  i = i + 1;\n"
      "}\n",
      "sleeper.vs", runner.registry);
  REQUIRE(program.ok);

  VmEnv env = runner.env();
  Execution execution = start(program.chunk);

  CHECK(run(execution, program.chunk, env) == ExecStatus::suspended);
  CHECK(runner.world.ticks == 1);
  CHECK(execution.wake_time == 100);

  // Not yet: the wake time has not arrived, so nothing may advance.
  env.now = 50;
  execution.status = ExecStatus::suspended;
  CHECK(execution.wake_time > env.now);

  for (std::int64_t now = 100; now <= 300; now += 100) {
    env.now = now;
    execution.status = ExecStatus::ready;
    run(execution, program.chunk, env);
  }
  CHECK(runner.world.ticks == 3);
  CHECK(execution.status == ExecStatus::finished);
}

TEST(vs_a_retrying_host_call_re_runs_on_resume) {
  Runner runner;
  const Program program = build(
      "// void\n"
      "WaitForTicks(2);\n"
      "Tick();\n",
      "wait.vs", runner.registry);
  REQUIRE(program.ok);

  VmEnv env = runner.env();
  Execution execution = start(program.chunk);
  CHECK(run(execution, program.chunk, env) == ExecStatus::suspended);
  CHECK(runner.world.ticks == 0);

  runner.world.ticks = 2;
  env.now = 10;
  execution.status = ExecStatus::ready;
  CHECK(run(execution, program.chunk, env) == ExecStatus::finished);
  CHECK(runner.world.ticks == 3);
}

// -- serialisation -----------------------------------------------------------

TEST(vs_a_suspended_script_round_trips_through_serialisation) {
  Runner runner;
  // This is the property that makes saves and lockstep determinism possible, so
  // it is proven rather than asserted: suspend mid-script, save, reload, and
  // run both copies to the end. They must agree instruction for instruction.
  const Program program = build(
      "// int\n"
      "int i, total;\n"
      "str tag;\n"
      "point pt;\n"
      "total = 0;\n"
      "tag = \"deer\";\n"
      "pt = Point(3, 4);\n"
      "for (i = 0; i < 4; i += 1) {\n"
      "  total = total + i * 10;\n"
      "  Sleep(25);\n"
      "}\n"
      "return total + pt.x;\n",
      "roundtrip.vs", runner.registry);
  REQUIRE(program.ok);

  VmEnv env = runner.env();
  Execution original = start(program.chunk);
  CHECK(run(original, program.chunk, env) == ExecStatus::suspended);
  CHECK(original.frames.size() == 1);
  REQUIRE(!original.frames.empty());
  CHECK(original.frames[0].ip > 0);

  std::vector<std::byte> saved;
  serialize(original, saved);
  Result<Execution> reloaded = deserialize(saved);
  REQUIRE(reloaded.ok());

  // The reloaded state must be byte-identical before either side runs on.
  std::vector<std::byte> resaved;
  serialize(reloaded.value(), resaved);
  CHECK(saved == resaved);
  REQUIRE(!reloaded.value().frames.empty());
  CHECK(reloaded.value().frames[0].ip == original.frames[0].ip);
  CHECK(reloaded.value().wake_time == original.wake_time);

  // Now run both to completion, in lockstep, and compare at every suspension.
  Execution restored = std::move(reloaded.value());
  for (std::int64_t now = 25; now <= 200; now += 25) {
    env.now = now;
    if (original.status == ExecStatus::suspended) original.status = ExecStatus::ready;
    if (restored.status == ExecStatus::suspended) restored.status = ExecStatus::ready;
    run(original, program.chunk, env);
    run(restored, program.chunk, env);

    std::vector<std::byte> a;
    std::vector<std::byte> b;
    serialize(original, a);
    serialize(restored, b);
    CHECK(a == b);
  }
  CHECK(original.status == ExecStatus::finished);
  CHECK(restored.status == ExecStatus::finished);
  CHECK(original.result.as_integer() == 63);
  CHECK(restored.result.as_integer() == 63);
}

TEST(vs_deserialising_rubbish_is_an_error_not_a_crash) {
  const std::vector<std::byte> empty;
  CHECK(!deserialize(empty).ok());

  std::vector<std::byte> wrong(32, std::byte{0x7F});
  CHECK(deserialize(wrong).error() == FormatError::bad_magic);

  Chunk chunk;
  chunk.source_name = "x.vs";
  std::vector<std::byte> saved;
  serialize(start(chunk), saved);
  saved.resize(saved.size() - 1);
  CHECK(!deserialize(saved).ok());
}

/// An object value's second word survives a save: a VS point is two 32-bit
/// integers, `x` in `id` and `y` in `aux`, and a save that dropped `aux` would
/// put every point a suspended script holds on the line y = 0.
TEST(vs_an_object_value_round_trips_both_of_its_words) {
  const Value values[] = {
      Value::object(ObjectRef{2, static_cast<std::uint32_t>(-70000), 71234u}),
      Value::object(ObjectRef{1, 42u}),
      Value::object(ObjectRef{}),
  };
  for (const Value& value : values) {
    std::vector<std::byte> bytes;
    write_value(bytes, value);
    ByteReader reader(bytes);
    Value back;
    REQUIRE(read_value(reader, back));
    CHECK(back == value);
    CHECK(back.as_object().aux == value.as_object().aux);
    CHECK(reader.remaining() == 0);
  }
}

// -- the scheduler -----------------------------------------------------------

namespace {

/// A scheduler with a world, a host and a library, wired the way an embedder
/// would wire it.
struct Stage {
  World world;
  TestHost host{world};
  HostRegistry registry = make_registry();
  Scheduler scheduler;
  std::vector<Program> programs;

  Stage() {
    scheduler.set_registry(&registry);
    scheduler.set_host(&host);
    scheduler.set_user(&world);
  }

  bool add(std::string_view source, const char* name) {
    programs.push_back(build(source, name, registry));
    if (!programs.back().ok) return false;
    scheduler.add_chunk(programs.back().chunk);
    return true;
  }
};

}  // namespace

TEST(vs_the_scheduler_runs_many_scripts_in_id_order) {
  Stage stage;
  REQUIRE(stage.add("// void, int n\nNote(n);\nSleep(10);\nNote(n + 100);\n", "note.vs"));

  for (int i = 1; i <= 3; ++i) {
    const Value argument = Value::integer(i);
    CHECK(stage.scheduler.spawn(0, std::span<const Value>(&argument, 1)) ==
          static_cast<ScriptId>(i));
  }

  stage.scheduler.run_ready();
  REQUIRE(stage.world.log.size() == 3);
  CHECK(stage.world.log[0] == "1");
  CHECK(stage.world.log[1] == "2");
  CHECK(stage.world.log[2] == "3");

  const RunReport report = stage.scheduler.advance(10);
  CHECK(report.resumed == 3);
  CHECK(report.completed == 3);
  CHECK(report.failed == 0);
  REQUIRE(stage.world.log.size() == 6);
  CHECK(stage.world.log[3] == "101");
  CHECK(stage.world.log[5] == "103");
  CHECK(stage.scheduler.live_count() == 0);
}

TEST(vs_airun_spawns_a_peer_and_aibreakscript_kills_it) {
  Stage stage;
  // The child is the standard behaviour shape: an infinite loop around a sleep.
  // 101 shipped scripts look exactly like this.
  REQUIRE(stage.add("// void\nwhile (1) {\n  Tick();\n  Sleep(10);\n}\n", "child.vs"));
  REQUIRE(stage.add(
      "// void\n"
      "int handle;\n"
      "handle = AIRun(\"child.vs\");\n"
      "Note(handle);\n"
      "Sleep(35);\n"
      "AIBreakScript(handle);\n",
      "parent.vs"));

  const ScriptId parent = stage.scheduler.spawn(1);
  CHECK(parent == 1);

  // The child is spawned during the parent's first slice and runs in the same
  // pass, because ids only increase and the walk is over a growing vector.
  stage.scheduler.run_ready();
  REQUIRE(stage.world.log.size() == 1);
  CHECK(stage.world.log[0] == "2");
  CHECK(stage.scheduler.alive(2));
  CHECK(stage.world.ticks == 1);

  for (int i = 0; i < 3; ++i) stage.scheduler.advance(10);
  CHECK(stage.world.ticks == 4);

  // At t = 35 the parent wakes and kills the child.
  stage.scheduler.advance(5);
  CHECK(!stage.scheduler.alive(2));
  CHECK(!stage.scheduler.alive(1));
  CHECK(stage.scheduler.live_count() == 0);

  const int ticks_at_death = stage.world.ticks;
  for (int i = 0; i < 5; ++i) stage.scheduler.advance(10);
  CHECK(stage.world.ticks == ticks_at_death);
}

TEST(vs_a_missing_script_is_survivable) {
  Stage stage;
  // Four `<behavior script=...>` bindings in the shipped class tree name files
  // that are not in the pack at all, so this must not be fatal.
  REQUIRE(stage.add("// void\nint handle;\nhandle = AIRun(\"not_shipped.vs\");\nNote(handle);\n",
                    "orphan.vs"));
  stage.scheduler.spawn(0);
  const RunReport report = stage.scheduler.run_ready();
  CHECK(report.failed == 0);
  REQUIRE(stage.world.log.size() == 1);
  CHECK(stage.world.log[0] == "0");
}

/// `Run(name)` is `AIRun` without a handle: the child runs, the caller gets
/// nothing back, and a name with no script behind it is survivable.
TEST(vs_run_spawns_a_peer_and_answers_nothing) {
  Stage stage;
  // The child declares a parameter so that a `Run` which forwarded its own
  // arguments -- the name -- would be seen binding a string to it.
  REQUIRE(stage.add("// void, int n\nNote(n);\nTick();\n", "DATA\\SUBAI\\CHILD.VS"));
  REQUIRE(stage.add("// void\nRun(\"data/subai/child.vs\");\nRun(\"data/subai/nonesuch.vs\");\nTick();\n",
                    "DBGRUNSCRIPT.VS"));
  stage.scheduler.spawn(1);
  const RunReport report = stage.scheduler.run_ready();
  CHECK(report.failed == 0);
  // The child ran in the same pass with nothing bound, and the missing one
  // cost nothing.
  CHECK(stage.world.ticks == 2);
  REQUIRE(stage.world.log.size() == 1);
  CHECK(stage.world.log[0] == "0");
  CHECK(stage.scheduler.live_count() == 0);

  // And it is `void`: the handle `AIRun` answers is not returned here.
  const std::uint32_t index = stage.registry.find(CallKind::free_function, "Run", 1);
  REQUIRE(index != kUnresolvedHost);
  std::vector<Value> args = {Value::string("data/subai/child.vs")};
  CallContext ctx;
  ctx.arguments = args;
  ctx.scheduler = &stage.scheduler;
  const HostOutcome direct = stage.registry.entry(index).fn(ctx);
  CHECK(direct.status == HostStatus::ok);
  CHECK(direct.value.is_nil());
}

TEST(vs_a_script_name_matches_however_it_is_spelled) {
  Stage stage;
  REQUIRE(stage.add("// void\nTick();\n", "DATA\\SUBAI\\DEER_IDLE.VS"));
  CHECK(stage.scheduler.find_chunk("DATA\\SUBAI\\DEER_IDLE.VS") == 0);
  CHECK(stage.scheduler.find_chunk("data/subai/deer_idle.vs") == 0);
  CHECK(stage.scheduler.find_chunk("Deer_Idle.vs") == 0);
  CHECK(stage.scheduler.find_chunk("wolf_idle.vs") == kNoChunk);
}

TEST(vs_a_trapping_script_is_reported_and_removed) {
  Stage stage;
  REQUIRE(stage.add("// void\nTick();\nSetPlayerStatus(1, 2, 3);\n", "bad.vs"));
  REQUIRE(stage.add("// void\nTick();\n", "good.vs"));
  stage.scheduler.spawn(0);
  stage.scheduler.spawn(1);

  const RunReport report = stage.scheduler.run_ready();
  CHECK(report.resumed == 2);
  CHECK(report.failed == 1);
  CHECK(report.completed == 1);
  REQUIRE(report.traps.size() == 1);
  CHECK(report.traps[0].trap.code == TrapCode::host_not_implemented);
  CHECK(report.traps[0].source_name == "bad.vs");
  // The good one still ran: one bad script does not take the tick down.
  CHECK(stage.world.ticks == 2);
  CHECK(stage.scheduler.live_count() == 0);
}

TEST(vs_the_whole_scheduler_round_trips_through_a_save) {
  Stage stage;
  REQUIRE(stage.add("// void, int n\nwhile (1) {\n  Note(n);\n  Sleep(20);\n}\n", "loop.vs"));

  for (int i = 1; i <= 2; ++i) {
    const Value argument = Value::integer(i * 7);
    stage.scheduler.spawn(0, std::span<const Value>(&argument, 1), ObjectRef{kUnit, 1937u});
  }
  stage.scheduler.advance(20);
  stage.scheduler.advance(20);
  const std::size_t before = stage.world.log.size();
  CHECK(before == 4);

  std::vector<std::byte> saved;
  stage.scheduler.serialize(saved);

  // A fresh scheduler with the same library, as a load would build it.
  Stage reloaded;
  REQUIRE(reloaded.add("// void, int n\nwhile (1) {\n  Note(n);\n  Sleep(20);\n}\n", "loop.vs"));
  REQUIRE(reloaded.scheduler.deserialize(saved).ok());
  CHECK(reloaded.scheduler.now() == stage.scheduler.now());
  CHECK(reloaded.scheduler.live_count() == 2);
  REQUIRE(reloaded.scheduler.scripts().size() == 2);
  CHECK(reloaded.scheduler.scripts()[0].owner.id == 1937u);
  CHECK(reloaded.scheduler.scripts()[0].owner.type == kUnit);

  // Both run on from the same point and produce the same thing.
  stage.scheduler.advance(20);
  reloaded.scheduler.advance(20);
  REQUIRE(stage.world.log.size() == before + 2);
  REQUIRE(reloaded.world.log.size() == 2);
  CHECK(reloaded.world.log[0] == stage.world.log[before]);
  CHECK(reloaded.world.log[1] == stage.world.log[before + 1]);

  std::vector<std::byte> a;
  std::vector<std::byte> b;
  stage.scheduler.serialize(a);
  reloaded.scheduler.serialize(b);
  CHECK(a == b);
}

TEST(vs_a_serialised_coroutine_does_not_carry_its_librarys_chunk_index) {
  // A chunk index means something only inside the session that built the
  // library, and two sessions of one save compile in different orders -- the
  // load compiles the files the save names, in the order it names them, while
  // the session that wrote it compiled them in the order it happened to need
  // them. Writing the index into the payload therefore made a save's *bytes*
  // depend on that order, and four of the shipped conquest's seven maps failed
  // the byte-identical re-save check on this field alone, off by one.
  //
  // Nothing ever read it: `Scheduler::deserialize` sets it from the record's
  // source name before anything can, and a bare `run` ignores it.
  Stage stage;
  REQUIRE(stage.add("// void\nTick();\nSleep(10);\nTick();\n", "a.vs"));
  REQUIRE(stage.add("// void\nTick();\nSleep(10);\nTick();\n", "b.vs"));

  // The same coroutine, at the same point, differing only in which slot of the
  // library its code sits in.
  Execution first = start(stage.scheduler.chunk(0));
  first.chunk_index = 0;
  Execution second = first;
  second.chunk_index = 1;

  std::vector<std::byte> a;
  std::vector<std::byte> b;
  serialize(first, a);
  serialize(second, b);
  CHECK(a == b);

  // And a restored one comes back with the field at its default, for whoever
  // restores it to bind.
  const Result<Execution> back = deserialize(a);
  REQUIRE(back.ok());
  CHECK(back.value().chunk_index == 0);
}

TEST(vs_a_save_names_every_file_it_needs_before_anything_is_bound) {
  // `deserialize` rebinds by source name and refuses the whole load when one
  // misses, so whoever builds the library has to compile those files first --
  // and the only list that cannot be short is the one the writer wrote.
  // Enumerating the ways a script can start was how this used to be done, and
  // it missed the class `<method>` a command launches for as long as
  // `CommandSystem` has had a library to compile through.
  Stage stage;
  REQUIRE(stage.add("// void, int n\nwhile (1) {\n  Note(n);\n  Sleep(20);\n}\n", "loop.vs"));
  REQUIRE(stage.add("// void\nwhile (1) {\n  Tick();\n  Sleep(30);\n}\n", "tick.vs"));

  const Value one = Value::integer(1);
  stage.scheduler.spawn(0, std::span<const Value>(&one, 1));
  stage.scheduler.spawn(1);
  const Value two = Value::integer(2);
  stage.scheduler.spawn(0, std::span<const Value>(&two, 1));
  stage.scheduler.advance(20);

  std::vector<std::byte> saved;
  stage.scheduler.serialize(saved);

  const Result<std::vector<std::string>> names = script_section_sources(saved);
  REQUIRE(names.ok());
  // Record order, and duplicates kept: two coroutines of one file name it
  // twice, which a caller compiling by path memoises away.
  REQUIRE(names.value().size() == 3);
  CHECK(names.value()[0] == "loop.vs");
  CHECK(names.value()[1] == "tick.vs");
  CHECK(names.value()[2] == "loop.vs");

  // And the list is exactly what a rebuilt library needs: a scheduler holding
  // only those files binds the save, and one missing either does not.
  Stage rebuilt;
  for (const std::string& name : names.value()) {
    if (rebuilt.scheduler.find_chunk(name) != kNoChunk) continue;
    REQUIRE(rebuilt.add(name == "loop.vs"
                            ? "// void, int n\nwhile (1) {\n  Note(n);\n  Sleep(20);\n}\n"
                            : "// void\nwhile (1) {\n  Tick();\n  Sleep(30);\n}\n",
                        name.c_str()));
  }
  CHECK(rebuilt.scheduler.deserialize(saved).ok());

  Stage short_of_one;
  REQUIRE(short_of_one.add("// void, int n\nwhile (1) {\n  Note(n);\n  Sleep(20);\n}\n",
                           "loop.vs"));
  const Status refused = short_of_one.scheduler.deserialize(saved);
  CHECK(!refused.ok());
}

TEST(vs_the_source_name_reader_refuses_what_is_not_a_script_section) {
  // It reads the same header `deserialize` does and stops at the same three
  // places, so a caller cannot be handed names out of somebody else's section.
  std::vector<std::byte> empty;
  CHECK(!script_section_sources(empty).ok());

  Stage stage;
  REQUIRE(stage.add("// void\nTick();\n", "one.vs"));
  stage.scheduler.spawn(0);
  std::vector<std::byte> saved;
  stage.scheduler.serialize(saved);
  REQUIRE(script_section_sources(saved).ok());

  std::vector<std::byte> bad_magic = saved;
  bad_magic[0] = std::byte{0x00};
  CHECK(!script_section_sources(bad_magic).ok());

  // Cut inside the first record's name.
  std::vector<std::byte> truncated(saved.begin(), saved.begin() + 24);
  CHECK(!script_section_sources(truncated).ok());
}

// -- a behaviour script, end to end ------------------------------------------

TEST(vs_a_wildlife_behaviour_runs_end_to_end) {
  // Shaped like `DATA\SUBAI\DEER_IDLE.VS`, which is what wild deer run: narrow
  // the `Obj` parameter into a `this`, pick a destination, reject it and sleep
  // until one passes, then walk. Written here rather than copied, because the
  // repository carries no game data.
  Stage stage;
  REQUIRE(stage.add(
      "// void, int me\n"
      "point pt, ptCenter;\n"
      "ObjList ol;\n"
      "int i, tries;\n"
      "\n"
      "pt = Point(1, 1);\n"
      "tries = 0;\n"
      "while (tries < 3)\n"
      "{\n"
      "  pt.Scale(2);\n"
      "  tries = tries + 1;\n"
      "  Sleep(1000);\n"
      "}\n"
      "Tick();\n"
      "ptCenter = Point(0, 0);\n"
      "for (i = 0; i < ol.count; i += 1)\n"
      "{\n"
      "  ptCenter = ptCenter + ol[i].Pos;\n"
      "}\n"
      "Note(\"walking \" + pt.x + \" herd \" + ptCenter.x);\n",
      "deer_like_idle.vs"));

  // `Pos` on a list element: every unit in this world sits at (2, 0).
  stage.registry.define(CallKind::member, "Pos", 0, [](CallContext& context) {
    World& world = world_of(context);
    return HostOutcome::ok_with(Value::object(kPoint, world.make_point(2, 0)));
  });

  const Value me = Value::integer(1937);
  const ScriptId deer =
      stage.scheduler.spawn(0, std::span<const Value>(&me, 1), ObjectRef{kUnit, 1937u});
  REQUIRE(deer != kNoScript);

  stage.scheduler.run_ready();
  CHECK(stage.scheduler.alive(deer));
  CHECK(stage.world.ticks == 0);  // still inside the sleep loop

  for (int i = 0; i < 3; ++i) stage.scheduler.advance(1000);
  CHECK(stage.world.ticks == 1);
  CHECK(stage.scheduler.live_count() == 0);
  REQUIRE(stage.world.log.size() == 1);
  CHECK(stage.world.log[0] == "walking 8 herd 6");
}

// A nested call must not leak its writebacks into the enclosing call's range.
//
// Regression test for a compiler bug that survived every synthetic test here
// and only surfaced when a real host function looked at its receiver.
// `Compiler::call` reserved `writeback_begin` before compiling sub-expressions,
// so an argument that was itself a call appended its writebacks inside the
// outer call's `[begin, begin + count)` window. The VM then copied the inner
// call's values back into the outer call's locals.
//
// In `deer_idle.vs` this rewrote `this` with a query handle, and every later
// `.Goto(...)` ran against a query instead of a unit. Stubs ignore their
// receiver, so the corpus compiled and ran clean for as long as nothing
// checked.
TEST(vs_nested_call_writebacks_do_not_leak_into_the_outer_call) {
  // `outer` takes the result of `inner`, and both have lvalue arguments that
  // are eligible for writeback.
  const char* source =
      "// int, int a, int b\n"
      "int a, b;\n"
      "a = 1;\n"
      "b = 2;\n"
      "outer(a, inner(b));\n"
      "return a;\n";

  imperivm::core::script::Diagnostic diagnostic;
  const auto parsed = imperivm::core::script::parse(
      std::as_bytes(std::span{source, std::strlen(source)}), "nested.vs", &diagnostic);
  REQUIRE(parsed.ok());

  const auto compiled = imperivm::core::script::compile(*parsed);
  REQUIRE(compiled.ok());
  const imperivm::core::script::Chunk& chunk = *compiled;

  // Every call site's writeback window must lie inside the table and must not
  // overlap any other site's. Overlap is precisely the bug.
  for (std::size_t i = 0; i < chunk.call_sites.size(); ++i) {
    const auto& a = chunk.call_sites[i];
    CHECK(a.writeback_begin + a.writeback_count <= chunk.writebacks.size());
    for (std::size_t j = i + 1; j < chunk.call_sites.size(); ++j) {
      const auto& b = chunk.call_sites[j];
      const bool disjoint = a.writeback_begin + a.writeback_count <= b.writeback_begin ||
                            b.writeback_begin + b.writeback_count <= a.writeback_begin;
      CHECK(disjoint);
    }
  }

  // The inner call is compiled first, so its writebacks must come first too.
  REQUIRE(chunk.call_sites.size() >= 2);
  CHECK(chunk.call_sites[0].writeback_begin < chunk.call_sites[1].writeback_begin ||
        chunk.call_sites[0].writeback_count == 0);
}

// -- the blocking primitive --------------------------------------------------
//
// `HostStatus::retry` suspends *at* a call and runs the whole call again on
// resume. That is what the eleven `Wait*` entry points are built on, and on its
// own it has no memory: these cases are about the three fields on `Execution`
// that give a retrying call one, and about the ways a wrong reading of them
// would break every timeout in the game rather than one of them.

TEST(vs_a_blocking_call_is_told_how_long_it_has_been_waiting) {
  Runner runner;
  const Program program = build(
      "// int\n"
      "bool ok;\n"
      "ok = WaitUntilTicks(2, 1000);\n"
      "return ok;\n",
      "waitfor.vs", runner.registry);
  REQUIRE(program.ok);

  VmEnv env = runner.env();
  Execution execution = start(program.chunk);

  // First run: the predicate is tested with **zero elapsed**, before anything
  // suspends. That is what makes `if (WaitQueryCountBetween(q, 1, 60, 100))`
  // work at all in the shipped scripts -- a 100 ms timeout inside an 800 ms
  // turn would otherwise never get an honest test.
  env.now = 500;
  CHECK(run(execution, program.chunk, env) == ExecStatus::suspended);
  CHECK(runner.world.wait_polls == 1);
  CHECK(runner.world.last_elapsed == 0);

  // And on each resume it grows, because `retry_since` is written once and not
  // on every poll. Written every time, elapsed would be zero forever and no
  // wait in the game would ever time out.
  env.now = 700;
  execution.status = ExecStatus::ready;
  CHECK(run(execution, program.chunk, env) == ExecStatus::suspended);
  CHECK(runner.world.last_elapsed == 200);

  env.now = 900;
  execution.status = ExecStatus::ready;
  CHECK(run(execution, program.chunk, env) == ExecStatus::suspended);
  CHECK(runner.world.last_elapsed == 400);

  // The predicate finally holds, and the call produces its bool.
  runner.world.ticks = 2;
  env.now = 1000;
  execution.status = ExecStatus::ready;
  CHECK(run(execution, program.chunk, env) == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 1);
}

TEST(vs_a_blocking_call_gives_up_when_its_deadline_passes) {
  Runner runner;
  const Program program = build(
      "// int\n"
      "bool ok;\n"
      "ok = WaitUntilTicks(9, 300);\n"
      "return ok;\n",
      "timeout.vs", runner.registry);
  REQUIRE(program.ok);

  VmEnv env = runner.env();
  Execution execution = start(program.chunk);
  env.now = 0;
  CHECK(run(execution, program.chunk, env) == ExecStatus::suspended);

  env.now = 299;  // one short of the deadline
  execution.status = ExecStatus::ready;
  CHECK(run(execution, program.chunk, env) == ExecStatus::suspended);

  env.now = 300;  // and now it is reached
  execution.status = ExecStatus::ready;
  CHECK(run(execution, program.chunk, env) == ExecStatus::finished);
  // False, not a trap: a timed-out wait is an answer, and 161 of the 208
  // `WaitQueryCountBetween` sites in the corpus read it.
  CHECK(execution.result.as_integer() == 0);
  CHECK(runner.world.ticks == 0);  // the predicate never held
}

TEST(vs_the_same_call_site_reached_again_starts_a_new_wait) {
  // The fault this case exists for: `retry_ip` identifies a call by its
  // instruction pointer, and a loop reaches the same instruction on every
  // iteration. If the second iteration were mistaken for a continuation of the
  // first, its timeout would already be spent and it would give up
  // immediately -- so a `while` loop polling a condition would poll it once and
  // then fall through forever after.
  Runner runner;
  const Program program = build(
      "// int\n"
      "int i, misses;\n"
      "misses = 0;\n"
      "for (i = 0; i < 3; i += 1) {\n"
      "  if (!WaitUntilTicks(9, 100)) { misses = misses + 1; }\n"
      "}\n"
      "return misses;\n",
      "loopwait.vs", runner.registry);
  REQUIRE(program.ok);

  VmEnv env = runner.env();
  Execution execution = start(program.chunk);
  env.now = 0;
  // Drive it to completion, advancing the clock the way a scheduler would.
  for (int step = 0; step < 60 && execution.status != ExecStatus::finished; ++step) {
    env.now += 60;
    execution.status = ExecStatus::ready;
    run(execution, program.chunk, env);
  }
  REQUIRE(execution.status == ExecStatus::finished);
  // All three iterations timed out, which is only possible if each got its own
  // 100 ms. Sharing one deadline would give 1.
  CHECK(execution.result.as_integer() == 3);
}

TEST(vs_a_script_waiting_on_a_deadline_round_trips_through_a_save) {
  // A suspended script is world state, and a script suspended *inside* a
  // blocking call is suspended at the call with its arguments still on the
  // frame stack -- so the condition itself needs no save format at all. What
  // does need saving is how long it has been waiting: drop that and every
  // pending timeout in the game silently restarts at zero on load, which is a
  // divergence no hash of the object table would ever see.
  Runner runner;
  const Program program = build(
      "// int\n"
      "bool ok;\n"
      "ok = WaitUntilTicks(9, 300);\n"
      "return ok;\n",
      "savewait.vs", runner.registry);
  REQUIRE(program.ok);

  VmEnv env = runner.env();
  Execution original = start(program.chunk);
  env.now = 0;
  CHECK(run(original, program.chunk, env) == ExecStatus::suspended);
  env.now = 200;
  original.status = ExecStatus::ready;
  CHECK(run(original, program.chunk, env) == ExecStatus::suspended);
  REQUIRE(original.retrying);
  CHECK(original.retry_since == 0);

  std::vector<std::byte> bytes;
  serialize(original, bytes);
  const Result<Execution> restored = deserialize(bytes);
  REQUIRE(restored.ok());
  Execution copy = restored.value();
  CHECK(copy.retrying == original.retrying);
  CHECK(copy.retry_since == original.retry_since);

  // And it gives up at the same moment the original would have: 300 from when
  // the wait began, not 300 from the load.
  env.now = 300;
  copy.status = ExecStatus::ready;
  CHECK(run(copy, program.chunk, env) == ExecStatus::finished);
  CHECK(copy.result.as_integer() == 0);
  CHECK(runner.world.last_elapsed == 300);
}

TEST(vs_a_second_wait_does_not_inherit_the_first_ones_stopwatch) {
  // The fault that makes the clear load-bearing, and the reason the loop case
  // above cannot stand in for it: that one reaches the *same* call site each
  // iteration, so a stale sequence and a fresh one give the same answer. Two
  // *different* waits tell them apart. Without the clear, the second one is
  // handed the first one's start time, computes an elapsed of 600-odd ms
  // against its own 300 ms budget, and gives up on its very first poll --
  // having tested its predicate once and never waited at all.
  //
  // Asserted on the *count of zeros* in the elapsed log rather than on the last
  // value: every fresh wait reports zero elapsed to its first poll, so two
  // waits that each got their own stopwatch produce exactly two zeros, and one
  // that inherited produces one.
  Runner runner;
  const Program program = build(
      "// int\n"
      "bool a, b;\n"
      "a = WaitUntilTicks(9, 100);\n"
      "Sleep(500);\n"
      "b = WaitUntilTicks(9, 300);\n"
      "return b;\n",
      "twowaits.vs", runner.registry);
  REQUIRE(program.ok);

  VmEnv env = runner.env();
  Execution execution = start(program.chunk);
  env.now = 0;
  // Driven the way `Scheduler::run_ready` drives it -- a suspended script is
  // only resumed once game time reaches its wake time -- because the `Sleep` in
  // the middle is what puts real distance between the two waits.
  for (int step = 0; step < 60 && execution.status != ExecStatus::finished; ++step) {
    env.now += 100;
    if (execution.status == ExecStatus::suspended && execution.wake_time > env.now) continue;
    execution.status = ExecStatus::ready;
    run(execution, program.chunk, env);
  }
  REQUIRE(execution.status == ExecStatus::finished);
  CHECK(execution.result.as_integer() == 0);  // the second wait timed out too

  const std::size_t zeros = static_cast<std::size_t>(
      std::count(runner.world.elapsed_log.begin(), runner.world.elapsed_log.end(), 0));
  CHECK(zeros == 2);
  // The preconditions: both waits really did poll more than once, and the clock
  // really had moved well past the second wait's budget by the time it started.
  CHECK(runner.world.elapsed_log.size() > 4);
  CHECK(env.now > 600);
}
