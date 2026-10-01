"""Minimal PNG encoder and decoder, standard library only.

The toolchain exports to PNG because it is the one lossless image format that
every viewer, editor and browser reads without help. We implement it directly
rather than depend on Pillow, so that the tools stay installable with nothing
but a Python interpreter.

Four colour types are supported, and the choice matters for this game:

`GRAY`
    8-bit single channel. Used for shadow masks and font coverage.

`RGB` / `RGBA`
    8 bits per channel. Used for terrain and for flattened sprite output.

`INDEXED`
    8-bit palette indices with a PLTE and optional tRNS chunk. This is the
    important one. Unit sprites carry team colour as a palette swap over
    indices 0 to 63, so flattening them to RGBA discards the information that
    makes recolouring possible. Exporting indexed keeps a sprite reversible and
    lets an editor recolour it the way the engine does.

Interlacing is not supported, in either direction. No shipped asset needs it.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from enum import IntEnum

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class ColorType(IntEnum):
    """PNG colour type codes, named for how this project uses them."""

    GRAY = 0
    RGB = 2
    INDEXED = 3
    GRAY_ALPHA = 4
    RGBA = 6


_CHANNELS = {
    ColorType.GRAY: 1,
    ColorType.RGB: 3,
    ColorType.INDEXED: 1,
    ColorType.GRAY_ALPHA: 2,
    ColorType.RGBA: 4,
}


class PngError(Exception):
    """Raised when PNG data is malformed or uses an unsupported feature."""


@dataclass(frozen=True)
class Image:
    """A decoded image.

    `pixels` is one bytes object of `width * height * channels`, row major with
    no padding. For `INDEXED`, each byte is a palette index and `palette` holds
    the RGB triples; `transparency`, when present, gives the alpha for the
    leading palette entries.
    """

    width: int
    height: int
    color_type: ColorType
    pixels: bytes
    palette: list[tuple[int, int, int]] | None = None
    transparency: bytes | None = None

    @property
    def channels(self) -> int:
        return _CHANNELS[self.color_type]

    def validate(self) -> None:
        expected = self.width * self.height * self.channels
        if len(self.pixels) != expected:
            raise PngError(
                f"pixel buffer is {len(self.pixels)} bytes, expected {expected}"
            )
        if self.color_type is ColorType.INDEXED:
            if not self.palette:
                raise PngError("indexed image has no palette")
            if len(self.palette) > 256:
                raise PngError(f"palette has {len(self.palette)} entries, max 256")
            if self.pixels and max(self.pixels) >= len(self.palette):
                raise PngError("pixel index outside the palette")


def _chunk(tag: bytes, payload: bytes) -> bytes:
    body = tag + payload
    return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))


def encode(image: Image, *, compression: int = 9) -> bytes:
    """Serialise `image` to PNG bytes.

    Each row is written with filter type 0 (None). Real filtering would compress
    better, but these files are intermediate artefacts, not shipped assets, and
    unfiltered rows keep the encoder short enough to audit.
    """
    image.validate()
    stride = image.width * image.channels

    raw = bytearray()
    for y in range(image.height):
        raw.append(0)
        raw += image.pixels[y * stride : (y + 1) * stride]

    out = bytearray(PNG_SIGNATURE)
    out += _chunk(
        b"IHDR",
        struct.pack(">IIBBBBB", image.width, image.height, 8, int(image.color_type), 0, 0, 0),
    )
    if image.color_type is ColorType.INDEXED:
        assert image.palette is not None
        plte = bytearray()
        for r, g, b in image.palette:
            plte += bytes((r, g, b))
        out += _chunk(b"PLTE", bytes(plte))
        if image.transparency is not None:
            out += _chunk(b"tRNS", image.transparency)
    out += _chunk(b"IDAT", zlib.compress(bytes(raw), compression))
    out += _chunk(b"IEND", b"")
    return bytes(out)


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode(data: bytes) -> Image:
    """Parse PNG bytes into an :class:`Image`."""
    if data[:8] != PNG_SIGNATURE:
        raise PngError("not a PNG file")

    pos = 8
    header: tuple[int, int, int, int] | None = None
    palette: list[tuple[int, int, int]] | None = None
    transparency: bytes | None = None
    idat = bytearray()

    while pos + 8 <= len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        tag = data[pos + 4 : pos + 8]
        payload = data[pos + 8 : pos + 8 + length]
        pos += 12 + length

        if tag == b"IHDR":
            w, h, depth, ctype, _comp, _filt, interlace = struct.unpack(">IIBBBBB", payload)
            if depth != 8:
                raise PngError(f"bit depth {depth} is not supported, only 8")
            if interlace:
                raise PngError("interlaced PNG is not supported")
            header = (w, h, ctype, depth)
        elif tag == b"PLTE":
            palette = [tuple(payload[i : i + 3]) for i in range(0, len(payload), 3)]  # type: ignore[misc]
        elif tag == b"tRNS":
            transparency = bytes(payload)
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break

    if header is None:
        raise PngError("no IHDR chunk")
    width, height, ctype, _ = header
    color_type = ColorType(ctype)
    channels = _CHANNELS[color_type]
    stride = width * channels

    raw = zlib.decompress(bytes(idat))
    if len(raw) != (stride + 1) * height:
        raise PngError("decompressed data does not match the declared dimensions")

    out = bytearray(stride * height)
    prev = bytearray(stride)
    src = 0
    for y in range(height):
        ftype = raw[src]
        src += 1
        row = bytearray(raw[src : src + stride])
        src += stride
        if ftype == 1:
            for i in range(channels, stride):
                row[i] = (row[i] + row[i - channels]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                left = row[i - channels] if i >= channels else 0
                upleft = prev[i - channels] if i >= channels else 0
                row[i] = (row[i] + _paeth(left, prev[i], upleft)) & 0xFF
        elif ftype != 0:
            raise PngError(f"unknown row filter {ftype}")
        out[y * stride : (y + 1) * stride] = row
        prev = row

    return Image(width, height, color_type, bytes(out), palette, transparency)


def write(path, image: Image) -> None:
    """Encode `image` and write it to `path`."""
    from pathlib import Path

    Path(path).write_bytes(encode(image))


def read(path) -> Image:
    """Read and decode the PNG at `path`."""
    from pathlib import Path

    return decode(Path(path).read_bytes())
