"""Two of the game screen's controls, driven through the app's own input.

**The Diplomacy screen** (`DIPLOMACY.INI`, 0x006cbbf0). OK is 0x1000 and
Cancel 0x1001 -- the app had them at 0x1002 and 0x1003, the legend's inert
pictures, so neither did anything. A toggle only flips; OK posts a
`diplomacy` order per player whose word changed, and one for the Allied
victory box, and the order changes the table on the next turn. The rows are
the description's *real* players (0x006cbca7), the list Statistics reads.

**A garrison portrait** (`CVXUIHolder`, 0x006d3550). A click selects the units
behind it, in place: they stay in the fort. Only when the fort's owner grants
the local player cease fire or shared control (0x006d35d8, `& 0x21`).

**A hero's army** (0x006d2ad0). A lone hero's `UIHolder` lists its army, the
hero left out, one portrait per class numbered with its count; a click on one
selects those units where they stand.

Every app run here is headless (`conftest.py` sets `IMPERIVM_HEADLESS=1`).
No step presses F5 or F9: those write and read the installation's
`Saves/quicksave.bfhp`.
"""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

import corpus
from conftest import requires_game

pytestmark = requires_game

CROSSROADS = "Scenarios/Crossroads.BFHP"
BALCANS = "Scenarios/Balcans.BFHP"
BOUDICCA = "Adventures/GreatChallenges/6_Great_loses_Boudicca.BFHP"
SIZE = ["--width", "1024", "--height", "768"]


@pytest.fixture(scope="module")
def app(game_dir) -> Path:
    candidates = [
        path
        for directory in corpus.tool_directories()
        if (path := directory / "engine" / "app" / "imperivm").is_file()
    ]
    if not candidates:
        pytest.skip("the app is not built")
    path = max(candidates, key=lambda p: p.stat().st_mtime)
    if not (game_dir / CROSSROADS).is_file():
        pytest.skip(f"{CROSSROADS} is not in this installation")
    probe = subprocess.run(
        [str(path), "--game", str(game_dir), "--map", CROSSROADS, "--frames", "2", *SIZE],
        capture_output=True, text=True, timeout=120,
    )
    if probe.returncode != 0:
        pytest.skip("the app cannot start here, even headless: " + probe.stderr[-200:])
    return path


def run(app: Path, game_dir: Path, *extra: str, env: dict[str, str] | None = None) -> str:
    assert not any("F5" in part or "F9" in part for part in extra), "never touch the quick save"
    done = subprocess.run(
        [str(app), "--game", str(game_dir), *SIZE, *extra],
        capture_output=True, text=True, timeout=300, env={**os.environ, **(env or {})},
    )
    assert done.returncode == 0, done.stdout + done.stderr
    return done.stdout


# -- the Diplomacy screen ------------------------------------------------------


def diplomacy(app: Path, game_dir: Path, steps: str) -> str:
    return run(app, game_dir, "--map", CROSSROADS, "--play", "--frames", "60", "--menu", "diplomacy",
               "--input", "wait:10;" + steps)


def test_diplomacy_lists_the_real_players_not_every_participant(app, game_dir):
    # Crossroads declares fourteen seats; the players' screen makes the first
    # four real, and seats 4-13 are the map's Computer seats.
    out = diplomacy(app, game_dir, "wait:2")
    assert re.search(r"^diplomacy:\s+a row for player 1 2 3$", out, re.M), out


def test_a_toggle_and_ok_change_the_table_on_the_next_turn(app, game_dir):
    out = diplomacy(app, game_dir, "press:Pl1.CF1;wait:2;press:Pl1.SV1;wait:2;press:Allied;wait:2;press:OK;wait:10")
    # Cease fire (bit 0) and shared vision (bit 4) toward player 1, no more:
    # the word is the whole of what the four toggles say.
    assert re.search(r"^diplomacy:\s+player 0's word for player 1 is 0x11 on turn 1$", out, re.M), out
    assert re.search(r"^diplomacy:\s+player 0's allied victory on on turn 1$", out, re.M), out
    # Nothing for the rows that were not touched.
    assert not re.search(r"word for player [23]", out), out


def test_cancel_drops_what_the_toggles_said(app, game_dir):
    out = diplomacy(app, game_dir, "press:Pl1.CF1;wait:2;press:Allied;wait:2;press:Cancel;wait:10")
    assert "word for player" not in out and "allied victory" not in out, out


def test_a_toggle_pressed_twice_posts_nothing(app, game_dir):
    out = diplomacy(app, game_dir, "press:Pl2.SC1;wait:2;press:Pl2.SC1;wait:2;press:OK;wait:10")
    assert "word for player" not in out, out


def test_the_other_sides_policy_is_inert(app, game_dir):
    out = diplomacy(app, game_dir, "press:Pl1.CF2;wait:2;press:OK;wait:10")
    assert "word for player" not in out, out


# -- a garrison portrait -------------------------------------------------------


@pytest.fixture(scope="module")
def garrisoned(app, game_dir, tmp_path_factory) -> tuple[Path, int]:
    """A Balcans save at turn 120, when the computer has put units into a town
    hall, and the owner of one that holds some. No shipped map starts with a
    garrison, so the save is made here, by `imsave` from the retail map."""
    imsave, complaint = corpus.find_tool("imsave", "IMPERIVM_IMSAVE")
    if imsave is None or complaint:
        pytest.skip(complaint)
    if not (game_dir / BALCANS).is_file():
        pytest.skip(f"{BALCANS} is not in this installation")
    save = tmp_path_factory.mktemp("garrison") / "balcans120.bfhp"
    made = subprocess.run(
        [str(imsave), str(game_dir), str(game_dir / BALCANS), "--turns", "120", "--more", "0",
         "--out", str(save)],
        capture_output=True, text=True, timeout=300,
    )
    assert made.returncode == 0 and save.is_file(), made.stdout + made.stderr
    steps = ["key:P", "wait:3"]
    for owner in range(8):
        steps += [f"select:class:BaseTownhall@{owner}", "wait:3"]
    out = run(app, game_dir, "--load", str(save), "--frames", "40", "--input", ";".join(steps),
              env={"IMPERIVM_INFOBAR": "1"})
    owner = None
    current = None
    for line in out.splitlines():
        if m := re.match(r"selected:\s+\d+ object\(s\) from select:class:BaseTownhall@(\d+)", line):
            current = int(m.group(1))
        elif current is not None and re.search(r"tags thumb building holder .* holder [1-9]", line):
            owner = current
            break
    if owner is None:
        pytest.skip("no town hall holds a garrison at turn 120 of this build's Balcans")
    return save, owner


def test_a_garrison_portrait_selects_its_unit_in_place(app, game_dir, garrisoned):
    save, owner = garrisoned
    out = run(app, game_dir, "--load", str(save), "--player", str(owner), "--frames", "30", "--input",
              "key:P;wait:3;select:class:BaseTownhall;wait:3;press:Holder@0;wait:3",
              env={"IMPERIVM_INFOBAR": "1"})
    # The portrait is a class's: a plain click selects every unit behind it.
    assert re.search(r"^holder: (\d+) of \1 selected in place:( \d+)+$", out, re.M), out
    # The bar now describes a unit, not the town hall it is still inside.
    after = out[out.index("holder:"):]
    assert re.search(r"^infobar: class \S+ .*tags thumb unit", after, re.M), after


def test_an_enemy_garrison_portrait_does_nothing(app, game_dir, garrisoned):
    save, owner = garrisoned
    enemy = (owner + 1) % 8
    out = run(app, game_dir, "--load", str(save), "--player", str(enemy), "--frames", "30", "--input",
              f"key:P;wait:3;select:class:BaseTownhall@{owner};wait:3;press:Holder@0;wait:3",
              env={"IMPERIVM_INFOBAR": "1"})
    assert "input: no open dialog or bar has Holder@0" not in out, out
    assert "holder:" not in out, out


# -- a hero's army -------------------------------------------------------------


def holder_cells(out: str) -> list[tuple[str, list[int]]]:
    """The last refresh's `UIHolder` cells: each one's number and the ids a
    click on it selects."""
    cells: list[tuple[str, list[int]]] = []
    for line in out.splitlines():
        if line.startswith("infobar: class "):
            cells = []
        elif m := re.match(r'infobar: holder \d+ icon .* number "(\d*)" selects((?: \d+)*)$', line):
            cells.append((m.group(1), [int(x) for x in m.group(2).split()]))
    return cells


def test_a_heros_army_is_listed_and_a_portrait_selects_its_units(app, game_dir):
    # Boudicca's own hero starts the adventure with a small army attached.
    if not (game_dir / BOUDICCA).is_file():
        pytest.skip(f"{BOUDICCA} is not in this installation")
    out = run(app, game_dir, "--map", BOUDICCA, "--play", "--frames", "30", "--input",
              "wait:10;select:class:Hero;wait:3", env={"IMPERIVM_INFOBAR": "1"})
    assert re.search(r"^selected:\s+1 object\(s\) from select:class:Hero$", out, re.M), out
    armies = re.findall(r"^infobar: hero (\d+) army((?: \d+)*)$", out, re.M)
    assert armies, out
    hero, members = armies[-1]
    army = [int(x) for x in members.split()]
    assert len(army) >= 2, out
    cells = holder_cells(out)
    assert len(cells) >= 2, cells
    # Every member once and the hero not among them; each cell numbered with
    # how many it holds; the cells in the order the army first meets them.
    listed = [unit for _, units in cells for unit in units]
    assert sorted(listed) == sorted(army), (cells, army)
    assert int(hero) not in listed
    for number, units in cells:
        assert number == str(len(units)), cells
    firsts = [units[0] for _, units in cells]
    assert firsts == sorted(firsts, key=army.index), (cells, army)

    # A click on the second portrait selects its units where they stand.
    out = run(app, game_dir, "--map", BOUDICCA, "--play", "--frames", "30", "--input",
              "wait:10;select:class:Hero;wait:3;press:Holder@1;wait:3", env={"IMPERIVM_INFOBAR": "1"})
    wanted = holder_cells(out[:out.index("holder:")])[1][1]
    m = re.search(r"^holder: (\d+) of (\d+) selected in place:((?: \d+)+)$", out, re.M)
    assert m, out
    assert [int(x) for x in m.group(3).split()] == wanted, (m.group(0), wanted)
    assert int(m.group(1)) == int(m.group(2)) == len(wanted)
    # The bar now describes those units, not the hero, and lists nothing.
    after = out[m.end():]
    assert re.search(r"^infobar: class (?!\S*Hero)\S+ .* holder 0 ", after, re.M), after
