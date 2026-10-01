"""What the app plays, through `IMPERIVM_SOUND_LOG`: never the audio itself.

`IMPERIVM_SOUND_LOG=1` prints one line per sound played,

    sound: <entity> <file> vol <millibels> pan <millibels>
    music: <file> vol <millibels>
    repan: <file> vol <millibels> pan <millibels>

the last when a live sound is moved with the view, quoted where a name has a
space, so a line splits as a shell's. Every run here is headless
(`conftest.py`), and a headless app opens SDL's dummy audio driver, which
consumes the mix and plays nothing (docs/engine/sound.md); `SDL_AUDIO_DRIVER`
is set to it besides.

**An order is acknowledged** by one of the selection (0x005e61db) speaking its
class's `command` sound (`Talk`, 0x005e36e0) on the `UnitOrder` type, whose
one channel drops a second voice while the first speaks (0x006b0450). Its
volume is the speech slider's on a 60 dB scale (0x006b0b22), and a unit on
screen is centred (0x006b0650). **A control's activation clicks**
(`Sounds/UI/click.wav`, 0x006b0d10) on the `UI` type, at the sound slider.

**Music.** The menus play `CONST.INI [GamePlay] PregameUIMusic` once each time
they are entered (0x00748808); a match draws a track of `music/` not starting
with `_`, never the last one, when the `Music` channel is idle on a two-second
look (0x00550f90). The installation's own `Settings.ini` may have music off
(the owner's has), so these runs give the app a user directory whose
`settings.ini` turns it on. **Selecting** a building the player controls
speaks its class's `select` sound (0x005e7d80, 0x005e2f80), and a **camera
move** re-applies level and pan to what is still playing (0x006b0810).
"""

from __future__ import annotations

import os
import re
import shlex
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

import corpus
from conftest import requires_game

pytestmark = requires_game

NUMANTIA = "Adventures/GreatBattles/2_Great_Battles_Numantia.bfhp"
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
    probe = subprocess.run(
        [str(path), "--game", str(game_dir), "--map", CROSSROADS, "--frames", "2", *SIZE],
        capture_output=True, text=True, timeout=120,
    )
    if probe.returncode != 0:
        pytest.skip("the app cannot start here, even headless: " + probe.stderr[-200:])
    return path


def run(app: Path, game_dir: Path, arguments: list[str], log: bool = True,
        user_dir: Path | None = None) -> str:
    """The app, headless and on the dummy audio driver, with the sound log."""
    env = {**os.environ, "SDL_AUDIO_DRIVER": "dummy"}
    env.pop("IMPERIVM_SOUND_LOG", None)
    if log:
        env["IMPERIVM_SOUND_LOG"] = "1"
    extra = ["--user-dir", str(user_dir)] if user_dir is not None else []
    done = subprocess.run(
        [str(app), "--game", str(game_dir), *SIZE, *extra, *arguments],
        capture_output=True, text=True, timeout=300, env=env,
    )
    assert done.returncode == 0, done.stdout + done.stderr
    return done.stdout


def play(app: Path, game_dir: Path, steps: list[str], where: str, frames: int = 60,
         log: bool = True, user_dir: Path | None = None) -> str:
    """A map, paused, with `steps` as the app's input."""
    if not (game_dir / where).is_file():
        pytest.skip(f"{where} is not in this installation")
    script = ";".join(["wait:10", "key:P", "wait:2", *steps])
    assert "F5" not in script and "F9" not in script, "never touch the quick save"
    return run(app, game_dir, ["--map", where, "--play", "--frames", str(frames), "--input", script],
               log=log, user_dir=user_dir)


def music_on(directory: Path, volume: int = 80, music: int = 1) -> Path:
    """A user directory whose `settings.ini` sets the music switch and slider.

    The engine reads the installation's `Settings.ini` and then its own over
    it, from the user directory, which is a temporary one here: nothing is
    written into the installation.
    """
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "settings.ini").write_text(f"[Options]\nMusic={music}\nMusicVolume={volume}\n")
    return directory


def music(out: str) -> list[tuple[str, int]]:
    """Every piece of music started: file, volume."""
    return [(m.group(1), int(m.group(2)))
            for m in re.finditer(r"^music: (\S+) vol (-?\d+)$", out, re.M)]


def repans(out: str) -> list[tuple[str, int, int]]:
    """Every live sound moved with the view: file, volume, pan."""
    found = []
    for line in out.splitlines():
        if line.startswith("repan: "):
            words = shlex.split(line[len("repan: "):])
            if len(words) == 5 and words[1] == "vol" and words[3] == "pan":
                found.append((words[0], int(words[2]), int(words[4])))
    return found


def sounds(out: str) -> list[tuple[str, str, int, int]]:
    """Every sound played: entity, file, volume, pan."""
    found = []
    for line in out.splitlines():
        if not line.startswith("sound: "):
            continue
        words = shlex.split(line[len("sound: "):])
        if len(words) == 6 and words[2] == "vol" and words[4] == "pan":
            found.append((words[0], words[1], int(words[3]), int(words[5])))
    return found


def setting(game_dir: Path, key: str, default: int) -> int:
    """An `[Options]` value of the installation's own `Settings.ini`."""
    try:
        text = (game_dir / "Settings.ini").read_text(errors="replace")
    except OSError:
        return default
    m = re.search(rf"^{key}\s*=\s*(\d+)", text, re.M)
    return int(m.group(1)) if m else default


def entity_files(game_dir: Path, stem: str) -> list[str]:
    """The files `DATA\\SOUND ENTITIES\\<stem>.XML` names."""
    from imperivm.formats.pak import PackFile

    pack = PackFile(game_dir / "Packs" / "data.pak")
    text = pack.read(f"DATA\\SOUND ENTITIES\\{stem.upper()}.XML").decode("cp1252")
    return [s.get("file") for s in ET.fromstring(text).iter("sound")]


def test_a_right_click_order_is_acknowledged_by_the_units_voice(app, game_dir, game_data):
    command = game_data.resolve_sounds("RHastatus").get("command", "")
    assert command, "RHastatus names no command sound"
    stem = re.split(r"[\\/]", command)[-1].rsplit(".", 1)[0]
    out = play(app, game_dir, ["select:class:RHastatus", "wait:2", "rclick:500,400", "wait:3"],
               NUMANTIA)
    assert re.search(r"^order: [1-9]\d* issued", out, re.M), out
    played = sounds(out)
    assert len(played) == 1, out
    entity, file, volume, pan = played[0]
    assert entity == stem, (entity, command)
    assert file in entity_files(game_dir, stem), file
    # On screen: no distance, centred; the speech slider over 60 dB.
    speech = setting(game_dir, "SpeechVolume", 52)
    assert volume == 6000 * speech // 100 - 6000, (volume, speech)
    assert pan == 0


def test_a_second_order_while_the_voice_speaks_is_not_acknowledged(app, game_dir):
    # One `UnitOrder` channel, which drops rather than steals: three orders a
    # few frames apart are acknowledged once.
    out = play(app, game_dir, ["select:class:RHastatus", "wait:2",
                               "rclick:500,400", "wait:3", "rclick:520,400", "wait:3",
                               "rclick:540,400", "wait:3"], NUMANTIA)
    assert len(re.findall(r"^order: [1-9]\d* issued", out, re.M)) == 3, out
    assert len(sounds(out)) == 1, out


def test_a_bar_button_clicks(app, game_dir):
    out = play(app, game_dir, ["select:class:BaseBarracks", "wait:2", "key:s", "wait:3"],
               CROSSROADS, frames=40)
    # Selecting the barracks speaks its `select` sound first (its own type,
    # `Select`); the button's click is the one `UI` sound.
    played = [p for p in sounds(out) if p[0] == "click"]
    assert [(e, f) for e, f, _, _ in played] == [("click", "Sounds/UI/click.wav")], out
    sound = setting(game_dir, "SoundVolume", 68)
    assert played[0][2] == 6000 * sound // 100 - 6000 and played[0][3] == 0, played


def test_without_the_log_nothing_is_said(app, game_dir):
    out = play(app, game_dir, ["select:class:RHastatus", "wait:2", "rclick:500,400", "wait:3"],
               NUMANTIA, log=False)
    assert re.search(r"^order: [1-9]\d* issued", out, re.M), out
    assert "sound: " not in out


# -- selection and the view -----------------------------------------------------


def stem_of(value: str) -> str:
    return re.split(r"[\\/]", value)[-1].rsplit(".", 1)[0]


def test_selecting_a_building_speaks_its_select_sound(app, game_dir, game_data):
    select = game_data.resolve_sounds("BaseBarracks").get("select", "")
    assert select, "BaseBarracks names no select sound"
    out = play(app, game_dir, ["select:class:BaseBarracks", "wait:3"], CROSSROADS, frames=30)
    played = sounds(out)
    assert len(played) == 1, out
    entity, file, volume, pan = played[0]
    assert entity == stem_of(select), (entity, select)
    assert file in entity_files(game_dir, entity), file
    # Placed at the building, by the view: at most the slider's level.
    sound = setting(game_dir, "SoundVolume", 68)
    assert -6000 <= volume <= 6000 * sound // 100 - 6000, volume
    assert -10000 <= pan <= 10000


def test_selecting_units_says_nothing(app, game_dir):
    # No unit class declares `select`; only buildings and decor do.
    out = play(app, game_dir, ["select:class:RHastatus", "wait:3"], NUMANTIA, frames=30)
    assert sounds(out) == [], out


def test_a_camera_move_moves_the_live_voice(app, game_dir):
    # The barracks' voice starts placed by the opening view; the view then
    # goes to the far side of the map, and the voice that is still playing
    # is re-placed: off screen to its left, and at the bottom of the scale.
    out = play(app, game_dir, ["select:class:BaseBarracks", "wait:2",
                               "look:class:BaseBarracks@1", "wait:3"], CROSSROADS, frames=40)
    played = sounds(out)
    assert len(played) == 1, out
    _, file, volume, pan = played[0]
    moved = [r for r in repans(out) if r[0] == file]
    assert moved, out
    assert moved[-1][2] != pan and moved[-1][2] < 0, (pan, moved)
    assert moved[-1][1] <= volume, (volume, moved)


# -- music ----------------------------------------------------------------------


def test_the_menus_play_their_music_once(app, game_dir, tmp_path):
    out = run(app, game_dir, ["--frames", "30"], user_dir=music_on(tmp_path / "user", 80))
    started = music(out)
    assert len(started) == 1, out
    file, volume = started[0]
    assert file.lower().replace("\\", "/") == "music/_menu.ogg", file
    assert volume == 6000 * 80 // 100 - 6000


def test_with_music_off_nothing_plays(app, game_dir, tmp_path):
    out = run(app, game_dir, ["--frames", "30"], user_dir=music_on(tmp_path / "user", 80, music=0))
    assert music(out) == [], out


def test_a_match_plays_a_track_of_its_own(app, game_dir, tmp_path):
    if not (game_dir / CROSSROADS).is_file():
        pytest.skip(f"{CROSSROADS} is not in this installation")
    # Unpaused: the first look is two seconds of game time in, about 20 turns.
    out = run(app, game_dir, ["--map", CROSSROADS, "--play", "--frames", "200"],
              user_dir=music_on(tmp_path / "user", 60))
    started = music(out)
    assert len(started) == 1, out
    file, volume = started[0]
    assert re.fullmatch(r"music/[^_/][^/]*", file), file
    assert (game_dir / file).is_file() or any(
        p.name.lower() == file.split("/")[-1].lower() for p in (game_dir / "music").iterdir()), file
    assert volume == 6000 * 60 // 100 - 6000


def test_every_track_decodes_a_block_at_a_time(game_dir):
    """The decoder over the installation's own music, as the mixer reads it.

    `sound_tests --decode` reads a file in place and prints what it decoded;
    every track is stereo 44,100 Hz, decodes to exactly the length its last
    page names, in many blocks rather than one, and is not silence.
    """
    candidates = [
        path
        for directory in corpus.tool_directories()
        if (path := directory / "engine" / "sound" / "sound_tests").is_file()
    ]
    if not candidates:
        pytest.skip("sound_tests is not built")
    tool = max(candidates, key=lambda p: p.stat().st_mtime)
    files = sorted((game_dir / "music").glob("*.ogg")) if (game_dir / "music").is_dir() else []
    if not files:
        pytest.skip("no music in this installation")
    for path in files:
        done = subprocess.run([str(tool), "--decode", str(path)], capture_output=True, text=True,
                              timeout=120)
        assert done.returncode == 0, (path.name, done.stdout)
        m = re.search(r"decoded: rate (\d+) channels (\d+) length (\d+) frames (\d+) reads (\d+) "
                      r"peak (\d+)", done.stdout)
        assert m, done.stdout
        rate, channels, length, frames, reads, peak = map(int, m.groups())
        assert (rate, channels) == (44100, 2), path.name
        assert frames == length and 80 * rate < frames < 340 * rate, (path.name, frames)
        assert reads > 100 and peak > 1000, (path.name, reads, peak)
