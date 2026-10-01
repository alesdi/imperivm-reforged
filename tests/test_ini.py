"""Data-free tests for the INI reader. Specification: docs/formats/ini.md.

Every fixture reproduces a shape that occurs in the shipped files. The dialect
was measured before the reader was written; `test_corpus_ini.py` asserts the
measurements against a real installation.
"""

from __future__ import annotations

import pytest

from imperivm.formats import ini


def test_parses_sections_and_pairs():
    doc = ini.parse("[First]\nAlpha = 1\nBeta=two\n\n[Second]\nGamma = 3\n")
    assert [s.name for s in doc.sections] == ["First", "Second"]
    first = doc.section("First")
    assert first.get("Alpha") == "1"
    assert first.get("Beta") == "two"
    assert first.get_int("Alpha") == 1


def test_looks_up_without_regard_to_case():
    doc = ini.parse("[SquadStates]\nKey = 7\n")
    assert doc.section("squadstates") is not None
    assert doc.section("SQUADSTATES").get("key") == "7"


def test_keeps_bare_lines_as_ordered_entries():
    # `[SquadStates]` in DATA/AI/AI.INI is an ordered enum declaration: a line's
    # position is the constant's value.
    doc = ini.parse("[SquadStates]\nSS_Approach\nSS_Wait\nSS_ApproachWait\n")
    section = doc.section("SquadStates")
    assert section.names == ["SS_Approach", "SS_Wait", "SS_ApproachWait"]
    # A bare line must never answer a lookup by key, or an enum member would
    # shadow a variable of the same name.
    assert section.get("SS_Wait") is None


def test_distinguishes_a_bare_line_from_an_empty_value():
    doc = ini.parse("[S]\nBare\nEmpty =\n")
    entries = doc.section("S").entries
    assert entries[0].is_bare
    assert not entries[1].is_bare
    assert entries[1].key == "Empty"
    assert entries[1].value == ""


def test_strips_comments_from_values_and_from_headers():
    # Twelve headers in the shipped corpus carry a trailing comment.
    doc = ini.parse(
        "; whole-line\n"
        "[SingleLineEdit] ; A single line edit :)\n"
        "PopGroup=5 ; peasants come in groups of this many\n"
    )
    assert [s.name for s in doc.sections] == ["SingleLineEdit"]
    assert doc.section("SingleLineEdit").get("PopGroup") == "5"


def test_does_not_treat_hash_or_slashes_as_comments():
    doc = ini.parse("[S]\nA = # not a comment\nB = // neither\n")
    assert doc.section("S").get("A") == "# not a comment"
    assert doc.section("S").get("B") == "// neither"


def test_rejects_an_unterminated_section_header():
    with pytest.raises(ini.IniError):
        ini.parse("[Unterminated\nA = 1\n")


def test_takes_the_first_of_a_duplicated_key():
    # `[Vars.All]` in AI.INI declares AIV_SquanderGoldAmount twice with
    # different values. Win32's GetPrivateProfileString returns the first match;
    # see the specification, where this is recorded as inferred.
    doc = ini.parse(
        "[Vars.All]\n"
        "AIV_SquanderGoldAmount=30000 ; without restrictions\n"
        "AIV_SquanderGoldAmount=15000 ; for research\n"
    )
    section = doc.section("Vars.All")
    assert len(section.entries) == 2  # both preserved; the lookup resolves
    assert section.get_int("AIV_SquanderGoldAmount") == 30000


def test_get_int_falls_back_rather_than_truncating():
    # ProductionInterval was recorded as 20 when it is 2000, because a value was
    # read part-way. A parser that stops at the first non-digit makes that silent.
    doc = ini.parse("[S]\nGood = 2000\nBad = 20abc\n")
    section = doc.section("S")
    assert section.get_int("Good") == 2000
    assert section.get_int("Bad", -1) == -1
    assert section.get_int("Missing", -1) == -1


def test_splits_comma_lists_and_keeps_holes():
    assert ini.split_list("a, b ,c") == ["a", "b", "c"]
    assert ini.split_list("a,,c") == ["a", "", "c"]
    assert ini.split_list("") == []
    assert ini.split_list("solo") == ["solo"]


def test_handles_crlf_and_a_missing_trailing_newline():
    doc = ini.parse("[S]\r\nA = 1\r\nB = 2")
    assert doc.section("S").get("A") == "1"
    assert doc.section("S").get("B") == "2"


def test_puts_entries_before_any_header_in_an_implicit_section():
    doc = ini.parse("Loose = 1\n[S]\nA = 2\n")
    assert doc.sections[0].name == ""
    assert doc.sections[0].get("Loose") == "1"


def test_reports_one_based_line_numbers():
    doc = ini.parse("\n[S]\n\nA = 1\n")
    assert doc.sections[0].line == 2
    assert doc.sections[0].entries[0].line == 4


def test_decodes_windows_1252():
    doc = ini.parse("[S]\nName = caf\xe9\n".encode("cp1252"))
    assert doc.section("S").get("Name") == "café"
