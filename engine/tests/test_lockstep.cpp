// The turn negotiator: sim/lockstep.hpp.
//
// Three things carry the reading, and each has a case that would fail without
// it:
//
//   * The wire is hostile input. Every prefix of a valid packet is refused,
//     a count is never believed past the bytes behind it, and nothing after
//     the last order is ignored quietly.
//   * A turn runs only when every peer has spoken for it. A missing packet
//     stalls; two different packets for one (peer, turn) are refused rather
//     than either being believed.
//   * The length is agreed: the largest proposal, clamped, converted by the
//     speed -- the 999 case is the dump's own 800 -> 799.
//
// And `check_netplay` is observed to fail, on a scenario that hands each peer
// a different world. The network model here scrambles, delays and repeats
// every packet; the corpus half is `tests/test_corpus_lockstep.py`.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/conformance.hpp"
#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace conformance = imperivm::core::sim::conformance;
using imperivm::core::NativeClass;

namespace {

NetOrder order_for(PlayerId issuer, std::uint32_t sequence, std::vector<ObjectId> actors) {
  NetOrder order;
  order.issuer = issuer;
  order.sequence = sequence;
  order.actors = std::move(actors);
  order.target.point = Point{-12, 4096};
  order.target.object = 77;
  order.mode = OrderMode::append;
  order.modifier = true;
  return order;
}

TurnPacket sample_packet() {
  TurnPacket packet;
  packet.peer = 3;
  packet.turn = 9;
  packet.proposed_ms = 460;
  packet.orders.push_back(order_for(3, 0, {5, 6, 7}));
  packet.orders.push_back(order_for(3, 1, {}));
  packet.orders[1].mode = OrderMode::replace;
  packet.orders[1].modifier = false;
  packet.orders[1].target.object = kNoObject;
  return packet;
}

bool same(const TurnPacket& a, const TurnPacket& b) { return encode(a) == encode(b); }

LockstepConfig two_peers(std::uint32_t delay = 2) {
  LockstepConfig config;
  config.peers = {4, 1};
  config.input_delay = delay;
  return config;
}

TurnPacket packet_from(PlayerId peer, std::uint32_t turn, std::int32_t ms,
                       std::vector<NetOrder> orders = {}) {
  TurnPacket packet;
  packet.peer = peer;
  packet.turn = turn;
  packet.proposed_ms = ms;
  packet.orders = std::move(orders);
  for (std::size_t i = 0; i < packet.orders.size(); ++i) {
    packet.orders[i].issuer = peer;
    packet.orders[i].sequence = static_cast<std::uint32_t>(i);
  }
  return packet;
}

class OwnedRun final : public conformance::Run {
 public:
  explicit OwnedRun(std::size_t units) {
    for (std::size_t i = 0; i < units; ++i) world_.spawn(NativeClass::unit, nullptr);
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
    return std::make_unique<OwnedRun>(2);
  }
};

/// The bug the peer comparison exists for: each call builds a different
/// world, which is two peers that loaded different maps.
class DriftingScenario final : public conformance::Scenario {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "drifting"; }
  [[nodiscard]] std::unique_ptr<conformance::Run> start(std::uint32_t) const override {
    return std::make_unique<OwnedRun>(2 + calls_++);
  }

 private:
  mutable std::size_t calls_ = 0;
};

/// A scenario whose world depends on whose screen it is: the leak a real
/// session's `local_player` must never be. `start` -- what the unnetworked
/// replay calls -- is the seat-less world; `start_as` adds one unit per seat
/// number, and records which seats it was asked for.
class SeatLeakingScenario final : public conformance::Scenario {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "seat-leaking"; }
  [[nodiscard]] std::unique_ptr<conformance::Run> start(std::uint32_t) const override {
    return std::make_unique<OwnedRun>(2);
  }
  [[nodiscard]] std::unique_ptr<conformance::Run> start_as(std::uint32_t,
                                                           PlayerId local) const override {
    seats.push_back(local);
    return std::make_unique<OwnedRun>(2 + (local == kNoPlayer ? 0u : local));
  }
  mutable std::vector<PlayerId> seats;
};

CommandStream intent_for(std::vector<PlayerId> players, std::size_t turns) {
  CommandStream intent;
  intent.turns.resize(turns);
  for (std::size_t t = 0; t < turns; ++t) {
    if (t % 3 == 2) continue;  // quiet turns too
    std::uint32_t seq = 0;
    for (const PlayerId player : players) {
      intent.turns[t].orders.push_back(
          order_for(player, seq++, {static_cast<ObjectId>(1 + t % 2)}));
    }
  }
  return intent;
}

}  // namespace

// --------------------------------------------------------------------------
// the wire
// --------------------------------------------------------------------------

TEST(a_turn_packet_survives_the_wire) {
  const TurnPacket packet = sample_packet();
  TurnPacket back;
  REQUIRE(decode(encode(packet), back) == DecodeStatus::ok);
  CHECK(back.peer == 3);
  CHECK(back.turn == 9);
  CHECK(back.proposed_ms == 460);
  REQUIRE(back.orders.size() == 2);
  CHECK(back.orders[0].actors == (std::vector<ObjectId>{5, 6, 7}));
  CHECK(back.orders[0].target.point.x == -12);
  CHECK(back.orders[0].target.point.y == 4096);
  CHECK(back.orders[0].target.object == 77);
  CHECK(back.orders[0].mode == OrderMode::append);
  CHECK(back.orders[0].modifier);
  CHECK(back.orders[1].actors.empty());
  CHECK(back.orders[1].mode == OrderMode::replace);
  CHECK(!back.orders[1].modifier);
  CHECK(back.orders[1].target.object == kNoObject);
  // Not carried, restored: the issuer is the sender and the sequence the
  // position, which is the whole of what a peer may say about either.
  CHECK(back.orders[0].issuer == 3);
  CHECK(back.orders[1].issuer == 3);
  CHECK(back.orders[0].sequence == 0);
  CHECK(back.orders[1].sequence == 1);
  CHECK(same(packet, back));
}

TEST(a_command_order_survives_the_wire) {
  TurnPacket packet = sample_packet();
  packet.orders[0].kind = NetOrderKind::command;
  packet.orders[0].command = "train_Legionary";
  packet.orders[0].aimed = true;
  // Ctrl on a train row: one order with a count (0x004e5e90 writes it).
  packet.orders[0].repeat = 5;
  packet.orders[1].kind = NetOrderKind::surrender;
  TurnPacket back;
  REQUIRE(decode(encode(packet), back) == DecodeStatus::ok);
  CHECK(back.orders[0].kind == NetOrderKind::command);
  CHECK(back.orders[0].command == "train_Legionary");
  CHECK(back.orders[0].aimed);
  CHECK(back.orders[0].repeat == 5);
  CHECK(back.orders[1].repeat == 1);
  CHECK(back.orders[1].kind == NetOrderKind::surrender);
  CHECK(back.orders[1].command.empty());
  CHECK(!back.orders[1].aimed);
}

TEST(a_cancel_order_survives_the_wire) {
  // `CVXCmdCancelCmd`'s two fields (0x004e5680): the building and `cmdid`.
  TurnPacket packet = sample_packet();
  packet.orders[0].kind = NetOrderKind::cancel_command;
  packet.orders[0].target.object = 184;
  packet.orders[0].command_id = 0x12345678u;
  TurnPacket back;
  REQUIRE(decode(encode(packet), back) == DecodeStatus::ok);
  CHECK(back.orders[0].kind == NetOrderKind::cancel_command);
  CHECK(back.orders[0].target.object == 184);
  CHECK(back.orders[0].command_id == 0x12345678u);
  CHECK(back.orders[1].command_id == 0);
  // Its id is its own tail: cut short, the packet is refused.
  packet.orders.resize(1);
  std::vector<std::byte> bytes = encode(packet);
  bytes.pop_back();
  TurnPacket cut;
  CHECK(decode(bytes, cut) == DecodeStatus::truncated);
}

TEST(submit_drops_a_command_name_the_wire_would_refuse) {
  TurnNegotiator n(two_peers(0), 1);
  NetOrder fine = order_for(1, 0, {1});
  fine.kind = NetOrderKind::command;
  fine.command = "move";
  NetOrder huge = fine;
  huge.command = std::string(kMaxCommandName + 1, 'x');
  const std::optional<TurnPacket> packet = n.submit({huge, fine}, 400);
  REQUIRE(packet.has_value());
  REQUIRE(packet->orders.size() == 1);
  CHECK(packet->orders[0].command == "move");
  CHECK(packet->orders[0].sequence == 0);  // renumbered after the drop
  TurnPacket back;
  CHECK(decode(encode(*packet), back) == DecodeStatus::ok);
}

TEST(every_prefix_of_a_packet_is_refused) {
  const std::vector<std::byte> bytes = encode(sample_packet());
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    TurnPacket out;
    const DecodeStatus status =
        decode(std::span<const std::byte>(bytes.data(), cut), out);
    CHECK(status != DecodeStatus::ok);
  }
}

TEST(a_byte_after_the_last_order_is_refused) {
  std::vector<std::byte> bytes = encode(sample_packet());
  bytes.push_back(std::byte{0});
  TurnPacket out;
  CHECK(decode(bytes, out) == DecodeStatus::trailing);
}

TEST(a_count_is_not_believed_past_the_bytes_behind_it) {
  // An order count of 0xFFFFFFFF on a packet with nothing after it: refused
  // before anything is reserved.
  TurnPacket empty;
  empty.proposed_ms = 400;
  std::vector<std::byte> bytes = encode(empty);
  for (std::size_t i = bytes.size() - 4; i < bytes.size(); ++i) bytes[i] = std::byte{0xFF};
  TurnPacket out;
  CHECK(decode(bytes, out) == DecodeStatus::truncated);

  // And an actor count likewise: the first order's count sits right after
  // the header's eighteen bytes.
  bytes = encode(sample_packet());
  for (std::size_t i = 18; i < 22; ++i) bytes[i] = std::byte{0xFF};
  CHECK(decode(bytes, out) == DecodeStatus::truncated);
}

TEST(the_wire_refuses_what_no_encoder_writes) {
  TurnPacket out;
  std::vector<std::byte> bytes = encode(sample_packet());
  bytes[0] = std::byte{'X'};
  CHECK(decode(bytes, out) == DecodeStatus::bad_magic);

  bytes = encode(sample_packet());
  bytes[4] = std::byte{kTurnPacketVersion + 1};
  CHECK(decode(bytes, out) == DecodeStatus::bad_version);

  // The last order carries no command name, so its last nine bytes are
  // mode, modifier, kind, aimed, a zero name length and the speed. A kind of
  // 8 is none (7 is a cancel); 3, a takeover, is one no peer may send.
  for (const std::size_t back : {9u, 8u, 7u, 6u}) {
    bytes = encode(sample_packet());
    bytes[bytes.size() - back] = std::byte{8};
    CHECK(decode(bytes, out) == DecodeStatus::bad_field);
  }
  bytes = encode(sample_packet());
  bytes[bytes.size() - 7] = std::byte{3};
  CHECK(decode(bytes, out) == DecodeStatus::bad_field);
  bytes[bytes.size() - 7] = std::byte{4};  // a speed is a kind a peer sends
  CHECK(decode(bytes, out) == DecodeStatus::ok);
  // A name longer than any the wire carries, with the bytes behind it.
  bytes = encode(sample_packet());
  bytes[bytes.size() - 5] = std::byte{kMaxCommandName + 1};
  bytes.insert(bytes.end() - 4, kMaxCommandName + 1, std::byte{'x'});
  CHECK(decode(bytes, out) == DecodeStatus::bad_field);

  TurnPacket zero = sample_packet();
  zero.proposed_ms = 0;
  CHECK(decode(encode(zero), out) == DecodeStatus::bad_field);
}

// --------------------------------------------------------------------------
// the negotiator
// --------------------------------------------------------------------------

TEST(the_turns_inside_the_delay_are_agreed_in_advance) {
  TurnNegotiator n(two_peers(2), 1);
  for (std::uint32_t t = 0; t < 2; ++t) {
    REQUIRE(n.ready());
    const std::optional<AgreedTurn> turn = n.take();
    REQUIRE(turn.has_value());
    CHECK(turn->index == t);
    CHECK(turn->orders.empty());
    CHECK(turn->real_ms == kDefaultTurnLength);
    CHECK(turn->length == kDefaultTurnLength);
  }
  // And turn 2 is the first that needs anybody.
  CHECK(!n.ready());
  CHECK(!n.take().has_value());
}

TEST(a_turn_waits_for_every_peer) {
  TurnNegotiator n(two_peers(0), 1);
  REQUIRE(n.submit({}, 400).has_value());
  CHECK(!n.ready());  // our own packet is not enough
  CHECK(n.receive(packet_from(4, 0, 400)) == ReceiveStatus::accepted);
  CHECK(n.ready());
  REQUIRE(n.take().has_value());
  CHECK(n.next_turn() == 1);
  CHECK(!n.ready());
}

TEST(submit_schedules_past_the_delay_and_stamps_the_orders) {
  TurnNegotiator n(two_peers(2), 4);
  NetOrder forged = order_for(1, 99, {3});  // claims another issuer and sequence
  const std::optional<TurnPacket> packet = n.submit({forged, forged}, 500);
  REQUIRE(packet.has_value());
  CHECK(packet->peer == 4);
  CHECK(packet->turn == 2);
  CHECK(packet->orders[0].issuer == 4);
  CHECK(packet->orders[1].issuer == 4);
  CHECK(packet->orders[0].sequence == 0);
  CHECK(packet->orders[1].sequence == 1);
  // Once per turn.
  CHECK(!n.submit({}, 500).has_value());
  (void)n.take();
  const std::optional<TurnPacket> next = n.submit({}, 500);
  REQUIRE(next.has_value());
  CHECK(next->turn == 3);
}

TEST(a_peer_that_is_not_in_the_match_cannot_submit) {
  TurnNegotiator n(two_peers(), 7);
  CHECK(!n.submit({}, 400).has_value());
}

TEST(a_repeated_packet_is_a_duplicate_and_a_different_one_is_refused) {
  TurnNegotiator n(two_peers(2), 1);
  REQUIRE(n.submit({}, 400).has_value());  // ours for turn 2
  const TurnPacket first = packet_from(4, 2, 400, {order_for(4, 0, {1})});
  CHECK(n.receive(first) == ReceiveStatus::accepted);
  CHECK(n.receive(encode(first)) == ReceiveStatus::duplicate);

  TurnPacket other = first;
  other.orders[0].target.point.x += 1;
  CHECK(n.receive(other) == ReceiveStatus::conflicting);
  other = first;
  other.proposed_ms = 401;  // the proposal is part of what was said
  CHECK(n.receive(other) == ReceiveStatus::conflicting);

  // And the first one is what stands.
  (void)n.take();
  (void)n.take();
  const std::optional<AgreedTurn> turn = n.take();
  REQUIRE(turn.has_value());
  REQUIRE(turn->orders.size() == 1);
  CHECK(turn->orders.orders[0].target.point.x == first.orders[0].target.point.x);
}

TEST(what_the_negotiator_refuses_to_hold) {
  TurnNegotiator n(two_peers(2), 1);
  REQUIRE(n.submit({}, 400).has_value());  // ours for turn 2
  CHECK(n.receive(packet_from(9, 2, 400)) == ReceiveStatus::unknown_peer);
  CHECK(n.receive(packet_from(4, 0, 400)) == ReceiveStatus::stale);  // agreed in advance
  CHECK(n.receive(packet_from(4, 1, 400)) == ReceiveStatus::stale);
  // The horizon is next + 2 * delay + 1, and an honest peer can reach it.
  CHECK(n.receive(packet_from(4, 5, 400)) == ReceiveStatus::accepted);
  CHECK(n.receive(packet_from(4, 6, 400)) == ReceiveStatus::too_far);
  CHECK(n.receive(packet_from(4, 3, 0)) == ReceiveStatus::malformed);

  TurnPacket forged = packet_from(4, 3, 400, {order_for(4, 0, {1})});
  forged.orders[0].issuer = 1;  // speaking for somebody else
  CHECK(n.receive(forged) == ReceiveStatus::malformed);
  forged = packet_from(4, 3, 400, {order_for(4, 0, {1})});
  forged.orders[0].sequence = 5;
  CHECK(n.receive(forged) == ReceiveStatus::malformed);

  std::vector<std::byte> junk = encode(packet_from(4, 3, 400));
  junk.pop_back();
  CHECK(n.receive(junk) == ReceiveStatus::malformed);

  // A turn that has run is stale too, not merely the advance ones.
  (void)n.take();
  (void)n.take();
  REQUIRE(n.receive(packet_from(4, 2, 400)) == ReceiveStatus::accepted);
  REQUIRE(n.take().has_value());
  CHECK(n.receive(packet_from(4, 2, 400)) == ReceiveStatus::stale);
}

TEST(the_honest_horizon_is_reached_by_an_honest_peer) {
  // The bound is not arbitrary: drive the fast peer as far as the slow one's
  // packets let it, and its next packet lands exactly on the slow one's
  // horizon. One less and a real match would refuse a real packet.
  const std::uint32_t delay = 2;
  TurnNegotiator slow(two_peers(delay), 1);
  TurnNegotiator fast(two_peers(delay), 4);
  const std::optional<TurnPacket> from_slow = slow.submit({}, 400);  // turn 2
  REQUIRE(from_slow.has_value());
  REQUIRE(fast.receive(*from_slow) == ReceiveStatus::accepted);
  std::optional<TurnPacket> last;
  for (;;) {
    last = fast.submit({}, 400);
    REQUIRE(last.has_value());
    CHECK(slow.receive(*last) == ReceiveStatus::accepted);
    if (!fast.ready()) break;
    REQUIRE(fast.take().has_value());
  }
  // Fast ran 0, 1, 2 and is stuck on 3; its last packet is for 3 + delay.
  CHECK(fast.next_turn() == 3);
  CHECK(last->turn == 5);
  CHECK(last->turn == slow.next_turn() + 2 * delay + 1);
}

TEST(the_length_is_the_largest_proposal_clamped_and_scaled) {
  LockstepConfig config = two_peers(0);
  TurnNegotiator n(config, 1);
  REQUIRE(n.submit({}, 291).has_value());
  REQUIRE(n.receive(packet_from(4, 0, 460)) == ReceiveStatus::accepted);
  std::optional<AgreedTurn> turn = n.take();
  REQUIRE(turn.has_value());
  CHECK(turn->real_ms == 460);
  CHECK(turn->length == 460);

  REQUIRE(n.submit({}, 50).has_value());
  REQUIRE(n.receive(packet_from(4, 1, 20)) == ReceiveStatus::accepted);
  turn = n.take();
  REQUIRE(turn.has_value());
  CHECK(turn->real_ms == kMinObservedTurnLength);

  REQUIRE(n.submit({}, 5000).has_value());
  REQUIRE(n.receive(packet_from(4, 2, 400)) == ReceiveStatus::accepted);
  turn = n.take();
  REQUIRE(turn.has_value());
  CHECK(turn->real_ms == kMaxObservedTurnLength);

  // The one off-speed dump: 800 ms at speed 999 is 799 game-time units.
  config.game_speed = 999;
  TurnNegotiator slow(config, 1);
  REQUIRE(slow.submit({}, 800).has_value());
  REQUIRE(slow.receive(packet_from(4, 0, 800)) == ReceiveStatus::accepted);
  turn = slow.take();
  REQUIRE(turn.has_value());
  CHECK(turn->length == 799);
  CHECK(slow.schedule() == (std::vector<std::int32_t>{799}));
}

TEST(the_agreed_turn_does_not_depend_on_arrival_order) {
  const TurnPacket a = packet_from(4, 0, 300, {order_for(4, 0, {1}), order_for(4, 0, {2})});
  LockstepConfig config;
  config.peers = {1, 4, 6};
  config.input_delay = 0;
  const TurnPacket c = packet_from(6, 0, 700, {order_for(6, 0, {3})});

  TurnNegotiator x(config, 1);
  TurnNegotiator y(config, 1);
  const std::optional<TurnPacket> mine_x = x.submit({order_for(1, 0, {9})}, 350);
  const std::optional<TurnPacket> mine_y = y.submit({order_for(1, 0, {9})}, 350);
  REQUIRE(mine_x.has_value() && mine_y.has_value());
  REQUIRE(x.receive(a) == ReceiveStatus::accepted);
  REQUIRE(x.receive(c) == ReceiveStatus::accepted);
  REQUIRE(y.receive(c) == ReceiveStatus::accepted);
  REQUIRE(y.receive(a) == ReceiveStatus::accepted);
  REQUIRE(x.take().has_value());
  REQUIRE(y.take().has_value());
  CHECK(stream_hash(x.history(), 1) == stream_hash(y.history(), 1));
  CHECK(x.schedule() == y.schedule());
  CHECK(x.schedule().front() == 700);
}

// --------------------------------------------------------------------------
// the loopback
// --------------------------------------------------------------------------

TEST(three_peers_over_a_scrambling_network_play_one_game) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 2, 5};
  options.seed = 0x51;
  options.max_delay = 4;
  const CommandStream intent = intent_for({0, 2, 5}, 30);
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 0x2a, 30, intent, options, &report);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("  %s\n", divergence.describe().c_str());
  CHECK(!report.deadlocked);
  CHECK(report.peers == 3);
  CHECK(report.turns == 30);
  CHECK(report.refused == 0);
  CHECK(report.duplicates > 0);  // the retransmits arrived, and changed nothing
  CHECK(report.stalls > 0);      // the delay was real enough to wait on
  // The agreed stream is the intent moved by the delay: 28 of the 30 intent
  // turns fit, 2 per 3 carry three orders.
  CHECK(report.orders == 57);
  // And the length moved, the way the dumps' does, within its bounds.
  REQUIRE(report.schedule.size() == 30);
  bool moved = false;
  for (std::size_t t = 0; t < report.schedule.size(); ++t) {
    CHECK(report.schedule[t] >= kMinObservedTurnLength);
    CHECK(report.schedule[t] <= kMaxObservedTurnLength);
    if (t >= 2 && report.schedule[t] != report.schedule[2]) moved = true;
  }
  CHECK(moved);
  CHECK(report.schedule[0] == kDefaultTurnLength);
  CHECK(report.schedule[1] == kDefaultTurnLength);
}

TEST(the_network_seed_changes_the_arrivals_and_not_the_game) {
  const QuietScenario scenario;
  const CommandStream intent = intent_for({1, 3}, 20);
  NetplayOptions options;
  options.lockstep.peers = {1, 3};
  options.vary_proposals = false;  // so only the network differs
  NetplayReport first;
  NetplayReport second;
  options.seed = 1;
  REQUIRE(!check_netplay(scenario, 7, 20, intent, options, &first).diverged());
  options.seed = 99;
  options.max_delay = 6;
  REQUIRE(!check_netplay(scenario, 7, 20, intent, options, &second).diverged());
  CHECK(first.stalls != second.stalls);
  CHECK(first.schedule == second.schedule);
  CHECK(first.orders == second.orders);
}

TEST(netplay_off_speed_runs_the_scaled_length_on_every_peer) {
  // At 999 the agreed real length and the game-time length differ by one in
  // eight hundred, which is the one place a peer that advanced by the wrong
  // one of the two would still agree with every other peer that made the
  // same mistake. Only the comparison with the unnetworked run can see it.
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 1};
  options.lockstep.game_speed = 999;
  const CommandStream intent = intent_for({0, 1}, 12);
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 3, 12, intent, options, &report);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("  %s\n", divergence.describe().c_str());
  REQUIRE(report.schedule.size() == 12);
  CHECK(report.schedule[0] == 399);  // 400 ms at 999
}

TEST(netplay_catches_peers_that_loaded_different_worlds) {
  const DriftingScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 1};
  const CommandStream intent = intent_for({0, 1}, 6);
  const conformance::Divergence divergence = check_netplay(scenario, 1, 6, intent, options);
  REQUIRE(divergence.diverged());
  CHECK(divergence.kind == conformance::Divergence::Kind::hash);
  const std::string message = divergence.describe();
  CHECK(message.find("peer 0") != std::string::npos);
  CHECK(message.find("peer 1") != std::string::npos);
}

TEST(netplay_starts_every_peer_from_its_own_seat_and_catches_a_seat_that_leaks) {
  // Every peer watching from nowhere would agree with every other about a
  // world that differs by seat; only starting each from its own finds it.
  const SeatLeakingScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {1, 0};
  const CommandStream intent = intent_for({0, 1}, 6);
  const conformance::Divergence divergence = check_netplay(scenario, 1, 6, intent, options);
  CHECK(scenario.seats == (std::vector<PlayerId>{0, 1}));
  REQUIRE(divergence.diverged());
  CHECK(divergence.kind == conformance::Divergence::Kind::hash);
}

TEST(netplay_with_no_peers_is_unbuildable_rather_than_a_pass) {
  const QuietScenario scenario;
  NetplayOptions options;
  const conformance::Divergence divergence =
      check_netplay(scenario, 1, 4, CommandStream{}, options);
  CHECK(divergence.kind == conformance::Divergence::Kind::unbuildable);
}
