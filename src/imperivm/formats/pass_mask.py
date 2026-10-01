"""Reader and writer for the HMMSYS grid container, used by passability masks (`.pass`).

Specification: docs/formats/pass.md

Reference implementation: correctness and legibility over speed. The reader is the
ground truth the C++ port is checked against; the writer (:func:`write`) is its
inverse, and ``write(parse(data)) == data`` for every grid in the retail install --
the 597 passability masks, ``DATA\\RANDOM_MAP.TERRAIN.GRID``, and the six terrain
layers of each of the 29 maps (``tests/test_corpus_grid.py``).

Named ``pass_mask`` because ``pass`` is a Python keyword.
"""

from __future__ import annotations

import struct
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"DIRG"  # the FourCC 'GRID' stored little-endian
HEADER_SIZE = 20

#: The cell depths the container is known to carry: 1 (a passability mask, and a
#: map's passability layer), 4 (the transition layer, and the terrain layer of the
#: five blank templates), 8 (height, light, terrain, the random-map terrain grid)
#: and 16 (decor). Anything else is refused as unsupported rather than guessed at.
SUPPORTED_DEPTHS = (1, 4, 8, 16)

# The parameters every shipped .pass file uses.
PASS_CELL_SIZE = 16
PASS_EXTENT = 2048
PASS_WIDTH = PASS_EXTENT // PASS_CELL_SIZE  # 128
PASS_ORIGIN_CELL = PASS_WIDTH // 2  # 64; see "Anchor" in the specification


class GridError(Exception):
    """Raised when a file does not conform to the grid format."""


@dataclass(frozen=True)
class Grid:
    """A rectangular grid of cells covering a square patch of world space.

    ``cell_size`` is the width of one cell in world units, so the grid spans
    ``extent_x`` by ``extent_y`` world units in total. ``cells`` is row-major;
    ``cells[y][x]`` is the value of the cell in column ``x`` of row ``y``.
    """

    cell_size: int
    bits_per_cell: int
    extent_x: int
    extent_y: int
    cells: tuple[tuple[int, ...], ...]

    @classmethod
    def from_cells(
        cls, cells: Iterable[Iterable[int]], *, cell_size: int, bits_per_cell: int
    ) -> "Grid":
        """A grid over a row-major table of cell values.

        The extents are the table's shape times the cell size -- what the header
        will carry, in world units. The result is validated, so a table whose
        rows differ in length, or whose values do not fit the depth, is refused
        here rather than at :func:`write`.
        """
        rows = tuple(tuple(int(v) for v in row) for row in cells)
        height = len(rows)
        width = len(rows[0]) if rows else 0
        grid = cls(cell_size, bits_per_cell, width * cell_size, height * cell_size, rows)
        grid.validate()
        return grid

    @property
    def width(self) -> int:
        return self.extent_x // self.cell_size

    @property
    def height(self) -> int:
        return self.extent_y // self.cell_size

    @property
    def stride(self) -> int:
        """Bytes per stored row."""
        return self.width * self.bits_per_cell // 8

    @property
    def max_cell_value(self) -> int:
        """The largest value a cell of this depth holds: 1, 15, 255 or 65535."""
        return (1 << self.bits_per_cell) - 1

    def __getitem__(self, position: tuple[int, int]) -> int:
        x, y = position
        return self.cells[y][x]

    def with_cell(self, x: int, y: int, value: int) -> "Grid":
        """A copy with one cell changed. The grid is immutable, so editing is
        by replacement; the value must fit the depth, and is not clamped."""
        if not (0 <= x < self.width and 0 <= y < self.height):
            raise GridError(f"cell ({x}, {y}) is outside {self.width}x{self.height}")
        if not (0 <= value <= self.max_cell_value):
            raise GridError(
                f"value {value} does not fit {self.bits_per_cell} bit(s) per cell"
            )
        rows = list(self.cells)
        row = list(rows[y])
        row[x] = value
        rows[y] = tuple(row)
        return Grid(self.cell_size, self.bits_per_cell, self.extent_x, self.extent_y, tuple(rows))

    def bounds(self) -> tuple[int, int, int, int] | None:
        """Inclusive ``(x0, y0, x1, y1)`` box of the non-zero cells, or None."""
        columns = [x for row in self.cells for x, v in enumerate(row) if v]
        rows = [y for y, row in enumerate(self.cells) for v in row if v]
        if not columns:
            return None
        return min(columns), min(rows), max(columns), max(rows)

    def count(self) -> int:
        """Number of non-zero cells."""
        return sum(1 for row in self.cells for v in row if v)

    def world_origin_of(self, x: int, y: int) -> tuple[int, int]:
        """World offset of cell ``(x, y)``'s top-left corner from the anchor.

        The anchor is the centre of the grid; see the specification for the
        caveat about the vertical anchor.
        """
        return (
            x * self.cell_size - self.extent_x // 2,
            y * self.cell_size - self.extent_y // 2,
        )

    def validate(self) -> None:
        """Assert the structural invariants described in the specification."""
        if self.cell_size <= 0 or self.extent_x % self.cell_size:
            raise GridError(f"extent_x {self.extent_x} is not a multiple of the cell size")
        if self.extent_y % self.cell_size:
            raise GridError(f"extent_y {self.extent_y} is not a multiple of the cell size")
        if self.bits_per_cell not in SUPPORTED_DEPTHS:
            raise GridError(f"unsupported bits_per_cell {self.bits_per_cell}")
        if (self.width * self.bits_per_cell) % 8:
            raise GridError(f"width {self.width} is not a whole number of bytes")
        if len(self.cells) != self.height:
            raise GridError(f"{len(self.cells)} rows decoded, expected {self.height}")
        limit = self.max_cell_value
        for y, row in enumerate(self.cells):
            if len(row) != self.width:
                raise GridError(f"{len(row)} columns decoded, expected {self.width}")
            for x, value in enumerate(row):
                if not (0 <= value <= limit):
                    raise GridError(
                        f"cell ({x}, {y}) holds {value}, which does not fit "
                        f"{self.bits_per_cell} bit(s)"
                    )


def _unpack_row(raw: bytes, width: int, bits_per_cell: int) -> tuple[int, ...]:
    """One stored row to cell values. Sub-byte cells run least significant first."""
    if bits_per_cell == 1:
        return tuple((raw[x >> 3] >> (x & 7)) & 1 for x in range(width))
    if bits_per_cell == 4:
        return tuple((raw[x >> 1] >> ((x & 1) * 4)) & 0xF for x in range(width))
    if bits_per_cell == 8:
        return tuple(raw)
    if bits_per_cell == 16:
        return struct.unpack(f"<{width}H", raw)
    raise GridError(f"unsupported bits_per_cell {bits_per_cell}")


def _pack_row(row: tuple[int, ...], bits_per_cell: int) -> bytes:
    """The inverse of :func:`_unpack_row`: cell ``x`` of a one-bit row goes to bit
    ``x % 8`` of byte ``x // 8``, a nibble row puts even cells in the low nibble,
    and wider cells are little-endian."""
    if bits_per_cell == 1:
        packed = bytearray(len(row) // 8)
        for x, value in enumerate(row):
            if value:
                packed[x >> 3] |= 1 << (x & 7)
        return bytes(packed)
    if bits_per_cell == 4:
        packed = bytearray(len(row) // 2)
        for x, value in enumerate(row):
            packed[x >> 1] |= (value & 0xF) << ((x & 1) * 4)
        return bytes(packed)
    if bits_per_cell == 8:
        return bytes(row)
    if bits_per_cell == 16:
        return struct.pack(f"<{len(row)}H", *row)
    raise GridError(f"unsupported bits_per_cell {bits_per_cell}")


def parse(data: bytes, name: str = "<bytes>") -> Grid:
    """Decode a grid container.

    One-bit grids are packed least-significant-bit first: cell ``x`` of a row
    lives in bit ``x % 8`` of byte ``x // 8``. Four-bit grids likewise put the
    even cell in the low nibble; sixteen-bit cells are little-endian.
    """
    if data[: len(MAGIC)] != MAGIC:
        raise GridError(f"{name}: not a HMMSYS grid (magic mismatch)")

    cell_size, bits_per_cell, extent_x, extent_y = struct.unpack_from("<4I", data, 4)
    if cell_size == 0:
        raise GridError(f"{name}: zero cell size")
    if bits_per_cell not in SUPPORTED_DEPTHS:
        raise GridError(f"{name}: unsupported bits_per_cell {bits_per_cell}")
    width = extent_x // cell_size
    height = extent_y // cell_size
    stride = width * bits_per_cell // 8
    expected = HEADER_SIZE + stride * height
    if len(data) != expected:
        raise GridError(f"{name}: file is {len(data)} bytes, expected {expected}")

    rows: list[tuple[int, ...]] = []
    for y in range(height):
        raw = data[HEADER_SIZE + y * stride : HEADER_SIZE + (y + 1) * stride]
        rows.append(_unpack_row(raw, width, bits_per_cell))

    grid = Grid(cell_size, bits_per_cell, extent_x, extent_y, tuple(rows))
    grid.validate()
    return grid


def write(grid: Grid) -> bytes:
    """Encode a grid container: the 20-byte header from the grid's own fields,
    then the rows, top to bottom, with no padding beyond the stride.

    The extents go out in world units as the header carries them, not as the
    width and height. The grid is validated first, so a table that does not
    match its header or a value that does not fit its depth is refused rather
    than written into a file the reader would then misread.
    """
    grid.validate()
    out = bytearray(MAGIC)
    out += struct.pack(
        "<4I", grid.cell_size, grid.bits_per_cell, grid.extent_x, grid.extent_y
    )
    for row in grid.cells:
        out += _pack_row(row, grid.bits_per_cell)
    return bytes(out)


def parse_path(path: str | Path) -> Grid:
    path = Path(path)
    return parse(path.read_bytes(), path.name)


def is_blocked(grid: Grid, x: int, y: int) -> bool:
    """True when cell ``(x, y)`` of a passability mask is impassable."""
    if grid.bits_per_cell != 1:
        raise GridError("is_blocked only applies to one-bit passability masks")
    return bool(grid[x, y])


def main() -> None:
    import argparse

    parser = argparse.ArgumentParser(description="Inspect a HMMSYS grid or .pass mask.")
    parser.add_argument("grid", type=Path)
    parser.add_argument("--png", type=Path, help="write the whole grid as a PNG")
    args = parser.parse_args()

    grid = parse_path(args.grid)
    box = grid.bounds()
    print(f"{args.grid}: {grid.width}x{grid.height} cells of {grid.cell_size} world units")
    print(f"  {grid.bits_per_cell} bit(s) per cell, extent {grid.extent_x}x{grid.extent_y}")
    print(f"  {grid.count()} non-zero cells, bounds {box}")

    if args.png:
        from apf import write_png

        peak = max((v for row in grid.cells for v in row), default=1) or 1
        pixels = [v * 255 // peak for row in grid.cells for v in row]
        write_png(args.png, grid.width, grid.height, pixels)
        print(f"  wrote {args.png}")
        return

    if box is None:
        print("  (empty mask)")
        return
    x0, y0, x1, y1 = box
    print(f"  origin cell is ({PASS_ORIGIN_CELL}, {PASS_ORIGIN_CELL}); '+' marks it")
    for y in range(y0, y1 + 1):
        line = "".join("#" if grid[x, y] else "." for x in range(x0, x1 + 1))
        if y0 <= PASS_ORIGIN_CELL <= y1 and y == PASS_ORIGIN_CELL:
            index = PASS_ORIGIN_CELL - x0
            if 0 <= index < len(line):
                line = line[:index] + "+" + line[index + 1 :]
        print("  " + line)


if __name__ == "__main__":
    main()
