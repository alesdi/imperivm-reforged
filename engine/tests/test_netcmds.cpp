// The net command stream: sim/netcmds.hpp.
//
// Two peers stay in step only if they apply the same orders in the same
// sequence, so the cases that carry the reading are about *order* and about
// *what the hash can see*:
//
//   * The canonical order is by issuer, then sequence, then arrival -- and the
//     third key exists so that a caller's bug (two orders with one sequence
//     number) is a repeatable result rather than a desync.
//   * The fold reaches every field. A field left out is a field two peers can
//     disagree about in silence, and the test that would catch it is the one
//     that changes each field in turn.
//   * `check_lockstep` fails on a driver that is not a pure function of the
//     turn. That is this file's half of `test_conformance.cpp`'s rule: a check
//     that has never been observed to fail is not a check.
//
// No game data here. The world is three objects and no systems, because none
// of the above needs one -- what needs a real pipeline is
// `tests/test_corpus_lockstep.py`, which runs this over shipped maps.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/conformance.hpp"
#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace conformance = imperivm::core::sim::conformance;
using imperivm::core::NativeClass;

namespace {

NetOrder order_of(PlayerId issuer, std::uint32_t sequence, std::vector<ObjectId> actors = {7}) {
  NetOrder order;
  order.issuer = issuer;
  order.sequence = sequence;
  order.actors = std::move(actors);
  order.target.point = Point{10, 20};
  return order;
}

std::vector<std::string> applied_sequence(const NetTurn& turn) {
  std::vector<std::string> out;
  for (const NetOrder* order : canonical_order(turn)) {
    out.push_back(std::to_string(order->issuer) + ":" + std::to_string(order->sequence) + ":" +
                  std::to_string(order->actors.empty() ? 0 : order->actors[0]));
  }
  return out;
}

/// A `Run` over a world it owns, so that `start()` can hand back a fresh one
/// every call the way `Scenario` demands.
class OwnedRun final : public conformance::Run {
 public:
  OwnedRun() {
    world_.spawn(NativeClass::unit, nullptr);
    world_.spawn(NativeClass::unit, nullptr);
  }
  [[nodiscard]] World& world() noexcept override { return world_; }
  void advance(std::int32_t turn_length) override { world_.advance(turn_length); }

 private:
  World world_;
};

class QuietScenario final : public conformance::Scenario {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "quiet"; }
  [[nodiscard]] std::unique_ptr<conformance::Run> start(std::uint32_t) const override {
    return std::make_unique<OwnedRun>();
  }
};

/// The driver the contract asks for: a pure function of the turn index.
class StreamDriverUnderTest final : public conformance::Driver {
 public:
  explicit StreamDriverUnderTest(const CommandStream& stream) noexcept : stream_(&stream) {}
  [[nodiscard]] std::uint64_t drive(conformance::Run& run, std::size_t turn) override {
    if (const NetTurn* orders = stream_->at(turn); orders != nullptr) {
      (void)apply_turn(run.world(), *orders, nullptr);
    }
    return stream_hash(*stream_, turn + 1);
  }

 private:
  const CommandStream* stream_;
};

/// The bug the contract names: a driver with a cursor. It gives peer 1 the
/// stream from the start and peer 2 whatever is left, which is the shape of
/// every "it worked when I tested one client" desync.
class CursorDriver final : public conformance::Driver {
 public:
  explicit CursorDriver(const CommandStream& stream) noexcept : stream_(&stream) {}
  [[nodiscard]] std::uint64_t drive(conformance::Run& run, std::size_t) override {
    if (const NetTurn* orders = stream_->at(cursor_); orders != nullptr) {
      (void)apply_turn(run.world(), *orders, nullptr);
    }
    ++cursor_;
    return stream_hash(*stream_, cursor_);
  }

 private:
  const CommandStream* stream_;
  std::size_t cursor_ = 0;
};

CommandStream two_turn_stream() {
  CommandStream stream;
  stream.turns.resize(3);
  stream.turns[0].orders.push_back(order_of(1, 0));
  stream.turns[2].orders.push_back(order_of(2, 0, {9}));
  return stream;
}

}  // namespace

// --------------------------------------------------------------------------
// the canonical order
// --------------------------------------------------------------------------

TEST(canonical_order_is_by_issuer_then_sequence) {
  NetTurn turn;
  turn.orders.push_back(order_of(2, 0, {30}));
  turn.orders.push_back(order_of(1, 1, {20}));
  turn.orders.push_back(order_of(1, 0, {10}));
  const std::vector<std::string> sequence = applied_sequence(turn);
  REQUIRE(sequence.size() == 3);
  CHECK(sequence[0] == "1:0:10");
  CHECK(sequence[1] == "1:1:20");
  CHECK(sequence[2] == "2:0:30");
}

TEST(canonical_order_does_not_depend_on_arrival_order) {
  NetTurn forwards;
  forwards.orders.push_back(order_of(1, 0, {10}));
  forwards.orders.push_back(order_of(2, 0, {20}));
  forwards.orders.push_back(order_of(1, 1, {30}));
  NetTurn backwards;
  backwards.orders.push_back(order_of(1, 1, {30}));
  backwards.orders.push_back(order_of(2, 0, {20}));
  backwards.orders.push_back(order_of(1, 0, {10}));
  CHECK(applied_sequence(forwards) == applied_sequence(backwards));
}

TEST(canonical_order_keeps_arrival_order_within_a_duplicate_sequence_number) {
  // Two orders from one player carrying one sequence number is a caller's
  // bug. The sort is stable, so it is a repeatable result instead of a
  // divergence that depends on the sort's internals.
  NetTurn turn;
  turn.orders.push_back(order_of(1, 4, {10}));
  turn.orders.push_back(order_of(1, 4, {20}));
  const std::vector<std::string> sequence = applied_sequence(turn);
  REQUIRE(sequence.size() == 2);
  CHECK(sequence[0] == "1:4:10");
  CHECK(sequence[1] == "1:4:20");
}

// --------------------------------------------------------------------------
// the fold
// --------------------------------------------------------------------------

TEST(the_fold_reaches_every_field_of_an_order) {
  const auto hash_of = [](const NetOrder& order) {
    std::uint64_t state = 0;
    hash_order(state, order);
    return state;
  };
  const NetOrder base = order_of(1, 0, {7});
  const std::uint64_t reference = hash_of(base);

  NetOrder issuer = base;
  issuer.issuer = 2;
  CHECK(hash_of(issuer) != reference);

  NetOrder sequence = base;
  sequence.sequence = 1;
  CHECK(hash_of(sequence) != reference);

  NetOrder actors = base;
  actors.actors = {8};
  CHECK(hash_of(actors) != reference);

  NetOrder more_actors = base;
  more_actors.actors = {7, 8};
  CHECK(hash_of(more_actors) != reference);

  // The actor list is ordered: `issue_default_order` issues in list order, so
  // two peers with the same set in a different order really are running
  // different matches.
  NetOrder reordered;
  reordered = order_of(1, 0, {7, 8});
  NetOrder swapped = order_of(1, 0, {8, 7});
  CHECK(hash_of(reordered) != hash_of(swapped));

  NetOrder point = base;
  point.target.point = Point{11, 20};
  CHECK(hash_of(point) != reference);
  NetOrder point_y = base;
  point_y.target.point = Point{10, 21};
  CHECK(hash_of(point_y) != reference);

  NetOrder object = base;
  object.target.object = 3;
  CHECK(hash_of(object) != reference);

  NetOrder mode = base;
  mode.mode = OrderMode::append;
  CHECK(hash_of(mode) != reference);

  NetOrder modifier = base;
  modifier.modifier = true;
  CHECK(hash_of(modifier) != reference);

  NetOrder kind = base;
  kind.kind = NetOrderKind::surrender;
  CHECK(hash_of(kind) != reference);

  NetOrder command = base;
  command.kind = NetOrderKind::command;
  command.command = "attack";
  NetOrder other_command = command;
  other_command.command = "attacK";
  CHECK(hash_of(command) != hash_of(other_command));

  NetOrder aimed = command;
  aimed.aimed = true;
  CHECK(hash_of(aimed) != hash_of(command));

  // Ctrl on a train row's count: five copies are not one.
  NetOrder repeated = command;
  repeated.repeat = 5;
  CHECK(hash_of(repeated) != hash_of(command));
  NetOrder none = command;
  none.repeat = 0;
  CHECK(hash_of(none) != hash_of(command));
  CHECK(hash_of(none) != hash_of(repeated));
  NetOrder four = command;
  four.repeat = 4;
  CHECK(hash_of(four) != hash_of(repeated));

  // A cancel names its command.
  NetOrder cancel = base;
  cancel.kind = NetOrderKind::cancel_command;
  cancel.command_id = 7;
  NetOrder other_cancel = cancel;
  other_cancel.command_id = 8;
  CHECK(hash_of(cancel) != hash_of(other_cancel));
}

namespace {

/// Records what it was asked to do, in order: whether `apply_turn` hands the
/// non-click kinds over, in canonical order, and counts what it says.
class RecordingSink final : public NetCommandSink {
 public:
  bool command(const NetOrder& order) override {
    seen.push_back("cmd:" + std::to_string(order.issuer) + ":" + order.command +
                   (order.aimed ? ":aimed" : ""));
    return order.command != "refused";
  }
  bool surrender(PlayerId issuer) override {
    seen.push_back("surrender:" + std::to_string(issuer));
    return true;
  }
  std::vector<std::string> seen;
};

}  // namespace

TEST(apply_turn_hands_commands_and_surrenders_to_the_sink_in_canonical_order) {
  World world;
  NetTurn turn;
  NetOrder surrender = order_of(3, 0);
  surrender.kind = NetOrderKind::surrender;
  NetOrder aimed = order_of(1, 1);
  aimed.kind = NetOrderKind::command;
  aimed.command = "build";
  aimed.aimed = true;
  NetOrder pressed = order_of(1, 0);
  pressed.kind = NetOrderKind::command;
  pressed.command = "refused";
  turn.orders = {surrender, aimed, pressed};

  RecordingSink sink;
  const NetTurnReport report = apply_turn(world, turn, nullptr, &sink);
  CHECK(sink.seen == (std::vector<std::string>{"cmd:1:refused", "cmd:1:build:aimed",
                                                "surrender:3"}));
  CHECK(report.commands == 2);
  CHECK(report.unapplied == 1);
  CHECK(report.applied == 0);  // no right click among them

  // With nothing to carry them out, all three are unapplied -- on every peer
  // alike, which is the point of counting rather than failing.
  const NetTurnReport none = apply_turn(world, turn);
  CHECK(none.unapplied == 3);
  CHECK(none.commands == 0);
}

TEST(the_stream_hash_sees_arrival_order_through_the_canonical_one) {
  CommandStream forwards;
  forwards.turns.resize(1);
  forwards.turns[0].orders.push_back(order_of(1, 0, {10}));
  forwards.turns[0].orders.push_back(order_of(2, 0, {20}));
  CommandStream backwards;
  backwards.turns.resize(1);
  backwards.turns[0].orders.push_back(order_of(2, 0, {20}));
  backwards.turns[0].orders.push_back(order_of(1, 0, {10}));
  // Same match, handed over in two arrival orders: the peers agree.
  CHECK(stream_hash(forwards, 1) == stream_hash(backwards, 1));

  CommandStream different;
  different.turns.resize(1);
  different.turns[0].orders.push_back(order_of(1, 0, {10}));
  different.turns[0].orders.push_back(order_of(2, 0, {21}));
  CHECK(stream_hash(different, 1) != stream_hash(forwards, 1));
}

TEST(the_stream_hash_counts_quiet_turns) {
  // Three quiet turns and four are different matches, and a fold over the
  // orders alone could not tell them apart.
  CommandStream stream;
  stream.turns.resize(5);
  stream.turns[4].orders.push_back(order_of(1, 0));
  CHECK(stream_hash(stream, 3) != stream_hash(stream, 4));
  CHECK(stream_hash(stream, 4) != stream_hash(stream, 5));
}

TEST(the_stream_hash_of_no_turns_is_the_seed) {
  const CommandStream empty;
  CHECK(stream_hash(empty, 0) == stream_hash(empty, 10));
}

TEST(an_order_moved_to_another_turn_changes_the_stream) {
  CommandStream early;
  early.turns.resize(3);
  early.turns[0].orders.push_back(order_of(1, 0));
  CommandStream late;
  late.turns.resize(3);
  late.turns[1].orders.push_back(order_of(1, 0));
  CHECK(stream_hash(early, 3) != stream_hash(late, 3));
}

// --------------------------------------------------------------------------
// applying
// --------------------------------------------------------------------------

TEST(applying_a_turn_to_a_world_with_no_command_system_refuses_everything) {
  // `issue_default_order` already refuses every actor; the report says zero
  // applied rather than raising, because a peer that skipped the turn and one
  // that did not would be the divergence this exists to prevent.
  World world;
  NetTurn turn;
  turn.orders.push_back(order_of(1, 0));
  const NetTurnReport report = apply_turn(world, turn, nullptr);
  CHECK(report.applied == 0);
  CHECK(report.issued == 0);
}

TEST(a_stream_counts_its_orders) {
  const CommandStream stream = two_turn_stream();
  CHECK(stream.size() == 3);
  CHECK(stream.order_count() == 2);
  CHECK(stream.at(1) != nullptr);
  CHECK(stream.at(1)->empty());
  CHECK(stream.at(9) == nullptr);
}

// --------------------------------------------------------------------------
// the check
// --------------------------------------------------------------------------

TEST(lockstep_passes_when_both_peers_are_driven_alike) {
  const QuietScenario scenario;
  const CommandStream stream = two_turn_stream();
  StreamDriverUnderTest driver(stream);
  const std::vector<std::int32_t> schedule = conformance::uniform_schedule(400, 4);
  conformance::Trace trace;
  const conformance::Divergence divergence =
      conformance::check_lockstep(scenario, 0x2a, conformance::Schedule(schedule), driver, &trace);
  CHECK(!divergence.diverged());
  REQUIRE(trace.entries.size() == 4);
  // The channel the driver supplies is not zero once a turn has been driven,
  // and it moves on the turn an order lands rather than later.
  CHECK(trace.entries[0].hashes.netcmds != 0);
  CHECK(trace.entries[0].hashes.netcmds != trace.entries[1].hashes.netcmds);
  CHECK(trace.entries[1].hashes.netcmds != trace.entries[2].hashes.netcmds);
  // Turn 3 is past the end of the stream, so nothing new folds in.
  CHECK(trace.entries[2].hashes.netcmds == trace.entries[3].hashes.netcmds);
}

TEST(lockstep_catches_a_driver_that_is_not_a_function_of_the_turn) {
  // The half that matters. A cursor driver gives peer 2 a different match,
  // and the world hashes cannot see it -- the command queue hashes nothing --
  // so the only thing that reports it is the `netcmds` channel.
  const QuietScenario scenario;
  const CommandStream stream = two_turn_stream();
  CursorDriver driver(stream);
  const std::vector<std::int32_t> schedule = conformance::uniform_schedule(400, 4);
  const conformance::Divergence divergence =
      conformance::check_lockstep(scenario, 0x2a, conformance::Schedule(schedule), driver);
  REQUIRE(divergence.diverged());
  CHECK(divergence.kind == conformance::Divergence::Kind::hash);
  CHECK(divergence.channel == conformance::Channel::netcmds);
  const std::string message = divergence.describe();
  CHECK(message.find("netcmds") != std::string::npos);
  CHECK(message.find("peer 1") != std::string::npos);
  CHECK(message.find("peer 2") != std::string::npos);
}

TEST(an_undriven_run_leaves_the_netcmds_channel_alone) {
  // Every trace this project already records is undriven, and none of them
  // may move: `record` without a driver must file zero, which is what the
  // golden and every save's metadata were written against.
  const QuietScenario scenario;
  const std::vector<std::int32_t> schedule = conformance::uniform_schedule(400, 2);
  const conformance::Trace trace =
      conformance::record(scenario, 0x2a, conformance::Schedule(schedule));
  REQUIRE(trace.entries.size() == 2);
  CHECK(trace.entries[0].hashes.netcmds == 0);
  CHECK(trace.entries[1].hashes.netcmds == 0);
  CHECK(trace.entries[0].hashes.hash_of_hashes == conformance::roll_up(trace.entries[0].hashes));
}
