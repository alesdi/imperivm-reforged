"""The passability rebuild against every shipped `Terrain.pass.grid`.

The original editor keeps a map's passability layer incrementally -- one
rebuild over a rectangle after each stroke, placement or removal, and the
layer written at save time is whatever those calls left (0x0054b260 writes
the live grid). So the shipped layers are the *output* of that rebuild, and
`immap passability` runs this engine's transcription of it (`core::edit::
rebuild_passability`, 0x00547700) over the whole map from nothing -- the
terrain's own rules, every object's `.pass` mask through the projection and
the height, every decoration's, and the frame -- and diffs the result
against the stored bytes.

Every cell of every layer must agree. That one number is what settles the
readings the header of `core/world/editor.hpp` records: that a mask is
stamped mirrored top to bottom and through the inverse projection, the
terrain bake's half-cell sampling, the deep-water shore rules, and the
frame. A mask flipped the other way, or laid flat, disagrees on every
building on every map.

`Packs/` is the exception, and is skipped rather than fixed. The four blank
templates' layers were not made by this rebuild -- one blocked row and
column at each edge where the frame puts two -- and they hold no object at
all, so there is nothing to check on them. `RandomMapSettlements.bfhp`, the
64 settlement templates, differs in 694 of its 4,194,304 cells: 301 cells
this rebuild sets that the stored layer does not, all of them the bottom
row of a settlement wall landing one cell lower, and 393 it does not set
that the stored one does, rims and blobs at spots where nothing stands
today. Both read as an earlier editor's stamp and an earlier pack's masks,
the same way the templates' frame does: the 23 maps a player can open,
edited later, all rebuild identically, walls included.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from conftest import requires_game

import corpus
from test_corpus_map_writer import _map_directories

pytestmark = requires_game

#: `Maps/<n>` directories across the install, less the six in `Packs/`.
AUTHORED_MAP_COUNT = 23


@pytest.fixture(scope="module")
def immap() -> Path:
    path, complaint = corpus.find_tool("immap", "IMPERIVM_IMMAP")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


def _authored(game_dir: Path) -> list[tuple[Path, int]]:
    packs = (game_dir / "Packs").resolve()
    return [
        (container, number)
        for container, number in _map_directories(game_dir)
        if container.resolve().parent != packs
    ]


def test_every_shipped_passability_layer_rebuilds_identically(immap, game_dir):
    directories = _authored(game_dir)
    assert len(directories) == AUTHORED_MAP_COUNT
    failures = []
    for container, number in directories:
        result = subprocess.run(
            [str(immap), "passability", str(game_dir), str(container), str(number)],
            capture_output=True,
            text=True,
            timeout=600,
        )
        if result.returncode != 0 or "identical" not in result.stdout:
            failures.append(
                f"{corpus.label(game_dir, container)} Maps/{number}: exit {result.returncode}\n"
                f"{result.stdout}{result.stderr}"
            )
    assert not failures, "\n".join(failures)
