"""Reader for the HMMSYS vector-quantised bitmap format (`.vq`).

Specification: docs/formats/vq.md

Reference implementation: correctness and legibility over speed.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

# On disk the four bytes read "mbqv"; they are the four-character code 'vqbm'
# stored as a little-endian u32.
MAGIC = b"mbqv"
HEADER_SIZE = 0x2C

# The quantisation unit. Constant across the whole retail data set; see the
# "What is still unknown" section of the specification.
BLOCK_WIDTH = 4
BLOCK_HEIGHT = 1
BYTES_PER_PIXEL = 2


class VQImageError(Exception):
    """Raised when a file does not conform to the vqbm format."""


@dataclass(frozen=True)
class VQHeader:
    """The 44-byte header of a `.vq` file."""

    index_size_log2: int
    codebook_len: int
    index_size: int
    bytes_per_pixel: int
    block_width: int
    block_height: int
    width: int
    height: int
    blocks_x: int
    blocks_y: int

    @property
    def codebook_bytes(self) -> int:
        return self.codebook_len * self.block_width * self.block_height * self.bytes_per_pixel

    @property
    def index_count(self) -> int:
        return self.blocks_x * self.blocks_y

    @property
    def expected_size(self) -> int:
        return HEADER_SIZE + self.codebook_bytes + self.index_count * self.index_size


class VQImage:
    """A parsed vector-quantised terrain texture.

    Pixels are stored as runs of four horizontally adjacent samples, each run
    replaced by an index into a shared codebook. Sample format is X1R5G5B5.
    """

    def __init__(self, data: bytes, name: str = "<memory>") -> None:
        self.name = name
        self._data = bytes(data)
        self.header = self._parse_header()
        self.codebook: list[tuple[int, ...]] = self._parse_codebook()
        self.indices: list[int] = self._parse_indices()

    @classmethod
    def from_file(cls, path: str | Path) -> VQImage:
        path = Path(path)
        return cls(path.read_bytes(), path.name)

    @classmethod
    def from_pack(cls, pack: str | Path, member: str) -> VQImage:
        """Read one `.vq` stored inside a HMMSYS pack archive."""
        from pak import PackFile

        return cls(PackFile(pack).read(member), member)

    # -- parsing ---------------------------------------------------------

    def _parse_header(self) -> VQHeader:
        data = self._data
        if data[: len(MAGIC)] != MAGIC:
            raise VQImageError(f"{self.name}: not a vqbm image (magic mismatch)")
        if len(data) < HEADER_SIZE:
            raise VQImageError(f"{self.name}: truncated header")
        return VQHeader(*struct.unpack_from("<10I", data, len(MAGIC)))

    def _parse_codebook(self) -> list[tuple[int, ...]]:
        h = self.header
        if h.expected_size != len(self._data):
            raise VQImageError(
                f"{self.name}: header describes {h.expected_size} bytes, "
                f"file is {len(self._data)} bytes"
            )
        per_entry = h.block_width * h.block_height
        fmt = f"<{per_entry}H"
        return [
            struct.unpack_from(fmt, self._data, HEADER_SIZE + i * per_entry * 2)
            for i in range(h.codebook_len)
        ]

    def _parse_indices(self) -> list[int]:
        h = self.header
        start = HEADER_SIZE + h.codebook_bytes
        raw = self._data[start:]
        if h.index_size == 1:
            return list(raw)
        if h.index_size == 2:
            return list(struct.unpack(f"<{h.index_count}H", raw))
        raise VQImageError(f"{self.name}: unsupported index size {h.index_size}")

    # -- access ----------------------------------------------------------

    @property
    def width(self) -> int:
        return self.header.width

    @property
    def height(self) -> int:
        return self.header.height

    def samples(self) -> list[int]:
        """Return every pixel as a raw X1R5G5B5 value, row major, top row first."""
        h = self.header
        out: list[int] = []
        for by in range(h.blocks_y):
            row = self.indices[by * h.blocks_x : (by + 1) * h.blocks_x]
            for _ in range(h.block_height):
                for index in row:
                    out.extend(self.codebook[index])
        return out

    def rgb(self) -> bytearray:
        """Return the image as packed 8-bit RGB triples, row major."""
        palette = {v: _expand(v) for entry in self.codebook for v in entry}
        out = bytearray()
        for value in self.samples():
            out += palette[value]
        return out

    def to_png(self, path: str | Path) -> None:
        """Write the decoded image as a PNG. Standard library only."""
        w, h = self.width, self.height
        rgb = self.rgb()
        raw = b"".join(b"\x00" + bytes(rgb[y * w * 3 : (y + 1) * w * 3]) for y in range(h))
        header = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
        blob = b"\x89PNG\r\n\x1a\n"
        blob += _chunk(b"IHDR", header)
        blob += _chunk(b"IDAT", zlib.compress(raw, 9))
        blob += _chunk(b"IEND", b"")
        Path(path).write_bytes(blob)

    def validate(self) -> None:
        """Assert the structural invariants described in the specification."""
        h = self.header
        if h.index_size != 1 << h.index_size_log2:
            raise VQImageError(
                f"{self.name}: index_size {h.index_size} disagrees with "
                f"index_size_log2 {h.index_size_log2}"
            )
        if h.index_size not in (1, 2):
            raise VQImageError(f"{self.name}: index_size {h.index_size} is not 1 or 2")
        if h.codebook_len == 0 or h.codebook_len & (h.codebook_len - 1):
            raise VQImageError(f"{self.name}: codebook_len {h.codebook_len} is not a power of two")
        if h.index_size == 1 and h.codebook_len > 256:
            raise VQImageError(f"{self.name}: {h.codebook_len} entries need 2-byte indices")
        if (h.bytes_per_pixel, h.block_width, h.block_height) != (
            BYTES_PER_PIXEL,
            BLOCK_WIDTH,
            BLOCK_HEIGHT,
        ):
            raise VQImageError(
                f"{self.name}: unexpected block geometry "
                f"{h.block_width}x{h.block_height}x{h.bytes_per_pixel}"
            )
        if h.blocks_x * h.block_width != h.width:
            raise VQImageError(f"{self.name}: blocks_x {h.blocks_x} does not cover width {h.width}")
        if h.blocks_y * h.block_height != h.height:
            raise VQImageError(
                f"{self.name}: blocks_y {h.blocks_y} does not cover height {h.height}"
            )
        if h.expected_size != len(self._data):
            raise VQImageError(
                f"{self.name}: header describes {h.expected_size} bytes, "
                f"file is {len(self._data)} bytes"
            )
        worst = max(self.indices, default=0)
        if worst >= h.codebook_len:
            raise VQImageError(f"{self.name}: index {worst} exceeds codebook of {h.codebook_len}")
        for entry in self.codebook:
            for sample in entry:
                if sample & 0x8000:
                    raise VQImageError(f"{self.name}: codebook sample {sample:#06x} sets bit 15")


def _expand(value: int) -> bytes:
    """X1R5G5B5 to an 8-bit RGB triple, by bit replication."""
    r, g, b = (value >> 10) & 31, (value >> 5) & 31, value & 31
    return bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)))


def _chunk(tag: bytes, payload: bytes) -> bytes:
    body = tag + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def main() -> None:
    import argparse

    parser = argparse.ArgumentParser(description="Inspect a HMMSYS vqbm terrain texture.")
    parser.add_argument("file", type=Path, help="a .vq file, or a .pak when --member is given")
    parser.add_argument("--member", help="read this .vq out of the pack given as FILE")
    parser.add_argument("--png", type=Path, metavar="OUT", help="write the decoded image")
    args = parser.parse_args()

    if args.member:
        image = VQImage.from_pack(args.file, args.member)
    else:
        image = VQImage.from_file(args.file)

    image.validate()
    h = image.header
    print(f"{image.name}: {h.width}x{h.height} X1R5G5B5")
    print(f"  blocks       {h.blocks_x} x {h.blocks_y} of {h.block_width}x{h.block_height}")
    print(f"  codebook     {h.codebook_len} entries, {h.codebook_bytes} bytes")
    print(f"  indices      {h.index_count} x {h.index_size} byte")
    print(f"  total        {h.expected_size} bytes")

    if args.png:
        image.to_png(args.png)
        print(f"  wrote        {args.png}")


if __name__ == "__main__":
    main()
