#pragma once

// The app's half of a networked match: the lobby, the order queue, and the
// clock that runs agreed turns at their agreed length.
//
// Everything a peer *decides* is the core's: `sim/lockstep.hpp` says when a
// turn may run and how long it is, `sim/netlink.hpp` what goes on the wire,
// `sim/netcmds.hpp` what an order does. `engine/net` owns the socket. This
// owns the two things only the app can: what the player did since the last
// turn, and when -- in real time -- the next turn is due.
//
// **What a networked match changes in the app.** A right click, a command bar
// press and a surrender stop acting on the session and become orders that act
// on the turn every peer agrees on; the clock stops being a local timer; and
// the things one peer cannot do alone -- pause, step, save, load, the editor --
// are refused. Looking stays local: selecting, hovering and the bars' verifiers
// change no hash, which `imconform observe` measures.
//
// **A late joiner** (`sim/netjoin.hpp`, this engine's: the original admits
// nobody once a match has started). Joining a host whose match is running
// is answered with the host's save instead of a lobby: `receive_state` waits
// for it, the session is built as every peer's was and loads it, and the
// match goes on from the turn the host named. A host hands its save over
// when a joiner is seated, between two turns, and every peer says who is
// joining.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/sim/cmdbar.hpp"
#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netchat.hpp"
#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/netdepart.hpp"
#include "imperivm/core/sim/netlobby.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/net/match.hpp"
#include "imperivm/net/udp.hpp"

namespace imperivm::app {

struct NetPlayOptions {
  /// Host on this port; -1 for not hosting.
  int host_port = -1;
  /// `host:port` to join; empty for not joining.
  std::string join;
  /// The slots the joiners take, in the order they arrive. Empty means the
  /// slot after the host's.
  std::vector<core::PlayerId> seats;
  std::string name = "player";
  std::uint32_t delay = core::sim::kDefaultInputDelay;
  /// Datagrams in a thousand to lose on purpose. For testing the link layer
  /// against a real socket; a real match leaves it at zero.
  std::uint32_t drop = 0;
  std::uint32_t timeout_ms = 120000;
  /// Stop after this many turns and print the hashes; zero plays on. What
  /// makes two windows comparable by a script.
  std::uint32_t stop_after = 0;
  /// Ask for this speed, per mille, once this many turns have run: what a
  /// script can do that the options screen does by hand. Zero turns never.
  std::uint32_t speed_turn = 0;
  std::int32_t speed = 0;

  [[nodiscard]] bool enabled() const noexcept { return host_port >= 0 || !join.empty(); }
};

/// FNV-1a over a file's bytes, chained. The content hash a lobby compares:
/// `data.pak` and the map, the two inputs a different copy of the game would
/// most plausibly differ in.
inline std::uint64_t hash_file(const std::filesystem::path& path,
                               std::uint64_t state = 1469598103934665603ULL) {
  std::ifstream in(path, std::ios::binary);
  char buffer[1 << 16];
  while (in) {
    in.read(buffer, sizeof(buffer));
    const std::streamsize got = in.gcount();
    for (std::streamsize i = 0; i < got; ++i) {
      state ^= static_cast<std::uint8_t>(buffer[i]);
      state *= 1099511628211ULL;
    }
  }
  return state;
}

class NetPlay {
 public:
  explicit NetPlay(NetPlayOptions options) : options_(std::move(options)) {}
  /// A lobby the players' screen ran: its socket and what it settled.
  NetPlay(NetPlayOptions options, std::unique_ptr<net::UdpSocket> socket, net::Lobby lobby)
      : options_(std::move(options)), socket_(std::move(socket)), lobby_(std::move(lobby)) {}

  /// Whether the start carries the players' screen's rows. Without them --
  /// the command line's `--host` -- the map's own setup is played, every seat
  /// human.
  [[nodiscard]] bool has_rows() const noexcept { return !lobby_.start.rows.empty(); }

  [[nodiscard]] const NetPlayOptions& options() const noexcept { return options_; }
  [[nodiscard]] const core::sim::Start& start() const noexcept { return lobby_.start; }
  [[nodiscard]] core::PlayerId seat() const noexcept { return lobby_.start.you; }

  /// Host: wait for every seat, then send each joiner its start. Blocking,
  /// with the timeout; the shipped lobby screen steps a `HostLobby` instead.
  bool host(core::sim::Start base, std::uint64_t content, std::string* error) {
    // `base.map` and `base.map_hash` are the caller's: the start names the
    // map, and a joiner checks its own copy against the hash.
    if (!open(static_cast<std::uint16_t>(options_.host_port), error)) return false;
    std::vector<core::PlayerId> seats = options_.seats;
    if (seats.empty()) seats.push_back(static_cast<core::PlayerId>(base.you + 1));
    base.config.peers = seats;
    base.config.peers.push_back(base.you);
    base.config.input_delay = options_.delay;
    base.match = net::now_ms() ^ (static_cast<std::uint32_t>(socket_->port()) << 16);
    std::printf("net:          hosting on port %u as player %u, waiting for %zu\n",
                static_cast<unsigned>(socket_->port()), static_cast<unsigned>(base.you),
                seats.size());
    std::fflush(stdout);
    lobby_ = net::host_lobby(*socket_, base, seats, content, options_.timeout_ms);
    if (!lobby_.ok) {
      if (error != nullptr) *error = lobby_.error;
      return false;
    }
    for (const auto& [player, at] : lobby_.links) {
      std::printf("net:          player %u joined from %s\n", static_cast<unsigned>(player),
                  at.str().c_str());
    }
    return true;
  }

  /// Joiner: say hello until the host answers. Blocking, with the timeout.
  /// Then the map the start names must be here and hash the same, or this
  /// peer leaves -- telling the host, which would otherwise wait on it.
  bool join(std::uint64_t content, const std::filesystem::path& root, std::string* error) {
    const std::optional<net::Endpoint> host = net::resolve(options_.join, 0);
    if (!host.has_value() || host->port == 0) {
      if (error != nullptr) *error = "cannot resolve '" + options_.join + "' (host:port)";
      return false;
    }
    if (!open(0, error)) return false;
    core::sim::Hello hello;
    hello.content = content;
    hello.name = options_.name;
    std::printf("net:          joining %s\n", host->str().c_str());
    std::fflush(stdout);
    lobby_ = net::join_lobby(*socket_, *host, hello, options_.timeout_ms);
    if (!lobby_.ok) {
      if (error != nullptr) *error = lobby_.error;
      return false;
    }
    if (hash_file(root / lobby_.start.map) != lobby_.start.map_hash) {
      core::sim::Refuse refuse;
      refuse.reason = core::sim::Refuse::Reason::map;
      for (int copy = 0; copy < 2; ++copy) {
        for (const auto& [player, at] : lobby_.links) (void)socket_->send(at, core::sim::encode(refuse));
      }
      if (error != nullptr) *error = std::string(core::sim::describe(refuse.reason)) + ": " + lobby_.start.map;
      return false;
    }
    std::printf("net:          seated as player %u on %s, seed %u\n",
                static_cast<unsigned>(lobby_.start.you), lobby_.start.map.c_str(), lobby_.start.seed);
    return true;
  }

  /// This peer joined a match already running.
  [[nodiscard]] bool late() const noexcept { return lobby_.join.has_value(); }
  [[nodiscard]] std::uint32_t first_turn() const noexcept {
    return late() ? lobby_.join->from_turn : 0;
  }

  /// A late joiner: the map the host named must be here, byte for byte;
  /// then the match starts here -- its first packet goes out at once, so the
  /// others play on meanwhile -- and this waits, pumping, until the host's
  /// save has arrived and hashes as the header says. Blocking, with the
  /// timeout: a save is a few hundred kilobytes.
  bool receive_state(const std::filesystem::path& root, std::vector<std::byte>& out,
                     std::string* error) {
    if (!late()) return true;
    if (hash_file(root / lobby_.start.map) != lobby_.start.map_hash) {
      core::sim::Refuse refuse;
      refuse.reason = core::sim::Refuse::Reason::map;
      for (const auto& [player, at] : lobby_.links) (void)socket_->send(at, core::sim::encode(refuse));
      if (error != nullptr) *error = std::string(core::sim::describe(refuse.reason)) + ": " + lobby_.start.map;
      return false;
    }
    std::printf("net:          joining a match already running, as player %u from turn %u; "
                "the host's save is %u bytes\n",
                static_cast<unsigned>(seat()), first_turn(), lobby_.join->size);
    std::fflush(stdout);
    match_ = std::make_unique<net::NetMatch>(*socket_, lobby_);
    match_->set_drop(options_.drop, lobby_.join->match ^ (0x9e3779b9u * (seat() + 1u)));
    const std::uint32_t begun = net::now_ms();
    (void)match_->submit({}, begun);
    for (;;) {
      const std::uint32_t now = net::now_ms();
      match_->pump(now);
      if (const std::optional<std::span<const std::byte>> state = match_->state()) {
        out.assign(state->begin(), state->end());
        return true;
      }
      if (match_->ended().has_value()) {
        if (error != nullptr) *error = core::sim::describe(*match_->ended());
        return false;
      }
      if (now - begun > options_.timeout_ms) {
        if (error != nullptr) {
          *error = "the host's save did not arrive: " + std::to_string(match_->state_received()) +
                   " of " + std::to_string(lobby_.join->size) + " bytes";
        }
        return false;
      }
      socket_->wait(5);
    }
  }

  /// The match options every peer builds identically: networked, every seat
  /// human, the match's own `human` the lowest seat because it is hashed.
  [[nodiscard]] core::sim::MatchOptions match_options() const {
    core::sim::MatchOptions options;
    options.multiplayer = true;
    for (const core::PlayerId seat : lobby_.start.config.peers) {
      if (seat >= core::sim::kPlayerCount) continue;
      options.control_set[seat] = true;
      options.controls[seat] = core::sim::PlayerControl::human;
      if (options.human == core::kNoPlayer || seat < options.human) options.human = seat;
    }
    return options;
  }

  /// The session is built and started -- a late joiner's, loaded from the
  /// host's save: begin the match. `identity` is the map identity a save of
  /// this session names, for a late joiner's.
  void begin(core::sim::GameSession& session, core::sim::CommandBar& bar, std::string identity) {
    identity_ = std::move(identity);
    sink_ = std::make_unique<core::sim::CommandBarSink>(session, bar);
    const std::uint32_t now = net::now_ms();
    next_due_ = now;
    computers_ = core::sim::ComputerSeats(seat());
    if (late()) {
      // Already running since `receive_state`: its first packet is out.
      match_->state_loaded();
      turns_ = first_turn();
      // The seats the computer already holds are marked from the start,
      // though this peer saw none of them go -- so no sound for them.
      const std::vector<core::sim::SeatStints> membership = match_->negotiator().membership();
      computers_ = core::sim::ComputerSeats::resumed(membership, first_turn(), seat());
      return;
    }
    match_ = std::make_unique<net::NetMatch>(*socket_, lobby_);
    match_->set_drop(options_.drop, lobby_.start.match ^ (0x9e3779b9u * (seat() + 1u)));
    (void)match_->submit({}, now);
  }

  /// Something the player did, to happen on a turn every peer agrees on.
  void queue(core::sim::NetOrder order) {
    order.issuer = seat();
    pending_.push_back(std::move(order));
  }

  /// Say `line` to the match; see `NetMatch::say`. Returns it stamped.
  core::sim::ChatLine say(core::sim::ChatLine line) {
    return match_ == nullptr ? line : match_->say(std::move(line));
  }
  /// Lines other peers said since the last call.
  [[nodiscard]] std::vector<core::sim::ChatLine> take_chat() {
    return match_ == nullptr ? std::vector<core::sim::ChatLine>{} : match_->take_chat();
  }
  /// A seat's name as the players' screen had it, or `Player N`.
  [[nodiscard]] std::string name_of(core::PlayerId slot) const {
    for (const core::sim::SeatRow& row : lobby_.start.rows) {
      if (row.slot == slot && !row.name.empty()) return row.name;
    }
    return "Player " + std::to_string(static_cast<unsigned>(slot) + 1);
  }
  /// The translation of `(AI)` the screen appends to a seat the computer
  /// took; see `sim/netdepart.hpp`. The app sets it from the language pack.
  void set_computer_marker(std::string marker) { marker_ = std::move(marker); }
  /// Whether the computer holds `slot` for a player who left.
  [[nodiscard]] bool computer_holds(core::PlayerId slot) const noexcept {
    return computers_.marked(slot);
  }
  /// `name`, marked while the computer holds `slot`: what a screen shows.
  [[nodiscard]] std::string display_name(core::PlayerId slot, std::string_view name) const {
    return computers_.display(slot, name, marker_);
  }
  [[nodiscard]] std::string display_name(core::PlayerId slot) const {
    return display_name(slot, name_of(slot));
  }
  /// Seats that changed hands on the turns run since the last call, a turn's
  /// changes together: one `taken` list is one dropped-player sound.
  [[nodiscard]] std::vector<core::sim::ComputerSeats::Change> take_seat_news() {
    std::vector<core::sim::ComputerSeats::Change> out = std::move(seat_news_);
    seat_news_.clear();
    return out;
  }

  /// The options screen's game speed, as a `set_speed` order: the position
  /// converted by `sim::game_speed_from_option` (`sim/tick.hpp`), the one
  /// place the position and the per-mille speed meet, so the shipped
  /// `GameSpeed=13` is 999.
  void ask_speed_option(int option) { ask_speed(core::sim::game_speed_from_option(option)); }
  void ask_speed(std::int32_t per_mille) {
    core::sim::NetOrder order;
    order.kind = core::sim::NetOrderKind::set_speed;
    order.speed = per_mille;
    queue(std::move(order));
  }

  /// Run every turn that is agreed and due. Returns how many ran.
  std::size_t advance(core::sim::GameSession& session) {
    if (match_ == nullptr) return 0;
    const std::uint32_t now = net::now_ms();
    match_->pump(now);
    announce_departures();
    announce_joins();
    hand_over_state(session, now);
    // The match cannot go on here -- the host left, or the others dropped
    // this peer -- and the clock stops rather than stalling without a word.
    // A joiner leaving is not this: the rest agree where its turns end and
    // play on (`sim/netdepart.hpp`).
    //
    // Except the turns already agreed: a host that has played to its last
    // turn and gone leaves a peer a turn or two behind it, by the clock,
    // holding every packet for them. Those turns are the match's whatever
    // happens next, so they run; nothing past them can.
    if (match_->ended().has_value() &&
        (*match_->ended() != core::sim::DepartureOutcome::State::coordinator_left ||
         !match_->negotiator().ready())) {
      return 0;
    }
    std::size_t ran = 0;
    // A bounded catch-up, as the single-player clock has: after a stall, run
    // what arrived, but not a minute of game in one frame.
    while (ran < 8 && !finished_turns()) {
      if (static_cast<std::int32_t>(now - next_due_) < 0) break;
      std::optional<core::sim::AgreedTurn> turn = match_->take();
      if (!turn.has_value()) {
        ++stalled_frames_;
        break;
      }
      core::sim::ScriptOrderVerifier verifier(session.scheduler(), session.host_context());
      const core::sim::NetTurnReport report =
          core::sim::apply_turn(session.world(), turn->orders, &verifier, sink_.get());
      // The takeovers and hand-backs this turn applied, as the screen shows
      // them: presentation, beside the world and never in it.
      if (core::sim::ComputerSeats::Change change = computers_.apply(turn->orders); !change.empty()) {
        for (const core::PlayerId gone : change.taken) {
          std::printf("net       player %u is the computer's from turn %u, shown as \"%s\"\n",
                      static_cast<unsigned>(gone), turn->index, display_name(gone).c_str());
        }
        for (const core::PlayerId back : change.handed_back) {
          std::printf("net       player %u is played again from turn %u, shown as \"%s\"\n",
                      static_cast<unsigned>(back), turn->index, display_name(back).c_str());
        }
        std::fflush(stdout);
        seat_news_.push_back(std::move(change));
      }
      // What 0x004e67c0 prints when more than one seat is networked: who
      // changed the speed, and to what, as a percentage.
      for (const core::sim::NetOrder& order : turn->orders.orders) {
        if (order.kind == core::sim::NetOrderKind::diplomacy) {
          // The Diplomacy screen's OK, as the table now holds it.
          const core::sim::PlayerTable& players = session.world().players();
          if (order.other == core::kNoPlayer) {
            std::printf("net       player %u's allied victory is %s from turn %u\n",
                        static_cast<unsigned>(order.issuer),
                        players.setup(order.issuer).allied_flag ? "on" : "off", turn->index);
          } else {
            std::printf("net       player %u's word for player %u is 0x%02x from turn %u\n",
                        static_cast<unsigned>(order.issuer), static_cast<unsigned>(order.other),
                        players.relation_word(order.issuer, order.other), turn->index);
          }
          std::fflush(stdout);
          continue;
        }
        if (order.kind != core::sim::NetOrderKind::set_speed) continue;
        std::printf("net       player %u set the speed to %d%% on turn %u\n",
                    static_cast<unsigned>(order.issuer),
                    core::sim::clamp_game_speed(order.speed) / 10, turn->index);
        std::fflush(stdout);
      }
      if (report.applied + report.commands + report.unapplied > 0) {
        std::printf("net:          turn %u: %zu order(s), %zu command(s) queued, %zu row(s)\n",
                    turn->index, report.applied, report.issued, report.commands);
      }
      session.advance(1, turn->length);
      ++turns_;
      fold(session.world().hashes().hash_of_hashes);
      {
        core::sim::WorldHashes h = session.world().hashes();
        h.netcmds = core::sim::stream_hash(match_->negotiator().history(), turn->index + 1);
        trace_.push_back(core::sim::conformance::roll_up(h));
      }
      // Due one agreed length after the last was due -- or, after a stall of
      // more than one, one length from now: a late turn does not buy a burst.
      const std::uint32_t base =
          static_cast<std::int32_t>(now - next_due_) > turn->real_ms ? now - turn->real_ms
                                                                     : next_due_;
      next_due_ = base + static_cast<std::uint32_t>(turn->real_ms);
      if (options_.speed_turn != 0 && turns_ == options_.speed_turn) ask_speed(options_.speed);
      if (!finished_turns()) {
        // The clock read now: `submit` pumps, and what it hears is stamped
        // with this time. The frame's first reading, eight turns ago on a
        // slow machine, would make the next frame's pump count the other
        // peers silent for the time those turns took.
        (void)match_->submit(std::move(pending_), net::now_ms());
        pending_.clear();
      }
      ++ran;
      // A late joiner seated meanwhile: the save is taken here, after the
      // turn before its first, and before that turn runs.
      hand_over_state(session, now);
    }
    return ran;
  }

  /// With `stop_after`: every turn ran, and everything this peer holds has
  /// been acknowledged -- a hub that left first would strand the others --
  /// or it has waited long enough. Prints the hashes the first time.
  [[nodiscard]] bool done(const core::sim::GameSession& session) {
    // A scripted run whose match ended ends here, saying so: nobody is there
    // to press the message box's OK.
    if (options_.stop_after != 0 && ended().has_value() && !finished_turns() &&
        !match_->negotiator().ready()) {
      if (!finished_at_.has_value()) {
        finished_at_ = net::now_ms();
        std::printf("net       the match ended (%s) after %u turns\n",
                    core::sim::describe(*ended()), turns_);
        std::fflush(stdout);
      }
      return true;
    }
    if (!finished_turns()) return false;
    const std::uint32_t now = net::now_ms();
    if (match_ != nullptr) match_->pump(now);
    if (!finished_at_.has_value()) {
      finished_at_ = now;
      const net::MatchStats& stats = match_->stats();
      std::printf("wire      %zu sent, %zu dropped, %zu received, %zu duplicates, %zu refused\n",
                  stats.sent, stats.dropped, stats.received, stats.duplicates, stats.refused);
      std::printf("hashes    world %016llx netcmds %016llx\n",
                  static_cast<unsigned long long>(world_),
                  static_cast<unsigned long long>(core::sim::stream_hash(
                      match_->negotiator().history(), match_->negotiator().history().size())));
      std::printf("net       %u turns as player %u\n", turns_, static_cast<unsigned>(seat()));
      // Every turn's rolled-up hash, from the first this peer ran: what a
      // late joiner is compared with the others by.
      std::printf("trace     from %u:", first_turn());
      for (const std::uint64_t h : trace_) std::printf(" %016llx", static_cast<unsigned long long>(h));
      std::printf("\n");
      std::fflush(stdout);
      (void)session;
    }
    const std::uint32_t lingered = now - *finished_at_;
    return (match_->node().held() == 0 && lingered >= 250) || lingered >= 3000;
  }

  /// Why the match cannot go on here, once it cannot; see `NetMatch::ended`.
  [[nodiscard]] std::optional<core::sim::DepartureOutcome::State> ended() const noexcept {
    return match_ == nullptr ? std::nullopt : match_->ended();
  }
  /// Departures the match has agreed and the screen has not yet been told
  /// of, oldest first. Each is returned once.
  [[nodiscard]] std::vector<net::NetDeparture> take_departures() {
    std::vector<net::NetDeparture> out = std::move(unshown_);
    unshown_.clear();
    return out;
  }
  /// Late joins agreed and not yet shown, as lines for the screen.
  [[nodiscard]] std::vector<std::string> take_join_news() {
    std::vector<std::string> out = std::move(join_news_);
    join_news_.clear();
    return out;
  }
  /// Tell everyone this peer is going. Once; the destructor does it too.
  void leave() {
    if (match_ != nullptr && !left_) match_->leave();
    left_ = true;
  }
  ~NetPlay() { leave(); }
  NetPlay(const NetPlay&) = delete;
  NetPlay& operator=(const NetPlay&) = delete;

  [[nodiscard]] std::uint32_t turns() const noexcept { return turns_; }
  [[nodiscard]] std::size_t queued() const noexcept { return pending_.size(); }
  [[nodiscard]] std::uint32_t next_turn() const noexcept {
    return match_ == nullptr ? 0 : match_->negotiator().next_turn();
  }

 private:
  bool open(std::uint16_t port, std::string* error) {
    std::string why;
    std::optional<net::UdpSocket> socket = net::UdpSocket::open(port, false, &why);
    if (!socket.has_value()) {
      if (error != nullptr) *error = why;
      return false;
    }
    socket_ = std::make_unique<net::UdpSocket>(std::move(*socket));
    return true;
  }

  [[nodiscard]] bool finished_turns() const noexcept {
    return options_.stop_after != 0 && turns_ >= options_.stop_after;
  }

  /// Say each late join once, as every peer learns it: who, and from which
  /// turn. What the others see while the save travels is this line, and --
  /// only if the save outlasts the turns already agreed -- the match waiting.
  void announce_joins() {
    for (const core::sim::Admit& admit : match_->node().admits()) {
      if (admit.peer == seat() && late()) continue;
      if (!joins_said_.insert({admit.peer, admit.from_turn}).second) continue;
      std::printf("net       player %u is joining from turn %u\n", static_cast<unsigned>(admit.peer),
                  admit.from_turn);
      std::fflush(stdout);
      join_news_.push_back(name_of(admit.peer) + " is joining the game.");
    }
    for (; seated_said_ < match_->seated().size(); ++seated_said_) {
      const net::NetMatch::Seating& done = match_->seated()[seated_said_];
      std::printf("net       player %u (%s) has the save, %u bytes\n",
                  static_cast<unsigned>(done.seat), done.name.c_str(), done.acked);
      std::fflush(stdout);
    }
  }

  /// The host: a seated late joiner's save, once the turn before its first
  /// has run.
  void hand_over_state(core::sim::GameSession& session, std::uint32_t now) {
    if (!match_->wants_state().has_value()) return;
    auto bytes = session.save(identity_);
    if (!bytes.ok()) {
      std::printf("net       the session would not save for a late joiner\n");
      return;
    }
    std::printf("net       the save for player %u, %zu bytes, after turn %u\n",
                static_cast<unsigned>(match_->seating()->seat), bytes.value().size(),
                *match_->wants_state() - 1);
    std::fflush(stdout);
    match_->provide_state(std::move(bytes.value()), now);
  }

  /// Say each agreed departure once, on the console and to the screen.
  void announce_departures() {
    for (const net::NetDeparture& gone : match_->departures()) {
      if (!gone.from_turn.has_value()) continue;
      // A seat a late joiner took can be left again: another departure.
      if (!announced_.insert({gone.player, gone.since}).second) continue;
      unshown_.push_back(gone);
      std::printf("net       player %u left (%s); dropped from turn %u\n",
                  static_cast<unsigned>(gone.player), core::sim::describe(gone.reason),
                  *gone.from_turn);
      std::fflush(stdout);
    }
  }

  void fold(std::uint64_t value) noexcept {
    for (int shift = 0; shift < 64; shift += 8) {
      world_ ^= (value >> shift) & 0xFF;
      world_ *= 1099511628211ULL;
    }
  }

  NetPlayOptions options_;
  std::unique_ptr<net::UdpSocket> socket_;
  net::Lobby lobby_;
  std::unique_ptr<net::NetMatch> match_;
  std::unique_ptr<core::sim::CommandBarSink> sink_;
  std::vector<core::sim::NetOrder> pending_;
  std::uint32_t next_due_ = 0;
  std::uint32_t turns_ = 0;
  std::size_t stalled_frames_ = 0;
  std::uint64_t world_ = 1469598103934665603ULL;
  std::optional<std::uint32_t> finished_at_;
  bool left_ = false;
  std::set<std::pair<core::PlayerId, std::uint32_t>> announced_;
  std::vector<net::NetDeparture> unshown_;
  std::string identity_;
  std::vector<std::uint64_t> trace_;
  std::set<std::pair<core::PlayerId, std::uint32_t>> joins_said_;
  std::size_t seated_said_ = 0;
  std::vector<std::string> join_news_;
  core::sim::ComputerSeats computers_;
  /// The key itself until the app says otherwise, as a table's miss is.
  std::string marker_ = "(AI)";
  std::vector<core::sim::ComputerSeats::Change> seat_news_;
};

}  // namespace imperivm::app
