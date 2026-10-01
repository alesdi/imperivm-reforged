#!/usr/bin/env python3
"""How much of the shipped host surface is actually implemented.

    python3 tools/host_coverage.py [--game DIR] [--imcheck PATH]
    python3 tools/host_coverage.py --leaders [--limit N]

Joins two things that are easy to get wrong separately:

  * the **table**, from `imcheck surface`, which prints every entry point the
    engine declares and whether a body exists for it; and
  * the **corpus**, from parsing every `.vs` file in the installation.

Both halves have been wrong in this project's history. The table used to be
generated from `docs/formats/vs-host-api.md`, which covers only the 577 scripts
in `data.pak`; the other 308 live inside the 24 `.bfhp` containers and use 66
free and 13 member entry points that appear nowhere in the packs. And the
denominator used to be a number somebody remembered rather than one anybody
could recompute, which is what this script exists to stop.

The line that matters most is the last column: **call sites that resolve to no
table entry at all**. That is not a coverage figure, it is a correctness one --
such a call traps as an *unknown* rather than by name, which is the single
failure mode `declare_shipped_surface` exists to prevent. It should be zero.

`--leaders` prints the other half of that join: the entry points the table
declares and does not implement, ranked by how many shipped call sites reach
them. That ranking is what `docs/plan.html` quotes to decide what to write
next, and every hand-assembled version of it has gone stale -- the last one
named four of the top nine and dropped five that outranked the ones it kept.
It exists here so the list can be recomputed instead of remembered. The
pack/container split is printed per row because a list built from `data.pak`
alone is a list of 65% of the corpus: the 308 scripts inside the `.bfhp`
containers reach 66 free and 13 member entry points that appear nowhere in the
packs, and the three names at the head of this ranking are three of them.

**The figure will not reach 100%, and that is a fact about the game rather than
about this project.** Eight call sites, in five development leftovers, name an
entry point the shipping `gbr.exe` never registers; they trapped by name on the
original too. So the summary prints a ceiling under the percentage, `--leaders`
says which rows are those, and `--block` refuses to offer the scripts as work.
See `UNREGISTERED` for the list and how it was established.
"""

from __future__ import annotations

import argparse
import collections
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))

import corpus  # noqa: E402
from imperivm.formats import vs_parse  # noqa: E402
from imperivm.formats.bfhp import BlockFile  # noqa: E402
from imperivm.formats.pak import PackFile  # noqa: E402


#: Where an installation plausibly lives. `tests/conftest.py` keeps the fuller
#: list; it is not imported here because it pulls in pytest and this script
#: deliberately runs on a bare interpreter.
CANDIDATE_PATHS = (
    "~/Imperivm",
    "~/Games/Imperivm",
    "~/Downloads/Imperivm",
    "~/Desktop/Imperivm",
    "/Applications/Imperivm",
)


#: The eight call sites that can never be made to work, and the reason the
#: figure this script prints will never reach 100%.
#:
#: `gbr.exe` registers 1,498 entry points. Joining that table against this
#: census leaves every remaining name matched -- same receiver, same arity,
#: no disagreement anywhere -- **except these eight**, whose names reach no
#: registrar at all. A script calling one of them traps by name the moment it
#: is reached, and always did, on the shipping build the player owns.
#:
#: They are all development leftovers. `Choose`, `ListFolder` and
#: `ListFolderEx` are a level-editor file picker: the first two strings are
#: still in the executable's pool, orphaned, and the third is not. `EFC` does
#: not appear in the pool in any form, and the two settlement scripts that
#: call it are one line long, so their whole body is the dead call.
#: `GetSelection` is in the pool but only `_GetSelection` is registered.
#:
#: Re-derive with `python3 tools/re/hostregs.py`, which needs the player's own
#: executable and so cannot run here; that is why the answer is written down
#: rather than computed. It is a closed list against the full table, not a
#: sample -- if a name is ever added to it, the ceiling below moves with it.
UNREGISTERED = {
    ("free", "Choose", 2),
    ("free", "Choose", 3),
    ("free", "ListFolder", 1),
    ("free", "ListFolderEx", 2),
    ("free", "GetSelection", 0),
    ("member", "script", 0),
    ("member", "upgradeefc", 0),
    ("member", "cancelefcupgrade", 0),
}


def find_game_dir() -> Path | None:
    """`IMPERIVM_GAME_DIR`, then the candidates. None when there is no install.

    Discovery, not a hardcoded path. This defaulted to one contributor's
    `~/Desktop/Imperivm` and broke the moment that directory moved.
    """
    override = os.environ.get("IMPERIVM_GAME_DIR")
    if override:
        candidate = Path(override).expanduser()
        return candidate if (candidate / "Packs" / "data.pak").is_file() else None
    for raw in CANDIDATE_PATHS:
        candidate = Path(raw).expanduser()
        if (candidate / "Packs" / "data.pak").is_file():
            return candidate
    return None


def read_table(imcheck: Path) -> tuple[set, set]:
    """`(implemented, declared_only)` as `(kind, key, arity)` triples."""
    out = subprocess.run([str(imcheck), "surface"], capture_output=True, text=True, check=True)
    implemented: set = set()
    declared: set = set()
    for line in out.stdout.splitlines():
        if line.startswith("#"):
            continue
        head, arity, state = line.rsplit(" ", 2)
        kind, name = head.split("|", 1)
        # Member lookup is case-insensitive (`GetGAIKA` / `GetGaika`); free
        # functions are not (`This` and `this` are live at once in one script).
        key = (kind, name.lower() if kind == "member" else name, int(arity))
        (implemented if state == "impl" else declared).add(key)
    return implemented, declared


def census(inventory: vs_parse.Inventory) -> collections.Counter:
    """Call sites by `(kind, key, arity)`, folding `NO_PARENS` into arity 0."""
    out: collections.Counter = collections.Counter()
    for (name, arity), count in inventory.functions.items():
        out[("free", name, max(arity, 0))] += count
    for (name, arity), count in inventory.methods.items():
        out[("member", name.lower(), max(arity, 0))] += count
    return out


def inventories(game: Path) -> tuple[vs_parse.Inventory, vs_parse.Inventory, int, int]:
    pack = vs_parse.Inventory()
    container = vs_parse.Inventory()
    n_pack = n_container = 0
    for path in corpus.pack_paths(game):
        archive = PackFile(path)
        for entry in archive.entries:
            if entry.name.lower().endswith(".vs"):
                n_pack += 1
                pack.add_script(
                    vs_parse.parse(archive.read(entry.name).decode("cp1252", "replace"), entry.name)
                )
    for path in corpus.container_paths(game):
        block = BlockFile(path)
        for entry in block.entries:
            if not entry.is_dir and entry.name.lower().endswith(".vs"):
                n_container += 1
                container.add_script(
                    vs_parse.parse(block.read(entry.name).decode("cp1252", "replace"), entry.name)
                )
    return pack, container, n_pack, n_container


def leaders(
    declared: set,
    pack_sites: collections.Counter,
    container_sites: collections.Counter,
) -> list[tuple[int, tuple[str, str, int]]]:
    """Declared-but-unimplemented entry points, most call sites first.

    The join is the same one the coverage table performs, read the other way
    round: the table says which `(kind, key, arity)` triples have no body, the
    census says how many shipped sites reach each. An entry the table declares
    and no script ever calls is not a lead and is dropped -- 350-odd of the
    table's rows are in that state, and including them would bury the ones that
    are actually in the way.

    Ties are broken by the key descending, which is arbitrary but stable: two
    entry points with equal call counts have equal claim, and the point of
    sorting them at all is that two runs of this tool agree.
    """
    total = pack_sites + container_sites
    return sorted(((count, key) for key, count in total.items() if key in declared), reverse=True)


def print_leaders(
    rows: list[tuple[int, tuple[str, str, int]]],
    pack_sites: collections.Counter,
    container_sites: collections.Counter,
    limit: int,
) -> None:
    """The ranked list, widths measured from the rows actually shown."""
    shown = rows if limit <= 0 else rows[:limit]
    print(
        f"\n{len(rows)} declared entry points with no body, over "
        f"{sum(count for count, _ in rows):,} call sites, most sites first:"
    )
    counts = max((count for count, _ in shown), default=0)
    labels = max((len(f"{key[1]}/{key[2]}") for _, key in shown), default=0)
    for count, key in shown:
        kind, name, arity = key
        dead = "  -- gbr.exe registers no such name" if key in UNREGISTERED else ""
        print(
            f"{count:>{len(str(counts))}}  {kind:<6} {f'{name}/{arity}':<{labels}}  "
            f"(pack {pack_sites[key]}, container {container_sites[key]}){dead}"
        )
    if len(shown) < len(rows):
        print(f"... {len(rows) - len(shown)} more; --limit 0 for all")


def per_script(game: Path, declared: set) -> list[tuple]:
    """Every shipped script, with the entry points blocking it.

    Returns `(blocked_names, sites, label, name, [(name/arity, count), ...],
    {unreachable name/arity})`, sorted so that the scripts nearest to running
    come first. The sixth field is non-empty for the five scripts that call a
    name `gbr.exe` never registers: they are blocked, they are counted as
    blocked, and no amount of work here will finish them. See `UNREGISTERED`.

    **This answers a different question from `--leaders`, and the difference is
    the point.** The ranking by call sites says which name is in the way of the
    most *calls*; this says which name is in the way of the most *scripts*, and
    they disagree sharply. A name with 300 sites spread over one file unblocks
    one file. Four names with a dozen sites each can be the whole of what stands
    between forty verifiers and running.

    It was a hand count before this existed -- "`SIEGE.VS`, 155 sites, 4
    blocked" -- and a hand count is what `--leaders` had to be written to stop.
    """
    rows = []
    for label, name, data in _every_script(game):
        try:
            script = vs_parse.parse(data.decode("cp1252", "replace"), name)
        except Exception:  # a script the parser cannot read is not a lead
            continue
        inventory = vs_parse.Inventory()
        inventory.add_script(script)
        sites = census(inventory)
        blocked: collections.Counter = collections.Counter()
        dead: set[str] = set()
        for key, count in sites.items():
            if key in declared:
                blocked[f"{key[1]}/{key[2]}"] += count
                if key in UNREGISTERED:
                    dead.add(f"{key[1]}/{key[2]}")
        if not blocked:
            continue
        rows.append(
            (
                len(blocked),
                sum(sites.values()),
                label,
                name,
                sorted(blocked.items(), key=lambda kv: (-kv[1], kv[0])),
                dead,
            )
        )
    # Fewest distinct blockers first, then the most call sites, then the name --
    # so the head of the list is "nearly running and worth a lot", and two runs
    # agree.
    rows.sort(key=lambda row: (row[0], -row[1], row[2], row[3]))
    return rows


def print_block(rows: list[tuple], steps: int) -> None:
    """Greedy set cover: which names, taken *together*, finish the most scripts.

    `--leaders` ranks by call sites and `--scripts` ranks by scripts blocked
    alone. This answers the question those two cannot: **after taking that
    name, which one helps most next.** The three disagree, and the reason is
    clustering -- a family of eight accessors that always appear together
    finishes nothing until the eighth lands, so every one of them scores zero
    on `--scripts` and the family never surfaces.

    It found the fog-of-war family, which is six names and twenty-six scripts
    and whose best member is the sole blocker of seven.

    Greedy, not optimal: set cover is NP-hard and this is a work list, not a
    schedule. The `sites` column breaks ties, so between two names that free
    the same number of files the one behind more call sites wins.
    """
    blocked = [(sites, f"{label}:{name}", {key for key, _ in names})
               for _, sites, label, name, names, dead in rows if not dead]
    dropped = sum(1 for row in rows if row[5])
    chosen: set[str] = set()
    print(f"\ngreedily, the names that finish the most scripts *together*:")
    if dropped:
        # Not a rounding difference. Two of the five are one-blocker scripts,
        # which is the class this list exists to promote, and each of their
        # blockers is the sole blocker of exactly one file -- so greedy cover
        # scores them as one-name, one-finished-file wins. They are not.
        print(f"  ({dropped} scripts left out -- they call a name gbr.exe never "
              f"registers and can never finish)")
    for step in range(steps):
        gain: collections.Counter = collections.Counter()
        weight: collections.Counter = collections.Counter()
        for sites, _who, names in blocked:
            rest = names - chosen
            if len(rest) == 1:
                only = next(iter(rest))
                gain[only] += 1
                weight[only] += sites
        if not gain:
            print("  nothing else finishes a script on its own")
            break
        best = max(gain, key=lambda key: (gain[key], weight[key]))
        chosen.add(best)
        done = sum(1 for _, _, names in blocked if not (names - chosen))
        print(f"{step + 1:>3}. + {best:<26} finishes {gain[best]:>2} more "
              f"({done:>2} total, {weight[best]:,} sites)")


def _every_script(game: Path):
    """`(container label, path, bytes)` for every `.vs` in the installation."""
    for path in corpus.pack_paths(game):
        archive = PackFile(path)
        for entry in archive.entries:
            if entry.name.lower().endswith(".vs"):
                yield path.name, entry.name, archive.read(entry.name)
    for path in corpus.container_paths(game):
        block = BlockFile(path)
        for entry in block.entries:
            if not entry.is_dir and entry.name.lower().endswith(".vs"):
                yield corpus.label(game, path), entry.name, block.read(entry.name)


def print_scripts(rows: list[tuple], limit: int, want: str | None) -> None:
    if want:
        needle = want.lower()
        rows = [row for row in rows if needle in row[3].lower() or needle in row[2].lower()]
    shown = rows if limit <= 0 else rows[:limit]
    print(
        f"\n{len(rows)} scripts are blocked by at least one entry point with no "
        f"body, fewest blockers first:"
    )
    for blocked, sites, label, name, names, dead in shown:
        joined = ", ".join(f"{key} x{count}" for key, count in names)
        tail = "  -- unreachable, see UNREGISTERED" if dead else ""
        print(f"{blocked:>2} blocked, {sites:>4} sites  {label}:{name}{tail}")
        print(f"                        {joined}")
    if len(shown) < len(rows):
        print(f"... {len(rows) - len(shown)} more; --limit 0 for all")

    # And the other half of the join: which name blocks the most *scripts*.
    by_name: collections.Counter = collections.Counter()
    alone: collections.Counter = collections.Counter()
    for blocked, _sites, _label, _name, names, _dead in rows:
        for key, _count in names:
            by_name[key] += 1
            if blocked == 1:
                alone[key] += 1
    print("\nblocking the most scripts, and how many of those it blocks alone:")
    width = max((len(key) for key, _ in by_name.most_common(20)), default=0)
    for key, count in by_name.most_common(20):
        print(f"{count:>4} scripts  {key:<{width}}  {alone[key]:>4} of them alone")

    # The cheapest wins: names that are the *only* thing standing between a
    # script and running. Implementing one of these finishes a file rather than
    # advancing several.
    if alone:
        print("\nsole blocker of the most scripts -- one name, one finished file each:")
        width = max(len(key) for key, _ in alone.most_common(20))
        for key, count in alone.most_common(20):
            print(f"{count:>4} scripts  {key:<{width}}  ({by_name[key]} blocked in total)")


def find_imcheck() -> tuple[Path | None, str]:
    """The newest built `imcheck`, or a complaint naming why there is none.

    **The newest candidate wins, and one older than the engine is refused.**
    This defaulted to `build/engine/tools/imcheck` and nothing else, which is a
    scratch directory like any other: it reported 27,870 of 33,965 sites for
    two commits after the two `SpawnGroup` entry points landed and took the
    real figure to 28,178, because the binary it read predated them. The number
    this tool exists to keep honest was the number it got wrong -- the same
    stale-binary failure `tests/corpus.py` grew `find_tool` for, in the one
    place that never got it.

    **It is `corpus.find_tool` now rather than a second copy of it.** The copy
    that lived here carried the earlier version of the staleness rule, which
    counted *every* tool's `.cpp` as an input to every tool -- so editing
    `imsave.cpp` made `imcheck` stale, and no rebuild could clear it, because
    `ninja` correctly declines to relink a binary whose inputs have not
    changed. This script then refused to run at all, and the one number it
    exists to publish went unmeasured for as long as the edit stood. Two copies
    of a rule is one copy that does not get the fix; the rule lives in
    `tests/corpus.py` and this asks it.
    """
    path, complaint = corpus.find_tool("imcheck")
    return (None, complaint) if complaint else (path, "")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--game", type=Path, default=find_game_dir())
    ap.add_argument("--imcheck", type=Path, default=None)
    ap.add_argument(
        "--leaders",
        action="store_true",
        help="also rank the declared-but-unimplemented entry points by call sites",
    )
    ap.add_argument(
        "--scripts",
        action="store_true",
        help="rank shipped scripts by how few entry points stand between them and running",
    )
    ap.add_argument("--only", default=None, help="--scripts: substring filter on the path")
    ap.add_argument(
        "--block",
        type=int,
        default=0,
        metavar="N",
        help="greedily pick the N names that together finish the most scripts",
    )
    ap.add_argument(
        "--limit",
        type=int,
        default=20,
        help="rows for --leaders and --scripts; 0 prints every one (default: 20)",
    )
    args = ap.parse_args()
    if args.imcheck is None:
        args.imcheck, complaint = find_imcheck()
        if args.imcheck is None:
            print(complaint, file=sys.stderr)
            return 2

    if args.game is None or not (args.game / "Packs" / "data.pak").is_file():
        print(
            f"no Imperivm installation found at {args.game}. "
            "Point --game at the directory holding Packs/, rle.mmp and gbr.exe, "
            "or set IMPERIVM_GAME_DIR.",
            file=sys.stderr,
        )
        return 2
    if not args.imcheck.is_file():
        print(f"no imcheck at {args.imcheck}; build it first", file=sys.stderr)
        return 2

    implemented, declared = read_table(args.imcheck)
    pack, container, n_pack, n_container = inventories(args.game)
    pack_sites, container_sites = census(pack), census(container)

    rows = [
        (f"data.pak ({n_pack} scripts)", pack_sites),
        (f"containers ({n_container} scripts)", container_sites),
        (f"whole install ({n_pack + n_container} scripts)", pack_sites + container_sites),
    ]
    print(f"{'corpus':<32} {'sites':>8} {'implemented':>18} {'declared':>10} {'unknown':>9}")
    unknown_total = 0
    for label, sites in rows:
        total = sum(sites.values())
        done = sum(v for k, v in sites.items() if k in implemented)
        dec = sum(v for k, v in sites.items() if k in declared)
        unknown = total - done - dec
        unknown_total += unknown if label.startswith("whole") else 0
        print(f"{label:<32} {total:>8,} {done:>11,} ({100 * done / total:>3.0f}%) {dec:>10,} {unknown:>9,}")

    # The ceiling, stated once, under the figure it is a ceiling on. It is
    # worth a line of output because the target this project was steering by
    # was "100%", and 100% is not reachable: eight of these sites call a name
    # the shipping executable never registers, so they trapped by name on the
    # original too. See `UNREGISTERED`.
    everywhere = pack_sites + container_sites
    total = sum(everywhere.values())
    dead = sum(v for k, v in everywhere.items() if k in UNREGISTERED)
    reachable = total - dead
    print(
        f"\n{dead} of those sites call a name gbr.exe never registers and could "
        f"not run on the original either;\nthe ceiling is {reachable:,} of "
        f"{total:,} ({100 * reachable / total:.3f}%)."
    )

    # Before the unknown-sites list, and unconditionally after the table, so
    # that asking for the ranking never costs sight of the correctness column
    # above it: the ranking is a work list, the zero is a claim.
    if args.leaders:
        print_leaders(leaders(declared, pack_sites, container_sites),
                      pack_sites, container_sites, args.limit)

    if args.scripts:
        print_scripts(per_script(args.game, declared), args.limit, args.only)

    if args.block:
        print_block(per_script(args.game, declared), args.block)

    if unknown_total:
        worst = sorted(
            ((v, k) for k, v in (pack_sites + container_sites).items()
             if k not in implemented and k not in declared),
            reverse=True,
        )[:20]
        print("\nCall sites with no table entry -- these trap as *unknown*, not by name:")
        for count, (kind, name, arity) in worst:
            print(f"  {kind:<6} {name}/{arity}  {count} sites")
        return 1
    print("\nEvery host call site in the installation resolves to a named table entry.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
