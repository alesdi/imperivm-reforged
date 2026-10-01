"""Shared fixtures, game-installation discovery, and the skip machinery.

The suite is deliberately in two halves.

**Data-free tests** (`test_<format>.py`) build every fixture they need in code and
run everywhere, including CI. They are the only safety net a push gets.

**Corpus tests** (`test_corpus_<format>.py`) validate the readers against a real
retail installation. The repository ships no game data and never will (see
`docs/legal.md`), so these must *skip* — not fail, not error — when no
installation is present. They are marked with :data:`requires_game`, which is a
``pytest.mark.skipif`` evaluated once at import time.

Point the suite at an installation with the ``IMPERIVM_GAME_DIR`` environment
variable, or install the game at one of :data:`CANDIDATE_PATHS`.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

# Let the test modules `import synthetic` regardless of how pytest was invoked.
sys.path.insert(0, str(Path(__file__).resolve().parent))

# Every app a test launches runs headless: no window to take the user's focus,
# no sound device. Subprocesses inherit it. `imperivm --help` says how to run
# the app visibly.
os.environ.setdefault("IMPERIVM_HEADLESS", "1")
os.environ.setdefault("SDL_AUDIO_DRIVER", "dummy")
# And nothing it writes -- the F5 quick save, settings.ini, a profile, a
# campaign's progress -- goes into the installation: the app keeps all of it,
# and reads back its own, under IMPERIVM_USER_DIR (`imperivm --help`, "User
# data"). One directory for the session, removed when the session ends. A test
# that needs a user file of its own (a save to load, a settings file) puts it
# here, never in the installation. The app would pick a temporary directory by
# itself when headless; this makes the choice the suite's, and visible.
if "IMPERIVM_USER_DIR" not in os.environ:
    import atexit
    import shutil
    import tempfile

    _user_dir = tempfile.mkdtemp(prefix="imperivm-tests-user-")
    atexit.register(shutil.rmtree, _user_dir, ignore_errors=True)
    os.environ.setdefault("IMPERIVM_USER_DIR", _user_dir)

#: Environment variable that overrides discovery.
ENV_VAR = "IMPERIVM_GAME_DIR"

#: Where an installation plausibly lives when the variable is not set. Checked in
#: order; the first that looks like an install wins.
CANDIDATE_PATHS: tuple[str, ...] = (
    "~/Imperivm",
    "~/Games/Imperivm",
    "~/Desktop/Imperivm",
    "~/Downloads/Imperivm",
    "/Applications/Imperivm",
    "/opt/imperivm",
    "~/.wine/drive_c/Program Files (x86)/FX Interactive/Imperivm",
    "~/.wine/drive_c/Program Files (x86)/Haemimont Games/Imperivm",
    "C:/Program Files (x86)/FX Interactive/Imperivm",
    "C:/Program Files/FX Interactive/Imperivm",
    "C:/GOG Games/Imperivm",
)

#: The file whose presence identifies a directory as an installation. `data.pak`
#: holds the class graph and is present in every edition.
INSTALL_MARKER = Path("Packs") / "data.pak"


def looks_like_install(path: Path) -> bool:
    """True when `path` is the root of an Imperivm installation."""
    try:
        return (path / INSTALL_MARKER).is_file()
    except OSError:  # pragma: no cover - unreadable path
        return False


def find_game_dir() -> Path | None:
    """Locate an installation, or return None.

    ``IMPERIVM_GAME_DIR`` wins outright: if it is set but does not point at an
    installation, discovery stops rather than silently falling through to some
    other copy on the machine. That keeps a deliberately-empty override honest,
    which is exactly the condition CI runs under.
    """
    override = os.environ.get(ENV_VAR)
    if override:
        candidate = Path(override).expanduser()
        return candidate if looks_like_install(candidate) else None

    for raw in CANDIDATE_PATHS:
        candidate = Path(raw).expanduser()
        if looks_like_install(candidate):
            return candidate
    return None


def _skip_reason() -> str:
    override = os.environ.get(ENV_VAR)
    if override:
        return (
            f"{ENV_VAR}={override!r} does not contain {INSTALL_MARKER.as_posix()}, so no "
            f"Imperivm installation was found. Point {ENV_VAR} at the directory holding "
            f"Packs/, rle.mmp and gbr.exe to enable the corpus tests."
        )
    return (
        "no Imperivm installation found. These tests validate the readers against retail "
        f"data, which this repository does not ship. Set {ENV_VAR} to the directory holding "
        f"Packs/, rle.mmp and gbr.exe -- for example "
        f"`{ENV_VAR}=/path/to/Imperivm python -m pytest` -- or install the game at one of: "
        + ", ".join(CANDIDATE_PATHS)
    )


#: Resolved once, at import time, so that every corpus module agrees.
GAME_DIR: Path | None = find_game_dir()
SKIP_REASON: str = _skip_reason()

#: Apply to a corpus module with ``pytestmark = requires_game``.
requires_game = pytest.mark.skipif(GAME_DIR is None, reason=SKIP_REASON)

#: `rle.mmp` is 400 MB and sits beside the executable rather than in a pack, so a
#: partial install can have the packs without it.
requires_pixel_store = pytest.mark.skipif(
    GAME_DIR is None or not (GAME_DIR / "rle.mmp").is_file(),
    reason=SKIP_REASON if GAME_DIR is None else "rle.mmp is not present in the installation",
)


# ---------------------------------------------------------------------------
# pytest plumbing
# ---------------------------------------------------------------------------


def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line(
        "markers", "corpus: requires a real game installation; skips without one"
    )
    config.addinivalue_line(
        "markers",
        "spread: every test in this module is independent work with cheap fixtures, so "
        "`--dist loadgroup` may run them on different workers; see _group_for_xdist",
    )
    config.addinivalue_line(
        "markers",
        "swept: also run, the same command and the same assertions, by a sweep that "
        "`tools/verify.py --full` runs; verify deselects it there, a bare pytest keeps it",
    )


#: Where the last run's per-test wall times are kept, in pytest's own cache.
DURATIONS_KEY = "imperivm/durations"


def _module_of(item: pytest.Item) -> str:
    return item.nodeid.split("::")[0]


def _group_for_xdist(item: pytest.Item) -> str | None:
    """The `xdist_group` a test runs in under `--dist loadgroup`, or None.

    **By default a module is one group**, which is `--dist loadfile`: its
    module-scoped fixtures -- a parsed installation, an `imcheck` census --
    are built once, on one worker, as they were written to be. A module marked
    `spread` has no group: each of its tests goes to whichever worker is free,
    which is what the few modules that are a list of independent subprocess
    runs want -- under `loadfile` the whole of `test_corpus_imsave.py` or
    `test_corpus_app_net.py` queued on one worker behind its longest run.

    A test that names its own `xdist_group` keeps it: the app-net tests that
    host through the shipped screens all listen on the default port, and must
    take turns.
    """
    if item.get_closest_marker("xdist_group") is not None:
        return None
    if item.get_closest_marker("spread") is not None:
        return None
    return _module_of(item)


def _scope(item: pytest.Item) -> str:
    """The unit `--dist loadgroup` schedules: a group's name, else the test."""
    names = sorted(
        str(mark.args[0] if mark.args else mark.kwargs.get("name", "default"))
        for mark in item.iter_markers("xdist_group")
    )
    return "_".join(names) if names else item.nodeid


@pytest.hookimpl(tryfirst=True)
def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    """Tag every corpus module so `-m corpus` / `-m 'not corpus'` select cleanly.

    And under `--dist loadgroup`, group the tests (see `_group_for_xdist`) and
    put the longest work first. xdist hands out work units in collection order
    (with `--no-loadscope-reorder`), so ordering them by last run's wall time,
    longest first, is what stops a four-minute test starting last and running
    alone at the end. The order is a function of the cached times alone, which
    every worker reads before the controller rewrites them, so every worker
    collects the same order, as xdist requires. With no times cached the
    order is collection order. It has no effect on what runs or what passes.
    """
    for item in items:
        if _module_of(item).split("/")[-1].startswith("test_corpus_"):
            item.add_marker(pytest.mark.corpus)

    # Collection happens on the workers, and xdist rewrites a worker's `dist`
    # to "no" and says `loadgroup` instead; the controller never collects.
    if not getattr(config.option, "loadgroup", False):
        return
    for item in items:
        group = _group_for_xdist(item)
        if group is not None:
            item.add_marker(pytest.mark.xdist_group(group))
    cache = getattr(config, "cache", None)
    durations = cache.get(DURATIONS_KEY, {}) if cache is not None else {}
    if not isinstance(durations, dict) or not durations:
        return
    cost: dict[str, float] = {}
    first: dict[str, int] = {}
    for index, item in enumerate(items):
        scope = _scope(item)
        cost[scope] = cost.get(scope, 0.0) + float(durations.get(item.nodeid, 0.0))
        first.setdefault(scope, index)
    position = {id(item): index for index, item in enumerate(items)}
    items.sort(key=lambda item: (-cost[_scope(item)], first[_scope(item)], position[id(item)]))


def _plain_nodeid(nodeid: str) -> str:
    """A report's node id without the `@group` suffix loadgroup adds."""
    at = nodeid.rfind("@")
    return nodeid[:at] if at > nodeid.rfind("]") else nodeid


_timed: dict[str, float] = {}


def pytest_runtest_logreport(report: pytest.TestReport) -> None:
    nodeid = _plain_nodeid(report.nodeid)
    _timed[nodeid] = _timed.get(nodeid, 0.0) + report.duration


def pytest_sessionfinish(session: pytest.Session) -> None:
    """Record this run's wall time per test, on the controller only."""
    config = session.config
    if hasattr(config, "workerinput") or not _timed or getattr(config, "cache", None) is None:
        return
    durations = config.cache.get(DURATIONS_KEY, {})
    if not isinstance(durations, dict):
        durations = {}
    durations.update({nodeid: round(took, 2) for nodeid, took in _timed.items()})
    config.cache.set(DURATIONS_KEY, durations)


def pytest_report_header() -> list[str]:
    if GAME_DIR is None:
        return [f"imperivm game data: not found (corpus tests will skip; set {ENV_VAR})"]
    return [f"imperivm game data: {GAME_DIR}"]


# ---------------------------------------------------------------------------
# corpus fixtures
# ---------------------------------------------------------------------------


class PackSet:
    """Lazily opened, session-cached view of every pack in an installation.

    Packs are identified by content (the `HMMSYS PackFile` magic) rather than by
    extension, which is what excludes `Packs/RandomMap.pak` -- it carries the
    `.pak` name but is a whole-file LZIS stream. See docs/formats/lzis.md.
    """

    def __init__(self, game_dir: Path) -> None:
        from imperivm.formats.pak import MAGIC

        self.game_dir = game_dir
        self.paths: list[Path] = []
        for path in sorted(game_dir.rglob("*.pak")):
            with path.open("rb") as handle:
                if handle.read(len(MAGIC)) == MAGIC:
                    self.paths.append(path)
        self._cache: dict[Path, object] = {}

    def label(self, path: Path) -> str:
        return path.relative_to(self.game_dir).as_posix()

    def open(self, path: Path):
        """Return a cached :class:`~imperivm.formats.pak.PackFile`."""
        from imperivm.formats.pak import PackFile

        if path not in self._cache:
            self._cache[path] = PackFile(path)
        return self._cache[path]

    def __iter__(self):
        for path in self.paths:
            yield self.label(path), self.open(path)

    def entries_named(self, suffix: str):
        """Yield `(pack_label, entry_name, bytes)` for every entry with `suffix`."""
        wanted = suffix.upper()
        for label, pack in self:
            for entry in pack.entries:
                if entry.name.upper().endswith(wanted):
                    yield label, entry.name, pack.read(entry.name)


@pytest.fixture(scope="session")
def game_dir() -> Path:
    """The installation root. Skips when there is none."""
    if GAME_DIR is None:
        pytest.skip(SKIP_REASON)
    return GAME_DIR


@pytest.fixture(scope="session")
def packs(game_dir: Path) -> PackSet:
    return PackSet(game_dir)


@pytest.fixture(scope="session")
def pixel_store(game_dir: Path):
    """The root-level `rle.mmp`, opened once for the whole session."""
    from imperivm.formats.rle import PixelStore

    path = game_dir / "rle.mmp"
    if not path.is_file():
        pytest.skip("rle.mmp is not present in the installation")
    store = PixelStore(path)
    yield store
    store.close()


@pytest.fixture(scope="session")
def game_data(game_dir: Path):
    """The resolved object model: 845 classes and 889 entities."""
    from imperivm.formats.gamedata import GameData

    return GameData(game_dir / "Packs")
