#pragma once

// Reader for the HMMSYS PackFile archive (`.pak`).
//
// Specification: docs/formats/pak.md
//
// The archive is a front-coded name table, a table of MS-DOS timestamps, and
// then every stored file laid end to end. Because names are front coded, entry
// N cannot be reconstructed without entry N-1: the table is a stream, not an
// array, and that shapes the API below.
//
// Two levels are offered, because the engine wants different things at
// different times:
//
//   * `PakCursor` walks the table one entry at a time into a caller-supplied
//     name buffer and allocates nothing. This is what a loader that only wants
//     to find one file should use.
//   * `PakDirectory` runs the cursor once and keeps the result — names in one
//     flat buffer, entries in one vector — for repeated lookup.
//
// Neither owns the archive bytes; the caller keeps the span alive.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core {

inline constexpr std::string_view kPakMagic = "HMMSYS PackFile\n";
inline constexpr std::size_t kPakHeaderSize = 0x20;
inline constexpr std::size_t kPakTableStart = 0x28;

/// A name is stored with a u8 length, so this is a hard limit of the format.
inline constexpr std::size_t kPakMaxNameLength = 255;

/// One stored file. `name` points into the caller's buffer (PakCursor) or into
/// the directory's own storage (PakDirectory), never into the archive: front
/// coding means a full name exists nowhere in the file.
struct PakEntry {
  std::string_view name;
  std::uint32_t offset = 0;
  std::uint32_t size = 0;
};

/// An MS-DOS packed date and time, unpacked. Read the stamps as UTC: the DOS
/// convention is local time, which would make a rebuilt archive depend on the
/// machine's time zone and be undefined inside a daylight-saving gap.
struct DosTimestamp {
  std::uint16_t year = 1980;  ///< absolute, already offset from the stored 1980 base
  std::uint8_t month = 0;     ///< 1..12
  std::uint8_t day = 0;       ///< 1..31
  std::uint8_t hour = 0;
  std::uint8_t minute = 0;
  std::uint8_t second = 0;  ///< always even; the format stores it halved
};

constexpr DosTimestamp decode_dos_timestamp(std::uint32_t packed) noexcept {
  DosTimestamp out;
  out.second = static_cast<std::uint8_t>((packed & 0x1F) * 2);
  out.minute = static_cast<std::uint8_t>((packed >> 5) & 0x3F);
  out.hour = static_cast<std::uint8_t>((packed >> 11) & 0x1F);
  out.day = static_cast<std::uint8_t>((packed >> 16) & 0x1F);
  out.month = static_cast<std::uint8_t>((packed >> 21) & 0x0F);
  out.year = static_cast<std::uint16_t>(1980 + ((packed >> 25) & 0x7F));
  return out;
}

/// The parts of the header a reader needs, plus the derived table bounds.
struct PakHeader {
  std::uint32_t file_count = 0;
  std::uint32_t name_table_bytes = 0;
  std::size_t table_end = 0;   ///< first byte after the entry table
  std::size_t data_start = 0;  ///< first byte after the timestamp table
};

Result<PakHeader> parse_pak_header(std::span<const std::byte> archive);

/// Sequential decoder for the entry table. Allocation free.
///
/// Every call to `next` needs the same name buffer back, because each entry
/// reuses a prefix of the previous name; passing a different buffer, or
/// scribbling on it between calls, corrupts every following name.
class PakCursor {
 public:
  static Result<PakCursor> open(std::span<const std::byte> archive);

  const PakHeader& header() const noexcept { return header_; }
  std::uint32_t index() const noexcept { return index_; }
  bool done() const noexcept { return index_ >= header_.file_count; }

  /// Decode the next entry. `name_buffer` must be at least kPakMaxNameLength.
  Result<PakEntry> next(std::span<char> name_buffer);

  /// The stamp of entry `index`, from the table that follows the names.
  Result<std::uint32_t> timestamp(std::uint32_t index) const;

  /// A default cursor is inert: it holds no archive and reports `done()`
  /// straight away, so it cannot walk off the end of anything. That is what
  /// lets `Result<PakCursor>` hand back a harmless value when a parse fails,
  /// which is the contract described in result.hpp. Use `open()` for a usable
  /// one.
  PakCursor() = default;

 private:
  std::span<const std::byte> archive_{};
  PakHeader header_{};
  std::size_t position_ = kPakTableStart;
  std::uint32_t index_ = 0;
  std::uint8_t previous_length_ = 0;
};

/// The whole entry table, decoded once and kept.
class PakDirectory {
 public:
  static Result<PakDirectory> parse(std::span<const std::byte> archive);

  const PakHeader& header() const noexcept { return header_; }
  std::size_t size() const noexcept { return entries_.size(); }

  /// Entries in stored order, which is bytewise ascending on the full name.
  /// The views point into this object's own name storage, so they stay valid
  /// across a move (a vector's buffer transfers; a std::string's may not).
  const std::vector<PakEntry>& entries() const noexcept { return entries_; }

  /// Case-insensitive lookup accepting either path separator, matching the
  /// engine, whose own paths are inconsistent about both.
  Result<PakEntry> find(std::string_view name) const;

  /// The stored bytes of one entry, as a subspan of the archive.
  Result<std::span<const std::byte>> read(const PakEntry& entry) const;
  Result<std::span<const std::byte>> read(std::string_view name) const;

  Result<std::uint32_t> timestamp(std::size_t index) const;

  /// The structural invariants from the specification: offsets ascend, files
  /// are contiguous, the first begins right after the timestamp table and the
  /// last ends on the final byte. Retail archives satisfy all of them, so a
  /// failure here means either a damaged file or a misunderstood format.
  Status validate() const;

 private:
  std::span<const std::byte> archive_{};
  PakHeader header_{};
  std::vector<PakEntry> entries_;
  std::vector<char> names_;  ///< every name concatenated; entries_ views point here
};

}  // namespace imperivm::core
