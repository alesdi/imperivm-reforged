# Contributing

Read [`docs/legal.md`](docs/legal.md) first. Its rules are not negotiable, and a violation
puts the whole project at risk.

The README's *Contributing* section covers the ways to help and the ground rules. This file
is the detail behind them.

## Where to start

[`docs/plan.html`](docs/plan.html) records what is done, what is open and the evidence
behind each. Its open threads and its playtest reports are the work list. Before starting
on anything large, say so in an issue, so that two people do not chase the same thing.

The best bug reports come from playing. Say what you saw, what the original does instead,
on which map, and how to get there. Attach a saved game if you have one.

## Changing the engine

**Match the original, not your idea of it.** The goal is the original's behaviour, read
from its data, its scripts and, where those do not settle it, its executable. When the
evidence does not determine a rule, implement one reading, label it as an inference in the
code, and record the alternatives. A labelled guess can be corrected later; an unlabelled
one looks like a fact.

**Keep the simulation deterministic.** Multiplayer depends on every machine computing the
same game from the same orders.

- No floating point in `engine/core`. Fixed-point only.
- One random generator, explicitly seeded and saved with the world.
- Iteration order is state: never iterate an unordered container in the simulation.
- Presentation (sound, animation timing, what is drawn) never feeds back into what is
  hashed.

**Keep the core freestanding.** `engine/core` may not touch SDL, the filesystem, the clock
or threads. `python3 tools/check_core_boundary.py` checks it.

**Test what you change.** A fix comes with a test that fails without it.

- Tests open no windows: the application runs headless under the test suite.
- Tests never write into the game's installation.
- A test asserts what it is about, not when something happens to occur. If it needs a
  fight, it orders one; a scripted run waits on a game turn, not on a number of frames.
- A test about the shipped data reads the real installation and skips without one
  (`tests/test_corpus_*.py`). A unit test builds its own data from the specifications,
  never from a copy of the game's files.

It helps to check that your tests would notice a mistake: break your change on purpose in
a few small ways and confirm a test fails each time.

## Checks before a pull request

CI runs on every pull request, but it cannot see the game: it builds the core and runs the C++ tests, the boundary check and the Python tests, whose corpus half skips without an installation. Everything that reads the game is yours to run, so run these before you push.

```sh
python3 tools/verify.py          # the quick checks, after most changes
python3 tools/verify.py --full   # everything, after changes to saves, the app or the tools
```

The quick tier builds and runs the C++ tests, checks the core boundary and the fixtures,
and runs every shipped map for a short while looking for script traps. `--full` adds the
save round-trip of every map and the Python suite. Both need `build-core` configured as
in the README; the corpus checks need `IMPERIVM_GAME_DIR` pointing at an installation and
skip without one.

## Documenting a format

Every format gets two things.

**A specification** at `docs/formats/<name>.md`, following the shape of
[`docs/formats/pak.md`](docs/formats/pak.md):

- byte-level layout tables, with offsets, sizes and types
- the meaning of every field you have pinned down
- at least one worked example, described rather than copied from the retail data
- the structural invariants you validated, and across how many files
- an explicit **"what is still unknown"** section

That last section is mandatory. A specification that quietly omits the parts nobody
understood is worse than one that names them, because the next person cannot tell the
difference between "solved" and "not looked at".

**A reference reader** in `src/imperivm/formats/<name>.py`:

- Python 3.11+, standard library only
- correctness and legibility over speed: these are executable documentation
- dataclasses for records, type annotations throughout
- a module docstring pointing at the specification
- a `validate()` method asserting the invariants the specification claims

## Standard of evidence

Reverse engineering rewards scepticism. A hypothesis that explains one file is a
coincidence; one that explains every file is a format.

- **Validate across the whole corpus.** If there are 546 files in a format, your reader
  parses 546 files. Report the number.
- **Use size arithmetic.** If a layout is right, the field arithmetic lands exactly on the
  file size, every time. If it lands "close", the layout is wrong.
- **Look at the output.** For anything that draws, render it and look. Correct sprites look
  like soldiers; incorrect ones look like noise, and no amount of plausible reasoning
  substitutes for the check.
- **Prefer ground truth already in the data.** The XML declares sprite rows and columns;
  filenames encode sizes. Validate guesses against those rather than against your own
  expectations.
- **Never fabricate.** An unresolved field marked `unknown` is a contribution. A
  confidently wrong guess costs the next person days.

## Reading the original executable

`gbr.exe` may be read to understand how the game behaves; `tools/re/` has helpers for it.
It may **not** be copied from. Do not paste decompiler or disassembler output into this
repository in any form, including as comments. Understand the behaviour, describe it in
your own words (the project cites addresses so a finding can be checked), then implement
independently from that description.

## Commits and pull requests

- One change per pull request, with a description of what changed, why, and how you
  checked it against the original.
- Write each commit message as a plain sentence saying what is now true ("A gate stands
  closed when nobody is near"), with the reasoning in the body. Wrap the body at 72
  characters.
- A commit that adds or corrects a specification states what was validated and over how
  many files, so the claim can be audited later.
