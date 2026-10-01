"""Corpus tests for the bitmap font reader. Specification: docs/formats/apf.md."""

from __future__ import annotations

import pytest

from conftest import requires_game
from imperivm.formats.apf import ApfFont

pytestmark = requires_game

FONT_COUNT = 6


@pytest.fixture(scope="module")
def fonts(packs) -> list[tuple[str, ApfFont]]:
    out: list[tuple[str, ApfFont]] = []
    for label, name, data in packs.entries_named(".APF"):
        try:
            font = ApfFont(data, name)
            font.validate()
        except Exception as exc:  # pragma: no cover - would be a real finding
            raise AssertionError(f"{label}:{name}: {exc}") from None
        out.append((f"{label}:{name}", font))
    return out


def test_every_font_parses_and_validates(fonts):
    assert len(fonts) == FONT_COUNT


def test_run_lengths_fill_every_glyph_box_exactly(fonts):
    """A glyph's RLE stream decodes to exactly `width * height` coverage values.

    The stream is a flat run-length code over the whole ink box, with runs free
    to cross row boundaries, so the only thing that pins it down is the total.
    """
    glyphs = 0
    for label, font in fonts:
        for code, glyph in font.glyphs.items():
            covered = sum((byte & 0x1F) + 1 for byte in glyph.data)
            assert covered == glyph.width * glyph.height, f"{label}: U+{code:04X}"
            assert len(font.coverage(code)) == glyph.width * glyph.height
            glyphs += 1
    assert glyphs > 0


def test_blank_glyphs_carry_no_data(fonts):
    for label, font in fonts:
        for code, glyph in font.glyphs.items():
            assert glyph.is_blank == (len(glyph.data) == 0), f"{label}: U+{code:04X}"
