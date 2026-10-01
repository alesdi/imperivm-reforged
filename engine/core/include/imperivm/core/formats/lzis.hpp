#pragma once

// Decompressor for the LZIS codec.
//
// Specification: docs/formats/lzis.md
//
// DEFLATE-family but not DEFLATE, and every one of the differences silently
// produces plausible-looking garbage rather than an error if it is missed:
//
//   * bits are consumed most-significant-first within a byte (DEFLATE: least);
//   * the two Huffman tables are plain fixed-width code-length arrays, each
//     with its own field width, not run-length coded with a third table;
//   * the literal/length alphabet is shifted down one symbol, so lengths start
//     at 256 and end-of-block is 285 (DEFLATE: 257 and 256);
//   * the distance alphabet is DEFLATE's 30 slots split by parity into 60
//     symbols, each carrying one fewer extra bit.
//
// The codec wraps whole files (`config.ini`, `RandomMap.pak`) and never
// individual entries inside an archive. Output goes into a caller-supplied
// buffer: the caller can size it exactly from the header, and a 47 MB stream is
// not something a decoder should be allocating behind the engine's back.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core {

inline constexpr std::string_view kLzisMagic = "LZIS";
inline constexpr std::size_t kLzisHeaderSize = 14;

inline constexpr std::size_t kLzisLiteralAlphabet = 286;
inline constexpr std::size_t kLzisDistanceAlphabet = 60;
inline constexpr std::uint32_t kLzisEndOfBlock = 285;

struct LzisHeader {
  std::uint32_t uncompressed_size = 0;
  std::uint32_t chunk_size = 0;
  /// How hard the *encoder* searched for matches (1..3). It does not change the
  /// bitstream and the decoder ignores it; every retail stream is level 2.
  std::uint8_t level = 0;

  /// Chunks the payload is split into, always at least one: a zero-length
  /// stream still carries one empty chunk entry.
  constexpr std::uint32_t chunk_count() const noexcept {
    if (uncompressed_size == 0) return 1;
    return (uncompressed_size - 1) / chunk_size + 1;
  }
};

Result<LzisHeader> parse_lzis_header(std::span<const std::byte> stream);

/// Byte range of chunk `index` within the stream. Chunks are independent: the
/// LZ77 window resets at every boundary, which is what makes the stream
/// seekable at chunk granularity.
Result<std::span<const std::byte>> lzis_chunk(std::span<const std::byte> stream,
                                              const LzisHeader& header, std::uint32_t index);

/// Uncompressed length of chunk `index` (the last one is short).
Result<std::uint32_t> lzis_chunk_size(const LzisHeader& header, std::uint32_t index);

/// Decode one compressed chunk into `out`, which must be exactly the chunk's
/// uncompressed length. Returns the bytes written, always `out.size()`.
Result<std::size_t> lzis_decompress_chunk(std::span<const std::byte> chunk,
                                          std::span<std::byte> out);

/// Decode a whole stream into `out`, which must hold `uncompressed_size` bytes.
Result<std::size_t> lzis_decompress(std::span<const std::byte> stream, std::span<std::byte> out);

}  // namespace imperivm::core
