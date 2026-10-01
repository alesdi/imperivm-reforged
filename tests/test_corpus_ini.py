"""Corpus tests for the INI reader. Specification: docs/formats/ini.md.

The counts below are the whole point of this module. The dialect the reader
implements was derived by measuring the shipped files, so a reader that no
longer reproduces those measurements has stopped describing the data. They are
exact for the same reason `test_corpus_vs.py`'s script count is: a change that
turns 3,278 bare lines into 3,277 is a regression, and an exact number cannot be
quietly absorbed.

This module earned its place immediately. The specification's first draft
claimed 149 files, 3,661 sections and "no value is ever quoted", all three
derived from a scan that had enumerated the packs by hand and missed
`local/italian.pak`. These tests discover by content and caught every one.
"""

from __future__ import annotations

import re

import pytest

from conftest import requires_game
from imperivm.formats import ini

pytestmark = requires_game

FILE_COUNT = 150
SECTION_COUNT = 3662
KEY_VALUE_COUNT = 18094
BARE_LINE_COUNT = 3278

#: Keys declared twice within their own section, across nine files.
DUPLICATE_KEY_COUNT = 22

PLACEHOLDER = re.compile(r"%[A-Za-z0-9_.]+%")


@pytest.fixture(scope="module")
def ini_files(packs) -> list[tuple[str, ini.IniFile]]:
    """Every `.ini` in the installation, parsed."""
    return [
        (f"{label}:{name}", ini.parse(data))
        for label, name, data in packs.entries_named(".INI")
    ]


def test_every_ini_is_found(ini_files):
    assert len(ini_files) == FILE_COUNT


def test_every_ini_parses(ini_files):
    # `ini.parse` raises rather than returning a partial file, so building the
    # fixture at all is the assertion; this documents it.
    assert all(document.sections for _, document in ini_files)


def test_the_dialect_still_measures_the_same(ini_files):
    sections = 0
    key_values = 0
    bare = 0
    for _, document in ini_files:
        sections += len(document.sections)
        for section in document.sections:
            for entry in section.entries:
                if entry.is_bare:
                    bare += 1
                else:
                    key_values += 1

    assert sections == SECTION_COUNT
    assert key_values == KEY_VALUE_COUNT
    assert bare == BARE_LINE_COUNT


def test_no_value_contains_a_semicolon(ini_files):
    """The one assumption the lexer rests on.

    `;` starts a comment unconditionally, with no quoting rule to respect. That
    holds only because no shipped value contains one. If a file ever does, this
    is the test that says so rather than the parser silently truncating it.
    """
    offenders = [
        f"{where} line {entry.line}: {entry.value!r}"
        for where, document in ini_files
        for section in document.sections
        for entry in section.entries
        if ";" in entry.value
    ]
    assert not offenders, offenders


def test_the_only_quoted_values_are_the_three_editor_placeholders(ini_files):
    """Three values are quoted, and they are all the same literal.

    The specification once claimed none were. They are `ImageType = "AAAAA"` in
    three of the editor's dialogs, the quotes are part of the value rather than
    delimiters, and none shares a line with a `;` -- so quoting never interacts
    with comment stripping in the shipped data.
    """
    quoted = [
        (entry.key, entry.value)
        for _, document in ini_files
        for section in document.sections
        for entry in section.entries
        if '"' in entry.value
    ]
    assert len(quoted) == 3, quoted
    assert {key for key, _ in quoted} == {"ImageType"}, quoted
    assert {value for _, value in quoted} == {'"AAAAA"'}, quoted


def test_duplicate_keys_are_still_confined_to_the_known_files(ini_files):
    """A section is not reliably a map, and this measures how much.

    `UNITICONS.INI`'s `[FillCombo]` gives the label `Republican Roman Hero 1` to
    three consecutive entries naming three different bitmaps, so a reader that
    resolved duplicates on the way in would lose data. Every entry is preserved;
    only `Section.get` breaks the tie, and it does so by taking the first.
    """
    duplicates = []
    for where, document in ini_files:
        for section in document.sections:
            seen: set[str] = set()
            for entry in section.entries:
                if entry.key is None:
                    continue
                key = entry.key.lower()
                if key in seen:
                    duplicates.append(f"{where} [{section.name}] {entry.key}")
                seen.add(key)

    assert len(duplicates) == DUPLICATE_KEY_COUNT, sorted(duplicates)


def test_a_repeated_key_keeps_every_entry(packs):
    """The `[FillCombo]` case, asserted directly rather than by counting."""
    for label, name, data in packs.entries_named(".INI"):
        if not name.upper().endswith("UNITICONS.INI"):
            continue
        section = ini.parse(data).section("FillCombo")
        assert section is not None, name
        icons = [e.value for e in section.entries if e.key == "Republican Roman Hero 1"]
        assert len(icons) == 3, icons
        assert len(set(icons)) == 3, icons
        # The lookup resolves to the first, which is the Win32 rule.
        assert section.get("Republican Roman Hero 1") == icons[0]
        return
    pytest.skip("UNITICONS.INI not present in this installation")


def test_placeholders_are_left_unexpanded(ini_files):
    """`%Name%` substitution is an interface-layer concern, not a reader's.

    Recorded as a measurement rather than a behaviour so that Part 6 knows the
    size of what it is taking on: 179 distinct names over 132 of the 150 files.
    """
    names: set[str] = set()
    files: set[str] = set()
    occurrences = 0
    for where, document in ini_files:
        for section in document.sections:
            for entry in section.entries:
                found = PLACEHOLDER.findall(entry.value)
                if found:
                    files.add(where)
                    names.update(found)
                    occurrences += len(found)

    assert occurrences == 4329
    assert len(names) == 179
    assert len(files) == 132
    assert "%TmplIni%" in names
