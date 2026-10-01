# Legal posture

Imperivm: Great Battles of Rome is the property of Haemimont Games and FX Interactive.
This project claims no rights over it.

## Rules for contributors

These are not negotiable. A violation puts the whole project at risk.

1. **No game assets in the repository.** No sprites, sounds, music, maps, campaigns, fonts,
   or text extracted from the game. Not as test fixtures, not as documentation examples,
   not temporarily. `.gitignore` blocks the common extensions; do not work around it.

   `.gitignore` stops a `.bmp` and a `.pak`. It does nothing about the case that actually
   happened here, which is retail *text* pasted into a C++ string literal —
   fifteen thousand characters of it, across six test files and one specification, every
   one introduced by somebody arguing that a copy tests more than a paraphrase.
   **`python3 tools/check_fixtures.py` is the check for that**: it reads every tracked
   text file and asks whether its string literals and byte arrays occur in the player's
   own installation. It runs in `tools/verify.py` and skips where there is no installation
   to compare against, which is also where nobody is in a position to paste from one.

   The argument for a copy is real and has to be answered rather than dismissed: a
   hand-authored fixture that is tidier than the data has accused a correct reader on this
   project four times. The answer is that there are two claims and they belong in two
   places. *"The reader handles a trailing comma"* is a unit test, and its fixture is
   written from `docs/formats/` — `tests/synthetic.py` says why that is better than a copy
   even setting this rule aside. *"The shipped conquest's territory graph closes"* is a
   claim about the installation, so it belongs in `tests/test_corpus_*.py`, where it reads
   the real file and skips without one. A claim about retail data asserted against a copy
   of retail data was never testing the data anyway; it was testing the copy.

2. **No original code.** Do not paste decompiler or disassembler output into this
   repository, in any language, including as comments. Describe behaviour in prose in
   `docs/formats/`, then implement it independently from that description.

3. **No bundled installer or game download.** The user supplies their own installation.

4. **Document formats, not implementations.** A format specification describes the bytes on
   disk. It does not reproduce the original's source structure, function names, or internal
   algorithms beyond what is necessary to read the data.

## Third-party code

Everything in this repository is the project's own, under its GPL-3.0-or-later,
except what `engine/third_party/` holds. Each directory there carries the
upstream's licence file and a README naming the upstream, the exact version or
commit, and any local change. Nothing in `engine/core` links any of it.

| directory | what | upstream | licence |
|---|---|---|---|
| `engine/third_party/stb_vorbis/` | Ogg Vorbis decoder, for `music/*.ogg` | `stb_vorbis.c` v1.22, github.com/nothings/stb | public domain (Unlicense) or MIT, at the user's choice |

`stb_vorbis` is the first vendored dependency (the owner's decision, recorded in
`docs/engine/sound.md`). Before it the only third-party code was SDL3, found by
CMake rather than shipped, and `capstone`, which stays inside `tools/re/`. A new
entry needs a licence compatible with GPL-3.0-or-later, the licence file beside
it, and a row here.

## Why this is permissible

Reverse engineering a file format in order to build an independently written, interoperable
implementation is well-established practice, and is what Julius, OpenTTD, OpenRA,
devilutionX, OpenMW and many others do. The protection lies in the separation: we
distribute our own code, the rights holder's data stays with the person who bought it.

## Trademark

"Imperivm" and "Great Battles of Rome" are used here only to identify the game this engine
is compatible with. This project is not affiliated with or endorsed by Haemimont Games or
FX Interactive.
