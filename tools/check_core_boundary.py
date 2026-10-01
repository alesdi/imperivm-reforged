#!/usr/bin/env python3
"""Assert that `engine/core` stays freestanding.

The core must transform bytes and state into state, and nothing else: no
windowing, no filesystem, no clock, no threads, no randomness it does not own.

This is not tidiness. The conformance harness replays recorded command streams
headlessly and compares per-tick state hashes against the original engine's
desync dumps, and it cannot exist unless the core runs with no platform beneath
it. Portability to WebAssembly, iOS and Android is the same property seen from a
different angle.

Boundaries erode one reasonable-looking include at a time, so this is a script rather
than a code-review habit. Run it before every commit that touches `engine/core`.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

CORE = Path("engine/core")

#: Headers the core may not include, with the reason a reviewer needs to hear.
FORBIDDEN: dict[str, str] = {
    "SDL3/": "SDL is a platform service; put the code in engine/platform",
    "SDL.h": "SDL is a platform service; put the code in engine/platform",
    "fstream": "the core does not touch the filesystem; take a byte span instead",
    "filesystem": "the core does not touch the filesystem; take a byte span instead",
    "cstdio": "the core does not perform I/O; return a value and let the caller print it",
    "iostream": "the core does not perform I/O; return a value and let the caller print it",
    "chrono": "the core has no clock; the tick count is state, wall time is not",
    "ctime": "the core has no clock; the tick count is state, wall time is not",
    "thread": "the core is single threaded by design; concurrency breaks determinism",
    "mutex": "the core is single threaded by design; concurrency breaks determinism",
    "atomic": "the core is single threaded by design; concurrency breaks determinism",
    "random": "the core owns one explicitly seeded RNG that is serialised as world state",
    "sys/socket.h": "the core has no network; hand it bytes (engine/net owns the socket)",
    "netinet/": "the core has no network; hand it bytes (engine/net owns the socket)",
    "arpa/inet.h": "the core has no network; hand it bytes (engine/net owns the socket)",
    "winsock2.h": "the core has no network; hand it bytes (engine/net owns the socket)",
    "ws2tcpip.h": "the core has no network; hand it bytes (engine/net owns the socket)",
}

#: Floating point in the simulation is a determinism hazard: the same expression
#: can round differently across compilers, architectures and optimisation
#: levels, and one divergent bit desynchronises a lockstep match.
FLOAT_DECL = re.compile(r"\b(float|double)\b(?!\s*\))")

INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)

ALLOW_FLOAT_MARKER = "imperivm: allow-float"


#: Matches a line comment and everything after it. Stripping these before
#: looking for `float` matters more than it sounds: prose legitimately contains
#: the words "float" and "double", and a checker that cannot tell code from
#: commentary trains people to ignore it. This one flagged the word "double" in
#: a doc comment and cost an agent a needless edit.
LINE_COMMENT = re.compile(r"//.*$")


def strip_comments(text: str) -> str:
    """Blank out comments, preserving line structure so numbers stay right."""
    # Block comments first, keeping their newlines.
    out: list[str] = []
    i = 0
    while i < len(text):
        start = text.find("/*", i)
        if start == -1:
            out.append(text[i:])
            break
        end = text.find("*/", start + 2)
        if end == -1:
            out.append(text[i:start])
            out.append("\n" * text.count("\n", start))
            break
        out.append(text[i:start])
        out.append("\n" * text.count("\n", start, end))
        i = end + 2
    without_blocks = "".join(out)
    return "\n".join(LINE_COMMENT.sub("", line) for line in without_blocks.splitlines())


def check_file(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    code = strip_comments(text)
    problems: list[str] = []

    for match in INCLUDE.finditer(text):
        header = match.group(1)
        for needle, reason in FORBIDDEN.items():
            if needle in header:
                line = text[: match.start()].count("\n") + 1
                problems.append(f"{path}:{line}: includes <{header}> — {reason}")
                break

    original = text.splitlines()
    for number, line in enumerate(code.splitlines(), start=1):
        if ALLOW_FLOAT_MARKER in original[number - 1]:
            continue
        if FLOAT_DECL.search(line):
            problems.append(
                f"{path}:{number}: floating point in the core — use fixed point, "
                f"or justify it with a trailing '// {ALLOW_FLOAT_MARKER}: <reason>'"
            )

    return problems


def main() -> int:
    if not CORE.is_dir():
        print(f"{CORE} not found; run from the repository root", file=sys.stderr)
        return 2

    sources = sorted(CORE.rglob("*.cpp")) + sorted(CORE.rglob("*.hpp"))
    problems: list[str] = []
    for path in sources:
        problems += check_file(path)

    if problems:
        print(f"core boundary violated ({len(problems)} in {len(sources)} files):\n")
        for problem in problems:
            print(f"  {problem}")
        print("\nSee docs/engine/architecture.md for what belongs where.")
        return 1

    print(f"core boundary intact across {len(sources)} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
