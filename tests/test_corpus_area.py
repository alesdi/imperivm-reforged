"""Corpus tests for map areas. Specification: `docs/formats/map.md`, "Area".

An area is a named circle or rectangle: the thing scripts mean by a *place*.
`AreaCenter("Ruins")`, `AttackArea(Q_Wave1, "A_LeftPos1")`,
`WaitUnitsInArea(G1, "Campfire", 100)`. 904 ship, and the engine side of them is
`engine/core/include/imperivm/core/sim/area.hpp`.

Three things here are worth more than the counts.

**The shape split is exact, and the `type` attribute is shared with cargo.**
`type="1"` is a circle and `type="0"` a rectangle, but `CVXWagon` carries a
`type` too, where it means a cargo kind. A reader that branched on `type` alone
would manufacture a degenerate rectangle at the origin for all 26 wagons. The
tests below assert the two sets are disjoint from *both* directions.

**The object's own `x`/`y` is exactly the centre of its region, in all 904.**
That is what makes the placement and the shape one fact rather than two, and it
is the invariant most likely to be broken by a future editor. `gbr.exe`'s
`AreaCenter` (0x004d8bb0) computes the centre from the shape and never reads
`x`/`y`, so this is a genuine cross-check and not a tautology.

**Every area is named by exactly one `<group type="0">`.** There is no area
registry anywhere in the format or in the executable -- `GetAreaByName`
(0x004d9050) is a lookup in the ordinary named-object map -- so an area no
alias names is unreachable from any script. The count of those is pinned at
zero, because it going non-zero is how the whole subsystem would quietly stop
working for one map.

Discovery is by content and has to be twice over, exactly as
`test_corpus_adventure.py` records: sniffing `HPFS` alone finds 23 containers
and misses `Packs/RandomMapSettlements.bfhp`, which is a whole-file LZIS stream
wrapping one -- and which holds 58 of the 904 areas. The totals here only add
up because of it.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

from conftest import requires_game
from imperivm.formats.bfhp import MAGIC as HPFS_MAGIC, BlockFile
from imperivm.formats.lzis import decompress
from imperivm.formats.map import AREA_CIRCLE, AREA_RECT, GROUP_ALIAS, ObjectList

pytestmark = requires_game

#: `Maps/<n>` directories holding a `map.obj.xml`, across all 24 containers.
MAP_DOCUMENT_COUNT = 29

#: Area objects, and the split by shape. `docs/formats/map.md`.
AREA_COUNT = 904
CIRCLE_COUNT = 661
RECTANGLE_COUNT = 243

#: The class every one of them is spawned from.
AREA_CLASS = "AdvArea"

#: Objects carrying `type` that are *not* areas: `CVXWagon` cargo.
CARGO_COUNT = 26
CARGO_CLASS = "CVXWagon"

#: Areas reachable by name, and areas no `<group type="0">` names.
NAMED_AREA_COUNT = 904
UNNAMED_AREA_COUNT = 0


@pytest.fixture(scope="module")
def map_documents(game_dir: Path) -> list[tuple[str, ObjectList]]:
    """Every `map.obj.xml` in the installation, by content.

    Returns `(label, ObjectList)` in a stable order so a failure names the map.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from corpus import _walk

    documents: list[tuple[str, ObjectList]] = []
    for path in sorted(_walk(game_dir)):
        try:
            with path.open("rb") as handle:
                head = handle.read(4)
        except OSError:  # pragma: no cover - unreadable file in the install
            continue
        if head == HPFS_MAGIC:
            container = BlockFile(path)
        elif head == b"LZIS":
            try:
                data = decompress(path.read_bytes())
            except Exception:  # noqa: BLE001 - not every LZIS stream is a container
                continue
            if data[:4] != HPFS_MAGIC:
                continue
            container = BlockFile(path, data=data)
        else:
            continue

        label = path.relative_to(game_dir).as_posix()
        for entry in sorted(container.entries, key=lambda e: e.name):
            if entry.is_dir or not entry.name.lower().endswith("map.obj.xml"):
                continue
            documents.append(
                (f"{label}:{entry.name}", ObjectList.parse(container.read(entry.name), entry.name))
            )
    return documents


def _areas(documents):
    for label, objects in documents:
        for obj in objects.objects:
            if obj.area is not None:
                yield label, obj


def test_the_installation_holds_the_expected_map_documents(map_documents):
    assert len(map_documents) == MAP_DOCUMENT_COUNT


def test_the_area_population_splits_cleanly_into_two_shapes(map_documents):
    shapes = [obj.area.shape for _, obj in _areas(map_documents)]
    assert len(shapes) == AREA_COUNT
    assert shapes.count(AREA_CIRCLE) == CIRCLE_COUNT
    assert shapes.count(AREA_RECT) == RECTANGLE_COUNT
    # No third value, which is what makes `type` a two-valued discriminator
    # rather than an enumeration with cases nobody has met yet.
    assert set(shapes) == {AREA_CIRCLE, AREA_RECT}


def test_every_area_is_an_advarea_and_every_advarea_is_an_area(map_documents):
    """The class and the shape agree in both directions, in all 904 cases."""
    with_shape = {(label, obj.num) for label, obj in _areas(map_documents)}
    with_class = {
        (label, obj.num)
        for label, objects in map_documents
        for obj in objects.objects
        if obj.class_name == AREA_CLASS
    }
    assert with_shape == with_class


def test_no_area_carries_attributes_of_the_wrong_shape(map_documents):
    """A circle has no rectangle fields set and a rectangle no circle fields.

    The reference reader fills both halves of :class:`Area` from whichever
    attributes are present, so a mixed object would show up as a shape with the
    other shape's fields non-zero.
    """
    for label, obj in _areas(map_documents):
        area = obj.area
        if area.shape == AREA_CIRCLE:
            assert (area.left, area.top, area.right, area.bottom) == (0, 0, 0, 0), (
                f"{label} num={obj.num}"
            )
        else:
            assert (area.x, area.y, area.radius) == (0, 0, 0), f"{label} num={obj.num}"


def test_the_cargo_type_attribute_is_not_an_area_shape(map_documents):
    """`type` on a `CVXWagon` is a cargo kind. The two sets are disjoint."""
    cargo = [
        (label, obj)
        for label, objects in map_documents
        for obj in objects.objects
        if obj.cargo_type is not None
    ]
    assert len(cargo) == CARGO_COUNT
    for label, obj in cargo:
        assert obj.area is None, f"{label} num={obj.num} read as an area"
        assert obj.cargo_amount is not None, f"{label} num={obj.num} has type but no amount"
    # And from the other side: no area object carries `amount`.
    for label, obj in _areas(map_documents):
        assert obj.cargo_amount is None, f"{label} num={obj.num}"


def test_an_areas_own_position_is_exactly_the_centre_of_its_region(map_documents):
    """All 904. This is the invariant that ties placement to shape."""
    for label, obj in _areas(map_documents):
        assert obj.position == obj.area.centre(), f"{label} num={obj.num}"


def test_an_areas_own_position_is_inside_its_own_region(map_documents):
    """The weaker consequence, checked separately.

    `contains` is the engine's membership test rather than the centre formula,
    so this exercises a different function against the same 904 objects. A
    degenerate region -- a zero radius, or a reversed rectangle -- would fail
    here while still passing the centre test.
    """
    for label, obj in _areas(map_documents):
        assert obj.area.contains(*obj.position), f"{label} num={obj.num}"


def test_nextmap_and_targetarea_are_empty_on_every_area(map_documents):
    """Both ship unset on all 904, which is why nothing reads them yet.

    `gbr.exe` takes them as the last two arguments of `_AdvPlaceAreaCirc` and
    `_AdvPlaceAreaRect`, so they are real fields of a real editor -- they are
    simply never authored. This test is what would notice a fan-made map using
    one.
    """
    for label, obj in _areas(map_documents):
        assert (obj.next_map or "") == "", f"{label} num={obj.num}"
        assert (obj.target_area or "") == "", f"{label} num={obj.num}"


def test_every_area_is_named_by_exactly_one_alias_group(map_documents):
    """There is no other way to say an area's name.

    An area no `<group type="0">` names cannot be reached from a script at all,
    so the count of those is the number of regions the map author drew and
    nothing can use.
    """
    named = 0
    unnamed = 0
    for _, objects in map_documents:
        aliases: dict[int, int] = {}
        for group in objects.groups:
            if group.type != GROUP_ALIAS:
                continue
            for num in group.members:
                aliases[num] = aliases.get(num, 0) + 1
        for obj in objects.objects:
            if obj.area is None:
                continue
            if aliases.get(obj.num, 0) >= 1:
                named += 1
            else:
                unnamed += 1
    assert named == NAMED_AREA_COUNT
    assert unnamed == UNNAMED_AREA_COUNT


def test_an_alias_naming_an_area_names_nothing_else(map_documents):
    """Every alias that names an area has exactly one member, and it is the area.

    `NamedObjectTable::bind` takes the first member of an alias group, so an
    alias with several members would bind a name to an object whose shape this
    table does not hold.
    """
    for label, objects in map_documents:
        areas = {obj.num for obj in objects.objects if obj.area is not None}
        for group in objects.groups:
            if group.type != GROUP_ALIAS:
                continue
            if not any(num in areas for num in group.members):
                continue
            assert len(group.members) == 1, f"{label} group {group.name!r}"
            assert group.members[0] in areas, f"{label} group {group.name!r}"
