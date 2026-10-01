#pragma once

// A little-endian byte builder for the format tests.
//
// The tests construct their inputs rather than shipping fixtures, because CI
// has no game installation and this project must never carry game assets. A
// hand-built buffer is also the only way to test the cases that matter most:
// the malformed ones, which by definition do not exist in the retail data.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace imperivm::test {

class Builder {
 public:
  Builder& u8(std::uint32_t value) {
    bytes_.push_back(static_cast<std::byte>(value & 0xFF));
    return *this;
  }
  Builder& u16(std::uint32_t value) { return u8(value).u8(value >> 8); }
  Builder& u32(std::uint32_t value) { return u16(value).u16(value >> 16); }

  Builder& text(std::string_view value) {
    for (const char c : value) u8(static_cast<std::uint8_t>(c));
    return *this;
  }
  Builder& zeros(std::size_t count) {
    bytes_.insert(bytes_.end(), count, std::byte{0});
    return *this;
  }
  Builder& fill(std::size_t count, std::uint8_t value) {
    bytes_.insert(bytes_.end(), count, static_cast<std::byte>(value));
    return *this;
  }
  /// Pad out to `offset`, so a test can describe a layout by its offsets.
  Builder& pad_to(std::size_t offset) {
    while (bytes_.size() < offset) bytes_.push_back(std::byte{0});
    return *this;
  }

  void patch_u32(std::size_t at, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      bytes_[at++] = static_cast<std::byte>((value >> shift) & 0xFF);
    }
  }
  void patch_u8(std::size_t at, std::uint32_t value) {
    bytes_[at] = static_cast<std::byte>(value & 0xFF);
  }

  std::size_t size() const { return bytes_.size(); }
  std::span<const std::byte> span() const { return bytes_; }
  std::vector<std::byte>& raw() { return bytes_; }

 private:
  std::vector<std::byte> bytes_;
};

}  // namespace imperivm::test
