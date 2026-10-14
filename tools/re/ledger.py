#!/usr/bin/env python3
"""The host audit's ledger: every entry point `gbr.exe` registers, and how far
this engine's answer to it has been checked against the executable.

    python3 tools/re/ledger.py            # write docs/audit/host-ledger.md
    python3 tools/re/ledger.py --summary  # print the counts only

**Why it exists.** The scripts the engine runs are the shipped ones; what they
get back from every call they make is this engine's. A wrong answer there is
invisible to a test written on the same assumption -- the seventh round's
temple lookup, player ledger, loyalty and `InRange` were all of that kind --
so the audit reads each entry point's body in the executable and records here
that it has. The ledger is regenerated, never edited, except for the one
column a reader decides: the status, kept in `docs/audit/host-status.tsv`.

It joins four things:

  * **the executable's table**, from `hostregs.py`: name, body address,
    registrar and signature, one row per distinct body;
  * **the engine's table**, from `imcheck surface`: which `(kind, name, arity)`
    this engine declares and whether a body exists for it;
  * **the corpus**, from `host_coverage.py`: how many call sites in the shipped
    scripts reach each name, which orders the work;
  * **the tree**: where the engine implements the name (the first
    `"Name", arity` registration under `engine/core/src`) and whether the body
    address is cited anywhere under `engine/core`.

Statuses, in `host-status.tsv` (`body<TAB>status<TAB>note`, one line per
body; a body with no line is `unread` if no script calls it is `unused`):

  verified  the whole body, and the callees that decide its answer, read; the
            behaviour described in prose at the implementation; a test holds it
  partial   read in part (some arms, or not its callees), or a known gap labelled
  inferred  implemented from its name, signature and call sites, not from the body
  unread    not looked at yet
  unused    no shipped script calls it

`cited` is mechanical and is not a status: an address in a comment says
somebody looked at the body once, not that the implementation matches it.

Ships no game data: names and addresses only, which the tree already cites.
"""
from __future__ import annotations

import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "src"))
sys.path.insert(0, str(ROOT / "tests"))

import host_coverage  # noqa: E402

STATUS_FILE = ROOT / "docs" / "audit" / "host-status.tsv"
LEDGER_FILE = ROOT / "docs" / "audit" / "host-ledger.md"
STATUSES = ("verified", "partial", "inferred", "unread", "unused")
ADDRESS = re.compile(r"0x00[4-7][0-9a-fA-F]{5}")


def binary_table() -> list[dict]:
    """`hostregs.py`'s rows, one per (name, body)."""
    out = subprocess.run([sys.executable, str(ROOT / "tools" / "re" / "hostregs.py")],
                         capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r"(\S+)\s+body=(0x[0-9a-f]+)\s+(\S+)\s*(.*)$", line)
        if m:
            rows.append({"name": m.group(1), "body": m.group(2), "registrar": m.group(3),
                         "signature": m.group(4).strip()})
    return rows


def engine_table(imcheck: Path) -> dict[str, list[str]]:
    """`name.lower()` -> ["member/2 impl", ...] from `imcheck surface`."""
    out = subprocess.run([str(imcheck), "surface"], capture_output=True, text=True,
                         check=True).stdout
    table: dict[str, list[str]] = collections.defaultdict(list)
    for line in out.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        head, arity, state = line.rsplit(" ", 2)
        kind, name = head.split("|", 1)
        table[name.lower()].append(f"{kind}/{arity}{'' if state == 'impl' else ' declared'}")
    return table


def call_sites(game: Path) -> collections.Counter:
    """Shipped call sites by `name.lower()`, every arity and kind together."""
    pack, container, _, _ = host_coverage.inventories(game)
    sites: collections.Counter = collections.Counter()
    for inventory in (pack, container):
        for (kind, name, _arity), count in host_coverage.census(inventory).items():
            sites[name.lower()] += count
    return sites


def tree_index() -> tuple[dict[str, str], dict[str, str]]:
    """Where each name is registered, and where each address is first cited."""
    source = ROOT / "engine" / "core" / "src"
    registered: dict[str, str] = {}
    cited: dict[str, str] = {}
    skip = {"host_surface.cpp"}  # the declared table, not an implementation
    for path in sorted((ROOT / "engine" / "core").rglob("*.[ch]pp")):
        rel = path.relative_to(ROOT).as_posix()
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            for address in ADDRESS.findall(line):
                cited.setdefault(address.lower(), f"{rel}:{number}")
            if path.is_relative_to(source) and path.name not in skip:
                for name in re.findall(r'"([A-Za-z_][A-Za-z0-9_]*)",\s*(?:\d+|arity)\s*,', line):
                    registered.setdefault(name.lower(), f"{rel}:{number}")
    return registered, cited


def read_statuses() -> dict[str, tuple[str, str]]:
    statuses: dict[str, tuple[str, str]] = {}
    if not STATUS_FILE.is_file():
        return statuses
    for number, line in enumerate(STATUS_FILE.read_text().splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        parts = line.split("\t")
        body, status = parts[0].strip().lower(), parts[1].strip() if len(parts) > 1 else ""
        if status not in STATUSES:
            sys.exit(f"{STATUS_FILE}:{number}: unknown status {status!r}")
        statuses[body] = (status, parts[2].strip() if len(parts) > 2 else "")
    return statuses


def subsystem(location: str) -> str:
    """The file an entry point lives in, which is how the audit batches it."""
    if not location:
        return "(not implemented here)"
    return Path(location.split(":")[0]).stem


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", type=Path, default=host_coverage.find_game_dir())
    ap.add_argument("--imcheck", type=Path, default=None)
    ap.add_argument("--summary", action="store_true", help="print the counts, write nothing")
    args = ap.parse_args()

    if args.imcheck is None:
        args.imcheck, complaint = host_coverage.find_imcheck()
        if args.imcheck is None:
            print(complaint, file=sys.stderr)
            return 2
    if args.game is None or not (args.game / "Packs" / "data.pak").is_file():
        print("no installation found; set IMPERIVM_GAME_DIR", file=sys.stderr)
        return 2

    rows = binary_table()
    ours = engine_table(args.imcheck)
    sites = call_sites(args.game)
    registered, cited = tree_index()
    statuses = read_statuses()

    # One row per body: a body registered under several names (aliases, or the
    # same entry point on several receiver types) is read once.
    by_body: dict[str, dict] = {}
    for row in rows:
        entry = by_body.setdefault(row["body"], {**row, "names": []})
        entry["names"].append(row["name"])
    for body, entry in by_body.items():
        # Members are registered as `Type::Name`; a script calls `x.Name`, and
        # the engine declares the bare name, so both are matched on that. Two
        # types with one name (`Unit::InRange`, `Catapult::InRange`) share the
        # name's call sites: the figure orders the work, it does not split it.
        keys = {n.split("::")[-1].lower() for n in entry["names"]}
        entry["sites"] = sum(sites.get(k, 0) for k in keys)
        entry["ours"] = sorted({e for k in keys for e in ours.get(k, [])})
        entry["where"] = next((registered[k] for k in sorted(keys) if k in registered), "")
        entry["cited"] = cited.get(body, "")
        status, note = statuses.get(body, ("", ""))
        if not status:
            status = "unread" if entry["sites"] > 0 else "unused"
        entry["status"], entry["note"] = status, note

    counts = collections.Counter(e["status"] for e in by_body.values())
    live = [e for e in by_body.values() if e["sites"] > 0]
    live_counts = collections.Counter(e["status"] for e in live)
    live_cited = sum(1 for e in live if e["cited"])
    summary = [
        f"bodies    {len(by_body)} registered by gbr.exe ({len(rows)} registrations)",
        f"called    {len(live)} reached by a shipped script "
        f"({sum(sites.values()):,} call sites in all)",
        f"cited     {live_cited} of those {len(live)} have their address cited under engine/core",
        "status    " + ", ".join(f"{s} {counts.get(s, 0)}" for s in STATUSES),
        "called    " + ", ".join(f"{s} {live_counts.get(s, 0)}" for s in STATUSES if s != "unused"),
    ]
    print("\n".join(summary))
    if args.summary:
        return 0

    groups: dict[str, list[dict]] = collections.defaultdict(list)
    for entry in live:
        groups[subsystem(entry["where"])].append(entry)
    lines = [
        "# Host audit ledger",
        "",
        "Generated by `python3 tools/re/ledger.py`; do not edit. Statuses live in",
        "`docs/audit/host-status.tsv`, and `tools/re/ledger.py`'s docstring says what each",
        "means. Names and addresses only: what a body *does* is described in prose at",
        "its implementation, never here (`docs/legal.md` rule 2).",
        "",
        "```",
        *summary,
        "```",
        "",
        "Entry points no shipped script calls are counted above and not listed.",
        "Batches are by the file that implements them, most-called file first.",
        "",
    ]
    order = sorted(groups.items(), key=lambda kv: -sum(e["sites"] for e in kv[1]))
    for group, entries in order:
        done = sum(1 for e in entries if e["status"] == "verified")
        lines += [f"## {group} -- {done} of {len(entries)} verified, "
                  f"{sum(e['sites'] for e in entries):,} call sites", "",
                  "| entry point | body | sites | engine | implemented at | cited | status | note |",
                  "|---|---|---:|---|---|---|---|---|"]
        for e in sorted(entries, key=lambda e: (-e["sites"], e["names"][0])):
            names = " / ".join(f"`{n}`" for n in sorted(set(e["names"])))
            where = f"`{e['where']}`" if e["where"] else "--"
            lines.append(f"| {names} | `{e['body']}` | {e['sites']:,} | "
                         f"{', '.join(e['ours']) or '--'} | {where} | {'yes' if e['cited'] else ''} | "
                         f"{e['status']} | {e['note']} |")
        lines.append("")
    LEDGER_FILE.parent.mkdir(parents=True, exist_ok=True)
    LEDGER_FILE.write_text("\n".join(lines))
    print(f"wrote     {LEDGER_FILE.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
