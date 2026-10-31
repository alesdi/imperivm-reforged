# Working in this repository

Read `CONTRIBUTING.md` and `docs/legal.md` first; `docs/plan.html` is the work list.

## The yardstick is gbr.exe, not this engine

Until every host entry point reachable from a shipped script is replicated,
the work is a port, and it is judged against the binary alone.

- **Reproduce what the binary does**, read end to end, described in prose in
  `docs/formats/` and implemented from it. A change is right when it matches
  the original, whatever it does to the game as it plays here today.
- **Never compare against this engine's current game to accept or refuse a
  change.** Its play is the product of partial ports and is not a reference:
  a more exact port can raise piles, slow the AI or end fewer matches. Seed
  medians (`tools/seeds.py`) are not a merge gate. They may be run once at the
  end of a round and written down as an observation, nothing more.
- **No engine choice to keep a number level.** A labelled departure is only
  for what the engine cannot yet do (a system not ported, a renderer-only
  effect), never to soften the effect of an exact port. When an exact port
  makes play worse, the cause is the next thing to port: find what the
  original has there that this engine lacks, and add it to the plan.
- **A merge is gated on what can be wrong regardless of the game:** the
  entry point's own tests, script traps, determinism and the save round
  trip (`verify.py`, `--full`), `--slow` once per round on the merged tree,
  and one timed Balcans run for blowups in time or memory.
- **Tests pin behaviour, not the AI's luck.** A corpus test stages what it
  needs (`IMRUN_PLACE`, `IMRUN_GOTO`) instead of a seed on which the AI
  happens to do it, so a more exact port does not move it.

Whole-game comparison comes when the hosts are done, and against the
original game, not this one.

## Testing: run the least that can see the change

- **While working:** build only the target you changed
  (`cmake --build build-core --target imrun`) and run the one test that covers
  it, by node id: `.venv/bin/python -m pytest tests/test_corpus_imrun.py::test_...`.
  `core_tests` runs whole in under two seconds.
- **When the change is done:** `python3 tools/verify.py --serial` (under a minute).
- **Once, before handing back:** `python3 tools/verify.py --full`.
  Not after every edit.
- **Never per change:** `python3 tools/verify.py --slow` plays the wars (tens of
  minutes) and is run once, by whoever merges a round, on the merged tree.

Tests marked `slow` are skipped unless `--slow` is given or the test is named by
node id. Don't write a new test that waits for the AI to start a war: hold the
claim in a few hundred turns, or order the fight (`IMRUN_PLACE`, `IMRUN_GOTO`).
`IMRUN_UNTIL_OVER` and `IMRUN_UNTIL_CAPTURE` stop a run at the answer instead of
a guessed turn.

Anything over a minute (a long `imrun`, `tools/seeds.py`, a sweep) runs in the
background; don't poll it from a foreground `sleep` loop.

This is a 16 GB laptop that has run out of memory under parallel test runs.
One heavy job at a time (a build, an `imrun`, a verify), never two; build
with `-j4` at most; `verify.py --serial` for the quick pass. Keep the machine
awake through a long run with `caffeinate -is`, or a sleep breaks the timing
tests.

Give every long `imrun` a wall-clock limit as well: a run whose memory stays
flat while its time grows is a livelock, not a slow game.

## Worktrees

When a batch's branch is merged into main, remove its worktree and delete its
branch at once (`git worktree remove`, `git branch -d`); don't leave finished
worktrees on disk.
