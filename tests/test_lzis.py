"""Data-free tests for the LZIS decompressor. Specification: docs/formats/lzis.md.

Every stream here is produced by the encoder in `synthetic.py`, which writes the
bitstream from the specification's own tables. A round trip therefore compares
two independent transcriptions of the format rather than the reader with itself.
LZIS is the one codec in the project with no external reference implementation --
zlib cannot read it -- so this is the only mechanical check it gets in CI.
"""

from __future__ import annotations

import struct

import pytest

import synthetic
from imperivm.formats import lzis
from imperivm.formats.lzis import LzisError

TEXT = (
    b"[system]\r\n;WindowX = 1024\r\n;WindowY = 768\r\n"
    b"DisableExceptionHandler = 0\r\n[render]\r\n;Windowed = 1\r\n"
) * 6

#: Long enough that a chunk still compresses once the ~180 bytes of code length
#: tables are paid for; below roughly 400 bytes the reader would take the stored
#: path instead, which is a different code path entirely.
BODY = TEXT * 10


def literals(payload: bytes) -> list[tuple]:
    return [("lit", byte) for byte in payload]


def stream(chunks, *, chunk_size, level=2) -> bytes:
    return synthetic.build_lzis(chunks, chunk_size=chunk_size, level=level)


# -- header --------------------------------------------------------------


def test_header_fields():
    chunk = synthetic.compressed_chunk(literals(TEXT))
    data = stream([chunk], chunk_size=32768, level=2)
    header = lzis.parse_header(data)
    assert header.uncompressed_size == len(TEXT)
    assert header.chunk_size == 32768
    assert header.level == 2
    assert header.chunk_count == 1


@pytest.mark.parametrize(
    ("size", "chunk_size", "expected"),
    [(0, 32768, 1), (1, 32768, 1), (32768, 32768, 1), (32769, 32768, 2), (100000, 32768, 4)],
)
def test_chunk_count_rounds_up_and_is_never_zero(size, chunk_size, expected):
    assert lzis.LzisHeader(size, chunk_size, 2).chunk_count == expected


def test_first_chunk_offset_is_the_first_byte_after_the_table():
    chunks = [
        synthetic.compressed_chunk(literals(BODY[:1500])),
        synthetic.compressed_chunk(literals(BODY[1500:3000])),
    ]
    data = stream(chunks, chunk_size=1500)
    header = lzis.parse_header(data)
    offsets = lzis.chunk_offsets(data, header)
    assert offsets[0] == lzis.HEADER_SIZE + 4 * header.chunk_count
    assert offsets[-1] == len(data)


# -- decoding ------------------------------------------------------------


def test_literals_only_round_trip():
    chunk = synthetic.compressed_chunk(literals(TEXT))
    assert lzis.decompress(stream([chunk], chunk_size=32768)) == TEXT


def test_matches_round_trip():
    """Exercises the length symbols, their extra bits, and the distance parity split."""
    tokens = literals(b"abcdefghij" * 4)
    tokens += [("match", 3, 1), ("match", 9, 17), ("match", 258, 40)]
    tokens += literals(b"tail")
    tokens += [("match", 27, 300), ("match", 11, 2)]
    payload = synthetic.expand_tokens(tokens)
    chunk = synthetic.LzisChunk(payload, synthetic.encode_lzis_chunk(tokens))
    assert lzis.decompress_chunk(chunk.encoded, len(payload)) == payload


def test_overlapping_match_copies_one_byte_at_a_time():
    """`distance < length` is legal and repeats the source region."""
    tokens = literals(b"ab" * 100) + [("match", 200, 2)]
    payload = synthetic.expand_tokens(tokens)
    assert payload.endswith(b"ab" * 100)
    assert lzis.decompress_chunk(synthetic.encode_lzis_chunk(tokens), len(payload)) == payload


@pytest.mark.parametrize("distance", [1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 63, 64, 300, 1000])
def test_every_distance_slot_shape_decodes(distance):
    """Slots 0-3 overlap because of the parity bit; a decoder must accept either."""
    tokens = literals(bytes((i % 251) for i in range(1200)))
    tokens.append(("match", 5, distance))
    payload = synthetic.expand_tokens(tokens)
    assert lzis.decompress_chunk(synthetic.encode_lzis_chunk(tokens), len(payload)) == payload


@pytest.mark.parametrize("length", [3, 10, 11, 12, 18, 22, 34, 66, 130, 257, 258])
def test_every_length_slot_shape_decodes(length):
    tokens = literals(bytes((i % 251) for i in range(600)))
    tokens.append(("match", length, 400))
    payload = synthetic.expand_tokens(tokens)
    assert lzis.decompress_chunk(synthetic.encode_lzis_chunk(tokens), len(payload)) == payload


def test_multiple_chunks_are_concatenated():
    parts = [BODY[i : i + 1500] for i in range(0, 6000, 1500)]
    chunks = [synthetic.compressed_chunk(literals(part)) for part in parts]
    data = stream(chunks, chunk_size=1500)
    assert lzis.parse_header(data).chunk_count == len(parts)
    assert lzis.decompress(data) == b"".join(parts)


def test_the_last_chunk_may_be_short():
    head, tail = BODY[:1500], BODY[1500:2117]
    chunks = [
        synthetic.compressed_chunk(literals(head)),
        synthetic.compressed_chunk(literals(tail)),
    ]
    assert lzis.decompress(stream(chunks, chunk_size=1500)) == head + tail


def test_stored_chunks_take_only_their_uncompressed_length():
    """There is no flag for a stored chunk; padding after it must be ignored."""
    payload = bytes(range(256))
    chunks = [synthetic.stored_chunk(payload, padding=9)]
    assert lzis.decompress(stream(chunks, chunk_size=256)) == payload


def test_stored_and_compressed_chunks_mix():
    stored = bytes(range(256)) + bytes(range(144))
    coded = TEXT[:400]
    data = stream(
        [synthetic.stored_chunk(stored), synthetic.compressed_chunk(literals(coded))],
        chunk_size=400,
    )
    assert lzis.decompress(data) == stored + coded


def test_chunks_are_independent_windows():
    """A match may not reach into the previous chunk, so the reader must reset."""
    first = synthetic.compressed_chunk(literals(TEXT[:400]))
    tokens = literals(b"zyxwvu" * 60) + [("match", 30, 6)]
    second = synthetic.LzisChunk(
        synthetic.expand_tokens(tokens), synthetic.encode_lzis_chunk(tokens)
    )
    data = stream([first, second], chunk_size=400)
    assert lzis.decompress(data) == first.payload + second.payload


def test_an_empty_distance_table_is_legal():
    """A chunk with no matches transmits 60 zero code lengths."""
    encoded = synthetic.encode_lzis_chunk(literals(TEXT[:400]))
    assert lzis.decompress_chunk(encoded, 400) == TEXT[:400]


# -- error paths ---------------------------------------------------------


def test_rejects_bad_magic():
    with pytest.raises(LzisError, match="magic mismatch"):
        lzis.parse_header(b"LZSS" + b"\0" * 32)


def test_rejects_a_stream_shorter_than_its_header():
    with pytest.raises(LzisError, match="shorter than its header"):
        lzis.parse_header(b"LZIS" + b"\0" * 4)


def test_rejects_zero_chunk_size():
    with pytest.raises(LzisError, match="chunk size is zero"):
        lzis.parse_header(b"LZIS" + struct.pack("<II", 16, 0) + b"\0\0")


def test_rejects_a_chunk_table_that_does_not_fit():
    data = b"LZIS" + struct.pack("<II", 1 << 20, 32768) + b"\x02\x00"
    with pytest.raises(LzisError, match="chunk table needs"):
        lzis.chunk_offsets(data, lzis.parse_header(data))


def test_rejects_a_chunk_offset_inside_the_table():
    chunk = synthetic.compressed_chunk(literals(TEXT))
    data = bytearray(stream([chunk], chunk_size=32768))
    struct.pack_into("<I", data, lzis.HEADER_SIZE, 4)
    with pytest.raises(LzisError, match="out of range"):
        lzis.decompress(bytes(data))


def test_rejects_an_unsaturated_huffman_table():
    """285 symbols of one length cannot form a complete code."""
    writer = synthetic._BitWriter()
    writer.write(2, 2)  # field width 4
    for symbol in range(synthetic.LITERAL_ALPHABET):
        writer.write(4 if symbol < 15 else 0, 4)
    writer.write(0, 2)
    for _ in range(synthetic.DISTANCE_ALPHABET):
        writer.write(0, 2)
    with pytest.raises(LzisError, match="do not form a complete code"):
        lzis.decompress_chunk(writer.finish() + b"\0" * 8, 16)


def test_rejects_a_match_reaching_before_the_start_of_the_chunk():
    tokens = literals(b"abc" * 200)
    encoded = bytearray(synthetic.encode_lzis_chunk(tokens))
    # Rebuild with a legal encoder, then decode against a chunk that has not
    # produced enough output yet by claiming a much smaller expected size.
    tokens = literals(b"abc" * 200) + [("match", 3, 700)]
    with pytest.raises(LzisError, match="reaches before the start"):
        lzis.decompress_chunk(synthetic.encode_lzis_chunk(tokens), 603)
    assert encoded  # the legal encoding above is unaffected


def test_rejects_a_match_overrunning_the_declared_chunk_length():
    tokens = literals(b"abcdef" * 40) + [("match", 100, 6)]
    with pytest.raises(LzisError, match="overruns the end of the chunk"):
        lzis.decompress_chunk(synthetic.encode_lzis_chunk(tokens), 250)


def test_rejects_a_chunk_that_decodes_to_the_wrong_length():
    encoded = synthetic.encode_lzis_chunk(literals(TEXT[:400]))
    with pytest.raises(LzisError, match="expected 399"):
        lzis.decompress_chunk(encoded, 399)


def test_rejects_a_chunk_too_short_to_hold_a_block():
    with pytest.raises(LzisError, match="too short to hold a block"):
        lzis.decompress_chunk(b"\0" * 5, 10)


def test_rejects_a_stream_whose_total_disagrees_with_the_header():
    chunk = synthetic.compressed_chunk(literals(TEXT))
    data = bytearray(stream([chunk], chunk_size=32768))
    struct.pack_into("<I", data, 4, len(TEXT) + 1)
    with pytest.raises(LzisError):
        lzis.decompress(bytes(data))
