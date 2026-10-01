"""Builders that construct valid files of each format, in memory, from nothing.

The repository ships no game assets, so every data-free test needs a fixture it
can manufacture. These builders are deliberately written *from the
specifications in `docs/formats/`* rather than by calling the readers: constants
such as the LZIS length and distance tables are typed in again here so that a
round trip actually cross-checks two independent transcriptions of the spec
instead of agreeing with itself.

Nothing in this module reads from disk.
"""

from __future__ import annotations

import heapq
import struct
import zlib
from dataclasses import dataclass

# ===========================================================================
# HMMSYS pack  --  docs/formats/pak.md
# ===========================================================================

PACK_MAGIC = b"HMMSYS PackFile\n"


def build_pack(files: list[tuple[str, bytes]], *, gap: int = 0) -> bytes:
    """Serialise `[(name, payload), ...]` as a HMMSYS pack.

    Names are front-coded against the previous entry, exactly as the format
    requires. `gap` inserts that many filler bytes between stored payloads,
    which is what the retail packs do and what the spec warns readers not to
    infer sizes from.
    """
    encoded = [(name.encode("cp1252"), payload) for name, payload in files]

    table = bytearray()
    previous = b""
    # Two passes: the entry table's own length is needed to know where the data
    # starts, and the table size does not depend on the offsets it stores.
    for name, _ in encoded:
        shared = 0
        while shared < min(len(name), len(previous), 255) and name[shared] == previous[shared]:
            shared += 1
        table += bytes((len(name), shared)) + name[shared:] + b"\0" * 8
        previous = name

    data_start = 0x28 + len(table)

    table = bytearray()
    blobs = bytearray()
    previous = b""
    for name, payload in encoded:
        shared = 0
        while shared < min(len(name), len(previous), 255) and name[shared] == previous[shared]:
            shared += 1
        offset = data_start + len(blobs)
        table += bytes((len(name), shared)) + name[shared:]
        table += struct.pack("<II", offset, len(payload))
        blobs += payload
        if gap:
            blobs += b"\xaa" * gap
        previous = name

    if gap and encoded:
        blobs = blobs[: len(blobs) - gap]  # the last entry must end at EOF

    header = PACK_MAGIC + b"\x1a" + b"\0" * 15
    header += struct.pack("<II", len(encoded), len(table))
    return bytes(header + table + blobs)


# ===========================================================================
# LZIS  --  docs/formats/lzis.md
# ===========================================================================
#
# The tables below are transcribed from the specification's own tables, not
# imported from the reader, so that a round trip compares two readings.

_LENGTH_BASE = (
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
)
_LENGTH_EXTRA = (
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
)
_DIST_BASE = (
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577,
)
_DIST_EXTRA = (
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
)

LZIS_MAGIC = b"LZIS"
LZIS_HEADER_SIZE = 14
LITERAL_ALPHABET = 286
DISTANCE_ALPHABET = 60
END_OF_BLOCK = 285


class _BitWriter:
    """Writes bits most-significant-first within each byte (LZIS's order)."""

    def __init__(self) -> None:
        self._acc = 0
        self._bits = 0
        self._out = bytearray()

    def write(self, value: int, count: int) -> None:
        for shift in range(count - 1, -1, -1):
            self._acc = (self._acc << 1) | ((value >> shift) & 1)
            self._bits += 1
            if self._bits == 8:
                self._out.append(self._acc)
                self._acc = 0
                self._bits = 0

    def finish(self) -> bytes:
        if self._bits:
            self._out.append(self._acc << (8 - self._bits))
            self._acc = 0
            self._bits = 0
        return bytes(self._out)


def _canonical(lengths: list[int]) -> dict[int, tuple[int, int]]:
    """Assign canonical codes: shortest first, then by symbol number."""
    codes: dict[int, tuple[int, int]] = {}
    if not any(lengths):
        return codes
    code = 0
    for length in range(1, max(lengths) + 1):
        code <<= 1
        for symbol, symbol_length in enumerate(lengths):
            if symbol_length == length:
                codes[symbol] = (length, code)
                code += 1
    assert code and not (code & (code - 1)), "code lengths are not exactly saturated"
    return codes


def _huffman_lengths(frequency: dict[int, int], alphabet: int) -> list[int]:
    """Code lengths from symbol frequencies.

    A Huffman code satisfies the Kraft equality, so the canonical code built from
    these lengths is exactly saturated -- which is what the decoder insists on. A
    table with a single used symbol is padded to two leaves, since one code of
    length zero is not a code.
    """
    used = sorted(s for s, f in frequency.items() if f > 0)
    if not used:
        return [0] * alphabet
    if len(used) == 1:
        used.append(next(s for s in range(alphabet) if s != used[0]))

    heap = [(max(frequency.get(s, 0), 1), i, [s]) for i, s in enumerate(used)]
    heapq.heapify(heap)
    lengths = dict.fromkeys(used, 0)
    tie = len(heap)
    while len(heap) > 1:
        weight_a, _, group_a = heapq.heappop(heap)
        weight_b, _, group_b = heapq.heappop(heap)
        merged = group_a + group_b
        for symbol in merged:
            lengths[symbol] += 1
        heapq.heappush(heap, (weight_a + weight_b, tie, merged))
        tie += 1

    if max(lengths.values()) > 31:  # pragma: no cover - needs pathological input
        raise ValueError("code lengths exceed the 31 the field width can express")

    out = [0] * alphabet
    for symbol, length in lengths.items():
        out[symbol] = length
    return out


def _field_width(lengths: list[int]) -> int:
    """The 2..5 bit field width that can hold every code length in the table."""
    longest = max(lengths) if lengths else 0
    return max(2, longest.bit_length())


def _length_symbol(length: int) -> tuple[int, int, int]:
    """`(symbol, extra_value, extra_bits)` for a match of `length` bytes."""
    for slot in range(len(_LENGTH_BASE) - 1, -1, -1):
        if _LENGTH_BASE[slot] <= length:
            extra = length - _LENGTH_BASE[slot]
            if extra < (1 << _LENGTH_EXTRA[slot]):
                return 256 + slot, extra, _LENGTH_EXTRA[slot]
            break
    raise ValueError(f"match length {length} is not encodable")


def _distance_symbol(distance: int) -> tuple[int, int, int]:
    """`(symbol, extra_value, extra_bits)` for a match distance.

    Slots are split by parity, so the symbol is `2 * slot + parity` and the
    remaining magnitude is halved before being written as extra bits.
    """
    for slot in range(len(_DIST_BASE) - 1, -1, -1):
        if _DIST_BASE[slot] <= distance:
            delta = distance - _DIST_BASE[slot]
            parity, magnitude = delta & 1, delta >> 1
            bits = max(_DIST_EXTRA[slot] - 1, 0)
            if magnitude < (1 << bits):
                return 2 * slot + parity, magnitude, bits
            break
    raise ValueError(f"match distance {distance} is not encodable")


#: A token is ``("lit", byte)`` or ``("match", length, distance)``.
Token = tuple


def expand_tokens(tokens: list[Token]) -> bytes:
    """The bytes a token list decodes to. The oracle for a round trip."""
    out = bytearray()
    for token in tokens:
        if token[0] == "lit":
            out.append(token[1])
        else:
            _, length, distance = token
            if distance > len(out):
                raise ValueError("match reaches before the start of the chunk")
            for _ in range(length):  # one at a time: distance may be < length
                out.append(out[-distance])
    return bytes(out)


def encode_lzis_chunk(tokens: list[Token]) -> bytes:
    """Encode one compressed LZIS chunk from an explicit token list."""
    literals: dict[int, int] = {END_OF_BLOCK: 1}
    distances: dict[int, int] = {}
    for token in tokens:
        if token[0] == "lit":
            literals[token[1]] = literals.get(token[1], 0) + 1
        else:
            symbol = _length_symbol(token[1])[0]
            literals[symbol] = literals.get(symbol, 0) + 1
            symbol = _distance_symbol(token[2])[0]
            distances[symbol] = distances.get(symbol, 0) + 1

    literal_lengths = _huffman_lengths(literals, LITERAL_ALPHABET)
    distance_lengths = _huffman_lengths(distances, DISTANCE_ALPHABET)
    literal_codes = _canonical(literal_lengths)
    distance_codes = _canonical(distance_lengths)

    writer = _BitWriter()
    for lengths, alphabet in (
        (literal_lengths, LITERAL_ALPHABET),
        (distance_lengths, DISTANCE_ALPHABET),
    ):
        width = _field_width(lengths)
        writer.write(width - 2, 2)
        for symbol in range(alphabet):
            writer.write(lengths[symbol], width)

    def emit(codes, symbol):
        length, code = codes[symbol]
        writer.write(code, length)

    for token in tokens:
        if token[0] == "lit":
            emit(literal_codes, token[1])
        else:
            symbol, extra, bits = _length_symbol(token[1])
            emit(literal_codes, symbol)
            writer.write(extra, bits)
            symbol, extra, bits = _distance_symbol(token[2])
            emit(distance_codes, symbol)
            writer.write(extra, bits)
    emit(literal_codes, END_OF_BLOCK)
    return writer.finish()


@dataclass
class LzisChunk:
    """One chunk of a synthetic stream: either token-coded or stored verbatim."""

    payload: bytes  # what it must decode to
    encoded: bytes  # what goes on disk


def compressed_chunk(tokens: list[Token]) -> LzisChunk:
    payload = expand_tokens(tokens)
    encoded = encode_lzis_chunk(tokens)
    if len(encoded) >= len(payload):
        raise ValueError(
            f"encoded chunk is {len(encoded)} bytes for {len(payload)} of output; the reader "
            "would take the stored path. Use a longer or more repetitive payload."
        )
    return LzisChunk(payload, encoded)


def stored_chunk(payload: bytes, *, padding: int = 3) -> LzisChunk:
    """A chunk held verbatim. Detected by `len(stored) >= len(payload)`."""
    return LzisChunk(payload, payload + b"\x00" * max(padding, 0))


def build_lzis(chunks: list[LzisChunk], *, chunk_size: int, level: int = 2) -> bytes:
    """Assemble a complete LZIS stream from already-encoded chunks."""
    for chunk in chunks[:-1]:
        assert len(chunk.payload) == chunk_size, "only the last chunk may be short"
    total = sum(len(chunk.payload) for chunk in chunks)

    table_end = LZIS_HEADER_SIZE + 4 * len(chunks)
    offsets = []
    cursor = table_end
    for chunk in chunks:
        offsets.append(cursor)
        cursor += len(chunk.encoded)

    out = bytearray(LZIS_MAGIC)
    out += struct.pack("<II", total, chunk_size)
    out += bytes((level, 0))
    out += struct.pack(f"<{len(chunks)}I", *offsets)
    for chunk in chunks:
        out += chunk.encoded
    return bytes(out)


# ===========================================================================
# Sprites  --  docs/formats/rle.md
# ===========================================================================

RLE_MAGIC = b"IMGRLE"
INDEXED8, RGB555, MASK = 1, 3, 7


def encode_rle_row(pixels: list[int | None], bytes_per_pixel: int) -> bytes:
    """Encode one row as `(gap, length, payload)` triples.

    `None` is transparent. Gaps wider than 255 become repeated `(255, 0)` pairs
    and runs longer than 255 are split, exactly as the format requires. The
    trailing transparent tail is written as a final `(gap, 0)` pair so that the
    row's gap and run counts sum to the full width.
    """
    out = bytearray()
    x, width = 0, len(pixels)
    while x < width:
        gap_start = x
        while x < width and pixels[x] is None:
            x += 1
        gap = x - gap_start
        run_start = x
        while x < width and pixels[x] is not None:
            x += 1
        run = x - run_start

        while gap > 255:
            out += bytes((255, 0))
            gap -= 255
        if run == 0:
            out += bytes((gap, 0))
            continue
        first, taken = True, 0
        while taken < run:
            take = min(run - taken, 255)
            out += bytes((gap if first else 0, take))
            for i in range(take):
                value = pixels[run_start + taken + i]
                assert value is not None
                if bytes_per_pixel == 1:
                    out.append(value)
                elif bytes_per_pixel == 2:
                    out += struct.pack("<H", value)
            first, taken = False, taken + take
    return bytes(out)


def encode_rle_frame(rows: list[list[int | None]], pixel_format: int, *, wide: bool) -> bytes:
    """Row offset table plus the encoded rows: one frame's `rle.mmp` payload."""
    per_pixel = {INDEXED8: 1, RGB555: 2, MASK: 0}[pixel_format]
    encoded = [encode_rle_row(row, per_pixel) for row in rows]
    entry = 4 if wide else 2
    offsets, cursor = [], len(rows) * entry
    for row in encoded:
        offsets.append(cursor)
        cursor += len(row)
    code = "I" if wide else "H"
    return struct.pack(f"<{len(rows)}{code}", *offsets) + b"".join(encoded)


@dataclass
class SyntheticFrame:
    """One frame of a synthetic sprite, described by its pixels on a canvas."""

    left: int
    top: int
    rows: list[list[int | None]]  # empty list means an empty frame
    wide: bool = False

    @property
    def width(self) -> int:
        return len(self.rows[0]) if self.rows else 0

    @property
    def height(self) -> int:
        return len(self.rows)


def build_rle_image(
    frames: list[SyntheticFrame],
    *,
    columns: int,
    rows: int,
    image_class: int,
    pixel_format: int,
    palette: list[tuple[int, int, int]] | None = None,
    color_key: int = 0,
    lut: list[int] | None = None,
    empty_box_seed: int = 300,
) -> tuple[bytes, bytes]:
    """Return `(frame_table_bytes, pixel_store_bytes)` for a synthetic sprite.

    The pixel store is a bare concatenation of the frame payloads, which is what
    the real `rle.mmp` is.
    """
    assert len(frames) == columns * rows
    table = bytearray(RLE_MAGIC)
    table += struct.pack("<IIIII", image_class, 0, 0, columns, rows)

    store = bytearray()
    for frame in frames:
        if not frame.rows:
            # The encoder never touched the box, so it kept its inward-growing
            # initial value: right < left and bottom < top.
            table += struct.pack("<IIII", empty_box_seed, empty_box_seed, 0, 0)
            continue
        payload = encode_rle_frame(frame.rows, pixel_format, wide=frame.wide)
        offset = len(store)
        store += payload
        table += struct.pack(
            "<IIII",
            frame.left,
            frame.top,
            frame.left + frame.width - 1,
            frame.top + frame.height - 1,
        )
        table += b"RLE2"
        table += struct.pack(
            "<IIHIIII",
            frame.width,
            frame.height,
            pixel_format,
            len(payload),
            0,
            1 if frame.wide else 0,
            color_key,
        )
        table += b"pamm" + struct.pack("<I", offset)

    entries = palette or []
    table += struct.pack("<I", len(entries))
    for red, green, blue in entries:
        table += bytes((blue, green, red, 0))
    if lut is not None:
        table += struct.pack("<256H", *lut)
    return bytes(table), bytes(store)


def mirrored_palette(size: int = 256) -> list[tuple[int, int, int]]:
    """A class-2 palette: entries 0..63 are a byte-identical copy of 64..127."""
    entries = [((i * 7) % 256, (i * 13) % 256, (i * 29) % 256) for i in range(size)]
    entries[0:64] = entries[64:128]
    return entries


# ===========================================================================
# Terrain textures  --  docs/formats/vq.md
# ===========================================================================

VQ_MAGIC = b"mbqv"
VQ_HEADER_SIZE = 0x2C


def build_vq(
    codebook: list[tuple[int, int, int, int]],
    indices: list[int],
    *,
    blocks_x: int,
    blocks_y: int,
    index_size: int | None = None,
    block_width: int = 4,
    block_height: int = 1,
    bytes_per_pixel: int = 2,
) -> bytes:
    """Serialise a vector-quantised bitmap. Samples are raw X1R5G5B5 values."""
    if index_size is None:
        index_size = 1 if len(codebook) <= 256 else 2
    log2 = {1: 0, 2: 1}[index_size]

    out = bytearray(VQ_MAGIC)
    out += struct.pack(
        "<10I",
        log2,
        len(codebook),
        index_size,
        bytes_per_pixel,
        block_width,
        block_height,
        blocks_x * block_width,
        blocks_y * block_height,
        blocks_x,
        blocks_y,
    )
    for entry in codebook:
        out += struct.pack(f"<{block_width * block_height}H", *entry)
    code = "B" if index_size == 1 else "H"
    out += struct.pack(f"<{len(indices)}{code}", *indices)
    return bytes(out)


def rgb555(red: int, green: int, blue: int) -> int:
    """Pack 5-bit components the way the format does. Bit 15 is never set."""
    return ((red & 31) << 10) | ((green & 31) << 5) | (blue & 31)


# ===========================================================================
# Block container  --  docs/formats/bfhp.md
# ===========================================================================

BFHP_MAGIC = b"HPFS"


class BfhpBuilder:
    """Assembles an `HPFS` container block by block.

    Blocks are allocated in the order the writer would: a node first, then its
    children, then its own data. Every block ends up owned by exactly one
    structure, which is the invariant the reader's `validate()` checks.
    """

    def __init__(self, block_size: int = 64) -> None:
        assert block_size >= 32 and not (block_size & (block_size - 1))
        self.block_size = block_size
        self.pointers = block_size // 4
        self.blocks: dict[int, bytes] = {}
        self.used: dict[int, int] = {}
        self._next = 2  # 0 is the header, 1 is the root node

    # -- allocation ------------------------------------------------------

    def _alloc(self) -> int:
        index = self._next
        self._next += 1
        return index

    def _write_node(self, node: int, payload: bytes) -> None:
        size = len(payload)
        needed = -(-size // self.block_size)
        data_blocks = []
        for i in range(needed):
            block = self._alloc()
            piece = payload[i * self.block_size : (i + 1) * self.block_size]
            self.blocks[block] = piece.ljust(self.block_size, b"\0")
            self.used[block] = len(piece)
            data_blocks.append(block)

        if 2 + needed <= self.pointers:
            # Level 0: a direct block list with a zero-filled tail.
            words = [size, 0, *data_blocks]
            words += [0] * (self.pointers - len(words))
        else:
            index_count = -(-needed // self.pointers)
            index_blocks = []
            for i in range(index_count):
                block = self._alloc()
                slice_ = data_blocks[i * self.pointers : (i + 1) * self.pointers]
                raw = struct.pack(f"<{len(slice_)}I", *slice_)
                # Level-1 tails are stale in the retail data, never zeroed.
                self.blocks[block] = raw.ljust(self.block_size, b"\xcd")
                self.used[block] = self.block_size
                index_blocks.append(block)
            words = [size, 1, *index_blocks]
            words += [0xCDCDCDCD] * (self.pointers - len(words))

        self.blocks[node] = struct.pack(f"<{self.pointers}I", *words)
        self.used[node] = self.block_size

    # -- tree ------------------------------------------------------------

    def add_file(self, payload: bytes) -> int:
        node = self._alloc()
        self._write_node(node, payload)
        return node

    def add_dir(self, children: list[tuple[str, int, bool]], *, node: int | None = None) -> int:
        """`children` is `[(name, child_node, is_dir), ...]`."""
        if node is None:
            node = self._alloc()
        packed = bytearray()
        for name, child, is_dir in children:
            raw = name.encode("cp1252")
            packed += struct.pack("<IHH", child, 1 if is_dir else 0, len(raw)) + raw
        self._write_node(node, bytes(packed))
        return node

    def finish(self, *, truncate: bool = True, block_size: int | None = None) -> bytes:
        """Emit the container. The final block is truncated to the bytes used."""
        count = self._next
        header = struct.pack(
            "<4sIIiIIII",
            BFHP_MAGIC,
            block_size or self.block_size,
            count,
            -1,
            0,
            0,
            8,
            1,
        ).ljust(self.block_size, b"\0")

        out = bytearray(header)
        for index in range(1, count):
            out += self.blocks[index]
        if truncate:
            tail = self.used.get(count - 1, self.block_size) or self.block_size
            del out[(count - 1) * self.block_size + tail :]
        return bytes(out)


def build_simple_bfhp(tree: dict, *, block_size: int = 64) -> bytes:
    """Build a container from a nested dict: `bytes` is a file, `dict` a directory."""
    builder = BfhpBuilder(block_size)

    def walk(node_index: int | None, mapping: dict) -> int:
        if node_index is None:
            node_index = builder._alloc()
        children = []
        for name, value in mapping.items():
            if isinstance(value, dict):
                children.append((name, walk(None, value), True))
            else:
                children.append((name, builder.add_file(value), False))
        return builder.add_dir(children, node=node_index)

    walk(1, tree)
    return builder.finish()


# ===========================================================================
# Bitmap fonts  --  docs/formats/apf.md
# ===========================================================================

APF_MAGIC = b"ABCF"
APF_HEADER_SIZE = 0x20
APF_METRIC_COUNT = 14
APF_GLYPH_RECORD = 32
APF_KERN_RECORD = 12


@dataclass
class SyntheticGlyph:
    """One glyph: bearings, the ink box, and its coverage in 0..7."""

    code: int
    abc_a: int
    abc_b: int
    abc_c: int
    top: int
    bottom: int
    coverage: list[int]  # abc_b * (bottom - top + 1) values, or empty when blank

    @property
    def height(self) -> int:
        return 0 if self.bottom < self.top else self.bottom - self.top + 1


def encode_apf_coverage(values: list[int]) -> bytes:
    """Run-length code alpha values: `(level << 5) | (run - 1)`, runs of 1..32."""
    out = bytearray()
    index = 0
    while index < len(values):
        level = values[index]
        assert 0 <= level <= 7, "coverage is a 3-bit value"
        run = 1
        while index + run < len(values) and values[index + run] == level and run < 32:
            run += 1
        out.append((level << 5) | (run - 1))
        index += run
    return bytes(out)


def build_apf(
    glyphs: list[SyntheticGlyph],
    *,
    face: str = "Testa",
    family: str = "Testa",
    point_size: int = 13,
    ascent: int = 10,
    descent: int = 3,
    italic: int = 0,
    bold: int = 0,
    kerning: list[tuple[int, int, int]] | None = None,
) -> bytes:
    """Serialise a single-range APF font.

    Layout order is header, strings, request block, metrics, range table, then
    the character blocks -- which the reader requires to be contiguous and to
    end on the last byte of the file.
    """
    glyphs = sorted(glyphs, key=lambda g: g.code)
    codes = [g.code for g in glyphs]
    assert codes == list(range(codes[0], codes[0] + len(codes))), "one contiguous range"
    kerning = kerning or []

    face_bytes = face.encode("cp1252") + b"\0"
    family_bytes = family.encode("cp1252") + b"\0"

    face_offset = APF_HEADER_SIZE
    family_offset = face_offset + len(face_bytes)
    request_offset = family_offset + len(family_bytes)
    metrics_offset = request_offset + 12
    range_table = metrics_offset + APF_METRIC_COUNT * 4
    block_offset = range_table + 4 + 16

    # -- the one character block
    pixels = bytearray()
    records = bytearray()
    for glyph in glyphs:
        expected = glyph.abc_b * glyph.height
        assert len(glyph.coverage) == expected, (
            f"U+{glyph.code:04X}: {len(glyph.coverage)} coverage values, expected {expected}"
        )
        start = len(pixels)
        pixels += encode_apf_coverage(glyph.coverage)
        records += struct.pack(
            "<8i",
            glyph.abc_a,
            glyph.abc_b,
            glyph.abc_c,
            0,
            glyph.top,
            glyph.abc_b - 1,
            glyph.bottom,
            start,
        )

    kern_offset = 16 + APF_GLYPH_RECORD * len(glyphs)
    pixel_offset = kern_offset + APF_KERN_RECORD * len(kerning)
    block = bytearray(struct.pack("<4I", kern_offset, len(kerning), pixel_offset, len(pixels)))
    block += records
    for first, second, amount in kerning:
        block += struct.pack("<2Ii", first, second, amount)
    block += pixels
    block_size = len(block)

    out = bytearray(APF_MAGIC)
    out += struct.pack(
        "<7i", metrics_offset, face_offset, family_offset, codes[0], 0, 0, request_offset
    )
    assert len(out) == APF_HEADER_SIZE
    out += face_bytes + family_bytes
    out += struct.pack("<3i", point_size, italic, bold)

    widest = max(g.abc_b for g in glyphs)
    average = sum(g.abc_b for g in glyphs) // len(glyphs)
    out += struct.pack(
        f"<{APF_METRIC_COUNT}i",
        ascent + descent,  # height
        widest,
        average,
        0,
        0,
        ascent,
        descent,
        *([0] * 7),
    )
    out += struct.pack("<I", 1)
    out += struct.pack("<4I", block_offset, block_size, codes[0], len(glyphs))
    assert len(out) == block_offset
    out += block
    return bytes(out)


# ===========================================================================
# Grids and passability masks  --  docs/formats/pass.md
# ===========================================================================

GRID_MAGIC = b"DIRG"
GRID_HEADER_SIZE = 20


def build_grid(cells: list[list[int]], *, cell_size: int, bits_per_cell: int) -> bytes:
    """Serialise a grid. Sub-byte cells are packed least-significant-bit first.

    Deliberately independent of ``pass_mask.write``: this is the oracle the
    writer is checked against, so it packs bit by bit rather than sharing code.
    """
    height = len(cells)
    width = len(cells[0])
    stride = width * bits_per_cell // 8

    out = bytearray(GRID_MAGIC)
    out += struct.pack(
        "<4I", cell_size, bits_per_cell, width * cell_size, height * cell_size
    )
    for row in cells:
        assert len(row) == width
        if bits_per_cell == 1:
            packed = bytearray(stride)
            for x, value in enumerate(row):
                if value:
                    packed[x >> 3] |= 1 << (x & 7)
            out += packed
        elif bits_per_cell == 4:
            packed = bytearray(stride)
            for x, value in enumerate(row):
                packed[x >> 1] |= value << (4 if x & 1 else 0)
            out += packed
        elif bits_per_cell == 8:
            out += bytes(row)
        elif bits_per_cell == 16:
            out += struct.pack(f"<{width}H", *row)
        else:  # pragma: no cover - the retail data ships 1, 4, 8 and 16
            raise ValueError(bits_per_cell)
    return bytes(out)


def blank_pass_mask() -> list[list[int]]:
    """A 128x128 grid of zeroes: the shape every retail `.pass` file uses."""
    return [[0] * 128 for _ in range(128)]


# ===========================================================================
# PNG  --  src/imperivm/png.py
# ===========================================================================

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _png_chunk(tag: bytes, payload: bytes) -> bytes:
    body = tag + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def build_filtered_png(
    width: int, height: int, channels: int, pixels: bytes, filters: list[int]
) -> bytes:
    """Encode a PNG applying a chosen row filter per row.

    `imperivm.png.encode` only ever writes filter 0, so this is the only way to
    exercise the decoder's Sub/Up/Average/Paeth reconstruction. The forward
    filter is written out here rather than derived from the decoder.
    """
    color_type = {1: 0, 2: 4, 3: 2, 4: 6}[channels]
    stride = width * channels

    raw = bytearray()
    previous = bytes(stride)
    for y in range(height):
        row = pixels[y * stride : (y + 1) * stride]
        kind = filters[y]
        encoded = bytearray()
        for i, value in enumerate(row):
            left = row[i - channels] if i >= channels else 0
            up = previous[i]
            upleft = previous[i - channels] if i >= channels else 0
            if kind == 0:
                predictor = 0
            elif kind == 1:
                predictor = left
            elif kind == 2:
                predictor = up
            elif kind == 3:
                predictor = (left + up) >> 1
            elif kind == 4:
                predictor = _paeth(left, up, upleft)
            else:  # pragma: no cover
                raise ValueError(kind)
            encoded.append((value - predictor) & 0xFF)
        raw.append(kind)
        raw += encoded
        previous = row

    out = bytearray(PNG_SIGNATURE)
    out += _png_chunk(
        b"IHDR", struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    )
    out += _png_chunk(b"IDAT", zlib.compress(bytes(raw), 6))
    out += _png_chunk(b"IEND", b"")
    return bytes(out)
