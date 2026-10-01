#!/usr/bin/env python3
"""Does every shipped map survive a save and a load, run map by map.

    python3 tools/save_sweep.py [--game DIR] [--imsave PATH] [--jobs N]
                                [--turns N] [--more N] [--no-mid-war]

Every `imsave` -- one per map and the mid-war case -- runs concurrently,
`--jobs` at a time (every core by default), and the report is in discovery
order whatever order they finish in, so it is identical to `--jobs 1`'s.

`imsave` answers this question for one map: run a real session, save it,
load the save into a fresh session, and require the two to be the same game
at every turn afterwards and byte for byte on a re-save. The figure worth
quoting is the sum over every shipped map, and this computes it the way
`trap_sweep.py` computes the trap tally -- by opening every container and
asking it which maps it holds, never from a list in this file.

**Why it exists.** The round trip held on every map for as long as the
headless tools ran with no entities loaded, and broke on the first turn after
the load the moment they did: `MoveState::walking`, the memory that turns the
walk cycle into a transition, was not in the movement section, so a loaded
session replayed every moving unit's walk from step zero. A check that runs
against the same inputs the app uses is the only kind that could have seen
it, and `tools/verify.py` runs this one.

A container that cannot build a session -- the four empty shells under
`Packs/` -- is reported as refused and counted in the denominator rather than
dropped, exactly as the trap sweep counts them.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
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
import trap_sweep  # noqa: E402

DEFAULT_TURNS = 40
DEFAULT_MORE = 20
RUN_TIMEOUT_SECONDS = 900

#: Saves taken mid-war: (container, turns before the save, turns compared).
#: The sweep above saves every map at turn 40, before anything has marched or
#: fallen, and so could not see the node table a load rebuilt with a gone
#: town hall's node in the map's corner. Crossroads at 1,600 turns has towns
#: broken and taken and armies on the road; `imsave` prints how many nodes
#: outlive their centre, how many centres stand broken and how many towns
#: changed hands, and a run where all three are zero is reported as a failure
#: of coverage, not a pass.
#: About twenty seconds, which doubles the sweep.
MID_WAR = (
    ("Scenarios/Crossroads.BFHP", 1600, 10),
)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game", type=Path, default=host_coverage.find_game_dir())
    ap.add_argument("--imsave", type=Path, default=None)
    ap.add_argument("--turns", type=int, default=DEFAULT_TURNS)
    ap.add_argument("--more", type=int, default=DEFAULT_MORE)
    ap.add_argument("--no-mid-war", action="store_true",
                    help="skip the saves taken mid-war (about twenty seconds)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                    help="imsave runs at once (default: every core); the output "
                    "does not depend on it")
    args = ap.parse_args()

    if args.imsave is None:
        args.imsave, complaint = corpus.find_tool("imsave", "IMPERIVM_IMSAVE")
        if complaint:
            print(complaint, file=sys.stderr)
            return 2
    if args.imsave is None or not args.imsave.is_file():
        print(f"no imsave at {args.imsave}; build it first", file=sys.stderr)
        return 2
    if args.game is None or not (args.game / "Packs" / "data.pak").is_file():
        print("no installation found; set IMPERIVM_GAME_DIR", file=sys.stderr)
        return 2

    refs, containers = trap_sweep.discover(args.game)
    print(f"imsave    {args.imsave}")
    print(f"sweep     save at turn {args.turns}, compare for {args.more} more, on each map")
    print(f"maps      {len(refs)} in {containers} containers")

    def imsave(command: list[str]) -> tuple[str, int] | None:
        """One `imsave`: its whole output and exit code, or None on a timeout."""
        try:
            done = subprocess.run(command, capture_output=True, text=True,
                                  timeout=RUN_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            return None
        return done.stdout + done.stderr, done.returncode

    commands: list[list[str]] = []
    for ref in refs:
        command = [str(args.imsave), str(args.game), str(ref.container),
                   "--turns", str(args.turns), "--more", str(args.more)]
        if ref.number is not None:
            command += ["--map-index", str(ref.number)]
        commands.append(command)
    # The saves taken mid-war. Each needs a long run before the save, so there
    # is one, not one per map; see `MID_WAR`. Submitted first, being the
    # longest, so that it is not the one left running alone at the end.
    mid_war: list[tuple[str, list[str] | None]] = []
    for container, turns, more in (() if args.no_mid_war else MID_WAR):
        path = args.game / container
        command = None
        if path.is_file():
            command = [str(args.imsave), str(args.game), str(path),
                       "--turns", str(turns), "--more", str(more)]
        mid_war.append((f"{container} at {turns}", command))
    queued = [command for _, command in mid_war if command is not None] + commands
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        # Threads are enough: each one only waits on its `imsave` process.
        # `map` returns in submission order, whatever order they finish in.
        outputs = dict(zip(map(id, queued), pool.map(imsave, queued)))

    ok: list[str] = []
    refused: list[tuple[str, str]] = []
    failed: list[tuple[str, str]] = []
    for ref, command in zip(refs, commands):
        result = outputs[id(command)]
        if result is None:
            failed.append((ref.label, "timed out"))
            continue
        text, returncode = result
        if "ROUND TRIP OK" in text:
            ok.append(ref.label)
        elif "session failed to build" in text:
            refused.append((ref.label, "no session could be built"))
        else:
            lines = [line.strip() for line in text.splitlines() if line.strip()]
            failed.append((ref.label, lines[-1] if lines else f"exit {returncode}"))

    print(f"clean     {len(ok)} map(s) saved, loaded and ran on identically")
    if refused:
        print(f"refused   {len(refused)} -- expected, and counted here rather than dropped:")
        for label, why in refused:
            print(f"            {label}  --  {why}")
    if failed:
        print(f"FAILED    {len(failed)} map(s) did not survive the round trip:")
        for label, why in failed:
            print(f"            {label}  --  {why}")
    war_ok: list[str] = []
    war_failed: list[tuple[str, str]] = []
    for label, command in mid_war:
        if command is None:
            war_failed.append((label, "not in this installation"))
            continue
        result = outputs[id(command)]
        if result is None:
            war_failed.append((label, "timed out"))
            continue
        text, returncode = result
        counts = [re.search(rf"^{key}\s+(\d+)", text, re.MULTILINE)
                  for key in ("fallen", "broken", "taken")]
        war = sum(int(c.group(1)) for c in counts if c is not None)
        if "ROUND TRIP OK" not in text:
            lines = [line.strip() for line in text.splitlines() if line.strip()]
            war_failed.append((label, lines[-1] if lines else f"exit {returncode}"))
        elif any(c is None for c in counts) or war == 0:
            # It round-trips but no longer covers what it is here for.
            war_failed.append((label, "no town is broken or taken by the save; move the turn"))
        else:
            war_ok.append(label)
    if war_ok:
        print(f"mid-war   {', '.join(war_ok)}: round-trip with a town broken or taken")
    for label, why in war_failed:
        print(f"FAILED    mid-war {label}  --  {why}")

    war = "" if args.no_mid_war else f"; mid-war {len(war_ok)} of {len(MID_WAR)}"
    print(f"\nsaves     {len(ok)} of {len(refs) - len(refused)} runnable maps round-trip; "
          f"{len(failed)} failed{war}")
    return 1 if failed or war_failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
