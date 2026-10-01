#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace imperivm::platform {

/// A read-only view of bytes the platform owns and the core borrows.
///
/// The architecture rule is that core takes spans and never learns where they
/// came from (docs/engine/architecture.md). This alias is the shape of that
/// hand-off; the platform side is the only side that knows whether the memory
/// behind it is a mapping, a heap buffer, or a slice of a mapped pack.
using ByteSpan = std::span<const std::uint8_t>;

/// The core readers spell bytes `std::byte`; anything that touches pixels wants
/// `std::uint8_t`. Neither is worth converting the other to, so the two
/// spellings meet here and nowhere else.
inline std::span<const std::byte> as_core_bytes(ByteSpan bytes) noexcept {
  return {reinterpret_cast<const std::byte*>(bytes.data()), bytes.size()};
}

inline ByteSpan as_platform_bytes(std::span<const std::byte> bytes) noexcept {
  return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}


}  // namespace imperivm::platform
