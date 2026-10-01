#include "imperivm/core/formats/lzis.hpp"

#include <array>
#include <cstring>

#include "imperivm/core/formats/byte_reader.hpp"

namespace imperivm::core {
namespace {

/// Match length for literal/length symbols 256..284 and the extra bits that
/// follow. DEFLATE's tables, shifted down by one symbol.
constexpr std::array<std::uint16_t, 29> kLengthBase = {
    3,  4,  5,  6,  7,  8,  9,  10, 11, 13, 15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                       2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

/// DEFLATE's 30 distance slots. An LZIS distance symbol is a slot paired with a
/// parity bit, so the coder can model odd and even distances separately and
/// spend one fewer extra bit per slot.
constexpr std::array<std::uint32_t, 30> kDistanceBase = {
    1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,    65,    97,    129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,
                                                         4, 4, 5,  5,  6,  6,  7,  7,  8,  8,
                                                         9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/// Longest code the 5-bit code-length field can express.
constexpr unsigned kMaxCodeLength = 31;

/// Bits are consumed most significant first within each byte, and bytes in
/// order — the opposite of DEFLATE. The decoder is allowed to read a few bits
/// past the final byte, because the original loads four bytes at a time and the
/// last symbol of a chunk can legitimately straddle the end; those bits read as
/// zero here rather than out of bounds.
class BitReader {
 public:
  explicit BitReader(std::span<const std::byte> data) noexcept : data_(data) {}

  bool exhausted() const noexcept { return position_ >= data_.size() * 8; }

  unsigned bit() noexcept {
    const std::size_t index = position_ >> 3;
    const unsigned shift = 7 - static_cast<unsigned>(position_ & 7);
    ++position_;
    if (index >= data_.size()) return 0;
    return (std::to_integer<unsigned>(data_[index]) >> shift) & 1u;
  }

  std::uint32_t read(unsigned count) noexcept {
    if (count == 0) return 0;
    std::uint64_t window = 0;
    const std::size_t index = position_ >> 3;
    for (unsigned i = 0; i < 5; ++i) {
      const std::size_t at = index + i;
      window =
          (window << 8) | (at < data_.size() ? std::to_integer<std::uint64_t>(data_[at]) : 0ull);
    }
    const unsigned shift = static_cast<unsigned>(position_ & 7);
    position_ += count;
    return static_cast<std::uint32_t>((window >> (40 - shift - count)) &
                                      ((1ull << count) - 1ull));
  }

 private:
  std::span<const std::byte> data_;
  std::size_t position_ = 0;
};

/// A canonical Huffman decoder held entirely in fixed-size arrays, so decoding
/// a stream allocates nothing at all.
struct Huffman {
  std::array<std::uint16_t, kMaxCodeLength + 1> counts{};
  std::array<std::uint16_t, kLzisLiteralAlphabet> symbols{};
  bool empty = true;
};

/// Build from code lengths in symbol order. The set of codes must be exactly
/// saturated: neither short (some bit patterns would decode to nothing) nor
/// over-subscribed (two symbols would share a code). A table with no used
/// symbols at all is legal — a chunk with no matches sends an empty distance
/// table.
Status build_huffman(Huffman& table, std::span<const std::uint8_t> lengths) {
  table.counts.fill(0);
  table.empty = true;
  for (const std::uint8_t length : lengths) {
    if (length > kMaxCodeLength) return FormatError::malformed;
    if (length != 0) {
      ++table.counts[length];
      table.empty = false;
    }
  }
  if (table.empty) return {};

  int left = 1;
  for (unsigned length = 1; length <= kMaxCodeLength; ++length) {
    left <<= 1;
    left -= table.counts[length];
    if (left < 0) return FormatError::malformed;  // over-subscribed
  }
  if (left != 0) return FormatError::malformed;  // incomplete

  std::array<std::uint16_t, kMaxCodeLength + 2> offsets{};
  for (unsigned length = 1; length <= kMaxCodeLength; ++length) {
    offsets[length + 1] = static_cast<std::uint16_t>(offsets[length] + table.counts[length]);
  }
  for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
    if (lengths[symbol] != 0) {
      table.symbols[offsets[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
    }
  }
  return {};
}

/// Walk the code lengths one bit at a time, the way the canonical assignment is
/// defined: codes ascend within a length and shift left on each step up.
Result<std::uint32_t> decode_symbol(const Huffman& table, BitReader& bits) {
  if (table.empty) return FormatError::malformed;
  int code = 0;
  int first = 0;
  int index = 0;
  for (unsigned length = 1; length <= kMaxCodeLength; ++length) {
    code |= static_cast<int>(bits.bit());
    const int count = table.counts[length];
    if (code - first < count) return static_cast<std::uint32_t>(table.symbols[index + code - first]);
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return FormatError::malformed;
}

/// One code length table: a 2-bit width field (giving 2..5), then one plain
/// fixed-width field per symbol. No run-length coding, and the two tables
/// frequently choose different widths.
void read_code_lengths(BitReader& bits, std::span<std::uint8_t> lengths) {
  const unsigned width = bits.read(2) + 2;
  for (std::uint8_t& length : lengths) {
    length = static_cast<std::uint8_t>(bits.read(width));
  }
}

std::uint32_t decode_distance(BitReader& bits, std::uint32_t symbol) noexcept {
  const std::uint32_t slot = symbol >> 1;
  const std::uint32_t parity = symbol & 1;
  const unsigned extra = kDistanceExtra[slot] != 0 ? kDistanceExtra[slot] - 1u : 0u;
  return kDistanceBase[slot] + parity + 2 * bits.read(extra);
}

}  // namespace

Result<LzisHeader> parse_lzis_header(std::span<const std::byte> stream) {
  if (!has_magic(stream, kLzisMagic)) return FormatError::bad_magic;
  if (stream.size() < kLzisHeaderSize) return FormatError::truncated;

  LzisHeader header;
  header.uncompressed_size = read_u32le(stream, 4);
  header.chunk_size = read_u32le(stream, 8);
  header.level = read_u8(stream, 12);
  if (header.chunk_size == 0) return FormatError::malformed;
  return header;
}

Result<std::uint32_t> lzis_chunk_size(const LzisHeader& header, std::uint32_t index) {
  if (index >= header.chunk_count()) return FormatError::out_of_range;
  const std::uint64_t consumed = static_cast<std::uint64_t>(index) * header.chunk_size;
  const std::uint64_t left = header.uncompressed_size - consumed;
  return static_cast<std::uint32_t>(left < header.chunk_size ? left : header.chunk_size);
}

Result<std::span<const std::byte>> lzis_chunk(std::span<const std::byte> stream,
                                              const LzisHeader& header, std::uint32_t index) {
  const std::uint32_t count = header.chunk_count();
  if (index >= count) return FormatError::out_of_range;

  const std::uint64_t table_end = kLzisHeaderSize + 4ull * count;
  if (table_end > stream.size()) return FormatError::truncated;

  const std::uint32_t start = read_u32le(stream, kLzisHeaderSize + 4 * index);
  // There is no terminating table entry: the last chunk runs to the end of the
  // stream, and the reader supplies the length as the sentinel.
  const std::uint64_t end = index + 1 < count
                                ? read_u32le(stream, kLzisHeaderSize + 4 * (index + 1))
                                : stream.size();
  if (start < table_end || end < start || end > stream.size()) return FormatError::out_of_range;
  return stream.subspan(start, static_cast<std::size_t>(end - start));
}

Result<std::size_t> lzis_decompress_chunk(std::span<const std::byte> chunk,
                                          std::span<std::byte> out) {
  if (chunk.size() <= 5) return FormatError::truncated;

  BitReader bits(chunk);

  std::array<std::uint8_t, kLzisLiteralAlphabet> literal_lengths{};
  std::array<std::uint8_t, kLzisDistanceAlphabet> distance_lengths{};
  read_code_lengths(bits, literal_lengths);
  read_code_lengths(bits, distance_lengths);

  Huffman literals;
  Huffman distances;
  if (const Status status = build_huffman(literals, literal_lengths); !status) return status.error();
  if (const Status status = build_huffman(distances, distance_lengths); !status) {
    return status.error();
  }

  std::size_t written = 0;
  while (!bits.exhausted()) {
    const Result<std::uint32_t> symbol = decode_symbol(literals, bits);
    if (!symbol) return symbol.error();
    if (*symbol == kLzisEndOfBlock) break;

    if (*symbol < 256) {
      if (written == out.size()) return FormatError::malformed;
      out[written++] = static_cast<std::byte>(*symbol);
      continue;
    }

    const std::uint32_t slot = *symbol - 256;
    if (slot >= kLengthBase.size()) return FormatError::malformed;
    const std::uint32_t length = kLengthBase[slot] + bits.read(kLengthExtra[slot]);

    const Result<std::uint32_t> distance_symbol = decode_symbol(distances, bits);
    if (!distance_symbol) return distance_symbol.error();
    if (*distance_symbol >= kLzisDistanceAlphabet) return FormatError::malformed;
    const std::uint32_t distance = decode_distance(bits, *distance_symbol);

    // No match may reach back before the start of its own chunk: the window is
    // reset at every chunk boundary, which is what makes the stream seekable.
    if (distance == 0 || distance > written) return FormatError::malformed;
    if (length > out.size() - written) return FormatError::malformed;

    // Byte at a time, deliberately: a match may overlap the current output
    // position, which is how the codec expresses runs.
    std::size_t source = written - distance;
    for (std::uint32_t i = 0; i < length; ++i) out[written++] = out[source++];
  }

  if (written != out.size()) return FormatError::malformed;
  return written;
}

Result<std::size_t> lzis_decompress(std::span<const std::byte> stream, std::span<std::byte> out) {
  const Result<LzisHeader> header = parse_lzis_header(stream);
  if (!header) return header.error();
  if (out.size() < header->uncompressed_size) return FormatError::buffer_too_small;

  std::size_t written = 0;
  const std::uint32_t count = header->chunk_count();
  for (std::uint32_t index = 0; index < count; ++index) {
    const Result<std::span<const std::byte>> chunk = lzis_chunk(stream, header.value(), index);
    if (!chunk) return chunk.error();
    const Result<std::uint32_t> expected = lzis_chunk_size(header.value(), index);
    if (!expected) return expected.error();

    const std::span<std::byte> target = out.subspan(written, *expected);
    if (*expected == 0) {
      // Nothing to produce; a zero-length stream still carries one chunk entry.
    } else if (chunk->size() >= *expected) {
      // A chunk that did not compress is stored verbatim, with no flag to say
      // so: the length comparison is the flag. Anything past the uncompressed
      // length is padding.
      std::memcpy(target.data(), chunk->data(), *expected);
    } else {
      const Result<std::size_t> produced = lzis_decompress_chunk(*chunk, target);
      if (!produced) return produced.error();
    }
    written += *expected;
  }

  if (written != header->uncompressed_size) return FormatError::malformed;
  return written;
}

}  // namespace imperivm::core
