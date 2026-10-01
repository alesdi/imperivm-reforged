"""Reader and writer for the HMMSYS block container format (`.bfhp`, magic `HPFS`).

Specification: docs/formats/bfhp.md

Reference implementation: correctness and legibility over speed.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"HPFS"
HEADER_FORMAT = "<4sIIiIIII"
HEADER_SIZE = 32

DIR_ENTRY_HEADER = "<IHH"
DIR_ENTRY_HEADER_SIZE = 8

KIND_FILE = 0
KIND_DIRECTORY = 1

#: The constants the original writer puts in the header fields whose meaning is
#: not established. See "What is still unknown" in the specification.
FREE_HEAD_EMPTY = -1
UNKNOWN_FIELD = 8

#: The block index of the root directory node in every retail container.
ROOT_NODE = 1

DEFAULT_BLOCK_SIZE = 512

#: Names are byte strings within cp1252; see the specification.
NAME_ENCODING = "cp1252"


class BlockFileError(Exception):
    """Raised when a file does not conform to the block container format."""


@dataclass(frozen=True)
class BlockFileEntry:
    """One name stored in a container.

    ``node`` is the block index of the entry's node block. ``size`` is the
    payload length in bytes, and is reported as 0 for a directory because a
    directory's payload is bookkeeping rather than content. ``node_size`` is
    what the node block itself declares, so for a directory it is the length of
    the packed entry list.
    """

    name: str
    node: int
    is_dir: bool
    size: int
    blocks: tuple[int, ...]
    level: int = 0
    node_size: int = 0


class BlockFile:
    """A parsed HMMSYS `.bfhp` container.

    The container is a flat array of fixed-size blocks holding a small
    hierarchical filesystem. Block 0 is the header, block 1 is the node block
    of the root directory.

    Entry names use ``/`` as the separator. Lookup via :meth:`read` is
    case-insensitive and accepts either separator.
    """

    def __init__(self, path: str | Path, data: bytes | None = None) -> None:
        self.path = Path(path)
        self._data = self.path.read_bytes() if data is None else bytes(data)

        magic, block_size, block_count, free_head, reserved_a, reserved_b, unknown, root = (
            self._parse_header()
        )
        self.block_size: int = block_size
        self.block_count: int = block_count
        self.free_list_head: int = free_head
        self.reserved: tuple[int, int] = (reserved_a, reserved_b)
        self.unknown_field: int = unknown
        self.root_node: int = root

        self.pointers_per_block: int = block_size // 4
        self.entries: list[BlockFileEntry] = []
        self._walk(self.root_node, "")
        self._index = {self._key(e.name): e for e in self.entries}

    @classmethod
    def from_bytes(cls, data: bytes, name: str = "<bytes>") -> BlockFile:
        """Parse a container held in memory, e.g. one unwrapped from LZIS."""
        return cls(name, data=data)

    @property
    def raw(self) -> bytes:
        """The container's bytes exactly as parsed."""
        return self._data

    # -- parsing ---------------------------------------------------------

    def _parse_header(self) -> tuple[bytes, int, int, int, int, int, int, int]:
        if len(self._data) < HEADER_SIZE:
            raise BlockFileError(f"{self.path}: shorter than a header")
        fields = struct.unpack_from(HEADER_FORMAT, self._data, 0)
        if fields[0] != MAGIC:
            raise BlockFileError(f"{self.path}: not a HPFS container (magic mismatch)")
        block_size = fields[1]
        if block_size < 32 or block_size & (block_size - 1):
            raise BlockFileError(f"{self.path}: block size {block_size} is not a power of two")
        return fields

    def _block(self, index: int) -> bytes:
        """Return one block. The final block of the file may be short."""
        if index < 0 or index >= self.block_count:
            raise BlockFileError(f"{self.path}: block {index} is out of range")
        start = index * self.block_size
        return self._data[start : start + self.block_size]

    def _pointers(self, index: int) -> tuple[int, ...]:
        """Return a block reinterpreted as an array of u32 block indices."""
        block = self._block(index)
        if len(block) < self.block_size:
            block = block.ljust(self.block_size, b"\0")
        return struct.unpack(f"<{self.pointers_per_block}I", block)

    def _node(self, index: int) -> tuple[int, int, tuple[int, ...]]:
        """Return ``(size, level, data_blocks)`` for the node block at ``index``.

        A node block is an array of u32. Word 0 is the payload size in bytes
        and word 1 is the indirection level. At level 0 the remaining words
        are data block indices; at level 1 they are indices of index blocks,
        each of which is a full block of data block indices.

        Only the words actually needed for ``size`` are meaningful. Level 1
        nodes retain stale values in the unused tail of the array.
        """
        words = self._pointers(index)
        size, level = words[0], words[1]
        needed = -(-size // self.block_size)

        if level == 0:
            if 2 + needed > self.pointers_per_block:
                raise BlockFileError(
                    f"{self.path}: node {index} declares {size} bytes, too many for a "
                    f"direct block list"
                )
            blocks = words[2 : 2 + needed]
        elif level == 1:
            index_blocks = -(-needed // self.pointers_per_block)
            if 2 + index_blocks > self.pointers_per_block:
                raise BlockFileError(
                    f"{self.path}: node {index} declares {size} bytes, too many for a "
                    f"single level of indirection"
                )
            collected: list[int] = []
            for slot in range(index_blocks):
                take = min(self.pointers_per_block, needed - len(collected))
                collected.extend(self._pointers(words[2 + slot])[:take])
            blocks = tuple(collected)
        else:
            raise BlockFileError(f"{self.path}: node {index} has indirection level {level}")

        return size, level, tuple(blocks)

    def _node_payload(self, index: int) -> bytes:
        size, _level, blocks = self._node(index)
        return b"".join(self._block(b) for b in blocks)[:size]

    def _walk(self, node: int, prefix: str) -> None:
        payload = self._node_payload(node)
        pos = 0
        while pos + DIR_ENTRY_HEADER_SIZE <= len(payload):
            child, kind, name_len = struct.unpack_from(DIR_ENTRY_HEADER, payload, pos)
            if child == 0:
                break
            pos += DIR_ENTRY_HEADER_SIZE
            name = payload[pos : pos + name_len].decode(NAME_ENCODING)
            pos += name_len
            if kind not in (KIND_FILE, KIND_DIRECTORY):
                raise BlockFileError(
                    f"{self.path}: entry {prefix + name!r} has unknown kind {kind}"
                )
            is_dir = kind == KIND_DIRECTORY
            size, level, blocks = self._node(child)
            self.entries.append(
                BlockFileEntry(
                    prefix + name, child, is_dir, 0 if is_dir else size, blocks, level, size
                )
            )
            if is_dir:
                self._walk(child, prefix + name + "/")

    # -- access ----------------------------------------------------------

    @staticmethod
    def _key(name: str) -> str:
        return name.upper().replace("\\", "/")

    def __len__(self) -> int:
        return len(self.entries)

    def __contains__(self, name: str) -> bool:
        return self._key(name) in self._index

    def files(self) -> list[BlockFileEntry]:
        """Return only the stored files, in directory order."""
        return [e for e in self.entries if not e.is_dir]

    def read(self, name: str) -> bytes:
        """Return the contents of one stored file."""
        try:
            entry = self._index[self._key(name)]
        except KeyError:
            raise KeyError(f"{name!r} is not in {self.path.name}") from None
        if entry.is_dir:
            raise IsADirectoryError(f"{name!r} is a directory in {self.path.name}")
        return b"".join(self._block(b) for b in entry.blocks)[:entry.size]

    def block_map(self) -> dict[int, tuple[str, str]]:
        """Return ``{block: (role, owner)}`` for every block the tree claims.

        ``role`` is one of ``header``, ``node``, ``index`` or ``data``, and
        ``owner`` is the path of the entry the block belongs to (``/`` for the
        root directory). Raises if any block is claimed twice, which would mean
        the block chains overlap.
        """
        owner: dict[int, tuple[str, str]] = {0: ("header", "")}

        def claim(block: int, role: str, by: str) -> None:
            if block in owner:
                raise BlockFileError(
                    f"{self.path}: block {block} claimed by both "
                    f"{owner[block][1]!r} and {by!r}"
                )
            if not 0 <= block < self.block_count:
                raise BlockFileError(
                    f"{self.path}: {by!r} references block {block}, outside the array"
                )
            owner[block] = (role, by)

        def account(node: int, label: str) -> None:
            claim(node, "node", label)
            words = self._pointers(node)
            size, level = words[0], words[1]
            needed = -(-size // self.block_size)
            if level == 1:
                for slot in range(-(-needed // self.pointers_per_block)):
                    claim(words[2 + slot], "index", label)
            for block in self._node(node)[2]:
                claim(block, "data", label)

        account(self.root_node, "/")
        for entry in self.entries:
            account(entry.node, entry.name)
        return owner

    def unreferenced_blocks(self) -> list[int]:
        """Blocks inside the array that the walk from the root never claims."""
        return sorted(set(range(self.block_count)) - self.block_map().keys())

    def validate(self) -> None:
        """Assert the structural invariants described in the specification."""
        if self.root_node >= self.block_count:
            raise BlockFileError(f"{self.path}: root node {self.root_node} is out of range")

        # Every block is owned by exactly one structure, and the whole array
        # is accounted for. The containers are written compacted: there is no
        # free space and therefore no free block map.
        missing = self.unreferenced_blocks()
        if missing:
            raise BlockFileError(
                f"{self.path}: {len(missing)} unreferenced blocks, first at {missing[0]}"
            )

        # The file is truncated at the last used byte of the last block, so it
        # falls inside the final block rather than on a block boundary.
        span = self.block_size * self.block_count
        if not span - self.block_size < len(self._data) <= span:
            raise BlockFileError(
                f"{self.path}: {len(self._data)} bytes does not match "
                f"{self.block_count} blocks of {self.block_size}"
            )

    # -- conversion ------------------------------------------------------

    def to_tree(self) -> BuildTree:
        """Return the whole container as a nested, order-preserving tree.

        The result is what :func:`build` consumes, so ``build(bf.to_tree(),
        bf.block_size)`` reconstructs the container.
        """
        children: dict[str, BuildTree] = {"": []}
        for entry in self.entries:
            parent, _, leaf = entry.name.rpartition("/")
            if entry.is_dir:
                node: BuildTree = []
                children[entry.name] = node
                children[parent].append((leaf, node))
            else:
                children[parent].append((leaf, self.read(entry.name)))
        return children[""]

    def to_plan(self) -> BuildPlan:
        """Return the container as a creation-order plan.

        The order is by node block index, which is the order the original
        writer created the entries: the block allocator is monotonic, so a
        higher node index means a later creation. Rebuilding from this plan
        reproduces the original file byte for byte.
        """
        return [
            (entry.name, None if entry.is_dir else self.read(entry.name))
            for entry in sorted(self.entries, key=lambda e: e.node)
        ]


# -- writing -------------------------------------------------------------
#
# The original writer is reproduced exactly, not merely compatibly, because
# doing so is the strongest available check that the format is understood: a
# container rebuilt from its own contents comes out byte for byte identical to
# the retail file. Three behaviours of the original are load bearing.
#
# 1. Blocks are handed out from one monotonic counter in the order the writer
#    touches them, and nothing is ever freed, so the result is compacted with
#    no free block map -- which is exactly what the retail containers are.
#
# 2. A node block is zero filled when its entry is created, so the unused tail
#    of a level-0 node is zero. When a file outgrows the direct block list the
#    node is promoted in place: only the words that hold index block indices
#    are overwritten, and the rest of the old direct list survives as the stale
#    tail the specification describes.
#
# 3. There is a single index block buffer, shared by every file in the
#    container and never cleared. A partly filled index block therefore keeps
#    whatever the previously flushed index block held in its unused slots --
#    including across file boundaries. Two files in the retail set only make
#    sense under that reading.


#: An ordered directory listing. Each item is ``(name, payload)``; the payload
#: is ``bytes`` for a stored file and a nested list for a subdirectory. Order is
#: significant: it is the order the entries appear in the directory record list.
BuildTree = list[tuple[str, "bytes | BuildTree"]]

#: A flat creation order: ``(path, payload)`` with ``/`` separators, ``None`` for
#: a directory, listed in the order the writer creates the entries. Every path's
#: parent directory must appear before it. This is the form the builder really
#: works in, because creation order is what fixes the block layout, and it is
#: not always a depth-first walk of the tree: the retail containers that ship
#: recorded speech create every text file first and come back for the ``.wav``
#: files afterwards, even though the directory records interleave them.
BuildPlan = list[tuple[str, "bytes | None"]]


def flatten(tree: BuildTree, prefix: str = "") -> BuildPlan:
    """Turn a nested tree into the depth-first creation order."""
    plan: BuildPlan = []
    for name, payload in tree:
        path = prefix + name
        if isinstance(payload, (bytes, bytearray, memoryview)):
            plan.append((path, bytes(payload)))
        else:
            plan.append((path, None))
            plan.extend(flatten(payload, path + "/"))
    return plan


class _NodeWriter:
    """One node block plus the payload hanging off it, written incrementally."""

    def __init__(self, builder: BlockFileBuilder) -> None:
        self._builder = builder
        self.block = builder._allocate()
        self.words = [0] * builder.pointers_per_block
        self.size = 0
        self.level = 0
        self.data_blocks: list[int] = []
        self.index_blocks: list[int] = []
        self._flush()

    def _flush(self) -> None:
        self.words[0] = self.size
        self.words[1] = self.level
        self._builder._put(self.block, struct.pack(f"<{len(self.words)}I", *self.words))

    def _grow(self) -> int:
        """Allocate one more data block and record it in the block map."""
        builder = self._builder
        pointers = builder.pointers_per_block
        direct_capacity = pointers - 2

        block = builder._allocate()
        position = len(self.data_blocks)
        self.data_blocks.append(block)

        if self.level == 0 and position < direct_capacity:
            self.words[2 + position] = block
            self._flush()
            return block

        if self.level == 0:
            # Promotion. The direct list moves into the shared index buffer,
            # the new block joins it, and word 2 -- and only word 2 -- becomes
            # the index block index. Words 3 onward keep the old direct list.
            builder._index_buffer[:direct_capacity] = self.words[2 : 2 + direct_capacity]
            builder._index_buffer[direct_capacity] = block
            self.level = 1
            index_block = builder._allocate()
            self.index_blocks.append(index_block)
            self.words[2] = index_block
            self._flush()
            builder._flush_index(index_block)
            return block

        slot = position // pointers
        if slot >= len(self.index_blocks):
            index_block = builder._allocate()
            self.index_blocks.append(index_block)
            if 2 + slot >= pointers:
                raise BlockFileError(
                    f"payload of {self.size} bytes needs more than one level of indirection"
                )
            self.words[2 + slot] = index_block
            self._flush()
        builder._index_buffer[position % pointers] = block
        builder._flush_index(self.index_blocks[slot])
        return block

    def append(self, payload: bytes) -> None:
        builder = self._builder
        block_size = builder.block_size
        written = 0
        while written < len(payload):
            offset = self.size % block_size
            if offset == 0:
                self._grow()
            take = min(block_size - offset, len(payload) - written)
            builder._put(
                self.data_blocks[self.size // block_size],
                payload[written : written + take],
                offset,
            )
            written += take
            self.size += take
        self._flush()


class BlockFileBuilder:
    """Assembles a container the way the engine's own writer does.

    Feed it a tree with :meth:`add_tree` and take the bytes from :meth:`build`.
    """

    def __init__(self, block_size: int = DEFAULT_BLOCK_SIZE) -> None:
        if block_size < HEADER_SIZE or block_size & (block_size - 1):
            raise BlockFileError(f"block size {block_size} is not a power of two")
        self.block_size = block_size
        self.pointers_per_block = block_size // 4
        self._blocks: list[bytearray] = []
        self._used: list[int] = []
        # One buffer for the whole container, deliberately never cleared.
        self._index_buffer = [0] * self.pointers_per_block
        self._allocate()  # block 0, the header
        self._put(0, b"\0" * HEADER_SIZE)
        self._root = _NodeWriter(self)
        if self._root.block != ROOT_NODE:
            raise BlockFileError("the root node must land in block 1")
        self._directories: dict[str, _NodeWriter] = {"": self._root}
        self._names: set[str] = set()

    # -- block store -----------------------------------------------------

    def _allocate(self) -> int:
        self._blocks.append(bytearray(self.block_size))
        self._used.append(0)
        return len(self._blocks) - 1

    def _put(self, block: int, payload: bytes, offset: int = 0) -> None:
        self._blocks[block][offset : offset + len(payload)] = payload
        self._used[block] = max(self._used[block], offset + len(payload))

    def _flush_index(self, block: int) -> None:
        self._put(block, struct.pack(f"<{self.pointers_per_block}I", *self._index_buffer))

    # -- tree ------------------------------------------------------------

    def add_tree(self, tree: BuildTree) -> None:
        """Populate the container from a nested tree, depth first."""
        self.add_plan(flatten(tree))

    def add_plan(self, plan: BuildPlan) -> None:
        """Populate the container in an explicit creation order."""
        for path, payload in plan:
            self.add(path, payload)

    def add(self, path: str, payload: bytes | None) -> None:
        """Create one entry. ``payload`` of ``None`` creates a directory.

        The three steps below are the writer's, and their order is what fixes
        the block layout: the entry's node block is allocated first, then its
        record is appended to the parent directory (which may grow the parent),
        and only then is the entry's own content written.
        """
        parent_path, _, name = path.rpartition("/")
        try:
            parent = self._directories[parent_path.upper()]
        except KeyError:
            raise BlockFileError(f"{path!r}: no directory {parent_path!r} in the container")
        encoded = self._check_name(name, parent_path, path)

        child = _NodeWriter(self)
        parent.append(
            struct.pack(
                DIR_ENTRY_HEADER,
                child.block,
                KIND_DIRECTORY if payload is None else KIND_FILE,
                len(encoded),
            )
            + encoded
        )
        if payload is None:
            self._directories[path.upper()] = child
        else:
            child.append(bytes(payload))

    def _check_name(self, name: str, parent_path: str, path: str) -> bytes:
        if not name:
            raise BlockFileError(f"{path!r} has an empty name component")
        if "\\" in name or "\0" in name:
            raise BlockFileError(f"entry name {path!r} contains a path separator")
        try:
            encoded = name.encode(NAME_ENCODING)
        except UnicodeEncodeError as exc:
            raise BlockFileError(
                f"entry name {path!r} is not representable in {NAME_ENCODING}"
            ) from exc
        if len(encoded) > 0xFFFF:
            raise BlockFileError(f"entry name {path!r} is too long")
        # Lookup in the engine is case-insensitive, so two names that differ
        # only in case are the same file as far as the game is concerned.
        key = path.upper()
        if key in self._names:
            raise BlockFileError(
                f"{parent_path or '/'!r} has two entries named {name!r} (ignoring case)"
            )
        self._names.add(key)
        return encoded

    # -- output ----------------------------------------------------------

    def build(self) -> bytes:
        """Return the finished container, truncated the way the engine does."""
        count = len(self._blocks)
        struct.pack_into(
            HEADER_FORMAT,
            self._blocks[0],
            0,
            MAGIC,
            self.block_size,
            count,
            FREE_HEAD_EMPTY,
            0,
            0,
            UNKNOWN_FIELD,
            ROOT_NODE,
        )
        # The file stops at the last byte actually written rather than at a
        # block boundary, so the final block on disk is usually short.
        tail = self._used[count - 1]
        return b"".join(bytes(b) for b in self._blocks[: count - 1]) + bytes(
            self._blocks[count - 1][:tail]
        )


def build(tree: BuildTree, block_size: int = DEFAULT_BLOCK_SIZE) -> bytes:
    """Build a container holding ``tree``. See :data:`BuildTree` for the shape."""
    builder = BlockFileBuilder(block_size)
    builder.add_tree(tree)
    return builder.build()


def build_plan(plan: BuildPlan, block_size: int = DEFAULT_BLOCK_SIZE) -> bytes:
    """Build a container in an explicit creation order. See :data:`BuildPlan`."""
    builder = BlockFileBuilder(block_size)
    builder.add_plan(plan)
    return builder.build()


def main() -> None:
    import argparse
    import sys

    parser = argparse.ArgumentParser(description="Inspect a HMMSYS .bfhp container.")
    parser.add_argument("container", type=Path)
    parser.add_argument("--extract", metavar="NAME", help="write one stored file to stdout")
    parser.add_argument("--extract-all", metavar="DIR", type=Path, help="write every file to DIR")
    args = parser.parse_args()

    container = BlockFile(args.container)

    if args.extract:
        sys.stdout.buffer.write(container.read(args.extract))
        return

    if args.extract_all:
        for entry in container.files():
            target = args.extract_all / entry.name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(container.read(entry.name))
        print(f"{len(container.files())} files written to {args.extract_all}")
        return

    container.validate()
    print(
        f"{args.container}: {container.block_count} blocks of {container.block_size}, "
        f"{len(container.files())} files"
    )
    for entry in container.entries:
        size = "" if entry.is_dir else str(entry.size)
        print(f"  {entry.node:>8} {size:>10}  {entry.name}{'/' if entry.is_dir else ''}")


if __name__ == "__main__":
    main()
