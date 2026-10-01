"""Round-trip tests for the `.bfhp` container reader and writer.

These build their own containers, so they run without the game installed. The
corpus-wide check -- that all 24 containers in a retail install rebuild byte for
byte identically -- is recorded in docs/tools/imfs.md and cannot live here,
because game data must never enter this repository.
"""

from __future__ import annotations

import pytest

from imperivm.formats.bfhp import (
    BlockFile,
    BlockFileError,
    build,
    build_plan,
)

#: 64,512 bytes: the largest payload a 512-byte-block node holds directly.
DIRECT_LIMIT = (512 // 4 - 2) * 512


def container(tree, block_size=512) -> BlockFile:
    parsed = BlockFile.from_bytes(build(tree, block_size))
    parsed.validate()
    return parsed


def test_empty_container_matches_an_unwritten_save_slot():
    parsed = container([], 4096)
    assert parsed.block_count == 2
    assert parsed.entries == []
    assert len(parsed.raw) == 8192


def test_tree_survives_a_round_trip():
    tree = [
        ("Conversations", []),
        ("game.xml", b"<game/>"),
        ("Maps", [("1", [("map.xml", b"<map/>"), ("empty.bin", b"")])]),
    ]
    parsed = container(tree)
    assert [(e.name, e.is_dir, e.size) for e in parsed.entries] == [
        ("Conversations", True, 0),
        ("game.xml", False, 7),
        ("Maps", True, 0),
        ("Maps/1", True, 0),
        ("Maps/1/map.xml", False, 6),
        ("Maps/1/empty.bin", False, 0),
    ]
    assert parsed.read("maps/1/MAP.XML") == b"<map/>"


def test_indirection_switches_over_at_the_documented_size():
    direct = container([("f", b"a" * DIRECT_LIMIT)]).entries[0]
    assert (direct.level, len(direct.blocks)) == (0, 126)

    indirect = container([("f", b"a" * (DIRECT_LIMIT + 1))]).entries[0]
    assert (indirect.level, len(indirect.blocks)) == (1, 127)


def test_a_multi_index_file_reads_back_intact():
    payload = bytes(range(256)) * 1200  # 307,200 bytes, five index blocks
    parsed = container([("big.bin", payload)])
    assert parsed.entries[0].level == 1
    assert parsed.read("big.bin") == payload


def test_every_block_is_claimed_exactly_once():
    parsed = container(
        [("a.bin", b"x" * 200_000), ("d", [("b.bin", b"y" * 3000)]), ("c.bin", b"z")]
    )
    assert parsed.unreferenced_blocks() == []
    assert len(parsed.block_map()) == parsed.block_count


def test_the_file_stops_at_the_last_byte_used():
    parsed = container([("f.bin", b"x" * 700)])
    # 700 bytes spill 188 bytes into a second data block, and the file ends
    # there rather than at a block boundary.
    assert len(parsed.raw) == (parsed.block_count - 1) * 512 + 188


def test_a_plan_rebuilds_the_container_it_came_from():
    tree = [
        ("Local", [("Italian", [("notes.xml", b"n")])]),
        ("Maps", [("1", [("Terrain.pass.grid", b"g" * 70_000)])]),
        ("game.xml", b"<game/>"),
    ]
    original = build(tree)
    parsed = BlockFile.from_bytes(original)
    assert build_plan(parsed.to_plan(), parsed.block_size) == original
    assert build(parsed.to_tree(), parsed.block_size) == original


def test_creation_order_is_independent_of_record_order():
    # The retail containers that ship recorded speech create every text file
    # first and come back for the .wav files, so a plan is not always a
    # depth-first walk. Both orders must produce the same readable tree.
    depth_first = build_plan(
        [("d", None), ("d/a.txt", b"a"), ("e", None), ("e/b.txt", b"b")]
    )
    interleaved = build_plan(
        [("d", None), ("e", None), ("d/a.txt", b"a"), ("e/b.txt", b"b")]
    )
    assert depth_first != interleaved
    for raw in (depth_first, interleaved):
        parsed = BlockFile.from_bytes(raw)
        parsed.validate()
        assert parsed.read("d/a.txt") == b"a"
        assert parsed.read("e/b.txt") == b"b"


@pytest.mark.parametrize(
    "tree",
    [
        [("a", b""), ("A", b"")],  # the engine resolves names case-insensitively
        [("a/b", b"")],  # separators belong in the path, not the name
        [("", b"")],
    ],
)
def test_unusable_names_are_refused(tree):
    with pytest.raises(BlockFileError):
        build(tree)


def test_a_missing_parent_directory_is_refused():
    with pytest.raises(BlockFileError):
        build_plan([("Maps/1/map.xml", b"<map/>")])


def test_block_size_must_be_a_power_of_two():
    with pytest.raises(BlockFileError):
        build([], 1000)
