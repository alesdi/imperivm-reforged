#pragma once

// Error handling for the format readers.
//
// Game files are untrusted input: every offset, length and count in them comes
// from the file itself, and a corrupt archive is a normal thing to meet, not an
// exceptional one. So malformed data is reported as a *value*, never thrown.
// Two further reasons the core does it this way:
//
//   * the conformance harness must behave identically on every target, and
//     -fno-exceptions builds (WebAssembly, consoles) are ordinary here;
//   * an exception unwinding out of a decoder and across the engine would take
//     the process down for what is really a bad byte in a sprite.
//
// One convention throughout, deliberately not mixed with out-parameters:
// every fallible function returns Result<T>, and Result<void> when it has
// nothing to hand back.

#include <cstdint>
#include <utility>

namespace imperivm::core {

/// Why a format reader refused its input. Deliberately coarse: callers act on
/// "this file is unusable", and a reader that needs to explain more than this
/// can say so in a comment at the check site.
enum class FormatError : std::uint8_t {
  none = 0,
  bad_magic,        ///< the file is not of the format asked for at all
  truncated,        ///< a field or payload runs past the end of the input
  malformed,        ///< structurally invalid: a count, flag or sentinel is wrong
  out_of_range,     ///< an index or offset in the file points outside it
  unsupported,      ///< well formed, but a variant this reader does not decode
  buffer_too_small, ///< the caller's output buffer cannot hold the result
  not_found,        ///< a lookup by name found nothing
};

/// A value or an error, whichever the operation produced.
///
/// `value()` on a failed result returns a default-constructed T rather than
/// misbehaving, so that a caller who forgets to check gets boring zeroes
/// instead of undefined behaviour on hostile input.
template <class T>
class Result {
 public:
  constexpr Result(T value) noexcept : value_(std::move(value)) {}
  constexpr Result(FormatError error) noexcept : error_(error) {}

  constexpr bool ok() const noexcept { return error_ == FormatError::none; }
  constexpr explicit operator bool() const noexcept { return ok(); }
  constexpr FormatError error() const noexcept { return error_; }

  constexpr const T& value() const& noexcept { return value_; }
  constexpr T& value() & noexcept { return value_; }
  constexpr T value_or(T fallback) const { return ok() ? value_ : std::move(fallback); }

  constexpr const T* operator->() const noexcept { return &value_; }
  constexpr const T& operator*() const noexcept { return value_; }
  // Not merely a convenience: a stateful reader such as PakCursor is held in a
  // Result and advanced through it, so the mutable overloads are how it walks.
  constexpr T* operator->() noexcept { return &value_; }
  constexpr T& operator*() noexcept { return value_; }

 private:
  T value_{};
  FormatError error_ = FormatError::none;
};

/// The same contract for operations that either work or do not.
template <>
class Result<void> {
 public:
  constexpr Result() noexcept = default;
  constexpr Result(FormatError error) noexcept : error_(error) {}

  constexpr bool ok() const noexcept { return error_ == FormatError::none; }
  constexpr explicit operator bool() const noexcept { return ok(); }
  constexpr FormatError error() const noexcept { return error_; }

 private:
  FormatError error_ = FormatError::none;
};

using Status = Result<void>;

}  // namespace imperivm::core
