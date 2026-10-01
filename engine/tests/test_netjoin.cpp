// A late joiner: sim/netjoin.hpp, and what it asks of the negotiator, the
// link layer and the stream hash. All of it is this engine's -- the original
// admits nobody once a match has started -- so these tests pin the rules
// netjoin.hpp states, not a reading.
//
// What carries the design:
//
//   * A seat is joined only after it was dropped, strictly after the turn it
//     was dropped from, and never into a turn this peer has run.
//   * The first turn of the joiner's part carries a `joined` order the
//     negotiator writes in, once, on every peer; its packets are needed from
//     `input_delay` turns later, and the turns between are agreed empty.
//   * A joiner built from the host's membership and stream hash at that turn
//     runs the same turns and folds the same `netcmds` as the host.
//   * The admission rides every datagram beside the drops, facts about one
//     stint never touch another, and a node takes a seat's packets only for
//     turns one of its stints needs.
//   * Over a star losing datagrams, a joiner hashes like everyone else from
//     its first turn -- through a speed change on the turn before it, and
//     through leaving again.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/conformance.hpp"
#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netdepart.hpp"
#include "imperivm/core/sim/netjoin.hpp"
#include "imperivm/core/sim/netlink.hpp"
#include "imperivm/core/sim/netlobby.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace conformance = imperivm::core::sim::conformance;
using imperivm::core::NativeClass;

namespace {

constexpr std::uint32_t kMatch = 0x10E1;

LockstepConfig peers_of(std::vector<PlayerId> peers, std::uint32_t delay = 2) {
  LockstepConfig config;
  config.peers = std::move(peers);
  config.input_delay = delay;
  return config;
}

TurnPacket packet(PlayerId peer, std::uint32_t turn, std::int32_t proposed_ms = 400) {
  TurnPacket out;
  out.peer = peer;
  out.turn = turn;
  out.proposed_ms = proposed_ms;
  NetOrder order;
  order.issuer = peer;
  order.actors = {static_cast<ObjectId>(10 + peer)};
  order.target.point = Point{static_cast<std::int32_t>(turn), peer};
  out.orders.push_back(order);
  return out;
}

std::size_t count_kind(const AgreedTurn& turn, PlayerId peer, NetOrderKind kind) {
  std::size_t count = 0;
  for (const NetOrder& order : turn.orders.orders) {
    if (order.issuer == peer && order.kind == kind) ++count;
  }
  return count;
}

bool has_order_from(const AgreedTurn& turn, PlayerId peer) {
  return count_kind(turn, peer, NetOrderKind::default_order) != 0;
}

/// Take every turn ready, and hand the next packet of each listed peer to the
/// negotiator the way the network would, until `limit` turns have run.
std::vector<AgreedTurn> run_to(TurnNegotiator& negotiator, std::uint32_t limit,
                               const std::vector<PlayerId>& others) {
  std::vector<AgreedTurn> taken;
  for (std::size_t guard = 0; guard < 200 && negotiator.next_turn() < limit; ++guard) {
    (void)negotiator.submit({}, 400);
    for (const PlayerId peer : others) {
      for (std::uint32_t t = negotiator.next_turn(); t <= negotiator.next_turn() + 2; ++t) {
        if (negotiator.required(peer, t)) (void)negotiator.receive(packet(peer, t));
      }
    }
    std::optional<AgreedTurn> turn = negotiator.take();
    if (!turn.has_value()) continue;
    taken.push_back(*turn);
  }
  return taken;
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

/// A save of the bare world, as `GameSession::save` is of a session.
class WorldSnapshots final : public RunSnapshots {
 public:
  [[nodiscard]] std::vector<std::byte> save(conformance::Run& run) const override {
    std::vector<std::byte> out;
    const SaveInputs inputs;
    if (!write_save(run.world(), inputs, out).ok()) out.clear();
    ++saves;
    return out;
  }
  [[nodiscard]] std::unique_ptr<conformance::Run> resume(
      std::uint32_t, std::span<const std::byte> bytes) const override {
    auto run = std::make_unique<OwnedRun>();
    const LoadOptions options;
    const imperivm::core::Result<LoadReport> loaded = read_save(bytes, run->world(), options);
    if (!loaded.ok() || !verify_hashes(run->world(), loaded->meta).ok) return nullptr;
    return run;
  }
  mutable std::size_t saves = 0;
};

/// `WorldSnapshots` whose late joiner's world depends on its seat, as a
/// session's would if `local_player` leaked: one unit more when there is one.
class SeatLeakingSnapshots final : public RunSnapshots {
 public:
  [[nodiscard]] std::vector<std::byte> save(conformance::Run& run) const override {
    return inner.save(run);
  }
  [[nodiscard]] std::unique_ptr<conformance::Run> resume(
      std::uint32_t seed, std::span<const std::byte> bytes) const override {
    return inner.resume(seed, bytes);
  }
  [[nodiscard]] std::unique_ptr<conformance::Run> resume_as(
      std::uint32_t seed, std::span<const std::byte> bytes, PlayerId local) const override {
    seats.push_back(local);
    std::unique_ptr<conformance::Run> run = inner.resume(seed, bytes);
    if (run != nullptr && local != kNoPlayer) run->world().spawn(NativeClass::unit, nullptr);
    return run;
  }
  WorldSnapshots inner;
  mutable std::vector<PlayerId> seats;
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

/// A peer: its negotiator and its link node.
struct Node {
  Node(const LockstepConfig& config, PlayerId self, std::vector<PlayerId> links)
      : negotiator(config, self), node(kMatch, config, self, std::move(links)) {}
  TurnNegotiator negotiator;
  LinkNode node;
};

/// One datagram from `from` to `to`, through `deliver` -- what every
/// transport uses: membership first, then the packets.
LinkReceipt deliver(Node& from, Node& to, std::uint32_t now = 0) {
  const Delivery delivery =
      deliver(to.negotiator, to.node, from.node.datagram_for(to.node.self(), now), now);
  CHECK(!delivery.irreconcilable);
  return delivery.receipt;
}

}  // namespace

// --------------------------------------------------------------------------
// the stream hash
// --------------------------------------------------------------------------

TEST(a_resumed_stream_hash_folds_on_from_the_value_it_was_given) {
  const CommandStream whole = intent_for({0, 3}, 12);
  for (std::uint32_t split = 1; split < 12; ++split) {
    CommandStream tail;
    tail.first = split;
    tail.prior = stream_hash(whole, split);
    tail.turns.assign(whole.turns.begin() + split, whole.turns.end());
    CHECK(tail.size() == whole.size());
    CHECK(tail.at(split - 1) == nullptr);
    REQUIRE(tail.at(split) != nullptr);
    for (std::size_t n = split; n <= 12; ++n) CHECK(stream_hash(tail, n) == stream_hash(whole, n));
  }
  // The turn folded is the match's, not the position in the stream: the
  // same orders held from turn 0 are another stream.
  CommandStream shifted;
  shifted.turns.assign(whole.turns.begin() + 5, whole.turns.end());
  CommandStream resumed;
  resumed.first = 5;
  resumed.prior = stream_hash(whole, 5);
  resumed.turns = shifted.turns;
  CommandStream from_zero_prior = resumed;
  from_zero_prior.prior = stream_hash(CommandStream{}, 0);
  CHECK(stream_hash(resumed, 12) != stream_hash(from_zero_prior, 12));
  CHECK(stream_hash(shifted, 7) != stream_hash(resumed, 12));
}

// --------------------------------------------------------------------------
// the negotiator
// --------------------------------------------------------------------------

TEST(a_seat_is_joined_only_after_it_was_dropped_and_never_into_the_past) {
  TurnNegotiator negotiator(peers_of({0, 1, 2}), 0);
  CHECK(!negotiator.admit(2, 9));          // still playing
  CHECK(!negotiator.admit(4, 9));          // not in the match
  REQUIRE(negotiator.drop(2, 6));
  CHECK(!negotiator.admit(2, 6));          // the turn it was dropped from
  CHECK(!negotiator.admit(2, 5));
  (void)run_to(negotiator, 8, {1, 2});
  REQUIRE(negotiator.next_turn() == 8);
  CHECK(!negotiator.admit(2, 7));          // a turn this peer has run
  CHECK(negotiator.admit(2, 9));
  CHECK(negotiator.admit(2, 9));           // the same admission again
  CHECK(!negotiator.admit(2, 12));         // a second, while it plays
  REQUIRE(negotiator.membership().size() == 3);
  const SeatStints seat = negotiator.membership()[2];
  REQUIRE(seat.stints.size() == 2);
  CHECK(seat.stints[0] == (Stint{0, 6}));
  CHECK(seat.stints[1] == (Stint{9, std::nullopt}));
  CHECK(!negotiator.dropped_from(2).has_value());
  CHECK(negotiator.plays(2, 5));
  CHECK(!negotiator.plays(2, 6));
  CHECK(!negotiator.plays(2, 8));
  CHECK(negotiator.plays(2, 9));
  CHECK(!negotiator.required(2, 10));      // agreed empty
  CHECK(negotiator.required(2, 11));
}

TEST(the_first_turn_of_a_joiners_part_carries_joined_and_its_packets_are_needed_later) {
  TurnNegotiator negotiator(peers_of({0, 1, 2}), 0);
  REQUIRE(negotiator.drop(2, 5));
  (void)run_to(negotiator, 7, {1, 2});
  REQUIRE(negotiator.admit(2, 9));
  // Before its part and in its agreed-empty turns, nothing of the seat's is
  // taken: the departed player's late packet is not the joiner's.
  CHECK(negotiator.receive(packet(2, 8)) == ReceiveStatus::departed);
  CHECK(negotiator.receive(packet(2, 10)) == ReceiveStatus::stale);
  CHECK(negotiator.receive(packet(2, 11)) == ReceiveStatus::accepted);
  std::vector<AgreedTurn> taken = run_to(negotiator, 11, {1});
  REQUIRE(taken.size() == 4);  // 7, 8, 9, 10: nobody waited on the joiner
  for (const AgreedTurn& turn : taken) {
    CHECK(count_kind(turn, 2, NetOrderKind::joined) == (turn.index == 9 ? 1u : 0u));
    CHECK(!has_order_from(turn, 2));
  }
  // Turn 11 needs it -- held already -- and 12 waits for it.
  taken = run_to(negotiator, 13, {1});
  REQUIRE(!taken.empty());
  CHECK(taken[0].index == 11);
  CHECK(has_order_from(taken[0], 2));
  CHECK(negotiator.next_turn() == 12);
  CHECK(!negotiator.ready());
  CHECK(negotiator.receive(packet(2, 12)) == ReceiveStatus::accepted);
  CHECK(negotiator.ready());
  // The history carries the joined order exactly where every peer takes it.
  std::size_t written = 0;
  for (std::size_t t = 0; t < negotiator.history().size(); ++t) {
    for (const NetOrder& order : negotiator.history().at(t)->orders) {
      if (order.kind == NetOrderKind::joined) {
        ++written;
        CHECK(t == 9);
        CHECK(order.issuer == 2);
      }
    }
  }
  CHECK(written == 1);
}

TEST(a_joiner_resumes_where_the_host_stood_and_runs_the_same_turns) {
  LockstepConfig config = peers_of({0, 1, 2});
  TurnNegotiator host(config, 0);
  REQUIRE(host.drop(2, 4));
  (void)run_to(host, 6, {1, 2});
  const std::uint32_t from = host.next_submit() + 1;
  REQUIRE(host.admit(2, from));
  (void)run_to(host, from, {1});
  REQUIRE(host.next_turn() == from);
  // What the header carries.
  const ResumePoint resume{host.next_turn(), stream_hash(host.history(), host.next_turn()),
                           host.membership()};
  TurnNegotiator joiner(host.config(), 2, resume);
  CHECK(joiner.next_turn() == from);
  CHECK(joiner.first_turn() == from);
  CHECK(joiner.next_submit() == from + 2);
  CHECK(stream_hash(joiner.history(), from) == stream_hash(host.history(), from));
  // The host hands the joiner what it holds; the joiner sends its first.
  for (const TurnPacket& held : host.held_from(from)) {
    CHECK(joiner.receive(held) == ReceiveStatus::accepted);
  }
  const std::optional<TurnPacket> first = joiner.submit({}, 400);
  REQUIRE(first.has_value());
  CHECK(first->turn == from + 2);
  CHECK(host.receive(*first) == ReceiveStatus::accepted);
  CHECK(!joiner.submit({}, 400).has_value());  // once per turn
  // Both run the same turns: same orders, same lengths, same stream hash.
  for (std::uint32_t t = from; t < from + 8; ++t) {
    if (std::optional<TurnPacket> mine = host.submit({}, 400)) (void)joiner.receive(*mine);
    if (std::optional<TurnPacket> mine = joiner.submit({}, 400)) (void)host.receive(*mine);
    for (std::uint32_t k = t; k <= t + 2; ++k) {
      (void)host.receive(packet(1, k));
      (void)joiner.receive(packet(1, k));
    }
    std::optional<AgreedTurn> a = host.take();
    std::optional<AgreedTurn> b = joiner.take();
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(a->index == b->index);
    CHECK(a->length == b->length);
    CHECK(count_kind(*a, 2, NetOrderKind::joined) == (t == from ? 1u : 0u));
    CHECK(count_kind(*b, 2, NetOrderKind::joined) == (t == from ? 1u : 0u));
    CHECK(stream_hash(host.history(), t + 1) == stream_hash(joiner.history(), t + 1));
  }
}

TEST(a_joiner_can_leave_again_and_its_second_part_ends_like_its_first) {
  TurnNegotiator negotiator(peers_of({0, 1, 2}), 0);
  REQUIRE(negotiator.drop(2, 4));
  (void)run_to(negotiator, 6, {1, 2});
  REQUIRE(negotiator.admit(2, 8));
  // An end inside the agreed-empty turns is an end at the first it could
  // have sent, as at the start.
  CHECK(negotiator.suspend(2) == 10);
  REQUIRE(negotiator.drop(2, 9));
  CHECK(negotiator.dropped_from(2) == 10u);
  // The first stint's decision, heard again, is only checked.
  CHECK(negotiator.drop(2, 4, 0u));
  CHECK(!negotiator.drop(2, 5, 0u));
  CHECK(negotiator.drop(2, 9, 8u));
  const std::vector<AgreedTurn> taken = run_to(negotiator, 12, {1});
  std::size_t joined = 0;
  std::size_t departed = 0;
  for (const AgreedTurn& turn : taken) {
    joined += count_kind(turn, 2, NetOrderKind::joined);
    departed += count_kind(turn, 2, NetOrderKind::departed);
    if (turn.index == 10) CHECK(count_kind(turn, 2, NetOrderKind::departed) == 1);
  }
  CHECK(joined == 1);
  CHECK(departed == 1);  // turn 4's was before this run
  // And it can be joined once more, past its second end.
  CHECK(!negotiator.admit(2, 10));
  CHECK(negotiator.admit(2, 13));
}

TEST(a_joiner_that_has_run_nothing_accepts_an_end_before_its_first_turn) {
  // The negotiator's rule: a decision is refused only if it cuts a turn this
  // peer ran. A joiner ran nothing before its first turn, so an end there
  // is one it can apply -- where a peer from the start could not.
  ResumePoint resume;
  resume.turn = 12;
  resume.membership = {SeatStints{0, {Stint{}}}, SeatStints{1, {Stint{}}},
                       SeatStints{2, {Stint{0, 5}, Stint{12, {}}}}};
  TurnNegotiator joiner(peers_of({0, 1, 2}), 2, resume);
  CHECK(joiner.drop(1, 9));
  CHECK(joiner.dropped_from(1) == 9u);
  TurnNegotiator old(peers_of({0, 1, 2}), 0);
  (void)run_to(old, 12, {1, 2});
  CHECK(!old.drop(1, 9));
  // And a joiner that has run a turn past the end refuses it.
  TurnNegotiator ran(peers_of({0, 1, 2}), 2, resume);
  (void)ran.submit({}, 400);
  for (std::uint32_t t = 12; t <= 14; ++t) {
    (void)ran.receive(packet(0, t));
    (void)ran.receive(packet(1, t));
  }
  REQUIRE(ran.take().has_value());
  CHECK(!ran.drop(1, 9));
  CHECK(!ran.drop(1, 12));
  CHECK(ran.drop(1, 13));
}

TEST(no_peer_can_say_a_seat_is_joined) {
  TurnPacket sent = packet(1, 3);
  sent.orders[0].kind = NetOrderKind::joined;
  TurnPacket back;
  CHECK(decode(encode(sent), back) == DecodeStatus::bad_field);
  TurnNegotiator negotiator(peers_of({0, 1}), 0);
  CHECK(negotiator.receive(sent) == ReceiveStatus::malformed);
  NetOrder mine;
  mine.kind = NetOrderKind::joined;
  const std::optional<TurnPacket> submitted = negotiator.submit({mine}, 400);
  REQUIRE(submitted.has_value());
  CHECK(submitted->orders.empty());
}

TEST(joined_hands_the_seat_back_through_the_sink) {
  struct Sink final : NetCommandSink {
    bool command(const NetOrder&) override { return false; }
    bool surrender(PlayerId) override { return false; }
    bool take_over(PlayerId departed) override {
      taken.push_back(departed);
      return true;
    }
    bool hand_back(PlayerId joined) override {
      given.push_back(joined);
      return true;
    }
    std::vector<PlayerId> taken;
    std::vector<PlayerId> given;
  };
  NetTurn turn;
  NetOrder joined;
  joined.issuer = 4;
  joined.kind = NetOrderKind::joined;
  turn.orders.push_back(joined);
  World world;
  Sink sink;
  const NetTurnReport applied = apply_turn(world, turn, nullptr, &sink);
  CHECK(applied.commands == 1);
  CHECK(sink.given == std::vector<PlayerId>{4});
  CHECK(sink.taken.empty());
  CHECK(apply_turn(world, turn, nullptr, nullptr).unapplied == 1);
  // A sink that cannot stop an AI says so, the same on every peer.
  struct Base final : NetCommandSink {
    bool command(const NetOrder&) override { return false; }
    bool surrender(PlayerId) override { return false; }
  } base;
  CHECK(apply_turn(world, turn, nullptr, &base).unapplied == 1);
}

// --------------------------------------------------------------------------
// the link layer
// --------------------------------------------------------------------------

TEST(admissions_and_stints_survive_the_wire_and_every_prefix_is_refused) {
  Datagram datagram;
  datagram.match = kMatch;
  datagram.from = 0;
  datagram.reports = {{2, 1, 7, 0}, {2, 0, 15, 11}};
  datagram.drops = {{2, 7, 0}};
  datagram.admits = {{2, 11}, {3, 40}};
  const std::vector<std::byte> bytes = encode(datagram);
  Datagram back;
  REQUIRE(decode(bytes, back) == DecodeStatus::ok);
  REQUIRE(back.reports.size() == 2);
  CHECK(back.reports[1].since == 11);
  CHECK(back.reports[1].held == 15);
  REQUIRE(back.drops.size() == 1);
  CHECK(back.drops[0].since == 0);
  CHECK(back.drops[0].from_turn == 7);
  REQUIRE(back.admits.size() == 2);
  CHECK(back.admits[0].peer == 2);
  CHECK(back.admits[0].from_turn == 11);
  CHECK(back.admits[1].peer == 3);
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    Datagram partial;
    CHECK(decode(std::span<const std::byte>(bytes.data(), cut), partial) != DecodeStatus::ok);
  }
  std::vector<std::byte> old = bytes;
  old[4] = std::byte{3};  // version 3 had neither
  CHECK(decode(old, back) == DecodeStatus::bad_version);
}

TEST(a_node_takes_a_seats_packets_only_inside_its_stints) {
  const LockstepConfig config = peers_of({0, 1, 2});
  Node hub(config, 0, {1, 2});
  Node one(config, 1, {0});
  hub.node.decide(2, 6);
  hub.node.admit(2, 12);
  CHECK(hub.node.since(2) == 12);
  CHECK(hub.node.accepts(2, 5));
  CHECK(!hub.node.accepts(2, 6));
  CHECK(!hub.node.accepts(2, 13));   // agreed empty
  CHECK(hub.node.accepts(2, 14));
  CHECK(!hub.node.accepts(2, 1));    // agreed in advance for everyone
  CHECK(hub.node.accepts(1, 9));
  CHECK(hub.node.departed().empty());
  CHECK(hub.node.drops().empty());   // the current stint's: none
  REQUIRE(hub.node.decisions().size() == 1);
  CHECK(hub.node.decisions()[0].since == 0);
  // What the departed player sent past its end, and in the joiner's
  // agreed-empty turns, is late, through the relay as from the seat.
  for (std::uint32_t turn : {7u, 13u}) {
    TurnPacket stale = packet(2, turn);
    Datagram datagram;
    datagram.match = kMatch;
    datagram.from = 0;
    datagram.hold = kNoEcho;
    datagram.packets = {encode(stale)};
    Node fresh(config, 1, {0, 2});
    fresh.node.decide(2, 6);
    fresh.node.admit(2, 12);
    const LinkReceipt receipt = fresh.node.receive(encode(datagram), 0);
    CHECK(receipt.late == 1);
    CHECK(receipt.packets.empty());
    CHECK(fresh.node.held() == 0);
  }
}

TEST(an_admission_arrives_with_the_joiners_first_packet_through_the_relay) {
  const LockstepConfig config = peers_of({0, 1, 2});
  Node hub(config, 0, {1});   // the joiner's link comes later
  Node one(config, 1, {0});
  REQUIRE(hub.negotiator.drop(2, 4));
  hub.node.decide(2, 4);
  (void)run_to(hub.negotiator, 6, {1, 2});
  deliver(hub, one);
  REQUIRE(one.negotiator.dropped_from(2) == 4u);
  REQUIRE(open_seat(hub.negotiator, hub.node) == 2);
  const std::optional<std::uint32_t> from = admit_joiner(hub.negotiator, hub.node, 2);
  REQUIRE(from.has_value());
  CHECK(*from == hub.negotiator.next_submit() + 1);
  (void)run_to(hub.negotiator, *from, {1});
  // The joiner: header, resume, first packet -- to the hub only.
  const std::vector<std::byte> save(3000, std::byte{7});
  const JoinHeader header = join_header(hub.negotiator, hub.node, Start{}, 2, kMatch, save);
  Node joiner(header.start.config, 2, {0});
  joiner.negotiator = TurnNegotiator(header.start.config, 2, header.resume());
  joiner.node = LinkNode(kMatch, header.start.config, 2, {0}, header.resume(), header.chat_next);
  hub.node.add_link(2, *from, hub.negotiator.held_from(*from));
  const std::optional<TurnPacket> first = joiner.negotiator.submit({}, 400);
  REQUIRE(first.has_value());
  joiner.node.offer(*first);
  deliver(joiner, hub);
  CHECK(hub.negotiator.required(2, first->turn));
  // Peer 1 has not heard of the admission, and learns it in the datagram
  // that brings the joiner's packet: the packet is taken, not late.
  CHECK(one.node.since(2) == 0);
  const LinkReceipt receipt = deliver(hub, one);
  CHECK(receipt.late == 0);
  CHECK(one.node.since(2) == *from);
  CHECK(one.negotiator.required(2, first->turn));
  bool learned = false;
  for (const TurnPacket& got : receipt.packets) learned = learned || got.peer == 2;
  CHECK(learned);
  CHECK(one.negotiator.membership() == hub.negotiator.membership());
}

TEST(a_datagram_with_an_admission_and_that_stints_end_takes_the_joiners_packets) {
  // A joiner that came and went between two datagrams: its admission, the
  // drop of its part, and its packets arrive together. The admission is read
  // first, so the drop is about its stint and its packets below the end are
  // the match's.
  // A delay of 3, so that the joiner's packet is inside the window of a
  // peer that has run nothing.
  const LockstepConfig config = peers_of({0, 1, 2}, 3);
  Node one(config, 1, {0});
  one.node.decide(2, 3);
  (void)apply_membership(one.negotiator, one.node);
  Datagram datagram;
  datagram.match = kMatch;
  datagram.from = 0;
  datagram.hold = kNoEcho;
  datagram.admits = {{2, 4}};
  datagram.drops = {{2, 3, 0}, {2, 9, 4}};
  datagram.packets = {encode(packet(2, 7))};
  const Delivery delivery = deliver(one.negotiator, one.node, encode(datagram), 0);
  CHECK(!delivery.irreconcilable);
  CHECK(delivery.receipt.late == 0);
  CHECK(delivery.accepted == 1);
  CHECK(delivery.refused == 0);
  const SeatStints seat = one.negotiator.membership()[2];
  REQUIRE(seat.stints.size() == 2);
  CHECK(seat.stints[1] == (Stint{4, 9}));
}

TEST(a_new_link_is_sent_what_the_host_held_from_its_first_turn_and_no_older) {
  const LockstepConfig config = peers_of({0, 1, 2});
  Node hub(config, 0, {1});
  Node one(config, 1, {0});
  REQUIRE(hub.negotiator.drop(2, 3));
  hub.node.decide(2, 3);
  // Peer 1's packets up to turn 9 reach the hub and are acknowledged by
  // everyone it links to, so the link layer forgets them.
  // (The link layer alone: the hub's negotiator is not the point here.)
  for (std::uint32_t t = 2; t <= 9; ++t) one.node.offer(packet(1, t));
  (void)hub.node.receive(one.node.datagram_for(0, 0), 0);
  (void)one.node.receive(hub.node.datagram_for(1, 0), 0);
  (void)hub.node.receive(one.node.datagram_for(0, 0), 0);
  CHECK(hub.node.held() == 0);
  std::vector<TurnPacket> held;
  for (std::uint32_t t = 6; t <= 9; ++t) held.push_back(packet(1, t));
  hub.node.admit(2, 6);
  hub.node.add_link(2, 6, held);
  ResumePoint resume;
  resume.turn = 6;
  resume.membership = {SeatStints{0, {Stint{}}}, SeatStints{1, {Stint{}}},
                       SeatStints{2, {Stint{0, 3}, Stint{6, {}}}}};
  Node joiner(config, 2, {0});
  joiner.node = LinkNode(kMatch, config, 2, {0}, resume, {});
  const LinkReceipt receipt = joiner.node.receive(hub.node.datagram_for(2, 0), 0);
  std::vector<std::uint32_t> turns;
  for (const TurnPacket& got : receipt.packets) turns.push_back(got.turn);
  CHECK(turns == (std::vector<std::uint32_t>{6, 7, 8, 9}));
  // Once the joiner acknowledges them, the hub forgets them again.
  (void)hub.node.receive(joiner.node.datagram_for(0, 0), 0);
  CHECK(hub.node.held() == 0);
}

TEST(a_new_link_is_not_sent_what_other_links_still_await_from_before_its_first_turn) {
  const LockstepConfig config = peers_of({0, 1, 2, 3});
  LinkNode hub(kMatch, config, 0, {1, 3});
  // Peer 3 has not acknowledged turns 4 and 5 of peer 1's, so the hub still
  // holds them -- for 3, not for a joiner starting at 6; nor the lines said
  // before it came.
  for (std::uint32_t t = 2; t <= 5; ++t) {
    Datagram datagram;
    datagram.match = kMatch;
    datagram.from = 1;
    datagram.hold = kNoEcho;
    datagram.packets = {encode(packet(1, t))};
    (void)hub.receive(encode(datagram), 0);
  }
  (void)hub.say(ChatLine{});
  (void)hub.say(ChatLine{});
  REQUIRE(hub.held() == 4);
  hub.decide(2, 3);
  hub.admit(2, 6);
  hub.add_link(2, 6, {packet(1, 6)});
  Datagram out;
  REQUIRE(decode(hub.datagram_for(2, 0), out) == DecodeStatus::ok);
  REQUIRE(out.packets.size() == 1);
  TurnPacket only;
  REQUIRE(decode(out.packets[0], only) == DecodeStatus::ok);
  CHECK(only.turn == 6);
  CHECK(out.chat.empty());
}

TEST(a_resumed_node_acknowledges_from_its_first_turn_and_continues_the_chat) {
  const LockstepConfig config = peers_of({0, 1, 2});
  ResumePoint resume;
  resume.turn = 20;
  resume.membership = {SeatStints{0, {Stint{}}}, SeatStints{1, {Stint{0, 9}, Stint{14, {}}}},
                       SeatStints{2, {Stint{0, 11}, Stint{20, {}}}}};
  const std::map<PlayerId, std::uint32_t> chat = {{0, 4}, {2, 7}};
  LinkNode joiner(kMatch, config, 2, {0}, resume, chat);
  CHECK(joiner.since(1) == 14);
  CHECK(joiner.since(2) == 20);
  CHECK(joiner.departed().empty());
  Datagram out;
  REQUIRE(decode(joiner.datagram_for(0, 0), out) == DecodeStatus::ok);
  for (const LinkAck& ack : out.acks) {
    CHECK(ack.next_turn == (ack.peer == 2 ? 22u : 20u));
  }
  CHECK(out.admits.size() == 2);
  CHECK(out.drops.size() == 2);
  // Its own lines continue its seat's numbering: the departed player's
  // lines 0..6 were heard, so its first is 7.
  CHECK(joiner.say(ChatLine{}).seq == 7);
  // A line from the host numbered where the host stood is shown.
  LinkNode hub(kMatch, config, 0, {2});
  for (int i = 0; i < 5; ++i) (void)hub.say(ChatLine{});
  const LinkReceipt receipt = joiner.receive(hub.datagram_for(2, 0), 0);
  REQUIRE(receipt.chat.size() == 1);
  CHECK(receipt.chat[0].seq == 4);
}

TEST(facts_about_an_earlier_stint_do_not_touch_the_current_one) {
  const LockstepConfig config = peers_of({0, 1, 2});
  Node one(config, 1, {0});
  // The joiner is playing; a lagging peer's datagram still carries its
  // departed predecessor's report and the drop. Neither is about the joiner.
  Datagram datagram;
  datagram.match = kMatch;
  datagram.from = 0;
  datagram.hold = kNoEcho;
  datagram.admits = {{2, 12}};
  datagram.drops = {{2, 6, 0}};
  (void)one.node.receive(encode(datagram), 0);
  (void)apply_membership(one.negotiator, one.node);
  datagram.admits.clear();
  datagram.drops.clear();
  datagram.reports = {{2, 0, 6, 0}};
  (void)one.node.receive(encode(datagram), 0);
  CHECK(one.node.departed().empty());
  CHECK(settle_departures(one.negotiator, one.node).state == DepartureOutcome::State::playing);
  CHECK(!one.negotiator.suspended(2));
  // Its own departure is: tagged with its stint.
  one.node.depart(2);
  CHECK(one.node.departed() == std::vector<PlayerId>{2});
  (void)settle_departures(one.negotiator, one.node);
  CHECK(one.node.reports().count({2, 1}) == 1);
  // A joiner never counts its seat's earlier drop as its own.
  ResumePoint resume;
  resume.turn = 12;
  resume.membership = {SeatStints{0, {Stint{}}}, SeatStints{1, {Stint{}}},
                       SeatStints{2, {Stint{0, 6}, Stint{12, {}}}}};
  Node joiner(config, 2, {0});
  joiner.negotiator = TurnNegotiator(config, 2, resume);
  joiner.node = LinkNode(kMatch, config, 2, {0}, resume, {});
  CHECK(settle_departures(joiner.negotiator, joiner.node).state ==
        DepartureOutcome::State::playing);
}

TEST(the_coordinator_seats_a_joiner_only_on_a_dropped_seat_with_nothing_undecided) {
  const LockstepConfig config = peers_of({0, 1, 2, 3});
  Node hub(config, 0, {1, 2, 3});
  Node one(config, 1, {0});
  CHECK(!open_seat(hub.negotiator, hub.node).has_value());  // nobody has left
  hub.node.depart(3);
  (void)settle_departures(hub.negotiator, hub.node);
  CHECK(!open_seat(hub.negotiator, hub.node).has_value());  // undecided
  CHECK(!admit_joiner(hub.negotiator, hub.node, 3).has_value());
  hub.node.decide(3, 5);
  (void)apply_membership(hub.negotiator, hub.node);
  CHECK(open_seat(hub.negotiator, hub.node) == 3);
  CHECK(!open_seat(one.negotiator, one.node).has_value());  // not the coordinator
  CHECK(!admit_joiner(hub.negotiator, hub.node, 2).has_value());  // still played
  // Past everything the host sent, and past the seat's end.
  (void)hub.negotiator.submit({}, 400);
  const std::uint32_t past = hub.negotiator.next_submit() + 1;
  const std::optional<std::uint32_t> from = admit_joiner(hub.negotiator, hub.node, 3);
  REQUIRE(from.has_value());
  CHECK(*from == std::max(past, 6u));
  CHECK(hub.node.since(3) == *from);
  CHECK(!open_seat(hub.negotiator, hub.node).has_value());
  // A seat open, and another departure undecided: nobody is seated until
  // it is decided -- the coordinator would need the joiner's report on it.
  Node busy(config, 0, {1, 2, 3});
  busy.node.decide(3, 5);
  (void)apply_membership(busy.negotiator, busy.node);
  REQUIRE(open_seat(busy.negotiator, busy.node) == 3);
  busy.node.depart(2);
  (void)settle_departures(busy.negotiator, busy.node);
  CHECK(!open_seat(busy.negotiator, busy.node).has_value());
  CHECK(!admit_joiner(busy.negotiator, busy.node, 3).has_value());
  // A seat dropped late: the end wins.
  Node other(config, 0, {1, 2, 3});
  other.node.decide(2, 30);
  (void)apply_membership(other.negotiator, other.node);
  CHECK(admit_joiner(other.negotiator, other.node, 2) == 31u);
}

// --------------------------------------------------------------------------
// the header, the chunks, the transfer
// --------------------------------------------------------------------------

namespace {

JoinHeader a_header() {
  JoinHeader header;
  header.match = kMatch;
  header.start.you = 2;
  header.start.map = "Adventures/Somewhere.bfhp";
  header.start.config = peers_of({0, 1, 2});
  header.start.config.game_speed = 1400;
  header.start.links = {0};
  header.from_turn = 17;
  header.netcmds = 0x1234567890ABCDEFull;
  header.size = 5000;
  header.hash = 0xFEEDFACEull;
  header.membership = {SeatStints{0, {Stint{}}}, SeatStints{1, {Stint{}}},
                       SeatStints{2, {Stint{0, 9}, Stint{17, {}}}}};
  header.chat_next = {{0, 3}, {2, 5}};
  return header;
}

}  // namespace

TEST(a_join_header_survives_the_wire_and_refuses_what_no_joiner_could_build) {
  const JoinHeader header = a_header();
  const std::vector<std::byte> bytes = encode(header);
  CHECK(lobby_kind(bytes) == LobbyKind::join_header);
  JoinHeader back;
  REQUIRE(decode(bytes, back) == DecodeStatus::ok);
  CHECK(back.match == kMatch);
  CHECK(back.start.you == 2);
  CHECK(back.start.map == header.start.map);
  CHECK(back.start.config.game_speed == 1400);
  CHECK(back.from_turn == 17);
  CHECK(back.netcmds == header.netcmds);
  CHECK(back.size == 5000);
  CHECK(back.hash == header.hash);
  CHECK(back.membership == header.membership);
  CHECK(back.chat_next == header.chat_next);
  CHECK(back.resume().turn == 17);
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    JoinHeader partial;
    CHECK(decode(std::span<const std::byte>(bytes.data(), cut), partial) != DecodeStatus::ok);
  }
  std::vector<std::byte> longer = bytes;
  longer.push_back(std::byte{0});
  CHECK(decode(longer, back) == DecodeStatus::trailing);
  std::vector<std::byte> other = bytes;
  other[4] = std::byte{6};
  CHECK(decode(other, back) == DecodeStatus::bad_version);

  const auto refused = [](JoinHeader bad) {
    JoinHeader out;
    return decode(encode(bad), out) == DecodeStatus::bad_field;
  };
  JoinHeader bad = header;
  bad.membership[2].stints.back().from = 16;  // not where it starts
  CHECK(refused(bad));
  bad = header;
  bad.membership[2].stints.back().until = 30;  // its own part already over
  CHECK(refused(bad));
  bad = header;
  bad.membership[1].stints = {Stint{0, 9}, Stint{9, {}}};  // touching stints
  CHECK(refused(bad));
  bad = header;
  bad.membership[1].stints = {Stint{3, {}}};  // not from the start
  CHECK(refused(bad));
  bad = header;
  bad.membership.push_back(SeatStints{6, {Stint{}}});  // not a peer
  CHECK(refused(bad));
  bad = header;
  bad.size = 0;
  CHECK(refused(bad));
  bad = header;
  bad.size = 0xFFFFFFFFu;  // nothing is allocated for a lie
  CHECK(refused(bad));
  bad = header;
  bad.from_turn = 0;
  bad.membership[2].stints.back().from = 0;
  CHECK(refused(bad));
}

TEST(chunks_and_acknowledgements_survive_the_wire) {
  JoinChunk chunk;
  chunk.match = kMatch;
  chunk.offset = 2 * kJoinChunk;
  chunk.bytes.assign(kJoinChunk, std::byte{9});
  const std::vector<std::byte> bytes = encode(chunk);
  CHECK(lobby_kind(bytes) == LobbyKind::join_chunk);
  JoinChunk back;
  REQUIRE(decode(bytes, back) == DecodeStatus::ok);
  CHECK(back.offset == chunk.offset);
  CHECK(back.bytes == chunk.bytes);
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    CHECK(decode(std::span<const std::byte>(bytes.data(), cut), back) != DecodeStatus::ok);
  }
  JoinChunk odd = chunk;
  odd.offset = 5;
  CHECK(decode(encode(odd), back) == DecodeStatus::bad_field);
  JoinChunk big = chunk;
  big.bytes.push_back(std::byte{0});
  CHECK(decode(encode(big), back) == DecodeStatus::bad_field);

  const JoinAck ack{kMatch, 4096};
  const std::vector<std::byte> said = encode(ack);
  CHECK(lobby_kind(said) == LobbyKind::join_ack);
  JoinAck heard;
  REQUIRE(decode(said, heard) == DecodeStatus::ok);
  CHECK(heard.match == kMatch);
  CHECK(heard.received == 4096);
  for (std::size_t cut = 0; cut < said.size(); ++cut) {
    CHECK(decode(std::span<const std::byte>(said.data(), cut), heard) != DecodeStatus::ok);
  }
}

TEST(a_transfer_takes_chunks_in_any_order_once_and_checks_the_hash) {
  std::vector<std::byte> save(3 * kJoinChunk + 100);
  for (std::size_t i = 0; i < save.size(); ++i) save[i] = static_cast<std::byte>(i * 7 + 1);
  JoinHeader header = a_header();
  header.size = static_cast<std::uint32_t>(save.size());
  header.hash = join_hash(save);
  const auto chunk = [&](std::size_t index) {
    JoinChunk out;
    out.match = kMatch;
    out.offset = static_cast<std::uint32_t>(index * kJoinChunk);
    const std::size_t end = std::min(save.size(), (index + 1) * kJoinChunk);
    out.bytes.assign(save.begin() + static_cast<std::ptrdiff_t>(out.offset),
                     save.begin() + static_cast<std::ptrdiff_t>(end));
    return out;
  };
  JoinTransfer transfer(header);
  CHECK(transfer.received() == 0);
  CHECK(transfer.receive(chunk(2)));
  CHECK(transfer.received() == 0);  // past a gap
  CHECK(transfer.receive(chunk(0)));
  CHECK(transfer.received() == kJoinChunk);
  CHECK(transfer.receive(chunk(0)));  // again: harmless
  JoinChunk wrong = chunk(1);
  wrong.match = kMatch + 1;
  CHECK(!transfer.receive(wrong));
  JoinChunk short_one = chunk(1);
  short_one.bytes.pop_back();
  CHECK(!transfer.receive(short_one));
  JoinChunk past;
  past.match = kMatch;
  past.offset = 8 * kJoinChunk;
  past.bytes.assign(10, std::byte{0});
  CHECK(!transfer.receive(past));
  CHECK(!transfer.complete());
  CHECK(transfer.receive(chunk(3)));
  CHECK(transfer.receive(chunk(1)));
  CHECK(transfer.complete());
  CHECK(transfer.received() == save.size());
  CHECK(transfer.verified());
  CHECK(std::equal(save.begin(), save.end(), transfer.bytes().begin()));
  // The same chunks under a header whose hash is another save's.
  header.hash ^= 1;
  JoinTransfer lied(header);
  for (std::size_t i = 0; i < 4; ++i) CHECK(lied.receive(chunk(i)));
  CHECK(lied.complete());
  CHECK(!lied.verified());
}

TEST(join_header_is_the_hosts_match_made_the_joiners) {
  const LockstepConfig config = peers_of({0, 1, 2});
  Node hub(config, 0, {1, 2});
  REQUIRE(hub.negotiator.drop(2, 3));
  hub.node.decide(2, 3);
  (void)run_to(hub.negotiator, 4, {1, 2});
  const std::optional<std::uint32_t> from = admit_joiner(hub.negotiator, hub.node, 2);
  REQUIRE(from.has_value());
  (void)run_to(hub.negotiator, *from, {1});
  (void)hub.node.say(ChatLine{});
  Start start;
  start.you = 0;
  start.config = config;
  start.links = {1, 2};
  start.chat = {ChatLine{}};
  const std::vector<std::byte> save(10, std::byte{1});
  const JoinHeader header = join_header(hub.negotiator, hub.node, start, 2, kMatch, save);
  CHECK(header.start.you == 2);
  CHECK(header.start.links == std::vector<PlayerId>{0});
  CHECK(header.start.chat.empty());
  CHECK(header.from_turn == *from);
  CHECK(header.netcmds == stream_hash(hub.negotiator.history(), *from));
  CHECK(header.size == 10);
  CHECK(header.hash == join_hash(save));
  CHECK(header.membership == hub.negotiator.membership());
  CHECK(header.chat_next.at(0) == 1);
  JoinHeader back;
  CHECK(decode(encode(header), back) == DecodeStatus::ok);
}

// --------------------------------------------------------------------------
// netplay with late joiners
// --------------------------------------------------------------------------

TEST(a_late_joiner_on_a_lossy_star_plays_the_same_game_from_its_first_turn) {
  const QuietScenario scenario;
  const WorldSnapshots snapshots;
  NetplayOptions options;
  options.lockstep.peers = {0, 3, 5};
  options.seed = 0x7A;
  options.relay = true;
  options.loss_per_mille = 250;
  options.departures = {{5, 8, 3}};
  options.joins = {{5, 14, 5, 0, 0}};
  options.snapshots = &snapshots;
  options.chat_every = 3;
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 3, 48, intent_for({0, 3, 5}, 48), options, &report);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("  %s\n", divergence.describe().c_str());
  CHECK(!report.deadlocked);
  CHECK(report.turns == 48);
  CHECK(report.refused == 0);
  CHECK(report.chat_misordered == 0);
  REQUIRE(report.dropped.size() == 1);
  REQUIRE(report.joined.size() == 1);
  CHECK(report.joined[0].first == 5);
  CHECK(report.joined[0].second > report.dropped[0].second);
  CHECK(report.joiner_turns == 48 - report.joined[0].second);
  CHECK(snapshots.saves == 1);
  CHECK(report.stalls > 0);
}

TEST(a_late_joiner_resumes_from_its_own_seat_and_a_seat_that_leaks_is_caught) {
  const QuietScenario scenario;
  const SeatLeakingSnapshots snapshots;
  NetplayOptions options;
  options.lockstep.peers = {0, 3, 5};
  options.seed = 0x7A;
  options.relay = true;
  options.loss_per_mille = 250;
  options.departures = {{5, 8, 3}};
  options.joins = {{5, 14, 5, 0, 0}};
  options.snapshots = &snapshots;
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 3, 48, intent_for({0, 3, 5}, 48), options, &report);
  CHECK(snapshots.seats == (std::vector<PlayerId>{5}));
  REQUIRE(report.joined.size() == 1);
  CHECK(divergence.diverged());
}

TEST(a_joiner_seated_during_a_speed_change_runs_at_the_speed_in_force) {
  const QuietScenario scenario;
  const WorldSnapshots snapshots;
  NetplayOptions options;
  options.lockstep.peers = {0, 1, 2};
  options.seed = 3;
  options.relay = true;
  options.loss_per_mille = 150;
  options.vary_proposals = false;  // 400 ms a turn, so the speed shows
  options.departures = {{2, 6, 2}};
  options.joins = {{2, 10, 3, 2000, 0}};
  options.snapshots = &snapshots;
  CommandStream intent = intent_for({0, 1, 2}, 40);
  // And the joiner changes it again once it plays.
  NetOrder later;
  later.issuer = 2;
  later.kind = NetOrderKind::set_speed;
  later.speed = 700;
  intent.turns[30].orders.push_back(later);
  NetplayReport report;
  const conformance::Divergence divergence = check_netplay(scenario, 1, 40, intent, options, &report);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("  %s\n", divergence.describe().c_str());
  REQUIRE(report.joined.size() == 1);
  const std::uint32_t from = report.joined[0].second;
  REQUIRE(report.schedule.size() == 40);
  // The coordinator's order landed in the last turn the save carries, so the
  // joiner's first turn is the first at the new speed.
  CHECK(report.schedule[from - 1] == 400);
  CHECK(report.schedule[from] == 800);
  CHECK(report.schedule[32] == 800);
  CHECK(report.schedule[33] == 280);
}

TEST(a_joiner_that_leaves_again_is_dropped_and_its_seat_can_be_joined_once_more) {
  const QuietScenario scenario;
  const WorldSnapshots snapshots;
  NetplayOptions options;
  options.lockstep.peers = {0, 1, 2, 4};
  options.seed = 0x33;
  options.relay = true;
  options.loss_per_mille = 200;
  options.departures = {{4, 5, 2}, {1, 7, 2}};
  // Seat 4 joined, left again, joined a third time; seat 1 joined once.
  options.joins = {{4, 9, 3, 0, 8}, {1, 12, 2, 0, 0}, {4, 30, 4, 0, 0}};
  options.snapshots = &snapshots;
  NetplayReport report;
  const conformance::Divergence divergence =
      check_netplay(scenario, 5, 60, intent_for({0, 1, 2, 4}, 60), options, &report);
  CHECK(!divergence.diverged());
  if (divergence.diverged()) std::printf("  %s\n", divergence.describe().c_str());
  CHECK(report.turns == 60);
  CHECK(report.refused == 0);
  REQUIRE(report.joined.size() == 3);
  CHECK(report.dropped.size() == 3);
  CHECK(snapshots.saves == 3);
  // In the order they were seated, and seat 4's second joiner after its
  // first had left again.
  CHECK(report.joined[0].second <= report.joined[1].second);
  CHECK(report.joined[1].second <= report.joined[2].second);
  CHECK(report.joined[2].first == 4);
}

TEST(joins_without_snapshots_or_the_link_layer_are_refused_not_ignored) {
  const QuietScenario scenario;
  const WorldSnapshots snapshots;
  NetplayOptions options;
  options.lockstep.peers = {0, 1};
  options.relay = true;
  options.departures = {{1, 3, 2}};
  options.joins = {{1, 6, 2, 0, 0}};
  CHECK(check_netplay(scenario, 1, 20, intent_for({0, 1}, 20), options).kind ==
        conformance::Divergence::Kind::unbuildable);
  options.snapshots = &snapshots;
  options.relay = false;
  CHECK(check_netplay(scenario, 1, 20, intent_for({0, 1}, 20), options).kind ==
        conformance::Divergence::Kind::unbuildable);
  // And a join that never happens -- its seat never opens -- is not a pass.
  options.relay = true;
  options.departures.clear();
  CHECK(check_netplay(scenario, 1, 20, intent_for({0, 1}, 20), options).kind ==
        conformance::Divergence::Kind::unbuildable);
}
