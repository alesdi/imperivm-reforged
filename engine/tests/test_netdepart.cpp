// A peer that leaves mid-match: sim/netdepart.hpp, and what it asks of the
// negotiator and the link layer.
//
// What carries the reading:
//
//   * A peer that learns of a departure runs nothing past what it already
//     holds of the departed peer's -- even a packet that arrives afterwards --
//     until the match decides, because the decision may end that peer's part
//     before it.
//   * The decision is the largest report of every peer still playing, taken by
//     the coordinator alone, and every peer applies exactly it: the departed
//     peer's packets below it are waited for, none from it on is taken, and
//     its proposal stops counting.
//   * The first turn without the departed peer carries its takeover -- the
//     computer takes the seat there -- written by the negotiator into the
//     agreed history, and no peer can send one.
//   * A decision behind what a peer has run is refused rather than applied.
//   * Over a star losing datagrams, the peers that stay play one game, the one
//     that left played the same game up to its end, and the coordinator
//     leaving ends the match instead of stalling it.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/conformance.hpp"
#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netdepart.hpp"
#include "imperivm/core/sim/netlink.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace conformance = imperivm::core::sim::conformance;
using imperivm::core::NativeClass;

namespace {

constexpr std::uint32_t kMatch = 0xD1CE;

LockstepConfig peers_of(std::vector<PlayerId> peers, std::uint32_t delay = 2) {
  LockstepConfig config;
  config.peers = std::move(peers);
  config.input_delay = delay;
  return config;
}

TurnPacket packet(PlayerId peer, std::uint32_t turn, std::int32_t proposed_ms = 400,
                  std::size_t orders = 1) {
  TurnPacket out;
  out.peer = peer;
  out.turn = turn;
  out.proposed_ms = proposed_ms;
  for (std::size_t i = 0; i < orders; ++i) {
    NetOrder order;
    order.issuer = peer;
    order.sequence = static_cast<std::uint32_t>(i);
    order.actors = {static_cast<ObjectId>(10 + peer)};
    out.orders.push_back(order);
  }
  return out;
}

/// Run every turn the negotiator is ready for, submitting an empty packet
/// after each the way a peer does; how many ran. Bounded, because a peer whose
/// every other peer has been dropped is always ready.
std::size_t run_and_submit(TurnNegotiator& negotiator, std::vector<AgreedTurn>* taken = nullptr) {
  std::size_t ran = 0;
  while (ran < 16) {
    std::optional<AgreedTurn> turn = negotiator.take();
    if (!turn.has_value()) break;
    if (taken != nullptr) taken->push_back(*turn);
    (void)negotiator.submit({}, 400);
    ++ran;
  }
  return ran;
}

/// Orders `peer` gave, as opposed to its departure the negotiator wrote in.
bool has_order_from(const AgreedTurn& turn, PlayerId peer) {
  for (const NetOrder& order : turn.orders.orders) {
    if (order.issuer == peer && order.kind != NetOrderKind::departed) return true;
  }
  return false;
}

/// How many takeovers of `peer`'s seat the turn carries.
std::size_t takeovers(const AgreedTurn& turn, PlayerId peer) {
  std::size_t count = 0;
  for (const NetOrder& order : turn.orders.orders) {
    if (order.issuer == peer && order.kind == NetOrderKind::departed) ++count;
  }
  return count;
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

CommandStream intent_for(const std::vector<PlayerId>& players, std::size_t turns) {
  CommandStream intent;
  intent.turns.resize(turns);
  for (std::size_t t = 0; t < turns; ++t) {
    if (t % 3 == 2) continue;
    for (const PlayerId player : players) {
      NetOrder order;
      order.issuer = player;
      order.actors = {static_cast<ObjectId>(1 + t % 2)};
      order.target.point = Point{static_cast<std::int32_t>(t), player};
      intent.turns[t].orders.push_back(order);
    }
  }
  return intent;
}

}  // namespace

// --------------------------------------------------------------------------
// the negotiator
// --------------------------------------------------------------------------

TEST(suspend_reports_what_is_held_contiguously_and_freezes_there) {
  TurnNegotiator negotiator(peers_of({0, 1, 2}), 0);
  REQUIRE(negotiator.submit({}, 400).has_value());  // turn 2
  CHECK(negotiator.receive(packet(1, 2)) == ReceiveStatus::accepted);
  CHECK(negotiator.receive(packet(2, 2)) == ReceiveStatus::accepted);
  CHECK(negotiator.receive(packet(2, 4)) == ReceiveStatus::accepted);  // past a gap
  // Held: turn 2. Turn 4 is past the gap at 3, and is not reported.
  CHECK(negotiator.suspend(2) == 3);
  CHECK(negotiator.suspended(2));
  // The gap filling afterwards changes neither the report nor what may run.
  CHECK(negotiator.receive(packet(2, 3)) == ReceiveStatus::accepted);
  CHECK(negotiator.suspend(2) == 3);
  CHECK(run_and_submit(negotiator) == 3);  // the two advance turns, and turn 2
  CHECK(negotiator.receive(packet(1, 3)) == ReceiveStatus::accepted);
  // Every packet for turn 3 is in hand, and turn 3 still does not run.
  CHECK(!negotiator.ready());
  CHECK(negotiator.next_turn() == 3);
  // A peer that is not in the match is not suspended.
  CHECK(negotiator.suspend(9) == 3);
  CHECK(!negotiator.suspended(9));
}

TEST(a_drop_ends_the_departed_peers_part_at_the_agreed_turn) {
  TurnNegotiator negotiator(peers_of({0, 1, 2}), 0);
  (void)negotiator.submit({}, 400);  // our turn 2
  for (const std::uint32_t turn : {2u, 3u}) {
    CHECK(negotiator.receive(packet(1, turn)) == ReceiveStatus::accepted);
    CHECK(negotiator.receive(packet(2, turn, /*proposed_ms=*/700)) == ReceiveStatus::accepted);
  }
  CHECK(negotiator.suspend(2) == 4);
  std::vector<AgreedTurn> taken;
  CHECK(run_and_submit(negotiator, &taken) == 4);  // 0 .. 3, then frozen at 4
  REQUIRE(negotiator.drop(2, 4));
  CHECK(negotiator.dropped_from(2) == std::optional<std::uint32_t>(4));
  REQUIRE(taken.size() == 4);
  CHECK(has_order_from(taken[3], 2));
  CHECK(taken[3].real_ms == 700);  // its proposal still counts on its own turns
  // From 4 on: not waited for, not taken, not its proposal.
  CHECK(negotiator.receive(packet(1, 4)) == ReceiveStatus::accepted);
  CHECK(negotiator.receive(packet(2, 4, 1600)) == ReceiveStatus::departed);
  CHECK(run_and_submit(negotiator, &taken) == 1);
  REQUIRE(taken.size() == 5);
  CHECK(!has_order_from(taken[4], 2));
  CHECK(taken[4].real_ms == 400);
  // The first turn without it carries its takeover, once; no other does.
  CHECK(takeovers(taken[4], 2) == 1);
  for (std::size_t t = 0; t < 4; ++t) CHECK(takeovers(taken[t], 2) == 0);
  CHECK(negotiator.receive(packet(1, 5)) == ReceiveStatus::accepted);
  CHECK(run_and_submit(negotiator, &taken) == 1);
  REQUIRE(taken.size() == 6);
  CHECK(takeovers(taken[5], 2) == 0);
  // And the history carries it, so a replay of the history does it too.
  CHECK(negotiator.history().turns[4].orders.size() == 2);
  CHECK(!negotiator.plays(2, 4));
  CHECK(negotiator.plays(2, 3));
}

TEST(a_drop_forgets_what_the_departed_peer_sent_past_its_end) {
  TurnNegotiator negotiator(peers_of({0, 1}), 0);
  (void)negotiator.submit({}, 400);
  for (const std::uint32_t turn : {2u, 3u, 4u}) {
    CHECK(negotiator.receive(packet(1, turn, 1200)) == ReceiveStatus::accepted);
  }
  REQUIRE(negotiator.drop(1, 3));
  std::vector<AgreedTurn> taken;
  CHECK(run_and_submit(negotiator, &taken) >= 4);
  REQUIRE(taken.size() >= 4);
  CHECK(has_order_from(taken[2], 1));
  CHECK(!has_order_from(taken[3], 1));
  CHECK(taken[3].real_ms == 400);
}

TEST(turns_below_the_end_wait_for_the_departed_peers_packets) {
  TurnNegotiator negotiator(peers_of({0, 1}), 0);
  (void)negotiator.submit({}, 400);
  CHECK(negotiator.suspend(1) == 2);  // holds nothing of peer 1's
  // Someone else held peer 1's turns 2 and 3: the end is 4.
  REQUIRE(negotiator.drop(1, 4));
  CHECK(run_and_submit(negotiator) == 2);  // the advance turns only
  CHECK(!negotiator.ready());
  CHECK(negotiator.receive(packet(1, 2)) == ReceiveStatus::accepted);
  std::vector<AgreedTurn> taken;
  CHECK(run_and_submit(negotiator, &taken) == 1);  // turn 3 waits for its packet
  REQUIRE(taken.size() == 1);
  CHECK(has_order_from(taken[0], 1));
  CHECK(negotiator.receive(packet(1, 3)) == ReceiveStatus::accepted);
  // Turn 3 with it, and turn 4 without.
  CHECK(run_and_submit(negotiator, &taken) >= 2);
  REQUIRE(taken.size() >= 3);
  CHECK(has_order_from(taken[1], 1));
  CHECK(!has_order_from(taken[2], 1));
}

TEST(a_drop_behind_what_was_run_is_refused) {
  TurnNegotiator negotiator(peers_of({0, 1}), 0);
  (void)negotiator.submit({}, 400);
  CHECK(negotiator.receive(packet(1, 2)) == ReceiveStatus::accepted);
  CHECK(run_and_submit(negotiator) == 3);  // turns 0, 1 and 2, the last with peer 1's
  CHECK(!negotiator.drop(1, 2));
  CHECK(!negotiator.dropped_from(1).has_value());
  CHECK(negotiator.drop(1, 3));
  CHECK(negotiator.drop(1, 3));   // the same decision again is the same
  CHECK(!negotiator.drop(1, 5));  // a different one is not
  // An end inside the advance turns is the first turn that carries packets.
  TurnNegotiator early(peers_of({0, 1}), 0);
  CHECK(early.drop(1, 0));
  CHECK(early.dropped_from(1) == std::optional<std::uint32_t>(2));
  CHECK(!early.drop(9, 4));
}

TEST(a_dropped_peer_submits_nothing_past_its_end) {
  TurnNegotiator kept(peers_of({0, 1}), 1);
  TurnNegotiator dropped(peers_of({0, 1}), 1);
  REQUIRE(dropped.drop(1, 3));
  for (TurnNegotiator* negotiator : {&kept, &dropped}) {
    CHECK(negotiator->submit({}, 400).has_value());  // turn 2: still its own
    CHECK(negotiator->take().has_value());
  }
  CHECK(kept.submit({}, 400).has_value());      // turn 3
  CHECK(!dropped.submit({}, 400).has_value());  // turn 3: past the end
}

namespace {

class TakeoverSink final : public NetCommandSink {
 public:
  bool command(const NetOrder&) override { return false; }
  bool surrender(PlayerId) override { return false; }
  bool take_over(PlayerId departed) override {
    taken.push_back(departed);
    return true;
  }
  std::vector<PlayerId> taken;
};

}  // namespace

TEST(a_takeover_reaches_the_sink_and_nothing_else_applies_it) {
  NetTurn turn;
  NetOrder takeover;
  takeover.issuer = 3;
  takeover.kind = NetOrderKind::departed;
  turn.orders.push_back(takeover);
  World world;
  TakeoverSink sink;
  const NetTurnReport applied = apply_turn(world, turn, nullptr, &sink);
  CHECK(applied.commands == 1);
  CHECK(sink.taken == std::vector<PlayerId>{3});
  // Without a sink it is counted, the same on every peer, and not guessed at.
  CHECK(apply_turn(world, turn, nullptr, nullptr).unapplied == 1);
}

TEST(no_peer_can_say_another_has_left) {
  // A takeover is written by the negotiator, never sent: the wire refuses
  // one, receive refuses one handed over directly, and submit drops one.
  TurnPacket sent = packet(1, 3);
  sent.orders[0].kind = NetOrderKind::departed;
  std::vector<std::byte> bytes = encode(sent);
  TurnPacket back;
  CHECK(decode(bytes, back) == DecodeStatus::bad_field);
  TurnNegotiator negotiator(peers_of({0, 1}), 0);
  CHECK(negotiator.receive(sent) == ReceiveStatus::malformed);
  NetOrder mine;
  mine.kind = NetOrderKind::departed;
  const std::optional<TurnPacket> submitted = negotiator.submit({mine}, 400);
  REQUIRE(submitted.has_value());
  CHECK(submitted->orders.empty());
}

// --------------------------------------------------------------------------
// the link layer
// --------------------------------------------------------------------------

TEST(departure_facts_survive_the_wire_and_every_prefix_is_refused) {
  Datagram datagram;
  datagram.match = kMatch;
  datagram.from = 1;
  datagram.acks = {{0, 4}};
  datagram.packets = {encode(packet(1, 3))};
  datagram.reports = {{2, 1, 7}, {2, 0, 5}};
  datagram.drops = {{2, 7}};
  const std::vector<std::byte> bytes = encode(datagram);
  Datagram back;
  REQUIRE(decode(bytes, back) == DecodeStatus::ok);
  REQUIRE(back.reports.size() == 2);
  CHECK(back.reports[0].departed == 2);
  CHECK(back.reports[0].reporter == 1);
  CHECK(back.reports[0].held == 7);
  CHECK(back.reports[1].reporter == 0);
  REQUIRE(back.drops.size() == 1);
  CHECK(back.drops[0].departed == 2);
  CHECK(back.drops[0].from_turn == 7);
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    Datagram out;
    CHECK(decode(std::span<const std::byte>(bytes.data(), cut), out) != DecodeStatus::ok);
  }
  std::vector<std::byte> longer = bytes;
  longer.push_back(std::byte{0});
  Datagram out;
  CHECK(decode(longer, out) == DecodeStatus::trailing);
}

TEST(a_departed_link_is_forgotten_and_what_waited_on_it_is_released) {
  const LockstepConfig config = peers_of({0, 1, 2});
  LinkNode hub(kMatch, config, 0, {1, 2});
  LinkNode one(kMatch, config, 1, {0});
  LinkNode two(kMatch, config, 2, {0});
  hub.offer(packet(0, 2));
  (void)one.receive(hub.datagram_for(1, 0), 0);
  (void)hub.receive(one.datagram_for(0, 0), 0);  // peer 1 acknowledges
  CHECK(hub.held() == 1);                        // peer 2 never will
  const std::vector<std::byte> from_two = two.datagram_for(0, 0);
  hub.depart(2);
  CHECK(hub.held() == 0);
  CHECK(hub.links() == std::vector<PlayerId>{1});
  CHECK(hub.departed() == std::vector<PlayerId>{2});
  // Its datagrams still arriving are late, not a stranger's.
  CHECK(hub.receive(from_two, 1).status == LinkReceipt::Status::departed_link);
  LinkNode other(kMatch, config, 0, {1});
  CHECK(other.receive(from_two, 1).status == LinkReceipt::Status::unknown_link);
}

TEST(reports_and_drops_travel_through_a_relay_and_a_drop_cuts_the_store) {
  const LockstepConfig config = peers_of({0, 1, 2});
  LinkNode hub(kMatch, config, 0, {1, 2});
  LinkNode one(kMatch, config, 1, {0});
  LinkNode two(kMatch, config, 2, {0});
  for (std::uint32_t turn = 2; turn < 6; ++turn) two.offer(packet(2, turn));
  const LinkReceipt learned = hub.receive(two.datagram_for(0, 0), 0);
  CHECK(learned.packets.size() == 4);
  CHECK(hub.held() == 4);  // peer 1 has acknowledged none
  hub.report(2, 6);
  hub.decide(2, 4);
  CHECK(hub.held() == 2);  // turns 4 and 5 are past the end
  const LinkReceipt relayed = one.receive(hub.datagram_for(1, 0), 0);
  CHECK(relayed.departures_changed);
  CHECK(relayed.packets.size() == 2);
  CHECK(one.drops().at(2) == 4);
  CHECK(one.reports().at({2, 0}) == 6);
  CHECK(one.departed() == std::vector<PlayerId>{2});
  // A node that holds peer 2's packets past the end and learns the end from
  // somebody else forgets them too.
  LinkNode two_meshed(kMatch, config, 2, {0, 1});
  for (std::uint32_t turn = 2; turn < 6; ++turn) two_meshed.offer(packet(2, turn));
  LinkNode mid(kMatch, config, 1, {0, 2});
  CHECK(mid.receive(two_meshed.datagram_for(1, 0), 0).packets.size() == 4);
  CHECK(mid.held() == 4);
  LinkNode decider(kMatch, config, 0, {1});  // holds none of peer 2's
  decider.decide(2, 4);
  (void)mid.receive(decider.datagram_for(1, 0), 0);
  CHECK(mid.drops().at(2) == 4);
  CHECK(mid.held() == 2);  // turns 2 and 3, for the decider, which lacks them
  // Peer 2's turn 5, arriving from somewhere after all: late, not refused.
  Datagram carrying;
  carrying.match = kMatch;
  carrying.from = 0;
  carrying.hold = kNoEcho;
  carrying.packets = {encode(packet(2, 5))};
  const LinkReceipt late = one.receive(encode(carrying), 1);
  CHECK(late.late == 1);
  CHECK(late.refused == 0);
  CHECK(late.packets.empty());
  // The same facts again change nothing; a different drop is a conflict.
  CHECK(!one.receive(hub.datagram_for(1, 1), 1).departures_changed);
  carrying.packets.clear();
  carrying.drops = {{2, 9}};
  const LinkReceipt conflict = one.receive(encode(carrying), 2);
  CHECK(conflict.conflicting == 1);
  CHECK(one.drops().at(2) == 4);
}

TEST(what_a_node_refuses_of_departure_facts) {
  const LockstepConfig config = peers_of({0, 1});
  LinkNode node(kMatch, config, 0, {1});
  Datagram datagram;
  datagram.match = kMatch;
  datagram.from = 1;
  datagram.hold = kNoEcho;
  datagram.reports = {{9, 1, 3}, {1, 1, 3}};  // a stranger; a peer on itself
  datagram.drops = {{9, 3}};
  const LinkReceipt receipt = node.receive(encode(datagram), 0);
  CHECK(receipt.refused == 3);
  CHECK(!receipt.departures_changed);
  CHECK(node.departed().empty());
}

// --------------------------------------------------------------------------
// the agreement
// --------------------------------------------------------------------------

namespace {

/// A peer: its negotiator and its link node.
struct Peer {
  Peer(const LockstepConfig& config, PlayerId self, std::vector<PlayerId> links)
      : negotiator(config, self), node(kMatch, config, self, std::move(links)) {}
  TurnNegotiator negotiator;
  LinkNode node;
  std::vector<AgreedTurn> taken;

  /// One order every turn, so that whose orders a turn carries shows.
  void offer_next() {
    NetOrder order;
    order.actors = {static_cast<ObjectId>(10 + node.self())};
    if (std::optional<TurnPacket> mine = negotiator.submit({order}, 400)) node.offer(*mine);
  }
  void run() {
    while (std::optional<AgreedTurn> turn = negotiator.take()) {
      taken.push_back(*turn);
      offer_next();
    }
  }
};

void deliver(Peer& from, Peer& to, std::uint32_t now) {
  const LinkReceipt receipt = to.node.receive(from.node.datagram_for(to.node.self(), now), now);
  for (const TurnPacket& learned : receipt.packets) (void)to.negotiator.receive(learned);
}

}  // namespace

TEST(the_coordinator_decides_only_once_every_peer_still_playing_has_reported) {
  const LockstepConfig config = peers_of({0, 1, 2});
  Peer hub(config, 0, {1, 2});
  Peer one(config, 1, {0, 2});  // linked to the departed peer too
  hub.node.depart(2);
  DepartureOutcome outcome = settle_departures(hub.negotiator, hub.node);
  CHECK(outcome.news);
  CHECK(hub.node.reports().count({2, 0}) == 1);
  CHECK(hub.node.drops().empty());  // peer 1 has not reported
  // Peer 1 learns of it from the hub's report, freezes and reports back.
  deliver(hub, one, 0);
  CHECK(one.node.departed() == std::vector<PlayerId>{2});
  // Learned, not seen, and still no longer a link: it will never acknowledge.
  CHECK(one.node.links() == std::vector<PlayerId>{0});
  outcome = settle_departures(one.negotiator, one.node);
  CHECK(one.negotiator.suspended(2));
  CHECK(one.node.drops().empty());  // it is not the coordinator
  deliver(one, hub, 1);
  outcome = settle_departures(hub.negotiator, hub.node);
  REQUIRE(hub.node.drops().count(2) == 1);
  CHECK(outcome.applied.size() == 1);
  CHECK(hub.negotiator.dropped_from(2).has_value());
  deliver(hub, one, 2);
  outcome = settle_departures(one.negotiator, one.node);
  REQUIRE(outcome.applied.size() == 1);
  CHECK(outcome.applied[0].departed == 2);
  CHECK(one.negotiator.dropped_from(2) == hub.negotiator.dropped_from(2));
  // Nothing new: nothing done.
  outcome = settle_departures(one.negotiator, one.node);
  CHECK(!outcome.news);
  CHECK(outcome.applied.empty());
}

TEST(the_end_is_the_largest_report_not_the_coordinators_own) {
  // A mesh where the coordinator hears nothing for a while: peers 1 and 2
  // run turns with each other's packets and the coordinator's, and the
  // coordinator holds none of peer 2's when peer 2 leaves.
  const LockstepConfig config = peers_of({0, 1, 2});
  Peer zero(config, 0, {1, 2});
  Peer one(config, 1, {0, 2});
  Peer two(config, 2, {0, 1});
  for (Peer* peer : {&zero, &one, &two}) peer->offer_next();
  std::uint32_t now = 0;
  // Everything but what goes to peer 0.
  for (int round = 0; round < 6; ++round, ++now) {
    for (Peer* from : {&zero, &one, &two}) {
      for (Peer* to : {&one, &two}) {
        if (from != to) deliver(*from, *to, now);
      }
    }
    for (Peer* peer : {&zero, &one, &two}) peer->run();
  }
  const std::size_t one_ran = one.taken.size();
  CHECK(one_ran > zero.taken.size());
  // Peer 2 leaves; both others see it.
  zero.node.depart(2);
  one.node.depart(2);
  for (int round = 0; round < 20; ++round, ++now) {
    for (Peer* peer : {&zero, &one}) {
      REQUIRE(settle_departures(peer->negotiator, peer->node).state ==
              DepartureOutcome::State::playing);
    }
    deliver(zero, one, now);
    deliver(one, zero, now);
    for (Peer* peer : {&zero, &one}) peer->run();
  }
  const std::optional<std::uint32_t> end = zero.negotiator.dropped_from(2);
  REQUIRE(end.has_value());
  CHECK(one.negotiator.dropped_from(2) == end);
  // The end is peer 1's report -- the turns it ran with peer 2's orders.
  CHECK(*end == one.node.reports().at({2, 1}));
  CHECK(*end > zero.node.reports().at({2, 0}));
  CHECK(*end >= one_ran);
  // And both applied exactly peer 2's turns below it: the peer that held
  // them passed them on.
  REQUIRE(zero.taken.size() > *end);
  REQUIRE(one.taken.size() > *end);
  for (std::uint32_t t = 0; t < zero.taken.size() && t < one.taken.size(); ++t) {
    CHECK(has_order_from(zero.taken[t], 2) == has_order_from(one.taken[t], 2));
    CHECK(zero.taken[t].real_ms == one.taken[t].real_ms);
  }
}

TEST(the_coordinator_leaving_ends_the_match_and_a_dropped_peer_knows) {
  const LockstepConfig config = peers_of({0, 1, 2});
  Peer one(config, 1, {0});
  one.node.depart(0);
  CHECK(settle_departures(one.negotiator, one.node).state ==
        DepartureOutcome::State::coordinator_left);
  // A coordinator named by the config, not the lowest peer.
  LockstepConfig hosted = config;
  hosted.coordinator = 2;
  Peer zero(hosted, 0, {2});
  zero.node.depart(1);
  CHECK(settle_departures(zero.negotiator, zero.node).state == DepartureOutcome::State::playing);
  zero.node.depart(2);
  CHECK(settle_departures(zero.negotiator, zero.node).state ==
        DepartureOutcome::State::coordinator_left);
  // A decision that drops this peer.
  Peer hub(config, 0, {1, 2});
  Peer two(config, 2, {0});
  hub.node.decide(2, 5);
  deliver(hub, two, 0);
  CHECK(settle_departures(two.negotiator, two.node).state == DepartureOutcome::State::dropped);
}

TEST(a_decision_a_peer_has_run_past_is_irreconcilable) {
  const LockstepConfig config = peers_of({0, 1, 2});
  Peer one(config, 1, {0});
  (void)one.negotiator.submit({}, 400);
  (void)one.negotiator.receive(packet(0, 2));
  (void)one.negotiator.receive(packet(2, 2));
  CHECK(run_and_submit(one.negotiator) == 3);
  Peer hub(config, 0, {1, 2});
  hub.node.decide(2, 2);  // behind what peer 1 ran
  deliver(hub, one, 0);
  CHECK(settle_departures(one.negotiator, one.node).state ==
        DepartureOutcome::State::irreconcilable);
}

TEST(resolved_coordinator_is_the_named_peer_or_the_lowest) {
  LockstepConfig config = peers_of({4, 2, 7});
  CHECK(config.resolved_coordinator() == 2);
  config.coordinator = 7;
  CHECK(config.resolved_coordinator() == 7);
  config.coordinator = 5;  // not in the match
  CHECK(config.resolved_coordinator() == 2);
}

// --------------------------------------------------------------------------
// netplay with departures
// --------------------------------------------------------------------------

TEST(a_peer_leaving_a_lossy_star_is_dropped_and_the_rest_play_one_game) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 3, 5, 6};
  options.seed = 0x51;
  options.relay = true;
  options.loss_per_mille = 250;
  options.departures = {{5, 10, 3}};
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 3, 36, intent_for({0, 3, 5, 6}, 36), options, &report);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("  %s\n", divergence.describe().c_str());
  CHECK(!report.deadlocked);
  CHECK(!report.coordinator_left);
  CHECK(report.turns == 36);
  CHECK(report.refused == 0);
  REQUIRE(report.dropped.size() == 1);
  CHECK(report.dropped[0].first == 5);
  // It ran ten turns, each needing everyone's packet, so the stayers held
  // its packets through turn 9 at least, and its last packet was for 12.
  CHECK(report.dropped[0].second >= 10);
  CHECK(report.dropped[0].second <= 13);
  CHECK(report.stalls > 0);
}

TEST(two_peers_leaving_at_different_times_and_one_before_its_first_turn) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {1, 2, 4, 7};
  options.seed = 9;
  options.relay = true;
  options.loss_per_mille = 150;
  options.departures = {{7, 0, 1}, {4, 14, 6}};
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 8, 30, intent_for({1, 2, 4, 7}, 30), options, &report);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("  %s\n", divergence.describe().c_str());
  CHECK(report.turns == 30);
  CHECK(report.refused == 0);
  REQUIRE(report.dropped.size() == 2);
  // Peer 4 then peer 7, in player order; peer 7 never sent a packet.
  CHECK(report.dropped[0].first == 4);
  CHECK(report.dropped[1].first == 7);
  CHECK(report.dropped[1].second == 2);
  CHECK(report.dropped[0].second >= 14);
}

TEST(two_peers_leaving_together_are_both_dropped) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 1, 2, 3, 4};
  options.seed = 4;
  options.relay = true;
  options.loss_per_mille = 200;
  options.departures = {{2, 9, 2}, {3, 9, 2}};
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 2, 26, intent_for({0, 1, 2, 3, 4}, 26), options, &report);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("  %s\n", divergence.describe().c_str());
  CHECK(report.turns == 26);
  CHECK(report.dropped.size() == 2);
  CHECK(report.refused == 0);
}

TEST(the_last_joiner_leaving_leaves_the_host_playing_alone) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 1};
  options.relay = true;
  options.loss_per_mille = 100;
  options.departures = {{1, 6, 2}};
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 1, 20, intent_for({0, 1}, 20), options, &report);
  CHECK(!divergence.diverged());
  CHECK(report.turns == 20);
  REQUIRE(report.dropped.size() == 1);
}

TEST(the_coordinator_leaving_ends_the_match_rather_than_stalling_it) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 1, 2};
  options.relay = true;
  options.departures = {{0, 5, 2}};
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 1, 20, intent_for({0, 1, 2}, 20), options, &report);
  CHECK(divergence.kind == conformance::Divergence::Kind::unbuildable);
  CHECK(report.coordinator_left);
  CHECK(!report.deadlocked);
}

TEST(departures_without_the_link_layer_are_refused_not_ignored) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 1};
  options.departures = {{1, 3, 2}};
  const conformance::Divergence divergence =
      check_netplay(scenario, 1, 10, intent_for({0, 1}, 10), options);
  CHECK(divergence.kind == conformance::Divergence::Kind::unbuildable);
}

// --------------------------------------------------------------------------
// what the players are shown: the "(AI)" marker
// --------------------------------------------------------------------------

namespace {

NetOrder membership_order(PlayerId seat, NetOrderKind kind) {
  NetOrder order;
  order.issuer = seat;
  order.kind = kind;
  return order;
}

}  // namespace

TEST(a_takeover_marks_the_seat_and_a_hand_back_unmarks_it) {
  ComputerSeats seats(0);
  NetTurn plain;
  plain.orders.push_back(packet(1, 4).orders[0]);
  CHECK(seats.apply(plain).empty());
  CHECK(!seats.marked(1));

  NetTurn gone;
  gone.orders.push_back(membership_order(1, NetOrderKind::departed));
  gone.orders.push_back(membership_order(2, NetOrderKind::departed));
  const ComputerSeats::Change taken = seats.apply(gone);
  // Both in one turn: one change, which is what plays the sound once.
  CHECK(taken.taken == (std::vector<PlayerId>{1, 2}));
  CHECK(taken.handed_back.empty());
  CHECK(seats.marked(1) && seats.marked(2));
  // Appended verbatim, as the original's append is: the marker is the
  // translation, and brings its own spacing if it has any.
  CHECK(seats.display(1, "Brennus", "[m]") == "Brennus[m]");
  CHECK(seats.display(3, "Hannibal", "[m]") == "Hannibal");
  // A second takeover of a marked seat is not news.
  CHECK(seats.apply(gone).empty());

  NetTurn back;
  back.orders.push_back(membership_order(2, NetOrderKind::joined));
  const ComputerSeats::Change returned = seats.apply(back);
  CHECK(returned.taken.empty());
  CHECK(returned.handed_back == std::vector<PlayerId>{2});
  CHECK(!seats.marked(2) && seats.marked(1));
  CHECK(seats.display(2, "Brennus", "[m]") == "Brennus");
  // And left again: marked again, and news again.
  NetTurn again;
  again.orders.push_back(membership_order(2, NetOrderKind::departed));
  CHECK(seats.apply(again).taken == std::vector<PlayerId>{2});
}

TEST(the_local_seat_is_never_marked) {
  ComputerSeats seats(1);
  NetTurn gone;
  gone.orders.push_back(membership_order(1, NetOrderKind::departed));
  CHECK(seats.apply(gone).empty());
  CHECK(!seats.marked(1));
  CHECK(seats.display(1, "Brennus", "[m]") == "Brennus");
}

TEST(a_late_joiner_starts_with_the_seats_the_computer_holds_at_its_first_turn) {
  // Seat 1 left at 10 and is the joiner's, from 20; seat 2 left at 15; seat
  // 3 plays on. Seat 4 was dropped at 25, a decision past the joiner's first
  // turn: it still plays turn 20, and is marked when turn 25 is applied.
  std::vector<SeatStints> membership = {
      {0, {Stint{0, std::nullopt}}},
      {1, {Stint{0, 10}, Stint{20, std::nullopt}}},
      {2, {Stint{0, 15}}},
      {3, {Stint{0, std::nullopt}}},
      {4, {Stint{0, 25}}},
  };
  const ComputerSeats seats = ComputerSeats::resumed(membership, 20, 1);
  CHECK(!seats.marked(0));
  CHECK(!seats.marked(1));
  CHECK(seats.marked(2));
  CHECK(!seats.marked(3));
  CHECK(!seats.marked(4));
  // The same membership seen by a peer that is not seat 1, before seat 1's
  // join: seat 1 is the computer's too.
  const ComputerSeats before = ComputerSeats::resumed(membership, 12, 0);
  CHECK(before.marked(1));
  CHECK(!before.marked(2));
}

TEST(the_marker_is_no_part_of_the_world_or_its_hash) {
  // The rename is the screen's: the world's names and hashes are what they
  // were, before and after, on the peer that shows it.
  World world;
  world.players().setup(1).name = "Brennus";
  const std::uint64_t before = world.hashes().hash_of_hashes;
  ComputerSeats seats(0);
  NetTurn gone;
  gone.orders.push_back(membership_order(1, NetOrderKind::departed));
  (void)seats.apply(gone);
  CHECK(seats.display(1, world.players().setup(1).name, "[m]") == "Brennus[m]");
  CHECK(world.players().setup(1).name == "Brennus");
  CHECK(world.hashes().hash_of_hashes == before);
}
