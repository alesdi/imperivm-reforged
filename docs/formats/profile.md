# `player.ini` — the player profile and its match journal

`Profiles/<name>/player.ini`. One file per player, holding three things: the
options that player last chose, a **journal** of every match they have finished,
and a hash over that journal. The *Change player* screen (`PROFILE.INI`) lists
the profiles and shows the selected one's career — twelve lines, none of which
is stored, all of which are recomputed from the journal every time the screen
opens.

This document specifies the journal, the aggregation and the hash. The
`[Player]` block's game-setup keys are specified where the screens that write
them are: see [`interface-ini.md`](interface-ini.md) and the skirmish setup in
`docs/plan.html`.

Reference reader: [`src/imperivm/formats/profile.py`](../../src/imperivm/formats/profile.py).
Engine reader: `engine/core/include/imperivm/core/game/profile.hpp`.

## What is a profile

A profile is a **subdirectory of `Profiles/`**. `gbr.exe` enumerates the
directory (0x005707d0) and takes every entry with the directory attribute whose
name is neither `.` nor `..`; the directory's name is the profile, and the
`[Player] name` inside it is only what is displayed. `Profiles/profiles.ini`
carries a single `default=profiles/<name>` line naming the one selected at
start; it is not a list, and a directory missing from it is still a profile.

The shipped install has one, `Profiles/Nome`, whose `[Player] name` is `Angel`.

## The file

```ini
[Player]
name=Angel
color=0
race=7
games=57
hash=-1650049073
fav=Pretoriano
fval=3395600
; ... the skirmish setup's rules, specified elsewhere

[Players]
Count=8

[Player 0]
plrname=Angel
plrteam=0
plrcolor=9
plrbonus=2
plrnation=1
plrtype=3
; ... one per slot, the setup screen's last roster

[game0]
; ... one per finished match, specified below

[favmap]
RPraetorian=3395600
RPriest=177000
; ... one per class the player has produced, and the weight behind `fav`
```

`[Player] fav` and `fval` are the winner of `[favmap]` and its weight, resolved
when the journal is appended to. The screen reads `fav`, not the map.

### The journal

Sections named `game0`, `game1`, … **consecutively**. The walk asks for
`game<n>` for rising `n` and stops at the first that is absent, so a hole
truncates the journal rather than leaving a gap in it. The shipped profile has
57, `game0` through `game56`.

Every key below is **required**. The record reader (0x0056b340) returns a
failure on the first key it cannot read, and the aggregation (0x0056d760)
propagates that rather than skipping the record: a half-written record fails the
whole file. `id` is read as text and the rest as signed decimals.

| Key | Type | Meaning |
|-----|------|---------|
| `id` | text | a 32-hex-digit match id; the first key, and the one whose absence ends the record |
| `year` `month` `day` `hour` `minute` | u16 | when the match ended, local time |
| `duration` | i32 | milliseconds played |
| `mapsize` | i32 | the map's size band |
| `multi` | flag | non-zero for a multiplayer match |
| `lost` | flag | non-zero when the player did not win |
| `gold` `food` | i32 | resources spent |
| `units_prod` `units_killed` `units_lost` | i32 | unit counts |
| `units_max` | i32 | the most units standing at once |
| `level_max` | i32 | the highest level any of the player's units reached |
| `level_max_unit` | text | that unit's display name |
| `health_sacr` | i32 | health spent on rituals |
| `priests` | i32 | priests produced |
| `favorite` | text | the most-produced unit's display name |
| `enemies` `allies` | i32 | how many of each the match had |
| `race` | i32 | the nation played |
| `damage_taken` `damage_inflicted` | i32 | damage, both ways |
| `player_id` | i32 | which slot the player held |
| `poser_score` | i32 | the match's own score line; the screen does not read it |
| `kill_healths` `die_healths` | i32 | health destroyed and lost |

Both flags are stored as `value != 0`, so any non-zero spelling means the same
thing and the hash folds the 0 or 1 rather than the number written.

## The aggregation

Run over the journal in order, from a cleared set of counters. What the twelve
lines show:

| Line | From |
|------|------|
| Rank | the rank table, below, over the military rating |
| Single player games | count and share won of the records with `multi=0` |
| Multiplayer games | the same for `multi` non-zero |
| Game time | `duration` summed, in whole hours (`/ 3'600'000`) |
| Favorite nation | the most played `race`, and its share |
| Favorite unit | `[Player] fav` — **not** aggregated from the journal's `favorite` |
| Resources spent | `gold` and `food` summed |
| Units eliminated | `units_killed` summed |
| Units lost | `units_lost` summed |
| Health spent for rituals | `health_sacr` summed |
| Most experienced unit | the record with the greatest `level_max`, and its `level_max_unit` |
| Maximum number of units | the greatest `units_max` |

The shares are integer percentages, truncated, with **100** where the divisor is
zero — visible only through the nation line, since a games line with no games is
not drawn.

The two maxima are compared differently and both readings are transcribed: the
level's test is unsigned (0x0056dfad) and the unit count's is signed
(0x0056dfd5). Nothing a match can produce tells them apart.

### The military rating

Each record contributes

```
((kill_healths / 2) + damage_inflicted + 1000) * 100
--------------------------------------------------------
((die_healths  / 2) + damage_taken     + 10000)
```

in unsigned 32-bit arithmetic, truncating; the contributions are summed as a
64-bit signed total. The rating is that total over the number of matches, the
divisor floored at one. The two constants are what stop a match in which
nothing happened from dividing by zero — such a match scores 10.

### The nation histogram has nine slots and admits ten values

`race` is counted when it is in `0..9`, into an array of **nine** counters, and
only the first nine are summed and ranked (0x0056e26c). A record claiming
nation 9 therefore writes past the histogram into an unrelated local and counts
towards no nation. No shipped record does — the nations are 0 to 7 with 8 for
random — so this is recorded rather than repaired, and the engine reproduces it
by keeping a tenth slot nothing reads.

Ties keep the **lower** nation index: the search moves only on a strict
improvement.

## The rank table

`DATA\CONST.INI`:

```ini
[Ranks]
; the ranks MUST be ordered in ascending order on their points
RanksCount = 13
RankPoints0 = 0
RankName0 = Peasant
...
```

The walk takes the last row, **in file order**, whose `RankPoints` is at or
below the rating, stopping at the first that is above. It is not a search and
it does not sort.

The shipped table breaks its own comment: `RankPoints10 = 2` where ascending
order wants something above 200. So a rating of 200 passes row 10 as well and
the walk only stops at row 11's 300 — **every rating from 200 to 299 answers
`Legend`, and `Hero` is unreachable in the retail game.** Reproduced rather
than repaired; `engine/tests/test_profile.cpp` pins it over a thousand ratings.

The thirteen names are Peasant, Apprentice, Fighter, Warrior, Veteran, Master,
Chieftain, Lord, Champion, Hero, Legend, Deity, Cheater.

## The hash

`[Player] hash` is a rolling fold over the journal, written as a **signed**
decimal. Seed `0x48564849`. For a 32-bit value `v` and the running hash `h`,
all arithmetic unsigned and wrapping:

```
mix(v)  = ((((v >> 8) ^ v) >> 5) & 0x07FFFFF8) + (v & 7) + 8 * v
fold(h, v) = x + (x >> 31)   where   x = v + (mix(v) ^ (2 * h))
```

Per record, in this order — which is **not** the order the keys sit in:

1. the running multiplayer game count, after this record's increment
2. the running single-player game count
3. `duration`
4. the nine nation counters, `0` through `8`, after this record's increment
5. `gold`, `food`, `units_killed`, `units_lost`, `health_sacr`
6. `units_max`
7. `lost`, as 0 or 1
8. `allies`
9. `year`, `month`, `day`, `hour`, `minute`
10. `enemies`
11. `level_max`
12. the string constant, for `level_max_unit`
13. `mapsize`, `priests`, `units_prod`
14. the string constant, for `favorite`
15. the string constant, for `id`

`multi`, `race`, `damage_taken`, `damage_inflicted`, `player_id`,
`poser_score`, `kill_healths` and `die_healths` are **not** folded.

### A string folds a constant

A string field contributes `fold(h, 0x0EA75617)` whatever it holds — the
executable walks the string to its terminator and discards the length. The
constant is visible in the binary as `(2h ^ 0x75B00047) + 0x0EA75617`, and
`mix(0x0EA75617)` is exactly `0x75B00047`, which is what says the step is an
ordinary fold of a literal rather than a hash of the bytes. Two journals
differing only in a unit's name hash alike.

### What was validated

The shipped `Profiles/Nome/player.ini` carries `hash=-1650049073`. Recomputing
it from the 57 records — 1,824 folds — returns `-1650049073` exactly.
`tests/test_corpus_profile.py` runs that check against every profile in the
installation on every run, and `engine/tests/test_profile.cpp` pins the same
arithmetic over three records so that it is checked where there is no
installation.

That one number settles the record layout, which fields fold, the order they
fold in, the seed, the nine-slot histogram and the string constant at once. It
is the whole evidence for this document.

## What is still unknown

- **Who writes the journal.** The writer at 0x0056b0e0 emits the same keys and
  is read only far enough to confirm the key set and their spellings. This
  engine never appends to a profile, so nothing here depends on it; a journal
  this engine wrote would be its own file beside the saves, under
  `docs/legal.md` rule 1.
- **Whether `hash` is ever acted on.** It is written and it is recomputable;
  nothing was found that refuses a profile whose stored hash disagrees.
- **`[favmap]`'s weight.** `fval=3395600` for `RPraetorian` is plainly a
  running total and plainly not a unit count; what it accumulates was not
  chased, because the screen reads `fav` and never the map.
- **`mapsize`'s bands.** Folded and stored, never displayed; the values in the
  shipped journal are all 0.
- **`poser_score`.** Read by the record reader into its own field and used by
  nothing the profile screen touches.
- **The two unused headings.** The screen's heading table has fourteen slots
  and the screen draws twelve; slots 4 and 8 hold `OUT FOR LUNCH` and
  `OUT FOR DINNER`, are translated along with the rest, and are never shown.
