"""Corpus tests for the `Env*` path vocabulary. Specification: `engine/core/include/imperivm/core/sim/env.hpp`.

The environment store is the largest keyed surface in the game, and the shape of
its keys is decided by two facts that no single call site shows:

1. **Every `Env*` name is registered at two arities**, and the shorter one drops
   the scope argument. The 577 `.vs` files in `data.pak` never use the shorter
   form; the 308 inside the 24 `.bfhp` containers use it 403 times. A survey of
   the packs alone concludes the root scope is dead. It is not.
2. **Nothing normalises the path.** `gbr.exe`'s `MakePath` (0x006ab0e0) returns
   the key unchanged when it starts with `/` and otherwise prepends the running
   script thread's `Context` string. A leading slash is therefore *significant*,
   and `"/Bonus"` and `"Bonus"` are two different slots.

Fact 2 is easy to get backwards, because two shipped scripts spell one key both
ways and look like evidence for normalisation. They are not: they are typos, and
the tabulation below is what says so — 75 of 77 distinct root keys use one
spelling consistently, and the two that do not mix *one-sidedly*. That is the
number this module pins, so the reading cannot drift back.

Counts here are exact on purpose. A change that makes one of them 402 is a
change in what the corpus says, and it should have to be typed out.
"""

from __future__ import annotations

import collections
from pathlib import Path

import pytest

from conftest import requires_game
from corpus import container_paths, pack_paths
from imperivm.formats import vs_parse
from imperivm.formats.bfhp import BlockFile
from imperivm.formats.pak import PackFile

pytestmark = requires_game

#: Every `.vs` in the installation: 577 in the packs, 308 in the containers.
SCRIPT_COUNT = 885
PACK_SCRIPT_COUNT = 577

#: `EnvRead*` / `EnvWrite*` call sites, by name.
CALLS_BY_NAME = {
    "EnvReadInt": 474,
    "EnvWriteInt": 358,
    "EnvReadString": 152,
    "EnvWriteString": 67,
    "EnvReadObj": 8,
    "EnvWriteObj": 6,
}
TOTAL_CALLS = 1065

#: The arity at which each name means "root scope" -- one argument fewer.
ROOT_ARITY = {
    "EnvReadInt": 1,
    "EnvReadString": 1,
    "EnvReadObj": 1,
    "EnvWriteInt": 2,
    "EnvWriteString": 2,
    "EnvWriteObj": 2,
}

ROOT_CALLS = 403
SCOPED_CALLS = 662

#: Root sites by name and arity. `EnvReadObj/1` and `EnvWriteObj/2` are declared
#: by `gbr.exe` and called by nothing, which is why they are absent.
ROOT_CALLS_BY_FORM = {
    ("EnvReadInt", 1): 191,
    ("EnvWriteInt", 2): 164,
    ("EnvReadString", 1): 34,
    ("EnvWriteString", 2): 14,
}

#: Root sites whose key is a string literal, and how those literals are spelled.
ROOT_LITERAL_SITES = 356
ROOT_ABSOLUTE_SITES = 343
ROOT_RELATIVE_SITES = 13
ROOT_COMPUTED_KEY_SITES = 47  # `"/En_DownArea" + i` and friends
DISTINCT_ROOT_LITERALS = 77
#: ...which fold to 75 keys once the leading slash is set aside: exactly two
#: literals collapse onto another, and those two are the whole of the evidence
#: that ever looked like a normalisation rule.
DISTINCT_ROOT_KEYS = 75

#: The only two keys in the whole installation that appear in both spellings,
#: and the exact call sites that make them do so. Both are one-sided: `Bonus` is
#: only ever *written* absolute and only ever *read* relative, and
#: `En_Direction` has one relative write against an absolute write and three
#: absolute reads. See env.hpp for what this costs the shipped conquest.
MIXED_SPELLING_KEYS = {
    "Bonus": {
        "absolute": {"reads": 7, "writes": 1},
        "relative": {"reads": 3, "writes": 0},
        "container": "Conquests/mediterranean.BFHP",
    },
    "En_Direction": {
        "absolute": {"reads": 3, "writes": 1},
        "relative": {"reads": 0, "writes": 1},
        "container": "Adventures/GreatChallenges/3_Great_Losses_Egypt.bfhp",
    },
}


def _sources(game_dir: Path):
    """Every `.vs` in the installation, as `(container_label, entry, text)`."""
    for path in pack_paths(game_dir):
        pack = PackFile(path)
        label = path.relative_to(game_dir).as_posix()
        for entry in pack.entries:
            if entry.name.upper().endswith(".VS"):
                yield label, entry.name, pack.read(entry.name).decode("cp1252")
    for path in container_paths(game_dir):
        block = BlockFile(path)
        label = path.relative_to(game_dir).as_posix()
        for entry in block.entries:
            if entry.name.upper().endswith(".VS"):
                yield label, entry.name, block.read(entry.name).decode("cp1252")


class EnvCall:
    """One `EnvRead*` / `EnvWrite*` call site."""

    __slots__ = ("container", "entry", "line", "name", "argc", "key")

    def __init__(self, container, entry, line, name, argc, key):
        self.container = container
        self.entry = entry
        self.line = line
        self.name = name
        self.argc = argc
        self.key = key  # the literal, or None when the key is computed

    @property
    def is_root(self) -> bool:
        return self.argc == ROOT_ARITY[self.name]

    @property
    def is_read(self) -> bool:
        return "Read" in self.name


@pytest.fixture(scope="module")
def env_calls(game_dir: Path) -> list[EnvCall]:
    calls: list[EnvCall] = []
    files = 0
    for container, entry, text in _sources(game_dir):
        files += 1
        tree = vs_parse.parse(text, f"{container}:{entry}")
        for node in vs_parse.walk(tree):
            if not isinstance(node, vs_parse.Call):
                continue
            callee = node.callee
            if not isinstance(callee, vs_parse.Name) or callee.ident not in ROOT_ARITY:
                continue
            argc = len(node.args)
            key_index = 0 if argc == ROOT_ARITY[callee.ident] else 1
            key = None
            if key_index < argc and isinstance(node.args[key_index], vs_parse.StrLit):
                key = node.args[key_index].value
            calls.append(EnvCall(container, entry, node.line, callee.ident, argc, key))
    assert files == SCRIPT_COUNT
    return calls


def test_every_script_is_scanned(game_dir: Path):
    """The 885/577 split is the whole reason this module exists."""
    packs = sum(
        1
        for path in pack_paths(game_dir)
        for entry in PackFile(path).entries
        if entry.name.upper().endswith(".VS")
    )
    assert packs == PACK_SCRIPT_COUNT
    total = sum(1 for _ in _sources(game_dir))
    assert total == SCRIPT_COUNT


def test_call_sites_by_name(env_calls):
    assert collections.Counter(c.name for c in env_calls) == CALLS_BY_NAME
    assert len(env_calls) == TOTAL_CALLS


def test_the_root_scope_is_used_only_by_the_containers(env_calls):
    """403 root sites, none of them in a pack, all of them in a sequence.

    This is the control on the claim. If the shorter arity were merely an
    alternative spelling the packs also used, its absence there would be a
    coincidence over 662 scoped sites; it is not one.
    """
    root = [c for c in env_calls if c.is_root]
    scoped = [c for c in env_calls if not c.is_root]
    assert len(root) == ROOT_CALLS
    assert len(scoped) == SCOPED_CALLS
    assert collections.Counter((c.name, c.argc) for c in root) == ROOT_CALLS_BY_FORM

    assert [c for c in root if c.container.endswith(".pak")] == []
    assert [c for c in root if "Sequences/" not in c.entry] == []


def test_scoped_keys_never_carry_a_leading_slash(env_calls):
    """The scope's prefix is the engine's to build, never the script's.

    `gbr.exe` composes `/<map>/Player<n>/<key>` itself, so a scoped key that
    began with `/` would produce a doubled separator. None does -- across 588
    string-literal scoped sites, which is the negative control for the root
    scope's 343 leading slashes.
    """
    scoped = [c for c in env_calls if not c.is_root and c.key is not None]
    assert len(scoped) == 588
    assert [c.key for c in scoped if c.key.startswith("/")] == []


def test_root_keys_are_absolute_or_relative_and_that_is_significant(env_calls):
    root = [c for c in env_calls if c.is_root]
    literal = [c for c in root if c.key is not None]
    assert len(literal) == ROOT_LITERAL_SITES
    assert len(root) - len(literal) == ROOT_COMPUTED_KEY_SITES

    absolute = [c for c in literal if c.key.startswith("/")]
    assert len(absolute) == ROOT_ABSOLUTE_SITES
    assert len(literal) - len(absolute) == ROOT_RELATIVE_SITES

    # No key differs from another only by case: whatever the store does about
    # case, the corpus never asks it to matter.
    per_container = collections.defaultdict(lambda: collections.defaultdict(set))
    for call in literal:
        per_container[call.container][call.key.lstrip("/").lower()].add(call.key.lstrip("/"))
    for container, keys in per_container.items():
        for folded, spellings in keys.items():
            assert len(spellings) == 1, f"{container}: {folded} -> {sorted(spellings)}"


def test_only_two_keys_mix_the_two_spellings(env_calls):
    """The evidence against a normalisation rule, counted rather than sampled.

    75 of the 77 distinct root keys use one spelling for every read and every
    write. The two that do not mix one-sidedly -- neither is both written and
    read in both spellings -- which is what a typo looks like and not what a
    convention looks like.
    """
    literal = [c for c in env_calls if c.is_root and c.key is not None]

    spellings = collections.defaultdict(lambda: collections.defaultdict(lambda: {"reads": 0, "writes": 0}))
    where = collections.defaultdict(set)
    for call in literal:
        folded = call.key.lstrip("/")
        form = "absolute" if call.key.startswith("/") else "relative"
        spellings[folded][form]["reads" if call.is_read else "writes"] += 1
        where[folded].add(call.container)

    assert len(spellings) == DISTINCT_ROOT_KEYS
    assert len({c.key for c in literal}) == DISTINCT_ROOT_LITERALS
    mixed = {key: forms for key, forms in spellings.items() if len(forms) == 2}
    assert sorted(mixed) == sorted(MIXED_SPELLING_KEYS)

    for key, expected in MIXED_SPELLING_KEYS.items():
        assert dict(mixed[key]["absolute"]) == expected["absolute"], key
        assert dict(mixed[key]["relative"]) == expected["relative"], key
        assert where[key] == {expected["container"]}, key
        # One-sided in both cases: no key is both written *and* read in both
        # spellings, which is what a deliberate convention would look like.
        assert 0 in (
            mixed[key]["absolute"]["writes"],
            mixed[key]["relative"]["writes"],
        ) or 0 in (mixed[key]["absolute"]["reads"], mixed[key]["relative"]["reads"])

    # The one key that is consistently relative on both sides works exactly as
    # the binary says it should, and is the reason the rule is "prepend a
    # context", not "strip a slash".
    assert dict(spellings["Waves"]) == {"relative": {"reads": 7, "writes": 2}}


def test_the_conquest_bonus_loops_read_a_key_nobody_writes(env_calls):
    """The defect this reading convicts, named so it is not mistaken for ours.

    `Sequences/seq0.vs` writes `/Bonus` and dispatches on it correctly. The
    three bonus sequences that must keep running -- rBritain, rEgypt, rIberia --
    each guard on `EnvReadString("Bonus")`, which is a different slot. In the
    retail engine those three loops never execute.
    """
    calls = [
        c
        for c in env_calls
        if c.container == "Conquests/mediterranean.BFHP" and c.is_root
    ]
    writes = {(c.entry, c.key) for c in calls if not c.is_read}
    reads = collections.Counter(c.entry for c in calls if c.is_read and c.key == "Bonus")

    assert writes == {("Sequences/seq0.vs", "/Bonus")}
    assert dict(reads) == {
        "Sequences/seq1.vs": 1,  # rBritain
        "Sequences/seq3.vs": 1,  # rEgypt
        "Sequences/seq6.vs": 1,  # rIberia
    }
