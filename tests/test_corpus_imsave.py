"""End-to-end tests for `imsave` against the shipped containers.

`engine/tests/core_tests` links `imperivm_core` alone -- "if a test needs a file
or a window, it is testing the wrong layer" -- so the save/load round trip it
can run is synthetic: a world built by hand, eleven systems poked one field at a
time. That proves the encoder and the decoder agree with each other. It cannot
prove the save carries *Numantia*: 1,680 objects, thirty-odd settlements, several
hundred suspended coroutines, eleven systems with state in all of them.

This is the half that opens the installation. `imsave` does the work and asserts
on itself; these tests drive it over real containers and require it to say so.

Three properties, and the third exists because the first two have a blind spot:

* `verify_hashes` at the load, which catches any system that folds into `slots`;
* the per-turn hash sequences, compared at *every* turn after the load;
* the re-save is byte-identical -- **the only one that sees `command`, `ai` and
  `areas`**, none of which contributes to any hash.

Point `IMPERIVM_IMSAVE` at a built binary, or let discovery find one.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

from conftest import requires_game

import corpus

# Each test is its own subprocess run and the module's fixtures only find a
# binary, so under `--dist loadgroup` its tests may go to different workers.
pytestmark = [requires_game, pytest.mark.spread]


#: One of each shape the installation ships: a scripted battle, the wildlife
#: skirmish (the only one whose movement and command queues move in a headless
#: run), and the conquest. Not the whole corpus -- `imsave` runs that on demand
#: and it costs a minute.
CONTAINERS = (
    "Adventures/GreatBattles/2_Great_Battles_Numantia.bfhp",
    "Scenarios/Balcans.BFHP",
    "Conquests/mediterranean.BFHP",
)


@pytest.fixture(scope="module")
def imsave() -> Path:
    """The newest built `imsave`, and only if it is newer than the engine.

    This used to be a first-wins walk over `("build-saves", "build")`, and the
    scratch copy in `build-saves` was three-quarters of an hour older than the
    engine -- so every assertion below spent a session validating a build in
    which `CombatSystem` was still empty, and reporting a pass. See
    `corpus.find_tool`. `IMPERIVM_IMSAVE` overrides.
    """
    path, complaint = corpus.find_tool("imsave", "IMPERIVM_IMSAVE")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


def run(imsave: Path, game_dir: Path, container: str, *extra: str) -> str:
    path = game_dir / container
    if not path.is_file():
        pytest.skip(f"the installation has no {container}")
    result = subprocess.run(
        [str(imsave), str(game_dir), str(path), *extra],
        capture_output=True,
        text=True,
        timeout=900,
    )
    assert result.returncode == 0, (
        f"imsave refused {container} {' '.join(extra)}:\n"
        f"{result.stdout}\n{result.stderr}"
    )
    return result.stdout


@pytest.mark.parametrize("container", CONTAINERS)
def test_a_real_session_round_trips_through_a_save(imsave, game_dir, container):
    """Save at turn 100, load into a fresh session, run 20 more on both.

    The assertion is `ROUND TRIP OK`, which `imsave` prints only when all three
    checks passed -- and the two lines below it are asserted separately so that
    a future change which quietly drops one of them cannot keep the headline.
    """
    output = run(imsave, game_dir, container, "--turns", "100", "--more", "20")
    assert "ROUND TRIP OK" in output, output
    assert re.search(r"hashes\s+match at every one of the 20 turns", output), output
    assert "re-save   byte-identical" in output, output
    # `unrestored_systems` and `unconsumed` are printed only when non-empty, and
    # either would mean a section this build wrote or expected went missing.
    assert "UNRESTORED" not in output, output
    assert "UNCONSUMED" not in output, output


# `tools/save_sweep.py`'s `MID_WAR` runs this same command and requires the same
# (ROUND TRIP OK, which `imsave` prints only when the hashes match at every
# turn and the re-save is byte-identical, and a town fallen, broken or taken),
# so `verify.py --full`, which runs the sweep, deselects it here.
@pytest.mark.swept
def test_a_save_taken_mid_war_loads_back_into_the_same_game(imsave, game_dir):
    """Crossroads at 1,600 turns: armies marching, towns broken and taken.

    Every other round trip here is saved in the first hundred turns, before
    anything has marched or fallen, and so none could see what this one
    first caught: the AI's node table was rebuilt on load over the restored
    world, where a town hall that was gone resolved to nowhere, so that
    town's node moved to (-1,-1) and a priest marching on it dropped its
    route three turns after the load. A load now restores the nodes.

    Town halls are not razed any more -- a building at no health stands
    broken, as it does in `gbr.exe` -- so the war this save has to carry is
    the one `imsave` counts: central buildings standing broken (tier 3,
    `IsBroken`) and settlements in other hands than the map's (3 and 1 at
    this turn). At least one of those, or a fallen node, is asserted, so the
    test says when it has stopped saving a game at war: a change to the AI
    that leaves every town whole and unowned past turn 1,600 would otherwise
    leave this passing and blind. The node restore itself keeps its core
    test, which erases a town hall outright. About fifteen seconds.
    """
    output = run(imsave, game_dir, "Scenarios/Crossroads.BFHP", "--turns", "1600", "--more", "10")
    war = 0
    for label in ("fallen", "broken", "taken"):
        found = re.search(rf"^{label}\s+(\d+) ", output, re.MULTILINE)
        assert found is not None, output
        war += int(found.group(1))
    assert war >= 1, output
    assert "ROUND TRIP OK" in output, output
    assert re.search(r"hashes\s+match at every one of the 10 turns", output), output
    assert "re-save   byte-identical" in output, output


def test_the_writer_is_deterministic(imsave, game_dir):
    """Two saves of one session are byte-identical.

    Asserted inside the tool -- it refuses with "two saves of one session
    differ" -- so this test is really about the tool having been run at all.
    A save that re-encodes differently is a save whose meaning depends on which
    end of the round trip you are standing at, and it would make a shared save
    useless between peers.
    """
    output = run(
        imsave, game_dir, CONTAINERS[1], "--turns", "60", "--more", "10"
    )
    assert "two saves of one session differ" not in output
    assert re.search(r"^saved\s+\d+ bytes$", output, re.MULTILINE), output


def test_deleting_a_system_section_is_caught(imsave, game_dir):
    """A check that has never been seen to fail is not a check.

    `--verify-faults` deletes each system's section from the save in turn and
    requires the round trip to notice. `Balcans` is the container to do it on:
    it is the only one whose `movement` and `command` state moves in a headless
    run, and `command` contributes to no hash at all, so it is caught by the
    byte comparison and by nothing else.

    A system whose section at turn N is byte-identical to its section at turn 0
    is reported as "nothing to lose" rather than as a pass, because deleting it
    genuinely loses nothing -- a fact about the scenario, not about the save.
    """
    output = run(
        imsave,
        game_dir,
        CONTAINERS[1],
        "--turns",
        "300",
        "--more",
        "25",
        "--verify-faults",
    )
    assert "ROUND TRIP OK" in output, output
    assert "NOT CAUGHT" not in output, output
    # The two the world hash cannot see. If either of these ever reads
    # "caught by verify_hashes", something started hashing script or AI state,
    # which the nine retail dumps say the original did not.
    assert re.search(r"^  command\s+caught by re-save differs", output, re.MULTILINE), output
    assert re.search(r"^  ai\s+caught by re-save differs", output, re.MULTILINE), output
    # And one that does fold into `slots`, so the hash check earns its place too.
    assert re.search(r"^  economy\s+caught by load refused", output, re.MULTILINE), output
    # `combat`, which is here because for a long time it was not. The system was
    # registered, hashed and serialised while `CombatSystem::add` had no caller
    # outside the unit tests, so `units_` was empty in every real session and the
    # section at turn 300 was byte-identical to the section at turn 0. This line
    # read `combat  nothing to lose` -- not a failure, and not a pass either.
    # It is asserted on every map-bearing container by
    # `test_a_real_session_round_trips_through_a_save` only indirectly; here it
    # is named, so that combat going quiet again fails by name.
    assert re.search(r"^  combat\s+caught by load refused", output, re.MULTILINE), output


# ---------------------------------------------------------------------------
# the save on disk
# ---------------------------------------------------------------------------
#
# Everything above round-trips the envelope inside one process. The file the
# application writes is the envelope inside a `.bfhp` container beside a
# manifest (`docs/formats/save.md`, "the file on disk"), and two claims about
# that file can only be checked from here: that the C++ container writer
# reproduces the Python reference byte for byte, and that `imrun --load`
# resumes what `imsave --out` wrote.


@pytest.fixture(scope="module")
def imrun() -> Path:
    path, complaint = corpus.find_tool("imrun", "IMPERIVM_IMRUN")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


def test_the_save_file_is_the_reference_writers_container(imsave, game_dir, tmp_path):
    """`BlockFileBuilder` is a port of `bfhp.py`'s builder, and this holds the two
    to each other: the file `imsave --out` wrote, taken apart with the Python
    reader and rebuilt with the Python writer in the documented creation order,
    is the same bytes. The session is several hundred kilobytes, so the check
    crosses the direct-block limit, the promotion and a run of index blocks --
    the whole of the layout a save exercises.
    """
    from imperivm.formats.bfhp import BlockFile, build_plan

    target = tmp_path / "balcans.bfhp"
    output = run(imsave, game_dir, CONTAINERS[1], "--turns", "40", "--more", "5", "--out", str(target))
    assert "ROUND TRIP OK" in output, output
    assert target.is_file()

    parsed = BlockFile(target)
    parsed.validate()
    assert [(e.name, e.is_dir) for e in parsed.entries] == [
        ("save.ini", False),
        ("session.isav", False),
    ]
    assert parsed.read("session.isav")[:4] == b"ISAV"
    manifest = parsed.read("save.ini").decode("cp1252")
    assert "[Save]" in manifest
    assert "container=Scenarios/Balcans.BFHP" in manifest
    assert "turns=40" in manifest

    plan = [
        ("save.ini", parsed.read("save.ini")),
        ("session.isav", parsed.read("session.isav")),
    ]
    assert build_plan(plan, parsed.block_size) == target.read_bytes()
    # And the reader's own account of the container agrees with the writer's.
    assert build_plan(parsed.to_plan(), parsed.block_size) == target.read_bytes()


def test_imrun_resumes_what_imsave_wrote(imsave, imrun, game_dir, tmp_path):
    """The file is a save of a *game*, not of a world: `imrun --load` rebuilds
    the session the manifest names -- container, map number, seed -- applies the
    envelope over it, and the hashes recorded at the save come back. Then it
    runs on. The conquest is the container to do it on, because its manifest
    has to carry a map number and its map has to carry its own sequences.
    """
    target = tmp_path / "med7.bfhp"
    output = run(
        imsave, game_dir, CONTAINERS[2], "--map-index", "7", "--turns", "30", "--more", "5",
        "--out", str(target),
    )
    assert "ROUND TRIP OK" in output, output

    result = subprocess.run(
        [str(imrun), str(game_dir), "--load", str(target), "10", "400"],
        capture_output=True,
        text=True,
        timeout=900,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    assert re.search(r"^loaded .*turn 30, time \d+, hashes verified$", result.stdout, re.MULTILINE), (
        result.stdout
    )
    assert "UNRESTORED" not in result.stdout and "UNCONSUMED" not in result.stdout, result.stdout
    assert "after 40 turns" in result.stdout, result.stdout
    assert "map       Maps/7" in result.stdout, result.stdout

    # A save of the seventh map refuses to load into the third: the identity
    # the manifest carries is the one the envelope was written under.
    from imperivm.formats.bfhp import BlockFile, build_plan

    parsed = BlockFile(target)
    wrong = build_plan(
        [
            ("save.ini", parsed.read("save.ini").replace(b"map=7", b"map=3")),
            ("session.isav", parsed.read("session.isav")),
        ],
        parsed.block_size,
    )
    (tmp_path / "wrong.bfhp").write_bytes(wrong)
    refused = subprocess.run(
        [str(imrun), str(game_dir), "--load", str(tmp_path / "wrong.bfhp"), "1"],
        capture_output=True,
        text=True,
        timeout=900,
    )
    assert refused.returncode != 0
    assert "load refused" in refused.stderr, refused.stdout + refused.stderr
