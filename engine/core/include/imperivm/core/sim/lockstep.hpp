#pragma once

/// The turn negotiator: when a lockstep turn may run, how long it is, and the
/// bytes a peer sends to say what it ordered.
///
/// `sim/netcmds.hpp` answers what a turn's orders *mean* once they are
/// complete. This answers when they *are* complete, and it is still not
/// networking: nothing here opens a socket, reads a clock or waits. A
/// transport hands this bytes in whatever order they arrive and asks whether
/// the next turn may run; the answer is a pure function of what has been
/// handed over. That is what lets `check_netplay` below prove the whole
/// protocol -- encoding, delay, reordering, the length negotiation -- on a
/// loopback in the core, before any platform code exists to get wrong.
///
/// ## The protocol, which is this engine's
///
/// Every peer sends **one packet per turn, for every turn, empty or not**,
/// naming the turn it is for. A turn runs on a peer when that peer holds a
/// packet from every peer for it, and not before. A missing packet stalls the
/// match; nothing here ever guesses that a peer had nothing to say, because
/// the one peer that guessed wrong would be running a different game.
///
/// Orders a player gives while turn `n` is the next to run are scheduled for
/// turn `n + input_delay`, which is what gives the packet time to arrive.
/// Turns `0 .. input_delay - 1` are agreed in advance: no orders, the initial
/// length. None of this is recovered from `gbr.exe`, and the constant is not
/// either -- the retail logs record what the pump ran, never when a command
/// was issued -- so the delay is a parameter and its default is labelled
/// below.
///
/// ## The length is agreed, not assumed
///
/// `docs/engine/tick.md` is the evidence: the dumps' turn length moves
/// mid-session, 400 → 460 → 800 in one log and 460, 291, 213, 200 in another,
/// and it is part of world state -- two peers that disagree about the length of
/// turn `n` are running different simulations. So each packet carries its
/// sender's proposal for the turn in **real milliseconds** (the platform
/// measures the round trip; the core cannot), and every peer computes the same
/// length from the same set of proposals: the largest, clamped to the
/// negotiator's bounds, converted to game time by `turn_length_from_real_ms`.
///
/// "The largest" is this engine's rule and is labelled: it is the only rule
/// under which the slowest peer's packets can keep up, and the dumps show what
/// the length became, never why. The bounds default to the observed extremes
/// (`kMinObservedTurnLength`, `kMaxObservedTurnLength`), which `tick.hpp` is
/// careful to call observations rather than limits; here they are used as
/// limits, and that is also this engine's choice.
///
/// ## What a peer may say
///
/// A packet's orders are all its sender's. `NetOrder::issuer` is the sending
/// peer and `NetOrder::sequence` is the order's position in the packet, so the
/// wire carries neither: a peer cannot issue as somebody else, and cannot
/// number its orders in a way the canonical sort would read differently from
/// how it sent them. Whether the issuer may command the *actors* is not this
/// layer's question -- `issue_default_order` filters by share-control, on every
/// peer alike.
///
/// Receiving the same packet twice is harmless and reported as a duplicate,
/// because a transport that retransmits is a transport that works. Receiving
/// two *different* packets for one (peer, turn) is refused and reported: that
/// peer is equivocating, and accepting either would let two honest peers be
/// told different things.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "imperivm/core/sim/conformance.hpp"
#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/tick.hpp"

namespace imperivm::core::sim {

/// One peer's orders for one turn, and its proposal for that turn's length.
struct TurnPacket {
  PlayerId peer = 0;
  std::uint32_t turn = 0;
  /// Real milliseconds, positive. What this peer can keep up with, as the
  /// platform measured it.
  std::int32_t proposed_ms = 0;
  /// `issuer == peer` and `sequence == index` for every order; see the header.
  std::vector<NetOrder> orders;
};

// -- the wire ----------------------------------------------------------------
//
// Little-endian, the `bytes::` convention the save uses:
//
//     "IMLT"  u8 version  u8 peer  u32 turn  i32 proposed_ms  u32 count
//     count x { u32 actors  actors x u32 id  i32 x  i32 y  u32 object
//               u8 mode  u8 modifier  u8 kind  u8 aimed  u8 length  length x char
//               i32 speed
//               [kind command: u8 repeat]
//               [kind diplomacy: u8 other  u32 relations  u8 allied]
//               [kind cancel_command: u32 command_id] }
//
// kind, aimed and the name being version 2's, when the command bar and the
// surrender joined the stream, the speed version 3's, when `SetSpeed` did,
// the diplomacy tail version 4's, when the Diplomacy screen did, and the
// command's repeat version 5's, when Ctrl on a train row did, and the
// cancel's id version 6's, when the queue strip's click did. Each tail is its
// kind's alone.
//
// and nothing after the last order. Issuer and sequence are not sent; the
// decoder restores them from the peer and the position.

inline constexpr std::uint8_t kTurnPacketVersion = 6;

/// The longest command row name a packet carries. The shipped names are a
/// few dozen characters at most; anything past this is refused, not cut.
inline constexpr std::size_t kMaxCommandName = 64;

[[nodiscard]] std::vector<std::byte> encode(const TurnPacket& packet);

/// Why a packet was not decoded. `ok` is the only success.
enum class DecodeStatus : std::uint8_t {
  ok,
  truncated,     ///< ran off the end, or a count promised more than is there
  bad_magic,
  bad_version,
  bad_field,     ///< a mode or boolean out of range, or a non-positive proposal
  trailing,      ///< bytes after the last order
};

[[nodiscard]] const char* describe(DecodeStatus status) noexcept;

/// Decode one packet. On any status but `ok`, `out` is unspecified.
///
/// Hostile input is the expected case: every count is checked against the
/// bytes remaining **before** anything is allocated, so a four-byte count of
/// four billion is `truncated` rather than an allocation.
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, TurnPacket& out);

// -- the negotiator ----------------------------------------------------------

/// The input delay when nothing else is said: orders given while turn `n` is
/// next run on turn `n + 2`. **This engine's value**, chosen so that a packet
/// has one full turn in flight and one in hand; the original's is not
/// recorded anywhere a retail install keeps.
inline constexpr std::uint32_t kDefaultInputDelay = 2;

struct LockstepConfig {
  /// Every peer in the match, one player slot each. Order does not matter;
  /// the negotiator sorts it.
  std::vector<PlayerId> peers;
  std::uint32_t input_delay = kDefaultInputDelay;
  /// The length of the agreed-in-advance turns, in real milliseconds. Every
  /// dump opens at 400 (`kDefaultTurnLength`) before the negotiation moves it.
  std::int32_t initial_ms = kDefaultTurnLength;
  std::int32_t min_ms = kMinObservedTurnLength;
  std::int32_t max_ms = kMaxObservedTurnLength;
  /// Converts the agreed real length to game time, per mille: the speed the
  /// match starts at. A `set_speed` order agreed for turn `t` moves it from
  /// turn `t + 1` on, on every peer alike.
  std::int32_t game_speed = kDefaultGameSpeed;
  /// Whether a player may change the speed mid-match: the lobby's
  /// `VariableSpeed` (-1 in `CONST.INI`), which is the lobby's default
  /// (0x006dc74c). A fixed speed refuses `set_speed` everywhere: `submit`
  /// never sends one, as the original's issuer never posts one (game +0x230,
  /// checked in 0x004c6ce0), and `take` strips any that arrive, so a peer
  /// that thinks otherwise changes nothing.
  bool variable_speed = true;
  /// The peer that decides where a departed peer's turns end
  /// (`sim/netdepart.hpp`). `kNoPlayer` is the lowest peer. The lobby names
  /// the host, which is also the relay every joiner links to.
  PlayerId coordinator = kNoPlayer;

  /// `coordinator`, or the lowest peer when it names nobody in the match.
  [[nodiscard]] PlayerId resolved_coordinator() const noexcept;
};

/// What `receive` made of a packet.
enum class ReceiveStatus : std::uint8_t {
  accepted,
  duplicate,     ///< the same packet again: harmless, and not stored twice
  conflicting,   ///< a different packet for a (peer, turn) already held: refused
  unknown_peer,
  stale,         ///< for a turn that has run, or one agreed in advance
  too_far,       ///< further ahead than any honest peer can be
  malformed,     ///< issuer or sequence not what the wire implies, or no proposal
  /// From a peer the match has dropped, for a turn past its last: not
  /// stored, and not a refusal -- a packet sent before its sender left is
  /// still arriving on an honest network.
  departed,
};

[[nodiscard]] const char* describe(ReceiveStatus status) noexcept;

/// A turn every peer agrees on, ready to apply and advance.
struct AgreedTurn {
  std::uint32_t index = 0;
  std::int32_t real_ms = 0;
  std::int32_t length = 0;  ///< game-time units: what `advance` is handed
  /// The speed `length` was converted at, per mille.
  std::int32_t game_speed = kDefaultGameSpeed;
  NetTurn orders;           ///< every peer's, in peer order; apply canonically
};

/// One stretch of a seat's part in the match: it plays from `from`, its
/// packets are needed from `from + input_delay` -- the turns before that are
/// agreed empty for it, as turns `0 .. input_delay - 1` are for everyone at
/// the start -- and, once dropped, until `until`.
///
/// Every peer starts with one stint from turn 0. A departure closes the last
/// one (`sim/netdepart.hpp`); a late join opens another on the same seat
/// (`sim/netjoin.hpp`). Stints never overlap and never touch: a seat is
/// joined strictly after the turn it was dropped from.
struct Stint {
  std::uint32_t from = 0;
  std::optional<std::uint32_t> until;
  friend bool operator==(const Stint&, const Stint&) = default;
};

/// A seat and every stint of it, as a late joiner is told them.
struct SeatStints {
  PlayerId peer = 0;
  std::vector<Stint> stints;
  friend bool operator==(const SeatStints&, const SeatStints&) = default;
};

/// Where a peer that joined a running match starts (`sim/netjoin.hpp`): the
/// first turn it runs, the stream hash through the turns before it, and who
/// plays which turns, as the host held them when it took the save.
struct ResumePoint {
  std::uint32_t turn = 0;
  std::uint64_t netcmds = 0;
  std::vector<SeatStints> membership;
};

class TurnNegotiator {
 public:
  /// `local` must be one of `config.peers`; a negotiator for a peer that is
  /// not in the match refuses every submit.
  TurnNegotiator(LockstepConfig config, PlayerId local);
  /// A late joiner's: it starts at `resume.turn`, with the membership it was
  /// told and its stream hash resuming from `resume.netcmds`. Its first
  /// packet is for `resume.turn + input_delay`, which its own stint needs --
  /// and if the membership gives it no such stint, it submits nothing.
  TurnNegotiator(LockstepConfig config, PlayerId local, const ResumePoint& resume);

  /// The local player's orders for the turn being scheduled, and its
  /// proposal. Stamps issuer and sequence, keeps the packet as received from
  /// itself, and returns it for the transport to send to everyone else.
  ///
  /// Once per turn: it schedules for `next_turn() + input_delay`, and a second
  /// call before that turn's `take` has nothing to schedule and returns
  /// nothing.
  [[nodiscard]] std::optional<TurnPacket> submit(std::vector<NetOrder> orders,
                                                 std::int32_t proposed_ms);

  ReceiveStatus receive(const TurnPacket& packet);
  /// Decode and receive. A packet that does not decode is `malformed`.
  ReceiveStatus receive(std::span<const std::byte> bytes);

  /// Whether `next_turn()` has a packet from every peer.
  [[nodiscard]] bool ready() const noexcept;

  /// The next turn, if ready, removed from the pending set and appended to
  /// the history. Its length is converted at the speed in force; the last
  /// `set_speed` among its orders, in canonical order, is the speed of every
  /// turn after it.
  [[nodiscard]] std::optional<AgreedTurn> take();

  [[nodiscard]] std::uint32_t next_turn() const noexcept { return next_turn_; }
  [[nodiscard]] const LockstepConfig& config() const noexcept { return config_; }
  [[nodiscard]] PlayerId local() const noexcept { return local_; }

  // -- a peer that left (`sim/netdepart.hpp` drives these) -------------------

  /// `departed` has gone. From now on this peer runs no turn it does not
  /// already hold that peer's packet for, until `drop` says where its turns
  /// end. Returns this peer's report: the first turn, from the next one on,
  /// for which it lacks `departed`'s packet -- every turn it has run is below
  /// that, and every turn it will run before the decision is too. Repeated
  /// calls return the first answer; a peer not in the match, or already
  /// dropped, returns `next_turn()`.
  std::uint32_t suspend(PlayerId departed);

  /// The match's decision: `departed`'s packets count for every turn below
  /// `from_turn` and it takes no part from then on -- no packet waited for,
  /// none taken, its proposal no longer in the largest. Turns below
  /// `from_turn` still wait for its packet, which the link layer is still
  /// carrying from whoever holds it.
  ///
  /// False, changing nothing, when this peer has already run a turn at or
  /// past `from_turn` with that peer's packet in it: an honest match never
  /// asks this (every report bounds what its reporter ran), and a peer that
  /// is asked has played a different game from the one decided.
  ///
  /// `since` names the stint the decision is about, when the caller knows
  /// it: a decision on an earlier stint than the current one is only
  /// checked against it, never applied to the current one.
  bool drop(PlayerId departed, std::uint32_t from_turn,
            std::optional<std::uint32_t> since = std::nullopt);

  [[nodiscard]] bool suspended(PlayerId peer) const noexcept;
  /// The turn `peer`'s current part ends at, once dropped.
  [[nodiscard]] std::optional<std::uint32_t> dropped_from(PlayerId peer) const noexcept;
  /// Whether `peer` takes part in `turn`: in the match, and inside one of its
  /// stints.
  [[nodiscard]] bool plays(PlayerId peer, std::uint32_t turn) const noexcept;
  /// Whether `peer`'s packet is needed for `turn`: it plays it, and the turn
  /// is not one of the `input_delay` agreed empty at the start of its stint.
  [[nodiscard]] bool required(PlayerId peer, std::uint32_t turn) const noexcept;

  // -- a late joiner (`sim/netjoin.hpp` drives these) ------------------------

  /// `peer`'s seat is played again from `from_turn`: a new stint, whose
  /// first `input_delay` turns are agreed empty for it and whose first turn
  /// carries a `joined` order the negotiator writes in. The host decides
  /// `from_turn` past every packet it has sent, so no peer can have run it.
  ///
  /// False, changing nothing, when `peer`'s last stint is still open, when
  /// `from_turn` is not after the turn it was dropped from, or when this peer
  /// has already run `from_turn`. True again for the same admission.
  bool admit(PlayerId peer, std::uint32_t from_turn);
  /// Every seat's stints, in peer order: what a joiner is told.
  [[nodiscard]] std::vector<SeatStints> membership() const;
  /// Every packet held for `turn` and after, oldest turn first: what a host
  /// hands the link to a joiner that starts there.
  [[nodiscard]] std::vector<TurnPacket> held_from(std::uint32_t turn) const;
  /// The turn this peer's next packet will be for.
  [[nodiscard]] std::uint32_t next_submit() const noexcept { return next_submit_; }
  /// The first turn this peer ran: zero, or where it joined.
  [[nodiscard]] std::uint32_t first_turn() const noexcept { return history_.first; }

  /// Every turn taken so far, as the stream `netcmds` folds and a
  /// `StreamDriver` replays, and the lengths they ran at.
  [[nodiscard]] const CommandStream& history() const noexcept { return history_; }
  [[nodiscard]] const std::vector<std::int32_t>& schedule() const noexcept { return schedule_; }

 private:
  [[nodiscard]] std::optional<std::size_t> slot_of(PlayerId peer) const noexcept;
  /// The stint of `slot` that `turn` falls in, if any.
  [[nodiscard]] const Stint* stint_at(std::size_t slot, std::uint32_t turn) const noexcept;
  [[nodiscard]] bool required_slot(std::size_t slot, std::uint32_t turn) const noexcept;

  LockstepConfig config_;
  PlayerId local_;
  std::uint32_t next_turn_ = 0;
  std::uint32_t next_submit_ = 0;
  /// `pending_[t - next_turn_][slot]`: a window, not a map, because nothing
  /// honest is more than `2 * input_delay` ahead and anything further is
  /// refused before it is stored.
  std::vector<std::vector<std::optional<TurnPacket>>> pending_;
  /// Per slot: the report `suspend` gave on its current stint, and every
  /// stint, oldest first -- never empty.
  std::vector<std::optional<std::uint32_t>> suspended_;
  std::vector<std::vector<Stint>> stints_;
  CommandStream history_;
  std::vector<std::int32_t> schedule_;
};

// -- the loopback check ------------------------------------------------------

/// How `check_netplay` saves a peer's run and builds a late joiner's run from
/// the bytes -- a save, as a real host sends one (`sim/netjoin.hpp`). The
/// harness knows nothing of what a run is; this does.
class RunSnapshots {
 public:
  virtual ~RunSnapshots() = default;
  [[nodiscard]] virtual std::vector<std::byte> save(conformance::Run& run) const = 0;
  /// Null when the bytes do not load.
  [[nodiscard]] virtual std::unique_ptr<conformance::Run> resume(
      std::uint32_t seed, std::span<const std::byte> save) const = 0;
  /// A late joiner's run, from its own seat: see `Scenario::start_as`.
  [[nodiscard]] virtual std::unique_ptr<conformance::Run> resume_as(
      std::uint32_t seed, std::span<const std::byte> save, PlayerId local) const {
    (void)local;
    return resume(seed, save);
  }
};

/// Where a peer's `command`, `surrender`, `departed` and `joined` orders go
/// in `check_netplay`: a sink over the run, when the run has something to
/// apply them to (`CommandBarSink` over a session). Without one they are
/// counted unapplied on every peer alike. The unnetworked replay uses the
/// same, so it takes the seat over and hands it back where the peers did.
class RunSinks {
 public:
  virtual ~RunSinks() = default;
  /// Null for a run with nothing to apply them to.
  [[nodiscard]] virtual std::unique_ptr<NetCommandSink> sink(conformance::Run& run) const = 0;
};

/// Orders a peer gives from what its own world shows, beside `intent`'s: a
/// player's, who clicks on what is there rather than on a list written
/// before the match. Asked as the peer sends each packet, with the packet's
/// index and the peer's run; only the orders whose issuer is the peer's
/// player are sent. Every peer is asked with its world on the same turn, so
/// a choice made from the world is the same on every peer.
class RunOrders {
 public:
  virtual ~RunOrders() = default;
  [[nodiscard]] virtual std::vector<NetOrder> orders(PlayerId player, std::size_t packet,
                                                     conformance::Run& run) = 0;
};

/// What a peer's run checks its agreed orders against before it queues them:
/// a click's verifier over the run's own scripts (`ScriptOrderVerifier` over
/// a session), as the app's peer has. Without one every row with a `verify=`
/// -- `attack` among them -- is blocked on every peer alike. The unnetworked
/// replay is given its own.
class RunVerifiers {
 public:
  virtual ~RunVerifiers() = default;
  /// Null for a run with no scripts to verify with.
  [[nodiscard]] virtual std::unique_ptr<OrderVerifier> verifier(conformance::Run& run) const = 0;
};

/// How the harness's network misbehaves. Every choice is drawn from `seed`
/// and nothing else.
struct NetplayOptions {
  LockstepConfig lockstep;
  std::uint32_t seed = 1;
  /// A packet is delivered between 0 and `max_delay` rounds after it is sent,
  /// independently per recipient, so arrival order is scrambled both across
  /// senders and across turns.
  ///
  /// A packet is sent as its sender starts turn `n` and is needed for turn
  /// `n + 1 + input_delay`, so the protocol has `input_delay + 1` rounds of
  /// slack and a network no later than that never makes anyone wait -- a
  /// check that would pass with the stall path deleted. The default is past
  /// the slack of the default delay, so some packets are late.
  std::uint32_t max_delay = kDefaultInputDelay + 3;
  /// Every packet is also delivered a second time, later: a retransmitting
  /// transport. `duplicate` receipts are counted and must change nothing.
  bool duplicate_every_packet = true;
  /// Each peer proposes a length drawn from `[min_ms, max_ms]` every turn, so
  /// the negotiated schedule moves the way the dumps' does.
  bool vary_proposals = true;

  /// Carry the packets through `sim/netlink.hpp` instead of handing them
  /// straight to the negotiators: the lowest peer is a relay every other peer
  /// links to, each node sends every link one datagram per round, and whole
  /// datagrams are lost at `loss_per_mille`. `duplicate_every_packet` does not
  /// apply -- the link layer resends by itself.
  bool relay = false;
  std::uint32_t loss_per_mille = 0;
  /// Real milliseconds a round stands for, which is the clock the link layer
  /// measures round trips on.
  std::uint32_t round_ms = 40;
  /// Propose what the link layer measured rather than a drawn length. Only
  /// with `relay`; otherwise there is nothing measured.
  bool measured_proposals = false;

  /// A peer that leaves mid-match: once it has run `after_turns` turns it
  /// sends and takes nothing more, and `noticed_after` rounds later every
  /// node linked to it learns it has gone -- the refusal a leaving peer sends,
  /// or the silence of one that crashed. Only with `relay`, since the
  /// agreement on where its turns end travels in the link layer.
  struct Departure {
    PlayerId peer = 0;
    std::size_t after_turns = 0;
    std::size_t noticed_after = 2;
  };
  std::vector<Departure> departures;

  /// A late joiner (`sim/netjoin.hpp`): once the coordinator has run
  /// `after_turns` turns and `peer`'s seat is open -- its player departed and
  /// the drop is applied -- the coordinator seats a new peer there. The new
  /// peer is told the header at once and sends its first packet; its world
  /// loads `transfer_rounds` rounds later, which is the save in flight. Only
  /// with `relay` and `snapshots`.
  struct Join {
    PlayerId peer = 0;
    std::size_t after_turns = 0;
    std::size_t transfer_rounds = 4;
    /// The coordinator asks for this speed, per mille, with its first packet
    /// after seating the joiner: turn `S - 1`, the last one the save carries,
    /// whose speed is the first the joiner runs at. Zero asks for none.
    std::int32_t speed = 0;
    /// The joiner leaves in turn once it has run this many turns, and is
    /// dropped as any peer is. Zero stays.
    std::size_t leave_after = 0;
  };
  std::vector<Join> joins;
  const RunSnapshots* snapshots = nullptr;
  /// See `RunSinks`; null counts those orders unapplied.
  const RunSinks* sinks = nullptr;
  /// See `RunOrders`; null gives `intent`'s alone.
  RunOrders* players = nullptr;
  /// See `RunVerifiers`; null applies with `check_netplay`'s `verifier`.
  const RunVerifiers* verifiers = nullptr;

  /// Every peer still playing says a line of chat every `chat_every` rounds.
  /// Only with `relay`. Chat is not an input: a match with it must be the
  /// same match, hash for hash, as one without.
  std::size_t chat_every = 0;
};

struct NetplayReport {
  std::size_t peers = 0;
  std::size_t turns = 0;          ///< turns every peer ran
  std::size_t rounds = 0;
  std::size_t stalls = 0;         ///< peer-rounds spent waiting for a packet
  std::size_t packets = 0;        ///< deliveries, duplicates included
  std::size_t bytes = 0;
  std::size_t duplicates = 0;
  std::size_t refused = 0;        ///< receipts that were neither accepted nor duplicate
  std::size_t orders = 0;         ///< orders in the agreed stream
  std::size_t issued = 0;         ///< commands queued, summed over every peer
  std::vector<std::int32_t> schedule;  ///< the agreed lengths, as peer 1 ran them
  /// Every turn's rolled-up world hash, as the first peer that stayed ran it:
  /// what one match is compared with another by, where the network's seed is
  /// all that differs.
  std::vector<std::uint64_t> hashes;
  bool deadlocked = false;
  // With `relay` only:
  std::size_t datagrams = 0;      ///< sent, lost ones included
  std::size_t lost = 0;
  std::size_t most_held = 0;      ///< the largest any node's forwarding store grew
  std::size_t still_held = 0;     ///< summed over nodes after the last round
  // With `departures` only:
  /// Each departed peer and the turn its part ended at, as the first
  /// remaining peer applied it.
  std::vector<std::pair<PlayerId, std::uint32_t>> dropped;
  /// Packets of a dropped peer past its last turn that arrived anyway.
  std::size_t late = 0;
  /// The coordinator left, so no departure could be decided and the match
  /// ended for every peer -- reported, never a deadlock.
  bool coordinator_left = false;
  // With `joins` only:
  /// Each seat a late joiner took and the first turn it ran, as the
  /// coordinator decided it.
  std::vector<std::pair<PlayerId, std::uint32_t>> joined;
  /// Turns late joiners ran, summed.
  std::size_t joiner_turns = 0;
  // With `chat_every` only:
  std::size_t chat_said = 0;
  std::size_t chat_heard = 0;       ///< deliveries, summed over every peer
  std::size_t chat_misordered = 0;  ///< a line not the next from its speaker
};

/// Run a match between `options.lockstep.peers` over a loopback that delays,
/// reorders and repeats every packet, and check that every peer ran the same
/// game.
///
/// `intent.turns[k]` holds the orders players give while their `k`-th turn is
/// next; each peer submits the ones its own player issued. Every peer's world
/// comes from `scenario.start(seed)`.
///
/// Two comparisons, both through `compare_traces`:
///
///   * each peer against the first, turn by turn, every channel -- `netcmds`
///     included, which here is `stream_hash` over the *agreed* history;
///   * the first peer against `record` of the same scenario driven by a
///     `StreamDriver` over that agreed history and schedule, with no network
///     at all. This is the one that proves the transport transparent: whatever
///     the network did, the match it produced is a match `check_lockstep`
///     could have been handed directly.
///
/// A deadlock (no peer able to move for longer than any delay explains) is
/// reported as `Divergence::Kind::unbuildable` with `deadlocked` set, because
/// it is a harness that could not finish rather than two peers that disagree.
///
/// With `departures`, the peers that stay are compared as above over every
/// turn, and a departed peer against the first that stayed over the turns it
/// ran below the agreed end of its part -- which is where it and the rest
/// were playing one game. A departed coordinator ends the match, reported as
/// `unbuildable` with `coordinator_left` set.
///
/// With `joins`, a late joiner is compared with the first peer that stayed
/// over every turn it ran, from its first -- every channel, `netcmds`
/// included -- and must have run to the end.
[[nodiscard]] conformance::Divergence check_netplay(const conformance::Scenario& scenario,
                                                    std::uint32_t seed, std::size_t turns,
                                                    const CommandStream& intent,
                                                    const NetplayOptions& options,
                                                    NetplayReport* report = nullptr,
                                                    OrderVerifier* verifier = nullptr);

}  // namespace imperivm::core::sim
