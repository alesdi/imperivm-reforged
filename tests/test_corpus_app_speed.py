"""The single-player game speed, in the app.

`Settings.ini`'s `GameSpeed` is a speed *position* (0x006e7ff0 sends
`700 + option * 2301 / 100` per mille), and a match starts at `NormalSpeed`
whatever it says (0x0052683b) and writes the position back from it
(0x006e71e0). So a match opens at 1000, and OK on the options screen -- the
slider untouched -- posts 999, which converts the next turn onwards: an 800 ms
turn becomes the 799 units of `tick.md`'s one off-speed dump.

Every app run here is headless (`conftest.py` sets `IMPERIVM_HEADLESS=1`): no
window, no sound device. Where the app cannot start even so, every test here
is skipped.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

import corpus
from conftest import requires_game

pytestmark = requires_game

ZAMA = "Adventures/GreatBattles/1_Great_Battles_Zama.bfhp"
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
    path = max(candidates, key=lambda p: p.stat().st_mtime)
    if not (game_dir / ZAMA).is_file():
        pytest.skip(f"{ZAMA} is not in this installation")
    probe = subprocess.run(
        [str(path), "--game", str(game_dir), "--map", ZAMA, "--frames", "2", *SIZE],
        capture_output=True, text=True, timeout=120,
    )
    if probe.returncode != 0:
        pytest.skip("the app cannot start here, even headless: " + probe.stderr[-200:])
    return path


def run(app: Path, game_dir: Path, *extra: str) -> str:
    done = subprocess.run(
        [str(app), "--game", str(game_dir), "--map", ZAMA, "--play", *SIZE, *extra],
        capture_output=True, text=True, timeout=180,
    )
    assert done.returncode == 0, done.stdout + done.stderr
    return done.stdout


def test_a_match_starts_at_normal_speed_and_converts_its_real_turn(app, game_dir):
    out = run(app, game_dir, "--frames", "3", "--turn-interval", "800")
    assert re.search(r"one turn every 800 ms, 800 game-time units at speed 1000\b", out), out
    # The default real turn is 100 ms, worth 100 units at 1000: real time.
    out = run(app, game_dir, "--frames", "3")
    assert re.search(r"one turn every 100 ms, 100 game-time units at speed 1000\b", out), out


def test_ok_on_the_options_screen_posts_the_shipped_position_as_999(app, game_dir):
    out = run(app, game_dir, "--frames", "200", "--turn-interval", "800", "--menu", "options",
              "--input", "wait:20;press:OkButton")
    said = re.findall(r"speed:\s+player (\d+) asked (\d+), (\d+) per mille from turn (\d+): "
                      r"(\d+) units a turn", out)
    # Exactly once, the local player's, applied with the first turn (the
    # open menu held the clock until OK) and converting the second on.
    assert said == [("0", "999", "999", "2", "799")], out


def test_no_speed_is_posted_without_the_options_screen(app, game_dir):
    out = run(app, game_dir, "--frames", "60")
    assert "speed:" not in out, out
