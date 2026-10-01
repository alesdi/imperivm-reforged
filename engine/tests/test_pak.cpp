// Pack archive tests. Synthetic buffers only: CI has no game installation, and
// the interesting cases — a front-coded name that reuses more of its
// predecessor than exists, an entry pointing past the end of the file — are
// exactly the ones no retail archive contains.

#include "builder.hpp"
#include "imperivm/core/formats/pak.hpp"
#include "test.hpp"

using namespace imperivm::core;
using imperivm::test::Builder;

namespace {

/// Two entries sharing the prefix `DATA\`, holding "abc" and "defg".
///
///   header      0x00..0x20   magic, 0x1A, padding
///   counts      0x20..0x28   file_count, name_table_bytes
///   entries     0x28..0x4B   two front-coded records
///   timestamps  0x4B..0x53   one u32 each
///   data        0x53..0x5A   "abcdefg"
Builder good_archive() {
  Builder archive;
  archive.text("HMMSYS PackFile\n").u8(0x1A).zeros(15);
  archive.u32(2).u32(35);  // file_count, name_table_bytes

  archive.u8(10).u8(0).text("DATA\\A.TXT").u32(83).u32(3);
  archive.u8(10).u8(5).text("B.TXT").u32(86).u32(4);

  archive.u32(0x2D3C8A61).u32(0x2D3C8A61);  // MS-DOS stamps
  archive.text("abcdefg");
  return archive;
}

std::string_view name_of(const PakEntry& entry) { return entry.name; }

}  // namespace

TEST(pak_parses_front_coded_names) {
  const Builder archive = good_archive();
  const auto directory = PakDirectory::parse(archive.span());
  CHECK(directory.ok());
  CHECK(directory->size() == 2);
  CHECK(name_of(directory->entries()[0]) == "DATA\\A.TXT");
  // The second name exists nowhere in the file: five bytes come from the
  // first entry and five from this one's suffix.
  CHECK(name_of(directory->entries()[1]) == "DATA\\B.TXT");
  CHECK(directory->entries()[1].offset == 86);
  CHECK(directory->entries()[1].size == 4);
  CHECK(directory->validate().ok());
}

TEST(pak_reads_stored_bytes) {
  const Builder archive = good_archive();
  const auto directory = PakDirectory::parse(archive.span());
  const auto blob = directory->read("DATA\\B.TXT");
  CHECK(blob.ok());
  CHECK(blob->size() == 4);
  CHECK(static_cast<char>((*blob)[0]) == 'd');
}

TEST(pak_lookup_ignores_case_and_separator) {
  const Builder archive = good_archive();
  const auto directory = PakDirectory::parse(archive.span());
  // The engine's own paths are inconsistent about both, so lookup folds them.
  CHECK(directory->find("data/a.txt").ok());
  CHECK(directory->find("DATA\\A.TXT").ok());
  CHECK(directory->find("DATA\\MISSING.TXT").error() == FormatError::not_found);
}

TEST(pak_rejects_foreign_files) {
  Builder other;
  other.text("PK\x03\x04").zeros(64);
  CHECK(PakDirectory::parse(other.span()).error() == FormatError::bad_magic);
}

TEST(pak_rejects_shared_prefix_longer_than_the_previous_name) {
  Builder archive = good_archive();
  // The first entry has no predecessor, so any shared length is a lie. A
  // reader that trusted it would hand back its own uninitialised buffer.
  archive.patch_u8(0x29, 4);
  CHECK(PakDirectory::parse(archive.span()).error() == FormatError::malformed);
}

TEST(pak_rejects_shared_prefix_longer_than_the_name) {
  Builder archive = good_archive();
  archive.patch_u8(0x3D, 200);  // shared 200 of a 10-byte name
  CHECK(PakDirectory::parse(archive.span()).error() == FormatError::malformed);
}

TEST(pak_rejects_entries_pointing_past_the_end) {
  Builder archive = good_archive();
  archive.patch_u32(0x34, 0xFFFFFFF0);  // first entry's offset
  CHECK(PakDirectory::parse(archive.span()).error() == FormatError::out_of_range);
}

TEST(pak_rejects_a_size_that_overflows_its_offset) {
  Builder archive = good_archive();
  archive.patch_u32(0x38, 0xFFFFFFFF);  // first entry's size
  // offset + size wraps in 32 bits; the check has to be done in 64.
  CHECK(PakDirectory::parse(archive.span()).error() == FormatError::out_of_range);
}

TEST(pak_rejects_a_name_table_that_does_not_end_where_the_header_says) {
  Builder archive = good_archive();
  archive.patch_u32(0x24, 36);  // name_table_bytes one too many
  CHECK(PakDirectory::parse(archive.span()).error() == FormatError::malformed);
}

TEST(pak_rejects_a_name_table_running_past_the_file) {
  Builder archive = good_archive();
  archive.patch_u32(0x24, 0x00FFFFFF);
  CHECK(PakDirectory::parse(archive.span()).error() == FormatError::truncated);
}

TEST(pak_rejects_a_file_count_whose_timestamp_table_does_not_fit) {
  Builder archive = good_archive();
  archive.patch_u32(0x20, 0x01000000);
  CHECK(parse_pak_header(archive.span()).error() == FormatError::truncated);
}

TEST(pak_validate_rejects_non_contiguous_files) {
  Builder archive = good_archive();
  archive.patch_u32(0x34, 84);  // a one-byte gap before the first file
  const auto directory = PakDirectory::parse(archive.span());
  CHECK(directory.ok());
  // Files abut exactly in all 14,678 consecutive pairs of the retail set.
  CHECK(!directory->validate().ok());
}

TEST(pak_cursor_needs_a_full_size_name_buffer) {
  const Builder archive = good_archive();
  auto cursor = PakCursor::open(archive.span());
  CHECK(cursor.ok());
  char small[16];
  CHECK(cursor->next(std::span<char>(small, sizeof small)).error() ==
        FormatError::buffer_too_small);
}

TEST(pak_cursor_walks_entries_then_stops) {
  const Builder archive = good_archive();
  auto cursor = PakCursor::open(archive.span());
  char names[kPakMaxNameLength];
  const std::span<char> buffer(names, sizeof names);
  CHECK(cursor->next(buffer).ok());
  CHECK(cursor->next(buffer).ok());
  CHECK(cursor->done());
  CHECK(cursor->next(buffer).error() == FormatError::out_of_range);
}

TEST(pak_reads_timestamps) {
  const Builder archive = good_archive();
  const auto directory = PakDirectory::parse(archive.span());
  const auto stamp = directory->timestamp(1);
  CHECK(stamp.ok());
  const DosTimestamp when = decode_dos_timestamp(*stamp);
  CHECK(when.year == 2002);
  CHECK(when.month == 9);
  CHECK(when.day == 28);
  CHECK(when.hour == 17);
  CHECK(when.minute == 19);
  CHECK(when.second == 2);
  CHECK(directory->timestamp(2).error() == FormatError::out_of_range);
}

TEST(pak_accepts_an_empty_archive) {
  Builder archive;
  archive.text("HMMSYS PackFile\n").u8(0x1A).zeros(15).u32(0).u32(0);
  const auto directory = PakDirectory::parse(archive.span());
  CHECK(directory.ok());
  CHECK(directory->size() == 0);
  CHECK(directory->validate().ok());
}
