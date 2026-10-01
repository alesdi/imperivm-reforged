#include "imperivm/core/sim/netchat.hpp"

#include <algorithm>

#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/save.hpp"

namespace imperivm::core::sim {

std::vector<std::byte> encode(const ChatLine& line) {
  std::vector<std::byte> out;
  for (const char c : {'I', 'M', 'C', 'H'}) bytes::put_u8(out, static_cast<std::uint8_t>(c));
  bytes::put_u8(out, kChatVersion);
  bytes::put_u8(out, line.from);
  bytes::put_u32(out, line.seq);
  bytes::put_u8(out, static_cast<std::uint8_t>(line.to));
  bytes::put_u8(out, line.target);
  bytes::put_u8(out, line.located ? 1u : 0u);
  bytes::put_i32(out, line.location.x);
  bytes::put_i32(out, line.location.y);
  const std::size_t length = std::min(line.text.size(), kMaxChatText);
  bytes::put_u8(out, static_cast<std::uint32_t>(length));
  for (std::size_t i = 0; i < length; ++i) bytes::put_u8(out, static_cast<std::uint8_t>(line.text[i]));
  return out;
}

DecodeStatus decode(std::span<const std::byte> data, ChatLine& out) {
  out = ChatLine{};
  if (data.size() < 4) return DecodeStatus::truncated;
  if (!has_magic(data, "IMCH")) return DecodeStatus::bad_magic;
  ByteReader reader(data, 4);
  std::uint8_t version = 0;
  if (!reader.u8(version)) return DecodeStatus::truncated;
  if (version != kChatVersion) return DecodeStatus::bad_version;
  std::uint8_t from = 0;
  std::uint8_t to = 0;
  std::uint8_t target = 0;
  std::uint8_t located = 0;
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint8_t length = 0;
  if (!reader.u8(from) || !reader.u32(out.seq) || !reader.u8(to) || !reader.u8(target) ||
      !reader.u8(located) || !reader.u32(x) || !reader.u32(y) || !reader.u8(length)) {
    return DecodeStatus::truncated;
  }
  std::span<const std::byte> text;
  if (!reader.bytes(length, text)) return DecodeStatus::truncated;
  if (to > static_cast<std::uint8_t>(ChatLine::To::player) || located > 1 ||
      length > kMaxChatText) {
    return DecodeStatus::bad_field;
  }
  out.from = from;
  out.to = static_cast<ChatLine::To>(to);
  out.target = target;
  out.located = located != 0;
  out.location = Point{static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)};
  out.text.assign(reinterpret_cast<const char*>(text.data()), text.size());
  if (reader.remaining() != 0) return DecodeStatus::trailing;
  return DecodeStatus::ok;
}

bool shown_to(const ChatLine& line, PlayerId viewer, const PlayerTable& players) {
  if (viewer == line.from) return true;
  switch (line.to) {
    case ChatLine::To::all: return true;
    case ChatLine::To::player: return viewer == line.target;
    case ChatLine::To::allies:
      return PlayerTable::is_valid(viewer) && PlayerTable::is_valid(line.from) &&
             players.are_allied(line.from, viewer);
  }
  return false;
}

}  // namespace imperivm::core::sim
