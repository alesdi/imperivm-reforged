"""Data-free tests for the grid / passability reader and writer.

Specification: docs/formats/pass.md.

The writer is checked against ``synthetic.build_grid``, which packs the same
format independently, bit by bit: a reader and a writer that shared one wrong
idea about the bit order would round-trip each other and agree with nothing
on disk, so the oracle has to be a third implementation.
"""

from __future__ import annotations

import struct

import pytest

import synthetic
from imperivm.formats import pass_mask
from imperivm.formats.pass_mask import (
    HEADER_SIZE,
    PASS_CELL_SIZE,
    PASS_EXTENT,
    PASS_ORIGIN_CELL,
    PASS_WIDTH,
    Grid,
    GridError,
    is_blocked,
    parse,
    parse_path,
    write,
)


def one_bit_mask(marks: set[tuple[int, int]]) -> bytes:
    cells = synthetic.blank_pass_mask()
    for x, y in marks:
        cells[y][x] = 1
    return synthetic.build_grid(cells, cell_size=PASS_CELL_SIZE, bits_per_cell=1)


# -- header arithmetic ---------------------------------------------------


def test_the_retail_pass_configuration_is_2068_bytes():
    """Every shipped `.pass` file has the same header, so the same size."""
    data = one_bit_mask({(64, 64)})
    assert len(data) == 2068
    grid = parse(data)
    assert (grid.cell_size, grid.bits_per_cell) == (16, 1)
    assert (grid.extent_x, grid.extent_y) == (PASS_EXTENT, PASS_EXTENT)
    assert (grid.width, grid.height) == (PASS_WIDTH, PASS_WIDTH) == (128, 128)
    assert grid.stride == 16
    assert HEADER_SIZE + grid.stride * grid.height == len(data)


def test_the_terrain_grid_configuration():
    """`DATA\\RANDOM_MAP.TERRAIN.GRID` is the same container, a byte per cell."""
    cells = [[(x + y) % 41 for x in range(256)] for y in range(256)]
    data = synthetic.build_grid(cells, cell_size=64, bits_per_cell=8)
    assert len(data) == 65556
    grid = parse(data)
    assert (grid.cell_size, grid.bits_per_cell) == (64, 8)
    assert (grid.width, grid.height) == (256, 256)
    assert grid.stride == 256
    assert grid[3, 5] == (3 + 5) % 41


@pytest.mark.parametrize(
    ("cell_size", "bits", "width", "height"),
    [(16, 1, 128, 128), (64, 8, 256, 256), (16, 1, 8, 3), (1, 8, 5, 7)],
)
def test_header_arithmetic_predicts_the_file_size(cell_size, bits, width, height):
    cells = [[0] * width for _ in range(height)]
    data = synthetic.build_grid(cells, cell_size=cell_size, bits_per_cell=bits)
    stride = width * bits // 8
    assert len(data) == HEADER_SIZE + stride * height
    parse(data).validate()


# -- bit order -----------------------------------------------------------


def test_one_bit_cells_are_packed_least_significant_bit_first():
    """The one thing that is easy to get backwards, and looks plausible if you do."""
    cells = [[0] * 8 for _ in range(1)]
    cells[0][0] = 1
    cells[0][3] = 1
    data = synthetic.build_grid(cells, cell_size=16, bits_per_cell=1)
    assert data[HEADER_SIZE] == 0b00001001
    grid = parse(data)
    assert [grid[x, 0] for x in range(8)] == [1, 0, 0, 1, 0, 0, 0, 0]


def test_round_trips_an_irregular_footprint():
    marks = {(60, 60), (61, 60), (62, 60), (60, 61), (62, 61), (60, 62), (62, 62)}
    grid = parse(one_bit_mask(marks))
    grid.validate()
    assert {(x, y) for y in range(128) for x in range(128) if grid[x, y]} == marks
    assert grid.count() == len(marks)
    assert grid.bounds() == (60, 60, 62, 62)
    assert grid[61, 61] == 0, "the interior hole must survive"


def test_an_entirely_clear_mask_has_no_bounds():
    grid = parse(one_bit_mask(set()))
    assert grid.count() == 0
    assert grid.bounds() is None


def test_is_blocked_reads_set_bits_as_impassable():
    grid = parse(one_bit_mask({(10, 20)}))
    assert is_blocked(grid, 10, 20) is True
    assert is_blocked(grid, 11, 20) is False

    terrain = parse(
        synthetic.build_grid([[3] * 4] * 4, cell_size=64, bits_per_cell=8)
    )
    with pytest.raises(GridError, match="only applies to one-bit"):
        is_blocked(terrain, 0, 0)


# -- anchor --------------------------------------------------------------


def test_the_anchor_is_the_centre_of_the_grid():
    grid = parse(one_bit_mask({(PASS_ORIGIN_CELL, PASS_ORIGIN_CELL)}))
    assert PASS_ORIGIN_CELL == 64
    assert grid.world_origin_of(PASS_ORIGIN_CELL, PASS_ORIGIN_CELL) == (0, 0)
    assert grid.world_origin_of(0, 0) == (-1024, -1024)
    assert grid.world_origin_of(128, 128) == (1024, 1024)
    assert grid.world_origin_of(65, 64) == (PASS_CELL_SIZE, 0)


def test_parse_path_reads_from_disk(tmp_path):
    path = tmp_path / "TOWER.PASS"
    path.write_bytes(one_bit_mask({(1, 2)}))
    grid = parse_path(path)
    assert grid[1, 2] == 1


# -- error paths ---------------------------------------------------------


def test_rejects_bad_magic():
    with pytest.raises(GridError, match="magic mismatch"):
        parse(b"GRID" + bytes(32))


def test_rejects_a_zero_cell_size():
    data = bytearray(one_bit_mask(set()))
    struct.pack_into("<I", data, 4, 0)
    with pytest.raises(GridError, match="zero cell size"):
        parse(bytes(data))


def test_rejects_a_file_size_that_disagrees_with_the_header():
    with pytest.raises(GridError, match="expected 2068"):
        parse(one_bit_mask(set()) + b"\0")


def test_rejects_an_unsupported_bit_depth():
    # Two bits per cell is a shape the container could carry and nothing in the
    # install does; it is refused as such, before the size arithmetic.
    data = bytearray(one_bit_mask(set()))
    struct.pack_into("<I", data, 8, 2)
    with pytest.raises(GridError, match="unsupported bits_per_cell 2"):
        parse(bytes(data))


def test_reads_four_and_sixteen_bit_cells():
    """The terrain layer of a blank template, and the decor layer."""
    nibbles = [[(x * 3 + y) % 16 for x in range(8)] for y in range(3)]
    grid = parse(synthetic.build_grid(nibbles, cell_size=64, bits_per_cell=4))
    assert grid.stride == 4
    assert [list(row) for row in grid.cells] == nibbles

    words = [[(x * 4099 + y * 17) & 0xFFFF for x in range(5)] for y in range(2)]
    grid = parse(synthetic.build_grid(words, cell_size=64, bits_per_cell=16))
    assert grid.stride == 10
    assert [list(row) for row in grid.cells] == words


def test_validate_rejects_an_extent_that_is_not_a_multiple_of_the_cell_size():
    grid = pass_mask.Grid(16, 1, 2050, 2048, ((0,) * 128,) * 128)
    with pytest.raises(GridError, match="extent_x 2050"):
        grid.validate()
    grid = pass_mask.Grid(16, 1, 2048, 2050, ((0,) * 128,) * 128)
    with pytest.raises(GridError, match="extent_y 2050"):
        grid.validate()


def test_validate_rejects_a_one_bit_width_that_is_not_a_whole_number_of_bytes():
    grid = pass_mask.Grid(16, 1, 16 * 12, 16 * 2, ((0,) * 12,) * 2)
    with pytest.raises(GridError, match="whole number of bytes"):
        grid.validate()


def test_validate_rejects_a_cell_array_of_the_wrong_shape():
    grid = pass_mask.Grid(16, 8, 16 * 4, 16 * 4, ((0,) * 4,) * 3)
    with pytest.raises(GridError, match="3 rows decoded"):
        grid.validate()
    grid = pass_mask.Grid(16, 8, 16 * 4, 16 * 4, ((0,) * 5,) * 4)
    with pytest.raises(GridError, match="5 columns decoded"):
        grid.validate()


# -- writing -------------------------------------------------------------


@pytest.mark.parametrize("bits", [1, 4, 8, 16])
def test_write_is_the_inverse_of_parse_at_every_depth(bits):
    width = 16 if bits < 8 else 5
    limit = (1 << bits) - 1
    cells = [[(x * 7919 + y * 104729 + (x ^ y)) & limit for x in range(width)] for y in range(3)]
    data = synthetic.build_grid(cells, cell_size=32, bits_per_cell=bits)
    grid = parse(data)
    assert write(grid) == data
    # And a grid built from the values alone packs to the same bytes: there is
    # nothing in a cell beyond its value.
    rebuilt = Grid.from_cells(cells, cell_size=32, bits_per_cell=bits)
    assert rebuilt == grid
    assert write(rebuilt) == data


def test_write_packs_one_bit_cells_least_significant_first():
    grid = Grid.from_cells([[1, 0, 0, 1, 0, 0, 0, 0]], cell_size=16, bits_per_cell=1)
    data = write(grid)
    assert data[HEADER_SIZE:] == bytes([0b00001001])
    assert parse(data) == grid


def test_write_puts_the_even_cell_in_the_low_nibble():
    grid = Grid.from_cells([[0xA, 0x5]], cell_size=64, bits_per_cell=4)
    assert write(grid)[HEADER_SIZE:] == bytes([0x5A])


def test_write_stores_sixteen_bit_cells_little_endian():
    grid = Grid.from_cells([[0x1234]], cell_size=64, bits_per_cell=16)
    assert write(grid)[HEADER_SIZE:] == bytes([0x34, 0x12])


def test_write_emits_the_extents_in_world_units():
    grid = Grid.from_cells([[0] * 4] * 2, cell_size=64, bits_per_cell=8)
    header = struct.unpack_from("<4s4I", write(grid), 0)
    # Not (4, 2): the header carries the world square, and the width and
    # height are derived from it.
    assert header == (b"DIRG", 64, 8, 256, 128)


def test_write_reproduces_the_retail_pass_configuration():
    data = one_bit_mask({(60, 60), (61, 60), (64, 64)})
    grid = parse(data)
    assert len(write(grid)) == 2068
    assert write(grid) == data


def test_with_cell_edits_by_replacement_and_keeps_the_depth():
    grid = Grid.from_cells([[0] * 8], cell_size=16, bits_per_cell=1)
    edited = grid.with_cell(3, 0, 1)
    assert edited[3, 0] == 1
    assert grid[3, 0] == 0, "the original is immutable"
    assert write(edited)[HEADER_SIZE:] == bytes([0b00001000])

    nibbles = Grid.from_cells([[3, 5]], cell_size=64, bits_per_cell=4)
    assert nibbles.max_cell_value == 15
    assert write(nibbles.with_cell(0, 0, 15))[HEADER_SIZE:] == bytes([0x5F])
    # Terrain type 16 does not fit a four-bit template layer, and is refused
    # rather than clamped or masked to a different terrain.
    with pytest.raises(GridError, match="does not fit 4 bit"):
        nibbles.with_cell(0, 0, 16)
    with pytest.raises(GridError, match="outside 2x1"):
        nibbles.with_cell(2, 0, 1)


def test_from_cells_refuses_what_the_reader_would_misread():
    with pytest.raises(GridError, match="does not fit 1 bit"):
        Grid.from_cells([[2] * 8], cell_size=16, bits_per_cell=1)
    with pytest.raises(GridError, match="whole number of bytes"):
        Grid.from_cells([[0] * 12], cell_size=16, bits_per_cell=1)
    with pytest.raises(GridError, match="columns decoded"):
        Grid.from_cells([[0] * 4, [0] * 3], cell_size=64, bits_per_cell=8)
    with pytest.raises(GridError, match="unsupported bits_per_cell"):
        Grid.from_cells([[0] * 4], cell_size=64, bits_per_cell=2)


def test_write_validates_before_it_packs():
    """A hand-built grid whose table disagrees with its header is refused, not
    written into a file the reader would then misread."""
    with pytest.raises(GridError, match="3 rows decoded"):
        write(pass_mask.Grid(16, 8, 16 * 4, 16 * 4, ((0,) * 4,) * 3))
    with pytest.raises(GridError, match="does not fit 8 bit"):
        write(pass_mask.Grid(16, 8, 16 * 4, 16, ((256, 0, 0, 0),)))
