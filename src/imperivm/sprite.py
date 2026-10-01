"""Composing HMMSYS sprites into sheets, and taking them apart again.

Specification: docs/formats/rle.md — this module is the layer above
:mod:`imperivm.formats.rle`, which reads the bytes. Everything here is about
turning the frames of one `.rle.mmp` into something a PNG can hold, and back.

Three decisions are worth stating up front, because they are what make the
export faithful rather than merely pretty.

**Indexed stays indexed.** An 8-bit sprite is exported as a PNG with a PLTE, not
flattened to RGBA. Palette entries 0..63 of a `player_color` image are the team
block — the engine swaps a per-player ramp into them at load time. Flattening
bakes the shipped neutral colours in and destroys the swap. See
:mod:`imperivm.png`.

**Transparency without an alpha channel.** The sprite format has no alpha; gaps
in the run encoding are the only transparency. An indexed PNG expresses that
with a `tRNS` chunk, which needs one palette index reserved. The natural
candidate is the frame's own `color_key`, the colour the artist painted the
background in, which the specification records as never occurring in the
compressed data. :func:`choose_transparent_index` picks it when it really is
unused in this image and falls back to any other unused index, so the choice is
verified per image rather than assumed.

**Tight cells, offsets in the manifest.** A sheet lays frames out on a grid of
uniform cells the size of the largest frame, and each frame sits at its cell's
top-left. The frame's real position on the entity's shared canvas goes in the
manifest as `left`/`top`. Laying the frames out on the canvas instead would
align a body sheet with its shadow sheet for free, but at the cost of a large
mostly-empty PNG per sprite, 3,898 times over. A subtraction is cheaper than
the pixels. The `sheet` command does use canvas coordinates, because there the
point is to look at the animation.
"""

from __future__ import annotations

import colorsys
from dataclasses import dataclass
from typing import Iterable, Iterator, Sequence

from imperivm import png
from imperivm.formats.rle import (
    Frame,
    ImageClass,
    PixelFormat,
    PixelStore,
    RleError,
    RleImage,
    row_offsets,
    spans,
)

PLAYER_COLOR_SLOTS = 64
SHADOW_GRAY = 255  # value written for a covered pixel of a 1-bit mask


class SpriteError(Exception):
    """Raised when a sprite cannot be composed or re-encoded."""


# -- layout ---------------------------------------------------------------


@dataclass(frozen=True)
class Layout:
    """Where each frame of a `rows` x `columns` grid lands in a sheet.

    `cell_width`/`cell_height` are uniform. `origin_x`/`origin_y` are subtracted
    from a frame's canvas coordinates before it is blitted, so a layout with a
    zero origin and canvas-sized cells reproduces the entity's own alignment
    while a layout sized to the largest frame packs them tightly.
    """

    rows: int
    columns: int
    cell_width: int
    cell_height: int
    canvas_relative: bool
    origin_x: int = 0
    origin_y: int = 0

    @property
    def width(self) -> int:
        return max(1, self.cell_width * self.columns)

    @property
    def height(self) -> int:
        return max(1, self.cell_height * self.rows)

    def cell(self, index: int) -> tuple[int, int]:
        """Top-left corner of the cell holding frame `index`."""
        row, column = divmod(index, self.columns)
        return (column * self.cell_width, row * self.cell_height)

    def place(self, frame: Frame) -> tuple[int, int]:
        """Top-left corner at which `frame` is drawn inside the sheet."""
        cell_x, cell_y = self.cell(frame.index)
        if not self.canvas_relative:
            return (cell_x, cell_y)
        return (cell_x + frame.left - self.origin_x, cell_y + frame.top - self.origin_y)


def populated(image: RleImage) -> list[Frame]:
    return [f for f in image.frames if not f.empty]


def tight_layout(image: RleImage) -> Layout:
    """A grid of cells the size of the largest frame, frames at the cell origin."""
    frames = populated(image)
    cell_w = max((f.width for f in frames), default=0)
    cell_h = max((f.height for f in frames), default=0)
    return Layout(image.rows, image.columns, cell_w, cell_h, canvas_relative=False)


def canvas_layout(image: RleImage) -> Layout:
    """A grid of cells covering the union of every frame's bounding box.

    Frames keep their relative positions, so an animation read down a column
    moves the way the engine moves it, and a body sheet and its shadow sheet
    line up when both are built against the same union box.
    """
    frames = populated(image)
    if not frames:
        return Layout(image.rows, image.columns, 0, 0, canvas_relative=True)
    left = min(f.left for f in frames)
    top = min(f.top for f in frames)
    right = max(f.right for f in frames)
    bottom = max(f.bottom for f in frames)
    return Layout(
        image.rows,
        image.columns,
        right - left + 1,
        bottom - top + 1,
        canvas_relative=True,
        origin_x=left,
        origin_y=top,
    )


def union_box(*images: RleImage) -> tuple[int, int, int, int]:
    """Smallest box containing every populated frame of every given image."""
    frames = [f for image in images for f in populated(image)]
    if not frames:
        return (0, 0, 0, 0)
    return (
        min(f.left for f in frames),
        min(f.top for f in frames),
        max(f.right for f in frames),
        max(f.bottom for f in frames),
    )


# -- decoding into a sheet ------------------------------------------------


def frame_coverage(frame: Frame, blob: bytes) -> int:
    """Total number of covered pixels in the frame, from its run lengths.

    Walks the same rows :func:`imperivm.formats.rle.spans` walks, so it inherits
    the row-exhaustion and width checks; a frame that does not tile its own
    bounding box raises rather than returning a plausible number.
    """
    return sum(length for _, _, length, _ in spans(frame, blob))


def used_indices(image: RleImage, store: PixelStore) -> set[int]:
    """Every palette index that appears in the covered pixels of any frame."""
    seen: set[int] = set()
    for frame in image.frames:
        if frame.empty or frame.pixel_format is not PixelFormat.INDEXED8:
            continue
        for _, _, _, payload in spans(frame, store.blob(frame)):
            seen.update(payload)
    return seen


def choose_transparent_index(image: RleImage, used: set[int]) -> int | None:
    """Pick the palette index that an indexed export will mark transparent.

    Prefers the frame's `color_key`, which is what the sprite was authored
    against, and only if no covered pixel actually uses it. Otherwise takes the
    lowest unused index, preferring one at or above the team block so the export
    never quietly reserves a team-colour slot. Returns `None` when every index
    is in use, which forces the caller to fall back to RGBA.
    """
    size = len(image.palette)
    if size == 0:
        return None

    team_block = image.image_class == ImageClass.PLAYER_COLOR
    keys = {f.color_key for f in image.frames if not f.empty}
    if len(keys) == 1:
        key = keys.pop()
        # Never reserve a team slot on a player-colour image: a swapped-in ramp
        # would land on a hole. No retail file needs this guard — 470 images do
        # key below 64 and every one of them is class 1, with no team block —
        # but the cost of being wrong here is an invisible unit.
        if key < size and key not in used and not (team_block and key < PLAYER_COLOR_SLOTS):
            return key

    for candidate in range(PLAYER_COLOR_SLOTS, size):
        if candidate not in used:
            return candidate
    if size < 256:
        return size  # room to append a fresh entry
    for candidate in range(PLAYER_COLOR_SLOTS):
        if candidate not in used:
            return candidate
    return None


def blit_indexed(
    frame: Frame, blob: bytes, dst: bytearray, stride: int, x0: int, y0: int
) -> None:
    for y, x, length, payload in spans(frame, blob):
        at = (y0 + y) * stride + x0 + x
        dst[at : at + length] = payload


def blit_mask(
    frame: Frame, blob: bytes, dst: bytearray, stride: int, x0: int, y0: int
) -> None:
    for y, x, length, _ in spans(frame, blob):
        at = (y0 + y) * stride + x0 + x
        dst[at : at + length] = b"\xff" * length


_RGB555 = bytes(round(v * 255 / 31) for v in range(32))


def blit_rgba(
    frame: Frame,
    blob: bytes,
    dst: bytearray,
    stride: int,
    x0: int,
    y0: int,
    palette: Sequence[tuple[int, int, int]] | None = None,
    shadow_color: tuple[int, int, int] = (0, 0, 0),
) -> None:
    """Expand one frame into an RGBA sheet buffer. `stride` is in bytes."""
    fmt = frame.pixel_format
    flat = b""
    if fmt is PixelFormat.INDEXED8:
        assert palette is not None
        flat = b"".join(bytes((r, g, b, 255)) for r, g, b in palette)
    shadow = bytes(shadow_color) + b"\xff"

    for y, x, length, payload in spans(frame, blob):
        at = (y0 + y) * stride + (x0 + x) * 4
        if fmt is PixelFormat.INDEXED8:
            dst[at : at + 4 * length] = b"".join(
                flat[4 * i : 4 * i + 4] for i in payload
            )
        elif fmt is PixelFormat.RGB555:
            out = bytearray(4 * length)
            for i in range(length):
                value = payload[2 * i] | payload[2 * i + 1] << 8
                out[4 * i] = _RGB555[(value >> 10) & 31]
                out[4 * i + 1] = _RGB555[(value >> 5) & 31]
                out[4 * i + 2] = _RGB555[value & 31]
                out[4 * i + 3] = 255
            dst[at : at + 4 * length] = out
        else:
            dst[at : at + 4 * length] = shadow * length


# -- team colour ----------------------------------------------------------

NAMED_COLORS: dict[str, tuple[int, int, int]] = {
    "red": (200, 30, 30),
    "blue": (40, 70, 200),
    "green": (30, 150, 50),
    "yellow": (225, 200, 40),
    "orange": (230, 120, 20),
    "purple": (140, 50, 180),
    "cyan": (40, 190, 200),
    "magenta": (210, 50, 170),
    "white": (235, 235, 235),
    "black": (35, 35, 35),
    "grey": (130, 130, 130),
    "gray": (130, 130, 130),
    "brown": (130, 85, 40),
    "teal": (30, 140, 140),
    "pink": (230, 140, 170),
}


def parse_color(text: str) -> tuple[int, int, int]:
    """Accept a name from :data:`NAMED_COLORS`, `#rrggbb`, or `r,g,b`."""
    key = text.strip().lower()
    if key in NAMED_COLORS:
        return NAMED_COLORS[key]
    if key.startswith("#"):
        key = key[1:]
    if len(key) == 6 and all(c in "0123456789abcdef" for c in key):
        return (int(key[0:2], 16), int(key[2:4], 16), int(key[4:6], 16))
    parts = [p for p in key.replace(";", ",").split(",") if p]
    if len(parts) == 3:
        try:
            values = [int(p) for p in parts]
        except ValueError:
            pass
        else:
            if all(0 <= v <= 255 for v in values):
                return (values[0], values[1], values[2])
    raise SpriteError(f"cannot read {text!r} as a colour")


def team_ramp(
    neutral: Sequence[tuple[int, int, int]], color: tuple[int, int, int]
) -> list[tuple[int, int, int]]:
    """Re-hue the 64 shipped neutral team colours towards `color`.

    The real per-player ramps are not in the sprite files — the specification is
    explicit that they live elsewhere and have not been sourced. What *is* in
    the file is the neutral block the artist painted, and the specification is
    equally explicit that slots 0..63 are not a sorted gradient, so laying a
    generated dark-to-light ramp over the indices would scramble the shading.

    This therefore keeps each slot's own brightness and takes only hue and
    saturation from `color`. Shading, folds and highlights survive; the cloth
    changes colour. It is a faithful *preview*, not a reproduction of a
    particular player's palette, and is documented as such.
    """
    hue, _, saturation = colorsys.rgb_to_hsv(*(c / 255 for c in color))
    out: list[tuple[int, int, int]] = []
    for red, green, blue in neutral:
        _, _, value = colorsys.rgb_to_hsv(red / 255, green / 255, blue / 255)
        r, g, b = colorsys.hsv_to_rgb(hue, saturation, value)
        out.append((round(r * 255), round(g * 255), round(b * 255)))
    return out


def apply_team(
    palette: Sequence[tuple[int, int, int]], color: tuple[int, int, int]
) -> list[tuple[int, int, int]]:
    """Return `palette` with its 64 team slots re-huing towards `color`."""
    out = list(palette)
    if len(out) < PLAYER_COLOR_SLOTS:
        return out
    out[:PLAYER_COLOR_SLOTS] = team_ramp(out[:PLAYER_COLOR_SLOTS], color)
    return out


# -- sheet building -------------------------------------------------------


@dataclass
class Sheet:
    """A composed sheet, ready to hand to :mod:`imperivm.png`."""

    image: png.Image
    layout: Layout
    transparent_index: int | None = None


def _palette_for(
    image: RleImage, team: tuple[int, int, int] | None
) -> list[tuple[int, int, int]]:
    palette = list(image.palette)
    if team is not None and image.image_class == ImageClass.PLAYER_COLOR:
        palette = apply_team(palette, team)
    return palette


def build_indexed_sheet(
    image: RleImage,
    store: PixelStore,
    layout: Layout,
    *,
    team: tuple[int, int, int] | None = None,
    transparent: int | None = None,
) -> Sheet:
    """Compose an 8-bit sprite as an indexed PNG image, palette intact."""
    palette = _palette_for(image, team)
    if transparent is None:
        raise SpriteError("an indexed sheet needs a transparent palette index")
    if transparent == len(palette):
        palette = palette + [(0, 0, 0)]

    stride = layout.width
    buffer = bytearray(bytes((transparent,)) * (stride * layout.height))
    for frame in image.frames:
        if frame.empty:
            continue
        x0, y0 = layout.place(frame)
        blit_indexed(frame, store.blob(frame), buffer, stride, x0, y0)

    trns = bytearray(b"\xff" * len(palette))
    trns[transparent] = 0
    return Sheet(
        png.Image(
            layout.width,
            layout.height,
            png.ColorType.INDEXED,
            bytes(buffer),
            palette,
            bytes(trns),
        ),
        layout,
        transparent,
    )


def build_mask_sheet(image: RleImage, store: PixelStore, layout: Layout) -> Sheet:
    """Compose a 1-bit shadow mask as an 8-bit greyscale coverage image."""
    stride = layout.width
    buffer = bytearray(stride * layout.height)
    for frame in image.frames:
        if frame.empty:
            continue
        x0, y0 = layout.place(frame)
        blit_mask(frame, store.blob(frame), buffer, stride, x0, y0)
    return Sheet(
        png.Image(layout.width, layout.height, png.ColorType.GRAY, bytes(buffer)),
        layout,
    )


def build_rgba_sheet(
    image: RleImage,
    store: PixelStore,
    layout: Layout,
    *,
    team: tuple[int, int, int] | None = None,
    shadow_color: tuple[int, int, int] = (0, 0, 0),
) -> Sheet:
    """Compose any sprite as flattened RGBA. Lossy for indexed sources."""
    palette = _palette_for(image, team)
    stride = layout.width * 4
    buffer = bytearray(stride * layout.height)
    for frame in image.frames:
        if frame.empty:
            continue
        x0, y0 = layout.place(frame)
        blit_rgba(
            frame,
            store.blob(frame),
            buffer,
            stride,
            x0,
            y0,
            palette,
            shadow_color,
        )
    return Sheet(
        png.Image(layout.width, layout.height, png.ColorType.RGBA, bytes(buffer)),
        layout,
    )


def composite_over(base: bytearray, top: bytes, count: int) -> None:
    """Alpha-composite `top` over `base` in place; both are RGBA buffers."""
    for i in range(0, count * 4, 4):
        alpha = top[i + 3]
        if alpha == 0:
            continue
        if alpha == 255:
            base[i : i + 4] = top[i : i + 4]
            continue
        inv = 255 - alpha
        for c in range(3):
            base[i + c] = (top[i + c] * alpha + base[i + c] * inv) // 255
        base[i + 3] = alpha + base[i + 3] * inv // 255


# -- re-encoding ----------------------------------------------------------
#
# The encoder exists to prove the format is understood well enough to write it,
# not just read it. It is validated by round-tripping retail frames: encode the
# decoded pixels again and compare bytes with the original payload. See
# docs/tools/imsprite.md for the measured result.


def encode_row(coverage: Sequence[int], pixels: bytes, stride: int) -> bytes:
    """Encode one row as the format's `(gap, length, payload)` triples.

    `coverage` is one flag per pixel; `pixels` holds `stride` bytes per pixel and
    is ignored when `stride` is zero, which is how a 1-bit mask is written. Gaps
    longer than 255 become repeated `(255, 0)` pairs and runs longer than 255 are
    split, both of which the specification describes and the retail data uses.
    A row that ends in transparency still emits its trailing gap, so the pairs
    always account for exactly `width` pixels.
    """
    out = bytearray()
    width = len(coverage)
    pos = 0
    while pos < width:
        gap_start = pos
        while pos < width and not coverage[pos]:
            pos += 1
        gap = pos - gap_start

        run_start = pos
        while pos < width and coverage[pos]:
            pos += 1
        run = pos - run_start

        while gap > 255:
            out += b"\xff\x00"
            gap -= 255
        if run == 0:
            out += bytes((gap, 0))
            continue
        first = min(run, 255)
        out += bytes((gap, first))
        if stride:
            out += pixels[run_start * stride : (run_start + first) * stride]
        done = first
        while done < run:
            chunk = min(run - done, 255)
            out += bytes((0, chunk))
            if stride:
                at = (run_start + done) * stride
                out += pixels[at : at + chunk * stride]
            done += chunk
    return bytes(out)


def encode_frame(
    coverage: Sequence[int],
    pixels: bytes,
    width: int,
    height: int,
    stride: int,
    *,
    wide_rows: bool | None = None,
) -> bytes:
    """Encode a whole frame payload: row offset table followed by the rows."""
    rows = [
        encode_row(
            coverage[y * width : (y + 1) * width],
            pixels[y * width * stride : (y + 1) * width * stride],
            stride,
        )
        for y in range(height)
    ]
    if wide_rows is None:
        narrow = 2 * height + sum(len(r) for r in rows)
        wide_rows = narrow > 0xFFFF
    entry = 4 if wide_rows else 2
    code = "<I" if wide_rows else "<H"

    import struct

    table = bytearray()
    offset = entry * height
    for row in rows:
        table += struct.pack(code, offset)
        offset += len(row)
    return bytes(table) + b"".join(rows)


def decode_frame_pixels(
    frame: Frame, blob: bytes, fill: int = 0
) -> tuple[bytearray, bytearray]:
    """Return `(coverage, pixels)` for one frame, both row-major and unpadded."""
    stride = frame.pixel_format.bytes_per_pixel if frame.pixel_format else 0
    count = frame.width * frame.height
    coverage = bytearray(count)
    pixels = bytearray(bytes((fill,)) * (count * stride))
    for y, x, length, payload in spans(frame, blob):
        at = y * frame.width + x
        coverage[at : at + length] = b"\x01" * length
        if stride:
            pixels[at * stride : (at + length) * stride] = payload
    return coverage, pixels


def reencode_frame(frame: Frame, blob: bytes) -> bytes:
    """Decode a retail frame and encode it again. Should reproduce `blob`."""
    coverage, pixels = decode_frame_pixels(frame, blob)
    stride = frame.pixel_format.bytes_per_pixel if frame.pixel_format else 0
    return encode_frame(
        coverage,
        bytes(pixels),
        frame.width,
        frame.height,
        stride,
        wide_rows=frame.wide_row_table,
    )
