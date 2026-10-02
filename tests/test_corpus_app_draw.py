"""What the live world draws, in the app: every object it holds, not only the first.

Playtest report #3: late in a sixteen-player match no unit was drawn, though
the minimap showed them and buildings, terrain and props drew. The cause was
the sprite renderer's palette lookup texture. Every sheet the world view
prepares takes a palette row, and so does its retint for each player that owns
something drawn from it; a row was spent per (sheet, player), and Balcans'
sixteen seats filled all 2,048 by turn 10. A sheet prepared after that had no
palette, so no art -- and whatever was drawn from it, most of the unit
animation sheets, was neither drawn nor picked, while the buildings and props
prepared before it still were. Rows are now shared by their contents.

The app's `drawn:` line, printed with each screenshot, says how many rows were
used, shared and refused; a refused row is art that did not resolve. Every app
run here is headless (`conftest.py` sets `IMPERIVM_HEADLESS=1`).
"""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

import corpus
from conftest import requires_game

pytestmark = requires_game

#: Sixteen seats: the most palettes a map can ask for.
BALCANS = "Scenarios/Balcans.BFHP"
SIZE = ["--width", "640", "--height", "480"]


@pytest.fixture(scope="module")
def app(game_dir) -> Path:
    candidates = [
        path
        for directory in corpus.tool_directories()
        if (path := directory / "engine" / "app" / "imperivm").is_file()
    ]
    if not candidates:
        pytest.skip("the app is not built")
    if not (game_dir / BALCANS).is_file():
        pytest.skip(f"{BALCANS} is not in this installation")
    return max(candidates, key=lambda p: p.stat().st_mtime)


def test_a_sixteen_player_match_has_a_palette_for_every_sheet(app, game_dir, tmp_path):
    shot = tmp_path / "balcans.png"
    done = subprocess.run(
        [str(app), "--game", str(game_dir), "--map", BALCANS, "--play", *SIZE,
         "--frames", "40", "--screenshot", str(shot)],
        capture_output=True, text=True, timeout=300,
    )
    out = done.stdout + done.stderr
    assert done.returncode == 0, out[-2000:]
    assert "palette lookup texture is full" not in out, out[-2000:]

    drawn = re.search(
        r"^drawn:\s+(\d+) object\(s\) in view in (\d+) layer\(s\), (\d+) without art; "
        r"palette (\d+) of (\d+) rows, (\d+) shared, (\d+) refused$",
        done.stdout, re.MULTILINE,
    )
    assert drawn, out[-2000:]
    in_view, layers, _, rows, capacity, shared, refused = map(int, drawn.groups())
    assert refused == 0
    assert 0 < rows < capacity
    # Sharing is what keeps a sixteen-player map in the texture at all: one row
    # per (sheet, player) filled 2,048 by turn 10.
    assert shared > rows
    # The opening view is the player's holding, and it draws.
    assert in_view > 0 and layers > 0
    assert shot.is_file()


#: Three crows on the ground in open country, and nothing else near them.
ISLAND_WAR = "Scenarios/Island War.BFHP"
CROWS = "4153,4847"
#: One line per layer drawn, with `IMPERIVM_DEBUG_VIEW` set (`world_view.cpp`).
LAYER = re.compile(
    r"^id (\d+) sheet -?\d+ grid (\d+)x(\d+) .* z (-?\d+) at -?\d+,(-?\d+)$", re.MULTILINE
)


def test_a_crow_in_the_air_is_drawn_above_its_shadow(app, game_dir):
    """Playtest report #10: a flock was drawn as one dark smear by a building.

    Every airborne crow was drawn standing on the ground under itself. The
    original lifts a flying unit's body layers (depth 1000 and 1050) by its
    altitude above the terrain and leaves the shadow (depth 800) on the ground,
    `sim::flying_lift`. The crow's flight sheet and its shadow are the two
    10 x 12 sheets, so the gap between them in one frame is the lift, give or
    take the two frames' own offsets -- 14 pixels at most before the fix, over
    a hundred once the birds climb.
    """
    if not (game_dir / ISLAND_WAR).is_file():
        pytest.skip(f"{ISLAND_WAR} is not in this installation")
    done = subprocess.run(
        [str(app), "--game", str(game_dir), "--map", ISLAND_WAR, "--play", "--no-fog",
         "--at", CROWS, *SIZE, "--frames", "400"],
        capture_output=True, text=True, timeout=300,
        env={**os.environ, "IMPERIVM_DEBUG_VIEW": "1"},
    )
    assert done.returncode == 0, (done.stdout + done.stderr)[-2000:]
    gap = 0
    frame: dict[int, dict[int, int]] = {}
    for match in LAYER.finditer(done.stdout):
        object_id, rows, columns, z, y = map(int, match.groups())
        if (rows, columns) != (10, 12):
            continue
        layers = frame.setdefault(object_id, {})
        layers[z] = y
        if 1000 in layers and 800 in layers:
            gap = max(gap, layers[800] - layers[1000])
            layers.clear()
    assert gap > 60


#: p1's walled town in Balcans' south-west, with its four gates in one view.
#: Its west gate opens for a unit walking out between turns 186 and 266 --
#: the scan counts units only (playtest #16), so a gate with nobody near it
#: stands closed, and the town in the south-east this used to watch opened
#: only because its own walls were taken for friends.
BALCANS_TOWN = "8400,22600"
#: The same line, with what it takes to put the layer's canvas origin back.
PLACED = re.compile(
    r"^id (\d+) sheet -?\d+ grid (\d+x\d+) .* lt -?\d+,(-?\d+) off -?\d+,(-?\d+) "
    r"layer \d+ z -?\d+ at -?\d+,(-?\d+)$",
    re.MULTILINE,
)


def test_an_open_gate_draws_its_portcullis_raised(app, game_dir):
    """Playtest report #6: an open gate was still drawn closed.

    No gate entity declares an animation. What opens is the third layer each
    one declares -- the portcullis -- which `gbr.exe` draws raised by the
    gate's position, 0 to 70 over two seconds (`sim/gate.hpp`). Every layer of
    an object is drawn from one canvas origin, so a layer's `y - offset - top`
    is the same for all of them -- except the portcullis's, which sits higher
    by its rise.
    `GATE_IDLE.VS` opens a gate with friends around it, and over the first
    twenty-five seconds of Balcans some gate in this town does, all the way and
    through the positions between. Before the fix no layer ever left its origin.
    """
    done = subprocess.run(
        [str(app), "--game", str(game_dir), "--map", BALCANS, "--play", "--no-fog",
         "--width", "2000", "--height", "1500", "--at", BALCANS_TOWN, "--frames", "1500"],
        capture_output=True, text=True, timeout=300,
        env={**os.environ, "IMPERIVM_DEBUG_VIEW": "1"},
    )
    assert done.returncode == 0, (done.stdout + done.stderr)[-2000:]
    # One object's layers are printed together, so a run of lines with one id
    # is that object in one frame. Only runs with a four-row sheet are
    # buildings; a crow in the air has its own lift, which is the test above's.
    runs: list[list[tuple[str, int]]] = []
    last = None
    for match in PLACED.finditer(done.stdout):
        object_id, grid = int(match.group(1)), match.group(2)
        top, offset, y = map(int, match.groups()[2:])
        if object_id != last:
            runs.append([])
            last = object_id
        runs[-1].append((grid, y - offset - top))
    seen: set[int] = set()
    for run in runs:
        if len(run) < 3 or not any(grid == "4x1" for grid, _ in run):
            continue
        origins = [origin for _, origin in run]
        ground = max(set(origins), key=origins.count)
        # A few pixels of the artists' own slack is not a portcullis's worth.
        moved = [i for i, origin in enumerate(origins) if abs(origin - ground) > 5]
        # What leaves the origin is the third layer the entity declares -- the
        # third drawn, after the shadow at depth 800 and the first wall -- and
        # it goes up.
        assert moved in ([], [2]), run
        rise = ground - origins[2] if moved else 0
        assert rise >= 0, run
        seen.add(rise)
    # Nothing is raised further than the original's 70.
    assert max(seen) == 70, sorted(seen)
    # And some gate was seen on its way up, not only standing open.
    assert any(0 < rise < 70 for rise in seen), sorted(seen)


#: Every layer line with what it takes to put its canvas origin back, both axes.
ORIGIN = re.compile(
    r"^id (\d+) sheet -?\d+ grid \d+x\d+ .* lt (-?\d+),(-?\d+) off (-?\d+),(-?\d+) "
    r"layer \d+ z (-?\d+) at (-?\d+),(-?\d+)$",
    re.MULTILINE,
)
#: The ring of a selected object, centre in window pixels (`world_view.cpp`).
RING = re.compile(r"^ring id (\d+) at (-?\d+),(-?\d+)$", re.MULTILINE)
#: The first of Balcans' fifteen eagles, which `initial_z` puts in the air at
#: spawn and `EAGLE_MOVE.VS` keeps there.
EAGLE = "class:Eagle@14"


def eagle_run(app: Path, game_dir: Path, steps: list[str], frames: int) -> tuple[int, str]:
    """A traced run of Balcans that looks at the eagle on turn 20; its id and output."""
    done = subprocess.run(
        [str(app), "--game", str(game_dir), "--map", BALCANS, "--play", "--no-fog",
         "--width", "1200", "--height", "900", "--frames", str(frames),
         "--input", ";".join(["turn:20", *steps])],
        capture_output=True, text=True, timeout=300,
        env={**os.environ, "IMPERIVM_DEBUG_VIEW": "1"},
    )
    assert done.returncode == 0, (done.stdout + done.stderr)[-2000:]
    looked = re.search(r"^view:\s+object (\d+) at", done.stdout, re.MULTILINE)
    assert looked, done.stdout[-2000:]
    return int(looked.group(1)), done.stdout


def layer_origins(out: str, object_id: int, depths: tuple[int, ...]) -> list[tuple[int, int]]:
    """Where one object's layers at these depths had their canvas origin, in order."""
    origins = []
    for match in ORIGIN.finditer(out):
        found, left, top, off_x, off_y, depth, x, y = map(int, match.groups())
        if found == object_id and depth in depths:
            origins.append((x - off_x - left, y - off_y - top))
    return origins


def test_an_airborne_birds_ring_is_drawn_under_its_body(app, game_dir):
    """Playtest report #10's residue: a flying bird's ring lay on the ground.

    `gbr.exe`'s flying visual update (0x0051b240) stores the body's lift in
    the visual's own `+0x600` pair as well as in each body layer, and the
    ring's draw (0x0062b5b0) adds that pair to the anchor, so the ring rises
    with the bird while the shadow stays down. Every layer of one object is
    drawn from one canvas origin -- the body's lifted -- so the ring's centre
    is the body layer's origin, and the shadow's is a hundred pixels or more
    below it. Before the fix the ring sat on the shadow's.
    """
    eagle, out = eagle_run(app, game_dir, [
        "key:P", "wait:2", f"look:{EAGLE}", "wait:3", f"select:{EAGLE}", "wait:3",
    ], 400)
    rings = [(int(x), int(y)) for i, x, y in RING.findall(out) if int(i) == eagle]
    # The crow's body is at depth 1000 and the eagle's at 1050; both lift.
    bodies = layer_origins(out, eagle, (1000, 1050))
    shadows = layer_origins(out, eagle, (800,))
    assert rings and bodies and shadows, out[-2000:]
    body, shadow = bodies[-1], shadows[-1]
    # Up in the air, and the ring with the body, not on the ground below it.
    assert shadow[1] - body[1] > 60, (body, shadow)
    assert rings[-1] == body, (rings[-1], body, shadow)


def test_a_bird_flies_between_the_ends_of_its_animation(app, game_dir):
    """Playtest report #10's residue: birds stepped one flight segment at a time.

    `Flying::PlayAnim` hands the animation its destination, and the
    original's visual (0x0053f4c0 -> 0x0062a0e0) runs its anchor across the
    screen from the leg's start to its end over the animation's length; the
    simulation here moves the object to the end at once
    (`sim::flight_progress`). Followed frame by frame, the eagle's shadow --
    its anchor, which nothing lifts -- now moves a few pixels on each turn
    instead of standing still for a whole animation and then jumping the
    length of its leg.
    """
    eagle, out = eagle_run(app, game_dir, [f"look:{EAGLE}", "wait:6"], 700)
    # The look moves the camera, which moves everything; a few frames on, the
    # view stands still.
    anchors = layer_origins(out, eagle, (800,))[10:]
    assert len(anchors) > 200, len(anchors)
    steps = [max(abs(a[0] - b[0]), abs(a[1] - b[1])) for a, b in zip(anchors, anchors[1:])]
    moving = [step for step in steps if step]
    # A turn's worth at a time: ten to fifteen pixels a turn, never a leg.
    assert len(moving) > 30, moving
    assert max(steps) < 30, sorted(steps)[-10:]

