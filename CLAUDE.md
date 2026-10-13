# Working in this repository

Read `CONTRIBUTING.md` and `docs/legal.md` first; `docs/plan.html` is the work list.

## Testing: run the least that can see the change

- **While working:** build only the target you changed
  (`cmake --build build-core --target imrun`) and run the one test that covers
  it, by node id: `.venv/bin/python -m pytest tests/test_corpus_imrun.py::test_...`.
  `core_tests` runs whole in under two seconds.
- **When the change is done:** `python3 tools/verify.py` (about 20 s).
- **Once, before handing back:** `python3 tools/verify.py --full` (about 2 min).
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
