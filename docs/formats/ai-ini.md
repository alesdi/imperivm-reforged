# AI profiles (`DATA/AI/AI.INI`)

**Status:** structure decoded and closed against the whole script corpus; the numbering origin is inferred
**Reference reader:** [`engine/core/src/sim/ai_profile.cpp`](../../engine/core/src/sim/ai_profile.cpp)
**Container:** [ini.md](ini.md)
**Companion:** [vs-host-api.md](vs-host-api.md) (the 245 globals), [vs-language.md](vs-language.md)

`AI.INI` is the AI profile manifest, and it is where a large part of the script VM's global
namespace comes from. [vs-host-api.md](vs-host-api.md) counts 245 bare identifiers the 577
shipped scripts read as globals; four whole prefix families of them — `SS_` (212 uses),
`GS_` (28), `ES_` (10), `TS_` (18) — are **declared here as data**, not compiled into the
engine. So are all 120 `AIV_`/`AIMV_` tuning variables.

## Sections

| Section | Shape | Holds |
|---|---|---|
| `[SquadStates]` | ordered list | the `SS_*` constants |
| `[GAIKAStrat]` | ordered list | the `GS_*` constants |
| `[EconomyScripts]` | ordered list | the `ES_*` constants |
| `[TacticScripts]` | ordered list | the `TS_*` constants, plus `TSH_*` helper scripts |
| `[Scripts]` | `file = signature` | every `.vs` entry point the profile declares |
| `[Vars.All]` | `key = int` | base `AIV_*` / `AIMV_*` values |
| `[Vars.Easy]`, `[Vars.Normal]`, `[Vars.Hard]` | `key = int` | difficulty overlays |

## The closure that proves the constants are data

For squad states, GAIKA strategies and economy scripts, the set of names the shipped
scripts use is **exactly** the set `AI.INI` declares plus one sentinel, with nothing left
over on either side:

| Family | Declared | Used by scripts | Sentinel | Left over |
|---|---:|---:|---|---|
| `SS_` | 13 | 14 | `SS_IDLE` | none |
| `GS_` | 5 | 6 | `GS_NONE` | none |
| `ES_` | 7 | 8 | `ES_NONE` | none |

(`SS_STR` is excluded: it is a host function, `str, int state`, not a constant.)

The four sentinels `SS_IDLE`, `GS_NONE`, `ES_NONE` and `TS_NONE` are the **only** names of
these families that appear as strings in `gbr.exe`. Not one of the names `AI.INI` declares
does. The engine hardcodes the sentinels and reads the rest.

A constant's value is its position in its section, **the sentinel is `0`, and the declared
entries run from `1`. All three are now proven**, not inferred: `gbr.exe` registers
`SS_IDLE` with the literal value 0, and the routine at `0x004435cb` registers the whole
enum set with a running index starting at 1.

**What is *not* the retail numbering is the order.** The engine parses `[SquadStates]`,
`[GAIKAStrat]`, `[EconomyScripts]` and `[TacticScripts]` from **every** `data/ai/*/AI.INI`
into one `std::map` keyed by the name (`0x00441160` builds it; `0x00440ac0` walks it with
`repe cmpsb`/`setl`), and only then assigns the running index. So a retail id is the name's
**alphabetical rank across all profiles**, not its line number in one file. The same holds
for `AIV_*`/`AIMV_*`, which share a second map and are numbered from 0, unioned across all
four `[Vars.*]` sections.

The reference reader numbers by declaration order within a profile instead, and that is a
**deliberate, documented divergence**. Both id spaces are internal — nothing outside the
engine sees them, and reader and writer only have to agree with each other — so the
difference is invisible until someone compares against a retail desync dump, at which point
it will matter and this paragraph is the place to start.

`[TacticScripts]` is not purely an enum: of its 21 entries, 7 are `TSH_*` helper scripts
and one (`TS_GaulStrongholdHeroRush`) is never referenced as a constant at all. It is a
script list from which the `TS_*` ids happen to be drawn.

## Profiles are overlays, and two of the three ship empty

`playerdata/@AI` names a subdirectory of `DATA/AI`. `gbr.exe` carries `/ai.ini` and
`data/ai/ai.ini` as separate strings, which is what a profile path and a default path look
like. Three subdirectories ship:

| Profile | Contents | Referenced by |
|---|---|---|
| `DEFENSIVE` | `AI.INI` + `EVALRECRUIT.VS`, `GAIKAMONITOR.VS`, `MAIN.VS` | `2_Great_loses_Spain`, `4_Great_Battles_Egypt` |
| `CHAOTIC` | **`DUMMY.TXT` only** | `3_Great_Losses_Egypt` (player 3) |
| `DEFAULT` | **`DUMMY.TXT` only** | nothing |

So a shipped map exercises the fallback path: `CHAOTIC` has no `AI.INI` and no scripts, and
the engine must fall back to `data/ai/ai.ini`.

`DEFENSIVE` is a genuine overlay. Its `[SquadStates]`, `[GAIKAStrat]` and
`[EconomyScripts]` are identical to the parent's; it replaces three of the parent's 65
script declarations and retunes a handful of variables (`AIV_Sleep_ES` becomes 5000 on
Easy and 2000 on Hard, against the parent's 3000).

Naming a profile is rare: of the 304 `playerdata` rows in the shipped containers, 285
carry `AI=""`, 16 omit the attribute, and **three** name a profile — the two `DEFENSIVE`
rows and the one `CHAOTIC` row above.

## Difficulty

`[Vars.All]` holds the base values and the three named sections overlay them. `gbr.exe`
builds the section name by concatenating `Vars.` with one of `Easy`, `Normal`, `Hard`, so
the three are selected by index.

`playerdata/@difficulty` takes the values 0 (76 rows), 2 (1 row) and 3 (195 rows) across
the shipped containers, and is absent from 16. Three overlays and a value that reaches 3
means the attribute cannot be a direct 0-based index into them. The reference reader maps
`1`, `2`, `3` to Easy, Normal and Hard and `0` to no overlay, which is consistent with
every row observed. **Inferred.**

This sits awkwardly against `CONST.INI`, which declares exactly three difficulty addends —
`EasyDifficultyLevelAddend`, `NormalDifficultyLevelAddend`, `HardDifficultyLevelAddend` —
and nothing for a fourth setting. Either `difficulty="0"` means "no AI overlay, default
combat difficulty", or the attribute is 0-based and one of the two readings above is
wrong. The engine's own combat code models three levels and its AI variables model a base
plus three, and the reimplementation currently keeps both rather than picking.

## Dead and missing variables

`[Vars.*]` declares 127 distinct names across all four sections; the scripts read 120.

- **8 declared but never read:** `AIV_MaxEChariot`, `AIV_MaxGWFighter`, `AIV_MaxRTribune`,
  `AIV_MaxTTeutonArcher`, `AIV_MaxTValkyrie`, `AIV_MilHeavyWant`, `AIV_PurposeArenaRush`,
  `AIV_TradeFood`. Compare `FullArmor` in `CONST.INI`, which is declared and appears zero
  times in `gbr.exe`: declared data is not necessarily live data.
- **1 read but never declared:** `AIV_NoRepair`. Whatever the engine's default for an
  undeclared variable is, the shipped AI depends on it.

## What is still unknown

- **Whether the alphabetical-rank numbering above matters in practice.** It is proven for
  the retail engine and the reference reader deliberately does not reproduce it. Nothing
  breaks until a conformance run compares against a retail dump.
- **What an empty section in an overlay means.** `DEFENSIVE`'s `[TacticScripts]` is present
  and empty. "Inherit the parent's table" and "this profile has no tactic scripts" are both
  readable, and the corpus does not choose: the only script that reads a `TS_` constant,
  `GETTACTICSCRIPT.VS`, lives in the shared directory rather than in either profile. The
  reference reader inherits, because that keeps a shared script's constants resolvable
  under every profile.
- **The engine's default for a variable no profile declares** (`AIV_NoRepair`). Zero is the
  obvious guess and is therefore the one to distrust.
- **How `DEFAULT` is selected.** No map names it and it is empty, so it may be a naming
  convention the editor offers rather than anything the engine reads.
- **Whether `[Scripts]` signatures are authoritative.** The same signature appears as a
  leading `//` comment inside each `.vs` file. The two agree wherever both exist in the
  shipped data; which one the engine actually reads is unknown.
