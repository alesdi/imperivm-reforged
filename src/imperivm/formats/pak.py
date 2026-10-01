"""Reader for the HMMSYS PackFile format (`.pak`).

Specification: docs/formats/pak.md

Reference implementation: correctness and legibility over speed.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"HMMSYS PackFile\n"
HEADER_SIZE = 0x20
TABLE_START = 0x28


class PackFileError(Exception):
    """Raised when a file does not conform to the pack format."""


@dataclass(frozen=True)
class PackEntry:
    """One file stored in a pack."""

    name: str
    offset: int
    size: int


class PackFile:
    """A parsed HMMSYS pack archive.

    Entry names use ``\\`` as the separator and are conventionally uppercase.
    Lookup via :meth:`read` is case-insensitive and accepts either separator.
    """

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self._data = self.path.read_bytes()
        self.entries: list[PackEntry] = self._parse()
        self._index = {self._key(e.name): e for e in self.entries}

    # -- parsing ---------------------------------------------------------

    def _parse(self) -> list[PackEntry]:
        data = self._data
        if data[: len(MAGIC)] != MAGIC:
            raise PackFileError(f"{self.path}: not a HMMSYS pack (magic mismatch)")

        file_count, name_table_bytes = struct.unpack_from("<II", data, HEADER_SIZE)
        table_end = TABLE_START + name_table_bytes
        if table_end > len(data):
            raise PackFileError(f"{self.path}: entry table overruns the file")

        entries: list[PackEntry] = []
        pos = TABLE_START
        previous = b""

        for i in range(file_count):
            if pos + 2 > table_end:
                raise PackFileError(f"{self.path}: entry table truncated at entry {i}")
            total_len, shared_len = data[pos], data[pos + 1]
            pos += 2

            if shared_len > total_len or shared_len > len(previous):
                raise PackFileError(f"{self.path}: bad front coding at entry {i}")
            suffix_len = total_len - shared_len
            name = previous[:shared_len] + data[pos : pos + suffix_len]
            pos += suffix_len
            previous = name

            offset, size = struct.unpack_from("<II", data, pos)
            pos += 8

            if offset + size > len(data):
                raise PackFileError(f"{self.path}: entry {i} data overruns the file")
            entries.append(PackEntry(name.decode("cp1252"), offset, size))

        if pos != table_end:
            raise PackFileError(
                f"{self.path}: entry table ended at {pos}, expected {table_end}"
            )
        return entries

    # -- access ----------------------------------------------------------

    @staticmethod
    def _key(name: str) -> str:
        return name.upper().replace("/", "\\")

    def __len__(self) -> int:
        return len(self.entries)

    def __contains__(self, name: str) -> bool:
        return self._key(name) in self._index

    def read(self, name: str) -> bytes:
        """Return the contents of one stored file."""
        try:
            entry = self._index[self._key(name)]
        except KeyError:
            raise KeyError(f"{name!r} is not in {self.path.name}") from None
        return self._data[entry.offset : entry.offset + entry.size]

    def validate(self) -> None:
        """Assert the structural invariants described in the specification."""
        offsets = [e.offset for e in self.entries]
        if offsets != sorted(offsets):
            raise PackFileError(f"{self.path}: entry offsets are not monotonic")
        if self.entries:
            last = self.entries[-1]
            if last.offset + last.size != len(self._data):
                raise PackFileError(
                    f"{self.path}: last entry ends at {last.offset + last.size}, "
                    f"file is {len(self._data)} bytes"
                )


def main() -> None:
    import argparse

    parser = argparse.ArgumentParser(description="Inspect a HMMSYS pack archive.")
    parser.add_argument("pack", type=Path)
    parser.add_argument("--extract", metavar="NAME", help="write one file to stdout")
    args = parser.parse_args()

    pack = PackFile(args.pack)
    if args.extract:
        import sys

        sys.stdout.buffer.write(pack.read(args.extract))
        return

    pack.validate()
    print(f"{args.pack}: {len(pack)} files")
    for entry in pack.entries:
        print(f"  {entry.offset:>10} {entry.size:>9}  {entry.name}")


if __name__ == "__main__":
    main()
