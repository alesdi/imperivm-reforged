#!/usr/bin/env python3
"""Read the player's own `gbr.exe` as decompiled C, from a local cache.

    python3 tools/re/decomp.py build              # analyse and decompile everything (~once)
    python3 tools/re/decomp.py show 0x005d72e0    # the function that contains an address
    python3 tools/re/decomp.py callers 0x005a7800 # who calls it
    python3 tools/re/decomp.py callees 0x005d72e0 # what it calls
    python3 tools/re/decomp.py grep "Unit::InRange"  # functions whose text mentions it

**Why a cache.** The host audit (`docs/audit/`, `tools/re/ledger.py`) reads a
thousand bodies and the callees that decide their answers, often several
agents at once. Asking Ghidra one function at a time re-analyses the
executable on every call -- about two minutes each -- and a decompiler that
only knows the functions its auto-analysis found never sees a host body,
which `gbr.exe` only ever *pushes* to its registrar, or a timer callback,
which it only ever stores. `build` runs Ghidra's headless analyzer once into a
saved project, makes a function of every host body `hostregs.py` lists, every
address in `tools/re/entry_points.txt`, and every code address an instruction
pushes or moves as an immediate, and writes each function's C to a file. After
that every question is a file read.

**Needs Ghidra 12 and a JDK 21.** `GHIDRA_INSTALL_DIR` names the installation
(`brew install ghidra` puts it at `$(brew --prefix ghidra)/libexec`), and
`JAVA_HOME` the JDK if `java` on `PATH` is not 21. Like `capstone` for
`gbr.py`, nothing that builds, tests or runs the engine needs either.

**Read the C critically.** It is Ghidra's reading, not the program. The first
body checked against an independent disassembly, the loyalty gain tick
(0x005c5690), agreed on every rule and still showed its unit counter as an
uninitialised register (`unaff_EBX`): a value kept in a callee-saved register
across a loop is a common casualty. Where an answer turns on such a detail,
confirm it with `gbr.py d <address>`.

**The cache is not the project's and must never be committed** (`docs/legal.md`
rule 2): it lives under `IMPERIVM_RE_CACHE`, by default
`~/Library/Caches/imperivm-re` on macOS, outside the tree. Read it to understand a body; describe the behaviour in prose
at the implementation; write the implementation from the prose.
"""
from __future__ import annotations

import argparse
import bisect
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
# Ghidra refuses a project under any path element that starts with a dot, so
# not `~/.cache`: the platform's cache folder on macOS, a plain one elsewhere.
CACHE = Path(os.environ.get("IMPERIVM_RE_CACHE") or (
    Path.home() / "Library" / "Caches" / "imperivm-re" if sys.platform == "darwin"
    else Path.home() / "imperivm-re-cache"))
OUT = CACHE / "decomp"


def executable() -> Path:
    for candidate in (os.environ.get("IMPERIVM_GBR"),
                      os.environ.get("IMPERIVM_GAME_DIR") and
                      str(Path(os.environ["IMPERIVM_GAME_DIR"]) / "gbr.exe"),
                      str(Path.home() / "Downloads" / "Imperivm" / "gbr.exe")):
        if candidate and Path(candidate).is_file():
            return Path(candidate)
    sys.exit("no gbr.exe: set IMPERIVM_GBR or IMPERIVM_GAME_DIR")


def headless() -> Path:
    home = os.environ.get("GHIDRA_INSTALL_DIR")
    if not home:
        sys.exit("set GHIDRA_INSTALL_DIR to the Ghidra installation (brew: $(brew --prefix ghidra)/libexec)")
    path = Path(home) / "support" / "analyzeHeadless"
    if not path.is_file():
        sys.exit(f"no analyzeHeadless under {home}")
    return path


def entry_points() -> list[str]:
    """Every host body hostregs.py finds, then the hand-kept list."""
    out = subprocess.run([sys.executable, str(HERE / "hostregs.py")], capture_output=True,
                         text=True, check=True).stdout
    found = re.findall(r"body=(0x[0-9a-f]+)", out)
    extra = HERE / "entry_points.txt"
    if extra.is_file():
        for line in extra.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                found.append(line.split()[0])
    return sorted(set(a.lower() for a in found))


def build(args: argparse.Namespace) -> int:
    exe = executable()
    OUT.mkdir(parents=True, exist_ok=True)
    points = CACHE / "entry_points.txt"
    points.write_text("\n".join(entry_points()) + "\n")
    project = CACHE / "project"
    project.mkdir(parents=True, exist_ok=True)
    have = (project / "gbr.gpr").is_file()
    command = [str(headless()), str(project), "gbr"]
    # The analysis is the slow part and is kept: a rebuild reopens the saved
    # program and only re-runs the export (`--reanalyse` starts over).
    if have and not args.reanalyse:
        command += ["-process", exe.name, "-noanalysis"]
    else:
        command += ["-import", str(exe), "-overwrite"]
    command += ["-scriptPath", str(HERE / "ghidra"),
                "-postScript", "ExportDecompiled.java", str(OUT), str(points)]
    print("running", " ".join(command), flush=True)
    return subprocess.run(command).returncode


class Index:
    def __init__(self) -> None:
        path = OUT / "index.tsv"
        if not path.is_file():
            sys.exit(f"no cache at {OUT}; run `tools/re/decomp.py build` first")
        self.rows: list[tuple[int, int, str, str, list[str], list[str]]] = []
        for line in path.read_text().splitlines():
            if line.startswith("#") or not line.strip():
                continue
            entry, name, low, high, callers, callees = (line.split("\t") + ["", ""])[:6]
            self.rows.append((int(low, 16), int(high, 16), entry, name,
                              [c for c in callers.split(",") if c],
                              [c for c in callees.split(",") if c]))
        self.rows.sort()
        self.starts = [r[0] for r in self.rows]

    def containing(self, address: int):
        i = bisect.bisect_right(self.starts, address) - 1
        # Bodies can be discontiguous; a min/max span is the honest bound, so
        # prefer the function whose entry is the address itself.
        for row in self.rows:
            if int(row[2], 16) == address:
                return row
        while i >= 0:
            row = self.rows[i]
            if row[0] <= address <= row[1]:
                return row
            i -= 1
            if address - self.rows[i + 1][0] > 0x10000:
                break
        return None


def address_of(text: str) -> int:
    return int(text, 16)


def show(args: argparse.Namespace) -> int:
    row = Index().containing(address_of(args.address))
    if row is None:
        print(f"no function contains {args.address}; add it to tools/re/entry_points.txt and rebuild")
        return 1
    entry = row[2]
    print(f"// {row[3]} at {entry}, body {row[0]:#010x}..{row[1]:#010x}, "
          f"{len(row[4])} caller(s), {len(row[5])} callee(s)")
    print((OUT / "functions" / f"{entry}.c").read_text())
    return 0


def related(args: argparse.Namespace, which: int) -> int:
    index = Index()
    row = index.containing(address_of(args.address))
    if row is None:
        print(f"no function contains {args.address}")
        return 1
    names = {r[2]: r[3] for r in index.rows}
    for entry in row[which]:
        print(f"{entry}  {names.get(entry, '')}")
    return 0


def grep(args: argparse.Namespace) -> int:
    index = Index()
    pattern = re.compile(args.pattern)
    hits = 0
    for row in index.rows:
        text = (OUT / "functions" / f"{row[2]}.c").read_text(errors="replace")
        if pattern.search(text):
            print(f"{row[2]}  {row[3]}")
            hits += 1
    return 0 if hits else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build", help="analyse once and decompile every function into the cache")
    b.add_argument("--reanalyse", action="store_true", help="re-import and re-run the analysis")
    for name in ("show", "callers", "callees"):
        sub.add_parser(name).add_argument("address")
    sub.add_parser("grep").add_argument("pattern")
    args = ap.parse_args()
    if args.command == "build":
        return build(args)
    if args.command == "show":
        return show(args)
    if args.command == "callers":
        return related(args, 4)
    if args.command == "callees":
        return related(args, 5)
    return grep(args)


if __name__ == "__main__":
    raise SystemExit(main())
