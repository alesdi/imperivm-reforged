// LZIS tests.
//
// These build real bitstreams rather than fixtures, which means the test file
// contains a small encoder. That is deliberate: the four things LZIS does
// differently from DEFLATE (MSB-first bits, plain fixed-width code length
// tables with their own widths, the literal alphabet shifted so end-of-block is
// 285, and distance slots split by parity) are all encoder-visible, so writing
// one is the cheapest way to pin every one of them down without game data.

#include <string>
#include <vector>

#include "builder.hpp"
#include "imperivm/core/formats/lzis.hpp"
#include "test.hpp"

using namespace imperivm::core;
using imperivm::test::Builder;

namespace {

/// Most significant bit first within each byte, which is the opposite of
/// DEFLATE and the single most likely thing for a port to get backwards.
class BitWriter {
 public:
  void write(std::uint32_t value, unsigned count) {
    for (unsigned i = count; i-- > 0;) push((value >> i) & 1u);
  }
  void push(unsigned bit) {
    if (used_ == 0) {
      bytes_.push_back(std::byte{0});
      used_ = 8;
    }
    --used_;
    if (bit != 0) {
      bytes_.back() |= static_cast<std::byte>(1u << used_);
    }
  }
  const std::vector<std::byte>& bytes() const { return bytes_; }

 private:
  std::vector<std::byte> bytes_;
  unsigned used_ = 0;
};

/// Emit one code length table: a 2-bit width field holding `width - 2`, then
/// one plain field per symbol, in symbol order. No run-length coding.
void write_lengths(BitWriter& bits, const std::vector<std::uint8_t>& lengths, unsigned width) {
  bits.write(width - 2, 2);
  for (const std::uint8_t length : lengths) bits.write(length, width);
}

/// A chunk coding the literals 'A' and 'B', then sixty 3-byte matches at
/// distance 2, producing 182 bytes of alternating "ABAB...".
///
/// **Sixty, not one.** The obvious fixture -- two literals and a single match,
/// producing "ABABA" -- cannot be decoded by any correct reader, and this file
/// shipped it for a while and blamed the codec. LZIS has **no flag for a stored
/// chunk**: a reader decides by comparing the chunk's size against the
/// uncompressed length the header declares, and treats anything not smaller as
/// stored. A five-byte output behind an eighty-nine-byte code length table is
/// therefore a stored chunk by definition, and `lzis_decompress` was right to
/// hand back the first five bytes of the table.
///
/// The tables cost 87 bytes whatever the payload does, so a fixture that
/// exercises the compressed path at all has to produce more than that. Sixty
/// matches make 182 bytes of output from a 111-byte chunk, which is comfortably
/// on the right side of the comparison and still exercises the overlapping
/// copy: every match reads bytes this same match is writing.
///
/// The literal table uses four symbols of length 2, so the canonical codes are
/// assigned in symbol order: 'A'=00, 'B'=01, 256 (a length-3 match) = 10, and
/// 285 (end of block) = 11. Note 285, not 256, is the terminator.
///
/// The distance table uses two symbols of length 1, because a Huffman code must
/// be exactly saturated and a single symbol cannot be: symbol 2 is slot 1 with
/// parity 0, which is distance 2 with no extra bits.
/// Sixty matches of three bytes each, after two literals.
constexpr int kFixtureMatches = 60;
constexpr std::size_t kFixtureSize = 2 + 3 * static_cast<std::size_t>(kFixtureMatches);

std::vector<std::byte> compressed_chunk() {
  std::vector<std::uint8_t> literals(kLzisLiteralAlphabet, 0);
  literals['A'] = 2;
  literals['B'] = 2;
  literals[256] = 2;
  literals[kLzisEndOfBlock] = 2;

  std::vector<std::uint8_t> distances(kLzisDistanceAlphabet, 0);
  distances[2] = 1;
  distances[3] = 1;

  BitWriter bits;
  write_lengths(bits, literals, 2);
  write_lengths(bits, distances, 2);

  bits.write(0b00, 2);  // literal 'A'
  bits.write(0b01, 2);  // literal 'B'
  for (int i = 0; i < kFixtureMatches; ++i) {
    bits.write(0b10, 2);  // length symbol 256: base 3, no extra bits
    bits.write(0b0, 1);   // distance symbol 2: slot 1, parity 0, distance 2
  }
  bits.write(0b11, 2);  // end of block
  return bits.bytes();
}

/// What `compressed_chunk` decodes to: `kFixtureSize` bytes of "ABAB...".
std::string fixture_text() {
  std::string text;
  for (std::size_t i = 0; i < kFixtureSize; ++i) text.push_back((i % 2) == 0 ? 'A' : 'B');
  return text;
}

/// Wrap one chunk payload in a stream header.
Builder stream_of(const std::vector<std::byte>& payload, std::uint32_t uncompressed_size,
                  std::uint32_t chunk_size = 32768) {
  Builder stream;
  stream.text("LZIS").u32(uncompressed_size).u32(chunk_size).u8(2).u8(0);
  stream.u32(kLzisHeaderSize + 4);  // one chunk, starting right after the table
  for (const std::byte b : payload) stream.u8(static_cast<std::uint8_t>(b));
  return stream;
}

bool equals(std::span<const std::byte> data, std::string_view text) {
  if (data.size() != text.size()) return false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (static_cast<char>(data[i]) != text[i]) return false;
  }
  return true;
}

}  // namespace

TEST(lzis_parses_a_header) {
  const Builder stream = stream_of(compressed_chunk(), kFixtureSize);
  const auto header = parse_lzis_header(stream.span());
  CHECK(header.ok());
  CHECK(header->uncompressed_size == kFixtureSize);
  CHECK(header->chunk_size == 32768);
  CHECK(header->level == 2);
  CHECK(header->chunk_count() == 1);
}

TEST(lzis_chunk_count_rounds_up_and_never_reaches_zero) {
  LzisHeader header;
  header.chunk_size = 32768;
  header.uncompressed_size = 0;
  CHECK(header.chunk_count() == 1);  // an empty stream still carries one entry
  header.uncompressed_size = 1;
  CHECK(header.chunk_count() == 1);
  header.uncompressed_size = 32768;
  CHECK(header.chunk_count() == 1);
  header.uncompressed_size = 32769;
  CHECK(header.chunk_count() == 2);
}

TEST(lzis_decodes_literals_and_an_overlapping_match) {
  const Builder stream = stream_of(compressed_chunk(), kFixtureSize);
  std::vector<std::byte> out(kFixtureSize);
  const auto written = lzis_decompress(stream.span(), out);
  REQUIRE(written.ok());
  CHECK(*written == kFixtureSize);
  // Every match is length 3 at distance 2, so it reads bytes it is itself
  // writing and has to be copied one byte at a time.
  CHECK(equals(out, fixture_text()));
}

TEST(lzis_stores_a_chunk_that_did_not_compress) {
  // There is no flag for a stored chunk: the reader compares the stored length
  // against the uncompressed length, and anything past the latter is padding.
  Builder stream;
  stream.text("LZIS").u32(4).u32(32768).u8(2).u8(0);
  stream.u32(kLzisHeaderSize + 4);
  stream.text("raw!padding");

  std::vector<std::byte> out(4);
  const auto written = lzis_decompress(stream.span(), out);
  CHECK(written.ok());
  CHECK(equals(out, "raw!"));
}

TEST(lzis_rejects_a_foreign_stream) {
  Builder other;
  other.text("LZSS").zeros(32);
  CHECK(parse_lzis_header(other.span()).error() == FormatError::bad_magic);
}

TEST(lzis_rejects_a_zero_chunk_size) {
  Builder stream;
  stream.text("LZIS").u32(16).u32(0).u8(2).u8(0).u32(18);
  // A zero chunk size would make the chunk count divide by zero.
  CHECK(parse_lzis_header(stream.span()).error() == FormatError::malformed);
}

TEST(lzis_rejects_a_truncated_header) {
  Builder stream;
  stream.text("LZIS").u32(16);
  CHECK(parse_lzis_header(stream.span()).error() == FormatError::truncated);
}

TEST(lzis_rejects_a_chunk_offset_inside_its_own_table) {
  Builder stream = stream_of(compressed_chunk(), kFixtureSize);
  stream.patch_u32(kLzisHeaderSize, 4);  // points back into the header
  std::vector<std::byte> out(kFixtureSize);
  CHECK(lzis_decompress(stream.span(), out).error() == FormatError::out_of_range);
}

TEST(lzis_rejects_a_chunk_offset_past_the_end) {
  Builder stream = stream_of(compressed_chunk(), kFixtureSize);
  stream.patch_u32(kLzisHeaderSize, 0xFFFFFF00);
  std::vector<std::byte> out(kFixtureSize);
  CHECK(lzis_decompress(stream.span(), out).error() == FormatError::out_of_range);
}

TEST(lzis_rejects_an_output_buffer_that_is_too_small) {
  const Builder stream = stream_of(compressed_chunk(), kFixtureSize);
  std::vector<std::byte> out(kFixtureSize - 1);
  CHECK(lzis_decompress(stream.span(), out).error() == FormatError::buffer_too_small);
}

TEST(lzis_rejects_a_chunk_that_decodes_short) {
  // Every chunk must produce exactly the length the header implies.
  //
  // The declared length has to stay **above** the chunk's own size, or the
  // reader classifies the chunk as stored and copies it verbatim instead of
  // decoding it -- there is no flag, only the comparison. See
  // `compressed_chunk`.
  const Builder stream = stream_of(compressed_chunk(), kFixtureSize + 64);
  std::vector<std::byte> out(kFixtureSize + 64);
  CHECK(lzis_decompress(stream.span(), out).error() == FormatError::malformed);
}

TEST(lzis_rejects_an_over_subscribed_huffman_table) {
  // Three symbols of length 1 cannot all have distinct codes.
  std::vector<std::uint8_t> literals(kLzisLiteralAlphabet, 0);
  literals['A'] = 1;
  literals['B'] = 1;
  literals[kLzisEndOfBlock] = 1;

  BitWriter bits;
  write_lengths(bits, literals, 2);
  write_lengths(bits, std::vector<std::uint8_t>(kLzisDistanceAlphabet, 0), 2);
  bits.write(0, 8);

  std::vector<std::byte> out(1);
  CHECK(lzis_decompress_chunk(bits.bytes(), out).error() == FormatError::malformed);
}

TEST(lzis_rejects_an_incomplete_huffman_table) {
  // One symbol of length 1 leaves the code space half unused, and the engine
  // rejects a table that is not exactly saturated.
  std::vector<std::uint8_t> literals(kLzisLiteralAlphabet, 0);
  literals[kLzisEndOfBlock] = 1;

  BitWriter bits;
  write_lengths(bits, literals, 2);
  write_lengths(bits, std::vector<std::uint8_t>(kLzisDistanceAlphabet, 0), 2);
  bits.write(0, 8);

  std::vector<std::byte> out(1);
  CHECK(lzis_decompress_chunk(bits.bytes(), out).error() == FormatError::malformed);
}

TEST(lzis_rejects_a_match_reaching_before_the_start_of_the_chunk) {
  // The window resets at every chunk boundary, which is what makes the stream
  // seekable: a match may never reach back past the chunk's own first byte.
  std::vector<std::uint8_t> literals(kLzisLiteralAlphabet, 0);
  literals['A'] = 2;
  literals['B'] = 2;
  literals[256] = 2;
  literals[kLzisEndOfBlock] = 2;
  std::vector<std::uint8_t> distances(kLzisDistanceAlphabet, 0);
  distances[2] = 1;
  distances[3] = 1;

  BitWriter bits;
  write_lengths(bits, literals, 2);
  write_lengths(bits, distances, 2);
  bits.write(0b00, 2);  // one literal, so only one byte precedes the match
  bits.write(0b10, 2);  // a length-3 match
  bits.write(0b0, 1);   // at distance 2, which is one byte too far back
  bits.write(0b11, 2);

  std::vector<std::byte> out(4);
  CHECK(lzis_decompress_chunk(bits.bytes(), out).error() == FormatError::malformed);
}

TEST(lzis_rejects_a_chunk_too_short_to_hold_a_block) {
  const std::vector<std::byte> tiny(4, std::byte{0});
  std::vector<std::byte> out(1);
  CHECK(lzis_decompress_chunk(tiny, out).error() == FormatError::truncated);
}

TEST(lzis_decodes_an_empty_stream) {
  Builder stream;
  stream.text("LZIS").u32(0).u32(32768).u8(2).u8(0).u32(kLzisHeaderSize + 4);
  std::vector<std::byte> out(1);
  const auto written = lzis_decompress(stream.span(), out);
  CHECK(written.ok());
  CHECK(*written == 0);
}
