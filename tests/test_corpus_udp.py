"""Two processes, one match, over a real socket.

`imconform netplay` proves the protocol on a loopback inside one process;
`udp-host` and `udp-join` play the same match between two processes over UDP
on localhost -- the lobby, the link layer's resends against real loss and,
with `--drop`, deliberate loss, and a clock measuring real round trips.

Each process checks itself against an unnetworked run of the stream and
schedule it agreed on, and prints two hashes. **The comparison that matters is
the one only this file can make: host against joiner, in the same match.**
Across two matches the world hash legitimately differs -- turn lengths come
from measured round trips, which is real time -- so nothing here compares one
run with another.

Localhost cannot lose a datagram on its own, so `--drop` is what makes the
link layer's repair path run; the test asserts datagrams were dropped and
that the peers agreed anyway, rather than trusting "ok".
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
ALESIA = "Adventures/GreatBattles/3_Great_Battles_Alesia.bfhp"


@pytest.fixture(scope="module")
def imconform() -> Path:
    path, complaint = corpus.find_tool("imconform", "IMPERIVM_IMCONFORM")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


def play(imconform: Path, game_dir: Path, host_map: str, join_map: str | None = None,
         turns: int = 20, drop: int = 0, timeout_ms: int = 60000,
         host_extra: tuple = (), join_extra: tuple = ()):
    """Start a host on an ephemeral port, then a joiner against it."""
    for relative in (host_map, join_map or host_map):
        if not (game_dir / relative).is_file():
            pytest.skip(f"{relative} is not in this installation")
    host = subprocess.Popen(
        [str(imconform), "udp-host", str(game_dir), str(game_dir / host_map),
         "--port", "0", "--turns", str(turns), "--drop", str(drop),
         "--timeout", str(timeout_ms), *host_extra],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    try:
        # The port line, which a `--skirmish` host prints after its setup.
        first = ""
        match = None
        while match is None:
            line = host.stdout.readline()
            first += line
            match = re.search(r"listening port (\d+)", line)
            if not line:
                break
        assert match, first + host.stdout.read()
        join = subprocess.run(
            [str(imconform), "udp-join", str(game_dir), str(game_dir / (join_map or host_map)),
             f"127.0.0.1:{match[1]}", "--drop", str(drop), "--timeout", str(timeout_ms),
             *join_extra],
            capture_output=True, text=True, timeout=timeout_ms / 1000 + 30,
        )
        rest, _ = host.communicate(timeout=timeout_ms / 1000 + 30)
    finally:
        if host.poll() is None:
            host.kill()
            host.wait()
    return (host.returncode, first + rest), (join.returncode, join.stdout + join.stderr)


def fields(text: str) -> dict:
    out: dict = {}
    if m := re.search(r"wire\s+(\d+) sent, (\d+) dropped, (\d+) received, (\d+) duplicates, "
                      r"(\d+) refused", text):
        out["sent"], out["dropped"], out["received"], out["duplicates"], out["refused"] = (
            int(m[i]) for i in range(1, 6))
    if m := re.search(r"agreed\s+(\d+) orders, (\d+) commands queued", text):
        out["orders"], out["queued"] = int(m[1]), int(m[2])
    if m := re.search(r"hashes\s+world ([0-9a-f]+) netcmds ([0-9a-f]+)", text):
        out["world"], out["netcmds"] = m[1], m[2]
    if m := re.search(r"udp\s+ok\s+(\d+) turns", text):
        out["turns"] = int(m[1])
    if m := re.search(r"peer\s+player (\d+)", text):
        out["player"] = int(m[1])
    return out


@pytest.mark.parametrize("relative", (ZAMA, NUMANTIA))
def test_two_processes_play_one_game(imconform, game_dir, relative):
    (host_rc, host_out), (join_rc, join_out) = play(imconform, game_dir, relative)
    assert host_rc == 0, host_out
    assert join_rc == 0, join_out
    host, join = fields(host_out), fields(join_out)
    assert host["turns"] == join["turns"] == 20
    assert host["player"] != join["player"]
    assert host["world"] == join["world"], host_out + join_out
    assert host["netcmds"] == join["netcmds"], host_out + join_out
    # And the match was not empty: orders agreed, commands queued on both.
    assert host["orders"] == join["orders"] > 0
    assert host["queued"] == join["queued"] > 0


def test_two_processes_agree_through_deliberate_loss(imconform, game_dir):
    (host_rc, host_out), (join_rc, join_out) = play(
        imconform, game_dir, ZAMA, turns=30, drop=300)
    assert host_rc == 0, host_out
    assert join_rc == 0, join_out
    host, join = fields(host_out), fields(join_out)
    # The repair path ran: both sides lost datagrams on purpose.
    assert host["dropped"] > 0 and join["dropped"] > 0, host_out + join_out
    assert host["refused"] == join["refused"] == 0
    assert host["world"] == join["world"], host_out + join_out
    assert host["netcmds"] == join["netcmds"], host_out + join_out


def test_two_processes_agree_through_the_computers_war(imconform, game_dir):
    """A networked match the computer plays, over the socket: Alesia between
    players 0 and 1, whose armies stand among each other from turn 0, and the
    strike -- a right click a seat gives from its own world on the first turn
    it can (`--strike`, see test_corpus_lockstep.py) -- sent by the process
    whose seat it is. Both processes must be told `--skirmish` and
    `--strike`, since nothing in the start says either; the seats travel in
    the start. No wait on when the computer goes to war, so no speed-up to
    reach it: it ran 420 turns at twelve times the speed, and lost its war to
    each faithful fix to the AI or to movement."""
    turns = 120
    (host_rc, host_out), (join_rc, join_out) = play(
        imconform, game_dir, ALESIA, turns=turns, drop=300, timeout_ms=300000,
        host_extra=("--skirmish", "--seats", "0,1", "--strike", "1"),
        join_extra=("--skirmish", "--strike", "1"))
    assert host_rc == 0, host_out
    assert join_rc == 0, join_out
    host, join = fields(host_out), fields(join_out)
    assert host["turns"] == join["turns"] == turns
    assert host["dropped"] > 0 and join["dropped"] > 0, host_out + join_out
    assert host["refused"] == join["refused"] == 0
    assert host["world"] == join["world"], host_out + join_out
    assert host["netcmds"] == join["netcmds"], host_out + join_out
    # One strike, chosen alike by both, and agreed as an order.
    strikes = [re.findall(r"strike\s+(turn \d+: .*)", out) for out in (host_out, join_out)]
    assert len(strikes[0]) == 1 and strikes[0] == strikes[1], host_out + join_out
    assert host["orders"] == join["orders"] == 1, host_out + join_out
    assert host["queued"] == join["queued"] > 0, host_out + join_out
    blows = [re.search(r"war\s+(\d+) blows", out) for out in (host_out, join_out)]
    assert all(blows) and int(blows[0][1]) == int(blows[1][1]) > 0, host_out + join_out


def test_a_joiner_with_another_map_leaves_and_says_why(imconform, game_dir):
    """The hello carries the game (`data.pak`); the map travels in the start,
    as a path and a hash, because a joiner from the players' screen does not
    know it until the host says. So a joiner holding a different map is
    seated, reads the start, and leaves -- telling the host, which says so
    rather than waiting out its timeout on a peer that will never send.

    A departed peer no longer ends the match: the host drops it from the
    first turn that could have carried its orders, which it never sent, and
    plays on alone."""
    (host_rc, host_out), (join_rc, join_out) = play(
        imconform, game_dir, ZAMA, join_map=NUMANTIA, timeout_ms=20000)
    assert join_rc != 0, join_out
    assert "the host's map is not installed here, or differs" in join_out, join_out
    assert re.search(r"departed\s+player \d+ \(the host's map is not installed here, or differs\) "
                     r"from turn 2\b", host_out), host_out
    assert host_rc == 0, host_out
    assert fields(host_out)["turns"] == 20


# --------------------------------------------------------------------------
# SetSpeed mid-match
# --------------------------------------------------------------------------
#
# A speed change is an order: the joiner asks for 2000 with its sixth packet
# and the host for 700 with its fourteenth. Both processes must apply each on
# the same turn, convert every later turn at it, and agree on the world -- and
# each must match its own unnetworked replay, which only the history carries
# the speed to.

def speeds(text: str) -> list[tuple[int, int, int]]:
    return [(int(p), int(v), int(t)) for p, v, t in
            re.findall(r"speed\s+player (\d+) set (\d+) on turn (\d+)", text)]


@pytest.mark.parametrize("drop", (0, 250))
def test_a_speed_change_takes_effect_on_the_same_turn_on_both(imconform, game_dir, drop):
    (host_rc, host_out), (join_rc, join_out) = play(
        imconform, game_dir, ZAMA, turns=30, drop=drop,
        host_extra=("--speed-at", "14:700"), join_extra=("--speed-at", "6:2000"))
    assert host_rc == 0, host_out
    assert join_rc == 0, join_out
    host, join = fields(host_out), fields(join_out)
    assert speeds(host_out) == speeds(join_out), host_out + join_out
    said = speeds(host_out)
    assert [v for _, v, _ in said] == [2000, 700], host_out
    # The joiner's sixth packet is for its turn 6 + delay; the host's
    # fourteenth for 14 + delay.
    assert said[0][2] == 8 and said[1][2] == 16, host_out
    assert host["world"] == join["world"], host_out + join_out
    assert host["netcmds"] == join["netcmds"]
    lo, hi = re.search(r"schedule\s+\d+ turns, (\d+)\.\.(\d+)", host_out).groups()
    # At 2000 a turn is worth twice its milliseconds; at 700, 0.7 of them.
    assert int(hi) >= 400 and int(lo) <= 280, host_out


# --------------------------------------------------------------------------
# looking is not an input
# --------------------------------------------------------------------------
#
# A networked peer's interface runs scripts no other peer runs: the command
# bar's and info bar's verifiers every refresh, and the cursor's order through
# the `verify=` scripts. `imconform observe` runs two sessions from one seed,
# watches one of them the way a screen does, and requires every per-turn hash
# to match. The check was shown to catch a verifier that draws from the RNG
# (injected, caught at turn 1) before this file trusted it.

@pytest.mark.parametrize("relative", (ZAMA, NUMANTIA))
def test_watching_a_match_changes_no_hash(imconform, game_dir, relative):
    path = game_dir / relative
    if not path.is_file():
        pytest.skip(f"{relative} is not in this installation")
    result = subprocess.run([str(imconform), "observe", str(game_dir), str(path), "40"],
                            capture_output=True, text=True, timeout=600)
    assert result.returncode == 0, result.stdout + result.stderr
    m = re.search(r"looked\s+(\d+) turns, (\d+) buttons described, (\d+) cursor orders", result.stdout)
    assert m, result.stdout
    # It looked, for real: a check that described nothing passes for nothing.
    assert int(m[1]) == 40 and int(m[2]) > 0 and int(m[3]) > 0, result.stdout
    assert "observe   ok" in result.stdout


def test_watching_a_skirmish_through_the_war_changes_no_hash(imconform, game_dir):
    """The same, on a networked match the computer plays, watched from the
    first peer's seat -- one class of its objects a turn, since a bar offers
    only what the whole selection shares -- through the war: Alesia between
    players 0 and 1, whose armies stand among each other, and the strike
    (see test_corpus_lockstep.py), on both runs alike."""
    path = game_dir / ALESIA
    if not path.is_file():
        pytest.skip(f"{ALESIA} is not in this installation")
    turns = 120
    result = subprocess.run([str(imconform), "observe", str(game_dir), str(path), str(turns),
                             "--skirmish", "--seats", "0,1", "--strike", "1"],
                            capture_output=True, text=True, timeout=600)
    assert result.returncode == 0, result.stdout + result.stderr
    m = re.search(r"looked\s+(\d+) turns, (\d+) buttons described, (\d+) cursor orders", result.stdout)
    assert m and int(m[1]) == turns and int(m[2]) > 0 and int(m[3]) > 0, result.stdout
    assert len(re.findall(r"strike\s+[1-9]\d* command\(s\) queued", result.stdout)) == 2, result.stdout
    blows = re.search(r"war\s+(\d+) blows", result.stdout)
    assert blows and int(blows[1]) > 0, result.stdout
    assert "observe   ok" in result.stdout


# --------------------------------------------------------------------------
# a peer leaving: three processes, one of which goes
# --------------------------------------------------------------------------
#
# Two peers cannot show a drop agreed: with one left there is nobody to agree
# with. Three can. The host seats two joiners (`--peers 3`), one joiner leaves
# part-way -- saying so, or silently -- and the host and the other joiner must
# name the same turn its part ended at, hand its seat to the computer on that
# turn, play every turn, match each other's hashes and each match its own
# unnetworked replay of what it agreed. The one that left is only asked to have
# run fewer turns than the rest.

def play_three(imconform: Path, game_dir: Path, relative: str, *, turns: int,
               leaver: list[str], drop: int = 0, silence: int = 0):
    if not (game_dir / relative).is_file():
        pytest.skip(f"{relative} is not in this installation")
    extra = ["--silence", str(silence)] if silence else []
    host = subprocess.Popen(
        [str(imconform), "udp-host", str(game_dir), str(game_dir / relative),
         "--port", "0", "--turns", str(turns), "--drop", str(drop), "--peers", "3",
         "--timeout", "60000", *extra],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    procs = []
    try:
        first = host.stdout.readline()
        match = re.search(r"listening port (\d+)", first)
        assert match, first + host.stdout.read()
        address = f"127.0.0.1:{match[1]}"
        for flags in (leaver, []):
            procs.append(subprocess.Popen(
                [str(imconform), "udp-join", str(game_dir), str(game_dir / relative), address,
                 "--drop", str(drop), "--timeout", "60000", *extra, *flags],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
        outs = [p.communicate(timeout=120)[0] for p in procs]
        rest, _ = host.communicate(timeout=120)
    finally:
        for p in [host, *procs]:
            if p.poll() is None:
                p.kill()
                p.wait()
    return ((host.returncode, first + rest), (procs[0].returncode, outs[0]),
            (procs[1].returncode, outs[1]))


def departed(text: str) -> list[tuple[int, str, int]]:
    return [(int(p), why, int(t)) for p, why, t in
            re.findall(r"departed\s+player (\d+) \(([^)]*)\) from turn (\d+)", text)]


@pytest.mark.parametrize("drop", (0, 250))
def test_a_joiner_that_leaves_is_dropped_and_the_others_play_on(imconform, game_dir, drop):
    (host_rc, host_out), (left_rc, left_out), (stay_rc, stay_out) = play_three(
        imconform, game_dir, ZAMA, turns=40, leaver=["--leave-after", "12"], drop=drop)
    assert left_rc == 0, left_out
    assert "udp       left after 12 turns" in left_out, left_out
    assert host_rc == 0, host_out
    assert stay_rc == 0, stay_out
    host, stay = fields(host_out), fields(stay_out)
    assert host["turns"] == stay["turns"] == 40
    assert host["world"] == stay["world"], host_out + stay_out
    assert host["netcmds"] == stay["netcmds"], host_out + stay_out
    assert host["refused"] == stay["refused"] == 0
    # One departure, the same turn on both, and past every turn it ran.
    gone_host, gone_stay = departed(host_out), departed(stay_out)
    assert len(gone_host) == len(gone_stay) == 1, host_out + stay_out
    assert gone_host[0][0] == gone_stay[0][0] == fields(left_out)["player"]
    assert gone_host[0][2] == gone_stay[0][2] >= 12
    assert gone_host[0][1] == "a player left"
    # And on that turn, on both, the computer took the seat -- which the
    # unnetworked replay each peer checked itself against did too.
    for out in (host_out, stay_out):
        assert re.search(rf"takeover\s+player {gone_host[0][0]} on turn {gone_host[0][2]}: "
                         r"the computer takes the seat", out), out


def test_a_joiner_that_crashes_is_timed_out_and_dropped(imconform, game_dir):
    (host_rc, host_out), (left_rc, left_out), (stay_rc, stay_out) = play_three(
        imconform, game_dir, ZAMA, turns=40, leaver=["--crash-after", "10"], silence=1500)
    assert "crashed after 10 turns" in left_out, left_out
    assert host_rc == 0, host_out
    assert stay_rc == 0, stay_out
    host, stay = fields(host_out), fields(stay_out)
    assert host["world"] == stay["world"], host_out + stay_out
    gone_host, gone_stay = departed(host_out), departed(stay_out)
    assert gone_host and gone_stay, host_out + stay_out
    # The host timed it out; the other joiner heard it from the host.
    assert gone_host[0][1] == "stopped answering"
    assert gone_host[0][2] == gone_stay[0][2]


# --------------------------------------------------------------------------
# a late joiner: four processes, one of which takes a departed player's seat
# --------------------------------------------------------------------------
#
# This engine's own feature -- the original admits nobody once a match has
# started (docs/engine/netjoin.md). The host seats two joiners; one leaves
# after 12 turns and the computer takes its seat; the host holds the match at
# turn 25 until a fourth process says hello to it, seats that process on the
# departed seat from the host's save, and everyone plays to 60. Every process
# prints every turn's rolled-up hash from the first turn it ran; the late
# joiner's must equal the others' on every turn it ran, its stream hash must
# equal theirs, and it must match its own unnetworked replay from the save.

def trace_of(text: str) -> tuple[int, list[str]]:
    m = re.search(r"trace\s+from (\d+):((?: [0-9a-f]+)*)", text)
    assert m, text
    return int(m[1]), m[2].split()


def play_late(imconform: Path, game_dir: Path, relative: str, *, drop: int = 0,
              host_extra: tuple = (), late_extra: tuple = ()):
    if not (game_dir / relative).is_file():
        pytest.skip(f"{relative} is not in this installation")
    path = str(game_dir / relative)
    common = ["--drop", str(drop), "--timeout", "90000"]
    host = subprocess.Popen(
        [str(imconform), "udp-host", str(game_dir), path, "--port", "0", "--turns", "60",
         "--peers", "3", "--wait-join-at", "25", *common, *host_extra],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    procs = []
    try:
        first = host.stdout.readline()
        match = re.search(r"listening port (\d+)", first)
        assert match, first + host.stdout.read()
        address = f"127.0.0.1:{match[1]}"
        stay = subprocess.Popen([str(imconform), "udp-join", str(game_dir), path, address, *common],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        procs.append(stay)
        leaver = subprocess.run([str(imconform), "udp-join", str(game_dir), path, address,
                                 "--leave-after", "12", *common],
                                capture_output=True, text=True, timeout=120)
        late = subprocess.run([str(imconform), "udp-join", str(game_dir), path, address,
                               *common, *late_extra],
                              capture_output=True, text=True, timeout=120)
        stay_out = stay.communicate(timeout=120)[0]
        rest, _ = host.communicate(timeout=120)
    finally:
        for p in [host, *procs]:
            if p.poll() is None:
                p.kill()
                p.wait()
    return ((host.returncode, first + rest), (stay.returncode, stay_out),
            (leaver.returncode, leaver.stdout + leaver.stderr),
            (late.returncode, late.stdout + late.stderr))


def check_late(host_out: str, stay_out: str, late_out: str) -> int:
    host, stay, late = fields(host_out), fields(stay_out), fields(late_out)
    seat = late["player"]
    joined = re.search(r"joined\s+as player (\d+) from turn (\d+)", late_out)
    assert joined, late_out
    first = int(joined[2])
    # It took the seat the leaver left, after the computer took it.
    gone = departed(host_out)
    assert len(gone) == 1 and gone[0][0] == seat and gone[0][2] < first, host_out
    for out in (host_out, stay_out, late_out):
        assert f"joining   player {seat} from turn {first}" in out, out
        assert re.search(rf"handback\s+player {seat} on turn {first}: the computer stops", out), out
    assert re.search(rf"seated\s+player {seat} from turn {first}, \d+ bytes", host_out), host_out
    assert "loaded" in late_out and "hashes verified" in late_out, late_out
    # Every channel, every turn it ran, from its first; and the stream.
    host_from, host_trace = trace_of(host_out)
    stay_from, stay_trace = trace_of(stay_out)
    late_from, late_trace = trace_of(late_out)
    assert host_from == stay_from == 0 and late_from == first
    assert len(host_trace) == len(stay_trace) == 60
    assert host_trace == stay_trace, host_out + stay_out
    assert late_trace == host_trace[first:], host_out + late_out
    assert host["netcmds"] == stay["netcmds"] == late["netcmds"]
    assert host["refused"] == stay["refused"] == late["refused"] == 0
    return first


@pytest.mark.parametrize("drop", (0, 200))
def test_a_late_joiner_takes_the_departed_seat_and_hashes_like_everyone(imconform, game_dir, drop):
    (host_rc, host_out), (stay_rc, stay_out), (left_rc, left_out), (late_rc, late_out) = play_late(
        imconform, game_dir, ZAMA, drop=drop)
    assert left_rc == 0 and "udp       left after 12 turns" in left_out, left_out
    assert host_rc == 0, host_out
    assert stay_rc == 0, stay_out
    assert late_rc == 0, late_out
    check_late(host_out, stay_out, late_out)


def test_a_late_joiner_seated_during_a_speed_change_starts_at_the_new_speed(imconform, game_dir):
    """The host asks for 2000 with the first packet it sends after seating
    the joiner: the last turn the save carries. So the header's speed is
    the new one, and the joiner's first turn is the first run at it."""
    (host_rc, host_out), (stay_rc, stay_out), _, (late_rc, late_out) = play_late(
        imconform, game_dir, ZAMA, drop=150, host_extra=("--speed-on-join", "2000"))
    assert host_rc == 0, host_out
    assert stay_rc == 0, stay_out
    assert late_rc == 0, late_out
    first = check_late(host_out, stay_out, late_out)
    assert re.search(r"late\s+as player \d+ from turn \d+, speed 2000,", late_out), late_out
    said = speeds(host_out)
    assert said == speeds(stay_out)
    assert said[-1][1] == 2000 and said[-1][2] == first - 1, host_out


def test_the_in_process_late_join_agrees_on_real_sessions(imconform, game_dir):
    path = game_dir / ZAMA
    if not path.is_file():
        pytest.skip(f"{ZAMA} is not in this installation")
    result = subprocess.run([str(imconform), "netjoin", str(game_dir), str(path)],
                            capture_output=True, text=True, timeout=600)
    assert result.returncode == 0, result.stdout + result.stderr
    assert len(re.findall(r"joined\s+player", result.stdout)) == 2, result.stdout
    assert "netjoin   ok" in result.stdout
