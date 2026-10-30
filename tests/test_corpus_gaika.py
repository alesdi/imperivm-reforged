"""The AI's node partition over a shipped map (docs/formats/gaika.md).

`engine/tests/test_gaika.cpp` checks every step of the cut on pictures small
enough to work by hand. This runs it over Crossroads as the session cuts it at
the start of the match (`IMRUN_GAIKA`, turn 0) and checks what the steps imply
for a whole map:

* the settlement nodes come first, one per seeded settlement, in the
  settlement list's order (steps 2 and 5), then the region nodes (step 6);
* a settlement whose central building is none of the five seeded bases has no
  node of its own;
* the neighbour lists are ascending and symmetric (step 9);
* a second cut of the same world is the same cut, slot for slot.

The node count and the pinned records are this engine's cut of the original's
algorithm on this map, seed 1. No dump of the original's table is at hand to
compare them with -- `CVXGlobalAI` persists `GAIKAs` and `GAIKASlots`, but no
save the original wrote is in the installation -- so they pin the port against
regressions; the claims above are what holds them to the algorithm.
"""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

from conftest import requires_game

import corpus

pytestmark = [requires_game, pytest.mark.spread]

NODE = re.compile(
    r"^ {12}#(\d+) lsa (\d+) set (\d+) rect (-?\d+),(-?\d+),(-?\d+),(-?\d+) "
    r"centre (-?\d+),(-?\d+) cover (\d+) links((?: \d+)*)$"
)
SETTLEMENT = re.compile(r"^ {12}#(\d+) p\d+ kind (\d+) .* gaika (\d+):")


@pytest.fixture(scope="module")
def imrun() -> Path:
    path, complaint = corpus.find_tool("imrun", "IMPERIVM_IMRUN")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


@pytest.fixture(scope="module")
def crossroads(imrun, game_dir) -> str:
    container = game_dir / "Scenarios" / "Crossroads.BFHP"
    if not container.is_file():
        pytest.skip("Crossroads.BFHP is not in this installation")
    result = subprocess.run(
        [str(imrun), str(game_dir), str(container), "0", "800"],
        capture_output=True,
        text=True,
        timeout=600,
        env={**os.environ, "IMRUN_GAIKA": "1", "IMRUN_SETTLEMENTS": "1"},
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return result.stdout


def nodes(output: str) -> dict[int, dict]:
    found = {}
    for line in output.splitlines():
        match = NODE.match(line)
        if match is None:
            continue
        g = [int(v) for v in match.groups()[:10]]
        found[g[0]] = {
            "lsa": g[1],
            "settlement": g[2],
            "rect": tuple(g[3:7]),
            "centre": (g[7], g[8]),
            "cover": g[9],
            "links": [int(v) for v in match.group(11).split()],
        }
    return found


def test_crossroads_is_cut_into_its_settlement_nodes_then_its_regions(crossroads):
    header = re.search(r"^  gaika +(\d+) nodes, (\d+) x (\d+) slots$", crossroads, re.MULTILINE)
    assert header is not None, crossroads
    assert header.groups() == ("57", "128", "128")
    table = nodes(crossroads)
    assert sorted(table) == list(range(1, 58))

    # Settlement nodes first, then regions: once a node has no settlement,
    # none after it has one.
    settled = [n for n in sorted(table) if table[n]["settlement"] != 0]
    assert settled == list(range(1, len(settled) + 1))
    assert len(settled) == 36

    # One node per seeded settlement, in the settlement list's order; the
    # settlements of kind 0 here (none of the five seeded bases) have none.
    seen = []
    for line in crossroads.splitlines():
        match = SETTLEMENT.match(line)
        if match is None:
            continue
        kind, gaika = int(match.group(2)), int(match.group(3))
        if kind == 0:
            assert gaika == 0, line
        else:
            seen.append(gaika)
    assert seen == list(range(1, 37))

    # Ascending, symmetric neighbour lists, never naming the node itself.
    for n, node in table.items():
        assert node["links"] == sorted(set(node["links"])), n
        assert n not in node["links"]
        for other in node["links"]:
            assert n in table[other]["links"], (n, other)

    # A second cut of the same world is the same cut.
    cut = re.search(r"^  gaika cut +\d+ us, (\d+) nodes, (\d+) slots moved$", crossroads, re.MULTILINE)
    assert cut is not None
    assert cut.groups() == ("57", "0")


def test_crossroads_pinned_nodes(crossroads):
    table = nodes(crossroads)
    # The first town's node: its snapped entrance slot's middle, and the
    # slots its seeds won.
    assert table[1] == {
        "lsa": 1, "settlement": 1, "rect": (2, 5, 39, 36), "centre": (2624, 2496),
        "cover": 829, "links": [8, 10, 15, 30, 34, 54, 55],
    }
    # The first region node, and the last.
    assert table[37] == {
        "lsa": 1, "settlement": 0, "rect": (13, 119, 26, 124), "centre": (2624, 15552),
        "cover": 46, "links": [3, 6],
    }
    assert table[57] == {
        "lsa": 1, "settlement": 0, "rect": (39, 0, 46, 4), "centre": (5376, 256),
        "cover": 29, "links": [8, 15],
    }
    # A region node that smoothing emptied keeps its record, with no slots
    # and no neighbours (step 7 then step 8: nothing to vote for it).
    assert table[43]["cover"] == 0 and table[43]["links"] == []
