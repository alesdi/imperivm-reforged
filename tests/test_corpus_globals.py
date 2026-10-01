"""Which bare identifiers the 308 container scripts read, and where each resolves.

A VS script mentions **bare identifiers** -- names with no call parentheses.
`engine/core/src/sim/globals.cpp`'s `resolve_global` is what answers them, and
until the most recent work it answered three ways and refused everything else,
so roughly half the mission sequences died on an unknown `NO_*` or `Q_*` name
before reaching a host call. Two steps were added -- `c<class id>` constants
minted from the class graph, and the map's own `<group>` names -- and this
module is the corpus test that pins what those two steps carry.

## Why a corpus test and not a unit test

`engine/tests/test_globals.cpp` proves the *rules*: the sanitiser spells a
constant the way `gbr.exe` `0x0059c060` does, a named object beats a group of
the same name, an unknown name traps rather than interning an empty group. Not
one of those cases can say how much of the shipped corpus the rules actually
reach, and that is the number that decides whether a mission runs. This module
answers it, from the install, with every table read from the engine that was
built rather than from a list somebody transcribed:

  * `imcheck globals` prints the 109 compiled-in constants,
  * `imcheck surface` prints every entry point and its arity,
  * `DATA\\AI\\**\\AI.INI` supplies the `AIV_`/`AIMV_` and family names,
  * `DATA\\CLASSES` supplies the class ids the `c*` constants are minted from,
  * each container's own `map.obj.xml` supplies its `<group>` names.

## The three numbers, and what they are not

The plan recorded this as a "611/600/11 split", and all three reproduce -- but
not with the meaning the older `test_corpus_host_surface.py` docstring gave
them, which said the containers "declare 611 bare identifiers that are exactly
that [map-declared]". They do not. Measured:

  * **627** distinct bare identifiers over the 308 container scripts, 3,832
    sites;
  * **611** of those appear in no pack script -- that is what 611 counts, and
    the arithmetic is 627 minus the 245 the packs use, sixteen of which the
    containers use too;
  * of the 611, **600** are not class constants and **11** are. That is the
    600/11 split;
  * and **596** -- not 611 -- are map-declared `<group>` names.

The residue, the names that resolve nowhere at all, is **empty**. It was one --
`selu`, the console's selected unit -- until `sim/orders.cpp` bound it as the
zero-argument free function it is in `gbr.exe`, at which point it moved to the
`free0` step, which is what a bound name does. See
`test_the_residue_is_empty_and_selu_is_why`.

## Reachability, and why imrun is not the check here

`imrun` reports no `unknown global` trap on any shipped container, and on its
own that proves less than it looks: a script stops at its *first* unimplemented
entry point, so a global further down is never evaluated. The census below does
not depend on execution reaching anything.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

from conftest import requires_game
from imperivm.formats import vs_parse
from imperivm.formats.bfhp import BlockFile
from imperivm.formats.map import ObjectList
from imperivm.formats.pak import PackFile

import corpus

pytestmark = requires_game


#: `.vs` files in the packs, and inside the shipped containers.
PACK_SCRIPT_COUNT = 577
CONTAINER_SCRIPT_COUNT = 308

#: Distinct bare identifiers, and call sites, over the container half.
CONTAINER_BARE_NAMES = 627
CONTAINER_BARE_SITES = 3832

#: The pack half's bare identifiers -- `docs/formats/vs-host-api.md`'s "245
#: global constants", which `sim/globals.hpp` tallies as 229 resolving, 13 host
#: functions and 3 refused.
PACK_BARE_NAMES = 245

#: Bare identifiers the containers use and no pack script does.
CONTAINER_ONLY = 611
#: ... split by whether they are `c<class id>` constants.
CONTAINER_ONLY_CLASS_CONSTANTS = 11
CONTAINER_ONLY_OTHER = 600

#: The resolution ladder, in `resolve_global`'s own order, as a name census.
#:
#: Pinned as a whole map rather than as a total. A total absorbs one step that
#: has stopped working while another over-fires; the map does not. `ambient` and
#: `aifamily` are zero and are listed anyway, because a step that starts
#: answering names it never answered before is exactly as much of a change as a
#: step that stops.
SIEVE_NAMES = {
    "free0": 4,
    "ambient": 0,
    "engine": 5,
    "aivar": 6,
    "aifamily": 0,
    "class": 16,
    "alias": 206,
    "group": 390,
    "residue": 0,
}

#: The same ladder counted in call sites.
SIEVE_SITES = {
    "free0": 48,
    "ambient": 0,
    "engine": 28,
    "aivar": 18,
    "aifamily": 0,
    "class": 620,
    "alias": 1191,
    "group": 1927,
    "residue": 0,
}

#: The six ambient command names. They are host functions rather than globals
#: (`test_globals.cpp` has the registration addresses) and no container script
#: mentions one, which is itself worth pinning: they are `DATA\SUBAI` vocabulary.
AMBIENT_COMMAND_NAMES = frozenset(
    {"cmdparam", "cmdcost_gold", "cmdcost_food", "cmdcost_pop", "cmdcost_stamina", "cmdwaiting"}
)

#: `gbr.exe`'s console-selection family, a contiguous block of registration name
#: strings at `0x3dec24`..`0x3dec5f`, sitting between `Desync` and `DumpStats`:
#:
#:     Desync \0\0 selg \0\0\0\0 selsq \0\0\0 sels \0\0\0\0 selb \0\0\0\0
#:     selh \0\0\0\0 selu \0\0\0\0 selo \0\0\0\0 sel \0 DumpStats
#:
#: group / squad / settlement / building / hero / **unit** / object / generic.
#: Unlike `DumpStats` (`"void, Obj unit"`) and `Dlg` (`"void, str INI_name"`),
#: none of the eight carries a signature string, which is the shape of a
#: zero-argument accessor. None is in any data table this engine reads.
CONSOLE_SELECTION_NAMES = ("sel", "selb", "selg", "selh", "selo", "sels", "selsq", "selu")


# ---------------------------------------------------------------------------
# the engine's own tables, read from the built engine
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def imcheck() -> Path:
    """The newest built `imcheck`, and only if it is newer than the engine.

    Every table this module sieves against comes out of this binary, so a stale
    one would answer for a build that is not this tree -- which is exactly what
    `corpus.find_tool` exists to stop. `IMPERIVM_IMCHECK` overrides.
    """
    path, complaint = corpus.find_tool("imcheck", "IMPERIVM_IMCHECK")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


def run_imcheck(imcheck: Path, subcommand: str) -> list[str]:
    result = subprocess.run([str(imcheck), subcommand], capture_output=True, text=True, timeout=120)
    assert result.returncode == 0, f"imcheck {subcommand}:\n{result.stderr}"
    return [line for line in result.stdout.splitlines() if line and not line.startswith("#")]


@pytest.fixture(scope="module")
def engine_constants(imcheck: Path) -> frozenset[str]:
    """The 109 constants `gbr.exe` registers, as the engine holds them."""
    return frozenset(line.split()[0] for line in run_imcheck(imcheck, "globals"))


@pytest.fixture(scope="module")
def zero_arity_free_functions(imcheck: Path) -> frozenset[str]:
    """Free entry points of arity 0.

    These never reach `resolve_global` at all: the compiler resolves a bare name
    local -> zero-argument free function -> global, so a name declared at arity
    zero compiles to a *call*. `GetDifficulty` is the visible case -- `imrun`
    reports `host entry point GetDifficulty/0 is declared but not implemented`
    on a container whose only mention of it is bare.
    """
    names = set()
    for line in run_imcheck(imcheck, "surface"):
        kind_and_name, arity, _state = line.rsplit(" ", 2)
        kind, name = kind_and_name.split("|", 1)
        if kind == "free" and arity == "0":
            names.add(name)
    return frozenset(names)


# ---------------------------------------------------------------------------
# the shipped data
# ---------------------------------------------------------------------------


@pytest.fixture(scope="module")
def ai_names(packs) -> tuple[frozenset[str], frozenset[str]]:
    """`(AIV_/AIMV_ variables, SS_/GS_/ES_/TS_ family constants)` from every AI.INI."""
    variables: set[str] = set()
    families: set[str] = set()
    for _label, name, data in packs.entries_named("AI.INI"):
        del name
        for raw in data.decode("cp1252").splitlines():
            token = raw.split(";", 1)[0].strip()
            if not token or token.startswith("["):
                continue
            key = token.split("=", 1)[0].strip()
            if key.startswith(("AIV_", "AIMV_")):
                variables.add(key)
            elif key.startswith(("SS_", "GS_", "ES_", "TS_")):
                families.add(key)
    return frozenset(variables), frozenset(families)


def sanitise(class_id: str) -> str:
    """`gbr.exe` `0x0059c060`: `isalpha` on the first byte, `isalnum` on the rest.

    Spelled out over ASCII rather than through `str.isalpha`, which is Unicode
    aware and would classify a byte in the 0x80..0xFF range differently from the
    executable's C runtime.
    """
    out = []
    for index, char in enumerate(class_id):
        ok = char.isascii() and (char.isalpha() if index == 0 else char.isalnum())
        out.append(char if ok else "_")
    return "".join(out)


@pytest.fixture(scope="module")
def class_constants(game_dir: Path) -> frozenset[str]:
    """`c` + the sanitised id of every class `DATA\\CLASSES` declares."""
    pack = PackFile(game_dir / "Packs" / "data.pak")
    names = set()
    for entry in pack.entries:
        if not entry.name.upper().startswith("DATA\\CLASSES"):
            continue
        for match in re.finditer(rb'<class id="([^"]+)"', pack.read(entry.name)):
            names.add("c" + sanitise(match.group(1).decode("cp1252")))
    return frozenset(names)


@pytest.fixture(scope="module")
def pack_bare_names(packs) -> frozenset[str]:
    """Bare identifiers used by the 577 pack scripts."""
    names: set[str] = set()
    count = 0
    for _label, name, data in packs.entries_named(".VS"):
        count += 1
        inventory = vs_parse.Inventory()
        inventory.add_script(vs_parse.parse(data.decode("cp1252"), name))
        names |= set(inventory.bare_names)
    assert count == PACK_SCRIPT_COUNT
    return frozenset(names)


class ContainerScript:
    """One container script, with the group tables of the map it belongs to."""

    def __init__(self, label: str, name: str, map_number: str | None, bare: dict[str, int]):
        self.label = label
        self.name = name
        self.map_number = map_number
        self.bare = bare
        self.aliases: frozenset[str] = frozenset()
        self.groups: frozenset[str] = frozenset()
        #: The union over every map in the container, for the attribution check.
        self.union_aliases: frozenset[str] = frozenset()
        self.union_groups: frozenset[str] = frozenset()

    def __repr__(self) -> str:  # pragma: no cover - diagnostics only
        return f"{self.label}:{self.name}"


@pytest.fixture(scope="module")
def container_scripts(game_dir: Path) -> list[ContainerScript]:
    """The 308 container scripts, each carrying its own map's `<group>` names.

    A script's map is its path: 300 sit at `Maps/<n>/Sequences/`. The conquest's
    eight container-root `Sequences/` scripts belong to no single map and get
    the union over the container, which is the only reading available -- and
    `test_per_map_attribution_loses_nothing` shows the choice costs nothing.
    """
    scripts: list[ContainerScript] = []
    for path in corpus.container_paths(game_dir):
        label = corpus.label(game_dir, path)
        block = BlockFile(path)
        aliases: dict[str, frozenset[str]] = {}
        groups: dict[str, frozenset[str]] = {}
        for entry in block.files():
            match = re.match(r"Maps/(\d+)/", entry.name, re.IGNORECASE)
            if match and entry.name.lower().endswith("map.obj.xml"):
                objects = ObjectList.parse(block.read(entry.name))
                aliases[match.group(1)] = frozenset(
                    g.name for g in objects.groups if g.type == 0
                )
                groups[match.group(1)] = frozenset(g.name for g in objects.groups if g.type == 1)
        union_aliases = frozenset().union(*aliases.values()) if aliases else frozenset()
        union_groups = frozenset().union(*groups.values()) if groups else frozenset()

        for entry in block.files():
            if not entry.name.upper().endswith(".VS"):
                continue
            match = re.match(r"Maps/(\d+)/", entry.name, re.IGNORECASE)
            number = match.group(1) if match else None
            inventory = vs_parse.Inventory()
            inventory.add_script(vs_parse.parse(block.read(entry.name).decode("cp1252"), entry.name))
            script = ContainerScript(label, entry.name, number, dict(inventory.bare_names))
            script.aliases = aliases.get(number, union_aliases) if number else union_aliases
            script.groups = groups.get(number, union_groups) if number else union_groups
            script.union_aliases = union_aliases
            script.union_groups = union_groups
            scripts.append(script)
    return scripts


@pytest.fixture(scope="module")
def sieve(
    container_scripts: list[ContainerScript],
    engine_constants: frozenset[str],
    zero_arity_free_functions: frozenset[str],
    ai_names: tuple[frozenset[str], frozenset[str]],
    class_constants: frozenset[str],
) -> tuple[dict[str, set[str]], dict[str, int]]:
    """Run every bare identifier down `resolve_global`'s ladder.

    One step sits *ahead* of `resolve_global` and belongs here: the compiler
    resolves a bare name as a zero-argument free function before it asks for a
    global, so `free0` is tried first. `ambient` is a subset of `free0` and is
    counted separately only so that a container script starting to use one is
    visible rather than absorbed.
    """
    ai_variables, ai_families = ai_names
    names: dict[str, set[str]] = {step: set() for step in SIEVE_NAMES}
    sites: dict[str, int] = {step: 0 for step in SIEVE_NAMES}
    for script in container_scripts:
        for name, count in script.bare.items():
            if name in AMBIENT_COMMAND_NAMES:
                step = "ambient"
            elif name in zero_arity_free_functions:
                step = "free0"
            elif name in engine_constants:
                step = "engine"
            elif name in ai_variables:
                step = "aivar"
            elif name in ai_families:
                step = "aifamily"
            elif name in class_constants:
                step = "class"
            elif name in script.aliases:
                step = "alias"
            elif name in script.groups:
                step = "group"
            else:
                step = "residue"
            names[step].add(name)
            sites[step] += count
    return names, sites


# ---------------------------------------------------------------------------
# the census
# ---------------------------------------------------------------------------


def test_both_halves_of_the_script_corpus_are_found(container_scripts, pack_bare_names):
    assert len(container_scripts) == CONTAINER_SCRIPT_COUNT
    assert len(pack_bare_names) == PACK_BARE_NAMES
    # All 308 live in 14 of the 23 containers: the twelve Great Battles and
    # Great Challenges, the tutorial and the conquest. The three scenarios, the
    # five `Packs` templates and `currentadv.bfhp` carry no script at all, which
    # is why a scenario has no mission narration and ends only by its victory
    # condition.
    assert len({script.label for script in container_scripts}) == 14


def test_the_container_half_uses_627_bare_identifiers(container_scripts):
    distinct: dict[str, int] = {}
    for script in container_scripts:
        for name, count in script.bare.items():
            distinct[name] = distinct.get(name, 0) + count
    assert len(distinct) == CONTAINER_BARE_NAMES
    assert sum(distinct.values()) == CONTAINER_BARE_SITES


def test_611_counts_container_only_names_not_map_declared_ones(container_scripts, pack_bare_names, sieve):
    """611 is *containers minus packs*, and the older docstring said otherwise.

    `test_corpus_host_surface.py` used to gloss 611 as "bare identifiers that are
    exactly that [a map-declared object rather than an engine entry point]".
    They are not: 596 of the 627 are map-declared. 611 is the count of names the
    containers use and no pack script does, and the arithmetic is exact --
    627 - (245 - 16 shared) - 16 = 611, i.e. 627 minus the sixteen names both
    halves use.
    """
    container = {name for script in container_scripts for name in script.bare}
    shared = container & pack_bare_names
    assert len(container - pack_bare_names) == CONTAINER_ONLY
    assert len(container) - len(shared) == CONTAINER_ONLY

    names, _sites = sieve
    map_declared = names["alias"] | names["group"]
    assert len(map_declared) == 596
    assert len(map_declared) < CONTAINER_ONLY

    # And the 600/11: of the 611, eleven are `c<class id>` constants.
    container_only = container - pack_bare_names
    assert len(container_only & names["class"]) == CONTAINER_ONLY_CLASS_CONSTANTS
    assert len(container_only - names["class"]) == CONTAINER_ONLY_OTHER
    assert CONTAINER_ONLY_CLASS_CONSTANTS + CONTAINER_ONLY_OTHER == CONTAINER_ONLY


def test_the_resolution_ladder_is_exactly_this(sieve):
    names, sites = sieve
    assert {step: len(value) for step, value in names.items()} == SIEVE_NAMES
    assert sites == SIEVE_SITES


def test_the_two_new_steps_carry_most_of_the_corpus(sieve):
    """`c<class id>` and the map's `<group>` names, which is what landed last.

    612 of the 627 names and 3,738 of the 3,832 sites. Before them
    `resolve_global` answered the other fifteen names and refused the rest,
    which is why roughly half the mission sequences died on an unknown `NO_*` or
    `Q_*` before reaching a host call.
    """
    names, sites = sieve
    new = names["class"] | names["alias"] | names["group"]
    assert len(new) == 612
    assert sites["class"] + sites["alias"] + sites["group"] == 3738


def test_the_sanitiser_path_is_not_exercised_by_the_corpus(sieve, class_constants, game_dir):
    """Every class constant the containers read is a direct id.

    The `0x0059c060` substitution -- `Rock Large 01` becoming `cRock_Large_01` --
    is reachable and is implemented, and *no shipped script uses it*. That is
    the reason `engine/tests/test_globals.cpp` carries the sanitiser cases: this
    corpus cannot hold the rule in place, so a synthetic case has to.
    """
    pack = PackFile(game_dir / "Packs" / "data.pak")
    direct = set()
    for entry in pack.entries:
        if not entry.name.upper().startswith("DATA\\CLASSES"):
            continue
        for match in re.finditer(rb'<class id="([^"]+)"', pack.read(entry.name)):
            direct.add("c" + match.group(1).decode("cp1252"))
    names, _sites = sieve
    assert names["class"] <= direct
    # The substitution really does produce constants no class id spells, so the
    # assertion above is not vacuous.
    assert class_constants - direct


def test_the_residue_is_empty_and_selu_is_why(sieve, container_scripts):
    """`selu`, twice, in one script -- the last name that resolved nowhere.

    `gbr.exe` carries it as a standalone string at `0x3dec4c`, inside a
    contiguous block of registration names: `Desync`, then
    `selg selsq sels selb selh selu selo sel`, then `DumpStats`. That is the
    console family -- selected group, squad, settlement, building, hero, unit,
    object -- and the script uses it as an object (`selu.IsValid()`,
    `selu.name`), which is what "the currently selected unit" would be. None of
    the eight is in any *data* table this engine reads; `selu` is the one of
    them a shipped map reaches for, and `sim/orders.cpp` binds it as the
    zero-argument free function 0x006a66b0 is. So it sits in `free0` now,
    and the residue is empty.

    This used to pin `selu` as the residue by name, so that binding it would
    force somebody to delete the assertion deliberately. That happened; the
    assertion now pins the other side, so that unbinding it would too.
    """
    names, sites = sieve
    assert names["residue"] == set()
    assert sites["residue"] == 0
    assert "selu" in names["free0"]
    holders = [
        script for script in container_scripts if "selu" in script.bare
    ]
    assert len(holders) == 1
    assert holders[0].label.endswith("5_Great_Battles_Britain.bfhp")
    assert holders[0].bare["selu"] == 2


def test_the_console_family_is_a_string_block_in_the_executable(game_dir: Path):
    """The evidence for the paragraph above, asserted rather than described."""
    binary = game_dir / "gbr.exe"
    if not binary.is_file():
        pytest.skip("the installation has no gbr.exe")
    data = binary.read_bytes()
    offsets = {}
    for name in CONSOLE_SELECTION_NAMES:
        needle = b"\x00" + name.encode("ascii") + b"\x00"
        found = [m.start() + 1 for m in re.finditer(re.escape(needle), data)]
        assert len(found) == 1, name
        offsets[name] = found[0]
    # One contiguous block, `Desync` before it and `DumpStats` after.
    assert max(offsets.values()) - min(offsets.values()) < 0x40
    window = data[min(offsets.values()) - 0x10 : max(offsets.values()) + 0x20]
    assert b"Desync" in window
    assert b"DumpStats" in window


def test_per_map_attribution_loses_nothing(container_scripts):
    """No site resolves only under the container union.

    The engine keeps its `<group>` tables per map, so a script that could only
    be satisfied by another map's names would be a script the engine cannot run.
    Guarded against vacuity: the conquest declares far more names across its
    seven maps than any one of them holds, so the union really is larger.
    """
    only_under_union = []
    for script in container_scripts:
        for name in script.bare:
            if name in script.aliases or name in script.groups:
                continue
            if name in script.union_aliases or name in script.union_groups:
                only_under_union.append((script.label, script.name, name))
    assert only_under_union == []

    conquest = [s for s in container_scripts if s.label.lower().endswith("mediterranean.bfhp")]
    assert conquest
    union = conquest[0].union_aliases | conquest[0].union_groups
    # Compared against the scripts that *have* a map, not against the conquest's
    # eight container-root scripts -- those are given the union by definition, so
    # including them would make the guard compare 192 with itself.
    per_map = max(
        len(s.aliases | s.groups) for s in conquest if s.map_number is not None
    )
    assert len(union) == 192
    assert len(union) > per_map


# ---------------------------------------------------------------------------
# the control
# ---------------------------------------------------------------------------
#
# Every classifier above answers "yes" for hundreds of names, and a classifier
# that answered yes for everything would satisfy all of it. These say what it
# says no to.


def test_map_names_and_engine_globals_are_disjoint_vocabularies(
    container_scripts, pack_bare_names, engine_constants, class_constants
):
    """Not one of the pack half's own bare identifiers is a shipped `<group>` name.

    Exact, not thresholded. If the two vocabularies overlapped, the ladder's
    order would be observable on retail data and every count above would depend
    on it; they do not overlap, which is why `sim/globals.hpp` can record the
    order as unrelied-upon.
    """
    every_group_name: set[str] = set()
    for script in container_scripts:
        every_group_name |= script.union_aliases | script.union_groups
    assert every_group_name

    pack_only = pack_bare_names - {n for s in container_scripts for n in s.bare}
    assert len(pack_only) == PACK_BARE_NAMES - 16
    assert pack_only & every_group_name == set()
    # And the same in the other direction for the two compiled-in tables.
    assert every_group_name & engine_constants == set()
    assert every_group_name & class_constants == set()


def test_the_console_family_resolves_nowhere(
    container_scripts, engine_constants, class_constants, ai_names
):
    """The control for `selu`: none of the eight is a global of any kind.

    If `selu` were a misspelt map name the family around it would not share its
    fate, and if the classifier were too permissive at least one of eight short
    lowercase identifiers would land somewhere. `selu` itself resolves -- as a
    host function, through the registry, which is not any of the tables below.
    """
    ai_variables, ai_families = ai_names
    every_group_name: set[str] = set()
    for script in container_scripts:
        every_group_name |= script.union_aliases | script.union_groups
    for name in CONSOLE_SELECTION_NAMES:
        assert name not in every_group_name, name
        assert name not in engine_constants, name
        assert name not in class_constants, name
        assert name not in ai_variables, name
        assert name not in ai_families, name


def test_a_mutated_map_name_almost_never_resolves(container_scripts):
    """Perturb the names that do resolve and require nearly all of them to stop.

    The classifier says yes to 596 names. This asks what it says to 596 names
    that are one character away from those, and the answer has to be "almost
    nothing" -- the survivors are real collisions (`NO_Hero2` mutating into
    `NO_Hero3`), not false positives, which is why the bound is small rather
    than zero.
    """
    every_group_name: set[str] = set()
    resolving: set[str] = set()
    for script in container_scripts:
        every_group_name |= script.union_aliases | script.union_groups
        for name in script.bare:
            if name in script.aliases or name in script.groups:
                resolving.add(name)
    assert len(resolving) == 596

    # legal-ok: the Latin alphabet twice and the digits. It occurs in the
    # installation because it occurs in everything.
    alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"
    hits = 0
    trials = 0
    for name in sorted(resolving):
        for index in range(min(len(name), 5)):
            # Deterministic: the replacement is fixed by the character it
            # replaces, so this test cannot pass on one run and fail on another.
            replacement = alphabet[(alphabet.index(name[index]) + 7) % len(alphabet)] \
                if name[index] in alphabet else "_"
            mutant = name[:index] + replacement + name[index + 1 :]
            if mutant == name:
                continue
            trials += 1
            if mutant in every_group_name:
                hits += 1
    assert trials > 2000
    assert hits < trials // 100, f"{hits} of {trials} mutants resolved"
