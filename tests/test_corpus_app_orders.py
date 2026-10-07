"""The keys an order is given with, through the app's own input.

**Shift and Ctrl are two flags** (playtest #14's follow-ups). The original's
command bar reads both when it posts an order (0x005e39f0): Shift into the
replace flag, as `!Shift` for any row but a `traincommand` one, and Ctrl into
the order's `bModifier`. A right click reads the same two the same way
(0x005e6050). The app passed Shift to the bar as both, so a Ctrl press was
lost and a Shift press told a dispatch script it had Ctrl.

**Ctrl on a train button** queues `[GamePlay] TrainMultipleCount` (5) of the
unit: the bar swaps the order for one with a count (0x005e3680) whose
execution runs the row that many times (0x004e5e60).

**A click on a queued unit cancels it.** The info bar's `BuildingQueue` posts
a `CVXCmdCancelCmd` -- the building and the entry's command id -- through the
local command path (0x006bf7c0); every peer takes that command out and puts
its gold back (0x004e63c0, 0x005b07d0). With Ctrl, the entry and every one
behind it go (0x006bfebc). The click is the release over the cell it went
down on (0x006ee27a); `press:Queue@1>X,Y` releases elsewhere.

Every app run here is headless (`conftest.py` sets `IMPERIVM_HEADLESS=1`). No
step presses F5 or F9: those write and read the installation's
`Saves/quicksave.bfhp`. `mod:` holds modifier keys for the mouse steps after
it.
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

NUMANTIA = "Adventures/GreatBattles/2_Great_Battles_Numantia.bfhp"
# The player's own barracks (a British `Caserma`) stands at the start here.
CROSSROADS = "Scenarios/Crossroads.BFHP"
SIZE = ["--width", "1024", "--height", "768"]


@pytest.fixture(scope="module")
def app(game_dir) -> Path:
    candidates = [
        path
        for directory in corpus.tool_directories()
        if (path := directory / "engine" / "app" / "imperivm").is_file()
    ]
    if not candidates:
        pytest.skip("the app is not built")
    path = max(candidates, key=lambda p: p.stat().st_mtime)
    if not (game_dir / NUMANTIA).is_file():
        pytest.skip(f"{NUMANTIA} is not in this installation")
    probe = subprocess.run(
        [str(path), "--game", str(game_dir), "--map", NUMANTIA, "--frames", "2", *SIZE],
        capture_output=True, text=True, timeout=120,
    )
    if probe.returncode != 0:
        pytest.skip("the app cannot start here, even headless: " + probe.stderr[-200:])
    return path


def play(app: Path, game_dir: Path, steps: list[str], frames: int = 90, where: str = NUMANTIA) -> str:
    """A map, paused, with `steps` as the app's input."""
    if not (game_dir / where).is_file():
        pytest.skip(f"{where} is not in this installation")
    script = ";".join(["wait:10", "key:P", "wait:2", *steps])
    assert "F5" not in script and "F9" not in script, "never touch the quick save"
    done = subprocess.run(
        [str(app), "--game", str(game_dir), *SIZE, "--map", where, "--play", "--frames",
         str(frames), "--input", script],
        capture_output=True, text=True, timeout=300,
        env={**os.environ, "IMPERIVM_INFOBAR": "1"},
    )
    assert done.returncode == 0, done.stdout + done.stderr
    return done.stdout


def queues(out: str) -> list[list[str]]:
    """Every `queue:` line the app printed after an order, its entries in
    queue order, the running command first."""
    found = []
    for line in out.splitlines():
        if m := re.match(r"queue:\s+object \d+ holds (\d+)(.*)$", line):
            entries = m.group(2).split(" | ")[1:]
            assert int(m.group(1)) == len(entries), line
            found.append(entries)
    return found


def test_a_shift_move_appends_and_a_plain_or_ctrl_one_replaces(app, game_dir):
    # The move row's key, then a click on the ground: the bar's aim, whose
    # flags are read at the click (0x005e720e).
    move = lambda x: ["key:m", "wait:2", f"click:{x},380", "wait:3"]
    out = play(app, game_dir, ["select:class:Unit", "wait:2",
                               *move(500), *move(520),
                               "mod:shift", "wait:1", *move(540),
                               "mod:ctrl", "wait:1", *move(560),
                               "mod:", "wait:1"])
    assert re.search(r"^selected:\s+1 object\(s\) from select:class:Unit$", out, re.M), out
    assert queues(out) == [["move"], ["move"], ["move", "move"], ["move"]], out


def test_a_shift_right_click_appends_and_a_plain_one_replaces(app, game_dir):
    out = play(app, game_dir, ["select:class:Unit", "wait:2",
                               "rclick:500,400", "wait:3",
                               "mod:shift", "wait:1", "rclick:520,400", "wait:3",
                               "mod:", "wait:1", "rclick:540,400", "wait:3"])
    held = queues(out)
    assert len(held) == 3, out
    assert len(held[0]) == 1 and len(held[1]) == 2 and len(held[2]) == 1, held
    assert held[1][0] == held[0][0], held


def gold(out: str) -> list[int]:
    """The barracks' `v2` slot after each refresh: its settlement's gold."""
    return [int(g) for g in re.findall(r'^infobar: class \S+ .* v2="(\d+)"', out, re.M)]


def test_ctrl_on_a_train_button_queues_five(app, game_dir):
    # `s` is the Caserma's swordsman (50 gold). A plain press queues one, a
    # Ctrl press five more behind it, and Shift with Ctrl is still five.
    out = play(app, game_dir, ["select:class:BaseBarracks", "wait:2",
                               "key:s", "wait:3", "key:ctrl+s", "wait:3", "key:ctrl+shift+s", "wait:3"],
               frames=60, where=CROSSROADS)
    held = queues(out)
    assert [len(q) for q in held] == [1, 6, 11], held
    assert all(set(q) == {"trainBSwordsman"} for q in held), held
    # Each of the eleven was paid for.
    purse = gold(out)
    assert purse[-1] == purse[0] - 11 * 50, purse


def test_a_queue_cell_cancels_its_entry_and_refunds_it(app, game_dir):
    # A click on a queued unit in the building's `BuildingQueue` posts a
    # `CVXCmdCancelCmd` for that entry's command id (0x006bf7c0); every peer
    # takes it out and puts its cost back (0x004e63c0, 0x005b07d0). The game
    # is paused, so the order waits for the turn `.` steps.
    out = play(app, game_dir, ["select:class:BaseBarracks", "wait:2",
                               "key:s", "wait:2", "key:a", "wait:2", "key:s", "wait:3",
                               "press:Queue@1", "wait:3", "key:.", "wait:4"],
               frames=60, where=CROSSROADS)
    held = queues(out)
    assert held[-2] == ["trainBSwordsman", "trainBBowman", "trainBSwordsman"], held
    assert held[-1] == ["trainBSwordsman", "trainBSwordsman"], held
    assert re.search(r"^queue:\s+cancel of command \d+ \(cell 1\) on object \d+ posted$", out, re.M), out
    assert re.search(r"^cancel:\s+1 queued command\(s\) taken out", out, re.M), out
    # 50 + 60 + 50 paid, the bowman's 60 back.
    purse = gold(out)
    assert purse[-1] == purse[0] - 100, purse


def test_a_ctrl_click_on_a_queue_cell_cancels_it_and_everything_behind(app, game_dir):
    # 0x006bfebc: with Ctrl, one cancel per entry from the last back to the
    # clicked one. The entry in front of it trains on.
    out = play(app, game_dir, ["select:class:BaseBarracks", "wait:2",
                               "key:s", "wait:2", "key:a", "wait:2", "key:s", "wait:3",
                               "mod:ctrl", "wait:1", "press:Queue@1", "wait:3", "mod:", "wait:1",
                               "key:.", "wait:4"],
               frames=60, where=CROSSROADS)
    posted = re.findall(r"^queue:\s+cancel of command \d+ \(cell (\d)\) on object \d+ posted$", out, re.M)
    assert posted == ["2", "1"], out
    assert queues(out)[-1] == ["trainBSwordsman"], out
    purse = gold(out)
    assert purse[-1] == purse[0] - 50, purse


def test_a_press_on_a_queue_cell_released_off_it_cancels_nothing(app, game_dir):
    # The strip acts on the release, over the cell the button went down on
    # (0x006ee27a); down on a cell and up on the map is no click.
    out = play(app, game_dir, ["select:class:BaseBarracks", "wait:2",
                               "key:s", "wait:2", "key:a", "wait:3",
                               "press:Queue@1>500,380", "wait:3", "key:.", "wait:4"],
               frames=50, where=CROSSROADS)
    assert "cancel of command" not in out, out
    assert queues(out)[-1] == ["trainBSwordsman", "trainBBowman"], out


#: The world view's debug output (`IMPERIVM_DEBUG_VIEW=1`): the line ahead of
#: each frame's placements, and a body's layer.
FRAME = re.compile(r"^frame turn (\d+) time \d+ drawn \d+$")
BODY = re.compile(r"^id (\d+) sheet .* lt (-?\d+),(-?\d+) off (-?\d+),(-?\d+) layer \d+ z 1000 "
                  r"at (-?\d+),(-?\d+)$")


def test_an_order_given_as_the_match_opens_is_walked(app, game_dir):
    """A right click on the first frame the input can give one is walked.

    `gbr.exe` starts a match's clock on a one-millisecond real window with no
    game time in it (0x00526637 calling 0x00528d10), so the match's first
    frame opens the first game window and runs the command pump on it before
    a millisecond has run (0x0051ea9e, 0x0051eab3). A player's order waits
    for the pump at the next window, after the scripts started with the match
    have had their first slice. The app let the world stand at turn 0 for a
    whole turn interval and issued a click in it at once, ahead of that
    slice: on Numantia the opening sequence then gave the legion
    `SetCommand(..., "attach", hero)`, which replaced the move, and the
    legionary never walked. The first turn now runs as the match opens.

    The turn is two real seconds, so that the click lands well inside what
    was turn 0 even on a loaded machine. The view does not move, so another
    place on the screen is a step in the world.
    """
    done = subprocess.run(
        [str(app), "--game", str(game_dir), *SIZE, "--map", NUMANTIA, "--play", "--no-fog",
         "--frames", "420", "--turn-interval", "2000",
         "--input", "select:class:RHastatus;rclick:700,500"],
        capture_output=True, text=True, timeout=300,
        env={**os.environ, "IMPERIVM_DEBUG_VIEW": "1"},
    )
    assert done.returncode == 0, (done.stdout + done.stderr)[-2000:]
    out = done.stdout
    ordered = re.search(r"^queue:\s+object (\d+) holds 1 \| move$", out, re.M)
    assert ordered, out[-2000:]
    walker = int(ordered.group(1))

    turn = None
    order_turn = None
    places: set[tuple[int, int]] = set()
    for line in out.splitlines():
        if match := FRAME.match(line):
            turn = int(match.group(1))
        elif line.startswith("order:") and order_turn is None:
            order_turn = turn
        elif (match := BODY.match(line)) and int(match.group(1)) == walker:
            left, top, off_x, off_y, x, y = map(int, match.groups()[1:])
            places.add((x - off_x - left, y - off_y - top))
    # The first turn had run when the click came: there is no turn 0 left
    # to give an order in.
    assert order_turn is not None and order_turn >= 1, order_turn
    assert turn is not None and turn >= 3, turn
    assert len(places) > 10, f"the legionary did not walk: {sorted(places)}"
