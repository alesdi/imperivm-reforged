"""Data-free tests for the bitmap font reader. Specification: docs/formats/apf.md."""

from __future__ import annotations

import struct

import pytest

import synthetic
from imperivm.formats.apf import ALPHA_LEVELS, ApfFont, FontError, write_png
from imperivm.png import ColorType, decode
from synthetic import SyntheticGlyph

# Codes 0x41..0x44: a solid block, a ramp, a blank, and a one-pixel dot.
GLYPHS = [
    SyntheticGlyph(0x41, abc_a=1, abc_b=3, abc_c=1, top=2, bottom=4, coverage=[7] * 9),
    SyntheticGlyph(
        0x42,
        abc_a=0,
        abc_b=4,
        abc_c=2,
        top=1,
        bottom=2,
        coverage=[0, 1, 2, 3, 4, 5, 6, 7],
    ),
    SyntheticGlyph(0x43, abc_a=2, abc_b=0, abc_c=3, top=5, bottom=4, coverage=[]),
    SyntheticGlyph(0x44, abc_a=0, abc_b=1, abc_c=0, top=0, bottom=0, coverage=[4]),
]

KERNING = [(0x41, 0x42, -1), (0x42, 0x41, 2)]


def font(**kwargs) -> ApfFont:
    return ApfFont(synthetic.build_apf(GLYPHS, kerning=KERNING, **kwargs), "test.apf")


# -- structure -----------------------------------------------------------


def test_header_metrics_and_names():
    f = font()
    f.validate()
    assert f.face_name == "Testa"
    assert f.family_name == "Testa"
    assert f.point_size == 13
    assert (f.italic, f.bold) == (0, 0)
    assert f.metrics.height == f.metrics.ascent + f.metrics.descent == 13
    assert f.metrics.max_char_width == 4
    assert len(f.metrics.unknown) == 7


def test_ranges_and_glyph_table():
    f = font()
    assert len(f.ranges) == 1
    only = f.ranges[0]
    assert only.first == 0x41 == f.first_char
    assert only.count == 4
    assert only.last == 0x44
    assert len(f) == 4
    assert all(code in f for code in (0x41, 0x42, 0x43, 0x44))
    assert 0x45 not in f


def test_ranges_end_exactly_on_the_last_byte_of_the_file():
    data = synthetic.build_apf(GLYPHS, kerning=KERNING)
    f = ApfFont(data, "test.apf")
    cursor = f.ranges[0].offset
    for char_range in f.ranges:
        assert char_range.offset == cursor
        cursor += char_range.size
    assert cursor == len(data)


def test_run_lengths_sum_to_width_times_height_for_every_glyph():
    f = font()
    for code, glyph in f.glyphs.items():
        coverage = f.coverage(code)
        assert len(coverage) == glyph.width * glyph.height
        assert all(0 <= value < ALPHA_LEVELS for value in coverage)


def test_coverage_round_trips_the_alpha_ramp():
    f = font()
    assert f.coverage(0x41) == [7] * 9
    assert f.coverage(0x42) == [0, 1, 2, 3, 4, 5, 6, 7]
    assert f.coverage(0x43) == []
    assert f.coverage(0x44) == [4]


def test_runs_may_cross_row_boundaries():
    """The stream is flat over the ink box, not row by row."""
    glyph = SyntheticGlyph(0x41, 0, 3, 0, 0, 2, coverage=[5] * 9)
    encoded = synthetic.encode_apf_coverage(glyph.coverage)
    assert encoded == bytes(((5 << 5) | 8,)), "nine identical pixels are one run"
    f = ApfFont(synthetic.build_apf([glyph]), "one.apf")
    f.validate()
    assert f.coverage(0x41) == [5] * 9


def test_runs_longer_than_32_are_split():
    coverage = [3] * 100
    glyph = SyntheticGlyph(0x41, 0, 10, 0, 0, 9, coverage=coverage)
    f = ApfFont(synthetic.build_apf([glyph]), "one.apf")
    f.validate()
    assert f.coverage(0x41) == coverage
    assert len(f.glyphs[0x41].data) == 4  # 100 pixels in runs of at most 32


def test_a_blank_glyph_stores_no_pixels():
    f = font()
    blank = f.glyphs[0x43]
    assert blank.is_blank
    assert blank.height == 0
    assert blank.data == b""


def test_advance_and_kerning():
    f = font()
    assert f.advance(0x41) == 1 + 3 + 1
    assert f.kern(0x41, 0x42) == -1
    assert f.kern(0x42, 0x41) == 2
    assert f.kern(0x41, 0x41) == 0
    assert f.text_width("AB") == f.advance(0x41) + f.advance(0x42) - 1
    assert f.text_width("") == 0


def test_render_lays_out_a_string_on_the_cell():
    f = font()
    width, height, buffer = f.render("AD")
    assert height == f.metrics.height
    assert width == f.text_width("AD")
    assert len(buffer) == width * height
    # 'A' is solid coverage 7, which scales to full intensity.
    assert buffer[2 * width + 1] == 7 * 255 // (ALPHA_LEVELS - 1) == 255
    # 'D' is a single pixel of coverage 4 at the top left of its own advance.
    assert buffer[0 * width + f.advance(0x41)] == 4 * 255 // (ALPHA_LEVELS - 1)


def test_render_writes_a_greyscale_png(tmp_path):
    f = font()
    width, height, buffer = f.render("AB")
    path = tmp_path / "text.png"
    write_png(path, width, height, buffer)
    image = decode(path.read_bytes())
    assert image.color_type is ColorType.GRAY
    assert (image.width, image.height) == (width, height)
    assert image.pixels == bytes(buffer)


# -- error paths ---------------------------------------------------------


def test_rejects_bad_magic():
    with pytest.raises(FontError, match="magic mismatch"):
        ApfFont(b"FCBA" + bytes(64), "bad.apf")


def test_rejects_a_range_that_overruns_the_file():
    data = bytearray(synthetic.build_apf(GLYPHS))
    metrics_offset = struct.unpack_from("<i", data, 4)[0]
    table = metrics_offset + 14 * 4 + 4
    struct.pack_into("<I", data, table + 4, 1 << 20)  # size
    with pytest.raises(FontError, match="overruns the file"):
        ApfFont(bytes(data), "bad.apf")


def test_rejects_a_glyph_table_of_the_wrong_length():
    data = bytearray(synthetic.build_apf(GLYPHS))
    metrics_offset = struct.unpack_from("<i", data, 4)[0]
    table = metrics_offset + 14 * 4 + 4
    block = struct.unpack_from("<I", data, table)[0]
    struct.pack_into("<I", data, block, 999)  # kern_offset
    with pytest.raises(FontError, match="glyph table of"):
        ApfFont(bytes(data), "bad.apf")


def test_rejects_a_misplaced_pixel_section():
    data = bytearray(synthetic.build_apf(GLYPHS, kerning=KERNING))
    metrics_offset = struct.unpack_from("<i", data, 4)[0]
    table = metrics_offset + 14 * 4 + 4
    block = struct.unpack_from("<I", data, table)[0]
    struct.pack_into("<I", data, block + 8, 4)  # pixel_offset
    with pytest.raises(FontError, match="misplaced pixel section"):
        ApfFont(bytes(data), "bad.apf")


def test_rejects_a_block_that_does_not_fill_its_range():
    data = bytearray(synthetic.build_apf(GLYPHS))
    metrics_offset = struct.unpack_from("<i", data, 4)[0]
    table = metrics_offset + 14 * 4 + 4
    block = struct.unpack_from("<I", data, table)[0]
    struct.pack_into("<I", data, block + 12, 1)  # pixel_size
    with pytest.raises(FontError, match="does not fill its range"):
        ApfFont(bytes(data), "bad.apf")


def test_validate_rejects_height_that_is_not_ascent_plus_descent():
    data = bytearray(synthetic.build_apf(GLYPHS))
    metrics_offset = struct.unpack_from("<i", data, 4)[0]
    struct.pack_into("<i", data, metrics_offset, 99)
    with pytest.raises(FontError, match="height is not ascent \\+ descent"):
        ApfFont(bytes(data), "bad.apf").validate()


def test_validate_rejects_a_first_char_that_disagrees_with_range_0():
    data = bytearray(synthetic.build_apf(GLYPHS))
    struct.pack_into("<i", data, 0x10, 0x20)
    with pytest.raises(FontError, match="first_char disagrees"):
        ApfFont(bytes(data), "bad.apf").validate()


def test_validate_rejects_a_widest_glyph_that_disagrees_with_the_metrics():
    data = bytearray(synthetic.build_apf(GLYPHS))
    metrics_offset = struct.unpack_from("<i", data, 4)[0]
    struct.pack_into("<i", data, metrics_offset + 4, 40)  # max_char_width
    with pytest.raises(FontError, match="widest glyph is"):
        ApfFont(bytes(data), "bad.apf").validate()


def test_validate_rejects_run_lengths_that_do_not_fill_the_ink_box():
    data = bytearray(synthetic.build_apf(GLYPHS))
    metrics_offset = struct.unpack_from("<i", data, 4)[0]
    table = metrics_offset + 14 * 4 + 4
    block = struct.unpack_from("<I", data, table)[0]
    # Widen the first glyph's ink box without touching its coverage stream.
    struct.pack_into("<i", data, block + 16 + 4, 5)  # abc_b
    with pytest.raises(FontError, match="decodes to 9 pixels, expected 15"):
        ApfFont(bytes(data), "bad.apf").validate()


def test_validate_rejects_blankness_that_disagrees_with_the_stored_bytes():
    """A glyph is blank exactly when `bottom < top`, not when its box is empty."""
    glyphs = list(GLYPHS)
    # Zero ink width but a two-row ink box: no pixels are stored, yet the glyph
    # does not declare itself blank.
    glyphs[2] = SyntheticGlyph(0x43, 2, 0, 3, top=0, bottom=1, coverage=[])
    data = synthetic.build_apf(glyphs)
    with pytest.raises(FontError, match="blankness disagrees with data"):
        ApfFont(data, "bad.apf").validate()
