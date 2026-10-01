"""The diplomacy matrix, counted rather than remembered.

Every container carries sixteen `player<i>.xml` documents, one per slot, and
each holds a `relations` attribute of sixteen eight-digit words: what that
player thinks of every other. The whole install is 4,864 words, and all but 320
of them are zero.

`engine/tests/test_player.cpp` used to assert these numbers against two of the
documents pasted into it as C++ string literals. `docs/legal.md` rule 1 forbids
that -- no game assets in the repository, "not as test fixtures" -- and
`tools/check_fixtures.py` refuses it now. The unit tests kept the *format*
properties, on rows written from `docs/formats/map.md`; the census is here,
where it is recomputed from the installation on every run instead of quoted
from a comment.

That distinction has already cost this project once. The figures below were
originally measured with a scan that sniffed each file for the `HPFS` magic,
which finds **eighteen** containers with relation tables, not nineteen:
`Packs/RandomMapSettlements.bfhp` is an LZIS stream *wrapping* a container, so
its magic is the compressor's. It holds sixteen diagonal entries, and a count
that misses them is out by exactly that. This module decompresses it, and
`test_the_lzis_wrapped_container_is_counted` fails if a future refactor stops.

## What the words mean

Four values occur, and the interesting one occurs once:

* `0x35` -- **self**. Every player's own column, 304 of them, one per document,
  and never anywhere else. That is the strongest single statement available
  that the attribute is a matrix row and not a list of allies.
* `0x11` -- friendly. Nine.
* `0x15` -- allied. Six.
* `0x01` -- ceasefire only. **One**, in `3_Great_Losses_Egypt.bfhp`, and its
  transpose is `0x11`.

## Why the asymmetry matters

Three unordered pairs disagree with their transpose. A reader that mirrored the
matrix -- and the obvious reader does, because a diplomatic relation *sounds*
mutual -- would be wrong about all three and right about the other 2,429, which
is exactly the kind of bug that survives a test suite.
"""

from __future__ import annotations

import collections
import re

import pytest

from conftest import requires_game
from imperivm.formats.bfhp import BlockFile
from imperivm.formats.lzis import decompress

import corpus

pytestmark = requires_game


#: `sim/player.hpp`'s named relation words.
SELF, ALLIED, FRIENDLY, CEASEFIRE_ONLY = 0x35, 0x15, 0x11, 0x01

#: The container whose `HPFS` magic is hidden behind an LZIS wrapper.
WRAPPED = "RandomMapSettlements.bfhp"

PLAYER_DOC = re.compile(r"(^|/)player\d+\.xml$", re.IGNORECASE)


def _containers(game_dir):
    """Every container, including the one whose magic is the compressor's."""
    seen = set()
    for path in corpus.container_paths(game_dir):
        seen.add(path.name)
        yield path.name, BlockFile(path)
    for path in game_dir.rglob("*.bfhp"):
        if path.name in seen:
            continue
        seen.add(path.name)
        try:
            yield path.name, BlockFile.from_bytes(decompress(path.read_bytes()), path.name)
        except Exception:  # not a container after all
            continue


@pytest.fixture(scope="module")
def matrices(game_dir):
    """`{(container, directory): {player id: [16 words]}}`, over the install."""
    out: dict[tuple[str, str], dict[int, list[int]]] = collections.defaultdict(dict)
    for label, block in _containers(game_dir):
        for entry in block.entries:
            if entry.is_dir:
                continue
            name = entry.name.replace("\\", "/")
            if not PLAYER_DOC.search(name):
                continue
            text = block.read(entry.name).decode("cp1252", "replace")
            identifier = re.search(r'id="(\d+)"', text)
            relations = re.search(r'relations="([0-9a-fA-F]*)"', text)
            if not identifier or not relations:
                continue
            digits = relations.group(1)
            words = [int(digits[i : i + 8], 16) for i in range(0, len(digits), 8)]
            directory = name.rsplit("/", 1)[0] if "/" in name else ""
            out[(label, directory)][int(identifier.group(1))] = words
    return dict(out)


@pytest.fixture(scope="module")
def census(matrices) -> collections.Counter:
    counter: collections.Counter = collections.Counter()
    for table in matrices.values():
        for words in table.values():
            counter.update(words)
    return counter


def test_the_lzis_wrapped_container_is_counted(matrices):
    # Sniffing for `HPFS` finds eighteen sets of relations. There are nineteen,
    # and the nineteenth is compressed.
    assert len(matrices) == 19
    assert any(label == WRAPPED for label, _ in matrices), (
        f"{WRAPPED} is missing; its magic is LZIS, not HPFS, and it must be "
        "decompressed before it can be walked"
    )


def test_every_slot_of_every_set_has_a_row(matrices):
    documents = sum(len(table) for table in matrices.values())
    assert documents == 304
    # Sixteen slots per set, whether or not the map has sixteen players.
    for key, table in matrices.items():
        assert sorted(table) == list(range(16)), key
        for identifier, words in table.items():
            assert len(words) == 16, (key, identifier)


def test_only_four_relation_words_ever_occur(census):
    assert sum(census.values()) == 4864
    assert set(census) == {0, CEASEFIRE_ONLY, FRIENDLY, ALLIED, SELF}
    assert census[0] == 4544
    assert census[SELF] == 304
    assert census[FRIENDLY] == 9
    assert census[ALLIED] == 6
    # The one that makes `kRelationCeasefireOnly` worth having a name.
    assert census[CEASEFIRE_ONLY] == 1


def test_the_self_word_is_the_diagonal_and_nothing_else(matrices):
    # 304 of them, exactly one per document, never off the diagonal. This is
    # what says the attribute is a matrix row rather than a list.
    for key, table in matrices.items():
        for identifier, words in table.items():
            assert words[identifier] == SELF, (key, identifier)
            assert words.count(SELF) == 1, (key, identifier)


def test_relations_are_sparse_and_off_diagonal_entries_are_rare(matrices):
    off_diagonal = 0
    sets_with_any = set()
    for key, table in matrices.items():
        for identifier, words in table.items():
            for other, word in enumerate(words):
                if word and other != identifier:
                    off_diagonal += 1
                    sets_with_any.add(key)
    assert off_diagonal == 16
    # Three maps in the whole install record any diplomacy at all.
    assert len(sets_with_any) == 3


def test_three_pairs_disagree_with_their_transpose(matrices):
    asymmetric = set()
    for key, table in matrices.items():
        for identifier, words in table.items():
            for other, word in enumerate(words):
                mirror = table.get(other)
                if mirror is not None and mirror[identifier] != word:
                    asymmetric.add((key, min(identifier, other), max(identifier, other)))
    assert len(asymmetric) == 3

    # And the one `0x01` is on one of them, against a `0x11`.
    found = False
    for key, low, high in asymmetric:
        words = matrices[key]
        if CEASEFIRE_ONLY in (words[low][high], words[high][low]):
            other = words[high][low] if words[low][high] == CEASEFIRE_ONLY else words[low][high]
            assert other == FRIENDLY
            found = True
    assert found, "the install's single 0x01 should sit opposite a 0x11"
