// Chat: sim/netchat.hpp, and how the link layer and the lobby carry it.
//
// What carries the reading:
//
//   * A line is hostile input like a turn packet: every prefix is refused,
//     and so is a byte more, a field out of range or a line past the cap.
//   * Whom a line is for is the viewer's question: all, allies (mutual, as
//     the viewer's world says), or the one player chosen; a speaker always
//     sees its own.
//   * Through a lossy link and a relay every line arrives once, in the order
//     it was said, and a line past a gap waits for the gap.
//   * Chat never pushes a turn packet out of a datagram, and a match with
//     chat is the same match, hash for hash, as one without.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/conformance.hpp"
#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netchat.hpp"
#include "imperivm/core/sim/netlink.hpp"
#include "imperivm/core/sim/netlobby.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
namespace conformance = imperivm::core::sim::conformance;
using imperivm::core::NativeClass;

namespace {

constexpr std::uint32_t kMatch = 0xC4A7;

ChatLine sample_line() {
  ChatLine line;
  line.from = 3;
  line.seq = 41;
  line.to = ChatLine::To::player;
  line.target = 5;
  line.located = true;
  line.location = Point{-1200, 3400};
  line.text = "hold the ford";
  return line;
}

LockstepConfig three() {
  LockstepConfig config;
  config.peers = {0, 1, 2};
  return config;
}

ChatLine said(std::string text) {
  ChatLine line;
  line.text = std::move(text);
  return line;
}

/// Hand every line `to` learns from `from`'s next datagram to `heard`.
void deliver(LinkNode& from, LinkNode& to, std::vector<ChatLine>& heard, std::uint32_t now = 0) {
  LinkReceipt receipt = to.receive(from.datagram_for(to.self(), now), now);
  for (ChatLine& line : receipt.chat) heard.push_back(std::move(line));
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

// --------------------------------------------------------------------------
// the line
// --------------------------------------------------------------------------

TEST(a_chat_line_survives_the_wire) {
  const ChatLine line = sample_line();
  ChatLine back;
  REQUIRE(decode(encode(line), back) == DecodeStatus::ok);
  CHECK(back.from == 3);
  CHECK(back.seq == 41);
  CHECK(back.to == ChatLine::To::player);
  CHECK(back.target == 5);
  CHECK(back.located);
  CHECK(back.location.x == -1200 && back.location.y == 3400);
  CHECK(back.text == "hold the ford");
}

TEST(every_prefix_of_a_chat_line_is_refused_and_so_is_what_no_encoder_writes) {
  const std::vector<std::byte> bytes = encode(sample_line());
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    ChatLine out;
    CHECK(decode(std::span<const std::byte>(bytes.data(), cut), out) != DecodeStatus::ok);
  }
  std::vector<std::byte> more = bytes;
  more.push_back(std::byte{0});
  ChatLine out;
  CHECK(decode(more, out) == DecodeStatus::trailing);
  // to (offset 10) and located (12) out of range.
  std::vector<std::byte> bad = bytes;
  bad[10] = std::byte{3};
  CHECK(decode(bad, out) == DecodeStatus::bad_field);
  bad = bytes;
  bad[12] = std::byte{2};
  CHECK(decode(bad, out) == DecodeStatus::bad_field);
  bad = bytes;
  bad[4] = std::byte{kChatVersion + 1};
  CHECK(decode(bad, out) == DecodeStatus::bad_version);
}

TEST(a_line_is_cut_to_the_cap_on_the_way_out_and_refused_past_it_on_the_way_in) {
  ChatLine line = sample_line();
  line.text.assign(kMaxChatText + 30, 'a');
  ChatLine back;
  std::vector<std::byte> bytes = encode(line);
  REQUIRE(decode(bytes, back) == DecodeStatus::ok);
  CHECK(back.text.size() == kMaxChatText);
  // A length byte past the cap, with the bytes behind it.
  bytes[21] = std::byte{kMaxChatText + 1};
  bytes.push_back(std::byte{'a'});
  CHECK(decode(bytes, back) == DecodeStatus::bad_field);
}

TEST(whom_a_line_is_shown_to) {
  PlayerTable players;
  players.set(1, 2, Relation::allied, true);
  players.set(2, 1, Relation::allied, true);
  players.set(1, 3, Relation::allied, true);  // not returned by 3
  ChatLine line;
  line.from = 1;
  line.to = ChatLine::To::all;
  CHECK(shown_to(line, 4, players));
  line.to = ChatLine::To::allies;
  CHECK(shown_to(line, 2, players));
  CHECK(!shown_to(line, 3, players));  // one-sided: not allies
  CHECK(!shown_to(line, 4, players));
  CHECK(shown_to(line, 1, players));   // the speaker sees its own
  line.to = ChatLine::To::player;
  line.target = 4;
  CHECK(shown_to(line, 4, players));
  CHECK(!shown_to(line, 2, players));
  CHECK(shown_to(line, 1, players));
  CHECK(!shown_to(line, imperivm::core::kNoPlayer, players));
}

// --------------------------------------------------------------------------
// the link layer
// --------------------------------------------------------------------------

TEST(a_line_is_stamped_by_its_speaker_and_carried_until_acknowledged) {
  const LockstepConfig config = three();
  LinkNode a(kMatch, config, 1, {0});
  LinkNode hub(kMatch, config, 0, {1, 2});
  ChatLine forged = said("hello");
  forged.from = 2;  // not a's to give
  forged.seq = 99;
  const ChatLine stamped = a.say(forged);
  CHECK(stamped.from == 1);
  CHECK(stamped.seq == 0);
  CHECK(a.say(said("again")).seq == 1);
  CHECK(a.chat_held() == 2);
  std::vector<ChatLine> heard;
  deliver(a, hub, heard);
  REQUIRE(heard.size() == 2);
  CHECK(heard[0].text == "hello");
  CHECK(heard[1].text == "again");
  // Held by the hub for peer 2, which has not acknowledged; dropped by a once
  // the hub has. And never offered back to the link it came from.
  CHECK(hub.chat_held() == 2);
  Datagram back;
  REQUIRE(decode(hub.datagram_for(1, 0), back) == DecodeStatus::ok);
  CHECK(back.chat.empty());
  deliver(hub, a, heard);
  CHECK(a.chat_held() == 0);
  CHECK(heard.size() == 2);  // nothing back where it came from
}

TEST(a_line_past_a_gap_waits_and_every_line_arrives_once_in_order) {
  const LockstepConfig config = three();
  LinkNode a(kMatch, config, 1, {0});
  LinkNode hub(kMatch, config, 0, {1, 2});
  LinkNode b(kMatch, config, 2, {0});
  // Line 0 is lost on the way to the hub; line 1 arrives alone.
  (void)a.say(said("one"));
  Datagram lost;
  REQUIRE(decode(a.datagram_for(0, 0), lost) == DecodeStatus::ok);
  (void)a.say(said("two"));
  Datagram only_second;
  REQUIRE(decode(a.datagram_for(0, 1), only_second) == DecodeStatus::ok);
  REQUIRE(only_second.chat.size() == 2);
  only_second.chat.erase(only_second.chat.begin());
  std::vector<ChatLine> hub_heard;
  LinkReceipt receipt = hub.receive(encode(only_second), 1);
  CHECK(receipt.chat.empty());  // past the gap: not taken
  // The next datagram repairs it, and both come, in order.
  deliver(a, hub, hub_heard, 2);
  REQUIRE(hub_heard.size() == 2);
  CHECK(hub_heard[0].seq == 0);
  CHECK(hub_heard[1].seq == 1);
  // Through the relay to the far side, once each.
  std::vector<ChatLine> b_heard;
  deliver(hub, b, b_heard, 3);
  deliver(hub, b, b_heard, 4);
  REQUIRE(b_heard.size() == 2);
  CHECK(b_heard[0].text == "one");
  CHECK(b_heard[0].from == 1);
  CHECK(b_heard[1].text == "two");
}

TEST(a_line_a_link_has_acknowledged_is_not_sent_it_again) {
  LockstepConfig config;
  config.peers = {0, 1, 2, 3};
  LinkNode hub(kMatch, config, 0, {1, 2, 3});
  LinkNode a(kMatch, config, 1, {0});
  LinkNode b(kMatch, config, 2, {0});
  (void)a.say(said("once"));
  std::vector<ChatLine> heard;
  deliver(a, hub, heard);
  deliver(hub, b, heard);
  REQUIRE(heard.size() == 2);
  deliver(b, hub, heard);  // b acknowledges; peer 3 never has
  CHECK(hub.chat_held() == 1);
  Datagram to_b;
  REQUIRE(decode(hub.datagram_for(2, 1), to_b) == DecodeStatus::ok);
  CHECK(to_b.chat.empty());
  Datagram to_c;
  REQUIRE(decode(hub.datagram_for(3, 1), to_c) == DecodeStatus::ok);
  CHECK(to_c.chat.size() == 1);
}

TEST(chat_never_pushes_a_turn_packet_out_of_a_datagram) {
  const LockstepConfig config = three();
  LinkNode a(kMatch, config, 1, {0});
  for (int i = 0; i < 20; ++i) (void)a.say(said(std::string(kMaxChatText, 'x')));
  TurnPacket packet;
  packet.peer = 1;
  packet.turn = 2;
  packet.proposed_ms = 400;
  a.offer(packet);
  Datagram datagram;
  const std::vector<std::byte> bytes = a.datagram_for(0, 0);
  REQUIRE(decode(bytes, datagram) == DecodeStatus::ok);
  CHECK(datagram.packets.size() == 1);
  CHECK(!datagram.chat.empty());
  CHECK(datagram.chat.size() < 20);  // the rest wait
  CHECK(bytes.size() < kDatagramBudget + 200);
}

TEST(a_node_refuses_a_line_it_could_not_have_been_sent) {
  const LockstepConfig config = three();
  LinkNode hub(kMatch, config, 0, {1});
  Datagram datagram;
  datagram.match = kMatch;
  datagram.from = 1;
  datagram.hold = kNoEcho;
  ChatLine stranger = said("x");
  stranger.from = 9;
  ChatLine own = said("y");
  own.from = 0;  // this node's own, echoed
  datagram.chat = {encode(stranger), encode(own), {std::byte{1}, std::byte{2}}};
  const LinkReceipt receipt = hub.receive(encode(datagram), 0);
  CHECK(receipt.refused == 3);
  CHECK(receipt.chat.empty());
}

TEST(chat_through_a_lossy_star_is_every_line_once_in_order_and_changes_no_hash) {
  const QuietScenario scenario;
  NetplayOptions options;
  options.lockstep.peers = {0, 2, 5, 6};
  options.seed = 0x61;
  options.relay = true;
  options.loss_per_mille = 250;
  CommandStream intent;
  intent.turns.resize(30);
  for (std::size_t t = 0; t < 30; t += 2) {
    NetOrder order;
    order.issuer = options.lockstep.peers[t / 2 % 4];
    order.actors = {1};
    intent.turns[t].orders.push_back(order);
  }
  NetplayReport quiet;
  REQUIRE(!check_netplay(scenario, 4, 30, intent, options, &quiet).diverged());
  options.chat_every = 3;
  NetplayReport chatty;
  const conformance::Divergence divergence =
      check_netplay(scenario, 4, 30, intent, options, &chatty);
  CHECK(!divergence.diverged());
  CHECK(chatty.chat_said > 20);
  CHECK(chatty.chat_heard > chatty.chat_said);  // three listeners each
  CHECK(chatty.chat_misordered == 0);
  CHECK(chatty.refused == 0);
  // The same match: every turn's world, and the agreed orders.
  REQUIRE(quiet.hashes.size() == 30);
  CHECK(chatty.hashes == quiet.hashes);
  CHECK(chatty.schedule == quiet.schedule);
  CHECK(chatty.orders == quiet.orders);
}

// --------------------------------------------------------------------------
// the lobby
// --------------------------------------------------------------------------

TEST(a_roster_and_a_choice_carry_chat) {
  Start roster;
  roster.you = 2;
  roster.chat_heard = 3;
  for (int i = 0; i < 20; ++i) {
    ChatLine line = said("line " + std::to_string(i));
    line.from = static_cast<PlayerId>(i % 3);
    line.seq = static_cast<std::uint32_t>(i);
    roster.chat.push_back(line);
  }
  Start back;
  REQUIRE(decode(encode(roster, /*final=*/false), back) == DecodeStatus::ok);
  CHECK(back.chat_heard == 3);
  // The last lines, oldest first.
  REQUIRE(back.chat.size() == kLobbyChatLines);
  CHECK(back.chat.front().text == "line 8");
  CHECK(back.chat.back().text == "line 19");

  Choice choice;
  choice.ready = true;
  choice.chat = {said("gg"), said("glhf")};
  choice.chat[1].seq = 1;
  Choice again;
  REQUIRE(decode(encode(choice), again) == DecodeStatus::ok);
  REQUIRE(again.chat.size() == 2);
  CHECK(again.chat[1].text == "glhf");
  CHECK(again.chat[1].seq == 1);
  // A count past what any encoder writes is refused.
  std::vector<std::byte> bytes = encode(Choice{});
  bytes.back() = std::byte{kLobbyChatLines + 1};
  CHECK(decode(bytes, again) == DecodeStatus::bad_field);
}
