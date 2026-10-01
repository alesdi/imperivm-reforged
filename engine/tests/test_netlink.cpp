// The link layer: sim/netlink.hpp.
//
// What carries the reading:
//
//   * A lost datagram is repaired by the next one, because every datagram
//     carries everything the far side has not acknowledged -- and stops
//     carrying it once it has.
//   * A relay forwards a packet everywhere except where it came from and
//     never back to its origin; nothing else makes a star work.
//   * The round trip is `now - echo - hold`, exactly, and a reordered stamp
//     does not drag it backwards.
//   * Over a star losing a quarter of its datagrams, every peer still plays
//     one game; over a link losing all of them, the harness says deadlock
//     rather than pass.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/conformance.hpp"
#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netlink.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace conformance = imperivm::core::sim::conformance;
using imperivm::core::NativeClass;

namespace {

constexpr std::uint32_t kMatch = 0xC0FFEE;

LockstepConfig three(std::uint32_t delay = 2) {
  LockstepConfig config;
  config.peers = {0, 1, 2};
  config.input_delay = delay;
  return config;
}

TurnPacket own(PlayerId peer, std::uint32_t turn, std::size_t actors = 1) {
  TurnPacket packet;
  packet.peer = peer;
  packet.turn = turn;
  packet.proposed_ms = 400;
  NetOrder order;
  order.issuer = peer;
  for (std::size_t i = 0; i < actors; ++i) order.actors.push_back(static_cast<ObjectId>(i + 1));
  packet.orders.push_back(order);
  return packet;
}

Datagram parsed(const std::vector<std::byte>& bytes) {
  Datagram out;
  CHECK(decode(bytes, out) == DecodeStatus::ok);
  return out;
}

std::vector<std::uint32_t> turns_in(const Datagram& datagram) {
  std::vector<std::uint32_t> out;
  for (const auto& bytes : datagram.packets) {
    TurnPacket packet;
    if (decode(bytes, packet) == DecodeStatus::ok) out.push_back(packet.turn);
  }
  return out;
}

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

CommandStream intent_for(std::vector<PlayerId> players, std::size_t turns) {
  CommandStream intent;
  intent.turns.resize(turns);
  for (std::size_t t = 0; t < turns; ++t) {
    if (t % 3 == 2) continue;
    for (const PlayerId player : players) {
      NetOrder order;
      order.issuer = player;
      order.actors = {static_cast<ObjectId>(1 + t % 2)};
      intent.turns[t].orders.push_back(order);
    }
  }
  return intent;
}

}  // namespace

// --------------------------------------------------------------------------
// the datagram
// --------------------------------------------------------------------------

TEST(a_datagram_survives_the_wire) {
  Datagram datagram;
  datagram.match = kMatch;
  datagram.from = 2;
  datagram.stamp = 123456;
  datagram.echo = 99;
  datagram.hold = 7;
  datagram.acks = {{0, 5}, {2, 9}};
  datagram.packets = {encode(own(2, 4)), encode(own(0, 3, 5))};
  const Datagram back = parsed(encode(datagram));
  CHECK(back.match == kMatch);
  CHECK(back.from == 2);
  CHECK(back.stamp == 123456);
  CHECK(back.echo == 99);
  CHECK(back.hold == 7);
  REQUIRE(back.acks.size() == 2);
  CHECK(back.acks[1].peer == 2);
  CHECK(back.acks[1].next_turn == 9);
  CHECK(back.packets == datagram.packets);
}

TEST(every_prefix_of_a_datagram_is_refused_and_so_is_a_byte_more) {
  Datagram datagram;
  datagram.acks = {{0, 5}};
  datagram.packets = {encode(own(1, 4))};
  std::vector<std::byte> bytes = encode(datagram);
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    Datagram out;
    CHECK(decode(std::span<const std::byte>(bytes.data(), cut), out) != DecodeStatus::ok);
  }
  bytes.push_back(std::byte{0});
  Datagram out;
  CHECK(decode(bytes, out) == DecodeStatus::trailing);
  bytes = encode(datagram);
  bytes[4] = std::byte{kDatagramVersion + 1};
  CHECK(decode(bytes, out) == DecodeStatus::bad_version);
}

// --------------------------------------------------------------------------
// the node
// --------------------------------------------------------------------------

TEST(a_packet_is_learned_once_and_dropped_once_acknowledged) {
  LinkNode a(kMatch, three(), 0, {1});
  LinkNode b(kMatch, three(), 1, {0});
  a.offer(own(0, 2));
  CHECK(a.held() == 1);

  const std::vector<std::byte> first = a.datagram_for(1, 10);
  LinkReceipt receipt = b.receive(first, 20);
  CHECK(receipt.status == LinkReceipt::Status::ok);
  REQUIRE(receipt.packets.size() == 1);
  CHECK(receipt.packets[0].turn == 2);
  CHECK(receipt.packets[0].peer == 0);

  // Resent until acknowledged, and a resend is a duplicate, not news.
  receipt = b.receive(a.datagram_for(1, 30), 40);
  CHECK(receipt.packets.empty());
  CHECK(receipt.duplicates == 1);

  // B says it holds turn 2 from A; A stops carrying it.
  CHECK(a.receive(b.datagram_for(0, 50), 60).status == LinkReceipt::Status::ok);
  CHECK(a.held() == 0);
  CHECK(parsed(a.datagram_for(1, 70)).packets.empty());
}

TEST(a_lost_datagram_is_repaired_by_the_next) {
  LinkNode a(kMatch, three(), 0, {1});
  LinkNode b(kMatch, three(), 1, {0});
  a.offer(own(0, 2));
  (void)a.datagram_for(1, 0);  // lost
  a.offer(own(0, 3));
  const Datagram next = parsed(a.datagram_for(1, 40));
  CHECK(turns_in(next) == (std::vector<std::uint32_t>{2, 3}));
  const LinkReceipt receipt = b.receive(encode(next), 50);
  CHECK(receipt.packets.size() == 2);
}

TEST(the_acknowledgement_is_contiguous) {
  // Turn 3 arrives before turn 2: B holds 3, but its acknowledgement must
  // still say "below 2", or A would stop resending the one B is missing.
  LinkNode a(kMatch, three(), 0, {1});
  LinkNode b(kMatch, three(), 1, {0});
  a.offer(own(0, 3));
  CHECK(b.receive(a.datagram_for(1, 0), 1).packets.size() == 1);
  a.offer(own(0, 2));
  CHECK(a.receive(b.datagram_for(0, 2), 3).status == LinkReceipt::Status::ok);
  CHECK(turns_in(parsed(a.datagram_for(1, 4))) == (std::vector<std::uint32_t>{2, 3}));
  CHECK(b.receive(a.datagram_for(1, 5), 6).packets.size() == 1);
  CHECK(a.receive(b.datagram_for(0, 7), 8).status == LinkReceipt::Status::ok);
  CHECK(a.held() == 0);
}

TEST(a_relay_forwards_everywhere_but_back) {
  LinkNode hub(kMatch, three(), 0, {1, 2});
  LinkNode left(kMatch, three(), 1, {0});
  LinkNode right(kMatch, three(), 2, {0});
  left.offer(own(1, 2));
  REQUIRE(hub.receive(left.datagram_for(0, 0), 1).packets.size() == 1);

  // Onward to the other client, not back to the one it came from.
  CHECK(turns_in(parsed(hub.datagram_for(2, 2))) == (std::vector<std::uint32_t>{2}));
  CHECK(turns_in(parsed(hub.datagram_for(1, 2))).empty());
  const LinkReceipt receipt = right.receive(hub.datagram_for(2, 3), 4);
  REQUIRE(receipt.packets.size() == 1);
  CHECK(receipt.packets[0].peer == 1);

  // And a client never offers the hub what the hub gave it.
  CHECK(turns_in(parsed(right.datagram_for(0, 5))).empty());

  // Once the far client has it, the hub lets it go.
  CHECK(hub.held() == 1);
  CHECK(hub.receive(right.datagram_for(0, 6), 7).status == LinkReceipt::Status::ok);
  CHECK(hub.held() == 0);
}

TEST(a_mesh_does_not_echo_a_turn_past_a_gap) {
  // The one topology where it matters. In a star a client drops a packet as
  // soon as it has it -- its only link is where it came from -- and the hub
  // learns every packet from the origin, which already acknowledges it. In a
  // mesh of four, C learns A's turn 3 from B while B is missing A's turn 2, so
  // B's acknowledgement for A still says "below 2". Only the record of which
  // link the packet came from stops C offering it straight back.
  LockstepConfig config;
  config.peers = {0, 1, 2, 3};
  LinkNode b(kMatch, config, 1, {0, 2, 3});
  LinkNode c(kMatch, config, 2, {0, 1, 3});
  Datagram only_three;  // A's datagram carrying turn 2 was lost on the way to B
  only_three.match = kMatch;
  only_three.from = 0;
  only_three.hold = kNoEcho;
  only_three.packets = {encode(own(0, 3))};
  REQUIRE(b.receive(encode(only_three), 1).packets.size() == 1);
  REQUIRE(c.receive(b.datagram_for(2, 2), 3).packets.size() == 1);
  CHECK(c.held() == 1);  // D has not got it yet
  CHECK(turns_in(parsed(c.datagram_for(1, 4))).empty());
  CHECK(turns_in(parsed(c.datagram_for(3, 4))) == (std::vector<std::uint32_t>{3}));
}

TEST(a_relay_sends_each_client_only_what_that_client_lacks) {
  LockstepConfig config;
  config.peers = {0, 1, 2, 3};
  LinkNode hub(kMatch, config, 0, {1, 2, 3});
  LinkNode left(kMatch, config, 1, {0});
  LinkNode right(kMatch, config, 2, {0});
  left.offer(own(1, 2));
  REQUIRE(hub.receive(left.datagram_for(0, 0), 1).packets.size() == 1);
  const std::vector<std::byte> stale_ack = right.datagram_for(0, 2);  // before it has it
  REQUIRE(right.receive(hub.datagram_for(2, 3), 4).packets.size() == 1);
  REQUIRE(hub.receive(right.datagram_for(0, 5), 6).status == LinkReceipt::Status::ok);
  // Held still -- the third client has not got it -- but not for the right.
  CHECK(hub.held() == 1);
  CHECK(turns_in(parsed(hub.datagram_for(2, 7))).empty());
  CHECK(turns_in(parsed(hub.datagram_for(3, 7))) == (std::vector<std::uint32_t>{2}));
  // A reordered acknowledgement from before cannot take that back.
  REQUIRE(hub.receive(stale_ack, 8).status == LinkReceipt::Status::ok);
  CHECK(turns_in(parsed(hub.datagram_for(2, 9))).empty());
}

TEST(a_packet_offered_again_after_it_was_dropped_stays_dropped) {
  LinkNode a(kMatch, three(), 0, {1});
  LinkNode b(kMatch, three(), 1, {0});
  a.offer(own(0, 2));
  REQUIRE(b.receive(a.datagram_for(1, 0), 1).packets.size() == 1);
  REQUIRE(a.receive(b.datagram_for(0, 2), 3).status == LinkReceipt::Status::ok);
  REQUIRE(a.held() == 0);
  a.offer(own(0, 2));
  CHECK(a.held() == 0);
}

TEST(the_proposal_is_the_slowest_link_rounded_up) {
  // Delay 3 spreads the round trip over eight half-turns: ceil(3 * 60 / 8)
  // is 23, where a floor would say 22.
  LinkNode hub(kMatch, three(3), 0, {1, 2});
  LinkNode fast(kMatch, three(3), 1, {0});
  LinkNode slow(kMatch, three(3), 2, {0});
  REQUIRE(fast.receive(hub.datagram_for(1, 0), 10).status == LinkReceipt::Status::ok);
  REQUIRE(hub.receive(fast.datagram_for(0, 10), 20).status == LinkReceipt::Status::ok);
  CHECK(hub.srtt(1) == 20);
  REQUIRE(slow.receive(hub.datagram_for(2, 0), 30).status == LinkReceipt::Status::ok);
  REQUIRE(hub.receive(slow.datagram_for(0, 30), 60).status == LinkReceipt::Status::ok);
  CHECK(hub.srtt(2) == 60);
  CHECK(hub.proposal() == 23);
}

TEST(the_round_trip_is_now_less_echo_less_hold) {
  LinkNode a(kMatch, three(), 0, {1});
  LinkNode b(kMatch, three(), 1, {0});
  CHECK(a.srtt(1) == 0);
  CHECK(a.proposal() == kDefaultTurnLength);  // nothing measured yet

  REQUIRE(b.receive(a.datagram_for(1, 100), 130).status == LinkReceipt::Status::ok);
  // B holds A's stamp 20 ms before answering; A hears back at 180.
  REQUIRE(a.receive(b.datagram_for(0, 150), 180).status == LinkReceipt::Status::ok);
  CHECK(a.srtt(1) == 60);  // 180 - 100 - 20
  // ceil(3 * 60 / (2 * (2 + 1)))
  CHECK(a.proposal() == 30);

  // Smoothed, in integers: (7 * 60 + 140) / 8.
  REQUIRE(b.receive(a.datagram_for(1, 200), 250).status == LinkReceipt::Status::ok);
  REQUIRE(a.receive(b.datagram_for(0, 250), 340).status == LinkReceipt::Status::ok);
  CHECK(a.srtt(1) == 70);
}

TEST(a_reordered_stamp_does_not_drag_the_echo_back) {
  LinkNode a(kMatch, three(), 0, {1});
  LinkNode b(kMatch, three(), 1, {0});
  const std::vector<std::byte> older = a.datagram_for(1, 100);
  const std::vector<std::byte> newer = a.datagram_for(1, 200);
  REQUIRE(b.receive(newer, 210).status == LinkReceipt::Status::ok);
  REQUIRE(b.receive(older, 220).status == LinkReceipt::Status::ok);
  const Datagram reply = parsed(b.datagram_for(0, 230));
  CHECK(reply.echo == 200);
  CHECK(reply.hold == 20);
}

TEST(what_a_node_refuses) {
  LinkNode a(kMatch, three(), 0, {1});
  LinkNode stranger(kMatch + 1, three(), 1, {0});
  CHECK(a.receive(stranger.datagram_for(0, 0), 1).status == LinkReceipt::Status::wrong_match);

  LinkNode unlinked(kMatch, three(), 2, {0});
  CHECK(a.receive(unlinked.datagram_for(0, 0), 1).status == LinkReceipt::Status::unknown_link);

  std::vector<std::byte> junk = LinkNode(kMatch, three(), 1, {0}).datagram_for(0, 0);
  junk.pop_back();
  CHECK(a.receive(junk, 1).status == LinkReceipt::Status::malformed);

  // Packets no peer may send: not a peer, our own, inside the advance turns,
  // and one that does not decode. The datagram survives; the packets do not.
  Datagram datagram;
  datagram.match = kMatch;
  datagram.from = 1;
  datagram.hold = kNoEcho;
  datagram.packets = {encode(own(9, 2)), encode(own(0, 2)), encode(own(1, 1)), {std::byte{1}},
                      encode(own(1, 2))};
  const LinkReceipt receipt = a.receive(encode(datagram), 1);
  CHECK(receipt.status == LinkReceipt::Status::ok);
  CHECK(receipt.refused == 4);
  CHECK(receipt.packets.size() == 1);
}

TEST(a_node_notices_two_versions_of_one_packet) {
  LinkNode hub(kMatch, three(), 0, {1, 2});
  Datagram datagram;
  datagram.match = kMatch;
  datagram.from = 1;
  datagram.hold = kNoEcho;
  datagram.packets = {encode(own(1, 2, 1))};
  REQUIRE(hub.receive(encode(datagram), 0).packets.size() == 1);
  datagram.packets = {encode(own(1, 2, 3))};
  const LinkReceipt receipt = hub.receive(encode(datagram), 1);
  CHECK(receipt.packets.empty());
  CHECK(receipt.conflicting == 1);
  // The first version is what goes onward.
  const Datagram onward = parsed(hub.datagram_for(2, 2));
  REQUIRE(onward.packets.size() == 1);
  TurnPacket packet;
  REQUIRE(decode(onward.packets[0], packet) == DecodeStatus::ok);
  CHECK(packet.orders[0].actors.size() == 1);
}

TEST(the_budget_keeps_the_oldest_turn) {
  LinkNode hub(kMatch, three(4), 0, {1, 2});
  // Big packets from two origins, the newest turn offered first.
  for (std::uint32_t turn = 8; turn >= 4; --turn) hub.offer(own(0, turn, 60));
  Datagram datagram;
  datagram.match = kMatch;
  datagram.from = 1;
  datagram.hold = kNoEcho;
  datagram.packets = {encode(own(1, 4, 60))};
  REQUIRE(hub.receive(encode(datagram), 0).packets.size() == 1);

  const std::vector<std::byte> bytes = hub.datagram_for(2, 1);
  const Datagram out = parsed(bytes);
  const std::vector<std::uint32_t> turns = turns_in(out);
  REQUIRE(!turns.empty());
  CHECK(turns.size() < 6);  // did not fit, so something waited
  CHECK(turns.front() == 4);
  CHECK(turns[1] == 4);     // both origins' turn 4 before anybody's 5
  std::size_t packed = 0;
  for (const auto& packet : out.packets) packed += packet.size() + 4;
  CHECK(packed <= kDatagramBudget);

  // One packet over the budget is still sent, alone.
  LinkNode big(kMatch, three(), 0, {1});
  big.offer(own(0, 2, 400));
  CHECK(parsed(big.datagram_for(1, 0)).packets.size() == 1);
}

// --------------------------------------------------------------------------
// netplay through the link layer
// --------------------------------------------------------------------------

TEST(a_star_losing_a_quarter_of_its_datagrams_plays_one_game) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 3, 5, 6};
  options.seed = 0x77;
  options.relay = true;
  options.loss_per_mille = 250;
  const CommandStream intent = intent_for({0, 3, 5, 6}, 30);
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 11, 30, intent, options, &report);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("  %s\n", divergence.describe().c_str());
  CHECK(!report.deadlocked);
  CHECK(report.turns == 30);
  CHECK(report.lost > report.datagrams / 6);
  CHECK(report.lost < report.datagrams / 3);
  CHECK(report.refused == 0);
  CHECK(report.duplicates > 0);  // the resends, arriving anyway
  CHECK(report.stalls > 0);
  // 28 intent turns fit, two in three carry four orders.
  CHECK(report.orders == 19 * 4);
  // The forwarding store stays within the protocol's window: at most every
  // peer's packets for every turn an honest peer can be ahead.
  CHECK(report.most_held <= 4 * (2 * 2 + 2));
}

TEST(proposals_from_measured_round_trips_are_agreed) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {1, 2, 4};
  options.lockstep.min_ms = 20;  // low enough that the measurement shows
  options.relay = true;
  options.measured_proposals = true;
  options.round_ms = 60;
  options.loss_per_mille = 100;
  const CommandStream intent = intent_for({1, 2, 4}, 24);
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 5, 24, intent, options, &report);
  CHECK(!divergence.diverged());
  REQUIRE(report.schedule.size() == 24);
  // The advance turns run at the initial length; once round trips are
  // measured the proposals move off it.
  CHECK(report.schedule[0] == kDefaultTurnLength);
  bool measured = false;
  for (std::size_t t = 2; t < report.schedule.size(); ++t) {
    if (report.schedule[t] != kDefaultTurnLength) measured = true;
    CHECK(report.schedule[t] >= 20);
  }
  CHECK(measured);
}

TEST(a_link_that_loses_everything_is_a_deadlock_not_a_pass) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 1};
  options.relay = true;
  options.loss_per_mille = 1000;
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 1, 8, intent_for({0, 1}, 8), options, &report);
  CHECK(divergence.kind == conformance::Divergence::Kind::unbuildable);
  CHECK(report.deadlocked);
  // It ran the turns agreed in advance and not one more.
  CHECK(report.turns == 2);
}
