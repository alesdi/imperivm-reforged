// The lobby messages: sim/netlobby.hpp.
//
// Codecs only -- the exchange needs a socket and lives in engine/net, where
// `tests/test_corpus_udp.py` runs it between processes. What carries the
// reading here is the version rule: a `Hello` or `Start` from another build is
// not decoded, because its layout is not ours, but a `Refuse` is read whatever
// its version, because "you are a different build" is exactly the refusal a
// different build has to be able to read.

#include <cstdint>
#include <string>
#include <vector>

#include "imperivm/core/sim/netlobby.hpp"
#include "test.hpp"

using namespace imperivm::core::sim;

namespace {

Start sample_start() {
  Start start;
  start.match = 0xA1B2C3D4;
  start.you = 3;
  start.seed = 77;
  start.turns = 120;
  start.map_index = 7;
  start.difficulty = 2;
  start.config.peers = {0, 3, 5};
  start.config.input_delay = 3;
  start.config.initial_ms = 460;
  start.config.min_ms = 200;
  start.config.max_ms = 800;
  start.config.game_speed = 999;
  start.config.coordinator = 5;
  start.config.variable_speed = false;
  start.links = {0};
  return start;
}

template <typename Message>
void every_prefix_refused(const Message& message) {
  const std::vector<std::byte> bytes = encode(message);
  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    Message out;
    CHECK(decode(std::span<const std::byte>(bytes.data(), cut), out) != DecodeStatus::ok);
  }
  std::vector<std::byte> longer = bytes;
  longer.push_back(std::byte{0});
  Message out;
  CHECK(decode(longer, out) == DecodeStatus::trailing);
}

/// The protocol word sits right after the magic in every lobby message.
std::vector<std::byte> from_another_build(std::vector<std::byte> bytes) {
  bytes[4] = static_cast<std::byte>(static_cast<std::uint8_t>(bytes[4]) + 1);
  return bytes;
}

}  // namespace

TEST(a_hello_survives_the_wire) {
  Hello hello;
  hello.content = 0x0123456789ABCDEFull;
  hello.name = "Vercingetorix";
  Hello back;
  REQUIRE(decode(encode(hello), back) == DecodeStatus::ok);
  CHECK(back.protocol == kNetProtocol);
  CHECK(back.content == hello.content);
  CHECK(back.name == "Vercingetorix");
  every_prefix_refused(hello);
}

TEST(a_hello_name_is_capped_on_the_way_out_and_refused_past_the_cap_on_the_way_in) {
  Hello hello;
  hello.name = std::string(200, 'x');
  Hello back;
  REQUIRE(decode(encode(hello), back) == DecodeStatus::ok);
  CHECK(back.name.size() == 64);

  // Handcrafted: a length of 65 with the bytes behind it.
  std::vector<std::byte> bytes = encode(Hello{});
  bytes.resize(bytes.size() - 4);
  bytes.push_back(std::byte{65});
  for (int i = 0; i < 3; ++i) bytes.push_back(std::byte{0});
  bytes.resize(bytes.size() + 65, std::byte{'y'});
  CHECK(decode(bytes, back) == DecodeStatus::bad_field);
}

TEST(a_start_survives_the_wire) {
  const Start start = sample_start();
  Start back;
  REQUIRE(decode(encode(start), back) == DecodeStatus::ok);
  CHECK(back.match == start.match);
  CHECK(back.you == 3);
  CHECK(back.seed == 77);
  CHECK(back.turns == 120);
  CHECK(back.map_index == 7);
  CHECK(back.difficulty == 2);
  CHECK(back.config.peers == start.config.peers);
  CHECK(back.config.input_delay == 3);
  CHECK(back.config.initial_ms == 460);
  CHECK(back.config.min_ms == 200);
  CHECK(back.config.max_ms == 800);
  CHECK(back.config.game_speed == 999);
  CHECK(back.config.coordinator == 5);
  CHECK(!back.config.variable_speed);
  CHECK(back.links == start.links);
  every_prefix_refused(start);
}

TEST(a_start_that_could_not_run_is_refused) {
  Start start = sample_start();
  Start back;
  start.config.game_speed = 0;
  CHECK(decode(encode(start), back) == DecodeStatus::bad_field);
  start = sample_start();
  start.config.max_ms = 100;  // below the minimum
  CHECK(decode(encode(start), back) == DecodeStatus::bad_field);
  start = sample_start();
  start.config.peers.clear();
  CHECK(decode(encode(start), back) == DecodeStatus::bad_field);
  start = sample_start();
  start.difficulty = 3;
  CHECK(decode(encode(start), back) == DecodeStatus::bad_field);
  // A coordinator who is not in the match could decide nothing.
  start = sample_start();
  start.config.coordinator = 4;
  CHECK(decode(encode(start), back) == DecodeStatus::bad_field);
  start.config.coordinator = imperivm::core::kNoPlayer;  // the lowest peer
  CHECK(decode(encode(start), back) == DecodeStatus::ok);
}

TEST(another_build_is_not_decoded_but_its_refusal_is) {
  Hello hello;
  CHECK(decode(from_another_build(encode(hello)), hello) == DecodeStatus::bad_version);
  Start start;
  CHECK(decode(from_another_build(encode(sample_start())), start) == DecodeStatus::bad_version);

  Refuse refuse;
  refuse.reason = Refuse::Reason::protocol;
  Refuse back;
  REQUIRE(decode(from_another_build(encode(refuse)), back) == DecodeStatus::ok);
  CHECK(back.reason == Refuse::Reason::protocol);
  every_prefix_refused(refuse);
}

TEST(a_refusal_reason_out_of_range_is_refused) {
  std::vector<std::byte> bytes = encode(Refuse{});
  bytes.back() = std::byte{7};
  Refuse back;
  CHECK(decode(bytes, back) == DecodeStatus::bad_field);
}

TEST(lobby_kind_reads_the_magic_and_nothing_else) {
  CHECK(lobby_kind(encode(Hello{})) == LobbyKind::hello);
  CHECK(lobby_kind(encode(sample_start())) == LobbyKind::start);
  CHECK(lobby_kind(encode(Refuse{})) == LobbyKind::refuse);
  const std::vector<std::byte> link = {std::byte{'I'}, std::byte{'M'}, std::byte{'L'},
                                       std::byte{'D'}};
  CHECK(lobby_kind(link) == LobbyKind::none);
  CHECK(lobby_kind(std::span<const std::byte>()) == LobbyKind::none);
}

TEST(a_message_of_one_kind_does_not_decode_as_another) {
  Start start;
  CHECK(decode(encode(Hello{}), start) == DecodeStatus::bad_magic);
  Hello hello;
  CHECK(decode(encode(sample_start()), hello) == DecodeStatus::bad_magic);
}

// --------------------------------------------------------------------------
// the players' screen: roster, choice, discovery
// --------------------------------------------------------------------------

namespace {

Start sample_roster() {
  Start roster = sample_start();
  roster.map = "Scenarios/Crossroads.BFHP";
  roster.map_hash = 0xFEEDFACECAFEBEEFull;
  SeatRow host;
  host.slot = 0;
  host.type = SeatRow::Type::human;
  host.name = "Hannibal";
  host.race = 2;
  host.team = 1;
  host.ready = true;
  SeatRow open;
  open.slot = 3;
  open.type = SeatRow::Type::open;
  SeatRow ai;
  ai.slot = 5;
  ai.type = SeatRow::Type::computer;
  ai.difficulty = 2;
  ai.bonus = 3;
  roster.rows = {host, open, ai};
  roster.rules.victory = "1 ELIMINATION";
  roster.rules.threshold = "3";
  roster.rules.world_population = 150;
  roster.rules.starting_gold = 5000;
  roster.rules.fog_of_war = false;
  roster.rules.shared_control = true;
  return roster;
}

}  // namespace

TEST(a_roster_survives_the_wire_and_says_it_is_a_roster) {
  const Start roster = sample_roster();
  const std::vector<std::byte> bytes = encode(roster, /*final=*/false);
  CHECK(lobby_kind(bytes) == LobbyKind::roster);
  Start back;
  REQUIRE(decode(bytes, back) == DecodeStatus::ok);
  CHECK(back.map == "Scenarios/Crossroads.BFHP");
  CHECK(back.map_hash == 0xFEEDFACECAFEBEEFull);
  REQUIRE(back.rows.size() == 3);
  CHECK(back.rows[0].name == "Hannibal");
  CHECK(back.rows[0].type == SeatRow::Type::human);
  CHECK(back.rows[0].race == 2);
  CHECK(back.rows[0].team == 1);
  CHECK(back.rows[0].ready);
  CHECK(back.rows[1].type == SeatRow::Type::open);
  CHECK(back.rows[1].slot == 3);
  CHECK(back.rows[2].difficulty == 2);
  CHECK(back.rows[2].bonus == 3);
  CHECK(back.rules.victory == "1 ELIMINATION");
  CHECK(back.rules.threshold == "3");
  CHECK(back.rules.world_population == 150);
  CHECK(back.rules.starting_gold == 5000);
  CHECK(!back.rules.fog_of_war);
  CHECK(back.rules.exploration);
  CHECK(back.rules.shared_control);
  CHECK(!back.rules.shared_support);

  for (std::size_t cut = 0; cut < bytes.size(); ++cut) {
    Start out;
    CHECK(decode(std::span<const std::byte>(bytes.data(), cut), out) != DecodeStatus::ok);
  }
}

TEST(a_start_never_carries_an_open_seat) {
  // A lobby may wait for a joiner; a match cannot. The host closes unfilled
  // seats before it starts, and a start that did not is refused.
  const Start roster = sample_roster();
  Start back;
  CHECK(decode(encode(roster, /*final=*/true), back) == DecodeStatus::bad_field);
  Start filled = roster;
  filled.rows[1].type = SeatRow::Type::closed;
  CHECK(decode(encode(filled, /*final=*/true), back) == DecodeStatus::ok);
}

TEST(a_roster_row_out_of_range_is_refused) {
  Start back;
  for (int field = 0; field < 5; ++field) {
    Start roster = sample_roster();
    SeatRow& row = roster.rows[2];
    if (field == 0) row.slot = 16;
    if (field == 1) row.race = 8;
    if (field == 2) row.difficulty = 3;
    if (field == 3) row.team = 5;
    if (field == 4) row.bonus = 4;
    CHECK(decode(encode(roster, false), back) == DecodeStatus::bad_field);
  }
}

TEST(a_choice_survives_the_wire_and_is_applied_within_bounds) {
  Choice choice;
  choice.race = 4;
  choice.team = 2;
  choice.bonus = 1;
  choice.ready = true;
  Choice back;
  REQUIRE(decode(encode(choice), back) == DecodeStatus::ok);
  CHECK(back.race == 4 && back.team == 2 && back.bonus == 1 && back.ready);
  every_prefix_refused(choice);

  SeatRow row;
  row.team = 3;
  row.bonus = 2;
  CHECK(apply_choice(row, back));
  CHECK(row.race == 4 && row.team == 2 && row.bonus == 1 && row.ready);
  CHECK(!apply_choice(row, back));  // nothing changed the second time

  // Out of range: a race becomes Random; a team or bonus keeps what it had.
  Choice wild;
  wild.race = 99;
  wild.team = 9;
  wild.bonus = 9;
  CHECK(apply_choice(row, wild));
  CHECK(row.race == -1);
  CHECK(row.team == 2);
  CHECK(row.bonus == 1);
  CHECK(!row.ready);
}

TEST(discovery_survives_the_wire) {
  Query query;
  Query query_back;
  REQUIRE(decode(encode(query), query_back) == DecodeStatus::ok);
  every_prefix_refused(query);

  Advert advert;
  advert.host = "Hannibal";
  advert.map = "Scenarios/Crossroads.BFHP";
  advert.port = 47811;
  advert.open = 1;
  advert.seats = 3;
  Advert back;
  REQUIRE(decode(encode(advert), back) == DecodeStatus::ok);
  CHECK(back.host == "Hannibal");
  CHECK(back.map == "Scenarios/Crossroads.BFHP");
  CHECK(back.port == 47811);
  CHECK(back.open == 1 && back.seats == 3);
  every_prefix_refused(advert);
  advert.open = 4;  // more free than there are
  CHECK(decode(encode(advert), back) == DecodeStatus::bad_field);
}

TEST(the_new_refusals_survive_the_wire) {
  for (const Refuse::Reason reason :
       {Refuse::Reason::closed, Refuse::Reason::left, Refuse::Reason::map, Refuse::Reason::silent}) {
    Refuse refuse;
    refuse.reason = reason;
    Refuse back;
    REQUIRE(decode(encode(refuse), back) == DecodeStatus::ok);
    CHECK(back.reason == reason);
    CHECK(std::string(describe(reason)) != "?");
  }
}

TEST(lobby_kind_knows_every_message) {
  CHECK(lobby_kind(encode(Choice{})) == LobbyKind::choice);
  CHECK(lobby_kind(encode(Query{})) == LobbyKind::query);
  CHECK(lobby_kind(encode(Advert{})) == LobbyKind::advert);
  CHECK(lobby_kind(encode(sample_start(), true)) == LobbyKind::start);
  CHECK(lobby_kind(encode(sample_start(), false)) == LobbyKind::roster);
}
