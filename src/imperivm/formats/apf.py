"""Reader for the HMMSYS bitmap font format (`.apf`).

Specification: docs/formats/apf.md

Reference implementation: correctness and legibility over speed.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"ABCF"
HEADER_SIZE = 0x20
METRIC_COUNT = 14
RANGE_RECORD_SIZE = 16
GLYPH_RECORD_SIZE = 32
KERN_RECORD_SIZE = 12
ALPHA_LEVELS = 8  # coverage is a 3-bit value, 0..7


class FontError(Exception):
    """Raised when a file does not conform to the font format."""


@dataclass(frozen=True)
class FontMetrics:
    """Cell metrics shared by every glyph in the font, in pixels."""

    height: int  # full cell height; equals ascent + descent
    max_char_width: int  # widest ink box in the font
    ave_char_width: int
    internal_leading: int
    external_leading: int
    ascent: int  # baseline measured down from the top of the cell
    descent: int
    unknown: tuple[int, ...]  # seven trailing fields, see the specification


@dataclass(frozen=True)
class Glyph:
    """One character's ink box and its run-length encoded coverage."""

    code: int  # Unicode code point
    abc_a: int  # left side bearing
    abc_b: int  # ink width; also the bitmap width
    abc_c: int  # right side bearing
    top: int  # first ink row, measured down from the top of the cell
    bottom: int  # last ink row, inclusive; < top for a blank glyph
    data: bytes  # RLE stream, see :meth:`ApfFont.coverage`

    @property
    def width(self) -> int:
        return self.abc_b

    @property
    def height(self) -> int:
        return 0 if self.bottom < self.top else self.bottom - self.top + 1

    @property
    def advance(self) -> int:
        """Pen movement for this glyph, before kerning."""
        return self.abc_a + self.abc_b + self.abc_c

    @property
    def is_blank(self) -> bool:
        return self.height == 0


@dataclass(frozen=True)
class CharRange:
    """One contiguous block of code points stored together in the file."""

    first: int
    count: int
    offset: int  # absolute, from the start of the file
    size: int

    @property
    def last(self) -> int:
        return self.first + self.count - 1


class ApfFont:
    """A parsed HMMSYS bitmap font.

    Coverage is an 8-level alpha ramp, not colour: the engine multiplies it
    against whatever ink colour the caller asks for.
    """

    def __init__(self, data: bytes, name: str = "<bytes>") -> None:
        self.name = name
        self._data = data
        self.ranges: list[CharRange] = []
        self.glyphs: dict[int, Glyph] = {}
        self.kerning: dict[tuple[int, int], int] = {}
        self._parse()

    @classmethod
    def from_path(cls, path: str | Path) -> ApfFont:
        path = Path(path)
        return cls(path.read_bytes(), path.name)

    # -- parsing ---------------------------------------------------------

    def _parse(self) -> None:
        data = self._data
        if data[: len(MAGIC)] != MAGIC:
            raise FontError(f"{self.name}: not an APF font (magic mismatch)")

        (
            metrics_offset,
            face_offset,
            family_offset,
            first_char,
            self.unknown_0x14,
            self.unknown_0x18,
            request_offset,
        ) = struct.unpack_from("<7i", data, 4)

        self.face_name = self._cstring(face_offset)
        self.family_name = self._cstring(family_offset)
        self.point_size, self.italic, self.bold = struct.unpack_from(
            "<3i", data, request_offset
        )

        raw = struct.unpack_from(f"<{METRIC_COUNT}i", data, metrics_offset)
        self.metrics = FontMetrics(*raw[:7], unknown=raw[7:])

        table = metrics_offset + METRIC_COUNT * 4
        (range_count,) = struct.unpack_from("<I", data, table)
        table += 4
        for i in range(range_count):
            offset, size, first, count = struct.unpack_from(
                "<4I", data, table + i * RANGE_RECORD_SIZE
            )
            if offset + size > len(data):
                raise FontError(f"{self.name}: range {i} overruns the file")
            self.ranges.append(CharRange(first, count, offset, size))

        self.first_char = first_char
        for char_range in self.ranges:
            self._parse_block(char_range)

    def _cstring(self, offset: int) -> str:
        end = self._data.index(b"\0", offset)
        return self._data[offset:end].decode("cp1252")

    def _parse_block(self, char_range: CharRange) -> None:
        data = self._data
        base = char_range.offset
        kern_offset, kern_count, pixel_offset, pixel_size = struct.unpack_from(
            "<4I", data, base
        )

        expected = 16 + GLYPH_RECORD_SIZE * char_range.count
        if kern_offset != expected:
            raise FontError(
                f"{self.name}: block at {base} has glyph table of {kern_offset} "
                f"bytes, expected {expected}"
            )
        if pixel_offset != kern_offset + KERN_RECORD_SIZE * kern_count:
            raise FontError(f"{self.name}: block at {base} has a misplaced pixel section")
        if pixel_offset + pixel_size != char_range.size:
            raise FontError(f"{self.name}: block at {base} does not fill its range")

        records = [
            struct.unpack_from("<8i", data, base + 16 + GLYPH_RECORD_SIZE * i)
            for i in range(char_range.count)
        ]
        pixels = base + pixel_offset
        for i, rec in enumerate(records):
            abc_a, abc_b, abc_c, _reserved, top, _right, bottom, start = rec
            end = records[i + 1][7] if i + 1 < char_range.count else pixel_size
            self.glyphs[char_range.first + i] = Glyph(
                code=char_range.first + i,
                abc_a=abc_a,
                abc_b=abc_b,
                abc_c=abc_c,
                top=top,
                bottom=bottom,
                data=data[pixels + start : pixels + end],
            )

        for i in range(kern_count):
            first, second, amount = struct.unpack_from(
                "<2Ii", data, base + kern_offset + KERN_RECORD_SIZE * i
            )
            self.kerning[(first, second)] = amount

    # -- access ----------------------------------------------------------

    def __contains__(self, code: int) -> bool:
        return code in self.glyphs

    def __len__(self) -> int:
        return len(self.glyphs)

    def coverage(self, code: int) -> list[int]:
        """Return the glyph's ink box as row-major alpha values in 0..7.

        The stored stream is a flat run-length code over the whole ink box;
        runs are free to cross row boundaries.
        """
        glyph = self.glyphs[code]
        out: list[int] = []
        for byte in glyph.data:
            out += [byte >> 5] * ((byte & 0x1F) + 1)
        expected = glyph.width * glyph.height
        if len(out) != expected:
            raise FontError(
                f"{self.name}: U+{code:04X} decodes to {len(out)} pixels, "
                f"expected {expected}"
            )
        return out

    def advance(self, code: int) -> int:
        return self.glyphs[code].advance

    def kern(self, left: int, right: int) -> int:
        return self.kerning.get((left, right), 0)

    def text_width(self, text: str) -> int:
        width = 0
        previous: int | None = None
        for char in text:
            code = ord(char)
            if previous is not None:
                width += self.kern(previous, code)
            width += self.advance(code)
            previous = code
        return width

    def render(self, text: str) -> tuple[int, int, list[int]]:
        """Lay out a string and return ``(width, height, alpha)`` in 0..255.

        The buffer is one byte per pixel and ``height`` is the cell height,
        so the baseline sits at ``metrics.ascent`` rows from the top.
        """
        width = max(1, self.text_width(text))
        height = self.metrics.height
        buffer = [0] * (width * height)
        pen = 0
        previous: int | None = None
        for char in text:
            code = ord(char)
            if previous is not None:
                pen += self.kern(previous, code)
            glyph = self.glyphs[code]
            alpha = self.coverage(code)
            for row in range(glyph.height):
                for column in range(glyph.width):
                    value = alpha[row * glyph.width + column]
                    if not value:
                        continue
                    x = pen + glyph.abc_a + column
                    y = glyph.top + row
                    if 0 <= x < width and 0 <= y < height:
                        scaled = value * 255 // (ALPHA_LEVELS - 1)
                        index = y * width + x
                        buffer[index] = max(buffer[index], scaled)
            pen += glyph.advance
            previous = code
        return width, height, buffer

    def validate(self) -> None:
        """Assert the structural invariants described in the specification."""
        if self.metrics.height != self.metrics.ascent + self.metrics.descent:
            raise FontError(f"{self.name}: height is not ascent + descent")
        if not self.ranges:
            raise FontError(f"{self.name}: no character ranges")
        if self.ranges[0].first != self.first_char:
            raise FontError(f"{self.name}: header first_char disagrees with range 0")

        cursor = self.ranges[0].offset
        for char_range in self.ranges:
            if char_range.offset != cursor:
                raise FontError(f"{self.name}: gap before range at {char_range.offset}")
            cursor += char_range.size
        if cursor != len(self._data):
            raise FontError(
                f"{self.name}: ranges end at {cursor}, file is {len(self._data)} bytes"
            )

        widest = 0
        for code in self.glyphs:
            glyph = self.glyphs[code]
            self.coverage(code)  # raises if the run lengths do not fill the box
            if glyph.is_blank != (len(glyph.data) == 0):
                raise FontError(f"{self.name}: U+{code:04X} blankness disagrees with data")
            widest = max(widest, glyph.width)
        if widest != self.metrics.max_char_width:
            raise FontError(
                f"{self.name}: widest glyph is {widest}, "
                f"metrics say {self.metrics.max_char_width}"
            )


def write_png(path: str | Path, width: int, height: int, gray: list[int]) -> None:
    """Write an 8-bit greyscale PNG. Standard library only."""
    import zlib

    raw = b"".join(
        b"\x00" + bytes(gray[y * width : (y + 1) * width]) for y in range(height)
    )

    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def main() -> None:
    import argparse

    parser = argparse.ArgumentParser(description="Inspect a HMMSYS bitmap font.")
    parser.add_argument("font", type=Path)
    parser.add_argument("--text", help="lay out a string instead of listing glyphs")
    parser.add_argument("--png", type=Path, help="with --text, write a greyscale PNG")
    args = parser.parse_args()

    font = ApfFont.from_path(args.font)
    font.validate()

    if args.text:
        width, height, buffer = font.render(args.text)
        if args.png:
            write_png(args.png, width, height, buffer)
            print(f"{args.png}: {width}x{height}")
            return
        ramp = " .:-=+*#%@"
        for y in range(height):
            row = buffer[y * width : (y + 1) * width]
            print("".join(ramp[v * (len(ramp) - 1) // 255] for v in row))
        return

    m = font.metrics
    print(f"{args.font}: {font.face_name!r} family {font.family_name!r}")
    print(f"  {font.point_size}pt bold={font.bold} italic={font.italic}")
    print(f"  cell {m.height}px  ascent {m.ascent}  descent {m.descent}")
    print(f"  {len(font)} glyphs in {len(font.ranges)} ranges, {len(font.kerning)} kern pairs")
    for char_range in font.ranges:
        print(
            f"    U+{char_range.first:04X}..U+{char_range.last:04X}"
            f"  {char_range.count:>4} glyphs  {char_range.size:>7} bytes"
        )


if __name__ == "__main__":
    main()
