#pragma once

/// A late joiner: a player who joins a match that is already running.
///
/// **All of this is this engine's.** The original admits nobody once a match
/// has started (`docs/engine/netjoin.md`, question 1): its transport's
/// membership is set once, when the game builds it (0x004071b0), and only
/// ever loses bits. There is nothing to reproduce, so what follows is built
/// from the parts the match already agrees through -- the stream, the link
/// layer's facts, the save -- and every choice below is labelled as ours.
///
/// ## Which seat
///
/// **A seat whose player left and which the computer now plays.** The
/// departure work (`sim/netdepart.hpp`) creates these, and a join is its
/// reverse: the same seat, the same `PlayerId`, played again. An empty open
/// seat is not joinable, because there is none in a running match -- a
/// lobby's open row that nobody took is closed when the match starts
/// (`HostLobby::finish`), and a closed seat has no forces on the map. With
/// several such seats the lowest is offered; the joiner does not choose.
///
/// ## What becomes of that seat's AI
///
/// **It stops**, on the first turn of the joiner's part, on every peer: the
/// negotiator writes a `joined` order into that turn, as it writes `departed`
/// into the first turn without a departed player, and applying it is
/// `AIStop` (0x00422720) for the seat -- the inverse of the takeover's
/// `AIStart` body. Whatever the computer's units were doing they go on doing
/// until the joiner says otherwise.
///
/// ## The agreement
///
/// Membership is a function of the agreed history, as it is for a
/// departure. Each seat's part in the match is a list of stints
/// (`lockstep.hpp`'s `Stint`); a join opens one on a dropped seat. The
/// coordinator (the host) alone decides, and decides a turn `S` **past
/// every packet it has sent**: `S = next_submit + 1`, and past the turn the
/// seat was dropped from. No peer can run `S` without the host's packet for
/// it, and every datagram that carries that packet carries the admission
/// too, as a link-layer fact beside the drops (`sim/netlink.hpp`) -- so every
/// peer knows the seat is played again before it runs `S`, and before it is
/// handed the joiner's first packet. `S + 1` rather than `S` keeps the
/// joiner's first packet (`S + input_delay`) past any packet the departed
/// player could still have in flight, which then arrives late rather than as
/// a rival.
///
/// The joiner's turns `S .. S + input_delay - 1` are agreed empty, as turns
/// `0 .. input_delay - 1` are for everyone at the start, so it can send its
/// first packet the moment it knows `S`, before it holds any state.
///
/// ## The state
///
/// After applying turn `S - 1`, the host takes `GameSession::save()` and
/// streams it to the joiner: a `JoinHeader` -- the running match's `Start`
/// with the joiner's seat, `S`, the save's size and hash, the stream hash
/// through `S - 1`, every seat's stints and where each speaker's chat
/// numbering stands -- then `JoinChunk`s, acknowledged cumulatively by
/// `JoinAck`. It is a bulk transfer beside the link layer, not inside it,
/// because a save is hundreds of datagrams and the link layer's budget is
/// one.
///
/// The joiner builds its session as every peer did (the same `Start`, so the
/// same `SessionInputs`), loads the save, which checks its own hashes, and
/// starts a negotiator at `S` whose stream hash resumes from the host's
/// value. It hashes like everyone else from its first turn -- world, stream,
/// every channel -- which is what the checks require.
///
/// ## What the others see
///
/// **The match plays on while the save travels**, and waits only when it
/// needs the joiner: the host holds turn `S` until it has taken the save, the
/// joiner's first packet is for `S + input_delay` and is sent as soon as the
/// header arrives, so the first turn anybody waits on the joiner for is
/// `S + input_delay + 1` -- which the joiner can only send once it has run
/// `S`. Every peer knows who is joining and from when (the admission is a
/// fact every node holds); the host alone knows how much of the save has
/// arrived. A joiner that never arrives is timed out like any silent peer,
/// and the computer takes the seat back at the turn that departure agrees.
///
/// One join at a time, and none while a departure is undecided: a second
/// joiner is not answered until the first is playing, and says hello until
/// it is.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "imperivm/core/sim/lockstep.hpp"
#include "imperivm/core/sim/netlink.hpp"
#include "imperivm/core/sim/netlobby.hpp"

namespace imperivm::core::sim {

/// How much of the save one chunk carries: under the link layer's datagram
/// budget, so a chunk never fragments either.
inline constexpr std::size_t kJoinChunk = 1024;

/// What a late joiner is told before the save.
struct JoinHeader {
  std::uint32_t match = 0;
  /// The running match's start, as the joiner is to use it: its own seat in
  /// `you`, the host as its only link, and the speed in force at `from_turn`
  /// in `config.game_speed`.
  Start start;
  /// The first turn the joiner runs: the save is the world after `from_turn
  /// - 1`.
  std::uint32_t from_turn = 0;
  /// `stream_hash` through `from_turn`.
  std::uint64_t netcmds = 0;
  std::uint32_t size = 0;
  /// FNV-1a over the save's bytes.
  std::uint64_t hash = 0;
  std::vector<SeatStints> membership;
  /// Where the host's chat numbering stands for each speaker.
  std::map<PlayerId, std::uint32_t> chat_next;

  /// The negotiator's and the node's starting point.
  [[nodiscard]] ResumePoint resume() const {
    return ResumePoint{from_turn, netcmds, membership};
  }
};

/// One piece of the save.
struct JoinChunk {
  std::uint32_t match = 0;
  std::uint32_t offset = 0;
  std::vector<std::byte> bytes;
};

/// "I hold the header and the save's first `received` bytes."
struct JoinAck {
  std::uint32_t match = 0;
  std::uint32_t received = 0;
};

[[nodiscard]] std::vector<std::byte> encode(const JoinHeader& header);
[[nodiscard]] std::vector<std::byte> encode(const JoinChunk& chunk);
[[nodiscard]] std::vector<std::byte> encode(const JoinAck& ack);
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, JoinHeader& out);
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, JoinChunk& out);
[[nodiscard]] DecodeStatus decode(std::span<const std::byte> bytes, JoinAck& out);

/// FNV-1a, 64 bit, over `bytes`: the save's hash in the header.
[[nodiscard]] std::uint64_t join_hash(std::span<const std::byte> bytes) noexcept;

/// The seat a late joiner would be given now, by the coordinator: the lowest
/// seat whose player has been dropped, while no departure is undecided. None
/// when this peer is not the coordinator. This engine's rule.
[[nodiscard]] std::optional<PlayerId> open_seat(const TurnNegotiator& negotiator,
                                                const LinkNode& node);

/// The coordinator seats a late joiner on `seat`: decides `S` (see the
/// header) and records the admission in the node, which carries it to
/// everyone, and in its own negotiator. The turn, or none if `seat` is not
/// `open_seat`'s.
std::optional<std::uint32_t> admit_joiner(TurnNegotiator& negotiator, LinkNode& node,
                                          PlayerId seat);

/// The header for the joiner admitted on `seat`, once the host has applied
/// turn `S - 1` and taken `save`: the host's own start made the joiner's.
[[nodiscard]] JoinHeader join_header(const TurnNegotiator& negotiator, const LinkNode& node,
                                     const Start& start, PlayerId seat, std::uint32_t match,
                                     std::span<const std::byte> save);

/// Collects the save on the joiner's side: chunks in any order, each once.
class JoinTransfer {
 public:
  explicit JoinTransfer(const JoinHeader& header);
  /// Take a chunk; false if it is not this transfer's or does not fit.
  bool receive(const JoinChunk& chunk);
  /// Bytes held contiguously from the start: what the acknowledgement says.
  [[nodiscard]] std::uint32_t received() const noexcept { return contiguous_; }
  [[nodiscard]] bool complete() const noexcept { return contiguous_ == header_.size; }
  /// Complete, and hashing as the header says.
  [[nodiscard]] bool verified() const noexcept;
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

 private:
  JoinHeader header_;
  std::vector<std::byte> bytes_;
  std::vector<bool> have_;  ///< per chunk
  std::uint32_t contiguous_ = 0;
};

}  // namespace imperivm::core::sim
