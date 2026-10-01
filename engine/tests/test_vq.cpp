// Vector-quantised terrain texture tests, plus the colour expansion, which is
// shared with RGB555 sprite frames and is the one place the no-floating-point
// rule has a visible consequence.

#include <vector>

#include "builder.hpp"
#include "imperivm/core/formats/color.hpp"
#include "imperivm/core/formats/vq.hpp"
#include "test.hpp"

using namespace imperivm::core;
using imperivm::test::Builder;

namespace {

/// An 8x2 texture: two codebook entries of four pixels, four indices.
Builder texture(std::uint32_t index_size = 1) {
  Builder data;
  data.text("mbqv");
  data.u32(index_size == 1 ? 0 : 1);  // index_size_log2
  data.u32(2);                        // codebook_len
  data.u32(index_size);
  data.u32(2).u32(4).u32(1);  // bytes per pixel, block width, block height
  data.u32(8).u32(2);         // width, height
  data.u32(2).u32(2);         // blocks_x, blocks_y

  // Entry 0: white, black, pure red, pure blue. Entry 1: four mid greys.
  data.u16(0x7FFF).u16(0x0000).u16(0x7C00).u16(0x001F);
  data.u16(0x4210).u16(0x4210).u16(0x4210).u16(0x4210);

  const std::uint32_t indices[4] = {0, 1, 1, 0};
  for (const std::uint32_t index : indices) {
    if (index_size == 1) {
      data.u8(index);
    } else {
      data.u16(index);
    }
  }
  return data;
}

}  // namespace

TEST(vq_expands_five_bit_channels_by_bit_replication) {
  // Replication is not an approximation of scaling by 255/31: it is the exact
  // round-to-nearest result, and it maps 0 to 0 and 31 to 255 by construction.
  CHECK(expand5(0) == 0);
  CHECK(expand5(31) == 255);
  CHECK(expand5(1) == 8);
  // A truncating 13 * 255 / 31 would give 106 here, one short.
  CHECK(expand5(13) == 107);

  const Rgb888 white = expand_x1r5g5b5(0x7FFF);
  CHECK(white.red == 255 && white.green == 255 && white.blue == 255);
  const Rgb888 red = expand_x1r5g5b5(0x7C00);
  CHECK(red.red == 255 && red.green == 0 && red.blue == 0);
  // Bit 15 is not alpha and is ignored.
  CHECK(expand_x1r5g5b5(0xFFFF) == white);
}

TEST(vq_parses_a_texture) {
  const Builder data = texture();
  const auto image = VqImage::parse(data.span());
  CHECK(image.ok());
  CHECK(image->width() == 8);
  CHECK(image->height() == 2);
  CHECK(image->header().codebook_len == 2);
  CHECK(image->header().expected_size() == data.size());
  CHECK(image->validate().ok());
}

TEST(vq_quantises_runs_of_four_horizontal_pixels) {
  const Builder data = texture();
  const auto image = VqImage::parse(data.span());
  // Row 0 is entry 0 then entry 1; row 1 is entry 1 then entry 0.
  CHECK(image->sample(0, 0) == 0x7FFF);
  CHECK(image->sample(3, 0) == 0x001F);
  CHECK(image->sample(4, 0) == 0x4210);
  CHECK(image->sample(0, 1) == 0x4210);
  CHECK(image->sample(7, 1) == 0x001F);
}

TEST(vq_decodes_to_rgb888) {
  const Builder data = texture();
  const auto image = VqImage::parse(data.span());
  std::vector<std::byte> pixels(8 * 2 * 3);
  CHECK(image->decode_rgb888(pixels).ok());
  CHECK(static_cast<std::uint8_t>(pixels[0]) == 255);
  CHECK(static_cast<std::uint8_t>(pixels[3]) == 0);   // second pixel is black
  CHECK(static_cast<std::uint8_t>(pixels[6]) == 255);  // third is pure red
  CHECK(static_cast<std::uint8_t>(pixels[7]) == 0);
}

TEST(vq_rejects_an_output_buffer_that_is_too_small) {
  const Builder data = texture();
  const auto image = VqImage::parse(data.span());
  std::vector<std::byte> pixels(8 * 2 * 3 - 1);
  CHECK(image->decode_rgb888(pixels).error() == FormatError::buffer_too_small);
}

TEST(vq_handles_two_byte_indices) {
  const Builder data = texture(2);
  const auto image = VqImage::parse(data.span());
  CHECK(image.ok());
  CHECK(image->header().index_size == 2);
  CHECK(image->sample(4, 0) == 0x4210);
  CHECK(image->validate().ok());
}

TEST(vq_rejects_a_foreign_file) {
  Builder data;
  data.text("vqbm").zeros(64);  // the FourCC the wrong way round
  CHECK(VqImage::parse(data.span()).error() == FormatError::bad_magic);
}

TEST(vq_rejects_a_size_that_does_not_match_the_header) {
  Builder data = texture();
  data.u8(0);  // one unexplained trailing byte
  CHECK(VqImage::parse(data.span()).error() == FormatError::malformed);
}

TEST(vq_rejects_disagreeing_index_size_fields) {
  Builder data = texture();
  data.patch_u32(0x04, 1);  // index_size_log2 says 2 bytes, index_size says 1
  CHECK(VqImage::parse(data.span()).error() == FormatError::malformed);
}

TEST(vq_rejects_unknown_block_geometry) {
  Builder data = texture();
  data.patch_u32(0x14, 2);  // block width 2 rather than 4
  // Nothing in the retail data varies this, so its layout is a guess rather
  // than a decode.
  CHECK(VqImage::parse(data.span()).error() == FormatError::unsupported);
}

TEST(vq_rejects_blocks_that_do_not_cover_the_image) {
  Builder data = texture();
  data.patch_u32(0x1C, 12);  // width 12 with only 2 blocks per row
  CHECK(VqImage::parse(data.span()).error() == FormatError::malformed);
}

TEST(vq_validate_rejects_an_index_outside_the_codebook) {
  Builder data = texture();
  data.raw()[0x2C + 16] = std::byte{9};  // first index
  const auto image = VqImage::parse(data.span());
  CHECK(image.ok());
  CHECK(image->validate().error() == FormatError::out_of_range);
  // Out-of-range indices still read as a defined colour rather than reading
  // past the codebook, because decoding must be safe on hostile input.
  CHECK(image->sample(0, 0) == 0);
}

TEST(vq_validate_rejects_a_sample_with_bit_fifteen_set) {
  Builder data = texture();
  data.patch_u8(0x2D, 0xFF);  // high byte of the first codebook sample
  const auto image = VqImage::parse(data.span());
  CHECK(image.ok());
  CHECK(image->validate().error() == FormatError::malformed);
}

TEST(vq_validate_rejects_a_codebook_length_that_is_not_a_power_of_two) {
  Builder data;
  data.text("mbqv").u32(0).u32(3).u32(1).u32(2).u32(4).u32(1);
  data.u32(4).u32(1).u32(1).u32(1);
  data.zeros(3 * 8).u8(0);
  const auto image = VqImage::parse(data.span());
  CHECK(image.ok());
  CHECK(image->validate().error() == FormatError::malformed);
}
