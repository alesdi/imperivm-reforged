"""What the app draws over the world: the band, the selection rings, the health bars.

Playtest report #9: dragging a band drew no rectangle, and a selected unit
carried no marker, so the player could see neither what was being selected nor
what was. The selection itself worked. Now the band is drawn while the left
button is held, a selected unit stands on its ring (`UI\\SELECTIONS\\<n>.RLE`,
picked by the class's `selection_radius`, as `gbr.exe` picks it), and the
backtick key shows the in-world health bars in the original's three modes.

Asserted on the app's own counts rather than on pixels: after each screenshot
it prints `overlays: band B, rings R, bars H (mode M)`. Every run here is
headless (`conftest.py` sets `IMPERIVM_HEADLESS=1`).
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

import corpus
from conftest import requires_game

pytestmark = requires_game

#: Alesia opens on the player's cavalry between the tents, in view.
ALESIA = "Adventures/GreatBattles/3_Great_Battles_Alesia.bfhp"
SIZE = ["--width", "1024", "--height", "768"]
#: A band over the six horsemen by the left-hand tent in the opening view.
BAND = (290, 290, 620, 480)

OVERLAYS = re.compile(r"^overlays:\s+band (\d+), rings (\d+), bars (\d+) \(mode (\d+)\)$", re.MULTILINE)


@pytest.fixture(scope="module")
def app(game_dir) -> Path:
    candidates = [
        path
        for directory in corpus.tool_directories()
        if (path := directory / "engine" / "app" / "imperivm").is_file()
    ]
    if not candidates:
        pytest.skip("the app is not built")
    if not (game_dir / ALESIA).is_file():
        pytest.skip(f"{ALESIA} is not in this installation")
    return max(candidates, key=lambda p: p.stat().st_mtime)


def run(app: Path, game_dir: Path, tmp_path: Path, steps: list[str],
        extra: tuple[str, ...] = ()) -> tuple[str, list[tuple[int, ...]]]:
    """One paused run of Alesia's opening; the `overlays:` line of every shot."""
    script = ";".join(["key:P", "wait:3", *steps])
    done = subprocess.run(
        [str(app), "--game", str(game_dir), "--map", ALESIA, "--play", *SIZE, *extra,
         "--frames", str(20 + 4 * len(steps)), "--input", script],
        capture_output=True, text=True, timeout=300,
    )
    out = done.stdout + done.stderr
    assert done.returncode == 0, out[-2000:]
    return out, [tuple(map(int, m.groups())) for m in OVERLAYS.finditer(done.stdout)]


def test_the_band_is_drawn_while_held_and_the_selection_is_ringed(app, game_dir, tmp_path):
    x1, y1, x2, y2 = BAND
    out, shots = run(app, game_dir, tmp_path, [
        f"shot:{tmp_path / 'before.png'}", "wait:2",
        f"hold:{x1},{y1},{x2},{y2}", "wait:2", f"shot:{tmp_path / 'during.png'}", "wait:2",
        f"release:{x2},{y2}", "wait:2", f"shot:{tmp_path / 'after.png'}", "wait:2",
    ])
    assert len(shots) == 3, out[-2000:]
    before, during, after = shots
    # Nothing selected, no band: nothing over the world.
    assert before == (0, 0, 0, 0)
    # The button held: the band, and still no ring -- the selection changes on release.
    assert during[0] == 1 and during[1] == 0
    # Released: no band, and one ring for every unit the band took.
    taken = re.search(r"^selected (\d+) of \d+ under the cursor \(player \d+ holds (\d+)\)$",
                      out, re.MULTILINE)
    assert taken, out[-2000:]
    held = int(taken.group(2))
    assert held > 0
    assert after[0] == 0
    assert after[1] == held
    # No bars: the mode starts off, as the original's does.
    assert after[2] == 0 and after[3] == 0


def test_a_click_on_nothing_draws_no_band_and_clears_the_rings(app, game_dir, tmp_path):
    x1, y1, x2, y2 = BAND
    out, shots = run(app, game_dir, tmp_path, [
        f"drag:{x1},{y1},{x2},{y2}", "wait:2", f"shot:{tmp_path / 'ringed.png'}", "wait:2",
        # A press that does not move is a click, and a click on bare ground
        # selects nothing.
        "hold:150,560,151,561", "wait:2", f"shot:{tmp_path / 'click.png'}", "wait:2",
        "release:151,561", "wait:2", f"shot:{tmp_path / 'cleared.png'}", "wait:2",
    ])
    assert len(shots) == 3, out[-2000:]
    ringed, click, cleared = shots
    assert ringed[1] > 0
    assert click[0] == 0
    assert cleared[1] == 0


#: One of the units of a player at war with Alesia's player 0, a map's width
#: from its cavalry: no view holds both. Fog hides it, so the run that looks
#: at it is `--no-fog`, which draws the units and changes no relation.
HOSTILE = 1262


def test_the_backtick_cycles_the_health_bars_as_the_original_does(app, game_dir, tmp_path):
    """The partition is checked in two views, each shot paused on one turn.

    **The opening view holds no enemy.** It once seemed to: the run paused on
    turn 0, before the mission's scripts make player 1 player 0's ally, and
    player 1's units counted as "the rest". Under a loaded machine the first
    frames ran a turn or two before the `P` landed, the alliance was in
    place, mode 3 drew nothing, and `--full` failed. So the rest are found
    where they stand, at a war's distance, and the opening view is asked only
    for what holds on every turn."""
    out, shots = run(app, game_dir, tmp_path, [
        "key:`", "wait:2", f"shot:{tmp_path / 'all.png'}", "wait:2",
        "key:ctrl+`", "wait:2", f"shot:{tmp_path / 'others.png'}", "wait:2",
        "key:ctrl+`", "wait:2", f"shot:{tmp_path / 'own.png'}", "wait:2",
        "key:ctrl+`", "wait:2", f"shot:{tmp_path / 'all_again.png'}", "wait:2",
        "key:`", "wait:2", f"shot:{tmp_path / 'off.png'}", "wait:2",
        "key:`", "wait:2", f"shot:{tmp_path / 'back.png'}", "wait:2",
        f"look:{HOSTILE}", "wait:3", f"shot:{tmp_path / 'enemy_all.png'}", "wait:2",
        "key:ctrl+`", "wait:2", f"shot:{tmp_path / 'enemy_others.png'}", "wait:2",
        "key:ctrl+`", "wait:2", f"shot:{tmp_path / 'enemy_own.png'}", "wait:2",
    ], extra=("--no-fog",))
    assert len(shots) == 9, out[-2000:]
    modes = [shot[3] for shot in shots]
    # Off at the start; the backtick turns on the mode last left, which ships
    # as 1 (everyone); Ctrl steps 1 -> 3 -> 2 -> 1; the backtick turns it off
    # and back on at the mode it left.
    assert modes == [1, 3, 2, 1, 0, 1, 1, 3, 2]
    everyone, others, friends, everyone_again, off, back = (shot[2] for shot in shots[:6])
    # Mode 2 is what the local player holds ceasefire with, itself included,
    # and mode 3 the rest: between them, everyone, once -- in each view.
    assert everyone > 0 and friends > 0
    assert others + friends == everyone
    assert everyone_again == everyone == back
    assert off == 0
    enemy_all, enemy_others, enemy_own = (shot[2] for shot in shots[6:])
    assert enemy_all > 0 and enemy_others > 0
    assert enemy_others + enemy_own == enemy_all
