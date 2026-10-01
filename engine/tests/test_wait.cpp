// The `Wait*` family: sim/wait.hpp.
//
// Two things are under test and they are not the same thing. The *arithmetic*
// -- `wait_slice` -- decides when a wait polls and when it gives up, and it is
// a pure function that can be checked exhaustively. The *entry points* decide
// what each one is waiting for, and each of those is one comparison read out of
// `gbr.exe`. The runtime machinery underneath both is tested in
// `test_vs_vm.cpp`, because it belongs to the VM and not to this domain.

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/globals.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/wait.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "test.hpp"

namespace {

using namespace imperivm::core;
using namespace imperivm::core::sim;
using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::HostRegistry;
using script::HostStatus;
using script::Value;

struct HostCall {
  std::vector<Value> arguments;
  CallContext context;
  HostContext context_state;

  HostCall(World& world, std::initializer_list<Value> args, std::int64_t now = 0,
           std::int64_t since = 0)
      : arguments(args) {
    context_state.world = &world;
    context.arguments = arguments;
    context.user = &context_state;
    context.now = now;
    context.waiting_since = since;
    context.first_call = now == since;
  }
};

HostOutcome invoke(const HostRegistry& registry, std::string_view name, std::uint16_t arity,
                   HostCall& call) {
  const std::uint32_t index = registry.find(CallKind::free_function, name, arity);
  if (index == script::kUnresolvedHost) return HostOutcome::failed("not registered");
  const script::HostEntry& entry = registry.entry(index);
  if (entry.fn == nullptr) return HostOutcome::failed("not implemented");
  return entry.fn(call.context);
}

HostRegistry wait_registry() {
  HostRegistry registry;
  script::declare_shipped_surface(registry);
  (void)register_wait_host(registry);
  return registry;
}

Value str(std::string_view text) { return Value::string(std::string(text)); }
Value query_value(ObjectId id) {
  return Value::object(script::ObjectRef{kTypeQuery, static_cast<std::uint32_t>(id)});
}

/// True when the outcome is a poll rather than an answer.
[[nodiscard]] bool polls(const HostOutcome& outcome) {
  return outcome.status == HostStatus::retry;
}
[[nodiscard]] bool answered(const HostOutcome& outcome, bool value) {
  return outcome.status == HostStatus::ok && outcome.value.is_integer() &&
         (outcome.value.as_integer() != 0) == value;
}

}  // namespace

// --------------------------------------------------------------------------
// the arithmetic
// --------------------------------------------------------------------------

TEST(a_finite_wait_polls_on_the_interval_and_lands_on_its_deadline) {
  // `slice = min(interval, remaining)`, which is what makes the **last** poll
  // land exactly on the deadline instead of overshooting it. A wait that always
  // slept a full interval would test a 250 ms timeout at 0, 100 and 200 and then
  // give up at 300 -- fifty milliseconds late, and never having tested at 250.
  CHECK(wait_slice(0, 250, 100) == 100);
  CHECK(wait_slice(100, 250, 100) == 100);
  CHECK(wait_slice(200, 250, 100) == 50);
  CHECK(wait_slice(250, 250, 100) == 0);
  // And past the deadline it stays over, however far past.
  CHECK(wait_slice(400, 250, 100) == 0);

  // An exact multiple: the deadline is reached by a whole slice, not by a
  // remainder of zero handed out as a poll.
  CHECK(wait_slice(0, 200, 100) == 100);
  CHECK(wait_slice(100, 200, 100) == 100);
  CHECK(wait_slice(200, 200, 100) == 0);
}

TEST(a_negative_timeout_never_expires_and_a_zero_one_answers_at_once) {
  // The two sentinels, both of which ship. 108 of the 331 corpus sites pass
  // `-1`; `if (!WaitUnitsInArea(Group("Oasis_Guards" + i), "A_OasisB" + i, 0))`
  // is the zero.
  CHECK(wait_slice(0, -1, 100) == 100);
  CHECK(wait_slice(100000, -1, 100) == 100);
  CHECK(wait_slice(1000000000, -1, 2000) == 2000);
  CHECK(wait_slice(0, 0, 100) == 0);
  // Zero is not "no timeout": the two sentinels are one apart and reading them
  // the same way would make every `-1` site answer false on its first poll.
  CHECK(wait_slice(0, -1, 100) != wait_slice(0, 0, 100));
}

TEST(the_three_poll_cadences_are_the_ones_gbr_exe_carries) {
  // Immediate operands in the executable, not tuning: a peer that polled on a
  // different cadence would wake a script on a different turn, which in a
  // lockstep match is a desync rather than a preference.
  CHECK(kQueryPollInterval == 100);
  CHECK(kSettlementPollInterval == 1000);
  CHECK(kEnvPollInterval == 2000);
}

// --------------------------------------------------------------------------
// the conditions
// --------------------------------------------------------------------------

namespace {

/// A world with `count` units at the origin and a query that selects them all.
struct QueryWorld {
  World world;
  ObjectId query = kNoObject;

  explicit QueryWorld(std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      const ObjectId id = world.spawn(NativeClass::unit, nullptr);
      world.set_position(id, Point{0, 0});
    }
    query = world.create_query(objs_in_circle(Point{0, 0}, 1000, ClassFilter{}));
  }
};

}  // namespace

TEST(query_count_between_has_two_inclusive_bounds_and_one_escape) {
  // `count >= low && (count <= high || high < 0)`, off 0x005ed08b..0x005ed099.
  // Both ends inclusive, and only the *upper* bound has a negative escape --
  // which is why this is not written as a symmetric range test.
  const HostRegistry registry = wait_registry();
  QueryWorld fixture(3);
  const Value q = query_value(fixture.query);

  // Exactly on either bound is inside.
  HostCall low(fixture.world, {q, Value::integer(3), Value::integer(9), Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitQueryCountBetween", 4, low), true));
  HostCall high(fixture.world, {q, Value::integer(1), Value::integer(3), Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitQueryCountBetween", 4, high), true));

  // One outside either bound is not, and polls instead of answering.
  HostCall under(fixture.world, {q, Value::integer(4), Value::integer(9), Value::integer(-1)});
  CHECK(polls(invoke(registry, "WaitQueryCountBetween", 4, under)));
  HostCall over(fixture.world, {q, Value::integer(1), Value::integer(2), Value::integer(-1)});
  CHECK(polls(invoke(registry, "WaitQueryCountBetween", 4, over)));

  // A negative upper bound is "no upper bound", which seven shipped sites use.
  HostCall unbounded(fixture.world, {q, Value::integer(1), Value::integer(-1), Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitQueryCountBetween", 4, unbounded), true));

  // **The asymmetry is transcribed, not observed, and that is worth saying.**
  // The executable gives the escape to the upper bound only, and adding a
  // matching one for the lower bound was injected as a fault and *could not be
  // made to fail*: a count is never negative, so `count >= low` already holds
  // for every reachable input when `low` is negative. The two readings agree on
  // every input this simulation can produce. It is written the way 0x005ed093
  // writes it because that is what the instruction does, and the case below
  // records the shape rather than proving it.
  HostCall low_negative(fixture.world,
                        {q, Value::integer(-1), Value::integer(9), Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitQueryCountBetween", 4, low_negative), true));
}

TEST(a_wait_tests_its_condition_before_it_ever_suspends) {
  // The property the shipped idiom depends on. 147 of the 208
  // `WaitQueryCountBetween` sites pass a timeout of 100 ms, and a turn here is
  // 200 to 800 ms, so a family that always yielded once would answer false to
  // every one of them. A satisfied condition answers in the same slice.
  const HostRegistry registry = wait_registry();
  QueryWorld fixture(3);
  HostCall call(fixture.world, {query_value(fixture.query), Value::integer(1),
                                Value::integer(60), Value::integer(100)});
  const HostOutcome outcome = invoke(registry, "WaitQueryCountBetween", 4, call);
  CHECK(answered(outcome, true));
  CHECK(!polls(outcome));

  // And an unsatisfied one with a zero timeout answers false immediately rather
  // than polling once -- the other end of the same rule.
  HostCall at_once(fixture.world, {query_value(fixture.query), Value::integer(99),
                                   Value::integer(-1), Value::integer(0)});
  CHECK(answered(invoke(registry, "WaitQueryCountBetween", 4, at_once), false));
}

TEST(a_wait_gives_up_when_the_clock_reaches_its_deadline) {
  const HostRegistry registry = wait_registry();
  QueryWorld fixture(0);
  const Value q = query_value(fixture.query);
  const auto call_at = [&](std::int64_t now) {
    HostCall call(fixture.world,
                  {q, Value::integer(1), Value::integer(-1), Value::integer(250)}, now, 0);
    return invoke(registry, "WaitQueryCountBetween", 4, call);
  };
  CHECK(polls(call_at(0)));
  CHECK(polls(call_at(100)));
  CHECK(polls(call_at(200)));
  CHECK(answered(call_at(250), false));
  CHECK(answered(call_at(9999), false));
  // The slice shrinks so the last poll lands on the deadline.
  HostCall late(fixture.world, {q, Value::integer(1), Value::integer(-1), Value::integer(250)},
                200, 0);
  const HostOutcome outcome = invoke(registry, "WaitQueryCountBetween", 4, late);
  REQUIRE(polls(outcome));
  CHECK(outcome.suspend_for == 50);
}

TEST(the_empty_and_non_empty_waits_are_each_others_negation) {
  const HostRegistry registry = wait_registry();
  QueryWorld empty(0);
  QueryWorld full(2);

  HostCall a(empty.world, {query_value(empty.query), Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitEmptyQuery", 2, a), true));
  HostCall b(empty.world, {query_value(empty.query), Value::integer(-1)});
  CHECK(polls(invoke(registry, "WaitNonEmptyQuery", 2, b)));

  HostCall c(full.world, {query_value(full.query), Value::integer(-1)});
  CHECK(polls(invoke(registry, "WaitEmptyQuery", 2, c)));
  HostCall d(full.world, {query_value(full.query), Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitNonEmptyQuery", 2, d), true));
}

TEST(obj_in_query_and_common_objects_ask_about_membership) {
  const HostRegistry registry = wait_registry();
  QueryWorld fixture(3);
  const ObjectId inside = fixture.world.objects().front().id;
  const ObjectId outside = fixture.world.spawn(NativeClass::unit, nullptr);
  fixture.world.set_position(outside, Point{100000, 100000});

  const Value obj_in = Value::object(script::ObjectRef{kTypeObj, inside});
  const Value obj_out = Value::object(script::ObjectRef{kTypeObj, outside});
  HostCall yes(fixture.world, {obj_in, query_value(fixture.query), Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitObjInQuery", 3, yes), true));
  HostCall no(fixture.world, {obj_out, query_value(fixture.query), Value::integer(-1)});
  CHECK(polls(invoke(registry, "WaitObjInQuery", 3, no)));

  // `WaitCommonObjects` over two queries that do and do not overlap.
  const ObjectId far = fixture.world.create_query(
      objs_in_circle(Point{100000, 100000}, 10, ClassFilter{}));
  HostCall overlap(fixture.world,
                   {query_value(fixture.query), query_value(fixture.query), Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitCommonObjects", 3, overlap), true));
  HostCall disjoint(fixture.world,
                    {query_value(fixture.query), query_value(far), Value::integer(-1)});
  CHECK(polls(invoke(registry, "WaitCommonObjects", 3, disjoint)));
}

TEST(env_int_between_reads_the_root_scope_and_shares_the_bounds_rule) {
  // The same `value >= low && (value <= high || high < 0)` shape, which is the
  // corroboration for both: 0x005ecbf5 and 0x005ed08b emit it identically.
  const HostRegistry registry = wait_registry();
  World world;
  EnvSystem env;
  REQUIRE(world.add_system(&env));
  env.env().write_int(EnvScope::root(), "/En_NumidiansCharge", 2);

  HostCall inside(world, {str("/En_NumidiansCharge"), Value::integer(1), Value::integer(2),
                          Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitEnvIntBetween", 4, inside), true));
  HostCall outside(world, {str("/En_NumidiansCharge"), Value::integer(3), Value::integer(4),
                           Value::integer(-1)});
  CHECK(polls(invoke(registry, "WaitEnvIntBetween", 4, outside)));
  HostCall unbounded(world, {str("/En_NumidiansCharge"), Value::integer(1), Value::integer(-1),
                             Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitEnvIntBetween", 4, unbounded), true));

  // A key nothing wrote reads as zero, which is `EnvStore`'s rule and not this
  // entry point's -- asserted so that a change there shows up here too.
  HostCall absent(world, {str("/nothing"), Value::integer(0), Value::integer(0),
                          Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitEnvIntBetween", 4, absent), true));

  // And it polls at the environment cadence, not the query one.
  HostCall cadence(world, {str("/En_NumidiansCharge"), Value::integer(9), Value::integer(9),
                           Value::integer(-1)});
  const HostOutcome outcome = invoke(registry, "WaitEnvIntBetween", 4, cadence);
  REQUIRE(polls(outcome));
  CHECK(outcome.suspend_for == kEnvPollInterval);
}

TEST(a_wait_on_a_dead_query_refuses_by_name_rather_than_answering_true) {
  // The one place this engine deliberately differs. `gbr.exe` re-checks the
  // handle every poll (0x005ed05f) and, when it fails, ends the wait
  // *successfully* after calling 0x00686eb0 -- a bare `ret` in the retail
  // build, so the diagnostic never prints. A dangling handle silently satisfies
  // its condition. Reproducing that would be a wrong answer no test could see.
  const HostRegistry registry = wait_registry();
  QueryWorld fixture(1);
  fixture.world.despawn(fixture.query);

  HostCall call(fixture.world, {query_value(fixture.query), Value::integer(1),
                                Value::integer(-1), Value::integer(-1)});
  const HostOutcome outcome = invoke(registry, "WaitQueryCountBetween", 4, call);
  CHECK(outcome.status == HostStatus::error);

  // And an argument that is not a query handle at all is a different refusal.
  HostCall wrong(fixture.world, {Value::integer(7), Value::integer(1), Value::integer(-1),
                                 Value::integer(-1)});
  CHECK(invoke(registry, "WaitQueryCountBetween", 4, wrong).status == HostStatus::error);
}

namespace {

/// A world whose units are all at rest as far as `WaitIdle` can tell, plus a
/// query over them and a command system to answer for them.
struct IdleWorld {
  World world;
  CommandSystem commands;
  ObjectId query = kNoObject;
  std::vector<ObjectId> units;

  explicit IdleWorld(std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
      const ObjectId id = world.spawn(NativeClass::unit, nullptr);
      world.set_position(id, Point{0, 0});
      units.push_back(id);
    }
    (void)world.add_system(&commands);
    query = world.create_query(objs_in_circle(Point{0, 0}, 1000, ClassFilter{}));
  }

  /// Put `id` on a verb without going through `set_command`, which would want a
  /// class graph and a scheduler to launch a script with.
  void set_verb(ObjectId id, std::string_view verb) {
    CommandQueue& q = commands.queue(id);
    q.entries.clear();
    Command c;
    c.verb = std::string(verb);
    q.entries.push_back(std::move(c));
  }
};

}  // namespace

TEST(waiting_for_idle_accepts_two_verbs_and_an_object_that_was_never_commanded) {
  // 0x005ece5a compares the object's command string with "idle" and 0x005ece6a
  // with "standstill"; nothing else is at rest. The third case is this
  // engine's, not the original's: an object with no queue at all has never been
  // commanded, and `GameSession::start_object_scripts` never gives one to a
  // unit running its class's own `idle` method.
  const HostRegistry registry = wait_registry();
  IdleWorld fixture(3);
  const Value q = query_value(fixture.query);
  const auto ask = [&] {
    HostCall call(fixture.world, {q, Value::integer(-1)});
    return invoke(registry, "WaitIdle", 2, call);
  };

  CHECK(answered(ask(), true));  // no queues at all
  fixture.set_verb(fixture.units[0], "idle");
  CHECK(answered(ask(), true));
  fixture.set_verb(fixture.units[1], "standstill");
  CHECK(answered(ask(), true));
  fixture.set_verb(fixture.units[2], "move");
  CHECK(polls(ask()));
  fixture.set_verb(fixture.units[2], "idle");
  CHECK(answered(ask(), true));
}

TEST(an_empty_query_is_never_idle_however_long_it_waits) {
  // 0x005ece05 jumps *past* the loop when the count is zero, so "all of
  // nothing" deliberately does not satisfy. Written as its own test because the
  // obvious `all_of` spelling gets this exactly backwards.
  const HostRegistry registry = wait_registry();
  IdleWorld fixture(0);
  const Value q = query_value(fixture.query);
  HostCall forever(fixture.world, {q, Value::integer(-1)});
  CHECK(polls(invoke(registry, "WaitIdle", 2, forever)));
  HostCall expired(fixture.world, {q, Value::integer(100)}, 100, 0);
  CHECK(answered(invoke(registry, "WaitIdle", 2, expired), false));
}

namespace {

std::vector<std::byte> class_bytes(std::string_view text) {
  std::vector<std::byte> out(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
  }
  return out;
}

/// One class with a `maxhealth` of 100, which is what makes a percentage
/// meaningful: the maximum is a class property, not a field on the object.
ClassGraph health_graph() {
  ClassGraph graph;
  graph.add(class_bytes(R"(<class id="Legionary" cpp_class="CVXUnit" parent="">
      <properties maxhealth="100"/>
    </class>)"), "test_wait.cpp");
  graph.link();
  return graph;
}

/// `count` units of a class that declares `maxhealth`, each at `health`.
struct HealthWorld {
  ClassGraph graph = health_graph();
  World world;
  ObjectId query = kNoObject;

  HealthWorld(std::size_t count, std::int32_t health) {
    world.set_class_graph(&graph);
    const ClassIndex legionary = graph.lookup("Legionary");
    for (std::size_t i = 0; i < count; ++i) {
      const ObjectId id = world.spawn(NativeClass::unit, nullptr, legionary);
      world.set_position(id, Point{0, 0});
      (void)world.set_health(id, health);
    }
    query = world.create_query(objs_in_circle(Point{0, 0}, 1000, ClassFilter{}));
  }
};

}  // namespace

TEST(health_between_has_two_inclusive_bounds_and_no_escape_at_all) {
  // The sibling of `query_count_between_has_two_inclusive_bounds_and_one
  // _escape`, and the point is the difference: 0x005ed3dd is `cmp`/`jg` then
  // `cmp`/`jl` with no `test`/`jge` between them, where 0x005ed08b has one. A
  // negative upper bound here is unsatisfiable rather than unbounded.
  const HostRegistry registry = wait_registry();
  HealthWorld fixture(2, 25);
  const Value q = query_value(fixture.query);
  const auto between = [&](std::int32_t low, std::int32_t high) {
    HostCall call(fixture.world, {q, Value::integer(low), Value::integer(high),
                                  Value::integer(-1)});
    return invoke(registry, "WaitHealthBetween", 4, call);
  };
  // Two units at 25 of a class maximum of 100: 50 of 200, so 25 per cent.
  CHECK(answered(between(25, 25), true));   // both bounds inclusive, at once
  CHECK(answered(between(0, 25), true));    // upper inclusive
  CHECK(answered(between(25, 100), true));  // lower inclusive
  CHECK(polls(between(26, 100)));
  CHECK(polls(between(0, 24)));
  // No escape: -1 as the upper bound is a bound, not "unbounded".
  CHECK(polls(between(0, -1)));
}

TEST(a_query_whose_objects_declare_no_maximum_is_refused_rather_than_divided_by) {
  // 0x005ed3d7 divides by `sumMax` behind a guard that only covers
  // `sumHP == sumMax`, so objects that carry health and declare no `maxhealth`
  // fault the retail build outright -- a `#DE`, not a wrong answer. There is
  // nothing to reproduce and nothing to infer, so this refuses by name.
  //
  // Found by fault injection, not by design: replacing the equality test with
  // `sum_max != 0` was the one fault of sixteen that no test noticed, and the
  // reason is that it only changes *this* path.
  const HostRegistry registry = wait_registry();
  QueryWorld fixture(2);  // no class graph, so no `maxhealth` anywhere
  for (const WorldObject& slot : fixture.world.objects()) {
    if (slot.internal != InternalKind::none) continue;
    (void)fixture.world.set_health(slot.id, 25);
  }
  HostCall call(fixture.world, {query_value(fixture.query), Value::integer(0),
                                Value::integer(10), Value::integer(-1)});
  CHECK(invoke(registry, "WaitHealthBetween", 4, call).status == HostStatus::error);
}

TEST(an_empty_query_reads_as_full_health_because_the_sums_are_equal) {
  // Not a special case in the original and not one here: 0x005ed3c7 tests
  // `sumHP == sumMax` and answers 100, and for an empty query both are zero.
  // There is no size check anywhere in that body -- unlike `WaitIdle`'s at
  // 0x005ece05 -- so this falls out of the arithmetic, which is why the shipped
  // `(0, 10)` bounds keep waiting on an empty query rather than firing.
  const HostRegistry registry = wait_registry();
  HealthWorld fixture(0, 0);
  const Value q = query_value(fixture.query);
  const auto between = [&](std::int32_t low, std::int32_t high) {
    HostCall call(fixture.world, {q, Value::integer(low), Value::integer(high),
                                  Value::integer(-1)});
    return invoke(registry, "WaitHealthBetween", 4, call);
  };
  CHECK(answered(between(100, 100), true));
  CHECK(polls(between(0, 10)));  // the shipped bounds, and they do not fire
}

namespace {

/// A world with one named settlement, owned by `owner`.
struct SettlementWorld {
  World world;
  EconomySystem economy;

  explicit SettlementWorld(std::string_view name, PlayerId owner) {
    (void)world.add_system(&economy);
    const World::SettlementIds ids = world.spawn_settlement(owner);
    SettlementInit init;
    init.settlement_object = ids.settlement;
    init.holder_object = ids.holder;
    init.warehouse_object = ids.warehouse;
    init.owner = owner;
    init.name = std::string(name);
    (void)economy.create(world, init);
  }
};

}  // namespace

TEST(waiting_for_a_capture_compares_the_owner_with_the_script_player_minus_one) {
  // 0x005edbc9 reads the settlement's owner index and 0x005edbd8 decrements the
  // script's player number before comparing -- one-based script numbering
  // meeting a zero-based owner. `Settlement::player` (0x005c2340) is the same
  // two dereferences with an increment instead, which is the control.
  const HostRegistry registry = wait_registry();
  SettlementWorld fixture("S_Yard", 3);
  const auto owned_by = [&](std::int32_t player) {
    HostCall call(fixture.world,
                  {Value::string("S_Yard"), Value::integer(player), Value::integer(-1)});
    return invoke(registry, "WaitSettlementCapture", 3, call);
  };
  CHECK(answered(owned_by(4), true));  // owner 3 is the script's player 4
  CHECK(polls(owned_by(3)));
  CHECK(polls(owned_by(5)));
}

TEST(an_unowned_settlement_is_not_captured_by_a_player_number_that_does_not_exist) {
  // `player_from_script` answers `kNoPlayer` for anything outside 1..16, and
  // `kNoPlayer` is *also* what an unowned settlement carries. Comparing the two
  // directly would satisfy the wait immediately, on the wrong settlement, for
  // the wrong reason -- the same widening `EnemyObjs` and
  // `objects_of_class_for_player` were each caught by once.
  const HostRegistry registry = wait_registry();
  SettlementWorld fixture("S_Ruin", kNoPlayer);
  const auto owned_by = [&](std::int32_t player) {
    HostCall call(fixture.world,
                  {Value::string("S_Ruin"), Value::integer(player), Value::integer(-1)});
    return invoke(registry, "WaitSettlementCapture", 3, call);
  };
  CHECK(polls(owned_by(0)));
  CHECK(polls(owned_by(-1)));
  CHECK(polls(owned_by(17)));
}

TEST(a_settlement_name_that_resolves_nowhere_is_refused_rather_than_satisfied) {
  // 0x005edc46 ends the wait *true* after calling a diagnostic that is a bare
  // `ret` in the retail build, so a misspelt name silently succeeds. Refused by
  // name here, for the same reason `kDeadQuery` is.
  const HostRegistry registry = wait_registry();
  SettlementWorld fixture("S_Yard", 0);
  HostCall call(fixture.world,
                {Value::string("S_Typo"), Value::integer(1), Value::integer(-1)});
  CHECK(invoke(registry, "WaitSettlementCapture", 3, call).status == HostStatus::error);
  // An empty name is not a wildcard either.
  HostCall blank(fixture.world, {Value::string(""), Value::integer(1), Value::integer(-1)});
  CHECK(invoke(registry, "WaitSettlementCapture", 3, blank).status == HostStatus::error);
}

namespace {

/// A world with a circular area of radius 100 at the origin, `count` units, and
/// a query that selects them.
struct AreaWorld {
  World world;
  AreaSystem areas;
  ObjectId query = kNoObject;
  std::vector<ObjectId> units;

  explicit AreaWorld(std::size_t count) {
    (void)world.add_system(&areas);
    const ObjectId marker = world.spawn_internal(InternalKind::query);
    (void)world.named_objects().bind("A_Ring", marker);
    areas.areas().bind(marker, AreaShape::of_circle(Point{0, 0}, 100));
    for (std::size_t i = 0; i < count; ++i) {
      const ObjectId id = world.spawn(NativeClass::unit, nullptr);
      world.set_position(id, Point{0, 0});
      units.push_back(id);
    }
    query = world.create_query(objs_in_circle(Point{0, 0}, 100000, ClassFilter{}));
  }
};

}  // namespace

TEST(waiting_for_units_in_an_area_uses_the_query_rule_and_not_the_samplers) {
  // The two rules differ by the one-unit shell at the rim, and 0x004d7df0 --
  // the predicate this entry point calls -- compares against the `r2` field of
  // the `{cx, cy, r, r2}` POD (`jle` at 0x004d7e76), which is `d2 <= r*r`. A
  // The points that tell them apart are those with `r*r < d2 < (r+1)*(r+1)`,
  // and `(100, 1)` is one: `d2` is 10,001, which is outside `100*100` and
  // inside `101*101`. `(101, 0)` is *not* such a point -- its `d2` is exactly
  // `101*101` and both rules put it outside -- which is worth saying because it
  // is the obvious choice and it would have made this test pass vacuously.
  const HostRegistry registry = wait_registry();
  AreaWorld fixture(1);
  const Value q = query_value(fixture.query);
  const auto ask = [&] {
    HostCall call(fixture.world, {q, Value::string("A_Ring"), Value::integer(-1)});
    return invoke(registry, "WaitUnitsInArea", 3, call);
  };
  const AreaShape ring = AreaShape::of_circle(Point{0, 0}, 100);

  fixture.world.set_position(fixture.units[0], Point{100, 0});  // exactly r
  CHECK(ring.contains(Point{100, 0}) && ring.contains_by_query_rule(Point{100, 0}));
  CHECK(answered(ask(), true));

  fixture.world.set_position(fixture.units[0], Point{100, 1});  // the rim shell
  CHECK(ring.contains(Point{100, 1}));                    // inside by the sampler
  CHECK(!ring.contains_by_query_rule(Point{100, 1}));     // outside by the query
  CHECK(polls(ask()));

  // And the obvious near-miss really is outside both ways.
  CHECK(!ring.contains(Point{101, 0}));
  CHECK(!ring.contains_by_query_rule(Point{101, 0}));
}

TEST(units_in_an_area_is_two_tests_and_an_empty_query_fails_the_second) {
  // "The loop ran to the end" (0x005ee4b5) and "the count is not zero"
  // (0x005ee4c0) are separate branches, so an empty query keeps waiting even
  // though every one of its zero objects is trivially inside.
  const HostRegistry registry = wait_registry();
  AreaWorld empty(0);
  HostCall none(empty.world,
                {query_value(empty.query), Value::string("A_Ring"), Value::integer(-1)});
  CHECK(polls(invoke(registry, "WaitUnitsInArea", 3, none)));

  // And one object outside is enough to fail the first test.
  AreaWorld two(2);
  two.world.set_position(two.units[1], Point{5000, 5000});
  HostCall straggler(two.world,
                     {query_value(two.query), Value::string("A_Ring"), Value::integer(-1)});
  CHECK(polls(invoke(registry, "WaitUnitsInArea", 3, straggler)));
}

TEST(an_area_name_that_resolves_nowhere_is_refused_rather_than_satisfied) {
  // 0x005ee419 falls through to 0x005ee41d and answers **true** with no
  // diagnostic at all -- not even the dead one the query path calls. A misspelt
  // area name satisfies every wait on it. Refused here.
  const HostRegistry registry = wait_registry();
  AreaWorld fixture(1);
  HostCall call(fixture.world,
                {query_value(fixture.query), Value::string("A_Typo"), Value::integer(-1)});
  CHECK(invoke(registry, "WaitUnitsInArea", 3, call).status == HostStatus::error);
}

TEST(a_named_object_is_a_query_of_one_and_empties_when_the_object_dies) {
  // `NamedObj` is a *subtype* of `Query` (0x005b6d96 registers the parent
  // edge), so the ten shipped sites that pass one are a free upcast rather than
  // a mistake. The query holds exactly the bound object while it lives and goes
  // **empty** when it dies -- not invalid -- which is what makes the tutorial's
  // `(Caesar, 1, 1, -1)` then `(Caesar, 0, 0, -1)` pair mean "wait until he
  // exists" and then "until he is gone".
  const HostRegistry registry = wait_registry();
  World world;
  const ObjectId caesar = world.spawn(NativeClass::unit, nullptr);
  world.set_position(caesar, Point{0, 0});
  REQUIRE(world.named_objects().bind("Caesar", caesar));
  const auto index = static_cast<std::uint32_t>(world.named_objects().find("Caesar"));
  const Value named = Value::object(script::ObjectRef{kTypeNamedObj, index});

  const auto count_is = [&](std::int32_t low, std::int32_t high) {
    HostCall call(world, {named, Value::integer(low), Value::integer(high),
                          Value::integer(-1)});
    return invoke(registry, "WaitQueryCountBetween", 4, call);
  };
  CHECK(answered(count_is(1, 1), true));
  CHECK(polls(count_is(0, 0)));

  REQUIRE(world.despawn(caesar));
  CHECK(answered(count_is(0, 0), true));
  CHECK(polls(count_is(1, 1)));

  // A name nothing ever bound is an empty query, not a refusal: `GetNamedObj`
  // creates one on demand (0x005532f3) and it simply has no members.
  const auto missing = static_cast<std::uint32_t>(world.named_objects().size() + 5);
  HostCall unknown(world, {Value::object(script::ObjectRef{kTypeNamedObj, missing}),
                           Value::integer(0), Value::integer(0), Value::integer(-1)});
  CHECK(answered(invoke(registry, "WaitQueryCountBetween", 4, unknown), true));
}

// --------------------------------------------------------------------------
// WaitConvRequest: the reach test, and the two overloads
// --------------------------------------------------------------------------

namespace {

std::span<const std::byte> bytes(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// Two classes with the same radius and **different sight**, which is what
/// makes the direction of the reach test observable: a `Walker` can never be
/// talked to and a `Talker` always can.
struct ConvWorld {
  World world;
  ClassGraph graph;
  ClassIndex walker_class = kNoClass;
  ClassIndex talker_class = kNoClass;

  ConvWorld() {
    graph.add(bytes(R"(<class id="Walker" cpp_class="CVXUnit"><properties
                         sight="0" radius="10"/></class>)"),
              "walker.sc.xml");
    graph.add(bytes(R"(<class id="Talker" cpp_class="CVXUnit"><properties
                         sight="300" radius="10"/></class>)"),
              "talker.sc.xml");
    graph.link();
    world.set_class_graph(&graph);
    walker_class = graph.find("Walker");
    talker_class = graph.find("Talker");
  }

  ObjectId spawn(ClassIndex which, Point at) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, which);
    world.set_position(id, at);
    return id;
  }

  /// Every object of one class, wherever it stands. A circle rather than a
  /// group because the two shipped query parties -- `ClassPlayerObjs(cUnit, 1)`
  /// and a `<group>` -- are both membership by rule, and a rule that moves with
  /// the objects is the harder case.
  ObjectId query_of(ClassIndex which) {
    return world.create_query(objs_in_circle(Point{0, 0}, 30000, ClassFilter::of(which)));
  }
};

[[nodiscard]] Value obj_value(ObjectId id) {
  return Value::object(script::ObjectRef{kTypeObj, static_cast<std::uint32_t>(id)});
}

[[nodiscard]] bool is_invalid_object(const Value& value) {
  return value.is_object() && value.as_object().type == script::kNoType;
}

}  // namespace

TEST(a_conversation_request_reads_the_destinations_sight_and_not_the_walkers) {
  // `0x005a7870` is a method on **one** of the two, and which one is not
  // symmetric: it returns false outright when *its own* sight is zero. All
  // seven shipped sites put the unit that walks first and the thing it walks to
  // second -- `WaitConvRequest(NO_Scipio, NO_Masinissa, 1000)`,
  // `WaitConvRequest(ClassPlayerObjs(cUnit, 1), Larax, -1, o1, o2)` -- which is
  // also the order the five-argument overload writes its outputs in.
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId walker = f.spawn(f.walker_class, Point{0, 0});
  const ObjectId talker = f.spawn(f.talker_class, Point{200, 0});

  HostCall met(f.world, {obj_value(walker), obj_value(talker), Value::integer(-1)}, 1000, 0);
  CHECK(answered(invoke(registry, "WaitConvRequest", 3, met), true));

  // The same two objects the other way round ask the walker for a sight it has
  // not got. A symmetric reach test would answer true here.
  HostCall reversed(f.world, {obj_value(talker), obj_value(walker), Value::integer(-1)}, 1000, 0);
  CHECK(polls(invoke(registry, "WaitConvRequest", 3, reversed)));
}

TEST(the_conversation_reach_is_edge_to_edge_and_its_boundary_is_inclusive) {
  // `0x005a77b0` subtracts **both** class radii from `isqrt(dx² + dy²)` and
  // `0x005a7870` compares the remainder with `jg` -- so equal reaches. With
  // radius 10 apiece against a sight of 300 the last centre distance that meets
  // is 320, and 321 does not.
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId walker = f.spawn(f.walker_class, Point{0, 0});
  const ObjectId talker = f.spawn(f.talker_class, Point{0, 0});
  const auto at = [&](std::int32_t x) {
    f.world.set_position(talker, Point{x, 0});
    HostCall call(f.world, {obj_value(walker), obj_value(talker), Value::integer(-1)}, 1000, 0);
    return invoke(registry, "WaitConvRequest", 3, call);
  };

  CHECK(answered(at(320), true));   // edge distance exactly 300
  CHECK(polls(at(321)));            // 301, and the comparison is not strict
  // And the twenty units of radius are really being spent: a centre-to-centre
  // reading -- which is what `Obj::DistTo` answers, and the obvious way to
  // write this -- would already have given up here.
  CHECK(answered(at(310), true));
}

TEST(an_object_whose_class_declares_no_sight_can_never_be_talked_to) {
  // The zero test comes **before** the measurement (`0x005a7873`), which is not
  // an optimisation: without it the two standing on the same point have an edge
  // distance of −20, which is inside a sight of zero, and every wait on a
  // sightless object would answer at once.
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId walker = f.spawn(f.walker_class, Point{0, 0});
  const ObjectId other = f.spawn(f.walker_class, Point{0, 0});
  HostCall call(f.world, {obj_value(walker), obj_value(other), Value::integer(-1)}, 1000, 0);
  CHECK(polls(invoke(registry, "WaitConvRequest", 3, call)));
}

TEST(an_object_in_both_parties_does_not_meet_itself) {
  // The original's matcher is handed two distinct objects and tests one against
  // each side of the record, so it cannot produce this pair at all. Here the
  // parties are two queries that may overlap, and a talker is trivially within
  // its own sight -- so the exclusion has to be written down.
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  (void)f.spawn(f.talker_class, Point{0, 0});
  const Value talkers = query_value(f.query_of(f.talker_class));
  HostCall call(f.world, {talkers, talkers, Value::integer(-1), Value::integer(0),
                          Value::integer(0)}, 1000, 0);
  CHECK(polls(invoke(registry, "WaitConvRequest", 5, call)));
}

TEST(the_five_argument_overload_answers_which_two_met_and_not_the_first_of_each) {
  // `o1` is the member of the first party and `o2` the member of the second --
  // `conv.SetActor("Pich", o1.AsUnit())` and `if (o2.name == "NO_mercenary")`
  // are two shipped sites reading them straight afterwards. Both parties here
  // hold two objects and the pair that meets is the second of each, so an
  // implementation that answered "the first of each list" fails.
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId stranger = f.spawn(f.walker_class, Point{20000, 0});
  const ObjectId walker = f.spawn(f.walker_class, Point{0, 0});
  const ObjectId distant = f.spawn(f.talker_class, Point{0, 20000});
  const ObjectId talker = f.spawn(f.talker_class, Point{200, 0});
  CHECK(stranger < walker && distant < talker);

  HostCall call(f.world, {query_value(f.query_of(f.walker_class)),
                          query_value(f.query_of(f.talker_class)), Value::integer(-1),
                          Value::integer(0), Value::integer(0)}, 1000, 0);
  CHECK(answered(invoke(registry, "WaitConvRequest", 5, call), true));
  CHECK(call.arguments[3].is_object() && call.arguments[3].as_object().id == walker);
  CHECK(call.arguments[4].is_object() && call.arguments[4].as_object().id == talker);
}

TEST(two_destinations_in_reach_answer_the_first_in_object_order_not_the_nearest) {
  // Nothing in the original ranks the candidates: it walks its list and takes
  // the first record that matches. Both parties arrive here in ascending object
  // id -- `World::evaluate_query`'s order -- so which pair a world yields is a
  // function of the world and not of when it was asked, and it is *not* the
  // closest.
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId walker = f.spawn(f.walker_class, Point{0, 0});
  const ObjectId first = f.spawn(f.talker_class, Point{250, 0});
  const ObjectId nearer = f.spawn(f.talker_class, Point{30, 0});
  CHECK(first < nearer);

  HostCall call(f.world, {query_value(f.query_of(f.walker_class)),
                          query_value(f.query_of(f.talker_class)), Value::integer(-1),
                          Value::integer(0), Value::integer(0)}, 1000, 0);
  CHECK(answered(invoke(registry, "WaitConvRequest", 5, call), true));
  CHECK(call.arguments[4].as_object().id == first);
  CHECK(call.arguments[3].as_object().id == walker);
}

TEST(the_walkers_are_the_outer_loop_so_the_first_walker_gets_its_pick) {
  // Two pairs are in reach at once and they share no object, so the nesting
  // decides which one the wait answers with: walkers outermost gives the first
  // *walker* its destination, destinations outermost would give the first
  // *destination* its walker. Both are stable and only one is the shape the
  // out-parameters are named for -- `o1` is the unit that walked.
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId near_east = f.spawn(f.walker_class, Point{10000, 100});
  const ObjectId near_west = f.spawn(f.walker_class, Point{0, 100});
  const ObjectId west = f.spawn(f.talker_class, Point{0, 0});
  const ObjectId east = f.spawn(f.talker_class, Point{10000, 0});
  CHECK(near_east < near_west && west < east);

  HostCall call(f.world, {query_value(f.query_of(f.walker_class)),
                          query_value(f.query_of(f.talker_class)), Value::integer(-1),
                          Value::integer(0), Value::integer(0)}, 1000, 0);
  CHECK(answered(invoke(registry, "WaitConvRequest", 5, call), true));
  CHECK(call.arguments[3].as_object().id == near_east);
  CHECK(call.arguments[4].as_object().id == east);
}

/// **A request is never met inside the call that made it.** The original
/// appends the record on the fresh frame and reads it back on a resume, and
/// the completing side is the input layer's per-frame offer -- so a pair that
/// is already within reach answers on the first *poll*, not the first call.
/// `3_Great_Losses_Egypt`'s `Maps/1/Sequences/seq8.vs` loops on
/// `WaitConvRequest(hero, talkers, -1, o1, o2)` and, with the hero beside a
/// talker, ran through its whole instruction budget without yielding when the
/// predicate answered at once. A timeout of zero is therefore always false.
TEST(a_conversation_request_answers_on_a_poll_and_never_on_the_first_call) {
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId walker = f.spawn(f.walker_class, Point{0, 0});
  const ObjectId talker = f.spawn(f.talker_class, Point{200, 0});

  HostCall fresh(f.world, {obj_value(walker), obj_value(talker), Value::integer(-1)});
  const HostOutcome first = invoke(registry, "WaitConvRequest", 3, fresh);
  CHECK(polls(first) && first.suspend_for == kSettlementPollInterval);
  HostCall resumed(f.world, {obj_value(walker), obj_value(talker), Value::integer(-1)}, 1000, 0);
  CHECK(answered(invoke(registry, "WaitConvRequest", 3, resumed), true));

  // Zero timeout, pair in reach: appended, found pending, countdown expired.
  HostCall expired(f.world, {obj_value(walker), obj_value(talker), Value::integer(0),
                             obj_value(talker), obj_value(talker)});
  CHECK(answered(invoke(registry, "WaitConvRequest", 5, expired), false));
  CHECK(is_invalid_object(expired.arguments[3]));
  CHECK(is_invalid_object(expired.arguments[4]));
}

TEST(a_conversation_wait_that_times_out_clears_both_out_parameters) {
  // `0x005ed693` writes `0xffff` into both by-reference arguments on the
  // timeout path, and nothing at all on the poll path. That is what keeps a
  // wait that gave up from handing the next conversation the actor the previous
  // one left behind -- three of the four sites read an output immediately.
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId leftover = f.spawn(f.talker_class, Point{20000, 20000});
  const ObjectId walker = f.spawn(f.walker_class, Point{0, 0});
  (void)walker;
  const Value walkers = query_value(f.query_of(f.walker_class));
  const Value talkers = query_value(f.query_of(f.talker_class));

  // Timeout zero: one honest test, then give up in the same slice.
  HostCall expired(f.world, {walkers, talkers, Value::integer(0), obj_value(leftover),
                             obj_value(leftover)});
  CHECK(answered(invoke(registry, "WaitConvRequest", 5, expired), false));
  CHECK(is_invalid_object(expired.arguments[3]));
  CHECK(is_invalid_object(expired.arguments[4]));

  // A poll is not an answer, and leaves the caller's locals as it found them.
  HostCall polling(f.world, {walkers, talkers, Value::integer(-1), obj_value(leftover),
                             obj_value(leftover)});
  CHECK(polls(invoke(registry, "WaitConvRequest", 5, polling)));
  CHECK(polling.arguments[3].is_object() && polling.arguments[3].as_object().id == leftover);
  CHECK(polling.arguments[4].is_object() && polling.arguments[4].as_object().id == leftover);
}

TEST(a_conversation_party_that_names_nothing_live_waits_it_out_rather_than_refusing) {
  // `0x005ed931` branches straight past the manager when a handle does not
  // resolve: the call registers nothing, can therefore never be completed, and
  // counts its own timeout down to `false`. It is not an error and it is not a
  // silent success -- both of which this family has elsewhere, and both of
  // which `sim/wait.hpp` records.
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId talker = f.spawn(f.talker_class, Point{0, 0});
  const ObjectId ghost = f.spawn(f.walker_class, Point{0, 0});
  REQUIRE(f.world.despawn(ghost));

  HostCall forever(f.world, {obj_value(ghost), obj_value(talker), Value::integer(-1)});
  CHECK(polls(invoke(registry, "WaitConvRequest", 3, forever)));
  HostCall once(f.world, {obj_value(ghost), obj_value(talker), Value::integer(0)});
  CHECK(answered(invoke(registry, "WaitConvRequest", 3, once), false));
}

TEST(the_three_argument_overload_takes_the_named_objects_its_three_sites_pass) {
  // It is registered `(Obj, Obj, int)` and every one of its three shipped sites
  // passes a `NamedObj` global. The original does not convert: it files the
  // handle id it was given and its matcher casts at test time, a `Query`
  // through the membership slot and an `Obj` by identity. So both readings have
  // to work on the same entry point.
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId scipio = f.spawn(f.walker_class, Point{0, 0});
  const ObjectId masinissa = f.spawn(f.talker_class, Point{100, 0});
  REQUIRE(f.world.named_objects().bind("NO_Scipio", scipio));
  REQUIRE(f.world.named_objects().bind("NO_Masinissa", masinissa));
  const auto named = [&](const char* name) {
    return Value::object(script::ObjectRef{
        kTypeNamedObj, static_cast<std::uint32_t>(f.world.named_objects().find(name))});
  };

  HostCall call(f.world, {named("NO_Scipio"), named("NO_Masinissa"), Value::integer(1000)}, 1000, 0);
  CHECK(answered(invoke(registry, "WaitConvRequest", 3, call), true));

  // And a name whose object has died is an empty party, not a refusal.
  REQUIRE(f.world.despawn(masinissa));
  HostCall gone(f.world, {named("NO_Scipio"), named("NO_Masinissa"), Value::integer(1000)});
  CHECK(polls(invoke(registry, "WaitConvRequest", 3, gone)));
}

TEST(the_conversation_wait_polls_on_the_thousand_millisecond_cadence) {
  // `0x005ecb30`, the same helper `WaitSettlementCapture` and `WaitUnitsInArea`
  // reach -- not the query family's 100. A peer that polled faster would wake
  // the script on a different turn, which is a desync rather than a
  // preference.
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId walker = f.spawn(f.walker_class, Point{0, 0});
  const ObjectId talker = f.spawn(f.talker_class, Point{20000, 0});
  HostCall call(f.world, {obj_value(walker), obj_value(talker), Value::integer(-1)}, 1000, 0);
  const HostOutcome outcome = invoke(registry, "WaitConvRequest", 3, call);
  CHECK(polls(outcome) && outcome.suspend_for == kSettlementPollInterval);

  // And the last poll of a finite wait still lands on its deadline.
  HostCall tail(f.world, {obj_value(walker), obj_value(talker), Value::integer(1500)}, 1000, 0);
  const HostOutcome last = invoke(registry, "WaitConvRequest", 3, tail);
  CHECK(polls(last) && last.suspend_for == 500);
}

TEST(the_wait_domain_takes_no_entry_point_from_another_domain) {
  // The manifest check every domain gets: registering `wait` must not replace
  // anything another domain had defined, because `HostRegistry::define`
  // replaces without saying so.
  HostRegistry alone;
  script::declare_shipped_surface(alone);
  const std::size_t defined = register_wait_host(alone);
  CHECK(defined == wait_host_entry_count());

  HostRegistry together;
  const std::size_t all = register_all_hosts(together);
  HostRegistry without;
  script::declare_shipped_surface(without);
  for (const HostDomain& domain : host_domains()) {
    if (domain.name != "wait") domain.define(without);
  }
  // Every entry point the wait domain implements is one nobody else does.
  CHECK(all == without.implemented() + wait_host_entry_count());
}

/// `EndConvSetup(This, other)`: the offer side of the conversation manager,
/// which this engine reduces to the reach test. Within reach, in either
/// direction, it suspends once for 300 ms; out of reach, or on re-entry, it
/// finishes; a handle to nothing finishes too.
TEST(end_conv_setup_suspends_once_within_reach_and_finishes_otherwise) {
  const HostRegistry registry = wait_registry();
  ConvWorld f;
  const ObjectId walker = f.spawn(f.walker_class, Point{0, 0});
  const ObjectId talker = f.spawn(f.talker_class, Point{200, 0});

  HostCall met(f.world, {obj_value(walker), obj_value(talker)});
  const HostOutcome first = invoke(registry, "EndConvSetup", 2, met);
  CHECK(first.status == HostStatus::suspend);
  CHECK(first.suspend_for == 300);

  // The matcher works out which side matched, so the order does not matter.
  HostCall reversed(f.world, {obj_value(talker), obj_value(walker)});
  CHECK(invoke(registry, "EndConvSetup", 2, reversed).status == HostStatus::suspend);

  // A re-entry after the wait is the interpreter's resume, and it finishes.
  HostCall again(f.world, {obj_value(walker), obj_value(talker)});
  again.context.first_call = false;
  CHECK(invoke(registry, "EndConvSetup", 2, again).status == HostStatus::ok);

  // Out of reach: nothing to complete, no wait.
  f.world.set_position(talker, Point{2000, 0});
  HostCall apart(f.world, {obj_value(walker), obj_value(talker)});
  const HostOutcome far = invoke(registry, "EndConvSetup", 2, apart);
  CHECK(far.status == HostStatus::ok);

  HostCall gone(f.world, {obj_value(walker), obj_value(999999)});
  CHECK(invoke(registry, "EndConvSetup", 2, gone).status == HostStatus::ok);
}
