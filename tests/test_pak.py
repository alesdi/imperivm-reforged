"""Data-free tests for the HMMSYS pack reader. Specification: docs/formats/pak.md."""

from __future__ import annotations

import struct

import pytest

import synthetic
from imperivm.formats.pak import MAGIC, PackFile, PackFileError


def write(tmp_path, data: bytes, name: str = "test.pak"):
    path = tmp_path / name
    path.write_bytes(data)
    return path


FILES = [
    ("FONTS\\COURIERNEW16.APF", b"courier new, sixteen point"),
    ("FONTS\\COURIERNEW16.INI", b"[font]\r\nsize=16\r\n"),
    ("FONTS\\TAHOMA13.APF", b"tahoma"),
    ("UNITS\\BBOWMAN\\ATTACK.RLE.MMP", bytes(range(256))),
]


def test_round_trip(tmp_path):
    pack = PackFile(write(tmp_path, synthetic.build_pack(FILES)))
    pack.validate()
    assert [e.name for e in pack.entries] == [name for name, _ in FILES]
    for name, payload in FILES:
        assert pack.read(name) == payload


def test_front_coding_reconstructs_shared_prefixes(tmp_path):
    """The spec's worked example: 0 / 19 / 6 shared bytes for the first three names."""
    data = synthetic.build_pack(FILES)
    file_count, table_bytes = struct.unpack_from("<II", data, 0x20)
    assert file_count == len(FILES)

    pos = 0x28
    shared_lengths = []
    for _ in range(file_count):
        total, shared = data[pos], data[pos + 1]
        shared_lengths.append(shared)
        pos += 2 + (total - shared) + 8
    assert shared_lengths[:3] == [0, 19, 6]
    assert pos == 0x28 + table_bytes


def test_entry_table_ends_exactly_where_the_header_says(tmp_path):
    data = bytearray(synthetic.build_pack(FILES))
    struct.pack_into("<I", data, 0x24, struct.unpack_from("<I", data, 0x24)[0] + 1)
    with pytest.raises(PackFileError, match="entry table ended at"):
        PackFile(write(tmp_path, bytes(data)))


def test_offsets_are_monotonic_and_last_entry_ends_at_eof(tmp_path):
    pack = PackFile(write(tmp_path, synthetic.build_pack(FILES)))
    offsets = [e.offset for e in pack.entries]
    assert offsets == sorted(offsets)
    last = pack.entries[-1]
    assert last.offset + last.size == pack.path.stat().st_size
    pack.validate()


def test_stored_files_need_not_be_contiguous(tmp_path):
    """Retail packs leave gaps between payloads; sizes must come from the table."""
    pack = PackFile(write(tmp_path, synthetic.build_pack(FILES, gap=7)))
    pack.validate()
    for name, payload in FILES:
        assert pack.read(name) == payload
    first, second = pack.entries[0], pack.entries[1]
    assert second.offset - (first.offset + first.size) == 7


def test_lookup_is_case_and_separator_insensitive(tmp_path):
    pack = PackFile(write(tmp_path, synthetic.build_pack(FILES)))
    assert pack.read("fonts/tahoma13.apf") == b"tahoma"
    assert "Fonts\\Tahoma13.APF" in pack
    assert "fonts/nope.apf" not in pack
    with pytest.raises(KeyError):
        pack.read("fonts/nope.apf")


def test_empty_pack(tmp_path):
    pack = PackFile(write(tmp_path, synthetic.build_pack([])))
    pack.validate()
    assert len(pack) == 0
    assert pack.entries == []


def test_names_are_decoded_as_cp1252(tmp_path):
    name = "DATA\\CAF\xc9.INI"
    pack = PackFile(write(tmp_path, synthetic.build_pack([(name, b"x")])))
    assert pack.entries[0].name == name


# -- error paths ---------------------------------------------------------


def test_rejects_bad_magic(tmp_path):
    data = b"NOT A PACK\n" + b"\0" * 64
    with pytest.raises(PackFileError, match="magic mismatch"):
        PackFile(write(tmp_path, data))


def test_rejects_entry_table_overrunning_the_file(tmp_path):
    data = MAGIC + b"\x1a" + b"\0" * 15 + struct.pack("<II", 1, 4096)
    with pytest.raises(PackFileError, match="overruns the file"):
        PackFile(write(tmp_path, data))


def test_rejects_truncated_entry_table(tmp_path):
    """file_count claims more entries than name_table_bytes can hold."""
    table = bytes((4, 0)) + b"NAME" + struct.pack("<II", 0x28 + 10, 1)
    data = MAGIC + b"\x1a" + b"\0" * 15 + struct.pack("<II", 2, len(table)) + table + b"x"
    with pytest.raises(PackFileError, match="truncated at entry"):
        PackFile(write(tmp_path, data))


def test_rejects_front_coding_that_reuses_more_than_the_previous_name(tmp_path):
    table = bytes((4, 2)) + b"NAME" + struct.pack("<II", 0, 0)
    data = MAGIC + b"\x1a" + b"\0" * 15 + struct.pack("<II", 1, len(table)) + table
    with pytest.raises(PackFileError, match="bad front coding"):
        PackFile(write(tmp_path, data))


def test_rejects_entry_data_outside_the_file(tmp_path):
    table = bytes((4, 0)) + b"NAME" + struct.pack("<II", 0, 1 << 20)
    data = MAGIC + b"\x1a" + b"\0" * 15 + struct.pack("<II", 1, len(table)) + table
    with pytest.raises(PackFileError, match="data overruns the file"):
        PackFile(write(tmp_path, data))


def test_validate_rejects_non_monotonic_offsets(tmp_path):
    data = bytearray(synthetic.build_pack(FILES))
    pack = PackFile(write(tmp_path, bytes(data)))
    # Swap the first two offsets in the parsed entry list to simulate a pack
    # whose table is out of order but whose ranges are still in bounds.
    a, b = pack.entries[0], pack.entries[1]
    pack.entries[0] = type(a)(a.name, b.offset, a.size)
    pack.entries[1] = type(b)(b.name, a.offset, b.size)
    with pytest.raises(PackFileError, match="not monotonic"):
        pack.validate()


def test_validate_rejects_a_last_entry_that_does_not_reach_eof(tmp_path):
    data = synthetic.build_pack(FILES) + b"trailing"
    with pytest.raises(PackFileError, match="last entry ends at"):
        PackFile(write(tmp_path, data)).validate()
