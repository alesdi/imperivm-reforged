# Imperivm Reforged

<img width="1014" height="579" alt="d9dfa74ac4bf82bf7eb06a58eaec75c1accbcff5eaa740256793354b2230bb66" src="https://github.com/user-attachments/assets/75a0d4e5-dc9e-4d08-b23d-2558a12448c8" />

An open source reimplementation of the game engine behind **Imperivm: Great Battles of Rome**
(Haemimont Games' `HMMSYS` engine), in the spirit of
[Julius](https://github.com/bvschaik/julius) for Caesar III.

The goal is a portable, maintainable executable that runs on an **unmodified original
installation** of the game, on platforms the original never reached, without the DirectDraw
and 16-bit-colour dependencies that make it fragile on modern systems.

## This repository ships no game assets

Imperivm remains the property of Haemimont Games and FX Interactive. This project contains
only independently written code and file-format documentation. It requires you to own and
install the original game; it reads that installation in place.

No original code, no decompiler output, and no game data will ever be committed here.
See [`docs/legal.md`](docs/legal.md).

## Status

**Playable, and not yet faithful everywhere.** The engine opens the original's maps,
campaigns and saved games, runs the game's own scripts and AI, draws the world through the
game's own interface, and plays sound and music. A skirmish goes to war by itself, and the
engine is checked to be deterministic so that multiplayer peers stay in step. Every file
format the game ships is documented, and a command line toolchain reads, exports and
rebuilds the game's data.

Much remains to match the original: parts of the AI's strategy, combat details, the rest
of the sound, and a steady stream of things that only show up when the game is played.
Nothing here should be mistaken for a finished game yet.

The plan, with what is done, what is open and the evidence behind each, is
[`docs/plan.html`](docs/plan.html). It is the place to look before starting on anything.

## Build and run

You need your own installation of the original game, and CMake 3.24+, a C++20 compiler and
SDL3.

```
cmake -S . -B build -G Ninja
cmake --build build
./build/engine/app/imperivm --game /path/to/Imperivm
```

That opens the game's own menus. To go straight onto a map:

```
./build/engine/app/imperivm --game /path/to/Imperivm --map Scenarios/Crossroads.BFHP --play --player 0
```

Left-drag selects, right-click orders, middle-drag pans, Space opens the map and F10 the
game menu. `imperivm --help` lists the other options.

The Python asset tools and tests need Python 3.11+: `pip install .` for the tools,
`pip install .[dev]` to add the test runner.

## Layout

```
engine/core/       the simulation: formats, class graph, script VM, game systems.
                   Freestanding: no SDL, no I/O, no floating point.
engine/platform/   window, GPU, input, audio, filesystem
engine/sound/      sound decoding and mixing
engine/app/        the executable
engine/tools/      headless tools: running, saving, conformance checks
engine/tests/      C++ tests, no game data required
docs/formats/      file format specifications
docs/engine/       architecture and the engine's design notes
docs/tools/        command line tool documentation
docs/plan.html     the plan and its state
src/imperivm/      Python reference readers and asset tools
tests/             Python tests; the corpus half needs an installation
tools/             verification and reverse-engineering helpers
```

## Contributing

Contributions are welcome, and you do not need to write C++ to make one.

**Ways to help**

- **Play it and report what is wrong.** Most of what remains is found by playing: something
  that behaves, looks or sounds different from the original. Open an issue with what you
  saw, what the original does, the map, and the steps to get there. A saved game helps
  most.
- **Know the original well?** Telling us how something really behaves is as valuable as
  code, especially for the AI, combat and campaigns.
- **Documentation.** The format specifications in `docs/formats/` are the project's most
  durable output. Filling in an "unknown" there is a contribution.
- **Code.** Pick something open in [`docs/plan.html`](docs/plan.html) or the issues, and
  say so in the issue before starting on anything large, so work is not duplicated.

**Ground rules**

These keep the project lawful and the engine trustworthy. They are not negotiable.

- **No game assets, and no original code.** Never commit anything extracted from the game:
  graphics, sounds, maps, text. Never paste decompiler or disassembler output, not even as
  a comment. Reading the original to understand it is fine; describe what it does in your
  own words, then implement from that description. [`docs/legal.md`](docs/legal.md) has the
  details.
- **Match the original, and say when you are guessing.** The aim is the original's
  behaviour, not an improvement on it. Where the evidence does not settle a rule, implement
  one reading, label it as such in the code, and record the alternatives.
- **Keep the simulation deterministic.** No floating point, one seeded random generator,
  and no iteration over unordered containers in `engine/core`. Multiplayer depends on two
  machines computing exactly the same game.
- **Keep the core freestanding.** `engine/core` touches no SDL, files, clock or threads;
  `tools/check_core_boundary.py` checks it.
- **Test what you change.** A fix comes with a test that fails without it. Tests open no
  windows and never write into the game's installation.

**Before you open a pull request**

```
python3 tools/verify.py          # the quick checks, after most changes
python3 tools/verify.py --full   # everything, after changes to saves, the app or the tools
```

Keep each pull request to one change, explain in the description what you changed and
why, and how you checked it against the original. Write commit messages as a plain
sentence saying what is now true. [`CONTRIBUTING.md`](CONTRIBUTING.md) has more on
documenting formats and the standard of evidence the project holds itself to.

## Licence

GPL-3.0-or-later. See [`LICENSE`](LICENSE). Third-party code under `engine/third_party/`
keeps its own licence; see [`docs/legal.md`](docs/legal.md).
