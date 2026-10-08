#!/usr/bin/env python3
"""Did this change help? One map, several seeds, and optionally two builds.

    python3 tools/seeds.py MAP [--game DIR] [--seeds K] [--from S]
                               [--turns T] [--turn-ms MS] [--map-index N]
                               [--imrun PATH] [--imrun-b PATH] [--jobs N]
                               [--overlap-every N] [--out DIR]

Runs `imrun MAP T MS --seed s` for the K seeds S, S+1, ... (default six seeds
from 1, 6,000 turns of 800 ms), `--jobs` at a time (default 4), and prints one
row per seed and a summary: the median, smallest and largest of each figure.
With `--imrun-b PATH` every seed is also run on a second build and the two
are printed side by side, with the change in each median -- the A/B a change
is judged by. MAP is a container path, absolute or relative to the game
directory (`Scenarios/Crossroads.BFHP`).

**Why it exists.** One long run measures one game. On Crossroads the turn the
match ends and the overlap census swing with the seed -- the seed even draws
each town's layout from its race's templates, walls and ground and all -- so a
before/after on seed 1 alone says which way that one game went, not whether
the change helped. A figure worth quoting is a median over seeds, with its
spread beside it, so a reader can see whether the change moved the middle or
stayed inside the noise.

**What each column is**, all from `imrun`'s own output:

* `end`, `winner` -- the turn the match was decided and who won, or `-`.
  `IMRUN_UNTIL_OVER` stops a decided run there, so `turns` is how far it ran.
* `caps`, `towns` -- settlements that changed hands, every kind (outposts,
  villages, strongholds), and of those the strongholds (`IMRUN_CAPTURES`).
* `deaths` -- death events, and by the dead unit's owner in the last column
  (`IMRUN_DEATHS`); a unit gone by the time it is reported counts as `p?`.
* `blows` -- strikes landed, melee and arrows alike.
* `made` -- units the computer players produced (p1 and up), from the match
  report's `units N produced`, and by player in the last column. The report's
  next number, before `killed`, is kills, not production.
* `overlap` -- the worst sampled turn of the overlap census: standing pairs
  nearer than their radii, and the turn (`IMRUN_OVERLAPS`, every
  `--overlap-every` turns).
* `traps` -- trap hits (the sum of `imrun`'s tally), not distinct traps.
* `hash` -- the world hash at the end. Two builds with one hash on a seed
  played the same game, and the summary counts those.

The environment is passed through, so `IMRUN_SEATS=0` and the like apply to
every run. `--out DIR` keeps each run's whole output, as
`<build>-seed<s>.txt`, for digging into the one seed that moved.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "tools"))

import corpus  # noqa: E402
import host_coverage  # noqa: E402

DEFAULT_SEEDS = 6
DEFAULT_TURNS = 6000
DEFAULT_TURN_MS = 800
#: Four, not every core: each run is one core for a minute or more, and these
#: machines are shared.
DEFAULT_JOBS = 4
#: The census is cheap, but every 25 turns is 240 samples in 6,000 turns,
#: which is enough to catch a pile that lasts twenty seconds of game time.
DEFAULT_OVERLAP_EVERY = 25
RUN_TIMEOUT_SECONDS = 3600

def run_env(overlap_every: int) -> dict[str, str]:
    """What a run needs `imrun` to print, and nothing else. The switches only
    read the world, so they do not change the game being measured -- the
    world hash with them is the hash without."""
    env = dict(os.environ)
    env.update({
        "IMRUN_UNTIL_OVER": "1",
        "IMRUN_CAPTURES": "1",
        "IMRUN_DEATHS": "1",
        "IMRUN_OVERLAPS": str(overlap_every),
    })
    return env


AFTER = re.compile(r"^after (\d+) turns", re.MULTILINE)
HASH = re.compile(r"^  hash\s+([0-9a-f]+)", re.MULTILINE)
MATCH = re.compile(r"^match\s+over=(yes|no) winner=(\d+)", re.MULTILINE)
ENDED = re.compile(r"^\s+ended at turn (\d+)", re.MULTILINE)
CAPTURE = re.compile(r"^  capture turn (\d+): #(\d+) kind (\d+) p(-?\d+) -> p(-?\d+)", re.MULTILINE)
DEATH = re.compile(r"^  death turn (\d+): \d+ \S+ p(-?\d+) ", re.MULTILINE)
STRIKES = re.compile(r"^  strikes\s+(\d+) blow", re.MULTILINE)
OVERLAP = re.compile(r"^  overlaps  worst at turn (\d+): (\d+) pair", re.MULTILINE)
TRAP_HEADER = re.compile(r"^(\d+) distinct traps, most-hit first:$", re.MULTILINE)
TRAP_LINE = re.compile(r"^\s+(\d+)\s\s(.+)$")

#: `SettlementKind::stronghold`.
STRONGHOLD = 1
#: `kNoPlayer`, which `MatchStatus::winner` holds while nobody has won.
NO_PLAYER = 255


@dataclasses.dataclass
class Result:
    """What one `imrun` run on one seed said."""

    seed: int
    ok: bool
    why: str = ""
    seconds: float = 0.0
    turns: int = 0
    ended: int | None = None
    winner: int | None = None
    captures: int = 0
    towns: int = 0
    deaths: int = 0
    by_owner: dict[int, int] = dataclasses.field(default_factory=dict)
    blows: int = 0
    made_by: dict[int, int] = dataclasses.field(default_factory=dict)
    overlap: int = 0
    overlap_turn: int = 0
    traps: int = 0
    hash: str = ""


#: `report p1: units 305 produced 20 killed ...` -- the first number is the
#: units that player produced.
REPORT = re.compile(r"^\s+report p(\d+): units (\d+) produced", re.MULTILINE)


def made(r: Result) -> int:
    return sum(r.made_by.values())


def parse(seed: int, text: str) -> Result:
    r = Result(seed=seed, ok=True)
    after = AFTER.search(text)
    digest = HASH.search(text)
    match = MATCH.search(text)
    if after is None or digest is None or match is None:
        lines = [line.strip() for line in text.splitlines() if line.strip()]
        return Result(seed=seed, ok=False, why=lines[-1] if lines else "no output")
    r.turns = int(after.group(1))
    r.hash = digest.group(1)
    if match.group(1) == "yes":
        ended = ENDED.search(text)
        r.ended = int(ended.group(1)) if ended else r.turns
        winner = int(match.group(2))
        r.winner = None if winner == NO_PLAYER else winner
    for capture in CAPTURE.finditer(text):
        r.captures += 1
        if int(capture.group(3)) == STRONGHOLD:
            r.towns += 1
    for death in DEATH.finditer(text):
        r.deaths += 1
        owner = int(death.group(2))
        r.by_owner[owner] = r.by_owner.get(owner, 0) + 1
    for report in REPORT.finditer(text):
        player = int(report.group(1))
        if player >= 1:
            r.made_by[player] = int(report.group(2))
    if (strikes := STRIKES.search(text)) is not None:
        r.blows = int(strikes.group(1))
    if (overlap := OVERLAP.search(text)) is not None:
        r.overlap_turn = int(overlap.group(1))
        r.overlap = int(overlap.group(2))
    if (header := TRAP_HEADER.search(text)) is not None:
        for line in text[header.end():].splitlines()[1:]:
            hit = TRAP_LINE.match(line)
            if hit is None:
                break
            r.traps += int(hit.group(1))
    return r


def run_one(imrun: Path, game: Path, container: Path, map_index: str | None, turns: int,
            turn_ms: int, seed: int, env: dict[str, str], out: Path | None,
            label: str) -> Result:
    command = [str(imrun), str(game), str(container), str(turns), str(turn_ms)]
    if map_index is not None:
        command.append(map_index)
    command += ["--seed", str(seed)]
    started = time.monotonic()
    try:
        done = subprocess.run(command, capture_output=True, text=True, env=env,
                              timeout=RUN_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return Result(seed=seed, ok=False, why=f"timed out after {RUN_TIMEOUT_SECONDS}s")
    text = done.stdout + done.stderr
    if out is not None:
        (out / f"{label}-seed{seed}.txt").write_text(text)
    result = parse(seed, text) if done.returncode == 0 else Result(
        seed=seed, ok=False,
        why=(text.strip().splitlines() or [f"exit {done.returncode}"])[-1])
    result.seconds = time.monotonic() - started
    return result


def owners_text(by_owner: dict[int, int]) -> str:
    return " ".join(f"p{p if p >= 0 else '?'}:{n}" for p, n in sorted(by_owner.items()))


HEADER = (f"{'seed':>5} {'build':<5} {'turns':>6} {'end':>6} {'winner':>6} {'caps':>5} "
          f"{'towns':>5} {'deaths':>6} {'blows':>7} {'made':>5} {'overlap':>12} {'traps':>6} "
          f"{'hash':<8} {'secs':>5}  deaths by owner | made by player")


def row(label: str, r: Result) -> str:
    if not r.ok:
        return f"{r.seed:>5} {label:<5} FAILED: {r.why}"
    end = "-" if r.ended is None else str(r.ended)
    winner = "-" if r.winner is None else f"p{r.winner}"
    overlap = f"{r.overlap}@{r.overlap_turn}"
    return (f"{r.seed:>5} {label:<5} {r.turns:>6} {end:>6} {winner:>6} {r.captures:>5} "
            f"{r.towns:>5} {r.deaths:>6} {r.blows:>7} {made(r):>5} {overlap:>12} {r.traps:>6} "
            f"{r.hash[:8]:<8} {r.seconds:>5.0f}  {owners_text(r.by_owner)} | "
            f"{owners_text(r.made_by)}")


#: The summarised figures: (name, how to read it off a result).
FIGURES = (
    ("end turn", lambda r: r.ended),
    ("captures", lambda r: r.captures),
    ("towns", lambda r: r.towns),
    ("deaths", lambda r: r.deaths),
    ("blows", lambda r: r.blows),
    ("made", made),
    ("overlap", lambda r: r.overlap),
    ("traps", lambda r: r.traps),
)


def spread(values: list[int]) -> tuple[float, int, int] | None:
    return (statistics.median(values), min(values), max(values)) if values else None


def fmt(s: tuple[float, int, int] | None) -> str:
    if s is None:
        return f"{'-':>9} {'':<15}"
    median, low, high = s
    median_text = f"{median:g}"
    return f"{median_text:>9} ({low:>5}..{high:<6})"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("map", help="the container, absolute or relative to the game directory")
    ap.add_argument("--game", type=Path, default=host_coverage.find_game_dir())
    ap.add_argument("--seeds", type=int, default=DEFAULT_SEEDS, help="how many seeds (default 6)")
    ap.add_argument("--from", dest="first", type=int, default=1, help="the first seed (default 1)")
    ap.add_argument("--turns", type=int, default=DEFAULT_TURNS)
    ap.add_argument("--turn-ms", type=int, default=DEFAULT_TURN_MS)
    ap.add_argument("--map-index", default=None, help="which Maps/<n> in a multi-map container")
    ap.add_argument("--imrun", type=Path, default=None, help="build A (default: this tree's)")
    ap.add_argument("--imrun-b", type=Path, default=None, help="build B, run on the same seeds")
    ap.add_argument("--jobs", type=int, default=DEFAULT_JOBS,
                    help="imrun processes at once (default 4); the output does not depend on it")
    ap.add_argument("--overlap-every", type=int, default=DEFAULT_OVERLAP_EVERY)
    ap.add_argument("--out", type=Path, default=None, help="keep each run's whole output here")
    args = ap.parse_args()

    if args.imrun is None:
        args.imrun, complaint = corpus.find_tool("imrun", "IMPERIVM_IMRUN")
        if complaint:
            print(complaint, file=sys.stderr)
            return 2
    builds = [("A", args.imrun)] + ([("B", args.imrun_b)] if args.imrun_b else [])
    for label, path in builds:
        if path is None or not path.is_file():
            print(f"no imrun at {path} (build {label})", file=sys.stderr)
            return 2
    if args.game is None or not (args.game / "Packs" / "data.pak").is_file():
        print("no installation found; set IMPERIVM_GAME_DIR or pass --game", file=sys.stderr)
        return 2
    container = Path(args.map)
    if not container.is_absolute():
        container = args.game / container
    if not container.is_file():
        print(f"no map at {container}", file=sys.stderr)
        return 2
    if args.out is not None:
        args.out.mkdir(parents=True, exist_ok=True)

    seeds = list(range(args.first, args.first + args.seeds))
    for label, path in builds:
        print(f"build {label}   {path}")
    print(f"map       {container.relative_to(args.game) if container.is_relative_to(args.game) else container}"
          + (f" Maps/{args.map_index}" if args.map_index else ""))
    print(f"runs      seeds {seeds[0]}..{seeds[-1]}, up to {args.turns} turns of {args.turn_ms} ms, "
          f"stopping when the match is decided; overlaps sampled every {args.overlap_every}")
    print()

    env = run_env(args.overlap_every)
    jobs = [(label, path, seed) for seed in seeds for label, path in builds]
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        # Threads are enough: each one only waits on its `imrun`. `map` gives
        # the results in submission order, whatever order they finish in.
        results = list(pool.map(
            lambda job: run_one(job[1], args.game, container, args.map_index, args.turns,
                                args.turn_ms, job[2], env, args.out, job[0]),
            jobs))
    by_build: dict[str, list[Result]] = {label: [] for label, _ in builds}
    print(HEADER)
    for (label, _, _), result in zip(jobs, results):
        by_build[label].append(result)
        print(row(label, result))

    print()
    print(f"{'':<10}" + "".join(f"  {label + ' median':>9} {'(min..max)':<15}" for label, _ in builds)
          + (f"  {'B-A median':>14}" if len(builds) == 2 else ""))
    for name, read in FIGURES:
        cells = []
        medians = []
        for label, _ in builds:
            values = [read(r) for r in by_build[label] if r.ok and read(r) is not None]
            s = spread(values)
            cells.append(f"  {fmt(s)}")
            medians.append(None if s is None else s[0])
        line = f"{name:<10}" + "".join(cells)
        if len(builds) == 2 and None not in medians:
            line += f"  {medians[1] - medians[0]:>+14g}"
        print(line)
    for label, _ in builds:
        done = [r for r in by_build[label] if r.ok]
        over = [r for r in done if r.ended is not None]
        winners: dict[str, int] = {}
        for r in over:
            key = "-" if r.winner is None else f"p{r.winner}"
            winners[key] = winners.get(key, 0) + 1
        won = ", ".join(f"{w} x{n}" for w, n in sorted(winners.items()))
        failed = len(by_build[label]) - len(done)
        print(f"build {label}   {len(over)} of {len(done)} decided within {args.turns} turns"
              + (f" ({won})" if won else "") + (f"; {failed} FAILED" if failed else ""))
    if len(builds) == 2:
        same = sum(1 for a, b in zip(by_build["A"], by_build["B"])
                   if a.ok and b.ok and a.hash == b.hash)
        print(f"same game {same} of {len(seeds)} seed(s) end on one world hash in both builds")
    return 0 if all(r.ok for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
