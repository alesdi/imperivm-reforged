"""The one shipped conquest, read by the engine's own reader.

These assertions used to live in `engine/tests/test_campaign.cpp`, against
5,729 bytes of `Conquests/mediterranean.BFHP` -> `territories.xml` pasted into
a C++ raw string literal. `docs/legal.md` rule 1 forbids that in as many words:
no game assets in the repository, "not as test fixtures". `tools/check_fixtures.py`
now refuses it mechanically.

They did not weaken in the move. A fixture is a *copy*, and a copy can drift
from the file it was taken from without anything noticing; these run against
the container on the machine of whoever owns the game, through
`imcheck conquest`, which drives the same `ConquestMap`, `parse_sequences` and
`resolve_maps` the session uses. What is left in `test_campaign.cpp` is the
reader's mechanics -- comma splitting, refusal of a graph that does not close,
defaulting of `autorunallowed` -- against documents written from
`docs/formats/`, which is what a unit test should have been asserting all along.

Four claims, and each is one this project got wrong at least once:

1. **`index` is not the map number.** Spain is `index="3"` and plays on
   `Maps/10`. Anything that treated the attribute as a directory number would
   load six wrong maps and one right one.

2. **`bonus` names a sequence, not a class.** `rIberia`, `rRepublicanRome` and
   the rest read exactly like class names -- a prefix and a race -- and all
   seven resolve against the container-root `Sequences/sequences.xml`, whose
   entries all carry `autorunallowed="no"` so that they run only when invoked.

3. **`mapname` and `visualname` both differ from `id`, in both directions.**
   `Gaul` displays as `Gallia` and plays on the map called `Galicia`. A reader
   that derived either would be wrong seven times out of seven.

4. **The graph closes and is symmetric.** 18 neighbour references, every one
   resolving, every one reciprocated. Symmetry is a fact about the content and
   not about the format, which is why the reader validates the first and this
   module checks the second.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from conftest import requires_game

import corpus

pytestmark = requires_game


#: The only conquest container in the installation.
CONQUEST = Path("Conquests/mediterranean.BFHP")


@pytest.fixture(scope="module")
def imcheck() -> Path:
    path, complaint = corpus.find_tool("imcheck", "IMPERIVM_IMCHECK")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


@pytest.fixture(scope="module")
def dump(imcheck: Path, game_dir: Path) -> dict[str, list[list[str]]]:
    """`imcheck conquest`'s output, grouped by its leading keyword.

    The report is line-oriented on purpose: one fact per line, keyword first,
    so that a test names the fact it is asserting rather than an offset into a
    blob.
    """
    container = game_dir / CONQUEST
    if not container.is_file():
        pytest.skip(f"{CONQUEST} is not in this installation")
    result = subprocess.run(
        [str(imcheck), "conquest", str(container)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert result.returncode == 0, f"imcheck conquest refused:\n{result.stdout}\n{result.stderr}"

    grouped: dict[str, list[list[str]]] = {}
    for line in result.stdout.splitlines():
        if not line.strip():
            continue
        head, _, rest = line.partition(" ")
        grouped.setdefault(head, []).append(rest.split())
    return grouped


def one(dump: dict[str, list[list[str]]], key: str) -> str:
    rows = dump.get(key, [])
    assert len(rows) == 1, f"expected exactly one {key!r} line, got {len(rows)}"
    return " ".join(rows[0])


def attributes(row: list[str]) -> dict[str, str]:
    """`['Spain', 'index=3', ...]` -> `{'id': 'Spain', 'index': '3', ...}`."""
    out = {"id": row[0]}
    for field in row[1:]:
        key, _, value = field.partition("=")
        out[key] = value
    return out


def test_the_document_parses_and_the_graph_closes(dump):
    assert one(dump, "validate") == "ok"
    # The trailing space in `name` is in the file. Trimming it would make the
    # reader disagree with whatever the original displays.
    assert one(dump, "name") == "Mediterranean"  # split() ate the trailing space
    assert one(dump, "choose") == "1"
    assert one(dump, "interface") == "-1"
    # Empty in the retail file: this install has never finished a conquest
    # mission. It is the field the original appends to with `, %d`.
    assert dump.get("order", [[]]) == [[]]

    # Nine colourisation knobs, not the twelve an earlier `docs/formats/map.md`
    # claimed: three `_colorize`, three `_hue`, three `_sat`.
    assert len(dump["display"]) == 9
    assert dump["display"][0] == ["owned_colorize", "1"]
    assert dump["display"][3] == ["owned_hue", "560"]


def test_the_seven_territories_are_in_document_order(dump):
    territories = [attributes(row) for row in dump["territory"]]
    assert [t["id"] for t in territories] == [
        "Spain",
        "Britain",
        "Gaul",
        "Italy",
        "Carthage",
        "Egypt",
        "Germany",
    ]
    # Every territory is authored `state="1"`. That is the only reason to read
    # `choose="1"` as "the player picks where to start"; it is not proof.
    assert {t["state"] for t in territories} == {"1"}


def test_index_is_not_the_map_number(dump):
    territories = [attributes(row) for row in dump["territory"]]
    assert [t["index"] for t in territories] == ["3", "4", "8", "7", "6", "2", "5"]

    # And here is the proof that it is not a directory number: `resolved` pairs
    # each territory with the `Maps/<n>` its `mapname` found, and Spain is
    # `index="3"` on `Maps/10`.
    resolved = {int(row[0]): int(row[1]) for row in dump["resolved"]}
    assert resolved == {0: 10, 1: 3, 2: 7, 3: 9, 4: 4, 5: 6, 6: 8}
    # Seven territories, seven maps, a bijection.
    assert sorted(resolved.values()) == sorted(int(row[0]) for row in dump["map"])
    assert len(set(resolved.values())) == 7


def test_display_name_and_map_name_both_differ_from_the_id(dump):
    by_id = {t["id"]: t for t in (attributes(row) for row in dump["territory"])}
    assert by_id["Gaul"]["visual"] == "Gallia"
    assert by_id["Gaul"]["map"] == "Galicia"
    assert by_id["Italy"]["visual"] == "Roma"
    assert by_id["Italy"]["map"] == "Rome"
    # Not one of the seven has an id equal to both of the other two.
    assert not [t for t in by_id.values() if t["visual"] == t["id"] == t["map"]]


def test_bonus_names_a_root_sequence_and_not_a_class(dump):
    assert one(dump, "bonuses") == "ok"

    sequences = {row[0]: attributes(row) for row in dump["sequence"]}
    # Eight sequences: the dispatcher plus one per territory.
    assert len(sequences) == 8
    assert sequences["StartBonuses"]["autorun"] == "1"
    assert {name for name, s in sequences.items() if s["autorun"] == "0"} == {
        "rBritain",
        "rCarthage",
        "rEgypt",
        "rGaul",
        "rGermany",
        "rIberia",
        "rRepublicanRome",
    }

    # Spain's bonus is `rIberia`, not `rSpain`: the territory id and the bonus
    # name disagree, which is why the mapping has to be read and not derived.
    by_id = {t["id"]: t for t in (attributes(row) for row in dump["territory"])}
    assert by_id["Spain"]["bonus"] == "rIberia"
    assert by_id["Italy"]["bonus"] == "rRepublicanRome"
    assert all(t["bonus"] in sequences for t in by_id.values())


def test_the_neighbour_graph_is_symmetric(dump):
    edges = [(row[0], row[1]) for row in dump["neighbour"]]
    # 18 references, and the split has to keep the last field of each list:
    # dropping it would still validate for Britain, which has one neighbour,
    # and would break Gaul, which has four.
    assert len(edges) == 18
    assert sorted(to for frm, to in edges if frm == "Gaul") == [
        "Britain",
        "Germany",
        "Italy",
        "Spain",
    ]
    # Symmetry is a property of the content, not of the format. Nothing in the
    # reader enforces it and nothing should.
    assert {(a, b) for a, b in edges} == {(b, a) for a, b in edges}
