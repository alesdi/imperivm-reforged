"""Reader for the HMMSYS sprite format: `*.rle.mmp` frame tables and `rle.mmp`.

Specification: docs/formats/rle.md

A sprite is split across two files. The frame table lives inside a pack and holds
the geometry, the palette and, for every frame, an absolute offset into the
root-level `rle.mmp` pixel store where the compressed rows live.

Reference implementation: correctness and legibility over speed.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path
from typing import Iterator, Sequence

MAGIC = b"IMGRLE"
FRAME_MAGIC = b"RLE2"
STORE_MAGIC = b"pamm"  # "mmap" with the bytes reversed
HEADER_SIZE = 26
LUT_SIZE = 512
PLAYER_COLOR_SLOTS = 64


class RleError(Exception):
    """Raised when a file does not conform to the sprite format."""


class ImageClass(IntEnum):
    """Value of the `image_class` header field.

    Correlates with the `drawmode` attribute of the matching `<image>` element in
    the entity XML, but the file is authoritative; a few hundred XML entries
    disagree with the bytes they point at.
    """

    TRUECOLOR = 0  # drawmode="normal", RGB555
    INDEXED = 1  # drawmode="normal" / "index", 8-bit
    PLAYER_COLOR = 2  # drawmode="player_color", 8-bit with 64 remappable slots
    SHADOW = 4  # drawmode="shadow", 1-bit coverage mask
    CLOUDS = 5  # drawmode="clouds", RGB555
    # 3 is not used by any retail file.


class PixelFormat(IntEnum):
    """Value of the `pixel_format` field of an `RLE2` record."""

    INDEXED8 = 1  # one palette index per pixel
    RGB555 = 3  # one little-endian u16 per pixel, 0rrrrrgggggbbbbb
    MASK = 7  # no payload at all; a run is simply "covered"

    @property
    def bytes_per_pixel(self) -> int:
        return {PixelFormat.INDEXED8: 1, PixelFormat.RGB555: 2, PixelFormat.MASK: 0}[
            self
        ]


@dataclass(frozen=True)
class Frame:
    """One frame of a sprite.

    `left`/`top`/`right`/`bottom` are inclusive bounds inside a canvas shared by
    every frame of the image (and by the matching shadow image). Empty frames
    carry only those four fields, with a degenerate box, and no pixel data.
    """

    index: int
    left: int
    top: int
    right: int
    bottom: int
    width: int
    height: int
    pixel_format: PixelFormat | None
    data_size: int
    data_offset: int
    color_key: int
    wide_row_table: bool

    @property
    def empty(self) -> bool:
        return self.pixel_format is None

    @property
    def row_entry_size(self) -> int:
        """Width in bytes of one entry of the frame's row-offset table."""
        return 4 if self.wide_row_table else 2


class RleImage:
    """A parsed `*.rle.mmp` frame table.

    Frames are stored row-major: `frames[row * columns + column]`. In unit and
    building sprites the column selects the facing direction and the row selects
    the animation frame.
    """

    def __init__(self, data: bytes) -> None:
        self._data = data
        self.image_class: int = 0
        self.columns: int = 0
        self.rows: int = 0
        self.frames: list[Frame] = []
        self.palette: list[tuple[int, int, int]] = []
        self.lut: tuple[int, ...] | None = None
        self._parse()

    # -- parsing ---------------------------------------------------------

    def _parse(self) -> None:
        data = self._data
        if data[: len(MAGIC)] != MAGIC:
            raise RleError("not an IMGRLE frame table (magic mismatch)")

        self.image_class, reserved_a, reserved_b, self.columns, self.rows = (
            struct.unpack_from("<IIIII", data, len(MAGIC))
        )
        if reserved_a or reserved_b:
            raise RleError("reserved header words are not zero")

        pos = HEADER_SIZE
        for index in range(self.columns * self.rows):
            frame, pos = self._parse_frame(index, pos)
            self.frames.append(frame)

        (palette_len,) = struct.unpack_from("<I", data, pos)
        pos += 4
        if pos + 4 * palette_len > len(data):
            raise RleError("palette overruns the frame table")
        for i in range(palette_len):
            blue, green, red = data[pos + 4 * i : pos + 4 * i + 3]
            self.palette.append((red, green, blue))
        pos += 4 * palette_len

        tail = len(data) - pos
        if tail == LUT_SIZE:
            self.lut = struct.unpack_from("<256H", data, pos)
            pos += LUT_SIZE
        elif tail:
            raise RleError(f"{tail} unexplained trailing bytes")

    def _parse_frame(self, index: int, pos: int) -> tuple[Frame, int]:
        data = self._data
        left, top, right, bottom = struct.unpack_from("<IIII", data, pos)
        pos += 16

        if right < left or bottom < top:
            # An empty frame: the encoder never touched the bounding box, so it
            # kept its "grows inwards" initial value. No RLE2 record follows.
            return (
                Frame(index, left, top, right, bottom, 0, 0, None, 0, 0, 0, False),
                pos,
            )

        if data[pos : pos + 4] != FRAME_MAGIC:
            raise RleError(f"frame {index}: expected RLE2 at offset {pos}")
        pos += 4
        width, height, pixel_format, data_size, zero, wide, color_key = (
            struct.unpack_from("<IIHIIII", data, pos)
        )
        pos += 26
        if data[pos : pos + 4] != STORE_MAGIC:
            raise RleError(f"frame {index}: expected pamm at offset {pos}")
        pos += 4
        (data_offset,) = struct.unpack_from("<I", data, pos)
        pos += 4

        if zero:
            raise RleError(f"frame {index}: reserved word is not zero")
        if wide not in (0, 1):
            raise RleError(f"frame {index}: bad wide-row-table flag {wide}")
        if left + width - 1 != right or top + height - 1 != bottom:
            raise RleError(f"frame {index}: bounding box disagrees with size")

        return (
            Frame(
                index,
                left,
                top,
                right,
                bottom,
                width,
                height,
                PixelFormat(pixel_format),
                data_size,
                data_offset,
                color_key,
                bool(wide),
            ),
            pos,
        )

    # -- access ----------------------------------------------------------

    def frame(self, row: int, column: int) -> Frame:
        """Return the frame at `row` (animation step) and `column` (direction)."""
        if not 0 <= row < self.rows or not 0 <= column < self.columns:
            raise IndexError(f"({row}, {column}) outside {self.rows}x{self.columns}")
        return self.frames[row * self.columns + column]

    @property
    def canvas_size(self) -> tuple[int, int]:
        """Smallest canvas that contains every frame's bounding box."""
        boxes = [f for f in self.frames if not f.empty]
        if not boxes:
            return (0, 0)
        return (max(f.right for f in boxes) + 1, max(f.bottom for f in boxes) + 1)

    def validate(self) -> None:
        """Assert the structural invariants described in the specification."""
        used = {f.pixel_format for f in self.frames if not f.empty}
        if len(used) > 1:
            raise RleError(f"mixed pixel formats in one image: {used}")

        for f in self.frames:
            if f.empty:
                continue
            if f.pixel_format is PixelFormat.INDEXED8 and not self.palette:
                raise RleError(f"frame {f.index}: 8-bit pixels but no palette")
            if f.pixel_format is PixelFormat.INDEXED8 and f.color_key >= len(
                self.palette
            ):
                raise RleError(f"frame {f.index}: colour key outside the palette")
            if f.wide_row_table != (f.data_size > 0xFFFF):
                raise RleError(f"frame {f.index}: wide flag disagrees with data size")

        if self.image_class == ImageClass.PLAYER_COLOR:
            head = self.palette[:PLAYER_COLOR_SLOTS]
            body = self.palette[PLAYER_COLOR_SLOTS : 2 * PLAYER_COLOR_SLOTS]
            if head != body:
                raise RleError("player-colour slots do not mirror palette 64..127")


class PixelStore:
    """The root-level `rle.mmp`: a headerless concatenation of frame payloads."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self._file = self.path.open("rb")
        self._size = self.path.stat().st_size

    def close(self) -> None:
        self._file.close()

    def __enter__(self) -> PixelStore:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def blob(self, frame: Frame) -> bytes:
        """Return the raw compressed payload of one frame."""
        if frame.empty:
            return b""
        if frame.data_offset + frame.data_size > self._size:
            raise RleError(f"frame {frame.index}: payload overruns {self.path.name}")
        self._file.seek(frame.data_offset)
        return self._file.read(frame.data_size)


# -- decompression -------------------------------------------------------


def row_offsets(frame: Frame, blob: bytes) -> tuple[int, ...]:
    """Return the byte offset of each row within `blob`."""
    code = "I" if frame.wide_row_table else "H"
    offsets = struct.unpack_from(f"<{frame.height}{code}", blob, 0)
    if offsets[0] != frame.height * frame.row_entry_size:
        raise RleError(f"frame {frame.index}: row table does not abut the row data")
    if list(offsets) != sorted(offsets):
        raise RleError(f"frame {frame.index}: row table is not monotonic")
    return offsets


def spans(frame: Frame, blob: bytes) -> Iterator[tuple[int, int, int, bytes]]:
    """Yield `(y, x, length, payload)` for every covered run of the frame.

    `payload` is empty for `MASK` frames, `length` bytes for `INDEXED8` and
    `2 * length` bytes for `RGB555`. Runs of length zero are skipped; they exist
    only so that gaps wider than 255 pixels can be expressed.
    """
    if frame.empty:
        return
    offsets = row_offsets(frame, blob)
    stride = frame.pixel_format.bytes_per_pixel

    for y in range(frame.height):
        start = offsets[y]
        end = offsets[y + 1] if y + 1 < frame.height else len(blob)
        pos, x = start, 0
        while pos + 1 < end:
            gap, length = blob[pos], blob[pos + 1]
            pos += 2
            x += gap
            if length:
                yield (y, x, length, blob[pos : pos + length * stride])
                pos += length * stride
                x += length
        if pos != end:
            raise RleError(f"frame {frame.index}, row {y}: trailing bytes in row")
        if x != frame.width:
            raise RleError(f"frame {frame.index}, row {y}: covers {x} of {frame.width}")


def _rgb555(value: int) -> tuple[int, int, int]:
    red, green, blue = (value >> 10) & 31, (value >> 5) & 31, value & 31
    return (red * 255 // 31, green * 255 // 31, blue * 255 // 31)


def decode_rgba(
    image: RleImage,
    frame: Frame,
    blob: bytes,
    player_colors: Sequence[tuple[int, int, int]] | None = None,
    shadow_color: tuple[int, int, int] = (0, 0, 0),
) -> bytes:
    """Expand one frame to a `width * height * 4` RGBA buffer.

    Uncovered pixels are fully transparent. `player_colors` optionally replaces
    palette slots 0..63, which is how the engine tints a unit for its owner;
    supply 64 entries or `None` to keep the neutral colours shipped in the file.
    """
    out = bytearray(frame.width * frame.height * 4)
    if frame.empty:
        return bytes(out)

    palette = list(image.palette)
    if player_colors is not None:
        if len(player_colors) != PLAYER_COLOR_SLOTS:
            raise ValueError(f"expected {PLAYER_COLOR_SLOTS} player colours")
        palette[:PLAYER_COLOR_SLOTS] = list(player_colors)

    for y, x, length, payload in spans(frame, blob):
        for i in range(length):
            if frame.pixel_format is PixelFormat.INDEXED8:
                red, green, blue = palette[payload[i]]
            elif frame.pixel_format is PixelFormat.RGB555:
                red, green, blue = _rgb555(payload[2 * i] | payload[2 * i + 1] << 8)
            else:
                red, green, blue = shadow_color
            at = (y * frame.width + x + i) * 4
            out[at : at + 4] = bytes((red, green, blue, 255))
    return bytes(out)


def write_png(path: str | Path, width: int, height: int, rgba: bytes) -> None:
    """Write an RGBA buffer as a PNG, using only the standard library."""
    import zlib

    raw = b"".join(
        b"\x00" + rgba[y * width * 4 : (y + 1) * width * 4] for y in range(height)
    )

    def chunk(kind: bytes, body: bytes) -> bytes:
        payload = kind + body
        return (
            struct.pack(">I", len(body))
            + payload
            + struct.pack(">I", zlib.crc32(payload))
        )

    Path(path).write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def main() -> None:
    import argparse
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from pak import PackFile  # noqa: PLC0415

    parser = argparse.ArgumentParser(description="Inspect a HMMSYS sprite.")
    parser.add_argument("pack", type=Path, help="pack holding the frame table")
    parser.add_argument("name", help="e.g. UNITS\\BBOWMAN\\ATTACK.RLE.MMP")
    parser.add_argument("--mmp", type=Path, help="path to the root-level rle.mmp")
    parser.add_argument("--frame", type=int, help="frame index to export")
    parser.add_argument("--png", type=Path, help="write the frame here")
    args = parser.parse_args()

    image = RleImage(PackFile(args.pack).read(args.name))
    image.validate()

    try:
        label = ImageClass(image.image_class).name
    except ValueError:
        label = "unknown"
    print(f"{args.name}: class {image.image_class} ({label})")
    print(f"  {image.columns} columns x {image.rows} rows, canvas {image.canvas_size}")
    print(f"  palette {len(image.palette)} entries, lut {'yes' if image.lut else 'no'}")

    if args.mmp is None:
        for f in image.frames:
            kind = "empty" if f.empty else f.pixel_format.name
            size = f"{f.width}x{f.height}"
            print(f"  {f.index:>4} {size:<10} {kind:<9} @{f.data_offset}")
        return

    with PixelStore(args.mmp) as store:
        wanted = image.frames if args.frame is None else [image.frames[args.frame]]
        for f in wanted:
            blob = store.blob(f)
            covered = sum(length for _, _, length, _ in spans(f, blob))
            print(f"  frame {f.index}: {f.width}x{f.height}, {covered} covered pixels")
            if args.png and args.frame is not None:
                write_png(args.png, f.width, f.height, decode_rgba(image, f, blob))
                print(f"  wrote {args.png}")


if __name__ == "__main__":
    main()
