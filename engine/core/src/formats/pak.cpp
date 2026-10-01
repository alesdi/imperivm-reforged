#include "imperivm/core/formats/pak.hpp"

#include <algorithm>

#include "imperivm/core/formats/byte_reader.hpp"

namespace imperivm::core {
namespace {

/// Uppercase ASCII and fold `/` onto `\`. Names are cp1252, but the archives
/// are ASCII within it and the engine only ever case-folds ASCII, so folding
/// the high half would invent matches the original would not make.
constexpr char normalise(char c) noexcept {
  if (c == '/') return '\\';
  if (c >= 'a' && c <= 'z') return static_cast<char>(c - 'a' + 'A');
  return c;
}

bool names_match(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (normalise(a[i]) != normalise(b[i])) return false;
  }
  return true;
}

}  // namespace

Result<PakHeader> parse_pak_header(std::span<const std::byte> archive) {
  if (!has_magic(archive, kPakMagic)) return FormatError::bad_magic;
  if (archive.size() < kPakTableStart) return FormatError::truncated;

  PakHeader header;
  header.file_count = read_u32le(archive, kPakHeaderSize);
  header.name_table_bytes = read_u32le(archive, kPakHeaderSize + 4);

  // Both of these come from the file, so both are computed in 64 bits before
  // being compared against the size.
  const std::uint64_t table_end =
      static_cast<std::uint64_t>(kPakTableStart) + header.name_table_bytes;
  if (table_end > archive.size()) return FormatError::truncated;

  // The timestamp table sits between the names and the first stored file. An
  // early revision of the specification mistook it for inter-file padding.
  const std::uint64_t data_start = table_end + 4ull * header.file_count;
  if (data_start > archive.size()) return FormatError::truncated;

  header.table_end = static_cast<std::size_t>(table_end);
  header.data_start = static_cast<std::size_t>(data_start);
  return header;
}

Result<PakCursor> PakCursor::open(std::span<const std::byte> archive) {
  const Result<PakHeader> header = parse_pak_header(archive);
  if (!header) return header.error();

  PakCursor cursor;
  cursor.archive_ = archive;
  cursor.header_ = header.value();
  return cursor;
}

Result<PakEntry> PakCursor::next(std::span<char> name_buffer) {
  if (done()) return FormatError::out_of_range;
  if (name_buffer.size() < kPakMaxNameLength) return FormatError::buffer_too_small;
  if (position_ + 2 > header_.table_end) return FormatError::truncated;

  const std::uint8_t total_length = read_u8(archive_, position_);
  const std::uint8_t shared_length = read_u8(archive_, position_ + 1);
  position_ += 2;

  // Front coding: the entry repeats a prefix of the *previous* full name. A
  // shared length longer than either name is the classic way a fuzzed archive
  // tries to read the buffer's uninitialised tail.
  if (shared_length > total_length || shared_length > previous_length_) {
    return FormatError::malformed;
  }

  const std::size_t suffix_length = static_cast<std::size_t>(total_length - shared_length);
  if (position_ + suffix_length + 8 > header_.table_end) return FormatError::truncated;

  for (std::size_t i = 0; i < suffix_length; ++i) {
    name_buffer[shared_length + i] = static_cast<char>(archive_[position_ + i]);
  }
  position_ += suffix_length;

  PakEntry entry;
  entry.offset = read_u32le(archive_, position_);
  entry.size = read_u32le(archive_, position_ + 4);
  position_ += 8;

  if (!span_contains(archive_, entry.offset, entry.size)) return FormatError::out_of_range;

  entry.name = std::string_view(name_buffer.data(), total_length);
  previous_length_ = total_length;
  ++index_;

  // The table must end exactly where the header said it would; anything else
  // means the entry count and the byte count disagree.
  if (done() && position_ != header_.table_end) return FormatError::malformed;
  return entry;
}

Result<std::uint32_t> PakCursor::timestamp(std::uint32_t index) const {
  if (index >= header_.file_count) return FormatError::out_of_range;
  return read_u32le(archive_, header_.table_end + 4ull * index);
}

Result<PakDirectory> PakDirectory::parse(std::span<const std::byte> archive) {
  Result<PakCursor> cursor = PakCursor::open(archive);
  if (!cursor) return cursor.error();

  PakDirectory directory;
  directory.archive_ = archive;
  directory.header_ = cursor->header();

  const std::uint32_t count = directory.header_.file_count;
  directory.entries_.reserve(count);
  // Names average well under 40 bytes; one reservation keeps the flat buffer
  // from copying while it grows, but correctness does not depend on it because
  // the views are attached after the walk.
  directory.names_.reserve(static_cast<std::size_t>(count) * 40);

  std::vector<std::size_t> name_starts;
  name_starts.reserve(count);

  char name_buffer[kPakMaxNameLength];
  for (std::uint32_t i = 0; i < count; ++i) {
    Result<PakEntry> entry = cursor.value().next(std::span<char>(name_buffer, sizeof name_buffer));
    if (!entry) return entry.error();
    name_starts.push_back(directory.names_.size());
    directory.names_.insert(directory.names_.end(), entry->name.begin(), entry->name.end());
    directory.entries_.push_back(*entry);
  }

  // Only now that the flat buffer has stopped growing is it safe to point at.
  for (std::size_t i = 0; i < directory.entries_.size(); ++i) {
    directory.entries_[i].name = std::string_view(directory.names_.data() + name_starts[i],
                                                  directory.entries_[i].name.size());
  }
  return directory;
}

Result<PakEntry> PakDirectory::find(std::string_view name) const {
  // Linear: the archives hold at most a few thousand entries and lookups
  // happen at load time. A map would need an allocation and an ordering, and
  // the ordering is exactly the thing that is easy to get wrong here.
  for (const PakEntry& entry : entries_) {
    if (names_match(entry.name, name)) return entry;
  }
  return FormatError::not_found;
}

Result<std::span<const std::byte>> PakDirectory::read(const PakEntry& entry) const {
  if (!span_contains(archive_, entry.offset, entry.size)) return FormatError::out_of_range;
  return archive_.subspan(entry.offset, entry.size);
}

Result<std::span<const std::byte>> PakDirectory::read(std::string_view name) const {
  const Result<PakEntry> entry = find(name);
  if (!entry) return entry.error();
  return read(entry.value());
}

Result<std::uint32_t> PakDirectory::timestamp(std::size_t index) const {
  if (index >= entries_.size()) return FormatError::out_of_range;
  return read_u32le(archive_, header_.table_end + 4 * index);
}

Status PakDirectory::validate() const {
  if (entries_.empty()) {
    return header_.data_start == archive_.size() ? Status{} : Status{FormatError::malformed};
  }

  // Stored files are contiguous. This is the invariant that caught the earlier
  // misreading of the layout: what looked like gaps was the timestamp table.
  std::uint64_t expected = header_.data_start;
  for (const PakEntry& entry : entries_) {
    if (entry.offset != expected) return FormatError::malformed;
    expected += entry.size;
  }
  if (expected != archive_.size()) return FormatError::malformed;
  return {};
}

}  // namespace imperivm::core
