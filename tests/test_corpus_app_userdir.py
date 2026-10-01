"""Nothing a test or a tool launches writes into the player's installation.

A scripted headless run once sent `key:F5`, and the app quick-saved over the
owner's `Saves/quicksave.bfhp`. The app keeps everything it writes -- the
quick save, the save dialog's slots, `settings.ini`, `Profiles/`,
`<conquest>.campaign.ini`, the editor's maps -- under one user directory:
`--user-dir DIR`, else `$IMPERIVM_USER_DIR`, else the installation's `Saves/`
in a window, else, headless, a fresh temporary directory removed at exit
(`imperivm --help`, "User data"). `conftest.py` sets `IMPERIVM_USER_DIR` for
the whole session.

**These runs never see the real `Saves/`.** Each one points the app at a
shadow installation: a temporary directory whose every top-level entry links
to the real one's, except `Saves/`, which is a real directory of the shadow's
own holding two sentinel files. So even an app that ignored the user
directory -- the one before this fix, which is how the tests were shown to
fail -- writes into the shadow and not into the player's saves.
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
SIZE = ["--width", "1024", "--height", "768"]
# The options screen opened at start: flip one check, OK (which writes the
# settings), then F5 (which writes the quick save) and F9 (which reads it back).
FLIP_AND_SAVE = ("wait:10;press:TurnOffWaterCheck;wait:2;press:OkButton;wait:5;"
                 "key:F5;wait:5;key:F9;wait:5")


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


@pytest.fixture
def shadow(game_dir, tmp_path) -> Path:
    """An installation that reads the real one's data and has a `Saves/` of its own."""
    root = tmp_path / "install"
    root.mkdir()
    for entry in game_dir.iterdir():
        if entry.name.lower() == "saves":
            continue
        (root / entry.name).symlink_to(entry)
    saves = root / "Saves"
    (saves / "Profiles" / "someone").mkdir(parents=True)
    (saves / "quicksave.bfhp").write_bytes(b"sentinel: not a save\n")
    (saves / "settings.ini").write_bytes(b"; sentinel\n")
    (saves / "Profiles" / "someone" / "player.ini").write_bytes(b"; sentinel\n")
    return root


def listing(directory: Path) -> list[tuple[str, int, int, bytes]]:
    """Every file and directory under `directory`: name, size, mtime and bytes."""
    rows = []
    for path in sorted(directory.rglob("*")):
        stat = path.lstat()
        content = path.read_bytes() if path.is_file() else b""
        rows.append((path.relative_to(directory).as_posix(), stat.st_size, stat.st_mtime_ns, content))
    stat = directory.stat()
    rows.append((".", 0, stat.st_mtime_ns, b""))
    return rows


def launch(app: Path, root: Path, *extra: str, user_dir: Path | None,
           env_user_dir: Path | None = None) -> str:
    env = {key: value for key, value in os.environ.items() if key != "IMPERIVM_USER_DIR"}
    if env_user_dir is not None:
        env["IMPERIVM_USER_DIR"] = str(env_user_dir)
    command = [str(app), "--game", str(root), *SIZE, "--headless", *extra]
    if user_dir is not None:
        command += ["--user-dir", str(user_dir)]
    done = subprocess.run(command, capture_output=True, text=True, timeout=300, env=env)
    assert done.returncode == 0, done.stdout + done.stderr
    return done.stdout


def flip_and_save(app: Path, root: Path, **where) -> str:
    return launch(app, root, "--map", CROSSROADS, "--play", "--frames", "80", "--menu", "options",
                  "--input", FLIP_AND_SAVE, **where)


def no_water_animation(settings: Path) -> str:
    found = re.search(r"^NoWaterAnimation=(\d)", settings.read_text(), re.MULTILINE)
    assert found, settings.read_text()
    return found.group(1)


def test_f5_and_an_option_go_to_the_user_dir_and_not_the_installation(app, shadow, tmp_path):
    user = tmp_path / "user"
    before = listing(shadow / "Saves")
    out = flip_and_save(app, shadow, user_dir=None, env_user_dir=user)
    assert listing(shadow / "Saves") == before, out
    assert f"user data:    {user}" in out, out
    assert f"saved: {user / 'quicksave.bfhp'}" in out, out
    # F9 reads back the file F5 wrote, not the installation's sentinel.
    assert f"loaded: {user / 'quicksave.bfhp'}" in out, out
    assert (user / "quicksave.bfhp").stat().st_size > 1000
    assert (user / "settings.ini").is_file(), out


def test_the_settings_are_read_back_from_the_user_dir(app, shadow, tmp_path):
    # The first run flips a check from the installation's default; the
    # second only presses OK, and writes back what it read -- which is the
    # first run's flip only if it read the user directory's file.
    user = tmp_path / "user"
    flip_and_save(app, shadow, user_dir=None, env_user_dir=user)
    first = no_water_animation(user / "settings.ini")
    before = listing(shadow / "Saves")
    out = launch(app, shadow, "--map", CROSSROADS, "--play", "--frames", "40", "--menu", "options",
                 "--input", "wait:10;press:OkButton;wait:2", user_dir=None, env_user_dir=user)
    assert no_water_animation(user / "settings.ini") == first, out
    assert listing(shadow / "Saves") == before, out


def test_the_flag_wins_over_the_environment(app, shadow, tmp_path):
    flag, env = tmp_path / "flag", tmp_path / "env"
    before = listing(shadow / "Saves")
    out = flip_and_save(app, shadow, user_dir=flag, env_user_dir=env)
    assert listing(shadow / "Saves") == before, out
    assert (flag / "quicksave.bfhp").is_file() and (flag / "settings.ini").is_file(), out
    assert not (env / "quicksave.bfhp").exists(), out


def test_headless_with_no_user_dir_writes_to_a_temporary_one_it_removes(app, shadow):
    before = listing(shadow / "Saves")
    out = flip_and_save(app, shadow, user_dir=None, env_user_dir=None)
    assert listing(shadow / "Saves") == before, out
    said = re.search(r"^user data:\s+(\S.*?) \(headless with no --user-dir", out, re.MULTILINE)
    assert said, out
    temporary = Path(said.group(1))
    assert not temporary.is_relative_to(shadow), out
    assert f"saved: {temporary / 'quicksave.bfhp'}" in out, out
    assert f"loaded: {temporary / 'quicksave.bfhp'}" in out, out
    assert not temporary.exists(), f"{temporary} was left behind"
