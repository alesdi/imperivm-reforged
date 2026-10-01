"""The command bar over a shipped map, through `imconform buttons`.

Playtest #2: "Ripara" -- Repair, key R, 500 gold -- was offered, lit, on a fort
at 5000/5000. The row (`DATA/COMMANDS/TOWNHALL.XML`, `repair townhall`) names
`VERIFY_CMDCOST_BUILDING.VS`, which never asks about health and answers true on
every path; what keeps the row off a standing building in the original is the
building's own command predicate (0x004de580, `vtbl+0xd8`), which the bar asks
of every selected object (0x005e787b) and which leaves off -- does not grey --
a row it refuses. A ruin offers its repair and nothing else.

The tool runs no window and advances nothing: it builds the session, selects
the first player-owned object of the class, sets its health when asked, and
prints the bar.
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

import corpus
from conftest import requires_game

pytestmark = [requires_game]

MAP = "Adventures/GreatBattles/2_Great_Battles_Numantia.bfhp"


@pytest.fixture(scope="module")
def imconform() -> Path:
    path, complaint = corpus.find_tool("imconform", "IMPERIVM_IMCONFORM")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


def bar(imconform: Path, game_dir: Path, klass: str, health: int | None = None):
    """(tier, {button: 'lit' | 'unlit'}) for the first `klass` a player owns."""
    path = game_dir / MAP
    if not path.is_file():
        pytest.skip(f"{MAP} is not in this installation")
    command = [str(imconform), "buttons", str(game_dir), str(path), klass]
    if health is not None:
        command.append(str(health))
    result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, result.stdout + result.stderr
    tier = re.search(r"^object .* tier (\d+)$", result.stdout, re.M)
    assert tier, result.stdout
    buttons = dict(re.findall(r"^button\s+(.+?) (lit|unlit) key=", result.stdout, re.M))
    count = re.search(r"^buttons\s+(\d+)$", result.stdout, re.M)
    assert count and int(count.group(1)) == len(buttons), result.stdout
    return int(tier.group(1)), buttons


def test_a_fort_at_full_health_offers_no_repair(imconform, game_dir):
    tier, buttons = bar(imconform, game_dir, "BaseTownhall")
    assert tier == 0
    assert "repair townhall" not in buttons
    # The rest of the fort's bar is there: the predicate took one row, not all.
    assert buttons, "a standing fort offers something"
    # And a verifier's refusal is still drawn, unlit -- the other way a button
    # says no, which the predicate's refusal is not. On this map the fort's
    # gold mules are refused.
    assert "unlit" in buttons.values(), buttons
    assert "lit" in buttons.values(), buttons


def test_a_damaged_fort_that_is_not_a_ruin_offers_no_repair(imconform, game_dir):
    # 60% is tier 1: damaged, standing, and nothing `RRepair` would act on.
    tier, buttons = bar(imconform, game_dir, "BaseTownhall", 3000)
    assert tier == 1
    assert "repair townhall" not in buttons


def test_a_broken_fort_offers_repair_lit_and_nothing_else(imconform, game_dir):
    tier, buttons = bar(imconform, game_dir, "BaseTownhall", 0)
    assert tier == 3
    assert buttons == {"repair townhall": "lit"}


def presses(imconform: Path, game_dir: Path, klass: str, names: list[str]):
    """[(verdict, [queue entries, running first])] after each press of `names`."""
    path = game_dir / MAP
    if not path.is_file():
        pytest.skip(f"{MAP} is not in this installation")
    command = [str(imconform), "buttons", str(game_dir), str(path), klass]
    for name in names:
        command += ["--press", name]
    result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, result.stdout + result.stderr
    verdicts = re.findall(r"^press\s+(.+) (issued|waiting|refused|unknown)$", result.stdout, re.M)
    queues = re.findall(r"^queue\s+(\d+)(.*)$", result.stdout, re.M)
    assert [name for name, _ in verdicts] == names, result.stdout
    assert len(queues) == len(names), result.stdout
    out = []
    for (_, verdict), (count, rest) in zip(verdicts, queues):
        # Names hold spaces ("Barrack Level 1"); the tool puts " | " before each.
        entries = rest.split(" | ")[1:]
        assert int(count) == len(entries), result.stdout
        out.append((verdict, entries))
    return out


def test_a_second_train_press_on_a_barracks_queues_behind_the_first(imconform, game_dir):
    # Playtest #14: the second button cancelled the unit in training. A
    # `traincommand="yes"` row never replaces (0x005e39f0, 0x004efbbb), so each
    # press adds one, behind what is running.
    first, second, third = presses(imconform, game_dir, "BaseBarracks",
                                   ["trainIMilitiaman", "trainIArcher", "trainIMilitiaman"])
    assert first == ("issued", ["trainIMilitiaman"])
    assert second == ("issued", ["trainIMilitiaman", "trainIArcher"])
    assert third == ("issued", ["trainIMilitiaman", "trainIArcher", "trainIMilitiaman"])


def test_a_research_press_replaces_what_the_barracks_trains(imconform, game_dir):
    # The flag is the row's, and a research row does not carry it: without
    # Shift it replaces the queue as a unit's move would.
    *_, research = presses(imconform, game_dir, "BaseBarracks",
                           ["trainIMilitiaman", "trainIArcher", "Barrack Level 1"])
    assert research == ("issued", ["Barrack Level 1"])


def test_shift_appends_a_move_and_ctrl_does_not(imconform, game_dir):
    # The bar reads two keys (0x005e39f0): Shift is the append, Ctrl the
    # dispatch's `bModifier`. A plain move replaces the last, a Shift move
    # queues behind it, and a Ctrl move -- which the app used to be unable to
    # give, passing Shift as both -- replaces like a plain one.
    first, second, shifted, ctrl = presses(imconform, game_dir, "Unit", [
        "move@1000,1000", "move@1200,1000", "shift+move@1400,1000", "ctrl+move@1000,1200"])
    assert first == ("issued", ["move"])
    assert second == ("issued", ["move"])
    assert shifted == ("issued", ["move", "move"])
    assert ctrl == ("issued", ["move"])


def test_ctrl_on_a_train_button_queues_train_multiple_count(imconform, game_dir):
    # 0x005e39f0: Ctrl on a `traincommand` row posts one order with a count,
    # `[GamePlay] TrainMultipleCount` (5), and its execution runs the row that
    # many times (0x004e5e60). Behind what is training; Shift changes nothing.
    first, ctrl, both, plain = presses(imconform, game_dir, "BaseBarracks", [
        "trainIMilitiaman", "ctrl+trainIArcher", "shift+ctrl+trainIMilitiaman", "trainIArcher"])
    assert first == ("issued", ["trainIMilitiaman"])
    assert ctrl == ("issued", ["trainIMilitiaman"] + ["trainIArcher"] * 5)
    assert both == ("issued", ["trainIMilitiaman"] + ["trainIArcher"] * 5 + ["trainIMilitiaman"] * 5)
    assert plain[1] == ["trainIMilitiaman"] + ["trainIArcher"] * 5 + ["trainIMilitiaman"] * 5 + ["trainIArcher"]


BALCANS = "Scenarios/Balcans.BFHP"


def test_a_cancelled_queue_cell_is_taken_out_and_refunded(imconform, game_dir):
    # `--cancel N` is a click on the queue strip's cell N: a `cancel_command`
    # order for that cell's command id, applied through the stream
    # (0x006bf7c0 posts it, 0x004e63c0 runs it). The row's gold comes back
    # (0x005b07d0 -> 0x005b18d0 -> a building's 0x004df400), the running
    # entry's too, and a cell past the end cancels nothing. Balcans' first
    # barracks has gold to spend; a hastatus is 100.
    path = game_dir / BALCANS
    if not path.is_file():
        pytest.skip(f"{BALCANS} is not in this installation")
    result = subprocess.run(
        [str(imconform), "buttons", str(game_dir), str(path), "BaseBarracks",
         "--press", "trainMHastatus", "--press", "ctrl+trainMHastatus",
         "--cancel", "5", "--cancel", "0", "--cancel", "9"],
        capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, result.stdout + result.stderr
    gold = [int(g) for g in re.findall(r"^gold\s+(-?\d+)$", result.stdout, re.M)]
    queues = [int(n) for n in re.findall(r"^queue\s+(\d+)", result.stdout, re.M)]
    cancels = re.findall(r"^cancel\s+(\d+) (done|nothing)$", result.stdout, re.M)
    # The last cell, the running one, and one past the end.
    assert cancels == [("5", "done"), ("0", "done"), ("9", "nothing")], result.stdout
    assert queues == [1, 6, 5, 4, 4], result.stdout
    start = gold[0]
    assert gold == [start, start - 100, start - 600, start - 500, start - 400, start - 400], result.stdout
