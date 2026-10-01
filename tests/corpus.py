"""Helpers shared by the corpus modules.

Nothing here is a fixture: these are plain functions so that a module can call
them from a fixture of whatever scope it needs. Everything expensive is cached
for the lifetime of the process, because several modules want the same bytes.

Discovery is by **content**, never by file name. The installation names files
inconsistently (`.BFHP` and `.bfhp`, passability masks called `PASS` with no
extension at all) and one file lies outright: `Packs/RandomMap.pak` carries the
pack extension but is a whole-file LZIS stream.
"""

from __future__ import annotations

import functools
import os
import re
from pathlib import Path

from imperivm.formats.bfhp import MAGIC as HPFS_MAGIC
from imperivm.formats.lzis import decompress
from imperivm.formats.pak import MAGIC as PACK_MAGIC

#: Directories that hold no readable container and are expensive to walk --
#: and `Saves`, which is this engine's own: every file in it is a container
#: the app wrote, not one the game shipped.
_SKIP_DIRS = frozenset({"Movies", "music", "Logs", "Saves"})


def _sniff(path: Path, magic: bytes) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(len(magic)) == magic
    except OSError:  # pragma: no cover - unreadable file in the install
        return False


def _walk(game_dir: Path):
    for path in sorted(game_dir.rglob("*")):
        if not path.is_file():
            continue
        if any(part in _SKIP_DIRS for part in path.relative_to(game_dir).parts[:-1]):
            continue
        yield path


@functools.lru_cache(maxsize=4)
def pack_paths(game_dir: Path) -> tuple[Path, ...]:
    """Every real HMMSYS pack image in the installation, by magic."""
    return tuple(p for p in sorted(game_dir.rglob("*.pak")) if _sniff(p, PACK_MAGIC))


@functools.lru_cache(maxsize=4)
def container_paths(game_dir: Path) -> tuple[Path, ...]:
    """Every uncompressed HPFS block container in the installation, by magic."""
    return tuple(p for p in _walk(game_dir) if _sniff(p, HPFS_MAGIC))


@functools.lru_cache(maxsize=4)
def random_map_image(game_dir: Path) -> bytes:
    """`Packs/RandomMap.pak` decompressed: a pack image, not an LZIS stream."""
    return decompress((game_dir / "Packs" / "RandomMap.pak").read_bytes())


def label(game_dir: Path, path: Path) -> str:
    return path.relative_to(game_dir).as_posix()


# ---------------------------------------------------------------------------
# built tools
# ---------------------------------------------------------------------------

#: The repository root, two levels up from this file.
REPO = Path(__file__).resolve().parent.parent

def tool_directories() -> list[Path]:
    """Every `build*/` directory, because the tree grows one per pass.

    **This was a hand-written tuple, and the hand had fallen behind.** It named
    seven directories -- `build`, `build-core`, `build-saves`, `build-campaign`,
    `build-combat`, `build-globals`, `build-app` -- assembled from what existed
    when it was written. The tree now carries twenty-eight, three of which hold
    built tools, and one of those three (`build-perf-default`) was not in the
    tuple. Nothing was wrong today only because the omitted copy happened to be
    the *oldest*; had it been the newest, `find_tool` would have returned an
    older binary while believing it had found the newest, which is precisely the
    failure the function exists to prevent. A curated list of what to look at is
    a list that stops being true without saying so -- the same shape as the
    hand-curated leaders list in `docs/plan.html`, which silently dropped five
    names that outranked its own cutoff.
    """
    return sorted(REPO.glob("build*"))


_LIBRARY_DIRECTORIES = {
    "imperivm_core": "core",
    "imperivm_gamedata": "gamedata",
    "imperivm_platform": "platform",
}


def tool_libraries(tool: str) -> tuple[str, ...] | None:
    """The `engine/<dir>` sources a tool is linked from, read off the CMake file.

    `target_link_libraries(imcheck PRIVATE imperivm_core)` is the one line that
    says what a tool is built from, so it is read rather than copied here --
    a copy is what every previous version of the staleness guard got wrong.
    `None` when the file does not say, which the caller reads as "everything",
    the conservative answer.
    """
    try:
        text = REPO.joinpath("engine", "tools", "CMakeLists.txt").read_text()
    except OSError:
        return None
    match = re.search(
        rf"target_link_libraries\(\s*{re.escape(tool)}\s+(?:PRIVATE|PUBLIC)?\s*([^)]*)\)", text
    )
    if match is None:
        return None
    directories = [
        _LIBRARY_DIRECTORIES[name]
        for name in match.group(1).split()
        if name in _LIBRARY_DIRECTORIES
    ]
    return tuple(directories) if directories else None


def newest_engine_source(tool: str | None = None) -> Path | None:
    """The most recently modified source `tool` is actually built from.

    **`engine/tests/` is excluded, and that is a fix rather than an oversight.**
    The tools link `imperivm_core` and their own `engine/tools/` sources; no
    test file is in any of their translation units, so a test edit cannot change
    what `imcheck` prints. Counting it meant that touching a test file made
    every corpus tool "stale" and forced a rebuild of binaries that had not
    changed -- which happened four times in one session, each time costing a
    full link to produce a byte-identical result.

    **And so is every *other* tool's `.cpp`, for exactly the same reason one
    step on.** `engine/tools/imsave.cpp` is not in `imcheck`'s translation units
    either, so editing one tool made all the others stale -- and unlike the test
    case there is no rebuild that clears it, because `ninja` correctly declines
    to relink a binary whose inputs have not changed. The guard then never goes
    quiet: a commit touching one tool silently skipped **32 corpus tests** in
    five modules, which is the guard's own failure mode turned inside out. It
    was written to stop a stale binary reporting a pass; skipping is the same
    silence by another route.

    **And `engine/app` and `engine/platform`, the third time round.** The
    tools link `imperivm_core` and `imperivm_gamedata` and nothing else -- the
    application and the SDL platform layer are in no headless binary -- so an
    edit to `engine/app/main.cpp` made every corpus tool stale with, again, no
    rebuild that could clear it: 23 tests skipped for as long as the app edit
    stood. Each copy of this mistake has been the same one: the guard's idea
    of "an input" wider than the linker's.

    What the guard is for is unaffected. An edit under `engine/core` or
    `engine/gamedata` is in every tool's translation units and still makes all
    of them stale, and that is the case that spent a session validating a build
    in which `CombatSystem` was still empty.
    """
    newest: Path | None = None
    libraries = tool_libraries(tool) if tool is not None else None
    for path in REPO.joinpath("engine").rglob("*"):
        if path.suffix not in (".cpp", ".hpp", ".h", ".cc"):
            continue
        parts = path.relative_to(REPO).parts
        if "tests" in parts:
            continue
        if parts[:2] in (("engine", "app"), ("engine", "platform")):
            continue
        # A library the tool does not link is not an input to it: `imcheck`
        # links `imperivm_core` alone, so `engine/gamedata` cannot change what
        # it prints, and counting it made every gamedata edit skip its tests.
        if libraries is not None and parts[:2] == ("engine", parts[1]) and parts[1] not in (
            "tools",
            *libraries,
        ):
            continue
        # A tool's own directory holds one translation unit per tool. Headers
        # there are shared (`vs_dump.hpp`) and stay in; the `.cpp` files are
        # each their own binary's.
        if tool is not None and parts[:2] == ("engine", "tools") and path.suffix == ".cpp":
            if path.stem != tool:
                continue
        if newest is None or path.stat().st_mtime > newest.stat().st_mtime:
            newest = path
    return newest


def find_tool(name: str, env_var: str | None = None) -> tuple[Path | None, str]:
    """Locate a built tool, and say whether it is this tree's.

    Returns `(path, complaint)`. `complaint` is empty when the tool is usable
    and is a sentence naming the problem otherwise.

    **The newest candidate wins, not the first.** This function replaced a
    per-module `CANDIDATES` tuple that listed a scratch directory ahead of
    `build`, and the scratch copy was three-quarters of an hour older than the
    engine -- so `test_corpus_imsave.py` spent a whole session validating a
    build in which `CombatSystem` was still empty, and reporting a pass. That is
    this project's signature failure wearing a new coat: the check ran, the
    check passed, and the check was not testing this tree. A corpus test that
    shells out to a binary is only as current as the binary.
    """
    if env_var:
        override = os.environ.get(env_var)
        if override:
            path = Path(override)
            return (path, "") if path.is_file() else (None, f"{env_var}={override!r} is not a file")

    found = [
        path
        for directory in tool_directories()
        if (path := directory / "engine" / "tools" / name).is_file()
    ]
    if not found:
        return None, (
            f"no built {name} found; configure a build directory"
            + (f" or set {env_var}" if env_var else "")
        )
    newest = max(found, key=lambda path: path.stat().st_mtime)

    source = newest_engine_source(name)
    if source is not None and source.stat().st_mtime > newest.stat().st_mtime:
        return newest, (
            f"{newest.relative_to(REPO)} is older than "
            f"{source.relative_to(REPO)}; rebuild before running the corpus tools"
        )
    return newest, ""
