#include "imperivm/core/formats/vq.hpp"

#include "imperivm/core/formats/byte_reader.hpp"

namespace imperivm::core {

Result<VqImage> VqImage::parse(std::span<const std::byte> data) {
  if (!has_magic(data, kVqMagic)) return FormatError::bad_magic;
  if (data.size() < kVqHeaderSize) return FormatError::truncated;

  VqImage image;
  VqHeader& header = image.header_;
  header.index_size_log2 = read_u32le(data, 0x04);
  header.codebook_len = read_u32le(data, 0x08);
  header.index_size = read_u32le(data, 0x0C);
  header.bytes_per_pixel = read_u32le(data, 0x10);
  header.block_width = read_u32le(data, 0x14);
  header.block_height = read_u32le(data, 0x18);
  header.width = read_u32le(data, 0x1C);
  header.height = read_u32le(data, 0x20);
  header.blocks_x = read_u32le(data, 0x24);
  header.blocks_y = read_u32le(data, 0x28);

  if (header.index_size != 1 && header.index_size != 2) return FormatError::unsupported;
  if (header.index_size != (1u << header.index_size_log2)) return FormatError::malformed;
  if (header.bytes_per_pixel != kVqBytesPerPixel || header.block_width != kVqBlockWidth ||
      header.block_height != kVqBlockHeight) {
    // Nothing in the retail data varies these, so anything else is a variant
    // whose layout is unknown rather than a file this reader can guess at.
    return FormatError::unsupported;
  }
  if (header.codebook_len == 0) return FormatError::malformed;
  if (header.index_size == 1 && header.codebook_len > 256) return FormatError::malformed;
  if (header.blocks_x * kVqBlockWidth != header.width ||
      header.blocks_y * kVqBlockHeight != header.height) {
    return FormatError::malformed;
  }

  // There is no padding anywhere: codebook, then indices, then the last byte
  // of the file. An exact size check is therefore also a strong format check.
  if (header.expected_size() != data.size()) return FormatError::malformed;

  image.codebook_ = data.subspan(kVqHeaderSize, static_cast<std::size_t>(header.codebook_bytes()));
  image.indices_ = data.subspan(kVqHeaderSize + static_cast<std::size_t>(header.codebook_bytes()));
  return image;
}

std::uint32_t VqImage::index_at(std::uint64_t block) const noexcept {
  if (block >= header_.index_count()) return 0;
  const std::size_t at = static_cast<std::size_t>(block * header_.index_size);
  return header_.index_size == 1 ? read_u8(indices_, at) : read_u16le(indices_, at);
}

std::uint16_t VqImage::sample(std::uint32_t x, std::uint32_t y) const noexcept {
  if (x >= header_.width || y >= header_.height) return 0;
  const std::uint64_t block =
      static_cast<std::uint64_t>(y / kVqBlockHeight) * header_.blocks_x + x / kVqBlockWidth;
  const std::uint32_t entry = index_at(block);
  if (entry >= header_.codebook_len) return 0;
  const std::size_t at = static_cast<std::size_t>(entry) * kVqBlockWidth * kVqBytesPerPixel +
                         (x % kVqBlockWidth) * kVqBytesPerPixel;
  return read_u16le(codebook_, at);
}

Status VqImage::decode_row_rgb888(std::uint32_t y, std::span<std::byte> out) const noexcept {
  if (y >= header_.height) return FormatError::out_of_range;
  if (out.size() < static_cast<std::size_t>(header_.width) * 3) return FormatError::buffer_too_small;

  for (std::uint32_t x = 0; x < header_.width; ++x) {
    const Rgb888 colour = expand_x1r5g5b5(sample(x, y));
    out[3 * x + 0] = static_cast<std::byte>(colour.red);
    out[3 * x + 1] = static_cast<std::byte>(colour.green);
    out[3 * x + 2] = static_cast<std::byte>(colour.blue);
  }
  return {};
}

Status VqImage::decode_rgb888(std::span<std::byte> out) const noexcept {
  const std::uint64_t needed = static_cast<std::uint64_t>(header_.width) * header_.height * 3;
  if (out.size() < needed) return FormatError::buffer_too_small;

  for (std::uint32_t y = 0; y < header_.height; ++y) {
    const std::size_t row = static_cast<std::size_t>(y) * header_.width * 3;
    const Status status = decode_row_rgb888(y, out.subspan(row));
    if (!status) return status;
  }
  return {};
}

Status VqImage::validate() const noexcept {
  if (header_.codebook_len & (header_.codebook_len - 1)) {
    return FormatError::malformed;  // codebook length is always a power of two
  }
  for (std::uint64_t block = 0; block < header_.index_count(); ++block) {
    if (index_at(block) >= header_.codebook_len) return FormatError::out_of_range;
  }
  for (std::size_t at = 0; at + 1 < codebook_.size(); at += 2) {
    if (read_u16le(codebook_, at) & 0x8000u) return FormatError::malformed;
  }
  return {};
}

}  // namespace imperivm::core
