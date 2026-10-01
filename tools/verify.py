#!/usr/bin/env python3
"""Every check this project makes, in one command, in the right order.

    python3 tools/verify.py             # quick: after most changes
    python3 tools/verify.py --full      # plus the save sweep and the Python suite
    python3 tools/verify.py --war       # plus the trap sweep's war pass (slow)

**Tiers.** Run the smallest one that can see what you changed.

  quick   build, core tests, net tests, sound tests, core boundary, fixtures, host
          coverage, the trap sweep's short pass over every map
  --full  quick, plus the app build, the save sweep (every map and the
          mid-war case) and the Python corpus suite -- after touching the save
          path, the session, the app, the tools, or before calling a
          milestone done
  --war   whichever of the above, plus the trap sweep's war pass: every
          skirmish for thousands of turns. A quarter of an hour on Balcans
          alone since class behaviours start, so it is never in a tier; the
          Crossroads corpus test already asserts a trap-free war to a capture.
          And beside it the long network runs of Crossroads' war
          (`IMPERIVM_LONG_NET` in test_corpus_lockstep.py)

It exists because running them by hand costs more than running them. The
corpus tools refuse a binary older than the newest engine source -- a guard
three separate checks in this project's history needed, because they had each
validated a stale build and reported a pass -- so the *order* matters: this
builds first, always, and runs everything else against what it built.

**Which binaries.** Every check is told which build's tools to run, rather
than left to pick the newest by modification time, so that what a tier ran
does not depend on which directory happened to link last. The quick tier runs
`--build`'s (`build-core`). `--full` also builds `--app-build` (`build`, the
app's build) -- the app-net and app-speed tests run that app, and until this
nothing rebuilt it -- and runs the long corpus runs, the save sweep and the
Python suite, from that build's tools when it is not a Debug build.

That last is a cost decision: an unoptimised build is several times slower on
the long runs. `IMPERIVM_SPATIAL_CHECK`, which answers every spatial query a
second time by the straight scan, used to come with every Debug build and made
Crossroads to its capture take 683 s rather than 84 s; it is now a CMake option
of its own, off unless `-DIMPERIVM_SPATIAL_CHECK=ON`. `core_tests`' randomised
oracle checks the grid against the scan in every build. Turn the option on, and
pass `--corpus-build build-core`, for a change to the spatial index itself.

**Parallel.** After the builds, the net tests run alone -- they open real
loopback sockets on timers, and once failed a late-join check under load --
and then every other check runs at once. The sweeps run their maps in
parallel themselves, and the Python suite runs across every core with
`--dist loadgroup`: a module is one unit unless it is marked `spread`, and the
longest units start first (see `tests/conftest.py`). Results print in a fixed
order whatever order they finish in; `--serial` runs one at a time, for
comparison or a quiet machine.

What it does not do is decide anything. Every line it prints is another tool's
number.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent

#: `(label, command, the substring of the line worth printing, environment)`.
Check = tuple[str, list[str], "str | None", "dict[str, str] | None"]

#: The corpus tools, by the variable `tests/corpus.py`'s `find_tool` reads.
TOOLS = {
    "imrun": "IMPERIVM_IMRUN",
    "imsave": "IMPERIVM_IMSAVE",
    "imcheck": "IMPERIVM_IMCHECK",
    "imconform": "IMPERIVM_IMCONFORM",
    "immap": "IMPERIVM_IMMAP",
}


def run(command: list[str], env: dict[str, str] | None = None) -> tuple[int, str, float]:
    """Run one check; its exit code, whole output and wall time."""
    started = time.time()
    done = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                          env=None if env is None else {**os.environ, **env})
    return done.returncode, done.stdout + done.stderr, time.time() - started


def show(label: str, keep: str | None, result: tuple[int, str, float]) -> bool:
    """Print one check's line. `keep` names a substring of the line worth printing."""
    code, text, took = result
    lines = [line for line in text.splitlines() if line.strip()]
    note = ""
    if keep:
        matched = [line for line in lines if keep in line]
        note = matched[-1].strip() if matched else ""
    elif lines:
        note = lines[-1].strip()
    ok = code == 0
    print(f"{'ok  ' if ok else 'FAIL'} {label:<22} {took:6.1f}s  {note}")
    if not ok:
        for line in text.strip().splitlines()[-12:]:
            print(f"       {line}")
    return ok


def configured(build: str) -> bool:
    return (ROOT / build / "CMakeCache.txt").is_file()


def build_type(build: str) -> str:
    """`CMAKE_BUILD_TYPE` of a configured build directory, or empty."""
    try:
        text = (ROOT / build / "CMakeCache.txt").read_text(errors="replace")
    except OSError:
        return ""
    match = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.*)$", text, re.MULTILINE)
    return match.group(1).strip() if match else ""


def spatial_check_on(build: str) -> bool:
    """Whether a configured build directory has `IMPERIVM_SPATIAL_CHECK` on."""
    try:
        text = (ROOT / build / "CMakeCache.txt").read_text(errors="replace")
    except OSError:
        return False
    return re.search(r"^IMPERIVM_SPATIAL_CHECK:BOOL=(ON|1|TRUE|YES)$", text,
                     re.MULTILINE | re.IGNORECASE) is not None


def suite_python() -> str:
    """The Python that has pytest: this tree's `.venv`, else the main checkout's.

    A git worktree has no `.venv` of its own, and falling back to the system
    Python there made `--full` fail with "No module named pytest" -- in the
    trees every agent works in.
    """
    candidates = [ROOT / ".venv" / "bin" / "python"]
    common = subprocess.run(["git", "rev-parse", "--git-common-dir"], cwd=ROOT,
                            capture_output=True, text=True)
    if common.returncode == 0 and common.stdout.strip():
        main_root = (ROOT / common.stdout.strip()).resolve().parent
        candidates.append(main_root / ".venv" / "bin" / "python")
    for venv in candidates:
        if venv.is_file():
            return str(venv)
    return sys.executable


def tool(build: str, name: str) -> str:
    return str(ROOT / build / "engine" / "tools" / name)


def tool_env(build: str) -> dict[str, str]:
    """Every corpus tool pinned to `build`'s copy, where it has one."""
    return {
        variable: tool(build, name)
        for name, variable in TOOLS.items()
        if pathlib.Path(tool(build, name)).is_file()
    }


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--full", action="store_true",
                    help="also the app build, the save sweep and the Python corpus suite")
    ap.add_argument("--war", action="store_true",
                    help="also the trap sweep's war pass and the long network runs of a war "
                    "(slow; never in a tier)")
    ap.add_argument("--serial", action="store_true",
                    help="run the checks one at a time rather than at once")
    ap.add_argument("--build", default="build-core")
    ap.add_argument("--app-build", default="build",
                    help="--full: the app's build directory, built too when configured")
    ap.add_argument("--corpus-build", default=None,
                    help="--full: whose tools the save sweep and the suite run (default: "
                    "--app-build's unless it is a Debug build, else --build's)")
    args = ap.parse_args()
    started = time.time()

    # First, always. Everything below is measured against what this built,
    # and the corpus tools refuse to run against anything older.
    builds = [args.build]
    if args.full and args.app_build != args.build and configured(args.app_build):
        builds.append(args.app_build)
    for build in builds:
        if not show(f"build {build}" if build != args.build else "build", None,
                    run(["cmake", "--build", build])):
            print("\nbuild failed; nothing below it would have meant anything")
            return 1
    if spatial_check_on(args.build):
        # Not a failure, and it may be on on purpose; but it is most of this
        # tier's wall time, and nothing on the screen says so.
        print(f"     {args.build}/ has IMPERIVM_SPATIAL_CHECK on: every spatial query is "
              "answered twice, and core tests and the trap sweep take far longer")
    failed = 0
    # The lobby and the match over real loopback sockets: what core_tests may
    # not open a socket to test. Alone, before the rest start competing for
    # the machine; see the module docstring.
    if not show("net tests", "checks,", run([f"./{args.build}/engine/net/net_tests"])):
        failed += 1

    sweep_jobs = ["--jobs", "1"] if args.serial else []
    trap = [sys.executable, "tools/trap_sweep.py", "--imrun", tool(args.build, "imrun"),
            *sweep_jobs]
    if args.war:
        trap.append("--war")
    checks: list[Check] = [
        ("core tests", [f"./{args.build}/engine/tests/core_tests"], "checks,", None),
        # The WAV decoder, the sound entities and the mixer, on synthetic
        # buffers; no device (engine/sound/tests).
        ("sound tests", [f"./{args.build}/engine/sound/sound_tests"], "checks,", None),
        ("core boundary", [sys.executable, "tools/check_core_boundary.py"], "boundary", None),
        # `docs/legal.md` rule 1. Skips cleanly where there is no installation
        # to compare against, which is also where nobody is in a position to
        # paste from one. Keeps each file's verdict until the file, the
        # installation or the checker changes; see its docstring.
        ("fixtures", [sys.executable, "tools/check_fixtures.py"], None, None),
        ("host coverage", [sys.executable, "tools/host_coverage.py",
                           "--imcheck", tool(args.build, "imcheck")], "whole install", None),
        ("trap sweep" + (" + war" if args.war else ""), trap,
         "war traps" if args.war else "traps ", None),
    ]
    if args.full:
        corpus_build = args.corpus_build
        if corpus_build is None:
            corpus_build = args.build
            if (args.app_build in builds and build_type(args.app_build) != "Debug"
                    and tool_env(args.app_build)):
                corpus_build = args.app_build
        corpus_env = tool_env(corpus_build)
        print(f"     corpus tools from {corpus_build}/ ({build_type(corpus_build) or '?'})")
        # Every map saved, loaded and run on identically: the only check that
        # runs the save path against the inputs the app uses, which is how
        # `MoveState::walking` was found missing from the movement section.
        checks.append(("save sweep", [sys.executable, "tools/save_sweep.py",
                                      "--imsave", tool(corpus_build, "imsave"), *sweep_jobs],
                       "saves ", None))
        python = suite_python()
        # `swept` tests run the same command a sweep above runs, with the same
        # assertions; a bare pytest keeps them.
        suite = [python, "-m", "pytest", "tests", "-q", "-m", "not swept"]
        # Across every core when pytest-xdist is there (the dev extras bring
        # it). `loadgroup` with the groups `tests/conftest.py` assigns: a
        # module to a worker, as `loadfile` would, except the modules marked
        # `spread`, whose tests go wherever a worker is free; longest first.
        # The network tests are safe side by side: the UDP ones bind port 0,
        # every app-net test that hosts from the command line names a port of
        # its own, and the ones that host through the shipped screens -- the
        # default port -- are one group.
        has_xdist = subprocess.run([python, "-c", "import xdist"], capture_output=True)
        if not args.serial and has_xdist.returncode == 0:
            suite += ["-n", "auto", "--dist", "loadgroup", "--no-loadscope-reorder"]
        checks.append(("python suite", suite, "passed", corpus_env))

    if args.war:
        # The computer's war between networked peers, to the capture and well
        # into the war: lockstep on the map's own setup against imrun's turn
        # and hash, netplay over 2,400 turns, a seat changing hands four times
        # over 1,500. Minutes each, so opt-in in the suite (`IMPERIVM_LONG_NET`)
        # and here, beside the war pass they take less time than.
        python = suite_python()
        net_war = [python, "-m", "pytest", "tests/test_corpus_lockstep.py", "-q",
                   "-k", "agrees_to_the_capture or well_into_the_war"]
        if not args.serial and subprocess.run([python, "-c", "import xdist"],
                                              capture_output=True).returncode == 0:
            net_war += ["-n", "3"]
        checks.append(("net war", net_war, "passed",
                       {**tool_env(args.build), "IMPERIVM_LONG_NET": "1"}))

    if args.serial:
        results = [run(command, env) for _, command, _, env in checks]
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(checks)) as pool:
            futures = [pool.submit(run, command, env) for _, command, _, env in checks]
            results = [future.result() for future in futures]
    for (label, _, keep, _), result in zip(checks, results):
        if not show(label, keep, result):
            failed += 1
    print(f"\n{'FAILED' if failed else 'all ok'}  {time.time() - started:.1f}s wall")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
