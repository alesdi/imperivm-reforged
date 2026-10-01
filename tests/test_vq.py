"""Data-free tests for the terrain texture reader. Specification: docs/formats/vq.md."""

from __future__ import annotations

import struct

import pytest

import synthetic
from imperivm.formats.vq import HEADER_SIZE, VQImage, VQImageError
from imperivm.png import ColorType, decode
from synthetic import rgb555

RED = rgb555(31, 0, 0)
GREEN = rgb555(0, 31, 0)
BLUE = rgb555(0, 0, 31)
BLACK = 0

CODEBOOK = [
    (RED, RED, RED, RED),
    (GREEN, GREEN, GREEN, GREEN),
    (BLUE, BLACK, BLUE, BLACK),
    (BLACK, BLACK, BLACK, BLACK),
]


def build(indices=None, blocks_x=2, blocks_y=3, **kwargs):
    if indices is None:
        indices = [0, 1, 2, 3, 1, 0]
    return synthetic.build_vq(
        CODEBOOK, indices, blocks_x=blocks_x, blocks_y=blocks_y, **kwargs
    )


# -- header arithmetic ---------------------------------------------------


def test_header_arithmetic_lands_exactly_on_the_file_size():
    data = build()
    image = VQImage(data)
    image.validate()
    header = image.header
    assert header.expected_size == len(data)
    assert HEADER_SIZE + header.codebook_bytes + header.index_count * header.index_size == len(
        data
    )
    assert (header.width, header.height) == (8, 3)
    assert header.blocks_x * header.block_width == header.width
    assert header.blocks_y * header.block_height == header.height


def test_quantisation_unit_is_four_horizontally_adjacent_pixels():
    image = VQImage(build())
    assert (image.header.block_width, image.header.block_height) == (4, 1)
    assert image.header.bytes_per_pixel == 2
    assert image.header.codebook_bytes == len(CODEBOOK) * 8


def test_two_byte_indices():
    codebook = [(i, i, i, i) for i in range(512)]
    data = synthetic.build_vq(codebook, [0, 511, 256, 7], blocks_x=2, blocks_y=2)
    image = VQImage(data)
    image.validate()
    assert image.header.index_size == 2
    assert image.header.index_size_log2 == 1
    assert image.header.expected_size == len(data)


# -- decoding ------------------------------------------------------------


def test_samples_expand_row_major():
    image = VQImage(build())
    samples = image.samples()
    assert len(samples) == image.width * image.height
    assert samples[:8] == [RED] * 4 + [GREEN] * 4
    assert samples[8:16] == [BLUE, BLACK, BLUE, BLACK] + [BLACK] * 4


def test_rgb_expands_by_bit_replication():
    image = VQImage(build())
    rgb = image.rgb()
    assert len(rgb) == image.width * image.height * 3
    assert bytes(rgb[0:3]) == bytes((255, 0, 0))
    assert bytes(rgb[12:15]) == bytes((0, 255, 0))
    # 5 bits of 0b10000 replicate to 0b10000100 = 132, not 128.
    dim = synthetic.build_vq(
        [(rgb555(16, 0, 0),) * 4], [0], blocks_x=1, blocks_y=1
    )
    assert bytes(VQImage(dim).rgb()[0:3]) == bytes((132, 0, 0))


def test_to_png_writes_a_decodable_rgb_image(tmp_path):
    image = VQImage(build())
    path = tmp_path / "terrain.png"
    image.to_png(path)
    decoded = decode(path.read_bytes())
    assert decoded.color_type is ColorType.RGB
    assert (decoded.width, decoded.height) == (image.width, image.height)
    assert decoded.pixels == bytes(image.rgb())


def test_animated_water_strip_shape():
    """DWATER/SWATER are 440x4800: 15 frames of 440x320 stacked vertically."""
    codebook = [(0, 0, 0, 0)]
    data = synthetic.build_vq(codebook, [0] * (110 * 4800), blocks_x=110, blocks_y=4800)
    image = VQImage(data)
    image.validate()
    assert (image.width, image.height) == (440, 4800)
    assert image.height % 15 == 0 and image.height // 15 == 320


# -- validation and error paths ------------------------------------------


def test_rejects_bad_magic():
    with pytest.raises(VQImageError, match="magic mismatch"):
        VQImage(b"vqbm" + bytes(64))


def test_rejects_a_truncated_header():
    with pytest.raises(VQImageError, match="truncated header"):
        VQImage(b"mbqv" + bytes(8))


def test_rejects_a_header_that_does_not_predict_the_file_size():
    with pytest.raises(VQImageError, match="header describes"):
        VQImage(build() + b"\0\0")


def test_validate_rejects_index_size_disagreeing_with_its_log():
    data = bytearray(build())
    struct.pack_into("<I", data, 4, 1)  # index_size_log2 = 1, index_size stays 1
    with pytest.raises(VQImageError, match="disagrees with"):
        VQImage(bytes(data)).validate()


def test_validate_rejects_a_codebook_length_that_is_not_a_power_of_two():
    data = synthetic.build_vq(CODEBOOK[:3], [0, 1, 2, 0], blocks_x=2, blocks_y=2)
    with pytest.raises(VQImageError, match="is not a power of two"):
        VQImage(data).validate()


def test_validate_rejects_unexpected_block_geometry():
    codebook = [(1, 2, 3, 4, 5, 6, 7, 8)]
    data = synthetic.build_vq(
        codebook, [0, 0], blocks_x=2, blocks_y=1, block_width=4, block_height=2
    )
    with pytest.raises(VQImageError, match="unexpected block geometry"):
        VQImage(data).validate()


def test_validate_rejects_blocks_that_do_not_cover_the_declared_size():
    data = bytearray(build())
    struct.pack_into("<I", data, 0x1C, 12)  # width, no longer 4 * blocks_x
    with pytest.raises(VQImageError, match="does not cover width"):
        VQImage(bytes(data)).validate()


def test_validate_rejects_an_index_past_the_end_of_the_codebook():
    data = build(indices=[0, 1, 2, 3, 1, 9])
    with pytest.raises(VQImageError, match="exceeds codebook"):
        VQImage(data).validate()


def test_validate_rejects_a_codebook_sample_with_bit_15_set():
    """The X1R5G5B5 finding: the top bit is never used in retail data."""
    codebook = [(0x8000, 0, 0, 0)]
    data = synthetic.build_vq(codebook, [0], blocks_x=1, blocks_y=1)
    with pytest.raises(VQImageError, match="sets bit 15"):
        VQImage(data).validate()


def test_rejects_an_unsupported_index_size():
    data = bytearray(build())
    struct.pack_into("<I", data, 0x0C, 4)
    data += b"\0" * 18  # keep the header arithmetic consistent with 4-byte indices
    with pytest.raises(VQImageError, match="unsupported index size"):
        VQImage(bytes(data))
