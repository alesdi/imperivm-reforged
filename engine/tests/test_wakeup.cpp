// A waiting script wakes on its own millisecond, not at the turn's end.
//
// `gbr.exe` steps the scripts' timing wheel one millisecond at a time inside a
// turn (0x00528b40 -> 0x006879a0) and re-arms a script that suspended at the
// wheel's current millisecond plus its wait (0x00688220). So a `Sleep(500)`
// loop runs every 500 game ms whatever the turn length, and `GetTime` reads
// the millisecond the script woke on. `Scheduler::advance` used to run every
// script once at the turn's end and count its next wait from there, which
// rounded each period up to whole turns: 600 ms at 150-ms turns, 594 at the
// 99-ms turn the options' speed of 999 gives, and once per 800-ms turn for a
// `Sleep(100)` poll. See `script/scheduler.hpp`, "A script wakes on its own
// millisecond", and `docs/engine/tick.md`, "The single-player turn".
//
// These run whole sessions, so `GetTime` is the real host function and the
// turn is `GameSession::advance`'s: systems first, then the scripts' pass.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

namespace {

using namespace imperivm::core;
using namespace imperivm::core::sim;

/// One `Mark(tag, time)` a script made.
struct Mark {
  std::int32_t tag = 0;
  std::int32_t time = 0;
};

/// Where `Mark` writes. A test fixture's, reset by every bench.
std::vector<Mark>& marks() {
  static std::vector<Mark> log;
  return log;
}

script::HostOutcome host_mark(script::CallContext& ctx) {
  marks().push_back(Mark{ctx.arg(0).as_integer(), ctx.arg(1).as_integer()});
  return script::HostOutcome::ok_void();
}

/// A session with nothing on its map, the real host surface, and `Mark`.
struct Bench {
  script::HostRegistry registry;
  ClassGraph graph;  // empty: a session needs one, and nothing is placed
  SessionInputs inputs;
  std::unique_ptr<GameSession> session;

  Bench() {
    marks().clear();
    (void)register_all_hosts(registry);
    registry.define(script::CallKind::free_function, "Mark", 2, &host_mark);
    graph.link();
    inputs.classes = &graph;
    auto made = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(made.ok());
    session = std::move(made.value());
  }

  /// Compile `source` as `name` into the session's library.
  std::uint32_t add(std::string_view source, const char* name) {
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(source.data()),
                                           source.size());
    script::Diagnostic diagnostic;
    const auto parsed = script::parse(bytes, name, &diagnostic);
    CHECK(parsed.ok());
    if (!parsed.ok()) return script::kNoChunk;
    script::CompileError error;
    auto chunk = script::compile(parsed.value(), &registry, &error);
    CHECK(chunk.ok());
    if (!chunk.ok()) return script::kNoChunk;
    return session->scheduler().add_chunk(std::move(chunk.value()));
  }

  script::ScriptId spawn(std::string_view source, const char* name) {
    const std::uint32_t chunk = add(source, name);
    if (chunk == script::kNoChunk) return script::kNoScript;
    return session->scheduler().spawn(chunk);
  }
};

constexpr std::string_view kSleepLoop500 =
    "// void\n"
    "while (1) {\n"
    "  Mark(1, GetTime());\n"
    "  Sleep(500);\n"
    "}\n";

/// The times tag `tag` was marked at, in order.
std::vector<std::int32_t> times_of(std::int32_t tag) {
  std::vector<std::int32_t> out;
  for (const Mark& mark : marks()) {
    if (mark.tag == tag) out.push_back(mark.time);
  }
  return out;
}

/// Every gap between consecutive marks of `tag` is `period`, over at least
/// `at_least` marks.
bool every_gap_is(std::int32_t tag, std::int32_t period, std::size_t at_least) {
  const std::vector<std::int32_t> times = times_of(tag);
  if (times.size() < at_least) return false;
  for (std::size_t i = 1; i < times.size(); ++i) {
    if (times[i] - times[i - 1] != period) return false;
  }
  return true;
}

}  // namespace

/// The original's single-player turn is 150 ms (0x0051ea97). A turn-end
/// scheduler ran this loop every 600 ms there; the wheel runs it every 500.
TEST(wakeup_a_sleep_500_loop_runs_every_500_ms_at_150_ms_turns) {
  Bench bench;
  REQUIRE(bench.session != nullptr);
  REQUIRE(bench.spawn(kSleepLoop500, "loop.vs") != script::kNoScript);
  bench.session->advance(40, 150);  // 6,000 ms

  const std::vector<std::int32_t> times = times_of(1);
  REQUIRE(!times.empty());
  // Spawned between turns, it first runs at the first turn's end.
  CHECK(times.front() == 150);
  CHECK(every_gap_is(1, 500, 12));
  CHECK(times.back() == 150 + 500 * 11);
  // And `GetTime` read the script's millisecond, not the turn's end: 650 is
  // not a multiple of 150.
  CHECK(times.size() > 1 && times[1] == 650);
}

/// After OK in Options the speed is 999 and a 100-ms real turn is 99 game ms.
/// A turn-end scheduler ran this loop every 594 ms there.
TEST(wakeup_a_sleep_500_loop_runs_every_500_ms_at_99_ms_turns) {
  Bench bench;
  REQUIRE(bench.session != nullptr);
  REQUIRE(bench.spawn(kSleepLoop500, "loop.vs") != script::kNoScript);
  bench.session->advance(61, 99);  // 6,039 ms

  const std::vector<std::int32_t> times = times_of(1);
  REQUIRE(!times.empty());
  CHECK(times.front() == 99);
  CHECK(every_gap_is(1, 500, 12));
  CHECK(times.back() == 99 + 500 * 11);
}

/// Two scripts due inside one turn run in the order they fall due, each
/// reading its own millisecond -- not both at the turn's end in id order.
TEST(wakeup_two_scripts_due_in_one_turn_run_in_wake_time_order) {
  Bench bench;
  REQUIRE(bench.session != nullptr);
  // The first spawned -- the lower id, which a turn-end pass runs first --
  // is the later to wake.
  REQUIRE(bench.spawn("// void\nSleep(120);\nMark(1, GetTime());\n", "late.vs") !=
          script::kNoScript);
  REQUIRE(bench.spawn("// void\nSleep(40);\nMark(2, GetTime());\n", "early.vs") !=
          script::kNoScript);
  bench.session->advance(1, 150);  // both start at 150 and sleep from there
  CHECK(marks().empty());
  bench.session->advance(1, 150);  // 190 and 270, both inside (150, 300]

  REQUIRE(marks().size() == 2);
  CHECK(marks()[0].tag == 2);
  CHECK(marks()[0].time == 190);
  CHECK(marks()[1].tag == 1);
  CHECK(marks()[1].time == 270);
  // The world itself stands at the turn's end.
  CHECK(bench.session->world().time() == 300);
  CHECK(bench.session->scheduler().now() == 300);
}

/// At the 800-ms turns the transport and the corpus runs use, a `Sleep(100)`
/// poll runs eight times a turn, as it does on the original's wheel -- it ran
/// once a turn. A zero wait still yields to the next turn rather than spinning
/// (the engine's choice; see `Scheduler`).
TEST(wakeup_a_sleep_100_poll_runs_eight_times_in_an_800_ms_turn) {
  Bench bench;
  REQUIRE(bench.session != nullptr);
  REQUIRE(bench.spawn("// void\nwhile (1) {\n  Mark(1, GetTime());\n  Sleep(100);\n}\n",
                      "poll.vs") != script::kNoScript);
  REQUIRE(bench.spawn("// void\nwhile (1) {\n  Mark(2, GetTime());\n  Sleep(0);\n}\n",
                      "yield.vs") != script::kNoScript);
  bench.session->advance(3, 800);

  // 800 at the first turn's end, then every 100 ms to 2,400.
  const std::vector<std::int32_t> polls = times_of(1);
  CHECK(polls.size() == 17);
  CHECK(!polls.empty() && polls.front() == 800 && polls.back() == 2400);
  CHECK(every_gap_is(1, 100, 17));
  const std::vector<std::int32_t> yields = times_of(2);
  CHECK(yields == (std::vector<std::int32_t>{800, 1600, 2400}));
}

/// A script spawned by a script mid-turn starts at its spawner's millisecond,
/// as a wake armed on the wheel's current millisecond does (0x0069f8f0), and
/// so its own waits count from there.
TEST(wakeup_a_script_spawned_mid_turn_starts_at_its_spawners_millisecond) {
  Bench bench;
  REQUIRE(bench.session != nullptr);
  REQUIRE(bench.add("// void\nMark(2, GetTime());\nSleep(50);\nMark(3, GetTime());\n",
                    "child.vs") != script::kNoChunk);
  REQUIRE(bench.spawn("// void\nSleep(40);\nMark(1, GetTime());\nAIRun(\"child.vs\");\n",
                      "parent.vs") != script::kNoScript);
  bench.session->advance(2, 150);

  REQUIRE(marks().size() == 3);
  CHECK(marks()[0].tag == 1);
  CHECK(marks()[0].time == 190);
  CHECK(marks()[1].tag == 2);
  CHECK(marks()[1].time == 190);
  CHECK(marks()[2].tag == 3);
  CHECK(marks()[2].time == 240);
}
