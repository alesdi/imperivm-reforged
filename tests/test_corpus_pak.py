"""Corpus tests for the pack reader. Specification: docs/formats/pak.md.

Every assertion here is a claim about the retail data, not about the reader in
isolation: the counts are the ones documented in the specification, and a
failure means either the reader regressed or the installation is not the one
the specification was written against.
"""

from __future__ import annotations

import datetime as dt
import struct
from dataclasses import dataclass

import pytest

import corpus
from conftest import requires_game
from imperivm.formats.pak import HEADER_SIZE, TABLE_START, PackFile

pytestmark = requires_game

#: Pack images shipped on disk, excluding the LZIS-wrapped `RandomMap.pak`.
PACK_COUNT = 13

#: Files stored across those 13, per docs/formats/pak.md.
FILE_COUNT = 14_602

#: Consecutive payload pairs across all 14 pack images -- the 13 on disk plus
#: the decompressed `RandomMap.pak`, whose 90 entries contribute 89 pairs.
ABUTTING_PAIRS = 14_678

#: The oldest and newest calendar years any timestamp decodes to.
YEAR_RANGE = (1998, 2006)


@dataclass(frozen=True)
class Tables:
    """The parts of a pack that sit before the file data."""

    file_count: int
    name_table_bytes: int
    entry_table: bytes  # exactly `name_table_bytes` long, starting at 0x28
    stamps: tuple[int, ...]
    first_offset: int


@pytest.fixture(scope="module")
def tables(packs) -> dict[str, Tables]:
    """Header, entry table and timestamp table of every pack, read once."""
    out: dict[str, Tables] = {}
    for label, pack in packs:
        data = pack.path.read_bytes()
        file_count, name_table_bytes = struct.unpack_from("<II", data, HEADER_SIZE)
        table_end = TABLE_START + name_table_bytes
        out[label] = Tables(
            file_count=file_count,
            name_table_bytes=name_table_bytes,
            entry_table=data[TABLE_START:table_end],
            stamps=struct.unpack_from(f"<{file_count}I", data, table_end),
            first_offset=pack.entries[0].offset,
        )
    return out


def dos_datetime(stamp: int) -> dt.datetime:
    """Decode one MS-DOS packed date and time. Raises ValueError if invalid."""
    return dt.datetime(
        year=((stamp >> 25) & 0x7F) + 1980,
        month=(stamp >> 21) & 0x0F,
        day=(stamp >> 16) & 0x1F,
        hour=(stamp >> 11) & 0x1F,
        minute=(stamp >> 5) & 0x3F,
        second=(stamp & 0x1F) * 2,
        tzinfo=dt.timezone.utc,
    )


def test_every_pack_parses_and_validates(packs):
    """All 13 shipped archives parse and satisfy the reader's own invariants."""
    labels = [label for label, _ in packs]
    assert len(labels) == PACK_COUNT, labels
    for _label, pack in packs:
        pack.validate()


def test_total_file_count(packs):
    assert sum(len(pack) for _label, pack in packs) == FILE_COUNT


def test_offsets_are_monotonic(packs):
    for label, pack in packs:
        offsets = [e.offset for e in pack.entries]
        assert offsets == sorted(offsets), label


def test_last_entry_ends_exactly_at_eof(packs):
    for label, pack in packs:
        last = pack.entries[-1]
        assert last.offset + last.size == pack.path.stat().st_size, label


def test_entry_table_ends_exactly_where_the_header_says(tables):
    """Walking `file_count` variable-length entries lands on `0x28 + name_table_bytes`."""
    for label, t in tables.items():
        pos = 0
        for _ in range(t.file_count):
            total_len, shared_len = t.entry_table[pos], t.entry_table[pos + 1]
            pos += 2 + (total_len - shared_len) + 8
        assert pos == t.name_table_bytes, label


def test_first_file_begins_after_the_timestamp_table(tables):
    """The gap between the entry table and the data is exactly `file_count * 4`."""
    for label, t in tables.items():
        table_end = TABLE_START + t.name_table_bytes
        assert t.first_offset == table_end + t.file_count * 4, label


def test_stored_files_are_contiguous(packs, game_dir, tmp_path):
    """No gaps and no padding between payloads, in any of the 14 pack images."""
    images = list(packs)

    unwrapped = tmp_path / "RandomMap.pak"
    unwrapped.write_bytes(corpus.random_map_image(game_dir))
    images.append(("Packs/RandomMap.pak (decompressed)", PackFile(unwrapped)))

    pairs = 0
    for label, pack in images:
        for before, after in zip(pack.entries, pack.entries[1:]):
            assert before.offset + before.size == after.offset, f"{label}: {before.name}"
            pairs += 1
    assert pairs == ABUTTING_PAIRS


def test_every_timestamp_decodes_to_a_valid_date(packs, tables):
    """All 14,602 stamps are real calendar dates, and all fall in 1998..2006."""
    names = {label: [e.name for e in pack.entries] for label, pack in packs}

    stamps = 0
    years: set[int] = set()
    for label, t in tables.items():
        assert len(t.stamps) == t.file_count == len(names[label]), label
        for index, stamp in enumerate(t.stamps):
            try:
                years.add(dos_datetime(stamp).year)
            except ValueError as exc:  # pragma: no cover - would be a real finding
                raise AssertionError(
                    f"{label}: entry {index} ({names[label][index]}) has undecodable "
                    f"stamp {stamp:#010x}: {exc}"
                ) from None
            stamps += 1

    assert stamps == FILE_COUNT
    assert (min(years), max(years)) == YEAR_RANGE
