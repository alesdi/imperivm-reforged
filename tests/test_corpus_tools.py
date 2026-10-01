"""Round-trip tests for the archive tools, against a retail installation.

Reading a format proves a theory is *consistent* with the bytes. Writing one
back proves it is *complete*: every field the original writer emitted has to be
reproduced, including the ones a reader is free to ignore. Both tools claim
byte-for-byte reproduction, and that claim is only worth anything if something
checks it against the shipped files.

The tools are imported defensively so this module skips, rather than errors,
if a CLI is not part of the installed package.
"""

from __future__ import annotations

import contextlib
import io
import tempfile
from pathlib import Path

import pytest

import corpus
from conftest import requires_game

impk = pytest.importorskip("imperivm.cli.impk", reason="impk is not installed")
imfs = pytest.importorskip("imperivm.cli.imfs", reason="imfs is not installed")

pytestmark = requires_game


def run(parser, argv) -> None:
    """Drive one subcommand. The tools print progress; only the bytes matter."""
    args = parser.parse_args(argv)
    with contextlib.redirect_stdout(io.StringIO()):
        code = args.func(args) if hasattr(args, "func") else args.run(args)
    assert code in (0, None), f"{argv} exited with {code}"


def roundtrip_pack(source, work) -> bytes:
    """Extract `source` to a directory tree and build a new pack from it."""
    tree, rebuilt = work / "tree", work / "rebuilt.pak"
    run(impk.build_parser(), ["extract", str(source), "--out", str(tree)])
    run(impk.build_parser(), ["create", str(tree), str(rebuilt)])
    return rebuilt.read_bytes()


def roundtrip_container(source, work) -> bytes:
    """Extract `source` to a directory tree and build a new container from it."""
    tree, rebuilt = work / "tree", work / "rebuilt.bfhp"
    run(imfs.parser(), ["extract", str(source), "--out", str(tree)])
    run(imfs.parser(), ["create", str(tree), str(rebuilt)])
    return rebuilt.read_bytes()


# -- impk ----------------------------------------------------------------


def test_impk_reproduces_a_small_pack(game_dir, tmp_path):
    source = game_dir / "Packs" / "Fonts.pak"
    assert roundtrip_pack(source, tmp_path) == source.read_bytes()


def test_impk_reproduces_a_large_pack(game_dir, tmp_path):
    """`UI.pak` is 43 MB and 4,141 files, with the deepest name tree of the set."""
    source = game_dir / "Packs" / "UI.pak"
    assert roundtrip_pack(source, tmp_path) == source.read_bytes()


def test_impk_reproduces_every_shipped_pack(game_dir):
    """All 13, because the tables that make this hard differ from pack to pack.

    Each pack is rebuilt in a directory of its own that is deleted immediately
    afterwards: extracting the whole set at once would need 220 MB of scratch
    space, and `Sounds.pak` alone is 136 MB of it.
    """
    checked = 0
    for source in corpus.pack_paths(game_dir):
        label = corpus.label(game_dir, source)
        with tempfile.TemporaryDirectory() as work:
            assert roundtrip_pack(source, Path(work)) == source.read_bytes(), label
        checked += 1
    assert checked == 13


# -- imfs ----------------------------------------------------------------


def test_imfs_reproduces_a_container(game_dir, tmp_path):
    source = game_dir / "Packs" / "randommap.BFHP"
    assert roundtrip_container(source, tmp_path) == source.read_bytes()


def test_imfs_reproduces_every_container(game_dir):
    """Every uncompressed container the installation holds, found by magic.

    The reference install has 23 and all 23 reproduce exactly; the assertion is
    a floor because some containers are written by the game at runtime, so the
    exact set depends on whether the install has ever been played.
    """
    checked = 0
    for source in corpus.container_paths(game_dir):
        label = corpus.label(game_dir, source)
        with tempfile.TemporaryDirectory() as work:
            assert roundtrip_container(source, Path(work)) == source.read_bytes(), label
        checked += 1
    assert checked >= 19
