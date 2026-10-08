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
    # Each placement of the shadow with its frame's two game times: the
    # instant it was drawn at and the turn end the world stood at. The look
    # moves the camera, which moves everything; a few frames on, the view
    # stands still.
    placed: list[tuple[int, int, tuple[int, int]]] = []
    frame = None
    for line in out.splitlines():
        if match := FRAME.match(line):
            frame = (int(match.group(3)), int(match.group(2)))
        elif frame is not None and (match := ORIGIN.match(line)):
            found, left, top, off_x, off_y, depth, x, y = map(int, match.groups())
            if found == eagle and depth == 800:
                placed.append((*frame, (x - off_x - left, y - off_y - top)))
    placed = placed[10:]
    assert len(placed) > 200, len(placed)
    pairs = [(max(abs(a[2][0] - b[2][0]), abs(a[2][1] - b[2][1])), b[1] - a[0], a, b)
             for a, b in zip(placed, placed[1:])]
    # It moves from frame to frame, not once a leg.
    moving = [step for step, *_ in pairs if step]
    assert len(moving) > 30, moving
    # And no faster than it flies. How far that is between two frames is a
    # matter of game time, not of frames -- a frame the machine was slow to
    # draw spans several turns -- and the span is from the first frame's drawn
    # instant to the second's turn end: a leg begun inside a turn is drawn
    # where the turn's end has it, since the view carries on only an
    # animation it saw at the turn before (`TurnGlide::anim_elapsed`). The
    # eagle's legs from turn 20 to 425 are 51 to 179 pixels long and take 400
    # to 1,200 ms; the fastest, 90 pixels in 400 ms, runs 0.225 pixels a game
    # millisecond. The bound is 0.3, and two pixels for the truncation at
    # either end. A leg drawn at once covers its length in one frame, 0.42
    # pixels a millisecond and more. A fixed 30 pixels between frames up to
    # 150 ms of drawn time apart, which this used to ask, is less than a fast
    # leg and its start cover: under load, 30 to 40 pixels.
    assert all(span >= 0 for _, span, *_ in pairs), "game time ran backwards between frames"
    fast = [(step, span, a, b) for step, span, a, b in pairs if 10 * step > 3 * span + 20]
    assert not fast, fast[:5]


#: Numantia's legionaries, one of whom is sent walking across the field.
NUMANTIA = "Adventures/GreatBattles/2_Great_Battles_Numantia.bfhp"
#: The line the world view prints ahead of each placement's layers.
FRAME = re.compile(r"^frame turn (\d+) time (\d+) drawn (\d+)$")
LAYER_ORIGIN = re.compile(
    r"^id (\d+) sheet -?\d+ grid \d+x\d+ row (\d+) col \d+ .* lt (-?\d+),(-?\d+) "
    r"off (-?\d+),(-?\d+) layer \d+ z (-?\d+) at (-?\d+),(-?\d+)$"
)


def test_a_walking_unit_is_drawn_between_turns(app, game_dir):
    """Playtest report #10's residue: every unit moved once a turn, not every frame.

    `gbr.exe`'s frame (0x0051ea30) runs game time up to the real clock's share
    of the turn's lockstep window (0x00528e40, 0x00528b40), and a moving
    object is drawn where its animation has it at that instant (0x0053d830).
    This engine runs a turn whole, and the view now draws the instant between
    the two turn ends (`sim/glide.hpp`). With 400 ms turns a walking
    legionary is drawn at several places inside one turn, at drawn times
    between the turn's ends, its walk cycle stepping as it goes -- and its
    ring under it every frame, so what is clicked is what is drawn.

    The order is given ten frames in, as it was before a move posted ahead
    of the first turn was found never to be walked: Numantia's opening
    sequence replaced it. The first turn now runs as the match opens, as
    `gbr.exe`'s first game window does
    (`test_an_order_given_as_the_match_opens_is_walked`), so the tenth frame
    is after it on any machine.
    """
    if not (game_dir / NUMANTIA).is_file():
        pytest.skip(f"{NUMANTIA} is not in this installation")
    done = subprocess.run(
        [str(app), "--game", str(game_dir), "--map", NUMANTIA, "--play", "--no-fog",
         "--width", "1024", "--height", "768", "--frames", "400", "--turn-interval", "400",
         "--input", "wait:10;select:class:RHastatus;wait:2;rclick:700,500"],
        capture_output=True, text=True, timeout=300,
        env={**os.environ, "IMPERIVM_DEBUG_VIEW": "1"},
    )
    assert done.returncode == 0, (done.stdout + done.stderr)[-2000:]
    ordered = re.search(r"^queue:\s+object (\d+) holds \d+ \| move", done.stdout, re.MULTILINE)
    assert ordered, done.stdout[-2000:]
    walker = int(ordered.group(1))

    # Per placement: the turn, the turn's end, the drawn time, and the
    # walker's body origin, walk row and ring.
    frames: list[dict] = []
    for line in done.stdout.splitlines():
        if match := FRAME.match(line):
            turn, time, drawn = map(int, match.groups())
            frames.append({"turn": turn, "time": time, "drawn": drawn, "rings": []})
        elif frames and (match := LAYER_ORIGIN.match(line)):
            found, row, left, top, off_x, off_y, depth, x, y = map(int, match.groups())
            if found == walker and depth == 1000:
                frames[-1]["origin"] = (x - off_x - left, y - off_y - top)
                frames[-1]["row"] = row
        elif frames and (match := RING.match(line)):
            if int(match.group(1)) == walker:
                frames[-1]["rings"].append((int(match.group(2)), int(match.group(3))))
    drawn = [f for f in frames if "origin" in f]
    assert len(drawn) > 60, len(drawn)
    # The view does not move (nothing in the script scrolls it), so a place
    # on the screen is a place in the world: the walker walked.
    assert len({f["origin"] for f in drawn}) > 10, "the order did not move the walker"

    # Never ahead of the world, never behind the turn before.
    times = {f["turn"]: f["time"] for f in frames}
    for f in drawn:
        before = times.get(f["turn"] - 1, f["time"] - 400)
        assert before <= f["drawn"] <= f["time"], f

    # Inside one turn the walker is drawn at more than one place -- a turn
    # drawn at its end is one place a turn. Two or more in each of several
    # turns holds down to a few frames a turn, on a loaded machine.
    by_turn: dict[int, set] = {}
    rows: dict[int, set] = {}
    for f in drawn:
        by_turn.setdefault(f["turn"], set()).add(f["origin"])
        rows.setdefault(f["turn"], set()).add(f["row"])
    gliding = [turn for turn, places in by_turn.items() if len(places) >= 2]
    assert len(gliding) >= 3, {turn: sorted(places) for turn, places in by_turn.items()}
    # Its walk cycle steps between turns too: a 400 ms turn covers several of
    # its frames, and they are drawn, not stepped over.
    assert any(len(rows[turn]) >= 2 for turn in gliding), rows

    # The ring is under the body wherever the body is drawn.
    ringed = [f for f in drawn if f["rings"]]
    assert ringed, "the walker was selected and drew no ring"
    for f in ringed:
        assert f["rings"][-1] == f["origin"], f



def test_a_bird_flies_its_last_leg_to_the_end_in_the_turn_the_next_begins(app, game_dir):
    """A bird jumped to the end of its leg on the first frame of a turn and stood there.

    `EAGLE_MOVE.VS` resumes when a leg's animation ends and starts the next
    with `PlayAnim`. Scripts run at a turn's end here, so the world holds the
    new leg from that turn end on, its clock at 0, and the view draws up to a
    turn behind the world. The view carried on only an animation it had seen
    at the turn end before, so for the whole of that turn it drew the new leg
    at its start: the rest of the old leg was crossed in one frame and the
    bird stood at the junction until the turn ended. The original's visual
    runs an animation from its own start time (0x0053e6c0 records it,
    0x0062a0e0 draws the clock less it), so before the new leg began the bird
    is still flying the old one (`TurnGlide::anim`).

    Judged by game time, not by frames: between two frames the shadow moves
    no further than the eagle flies in the drawn time between them -- 0.3
    pixels a game millisecond and two for truncation, the bound
    `test_a_bird_flies_between_the_ends_of_its_animation` explains. Before the
    fix the first frame of such a turn crossed 11 to 18 pixels in 17 ms.
    A frame that spans more than one turn end is not judged: the view keeps
    one turn end before the world's, and a slow frame can step over a whole
    leg between them.
    """
    eagle, out = eagle_run(app, game_dir, [f"look:{EAGLE}", "wait:6"], 700)
    placed: list[tuple[int, int, tuple[int, int]]] = []
    frame = None
    for line in out.splitlines():
        if match := FRAME.match(line):
            frame = (int(match.group(2)), int(match.group(3)))
        elif frame is not None and (match := ORIGIN.match(line)):
            found, left, top, off_x, off_y, depth, x, y = map(int, match.groups())
            if found == eagle and depth == 800:
                placed.append((*frame, (x - off_x - left, y - off_y - top)))
    # The look moves the camera; a few frames on, the view stands still.
    placed = placed[10:]
    assert len(placed) > 200, len(placed)
    turn = min(b[0] - a[0] for a, b in zip(placed, placed[1:]) if b[0] > a[0])
    judged = [(a, b) for a, b in zip(placed, placed[1:]) if b[0] - a[0] <= turn]
    assert len(judged) > 100, len(judged)
    # Drawn times never run backwards.
    assert all(a[1] <= b[1] for a, b in judged), "drawn time ran backwards"
    fast = []
    for a, b in judged:
        step = max(abs(a[2][0] - b[2][0]), abs(a[2][1] - b[2][1]))
        if 10 * step > 3 * (b[1] - a[1]) + 20:
            fast.append((step, b[1] - a[1], a, b))
    assert not fast, fast[:5]
