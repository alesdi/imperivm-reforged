"""The map writer against every shipped `map.obj.xml`, and a container rebuilt.

`core::write_map_objects` is the editor's Save. The property it has to have is
the one the byte-identical pack rebuilds have: **what was read is what is
written**. A writer that emitted only the attributes this engine understands
would strip a hero of its `hs*` skills and a unit of its `Level` on the first
save -- attributes nothing in the simulation reads yet, which is exactly why a
round trip through the *tables* would not notice.

So the check is bytes. `imcheck objects` parses a stored document, writes it
back and compares the two byte for byte (exit 0), or only their parsed tables
(exit 3); the corpus has 28 documents and every one must come back identical.

The second check is the container: `immap rewrite` copies a container with its
object list **and its six terrain layers** regenerated -- the document by
`core::write_map_objects`, the layers by `core::write_grid` through
`gamedata::write_map_container`'s `MapEdits` -- and `imrun` over the original
and the copy must reach the same world hash. The copy is a different
arrangement of the same blocks, and the game cannot tell. The same rewrite is
run over every map directory in the install, and every one of the seven
regenerated documents must come back identical to what was stored.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

from conftest import requires_game

import corpus
from imperivm.formats.bfhp import BlockFile

# Each test is its own subprocess run and the module's fixtures only find a
# binary, so under `--dist loadgroup` its tests may go to different workers.
pytestmark = [requires_game, pytest.mark.spread]

#: Every `map.obj.xml` across the 23 map-bearing containers (28: the conquest
#: holds seven, and `randommap.BFHP` is found through its LZIS wrapper).
DOCUMENT_COUNT = 28


@pytest.fixture(scope="module")
def imcheck() -> Path:
    path, complaint = corpus.find_tool("imcheck", "IMPERIVM_IMCHECK")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


@pytest.fixture(scope="module")
def immap() -> Path:
    path, complaint = corpus.find_tool("immap", "IMPERIVM_IMMAP")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


@pytest.fixture(scope="module")
def imrun() -> Path:
    path, complaint = corpus.find_tool("imrun", "IMPERIVM_IMRUN")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


def _documents(game_dir: Path) -> list[tuple[Path, str]]:
    """Every `(container, entry)` holding an object list, by content."""
    found: list[tuple[Path, str]] = []
    paths = list(corpus.container_paths(game_dir))
    random_map = game_dir / "Packs" / "randommap.BFHP"
    if random_map.is_file() and random_map not in paths:
        paths.append(random_map)
    for path in sorted(paths):
        block_file = BlockFile.from_bytes(_plain(path.read_bytes()), str(path))
        for entry in block_file.entries:
            if entry.is_dir:
                continue
            if entry.name.replace("\\", "/").lower().endswith("map.obj.xml"):
                found.append((path, entry.name.replace("\\", "/")))
    return found


def _plain(data: bytes) -> bytes:
    from imperivm.formats.bfhp import MAGIC
    from imperivm.formats.lzis import decompress

    return data if data[: len(MAGIC)] == MAGIC else decompress(data)


def test_every_shipped_object_list_writes_back_byte_for_byte(imcheck, game_dir):
    documents = _documents(game_dir)
    assert len(documents) == DOCUMENT_COUNT
    failures = []
    for container, entry in documents:
        result = subprocess.run(
            [str(imcheck), "objects", str(container), entry],
            capture_output=True,
            text=True,
            timeout=300,
        )
        if result.returncode != 0:
            failures.append(f"{corpus.label(game_dir, container)} {entry}: "
                            f"exit {result.returncode}\n{result.stdout}{result.stderr}")
    assert not failures, "\n".join(failures)


def test_a_rebuilt_container_plays_the_same_game(immap, imrun, game_dir, tmp_path):
    source = game_dir / "Scenarios" / "Crossroads.BFHP"
    if not source.is_file():
        pytest.skip("the installation has no Scenarios/Crossroads.BFHP")
    copy = tmp_path / "crossroads.bfhp"
    rewritten = subprocess.run(
        [str(immap), "rewrite", str(source), "1", str(copy)],
        capture_output=True,
        text=True,
        timeout=300,
    )
    assert rewritten.returncode == 0, rewritten.stdout + rewritten.stderr
    assert "differs" not in rewritten.stdout, rewritten.stdout
    # The object list and all six layers, each regenerated and each identical.
    regenerated = [line for line in rewritten.stdout.splitlines() if line.endswith("identical")]
    assert len(regenerated) == 7, rewritten.stdout
    assert copy.is_file()

    def final_hash(container: Path) -> str:
        result = subprocess.run(
            [str(imrun), str(game_dir), str(container), "20", "800"],
            capture_output=True,
            text=True,
            timeout=900,
        )
        assert result.returncode == 0, result.stdout + result.stderr
        match = re.search(r"^\s+hash\s+([0-9a-f]{16})$", result.stdout, re.M)
        assert match is not None, result.stdout
        return match.group(1)

    assert final_hash(copy) == final_hash(source)


def _map_directories(game_dir: Path) -> list[tuple[Path, int]]:
    """Every `(container, map number)` in the install, the LZIS-wrapped ones
    included, so that the 4-bit terrain layers of the blank templates and the
    seven maps of the conquest all go through the writer."""
    import sys

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from corpus import _walk
    from imperivm.formats.bfhp import MAGIC

    found: list[tuple[Path, int]] = []
    for path in _walk(game_dir):
        with path.open("rb") as handle:
            head = handle.read(4)
        if head == MAGIC:
            block_file = BlockFile(path)
        elif head == b"LZIS":
            try:
                data = _plain(path.read_bytes())
            except Exception:  # noqa: BLE001 - not every LZIS stream is a container
                continue
            if data[:4] != MAGIC:
                continue
            block_file = BlockFile(path, data=data)
        else:
            continue
        numbers = set()
        for entry in block_file.entries:
            parts = entry.name.replace("\\", "/").split("/")
            if len(parts) == 3 and parts[0].lower() == "maps" and parts[2].lower() == "map.xml":
                numbers.add(int(parts[1]))
        found.extend((path, number) for number in sorted(numbers))
    return found


#: `Maps/<n>` directories across the install; see `test_corpus_adventure.py`.
MAP_DIRECTORY_COUNT = 29


def test_every_map_directory_rewrites_its_seven_documents_identically(immap, game_dir, tmp_path):
    directories = _map_directories(game_dir)
    assert len(directories) == MAP_DIRECTORY_COUNT
    failures = []
    for index, (container, number) in enumerate(directories):
        copy = tmp_path / f"rewrite-{index}.bfhp"
        result = subprocess.run(
            [str(immap), "rewrite", str(container), str(number), str(copy)],
            capture_output=True,
            text=True,
            timeout=300,
        )
        regenerated = [line for line in result.stdout.splitlines() if line.startswith("  ")]
        if (
            result.returncode != 0
            or len(regenerated) != 7
            or any(not line.endswith("identical") for line in regenerated)
        ):
            failures.append(
                f"{corpus.label(game_dir, container)} Maps/{number}: exit {result.returncode}\n"
                f"{result.stdout}{result.stderr}"
            )
        copy.unlink(missing_ok=True)
    assert not failures, "\n".join(failures)


def test_a_dropped_map_leaves_the_copy_with_the_others_untouched(immap, game_dir, tmp_path):
    """`--drop-map 3` on the conquest: `Maps/3/` and everything under it is
    gone, every other file is the source's bytes, and the drop is proved to
    have taken a whole directory rather than the seven documents alone."""
    source = game_dir / "Conquests" / "mediterranean.BFHP"
    if not source.is_file():
        pytest.skip("the installation has no Conquests/mediterranean.BFHP")
    copy = tmp_path / "mediterranean-less-3.bfhp"
    rewritten = subprocess.run(
        [str(immap), "rewrite", str(source), "4", str(copy), "--drop-map", "3"],
        capture_output=True,
        text=True,
        timeout=300,
    )
    assert rewritten.returncode == 0, rewritten.stdout + rewritten.stderr
    before = BlockFile(source)
    after = BlockFile(copy)
    before_names = {entry.name.replace("\\", "/") for entry in before.entries}
    after_names = {entry.name.replace("\\", "/") for entry in after.entries}
    dropped = {name for name in before_names if name.lower().startswith("maps/3/") or name.lower() == "maps/3"}
    assert dropped, sorted(before_names)[:10]
    assert len(dropped) > 7, sorted(dropped)
    assert after_names == before_names - dropped
    for entry in after.entries:
        name = entry.name.replace("\\", "/")
        if entry.is_dir or name.lower().startswith("maps/4/"):
            continue  # regenerated by the rewrite, proved identical elsewhere
        assert after.read(entry.name) == before.read(entry.name), name
