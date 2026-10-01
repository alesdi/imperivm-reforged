"""Corpus tests for the sprite reader. Specification: docs/formats/rle.md.

The strongest claim the project makes about `rle.mmp` is that it is nothing but
frame payloads: every byte of the 400 MB store belongs to exactly one frame of
exactly one of the 3,898 frame tables. That is what
:func:`test_frames_tile_the_pixel_store_exactly` checks, and it is a joint test
of the store, of every frame table, and of the offsets that link them.
"""

from __future__ import annotations

from dataclasses import dataclass

import pytest

from conftest import requires_game, requires_pixel_store
from imperivm.formats.rle import RleImage, spans

pytestmark = requires_game

IMAGE_COUNT = 3_898
FRAME_COUNT = 197_432

#: Size of `rle.mmp`, and therefore the end of the last frame payload.
STORE_SIZE = 400_221_427


@dataclass(frozen=True)
class Survey:
    images: int
    frames: int
    payloads: tuple[tuple[int, int], ...]  # (offset, size), sorted


@pytest.fixture(scope="module")
def survey(packs) -> Survey:
    """Parse and validate every `.rle.mmp` frame table in every pack."""
    images = frames = 0
    payloads: list[tuple[int, int]] = []
    for label, name, data in packs.entries_named(".RLE.MMP"):
        try:
            image = RleImage(data)
            image.validate()
        except Exception as exc:  # pragma: no cover - would be a real finding
            raise AssertionError(f"{label}:{name}: {exc}") from None
        images += 1
        frames += len(image.frames)
        payloads += [(f.data_offset, f.data_size) for f in image.frames if not f.empty]
    return Survey(images, frames, tuple(sorted(payloads)))


def test_every_image_parses_and_validates(survey):
    assert survey.images == IMAGE_COUNT


def test_total_frame_count(survey):
    assert survey.frames == FRAME_COUNT


@requires_pixel_store
def test_frames_tile_the_pixel_store_exactly(survey, pixel_store):
    """First payload at byte 0, no gaps, no overlaps, last ending at EOF."""
    assert survey.payloads[0][0] == 0

    cursor = 0
    for offset, size in survey.payloads:
        assert offset == cursor, f"payload at {offset} does not abut {cursor}"
        cursor = offset + size

    assert cursor == STORE_SIZE
    assert pixel_store.path.stat().st_size == STORE_SIZE


@requires_pixel_store
def test_a_sampled_frame_decompresses_against_the_store(packs, pixel_store):
    """Spot check that the offsets address real rows, not just plausible ranges.

    Row decoding is self-checking -- :func:`~imperivm.formats.rle.spans` raises
    unless every row consumes its bytes exactly and covers exactly `width`
    pixels -- so running it over a sample proves the payloads are where the
    frame tables say they are.
    """
    checked = 0
    for _label, _name, data in packs.entries_named(".RLE.MMP"):
        image = RleImage(data)
        for frame in image.frames:
            if frame.empty:
                continue
            covered = sum(length for _y, _x, length, _p in spans(frame, pixel_store.blob(frame)))
            assert 0 <= covered <= frame.width * frame.height
            checked += 1
            break
        if checked >= 200:
            break
    assert checked == 200
