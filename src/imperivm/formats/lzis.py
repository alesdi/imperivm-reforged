"""Decompressor for the LZIS compression container.

Specification: docs/formats/lzis.md

Reference implementation: correctness and legibility over speed.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"LZIS"
HEADER_SIZE = 14

LITERAL_ALPHABET = 286
DISTANCE_ALPHABET = 60
END_OF_BLOCK = 285

#: Match length for literal/length symbols 256..284, and the extra bits that
#: follow each. Identical to DEFLATE's, shifted down by one symbol.
LENGTH_BASE = (
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
)
LENGTH_EXTRA = (
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
)

#: DEFLATE's 30 distance slots. An LZIS distance symbol is a slot number
#: paired with a parity bit; see :func:`_decode_distance`.
DISTANCE_BASE = (
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577,
)
DISTANCE_EXTRA = (
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
)


class LzisError(Exception):
    """Raised when a stream does not conform to the LZIS format."""


@dataclass(frozen=True)
class LzisHeader:
    """The fixed part of an LZIS stream header."""

    uncompressed_size: int
    chunk_size: int
    level: int

    @property
    def chunk_count(self) -> int:
        """Number of chunks the payload is split into (at least one)."""
        if self.uncompressed_size == 0:
            return 1
        return (self.uncompressed_size - 1) // self.chunk_size + 1


# -- bit reader ----------------------------------------------------------
#
# Bits are consumed most-significant-first within each byte, and bytes are
# consumed in order. This is the opposite of DEFLATE's bit order.


class _BitReader:
    __slots__ = ("_data", "_pos")

    def __init__(self, data: bytes) -> None:
        # The decoder loads four bytes at a time, so the last symbol of a
        # chunk may legitimately reach past the final byte.
        self._data = data + b"\x00" * 4
        self._pos = 0

    @property
    def exhausted(self) -> bool:
        return self._pos >= (len(self._data) - 4) * 8

    def read(self, count: int) -> int:
        """Consume ``count`` bits and return them as an integer."""
        if count == 0:
            return 0
        start = self._pos
        byte = start >> 3
        window = int.from_bytes(self._data[byte : byte + 5], "big")
        self._pos = start + count
        return (window >> (40 - (start & 7) - count)) & ((1 << count) - 1)


# -- Huffman -------------------------------------------------------------


class _Huffman:
    """A canonical Huffman decoder over a fixed alphabet.

    Codes are assigned in order of increasing length, and within one length in
    increasing symbol order, starting from zero. The complete set of codes must
    be exactly saturated.
    """

    __slots__ = ("max_length", "_lookup")

    def __init__(self, lengths: list[int]) -> None:
        self.max_length = max(lengths)
        if self.max_length == 0:
            # A table with no used symbols. Legal: a chunk that codes no
            # matches at all transmits an empty distance table.
            self._lookup: dict[tuple[int, int], int] = {}
            return

        lookup = {}
        code = 0
        for length in range(1, self.max_length + 1):
            code <<= 1
            for symbol, symbol_length in enumerate(lengths):
                if symbol_length == length:
                    lookup[(length, code)] = symbol
                    code += 1
        if code & (code - 1):
            raise LzisError("Huffman code lengths do not form a complete code")
        self._lookup = lookup

    def decode(self, bits: _BitReader) -> int:
        code = 0
        for length in range(1, self.max_length + 1):
            code = (code << 1) | bits.read(1)
            symbol = self._lookup.get((length, code))
            if symbol is not None:
                return symbol
        raise LzisError("no Huffman code matches the bits at this position")


def _read_code_lengths(bits: _BitReader, count: int) -> list[int]:
    """Read one code length table: a 2-bit field width, then fixed fields."""
    width = bits.read(2) + 2
    return [bits.read(width) for _ in range(count)]


# -- chunk decoder -------------------------------------------------------


def _decode_distance(bits: _BitReader, symbol: int) -> int:
    """Turn a distance symbol plus its extra bits into a match distance.

    The 60 distance symbols are DEFLATE's 30 distance slots split by parity:
    symbol ``2 * slot + parity`` covers the distances of ``slot`` that have
    that parity, so one fewer extra bit is needed per slot.
    """
    slot, parity = symbol >> 1, symbol & 1
    extra_bits = max(DISTANCE_EXTRA[slot] - 1, 0)
    return DISTANCE_BASE[slot] + parity + 2 * bits.read(extra_bits)


def decompress_chunk(data: bytes, expected_size: int) -> bytes:
    """Decompress one LZIS chunk. Each chunk is an independent LZ77 window."""
    if len(data) <= 5:
        raise LzisError(f"chunk is {len(data)} bytes, too short to hold a block")

    bits = _BitReader(data)
    literals = _Huffman(_read_code_lengths(bits, LITERAL_ALPHABET))
    distances = _Huffman(_read_code_lengths(bits, DISTANCE_ALPHABET))

    out = bytearray()
    while not bits.exhausted:
        symbol = literals.decode(bits)
        if symbol == END_OF_BLOCK:
            break
        if symbol < 256:
            out.append(symbol)
            continue

        slot = symbol - 256
        length = LENGTH_BASE[slot] + bits.read(LENGTH_EXTRA[slot])
        distance = _decode_distance(bits, distances.decode(bits))
        if distance > len(out):
            raise LzisError(
                f"match distance {distance} reaches before the start of the chunk"
            )
        if len(out) + length > expected_size:
            raise LzisError("match overruns the end of the chunk")
        # Deliberately byte at a time: distance may be smaller than length.
        for _ in range(length):
            out.append(out[-distance])

    if len(out) != expected_size:
        raise LzisError(f"chunk decoded to {len(out)} bytes, expected {expected_size}")
    return bytes(out)


# -- container -----------------------------------------------------------


def parse_header(data: bytes) -> LzisHeader:
    """Read the 14-byte stream header. Does not decompress anything."""
    if data[: len(MAGIC)] != MAGIC:
        raise LzisError("not an LZIS stream (magic mismatch)")
    if len(data) < HEADER_SIZE:
        raise LzisError("stream is shorter than its header")
    uncompressed_size, chunk_size = struct.unpack_from("<II", data, 4)
    if chunk_size == 0:
        raise LzisError("chunk size is zero")
    return LzisHeader(uncompressed_size, chunk_size, data[12])


def chunk_offsets(data: bytes, header: LzisHeader) -> list[int]:
    """Return the chunk start offsets, plus a final sentinel of ``len(data)``."""
    count = header.chunk_count
    table_end = HEADER_SIZE + 4 * count
    if table_end > len(data):
        raise LzisError(f"chunk table needs {table_end} bytes, stream has {len(data)}")

    offsets = list(struct.unpack_from(f"<{count}I", data, HEADER_SIZE))
    offsets.append(len(data))
    for i in range(count):
        if not table_end <= offsets[i] <= offsets[i + 1]:
            raise LzisError(f"chunk {i} offset {offsets[i]} is out of range")
    return offsets


def decompress(data: bytes) -> bytes:
    """Decompress a complete LZIS stream."""
    header = parse_header(data)
    offsets = chunk_offsets(data, header)

    out = bytearray()
    for i in range(header.chunk_count):
        stored = data[offsets[i] : offsets[i + 1]]
        expected = min(header.chunk_size, header.uncompressed_size - i * header.chunk_size)
        if len(stored) >= expected:
            # Chunks that did not compress are held verbatim, and only the
            # first `expected` bytes belong to them.
            out += stored[:expected]
        else:
            out += decompress_chunk(stored, expected)

    if len(out) != header.uncompressed_size:
        raise LzisError(
            f"stream decoded to {len(out)} bytes, header says {header.uncompressed_size}"
        )
    return bytes(out)


def main() -> None:
    import argparse
    import sys

    parser = argparse.ArgumentParser(description="Decompress an LZIS stream.")
    parser.add_argument("stream", type=Path)
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        help="write the decompressed bytes here, or '-' for stdout",
    )
    args = parser.parse_args()

    data = args.stream.read_bytes()
    header = parse_header(data)

    if args.output is None:
        ratio = len(data) / header.uncompressed_size if header.uncompressed_size else 0
        print(f"{args.stream}: LZIS stream")
        print(f"  compressed        {len(data)}")
        print(f"  uncompressed      {header.uncompressed_size}")
        print(f"  chunk size        {header.chunk_size}")
        print(f"  chunks            {header.chunk_count}")
        print(f"  compression level {header.level}")
        print(f"  ratio             {ratio:.3f}")
        decompress(data)
        print("  decodes cleanly")
        return

    result = decompress(data)
    if str(args.output) == "-":
        sys.stdout.buffer.write(result)
    else:
        args.output.write_bytes(result)


if __name__ == "__main__":
    main()
