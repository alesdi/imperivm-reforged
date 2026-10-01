"""Corpus tests for the VS script parser. Specification: docs/formats/vs-language.md.

The parser reaches 100% of the shipped scripts, and the assertion below is
exact on purpose: a change that makes it parse 576 of 577 is a regression, and
the point of an exact count is that it cannot be quietly absorbed.
"""

from __future__ import annotations

import pytest

from conftest import requires_game
from imperivm.formats import vs_parse

pytestmark = requires_game

SCRIPT_COUNT = 577


@pytest.fixture(scope="module")
def scripts(packs) -> list[tuple[str, str]]:
    """Every `.vs` source in the installation, as `(label, text)`."""
    return [
        (f"{label}:{name}", data.decode("cp1252"))
        for label, name, data in packs.entries_named(".VS")
    ]


def test_every_script_is_found(scripts):
    assert len(scripts) == SCRIPT_COUNT


def test_every_script_parses(scripts):
    failures: list[str] = []
    for label, source in scripts:
        try:
            vs_parse.parse(source, label)
        except vs_parse.VSSyntaxError as exc:
            failures.append(f"{label}: {exc}")
    assert failures == []


def test_every_script_yields_a_walkable_tree(scripts):
    """Parsing is not enough: the result has to be a tree the tools can walk."""
    statements = 0
    for label, source in scripts:
        script = vs_parse.parse(source, label)
        nodes = list(vs_parse.walk(script))
        assert nodes, label
        statements += len(nodes)
    assert statements > SCRIPT_COUNT
