#include "imperivm/core/sim/netlobby.hpp"

#include <algorithm>

#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/save.hpp"

namespace imperivm::core::sim {
namespace {

void put_magic(std::vector<std::byte>& out, const char* magic) {
  for (int i = 0; i < 4; ++i) bytes::put_u8(out, static_cast<std::uint8_t>(magic[i]));
}

/// The magic and the protocol version every lobby message opens with. The
/// version is checked by the receiver, not only reported: a message from
/// another build is not decoded, because its layout is not ours.
DecodeStatus open(std::span<const std::byte> data, const char* magic, ByteReader& reader) {
  if (data.size() < 4) return DecodeStatus::truncated;
  if (!has_magic(data, magic)) return DecodeStatus::bad_magic;
  reader.seek(4);
  std::uint32_t protocol = 0;
  if (!reader.u32(protocol)) return DecodeStatus::truncated;
  if (protocol != kNetProtocol) return DecodeStatus::bad_version;
  return DecodeStatus::ok;
}

bool get_u64(ByteReader& reader, std::uint64_t& out) {
  std::uint32_t low = 0;
  std::uint32_t high = 0;
  if (!reader.u32(low) || !reader.u32(high)) return false;
  out = (static_cast<std::uint64_t>(high) << 32) | low;
  return true;
}

bool get_i32(ByteReader& reader, std::int32_t& out) {
  std::uint32_t raw = 0;
  if (!reader.u32(raw)) return false;
  out = static_cast<std::int32_t>(raw);
  return true;
}

/// A boolean byte. Anything but 0 or 1 marks the message bad rather than
/// failing the read here, so that a truncation is still reported as one.
bool get_bool(ByteReader& reader, bool& out, bool& bad) {
  std::uint8_t raw = 0;
  if (!reader.u8(raw)) return false;
  if (raw > 1) bad = true;
  out = raw != 0;
  return true;
}

void put_players(std::vector<std::byte>& out, const std::vector<PlayerId>& players) {
  bytes::put_u8(out, static_cast<std::uint32_t>(players.size()));
  for (const PlayerId player : players) bytes::put_u8(out, player);
}

bool get_players(ByteReader& reader, std::vector<PlayerId>& out) {
  std::uint8_t count = 0;
  if (!reader.u8(count)) return false;
  out.resize(count);
  for (PlayerId& player : out) {
    if (!reader.u8(player)) return false;
  }
  return true;
}

/// Text in the lobby is names and paths for a human to read; the caps bound
/// what a stranger can make the other side store.
constexpr std::uint32_t kMaxText = 64;
constexpr std::uint32_t kMaxPath = 260;

void put_text(std::vector<std::byte>& out, std::string_view text, std::uint32_t cap) {
  bytes::put_string(out, text.substr(0, cap));
}

/// A u32 length and its bytes, refused past `cap` rather than cut.
DecodeStatus get_text(ByteReader& reader, std::string& out, std::uint32_t cap) {
  std::uint32_t length = 0;
  if (!reader.u32(length)) return DecodeStatus::truncated;
  if (length > cap) return DecodeStatus::bad_field;
  std::span<const std::byte> text;
  if (!reader.bytes(length, text)) return DecodeStatus::truncated;
  out.assign(reinterpret_cast<const char*>(text.data()), text.size());
  return DecodeStatus::ok;
}

constexpr std::size_t kMaxRows = 16;

/// Chat lines inside a lobby message: a count, then each line's own
/// encoding behind its length. At most `kLobbyChatLines`.
void put_chat(std::vector<std::byte>& out, const std::vector<ChatLine>& lines) {
  const std::size_t first = lines.size() > kLobbyChatLines ? lines.size() - kLobbyChatLines : 0;
  bytes::put_u8(out, static_cast<std::uint32_t>(lines.size() - first));
  for (std::size_t i = first; i < lines.size(); ++i) {
    const std::vector<std::byte> line = encode(lines[i]);
    bytes::put_u32(out, static_cast<std::uint32_t>(line.size()));
    out.insert(out.end(), line.begin(), line.end());
  }
}

DecodeStatus get_chat(ByteReader& reader, std::vector<ChatLine>& out) {
  std::uint8_t count = 0;
  if (!reader.u8(count)) return DecodeStatus::truncated;
  if (count > kLobbyChatLines) return DecodeStatus::bad_field;
  out.resize(count);
  for (ChatLine& line : out) {
    std::uint32_t length = 0;
    std::span<const std::byte> body;
    if (!reader.u32(length) || !reader.bytes(length, body)) return DecodeStatus::truncated;
    const DecodeStatus status = decode(body, line);
    if (status != DecodeStatus::ok) return status == DecodeStatus::truncated ? DecodeStatus::bad_field : status;
  }
  return DecodeStatus::ok;
}

}  // namespace

const char* describe(Refuse::Reason reason) noexcept {
  switch (reason) {
    case Refuse::Reason::protocol: return "a different build";
    case Refuse::Reason::content: return "a different game: data.pak differs";
    case Refuse::Reason::full: return "the game is full";
    case Refuse::Reason::closed: return "the host closed the game";
    case Refuse::Reason::left: return "a player left";
    case Refuse::Reason::map: return "the host's map is not installed here, or differs";
    case Refuse::Reason::silent: return "stopped answering";
  }
  return "?";
}

LobbyKind lobby_kind(std::span<const std::byte> data) noexcept {
  if (has_magic(data, "IMLH")) return LobbyKind::hello;
  if (has_magic(data, "IMLO")) return LobbyKind::roster;
  if (has_magic(data, "IMLS")) return LobbyKind::start;
  if (has_magic(data, "IMLC")) return LobbyKind::choice;
  if (has_magic(data, "IMLR")) return LobbyKind::refuse;
  if (has_magic(data, "IMLQ")) return LobbyKind::query;
  if (has_magic(data, "IMLA")) return LobbyKind::advert;
  if (has_magic(data, "IMLJ")) return LobbyKind::join_header;
  if (has_magic(data, "IMLB")) return LobbyKind::join_chunk;
  if (has_magic(data, "IMLK")) return LobbyKind::join_ack;
  return LobbyKind::none;
}

// -- hello --------------------------------------------------------------------

std::vector<std::byte> encode(const Hello& hello) {
  std::vector<std::byte> out;
  put_magic(out, "IMLH");
  bytes::put_u32(out, hello.protocol);
  bytes::put_u64(out, hello.content);
  put_text(out, hello.name, kMaxText);
  return out;
}

DecodeStatus decode(std::span<const std::byte> data, Hello& out) {
  out = Hello{};
  ByteReader reader(data);
  if (const DecodeStatus status = open(data, "IMLH", reader); status != DecodeStatus::ok) {
    return status;
  }
  if (!get_u64(reader, out.content)) return DecodeStatus::truncated;
  if (const DecodeStatus status = get_text(reader, out.name, kMaxText); status != DecodeStatus::ok) {
    return status;
  }
  if (reader.remaining() != 0) return DecodeStatus::trailing;
  return DecodeStatus::ok;
}

// -- roster and start ---------------------------------------------------------

std::vector<std::byte> encode(const Start& start, bool final) {
  std::vector<std::byte> out;
  put_magic(out, final ? "IMLS" : "IMLO");
  bytes::put_u32(out, kNetProtocol);
  bytes::put_u8(out, start.you);
  put_text(out, start.map, kMaxPath);
  bytes::put_u64(out, start.map_hash);
  bytes::put_u32(out, start.map_index);
  bytes::put_u32(out, start.difficulty);
  const std::size_t rows = std::min(start.rows.size(), kMaxRows);
  bytes::put_u8(out, static_cast<std::uint32_t>(rows));
  for (std::size_t i = 0; i < rows; ++i) {
    const SeatRow& row = start.rows[i];
    bytes::put_u8(out, row.slot);
    bytes::put_u8(out, static_cast<std::uint8_t>(row.type));
    put_text(out, row.name, kMaxText);
    bytes::put_i32(out, row.race);
    bytes::put_u8(out, row.difficulty);
    bytes::put_u8(out, row.team);
    bytes::put_u8(out, row.bonus);
    bytes::put_u8(out, row.ready ? 1u : 0u);
  }
  const LobbyRules& r = start.rules;
  put_text(out, r.victory, kMaxText);
  put_text(out, r.threshold, kMaxText);
  bytes::put_i32(out, r.world_population);
  bytes::put_i32(out, r.starting_gold);
  bytes::put_u8(out, r.fog_of_war ? 1u : 0u);
  bytes::put_u8(out, r.exploration ? 1u : 0u);
  bytes::put_u8(out, r.bonuses ? 1u : 0u);
  bytes::put_u8(out, r.shared_support ? 1u : 0u);
  bytes::put_u8(out, r.shared_control ? 1u : 0u);
  put_players(out, start.config.peers);
  bytes::put_u32(out, start.config.input_delay);
  bytes::put_i32(out, start.config.initial_ms);
  bytes::put_i32(out, start.config.min_ms);
  bytes::put_i32(out, start.config.max_ms);
  bytes::put_i32(out, start.config.game_speed);
  bytes::put_u8(out, start.config.coordinator);
  bytes::put_u8(out, start.config.variable_speed ? 1u : 0u);
  bytes::put_u32(out, start.match);
  bytes::put_u32(out, start.seed);
  bytes::put_u32(out, start.turns);
  put_players(out, start.links);
  bytes::put_u32(out, start.chat_heard);
  put_chat(out, start.chat);
  return out;
}

DecodeStatus decode(std::span<const std::byte> data, Start& out) {
  out = Start{};
  const bool roster = lobby_kind(data) == LobbyKind::roster;
  ByteReader reader(data);
  if (const DecodeStatus status = open(data, roster ? "IMLO" : "IMLS", reader);
      status != DecodeStatus::ok) {
    return status;
  }
  bool bad = false;
  std::uint8_t you = 0;
  if (!reader.u8(you)) return DecodeStatus::truncated;
  out.you = you;
  if (const DecodeStatus status = get_text(reader, out.map, kMaxPath); status != DecodeStatus::ok) {
    return status;
  }
  std::uint8_t rows = 0;
  if (!get_u64(reader, out.map_hash) || !reader.u32(out.map_index) ||
      !reader.u32(out.difficulty) || !reader.u8(rows)) {
    return DecodeStatus::truncated;
  }
  if (rows > kMaxRows) return DecodeStatus::bad_field;
  out.rows.resize(rows);
  for (SeatRow& row : out.rows) {
    std::uint8_t slot = 0;
    std::uint8_t type = 0;
    if (!reader.u8(slot) || !reader.u8(type)) return DecodeStatus::truncated;
    row.slot = slot;
    if (type > static_cast<std::uint8_t>(SeatRow::Type::open)) bad = true;
    row.type = static_cast<SeatRow::Type>(type);
    if (const DecodeStatus status = get_text(reader, row.name, kMaxText);
        status != DecodeStatus::ok) {
      return status;
    }
    if (!get_i32(reader, row.race) || !reader.u8(row.difficulty) || !reader.u8(row.team) ||
        !reader.u8(row.bonus) || !get_bool(reader, row.ready, bad)) {
      return DecodeStatus::truncated;
    }
    if (row.slot >= kPlayerCount || row.race < kNoRace || row.race >= kRaceCount ||
        row.difficulty > 2 || row.team > 4 || row.bonus > 3) {
      bad = true;
    }
    // An open seat is a lobby's; a match is never started with one.
    if (!roster && row.type == SeatRow::Type::open) bad = true;
  }
  LobbyRules& r = out.rules;
  if (const DecodeStatus status = get_text(reader, r.victory, kMaxText); status != DecodeStatus::ok) {
    return status;
  }
  if (const DecodeStatus status = get_text(reader, r.threshold, kMaxText);
      status != DecodeStatus::ok) {
    return status;
  }
  if (!get_i32(reader, r.world_population) || !get_i32(reader, r.starting_gold) ||
      !get_bool(reader, r.fog_of_war, bad) || !get_bool(reader, r.exploration, bad) ||
      !get_bool(reader, r.bonuses, bad) || !get_bool(reader, r.shared_support, bad) ||
      !get_bool(reader, r.shared_control, bad) || !get_players(reader, out.config.peers) ||
      !reader.u32(out.config.input_delay) || !get_i32(reader, out.config.initial_ms) ||
      !get_i32(reader, out.config.min_ms) || !get_i32(reader, out.config.max_ms) ||
      !get_i32(reader, out.config.game_speed) || !reader.u8(out.config.coordinator) ||
      !get_bool(reader, out.config.variable_speed, bad) ||
      !reader.u32(out.match) ||
      !reader.u32(out.seed) || !reader.u32(out.turns) || !get_players(reader, out.links) ||
      !reader.u32(out.chat_heard)) {
    return DecodeStatus::truncated;
  }
  if (const DecodeStatus status = get_chat(reader, out.chat); status != DecodeStatus::ok) {
    return status;
  }
  const LockstepConfig& c = out.config;
  // The coordinator is somebody in the match, or nobody (the lowest peer).
  const bool coordinator_ok =
      c.coordinator == kNoPlayer ||
      std::find(c.peers.begin(), c.peers.end(), c.coordinator) != c.peers.end();
  if (bad || c.initial_ms <= 0 || c.min_ms <= 0 || c.max_ms < c.min_ms || c.game_speed <= 0 ||
      out.difficulty > 2 || (!roster && c.peers.empty()) || !coordinator_ok) {
    return DecodeStatus::bad_field;
  }
  if (reader.remaining() != 0) return DecodeStatus::trailing;
  return DecodeStatus::ok;
}

// -- choice -------------------------------------------------------------------

std::vector<std::byte> encode(const Choice& choice) {
  std::vector<std::byte> out;
  put_magic(out, "IMLC");
  bytes::put_u32(out, kNetProtocol);
  bytes::put_i32(out, choice.race);
  bytes::put_u8(out, choice.team);
  bytes::put_u8(out, choice.bonus);
  bytes::put_u8(out, choice.ready ? 1u : 0u);
  put_chat(out, choice.chat);
  return out;
}

DecodeStatus decode(std::span<const std::byte> data, Choice& out) {
  out = Choice{};
  ByteReader reader(data);
  if (const DecodeStatus status = open(data, "IMLC", reader); status != DecodeStatus::ok) {
    return status;
  }
  bool bad = false;
  if (!get_i32(reader, out.race) || !reader.u8(out.team) || !reader.u8(out.bonus) ||
      !get_bool(reader, out.ready, bad)) {
    return DecodeStatus::truncated;
  }
  if (const DecodeStatus status = get_chat(reader, out.chat); status != DecodeStatus::ok) {
    return status;
  }
  if (bad) return DecodeStatus::bad_field;
  if (reader.remaining() != 0) return DecodeStatus::trailing;
  return DecodeStatus::ok;
}

bool apply_choice(SeatRow& row, const Choice& choice) noexcept {
  const SeatRow before = row;
  row.race = choice.race >= 0 && choice.race < kRaceCount ? choice.race : kNoRace;
  if (choice.team <= 4) row.team = choice.team;
  if (choice.bonus <= 3) row.bonus = choice.bonus;
  row.ready = choice.ready;
  return row.race != before.race || row.team != before.team || row.bonus != before.bonus ||
         row.ready != before.ready;
}

// -- refuse -------------------------------------------------------------------

std::vector<std::byte> encode(const Refuse& refuse) {
  std::vector<std::byte> out;
  put_magic(out, "IMLR");
  bytes::put_u32(out, kNetProtocol);
  bytes::put_u8(out, static_cast<std::uint8_t>(refuse.reason));
  return out;
}

DecodeStatus decode(std::span<const std::byte> data, Refuse& out) {
  out = Refuse{};
  // A refusal is read whatever the sender's version, because "you are a
  // different build" is exactly the refusal a different build has to be able
  // to read. So the version is skipped here, not checked.
  if (data.size() < 4) return DecodeStatus::truncated;
  if (!has_magic(data, "IMLR")) return DecodeStatus::bad_magic;
  ByteReader reader(data, 4);
  std::uint32_t protocol = 0;
  std::uint8_t reason = 0;
  if (!reader.u32(protocol) || !reader.u8(reason)) return DecodeStatus::truncated;
  if (reason > static_cast<std::uint8_t>(Refuse::Reason::silent)) return DecodeStatus::bad_field;
  out.reason = static_cast<Refuse::Reason>(reason);
  if (reader.remaining() != 0) return DecodeStatus::trailing;
  return DecodeStatus::ok;
}

// -- query and advert ---------------------------------------------------------

std::vector<std::byte> encode(const Query& query) {
  std::vector<std::byte> out;
  put_magic(out, "IMLQ");
  bytes::put_u32(out, query.protocol);
  return out;
}

DecodeStatus decode(std::span<const std::byte> data, Query& out) {
  out = Query{};
  ByteReader reader(data);
  if (const DecodeStatus status = open(data, "IMLQ", reader); status != DecodeStatus::ok) {
    return status;
  }
  if (reader.remaining() != 0) return DecodeStatus::trailing;
  return DecodeStatus::ok;
}

std::vector<std::byte> encode(const Advert& advert) {
  std::vector<std::byte> out;
  put_magic(out, "IMLA");
  bytes::put_u32(out, kNetProtocol);
  put_text(out, advert.host, kMaxText);
  put_text(out, advert.map, kMaxPath);
  bytes::put_u16(out, advert.port);
  bytes::put_u8(out, advert.open);
  bytes::put_u8(out, advert.seats);
  return out;
}

DecodeStatus decode(std::span<const std::byte> data, Advert& out) {
  out = Advert{};
  ByteReader reader(data);
  if (const DecodeStatus status = open(data, "IMLA", reader); status != DecodeStatus::ok) {
    return status;
  }
  if (const DecodeStatus status = get_text(reader, out.host, kMaxText); status != DecodeStatus::ok) {
    return status;
  }
  if (const DecodeStatus status = get_text(reader, out.map, kMaxPath); status != DecodeStatus::ok) {
    return status;
  }
  if (!reader.u16(out.port) || !reader.u8(out.open) || !reader.u8(out.seats)) {
    return DecodeStatus::truncated;
  }
  if (out.open > out.seats) return DecodeStatus::bad_field;
  if (reader.remaining() != 0) return DecodeStatus::trailing;
  return DecodeStatus::ok;
}

}  // namespace imperivm::core::sim
