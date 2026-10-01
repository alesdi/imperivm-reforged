"""Data-free tests for the sprite reader. Specification: docs/formats/rle.md."""

from __future__ import annotations

import struct

import pytest

import synthetic
from imperivm.formats import rle
from imperivm.formats.rle import (
    Frame,
    ImageClass,
    PixelFormat,
    PixelStore,
    RleError,
    RleImage,
)
from synthetic import INDEXED8, MASK, RGB555, SyntheticFrame

PALETTE = [(i, (i * 3) % 256, (i * 5) % 256) for i in range(256)]

# A 6x4 indexed frame with a transparent border and an interior hole.
GRID: list[list[int | None]] = [
    [None, 10, 10, 11, None, None],
    [None, 12, None, 13, 13, None],
    [14, 14, 14, None, None, None],
    [None, None, None, None, None, None],
]


def indexed_image(**kwargs):
    frames = [SyntheticFrame(left=5, top=7, rows=GRID)]
    return synthetic.build_rle_image(
        frames,
        columns=1,
        rows=1,
        image_class=ImageClass.INDEXED,
        pixel_format=INDEXED8,
        palette=PALETTE,
        color_key=200,
        **kwargs,
    )


# -- header and geometry -------------------------------------------------


def test_header_and_frame_geometry():
    table, _ = indexed_image()
    image = RleImage(table)
    image.validate()
    assert image.image_class == ImageClass.INDEXED
    assert (image.columns, image.rows) == (1, 1)
    assert len(image.frames) == 1

    frame = image.frames[0]
    assert (frame.left, frame.top) == (5, 7)
    assert (frame.width, frame.height) == (6, 4)
    assert frame.right == frame.left + frame.width - 1
    assert frame.bottom == frame.top + frame.height - 1
    assert frame.pixel_format is PixelFormat.INDEXED8
    assert len(image.palette) == 256
    assert image.lut is None


def test_frames_are_row_major_and_indexable():
    frames = [
        SyntheticFrame(left=c, top=r, rows=[[c + r * 10]])
        for r in range(3)
        for c in range(4)
    ]
    table, _ = synthetic.build_rle_image(
        frames,
        columns=4,
        rows=3,
        image_class=ImageClass.INDEXED,
        pixel_format=INDEXED8,
        palette=PALETTE,
    )
    image = RleImage(table)
    assert len(image.frames) == 12
    assert image.frame(2, 1) is image.frames[2 * 4 + 1]
    with pytest.raises(IndexError):
        image.frame(3, 0)
    with pytest.raises(IndexError):
        image.frame(0, 4)


def test_canvas_size_is_the_union_of_the_bounding_boxes():
    frames = [
        SyntheticFrame(left=0, top=0, rows=[[1, 1]]),
        SyntheticFrame(left=10, top=20, rows=[[1], [1], [1]]),
    ]
    table, _ = synthetic.build_rle_image(
        frames,
        columns=2,
        rows=1,
        image_class=ImageClass.INDEXED,
        pixel_format=INDEXED8,
        palette=PALETTE,
    )
    assert RleImage(table).canvas_size == (11, 23)


def test_empty_frames_carry_only_a_degenerate_box():
    frames = [
        SyntheticFrame(left=0, top=0, rows=GRID),
        SyntheticFrame(left=0, top=0, rows=[]),
    ]
    table, store = synthetic.build_rle_image(
        frames,
        columns=2,
        rows=1,
        image_class=ImageClass.INDEXED,
        pixel_format=INDEXED8,
        palette=PALETTE,
        empty_box_seed=317,
    )
    image = RleImage(table)
    image.validate()
    empty = image.frames[1]
    assert empty.empty
    assert empty.right < empty.left and empty.bottom < empty.top
    assert (empty.width, empty.height) == (0, 0)
    assert list(rle.spans(empty, b"")) == []
    assert rle.decode_rgba(image, empty, b"") == b""
    # An empty frame contributes nothing to the pixel store.
    assert len(store) == image.frames[0].data_size


def test_an_all_empty_image_has_a_zero_canvas():
    table, _ = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, [])],
        columns=1,
        rows=1,
        image_class=ImageClass.SHADOW,
        pixel_format=MASK,
    )
    assert RleImage(table).canvas_size == (0, 0)


# -- run decoding --------------------------------------------------------


def expected_rgba(grid, palette):
    out = bytearray()
    for row in grid:
        for value in row:
            if value is None:
                out += bytes(4)
            else:
                out += bytes((*palette[value], 255))
    return bytes(out)


def test_spans_cover_each_row_exactly():
    table, store = indexed_image()
    image = RleImage(table)
    frame = image.frames[0]
    covered = {}
    for y, x, length, payload in rle.spans(frame, store):
        assert len(payload) == length
        for i in range(length):
            covered[(y, x + i)] = payload[i]
    expected = {
        (y, x): value
        for y, row in enumerate(GRID)
        for x, value in enumerate(row)
        if value is not None
    }
    assert covered == expected


def test_indexed_frame_decodes_to_the_source_pixels():
    table, store = indexed_image()
    image = RleImage(table)
    frame = image.frames[0]
    assert rle.decode_rgba(image, frame, store) == expected_rgba(GRID, PALETTE)


def test_rgb555_frame_decodes_by_bit_expansion():
    grid = [[synthetic.rgb555(31, 0, 0), None], [None, synthetic.rgb555(0, 15, 31)]]
    table, store = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, grid)],
        columns=1,
        rows=1,
        image_class=ImageClass.TRUECOLOR,
        pixel_format=RGB555,
    )
    image = RleImage(table)
    image.validate()
    rgba = rle.decode_rgba(image, image.frames[0], store)
    assert rgba[0:4] == bytes((255, 0, 0, 255))
    assert rgba[4:8] == bytes(4)
    assert rgba[12:16] == bytes((0, 15 * 255 // 31, 255, 255))


def test_mask_frames_carry_no_payload_at_all():
    grid = [[1, 1, None, 1], [None, None, 1, None]]
    table, store = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, grid)],
        columns=1,
        rows=1,
        image_class=ImageClass.SHADOW,
        pixel_format=MASK,
    )
    image = RleImage(table)
    image.validate()
    frame = image.frames[0]
    assert all(payload == b"" for *_, payload in rle.spans(frame, store))
    assert sum(length for _, _, length, _ in rle.spans(frame, store)) == 4
    rgba = rle.decode_rgba(image, frame, store, shadow_color=(9, 9, 9))
    assert rgba[0:4] == bytes((9, 9, 9, 255))
    assert rgba[8:12] == bytes(4)


def test_gaps_wider_than_255_are_split_into_multiple_pairs():
    row: list[int | None] = [7] + [None] * 600 + [8]
    table, store = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, [row])],
        columns=1,
        rows=1,
        image_class=ImageClass.INDEXED,
        pixel_format=INDEXED8,
        palette=PALETTE,
    )
    image = RleImage(table)
    image.validate()
    spans = list(rle.spans(image.frames[0], store))
    assert [(x, length) for _, x, length, _ in spans] == [(0, 1), (601, 1)]


def test_runs_longer_than_255_are_split():
    row: list[int | None] = [None] * 3 + [9] * 700
    table, store = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, [row])],
        columns=1,
        rows=1,
        image_class=ImageClass.INDEXED,
        pixel_format=INDEXED8,
        palette=PALETTE,
    )
    image = RleImage(table)
    spans = list(rle.spans(image.frames[0], store))
    assert len(spans) > 1
    assert sum(length for _, _, length, _ in spans) == 700
    assert all(length <= 255 for _, _, length, _ in spans)


def test_row_offset_table_abuts_the_row_data():
    table, store = indexed_image()
    frame = RleImage(table).frames[0]
    offsets = rle.row_offsets(frame, store)
    assert offsets[0] == frame.height * frame.row_entry_size
    assert list(offsets) == sorted(offsets)


def test_wide_row_tables_use_u32_entries():
    """`wide_rows` exists only because a u16 offset cannot reach past 65,535."""
    rows = [[(x + y) % 256 for x in range(300)] for y in range(240)]
    table, store = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, rows, wide=True)],
        columns=1,
        rows=1,
        image_class=ImageClass.INDEXED,
        pixel_format=INDEXED8,
        palette=PALETTE,
    )
    image = RleImage(table)
    frame = image.frames[0]
    assert frame.wide_row_table
    assert frame.row_entry_size == 4
    assert frame.data_size > 0xFFFF
    image.validate()
    spans = list(rle.spans(frame, store))
    assert len(spans) == 480  # 300 pixels per row is two runs of at most 255
    assert sum(length for _, _, length, _ in spans) == 300 * 240


# -- palettes and the player-colour block --------------------------------


def test_class_2_palette_mirrors_slots_0_to_63():
    palette = synthetic.mirrored_palette()
    lut = [
        ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3) for r, g, b in palette
    ]
    lut[:64] = [0x6666] * 64  # the engine fills these per player at load time
    table, _ = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, GRID)],
        columns=1,
        rows=1,
        image_class=ImageClass.PLAYER_COLOR,
        pixel_format=INDEXED8,
        palette=palette,
        lut=lut,
    )
    image = RleImage(table)
    image.validate()
    assert image.palette[:64] == image.palette[64:128]
    assert image.lut is not None and len(image.lut) == 256
    assert image.lut[:64] == (0x6666,) * 64


def test_validate_rejects_a_class_2_palette_that_does_not_mirror():
    palette = synthetic.mirrored_palette()
    palette[0] = (1, 2, 3)
    table, _ = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, GRID)],
        columns=1,
        rows=1,
        image_class=ImageClass.PLAYER_COLOR,
        pixel_format=INDEXED8,
        palette=palette,
        lut=[0] * 256,
    )
    with pytest.raises(RleError, match="do not mirror"):
        RleImage(table).validate()


def test_player_colors_replace_only_the_first_64_slots():
    palette = synthetic.mirrored_palette()
    grid = [[0, 64, 200]]
    table, store = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, grid)],
        columns=1,
        rows=1,
        image_class=ImageClass.PLAYER_COLOR,
        pixel_format=INDEXED8,
        palette=palette,
        lut=[0] * 256,
    )
    image = RleImage(table)
    ramp = [(255, 0, 0)] * 64
    rgba = rle.decode_rgba(image, image.frames[0], store, player_colors=ramp)
    assert rgba[0:4] == bytes((255, 0, 0, 255))  # slot 0: tinted
    assert rgba[4:8] == bytes((*palette[64], 255))  # slot 64: untouched
    assert rgba[8:12] == bytes((*palette[200], 255))

    with pytest.raises(ValueError, match="64 player colours"):
        rle.decode_rgba(image, image.frames[0], store, player_colors=[(0, 0, 0)])


# -- error paths ---------------------------------------------------------


def test_rejects_bad_magic():
    with pytest.raises(RleError, match="magic mismatch"):
        RleImage(b"NOTRLE" + bytes(64))


def test_rejects_non_zero_reserved_header_words():
    table, _ = indexed_image()
    broken = bytearray(table)
    struct.pack_into("<I", broken, 0x0A, 1)
    with pytest.raises(RleError, match="reserved header words"):
        RleImage(bytes(broken))


def test_rejects_a_missing_rle2_tag():
    table, _ = indexed_image()
    broken = bytearray(table)
    broken[0x1A + 16 : 0x1A + 20] = b"XXXX"
    with pytest.raises(RleError, match="expected RLE2"):
        RleImage(bytes(broken))


def test_rejects_a_missing_pamm_tag():
    table, _ = indexed_image()
    broken = bytearray(table)
    broken[0x1A + 46 : 0x1A + 50] = b"XXXX"
    with pytest.raises(RleError, match="expected pamm"):
        RleImage(bytes(broken))


def test_rejects_a_bounding_box_that_disagrees_with_the_size():
    table, _ = indexed_image()
    broken = bytearray(table)
    struct.pack_into("<I", broken, 0x1A + 8, 99)  # right
    with pytest.raises(RleError, match="bounding box disagrees"):
        RleImage(bytes(broken))


def test_rejects_a_non_zero_reserved_frame_word():
    table, _ = indexed_image()
    broken = bytearray(table)
    struct.pack_into("<I", broken, 0x1A + 34, 7)
    with pytest.raises(RleError, match="reserved word is not zero"):
        RleImage(bytes(broken))


def test_rejects_a_bad_wide_row_flag():
    table, _ = indexed_image()
    broken = bytearray(table)
    struct.pack_into("<I", broken, 0x1A + 38, 2)
    with pytest.raises(RleError, match="bad wide-row-table flag"):
        RleImage(bytes(broken))


def test_rejects_unexplained_trailing_bytes():
    table, _ = indexed_image()
    with pytest.raises(RleError, match="unexplained trailing bytes"):
        RleImage(table + b"junk")


def test_rejects_a_palette_that_overruns_the_table():
    table, _ = indexed_image()
    broken = bytearray(table)
    palette_at = len(table) - 4 - 256 * 4
    struct.pack_into("<I", broken, palette_at, 4096)
    with pytest.raises(RleError, match="palette overruns"):
        RleImage(bytes(broken))


def test_validate_rejects_mixed_pixel_formats():
    frames = [SyntheticFrame(0, 0, [[1, 2]]), SyntheticFrame(0, 0, [[3, 4]])]
    table, _ = synthetic.build_rle_image(
        frames,
        columns=2,
        rows=1,
        image_class=ImageClass.INDEXED,
        pixel_format=INDEXED8,
        palette=PALETTE,
    )
    image = RleImage(table)
    first = image.frames[0]
    image.frames[0] = Frame(
        first.index,
        first.left,
        first.top,
        first.right,
        first.bottom,
        first.width,
        first.height,
        PixelFormat.RGB555,
        first.data_size,
        first.data_offset,
        first.color_key,
        first.wide_row_table,
    )
    with pytest.raises(RleError, match="mixed pixel formats"):
        image.validate()


def test_validate_rejects_a_colour_key_outside_the_palette():
    table, _ = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, [[0, 1]])],
        columns=1,
        rows=1,
        image_class=ImageClass.INDEXED,
        pixel_format=INDEXED8,
        palette=PALETTE[:8],
        color_key=200,
    )
    with pytest.raises(RleError, match="colour key outside the palette"):
        RleImage(table).validate()


def test_validate_rejects_8_bit_pixels_without_a_palette():
    table, _ = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, [[0, 1]])],
        columns=1,
        rows=1,
        image_class=ImageClass.INDEXED,
        pixel_format=INDEXED8,
        palette=[],
    )
    with pytest.raises(RleError, match="8-bit pixels but no palette"):
        RleImage(table).validate()


def test_validate_rejects_a_wide_flag_that_disagrees_with_the_data_size():
    table, _ = synthetic.build_rle_image(
        [SyntheticFrame(0, 0, GRID, wide=True)],
        columns=1,
        rows=1,
        image_class=ImageClass.INDEXED,
        pixel_format=INDEXED8,
        palette=PALETTE,
    )
    with pytest.raises(RleError, match="wide flag disagrees"):
        RleImage(table).validate()


def test_rejects_a_row_table_that_does_not_abut_the_row_data():
    table, store = indexed_image()
    frame = RleImage(table).frames[0]
    broken = bytearray(store)
    struct.pack_into("<H", broken, 0, 2)
    with pytest.raises(RleError, match="does not abut"):
        rle.row_offsets(frame, bytes(broken))


def test_rejects_a_non_monotonic_row_table():
    table, store = indexed_image()
    frame = RleImage(table).frames[0]
    broken = bytearray(store)
    struct.pack_into("<H", broken, 2, 0)
    with pytest.raises(RleError, match="not monotonic"):
        rle.row_offsets(frame, bytes(broken))


def test_rejects_a_row_that_does_not_cover_the_full_width():
    table, store = indexed_image()
    image = RleImage(table)
    frame = image.frames[0]
    broken = bytearray(store)
    broken[frame.height * 2] = 0  # shrink row 0's leading gap
    with pytest.raises(RleError, match="covers 5 of 6"):
        list(rle.spans(frame, bytes(broken)))


def test_pixel_store_rejects_a_payload_past_the_end_of_the_file(tmp_path):
    path = tmp_path / "rle.mmp"
    path.write_bytes(b"\0" * 32)
    frame = Frame(0, 0, 0, 1, 1, 2, 2, PixelFormat.INDEXED8, 64, 0, 0, False)
    with PixelStore(path) as store:
        with pytest.raises(RleError, match="overruns"):
            store.blob(frame)


def test_pixel_store_returns_the_exact_payload(tmp_path):
    table, blob = indexed_image()
    path = tmp_path / "rle.mmp"
    path.write_bytes(b"\xff" * 16 + blob)
    image = RleImage(table)
    frame = image.frames[0]
    shifted = Frame(
        frame.index,
        frame.left,
        frame.top,
        frame.right,
        frame.bottom,
        frame.width,
        frame.height,
        frame.pixel_format,
        frame.data_size,
        16,
        frame.color_key,
        frame.wide_row_table,
    )
    with PixelStore(path) as store:
        assert store.blob(shifted) == blob
        assert store.blob(Frame(1, 5, 5, 0, 0, 0, 0, None, 0, 0, 0, False)) == b""
