#pragma once

// Bounds-checked little-endian reads over a byte span.
//
// Every integer in every HMMSYS format is little-endian and unsigned, and every
// one of them is attacker-controlled as far as the engine is concerned. These
// helpers exist so that no reader ever indexes a span by hand: the checked
// accessors are as short to write as the unchecked ones, which is the only way
// bounds checking survives contact with a large parser.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace imperivm::core {

/// True when `[offset, offset + length)` lies inside `data`, computed without
/// overflowing: `offset + length` on 32-bit values from a file can wrap.
constexpr bool span_contains(std::span<const std::byte> data, std::uint64_t offset,
                             std::uint64_t length) noexcept {
  const std::uint64_t size = static_cast<std::uint64_t>(data.size());
  return offset <= size && length <= size - offset;
}

constexpr std::uint8_t read_u8(std::span<const std::byte> data, std::size_t offset) noexcept {
  return static_cast<std::uint8_t>(data[offset]);
}

constexpr std::uint16_t read_u16le(std::span<const std::byte> data, std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(read_u8(data, offset) |
                                    (static_cast<std::uint16_t>(read_u8(data, offset + 1)) << 8));
}

constexpr std::uint32_t read_u32le(std::span<const std::byte> data, std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(read_u8(data, offset)) |
         (static_cast<std::uint32_t>(read_u8(data, offset + 1)) << 8) |
         (static_cast<std::uint32_t>(read_u8(data, offset + 2)) << 16) |
         (static_cast<std::uint32_t>(read_u8(data, offset + 3)) << 24);
}

/// Compare the first `text.size()` bytes of `data` against an ASCII magic.
constexpr bool has_magic(std::span<const std::byte> data, std::string_view text) noexcept {
  if (data.size() < text.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (static_cast<char>(data[i]) != text[i]) return false;
  }
  return true;
}

/// A forward cursor over a span. Reads that would run off the end fail rather
/// than reading adjacent memory, and leave the cursor where it was.
class ByteReader {
 public:
  constexpr explicit ByteReader(std::span<const std::byte> data, std::size_t offset = 0) noexcept
      : data_(data), position_(offset) {}

  constexpr std::size_t position() const noexcept { return position_; }
  constexpr std::size_t remaining() const noexcept {
    return position_ <= data_.size() ? data_.size() - position_ : 0;
  }
  constexpr bool has(std::size_t count) const noexcept { return remaining() >= count; }

  constexpr void seek(std::size_t offset) noexcept { position_ = offset; }
  constexpr bool skip(std::size_t count) noexcept {
    if (!has(count)) return false;
    position_ += count;
    return true;
  }

  constexpr bool u8(std::uint8_t& out) noexcept {
    if (!has(1)) return false;
    out = read_u8(data_, position_);
    position_ += 1;
    return true;
  }

  constexpr bool u16(std::uint16_t& out) noexcept {
    if (!has(2)) return false;
    out = read_u16le(data_, position_);
    position_ += 2;
    return true;
  }

  constexpr bool u32(std::uint32_t& out) noexcept {
    if (!has(4)) return false;
    out = read_u32le(data_, position_);
    position_ += 4;
    return true;
  }

  /// Take `count` bytes as a subspan, or fail without moving.
  constexpr bool bytes(std::size_t count, std::span<const std::byte>& out) noexcept {
    if (!has(count)) return false;
    out = data_.subspan(position_, count);
    position_ += count;
    return true;
  }

 private:
  std::span<const std::byte> data_;
  std::size_t position_ = 0;
};

}  // namespace imperivm::core
