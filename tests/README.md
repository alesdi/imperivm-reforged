# Tests

The suite is deliberately split in two, and the split follows from
[`docs/legal.md`](../docs/legal.md): **this repository ships no game assets, ever.**

That creates a tension. Every format reader here is only meaningful against real game data,
but the data cannot live in the repository and cannot exist on a CI runner. So:

| Half | Marker | Needs an installation | Runs in CI |
|------|--------|-----------------------|------------|
| Synthetic | *(none)* | no | yes |
| Corpus | `corpus` | yes | no — skips |

**Synthetic tests** build their fixtures in code: a small pack assembled in memory and parsed
back, a PNG round-tripped, malformed input fed to a reader to check it fails cleanly. They
are the only safety net CI has, so they are written to be genuinely load-bearing rather than
decorative.

**Corpus tests** assert the invariants the specifications claim, across the whole retail
install — that all 3,898 sprite frame tables parse and their 197,432 frames tile `rle.mmp`
exactly, that all 577 scripts parse, that `impk` and `imfs` reproduce shipped archives byte
for byte. These are the strongest evidence the project has that its readers are correct.

## Running them

Without an installation, the corpus half skips and you still get useful coverage:

```
python -m pytest
```

With an installation, point the suite at it and everything runs:

```
IMPERIVM_GAME_DIR=/path/to/Imperivm python -m pytest
```

The directory is the one containing `Packs/`, `rle.mmp` and `gbr.exe`. Discovery falls back
to a few plausible locations, but the environment variable always wins — and if it is set to
something that is not an installation, the tests skip with a message saying so rather than
silently searching elsewhere.

Select one half explicitly:

```
python -m pytest -m "not corpus"    # what CI runs
python -m pytest -m corpus          # requires an installation
```

Expected results, measured on the retail install:

```
506 passed, 7 skipped (slow)      with game data, -n auto --dist loadgroup   (~1.5-2 min)
218 passed, 295 skipped            without                                     (~2 s)
```

`tools/verify.py --full` runs the first, deselecting the one `swept` test.

## Slow tests

A test that plays a war -- Crossroads for thousands of turns, in `imrun`, in
the app, between networked peers -- is marked `slow` and skipped unless asked
for:

```
python -m pytest --slow                              # everything
python -m pytest --slow -m slow                      # only the wars
python -m pytest tests/test_corpus_imrun.py::test_a_skirmish_on_crossroads_goes_to_war_and_its_walled_town_holds
```

A slow test named by node id runs without `--slow`. `tools/verify.py --slow`
runs them all, once per merged round of work. They were five tests and 44% of
the suite's time, and what they wait for is the AI's to decide, so they moved
whenever it changed; before marking a new test `slow`, see whether its claim
holds in a few hundred turns, or in a fight it orders.

## The failure mode this design guards against

A corpus test that *fails* without game data would turn CI red for every contributor who
does not own the game. A corpus test that silently *passes* without touching data is worse:
the suite reports green while testing nothing.

Both have happened here. An earlier revision of this suite reported the same 191 passes with
and without an installation, because the skip markers existed but no test used them.

So CI does not merely run `-m "not corpus"` and trust it. It also runs `-m corpus` against a
deliberately absent installation and asserts that the tests were **collected and skipped** —
not passed, not failed, not silently uncollected. If a future change breaks the skip
machinery in any of those directions, CI catches it.

## Adding tests

Corpus tests carry `pytestmark = requires_game`, or `@requires_pixel_store` when they read
`rle.mmp`. Both come from [`conftest.py`](conftest.py), which also provides the shared
fixtures — `game_dir`, `packs`, `pixel_store` — and does the discovery.

Prefer module-scoped fixtures for expensive work. Parsing every frame table in the game takes
a few seconds; doing it once per module rather than once per test is what keeps the corpus
run under twenty seconds.

When you assert a corpus-wide number, write it as a named constant with the count in it
(`FRAME_COUNT = 197_432`), so that a future failure reads as a change in the data rather than
as an unexplained mismatch.
