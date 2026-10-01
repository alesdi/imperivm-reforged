"""The host surface the shipped scripts actually use, and where it hides.

`docs/formats/vs-host-api.md` — and, until this module existed,
`engine/core/src/script/host_surface.cpp` with it — is generated from the **577**
`.vs` files in `data.pak`. The install ships **885**. The other **308** live
inside the 24 `.bfhp` containers (13 adventures, 1 conquest, the scenarios),
which no pack carries, and between them they use **66 free entry points and 13
member entry points that appear nowhere in the packs** — the whole campaign
layer. Every one of them read as an *unknown call* rather than a named trap,
which is precisely the failure `declare_shipped_surface` exists to prevent.

This module pins the split so it cannot come back. It is a corpus test and
skips without an installation, but the C++ table it guards is checked
independently by `test_vs_vm.cpp`'s exact `registry.size()`, so a drift on a
machine with no game data still fails the build somewhere.

## Why these are engine surface and not content

A name in a container script could be a map-declared object rather than an
engine entry point — the containers use 627 bare identifiers, 596 of which are
exactly that (`NO_MyTown`, `Q_HannibalArmy`, `T_RomanFleet`, all three of them
real `<group>` names in the shipped containers). 611 is a different count of a
different thing: bare identifiers the containers use and no pack script does.
An earlier revision of this docstring conflated the two; `test_corpus_globals.py`
now measures both and pins the derivation. The test that separates a map name
from an entry point is `gbr.exe`'s string pool, and it is run **with a
control**:

  * all 65 container-only free-function names occur in the binary as standalone
    null-terminated strings,
  * all 13 member names occur in the `Receiver::Method` form the member
    registrar uses (`Unit::AddBonus`, `ObjList::RemoveFromGroup`,
    `Conversation::SetActor`, `Query::NearestObj`, ...),
  * and **0 of 60** sampled map-declared object names occur in it at all.

65/65 against 0/60 is the whole argument. `test_the_control_finds_nothing`
below is not decoration: without it the first two assertions would be satisfied
by a binary that happened to contain every short identifier.
"""

from __future__ import annotations

import random
import re

import pytest

from conftest import requires_game
from imperivm.formats import vs_parse
from imperivm.formats.bfhp import BlockFile
from imperivm.formats.pak import PackFile

import corpus

pytestmark = requires_game

#: `.vs` files in `data.pak` and the other packs.
PACK_SCRIPT_COUNT = 577
#: `.vs` files inside the shipped `.bfhp` containers.
CONTAINER_SCRIPT_COUNT = 308

#: Free-function entry points used only inside containers, with their site counts.
#: Regenerate by diffing the two inventories; do not hand-edit.
CONTAINER_ONLY_FREE = {
    "AIStart/3": 20, "AIStop/1": 1, "AreaAIMaxPriority/3": 2, "AreaAINoRecruit/3": 8,
    "AreaAISetAttackOptimism/3": 2, "AreaCenter/1": 279, "AreaObjs/2": 193,
    "BlockUserInput/0": 17, "ClassPlayerAreaObjs/3": 274, "ClearDiplomacy/0": 4,
    "ClearNotes/0": 16, "ConquestBonus/0": 1, "ConvResult/1": 5, "DiplCeaseFire/3": 116,
    "DiplShareControl/3": 18, "DiplShareSupport/3": 22, "DiplShareView/3": 55,
    "EndGame/3": 61, "EnvReadInt/1": 191, "EnvReadString/1": 34, "EnvWriteInt/2": 164,
    "EnvWriteString/2": 14, "ExploreAll/0": 9, "ExploreArea/2": 20, "GetDifficulty/0": 136,
    "GiveNote/1": 115, "HideAnnouncement/1": 6, "HideZoomMap/0": 8,
    "IsAIHelperRunning/1": 236, "IsFinished/1": 3, "IsNoteActive/1": 24, "IsRunning/1": 5,
    "IsWaiting/1": 1, "KillScript/0": 13, "LockView/0": 2, "MoveToArea/2": 8,
    "PlayMovie/1": 24, "RemoveNote/1": 65, "RunAIHelper/4": 285, "RunConv/1": 74,
    "RunSequence/1": 34, "SetFog/1": 18, "SetShortcutSel/3": 24, "SetTerritoryState/2": 7,
    "ShowAnnouncement/2": 11, "SpawnGroup/1": 181, "SpawnGroupInHolder/2": 127,
    "SpawnNamed/1": 3, "StopAIHelper/1": 179, "StrMid/3": 1, "UnblockUserInput/0": 17,
    "UnlockView/0": 2, "View/2": 35, "ViewPos/0": 7, "WaitCommonObjects/3": 1,
    "WaitConvRequest/3": 3, "WaitConvRequest/5": 4, "WaitEmptyQuery/2": 50,
    "WaitEnvIntBetween/4": 18, "WaitHealthBetween/4": 8, "WaitIdle/2": 13,
    "WaitObjInQuery/3": 1, "WaitSettlementCapture/3": 14, "WaitUnitsInArea/3": 12,
    "_PlaceEx/4": 89, "abs/1": 2,
}

#: Member entry points used only inside containers, and the qualified string in
#: `gbr.exe` that proves each is a registered member rather than a coincidence.
CONTAINER_ONLY_MEMBERS = {
    "AddBonus/5": "Unit::AddBonus",
    "AddCommandOffset/3": "ObjList::AddCommandOffset",
    "AllowCapture/1": "Settlement::AllowCapture",
    "CmdDisable/1": "Obj::CmdDisable",
    "Disease/0": "Unit::Disease",
    "InHolder/1": "Query::InHolder",
    "Init/1": "Conversation::Init",
    "NearestObj/1": "Query::NearestObj",
    "RemoveFromGroup/1": "ObjList::RemoveFromGroup",
    "Run/0": "Conversation::Run",
    "SetActor/2": "Conversation::SetActor",
    "SetAutocast/1": "Hero::SetAutocast",
    "SetName/1": "Obj::SetName",
}

#: Free-function names both halves use, where the containers add an arity the
#: packs never show. `EndGame` is the three-argument form; the four `Env*` are
#: the root scope.
CONTAINER_ONLY_ARITIES = {
    "EndGame": ({2}, {3}),
    "EnvReadInt": ({2}, {1}),
    "EnvReadString": ({2}, {1}),
    "EnvWriteInt": ({3}, {2}),
    "EnvWriteString": ({3}, {2}),
}

#: The same, for members. This half was missed on the first pass -- the count
#: read "five names" until somebody checked members too -- which is the whole
#: reason it has a test of its own rather than sharing one with the free half.
#: `Unit::InHolder()` takes no argument; `ObjList::InHolder(str)` and
#: `Query::InHolder(str)` take one, registered at 0x00563755 and 0x0057b0f1.
CONTAINER_ONLY_MEMBER_ARITIES = {
    "InHolder": ({0}, {1}),
}


def _fold(table: dict[tuple[str, int], int]) -> dict[tuple[str, int], int]:
    """Fold `vs_parse.NO_PARENS` (-1) into arity 0, as the language does."""
    out: dict[tuple[str, int], int] = {}
    for (name, arity), count in table.items():
        key = (name, max(arity, 0))
        out[key] = out.get(key, 0) + count
    return out


@pytest.fixture(scope="module")
def inventories(game_dir):
    """`(pack, container)` inventories, and the two script counts."""
    pack = vs_parse.Inventory()
    container = vs_parse.Inventory()
    n_pack = n_container = 0
    for path in corpus.pack_paths(game_dir):
        archive = PackFile(path)
        for entry in archive.entries:
            if not entry.name.lower().endswith(".vs"):
                continue
            n_pack += 1
            pack.add_script(
                vs_parse.parse(archive.read(entry.name).decode("cp1252", "replace"), entry.name)
            )
    for path in corpus.container_paths(game_dir):
        block = BlockFile(path)
        for entry in block.entries:
            if entry.is_dir or not entry.name.lower().endswith(".vs"):
                continue
            n_container += 1
            container.add_script(
                vs_parse.parse(block.read(entry.name).decode("cp1252", "replace"), entry.name)
            )
    return pack, container, n_pack, n_container


@pytest.fixture(scope="module")
def binary(game_dir) -> bytes:
    path = game_dir / "gbr.exe"
    if not path.is_file():
        pytest.skip("gbr.exe is not present in the installation")
    return path.read_bytes()


def _standalone(binary: bytes, name: str) -> bool:
    """Is `name` a whole null-terminated string in the binary's pool?"""
    return re.search(rb"\x00" + re.escape(name.encode()) + rb"\x00", binary) is not None


# ---------------------------------------------------------------------------
# the split
# ---------------------------------------------------------------------------


def test_both_halves_of_the_corpus_are_found(inventories):
    _pack, _container, n_pack, n_container = inventories
    assert n_pack == PACK_SCRIPT_COUNT
    assert n_container == CONTAINER_SCRIPT_COUNT


def test_the_container_only_free_surface_is_exactly_this(inventories):
    pack, container, *_ = inventories
    pf, cf = _fold(pack.functions), _fold(container.functions)
    only = {f"{n}/{a}": c for (n, a), c in sorted(cf.items()) if (n, a) not in pf}
    assert only == CONTAINER_ONLY_FREE


def test_the_container_only_member_surface_is_exactly_this(inventories):
    pack, container, *_ = inventories
    pm, cm = _fold(pack.methods), _fold(container.methods)
    only = {f"{n}/{a}" for (n, a) in cm if (n, a) not in pm}
    assert only == set(CONTAINER_ONLY_MEMBERS)


def _extra_arities(pack_table, container_table) -> dict[str, tuple[set[int], set[int]]]:
    """Names both halves use, where the container half adds an arity."""
    found: dict[str, tuple[set[int], set[int]]] = {}
    for name in {n for n, _ in pack_table} & {n for n, _ in container_table}:
        pa = {a for n, a in pack_table if n == name}
        ca = {a for n, a in container_table if n == name}
        if ca - pa:
            found[name] = (pa, ca - pa)
    return found


def test_free_names_used_at_an_arity_the_packs_never_show(inventories):
    """The worst case: declared, and declared wrong.

    A name missing altogether traps with "unknown call". A name present at the
    wrong arity traps with the *right* name and the wrong arity, which reads
    like a script bug rather than a gap in the surface. `EndGame` sat in that
    state for months.
    """
    pack, container, *_ = inventories
    found = _extra_arities(_fold(pack.functions), _fold(container.functions))
    assert found == CONTAINER_ONLY_ARITIES


def test_member_names_used_at_an_arity_the_packs_never_show(inventories):
    """The half that was missed.

    The first pass compared free functions only and reported "five names".
    There are six: members are the half nobody thinks of as "the API", and the
    same blindness that hid the containers hid this inside them.
    """
    pack, container, *_ = inventories
    found = _extra_arities(_fold(pack.methods), _fold(container.methods))
    assert found == CONTAINER_ONLY_MEMBER_ARITIES


# ---------------------------------------------------------------------------
# the evidence, and its control
# ---------------------------------------------------------------------------


def test_every_container_only_free_name_is_a_string_in_the_executable(binary):
    missing = sorted(
        {key.rsplit("/", 1)[0] for key in CONTAINER_ONLY_FREE}
        - {n for n in {k.rsplit("/", 1)[0] for k in CONTAINER_ONLY_FREE} if _standalone(binary, n)}
    )
    assert missing == []


def test_every_container_only_member_is_a_qualified_string_in_the_executable(binary):
    """Members register under `Receiver::Method`, which is why the bare name misses.

    Nine of the thirteen are not standalone strings at all. Searching for the
    bare name and calling it absent is how this list nearly got cut to four.
    """
    missing = [
        qualified
        for qualified in CONTAINER_ONLY_MEMBERS.values()
        if not _standalone(binary, qualified)
    ]
    assert missing == []


def test_the_control_finds_nothing(inventories, binary):
    """The assertions above are worthless without this one.

    Sixty map-declared object names, drawn from the same container scripts by
    the same walk. If any of them turned up in the binary, "the name is in
    `gbr.exe`" would not distinguish an entry point from a piece of content,
    and the two tests above would be measuring nothing.
    """
    _pack, container, *_ = inventories
    declared = sorted(
        name
        for name in container.bare_names
        if name.startswith(("NO_", "Q_", "T_"))
    )
    assert len(declared) >= 60, "not enough map-declared names to form a control"
    sample = random.Random(7).sample(declared, 60)
    assert [name for name in sample if _standalone(binary, name)] == []
