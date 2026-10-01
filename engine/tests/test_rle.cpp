// Sprite tests.
//
// A sprite lives in two files, so these build both: a frame table with the
// geometry and palette, and a stand-in pixel store holding the compressed rows
// at the absolute offsets the frame records name.

#include <vector>

#include "builder.hpp"
#include "imperivm/core/formats/rle.hpp"
#include "test.hpp"

using namespace imperivm::core;
using imperivm::test::Builder;

namespace {

constexpr std::uint32_t kStorePrefix = 8;  ///< payloads never start at 0 here

/// A 2x1 sheet: one populated 4x2 indexed frame, one empty frame.
Builder frame_table(std::uint32_t image_class = 1, std::uint32_t data_size = 16,
                    std::uint32_t wide = 0) {
  Builder table;
  table.text("IMGRLE").u32(image_class).u32(0).u32(0);
  table.u32(2).u32(1);  // columns, rows

  // Frame 0: box (3,5)..(6,6), so 4 by 2.
  table.u32(3).u32(5).u32(6).u32(6);
  table.text("RLE2").u32(4).u32(2).u16(1);
  table.u32(data_size).u32(0).u32(wide).u32(0);
  table.text("pamm").u32(kStorePrefix);

  // Frame 1: empty. The encoder grows the box inwards and never touched it, so
  // right < left and no RLE2 block follows.
  table.u32(200).u32(200).u32(0).u32(0);

  table.u32(2);                       // palette entries
  table.u8(0).u8(0).u8(0).u8(0);      // index 0: black, in BGRx order
  table.u8(0x10).u8(0x20).u8(0x30).u8(0);  // index 1: blue 0x10, green 0x20, red 0x30
  return table;
}

/// The rows of frame 0, prefixed so the payload does not begin at offset 0.
///
/// Row 0: skip 1, draw 2, skip 1, draw 0 — the trailing pair is how a run that
/// ends before the right edge is padded out to the full width.
/// Row 1: draw all four pixels.
Builder pixel_store() {
  Builder store;
  store.zeros(kStorePrefix);
  store.u16(4).u16(10);                    // row offset table, u16 entries
  store.u8(1).u8(2).u8(1).u8(1).u8(1).u8(0);  // row 0, 6 bytes
  store.u8(0).u8(4).u8(0).u8(1).u8(0).u8(1);  // row 1, 6 bytes
  return store;
}

std::uint8_t at(const std::vector<std::byte>& rgba, std::uint32_t x, std::uint32_t y,
                std::uint32_t channel) {
  return static_cast<std::uint8_t>(rgba[(y * 4 + x) * 4 + channel]);
}

}  // namespace

TEST(rle_parses_a_frame_table) {
  const Builder table = frame_table();
  const auto image = RleImage::parse(table.span());
  CHECK(image.ok());
  CHECK(image->columns() == 2);
  CHECK(image->rows() == 1);
  CHECK(image->frames().size() == 2);
  CHECK(image->palette_size() == 2);
  CHECK(image->validate().ok());

  const RleFrame& frame = image->frames()[0];
  CHECK(!frame.empty);
  CHECK(frame.width == 4);
  CHECK(frame.height == 2);
  CHECK(frame.left == 3 && frame.top == 5);
  CHECK(frame.pixel_format == RlePixelFormat::indexed8);
  CHECK(frame.data_offset == kStorePrefix);
  CHECK(frame.row_entry_size() == 2);
}

TEST(rle_detects_empty_frames_by_their_inverted_box) {
  const Builder table = frame_table();
  const auto image = RleImage::parse(table.span());
  const RleFrame& frame = image->frames()[1];
  // 913 of the retail set's 197,432 frames look like this, and the degenerate
  // value varies per file, so nothing may test for a sentinel.
  CHECK(frame.empty);
  CHECK(frame.left == 200 && frame.right == 0);
  CHECK(frame.data_size == 0);
}

TEST(rle_addresses_payloads_in_the_pixel_store) {
  const Builder table = frame_table();
  const Builder store = pixel_store();
  const auto image = RleImage::parse(table.span());
  const auto payload = rle_frame_payload(image->frames()[0], store.span());
  CHECK(payload.ok());
  CHECK(payload->size() == 16);
  // The empty frame owns no payload at all.
  const auto none = rle_frame_payload(image->frames()[1], store.span());
  CHECK(none.ok());
  CHECK(none->empty());
}

TEST(rle_walks_covered_spans) {
  const Builder table = frame_table();
  const Builder store = pixel_store();
  const auto image = RleImage::parse(table.span());
  const auto payload = rle_frame_payload(image->frames()[0], store.span());

  int spans = 0;
  std::uint32_t covered = 0;
  const Status status = rle_for_each_span(
      image->frames()[0], *payload,
      [&](std::uint32_t y, std::uint32_t x, std::uint32_t length,
          std::span<const std::byte> run) {
        ++spans;
        covered += length;
        if (spans == 1) {
          CHECK(y == 0 && x == 1 && length == 2);
          CHECK(run.size() == 2);  // one byte per pixel for an indexed frame
        }
      });
  CHECK(status.ok());
  // The zero-length run of row 0 is skipped: it exists only so a gap can be
  // expressed, and it carries no pixels.
  CHECK(spans == 2);
  CHECK(covered == 6);
}

TEST(rle_decodes_to_rgba_leaving_gaps_transparent) {
  const Builder table = frame_table();
  const Builder store = pixel_store();
  const auto image = RleImage::parse(table.span());
  const auto payload = rle_frame_payload(image->frames()[0], store.span());

  std::vector<std::byte> rgba(4 * 2 * 4);
  CHECK(rle_decode_rgba(*image, image->frames()[0], *payload, rgba).ok());

  // Pixel (0,0) is inside the leading gap: transparency is carried entirely by
  // the gaps, and there is no alpha channel anywhere in the format.
  CHECK(at(rgba, 0, 0, 3) == 0);
  // Pixel (1,0) is palette index 1, stored blue-green-red.
  CHECK(at(rgba, 1, 0, 0) == 0x30);
  CHECK(at(rgba, 1, 0, 1) == 0x20);
  CHECK(at(rgba, 1, 0, 2) == 0x10);
  CHECK(at(rgba, 1, 0, 3) == 255);
  // Row 1 is fully covered, alternating the two palette entries.
  CHECK(at(rgba, 0, 1, 3) == 255);
  CHECK(at(rgba, 0, 1, 0) == 0);
  CHECK(at(rgba, 3, 1, 0) == 0x30);
}

TEST(rle_substitutes_player_colors) {
  const Builder table = frame_table();
  const Builder store = pixel_store();
  const auto image = RleImage::parse(table.span());
  const auto payload = rle_frame_payload(image->frames()[0], store.span());

  // Slots 0..63 are the team-colour block; the shipped colours are only a
  // neutral default.
  std::vector<Rgb888> ramp(kRlePlayerColorSlots, Rgb888{1, 2, 3});
  std::vector<std::byte> rgba(4 * 2 * 4);
  CHECK(rle_decode_rgba(*image, image->frames()[0], *payload, rgba, ramp).ok());
  CHECK(at(rgba, 1, 0, 0) == 1);
  CHECK(at(rgba, 1, 0, 1) == 2);

  std::vector<Rgb888> wrong(3, Rgb888{});
  CHECK(rle_decode_rgba(*image, image->frames()[0], *payload, rgba, wrong).error() ==
        FormatError::malformed);
}

TEST(rle_decoding_an_empty_frame_produces_nothing) {
  const Builder table = frame_table();
  const auto image = RleImage::parse(table.span());
  std::vector<std::byte> rgba(4);
  CHECK(rle_decode_rgba(*image, image->frames()[1], {}, rgba).ok());
  CHECK(static_cast<std::uint8_t>(rgba[3]) == 0);
}

TEST(rle_rejects_a_foreign_file) {
  Builder other;
  other.text("IMGRL2").zeros(64);
  CHECK(RleImage::parse(other.span()).error() == FormatError::bad_magic);
}

TEST(rle_rejects_a_missing_frame_magic) {
  Builder table = frame_table();
  table.patch_u8(0x2A, 'X');  // the RLE2 tag of frame 0
  CHECK(RleImage::parse(table.span()).error() == FormatError::malformed);
}

TEST(rle_rejects_a_box_that_disagrees_with_the_size) {
  Builder table = frame_table();
  table.patch_u32(0x2E, 5);  // width 5 in a 4-wide box
  CHECK(RleImage::parse(table.span()).error() == FormatError::malformed);
}

TEST(rle_rejects_unexplained_trailing_bytes) {
  Builder table = frame_table();
  table.u8(0);
  // The file ends on the palette, or on the 512-byte lookup table. Anything
  // else means the frame records were misparsed.
  CHECK(RleImage::parse(table.span()).error() == FormatError::malformed);
}

TEST(rle_accepts_the_class_two_lookup_table) {
  Builder table = frame_table(2);
  // Class 2 needs 128 palette entries for the mirror check, so this file only
  // exercises the 512-byte tail; the mirror itself is checked below.
  table.zeros(kRleLutSize);
  const auto image = RleImage::parse(table.span());
  CHECK(image.ok());
  CHECK(image->has_lut());
  CHECK(image->image_class() == RleImageClass::player_color);
}

TEST(rle_validate_rejects_class_two_without_mirrored_slots) {
  Builder table = frame_table(2);
  table.zeros(kRleLutSize);
  const auto image = RleImage::parse(table.span());
  // Entries 0..63 must be a byte-identical copy of 64..127 in all 1,668 class
  // 2 files; this one has only two entries.
  CHECK(image->validate().error() == FormatError::malformed);
}

TEST(rle_validate_rejects_a_wide_flag_that_disagrees_with_the_payload) {
  const Builder table = frame_table(1, 16, 1);
  const auto image = RleImage::parse(table.span());
  CHECK(image.ok());
  // The flag is set on exactly the frames whose payload exceeds 65,535 bytes.
  CHECK(image->validate().error() == FormatError::malformed);
}

TEST(rle_validate_rejects_a_colour_key_outside_the_palette) {
  Builder table = frame_table();
  table.patch_u32(0x44, 7);  // colour key of frame 0, palette holds 2 entries
  const auto image = RleImage::parse(table.span());
  CHECK(image.ok());
  CHECK(image->validate().error() == FormatError::malformed);
}

TEST(rle_rejects_a_payload_outside_the_store) {
  Builder table = frame_table();
  table.patch_u32(0x4C, 0xFFFFFFF0);  // data_offset of frame 0
  const Builder store = pixel_store();
  const auto image = RleImage::parse(table.span());
  CHECK(rle_frame_payload(image->frames()[0], store.span()).error() ==
        FormatError::out_of_range);
}

TEST(rle_rejects_a_row_table_that_does_not_abut_the_rows) {
  const Builder table = frame_table();
  Builder store = pixel_store();
  store.patch_u8(kStorePrefix, 6);  // first row offset, should be 4
  const auto image = RleImage::parse(table.span());
  const auto payload = rle_frame_payload(image->frames()[0], store.span());
  const Status status = rle_for_each_span(image->frames()[0], *payload,
                                          [](std::uint32_t, std::uint32_t, std::uint32_t,
                                             std::span<const std::byte>) {});
  CHECK(status.error() == FormatError::malformed);
}

TEST(rle_rejects_a_row_that_does_not_cover_the_frame_width) {
  const Builder table = frame_table();
  Builder store = pixel_store();
  store.patch_u8(kStorePrefix + 4, 2);  // row 0's first gap, widening the row
  const auto image = RleImage::parse(table.span());
  const auto payload = rle_frame_payload(image->frames()[0], store.span());
  // Gaps and runs must sum to exactly the width; this is the check that
  // catches a payload read at the wrong offset in the store.
  const Status status = rle_for_each_span(image->frames()[0], *payload,
                                          [](std::uint32_t, std::uint32_t, std::uint32_t,
                                             std::span<const std::byte>) {});
  CHECK(status.error() == FormatError::malformed);
}

TEST(rle_rejects_a_run_reaching_past_its_row) {
  const Builder table = frame_table();
  Builder store = pixel_store();
  store.patch_u8(kStorePrefix + 5, 6);  // row 0's first run length
  const auto image = RleImage::parse(table.span());
  const auto payload = rle_frame_payload(image->frames()[0], store.span());
  const Status status = rle_for_each_span(image->frames()[0], *payload,
                                          [](std::uint32_t, std::uint32_t, std::uint32_t,
                                             std::span<const std::byte>) {});
  CHECK(status.error() == FormatError::malformed);
}
