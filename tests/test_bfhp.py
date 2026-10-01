"""Data-free tests for the block container reader. Specification: docs/formats/bfhp.md."""

from __future__ import annotations

import struct

import pytest

import synthetic
from imperivm.formats.bfhp import BlockFile, BlockFileError

TREE = {
    "game.xml": b"<game/>",
    "map": {
        "terrain.grid": bytes(range(256)) * 3,
        "objects": {"placement.bin": b"objects" * 40},
    },
    "empty.dat": b"",
}


def container(tmp_path, data: bytes, name: str = "map.bfhp"):
    path = tmp_path / name
    path.write_bytes(data)
    return path


def build(tmp_path, tree=None, block_size: int = 64):
    if tree is None:
        tree = TREE
    return container(tmp_path, synthetic.build_simple_bfhp(tree, block_size=block_size))


# -- structure -----------------------------------------------------------


def test_walks_the_tree_and_reads_every_file(tmp_path):
    fs = BlockFile(build(tmp_path))
    fs.validate()
    names = {e.name for e in fs.entries}
    assert names == {
        "game.xml",
        "map",
        "map/terrain.grid",
        "map/objects",
        "map/objects/placement.bin",
        "empty.dat",
    }
    assert fs.read("game.xml") == b"<game/>"
    assert fs.read("map/terrain.grid") == bytes(range(256)) * 3
    assert fs.read("map/objects/placement.bin") == b"objects" * 40
    assert fs.read("empty.dat") == b""


def test_header_fields(tmp_path):
    fs = BlockFile(build(tmp_path))
    assert fs.block_size == 64
    assert fs.root_node == 1
    assert fs.free_list_head == -1
    assert fs.reserved == (0, 0)
    assert fs.unknown_field == 8
    assert fs.pointers_per_block == 16


def test_directories_are_listed_but_carry_no_payload_size(tmp_path):
    fs = BlockFile(build(tmp_path))
    directories = {e.name: e for e in fs.entries if e.is_dir}
    assert set(directories) == {"map", "map/objects"}
    assert all(e.size == 0 for e in directories.values())
    assert {e.name for e in fs.files()} == {
        "game.xml",
        "map/terrain.grid",
        "map/objects/placement.bin",
        "empty.dat",
    }
    assert len(fs) == 6


def test_lookup_is_case_and_separator_insensitive(tmp_path):
    fs = BlockFile(build(tmp_path))
    assert fs.read("MAP\\TERRAIN.GRID") == fs.read("map/terrain.grid")
    assert "Map/Objects" in fs
    with pytest.raises(IsADirectoryError):
        fs.read("map")
    with pytest.raises(KeyError):
        fs.read("nope.txt")


def test_block_walk_claims_every_block_exactly_once(tmp_path):
    """The core invariant: no block is orphaned and none is claimed twice."""
    fs = BlockFile(build(tmp_path))
    fs.validate()
    claimed = fs.block_map()  # raises if any block is claimed twice
    assert set(claimed) == set(range(fs.block_count)), "a block was left unclaimed"
    assert fs.unreferenced_blocks() == []
    assert claimed[0][0] == "header"
    roles = {role for role, _ in claimed.values()}
    assert roles == {"header", "node", "data"}


def test_the_final_block_may_be_short(tmp_path):
    """The writer truncates at the last used byte rather than padding."""
    path = build(tmp_path)
    fs = BlockFile(path)
    physical = path.stat().st_size
    span = fs.block_size * fs.block_count
    assert span - fs.block_size < physical <= span
    fs.validate()


def test_level_1_indirection(tmp_path):
    """A payload past the direct limit moves to one level of index blocks."""
    direct_limit = (64 // 4 - 2) * 64  # 896 bytes with 64-byte blocks
    big = bytes((i * 7) % 256 for i in range(direct_limit * 3))
    fs = BlockFile(build(tmp_path, {"big.bin": big, "small.bin": b"x" * 100}))
    fs.validate()
    assert fs.read("big.bin") == big
    assert fs.read("small.bin") == b"x" * 100

    entry = {e.name: e for e in fs.entries}["big.bin"]
    assert entry.size == len(big)
    needed = -(-len(big) // fs.block_size)
    assert len(entry.blocks) == needed
    # More data blocks than a node's own word array can hold, so the writer
    # must have moved to index blocks.
    assert needed > fs.pointers_per_block - 2
    assert "index" in {role for role, _ in fs.block_map().values()}


def test_level_1_stale_tails_are_ignored(tmp_path):
    """Never scan for a zero terminator: level-1 tails hold stale block lists."""
    direct_limit = (64 // 4 - 2) * 64
    big = bytes(range(256)) * 8
    assert len(big) > direct_limit
    raw = synthetic.build_simple_bfhp({"big.bin": big})
    assert b"\xcd\xcd\xcd\xcd" in raw, "the fixture must leave a stale level-1 tail"
    fs = BlockFile(container(tmp_path, raw))
    fs.validate()
    assert fs.read("big.bin") == big


def test_a_larger_block_size_is_supported(tmp_path):
    """`currentadv.bfhp` uses 4096; a reader must not assume 512."""
    fs = BlockFile(build(tmp_path, {"a.txt": b"a" * 5000}, block_size=512))
    fs.validate()
    assert fs.block_size == 512
    assert fs.read("a.txt") == b"a" * 5000


def test_an_empty_container(tmp_path):
    fs = BlockFile(build(tmp_path, {}))
    fs.validate()
    assert fs.entries == []
    assert fs.files() == []


# -- error paths ---------------------------------------------------------


def test_rejects_bad_magic(tmp_path):
    data = bytearray(synthetic.build_simple_bfhp(TREE))
    data[0:4] = b"SFPH"
    with pytest.raises(BlockFileError, match="magic mismatch"):
        BlockFile(container(tmp_path, bytes(data)))


def test_rejects_a_file_shorter_than_a_header(tmp_path):
    with pytest.raises(BlockFileError, match="shorter than a header"):
        BlockFile(container(tmp_path, b"HPFS" + bytes(8)))


def test_rejects_a_block_size_that_is_not_a_power_of_two(tmp_path):
    data = bytearray(synthetic.build_simple_bfhp(TREE))
    struct.pack_into("<I", data, 4, 96)
    with pytest.raises(BlockFileError, match="is not a power of two"):
        BlockFile(container(tmp_path, bytes(data)))


def test_rejects_an_unknown_entry_kind(tmp_path):
    builder = synthetic.BfhpBuilder(64)
    node = builder.add_file(b"hello")
    packed = struct.pack("<IHH", node, 7, 4) + b"file"
    builder._write_node(1, packed)
    with pytest.raises(BlockFileError, match="unknown kind"):
        BlockFile(container(tmp_path, builder.finish()))


def test_rejects_an_indirection_level_above_one(tmp_path):
    data = bytearray(synthetic.build_simple_bfhp({"a.bin": b"x" * 200}))
    fs = BlockFile(container(tmp_path, bytes(data), "probe.bfhp"))
    node = {e.name: e.node for e in fs.entries}["a.bin"]
    struct.pack_into("<I", data, node * fs.block_size + 4, 2)
    with pytest.raises(BlockFileError, match="indirection level 2"):
        BlockFile(container(tmp_path, bytes(data), "broken.bfhp"))


def test_rejects_a_block_index_outside_the_array(tmp_path):
    data = bytearray(synthetic.build_simple_bfhp({"a.bin": b"x" * 100}))
    fs = BlockFile(container(tmp_path, bytes(data), "probe.bfhp"))
    node = {e.name: e.node for e in fs.entries}["a.bin"]
    struct.pack_into("<I", data, node * fs.block_size + 8, 9999)
    broken = BlockFile(container(tmp_path, bytes(data), "broken.bfhp"))
    with pytest.raises(BlockFileError, match="out of range|outside the array"):
        broken.read("a.bin")
    with pytest.raises(BlockFileError):
        broken.validate()


def test_validate_rejects_a_root_node_outside_the_array(tmp_path):
    data = bytearray(synthetic.build_simple_bfhp({}))
    struct.pack_into("<I", data, 0x1C, 4096)
    with pytest.raises(BlockFileError):
        BlockFile(container(tmp_path, bytes(data))).validate()


def test_validate_rejects_unreferenced_blocks(tmp_path):
    """Retail containers are written compacted: no free space, no free map."""
    data = bytearray(synthetic.build_simple_bfhp(TREE))
    count = struct.unpack_from("<I", data, 8)[0]
    struct.pack_into("<I", data, 8, count + 2)
    data += b"\0" * (2 * 64)
    with pytest.raises(BlockFileError, match="unreferenced blocks"):
        BlockFile(container(tmp_path, bytes(data))).validate()


def test_validate_rejects_a_block_claimed_twice(tmp_path):
    builder = synthetic.BfhpBuilder(64)
    shared = builder.add_file(b"payload that lives in one block")
    builder._write_node(1, b"".join(
        struct.pack("<IHH", shared, 0, len(name)) + name
        for name in (b"one.txt", b"two.txt")
    ))
    with pytest.raises(BlockFileError, match="claimed by both"):
        BlockFile(container(tmp_path, builder.finish())).validate()


def test_validate_rejects_a_physical_length_that_disagrees_with_the_block_count(tmp_path):
    data = synthetic.build_simple_bfhp(TREE) + b"\0" * 200
    with pytest.raises(BlockFileError, match="does not match"):
        BlockFile(container(tmp_path, data)).validate()
