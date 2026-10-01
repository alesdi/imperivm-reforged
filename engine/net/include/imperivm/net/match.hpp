#pragma once

// A networked match: the lobby exchanges, and the pump that moves datagrams
// between a socket and the core's link layer and negotiator.
//
// Everything this decides is decided by the core -- which packets to send,
// what a datagram means, when a turn may run, how long it is, what a lobby
// message says. This file owns only *when*: how often to send, how long to
// wait for a joiner, when a finished peer may leave. Those are real-time
// questions, and the core has no clock.

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netdepart.hpp"
#include "imperivm/core/sim/netjoin.hpp"
#include "imperivm/core/sim/netlink.hpp"
#include "imperivm/core/sim/netlobby.hpp"
#include "imperivm/net/udp.hpp"

namespace imperivm::net {

using core::PlayerId;

/// How often a node sends each link a datagram when it has nothing new: the
/// resend that repairs a loss, and the carrier of the acknowledgements and the
/// clock. Something new is sent at once.
inline constexpr std::uint32_t kSendIntervalMs = 15;
/// How often lobby messages are resent: a joiner's hello and choice, the
/// host's roster and a start not yet acknowledged.
inline constexpr std::uint32_t kLobbyResendMs = 100;
inline constexpr std::uint32_t kRosterIntervalMs = 250;
/// How long a link may say nothing before it counts as gone. Every link hears
/// a datagram every `kSendIntervalMs`, so this is some thousand of them lost in
/// a row -- or a peer that crashed, or is still loading a map several seconds
/// slower than this one. **This engine's number.**
inline constexpr std::uint32_t kSilenceLimitMs = 15000;
/// How often the host resends a late joiner's header, or the part of the save
/// it has not acknowledged, and how much of it is in flight at once. **This
/// engine's numbers**: a 369 KB save is some 360 chunks, a few round trips.
inline constexpr std::uint32_t kJoinResendMs = 40;
inline constexpr std::size_t kJoinWindow = 64;

struct Lobby {
  bool ok = false;
  std::string error;
  core::sim::Start start;                ///< this peer's
  std::map<PlayerId, Endpoint> links;    ///< every link, by player slot
  /// Host only: the `Start` each joiner was sent, to resend until heard.
  std::map<PlayerId, std::vector<std::byte>> pending;
  /// Host only: the `data.pak` hash a late joiner's hello must carry.
  std::uint64_t content = 0;
  /// A late joiner's only: the match was already running, and this is what
  /// the host said about it (`sim/netjoin.hpp`). `start` is its `start`.
  std::optional<core::sim::JoinHeader> join;
};

/// The host's side of the lobby, one step at a time, so that the players'
/// screen can show who has joined while it waits.
///
/// Seats go to joiners in the order they say hello. With `base.rows` empty --
/// the command line's form -- the seats are `seats`, fixed; otherwise they are
/// whichever rows the host has marked open, and the host may edit the rows
/// between steps. Refuses another build, another `data.pak`, and anybody once
/// every seat is taken; answers LAN queries while it is open.
class HostLobby {
 public:
  HostLobby(UdpSocket& socket, core::sim::Start base, std::vector<PlayerId> seats,
            std::uint64_t content, std::string host_name = "host");

  /// Read what has arrived and send the roster when it is due. Returns
  /// whether the lobby could start now: every seat of the command line's form
  /// taken, or, with rows, at least one joiner seated and every one ready.
  bool step(std::uint32_t now);

  /// The rows as they stand, joiners' names and choices included. The host
  /// edits its own row, the computers and which seats are open; a seated
  /// joiner's row is the joiner's.
  [[nodiscard]] std::vector<core::sim::SeatRow>& rows() noexcept { return base_.rows; }
  [[nodiscard]] core::sim::LobbyRules& rules() noexcept { return base_.rules; }
  [[nodiscard]] const std::map<PlayerId, Endpoint>& seated() const noexcept { return seated_; }
  [[nodiscard]] const std::map<PlayerId, std::string>& names() const noexcept { return names_; }
  [[nodiscard]] std::size_t seats() const noexcept;

  /// Send every joiner its `Start` and hand over the match. Open seats are
  /// closed first.
  [[nodiscard]] Lobby finish(std::uint32_t seed);

  /// Tell every seated joiner the game is closed.
  void close();

  /// The host says `text` in the lobby's chat.
  void say(std::string text);
  /// The lobby's log, everyone's lines in the order the host logged them.
  [[nodiscard]] const std::vector<core::sim::ChatLine>& chat() const noexcept { return chat_; }

 private:
  [[nodiscard]] std::vector<PlayerId> open_seats() const;
  void send_rosters();
  void unseat(PlayerId slot);

  UdpSocket* socket_;
  core::sim::Start base_;
  std::vector<PlayerId> fixed_seats_;
  std::uint64_t content_;
  std::string host_name_;
  std::map<PlayerId, Endpoint> seated_;
  std::map<PlayerId, std::string> names_;
  std::uint32_t last_roster_ = 0;
  bool roster_due_ = true;
  std::vector<core::sim::ChatLine> chat_;
  std::map<PlayerId, std::uint32_t> heard_;  ///< per joiner: its lines logged
  std::uint32_t said_ = 0;                   ///< the host's own lines
};

/// The joiner's side, one step at a time: say hello until seated, then hold
/// the roster the host streams and send it this joiner's choice.
///
/// A host whose match is already running answers the hello with a late
/// join's header instead (`sim/netjoin.hpp`); `step` then says `started`, and
/// `result().join` holds it.
class JoinLobby {
 public:
  JoinLobby(UdpSocket& socket, Endpoint host, core::sim::Hello hello);
  enum class State : std::uint8_t { waiting, seated, started, refused };
  State step(std::uint32_t now);

  /// Once seated: the rows as the host last sent them. `roster().you` is this
  /// joiner's row.
  [[nodiscard]] const core::sim::Start& roster() const noexcept { return roster_; }
  void set_choice(const core::sim::Choice& choice) noexcept {
    choice_ = choice;
    choice_due_ = true;
  }
  [[nodiscard]] const core::sim::Choice& choice() const noexcept { return choice_; }
  /// Leave, telling the host why.
  void leave(core::sim::Refuse::Reason reason);
  /// Say `text` in the lobby's chat: sent with every choice until the host's
  /// roster shows it heard.
  void say(std::string text);
  /// The lobby's log as the host last sent it.
  [[nodiscard]] const std::vector<core::sim::ChatLine>& chat() const noexcept {
    return roster_.chat;
  }
  /// Valid once `step` has said `started` (the match) or `refused` (`error`).
  [[nodiscard]] const Lobby& result() const noexcept { return result_; }

 private:
  UdpSocket* socket_;
  Endpoint host_;
  std::vector<std::byte> hello_;
  std::uint32_t last_ = 0;
  bool sent_ = false;
  bool seated_ = false;
  core::sim::Start roster_;
  core::sim::Choice choice_;
  bool choice_due_ = true;
  Lobby result_;
  std::vector<core::sim::ChatLine> unheard_;  ///< said, not yet in a roster's count
  std::uint32_t said_ = 0;
};

/// The LAN's open games, for `MPMENU.INI`'s list.
class LanBrowser {
 public:
  struct Found {
    Endpoint at;
    core::sim::Advert advert;
    std::uint32_t seen = 0;
  };
  explicit LanBrowser(UdpSocket& socket, std::uint16_t port = core::sim::kDefaultNetPort)
      : socket_(&socket), port_(port) {}
  /// Broadcast a query on the LAN, and to this machine's own loopback -- the
  /// one address a broadcast does not reliably reach.
  void refresh(std::uint32_t now);
  /// Collect the answers. Games not heard from for five seconds are dropped.
  void step(std::uint32_t now);
  [[nodiscard]] const std::vector<Found>& games() const noexcept { return games_; }

 private:
  UdpSocket* socket_;
  std::uint16_t port_;
  std::vector<Found> games_;
};

/// Host: seat one joiner per entry of `seats`, in the order they say hello,
/// and give each a `Start` built from `base` -- whose `you` is the host's own
/// slot and whose `links` the host fills. Blocking, with a timeout: the
/// command line's form of `HostLobby`.
[[nodiscard]] Lobby host_lobby(UdpSocket& socket, const core::sim::Start& base,
                               std::vector<PlayerId> seats, std::uint64_t content,
                               std::uint32_t timeout_ms);

/// Joiner: say hello to `host` until it answers with a `Start` or a `Refuse`.
/// Blocking, and always ready: the command line's form of `JoinLobby`.
[[nodiscard]] Lobby join_lobby(UdpSocket& socket, const Endpoint& host,
                               const core::sim::Hello& hello, std::uint32_t timeout_ms);

struct MatchStats {
  std::size_t sent = 0;
  std::size_t dropped = 0;       ///< lost on purpose, by `set_drop`
  std::size_t received = 0;
  std::size_t duplicates = 0;
  std::size_t refused = 0;       ///< anything the link layer or negotiator refused
  std::size_t strangers = 0;     ///< datagrams from an endpoint that is no link
  std::size_t late = 0;          ///< a departed peer's, past its end: not refused
};

/// A peer that left, as this one knows it.
struct NetDeparture {
  PlayerId player = 0;
  /// The first turn of the part it left: zero, or where it had joined late.
  std::uint32_t since = 0;
  /// What this peer saw: the departed peer's own refusal, or `silent`. One
  /// learned from another peer's report says `left`.
  core::sim::Refuse::Reason reason = core::sim::Refuse::Reason::left;
  /// The turn its part ended at, once the match agreed it.
  std::optional<std::uint32_t> from_turn;
};

class NetMatch {
 public:
  NetMatch(UdpSocket& socket, const Lobby& lobby);

  /// Lose this share of outgoing datagrams, drawn from `seed`. For proving
  /// the link layer against a real socket; a match leaves it at zero.
  void set_drop(std::uint32_t per_mille, std::uint32_t seed) noexcept {
    drop_per_mille_ = per_mille;
    draw_ = seed | 1u;
  }

  /// Receive everything waiting, and send every link that is due.
  void pump(std::uint32_t now);

  /// This peer's orders for the next turn it schedules, proposing what the
  /// link layer measured. False if this turn's packet was already submitted.
  bool submit(std::vector<core::sim::NetOrder> orders, std::uint32_t now);

  /// The next agreed turn. None while a late joiner's state is owed -- the
  /// host's, before it has handed it over; the joiner's, before it has loaded
  /// it -- because the turn needs it.
  [[nodiscard]] std::optional<core::sim::AgreedTurn> take();

  // -- a late joiner, the host's side (`sim/netjoin.hpp`) ------------------

  /// Seat a late joiner that says hello to a running match. On by default;
  /// off refuses every one as `full`.
  void set_late_join(bool allowed) noexcept { late_join_ = allowed; }
  /// The turn a seated late joiner's state is due before: once this peer
  /// has applied the turn before it, the caller takes the save and hands it
  /// to `provide_state`. None otherwise.
  [[nodiscard]] std::optional<std::uint32_t> wants_state() const noexcept;
  /// The save of the session as it stands, after the turn before the
  /// joiner's first. Sent until acknowledged.
  void provide_state(std::vector<std::byte> save, std::uint32_t now);
  /// The joiner being seated or sent its state, if any: one at a time.
  struct Seating {
    PlayerId seat = 0;
    std::string name;
    std::uint32_t from_turn = 0;
    std::uint32_t size = 0;      ///< once the state is provided
    std::uint32_t acked = 0;     ///< bytes the joiner holds
    bool header_heard = false;
  };
  [[nodiscard]] const std::optional<Seating>& seating() const noexcept { return seating_; }
  /// Late joiners whose state went out and was acknowledged in full, oldest
  /// first.
  [[nodiscard]] const std::vector<Seating>& seated() const noexcept { return seated_; }

  // -- a late joiner, its own side -----------------------------------------

  /// This peer joined late and has not loaded its state yet.
  [[nodiscard]] bool awaiting_state() const noexcept { return transfer_.has_value() && !loaded_; }
  /// The state, once every byte is in and hashes as the header says.
  [[nodiscard]] std::optional<std::span<const std::byte>> state() const noexcept;
  /// Bytes of the state held so far, and in all.
  [[nodiscard]] std::uint32_t state_received() const noexcept {
    return transfer_.has_value() ? transfer_->received() : 0;
  }
  /// The caller has built its session from `state()`: turns may run.
  void state_loaded() noexcept { loaded_ = true; }

  /// Tell every link this peer is leaving.
  void leave();

  /// This peer's player says `line`; see `LinkNode::say`. Returns it stamped.
  core::sim::ChatLine say(core::sim::ChatLine line);
  /// Lines received since the last call, each speaker's in order. Never
  /// this peer's own: the screen shows those as it sends them.
  [[nodiscard]] std::vector<core::sim::ChatLine> take_chat() {
    std::vector<core::sim::ChatLine> out = std::move(heard_chat_);
    heard_chat_.clear();
    return out;
  }

  /// Every peer that has left, in the order this one learned of it. The rest
  /// agree on where each one's turns end (`sim/netdepart.hpp`) and play on
  /// without it; `from_turn` is set once they have.
  [[nodiscard]] const std::vector<NetDeparture>& departures() const noexcept {
    return departures_;
  }
  /// Why the match cannot go on here: the coordinator left, or the others
  /// dropped this peer. Empty while it can.
  [[nodiscard]] std::optional<core::sim::DepartureOutcome::State> ended() const noexcept {
    return ended_;
  }
  /// How long a link may be silent before it counts as gone; see
  /// `kSilenceLimitMs`. Zero never times out.
  void set_silence_limit(std::uint32_t ms) noexcept { silence_limit_ms_ = ms; }

  [[nodiscard]] const core::sim::TurnNegotiator& negotiator() const noexcept { return negotiator_; }
  [[nodiscard]] const core::sim::LinkNode& node() const noexcept { return node_; }
  [[nodiscard]] const MatchStats& stats() const noexcept { return stats_; }

 private:
  void send_all(std::uint32_t now);
  [[nodiscard]] bool lose() noexcept;
  /// A hello from an endpoint that is no link: a late joiner.
  void hello_from(const Endpoint& from, std::span<const std::byte> bytes);
  void send_state(std::uint32_t now);
  void send(const Endpoint& to, const std::vector<std::byte>& bytes);
  /// `player` has gone, as this peer saw it.
  void depart(PlayerId player, core::sim::Refuse::Reason reason);
  void settle();
  NetDeparture& departure(PlayerId player, core::sim::Refuse::Reason reason);

  UdpSocket* socket_;
  core::sim::TurnNegotiator negotiator_;
  core::sim::LinkNode node_;
  std::map<PlayerId, Endpoint> links_;
  std::map<PlayerId, std::vector<std::byte>> pending_;
  std::uint32_t last_sent_ = 0;
  std::uint32_t last_start_ = 0;
  bool dirty_ = true;
  std::uint32_t drop_per_mille_ = 0;
  std::uint32_t draw_ = 1;
  MatchStats stats_;
  std::map<PlayerId, std::uint32_t> last_heard_;
  bool clock_started_ = false;
  std::uint32_t silence_limit_ms_ = kSilenceLimitMs;
  std::vector<NetDeparture> departures_;
  std::optional<core::sim::DepartureOutcome::State> ended_;
  std::vector<core::sim::ChatLine> heard_chat_;
  // A late joiner, the host's side.
  core::sim::Start start_;
  std::uint64_t content_ = 0;
  bool late_join_ = true;
  std::optional<Seating> seating_;
  Endpoint seating_at_;
  std::vector<std::byte> header_;
  std::vector<std::byte> state_;
  std::uint32_t last_state_ = 0;
  std::vector<Seating> seated_;
  // A late joiner, its own.
  std::optional<core::sim::JoinTransfer> transfer_;
  bool loaded_ = false;
  bool ack_due_ = false;
};

}  // namespace imperivm::net
