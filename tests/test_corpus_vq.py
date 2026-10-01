"""Corpus tests for the terrain texture reader. Specification: docs/formats/vq.md."""

from __future__ import annotations

import pytest

from conftest import requires_game
from imperivm.formats.vq import HEADER_SIZE, VQImage

pytestmark = requires_game

TEXTURE_COUNT = 55


@pytest.fixture(scope="module")
def textures(packs) -> list[tuple[str, VQImage, int]]:
    """Every `.vq` in the installation, parsed and validated, with its file size."""
    out: list[tuple[str, VQImage, int]] = []
    for label, name, data in packs.entries_named(".VQ"):
        try:
            image = VQImage(data, name)
            image.validate()
        except Exception as exc:  # pragma: no cover - would be a real finding
            raise AssertionError(f"{label}:{name}: {exc}") from None
        out.append((f"{label}:{name}", image, len(data)))
    return out


def test_every_texture_parses_and_validates(textures):
    assert len(textures) == TEXTURE_COUNT


def test_header_arithmetic_lands_exactly_on_the_file_size(textures):
    for label, image, size in textures:
        header = image.header
        codebook = (
            header.codebook_len
            * header.block_width
            * header.block_height
            * header.bytes_per_pixel
        )
        indices = header.blocks_x * header.blocks_y * header.index_size
        assert HEADER_SIZE + codebook + indices == header.expected_size == size, label


def test_no_codebook_sample_sets_bit_15(textures):
    """Samples are X1R5G5B5: the top bit is unused and always clear."""
    for label, image, _size in textures:
        for entry in image.codebook:
            for sample in entry:
                assert not sample & 0x8000, f"{label}: {sample:#06x}"


def test_every_texture_decodes_to_rgb(textures):
    for label, image, _size in textures:
        rgb = image.rgb()
        assert len(rgb) == image.width * image.height * 3, label
        assert image.width and image.height, label
