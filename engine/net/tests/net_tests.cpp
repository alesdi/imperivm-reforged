// The lobby and the match's departures, over real UDP sockets on loopback.
//
// `core_tests` proves every message's codec and may not open a socket; this
// proves the exchanges those messages make up, which are the net library's:
//
//   * joiners take the open rows in the order they say hello, and each sees
//     the roster with its own row marked;
//   * a joiner's choice changes its own row and no other, and every other
//     joiner sees it;
//   * the lobby can start only once every seated joiner is ready;
//   * a joiner that leaves frees its seat, and a host that closes a seated
//     row tells the joiner;
//   * another `data.pak`, or a full game, is refused;
//   * `finish` closes the seats nobody took, and the start every joiner
//     receives is the roster that was showing;
//   * a LAN query finds the lobby, with its free seats;
//   * a peer that leaves a match is seen to have left;
//   * of three, a joiner that leaves -- saying so, or going silent -- is
//     dropped at one turn every remaining peer agrees on, the rest play on
//     the same match, and the one that left played it too up to that turn;
//   * the host leaving ends the match for every joiner, visibly;
//   * the lobby's chat is one log on every screen, each line once, and a
//     line said in a match reaches every other peer through the host;
//   * a late joiner says hello to a running match, takes the seat the
//     computer took from a player who left, is sent the host's save through
//     loss, and from its first turn hashes like every other peer -- the
//     world, the stream, every channel -- through a speed change on the turn
//     before it; a match with no such seat refuses it, a second joiner waits
//     for the first, and one that never arrives is timed out and its seat
//     opens again. All of the late join is this engine's: the original
//     admits nobody once a match has started.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/net/match.hpp"
#include "imperivm/net/udp.hpp"
#include "test.hpp"

namespace imperivm::test {
int failures = 0;
int checks = 0;
}  // namespace imperivm::test

using namespace imperivm::core::sim;
using namespace imperivm::net;

namespace {

constexpr std::uint64_t kGame = 0xA11CE;

UdpSocket open_loopback() {
  std::optional<UdpSocket> socket = UdpSocket::open(0, /*loopback_only=*/true);
  if (!socket.has_value()) {
    std::printf("cannot open a loopback socket\n");
    std::abort();
  }
  return std::move(*socket);
}

Endpoint loopback(std::uint16_t port) { return Endpoint{0x7F000001u, port}; }

SeatRow row(PlayerId slot, SeatRow::Type type, std::string name = {}) {
  SeatRow r;
  r.slot = slot;
  r.type = type;
  r.name = std::move(name);
  return r;
}

/// The players' screen a host opens: itself on 0, open seats on 2 and 5, a
/// computer on 4.
Start screen() {
  Start base;
  base.you = 0;
  base.map = "Scenarios/Crossroads.BFHP";
  base.map_hash = 0xBEEF;
  base.rows = {row(0, SeatRow::Type::human, "Hannibal"), row(2, SeatRow::Type::open),
               row(4, SeatRow::Type::computer), row(5, SeatRow::Type::open)};
  base.rules.victory = "1 ELIMINATION";
  return base;
}

Hello hello(std::string name, std::uint64_t content = kGame) {
  Hello h;
  h.content = content;
  h.name = std::move(name);
  return h;
}

/// Step everything until `done` or a second passes. Real sockets, so the
/// loop waits a little between rounds rather than spinning.
bool settle(const std::function<void(std::uint32_t)>& step, const std::function<bool()>& done) {
  const std::uint32_t begun = now_ms();
  while (now_ms() - begun < 1000) {
    step(now_ms());
    if (done()) return true;
    struct timespec pause {0, 2'000'000};
    nanosleep(&pause, nullptr);
  }
  return false;
}

const SeatRow* find_row(const std::vector<SeatRow>& rows, PlayerId slot) {
  for (const SeatRow& r : rows) {
    if (r.slot == slot) return &r;
  }
  return nullptr;
}

/// A host and two joiners, seated in that order.
struct Table {
  UdpSocket host_socket = open_loopback();
  UdpSocket a_socket = open_loopback();
  UdpSocket b_socket = open_loopback();
  HostLobby host{host_socket, screen(), {}, kGame, "Hannibal"};
  JoinLobby a{a_socket, loopback(host_socket.port()), hello("Scipio")};
  std::optional<JoinLobby> b;

  void step_all(std::uint32_t now) {
    (void)host.step(now);
    (void)a.step(now);
    if (b.has_value()) (void)b->step(now);
  }

  bool seat_both() {
    if (!settle([this](std::uint32_t now) { step_all(now); },
                [this] { return host.seated().size() == 1; })) {
      return false;
    }
    b.emplace(b_socket, loopback(host_socket.port()), hello("Viriathus"));
    return settle([this](std::uint32_t now) { step_all(now); },
                  [this] { return host.seated().size() == 2 && b->roster().you == 5; });
  }
};

}  // namespace

TEST(joiners_take_the_open_rows_in_the_order_they_say_hello) {
  Table t;
  REQUIRE(t.seat_both());
  CHECK(t.host.seated().count(2) == 1);
  CHECK(t.host.seated().count(5) == 1);
  CHECK(t.host.names().at(2) == "Scipio");
  CHECK(t.host.names().at(5) == "Viriathus");
  // Each sees the roster with its own row marked, and the host's rows.
  REQUIRE(settle([&t](std::uint32_t now) { t.step_all(now); },
                 [&t] { return t.a.roster().rows.size() == 4 && t.a.roster().you == 2; }));
  const SeatRow* hannibal = find_row(t.a.roster().rows, 0);
  REQUIRE(hannibal != nullptr);
  CHECK(hannibal->name == "Hannibal");
  CHECK(t.a.roster().map == "Scenarios/Crossroads.BFHP");
  CHECK(t.a.roster().rules.victory == "1 ELIMINATION");
}

TEST(a_choice_changes_its_own_row_and_everybody_sees_it) {
  Table t;
  REQUIRE(t.seat_both());
  Choice choice;
  choice.race = 3;
  choice.team = 2;
  choice.ready = true;
  t.a.set_choice(choice);
  REQUIRE(settle([&t](std::uint32_t now) { t.step_all(now); }, [&t] {
    const SeatRow* seen = find_row(t.b->roster().rows, 2);
    return seen != nullptr && seen->race == 3 && seen->ready;
  }));
  const SeatRow* mine = find_row(t.host.rows(), 2);
  const SeatRow* theirs = find_row(t.host.rows(), 5);
  REQUIRE(mine != nullptr && theirs != nullptr);
  CHECK(mine->race == 3);
  CHECK(mine->team == 2);
  CHECK(theirs->race == -1);
  CHECK(!theirs->ready);
  CHECK(find_row(t.host.rows(), 0)->race == -1);  // nor the host's
}

TEST(the_lobby_starts_only_when_every_joiner_is_ready) {
  Table t;
  REQUIRE(t.seat_both());
  Choice ready;
  ready.ready = true;
  t.a.set_choice(ready);
  // One ready is not enough.
  REQUIRE(settle([&t](std::uint32_t now) { t.step_all(now); },
                 [&t] { return find_row(t.host.rows(), 2)->ready; }));
  CHECK(!t.host.step(now_ms()));
  t.b->set_choice(ready);
  CHECK(settle([&t](std::uint32_t now) { t.step_all(now); },
               [&t] { return t.host.step(now_ms()); }));
}

TEST(a_joiner_that_leaves_frees_its_seat) {
  Table t;
  REQUIRE(t.seat_both());
  t.b->leave(Refuse::Reason::left);
  REQUIRE(settle([&t](std::uint32_t now) { t.step_all(now); },
                 [&t] { return t.host.seated().size() == 1; }));
  CHECK(t.host.seated().count(2) == 1);
  CHECK(find_row(t.host.rows(), 5)->type == SeatRow::Type::open);
  CHECK(find_row(t.host.rows(), 5)->name.empty());
}

TEST(a_host_that_closes_a_seated_row_tells_the_joiner) {
  Table t;
  REQUIRE(t.seat_both());
  for (SeatRow& r : t.host.rows()) {
    if (r.slot == 2) r.type = SeatRow::Type::closed;
  }
  REQUIRE(settle([&t](std::uint32_t now) { t.step_all(now); },
                 [&t] { return t.a.step(now_ms()) == JoinLobby::State::refused; }));
  CHECK(t.a.result().error == "refused: the host closed the game");
  CHECK(t.host.seated().count(2) == 0);
  CHECK(t.host.seated().count(5) == 1);
}

TEST(another_game_and_a_full_game_are_refused) {
  Table t;
  REQUIRE(t.seat_both());
  UdpSocket stranger_socket = open_loopback();
  JoinLobby stranger(stranger_socket, loopback(t.host_socket.port()), hello("Brennus", kGame + 1));
  REQUIRE(settle([&](std::uint32_t now) { t.step_all(now); (void)stranger.step(now); },
                 [&] { return stranger.step(now_ms()) == JoinLobby::State::refused; }));
  CHECK(stranger.result().error == "refused: a different game: data.pak differs");

  UdpSocket late_socket = open_loopback();
  JoinLobby late(late_socket, loopback(t.host_socket.port()), hello("Brennus"));
  REQUIRE(settle([&](std::uint32_t now) { t.step_all(now); (void)late.step(now); },
                 [&] { return late.step(now_ms()) == JoinLobby::State::refused; }));
  CHECK(late.result().error == "refused: the game is full");
}

TEST(finish_closes_the_seats_nobody_took_and_starts_every_joiner_alike) {
  UdpSocket host_socket = open_loopback();
  UdpSocket a_socket = open_loopback();
  HostLobby host(host_socket, screen(), {}, kGame, "Hannibal");
  JoinLobby a(a_socket, loopback(host_socket.port()), hello("Scipio"));
  Choice ready;
  ready.ready = true;
  a.set_choice(ready);
  REQUIRE(settle([&](std::uint32_t now) { (void)host.step(now); (void)a.step(now); },
                 [&] { return host.step(now_ms()); }));
  const Lobby lobby = host.finish(/*seed=*/77);
  REQUIRE(lobby.ok);
  CHECK(find_row(lobby.start.rows, 5)->type == SeatRow::Type::closed);
  CHECK(lobby.start.config.peers == (std::vector<PlayerId>{0, 2}));
  REQUIRE(settle([&](std::uint32_t now) { (void)a.step(now); },
                 [&] { return a.step(now_ms()) == JoinLobby::State::started; }));
  const Start& theirs = a.result().start;
  CHECK(theirs.you == 2);
  CHECK(theirs.seed == 77);
  CHECK(theirs.links == (std::vector<PlayerId>{0}));
  CHECK(theirs.rows.size() == lobby.start.rows.size());
  CHECK(find_row(theirs.rows, 5)->type == SeatRow::Type::closed);
  CHECK(find_row(theirs.rows, 2)->name == "Scipio");
}

TEST(a_lan_query_finds_the_lobby_and_its_free_seats) {
  UdpSocket host_socket = open_loopback();
  HostLobby host(host_socket, screen(), {}, kGame, "Hannibal");
  UdpSocket browser_socket = open_loopback();
  LanBrowser browser(browser_socket, host_socket.port());
  browser.refresh(now_ms());
  REQUIRE(settle([&](std::uint32_t now) { (void)host.step(now); browser.step(now); },
                 [&] { return !browser.games().empty(); }));
  const LanBrowser::Found& found = browser.games().front();
  CHECK(found.advert.host == "Hannibal");
  CHECK(found.advert.map == "Scenarios/Crossroads.BFHP");
  CHECK(found.advert.open == 2);
  CHECK(found.advert.seats == 3);
  CHECK(found.at.port == host_socket.port());
  // Asked again, it is still one game, not one per address it answered on.
  browser.refresh(now_ms());
  (void)settle([&](std::uint32_t now) { (void)host.step(now); browser.step(now); }, [] { return false; });
  CHECK(browser.games().size() == 1);
}

TEST(a_peer_that_leaves_a_match_is_seen_to_have_left) {
  UdpSocket host_socket = open_loopback();
  UdpSocket a_socket = open_loopback();
  HostLobby host(host_socket, screen(), {}, kGame, "Hannibal");
  JoinLobby a(a_socket, loopback(host_socket.port()), hello("Scipio"));
  Choice ready;
  ready.ready = true;
  a.set_choice(ready);
  REQUIRE(settle([&](std::uint32_t now) { (void)host.step(now); (void)a.step(now); },
                 [&] { return host.step(now_ms()); }));
  const Lobby hosted = host.finish(1);
  REQUIRE(settle([&](std::uint32_t now) { (void)a.step(now); },
                 [&] { return a.step(now_ms()) == JoinLobby::State::started; }));
  NetMatch host_match(host_socket, hosted);
  NetMatch join_match(a_socket, a.result());
  join_match.leave();
  REQUIRE(settle([&](std::uint32_t now) { host_match.pump(now); },
                 [&] { return !host_match.departures().empty(); }));
  CHECK(host_match.departures()[0].player == 2);
  CHECK(host_match.departures()[0].reason == Refuse::Reason::left);
  // The last joiner gone, the host plays on alone rather than ending.
  CHECK(!host_match.ended().has_value());
}

// --------------------------------------------------------------------------
// a peer leaving a match of three
// --------------------------------------------------------------------------

namespace {

/// A save of a bare world, as `GameSession::save` is of a session.
std::vector<std::byte> save_world(const World& world) {
  std::vector<std::byte> out;
  const SaveInputs inputs;
  if (!write_save(world, inputs, out).ok()) out.clear();
  return out;
}

bool load_world(std::span<const std::byte> bytes, World& world) {
  const LoadOptions options;
  const imperivm::core::Result<LoadReport> loaded = read_save(bytes, world, options);
  return loaded.ok() && verify_hashes(world, loaded->meta).ok;
}

/// One peer of a three-peer match: its socket, its match, the turns it ran,
/// and a world it runs them on, whose every channel it records per turn.
struct Seat {
  Seat() {
    world.spawn(imperivm::core::NativeClass::unit, nullptr);
    world.spawn(imperivm::core::NativeClass::unit, nullptr);
  }
  UdpSocket socket = open_loopback();
  std::optional<NetMatch> match;
  std::vector<AgreedTurn> turns;
  bool stopped = false;  ///< left, or crashed: pumps nothing more
  World world;
  /// Per turn run, by the match's turn: the world's channels, `netcmds` the
  /// stream hash through it, rolled up.
  std::map<std::uint32_t, WorldHashes> hashes;
  std::uint32_t first_turn = 0;
  /// A speed to ask for with the next packet, and one to ask for with the
  /// first packet after seating a late joiner.
  std::int32_t speed = 0;
  std::int32_t speed_on_seating = 0;
  std::size_t states_sent = 0;

  /// Receive, send, and run every turn agreed, giving one order a turn --
  /// and hand a late joiner the state when it is due, or load it.
  void step(std::uint32_t now, std::size_t limit) {
    if (stopped || !match.has_value()) return;
    match->pump(now);
    if (speed_on_seating != 0 && match->seating().has_value()) {
      speed = speed_on_seating;
      speed_on_seating = 0;
    }
    if (match->wants_state().has_value()) {
      match->provide_state(save_world(world), now);
      ++states_sent;
    }
    if (match->awaiting_state()) {
      const std::optional<std::span<const std::byte>> state = match->state();
      if (!state.has_value()) return;
      if (!load_world(*state, world)) {
        std::printf("  the late joiner's state did not load\n");
        stopped = true;
        return;
      }
      match->state_loaded();
    }
    while (turns.size() + first_turn < limit) {
      std::optional<AgreedTurn> turn = match->take();
      if (!turn.has_value()) break;
      (void)apply_turn(world, turn->orders);
      world.advance(turn->length);
      WorldHashes h = world.hashes();
      h.netcmds = stream_hash(match->negotiator().history(), turn->index + 1);
      h.hash_of_hashes = conformance::roll_up(h);
      hashes[turn->index] = h;
      turns.push_back(std::move(*turn));
      NetOrder order;
      order.actors = {static_cast<ObjectId>(100 + match->negotiator().local())};
      order.target.point = Point{static_cast<std::int32_t>(turns.size()), 0};
      std::vector<NetOrder> orders = {order};
      if (speed != 0) {
        NetOrder faster;
        faster.kind = NetOrderKind::set_speed;
        faster.speed = speed;
        orders.push_back(faster);
        speed = 0;
      }
      (void)match->submit(std::move(orders), now);
    }
  }
  [[nodiscard]] std::uint32_t reached() const noexcept {
    return first_turn + static_cast<std::uint32_t>(turns.size());
  }
};

/// A host on slot 0 and two joiners, seated on 2 and 5 in the order they
/// said hello -- which here is not fixed, so the tests ask each seat its slot.
struct Three {
  Seat host;
  Seat a;
  Seat b;
  Lobby host_lobby;

  bool start() {
    Start base;
    base.you = 0;
    base.map = "Scenarios/Crossroads.BFHP";
    HostLobby lobby(host.socket, base, {2, 5}, kGame);
    JoinLobby join_a(a.socket, loopback(host.socket.port()), hello("Scipio"));
    JoinLobby join_b(b.socket, loopback(host.socket.port()), hello("Viriathus"));
    Choice ready;
    ready.ready = true;
    join_a.set_choice(ready);
    join_b.set_choice(ready);
    if (!settle([&](std::uint32_t now) { (void)join_a.step(now); (void)join_b.step(now); },
                [&] { return lobby.step(now_ms()); })) {
      return false;
    }
    const Lobby hosted = lobby.finish(3);
    host_lobby = hosted;
    if (!settle([&](std::uint32_t now) { (void)join_a.step(now); (void)join_b.step(now); }, [&] {
          return join_a.step(now_ms()) == JoinLobby::State::started &&
                 join_b.step(now_ms()) == JoinLobby::State::started;
        })) {
      return false;
    }
    host.match.emplace(host.socket, hosted);
    a.match.emplace(a.socket, join_a.result());
    b.match.emplace(b.socket, join_b.result());
    for (Seat* seat : {&host, &a, &b}) (void)seat->match->submit({}, now_ms());
    return true;
  }

  /// Step every seat until `done`, for up to `ms`.
  bool run(std::size_t limit, const std::function<bool()>& done, std::uint32_t ms = 5000) {
    const std::uint32_t begun = now_ms();
    while (now_ms() - begun < ms) {
      const std::uint32_t now = now_ms();
      for (Seat* seat : {&host, &a, &b}) seat->step(now, limit);
      if (done()) return true;
      struct timespec pause {0, 1'000'000};
      nanosleep(&pause, nullptr);
    }
    return false;
  }
};

namespace conformance = imperivm::core::sim::conformance;

std::uint64_t history_hash(const NetMatch& match, std::size_t turns) {
  return stream_hash(match.negotiator().history(), turns);
}

/// Orders `peer` gave, as opposed to its departure the negotiator wrote in.
bool has_order_from(const AgreedTurn& turn, PlayerId peer) {
  for (const NetOrder& order : turn.orders.orders) {
    if (order.issuer == peer && order.kind != NetOrderKind::departed) return true;
  }
  return false;
}

/// How many orders of `kind` for `peer`'s seat the turn carries.
std::size_t count_of(const AgreedTurn& turn, PlayerId peer, NetOrderKind kind) {
  std::size_t count = 0;
  for (const NetOrder& order : turn.orders.orders) {
    if (order.issuer == peer && order.kind == kind) ++count;
  }
  return count;
}

/// How many takeovers of `peer`'s seat the turn carries.
std::size_t takeovers(const AgreedTurn& turn, PlayerId peer) {
  std::size_t count = 0;
  for (const NetOrder& order : turn.orders.orders) {
    if (order.issuer == peer && order.kind == NetOrderKind::departed) ++count;
  }
  return count;
}

}  // namespace

TEST(a_joiner_that_leaves_is_dropped_at_one_turn_and_the_rest_play_on) {
  Three t;
  REQUIRE(t.start());
  for (Seat* seat : {&t.host, &t.a, &t.b}) seat->match->set_drop(200, 7 + seat->socket.port());
  REQUIRE(t.run(40, [&] { return t.b.turns.size() >= 12; }));
  const PlayerId gone = t.b.match->negotiator().local();
  t.b.match->leave();
  t.b.stopped = true;
  REQUIRE(t.run(40, [&] { return t.host.turns.size() >= 40 && t.a.turns.size() >= 40; }, 10000));
  // Both agree on where its part ends, and it is past every turn it ran with
  // the others -- the twelve it ran needed their packets and they its.
  REQUIRE(t.host.match->departures().size() == 1);
  REQUIRE(t.a.match->departures().size() == 1);
  const NetDeparture& seen = t.host.match->departures()[0];
  CHECK(seen.player == gone);
  CHECK(seen.reason == Refuse::Reason::left);
  REQUIRE(seen.from_turn.has_value());
  CHECK(t.a.match->departures()[0].from_turn == seen.from_turn);
  CHECK(*seen.from_turn >= 12);
  // The same match on both, turn for turn: its orders up to the end, none
  // after. (Turn 2 is every peer's first packet, sent empty.)
  CHECK(history_hash(*t.host.match, 40) == history_hash(*t.a.match, 40));
  for (std::size_t turn = 0; turn < 40; ++turn) {
    CHECK(has_order_from(t.host.turns[turn], gone) ==
          (turn >= 3 && turn < *seen.from_turn));
  }
  // And the one that left played that same match for every turn it ran.
  const std::size_t shared = std::min<std::size_t>(t.b.turns.size(), *seen.from_turn);
  CHECK(history_hash(*t.b.match, shared) == history_hash(*t.host.match, shared));
  // What it sent before going and arrived after was not refused.
  CHECK(t.host.match->stats().refused == 0);
  CHECK(t.a.match->stats().refused == 0);
  CHECK(!t.host.match->ended().has_value());
  CHECK(!t.a.match->ended().has_value());
}

TEST(a_joiner_that_goes_silent_is_timed_out_and_dropped) {
  Three t;
  REQUIRE(t.start());
  for (Seat* seat : {&t.host, &t.a}) seat->match->set_silence_limit(400);
  REQUIRE(t.run(30, [&] { return t.b.turns.size() >= 8; }));
  const PlayerId gone = t.b.match->negotiator().local();
  t.b.stopped = true;  // no refusal: a crash
  REQUIRE(t.run(30, [&] { return t.host.turns.size() >= 30 && t.a.turns.size() >= 30; }, 10000));
  REQUIRE(t.host.match->departures().size() == 1);
  CHECK(t.host.match->departures()[0].player == gone);
  CHECK(t.host.match->departures()[0].reason == Refuse::Reason::silent);
  // The other joiner never heard from it directly: it learned from the host.
  REQUIRE(t.a.match->departures().size() == 1);
  CHECK(t.a.match->departures()[0].reason == Refuse::Reason::left);
  CHECK(t.a.match->departures()[0].from_turn == t.host.match->departures()[0].from_turn);
  CHECK(history_hash(*t.host.match, 30) == history_hash(*t.a.match, 30));
}

TEST(the_host_leaving_ends_the_match_for_every_joiner) {
  Three t;
  REQUIRE(t.start());
  REQUIRE(t.run(30, [&] { return t.host.turns.size() >= 6; }));
  t.host.match->leave();
  t.host.stopped = true;
  REQUIRE(t.run(30, [&] { return t.a.match->ended().has_value() && t.b.match->ended().has_value(); }));
  CHECK(*t.a.match->ended() == DepartureOutcome::State::coordinator_left);
  CHECK(*t.b.match->ended() == DepartureOutcome::State::coordinator_left);
}

TEST(a_start_names_the_host_as_the_coordinator) {
  Three t;
  REQUIRE(t.start());
  CHECK(t.a.match->negotiator().config().coordinator == 0);
  CHECK(t.b.match->negotiator().config().coordinator == 0);
  CHECK(t.host.match->negotiator().config().coordinator == 0);
}

// --------------------------------------------------------------------------
// chat
// --------------------------------------------------------------------------

TEST(the_lobby_chat_reaches_everyone_once_in_order) {
  Table t;
  REQUIRE(t.seat_both());
  t.host.say("welcome");
  t.a.say("hail");
  t.a.say("ready soon");
  t.b->say("ave");
  const auto texts = [](const std::vector<ChatLine>& log) {
    std::vector<std::string> out;
    for (const ChatLine& line : log) out.push_back(std::to_string(line.from) + ":" + line.text);
    return out;
  };
  REQUIRE(settle([&t](std::uint32_t now) { t.step_all(now); },
                 [&t] { return t.host.chat().size() == 4 && t.b->chat().size() == 4 &&
                               t.a.chat().size() == 4; }));
  // One log, the same everywhere; each speaker's lines in the order said.
  CHECK(texts(t.a.chat()) == texts(t.host.chat()));
  CHECK(texts(t.b->chat()) == texts(t.host.chat()));
  const std::vector<std::string> log = texts(t.host.chat());
  const auto at = [&log](const std::string& text) {
    return std::find(log.begin(), log.end(), text) - log.begin();
  };
  CHECK(at("0:welcome") < 4);
  CHECK(at("2:hail") < at("2:ready soon"));
  CHECK(at("5:ave") < 4);
  // And said once: the resends that carried them are not logged again.
  (void)settle([&t](std::uint32_t now) { t.step_all(now); }, [] { return false; });
  CHECK(t.host.chat().size() == 4);
}

TEST(a_joiner_that_says_more_than_a_choice_carries_is_heard_in_full_and_once) {
  Table t;
  REQUIRE(t.seat_both());
  for (int i = 0; i < 14; ++i) t.a.say("line " + std::to_string(i));
  REQUIRE(settle([&t](std::uint32_t now) { t.step_all(now); },
                 [&t] { return t.host.chat().size() == 14; }));
  for (std::size_t i = 0; i < 14; ++i) CHECK(t.host.chat()[i].text == "line " + std::to_string(i));
  // A choice resent with a line the host already logged logs nothing.
  Choice again;
  ChatLine old;
  old.seq = 3;
  old.text = "line 3";
  again.chat = {old};
  (void)t.a_socket.send(loopback(t.host_socket.port()), encode(again));
  (void)t.a_socket.send(loopback(t.host_socket.port()), encode(again));
  (void)settle([&t](std::uint32_t now) { t.step_all(now); }, [] { return false; });
  CHECK(t.host.chat().size() == 14);
}

TEST(chat_in_a_match_goes_through_the_host_to_every_peer) {
  Three t;
  REQUIRE(t.start());
  ChatLine line;
  line.text = "flank left";
  line.to = ChatLine::To::allies;
  const ChatLine sent = t.a.match->say(line);
  CHECK(sent.from == t.a.match->negotiator().local());
  std::vector<ChatLine> host_heard;
  std::vector<ChatLine> b_heard;
  REQUIRE(t.run(20, [&] {
    for (ChatLine& heard : t.host.match->take_chat()) host_heard.push_back(heard);
    for (ChatLine& heard : t.b.match->take_chat()) b_heard.push_back(heard);
    return !host_heard.empty() && !b_heard.empty();
  }));
  REQUIRE(b_heard.size() == 1);
  CHECK(b_heard[0].text == "flank left");
  CHECK(b_heard[0].from == sent.from);
  CHECK(b_heard[0].to == ChatLine::To::allies);
  CHECK(host_heard.size() == 1);
  CHECK(t.a.match->take_chat().empty());  // never one's own back
  CHECK(t.host.match->stats().refused == 0);
}

// --------------------------------------------------------------------------
// a late joiner
// --------------------------------------------------------------------------

namespace {

/// Three, one of whom (b) left after `after` turns and was dropped, the
/// computer taking the seat. Returns the seat.
PlayerId three_less_one(Three& t, std::uint32_t loss, std::size_t after = 8) {
  for (Seat* seat : {&t.host, &t.a, &t.b}) seat->match->set_drop(loss, 11 + seat->socket.port());
  if (!t.run(200, [&] { return t.b.turns.size() >= after; })) return kNoPlayer;
  const PlayerId gone = t.b.match->negotiator().local();
  t.b.match->leave();
  t.b.stopped = true;
  if (!t.run(200, [&] {
        return t.host.match->negotiator().dropped_from(gone).has_value() &&
               t.a.match->negotiator().dropped_from(gone).has_value();
      }, 10000)) {
    return kNoPlayer;
  }
  return gone;
}

/// Say hello to the running match until the host answers: with a header, or
/// a refusal.
JoinLobby::State knock(Three& t, Seat& late, std::optional<JoinLobby>& lobby,
                       std::uint64_t content = kGame, std::uint32_t ms = 5000) {
  lobby.emplace(late.socket, loopback(t.host.socket.port()), hello("Late", content));
  JoinLobby::State state = JoinLobby::State::waiting;
  (void)t.run(200, [&] {
    state = lobby->step(now_ms());
    return state == JoinLobby::State::started || state == JoinLobby::State::refused;
  }, ms);
  return state;
}

bool same(const WorldHashes& a, const WorldHashes& b) {
  return a.slots == b.slots && a.threads == b.threads && a.netcmds == b.netcmds &&
         a.extrahash == b.extrahash && a.pathfinder == b.pathfinder &&
         a.exploration == b.exploration && a.scriptstate == b.scriptstate &&
         a.aihash == b.aihash && a.hash_of_hashes == b.hash_of_hashes;
}

/// Every turn two seats both ran, with every channel equal.
std::size_t same_turns(const Seat& one, const Seat& two, std::size_t* differ) {
  std::size_t shared = 0;
  for (const auto& [turn, h] : two.hashes) {
    const auto it = one.hashes.find(turn);
    if (it == one.hashes.end()) continue;
    ++shared;
    if (!same(it->second, h)) ++*differ;
  }
  return shared;
}

}  // namespace

TEST(a_late_joiner_takes_the_departed_seat_and_hashes_like_everyone_from_its_first_turn) {
  Three t;
  REQUIRE(t.start());
  const PlayerId seat = three_less_one(t, 200);
  REQUIRE(seat != kNoPlayer);
  const std::uint32_t dropped = *t.host.match->negotiator().dropped_from(seat);
  Seat late;
  std::optional<JoinLobby> lobby;
  REQUIRE(knock(t, late, lobby) == JoinLobby::State::started);
  const Lobby& joined = lobby->result();
  REQUIRE(joined.join.has_value());
  CHECK(joined.start.you == seat);
  CHECK(joined.join->from_turn > dropped);
  late.first_turn = joined.join->from_turn;
  late.match.emplace(late.socket, joined);
  late.match->set_drop(200, 99);
  CHECK(late.match->awaiting_state());
  // Its first packet goes as soon as it knows where it starts.
  REQUIRE(late.match->submit({}, now_ms()));
  const std::uint32_t from = joined.join->from_turn;
  // It holds everything it needs for its first turn long before its world
  // has loaded -- and runs nothing until it has.
  REQUIRE(t.run(from + 2, [&] {
    late.match->pump(now_ms());
    return late.match->negotiator().ready();
  }, 10000));
  CHECK(late.match->awaiting_state());
  CHECK(!late.match->take().has_value());
  CHECK(late.match->negotiator().next_turn() == from);
  // Everyone plays on to thirty turns past the joiner's first.
  const std::uint32_t end = from + 30;
  REQUIRE(t.run(end, [&] {
    late.step(now_ms(), end);
    return t.host.reached() >= end && t.a.reached() >= end && late.reached() >= end;
  }, 20000));
  CHECK(t.host.states_sent == 1);
  CHECK(!late.match->awaiting_state());
  // The host counts the joiner seated when it hears the joiner holds the
  // whole state. That acknowledgement is one datagram, lost like any other
  // and answered again only at the next window resend, 40 ms on -- by which
  // time the joiner, which needs nothing more, can be thirty turns in. So
  // wait for the host to hear it, not for the turns.
  REQUIRE(t.run(end, [&] {
    late.step(now_ms(), end);
    return !t.host.match->seated().empty();
  }, 10000));
  REQUIRE(t.host.match->seated().size() == 1);
  CHECK(t.host.match->seated()[0].seat == seat);
  CHECK(t.host.match->seated()[0].name == "Late");
  CHECK(t.host.match->seated()[0].acked == t.host.match->seated()[0].size);
  CHECK(t.host.match->seated()[0].size > 0);
  // The same game from its first turn, every channel, on all three.
  std::size_t differ = 0;
  CHECK(same_turns(t.host, late, &differ) == 30);
  CHECK(same_turns(t.a, late, &differ) == 30);
  CHECK(same_turns(t.host, t.a, &differ) >= end);
  CHECK(differ == 0);
  CHECK(late.hashes.begin()->first == from);
  // Its first turn is where the computer stopped, on every peer, and its
  // orders are in the turns after its agreed-empty ones.
  CHECK(count_of(t.host.turns[from], seat, NetOrderKind::joined) == 1);
  CHECK(count_of(late.turns.front(), seat, NetOrderKind::joined) == 1);
  CHECK(has_order_from(t.a.turns[from + 3], seat));
  CHECK(!has_order_from(t.a.turns[from + 1], seat));
  // Nothing refused on an honest network, however lossy.
  CHECK(t.host.match->stats().refused == 0);
  CHECK(t.a.match->stats().refused == 0);
  CHECK(late.match->stats().refused == 0);
  CHECK(t.host.match->stats().dropped > 0);
  CHECK(late.match->stats().dropped > 0);
}

TEST(a_late_joiner_seated_during_a_speed_change_runs_at_the_speed_in_force) {
  Three t;
  REQUIRE(t.start());
  const PlayerId seat = three_less_one(t, 0);
  REQUIRE(seat != kNoPlayer);
  Seat late;
  std::optional<JoinLobby> lobby;
  lobby.emplace(late.socket, loopback(t.host.socket.port()), hello("Late"));
  // The host asks for a new speed with the first packet it sends after
  // seating the joiner: the last turn the save carries.
  t.host.speed_on_seating = 2000;
  REQUIRE(t.run(400, [&] { return lobby->step(now_ms()) == JoinLobby::State::started; }));
  const Lobby& joined = lobby->result();
  REQUIRE(joined.join.has_value());
  const std::uint32_t from = joined.join->from_turn;
  late.first_turn = from;
  late.match.emplace(late.socket, joined);
  REQUIRE(late.match->submit({}, now_ms()));
  const std::uint32_t end = from + 12;
  REQUIRE(t.run(end, [&] {
    late.step(now_ms(), end);
    return t.host.reached() >= end && late.reached() >= end;
  }, 20000));
  // The speed was agreed for the turn before the joiner's first, so the
  // joiner's first turn is the first at it -- on every peer.
  CHECK(joined.join->start.config.game_speed == 2000);
  CHECK(t.host.turns[from - 1].game_speed == 1000);
  CHECK(t.host.turns[from].game_speed == 2000);
  CHECK(late.turns.front().game_speed == 2000);
  CHECK(late.turns.front().length == t.host.turns[from].length);
  std::size_t differ = 0;
  CHECK(same_turns(t.host, late, &differ) == 12);
  CHECK(differ == 0);
}

TEST(a_running_match_with_no_open_seat_refuses_a_late_joiner) {
  Three t;
  REQUIRE(t.start());
  REQUIRE(t.run(20, [&] { return t.host.turns.size() >= 4; }));
  Seat late;
  std::optional<JoinLobby> lobby;
  CHECK(knock(t, late, lobby) == JoinLobby::State::refused);
  CHECK(lobby->result().error == "refused: the game is full");
  // Nor another game.
  const PlayerId seat = three_less_one(t, 0, 6);
  REQUIRE(seat != kNoPlayer);
  Seat other;
  std::optional<JoinLobby> other_lobby;
  CHECK(knock(t, other, other_lobby, kGame + 1) == JoinLobby::State::refused);
  CHECK(other_lobby->result().error == "refused: a different game: data.pak differs");
  // And a host that turned late joins off says it is full.
  t.host.match->set_late_join(false);
  Seat third;
  std::optional<JoinLobby> third_lobby;
  CHECK(knock(t, third, third_lobby) == JoinLobby::State::refused);
  CHECK(!t.host.match->seating().has_value());
}

TEST(a_late_joiner_that_never_arrives_is_dropped_and_the_seat_opens_again) {
  Three t;
  REQUIRE(t.start());
  for (Seat* seat : {&t.host, &t.a}) seat->match->set_silence_limit(600);
  const PlayerId seat = three_less_one(t, 0);
  REQUIRE(seat != kNoPlayer);
  // One joiner is being seated; a second says hello meanwhile and is not
  // answered until the first is done with.
  Seat first;
  std::optional<JoinLobby> first_lobby;
  REQUIRE(knock(t, first, first_lobby) == JoinLobby::State::started);
  // It takes the header and is never heard from again: never stepped.
  first.first_turn = first_lobby->result().join->from_turn;
  Seat second;
  std::optional<JoinLobby> second_lobby;
  second_lobby.emplace(second.socket, loopback(t.host.socket.port()), hello("Second"));
  // Its part is dropped like any silent peer's, and the computer takes the
  // seat back on the turn that departure agrees.
  REQUIRE(t.run(400, [&] {
    (void)second_lobby->step(now_ms());
    return t.host.match->departures().size() == 2 && t.host.match->departures()[1].from_turn &&
           t.a.match->departures().size() == 2 && t.a.match->departures()[1].from_turn;
  }, 15000));
  const NetDeparture& again = t.host.match->departures()[1];
  CHECK(again.player == seat);
  CHECK(again.since == first.first_turn);
  CHECK(again.reason == Refuse::Reason::silent);
  CHECK(t.a.match->departures()[1].from_turn == again.from_turn);
  // Then the second joiner is seated on it, and plays.
  REQUIRE(t.run(400, [&] { return second_lobby->step(now_ms()) == JoinLobby::State::started; }));
  const Lobby& joined = second_lobby->result();
  REQUIRE(joined.join.has_value());
  CHECK(joined.start.you == seat);
  CHECK(joined.join->from_turn > *again.from_turn);
  second.first_turn = joined.join->from_turn;
  second.match.emplace(second.socket, joined);
  REQUIRE(second.match->submit({}, now_ms()));
  const std::uint32_t end = second.first_turn + 10;
  REQUIRE(t.run(end, [&] {
    second.step(now_ms(), end);
    return t.host.reached() >= end && t.a.reached() >= end && second.reached() >= end;
  }, 20000));
  std::size_t differ = 0;
  CHECK(same_turns(t.host, second, &differ) == 10);
  CHECK(same_turns(t.a, second, &differ) == 10);
  CHECK(differ == 0);
  // The seat's history, as every peer holds it: played, dropped, joined and
  // dropped, joined.
  const std::vector<SeatStints> seats = t.a.match->negotiator().membership();
  for (const SeatStints& held : seats) {
    if (held.peer != seat) continue;
    REQUIRE(held.stints.size() == 3);
    CHECK(held.stints[1].from == first.first_turn);
    CHECK(held.stints[2].from == second.first_turn);
  }
  CHECK(seats == t.host.match->negotiator().membership());
}

int main(int argc, char**) {
  imperivm::test::run_all(argc > 1);
  std::printf("%d checks, %d failures\n", imperivm::test::checks, imperivm::test::failures);
  return imperivm::test::failures == 0 ? 0 : 1;
}
