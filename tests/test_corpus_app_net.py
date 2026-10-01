"""Two app windows, one networked match.

`test_corpus_udp.py` proves the protocol between two `imconform` processes.
This proves the *app's* half: the lobby before the window, the match options
every peer builds alike, the right click becoming an order that runs on the
agreed turn, and the clock running agreed turns at their agreed length --
with each window playing a different seat.

Each window selects its own seat's objects (`--select mine:N`), right-clicks
on a script (`--input "wait:..;rclick:X,Y"`), plays `--net-turns` turns and
prints the same `hashes` line `imconform udp-*` prints. The test compares the
two windows with each other, never with an earlier run: turn lengths come from
measured round trips, so two matches legitimately differ.

A "window" here is one app process, and none opens a real one: `conftest.py`
sets `IMPERIVM_HEADLESS=1`, so every app a test launches runs headless -- no
window, no sound device, no focus taken -- drawing its frames off screen. If
the app cannot start even so, the probe below fails and every test here is
skipped, rather than passing having run nothing.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

import corpus
from conftest import requires_game

# Each test is its own subprocess run and the module's fixtures only find a
# binary, so under `--dist loadgroup` its tests may go to different workers.
pytestmark = [requires_game, pytest.mark.spread]

ZAMA = "Adventures/GreatBattles/1_Great_Battles_Zama.bfhp"
NUMANTIA = "Adventures/GreatBattles/2_Great_Battles_Numantia.bfhp"
CROSSROADS = "Scenarios/Crossroads.BFHP"
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
    probe = subprocess.run(
        [str(path), "--game", str(game_dir), "--map", ZAMA, "--frames", "2", *SIZE],
        capture_output=True, text=True, timeout=120,
    )
    if probe.returncode != 0:
        pytest.skip("the app cannot start here, even headless: " + probe.stderr[-200:])
    return path


def play(app: Path, game_dir: Path, relative: str, *, port: int, seats: str | None = None,
         drop: int = 0, turns: int = 60, host_extra: tuple = (), join_extra: tuple = ()):
    if not (game_dir / relative).is_file():
        pytest.skip(f"{relative} is not in this installation")
    common = ["--game", str(game_dir), "--map", relative, "--net-turns", str(turns),
              "--net-drop", str(drop), "--net-timeout", "60000", "--select", "mine:30", *SIZE]
    host_args = [str(app), *common, "--host", str(port), "--player", "0",
                 "--input", "wait:40;rclick:300,250;wait:60;rclick:500,400"]
    if seats:
        host_args += ["--seats", seats]
    host_args += list(host_extra)
    host = subprocess.Popen(host_args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    try:
        # The host prints its port before it waits; reading to that line is
        # what orders the two starts without a sleep.
        lines = []
        for line in host.stdout:
            lines.append(line)
            if "hosting on port" in line:
                break
        else:
            pytest.fail("the host never listened:\n" + "".join(lines))
        join = subprocess.run(
            [str(app), *common, "--join", f"127.0.0.1:{port}", "--input", "wait:50;rclick:350,300",
             *join_extra],
            capture_output=True, text=True, timeout=180,
        )
        rest, _ = host.communicate(timeout=180)
    finally:
        if host.poll() is None:
            host.kill()
            host.wait()
    return (host.returncode, "".join(lines) + rest), (join.returncode, join.stdout + join.stderr)


def fields(text: str) -> dict:
    out: dict = {}
    if m := re.search(r"hashes\s+world ([0-9a-f]+) netcmds ([0-9a-f]+)", text):
        out["world"], out["netcmds"] = m[1], m[2]
    if m := re.search(r"net\s+(\d+) turns as player (\d+)", text):
        out["turns"], out["player"] = int(m[1]), int(m[2])
    if m := re.search(r"wire\s+(\d+) sent, (\d+) dropped, (\d+) received, (\d+) duplicates, "
                      r"(\d+) refused", text):
        out["dropped"], out["refused"] = int(m[2]), int(m[5])
    out["turn_lines"] = re.findall(r"net:\s+turn (\d+): (\d+) order\(s\), (\d+) command", text)
    return out


def test_two_windows_play_one_game_and_their_clicks_are_orders(app, game_dir):
    (host_rc, host_out), (join_rc, join_out) = play(app, game_dir, ZAMA, port=47851)
    assert host_rc == 0, host_out
    assert join_rc == 0, join_out
    host, join = fields(host_out), fields(join_out)
    assert host["turns"] == join["turns"] == 60
    assert host["player"] != join["player"]
    # The clicks ran as orders, on the same turns, with the same effect.
    assert host["turn_lines"], host_out
    assert host["turn_lines"] == join["turn_lines"]
    assert sum(int(queued) for _, _, queued in host["turn_lines"]) > 0
    assert host["world"] == join["world"], host_out + join_out
    assert host["netcmds"] == join["netcmds"]


def test_two_windows_agree_through_loss_on_another_seat(app, game_dir):
    (host_rc, host_out), (join_rc, join_out) = play(
        app, game_dir, NUMANTIA, port=47852, seats="3", drop=300)
    assert host_rc == 0, host_out
    assert join_rc == 0, join_out
    host, join = fields(host_out), fields(join_out)
    assert join["player"] == 3
    assert host["dropped"] > 0 and join["dropped"] > 0
    assert host["refused"] == join["refused"] == 0
    assert host["turn_lines"] == join["turn_lines"]
    assert host["world"] == join["world"], host_out + join_out


def test_a_speed_change_from_either_window_is_agreed_on_one_turn(app, game_dir):
    """The joiner asks for 2000 after 12 turns, the host for 700 after 30:
    both windows apply each on the same turn and stay one game."""
    (host_rc, host_out), (join_rc, join_out) = play(
        app, game_dir, ZAMA, port=47853, turns=50,
        host_extra=("--net-speed", "30:700"), join_extra=("--net-speed", "12:2000"))
    assert host_rc == 0, host_out
    assert join_rc == 0, join_out
    said = re.findall(r"net\s+player (\d+) set the speed to (\d+)% on turn (\d+)", host_out)
    assert said == re.findall(r"net\s+player (\d+) set the speed to (\d+)% on turn (\d+)",
                              join_out), host_out + join_out
    assert [pct for _, pct, _ in said] == ["200", "70"], host_out
    host, join = fields(host_out), fields(join_out)
    assert host["world"] == join["world"], host_out + join_out
    assert host["netcmds"] == join["netcmds"]


def test_a_diplomacy_change_in_one_window_is_agreed_by_both(app, game_dir):
    """With nothing selected the host opens Diplomacy from the bar, grants the
    first row -- the joiner's seat -- cease fire and shared vision and presses
    OK: the change is a `diplomacy` order, and both windows apply it on the
    same turn and stay one game (the relations are in the world hash). A
    skirmish, because a mission's other seats are the map's Computer seats,
    which are never real, and its Diplomacy has no row."""
    host_dipl = "wait:60;press:Diplomacy;wait:5;press:Pl1.CF1;wait:2;press:Pl1.SV1;wait:2;press:OK;wait:10"
    (host_rc, host_out), (join_rc, join_out) = play(
        app, game_dir, CROSSROADS, port=47857, turns=60,
        host_extra=("--select", "", "--input", host_dipl))
    assert host_rc == 0, host_out
    assert join_rc == 0, join_out
    host, join = fields(host_out), fields(join_out)
    pattern = r"net\s+player (\d+)'s word for player (\d+) is (0x[0-9a-f]+) from turn (\d+)"
    said = re.findall(pattern, host_out)
    assert said and said == re.findall(pattern, join_out), host_out + join_out
    assert [(issuer, other, word) for issuer, other, word, _ in said] == [
        (str(host["player"]), str(join["player"]), "0x11")], host_out
    assert host["world"] == join["world"], host_out + join_out
    assert host["netcmds"] == join["netcmds"]


def test_chat_said_in_one_window_is_shown_in_the_other_and_is_not_the_game(app, game_dir):
    """Enter opens INGAMECHAT.INI, the line is typed, Enter sends: the other
    window hears it, once, from the right seat. The line never reaches the
    command stream: both windows still agree on every hash, and the joiner's
    own chat, sent to one player only, is heard by that player too."""
    host_chat = "wait:70;key:Return;text:hold the ford;key:Return;wait:10"
    join_chat = ("wait:90;key:Return;press:FoesRadio;text:on my way;press:SendBtn;wait:10")
    (host_rc, host_out), (join_rc, join_out) = play(
        app, game_dir, ZAMA, port=47854, turns=60,
        host_extra=("--input", host_chat), join_extra=("--input", join_chat))
    assert host_rc == 0, host_out
    assert join_rc == 0, join_out
    host, join = fields(host_out), fields(join_out)
    assert re.findall(r"chat\s+player (\d+) to (\w+): (.*)", join_out) == [
        (str(host["player"]), "all", "hold the ford")], join_out
    assert re.findall(r"chat\s+player (\d+) to (\w+): (.*)", host_out) == [
        (str(join["player"]), "player", "on my way")], host_out
    assert host["world"] == join["world"], host_out + join_out
    assert host["netcmds"] == join["netcmds"]


def test_the_command_bars_chat_button_opens_the_chat_as_enter_does(app, game_dir):
    """The no-selection bar's `Chat (Enter)` (ID 0x1006 in every EMPTY_*.INI)
    is a button as well as a key, and the button did nothing. With nothing
    selected the joiner clicks it -- at 640 x 480 it is the red mouth, second
    from the right -- types, and presses Send; the host hears the line."""
    join_chat = "wait:90;click:538,452;wait:5;text:by the button;press:SendBtn;wait:10"
    (host_rc, host_out), (join_rc, join_out) = play(
        app, game_dir, ZAMA, port=47856, turns=60,
        join_extra=("--select", "", "--input", join_chat))
    assert host_rc == 0, host_out
    assert join_rc == 0, join_out
    join = fields(join_out)
    assert re.findall(r"chat\s+player (\d+) to (\w+): (.*)", host_out) == [
        (str(join["player"]), "all", "by the button")], host_out + join_out


# --------------------------------------------------------------------------
# the shipped screens: MPMENU.INI, the players' screen, MULTI.INI
# --------------------------------------------------------------------------

HOST_SCREENS = ("wait:15;press:MultiPlayer;wait:20;press:Host;wait:20;press:List@0"
                + ";wait:150;press:Start" * 12)
JOIN_SCREENS = ("wait:15;press:MultiPlayer;wait:20;press:InetHostCombo;text:127.0.0.1;"
                "press:Join;wait:200;press:ImReadyBig")


#: The shipped screens host on the default port (47800) and the joiner types
#: the bare address, so two of these at once would join each other's lobby.
#: One group, so that under `--dist loadgroup` they run on one worker in turn.
DEFAULT_PORT = pytest.mark.xdist_group("app-net-default-port")


def through_screens(app: Path, game_dir: Path, host_turns: int, join_turns: int,
                    host_screens: str = HOST_SCREENS, join_screens: str = JOIN_SCREENS):
    """Host and join the way a player does: the main menu's Multiplayer, Host
    and a scenario on one side; the typed address, Join and I'm ready on the
    other; then the host's Start."""
    common = ["--game", str(game_dir), "--net-timeout", "60000", *SIZE]
    host = subprocess.Popen([str(app), *common, "--net-turns", str(host_turns),
                             "--input", host_screens],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        lines = []
        for line in host.stdout:
            lines.append(line)
            if "hosting" in line:
                break
        else:
            pytest.fail("the host never opened its lobby:\n" + "".join(lines))
        join = subprocess.run([str(app), *common, "--net-turns", str(join_turns),
                               "--input", join_screens],
                              capture_output=True, text=True, timeout=240)
        rest, _ = host.communicate(timeout=240)
    finally:
        if host.poll() is None:
            host.kill()
            host.wait()
    return "".join(lines) + rest, join.stdout + join.stderr


@DEFAULT_PORT
def test_a_game_hosted_and_joined_through_the_shipped_screens(app, game_dir):
    host_out, join_out = through_screens(app, game_dir, 40, 40)
    host, join = fields(host_out), fields(join_out)
    assert host.get("turns") == join.get("turns") == 40, host_out + join_out
    assert host["player"] != join["player"]
    assert host["world"] == join["world"], host_out + join_out
    # The joiner played the host's map, which it was told rather than given.
    host_map = re.search(r"playing (\S+) as player", host_out)
    join_map = re.search(r"playing (\S+) as player", join_out)
    assert host_map and join_map and host_map[1] == join_map[1]


def computer_marker(game_dir: Path) -> str:
    """What the installation's language pack makes of `(AI)`: the marker the
    original appends to a seat it hands to the computer (0x00406840, the
    table's own comment "Added to names of AI players"). The language is the
    one the app picks: `Settings.ini`'s `[Language] Default=`, else English;
    a pack without the entry leaves the key, as a table's miss does."""
    from imperivm.formats.pak import PackFile

    language = "English"
    settings = game_dir / "Settings.ini"
    if settings.is_file():
        if m := re.search(r"\[Language\][^\[]*?^\s*Default\s*=\s*(\S+)",
                          settings.read_text(errors="replace"), re.M):
            language = m[1]
    packs = [p for p in (game_dir / "local").glob("*.pak") if p.stem.lower() == language.lower()]
    if not packs:
        return "(AI)"
    pack = PackFile(packs[0])
    table = next((e.name for e in pack.entries if e.name.upper().endswith("TRANSLATION.LOC.XML")),
                 None)
    if table is None:
        return "(AI)"
    text = pack.read(table).decode("utf-8", errors="replace")
    m = re.search(r'<translationtableentry text="\(AI\)"[^>]*?result="([^"]*)"', text)
    return m[1] if m else "(AI)"


def takeovers(text: str) -> list[tuple[int, int, str]]:
    """Every `(seat, turn, shown as)` the computer took, as a window said it."""
    return [(int(s), int(t), shown) for s, t, shown in
            re.findall(r'net\s+player (\d+) is the computer\'s from turn (\d+), shown as "(.*)"', text)]


def hand_backs(text: str) -> list[tuple[int, int, str]]:
    return [(int(s), int(t), shown) for s, t, shown in
            re.findall(r'net\s+player (\d+) is played again from turn (\d+), shown as "(.*)"', text)]


DROPPED_SOUND = "sound     Sounds/UI/PlayerDropped.wav"


@DEFAULT_PORT
def test_a_peer_that_leaves_is_dropped_and_the_other_plays_on(app, game_dir):
    """The joiner stops at 30 turns and leaves. The host no longer ends the
    match: it drops the joiner from an agreed turn -- past the 30 it ran --
    says so, and plays its own 60 to the end, the departed seat idle.

    On the turn the computer takes the seat, the host shows the seat's name
    with the language pack's `(AI)` appended and plays `PlayerDropped.wav`,
    once. The joiner, gone, shows neither."""
    host_out, join_out = through_screens(app, game_dir, 60, 30)
    assert re.search(r"net\s+30 turns as player", join_out), join_out
    m = re.search(r"net\s+player (\d+) left \(a player left\); dropped from turn (\d+)", host_out)
    assert m, host_out
    assert int(m[2]) >= 30
    assert re.search(r"net\s+60 turns as player", host_out), host_out
    assert "the match ended" not in host_out
    marker = computer_marker(game_dir)
    taken = takeovers(host_out)
    assert len(taken) == 1, host_out
    seat, turn, shown = taken[0]
    assert seat == int(m[1]) and turn == int(m[2]), host_out
    assert shown.endswith(marker) and len(shown) > len(marker), (marker, host_out)
    assert host_out.count(DROPPED_SOUND) == 1, host_out
    assert host_out.index(DROPPED_SOUND) > host_out.index("is the computer's from turn")
    assert not takeovers(join_out) and DROPPED_SOUND not in join_out, join_out


@DEFAULT_PORT
def test_the_lobby_chat_is_one_log_on_both_screens(app, game_dir):
    """MPCHAT.INI beside the players' screen: the host says a line before the
    joiner arrives and one after, the joiner one of its own; each screen's log
    ends up with all three, and the match that follows is still one game."""
    host = ("wait:15;press:MultiPlayer;wait:20;press:Host;wait:20;press:List@0;wait:20;"
            "text:salve;key:Return;wait:260;text:welcome;key:Return" + ";wait:150;press:Start" * 12)
    join = ("wait:15;press:MultiPlayer;wait:20;press:InetHostCombo;text:127.0.0.1;"
            "press:Join;wait:200;text:ave;key:Return;wait:20;press:ImReadyBig")
    host_out, join_out = through_screens(app, game_dir, 20, 20, host, join)
    for out in (host_out, join_out):
        said = [text for _, text in re.findall(r"lobby\s+(.*?): (.*)", out)]
        for line in ("salve", "welcome", "ave"):
            assert line in said, out
    assert fields(host_out)["world"] == fields(join_out)["world"], host_out + join_out


# --------------------------------------------------------------------------
# a late joiner (this engine's: the original admits nobody once a match has
# started -- docs/engine/netjoin.md)
# --------------------------------------------------------------------------

def trace_of(text: str) -> tuple[int, list[str]]:
    m = re.search(r"trace\s+from (\d+):((?: [0-9a-f]+)*)", text)
    assert m, text
    return int(m[1]), m[2].split()


def check_late_window(host_out: str, late_out: str, others: tuple = ()) -> int:
    """The late window took the departed seat from the host's save and ran
    every turn it ran the way the host did, every channel rolled up."""
    m = re.search(r"joining a match already running, as player (\d+) from turn (\d+)", late_out)
    assert m, late_out
    seat, first = int(m[1]), int(m[2])
    assert re.search(r"loaded:\s+\d+ objects, \d+ scripts, turn %d, hashes verified" % first,
                     late_out), late_out
    assert re.search(rf"net\s+player {seat} left \(a player left\); dropped from turn (\d+)",
                     host_out), host_out
    assert f"player {seat} is joining from turn {first}" in host_out, host_out
    assert re.search(rf"net\s+the save for player {seat}, \d+ bytes, after turn {first - 1}",
                     host_out), host_out
    host_from, host_trace = trace_of(host_out)
    late_from, late_trace = trace_of(late_out)
    assert host_from == 0 and late_from == first
    assert late_trace and late_trace == host_trace[first:first + len(late_trace)], host_out + late_out
    assert len(host_trace) == first + len(late_trace), host_out + late_out
    host, late = fields(host_out), fields(late_out)
    assert host["netcmds"] == late["netcmds"], host_out + late_out
    assert host["refused"] == late["refused"] == 0
    for out in others:
        _, trace = trace_of(out)
        assert trace == host_trace, out
    return first


@pytest.mark.parametrize("drop", (0, 150))
def test_a_late_window_takes_the_seat_of_one_that_left(app, game_dir, drop):
    """Three windows from the command line; one leaves after 15 turns and
    the computer takes its seat; a fourth window joins the running match,
    is handed the host's save, takes that seat and plays to the end -- its
    right click an order like anyone's."""
    if not (game_dir / ZAMA).is_file():
        pytest.skip(f"{ZAMA} is not in this installation")
    port = 47871 + drop // 150
    common = ["--game", str(game_dir), "--map", ZAMA, "--net-timeout", "60000",
              "--net-drop", str(drop), "--select", "mine:30", *SIZE]
    host = subprocess.Popen([str(app), *common, "--net-turns", "90", "--host", str(port),
                             "--player", "0", "--seats", "1,2",
                             "--input", "wait:40;rclick:300,250;wait:60;rclick:500,400"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs = []
    try:
        lines = []
        for line in host.stdout:
            lines.append(line)
            if "hosting on port" in line:
                break
        else:
            pytest.fail("the host never listened:\n" + "".join(lines))
        address = f"127.0.0.1:{port}"
        stay = subprocess.Popen([str(app), *common, "--net-turns", "90", "--join", address,
                                 "--input", "wait:50;rclick:350,300"],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        procs.append(stay)
        leaver = subprocess.run([str(app), *common, "--net-turns", "15", "--join", address],
                                capture_output=True, text=True, timeout=180)
        late = subprocess.run([str(app), *common, "--net-turns", "90", "--join", address,
                               "--input", "wait:50;rclick:320,280"],
                              capture_output=True, text=True, timeout=180)
        stay_out = stay.communicate(timeout=180)[0]
        rest, _ = host.communicate(timeout=180)
    finally:
        for p in [host, *procs]:
            if p.poll() is None:
                p.kill()
                p.wait()
    host_out = "".join(lines) + rest
    late_out = late.stdout + late.stderr
    assert host.returncode == 0, host_out
    assert late.returncode == 0, late_out
    assert re.search(r"net\s+15 turns as player", leaver.stdout), leaver.stdout
    assert re.search(r"net\s+90 turns as player", late_out), late_out
    first = check_late_window(host_out, late_out, others=(stay_out,))
    # Its click became an order on a turn it ran.
    assert any(int(turn) >= first for turn, _, _ in fields(late_out)["turn_lines"]), late_out
    # The windows that saw the seat go marked it with the pack's "(AI)" and
    # played the sound once; the late join took the marker off on its first
    # turn, silently. The late window saw neither: it arrived after.
    marker = computer_marker(game_dir)
    seat = int(re.search(r"as player (\d+) from turn", late_out)[1])
    for out in (host_out, stay_out):
        taken = takeovers(out)
        assert [s for s, _, _ in taken] == [seat], out
        assert taken[0][2].endswith(marker) and taken[0][1] < first, out
        assert out.count(DROPPED_SOUND) == 1, out
        back = hand_backs(out)
        assert [(s, t) for s, t, _ in back] == [(seat, first)], out
        assert back[0][2] + marker == taken[0][2], out
    assert not takeovers(late_out) and not hand_backs(late_out), late_out
    assert DROPPED_SOUND not in late_out, late_out


@DEFAULT_PORT
def test_a_late_window_joins_through_the_shipped_screens(app, game_dir):
    """The host through its screens, a joiner through its screens who plays
    20 turns and leaves; then a third window types the address on
    MPMENU.INI and presses Join as a player does -- and is taken into the
    running match, on the seat the computer took, instead of a lobby."""
    common = ["--game", str(game_dir), "--net-timeout", "60000", *SIZE]
    host = subprocess.Popen([str(app), *common, "--net-turns", "100", "--input", HOST_SCREENS],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        lines = []
        for line in host.stdout:
            lines.append(line)
            if "hosting" in line:
                break
        else:
            pytest.fail("the host never opened its lobby:\n" + "".join(lines))
        leaver = subprocess.run([str(app), *common, "--net-turns", "20", "--input", JOIN_SCREENS],
                                capture_output=True, text=True, timeout=240)
        late_screens = ("wait:15;press:MultiPlayer;wait:20;press:InetHostCombo;text:127.0.0.1;"
                        "press:Join")
        late = subprocess.run([str(app), *common, "--net-turns", "100", "--input", late_screens],
                              capture_output=True, text=True, timeout=240)
        rest, _ = host.communicate(timeout=240)
    finally:
        if host.poll() is None:
            host.kill()
            host.wait()
    host_out = "".join(lines) + rest
    late_out = late.stdout + late.stderr
    assert re.search(r"net\s+20 turns as player", leaver.stdout), leaver.stdout + host_out
    assert re.search(r"net\s+100 turns as player", host_out), host_out
    assert re.search(r"net\s+100 turns as player", late_out), late_out
    check_late_window(host_out, late_out)
    # The same map, told rather than given.
    host_map = re.search(r"playing (\S+) as player", host_out)
    late_map = re.search(r"playing (\S+) as player", late_out)
    assert host_map and late_map and host_map[1] == late_map[1]
