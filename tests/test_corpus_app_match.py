"""A skirmish plays in the app as it does in `imrun`.

The vertical slice's claim, played rather than simulated: Crossroads with
nothing ordered and the human seat idle, run by the app, goes to war, and it
is the *same* game `imrun` plays -- the same world hash on the same turn, from
two processes that share nothing but the engine and the installation.

It used to be held at the turn the match was decided, 2,369, when one p2 hero
took p0's town by loyalty. That ending came from sentries that never reached
their walls; with them posted, p0's town holds (`test_corpus_imrun.py`, and
the open thread on the AI's siege in `docs/plan.html`), so the two are held
side by side at a pinned turn instead.

That second half is what this module is for. For as long as nothing held the
two side by side they were two games: the headless entity resolver took each
image's grid from the XML while the app took it from the sheet's frame table,
an animation's length is its sheet's rows, and Crossroads parted at turn 4.
And the app ran its match at the options' difficulty where `imrun` runs the
match's own 0, which parted it at turn 0; `--difficulty 0` says which.

Costly: both run 2,200 turns, about a minute and a half side by side, and
a saved game resumed in both takes another half minute. `tools/verify.py
--full` runs it (the Python suite), the quick tier does not. Every app run here is headless (`conftest.py` sets `IMPERIVM_HEADLESS=1`).

The Statistics screen the end-game screen opens is held to the original's
rules by a cheap test beside it: `--declare-won` ends the match on its first
turns, so which players are rows can be asked in a second or two. The long
run used to ask the Statistics' Captured book of the town that changed hands;
nothing changes hands by the pinned turn now, so it does not.
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

CROSSROADS = "Scenarios/Crossroads.BFHP"
#: The turn the two are held at: just past `test_corpus_imrun.py`'s 2,200, and
#: on purpose not a multiple of eight -- a frame runs up to eight turns, so a
#: `turn:N` that did not hold the clock on N would read the world a few turns
#: late here, where on a multiple of eight it can land on N by luck.
TURNS = 2_205
#: A frame runs at most eight turns, however fast the machine, so 2,200 turns
#: take about 280 frames; this bounds a run whose script never finishes.
FRAMES = 2_000


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


def statistics(out: str) -> dict[str, dict[str, list[tuple[int, int]]]]:
    """The app's `statistics:` lines: tab -> player -> [(value, share)]."""
    tabs: dict[str, dict[str, list[tuple[int, int]]]] = {}
    for line in re.findall(r"^statistics: (.*)$", out, re.MULTILINE):
        tab, player, *cells = [part.strip() for part in line.split("|")]
        values = [re.fullmatch(r"(-?\d+) \((-?\d+)%\)", cell) for cell in cells]
        assert all(values), line
        tabs.setdefault(tab, {})[player] = [(int(m.group(1)), int(m.group(2))) for m in values]
    return tabs


#: Crossroads' four seats the players' screen gives out. It declares ten more
#: -- 4 to 13, `control="Computer"`, no holdings -- which the original never
#: makes *real*, so never lists (0x006dbdf3, 0x006fc8b0).
REAL = ["Player 1", "Player 2", "Player 3", "Player 4"]


def test_statistics_lists_the_real_players_in_place_of_the_end_menu(app, game_dir, tmp_path):
    stats, back = tmp_path / "stats.png", tmp_path / "back.png"
    # `WinLose` is the end menu's picture, a button whose id (0x1008) does
    # nothing: pressing it asks whether the end menu is open without acting.
    # Statistics takes the end menu's place, so the first press finds no
    # dialog that has it; Escape closes Statistics, F10 brings the end menu
    # back, and the second press finds it there, as does Quit.
    script = (f"over;wait:5;press:Statistics;wait:5;shot:{stats};press:WinLose;wait:2;"
              f"press:0.scores;wait:3;key:Escape;wait:2;key:F10;wait:3;shot:{back};"
              "press:WinLose;wait:2;press:Quit")
    played = subprocess.run(
        [str(app), "--game", str(game_dir), "--map", CROSSROADS, "--play", "--player", "0",
         "--difficulty", "0", "--turn-length", "800", "--turn-interval", "1", "--declare-won",
         "--width", "1024", "--height", "768", "--frames", "120", "--input", script],
        capture_output=True, text=True, timeout=300,
    )
    out = played.stdout + played.stderr
    assert played.returncode == 0, out
    assert "*** match over: you win ***" in out, out[-3000:]
    tabs = statistics(out)
    # A row per real player, in slot order, on every tab it was filled for.
    assert list(tabs) == ["Resources", "Scores"], tabs
    for tab in tabs.values():
        assert list(tab) == REAL, tab
    # Three columns on Scores; nobody has fought, so each rates 10 -- the
    # rating of nothing (0x0056a260), which is what the empty seats showed.
    assert all(len(cells) == 3 and cells[0][0] == 10 for cells in tabs["Scores"].values()), tabs
    # The end menu was not under Statistics, and was back after F10.
    assert re.findall(r"^input: .*$", out, re.MULTILINE) == ["input: no open dialog or bar has WinLose"], out[-3000:]
    assert stats.stat().st_size > 0 and back.stat().st_size > 0


@pytest.fixture(scope="module")
def imrun() -> Path:
    path, complaint = corpus.find_tool("imrun", "IMPERIVM_IMRUN")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


def test_crossroads_is_the_same_game_in_the_app_as_in_imrun(app, imrun, game_dir, tmp_path):
    headless = subprocess.Popen(
        [str(imrun), str(game_dir), str(game_dir / CROSSROADS), str(TURNS), "800"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    shot = tmp_path / "war.png"
    # Nothing is ordered. `turn:N` holds the clock on turn N, so the `hash`
    # after it is that turn's; then the app leaves by the menu's Quit and its
    # confirmation's Yes, which a press finds on the open dialog or says it
    # could not. (Quit alone opened the confirmation and left the app to run
    # out its frames, half a minute of an idle menu.)
    script = f"turn:{TURNS};hash;shot:{shot};key:F10;wait:3;press:Quit;wait:3;press:Yes"
    played = subprocess.run(
        [str(app), "--game", str(game_dir), "--map", CROSSROADS, "--play", "--player", "0",
         "--difficulty", "0", "--turn-length", "800", "--turn-interval", "1",
         "--width", "1024", "--height", "768", "--frames", str(FRAMES), "--input", script],
        capture_output=True, text=True, timeout=1200,
    )
    reference, _ = headless.communicate(timeout=1200)
    out = played.stdout + played.stderr
    assert played.returncode == 0, out
    assert headless.returncode == 0, reference

    # Not decided on either side.
    assert "*** match over" not in out, out[-3000:]
    assert re.search(r"^match\s+over=no", reference, re.MULTILINE), reference[-2000:]

    # The same game: the same world on the same turn.
    held = re.search(r"^hash:\s+at turn (\d+), hash ([0-9a-f]{16})$", out, re.MULTILINE)
    assert held, out[-3000:]
    turn, digest = int(held.group(1)), held.group(2)
    ran = int(re.search(r"^after (\d+) turns", reference, re.MULTILINE).group(1))
    reference_hash = re.search(r"^\s+hash\s+([0-9a-f]{16})$", reference, re.MULTILINE).group(1)
    assert (turn, digest) == (ran, reference_hash) == (TURNS, reference_hash), (
        out[-2000:], reference[-2000:])

    # And the app got there by itself and left by its menu before FRAMES.
    assert re.search(rf"screenshot: {re.escape(str(shot))} \(1024x768\) at turn \d+$", out,
                     re.MULTILINE), out[-3000:]
    assert "input:" not in out, out[-3000:]
    assert shot.stat().st_size > 0


#: A loaded game: the app saves Crossroads on `SAVED` with F5 and plays on to
#: `RESUMED`; then the save is loaded into `imrun` and into a second app, and
#: both play to `RESUMED`. Neither is a multiple of eight, for the reason
#: `TURNS` gives.
SAVED = 405
RESUMED = 645


def held_hash(out: str) -> tuple[int, str]:
    held = re.search(r"^hash:\s+at turn (\d+), hash ([0-9a-f]{16})$", out, re.MULTILINE)
    assert held, out[-3000:]
    return int(held.group(1)), held.group(2)


def test_a_loaded_game_is_the_same_game_in_the_app_as_in_imrun(app, imrun, game_dir, tmp_path):
    """A save is a game, and a loaded one is the same game wherever it is
    loaded: the app's F5 on Crossroads, resumed by `imrun --load` and by the
    app's `--load`, reaches the world the app that saved it reached by playing
    on, on the same turn.

    The plan once recorded that the two parted after a `--load`, at turn
    1,504. Measured again on the tree this was written on, they did not, from
    app saves taken on turns 308 to 1,508 (one with units selected), an
    `imsave` save, the conquest's seventh map and Balcans, over 300 to 4,000
    turns each, by both the command line's `--load` and F9; this holds the
    claim so that it cannot quietly stop being true. About 30 s.
    """
    saving = tmp_path / "saving"
    common = ["--difficulty", "0", "--turn-length", "800", "--turn-interval", "1",
              "--width", "1024", "--height", "768", "--frames", str(FRAMES)]
    played = subprocess.run(
        [str(app), "--game", str(game_dir), "--map", CROSSROADS, "--play", "--player", "0",
         "--user-dir", str(saving), *common, "--input",
         f"turn:{SAVED};key:F5;wait:2;turn:{RESUMED};hash;key:F10;wait:3;press:Quit;wait:3;press:Yes"],
        capture_output=True, text=True, timeout=1200,
    )
    out = played.stdout + played.stderr
    assert played.returncode == 0, out[-3000:]
    save = saving / "quicksave.bfhp"
    assert re.search(rf"^saved: .*quicksave\.bfhp \(turn {SAVED}, \d+ bytes\)$", out, re.MULTILINE), (
        out[-3000:])
    assert held_hash(out)[0] == RESUMED
    played_on = held_hash(out)

    headless = subprocess.Popen(
        [str(imrun), str(game_dir), "--load", str(save), str(RESUMED - SAVED), "800"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    resumed = subprocess.run(
        [str(app), "--game", str(game_dir), "--load", str(save), "--player", "0",
         "--user-dir", str(tmp_path / "resuming"), *common, "--input",
         f"turn:{RESUMED};hash;key:F10;wait:3;press:Quit;wait:3;press:Yes"],
        capture_output=True, text=True, timeout=1200,
    )
    reference, _ = headless.communicate(timeout=1200)
    again = resumed.stdout + resumed.stderr
    assert resumed.returncode == 0, again[-3000:]
    assert headless.returncode == 0, reference[-3000:]
    assert re.search(rf"^loaded: .*turn {SAVED}, hashes verified$", again, re.MULTILINE), again[-3000:]
    assert re.search(rf"^loaded .*turn {SAVED}, time \d+, hashes verified$", reference, re.MULTILINE), (
        reference[-3000:])

    ran = int(re.search(r"^after (\d+) turns", reference, re.MULTILINE).group(1))
    reference_hash = re.search(r"^\s+hash\s+([0-9a-f]{16})$", reference, re.MULTILINE).group(1)
    assert held_hash(again) == played_on == (ran, reference_hash), (
        again[-2000:], reference[-2000:])
