"""The shipped victory script, compiled and run by the engine itself.

`DATA/GAMESCRIPTS/1 ELIMINATION.VS` is the script every skirmish and every
`victory_condition="1 Elimination"` map starts, one copy per participant. It is
2,142 bytes and it uses thirteen host entry points across five domains -- the
environment store, the player table, the query system, the match rules and
localisation -- which makes it the best single end-to-end statement available
about whether this engine's script surface is the one the game expects.

It used to be asserted in `engine/tests/test_match.cpp`, against a copy of the
file pasted into a C++ raw string literal. `docs/legal.md` rule 1 forbids that
-- no game assets in the repository, "not as test fixtures" -- and
`tools/check_fixtures.py` now refuses it mechanically.

The claim is stronger here than it was there. A fixture is a *copy*: it proves
that the bytes somebody once pasted still compile, and nothing about the file
they came from. This opens `data.pak`.

## What "it works" means

Four things, and the script only reaches the fourth by way of the first three:

* it **parses** with this engine's grammar;
* it **compiles** against the real `HostRegistry`, which is where a wrong arity
  or a missing entry point shows up;
* it **runs to completion with no traps**, which is where a wrong *semantic*
  shows up -- a `Query` that never empties, a `Sleep` that never wakes;
* and it **ends the match**, by reaching `EndGame(player, true)` and writing its
  own bookkeeping into the environment store, which is the value every other
  player's copy reads to decide whether it has won.

The world `imcheck victory` builds is the smallest one in which the script can
finish: two computer players, no town halls, no units.
`GetConst("EliminationTimeout")` answers zero without a `CONST.INI`, so the
countdown takes its `nTime == 0` branch on the first pass rather than after
three minutes -- the same branch, reached sooner.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from conftest import requires_game

import corpus

pytestmark = requires_game


#: `MatchOutcome`, from `sim/match.hpp`.
UNDECIDED, WON, LOST = 0, 1, 2


@pytest.fixture(scope="module")
def imcheck() -> Path:
    path, complaint = corpus.find_tool("imcheck", "IMPERIVM_IMCHECK")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


@pytest.fixture(scope="module")
def report(imcheck: Path, game_dir: Path) -> dict[str, str]:
    packs = game_dir / "Packs"
    result = subprocess.run(
        [str(imcheck), "victory", str(packs)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, (
        f"imcheck victory refused:\n{result.stdout}\n{result.stderr}"
    )
    out: dict[str, str] = {}
    traps: list[str] = []
    for line in result.stdout.splitlines():
        head, _, rest = line.partition(" ")
        if head == "trap":
            traps.append(rest.strip())
        elif head:
            out[head] = rest.strip()
    out["_traps"] = "\n".join(traps)
    return out


def test_the_shipped_script_is_the_one_the_engine_asks_for(report):
    # `victory_script_path` builds this name; `data.pak` spells it in upper case
    # with backslashes. If the two ever stop meeting, every skirmish silently
    # runs with no victory condition at all.
    assert report["script"].lower().replace("\\", "/") == "data/gamescripts/1 elimination.vs"
    assert int(report["bytes"]) > 1500


def test_the_shipped_script_compiles_against_the_real_registry(report):
    # This is where a wrong arity or a missing entry point shows up. The script
    # reaches ClassPlayerObjs/2, EnvWriteInt/3, EnvReadInt/3, SetPlayerStatus/4
    # and /3, DiplAreAllied/2, MilUnits/1, GetConst/1, Translate/1,
    # Translatef/3, EndGame/2, Query::IsEmpty/0 and Sleep/1.
    assert report["compiled"] == "ok"


def test_the_shipped_script_runs_without_a_single_trap(report):
    assert report["traps"] == "0", f"traps:\n{report['_traps']}"
    # And it finished, rather than still spinning after eight slices. A victory
    # script that never returns is one that never decides the match.
    assert report["running"] == "0"


def test_the_shipped_script_ends_the_game(report):
    assert int(report["outcome"]) == LOST
    assert int(report["human"]) == LOST
    assert report["over"] == "1"
    # The script's own bookkeeping landed in the environment store under the
    # player's scope. This is the value every rival's copy of the same script
    # polls, so a match in which nobody can win would still pass the assertions
    # above and fail this one.
    assert report["env"] == "1"
