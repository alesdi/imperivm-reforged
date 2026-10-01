"""``impk`` — list, extract, create and verify HMMSYS pack archives.

Specification: docs/formats/pak.md
Tool documentation: docs/tools/impk.md

The reader in :mod:`imperivm.formats.pak` covers the header, the front-coded entry
table and the file data. Writing a pack byte-for-byte needs one more thing that the
reader has no reason to look at: between the entry table and the first stored file
there is an array of ``file_count`` little-endian ``u32`` MS-DOS packed date/time
stamps, one per entry, in entry order. It is the only part of a pack that is not
recoverable from the file names and contents, so ``extract`` writes each stamp to the
extracted file's modification time and ``create`` reads it back. That is what makes
the round trip exact.

Reference implementation: correctness and legibility over speed.
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import struct
import sys
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from imperivm.formats.lzis import MAGIC as LZIS_MAGIC
from imperivm.formats.lzis import decompress
from imperivm.formats.pak import HEADER_SIZE, MAGIC, TABLE_START, PackFileError

#: Size of one MS-DOS packed date/time stamp in the timestamp table.
STAMP_SIZE = 4

#: Longest name the entry table can express: ``total_name_len`` is a ``u8``.
MAX_NAME_LEN = 0xFF

#: The encoding of stored names. ASCII in the retail data, but cp1252 is what the
#: reader assumes and localised packs could plausibly use the upper half.
NAME_ENCODING = "cp1252"

#: Extensions the game uses as a unit, longest first, so that ``.rle.mmp`` is not
#: reported as ``.mmp`` and ``.sc.xml`` is not reported as ``.xml``.
COMPOUND_SUFFIXES = (".rle.mmp", ".ent.xml", ".sc.xml")


# -- timestamps ----------------------------------------------------------------
#
# MS-DOS packs a local date and time into 32 bits: the high half is the date as
# ((year - 1980) << 9) | (month << 5) | day, the low half the time as
# (hour << 11) | (minute << 5) | (second // 2). Seconds therefore have two-second
# resolution and the representable years are 1980 through 2107.
#
# We interpret a stamp as UTC rather than as local time. The format itself carries
# no zone, so either reading is defensible, and UTC has the property that matters
# here: it makes stamp -> mtime -> stamp a bijection on every machine, with no
# daylight-saving gap in which a wall-clock time does not exist. A pack extracted
# in one zone therefore rebuilds byte-identically in another.

DOS_EPOCH_YEAR = 1980
DOS_MAX_YEAR = DOS_EPOCH_YEAR + 0x7F


class DosTimeError(PackFileError):
    """Raised when a timestamp cannot be represented as an MS-DOS stamp."""


def dos_to_posix(stamp: int) -> int:
    """Convert an MS-DOS packed date/time to a POSIX timestamp, read as UTC."""
    date, time = stamp >> 16, stamp & 0xFFFF
    moment = datetime(
        year=DOS_EPOCH_YEAR + (date >> 9),
        month=(date >> 5) & 0x0F,
        day=date & 0x1F,
        hour=time >> 11,
        minute=(time >> 5) & 0x3F,
        second=(time & 0x1F) * 2,
        tzinfo=timezone.utc,
    )
    return int(moment.timestamp())


def posix_to_dos(seconds: int) -> int:
    """Convert a POSIX timestamp to an MS-DOS packed date/time, written as UTC.

    Seconds are truncated to the format's two-second resolution.
    """
    moment = datetime.fromtimestamp(seconds, tz=timezone.utc)
    if not DOS_EPOCH_YEAR <= moment.year <= DOS_MAX_YEAR:
        raise DosTimeError(
            f"{moment.isoformat()} is outside the MS-DOS range "
            f"{DOS_EPOCH_YEAR}-{DOS_MAX_YEAR} and cannot be stored in a pack"
        )
    date = ((moment.year - DOS_EPOCH_YEAR) << 9) | (moment.month << 5) | moment.day
    time = (moment.hour << 11) | (moment.minute << 5) | (moment.second // 2)
    return (date << 16) | time


def dos_is_valid(stamp: int) -> bool:
    """Return whether a stamp decodes to a real calendar date and clock time."""
    try:
        dos_to_posix(stamp)
    except ValueError:
        return False
    return True


def format_dos(stamp: int) -> str:
    """Render a stamp as ``YYYY-MM-DD HH:MM:SS``, or as raw hex if it is invalid."""
    if not dos_is_valid(stamp):
        return f"<invalid 0x{stamp:08x}>"
    return datetime.fromtimestamp(dos_to_posix(stamp), tz=timezone.utc).strftime(
        "%Y-%m-%d %H:%M:%S"
    )


# -- reading -------------------------------------------------------------------


@dataclass(frozen=True)
class PackedFile:
    """One file stored in a pack, including its timestamp.

    ``name`` keeps the stored form: uppercase, ``\\``-separated, cp1252.
    """

    name: str
    offset: int
    size: int
    dos_time: int
    shared_len: int

    @property
    def raw_name(self) -> bytes:
        """The name as it is stored in the entry table."""
        return self.name.encode(NAME_ENCODING)

    @property
    def posix_time(self) -> int:
        """The timestamp as a POSIX timestamp."""
        return dos_to_posix(self.dos_time)

    @property
    def out_path(self) -> str:
        """The name in extraction form: lowercase, ``/``-separated."""
        return self.name.lower().replace("\\", "/")


@dataclass(frozen=True)
class PackArchive:
    """A parsed pack, held as a decompressed in-memory image.

    ``compressed`` records whether the file on disk was wrapped in an LZIS stream,
    in which case ``image`` is the decompressed pack and is longer than the file.
    """

    path: Path
    image: bytes
    compressed: bool
    files: list[PackedFile]
    name_table_bytes: int

    @classmethod
    def open(cls, path: str | Path) -> PackArchive:
        """Read a pack, transparently decompressing a whole-file LZIS wrapper."""
        path = Path(path)
        raw = path.read_bytes()
        compressed = raw[: len(LZIS_MAGIC)] == LZIS_MAGIC
        image = decompress(raw) if compressed else raw
        files, name_table_bytes = cls._parse(path, image)
        return cls(path, image, compressed, files, name_table_bytes)

    @staticmethod
    def _parse(path: Path, data: bytes) -> tuple[list[PackedFile], int]:
        if data[: len(MAGIC)] != MAGIC:
            raise PackFileError(f"{path}: not a HMMSYS pack (magic mismatch)")

        file_count, name_table_bytes = struct.unpack_from("<II", data, HEADER_SIZE)
        table_end = TABLE_START + name_table_bytes
        if table_end > len(data):
            raise PackFileError(f"{path}: entry table overruns the file")

        raw_names: list[bytes] = []
        offsets: list[tuple[int, int]] = []
        shared_lens: list[int] = []
        pos = TABLE_START
        previous = b""

        for i in range(file_count):
            if pos + 2 > table_end:
                raise PackFileError(f"{path}: entry table truncated at entry {i}")
            total_len, shared_len = data[pos], data[pos + 1]
            pos += 2

            if shared_len > total_len or shared_len > len(previous):
                raise PackFileError(f"{path}: bad front coding at entry {i}")
            suffix_len = total_len - shared_len
            name = previous[:shared_len] + data[pos : pos + suffix_len]
            pos += suffix_len
            previous = name

            offset, size = struct.unpack_from("<II", data, pos)
            pos += 8
            if offset + size > len(data):
                raise PackFileError(f"{path}: entry {i} data overruns the file")
            raw_names.append(name)
            offsets.append((offset, size))
            shared_lens.append(shared_len)

        if pos != table_end:
            raise PackFileError(
                f"{path}: entry table ended at {pos}, expected {table_end}"
            )

        stamps_end = table_end + file_count * STAMP_SIZE
        if stamps_end > len(data):
            raise PackFileError(f"{path}: timestamp table overruns the file")
        stamps = struct.unpack_from(f"<{file_count}I", data, table_end)

        files = [
            PackedFile(name.decode(NAME_ENCODING), offset, size, stamp, shared)
            for name, (offset, size), stamp, shared in zip(
                raw_names, offsets, stamps, shared_lens
            )
        ]
        return files, name_table_bytes

    def __len__(self) -> int:
        return len(self.files)

    @property
    def data_start(self) -> int:
        """Where file data begins: after the entry table and the timestamp table."""
        return TABLE_START + self.name_table_bytes + len(self.files) * STAMP_SIZE

    def read(self, entry: PackedFile) -> bytes:
        """Return the contents of one stored file."""
        return self.image[entry.offset : entry.offset + entry.size]


# -- writing -------------------------------------------------------------------


@dataclass(frozen=True)
class SourceFile:
    """A file on disk destined for a pack."""

    path: Path
    name: bytes
    size: int
    dos_time: int


def shared_prefix_len(a: bytes, b: bytes) -> int:
    """Length of the common prefix of two names, capped at what a ``u8`` holds."""
    limit = min(len(a), len(b), MAX_NAME_LEN)
    i = 0
    while i < limit and a[i] == b[i]:
        i += 1
    return i


def encode_entry_table(sources: list[SourceFile], offsets: list[int]) -> bytes:
    """Encode the front-coded entry table for an already sorted list of files."""
    table = bytearray()
    previous = b""
    for source, offset in zip(sources, offsets):
        shared = shared_prefix_len(previous, source.name)
        table.append(len(source.name))
        table.append(shared)
        table += source.name[shared:]
        table += struct.pack("<II", offset, source.size)
        previous = source.name
    return bytes(table)


def entry_table_size(sources: list[SourceFile]) -> int:
    """Size of the entry table, which does not depend on the data offsets."""
    total = 0
    previous = b""
    for source in sources:
        total += 2 + (len(source.name) - shared_prefix_len(previous, source.name)) + 8
        previous = source.name
    return total


def build_pack(sources: list[SourceFile]) -> bytes:
    """Assemble a complete pack image from files sorted into storage order.

    The layout is fully determined: the entry table starts at ``0x28``, the
    timestamp table follows it, and the file data follows that with no padding
    and no gaps between files.
    """
    name_table_bytes = entry_table_size(sources)
    cursor = TABLE_START + name_table_bytes + len(sources) * STAMP_SIZE

    offsets: list[int] = []
    for source in sources:
        offsets.append(cursor)
        cursor += source.size

    image = bytearray()
    image += MAGIC
    image += b"\x1a"
    image += bytes(15)
    image += struct.pack("<II", len(sources), name_table_bytes)
    image += encode_entry_table(sources, offsets)
    image += struct.pack(f"<{len(sources)}I", *(s.dos_time for s in sources))

    for source, offset in zip(sources, offsets):
        assert len(image) == offset, "entry table and data layout disagree"
        contents = source.path.read_bytes()
        if len(contents) != source.size:
            raise PackFileError(f"{source.path}: changed size while being packed")
        image += contents
    return bytes(image)


def collect_sources(root: Path, include_hidden: bool = False) -> list[SourceFile]:
    """Walk a directory tree and return its files in pack storage order.

    Names are uppercased and ``/`` is replaced by ``\\``, undoing what ``extract``
    does. The sort is plain bytewise ascending on the resulting stored name, which
    is the order the retail packs use.
    """
    if not root.is_dir():
        raise PackFileError(f"{root}: not a directory")

    sources: list[SourceFile] = []
    for dirpath, dirnames, filenames in os.walk(root):
        if not include_hidden:
            dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        dirnames.sort()
        for filename in sorted(filenames):
            if not include_hidden and filename.startswith("."):
                continue
            path = Path(dirpath) / filename
            if path.is_symlink() or not path.is_file():
                continue
            relative = path.relative_to(root).as_posix()
            try:
                name = relative.upper().replace("/", "\\").encode(NAME_ENCODING)
            except UnicodeEncodeError as exc:
                raise PackFileError(
                    f"{path}: name is not encodable as {NAME_ENCODING}"
                ) from exc
            if len(name) > MAX_NAME_LEN:
                raise PackFileError(
                    f"{path}: stored name is {len(name)} bytes, "
                    f"the format allows at most {MAX_NAME_LEN}"
                )
            stat = path.stat()
            try:
                dos_time = posix_to_dos(stat.st_mtime_ns // 1_000_000_000)
            except DosTimeError as exc:
                raise PackFileError(f"{path}: {exc}") from exc
            sources.append(SourceFile(path, name, stat.st_size, dos_time))

    sources.sort(key=lambda s: s.name)
    duplicates = [n for n, c in Counter(s.name for s in sources).items() if c > 1]
    if duplicates:
        shown = duplicates[0].decode(NAME_ENCODING)
        raise PackFileError(
            f"{root}: {len(duplicates)} name(s) collide once uppercased, "
            f"the first being {shown!r}"
        )
    return sources


# -- safe extraction paths -----------------------------------------------------


class UnsafePathError(PackFileError):
    """Raised when a stored name would escape the extraction directory."""


def safe_relative_path(name: str) -> Path:
    """Convert a stored name to a relative output path, refusing to escape.

    A pack is untrusted input. Rejected: absolute paths, drive letters, UNC roots,
    ``..`` components, and empty names. The caller must still confirm that the
    joined path stays inside the output directory, since a symlink planted by an
    earlier entry could redirect a later one.
    """
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".")]
    if not parts:
        raise UnsafePathError(f"{name!r}: empty path")
    if name.startswith(("/", "\\")):
        raise UnsafePathError(f"{name!r}: absolute path")
    if ".." in parts:
        raise UnsafePathError(f"{name!r}: contains a '..' component")
    if len(parts[0]) >= 2 and parts[0][1] == ":":
        raise UnsafePathError(f"{name!r}: contains a drive letter")
    return Path(*(p.lower() for p in parts))


def resolve_output_path(out_dir: Path, name: str) -> Path:
    """Return the path to write a stored file to, or raise if it escapes."""
    target = out_dir / safe_relative_path(name)
    root = out_dir.resolve()
    resolved = Path(os.path.normpath(target))
    if root not in resolved.resolve().parents and resolved.resolve() != root:
        raise UnsafePathError(f"{name!r}: resolves outside {out_dir}")
    return target


# -- reporting helpers ---------------------------------------------------------


def file_type(name: str) -> str:
    """The extension a name should be counted under, honouring compound suffixes."""
    lowered = name.lower()
    for suffix in COMPOUND_SUFFIXES:
        if lowered.endswith(suffix):
            return suffix
    _, dot, extension = lowered.rpartition(".")
    return f".{extension}" if dot and "\\" not in extension else "<none>"


def human_size(size: int) -> str:
    """Render a byte count compactly."""
    value = float(size)
    for unit in ("B", "KB", "MB", "GB"):
        if value < 1024 or unit == "GB":
            return f"{value:.1f} {unit}" if unit != "B" else f"{int(value)} B"
        value /= 1024
    raise AssertionError("unreachable")


def matches(entry: PackedFile, patterns: list[str]) -> bool:
    """Whether an entry is selected by any of the given patterns.

    Patterns are matched case-insensitively against the extraction-form path, with
    either separator accepted. A pattern with no wildcard also matches anything
    beneath it, so ``units`` selects the whole directory.
    """
    if not patterns:
        return True
    path = entry.out_path
    for pattern in patterns:
        normalised = pattern.lower().replace("\\", "/")
        if fnmatch.fnmatchcase(path, normalised):
            return True
        if not any(c in normalised for c in "*?["):
            if path == normalised or path.startswith(normalised.rstrip("/") + "/"):
                return True
    return False


def describe_source(archive: PackArchive) -> str:
    """One line naming the pack and whether it arrived LZIS-compressed."""
    if archive.compressed:
        return (
            f"{archive.path}: LZIS-compressed, decompressed in memory to "
            f"{len(archive.image)} bytes"
        )
    return f"{archive.path}: {len(archive.image)} bytes"


# -- commands ------------------------------------------------------------------


def cmd_list(args: argparse.Namespace) -> int:
    archive = PackArchive.open(args.pack)
    print(describe_source(archive))
    selected = [e for e in archive.files if matches(e, args.pattern)]

    for entry in selected:
        if args.long:
            print(
                f"  {entry.offset:>10} {entry.size:>10}  "
                f"{format_dos(entry.dos_time)}  {entry.name}"
            )
        else:
            print(f"  {entry.offset:>10} {entry.size:>10}  {entry.name}")

    if not args.long:
        print(f"{len(selected)} of {len(archive)} files")
        return 0

    total = sum(e.size for e in selected)
    overhead = archive.data_start
    print()
    print(f"  files          {len(selected)} of {len(archive)}")
    print(f"  file data      {total} bytes ({human_size(total)})")
    print(
        f"  header/tables  {overhead} bytes "
        f"(entry table {archive.name_table_bytes}, "
        f"timestamps {len(archive) * STAMP_SIZE})"
    )
    if selected:
        stamps = [e.dos_time for e in selected]
        print(
            f"  timestamps     {format_dos(min(stamps))} to {format_dos(max(stamps))}"
        )
    print()
    print("  type breakdown")
    counts = Counter(file_type(e.name) for e in selected)
    sizes: Counter[str] = Counter()
    for entry in selected:
        sizes[file_type(entry.name)] += entry.size
    for extension, count in sorted(counts.items(), key=lambda kv: -sizes[kv[0]]):
        print(f"    {extension:<12} {count:>6}  {human_size(sizes[extension]):>10}")
    return 0


def cmd_extract(args: argparse.Namespace) -> int:
    archive = PackArchive.open(args.pack)
    out_dir = Path(args.out) if args.out else Path(archive.path.stem.lower())
    print(describe_source(archive))

    written = 0
    for entry in archive.files:
        if not matches(entry, args.pattern):
            continue
        target = resolve_output_path(out_dir, entry.name)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(archive.read(entry))
        # The timestamp table is the only part of a pack that a plain directory
        # tree cannot hold. Parking it in the file's mtime is what lets `create`
        # rebuild the pack exactly.
        stamp = entry.posix_time if dos_is_valid(entry.dos_time) else 0
        os.utime(target, (stamp, stamp))
        written += 1
        if args.verbose:
            print(f"  {target}")

    print(f"extracted {written} of {len(archive)} files to {out_dir}")
    return 0


def cmd_create(args: argparse.Namespace) -> int:
    root = Path(args.dir)
    sources = collect_sources(root, include_hidden=args.include_hidden)
    if not sources:
        raise PackFileError(f"{root}: no files to pack")

    image = build_pack(sources)
    out = Path(args.pack)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)

    total = sum(s.size for s in sources)
    print(f"{out}: {len(sources)} files, {len(image)} bytes")
    print(f"  file data      {total} bytes ({human_size(total)})")
    print(f"  header/tables  {len(image) - total} bytes")
    return 0


def cmd_verify(args: argparse.Namespace) -> int:
    archive = PackArchive.open(args.pack)
    print(describe_source(archive))
    image = archive.image
    files = archive.files
    failures: list[str] = []

    def check(label: str, ok: bool, detail: str = "") -> None:
        status = "ok" if ok else "FAIL"
        print(f"  [{status}] {label}" + (f" — {detail}" if detail else ""))
        if not ok:
            failures.append(label)

    check("magic and header", image[: len(MAGIC)] == MAGIC)

    # Re-derive the table size from the entries; it must land exactly on the
    # value the header declares, which is what makes the parse cursor check real.
    rebuilt = sum(2 + len(e.raw_name) - e.shared_len + 8 for e in files)
    check(
        "entry table size matches the header",
        rebuilt == archive.name_table_bytes,
        f"{len(files)} entries in {archive.name_table_bytes} bytes",
    )

    offsets = [e.offset for e in files]
    check("data offsets are monotonically non-decreasing", offsets == sorted(offsets))
    check(
        "every entry lies inside the file",
        all(e.offset + e.size <= len(image) for e in files),
    )

    if files:
        last = files[-1]
        end = last.offset + last.size
        check(
            "last entry ends on the last byte of the pack",
            end == len(image),
            f"ends at {end}, image is {len(image)} bytes",
        )
        check(
            "file data starts after the entry and timestamp tables",
            files[0].offset == archive.data_start,
            f"first file at {files[0].offset}, tables end at {archive.data_start}",
        )
        gaps = [
            files[i + 1].offset - (files[i].offset + files[i].size)
            for i in range(len(files) - 1)
        ]
        nonzero = sum(1 for g in gaps if g != 0)
        check(
            "stored files are contiguous",
            nonzero == 0,
            "no gaps" if nonzero == 0 else f"{nonzero} gap(s)",
        )

    raw = [e.raw_name for e in files]
    check(
        "names are in strict bytewise ascending order",
        all(a < b for a, b in zip(raw, raw[1:])),
    )

    previous = b""
    non_maximal = 0
    for entry in files:
        if entry.shared_len != shared_prefix_len(previous, entry.raw_name):
            non_maximal += 1
        previous = entry.raw_name
    check(
        "front coding uses the maximal shared prefix",
        non_maximal == 0,
        "all entries" if not non_maximal else f"{non_maximal} entries do not",
    )

    invalid = [e for e in files if not dos_is_valid(e.dos_time)]
    check(
        "every timestamp decodes to a real date and time",
        not invalid,
        "all valid" if not invalid else f"{len(invalid)} invalid",
    )

    print(f"{'FAILED' if failures else 'passed'}: {len(files)} entries checked")
    return 1 if failures else 0


# -- entry point ---------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="impk", description="List, extract, create and verify HMMSYS pack archives."
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_list = sub.add_parser("list", help="list the files in a pack")
    p_list.add_argument("pack", type=Path)
    p_list.add_argument("pattern", nargs="*", help="only list matching entries")
    p_list.add_argument("--long", action="store_true", help="totals and type breakdown")
    p_list.set_defaults(func=cmd_list)

    p_extract = sub.add_parser("extract", help="extract files from a pack")
    p_extract.add_argument("pack", type=Path)
    p_extract.add_argument("pattern", nargs="*", help="only extract matching entries")
    p_extract.add_argument("--out", type=Path, help="output directory")
    p_extract.add_argument("-v", "--verbose", action="store_true")
    p_extract.set_defaults(func=cmd_extract)

    p_create = sub.add_parser("create", help="build a pack from a directory tree")
    p_create.add_argument("dir", type=Path)
    p_create.add_argument("pack", type=Path)
    p_create.add_argument(
        "--include-hidden",
        action="store_true",
        help="also pack dot-files, which are skipped by default",
    )
    p_create.set_defaults(func=cmd_create)

    p_verify = sub.add_parser("verify", help="check a pack's structural invariants")
    p_verify.add_argument("pack", type=Path)
    p_verify.set_defaults(func=cmd_verify)

    return parser


def main() -> None:
    args = build_parser().parse_args()
    try:
        sys.exit(args.func(args))
    except PackFileError as exc:
        print(f"impk: {exc}", file=sys.stderr)
        sys.exit(2)
    except BrokenPipeError:
        os._exit(0)


if __name__ == "__main__":
    main()
