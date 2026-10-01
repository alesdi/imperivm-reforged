"""The zoom map's ground, in the app: the roads are drawn on it.

Playtest report #20: roads did not show on the zoom map (Space). It coloured
each cell by the mean of one texture -- the base, replaced only by a layer
holding all four of the cell's corners -- and a road one vertex wide never
holds four, so the roads the settlement templates lay on Crossroads (layers
7 and 19, 2,248 vertices) came out as the ground under them. The original
composes the zoom map as it composes the ground, every layer at a corner
through its mask, from `Minimap.pak`'s own tiles (0x006186a0, see
`core/world/zoom_ground.hpp`); `engine/tests/test_zoom_ground.cpp` pins the
rule on synthetic cells, and this checks it on a shipped map.

The app prints a `zoom map:` line when it composes the picture: its size, the
minimap tiles and masks that loaded, and the pixels at least half road. Every
app run here is headless (`conftest.py` sets `IMPERIVM_HEADLESS=1`).
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

import corpus
from conftest import requires_game

pytestmark = requires_game

CROSSROADS = "Scenarios/Crossroads.BFHP"
ZOOM = re.compile(
    r"^zoom map:\s+(\d+)x(\d+) at 1/(\d+), (\d+) tiles, (\d+) masks, (\d+) pixels of road$",
    re.MULTILINE,
)


@pytest.fixture(scope="module")
def app(game_dir) -> Path:
    candidates = [
        path
        for directory in corpus.tool_directories()
        if (path := directory / "engine" / "app" / "imperivm").is_file()
    ]
    if not candidates:
        pytest.skip("the app is not built")
    if not (game_dir / CROSSROADS).is_file():
        pytest.skip(f"{CROSSROADS} is not in this installation")
    return max(candidates, key=lambda p: p.stat().st_mtime)


def test_the_roads_are_drawn_on_the_zoom_map(app, game_dir):
    done = subprocess.run(
        [str(app), "--game", str(game_dir), "--map", CROSSROADS, "--play",
         "--width", "1100", "--height", "850", "--input", "wait:5;key:Space;wait:5",
         "--frames", "20"],
        capture_output=True, text=True, timeout=300,
    )
    out = done.stdout + done.stderr
    assert done.returncode == 0, out[-2000:]
    zoom = ZOOM.search(done.stdout)
    assert zoom, out[-2000:]
    width, height, divisor, tiles, masks, road = map(int, zoom.groups())
    # A 16,384-unit map at a sixteenth.
    assert (width, height, divisor) == (1024, 736, 16)
    # Every layer's minimap tile ships but the waves' (10), which names a
    # `%season%/waves.bmp` that is not in the pack; and the fourteen C and
    # fourteen D masks.
    assert tiles == 40
    assert masks == 28
    # 26,035 pixels when this was written: a road vertex is about 11.5 of
    # them at this scale, and the old rule drew a road only in a cell it
    # wholly covered.
    assert road > 13000
