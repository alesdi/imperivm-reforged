#pragma once

/// The link layer: turn packets over a network that loses them.
///
/// `sim/lockstep.hpp` tolerates a packet arriving late, twice, or out of
/// order. It does not tolerate one never arriving -- the match stalls on it,
/// correctly and forever -- and UDP loses packets. This is what stands between
/// the two, and like the negotiator it is pure: no socket, no clock. The
/// platform hands it bytes and the time and sends what it returns.
///
/// ## Every datagram repairs the last one
///
/// A node keeps every turn packet it has originated or learned until each of
/// its links has acknowledged it, and **every datagram carries all of them that
/// the other side has not acknowledged**, oldest turn first, up to a byte
/// budget. There is no retransmission timer, because there is nothing to time:
/// a lost datagram is repaired by the next one, whenever the platform next
/// sends. The backlog is bounded by the protocol itself -- no honest peer runs
/// more than `2 * input_delay + 1` turns ahead -- so in steady state a datagram
/// holds a handful of small packets. This is the redundant-send scheme lockstep
/// games have used since the modem era; nothing here is recovered from
/// `gbr.exe`, whose transport was DirectPlay and is not reproduced.
///
/// Acknowledgement is **per origin**, not per datagram: "I hold every packet
/// from peer P for every turn below N". That is the only fact a sender needs,
/// and it is idempotent, so an acknowledgement that is itself lost or reordered
/// costs nothing but a resend.
///
/// ## Relaying
///
/// A packet is offered on every link except the one it arrived on, and never
/// to the peer that originated it -- which falls out of the acknowledgements,
/// since a peer's own packets are contiguous from the moment it offers them. With every peer linked only to a host, that
/// makes the host a relay -- the topology that needs one reachable address,
/// which is the usual case behind home routers. With every peer linked to
/// every other it is a mesh with some redundancy that the acknowledgements
/// switch off after one round trip. The node does not know which it is in, and
/// does not need to: the topology is the list of links it was built with.
///
/// ## Round trip, and what a peer proposes
///
/// Every datagram carries the sender's clock (`stamp`), the last stamp it
/// received on this link (`echo`) and how long it held that stamp before
/// replying (`hold`). The receiver's round trip is `now - echo - hold`, which
/// neither needs the two clocks to agree nor counts the time the far side spent
/// before answering. It is smoothed as `srtt = (7 * srtt + sample) / 8`, in
/// integers.
///
/// `proposal` turns that into the length this peer asks the negotiator for.
/// A packet sent as turn `n` starts is needed when turn `n + 1 + delay`
/// starts, so it has `delay + 1` turns to arrive; through a relay it crosses
/// two links, about one round trip. So the proposal is the slowest link's
/// smoothed round trip spread over `delay + 1` turns, with half as much again
/// as margin: `ceil(3 * srtt / (2 * (delay + 1)))`. **This engine's rule**, and
/// the negotiator clamps it; on a LAN it is always the floor.
///
/// ## Departures travel here too
///
/// When a peer leaves, the rest must agree on the last turn it played
/// (`sim/netdepart.hpp` says how). The facts that agreement is made of --
/// "reporter R saw D go and holds D's packets below turn N", and the
/// coordinator's "D's part ends at turn L" -- are carried by every datagram,
/// all of them, every time, the way the acknowledgements are: they are few,
/// idempotent and never retracted, so a lost datagram is repaired by the next
/// one and a relay passes them on without knowing what they mean. A node that
/// knows D has gone stops treating D as a link (it will never acknowledge
/// anything again), and once D's end is known, D's packets past it are
/// forgotten and, arriving late, are counted as late rather than refused.
///
/// ## Joins too
///
/// A late joiner (`sim/netjoin.hpp`) takes a seat whose player left. The
/// coordinator's "`P` plays again from turn `F`" is a fact of the same kind
/// as a drop -- few, idempotent, never retracted, carried by every datagram --
/// and every departure fact names the stint it is about by that `F` (zero for
/// the part every seat starts the match with), so that a seat can leave, be
/// joined and leave again without one part's facts being read as another's.
/// A node accepts a seat's packets only for the turns one of its stints needs;
/// anything else from it is late. A joiner starts from the host's
/// `ResumePoint`, and the host gains a link to it with `add_link`.
///
/// ## And chat
///
/// What players say (`sim/netchat.hpp`) rides beside the turn packets, in the
/// room the budget leaves after them: the same repeat-until-acknowledged
/// scheme, keyed by speaker and line number, relayed the same way. It never
/// reaches the negotiator -- the node hands a line to its caller, in order,
/// once -- so no line is in any hash.

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <tuple>
#include <span>
#include <vector>

#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netchat.hpp"

namespace imperivm::core::sim {

/// "I hold every packet from `peer` for every turn below `next_turn`."
struct LinkAck {
  PlayerId peer = 0;
  std::uint32_t next_turn = 0;
};

/// "`reporter` saw `departed` go, and holds its packets for every turn below
/// `held`" -- `TurnNegotiator::suspend`'s answer, sent to the coordinator.
/// `since` is the first turn of the stint it is about: zero, or where a late
/// joiner came in.
struct DepartureReport {
  PlayerId departed = 0;
  PlayerId reporter = 0;
  std::uint32_t held = 0;
  std::uint32_t since = 0;
};

/// The coordinator's decision: `departed`'s packets count for every turn below
/// `from_turn` of the stint that began at `since`, and it takes no part after.
/// Final.
struct Drop {
  PlayerId departed = 0;
  std::uint32_t from_turn = 0;
  std::uint32_t since = 0;
};

/// The coordinator's other decision: `peer`'s seat is played again from
/// `from_turn`, by a late joiner. Final; `from_turn` is never zero.
struct Admit {
  PlayerId peer = 0;
  std::uint32_t from_turn = 0;
};

/// What one node sends another.
///
///     "IMLD"  u8 version  u32 match  u8 from  u32 stamp  u32 echo  u32 hold
///     u8 acks x { u8 peer  u32 next_turn }
///     u16 packets x { u32 length  length x byte }   -- an encoded TurnPacket
///     u8 reports x { u8 departed  u8 reporter  u32 held  u32 since }
///     u8 drops x { u8 departed  u32 from_turn  u32 since }
///     u8 admits x { u8 peer  u32 from_turn }
///     u8 chat_acks x { u8 peer  u32 next_line }
///     u16 chat x { u32 length  length x byte }       -- an encoded ChatLine
///
/// reports and drops version 2's, when a departed peer stopped ending the
/// match; chat version 3's; admits and the stint a departure is about (`since`)
/// version 4's, when a late joiner could take a departed player's seat.
/// `hold == kNoEcho` means the sender has not heard from this link yet, and
/// `echo` is then meaningless.
struct Datagram {
  std::uint32_t match = 0;
  PlayerId from = 0;
  std::uint32_t stamp = 0;
  std::uint32_t echo = 0;
  std::uint32_t hold = 0;
  std::vector<LinkAck> acks;
  std::vector<std::vector<std::byte>> packets;
  std::vector<DepartureReport> reports;
  std::vector<Drop> drops;
  std::vector<Admit> admits;
  /// "I hold every line `peer` said below `next_turn`" -- the field reused.
  std::vector<LinkAck> chat_acks;
  std::vector<std::vector<std::byte>> chat;
};

inline constexpr std::uint8_t kDatagramVersion = 4;
inline constexpr std::uint32_t kNoEcho = 0xFFFFFFFFu;

/// The size a datagram's packets are packed to. Under the 1280-byte IPv6
/// minimum MTU with room for the headers, so nothing fragments; a single
/// packet larger than this is still sent, alone, and fragments.
inline constexpr std::size_t kDatagramBudget = 1200;

[[nodiscard]] std::vector<std::byte> encode(const Datagram& datagram);
/// Structure only: the packets inside are left encoded, and a bad one is the
/// receiver's to refuse without losing the rest of the datagram.
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, Datagram& out);

/// What a node made of one datagram.
struct LinkReceipt {
  enum class Status : std::uint8_t {
    ok,
    malformed,      ///< did not decode
    wrong_match,    ///< another match's traffic on this port
    unknown_link,   ///< from a node this one has no link to
    departed_link,  ///< from a peer known to have gone: late, not refused
  };
  Status status = Status::ok;
  /// New to this node, in datagram order: hand each to the negotiator.
  std::vector<TurnPacket> packets;
  std::size_t duplicates = 0;   ///< already held, and the same bytes
  std::size_t conflicting = 0;  ///< already held, different bytes: kept the first
  std::size_t refused = 0;      ///< did not decode, or not a packet any peer may send
  /// A dropped peer's packets past its end: sent before it left, and not part
  /// of the match. Not refused -- an honest network delivers them.
  std::size_t late = 0;
  /// A departure report or a drop this node had not heard of, or one that
  /// contradicts what it holds (kept the first; counted in `conflicting`).
  bool departures_changed = false;
  /// Lines new to this node, each speaker's in the order said: show them.
  std::vector<ChatLine> chat;
};

class LinkNode {
 public:
  /// `links` are the nodes this one exchanges datagrams with, by player slot.
  /// Every peer in `config.peers` must be reachable through them for the match
  /// to run, which is the caller's topology to get right.
  LinkNode(std::uint32_t match, const LockstepConfig& config, PlayerId self,
           std::vector<PlayerId> links);
  /// A late joiner's node: it needs every packet from `resume.turn` on, its
  /// own from its stint's first packet, and it knows every seat's stints and
  /// how many lines the host held from each speaker -- the numbering it
  /// continues from, its own included.
  LinkNode(std::uint32_t match, const LockstepConfig& config, PlayerId self,
           std::vector<PlayerId> links, const ResumePoint& resume,
           const std::map<PlayerId, std::uint32_t>& chat_next);

  /// A packet this node originated. Held until every link has it.
  void offer(const TurnPacket& packet);

  /// The datagram to send `link` now. Always something -- a datagram with no
  /// packets still carries the acknowledgements and the clock -- and always a
  /// function of what this node holds and knows, so sending it twice is
  /// harmless.
  [[nodiscard]] std::vector<std::byte> datagram_for(PlayerId link, std::uint32_t now);

  LinkReceipt receive(std::span<const std::byte> bytes, std::uint32_t now);

  // -- chat --------------------------------------------------------------

  /// This node's player says `line`. Its speaker and number are this node's
  /// to give, and are returned stamped; carried until every link has it.
  ChatLine say(ChatLine line);
  /// Lines held for forwarding.
  [[nodiscard]] std::size_t chat_held() const noexcept { return chat_store_.size(); }

  // -- departures --------------------------------------------------------

  /// `peer` has gone, as this node saw it directly: a refusal saying so, or
  /// a silence the platform timed. It stops being a link.
  void depart(PlayerId peer);
  /// This node's own report on `departed`. Once per departed peer.
  void report(PlayerId departed, std::uint32_t held);
  /// The coordinator's decision. Once per departed peer; a second, different
  /// one is ignored.
  void decide(PlayerId departed, std::uint32_t from_turn);

  /// Every peer this node knows has gone from its current stint: seen,
  /// reported by anyone, or dropped. Ascending.
  [[nodiscard]] std::vector<PlayerId> departed() const;
  /// The reports on each peer's current stint, keyed (departed, reporter).
  [[nodiscard]] std::map<std::pair<PlayerId, PlayerId>, std::uint32_t> reports() const;
  /// The decisions on each peer's current stint.
  [[nodiscard]] std::map<PlayerId, std::uint32_t> drops() const;
  /// Every decision, on every stint, in (peer, stint) order.
  [[nodiscard]] std::vector<Drop> decisions() const;

  // -- joins -------------------------------------------------------------

  /// The coordinator's decision that `peer` plays again from `from_turn`.
  /// Once per stint; the seat's current stint must have been dropped.
  void admit(PlayerId peer, std::uint32_t from_turn);
  /// Every admission, in (peer, turn) order.
  [[nodiscard]] std::vector<Admit> admits() const;
  /// The first turn of `peer`'s current stint: zero, or where it was joined.
  [[nodiscard]] std::uint32_t since(PlayerId peer) const noexcept;
  /// Whether a packet from `peer` for `turn` is one of the match's: inside a
  /// stint of that seat, past its agreed-empty turns.
  [[nodiscard]] bool accepts(PlayerId peer, std::uint32_t turn) const noexcept;
  /// A link to a late joiner that starts at `from_turn` (the host's, when it
  /// sends the save). `packets` are what the host holds for that turn and
  /// after -- its negotiator's, since the link layer may already have
  /// forgotten them -- and are sent it like any other; nothing older is.
  void add_link(PlayerId peer, std::uint32_t from_turn, const std::vector<TurnPacket>& packets);
  /// The next line this node expects from each speaker: what a joiner
  /// continues from.
  [[nodiscard]] const std::map<PlayerId, std::uint32_t>& chat_next() const noexcept {
    return chat_next_;
  }

  /// Smoothed round trip to `link` in milliseconds; zero until measured.
  [[nodiscard]] std::uint32_t srtt(PlayerId link) const noexcept;
  /// The turn length to propose, in real milliseconds. `config.initial_ms`
  /// until some link has been measured.
  [[nodiscard]] std::int32_t proposal() const noexcept;

  /// Packets held for forwarding. Bounded by the slowest link's lag; a test
  /// asserts it drains.
  [[nodiscard]] std::size_t held() const noexcept { return store_.size(); }
  [[nodiscard]] PlayerId self() const noexcept { return self_; }
  [[nodiscard]] const std::vector<PlayerId>& links() const noexcept { return links_; }

 private:
  struct Stored {
    std::vector<std::byte> bytes;
    PlayerId from_link = kNoPlayer;  ///< kNoPlayer for our own
  };
  struct Link {
    PlayerId peer = 0;
    std::map<PlayerId, std::uint32_t> acked;  ///< origin -> their next_turn
    std::uint32_t last_stamp = 0;
    std::uint32_t stamp_received_at = 0;
    bool heard = false;
    std::uint32_t srtt = 0;
    std::map<PlayerId, std::uint32_t> chat_acked;  ///< speaker -> their next line
  };
  struct Held {
    std::uint32_t next = 0;           ///< contiguous from `input_delay`
    std::set<std::uint32_t> above;    ///< held beyond the gap
  };

  [[nodiscard]] Link* link_of(PlayerId peer) noexcept;
  [[nodiscard]] const Link* link_of(PlayerId peer) const noexcept;
  [[nodiscard]] bool is_peer(PlayerId peer) const noexcept;
  [[nodiscard]] bool holds(PlayerId origin, std::uint32_t turn) const;
  void mark_held(PlayerId origin, std::uint32_t turn);
  [[nodiscard]] std::uint32_t acked(const Link& link, PlayerId origin) const;
  void prune();
  void forget_link(PlayerId peer);
  /// Forget every packet held that no stint needs: a departed peer's past its
  /// end.
  void cut();
  /// `peer` plays from `from_turn`: its packets are needed from the end of
  /// that stint's agreed-empty turns, and nothing before them.
  void open_stint(PlayerId peer, std::uint32_t from_turn);

  std::uint32_t match_;
  LockstepConfig config_;
  PlayerId self_;
  std::vector<PlayerId> links_;
  std::vector<Link> link_state_;
  std::map<PlayerId, Held> held_;
  /// Keyed (origin, turn), so iteration is oldest turn within each origin and
  /// the order is a function of the contents alone.
  std::map<std::pair<PlayerId, std::uint32_t>, Stored> store_;
  /// Chat, the same shape: (speaker, line) -> the encoded line, and the next
  /// line expected from each speaker. Lines are taken in order only.
  std::map<std::pair<PlayerId, std::uint32_t>, Stored> chat_store_;
  std::map<PlayerId, std::uint32_t> chat_next_;
  /// Departures: seen directly (keyed peer, stint), reported (keyed departed,
  /// stint, reporter), decided (keyed departed, stint); and admissions (peer,
  /// stint). A stint is named by its first turn.
  std::set<std::pair<PlayerId, std::uint32_t>> seen_;
  std::map<std::tuple<PlayerId, std::uint32_t, PlayerId>, std::uint32_t> reports_;
  std::map<std::pair<PlayerId, std::uint32_t>, std::uint32_t> drops_;
  std::set<std::pair<PlayerId, std::uint32_t>> admits_;
};

}  // namespace imperivm::core::sim
