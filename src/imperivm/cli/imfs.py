"""``imfs`` — read, extract and write HMMSYS `.bfhp` block containers.

Specification: docs/formats/bfhp.md
Usage: docs/tools/imfs.md

A `.bfhp` container is the game's read/write virtual filesystem: every scenario,
adventure, campaign map and saved game is one. This tool is the whole round
trip — list, extract, edit and build — and it reproduces the original writer's
block layout exactly, so a container rebuilt from its own contents comes out
byte for byte identical to the retail file.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import os
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from imperivm.formats import lzis, pass_mask
from imperivm.formats.bfhp import (
    DEFAULT_BLOCK_SIZE,
    BlockFile,
    BlockFileError,
    BuildPlan,
    build_plan,
)

#: Written at the root of an extraction so that ``create`` can put the tree back
#: in the order the original writer used. Without it the tree is rebuilt in
#: sorted depth-first order, which is still a valid container but not usually
#: the same bytes.
MANIFEST_NAME = ".imfs-manifest.json"

SEPARATORS = re.compile(r"[\\/]+")


class UsageError(Exception):
    """A problem with what the user asked for, reported without a traceback."""


# -- loading -------------------------------------------------------------


@dataclass(frozen=True)
class Container:
    """A parsed container plus how it was stored on disk."""

    path: Path
    block_file: BlockFile
    compressed: bool

    @property
    def stored_size(self) -> int:
        return self.path.stat().st_size

    @property
    def container_size(self) -> int:
        return len(self.block_file.raw)


def load(path: Path) -> Container:
    """Read a container, transparently unwrapping a whole-file LZIS stream."""
    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise UsageError(str(exc)) from None
    compressed = raw[: len(lzis.MAGIC)] == lzis.MAGIC
    if compressed:
        # `Packs/RandomMapSettlements.bfhp` carries the extension but is an
        # LZIS stream wrapping the container.
        raw = lzis.decompress(raw)
    return Container(path, BlockFile(path, data=raw), compressed)


def writable(container: Container) -> None:
    if container.compressed:
        raise UsageError(
            f"{container.path} is an LZIS-compressed container; imfs can decompress "
            f"it but not recompress it, so it cannot be written back"
        )


# -- paths ---------------------------------------------------------------


def split_path(name: str) -> list[str]:
    """Split a container path, rejecting anything that could escape a directory."""
    parts = [p for p in SEPARATORS.split(name) if p]
    if not parts:
        raise UsageError(f"{name!r} is not a usable path")
    for part in parts:
        if part in (".", "..") or part.endswith(":") or "\0" in part:
            raise UsageError(f"{name!r} contains the unusable component {part!r}")
    return parts


def target_of(out: Path, name: str) -> Path:
    """Resolve a stored name under ``out``, refusing to leave that directory."""
    target = out.joinpath(*split_path(name))
    root = os.path.realpath(out)
    resolved = os.path.realpath(target)
    if resolved != root and not resolved.startswith(root + os.sep):
        raise UsageError(f"{name!r} would be written outside {out}")
    return target


def normalise(name: str) -> str:
    """Canonical in-container form of a user-supplied path."""
    return "/".join(split_path(name))


# -- manifest ------------------------------------------------------------


def write_manifest(out: Path, container: Container) -> None:
    manifest = {
        "tool": "imfs",
        "format": "bfhp",
        "container": container.path.name,
        "block_size": container.block_file.block_size,
        "creation_order": [path for path, _ in container.block_file.to_plan()],
    }
    (out / MANIFEST_NAME).write_text(json.dumps(manifest, indent=1) + "\n", "utf-8")


def read_manifest(path: Path) -> tuple[int | None, list[str]]:
    try:
        manifest = json.loads(path.read_text("utf-8"))
    except (OSError, ValueError) as exc:
        raise UsageError(f"{path}: unreadable manifest ({exc})") from None
    order = manifest.get("creation_order") or []
    if not isinstance(order, list) or not all(isinstance(p, str) for p in order):
        raise UsageError(f"{path}: 'creation_order' is not a list of paths")
    block_size = manifest.get("block_size")
    if block_size is not None and not isinstance(block_size, int):
        raise UsageError(f"{path}: 'block_size' is not an integer")
    return block_size, order


# -- reading a directory tree --------------------------------------------


def scan(source: Path) -> BuildPlan:
    """Read a directory tree into a sorted, depth-first creation plan.

    Sorted case-insensitively, which is the order the game's editor writes
    directory records in.
    """
    plan: BuildPlan = []

    def walk(directory: Path, prefix: str) -> None:
        children = sorted(directory.iterdir(), key=lambda p: p.name.upper())
        for child in children:
            if prefix == "" and child.name == MANIFEST_NAME:
                continue
            path = prefix + child.name
            if child.is_symlink():
                print(f"imfs: skipping symlink {child}", file=sys.stderr)
            elif child.is_dir():
                plan.append((path, None))
                walk(child, path + "/")
            elif child.is_file():
                plan.append((path, child.read_bytes()))
            else:
                print(f"imfs: skipping {child} (not a regular file)", file=sys.stderr)

    if not source.is_dir():
        raise UsageError(f"{source} is not a directory")
    walk(source, "")
    return plan


def reorder(plan: BuildPlan, order: list[str]) -> BuildPlan:
    """Put ``plan`` into the creation order a manifest records.

    Entries the manifest does not mention keep their sorted position at the end,
    so a tree that gained files since extraction still builds.
    """
    remaining = {path.upper(): (path, payload) for path, payload in plan}
    result: BuildPlan = []
    for path in order:
        item = remaining.pop(path.upper(), None)
        if item is not None:
            result.append(item)
    for path, payload in plan:
        item = remaining.pop(path.upper(), None)
        if item is not None:
            result.append(item)
    return result


# -- writing -------------------------------------------------------------


def emit(path: Path, data: bytes) -> None:
    """Write a container, leaving the old one in place if anything goes wrong."""
    temporary = path.with_name(path.name + ".imfs-new")
    temporary.write_bytes(data)
    os.replace(temporary, path)


def rebuild(container: Container, plan: BuildPlan) -> bytes:
    return build_plan(plan, container.block_file.block_size)


def insertion_point(plan: BuildPlan, path: str) -> int:
    """Where a new entry goes so that its parent's records stay sorted."""
    parent = path.rpartition("/")[0]
    name = path.rpartition("/")[2].upper()
    prefix = (parent + "/").upper() if parent else ""
    last_under_parent = -1
    for index, (existing, _payload) in enumerate(plan):
        if not existing.upper().startswith(prefix):
            continue
        last_under_parent = index
        if existing.rpartition("/")[0].upper() == parent.upper():
            if existing.rpartition("/")[2].upper() > name:
                return index
    if last_under_parent >= 0:
        return last_under_parent + 1
    return len(plan)


def missing_parents(plan: BuildPlan, path: str) -> list[str]:
    """Directories that ``path`` needs and the plan does not have yet."""
    have = {p.upper() for p, _ in plan}
    parts = path.split("/")[:-1]
    needed = []
    for depth in range(len(parts)):
        parent = "/".join(parts[: depth + 1])
        if parent.upper() not in have:
            needed.append(parent)
    return needed


# -- content summary -----------------------------------------------------

GRID_HEADER = "<4s4I"


@dataclass(frozen=True)
class GridInfo:
    cell_size: int
    bits_per_cell: int
    extent_x: int
    extent_y: int
    consistent: bool

    @property
    def width(self) -> int:
        return self.extent_x // self.cell_size if self.cell_size else 0

    @property
    def height(self) -> int:
        return self.extent_y // self.cell_size if self.cell_size else 0


def grid_info(data: bytes) -> GridInfo | None:
    """Read a `DIRG` grid header, the layer geometry of a map terrain layer."""
    if len(data) < pass_mask.HEADER_SIZE:
        return None
    magic, cell_size, bits, extent_x, extent_y = struct.unpack_from(GRID_HEADER, data, 0)
    if magic != pass_mask.MAGIC or cell_size == 0:
        return None
    width, height = extent_x // cell_size, extent_y // cell_size
    expected = pass_mask.HEADER_SIZE + -(-width * height * bits // 8)
    return GridInfo(cell_size, bits, extent_x, extent_y, expected == len(data))


def attribute(xml: bytes, name: str, default: str = "-") -> str:
    """Pull one XML attribute out by hand.

    The contained XML is machine-written and flat, so a regular expression is
    enough here and avoids pulling a parser into a summary command.
    """
    match = re.search(rf'\b{name}\s*=\s*"([^"]*)"', xml.decode("cp1252", "replace"))
    return match.group(1) if match else default


def map_numbers(block_file: BlockFile) -> list[str]:
    return [
        e.name.split("/", 1)[1]
        for e in block_file.entries
        if e.is_dir and e.name.count("/") == 1 and e.name.upper().startswith("MAPS/")
    ]


def languages(block_file: BlockFile) -> list[str]:
    return [
        e.name.split("/", 1)[1]
        for e in block_file.entries
        if e.is_dir and e.name.count("/") == 1 and e.name.upper().startswith("LOCAL/")
    ]


# -- output helpers ------------------------------------------------------


def thousands(value: int) -> str:
    return f"{value:,}"


def describe_size(entry) -> str:
    return "" if entry.is_dir else thousands(entry.size)


# -- commands ------------------------------------------------------------


def cmd_list(args: argparse.Namespace) -> int:
    container = load(args.container)
    block_file = container.block_file
    files = block_file.files()
    payload = sum(e.size for e in files)
    note = " (LZIS-compressed)" if container.compressed else ""
    print(
        f"{container.path}{note}: {block_file.block_count} blocks of "
        f"{block_file.block_size}, {len(files)} files, "
        f"{len(block_file.entries) - len(files)} directories, "
        f"{thousands(payload)} bytes of payload"
    )
    if args.long:
        print(f"{'node':>8} {'blocks':>7} {'lvl':>3} {'size':>12}  name")
    for entry in block_file.entries:
        depth = entry.name.count("/")
        leaf = entry.name.rsplit("/", 1)[-1] + ("/" if entry.is_dir else "")
        name = "  " * depth + leaf
        if args.long:
            print(
                f"{entry.node:>8} {len(entry.blocks):>7} {entry.level:>3} "
                f"{describe_size(entry):>12}  {name}"
            )
        else:
            print(f"{describe_size(entry):>12}  {name}")
    return 0


def cmd_extract(args: argparse.Namespace) -> int:
    container = load(args.container)
    block_file = container.block_file
    out = args.out or Path(container.path.stem)
    wanted = args.patterns

    def selected(name: str) -> bool:
        if not wanted:
            return True
        upper = name.upper()
        return any(
            fnmatch.fnmatchcase(upper, p.upper()) or upper.startswith(p.upper() + "/")
            for p in wanted
        )

    out.mkdir(parents=True, exist_ok=True)
    written = total = 0
    for entry in block_file.entries:
        if not selected(entry.name):
            continue
        target = target_of(out, entry.name)
        if entry.is_dir:
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        data = block_file.read(entry.name)
        target.write_bytes(data)
        written += 1
        total += len(data)
    if not wanted and not args.no_manifest:
        write_manifest(out, container)
    print(f"{written} files, {thousands(total)} bytes written to {out}")
    if not wanted and not args.no_manifest:
        print(f"creation order recorded in {out / MANIFEST_NAME}")
    return 0


def cmd_cat(args: argparse.Namespace) -> int:
    container = load(args.container)
    try:
        data = container.block_file.read(normalise(args.path))
    except KeyError as exc:
        raise UsageError(str(exc)) from None
    except IsADirectoryError as exc:
        raise UsageError(str(exc)) from None
    sys.stdout.buffer.write(data)
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    container = load(args.container)
    block_file = container.block_file
    size = container.container_size

    print(f"{container.path}")
    if container.compressed:
        print(f"  storage        LZIS stream, {thousands(container.stored_size)} bytes on disk")
    print("  magic          HPFS")
    print(f"  block size     {block_file.block_size}")
    print(f"  blocks         {block_file.block_count}")
    print(f"  root node      {block_file.root_node}")
    print(f"  free head      {block_file.free_list_head}")
    # `RandomMapSettlements.bfhp` is the only container with a non-zero
    # reserved_b, and it spells "LZIS" -- which is exactly how that container is
    # stored. Show any such tag rather than a bare number.
    tag = block_file.reserved[1].to_bytes(4, "little")
    label = f'  ("{tag.decode("ascii")}")' if tag.isalnum() and tag.isascii() else ""
    print(f"  reserved       {block_file.reserved[0]}, {block_file.reserved[1]}{label}")
    print(f"  unknown +0x18  {block_file.unknown_field}")
    tail = size - (block_file.block_count - 1) * block_file.block_size
    print(
        f"  length         {thousands(size)} bytes "
        f"= {block_file.block_count - 1} x {block_file.block_size} + {tail}"
    )

    try:
        owners = block_file.block_map()
    except BlockFileError as exc:
        print(f"\n  block accounting FAILED: {exc}")
        return 1
    roles = {"header": 0, "node": 0, "index": 0, "data": 0}
    for role, _owner in owners.values():
        roles[role] += 1
    missing = sorted(set(range(block_file.block_count)) - owners.keys())
    files = block_file.files()
    payload = sum(e.size for e in files)
    print("\n  block accounting")
    print(f"    header       {roles['header']:>8}")
    print(f"    nodes        {roles['node']:>8}")
    print(f"    index        {roles['index']:>8}")
    print(f"    data         {roles['data']:>8}")
    print(f"    claimed      {len(owners):>8} of {block_file.block_count}")
    print(f"    unreferenced {len(missing):>8}")
    print(
        f"    compaction   {'compacted, no gaps and no duplicates' if not missing else 'GAPS'}"
    )
    print(f"    payload      {thousands(payload):>8} bytes in {len(files)} files")
    print(f"    overhead     {thousands(size - payload):>8} bytes")

    print("\n  contents")
    if "game.xml" in block_file:
        game = block_file.read("game.xml")
        print(
            f'    game         "{attribute(game, "name")}" by {attribute(game, "author")}, '
            f'type {attribute(game, "game_type")}, season {attribute(game, "season")}, '
            f'start map {attribute(game, "start_map")}'
        )
    maps = map_numbers(block_file)
    print(f"    maps         {len(maps)}")
    for number in maps:
        base = f"Maps/{number}"
        header = ""
        if f"{base}/map.xml" in block_file:
            xml = block_file.read(f"{base}/map.xml")
            header = f'"{attribute(xml, "name")}", world {attribute(xml, "x")}'
            header += f' x {attribute(xml, "y")}'
        print(f"      {base}  {header}")
        for entry in block_file.files():
            if not entry.name.upper().startswith(base.upper() + "/"):
                continue
            if not entry.name.upper().endswith(".GRID"):
                continue
            info = grid_info(block_file.read(entry.name))
            leaf = entry.name.rsplit("/", 1)[-1]
            if info is None:
                print(f"        {leaf:<24} not a DIRG grid")
                continue
            flag = "" if info.consistent else "  SIZE MISMATCH"
            print(
                f"        {leaf:<24} {info.width} x {info.height} cells of "
                f"{info.cell_size} units, {info.bits_per_cell} bit(s)/cell{flag}"
            )
    players = [e for e in files if re.fullmatch(r"player\d+\.xml", e.name, re.I)]
    placed = 0
    for entry in players:
        xml = block_file.read(entry.name)
        if (attribute(xml, "startx"), attribute(xml, "starty")) != ("0", "0"):
            placed += 1
    print(f"    players      {len(players)} slots, {placed} with a start position")
    scripts = [e for e in files if e.name.upper().endswith(".VS")]
    game_scripts = [e for e in scripts if e.name.upper().startswith("SEQUENCES/")]
    print(
        f"    sequences    {len(scripts)} scripts "
        f"({len(game_scripts)} at game level, {len(scripts) - len(game_scripts)} in maps)"
    )
    found = languages(block_file)
    print(f"    localisation {len(found)} language(s): {', '.join(found) or '-'}")
    if "territories.xml" in block_file:
        territories = block_file.read("territories.xml").count(b"<territory")
        print(f"    conquest     {territories} territories")
    speech = [e for e in files if e.name.upper().endswith(".WAV")]
    if speech:
        print(
            f"    speech       {len(speech)} .wav, "
            f"{thousands(sum(e.size for e in speech))} bytes"
        )
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    failures = 0
    for path in args.containers:
        problems: list[str] = []
        try:
            container = load(path)
            block_file = container.block_file
            block_file.validate()

            # Sizes are consistent with the block chains they are stored in.
            for entry in block_file.entries:
                needed = -(-entry.node_size // block_file.block_size)
                if len(entry.blocks) != needed:
                    problems.append(
                        f"{entry.name}: {entry.node_size} bytes needs {needed} blocks, "
                        f"node lists {len(entry.blocks)}"
                    )
                if entry.level not in (0, 1):
                    problems.append(f"{entry.name}: indirection level {entry.level}")
                direct = block_file.pointers_per_block - 2
                if entry.level == 0 and needed > direct:
                    problems.append(f"{entry.name}: {needed} blocks exceed the direct list")

            # Names are unique within a directory, case-insensitively, because
            # that is how the engine resolves them.
            seen: set[str] = set()
            for entry in block_file.entries:
                key = entry.name.upper()
                if key in seen:
                    problems.append(f"{entry.name}: duplicate name (ignoring case)")
                seen.add(key)
        except (BlockFileError, lzis.LzisError, UsageError) as exc:
            problems.append(str(exc))

        if problems:
            failures += 1
            print(f"FAIL {path}")
            for problem in problems[:10]:
                print(f"     {problem}")
            if len(problems) > 10:
                print(f"     ... and {len(problems) - 10} more")
        else:
            print(
                f"ok   {path}  {block_file.block_count} blocks of "
                f"{block_file.block_size}, {len(block_file.files())} files, "
                f"every block claimed exactly once"
            )
    print(f"\n{len(args.containers) - failures}/{len(args.containers)} containers verify")
    return 1 if failures else 0


def cmd_create(args: argparse.Namespace) -> int:
    plan = scan(args.source)
    block_size = args.block_size
    manifest = args.manifest
    if manifest is None and not args.no_manifest:
        candidate = args.source / MANIFEST_NAME
        manifest = candidate if candidate.is_file() else None
    if manifest is not None:
        recorded_size, order = read_manifest(manifest)
        plan = reorder(plan, order)
        if block_size is None:
            block_size = recorded_size
    if block_size is None:
        block_size = DEFAULT_BLOCK_SIZE

    data = build_plan(plan, block_size)
    emit(args.container, data)
    directories = sum(1 for _p, payload in plan if payload is None)
    print(
        f"{args.container}: {len(plan) - directories} files, {directories} directories, "
        f"{thousands(len(data))} bytes, {block_size}-byte blocks"
    )
    return 0


def cmd_add(args: argparse.Namespace) -> int:
    container = load(args.container)
    writable(container)
    path = normalise(args.path)
    if path.upper() in {p.upper() for p, _ in container.block_file.to_plan()}:
        raise UsageError(f"{path!r} already exists; use 'imfs replace'")
    payload = None if args.directory else read_input(args.file)

    plan = container.block_file.to_plan()
    for parent in missing_parents(plan, path):
        plan.insert(insertion_point(plan, parent), (parent, None))
    plan.insert(insertion_point(plan, path), (path, payload))
    emit(args.container, rebuild(container, plan))
    kind = "directory" if payload is None else f"{thousands(len(payload))} bytes"
    print(f"{args.container}: added {path} ({kind})")
    return 0


def cmd_replace(args: argparse.Namespace) -> int:
    container = load(args.container)
    writable(container)
    path = normalise(args.path)
    payload = read_input(args.file)

    plan = container.block_file.to_plan()
    for index, (existing, old) in enumerate(plan):
        if existing.upper() == path.upper():
            if old is None:
                raise UsageError(f"{path!r} is a directory")
            plan[index] = (existing, payload)
            break
    else:
        raise UsageError(f"{path!r} is not in {container.path.name}; use 'imfs add'")
    emit(args.container, rebuild(container, plan))
    print(f"{args.container}: replaced {path} ({thousands(len(payload))} bytes)")
    return 0


def cmd_rm(args: argparse.Namespace) -> int:
    container = load(args.container)
    writable(container)
    path = normalise(args.path)

    plan = container.block_file.to_plan()
    doomed = {path.upper()}
    children = [p for p, _ in plan if p.upper().startswith(path.upper() + "/")]
    if children:
        if not args.recursive:
            raise UsageError(f"{path!r} has {len(children)} entries; pass -r to remove them")
        doomed.update(p.upper() for p in children)
    kept = [(p, payload) for p, payload in plan if p.upper() not in doomed]
    if len(kept) == len(plan):
        raise UsageError(f"{path!r} is not in {container.path.name}")
    emit(args.container, rebuild(container, kept))
    print(f"{args.container}: removed {len(plan) - len(kept)} entrie(s) under {path}")
    return 0


def read_input(source: Path | None) -> bytes:
    if source is None or str(source) == "-":
        return sys.stdin.buffer.read()
    try:
        return source.read_bytes()
    except OSError as exc:
        raise UsageError(str(exc)) from None


# -- entry point ---------------------------------------------------------


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(
        prog="imfs",
        description="Read, extract and write HMMSYS .bfhp block containers.",
    )
    sub = root.add_subparsers(dest="command", required=True)

    listing = sub.add_parser("list", help="print the directory tree")
    listing.add_argument("container", type=Path)
    listing.add_argument(
        "--long", action="store_true", help="also show node, block count and level"
    )
    listing.set_defaults(run=cmd_list)

    extract = sub.add_parser("extract", help="write the tree to a directory")
    extract.add_argument("container", type=Path)
    extract.add_argument("patterns", nargs="*", metavar="PATTERN")
    extract.add_argument("--out", type=Path, help="output directory (default: the stem)")
    extract.add_argument(
        "--no-manifest", action="store_true", help=f"do not write {MANIFEST_NAME}"
    )
    extract.set_defaults(run=cmd_extract)

    cat = sub.add_parser("cat", help="write one stored file to stdout")
    cat.add_argument("container", type=Path)
    cat.add_argument("path")
    cat.set_defaults(run=cmd_cat)

    info = sub.add_parser("info", help="header, block accounting and content summary")
    info.add_argument("container", type=Path)
    info.set_defaults(run=cmd_info)

    verify = sub.add_parser("verify", help="check the specification's invariants")
    verify.add_argument("containers", nargs="+", type=Path)
    verify.set_defaults(run=cmd_verify)

    create = sub.add_parser("create", help="build a container from a directory tree")
    create.add_argument("source", type=Path)
    create.add_argument("container", type=Path)
    create.add_argument("--block-size", type=int, help="default 512, or the manifest's")
    create.add_argument("--manifest", type=Path, help=f"order file (default: {MANIFEST_NAME})")
    create.add_argument("--no-manifest", action="store_true", help="ignore any manifest")
    create.set_defaults(run=cmd_create)

    add = sub.add_parser("add", help="add one file, rewriting the container compacted")
    add.add_argument("container", type=Path)
    add.add_argument("path")
    add.add_argument("file", nargs="?", type=Path, help="source file, or - for stdin")
    add.add_argument("--directory", action="store_true", help="create a directory instead")
    add.set_defaults(run=cmd_add)

    replace = sub.add_parser("replace", help="replace one stored file's contents")
    replace.add_argument("container", type=Path)
    replace.add_argument("path")
    replace.add_argument("file", nargs="?", type=Path, help="source file, or - for stdin")
    replace.set_defaults(run=cmd_replace)

    remove = sub.add_parser("rm", help="remove an entry, rewriting the container compacted")
    remove.add_argument("container", type=Path)
    remove.add_argument("path")
    remove.add_argument("-r", "--recursive", action="store_true")
    remove.set_defaults(run=cmd_rm)

    return root


def main() -> None:
    args = parser().parse_args()
    try:
        sys.exit(args.run(args))
    except (UsageError, BlockFileError, lzis.LzisError) as exc:
        print(f"imfs: {exc}", file=sys.stderr)
        sys.exit(2)
    except BrokenPipeError:
        os._exit(0)


if __name__ == "__main__":
    main()
