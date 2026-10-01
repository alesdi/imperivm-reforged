"""Corpus tests for the resolved object model. Specification: docs/data-model.md."""

from __future__ import annotations

from conftest import requires_game
from imperivm.formats.gamedata import GameDataError

pytestmark = requires_game

CLASS_COUNT = 845
ENTITY_COUNT = 889
ROOT = "Object"
MAX_DEPTH = 7

#: Dangling resource references in the retail install, enumerated in
#: docs/data-model.md. The bound is an upper one: fewer would mean the reader
#: learned to resolve something it used to miss, which is an improvement, but
#: more means a regression.
MAX_DANGLING = 38


def test_the_class_graph_has_the_expected_size(game_data):
    assert len(game_data.classes) == CLASS_COUNT
    assert len(game_data.entities) == ENTITY_COUNT


def test_all_classes_resolve_to_a_single_root(game_data):
    assert game_data.roots() == [ROOT]

    reached = {ROOT}
    frontier = [ROOT]
    while frontier:
        current = frontier.pop()
        for child in game_data.children(current):
            reached.add(child)
            frontier.append(child)
    assert reached == set(game_data.classes)


def test_the_inheritance_graph_is_acyclic(game_data):
    for class_id in game_data.classes:
        try:
            chain = game_data.ancestry(class_id)
        except GameDataError as exc:  # pragma: no cover - would be a real finding
            raise AssertionError(f"{class_id}: {exc}") from None
        assert chain[-1].id == ROOT, class_id


def test_the_hierarchy_is_seven_deep(game_data):
    """`depth` counts the class itself, so the root is 1 and the deepest leaf 7."""
    depths = {class_id: game_data.depth(class_id) for class_id in game_data.classes}
    assert depths[ROOT] == 1
    assert max(depths.values()) == MAX_DEPTH


def test_dangling_references_do_not_exceed_the_known_set(game_data):
    issues = game_data.validate()
    assert len(issues) <= MAX_DANGLING, "\n".join(str(i) for i in issues)
