"""Two peers, one command stream, over the maps the installation ships.

`imconform lockstep <game> <map>` starts the same scenario twice, hands both
runs the same synthesised orders on the same turns, and compares their per-turn
hashes. It is the claim multiplayer rests on and the one thing the other
conformance checks do not make: `self` and `map` prove the simulation is a
function of its seed and its schedule, and they feed it no input at all.

## Why "commands queued" is asserted and not just "ok"

A lockstep check that drove nothing passes, and passes for the wrong reason.
That is not hypothetical -- it is what the first version of this did on every
map, twice over, for two different reasons:

* `Crossroads` is a skirmish map and **owns no units at turn 0**. Every object
  a player owns there is a building or a piece of scenery, none of which has a
  ground block in its `<defaultcmd>`, so every synthesised order resolved to
  nothing and the check reported two peers agreeing about an empty match.
* Then, on a map that does have units, every order still resolved to nothing --
  because `imconform`'s scenario had never loaded `DATA/COMMANDS/*.XML`. The
  candidate list said `move` and the command table had no row of that name, so
  the click resolved to `unknown command` 982 times without a word.

Both failures look exactly like success from the outside. So these tests assert
on the *numbers the tool prints about its own stream*, and the map list below
is adventure maps -- the ones with armies standing on them at turn 0.
"""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

import corpus
from conftest import requires_game

# Each test is its own subprocess run and the module's fixtures only find a
# binary, so under `--dist loadgroup` its tests may go to different workers.
pytestmark = [requires_game, pytest.mark.spread]

#: Maps with units on them at turn 0. A skirmish map has none -- see the module
#: docstring -- so one is included deliberately, as the case that must report
#: having driven nothing rather than quietly passing.
ADVENTURE_MAPS = (
    "Adventures/GreatBattles/1_Great_Battles_Zama.bfhp",
    "Adventures/GreatBattles/2_Great_Battles_Numantia.bfhp",
)

TURNS = 15
LENGTH = 800


@pytest.fixture(scope="module")
def imconform() -> Path:
    path, complaint = corpus.find_tool("imconform", "IMPERIVM_IMCONFORM")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


#: One run of the tool per (map, turns), shared by every test below.
#:
#: Each run builds a whole session over a shipped map and steps it twice with
#: scripts and the AI on, which is seconds rather than milliseconds. Six tests
#: each starting their own was most of a ten-minute Python suite; the tool is a
#: pure function of its arguments, so one run answers all of them.
_RUNS: dict[tuple[str, int], subprocess.CompletedProcess] = {}


def run_lockstep(imconform: Path, game_dir: Path, relative: str, turns: int = TURNS):
    path = game_dir / relative
    if not path.is_file():
        pytest.skip(f"{relative} is not in this installation")
    key = (relative, turns)
    if key not in _RUNS:
        _RUNS[key] = subprocess.run(
            [str(imconform), "lockstep", str(game_dir), str(path), str(turns), str(LENGTH)],
            capture_output=True, text=True, timeout=600,
        )
    return _RUNS[key]


def fields(stdout: str) -> dict[str, int]:
    """The numbers the tool prints about its own run."""
    out: dict[str, int] = {}
    if m := re.search(r"stream\s+(\d+) orders over (\d+) turns, (\d+) actors", stdout):
        out["orders"], out["stream_turns"], out["actors"] = (int(m[1]), int(m[2]), int(m[3]))
    if m := re.search(r"driven\s+(\d+) orders applied, (\d+) commands queued", stdout):
        out["applied"], out["queued"] = int(m[1]), int(m[2])
    if m := re.search(r"refused\s+(\d+) actors refused, (\d+) unresolved, (\d+) blocked", stdout):
        out["refused"], out["unresolved"], out["blocked"] = (int(m[1]), int(m[2]), int(m[3]))
    if m := re.search(r"lockstep\s+ok\s+two peers agree on all (\d+) turns", stdout):
        out["agreed"] = int(m[1])
    if m := re.search(r"netcmds\s+([0-9a-f]+)", stdout):
        out["netcmds"] = int(m[1], 16)
    return out


@pytest.mark.parametrize("relative", ADVENTURE_MAPS)
def test_two_peers_agree_over_a_driven_match(imconform, game_dir, relative):
    result = run_lockstep(imconform, game_dir, relative)
    assert result.returncode == 0, result.stdout + result.stderr
    got = fields(result.stdout)
    assert got.get("agreed") == TURNS, result.stdout


@pytest.mark.parametrize("relative", ADVENTURE_MAPS)
def test_the_match_was_not_empty(imconform, game_dir, relative):
    """The assertion that makes the one above mean something.

    Orders synthesised, orders applied, and **commands actually queued** --
    the last is the one that both earlier versions of this got wrong while
    reporting two peers in perfect agreement.
    """
    result = run_lockstep(imconform, game_dir, relative)
    assert result.returncode == 0, result.stdout + result.stderr
    got = fields(result.stdout)
    assert got.get("orders", 0) > 0, result.stdout
    assert got.get("actors", 0) > 0, result.stdout
    # Applied counts both peers, so it is twice the stream's orders.
    assert got.get("applied", 0) == 2 * got["orders"], result.stdout
    assert got.get("queued", 0) > 0, result.stdout
    assert got.get("netcmds", 0) != 0, result.stdout


@pytest.mark.parametrize("relative", ADVENTURE_MAPS)
def test_every_synthesised_order_resolves(imconform, game_dir, relative):
    """The stream is built from actors whose click actually resolves.

    Nothing refused, nothing unresolved: the synthesiser asks the class graph
    the same question the click asks before it puts an actor in the stream, so
    an order that reaches `issue_default_order` and does nothing means the two
    disagree -- which is a finding about the order layer, not about lockstep.
    """
    result = run_lockstep(imconform, game_dir, relative)
    assert result.returncode == 0, result.stdout + result.stderr
    got = fields(result.stdout)
    assert got.get("refused") == 0, result.stdout
    assert got.get("unresolved") == 0, result.stdout


def test_a_map_with_no_units_reports_having_driven_nothing(imconform, game_dir):
    """A skirmish map at turn 0 owns buildings and no units.

    The tool must say so and fail, rather than report two peers agreeing about
    a match in which nobody did anything. This is the guard that would have
    caught both of the false passes in the module docstring.
    """
    path = game_dir / "Scenarios" / "Crossroads.BFHP"
    if not path.is_file():
        pytest.skip("Crossroads.BFHP is not in this installation")
    result = subprocess.run(
        [str(imconform), "lockstep", str(game_dir), str(path), "5", str(LENGTH)],
        capture_output=True, text=True, timeout=600,
    )
    if result.returncode == 0:
        # A later change may give a skirmish map starting units; that is a
        # finding about the map, not a failure, but it must be noticed rather
        # than silently weakening this file.
        got = fields(result.stdout)
        assert got.get("queued", 0) > 0, result.stdout
        pytest.skip("Crossroads now starts with commandable units")
    assert "no orders could be synthesised" in result.stderr, result.stdout + result.stderr


def test_the_stream_hash_is_a_function_of_the_map_and_the_turns(imconform, game_dir):
    """Two runs of the tool agree; a longer run does not.

    The first half is the tool being deterministic, which it has to be before
    anything it reports means anything. The second is the fold seeing the turn
    index -- a stream that ran longer is a different stream.
    """
    path = game_dir / ADVENTURE_MAPS[0]
    if not path.is_file():
        pytest.skip(f"{ADVENTURE_MAPS[0]} is not in this installation")
    # Not through the cache: "two runs agree" means two runs.
    command = [str(imconform), "lockstep", str(game_dir), str(path), str(TURNS), str(LENGTH)]
    first = subprocess.run(command, capture_output=True, text=True, timeout=600)
    second = subprocess.run(command, capture_output=True, text=True, timeout=600)
    assert first.returncode == 0 and second.returncode == 0
    assert fields(first.stdout)["netcmds"] == fields(second.stdout)["netcmds"]

    longer = run_lockstep(imconform, game_dir, ADVENTURE_MAPS[0], turns=TURNS + 4)
    assert longer.returncode == 0, longer.stdout + longer.stderr
    assert fields(longer.stdout)["netcmds"] != fields(first.stdout)["netcmds"]


def test_two_maps_do_not_share_a_stream(imconform, game_dir):
    hashes = set()
    for relative in ADVENTURE_MAPS:
        result = run_lockstep(imconform, game_dir, relative)
        assert result.returncode == 0, result.stdout + result.stderr
        hashes.add(fields(result.stdout)["netcmds"])
    assert len(hashes) == len(ADVENTURE_MAPS)


# --------------------------------------------------------------------------
# netplay: the same claim, through the turn negotiator and a hostile loopback
# --------------------------------------------------------------------------
#
# `lockstep` hands two peers one stream. `netplay` gives every player its own
# peer and session, hands each only its own player's orders, and lets the
# protocol in `sim/lockstep.hpp` assemble the turns -- over a loopback that
# delays, reorders and repeats every packet, with each peer proposing its own
# turn length. What it adds is the wire, the input delay, the stalls and the
# negotiated length; what it proves is that none of them changes the game.
#
# Same rule as above: "ok" is not enough. A network that is never late never
# makes a peer wait, and then the one path that makes lockstep lockstep --
# not running a turn you have not heard about -- has not run. The first
# version of this printed "0 stalls" on both maps and passed.

NETPLAY_TURNS = 20

_NETPLAY: dict[tuple[str, int, int, int], subprocess.CompletedProcess] = {}


def run_netplay(imconform: Path, game_dir: Path, relative: str, turns: int = NETPLAY_TURNS,
                delay: int = 2, net_seed: int = 1):
    path = game_dir / relative
    if not path.is_file():
        pytest.skip(f"{relative} is not in this installation")
    key = (relative, turns, delay, net_seed)
    if key not in _NETPLAY:
        _NETPLAY[key] = subprocess.run(
            [str(imconform), "netplay", str(game_dir), str(path), str(turns), str(delay),
             str(net_seed)],
            capture_output=True, text=True, timeout=600,
        )
    return _NETPLAY[key]


def netplay_fields(stdout: str) -> dict[str, int]:
    out: dict[str, int] = {}
    if m := re.search(r"intent\s+(\d+) orders over (\d+) turns", stdout):
        out["intent"] = int(m[1])
    if m := re.search(r"network\s+(\d+) peers, input delay (\d+)", stdout):
        out["peers"], out["delay"] = int(m[1]), int(m[2])
    if m := re.search(r"wire\s+(\d+) deliveries, (\d+) bytes, (\d+) duplicates, (\d+) refused, "
                      r"(\d+) stalls", stdout):
        (out["deliveries"], out["bytes"], out["duplicates"], out["refused"],
         out["stalls"]) = (int(m[i]) for i in range(1, 6))
    if m := re.search(r"agreed\s+(\d+) orders, (\d+) commands queued", stdout):
        out["orders"], out["queued"] = int(m[1]), int(m[2])
    if m := re.search(r"schedule\s+(\d+) turns, (\d+)\.\.(\d+) game-time units", stdout):
        out["scheduled"], out["shortest"], out["longest"] = int(m[1]), int(m[2]), int(m[3])
    if m := re.search(r"netplay\s+ok\s+(\d+) peers agree on all (\d+) turns", stdout):
        out["agreed_peers"], out["agreed"] = int(m[1]), int(m[2])
    return out


@pytest.mark.parametrize("relative", ADVENTURE_MAPS)
def test_every_peer_plays_one_game_over_a_hostile_network(imconform, game_dir, relative):
    result = run_netplay(imconform, game_dir, relative)
    assert result.returncode == 0, result.stdout + result.stderr
    got = netplay_fields(result.stdout)
    assert got.get("agreed") == NETPLAY_TURNS, result.stdout
    assert got.get("agreed_peers") == got.get("peers") >= 2, result.stdout


@pytest.mark.parametrize("relative", ADVENTURE_MAPS)
def test_the_network_was_hostile_and_the_match_was_not_empty(imconform, game_dir, relative):
    """The assertions that make the one above mean something."""
    result = run_netplay(imconform, game_dir, relative)
    assert result.returncode == 0, result.stdout + result.stderr
    got = netplay_fields(result.stdout)
    assert got.get("stalls", 0) > 0, result.stdout       # somebody waited
    assert got.get("duplicates", 0) > 0, result.stdout   # retransmits arrived
    assert got.get("refused") == 0, result.stdout        # and none was refused
    assert got.get("queued", 0) > 0, result.stdout       # orders did something
    # The length was negotiated, not constant.
    assert got["shortest"] < got["longest"], result.stdout


@pytest.mark.parametrize("relative", ADVENTURE_MAPS)
def test_every_order_given_in_time_is_agreed(imconform, game_dir, relative):
    """The synthesiser gives orders on turns 1, 5, 9, ... and the protocol
    schedules them `delay` turns later, so every intent order lands inside a
    20-turn match: none may be lost on the way."""
    result = run_netplay(imconform, game_dir, relative)
    assert result.returncode == 0, result.stdout + result.stderr
    got = netplay_fields(result.stdout)
    assert got.get("orders") == got.get("intent") > 0, result.stdout


def test_netplay_is_a_function_of_its_arguments(imconform, game_dir):
    path = game_dir / ADVENTURE_MAPS[0]
    if not path.is_file():
        pytest.skip(f"{ADVENTURE_MAPS[0]} is not in this installation")
    command = [str(imconform), "netplay", str(game_dir), str(path), "12", "2", "3"]
    first = subprocess.run(command, capture_output=True, text=True, timeout=600)
    second = subprocess.run(command, capture_output=True, text=True, timeout=600)
    assert first.returncode == 0, first.stdout + first.stderr
    assert first.stdout == second.stdout


def test_netplay_with_no_input_delay(imconform, game_dir):
    """Delay 0 is the protocol's edge: every packet is needed the turn after it
    is sent, so most turns wait. It must still finish, and agree."""
    result = run_netplay(imconform, game_dir, ADVENTURE_MAPS[0], turns=12, delay=0)
    assert result.returncode == 0, result.stdout + result.stderr
    got = netplay_fields(result.stdout)
    assert got.get("agreed") == 12, result.stdout
    assert got.get("stalls", 0) > 0, result.stdout


def test_netplay_refuses_a_map_nobody_can_order_on(imconform, game_dir):
    path = game_dir / "Scenarios" / "Crossroads.BFHP"
    if not path.is_file():
        pytest.skip("Crossroads.BFHP is not in this installation")
    result = subprocess.run(
        [str(imconform), "netplay", str(game_dir), str(path), "5"],
        capture_output=True, text=True, timeout=600,
    )
    if result.returncode == 0:
        pytest.skip("Crossroads now starts with commandable units")
    assert "fewer than two players could be given orders" in result.stderr, (
        result.stdout + result.stderr)


# --------------------------------------------------------------------------
# a networked match the computer plays, between peers that barely give orders
# --------------------------------------------------------------------------
#
# The checks above refuse Crossroads, rightly: nobody owns a unit at turn 0,
# so no order can be synthesised. But a skirmish is where the simulation does
# most -- class behaviours, garrisons inside their holders, the AI training,
# marching and fighting, hooks inside a death -- and none of it needs an
# order. `--skirmish` builds the map as `imrun` and the app do (terrain,
# settlement templates, entities, a match started over the peers' seats as a
# networked game's humans) and the peers give at most one order. What stands
# in for "the match was not empty" is the war: a blow between two players.
# Each peer is started as its own seat (`screens`), because whose screen it is
# is the one thing two peers of a match are sure to disagree about.
#
# **The short runs do not wait for the computer to start a war.** They stood
# on it until a faithful fix moved it out of reach for the third time in one
# day: Crossroads' first blow came at turn 261 while sentries walked off their
# walls, at 736 once they kept them, at 405 from the settings screen's richest
# gold, and not inside the run once units stopped standing on one another. A
# test that has to be retuned whenever the AI or movement improves is
# measuring when the AI comes to blows, not whether the peers agree.
#
# So the short runs play Alesia as a networked match between players 0 and 1
# (`--seats 0,1`), the computer playing everyone else. Its two armies stand
# among each other from turn 0, 68 world units apart at the nearest, and in a
# networked match nobody has made them allies: blows from turn 1, by
# placement. And the peers give a real order (`--strike 1`): on the first turn
# a seat has a unit that a right click on an enemy sends to `attack`, the seat
# holding the nearest such pair right-clicks it with up to eight of its units
# -- chosen on that turn from that peer's own world, so whatever the turn
# lengths the network agreed, the order names units that are there. Neither
# the AI's plans nor a long march stands between the run and its blow.
#
# Crossroads keeps the guard below -- twenty turns into a skirmish no army can
# have reached another's -- and the long opt-in runs, which play its own setup
# to the capture.

CROSSROADS = "Scenarios/Crossroads.BFHP"
ALESIA = "Adventures/GreatBattles/3_Great_Battles_Alesia.bfhp"
WAR_TURNS = 120
#: Players 0 and 1 as the peers, and the strike; see above.
WAR = ("--seats", "0,1", "--strike", "1")

_SKIRMISH: dict[tuple[str, ...], subprocess.CompletedProcess] = {}


def run_skirmish(imconform: Path, game_dir: Path, command: str, *args: str,
                 timeout: int = 900, relative: str = CROSSROADS) -> subprocess.CompletedProcess:
    path = game_dir / relative
    if not path.is_file():
        pytest.skip(f"{relative} is not in this installation")
    key = (relative, command, *args)
    if key not in _SKIRMISH:
        _SKIRMISH[key] = subprocess.run(
            [str(imconform), command, str(game_dir), str(path), *args, "--skirmish"],
            capture_output=True, text=True, timeout=timeout,
        )
    return _SKIRMISH[key]


def war_fields(stdout: str) -> dict:
    out: dict = {}
    if m := re.search(r"war\s+(\d+) blows and (\d+) deaths between players", stdout):
        out["blows"], out["deaths"] = int(m[1]), int(m[2])
    if m := re.search(r"screens\s+(.*)", stdout):
        out["screens"] = m[1].split()
    if m := re.search(r"computer\s+plays((?: \d+)*) from the start", stdout):
        out["computer"] = [int(p) for p in m[1].split()]
    out["takes"] = re.findall(r"turn \d+: the computer takes p(\d+)", stdout)
    out["leaves"] = re.findall(r"turn \d+: the computer leaves p(\d+)", stdout)
    out["strikes"] = re.findall(r"strike\s+turn (\d+): player (\d+)'s (\d+) unit", stdout)
    return out


def test_two_peers_agree_through_the_computers_war(imconform, game_dir):
    result = run_skirmish(imconform, game_dir, "lockstep", str(WAR_TURNS), str(LENGTH), *WAR,
                          relative=ALESIA)
    assert result.returncode == 0, result.stdout + result.stderr
    assert (f"lockstep  ok    two peers, as players 0 and 1, agree on all {WAR_TURNS} turns"
            in result.stdout), result.stdout
    war = war_fields(result.stdout)
    assert war["blows"] > 0, result.stdout
    # Both peers chose the same strike, and it queued what it named.
    assert len(war["strikes"]) == 2 and war["strikes"][0] == war["strikes"][1], result.stdout
    queued = re.findall(r"strike\s+(\d+) command\(s\) queued, 0 blocked", result.stdout)
    assert len(queued) == 2 and int(queued[0]) > 0, result.stdout
    # Each peer watched from its own seat, and the seats are a networked
    # match's humans: the computer plays everyone else, and not them.
    assert war["screens"] == ["0", "1"], result.stdout
    assert 0 not in war["computer"] and 1 not in war["computer"], result.stdout
    assert 2 in war["computer"] and 3 in war["computer"], result.stdout


def test_a_skirmish_that_never_fought_checked_nothing(imconform, game_dir):
    """The guard that makes the war tests mean something: twenty turns into
    Crossroads, sixteen seconds of game, no army can have reached another's
    town, and the tool says so and fails. A sentry fighting the wildlife is
    not a war and must not count as one."""
    result = run_skirmish(imconform, game_dir, "lockstep", "20", str(LENGTH))
    assert result.returncode == 1, result.stdout + result.stderr
    assert "the war never started" in result.stdout, result.stdout
    # The peers still agreed; that is not what failed.
    assert "lockstep  ok" in result.stdout, result.stdout


def test_netplay_through_the_computers_war(imconform, game_dir):
    """Each peer its own session and seat, the hostile loopback, and an
    unnetworked replay -- the strike an order in the agreed stream, sent by
    the seat whose order it is."""
    turns = WAR_TURNS
    result = run_skirmish(imconform, game_dir, "netplay", str(turns), "2", "1", *WAR,
                          relative=ALESIA)
    assert result.returncode == 0, result.stdout + result.stderr
    got = netplay_fields(result.stdout)
    assert got.get("agreed") == turns and got.get("agreed_peers") == 2, result.stdout
    assert got.get("stalls", 0) > 0 and got.get("refused") == 0, result.stdout
    assert re.search(r"agreed\s+1 orders, [1-9]\d* commands queued", result.stdout), result.stdout
    war = war_fields(result.stdout)
    assert war["blows"] > 0, result.stdout
    # Two peers as their own seats, and the replay as nobody's.
    assert sorted(war["screens"]) == ["0", "1", "none"], result.stdout


def test_a_seat_left_and_taken_back_twice_through_the_war(imconform, game_dir):
    """`netjoin`'s match, players 0, 1 and 2 seated: player 2 leaves and the
    computer takes the seat, a late joiner loads the host's save and takes it
    back, leaves in turn, and a second joiner takes it back again. This is
    the run that found a session loaded without being started never giving a
    newly trained unit its `idle`: the second joiner parted from its peers
    the turn after it was seated."""
    result = run_skirmish(imconform, game_dir, "netjoin", str(WAR_TURNS), "1",
                          "--seats", "0,1,2", "--strike", "1", relative=ALESIA)
    assert result.returncode == 0, result.stdout + result.stderr
    assert len(re.findall(r"joined\s+player 2", result.stdout)) == 2, result.stdout
    assert len(re.findall(r"departed\s+player 2", result.stdout)) == 2, result.stdout
    war = war_fields(result.stdout)
    assert war["blows"] > 0, result.stdout
    assert war["strikes"], result.stdout
    # The computer really took the seat and really gave it back, twice.
    assert war["takes"] == ["2", "2"] and war["leaves"] == ["2", "2"], result.stdout
    # Three peers and both joiners each from their own seat; the replay from none.
    assert sorted(war["screens"]) == ["0", "1", "2", "2", "2", "none"], result.stdout
    assert "netjoin   ok" in result.stdout, result.stdout


# The long runs: minutes each, so opt-in -- `IMPERIVM_LONG_NET=1`, which
# `tools/verify.py --war` sets. What they add to the ones above is the rest of
# the war: towns change hands, and on the map's own setup the match is decided
# by a capture on the same turn and with the same final hash `imrun` reaches,
# which is the proof that the peers played the game and not a lesser one.

long_net = pytest.mark.skipif(not os.environ.get("IMPERIVM_LONG_NET"),
                              reason="a long network run; IMPERIVM_LONG_NET=1 runs it")


@long_net
def test_the_computers_war_agrees_to_the_capture(imconform, game_dir):
    imrun, complaint = corpus.find_tool("imrun", "IMPERIVM_IMRUN")
    if imrun is None or complaint:
        pytest.skip(complaint)
    path = game_dir / CROSSROADS
    if not path.is_file():
        pytest.skip(f"{CROSSROADS} is not in this installation")
    solo = subprocess.run(
        [str(imrun), str(game_dir), str(path), "4000", str(LENGTH)],
        capture_output=True, text=True, timeout=1800,
        env={**os.environ, "IMRUN_UNTIL_OVER": "1"},
    )
    assert solo.returncode == 0, solo.stdout + solo.stderr
    over = re.search(r"after (\d+) turns of", solo.stdout)
    digest = re.search(r"hash\s+([0-9a-f]{16})", solo.stdout)
    assert over and digest, solo.stdout
    turns = int(over[1])
    result = run_skirmish(imconform, game_dir, "lockstep", str(turns), str(LENGTH),
                          "--setup", "map", timeout=1800)
    assert result.returncode == 0, result.stdout + result.stderr
    assert f"agree on all {turns} turns" in result.stdout, result.stdout
    assert re.search(rf"session\s+hash {digest[1]} after turn {turns}", result.stdout), (
        solo.stdout + result.stdout)
    assert re.search(r"p\d+ takes p0's stronghold", result.stdout), result.stdout
    war = war_fields(result.stdout)
    assert war["blows"] > 0 and war["deaths"] > 0, result.stdout


@long_net
def test_netplay_agrees_well_into_the_war(imconform, game_dir):
    result = run_skirmish(imconform, game_dir, "netplay", "2400", "2", "1", timeout=1800)
    assert result.returncode == 0, result.stdout + result.stderr
    assert netplay_fields(result.stdout).get("agreed") == 2400, result.stdout
    assert war_fields(result.stdout)["deaths"] > 0, result.stdout


@long_net
def test_a_seat_changes_hands_well_into_the_war(imconform, game_dir):
    result = run_skirmish(imconform, game_dir, "netjoin", "1500", "1", timeout=1800)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "netjoin   ok" in result.stdout, result.stdout
    assert war_fields(result.stdout)["deaths"] > 0, result.stdout
