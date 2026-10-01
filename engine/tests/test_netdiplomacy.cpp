// The Diplomacy screen's OK: a `diplomacy` order, agreed like any other.
//
// What carries the reading (0x006cb680, 0x004e5f80, 0x004e6030):
//
//   * The screen writes nothing itself. OK posts one command per row whose
//     word changed -- the issuer, the row's player, the whole new word -- and
//     one with no second player when the Allied victory box changed, and every
//     peer's execution writes it. So it is an order, in the agreed history and
//     the stream hash.
//   * The word replaces the cell, all of it, in the issuer's row only: the
//     matrix is not symmetric and `SetRelation` writes one record's row.
//   * `SetRelation` returns at once for a player's own record, so a word for
//     oneself changes nothing; no second player means the `allied` flag.

#include <cstdint>
#include <vector>

#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;
using imperivm::core::kNoPlayer;
using imperivm::core::PlayerId;

namespace {

NetOrder diplomacy(PlayerId issuer, PlayerId other, std::uint32_t word) {
  NetOrder order;
  order.issuer = issuer;
  order.kind = NetOrderKind::diplomacy;
  order.other = other;
  order.relations = word;
  return order;
}

NetOrder allied(PlayerId issuer, bool on) {
  NetOrder order;
  order.issuer = issuer;
  order.kind = NetOrderKind::diplomacy;
  order.other = kNoPlayer;
  order.allied = on;
  return order;
}

TurnPacket packet(PlayerId peer, std::vector<NetOrder> orders) {
  TurnPacket out;
  out.peer = peer;
  out.turn = 3;
  out.proposed_ms = 400;
  for (std::size_t i = 0; i < orders.size(); ++i) {
    orders[i].issuer = peer;
    orders[i].sequence = static_cast<std::uint32_t>(i);
  }
  out.orders = std::move(orders);
  return out;
}

}  // namespace

TEST(a_diplomacy_order_survives_the_wire) {
  const TurnPacket sent = packet(2, {diplomacy(2, 5, kRelationFriendly), allied(2, true)});
  TurnPacket back;
  REQUIRE(decode(encode(sent), back) == DecodeStatus::ok);
  REQUIRE(back.orders.size() == 2);
  CHECK(back.orders[0].kind == NetOrderKind::diplomacy);
  CHECK(back.orders[0].other == 5);
  CHECK(back.orders[0].relations == kRelationFriendly);
  CHECK(back.orders[1].other == kNoPlayer);
  CHECK(back.orders[1].allied);
}

TEST(a_diplomacy_order_naming_no_real_seat_is_refused_on_the_wire) {
  std::vector<std::byte> bytes = encode(packet(2, {diplomacy(2, 5, 1)}));
  // The tail is the last six bytes: other, relations, allied.
  bytes[bytes.size() - 6] = std::byte{16};
  TurnPacket back;
  CHECK(decode(bytes, back) == DecodeStatus::bad_field);
  bytes[bytes.size() - 6] = std::byte{5};
  bytes[bytes.size() - 1] = std::byte{2};
  CHECK(decode(bytes, back) == DecodeStatus::bad_field);
}

TEST(a_diplomacy_order_writes_the_issuers_cell_whole_and_only_it) {
  World world;
  world.players().set_relation_word(0, 3, 0x02);  // a bit no screen shows
  NetTurn turn;
  turn.orders.push_back(diplomacy(0, 3, kRelationAllied));
  const NetTurnReport report = apply_turn(world, turn);
  CHECK(report.diplomacy == 1);
  CHECK(world.players().relation_word(0, 3) == kRelationAllied);
  // One row: the transpose is untouched.
  CHECK(world.players().relation_word(3, 0) == 0);
  CHECK(world.players().has(0, 3, Relation::ceasefire));
  CHECK(!world.players().are_allied(0, 3));
}

TEST(a_diplomacy_order_for_oneself_changes_nothing_and_no_seat_sets_the_flag) {
  World world;
  const std::uint32_t self = world.players().relation_word(1, 1);
  std::uint64_t before = 0;
  world.players().hash(before);
  NetTurn turn;
  turn.orders.push_back(diplomacy(1, 1, 0));
  turn.orders.push_back(allied(1, true));
  const NetTurnReport report = apply_turn(world, turn);
  CHECK(world.players().relation_word(1, 1) == self);
  std::uint64_t after = 0;
  world.players().hash(after);
  CHECK(after == before);
  CHECK(world.players().setup(1).allied_flag);
  CHECK(report.diplomacy == 1);
}

TEST(a_diplomacy_order_moves_the_stream_hash_and_other_streams_keep_theirs) {
  CommandStream a;
  CommandStream b;
  a.turns.resize(1);
  b.turns.resize(1);
  a.turns[0].orders.push_back(diplomacy(0, 1, 0x01));
  b.turns[0].orders.push_back(diplomacy(0, 1, 0x11));
  CHECK(stream_hash(a, 1) != stream_hash(b, 1));
  b.turns[0].orders[0] = allied(0, true);
  CommandStream c = b;
  c.turns[0].orders[0].allied = false;
  CHECK(stream_hash(b, 1) != stream_hash(c, 1));
  // A kind that does not carry the fields does not fold them.
  NetOrder speed;
  speed.kind = NetOrderKind::set_speed;
  speed.speed = 1400;
  NetOrder stray = speed;
  stray.other = 4;
  stray.relations = 0x35;
  std::uint64_t x = 1;
  std::uint64_t y = 1;
  hash_order(x, speed);
  hash_order(y, stray);
  CHECK(x == y);
}
