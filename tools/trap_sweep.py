#!/usr/bin/env python3
"""What the whole installation still asks for, run map by map.

    python3 tools/trap_sweep.py [--game DIR] [--imrun PATH] [--jobs N]
                                [--turns N] [--turn-ms N] [--top N]
                                [--war] [--war-turns N]

The maps run concurrently, `--jobs` at a time (every core by default), and are
reported in discovery order whatever order they finish in: the output of a
parallel sweep is byte-for-byte that of `--jobs 1`. The war pass is opt-in
(`--war`) because since class behaviours start it takes a quarter of an hour
on Balcans alone; `--turns 0` skips the short pass, for a war pass on its own.

`imrun` answers this question for one map. The figure the plan actually quotes
as its work list -- "N distinct traps, M hits, corpus-wide" -- is the sum over
every shipped map, and it has been assembled by hand every time it has been
written down. That is this project's signature failure: a number that was true
once, quoted afterwards. This tool computes it.

**What the numbers mean.** A trap is a script reaching an entry point this
engine declares and has not implemented. It is the expected outcome at this
coverage level and is not a crash: the script stops, the other 884 keep going.
`imrun` keys its tally on the whole message, and the message carries the source
script -- so `PlayAnim/2` reached from two different `.vs` files is *two*
distinct traps and *one* missing entry point. Both groupings are printed below,
because they rank different things: by (message, source) tells you which
script is stuck, by message alone tells you which entry point to write. Neither
is the "right" one, and quoting one while meaning the other is how a work list
goes wrong.

**What they do not mean.** A trap count is not a coverage figure. It counts
what the shipped content *reached* in this many turns of this length, not what
it declares -- `tools/host_coverage.py` is the static half, and an entry point
with 285 call sites can trap zero times here because nothing woke the script
that calls it. The tally moves with `--turns` and `--turn-ms`, which is why
both are printed with the result. Two sweeps are only comparable at equal
settings.

**Discovery is by content.** Maps are found by opening every container in the
installation and asking it which `Maps/<n>` it holds, never from a list in this
file. A hardcoded list of the conquest's seven map numbers is precisely the
mistake the rule exists to prevent, and it would also have missed
`Packs/RandomMapSettlements.bfhp`, which is a runnable map wearing an LZIS
wrapper and a name that says it is furniture.
"""

from __future__ import annotations

import argparse
import os
import collections
import concurrent.futures
import dataclasses
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import corpus  # noqa: E402
import host_coverage  # noqa: E402
from imperivm.formats import lzis  # noqa: E402
from imperivm.formats.bfhp import MAGIC as HPFS_MAGIC  # noqa: E402
from imperivm.formats.bfhp import BlockFile  # noqa: E402

#: Turns and turn length the tally below is measured at.
#:
#: 800 ms is the turn length `CONTRIBUTING.md`'s own smoke run uses and the top
#: of the 200--800 range the shipped scripts are written against; 60 turns is
#: 48 seconds of game time, long enough for the idle scripts that dominate the
#: tally to come round several times. Neither is sacred -- they are here so the
#: figure has stated conditions rather than remembered ones.
DEFAULT_TURNS = 60
DEFAULT_TURN_MS = 800

#: How long the **war pass** runs each skirmish, and why there is one.
#:
#: Sixty turns is long enough for the idle scripts and short enough for the
#: whole installation, and far too short for anything a computer player does:
#: on Crossroads the first army marches after about 600 turns, and the one
#: runaway loop this sweep had to be told about by a corpus test --
#: `UNIT_CAPTURE.VS` spinning its budget at an independent camp, 916 hits in
#: 4,000 turns, the first at turn 3,086 -- was invisible here at the settings
#: anybody runs. So every map that carries no mission sequences, which is what
#: a skirmish is (the war there is the AI's and nothing else's), is also run
#: this long, under `IMRUN_UNTIL_OVER` so that a match that ends stops. It is
#: a second tally and is never folded into the first: the two horizons are not
#: comparable, which is this module's first rule.
DEFAULT_WAR_TURNS = 4000

#: Long enough that a slow map is never mistaken for a hung one. The whole
#: sweep is well under a minute on a warm machine; a map that wants ten times
#: that is a bug worth seeing rather than a timeout worth tuning.
RUN_TIMEOUT_SECONDS = 600

#: The war pass's ceiling. Balcans alone took fourteen minutes in a release
#: build once class behaviours started, which the ceiling above would have
#: turned into a traceback rather than a tally.
WAR_TIMEOUT_SECONDS = 3600

#: `  %5zu  <message>` -- `imrun`'s trap line. The count is hits, not scripts.
TRAP_LINE = re.compile(r"^\s+(\d+)\s\s(.+)$")
TRAP_HEADER = re.compile(r"^\d+ distinct traps, most-hit first:$")


@dataclasses.dataclass(frozen=True)
class MapRef:
    """One runnable map: a container, and the `Maps/<n>` inside it.

    `number` is the directory number, **not** an index -- the conquest numbers
    its seven maps 3, 4, 6, 7, 8, 9, 10 -- and is `None` for the containers
    that keep their map at the root, which is what `imrun` does with no fifth
    argument.
    """

    container: Path
    number: str | None
    label: str
    #: Whether the map carries mission sequences. One without them is a
    #: skirmish, and the war pass runs it long.
    scripted: bool = True


@dataclasses.dataclass
class MapResult:
    """What one `imrun` invocation produced."""

    ref: MapRef
    ran: bool
    reason: str = ""
    traps: collections.Counter = dataclasses.field(default_factory=collections.Counter)


def container_bytes(path: Path) -> bytes | None:
    """The file as an HPFS image, unwrapping LZIS, or None if it is neither.

    The installation holds one container that is compressed whole
    (`Packs/RandomMapSettlements.bfhp`) and one file that is compressed whole
    and is *not* a container (`Packs/RandomMap.pak`, a pack image), so the
    wrapper has to be opened to tell them apart. Only the first chunk is
    decoded for the test, because the pack behind the second is 47 MB and this
    runs for every file in the install.
    """
    with path.open("rb") as handle:
        head = handle.read(len(lzis.MAGIC))
    if head == HPFS_MAGIC:
        return path.read_bytes()
    if head != lzis.MAGIC:
        return None

    data = path.read_bytes()
    try:
        header = lzis.parse_header(data)
        offsets = lzis.chunk_offsets(data, header)
        stored = data[offsets[0] : offsets[1]]
        expected = min(header.chunk_size, header.uncompressed_size)
        # A chunk that did not compress is held verbatim and carries no flag
        # saying so; the reader compares sizes. See `lzis.decompress`.
        first = (
            stored[:expected]
            if len(stored) >= expected
            else lzis.decompress_chunk(stored, expected)
        )
        if not first.startswith(HPFS_MAGIC):
            return None
        return lzis.decompress(data)
    except lzis.LzisError:
        return None


def maps_in(data: bytes, name: str) -> list[tuple[str | None, bool]]:
    """The maps a container image holds, in numeric order, each with whether
    it carries mission sequences.

    A map is a `map.obj.xml`, which is what `gamedata::read_payloads` looks for
    and therefore the only definition that agrees with the thing being run. A
    container holding one at the root reports `[(None, ...)]`; one holding none
    -- `currentadv.bfhp` is empty and is exactly that -- reports nothing and is
    not a map at all, which is different from a map that refuses.

    A numbered map is **scripted** when its container holds a `.vs` under
    `Maps/<n>/Sequences/` or under the game-wide `Sequences/` at the root, which is
    where the conquest keeps the eight it runs on every map. Every shipped
    container carries a `sequences.xml` in both places, the skirmishes empty
    ones, so it is the scripts that count and not the directory. A root map is
    scripted when the container holds a sequence script anywhere.
    """
    numbers: set[str] = set()
    root = False
    sequenced: set[str] = set()
    root_sequenced = False
    for entry in BlockFile.from_bytes(data, name).entries:
        if entry.is_dir:
            continue
        path = entry.name.replace("\\", "/").lower()
        parts = path.split("/")
        if "sequences" in parts[:-1] and path.endswith(".vs"):
            if len(parts) >= 3 and parts[0] == "maps":
                sequenced.add(parts[1])
            else:
                root_sequenced = True
        if not path.endswith("map.obj.xml"):
            continue
        if len(parts) == 3 and parts[0] == "maps":
            numbers.add(parts[1])
        elif len(parts) == 1:
            root = True
    ordered: list[str | None] = sorted(numbers, key=lambda n: (int(n) if n.isdigit() else 0, n))
    result: list[tuple[str | None, bool]] = [
        (number, number in sequenced or root_sequenced) for number in ordered
    ]
    if root:
        result.insert(0, (None, root_sequenced or bool(sequenced)))
    return result


def discover(game: Path) -> tuple[list[MapRef], int]:
    """Every runnable map in the installation, and how many containers held them.

    `corpus._walk` is the project's own content-walk, skip list and all; going
    through it rather than writing a second one is the point. `container_paths`
    is not enough on its own here because it sniffs for an *uncompressed* HPFS
    image and one shipped container is not one.
    """
    refs: list[MapRef] = []
    containers = 0
    for path in corpus._walk(game):
        data = container_bytes(path)
        if data is None:
            continue
        label = corpus.label(game, path)
        numbers = maps_in(data, label)
        if not numbers:
            continue
        containers += 1
        for number, scripted in numbers:
            where = f"{label} Maps/{number}" if number is not None else f"{label} (root)"
            refs.append(MapRef(path, number, where, scripted))
    return refs, containers


def parse_traps(stdout: str) -> collections.Counter:
    """`imrun`'s trap block as `message -> hits`. Empty when it said `no traps.`"""
    traps: collections.Counter = collections.Counter()
    collecting = False
    for line in stdout.splitlines():
        if TRAP_HEADER.match(line):
            collecting = True
            continue
        if not collecting:
            continue
        match = TRAP_LINE.match(line)
        if match is None:  # nothing follows the block, but do not assume it
            break
        traps[match.group(2)] += int(match.group(1))
    return traps


def run_map(
    imrun: Path,
    game: Path,
    ref: MapRef,
    turns: int,
    turn_ms: int,
    until_over: bool = False,
    timeout: int = RUN_TIMEOUT_SECONDS,
) -> MapResult:
    """One `imrun`, classified as run or refused.

    A non-zero exit is a **refusal**, not a failure of this sweep: the three
    editor blank templates and `newmap.BFHP` have no player table and cannot
    build a session. They are reported by name rather than dropped, because a
    denominator that quietly shrinks is how a corpus figure stops meaning what
    it says.
    """
    command = [str(imrun), str(game), str(ref.container), str(turns), str(turn_ms)]
    if ref.number is not None:
        command.append(ref.number)
    env = {**os.environ, "IMRUN_UNTIL_OVER": "1"} if until_over else None
    done = subprocess.run(
        command, capture_output=True, text=True, timeout=timeout, env=env
    )
    if done.returncode != 0:
        lines = [line for line in (done.stderr + done.stdout).splitlines() if line.strip()]
        return MapResult(ref, ran=False, reason=lines[0] if lines else f"exit {done.returncode}")
    return MapResult(ref, ran=True, traps=parse_traps(done.stdout))


def run_all(jobs: int, refs: list[MapRef], run) -> list[MapResult]:
    """`run` over every ref, `jobs` at a time, results in the order of `refs`.

    Threads, not processes, and that is enough: each worker spends its whole
    life in `subprocess.run` waiting on an `imrun`, which is the process that
    does the work. `Executor.map` returns in submission order, which is what
    keeps a parallel sweep's report identical to a serial one's.
    """
    if jobs <= 1 or len(refs) <= 1:
        return [run(ref) for ref in refs]
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        return list(pool.map(run, refs))


def report(results: list[MapResult], containers: int, top: int) -> None:
    """The two rankings, and everything needed to reproduce them."""
    ran = [r for r in results if r.ran]
    refused = [r for r in results if not r.ran]
    clean = [r for r in ran if not r.traps]

    print(f"maps      {len(results)} in {containers} containers: "
          f"{len(ran)} run, {len(refused)} refused")
    if refused:
        print("refused   no session could be built -- expected, and counted here rather "
              "than dropped from the denominator:")
        for result in refused:
            print(f"            {result.ref.label}  --  {result.reason}")
    if clean:
        print(f"clean     {len(clean)} map(s) ran and trapped nothing:")
        for result in clean:
            print(f"            {result.ref.label}")
    # The maps that did trap, each with its own tally: the rankings below sum
    # over them, and a trap has to be reproduced on one map to be fixed.
    trapped = [r for r in ran if r.traps]
    if trapped:
        print(f"trapped   {len(trapped)} map(s) ran and trapped:")
        for result in trapped:
            print(f"            {result.ref.label}")
            for message, hits in result.traps.most_common(top):
                print(f"              {hits:5d}  {message}")

    by_pair: collections.Counter = collections.Counter()
    for result in ran:
        by_pair.update(result.traps)

    # The same missing entry point reached from two scripts is two rows above
    # and one row here. Both are printed because both get quoted, and they are
    # not the same number.
    by_message: collections.Counter = collections.Counter()
    sources: dict[str, set[str]] = collections.defaultdict(set)
    for message, hits in by_pair.items():
        head, _, tail = message.rpartition("  (")
        if head and tail.endswith(")"):
            by_message[head] += hits
            sources[head].add(tail[:-1])
        else:
            by_message[message] += hits

    print(f"\ntraps     {len(by_pair)} distinct (message, source script), "
          f"{len(by_message)} distinct messages, {sum(by_pair.values()):,} hits")

    def show(title: str, counter: collections.Counter, annotate) -> None:
        rows = counter.most_common() if top <= 0 else counter.most_common(top)
        print(f"\n{title}")
        width = len(f"{max((hits for _, hits in rows), default=0):,}")
        for key, hits in rows:
            print(f"  {hits:>{width},}  {key}{annotate(key)}")
        if len(rows) < len(counter):
            print(f"  ... {len(counter) - len(rows)} more; --top 0 for all")

    show(
        f"By (message, source script) -- {len(by_pair)} of them, which is what "
        "`imrun` counts as distinct:",
        by_pair,
        lambda key: "",
    )
    show(
        f"By message -- {len(by_message)} of them, which is what the entry-point "
        "work list wants:",
        by_message,
        lambda key: f"  [{len(sources.get(key, ()))} script(s)]" if sources.get(key) else "",
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game", type=Path, default=host_coverage.find_game_dir())
    ap.add_argument("--imrun", type=Path, default=None)
    ap.add_argument("--turns", type=int, default=DEFAULT_TURNS)
    ap.add_argument("--turn-ms", type=int, default=DEFAULT_TURN_MS)
    ap.add_argument("--top", type=int, default=20, help="rows per ranking; 0 for all")
    ap.add_argument(
        "--war",
        action="store_true",
        help="also run the war pass: every skirmish for --war-turns, stopping where "
        "the match is decided (slow: a quarter of an hour on Balcans)",
    )
    ap.add_argument(
        "--war-turns",
        type=int,
        default=DEFAULT_WAR_TURNS,
        help=f"turns for the war pass (default {DEFAULT_WAR_TURNS}); only with --war",
    )
    ap.add_argument(
        "--jobs",
        type=int,
        default=os.cpu_count() or 1,
        help="maps run at once (default: every core); the output does not depend on it",
    )
    args = ap.parse_args()

    if args.imrun is None:
        # `tests/corpus.py`'s rule, not a second one: newest build wins, and a
        # build older than the newest engine source is refused by name. Three
        # separate corpus checks in this project's history validated a stale
        # binary and reported a pass.
        args.imrun, complaint = corpus.find_tool("imrun", "IMPERIVM_IMRUN")
        if complaint:
            print(complaint, file=sys.stderr)
            return 2
    if args.imrun is None or not args.imrun.is_file():
        print(f"no imrun at {args.imrun}; build it first", file=sys.stderr)
        return 2

    if args.game is None or not (args.game / "Packs" / "data.pak").is_file():
        print(
            f"no Imperivm installation found at {args.game}. "
            "Point --game at the directory holding Packs/, rle.mmp and gbr.exe, "
            "or set IMPERIVM_GAME_DIR.",
            file=sys.stderr,
        )
        return 2

    game = args.game.resolve()
    refs, containers = discover(game)
    if not refs:
        print(f"no map found in {game}", file=sys.stderr)
        return 2

    print(f"imrun     {args.imrun}")
    if args.turns > 0:
        print(f"sweep     {args.turns} turns of {args.turn_ms} ms on each map "
              "(the tally moves with both)")
        results = run_all(
            args.jobs,
            refs,
            lambda ref: run_map(args.imrun, game, ref, args.turns, args.turn_ms),
        )
        report(results, containers, args.top)

    skirmishes = [ref for ref in refs if not ref.scripted]
    if args.war and args.war_turns > 0 and skirmishes:
        print(f"\nwar       {args.war_turns} turns of {args.turn_ms} ms on each of the "
              f"{len(skirmishes)} map(s) with no mission sequences, stopping where a "
              "match is decided -- a second tally, not comparable with the one above")
        war = run_all(
            args.jobs,
            skirmishes,
            lambda ref: run_map(
                args.imrun, game, ref, args.war_turns, args.turn_ms,
                until_over=True, timeout=WAR_TIMEOUT_SECONDS,
            ),
        )
        print(f"war traps {sum(sum(r.traps.values()) for r in war if r.ran):,} hits")
        report(war, len({ref.container for ref in skirmishes}), args.top)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
