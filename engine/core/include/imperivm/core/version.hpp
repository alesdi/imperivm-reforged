#pragma once

namespace imperivm::core {

inline constexpr int version_major = 0;
inline constexpr int version_minor = 1;
inline constexpr int version_patch = 0;

/// Returns "major.minor.patch". Present mostly so that the freestanding core
/// has something to link against before the real subsystems land.
const char* version_string() noexcept;

}  // namespace imperivm::core
