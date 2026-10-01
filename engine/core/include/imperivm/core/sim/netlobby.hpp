#pragma once

/// The lobby: how peers that have never met agree to play one match.
///
/// Each message is a datagram of its own, and nothing here but their codecs --
/// the exchange needs a clock and a socket, so it lives in `engine/net`. The
/// whole of what they settle is what `check_netplay` hands its peers for free:
/// **the same map, the same data, the same seed, the same setup and lockstep
/// configuration, and a player slot each.** Everything else follows, because
/// the simulation is a function of them.
///
/// The screen is the original's -- `MPMENU.INI` to find or name a game,
/// `MPGAMEMENU.INI` for the players, where joiners see the same rows the host
/// does, tick "I'm ready", and the host presses Start -- and the messages are
/// what that screen needs:
///
///   * `Query` / `Advert` -- a joiner broadcasts a query on the LAN; a host with
///     its lobby open answers with the map and how many seats are free. What
///     `MPMENU.INI`'s game list and Refresh are for.
///   * `Hello` -- a joiner, to the host, resent until answered. It carries the
///     protocol version and a hash of the joiner's `data.pak`, so that a
///     different game is refused *before* the match rather than discovered by
///     a desync inside it.
///   * `Roster` -- the host, to every seated joiner, repeatedly while the lobby
///     is open: the rows, the rules, the map, and which row is theirs. The map
///     travels as a path and a hash of the file, and a joiner that does not
///     hold that file, byte for byte, leaves.
///   * `Choice` -- a seated joiner, to the host, repeatedly: its own nation,
///     team, bonus and whether it is ready. The host applies it to that
///     joiner's row and to no other.
///   * `Start` -- the host, to each joiner: a `Roster` made final, plus the
///     seed and the match id every datagram will carry. Resent until the
///     joiner's first link-layer datagram arrives, which is the
///     acknowledgement.
///   * `Refuse` -- either way: the host will not seat a joiner, the host closed
///     the game, a joiner left, or a joiner lacks the map.
///
/// And the players' screen's chat, `MPCHAT.INI`, rides the two messages that
/// are resent anyway: a joiner's `Choice` carries the lines it has said and
/// not yet seen heard, and every `Roster` carries the last lines of the
/// lobby's log and how many of this joiner's the host holds. A lost line is
/// sent again with the next choice; a line is logged once, in the order its
/// speaker said it.
///
/// `state-vector.md` names `cmdidseed` and `syncseed` as what a
/// join-in-progress must carry; none of this is that -- a joiner here joins
/// before turn 0, and the seed is the whole of the RNG's state then. A joiner
/// that says hello to a match already running is answered with a save, which
/// carries both (`sim/netjoin.hpp`). None of it is the original's wire
/// either, which was DirectPlay's and is not reproduced; `interface-ini.md`
/// records the screen, not the protocol.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netchat.hpp"

namespace imperivm::core::sim {

/// Bumped whenever any wire format -- turn packet, datagram, lobby -- changes
/// shape. Two builds that disagree on it cannot play.
///
/// 4: the link datagram carries departures, the start names the coordinator,
/// and a refusal can say a peer stopped answering. 5: a turn packet's order
/// carries a speed, and the start says whether the speed may change. 6: chat,
/// in the lobby's roster and choice and in the link datagram. 7: a late
/// joiner (`sim/netjoin.hpp`) -- the join header, chunk and acknowledgement,
/// and the link datagram's admissions and stint-tagged departures.
inline constexpr std::uint32_t kNetProtocol = 7;

/// How many of the lobby's last lines a roster carries: enough for the
/// screen's log, and small enough to keep a roster one datagram.
inline constexpr std::size_t kLobbyChatLines = 12;

/// Where a host listens and a LAN query is broadcast, unless told otherwise.
/// **This engine's number**; the original's DirectPlay port is not used, so
/// that nothing here can be mistaken for talking to it.
inline constexpr std::uint16_t kDefaultNetPort = 47800;

/// `Start::map_index` for "the container's first map", as the app's
/// `--map-index -1` says it.
inline constexpr std::uint32_t kFirstMap = 0xFFFFFFFFu;

/// One row of the players' screen.
struct SeatRow {
  enum class Type : std::uint8_t {
    human = 0,     ///< a peer: the host's own row, or a seated joiner's
    computer = 1,
    closed = 2,
    open = 3,      ///< a seat waiting for a joiner; lobby only, never in a Start
  };
  PlayerId slot = 0;
  Type type = Type::computer;
  std::string name;
  /// `kNoRace` (-1) for Random, else 0..7.
  std::int32_t race = -1;
  /// A computer's strength, 0..2.
  std::uint8_t difficulty = 1;
  /// 0 none, 1..4.
  std::uint8_t team = 0;
  /// 0 none, 1 wealth, 2 riches, 3 hero.
  std::uint8_t bonus = 0;
  bool ready = false;
};

/// The settings screen's rules, as the skirmish setup keeps them.
struct LobbyRules {
  std::string victory;    ///< game script basename, or empty for the map's
  std::string threshold;
  std::int32_t world_population = 100;
  std::int32_t starting_gold = -1;
  bool fog_of_war = true;
  bool exploration = true;
  bool bonuses = true;
  bool shared_support = false;
  bool shared_control = false;
};

struct Hello {
  std::uint32_t protocol = kNetProtocol;
  /// The joiner's `data.pak`, hashed by the platform.
  std::uint64_t content = 0;
  std::string name;
};

/// A `Roster` and a `Start` are the same record; a `Start` is the roster the
/// match was started from, and fills the last four fields.
struct Start {
  PlayerId you = 0;
  /// The container, relative to the installation, as a save names it.
  std::string map;
  /// The container file's hash. The joiner hashes its own copy and leaves if
  /// they differ.
  std::uint64_t map_hash = 0;
  std::uint32_t map_index = kFirstMap;
  /// The game's difficulty, 0..2: `GetDifficulty` has 136 readers.
  std::uint32_t difficulty = 1;
  /// Empty: the map's own setup, every seat human -- what the command line's
  /// `--host` plays. Otherwise the players' screen, every row.
  std::vector<SeatRow> rows;
  LobbyRules rules;
  LockstepConfig config;

  // -- Start only --
  std::uint32_t match = 0;
  std::uint32_t seed = 0;
  /// How many turns the match runs; zero for a match with no fixed end.
  std::uint32_t turns = 0;
  std::vector<PlayerId> links;

  // -- the lobby's chat, in a roster --
  /// The log's last `kLobbyChatLines` lines, oldest first.
  std::vector<ChatLine> chat;
  /// How many lines of `you`'s the host holds: the acknowledgement.
  std::uint32_t chat_heard = 0;
};

/// A joiner's own row, as it chose it.
struct Choice {
  std::int32_t race = -1;
  std::uint8_t team = 0;
  std::uint8_t bonus = 0;
  bool ready = false;
  /// Lines this joiner said that the host has not yet shown it heard, oldest
  /// first. Their speaker is whoever sent the choice, whatever they say.
  std::vector<ChatLine> chat;
};

struct Refuse {
  enum class Reason : std::uint8_t {
    protocol,   ///< a different build
    content,    ///< a different game: data.pak differs
    full,       ///< every seat is taken
    closed,     ///< the host closed the game
    left,       ///< the peer left
    map,        ///< the joiner does not hold the host's map, byte for byte
    /// Nothing heard from the peer for longer than a match waits: the
    /// platform's timing, never sent by the peer itself.
    silent,
  };
  Reason reason = Reason::full;
};

struct Query {
  std::uint32_t protocol = kNetProtocol;
};

struct Advert {
  std::string host;       ///< the host's player name
  std::string map;        ///< the container, as `Start::map`
  std::uint16_t port = kDefaultNetPort;
  std::uint8_t open = 0;  ///< seats free
  std::uint8_t seats = 0; ///< seats in all, joiners and host
};

[[nodiscard]] const char* describe(Refuse::Reason reason) noexcept;

/// What kind of lobby message a datagram is, by its magic, without decoding
/// it. `none` for anything else, including the link layer's own datagrams.
///
/// The late join's three messages (`sim/netjoin.hpp`) share the lobby's
/// framing and are told apart here too.
enum class LobbyKind : std::uint8_t {
  none,
  hello,
  roster,
  start,
  choice,
  refuse,
  query,
  advert,
  join_header,
  join_chunk,
  join_ack,
};
[[nodiscard]] LobbyKind lobby_kind(std::span<const std::byte> bytes) noexcept;

[[nodiscard]] std::vector<std::byte> encode(const Hello& hello);
/// `final` picks the magic: a roster while the lobby is open, a start when it
/// closes.
[[nodiscard]] std::vector<std::byte> encode(const Start& start, bool final = true);
[[nodiscard]] std::vector<std::byte> encode(const Choice& choice);
[[nodiscard]] std::vector<std::byte> encode(const Refuse& refuse);
[[nodiscard]] std::vector<std::byte> encode(const Query& query);
[[nodiscard]] std::vector<std::byte> encode(const Advert& advert);
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, Hello& out);
/// Reads a roster or a start; `lobby_kind` tells them apart.
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, Start& out);
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, Choice& out);
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, Refuse& out);
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, Query& out);
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, Advert& out);

/// Apply a joiner's choice to its row, within what a row may hold. A race
/// out of range becomes Random; a team or bonus out of range is refused and
/// the row keeps what it had. Returns whether anything changed.
bool apply_choice(SeatRow& row, const Choice& choice) noexcept;

}  // namespace imperivm::core::sim
