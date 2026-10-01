# Adventures and conquests: the campaign layer

**Status:** structure established from the shipped containers and `gbr.exe`; the mission
transition mechanism is read out of the executable and is **unexercised by the shipped content**
**Reference reader:** [`src/imperivm/formats/map.py`](../../src/imperivm/formats/map.py) (`GameInfo`,
`ConquestMap`, `SequenceRef`), ported in
[`engine/core/src/sim/campaign.cpp`](../../engine/core/src/sim/campaign.cpp)
**Corpus tests:** [`tests/test_corpus_adventure.py`](../../tests/test_corpus_adventure.py)
**Container format:** [bfhp.md](bfhp.md); **container contents:** [map.md](map.md)

[map.md](map.md) describes what one container holds. This document describes what makes a
*sequence* of them a campaign: the three container flavours, the conquest's territory graph,
and — the question the whole layer exists to answer — **what crosses a mission boundary**.

Everything below is measured over **24 containers**, found by content. Sniffing the `HPFS`
magic finds 23 and misses `Packs/RandomMapSettlements.bfhp`, which is a whole-file
[LZIS](lzis.md) stream wrapping one. Twenty-two of the 24 carry a `game.xml`; the two that do
not are `Packs/newmap.BFHP` (a bare `Maps/1` template) and the installation root's
`currentadv.bfhp`.

## The three flavours

`game.xml`'s `game_type` is the only thing distinguishing them, and the editor's three blank
templates in `Packs/` prove what the number means by carrying one of each:

| `game_type` | Flavour | Template | Retail containers |
|---:|---|---|---:|
| 0 | scenario | `Packs/emptyscn.bfhp` | 6 |
| 1 | adventure | `Packs/emptyadv.bfhp` | 14 |
| 2 | conquest | `Packs/emptyconquest.bfhp` | 2 |

`DATA/SETTINGS.XML` names the vocabulary from the other side: `<game name="Adventures/archtest"
type="adventure"/>`.

- A **scenario** is a standalone skirmish map. The three hand-made ones in `Scenarios/` carry no
  scripts, no conversations and no groups at all.
- An **adventure** is a scripted mission. All thirteen shipped ones — six `GreatBattles`, six
  `GreatChallenges`, the `Tutorial` — hold **exactly one** `Maps/<n>` directory. The engine
  supports more; see [The adventure carry mechanism](#the-adventure-carry-mechanism-exists-and-the-shipped-content-cut-it).
- A **conquest** is a territory graph over several maps. `Conquests/mediterranean.BFHP` holds
  seven — `Maps/3, 4, 6, 7, 8, 9, 10`, **not contiguous** — plus one document no other container
  has: `territories.xml`. It is the only conquest in the retail install, so every claim about
  conquests here rests on a corpus of one.

`start_map` is a directory number, never an index: the conquest starts on `Maps/10` and
`GreatBattles/1_Great_Battles_Zama` keeps its only map in `Maps/6`. It names a directory the
container actually holds in all 22 files.

## Running one: what the engine has to connect

The campaign layer is not a subsystem, it is a *chain*, and every link of it was missing at once
for longer than anybody noticed, because each subsystem passed its own tests.

1. **The container's own scripts are in no pack.** `Installation` resolves `.vs` sources against
   `data.pak`, which holds the 577 shared behaviour and AI scripts. The other **308** — every
   mission script in the game — live inside the containers. `imperivm::gamedata::ContainerScripts`
   chains the container ahead of the pack, the way the retail engine's `CurrentGame/` and
   `CurrentMap/` roots do.
2. **Somebody has to read `sequences.xml`.** Nothing did. `parse_sequences` and
   `sequence_entry_path` (in `sim/campaign.hpp`) turn a manifest into container entry names, and
   `GameSession::start_sequences` compiles all of them and spawns the autorun ones.
3. **Somebody has to hand `territories.xml` to `CampaignSystem`.** `SessionInputs::conquest`
   carries the bytes and `GameSession::create` calls `configure`. Without it the seven
   territories are seven rows nothing has read, and `ConquestBonus` answers against an empty
   table while reporting success.

The measurement, on `Adventures/GreatBattles/1_Great_Battles_Zama.bfhp` at 200 turns of 800 ms:
before, 1,119 scripts started and 6 distinct traps, all of them in `data/subai/*_idle.vs`; after,
1,136 scripts and 23 distinct traps, the 17 new ones being the map's 17 sequences each dying at
its own first unimplemented host call. The conquest is starker: all seven of its maps now load,
`Sequences/seq0.vs` runs, and `/Bonus` appears in the environment's root scope — which is
`ConquestBonus`, `EnvWriteString/2` and a configured `CampaignSystem` all working at once.

**No `EndGame` site is reached yet, and that is the honest state.** The 61 `EndGame/3` sites are
all at the end of a mission, and every sequence traps long before then: the 308 sequence scripts
make 3,832 free-function call sites against 71 unimplemented entry points, led by
`RunAIHelper/4` (285), `AreaCenter/1` (279), `ClassPlayerAreaObjs/3` (274), `AttackArea/2` (262)
and `IsAIHelperRunning/1` (236), plus 1,727 member call sites over 33 names. The other blocker is
`Host::global`: a map's own `NO_*`, `Q_*` and group names are declared by `map.obj.xml`, not by
`gbr.exe`, and nothing resolves them, so about half the sequences die on an unknown global before
they reach a host call at all.

## How a mission sequence is declared

**It is not, for adventures.** There is no manifest anywhere in the installation. `gbr.exe`'s
`CVXUIPreAdventureMenu` carries two literals — `adventures/GreatBattles/` and
`adventures/GreatChallenges/` — and enumerates each directory. The twelve files in those two
directories are named `<n>_<title>.bfhp` with `n` a 1-based run of 1..6 in each, and that prefix
is the only ordering the data provides. `Adventures/Tutorial.BFHP` sits in neither directory and
has no prefix. Each of the twelve numbered containers has a sibling `.bmp` of 316,856 bytes —
the menu's preview image. `Tutorial.BFHP` has none.

**For a conquest it is a graph**, declared in `territories.xml`.

## `territories.xml`

One `<conquestmap>` of `<territory>`, at the container root. A *second, smaller* copy of the same
document lives outside the container at the path `data` names, and the two disagree — see
[What is still unknown](#what-is-still-unknown).

```xml
<conquestmap name="Mediterranean " data="ConquestMaps/1 - GBR Europe" ConqueredOrder=""
             choose="1" owned_colorize="1" enemy_colorize="1" disabled_colorize="0"
             owned_hue="560" enemy_hue="0" disabled_hue="1000"
             owned_sat="1360" enemy_sat="1000" disabled_sat="1024" interface="-1">
  <territory id="Spain" index="3" state="1" visualname="Hispania" mapname="Spain"
             description="Iberia is a peaceful nation of farmers…"
             bonus="rIberia" bonus_descr="Your warriors heal themselves…"
             neighbours="Gaul, Carthage" interface="3"/>
  …
</conquestmap>
```

| `<conquestmap>` attribute | Domain | Meaning |
|---|---|---|
| `name` | free text | `"Mediterranean "`, with the trailing space |
| `data` | path | a directory **outside** the container holding the campaign-map art. Spelled `ConquestMaps/1 - GBR Europe`; the directory on disk is `1 - GBR europe`. **Resolve case-insensitively.** |
| `choose` | 0, 1 | `1`. Reads as "the player picks a starting territory"; not proven |
| `ConqueredOrder` | see below | empty in the retail file |
| `interface` | -1 | none; a race index on a territory (below) |
| nine colour knobs | integers | `owned_`/`enemy_`/`disabled_` × `_colorize`/`_hue`/`_sat`. **Nine, not the twelve [map.md](map.md) claims** |

| `<territory>` attribute | Domain | Meaning |
|---|---|---|
| `id` | 7 distinct ASCII names | the handle `SetTerritoryState` and `neighbours` use |
| `index` | 2..8, distinct | **not** the `Maps/<n>` number. What it indexes is not established |
| `state` | see [Territory state](#territory-state) | `1` (`tsOwned`) on all seven |
| `visualname` | Latin name | `Hispania`, `Roma`, `Gallia` — what the player sees |
| `mapname` | a `map.xml` `name` | resolves to exactly one `Maps/<n>` in all seven cases |
| `description` | free text | the pre-mission briefing |
| `bonus` | a root sequence name | **not a class name** — see below |
| `bonus_descr` | free text | what the reward does, in prose |
| `neighbours` | comma-separated `id`s | 18 references; all resolve, and the graph is symmetric |
| `interface` | 0..7, distinct | the territory's race index (Gaul 0 … Germany 7); kept within the eight races, 0x00506560. See the unknowns list |

`mapname` binds a territory to a map through the map's own `name`, not through a number:

| Territory | `mapname` | `Maps/<n>` |
|---|---|---:|
| Spain | `Spain` | 10 |
| Britain | `Britain` | 3 |
| Gaul | `Galicia` | 7 |
| Italy | `Rome` | 9 |
| Carthage | `Cartago` | 4 |
| Egypt | `Egypt` | 6 |
| Germany | `Germany` | 8 |

Seven territories, seven maps, a bijection.

### `bonus` is the trap

Its seven values — `rIberia`, `rBritain`, `rGaul`, `rRepublicanRome`, `rCarthage`, `rEgypt`,
`rGermany` — look exactly like class names: a prefix and one of the race names the class graph
uses. **None of them is a class.** They are the `name`s of `<sequence>` elements in the
**container-root** `Sequences/sequences.xml`, and all seven carry `autorunallowed="no"` so that
they run only when invoked. [map.md](map.md) records the same trap.

The conquest is the **only** container in the install with any root-level sequences. It has
eight: `StartBonuses`, which autoruns, and the seven bonuses, which do not. A root sequence's
`script` attribute is rooted at `CurrentGame/`; a map's is rooted at `CurrentMap/`.

### Territory state

`gbr.exe` registers three constants for it. The values are **proven**, read from the
registration block at VA `0x503800`, which pushes each literal beside its name and calls
`RegisterConstant` (`0x69c3a0`, a `__thiscall` taking `(const char* name, int value)`):

```
    push 1 ; push 0x7BD1E8 "tsOwned"    ; call RegisterConstant
    push 0 ; push 0x7BD1E0 "tsEnemy"    ; call RegisterConstant
    push 2 ; push 0x7BD1D4 "tsDisabled" ; call RegisterConstant
```

| Constant | Value |
|---|---:|
| `tsEnemy` | 0 |
| `tsOwned` | 1 |
| `tsDisabled` | 2 |

**The registration order is not the string-pool order.** The pool lists them `tsDisabled`,
`tsEnemy`, `tsOwned`, which reads as 0, 1, 2 and is exactly wrong for two of the three. Reading
values off adjacency would have made the value the shipped file carries `tsDisabled`.

### `ConqueredOrder`

`gbr.exe` writes it as an XML attribute (`%s="%s"`, at `0x502040`) using the format string
`, %d`, so it is a comma-separated list of integers, appended to as territories fall. It is
**empty in the retail file**, so no populated example exists and the numbering — whether the
integer is a territory's `index` (2..8) or its position in the document (0..6) — is not
established.

## The host API

Three entry points, all registered together at VA `0x503800`. **None of them appears in
[vs-host-api.md](vs-host-api.md)**, because that inventory is built from the 577 `.vs` files in
`data.pak` and all three are reached only from scripts stored *inside* a conquest container.

All three go through `gbr.exe`'s **numeric** registrar (`0x699bb0`), not the signature-string one
(`0x699d20`), so **there is no signature string for any of them and no argument names exist in
the binary**. The types below come from the pushed type codes, against the table the 35
`RegisterType` sites define (`int` = 0x01, `str` = 0x0B).

| Entry point | Signature | Call sites in the install |
|---|---|---|
| `ConquestBonus` | `str ConquestBonus()` | 1 |
| `SetTerritoryState` | `void SetTerritoryState(str, int)` | 7 |
| `GetTerritoryState` | `int GetTerritoryState(str)` | **0** |

The conquest's map scripts also settle a second correction to
[vs-host-api.md](vs-host-api.md): **`EndGame` has a three-argument form.** That document records
arity 2, `(int, bool)`, from the four `DATA/GAMESCRIPTS` victory conditions. **All 61 `EndGame`
call sites in the 24 containers pass three arguments** — not one passes two — and the third is
the message shown on the end-game screen:

```
    EndGame(1, false, Translate("You have conquered Hispania!") );
    EndGame(1, true,  Translate("You have failed to conquer Hispania!") );
```

> This is a correction to [vs-host-api.md](vs-host-api.md)'s claim that "`gbr.exe` carries the
> real thing … interleaved with the host entry point names in its string pool are their C-style
> signatures". That is true of the ~406 entry points registered through `0x699d20` and **false
> of the ~1058 registered through `0x699bb0`**, which carry no signature string at all. Both
> registrars exist and both are used.

## What crosses a mission boundary

The question the layer exists to answer. There are two answers, because there are two
mechanisms, and only one of them is used by anything that shipped.

### A conquest carries territory state and one string, and resets everything else

Every conquest mission's victory sequence ends the same way. This is
`Maps/10/Sequences/seq6.vs`, verbatim and entire:

```
while(1)
{
	Sleep(1000);
	if(NO_AI.obj.player == 1 )
	{
		if(Q_Lose.IsEmpty() )
		{
			SetTerritoryState("Spain",tsOwned);
			EndGame(1, false, Translate("You have conquered Hispania!") );
		}
	}
}
```

All seven maps do exactly this for their own territory, always with `tsOwned`, always
immediately before the winning `EndGame`. No script anywhere writes a territory state on defeat.

And every mission *begins* by autorunning the container-root `StartBonuses`
(`Sequences/seq0.vs`), whose first half is:

```
set = ClassPlayerObjs("BaseTownhall" , 1).GetObjList()[0].AsBuilding().settlement;

gold = 6000;
pop = 40;
food = 2000;

set.SetFood(food);
set.SetGold(gold);
set.AddToPopulation(pop);

EnvWriteString("/Bonus" , ConquestBonus());

if ( EnvReadString("/Bonus")=="rIberia" )
	RunSequence("rIberia");
… six more, one per territory …
```

Read those together and the answer is flat, and it is not the answer one expects:

- **No hero, no unit, no item and no gold crosses.** Every conquest mission starts its town hall
  at exactly 6000 gold, 40 population and 2000 food, unconditionally, by assignment. There is no
  roster to carry and nothing reads one. `persist_state` is `0` on all seven conquest maps.
- **Territory state crosses**, written by the finished mission's own script and read back by the
  engine from the conquest's `territories.xml` under the `ConquestTempFolder/` mount.
- **And one string.** `ConquestBonus()` returns the name of exactly one bonus sequence and
  `StartBonuses` runs exactly that one. The bonuses are **not cumulative**: `seq0.vs` is a chain
  of seven independent `if`s over a single value, and each bonus script itself loops on
  `while (EnvReadString("Bonus") == "rX")`. One conquered territory's reward is live at a time.

The rewards are **spawned, not transferred**. `rRepublicanRome` (`Sequences/seq7.vs`) places a
fresh `RHero3` at level 20 and a `Trader` wagon holding 6000 gold beside the town hall;
`rGaul` places four fresh `GTridentWarrior` at level 12; `rCarthage` researches `Spoils of war`
and places four `CWarElephant`. A level-20 hero appears in mission two whether or not you had one
in mission one.

`seq0.vs` is also the only place in the whole install that uses the environment store's **root
scope** — `EnvWriteString("/Bonus", …)` with a one-argument key. [`sim/env.hpp`](../../engine/core/include/imperivm/core/sim/env.hpp)
records that scope as declared by `gbr.exe` and called by none of the 577 scripts in `data.pak`,
which is true of `data.pak` and not of the install.

### The adventure carry mechanism exists, and the shipped content cut it

`gbr.exe` implements a **party carry-over** for adventures: heroes and the units attached to
them, moved from one `Maps/<n>` to another inside one container. The class is `CVXPartyList`, it
sits between `CVXGameConq` and `CVXGameAdv` in the string pool, and its trace strings spell the
mechanism out:

```
Storing party before moving to another map:
Party unit of class %s, this ptr is 0x%08x, handle is %d
attach
attaching unit %d to hero %d
-- AIP unit %d -> hero %d
--- unit found in storebin, object is %08x, stored handle is %d
Party unit of class %s, this ptr is 0x%08x, old handle is %d
Placing party on new map:
Entering new map...
mapchangecounter
```

It is driven by the host entry point `ChangeMap`, guarded by `ChangeMap: Maps can be changed only
in adventure mode` and `ChangeMap: No map is named %s` — so its first argument is a map's `name`,
the same string `map.xml` carries, and the feature is adventure-only. `WaitForMapChange` and
`SaveAdventure` carry the matching `… called in a non-adventure game!` guards.

**Nothing in the shipped content uses it.** Across all **308** `.vs` files stored in every
container in the install there are exactly **12** `ChangeMap` call sites and **0** of them are
outside a comment block. All twelve are in the six `GreatBattles` adventures, all twelve have the
same shape, and all twelve sit immediately after the `EndGame` that replaced them:

```
PlayMovie(Translate("movies\\1_Extro_Great_B_Zama.avi") );
EndGame(1, false, Translate("You have defeated Hannibal at Zama.") );
/*
EnvWriteString("/LastMap","WinZama1");
ChangeMap("MainMap", "A_MainMap");
*/
```

Eleven distinct outcome tokens are written to `/LastMap` in those comments — `WinZama1`,
`LoseZama1`, `WinNumantia1`, `WinAlesia1`, `LoseAlesia1`, `WinBattleForEG1`, `LoseBattleForEG1`,
`WinBritainConquest1`, `LoseBritainConquest1`, `WinGerman1`, `LoseGerman1` — and every one of
them is inside a comment too.

So the Great Battles were built as **one container with a `MainMap` hub** that each battle
returned to, carrying its party and an outcome token in the root-scope environment store. The
shipping build cut the hub: each battle became a standalone container that ends at `EndGame`,
and **no map named `MainMap` exists anywhere in the installation**. That is why every adventure
holds exactly one map.

## The live-state files

### `currentadv.bfhp` — the adventure save

`gbr.exe` mounts it as the virtual folder `AdvSave/` while an adventure runs (`0x4ccabc`), and
copies it to and from `AdvSaveGame/currentadv.bfhp` when a named slot is saved or loaded
(`0x4cc960`). It is an [HPFS](bfhp.md) container, not a flat record: the whole mounted
filesystem is what gets persisted. `ConquestTempFolder/` is a second mount point, referenced
from four sites in the conquest code; that it is the conquest's equivalent of `AdvSave/` is
inference from its name and its neighbours.

In this installation it is **two 4096-byte blocks holding an empty root directory** — a valid
container with no entries at all. No campaign is in progress, which is what makes the file
evidence of the mechanism and not of its contents.

The save-file extensions live beside the menu code: `-adv.adv`, `-conq.con`, `.snr`, `.con`.

### `tempadv.sdw` — not campaign state

Zero bytes here. One write site in the whole binary (`0x495210`, opened with create/read-write in
the editor path) and **no read site at all**: a scratch shadow file. Its layout is not
determinable and nothing should try.

### `Profiles/profiles.ini`

Decodes with the [ini.md](ini.md) reader, and is the one file that exercises a corner of that
dialect:

```ini
[]
default=profiles/Nome
```

**The section name is empty.** A stricter reader would reject the file that tells the game which
profile to load. `default` names a directory under the installation root.

### `Profiles/<name>/player.ini`

Settings, a lobby snapshot, and a post-match journal. Sixty-eight sections in the shipped
profile:

- `[Player]` — the profile itself: `name`, `race`, `games`, `fav`/`fval` (favourite unit and its
  score), plus the last lobby the player configured (`randommap`, `mapname`, `mapsize`,
  `worldpop`, `maxplayers`, `victorycond`, `victorytreshold` (sic), `control`, `difficulties`).
  `victorycond=1 Elimination` is the same `game.xml` spelling [map.md](map.md) documents.
  The lobby keys are the skirmish setup's rules (`SETTINGS.INI`), written by the serialiser at
  0x0056ba60 as `nobonus nofogofwar noexploration sharedcontrol sharedsupport randommap mapname
  randommapflags mapsize startinggold worldpop maxplayers victorycond victorytreshold
  gamespeed`; the shipped profile reads `startinggold=-1 worldpop=150 victorycond=1
  Elimination victorytreshold= sharedsupport=1`. `startinggold=-1` is "Default", the map's
  own; `worldpop` is a percent (`CONST.INI [GamePlay] LowPop/NormalPop/HighPop` = 50/100/150);
  `victorycond` empty is "Map Default". This engine keeps the same keys under `[Player]` in its
  own `Saves/settings.ini`. What each rule does at game start is in
  [interface-ini.md](interface-ini.md), "The skirmish setup".
- `[Players]` `Count=8`, then `[Player 0]`..`[Player 7]` — `plrname`, `plrteam`, `plrcolor`,
  `plrbonus`, `plrnation`, `plrtype`.
- `[game0]`..`[game56]` — one record per finished game, 57 of them, matching `[Player] games=57`.
  `id`, a date, `duration`, `multi`, `lost`, `gold`, `food`, `units_prod`, `units_killed`,
  `units_lost`, `units_max`, `level_max`, `level_max_unit`, `health_sacr`, `priests`,
  `favorite`, `enemies`, `allies`, `race`, `damage_taken`, `damage_inflicted`, `player_id`,
  `poser_score`, `kill_healths`, `die_healths`. The last five are the five counters
  [`sim/match.hpp`](../../engine/core/include/imperivm/core/sim/match.hpp) recovers from
  `gbr.exe`'s two serialisers, with `poser_score` misspelt the same way in both.
- `[favmap]` — a per-unit-class tally, 26 classes.

**Nothing in it names an adventure, a conquest, a territory or a campaign.** Campaign progress is
not kept per profile.

### `Profiles/lastconquest.usr` and `Profiles/lastsettings.usr`

Both are [LZIS](lzis.md) streams. Neither is campaign progress despite the names — both are
"the last lobby I configured", which is what the names actually say.

`lastconquest.usr` decompresses to 4,695 bytes opening with a `CVXPersistStream` header:

| Offset | Size | Field | Value here |
|---:|---:|---|---|
| 0 | 4 | `magic` | `gsxv` |
| 4 | 4 | `build` | `0x00012800` |
| 8 | 4 | `version` | `0x67` |

Both `build` and `version` are read back out of `.data` at the two globals the writer pushes
(`0x830214` and `0x8212f4`) and match byte for byte. The loader at `0x5969f5` **rejects any
stream whose `build` differs from the running executable's**, so these files are build-locked.
After the header comes a length-prefixed container path (`conquests/mediterranean`), sixteen
player-slot records carrying a name, a colour and a relations row, and finally the `game.xml`
property strings — including `last_edited_map` `"4"` and `start_map` `"10"`, which match
`Conquests/mediterranean.BFHP`'s `game.xml` exactly. The whole record appears **twice**.

`lastsettings.usr` decompresses to 2,219 bytes and has **no `gsxv` header** — it opens straight
into a length-prefixed `Packs/RandomMap`, then `data/RandomMap/ISLAND.RM.XML`, then the same
sixteen slots. It is written on every path; `lastconquest.usr` only when the persisted `gametype`
field is 1.

Neither file contains any territory name.

### `hof/hof.txt`

Twenty-two bytes: `Imperivm Hall of Fame.`. The hall of fame is the **online GameSpy** one
(`CVXUIOBHallOfFame`, `hof/hof.tmp`, `HallOfFameDownloadWait`), not campaign progress.

## `Local/<lang>/adventure.loc.xml`

A `<translationtable>` of `<translationtableentry id text result>`, one per container. `id`
selects which document the string came from, and the set of ids is what makes it a campaign
document rather than a string table:

| `id` | What it translates |
|---|---|
| `adventure name` | `game.xml`'s `name` |
| `adventure author` | `game.xml`'s `author` |
| `adventure description` | `game.xml`'s `description` — the briefing |
| `adventure player name` | each `player<i>.xml`'s `name` |
| `map display name` | `map.xml`'s `displayname` |
| `map label` | each `labels.xml` `<label>`'s `text` |
| `actor name` | each conversation actor |
| `text from a sequence` | every string a `.vs` passes to `Translate` |

The last one is why movie paths appear in it: `Translate("movies\\1_Extro_Great_B_Zama.avi")` is
a `Translate` call, so the path is a translatable string and the Italian table maps it to itself.
The rest of `Local/<lang>/` is described in [map.md](map.md).

## `Notes.xml` — the mission's objectives

Two documents per container, and the engine searches both: the container root's `Notes.xml`
and the chosen map's `Maps/<n>/Notes.xml`. `gbr.exe`'s catalogue lookup (`0x00557df0`) tries
the `std::map` at `[0x009bdaa0]`, falls through to the one at `[0x009bdaa4]`, and returns null
when neither holds the id.

```xml
<notes>
    <note
        id="GOAL"
        title="Numantia"
        text="Capture the rebel stronghold of Numantia."
        icon=""
        map="Numantia"
        show_on_minimap="1"
        locationx="15231"
        locationy="830"/>
</notes>
```

| Attribute | Meaning |
|---|---|
| `id` | the key `GiveNote`, `RemoveNote` and `IsNoteActive` name. Free-form and **case-sensitive**: the 112 distinct ids include `GOAL`, `War_Strategy1`, `Kill Syphax` and `Roman reinforcements`, so spaces are significant too |
| `title` | the heading on the note list |
| `text` | the body. Carries literal `\n` escapes, which are the author's; nothing unescapes them before the interface |
| `icon` | `gameres/noteicons/triangle.bmp`, or empty for the default |
| `map` | the map name the minimap pin belongs to, or empty |
| `show_on_minimap` | `0` or `1` |
| `locationx`, `locationy` | the pin, in world units. `(-1, -1)` is "nowhere", and 51 of the 122 declarations carry it |

**The catalogue is not the list.** Declaring a note puts nothing on the player's list; a script
does that with `GiveNote(id)`. The two halves are why the catalogue is configuration — rebuilt
from the container on both sides of a save — and the set of *given* ids is world state.

**A note nothing declares cannot be given.** The add helper (`0x005584a0`) looks the id up and
returns before it touches the active set when the lookup misses. Of the installation's 204
**live** literal note calls, 203 name an id their own container declares. The one that does not
is `GiveNote("Historical Inconsistency")` in `1_Great_Battles_Zama` map 6's `seq5.vs`, and its
twin explains it: the same note is written `GiveNote("Historical Inconcistency")` in
`3_Great_Battles_Alesia` map 5's `seq4.vs` and is **commented out** there, so nobody was ever
going to notice that neither container declares it. A grep finds 206 calls; the two it adds are
the commented ones, and one of those two is half the evidence.

`Local/<lang>/**/notes.xml` is the translated copy: the same ids with `title` and `text` in
another language. It is not a second catalogue and must not be added as one.

### What is still unknown

- **Which of the two documents wins a duplicate id.** `0x00557df0` searches
  `[0x009bdaa0]` before `[0x009bdaa4]` and nothing says which global is the container's and
  which is the map's. It is unobservable on shipped data: no id is declared in both, and in
  fact **all 122 declarations are in a map's document** — every one of the 25 container-root
  `Notes.xml` files is an empty `<notes></notes>`, and 36 of the 50 documents in total declare
  nothing at all.
- **What the engine does with `map`, `icon` and the minimap pin.** They are display, and no
  script reads any of them.

`Maps/<n>/labels.xml` and `Maps/<n>/Sequences/sequences.xml` are per-map documents and belong to
[map.md](map.md), which specifies both. Neither is campaign structure: a label is a place name
pinned at a world position, and a per-map `sequences.xml` is that map's trigger scripts. The
campaign-level document with the same name is the **container-root** one, and only the conquest
has any entries in it.

## What is still unknown

- **What the shipped `state="1"` means as a starting position.** The value is `tsOwned`, and the
  container gives it to all seven territories at once, which cannot be where a conquest begins.
  The authoring copy at `ConquestMaps/1 - GBR europe/territories.xml` disagrees with the
  container anyway — it marks Spain and Italy `tsOwned` and the other five `tsEnemy` — and
  spells one attribute `disable_colorize` where the container spells it `disabled_colorize`.
  Either the engine overrides the attribute when a conquest starts, or the shipped file carries
  the author's last saved play state. Nothing distinguishes the two.
- **Which territory `ConquestBonus()` names.** That it returns *one* bonus name is proven by its
  only call site and by its implementation at `0x501440`, which indexes the conquest object by
  one field into records of stride `0xE8` and returns the `std::string` at record `+0x80`. That
  the one it returns is the most recently conquered territory's is **inference** from
  `ConqueredOrder` being an ordered list; a player choosing their reward at the campaign map fits
  the same evidence. The engine acts on the inference in exactly one place —
  `CampaignSystem::carry`, which writes the next mission's reward when a mission is won — and
  labels it there, so that whoever establishes the answer has one line to change. The carry
  itself is [save.md](save.md)'s "campaign between missions".
- **`ConqueredOrder`'s element numbering.** Comma-separated integers, from the `, %d` format
  string. Empty in the one shipped conquest, so whether the integer is `index` or a document
  position is unestablished.
- ~~**`territory/@index`.**~~ **Settled by the art:** `ConquestMaps/<data>/territories.bmp` is
  an 8-bit 1024 × 768 bitmap whose pixel value is the territory's `index` — the histogram is
  exactly `{1, 2, 3, 4, 5, 6, 7, 8}`, 1 being the 707,019 pixels of sea and foreign land, and
  each of 2..8 a contiguous region under the matching name on `global.bmp` (Hispania at 3,
  Britannia at 4, and so on). Its palette is one colour per index for the editor's eyes. The
  `%s/Shield%s.bmp` format takes the territory's **`id`**: `ShieldItaly.bmp` for the territory
  whose map is named `Rome`, `ShieldSpain.bmp`, and the five others, plus `ShieldArrow.bmp`.
  ~~**`territory/@interface`** (0..7, distinct) is still unread.~~ **Read:** it is the
  territory's race, as the race enumeration numbers it (Gaul 0, RepublicanRome 1, Carthage 2,
  Iberia 3, ImperialRome 4, Britain 5, Egypt 6, Germany 7). 0x00506560 keeps the value only
  when `0 <= v < 8`, and all seven shipped territories agree with their `bonus` sequence's
  `r<Race>` name. The interface skin (`INFOBAR_<RACE>.INI`, hence the attribute's name) and
  the human's race when a conquest starts from the territory follow it; that it is the race
  the *player* takes, rather than only the skin, is the reading.
- **What the campaign map shows**, as this engine draws it (`CONQUESTGAME.INI`), each a
  reading: `global.bmp` scaled into the 600 × 400 `ConquestMap` widget (the shields' frame sits
  over the widget's top 90 pixels, which is open sea on the scaled map and land on a scrolled
  one); owned territories tinted with `owned_hue`/`owned_sat` and enemy territories the player
  can reach with `enemy_hue`/`enemy_sat`, on a hue scale of 1536 and a saturation scale where
  1024 is unchanged; unreachable ones untinted (`disabled_colorize` is off and the legend's
  third entry is commented out of the screen); the seven shields as the road of conquest — the
  start, the arrow, then every territory conquered after it; a fresh conquest with `choose="1"`
  offering every territory as the start, the chosen one becoming the player's nation through
  its `bonus` name (`rIberia` → Iberia) applied to the `Mutable` human slot.
- ~~**The `Local/<language>/` copies.**~~ Read now: every `.xml` under it is a
  `<translationtable>` keyed by the English source text -- `adventure.loc.xml` (`adventure name`,
  `adventure description`, `actor name`, `territory name`/`description`/`bonus description`,
  `map display name`, `text from a sequence`), `Maps/<n>/notes.xml` (`note-<map>-<id>-<n>` and
  its `-text-`), `itemsCustom.loc.xml`, and the conversations' `.conv.xml`, which sit beside the
  phrase `.wav`s. `gamedata::read_localisation` collects them and the session merges them over
  the language pack's table, so a note or a `Translate` call finds the container's own words
  first.
- **`persist_state`.** Twelve of the 29 shipped maps set it and seventeen do not, so it is a real
  split, not the near-constant [map.md](map.md) lists it as. All seven conquest maps have it off.
  [map.md](map.md) glosses it as "carry unit state to the next map"; **no shipped container has a
  next map**, so nothing exercises that reading, and the alternative — "save this map's state
  into `currentadv.bfhp` when leaving it" — fits the same data.
- **The party record's layout.** `CVXPartyList` and the store-bin the trace strings describe are
  real, and their serialised form is inside `currentadv.bfhp`, which is empty in this install.
  Nothing here can say what a stored party unit's record contains.
- **Whether finishing adventure *n* unlocks *n+1*.** Nothing in the install records which
  adventures a profile has completed — not `player.ini`, not `profiles.ini`, not either `.usr`,
  and `currentadv.bfhp` is empty. The `1_`..`6_` prefix is a menu ordering and nothing more.
- **`GetTerritoryState`'s purpose.** Registered, typed, and reached by zero call sites in the
  whole installation.
- **The `.usr` payload past the header.** The three header fields are proven and the
  length-prefixed strings are legible, but the fixed-size regions between them — the four bytes
  between `version` and the first length word, and the ~0x88-byte player-slot record — were not
  decoded field by field. The two `0x5f`-ish integers before each name (`0x3539`, `0x1273` in the
  first slot) have no established meaning.
- **Why `lastconquest.usr` holds the same record twice**, and why `lastsettings.usr` has no
  `gsxv` header when `lastconquest.usr` does.
- **The `-adv.adv` / `-conq.con` / `.snr` save files themselves.** None is present in this
  installation, so the named-slot save format is entirely unobserved. See `sim/save.hpp`.
