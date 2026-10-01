# HMMSYS configuration files (`.ini`)

**Status:** dialect measured across the whole retail corpus; reader reproduces its counts exactly
**Reference reader:** [`src/imperivm/formats/ini.py`](../../src/imperivm/formats/ini.py), ported in [`engine/core/src/formats/ini.cpp`](../../engine/core/src/formats/ini.cpp)
**Used by:** [ai-ini.md](ai-ini.md) (AI profiles), the interface layer, `CONST.INI`

150 `.ini` files ship in the retail packs. They carry the entire user interface, the AI
profiles, and the tuning constants. This document describes the dialect those files
actually use — which is narrower than "INI" in general, and narrow in ways that matter.

## The dialect

Measured over all 150 files, by two independent implementations that agree:

| Construct | Count |
|---|---:|
| `[Section]` headers | 3,662 |
| `key = value` lines | 18,094 |
| **bare lines (no `=`)** | 3,278 |
| whole-line `;` comments | 672 |
| lines with an inline `;` comment | 408 |
| section headers with a trailing comment | 12 |
| values containing a comma | 8,192 |
| **values containing a `;`** | **0** |
| quoted values | 3 |

Note that:

- **`;` starts a comment unconditionally.** This is the one assumption the lexer rests on,
  and it holds because **no value in any shipped file contains a `;`**. There is no
  quoting rule to respect either: only three values are quoted at all
  (`ImageType = "AAAAA"`, in three of the editor's dialogs), the quotes are part of the
  value rather than delimiters, and no quoted value shares a line with a `;`.
- A `;` comment may follow a **section header** as well as a value. Twelve headers do, all
  in the editor's `TEMPLATE.INI` and `ADVOBJPROPS.INI`.
- **`#` and `//` never begin a comment.** A reader that treated them as one would change
  nothing about the shipped files today and something unpredictable about a modded one
  tomorrow.
- Whitespace around keys, values and header names is insignificant.

## A section can be an ordered list

3,278 lines carry no `=` at all, and they are not malformed. Two whole families of section
use this shape:

- `AI.INI`'s `[SquadStates]`, `[GAIKAStrat]`, `[EconomyScripts]` and `[TacticScripts]`,
  where **a line's position is a constant's value** — see [ai-ini.md](ai-ini.md).
- the interface files' `[<Screen> Objects]` sections, which name a screen's widgets.

So an entry is a key and a value of which either may be empty, and a reader has to record
whether an `=` was present: a bare `SS_Approach` and a hypothetical `SS_Approach =` mean
different things and only the file can say which it is. A bare line must never answer a
lookup by key, or an enum member would shadow a variable of the same name.

## Duplicate keys, and why a section is not simply a map

**22 keys are declared twice within their own section**, spread over nine files, and nine
of those carry a different value each time:

| File | Section | Key | Values |
|---|---|---|---|
| `AI.INI` | `[Vars.All]` | `AIV_SquanderGoldAmount` | `30000`, `15000` |
| `AI.INI` | `[Vars.All]` | `AIV_SquanderFoolAmount` | `30000`, `15000` |
| `ADVDIPLOMACY.INI` | `[Back]` | `RectWH` | `0, 0, 640, 500`, `6, 6, 630, 490` |
| `ADVDIPLOMACY.INI` | `[Back]` | `HAlign`, `VAlign` | `0, 0, 0`, `1, 0, 0` |
| `UNITICONS.INI` | `[FillCombo]` | `Republican Roman Hero 1` | three different bitmaps |
| `PLACEBUILDING.INI` | `[PlayerText]` | `Text` | `Strength`, `Player 1` |
| `MENU/TEMPLATE.INI` | `[Caption]` | `TextColor` | `255, 255, 128`, `%TextColor%` |

`UNITICONS.INI`'s `[FillCombo]` is the one that settles the design: three consecutive
entries share the label `Republican Roman Hero 1` and name `RHero1.bmp`, `RHero2.bmp` and
`RHero3.bmp`. Reading that section as a map loses two thirds of it.

So **a reader must preserve every entry in file order** and treat by-key lookup as a
convenience for the sections that really are maps. A consumer of a list-shaped section
iterates; it does not look up.

Where a lookup does have to break a tie, the **first** match wins, which is what Win32's
`GetPrivateProfileString` does — and Win32 is what a game of this vintage reads `.ini`
files with. **Inferred from the platform, not proven from the data.**

## Placeholders are a layer above this one

**4,329 values in 132 of the 150 files contain `%Name%` placeholders**, 179 distinct names,
led by `%TmplIni%` (2,367 uses), `%RaceArt%` (265) and `%Art%` (157).

This reader does **not** expand them: substitution needs a scope that the file alone does
not define, and defining that scope is part of the interface layer rather than part of
reading the container. Values come back with their placeholders intact. Anything that
consumes an interface `.ini` has to expand them, and this specification does not yet say
how.

## Encoding

Windows-1252. The C++ reader is byte-oriented and does not transcode; the Python one
decodes on read, because a Python caller has nothing to gain from bytes.

## Errors

The only rejection is an unterminated section header — a `[` with no `]` on the same line.
Guessing where the name ends would make that failure silent. Every one of the 150 shipped
files parses.

## What is still unknown

- **How `%Name%` placeholders resolve.** Scope, defaults, nesting and whether a value may
  be *entirely* a placeholder that expands to several lines are all open. `%TmplIni%`'s
  2,367 uses suggest a file-level template include rather than a plain string
  substitution. This is Part 6 work.
- **The tie-breaking rule for a duplicated key.** First-match is inferred from
  `GetPrivateProfileString`. Nine keys in the corpus are affected. A decompilation would
  settle it in one look.
- **Whether the three quoted values are quoted meaningfully.** All three are the same
  literal `"AAAAA"` on the same key, `ImageType`, in three editor dialogs, which reads more
  like placeholder text than like a quoting convention. The reader keeps the quotes.
- **Whether a section may legally repeat.** No shipped file repeats one. The reader
  resolves a lookup against the first section of a given name and does not merge.
- **What an entry before the first section header means.** No shipped file has one. The
  reader keeps such entries in an implicit unnamed section rather than dropping them.
