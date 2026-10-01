"""Corpus tests for the block container reader. Specification: docs/formats/bfhp.md.

Containers are found by the `HPFS` magic rather than by extension: the shipped
files are spelled `.BFHP` and `.bfhp` interchangeably, and
`Packs/RandomMapSettlements.bfhp` is not a container at all until it has been
through LZIS (that one is covered in `test_corpus_lzis.py`).
"""

from __future__ import annotations

import pytest

import corpus
from conftest import requires_game
from imperivm.formats.bfhp import BlockFile

pytestmark = requires_game

#: Uncompressed containers a retail installation is known to hold. The reference
#: install used to write this suite has 23 and every one of them verifies; the
#: figure is a floor rather than an equality because some of them are written by
#: the game at runtime (`currentadv.bfhp` is the saved-adventure scratch file)
#: and so their presence depends on whether the install has ever been played.
MIN_CONTAINER_COUNT = 19


@pytest.fixture(scope="module")
def containers(game_dir) -> list[tuple[str, BlockFile]]:
    return [
        (corpus.label(game_dir, path), BlockFile(path))
        for path in corpus.container_paths(game_dir)
    ]


def test_every_container_parses_and_verifies(containers):
    assert len(containers) >= MIN_CONTAINER_COUNT, [label for label, _ in containers]
    for label, container in containers:
        try:
            container.validate()
        except Exception as exc:  # pragma: no cover - would be a real finding
            raise AssertionError(f"{label}: {exc}") from None


def test_the_block_walk_claims_every_block_exactly_once(containers):
    """No gaps and no double-claims: `block_map` raises on the latter itself."""
    for label, container in containers:
        claimed = container.block_map()  # raises if two structures claim one block
        assert set(claimed) == set(range(container.block_count)), label
        assert container.unreferenced_blocks() == [], label


def test_the_file_ends_inside_its_final_block(containers):
    """Containers are truncated at the last used byte, not padded to a block."""
    for label, container in containers:
        span = container.block_size * container.block_count
        assert span - container.block_size < len(container.raw) <= span, label


def test_every_stored_file_reads_back_at_its_declared_size(containers):
    for label, container in containers:
        for entry in container.files():
            assert len(container.read(entry.name)) == entry.size, f"{label}:{entry.name}"
