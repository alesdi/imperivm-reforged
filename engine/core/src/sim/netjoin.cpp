#include "imperivm/core/sim/netjoin.hpp"

#include <algorithm>

#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/sim/save.hpp"

namespace imperivm::core::sim {
namespace {

/// A save is a few hundred kilobytes (`imsave` on Danube at 200 turns is
/// 369 KB); a header promising more than this is refused before anything is
/// allocated for it.
constexpr std::uint32_t kMaxJoinSave = 64u << 20;
constexpr std::size_t kMaxStints = 64;

void put_magic(std::vector<std::byte>& out, const char* magic) {
  for (int i = 0; i < 4; ++i) bytes::put_u8(out, static_cast<std::uint8_t>(magic[i]));
}

DecodeStatus open(std::span<const std::byte> data, const char* magic, ByteReader& reader) {
  if (data.size() < 4) return DecodeStatus::truncated;
  if (!has_magic(data, magic)) return DecodeStatus::bad_magic;
  reader.seek(4);
  std::uint32_t protocol = 0;
  if (!reader.u32(protocol)) return DecodeStatus::truncated;
  if (protocol != kNetProtocol) return DecodeStatus::bad_version;
  return DecodeStatus::ok;
}

/// Stints as a seat can have them: the first from turn 0, each after the
/// last one's end, only the last one open.
bool well_formed(const std::vector<Stint>& stints) {
  if (stints.empty() || stints.front().from != 0) return false;
  for (std::size_t i = 0; i < stints.size(); ++i) {
    const Stint& stint = stints[i];
    if (i + 1 < stints.size()) {
      if (!stint.until.has_value() || stints[i + 1].from <= *stint.until) return false;
    }
    if (stint.until.has_value() && *stint.until < stint.from) return false;
  }
  return true;
}

}  // namespace

std::uint64_t join_hash(std::span<const std::byte> data) noexcept {
  std::uint64_t state = 1469598103934665603ULL;
  for (const std::byte b : data) {
    state ^= static_cast<std::uint64_t>(b);
    state *= 1099511628211ULL;
  }
  return state;
}

// --------------------------------------------------------------------------
// the messages
// --------------------------------------------------------------------------

std::vector<std::byte> encode(const JoinHeader& header) {
  std::vector<std::byte> out;
  put_magic(out, "IMLJ");
  bytes::put_u32(out, kNetProtocol);
  bytes::put_u32(out, header.match);
  bytes::put_u32(out, header.from_turn);
  bytes::put_u64(out, header.netcmds);
  bytes::put_u32(out, header.size);
  bytes::put_u64(out, header.hash);
  const std::vector<std::byte> start = encode(header.start, /*final=*/true);
  bytes::put_u32(out, static_cast<std::uint32_t>(start.size()));
  out.insert(out.end(), start.begin(), start.end());
  bytes::put_u8(out, static_cast<std::uint32_t>(std::min<std::size_t>(header.membership.size(), 0xFF)));
  for (std::size_t i = 0; i < header.membership.size() && i < 0xFF; ++i) {
    const SeatStints& seat = header.membership[i];
    bytes::put_u8(out, seat.peer);
    const std::size_t count = std::min(seat.stints.size(), kMaxStints);
    bytes::put_u8(out, static_cast<std::uint32_t>(count));
    for (std::size_t k = 0; k < count; ++k) {
      bytes::put_u32(out, seat.stints[k].from);
      bytes::put_u8(out, seat.stints[k].until.has_value() ? 1u : 0u);
      bytes::put_u32(out, seat.stints[k].until.value_or(0));
    }
  }
  bytes::put_u8(out, static_cast<std::uint32_t>(std::min<std::size_t>(header.chat_next.size(), 0xFF)));
  std::size_t written = 0;
  for (const auto& [speaker, next] : header.chat_next) {
    if (written++ == 0xFF) break;
    bytes::put_u8(out, speaker);
    bytes::put_u32(out, next);
  }
  return out;
}

DecodeStatus decode(std::span<const std::byte> data, JoinHeader& out) {
  out = JoinHeader{};
  ByteReader reader(data);
  if (const DecodeStatus status = open(data, "IMLJ", reader); status != DecodeStatus::ok) {
    return status;
  }
  std::uint32_t start_size = 0;
  std::span<const std::byte> start;
  if (!reader.u32(out.match) || !reader.u32(out.from_turn) || !bytes::get_u64(reader, out.netcmds) ||
      !reader.u32(out.size) || !bytes::get_u64(reader, out.hash) || !reader.u32(start_size) ||
      !reader.bytes(start_size, start)) {
    return DecodeStatus::truncated;
  }
  if (lobby_kind(start) != LobbyKind::start) return DecodeStatus::bad_field;
  if (const DecodeStatus status = decode(start, out.start); status != DecodeStatus::ok) {
    return status == DecodeStatus::truncated ? DecodeStatus::bad_field : status;
  }
  std::uint8_t seats = 0;
  if (!reader.u8(seats)) return DecodeStatus::truncated;
  out.membership.resize(seats);
  for (SeatStints& seat : out.membership) {
    std::uint8_t peer = 0;
    std::uint8_t count = 0;
    if (!reader.u8(peer) || !reader.u8(count)) return DecodeStatus::truncated;
    if (count > kMaxStints) return DecodeStatus::bad_field;
    seat.peer = peer;
    seat.stints.resize(count);
    for (Stint& stint : seat.stints) {
      std::uint8_t has_until = 0;
      std::uint32_t until = 0;
      if (!reader.u32(stint.from) || !reader.u8(has_until) || !reader.u32(until)) {
        return DecodeStatus::truncated;
      }
      if (has_until > 1) return DecodeStatus::bad_field;
      if (has_until == 1) stint.until = until;
    }
  }
  std::uint8_t speakers = 0;
  if (!reader.u8(speakers)) return DecodeStatus::truncated;
  for (std::uint8_t i = 0; i < speakers; ++i) {
    std::uint8_t speaker = 0;
    std::uint32_t next = 0;
    if (!reader.u8(speaker) || !reader.u32(next)) return DecodeStatus::truncated;
    out.chat_next[speaker] = next;
  }
  if (reader.remaining() != 0) return DecodeStatus::trailing;

  // What a joiner is about to build a match from: every seat a peer of the
  // match with a history a seat can have, and its own seat's last stint
  // opening at the turn it starts.
  const std::vector<PlayerId>& peers = out.start.config.peers;
  bool own = false;
  for (const SeatStints& seat : out.membership) {
    if (std::find(peers.begin(), peers.end(), seat.peer) == peers.end() ||
        !well_formed(seat.stints)) {
      return DecodeStatus::bad_field;
    }
    if (seat.peer == out.start.you) {
      own = seat.stints.back().from == out.from_turn && !seat.stints.back().until.has_value();
    }
  }
  if (!own || out.from_turn == 0 || out.size == 0 || out.size > kMaxJoinSave) {
    return DecodeStatus::bad_field;
  }
  return DecodeStatus::ok;
}

std::vector<std::byte> encode(const JoinChunk& chunk) {
  std::vector<std::byte> out;
  put_magic(out, "IMLB");
  bytes::put_u32(out, kNetProtocol);
  bytes::put_u32(out, chunk.match);
  bytes::put_u32(out, chunk.offset);
  bytes::put_u16(out, static_cast<std::uint32_t>(chunk.bytes.size()));
  out.insert(out.end(), chunk.bytes.begin(), chunk.bytes.end());
  return out;
}

DecodeStatus decode(std::span<const std::byte> data, JoinChunk& out) {
  out = JoinChunk{};
  ByteReader reader(data);
  if (const DecodeStatus status = open(data, "IMLB", reader); status != DecodeStatus::ok) {
    return status;
  }
  std::uint16_t length = 0;
  std::span<const std::byte> body;
  if (!reader.u32(out.match) || !reader.u32(out.offset) || !reader.u16(length) ||
      !reader.bytes(length, body)) {
    return DecodeStatus::truncated;
  }
  if (length == 0 || length > kJoinChunk || out.offset % kJoinChunk != 0) {
    return DecodeStatus::bad_field;
  }
  out.bytes.assign(body.begin(), body.end());
  if (reader.remaining() != 0) return DecodeStatus::trailing;
  return DecodeStatus::ok;
}

std::vector<std::byte> encode(const JoinAck& ack) {
  std::vector<std::byte> out;
  put_magic(out, "IMLK");
  bytes::put_u32(out, kNetProtocol);
  bytes::put_u32(out, ack.match);
  bytes::put_u32(out, ack.received);
  return out;
}

DecodeStatus decode(std::span<const std::byte> data, JoinAck& out) {
  out = JoinAck{};
  ByteReader reader(data);
  if (const DecodeStatus status = open(data, "IMLK", reader); status != DecodeStatus::ok) {
    return status;
  }
  if (!reader.u32(out.match) || !reader.u32(out.received)) return DecodeStatus::truncated;
  if (reader.remaining() != 0) return DecodeStatus::trailing;
  return DecodeStatus::ok;
}

// --------------------------------------------------------------------------
// the coordinator's decisions
// --------------------------------------------------------------------------

std::optional<PlayerId> open_seat(const TurnNegotiator& negotiator, const LinkNode& node) {
  const PlayerId self = negotiator.local();
  if (negotiator.config().resolved_coordinator() != self) return std::nullopt;
  // No departure undecided: its report would be asked of the joiner too, and
  // the seat it frees is not yet the computer's.
  const std::map<PlayerId, std::uint32_t> drops = node.drops();
  for (const PlayerId gone : node.departed()) {
    if (drops.count(gone) == 0) return std::nullopt;
  }
  for (const PlayerId peer : negotiator.config().peers) {
    if (peer == self) continue;
    // Dropped here and in the node alike: the decision is applied.
    if (negotiator.dropped_from(peer).has_value() && drops.count(peer) != 0) return peer;
  }
  return std::nullopt;
}

std::optional<std::uint32_t> admit_joiner(TurnNegotiator& negotiator, LinkNode& node,
                                          PlayerId seat) {
  if (open_seat(negotiator, node) != seat) return std::nullopt;
  const std::optional<std::uint32_t> until = negotiator.dropped_from(seat);
  if (!until.has_value()) return std::nullopt;
  // Past every packet this host has sent (so nobody has run it and every
  // peer learns of it with the host's packet for it) and one more, past any
  // packet the departed player could still have in flight; and after the
  // turn the seat was dropped from.
  const std::uint32_t from = std::max(negotiator.next_submit() + 1, *until + 1);
  node.admit(seat, from);
  if (!negotiator.admit(seat, from)) return std::nullopt;
  return from;
}

JoinHeader join_header(const TurnNegotiator& negotiator, const LinkNode& node, const Start& start,
                       PlayerId seat, std::uint32_t match, std::span<const std::byte> save) {
  JoinHeader header;
  header.match = match;
  header.start = start;
  header.start.you = seat;
  header.start.links = {negotiator.local()};
  header.start.config = negotiator.config();
  header.start.chat.clear();
  header.start.chat_heard = 0;
  header.from_turn = negotiator.next_turn();
  header.netcmds = stream_hash(negotiator.history(), negotiator.next_turn());
  header.size = static_cast<std::uint32_t>(save.size());
  header.hash = join_hash(save);
  header.membership = negotiator.membership();
  header.chat_next = node.chat_next();
  return header;
}

// --------------------------------------------------------------------------
// the joiner's side of the transfer
// --------------------------------------------------------------------------

JoinTransfer::JoinTransfer(const JoinHeader& header)
    : header_(header),
      bytes_(header.size),
      have_((header.size + kJoinChunk - 1) / kJoinChunk, false) {}

bool JoinTransfer::receive(const JoinChunk& chunk) {
  if (chunk.match != header_.match || chunk.offset % kJoinChunk != 0) return false;
  const std::size_t index = chunk.offset / kJoinChunk;
  if (index >= have_.size()) return false;
  // Every chunk is full but the last.
  const std::size_t expected = std::min<std::size_t>(kJoinChunk, header_.size - chunk.offset);
  if (chunk.bytes.size() != expected) return false;
  if (have_[index]) return true;
  std::copy(chunk.bytes.begin(), chunk.bytes.end(), bytes_.begin() + chunk.offset);
  have_[index] = true;
  while (contiguous_ < header_.size && have_[contiguous_ / kJoinChunk]) {
    contiguous_ = static_cast<std::uint32_t>(
        std::min<std::size_t>(contiguous_ + kJoinChunk, header_.size));
  }
  return true;
}

bool JoinTransfer::verified() const noexcept {
  return complete() && join_hash(bytes_) == header_.hash;
}

}  // namespace imperivm::core::sim
