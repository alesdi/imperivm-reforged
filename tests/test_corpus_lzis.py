"""Corpus tests for the LZIS decompressor. Specification: docs/formats/lzis.md.

Three streams, chosen because each proves something different: `config.ini` is
tiny and lands in text, `Packs/RandomMap.pak` is 45 MB across many chunks and
lands in another container format entirely, and
`Packs/RandomMapSettlements.bfhp` lands in a block filesystem that then has to
walk. Decompressing to the right *length* is easy; decompressing to something a
second parser accepts is not.
"""

from __future__ import annotations

import corpus
from conftest import requires_game
from imperivm.formats import lzis
from imperivm.formats.bfhp import MAGIC as HPFS_MAGIC
from imperivm.formats.bfhp import BlockFile
from imperivm.formats.pak import MAGIC as PACK_MAGIC
from imperivm.formats.pak import PackFile

pytestmark = requires_game

RANDOM_MAP_SIZE = 47_192_703
RANDOM_MAP_ENTRIES = 90
SETTLEMENT_ENTRIES = 39


def test_config_ini_decompresses_to_text(game_dir):
    raw = (game_dir / "config.ini").read_bytes()
    assert raw[: len(lzis.MAGIC)] == lzis.MAGIC

    plain = lzis.decompress(raw)
    header = lzis.parse_header(raw)
    assert len(plain) == header.uncompressed_size
    assert plain.startswith(b"[system]")
    plain.decode("cp1252")  # raises if it is not text at all


def test_random_map_pak_decompresses_to_a_parseable_pack(game_dir, tmp_path):
    image = corpus.random_map_image(game_dir)
    assert len(image) == RANDOM_MAP_SIZE
    assert image[: len(PACK_MAGIC)] == PACK_MAGIC

    path = tmp_path / "RandomMap.pak"
    path.write_bytes(image)
    pack = PackFile(path)
    pack.validate()
    assert len(pack) == RANDOM_MAP_ENTRIES


def test_random_map_settlements_decompresses_to_a_walkable_container(game_dir):
    raw = (game_dir / "Packs" / "RandomMapSettlements.bfhp").read_bytes()
    assert raw[: len(lzis.MAGIC)] == lzis.MAGIC

    image = lzis.decompress(raw)
    assert image[: len(HPFS_MAGIC)] == HPFS_MAGIC

    container = BlockFile.from_bytes(image, "RandomMapSettlements.bfhp")
    container.validate()
    assert len(container.entries) == SETTLEMENT_ENTRIES
