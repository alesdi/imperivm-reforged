// SetSpeed mid-match: a `set_speed` order, agreed like any other.
//
// What carries the reading:
//
//   * The speed travels as an order, so it is in the agreed history and the
//     stream hash, and every peer applies it on the same turn.
//   * It takes effect on the turn *after* the one it was agreed for: that
//     turn's length is converted at the old speed, every later one at the
//     new (the original's clock, 0x00528a80, converts with the speed the
//     command wrote while the turn ran). The world's clock takes it as the
//     order is applied, which is the rate of the next turn.
//   * Two in one turn: the last in canonical order wins. Out of 1..100000 it
//     is clamped (0x004e67c0). A match with a fixed speed takes none.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/conformance.hpp"
#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace conformance = imperivm::core::sim::conformance;
using imperivm::core::NativeClass;

namespace {

NetOrder speed(std::int32_t per_mille, PlayerId issuer = 0) {
  NetOrder order;
  order.issuer = issuer;
  order.kind = NetOrderKind::set_speed;
  order.speed = per_mille;
  return order;
}

TurnPacket packet(PlayerId peer, std::uint32_t turn, std::vector<NetOrder> orders = {}) {
  TurnPacket out;
  out.peer = peer;
  out.turn = turn;
  out.proposed_ms = 400;
  for (std::size_t i = 0; i < orders.size(); ++i) {
    orders[i].issuer = peer;
    orders[i].sequence = static_cast<std::uint32_t>(i);
  }
  out.orders = std::move(orders);
  return out;
}

LockstepConfig two(bool variable = true) {
  LockstepConfig config;
  config.peers = {0, 1};
  config.variable_speed = variable;
  return config;
}

/// Take every ready turn, submitting nothing after each but an empty packet.
std::vector<AgreedTurn> run(TurnNegotiator& negotiator) {
  std::vector<AgreedTurn> out;
  while (out.size() < 16) {
    std::optional<AgreedTurn> turn = negotiator.take();
    if (!turn.has_value()) break;
    out.push_back(*turn);
    (void)negotiator.submit({}, 400);
  }
  return out;
}

class OwnedRun final : public conformance::Run {
 public:
  OwnedRun() { world_.spawn(NativeClass::unit, nullptr); }
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

}  // namespace

TEST(a_speed_survives_the_wire_and_reaches_the_stream_hash) {
  TurnPacket sent = packet(1, 4, {speed(1400)});
  TurnPacket back;
  REQUIRE(decode(encode(sent), back) == DecodeStatus::ok);
  REQUIRE(back.orders.size() == 1);
  CHECK(back.orders[0].kind == NetOrderKind::set_speed);
  CHECK(back.orders[0].speed == 1400);
  // Out of range on the wire is still what was said; it is clamped where
  // it is applied, as the original's execution clamps.
  sent.orders[0].speed = -5;
  REQUIRE(decode(encode(sent), back) == DecodeStatus::ok);
  CHECK(back.orders[0].speed == -5);
  CommandStream a;
  CommandStream b;
  a.turns.resize(1);
  b.turns.resize(1);
  a.turns[0].orders.push_back(speed(1400));
  b.turns[0].orders.push_back(speed(2000));
  CHECK(stream_hash(a, 1) != stream_hash(b, 1));
}

TEST(the_speed_changes_from_the_turn_after_its_order) {
  TurnNegotiator negotiator(two(), 0);
  REQUIRE(negotiator.submit({speed(2000)}, 400).has_value());  // for turn 2
  CHECK(negotiator.receive(packet(1, 2)) == ReceiveStatus::accepted);
  CHECK(negotiator.receive(packet(1, 3)) == ReceiveStatus::accepted);
  CHECK(negotiator.receive(packet(1, 4)) == ReceiveStatus::accepted);
  const std::vector<AgreedTurn> turns = run(negotiator);
  REQUIRE(turns.size() >= 4);
  CHECK(turns[2].game_speed == 1000);
  CHECK(turns[2].length == 400);  // the turn it was agreed for: the old speed
  CHECK(turns[3].game_speed == 2000);
  CHECK(turns[3].length == 800);  // and every turn after, the new
  CHECK(negotiator.history().turns[2].orders.size() == 1);
}

TEST(the_last_speed_in_canonical_order_wins) {
  TurnNegotiator negotiator(two(), 1);
  REQUIRE(negotiator.submit({speed(700), speed(2000)}, 400).has_value());
  CHECK(negotiator.receive(packet(0, 2, {speed(1400)})) == ReceiveStatus::accepted);
  CHECK(negotiator.receive(packet(0, 3)) == ReceiveStatus::accepted);
  const std::vector<AgreedTurn> turns = run(negotiator);
  REQUIRE(turns.size() >= 4);
  // Player 0's 1400, then player 1's 700 and 2000: 2000, whoever arrived last.
  CHECK(turns[3].game_speed == 2000);
}

TEST(a_speed_out_of_range_is_clamped_as_it_is_applied) {
  TurnNegotiator slow(two(), 0);
  REQUIRE(slow.submit({speed(0)}, 400).has_value());
  CHECK(slow.receive(packet(1, 2)) == ReceiveStatus::accepted);
  CHECK(slow.receive(packet(1, 3)) == ReceiveStatus::accepted);
  std::vector<AgreedTurn> turns = run(slow);
  REQUIRE(turns.size() >= 4);
  CHECK(turns[3].game_speed == kMinGameSpeed);
  CHECK(turns[3].length == 1);  // 400 ms at 1 per mille, never zero

  TurnNegotiator fast(two(), 0);
  REQUIRE(fast.submit({speed(900000)}, 400).has_value());
  CHECK(fast.receive(packet(1, 2)) == ReceiveStatus::accepted);
  CHECK(fast.receive(packet(1, 3)) == ReceiveStatus::accepted);
  turns = run(fast);
  REQUIRE(turns.size() >= 4);
  CHECK(turns[3].game_speed == kMaxGameSpeed);
  CHECK(turns[3].length == 40000);

  World world;
  NetTurn turn;
  turn.orders.push_back(speed(-3));
  (void)apply_turn(world, turn);
  CHECK(world.clock().config().game_speed == kMinGameSpeed);
}

TEST(a_fixed_speed_takes_no_speed_order_on_any_peer) {
  TurnNegotiator negotiator(two(/*variable=*/false), 0);
  const std::optional<TurnPacket> mine = negotiator.submit({speed(2000)}, 400);
  REQUIRE(mine.has_value());
  CHECK(mine->orders.empty());  // never sent
  // A peer that thinks otherwise sends one anyway: it is stripped.
  CHECK(negotiator.receive(packet(1, 2, {speed(1400)})) == ReceiveStatus::accepted);
  CHECK(negotiator.receive(packet(1, 3)) == ReceiveStatus::accepted);
  const std::vector<AgreedTurn> turns = run(negotiator);
  REQUIRE(turns.size() >= 4);
  CHECK(turns[2].orders.empty());
  CHECK(negotiator.history().turns[2].orders.empty());
  CHECK(turns[3].game_speed == 1000);
}

TEST(applying_a_speed_writes_the_worlds_clock_and_needs_no_sink) {
  World world;
  const std::uint64_t before = world.hashes().hash_of_hashes;
  NetTurn turn;
  turn.orders.push_back(speed(1400, 2));
  const NetTurnReport report = apply_turn(world, turn);
  CHECK(report.speeds == 1);
  CHECK(report.unapplied == 0);
  CHECK(world.clock().config().game_speed == 1400);
  CHECK(world.hashes().hash_of_hashes != before);  // the speed is world state
}

TEST(netplay_with_a_speed_change_is_one_game_and_the_replay_agrees) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 3, 5};
  options.vary_proposals = false;  // every turn 400 ms, so the speed shows
  CommandStream intent;
  intent.turns.resize(12);
  intent.turns[3].orders.push_back(speed(2000, 3));  // agreed for turn 5
  intent.turns[8].orders.push_back(speed(700, 5));   // agreed for turn 10
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 1, 14, intent, options, &report);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("  %s\n", divergence.describe().c_str());
  REQUIRE(report.schedule.size() == 14);
  CHECK(report.schedule[5] == 400);
  CHECK(report.schedule[6] == 800);
  CHECK(report.schedule[10] == 800);
  CHECK(report.schedule[11] == 280);
  CHECK(report.stalls > 0);

  // And through the link layer, losing datagrams.
  options.relay = true;
  options.loss_per_mille = 200;
  NetplayReport relayed;
  CHECK(!check_netplay(scenario, 1, 14, intent, options, &relayed).diverged());
  CHECK(relayed.schedule == report.schedule);
}
