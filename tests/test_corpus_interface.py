"""Every interface screen the installation ships, through the C++ interpreter.

`imcheck interface <Packs> [local pack]` loads every `.INI` under
`DATA/INTERFACE/` that declares a `[<name> Objects]` section -- the 24 in-game
files, the 48 menus, the 52 editor dialogs and the common ones -- and reports
what it did not understand and which bitmaps it names that no pack holds. The
numbers below are the shipped data's own defects, listed so that a change in
the interpreter that hides one (or finds a new one) is heard about.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

import corpus

#: What the sweep found on the retail install, by name.
SCREENS = 136
WIDGETS = 2954
#: Six widgets in the Online Battle files inherit from a `MenuTitle` template
#: that no file defines, and so have no `Type`.
WIDGETS_WITHOUT_TYPE = 6
#: `TRANSPRENT`, four times, in the editor.
MISSPELT_STYLES = 4
#: Bitmaps named by a screen that no pack holds, by resolved path.
MISSING_ART = {
    "CurrentLang/GameSpyBackground.bmp",
    "CurrentLang/MenuBackgroundImperivmOpenBeta.bmp",
    "UI/menu/Celtic Kings.bmp",
    "asdf",
    "ui/common/ComboArrow.bmp",
    "ui/infobar/common/back.rle",
}


@pytest.fixture(scope="module")
def imcheck() -> Path:
    path, complaint = corpus.find_tool("imcheck", "IMPERIVM_IMCHECK")
    if path is None or complaint:
        pytest.skip(complaint)
    return path


@pytest.fixture(scope="module")
def sweep(imcheck: Path, game_dir: Path):
    packs = game_dir / "Packs"
    command = [str(imcheck), "interface", str(packs)]
    local = sorted((game_dir / "local").glob("*.pak"))
    if local:
        command.append(str(local[0]))
    result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    summary = next(line for line in lines if line.startswith("# "))
    # `# files 137 screens 136 ... images 2270 (missing 35) fonts 1626 (missing 0)`
    totals: dict[str, int] = {}
    previous = ""
    for key, value in re.findall(r"([a-z-]+) (\d+)", summary):
        name = f"{previous}-{key}" if key == "missing" else key
        totals[name] = int(value)
        previous = key if key != "missing" else previous
    return lines, totals


def test_every_screen_loads(sweep):
    lines, totals = sweep
    assert totals["screens"] == SCREENS
    assert totals["widgets"] == WIDGETS
    assert not [line for line in lines if "would not load" in line]


def test_every_widget_class_and_style_is_known(sweep):
    lines, totals = sweep
    unknown_types = {
        line.split()[1] if len(line.split()) > 2 else ""
        for line in lines
        if line.startswith("unknown-type")
    }
    assert unknown_types == {""}, unknown_types
    assert totals["unknown-types"] == WIDGETS_WITHOUT_TYPE
    unknown_styles = {line.split()[1] for line in lines if line.startswith("unknown-style")}
    assert unknown_styles == {"TRANSPRENT"}
    assert totals["unknown-styles"] == MISSPELT_STYLES


def test_no_screen_has_a_bad_rectangle(sweep):
    lines, _ = sweep
    bad = [line for line in lines if ": bad " in line]
    assert not bad, "\n".join(bad)


def test_only_the_known_art_is_missing(sweep):
    lines, totals = sweep
    missing = {line[len("missing "):] for line in lines if line.startswith("missing ")}
    assert missing == MISSING_ART
    assert totals["fonts"] > 0
    assert totals["fonts-missing"] == 0
