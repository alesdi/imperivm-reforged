"""Corpus tests for the grid reader and writer. Specification: docs/formats/pass.md.

Grids are found by the `DIRG` magic, never by name. 546 of them are called
`*.PASS`, but 51 more are called `PASS` with no extension at all, and one --
the random-map terrain grid -- is a different shape entirely. Any discovery
that goes by file name misses a twelfth of the corpus.

The writer's claim is the strongest one a writer can make: **what was read is
what is written**, byte for byte, for every grid in the install -- the 598 in
the packs and the 174 terrain layers in the 29 map directories, at all four
depths the data ships. Both the Python reference (`pass_mask.write`) and the
C++ port (`immap grid-roundtrip`, which also rebuilds each grid through
`Grid::set_cell` and requires the same bytes) are held to it here.

One honest limit, found by injecting the fault: the corpus **cannot** tell the
four-bit nibble order. Every four-bit byte the game ships has equal nibbles
(the transition layers are all zero; the templates' terrain layer is `0x33`
throughout), so a writer with the nibbles swapped passes every test in this
file. The synthetic tests in `test_grid.py` pin the convention; the data does
not prove it. `test_every_four_bit_byte_ships_with_equal_nibbles` below keeps
that fact visible, so that the first grid to break it is noticed.
"""

from __future__ import annotations

import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

from conftest import requires_game

import corpus
from imperivm.formats import pass_mask
from imperivm.formats.bfhp import MAGIC as HPFS_MAGIC, BlockFile
from imperivm.formats.lzis import decompress

pytestmark = requires_game

#: Passability masks: 546 named `*.PASS` plus 51 named `PASS` with no extension.
MASK_COUNT = 597
EXTENSIONLESS_MASKS = 51

#: Every mask is 2,048 world units square at 16 units per cell, one bit per
#: cell: 20 header bytes + 128 * 128 / 8.
MASK_SIZE = 2_068

#: The one grid in the packs that is not a passability mask.
TERRAIN_GRID = "DATA\\RANDOM_MAP.TERRAIN.GRID"
TERRAIN_GRID_SIZE = 65_556

#: `Maps/<n>` directories across the containers (21 containers with one, the
#: conquest with seven, and `Packs/RandomMapSettlements.bfhp` behind its LZIS
#: wrapper), and six `Terrain.*.grid` layers in every one of them.
MAP_DIRECTORY_COUNT = 29
LAYER_COUNT = 6 * MAP_DIRECTORY_COUNT

#: Every grid in the install: the pack grids plus the map layers.
GRID_COUNT = MASK_COUNT + 1 + LAYER_COUNT

#: The depths the layers ship at. The terrain layer is 8 bits on the 24
#: authored maps and 4 on the five blank templates in `Packs/`.
LAYER_DEPTHS = {"pass": {1}, "height": {8}, "light": {8}, "terrain": {4, 8}, "decor": {16}, "trans": {4}}


@dataclass(frozen=True)
class Found:
    label: str
    name: str
    data: bytes


@pytest.fixture(scope="module")
def grids(packs) -> list[Found]:
    """Every stored file in the packs that starts with the grid magic."""
    out: list[Found] = []
    for label, pack in packs:
        for entry in pack.entries:
            data = pack.read(entry.name)
            if data[: len(pass_mask.MAGIC)] == pass_mask.MAGIC:
                out.append(Found(label, entry.name, data))
    return out


@pytest.fixture(scope="module")
def containers(game_dir: Path) -> list[tuple[Path, BlockFile]]:
    """Every `.bfhp` in the install, by content: bare `HPFS` images and the LZIS
    streams that decompress to one. Nothing here reads a file name."""
    found: list[tuple[Path, BlockFile]] = []
    for path in corpus._walk(game_dir):
        try:
            with path.open("rb") as handle:
                head = handle.read(4)
        except OSError:  # pragma: no cover - unreadable file in the install
            continue
        if head == HPFS_MAGIC:
            found.append((path, BlockFile(path)))
        elif head == b"LZIS":
            try:
                data = decompress(path.read_bytes())
            except Exception:  # noqa: BLE001 - not every LZIS stream is a container
                continue
            if data[:4] == HPFS_MAGIC:
                found.append((path, BlockFile(path, data=data)))
    return found


@pytest.fixture(scope="module")
def layer_grids(game_dir: Path, containers) -> list[Found]:
    """Every `Terrain.*.grid` in every map directory of every container, found
    by the grid magic like the pack grids."""
    out: list[Found] = []
    for path, block_file in containers:
        label = corpus.label(game_dir, path)
        for entry in block_file.entries:
            if entry.is_dir:
                continue
            data = block_file.read(entry.name)
            if data[: len(pass_mask.MAGIC)] == pass_mask.MAGIC:
                out.append(Found(label, entry.name.replace("\\", "/"), data))
    return out


@pytest.fixture(scope="module")
def immap() -> Path:
    path, complaint = corpus.find_tool("immap", "IMPERIVM_IMMAP")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


def predicted_size(data: bytes) -> int:
    """File size implied by the header alone."""
    cell_size, bits_per_cell, extent_x, extent_y = struct.unpack_from("<4I", data, 4)
    width = extent_x // cell_size
    height = extent_y // cell_size
    return pass_mask.HEADER_SIZE + (width * bits_per_cell // 8) * height


def test_masks_are_discovered_by_magic_not_by_name(grids):
    masks = [g for g in grids if g.name != TERRAIN_GRID]
    assert len(masks) == MASK_COUNT

    hidden = [g for g in masks if "." not in g.name.rpartition("\\")[2]]
    assert len(hidden) == EXTENSIONLESS_MASKS
    assert {g.name.rpartition("\\")[2] for g in hidden} == {"PASS"}


def test_every_mask_is_exactly_2068_bytes(grids):
    for found in grids:
        if found.name == TERRAIN_GRID:
            continue
        assert len(found.data) == MASK_SIZE, f"{found.label}:{found.name}"


def test_header_arithmetic_predicts_the_file_size_of_every_mask(grids):
    for found in grids:
        if found.name == TERRAIN_GRID:
            continue
        assert predicted_size(found.data) == len(found.data), f"{found.label}:{found.name}"
        grid = pass_mask.parse(found.data, found.name)
        grid.validate()
        assert (grid.width, grid.height, grid.bits_per_cell) == (128, 128, 1)


def test_header_arithmetic_predicts_the_file_size_of_the_terrain_grid(grids):
    """The same arithmetic, on the one grid with different geometry."""
    found = next(g for g in grids if g.name == TERRAIN_GRID)
    assert len(found.data) == TERRAIN_GRID_SIZE
    assert predicted_size(found.data) == TERRAIN_GRID_SIZE

    grid = pass_mask.parse(found.data, found.name)
    grid.validate()
    assert grid.bits_per_cell == 8
    assert pass_mask.HEADER_SIZE + grid.stride * grid.height == TERRAIN_GRID_SIZE


# -- writing -------------------------------------------------------------


def test_the_map_layers_are_found_by_magic_in_every_map_directory(layer_grids):
    assert len(layer_grids) == LAYER_COUNT
    by_layer: dict[str, set[int]] = {}
    for found in layer_grids:
        name = found.name.rpartition("/")[2]
        assert name.startswith("Terrain.") and name.endswith(".grid"), f"{found.label}:{found.name}"
        layer = name[len("Terrain.") : -len(".grid")]
        grid = pass_mask.parse(found.data, found.name)
        grid.validate()
        by_layer.setdefault(layer, set()).add(grid.bits_per_cell)
    assert by_layer == LAYER_DEPTHS


def test_the_python_writer_reproduces_every_grid_byte_for_byte(grids, layer_grids):
    """`write(parse(data)) == data` for all 772: the whole retail corpus, at
    all four depths, through the reference implementation."""
    everything = grids + layer_grids
    assert len(everything) == GRID_COUNT
    failures = []
    for found in everything:
        grid = pass_mask.parse(found.data, found.name)
        if pass_mask.write(grid) != found.data:
            failures.append(f"{found.label}:{found.name}")
    assert not failures, failures


def test_a_grid_rebuilt_from_its_cells_alone_is_the_same_bytes(grids, layer_grids):
    """`Grid.from_cells` knows nothing but the values and the geometry, and
    packs to the stored file regardless: there is no hidden state in a cell,
    and no padding bit the original editor left set. One of each depth, and
    every one-bit mask, because that is the depth that is easy to get wrong."""
    everything = grids + layer_grids
    chosen: dict[int, Found] = {}
    for found in everything:
        bits = struct.unpack_from("<I", found.data, 8)[0]
        chosen.setdefault(bits, found)
    assert set(chosen) == {1, 4, 8, 16}
    sample = list(chosen.values()) + [g for g in grids if g.name != TERRAIN_GRID]
    for found in sample:
        grid = pass_mask.parse(found.data, found.name)
        rebuilt = pass_mask.Grid.from_cells(
            grid.cells, cell_size=grid.cell_size, bits_per_cell=grid.bits_per_cell
        )
        assert rebuilt == grid, f"{found.label}:{found.name}"
        assert pass_mask.write(rebuilt) == found.data, f"{found.label}:{found.name}"


def test_the_engine_writer_and_setter_reproduce_every_grid_byte_for_byte(
    immap, packs, containers
):
    """The C++ port, over the same 772 files: `core::write_grid` on the parsed
    grid must give the stored bytes, and so must a fresh `OwnedGrid` of the
    same geometry painted cell by cell through `Grid::set_cell`."""
    files = sorted(set(packs.paths) | {path for path, _ in containers})
    result = subprocess.run(
        [str(immap), "grid-roundtrip", *map(str, files)],
        capture_output=True,
        text=True,
        timeout=600,
    )
    lines = [line for line in result.stdout.splitlines() if line.strip()]
    bad = [line for line in lines if not line.endswith(" identical identical")]
    assert result.returncode == 0, result.stderr + "\n".join(bad[:20])
    assert not bad, bad[:20]
    assert len(lines) == GRID_COUNT, (len(lines), result.stderr)
    assert f"{GRID_COUNT} grids, 0 failures" in result.stderr


def test_every_four_bit_byte_ships_with_equal_nibbles(layer_grids):
    """The reason the corpus is silent on the nibble order, pinned. If an
    install ever carries a four-bit layer that is not uniform within a byte,
    this fails, and that grid becomes the evidence `docs/formats/pass.md` is
    waiting for -- check which nibble order makes it render right, then
    retire this test."""
    four_bit = [g for g in layer_grids if struct.unpack_from("<I", g.data, 8)[0] == 4]
    assert len(four_bit) == MAP_DIRECTORY_COUNT + 5, "29 transition layers and 5 templates"
    for found in four_bit:
        body = found.data[pass_mask.HEADER_SIZE :]
        mixed = sum(1 for byte in body if (byte & 0xF) != (byte >> 4))
        assert mixed == 0, f"{found.label}:{found.name} has {mixed} bytes with unequal nibbles"
