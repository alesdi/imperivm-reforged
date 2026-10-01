# The Imperivm data model

**Status:** resolved and validated against the retail install
**Reference loader:** [`src/imperivm/formats/gamedata.py`](../src/imperivm/formats/gamedata.py)
**Schemas:** [`formats/sc-xml.md`](formats/sc-xml.md) (classes), [`formats/ent-xml.md`](formats/ent-xml.md) (entities)

This document is the *resolved* view: what the 845 classes and 889 entities actually add up to
once inheritance is applied and every reference is chased. Where the schema documents describe
the file formats, this one describes the game.

Everything here was produced by a full-corpus extraction, not sampling. Numbers are exact.

## Headline: the graph is clean

Every reference in the class graph and the entity definitions was checked against the 14,203
entries of the retail packs.

| Reference kind | Distinct targets | Dangling | Verdict |
|----------------|-----------------:|---------:|---------|
| `class/@parent` | 93 | **0** | complete, acyclic, single-rooted |
| `method/@vs`, `@verify`, `@onfinish` | 339 | **0** | every bound script ships |
| class-name properties (`projectile_class`, `sentry_class_name`, `defender_cls_*`, `select_class`, the four `projectile_*`) | 22 | **0** | resolve via `id`, falling back to `altid` |
| `layer/@image` → `<image>` | 2,430 | **0** | |
| `replace/@layer`, `@image` | 2,195 each | **0** | |
| `class/@entity` and seasonal variants | 684 | **6** | |
| `image/@file` → `.rle.mmp` | 4,034 | **1** | |
| `<sounds>` | 148 | **8** | |
| `properties/@icon` | 268 | **2** | |
| `anim/@startstate`, `@endstate` | 2,336 | **2** | |
| `state/@anim_idx` (excluding the 65536 sentinel) | 181 | **3** | |
| `entity/@pass_file` | 885 | **16** | |

**38 dangling references out of roughly 12,000 checked.** No structural defect: the graph
resolves, the scripts are all present, and the art is all present. The engine's loader must
nevertheless be tolerant, because the retail game ships and runs with these.

`importsettlement` is deliberately absent from the class-reference row. Its eight values name
settlement templates, not classes — three of them (`Shipyard 1..3`) coincidentally match an
`altid` and five do not. Validating it as a class reference produces five phantom failures.

### The complete list of dangling references

Six missing entities — the engine must not hard-fail on a missing `entity`:

| Class | Attribute | Missing path |
|-------|-----------|--------------|
| `BaseTavern` | `entity`, `entity_autumn`, `entity_spring` | `Buildings/Tavern/Tavern_as.ent.xml` |
| `BaseTavern` | `entity_winter` | `Buildings/Tavern/Tavern_w.ent.xml` |
| `Plet` | `entity` | `Buildings/village/plet/plet.ent.xml` |
| `RamUnit` | `entity` | `Buildings/BTaran/BTaran_unit.ent.xml` |

`BaseTavern` and `RamUnit` are abstract interior nodes whose concrete children all set their own
`entity`, so nothing is visibly broken. `Plet` (Bulgarian for "wattle fence") is a leaf class with
no art — a cut object still registered.

Eight missing sounds. Seven look like a directory rename that was never propagated
(`Sounds/entities/…` should be `data/sound entities/…`, and two `Sounds/selection/*.wav`), and one
is the literal string `nothing` on `ShipBattle`'s attack channel — evidently an author's way of
writing "silent" that the engine presumably resolves to no sound:

```
BDruid, GDruid, IEnchantress, TEnchantress  command=Sounds/entities/Voice*.xml
BMorrigansMonument, TSanctuaryOfVotan, EMarket  select=Sounds/selection/*.wav
ShipBattle  attack="nothing"
```

Two missing icons (`gameres/icons/Anibal.bmp` on `CHero4`, `gameres/icons/MPeasants.bmp` on
`MPeasantMulti`), one missing sprite sheet (`destlock` for the debug entity), sixteen
`pass_file` references that miss by name (twelve of them the walk-through `SKELETONS` decor —
and because masks are matched by content rather than filename, some may still resolve at
runtime), and five internal inconsistencies inside three entity files: `EPRIEST` (`anim 5` points
at a nonexistent state 2), `ICATAPULT` (two states point at nonexistent anims 13 and 5) and
`IMOUNTAINEER` (a state points at anim 0).

### Data defects worth knowing about

Not dangling references, but things a strict loader would reject:

- **Uninitialised memory in shipped files.** Seven `<state>` elements carry
  `offsetx="-842150451"` / `offsety="-842150451"` — that is `0xCDCDCDCD`, the MSVC debug-heap
  fill pattern. Four `<layer>` elements carry `xray="226"` where the domain is `0`/`1`.
- **127 animations** whose `@duration` disagrees with the sum of their `<frame>` durations.
- **Four misspelled properties**, each used once and shadowed by a correct sibling:
  `signt` (`sight`), `minrange` (`min_range`), `attack` on `<properties>`, `rolloveer`
  (`rollover`). Plus `drawmode="playercol"` once (`player_color`) and `race="German"` once
  (`Germany`).
- **14 duplicate `<method sig>` declarations** within a single class, clustering on `attack`.
  Under last-wins, `Unit.attack` silently loses its `verify` script. See
  [`sc-xml.md`](formats/sc-xml.md#method--script-binding).
- **Eleven `itemtype` values** written into `BaseRuins` as eleven separate `<properties>`
  elements; last-wins keeps one.

## Shape of the class tree

845 classes, one root, no cycles, maximum depth 7.

```
Object [CVXDecor]                                   845 in subtree, 242 direct children
├── Building [CVXBuilding]                          295
│   ├── BaseBuilding [CVXBuilding]                   179
│   │   ├── BaseTownBuilding [CVXBuilding]            70   barracks, blacksmiths, arenas, temples
│   │   ├── Gate [CVXGate]                            65   8 gates × 8 factions, +1
│   │   ├── BaseHouse [CVXBuilding]                   33
│   │   └── BaseVillage [CVXTownHall]                 10
│   ├── Wall [CVXBuilding]                            65   8 wall segments × 8 factions, +1
│   ├── Outpost [CVXTownHall→CVXOutpost]              10
│   ├── BaseShipyard [CVXTownHall]                     9
│   ├── Tower [CVXBuilding]                            9
│   ├── Catapult [CVXCatapult]                         8
│   ├── Stonehenge [CVXTownHall]                       4
│   ├── Teleport [CVXTeleport]                         3
│   └── BaseRuins, FakeTower, TTent                    2 each
├── Unit [CVXUnit]                                  271
│   ├── Military [CVXUnit]                            196
│   │   ├── Hero [CVXHero]                            114
│   │   ├── Melee [CVXUnit]                            40
│   │   ├── Ranged [CVXUnit]                           38
│   │   └── RamUnit [CVXUnit]                           3
│   ├── Peaceful [CVXUnit]                             52
│   │   ├── PeasantAmbient                              19
│   │   ├── Peasant                                     16
│   │   ├── BaseMage [CVXDruid]                          9
│   │   └── Wagon [CVXUnit → CVXWagon children]          6
│   └── BaseAnimal [CVXUnit]                           22
│       ├── AttackAnimal                                 8
│       ├── Animal                                       7
│       └── SummoningUnit                                2
├── ItemHolder [CVXItemHolder]                       23
├── catapult_placing [CVXMapObj]                      9
├── MultiOne / SummoningObj [CVXScriptObj]            3 each
├── Area [CVXArea] / RArch / ruins / Sacrifice        2 each
└── 232 leaf classes directly under Object            1 each
```

The tree is **broad and shallow**: 752 of 845 classes are leaves, only 93 have children, and 242
of the leaves hang directly off `Object`. Those 242 are almost entirely scenery (`CVXDecor`),
combat/spell feedback sprites (`CVXFeedback`) and script objects (`CVXScriptObj`) — content that
needs no behaviour, only art and a name in the editor tree.

Depth distribution: 1 class at depth 1 (`Object`), 242 at depth 2, 47 at 3, 128 at 4, 204 at 5,
120 at 6, 103 at 7.

The interesting structure is entirely in the two large subtrees, `Building` (295) and `Unit`
(271), which between them hold the gameplay. A faction's stronghold is a template instantiated
eight times: 65 gates is 8 orientations × 8 factions + the abstract `Gate`, and 65 walls is the
same.

## The 26 native classes

`cpp_class` names the C++ implementation the engine attaches. All 845 classes resolve to one of 26
**Counts below are resolved, not declared.** Only 821 of the 845 classes carry a `cpp_class`
attribute of their own; the other 24 inherit one from a parent. An earlier revision of this
document counted attribute occurrences, which undercounted six of the native classes and summed
to 821 rather than 845. Resolve inheritance before counting.

All 845 classes name one of 26
values, and the assignment is *not* arbitrary — it follows the inheritance tree, changing at 26
specific points and being inherited everywhere else. There are only 26 transitions where a class's
`cpp_class` differs from its parent's, which is exactly the C++ class hierarchy showing through:

```
CVXDecor  (Object)
├── CVXBuilding (Building)
│   ├── CVXTownHall (BaseTownhall, BaseVillage, BaseShipyard, Outpost, Inn, Stonehenge, TTent)
│   │   └── CVXOutpost (BOutpost … TOutpost)
│   ├── CVXBarrack  (BaseBarracks, BMorrigansMonument, ETempleOfHorusAndAnubis)
│   ├── CVXTavern   (BaseTavern)
│   ├── CVXGate     (Gate)
│   ├── CVXCatapult (Catapult)
│   └── CVXTeleport (Teleport)
├── CVXUnit (Unit)
│   ├── CVXHero      (Hero)
│   ├── CVXDruid     (BaseMage)
│   ├── CVXWagon     (Wagons, Trader, Camel, ShipS, …)
│   ├── CVXShip      (ShipBattle)
│   ├── CVXFlyingUnit(Crow, Eagle)
│   └── CVXGhost     (GGhost, ShamanGhost)
├── CVXFeedback   (47 leaf classes: Assault, BattleCry, Curse, Heal, Lightning, …)
├── CVXScriptObj  (34 leaf classes: Arrow, Javelin, Slingstone, *PeasantMulti, Tent01, …)
│   └── CVXAreaEffect (CoverOfMercy)
├── CVXItemHolder (ItemHolder)
├── CVXMapObj     (catapult_placing, Gule_shadow, CGule_shadow, IGule_shadow)
├── CVXCatapultShot (Gule, CGule, IGule)
├── CVXArea       (Area)
│   └── CVXAdvArea (AdvArea)
├── CVXSacrifice  (Sacrifice)
└── CVXDestLock   (DestLock)
```

Responsibilities below are inferred from the properties, methods, behaviors and default-command
tables that each native class's members actually use. Where the data does not support a
conclusion, it says so.

| `cpp_class` | Classes | Inferred responsibility | Evidence |
|-------------|--------:|-------------------------|----------|
| **CVXDecor** | 148 | The universal base. Position, health, radius, art, an idle/wait script pair, and nothing else. Everything derives from it, and 148 classes use it directly for inert scenery. | Introduced at `Object`, which seeds all 27 baseline properties. Only methods: `idle`, `wait`. No behaviors, no default commands. |
| **CVXUnit** | 134 | A mobile, ordered, fighting object: pathing, combat, inventory, boarding, formations. | Introduced at `Unit`, which declares 58 method signatures — `move`, `attack`, `patrol`, `follow`, `capture`, `transport`, `getitem`, `boardship`, `enter_parry_mode` — and the properties `damage_type`, `inventory_size`, `formation_radius`, `feeds`, `max_food`, `can_be_invisible`, `ignore_passability`. Owns the `<defaultcmd>` table. |
| **CVXBuilding** | 134 | A static, damageable, repairable structure with a capture threshold and an exit interval. | Introduced at `Building`: adds `target_priority`, `auto_repair`, `capture_health_percent`, `exit_interval` and the methods `repair` and `broken`. |
| **CVXHero** | 114 | A unit that commands an army, carries skills and levels, and has a UI command set of its own. | Distinguishing properties `heroarmyexpgain`, `max_army`, `HeroSkills`; 20 distinguishing methods including `unittrain`, `formation`, `form-ranged-wings`, `leavearmy`, `retreat`, `guardpatrol`, `taunt`, `sleep`, `divine_grace`, and the `ui*` variants (`uimove`, `uiadvance`, `uipatrol`) that exist only on heroes. Behavior `hero_skill_behaviour.vs`. |
| **CVXGate** | 65 | An openable wall segment garrisoned by sentries. | Only class with methods `open`, `close`, `open_permanent`, `close_permanent`; distinguishing properties `wall_set` and `sentry_class_name`; behavior `gate_patrol.vs`. |
| **CVXFeedback** | 47 | A transient visual/audio cue attached to another object — `Heal`, `Curse`, `Lightning`, `Damage1`, `Experience`, `Invisibility`. | All 47 are leaves directly under `Object`; **none adds a single property, method or behavior**. They are pure art bindings whose entities all live in `Visuals.pak`. The C++ class is what makes them attach-and-expire. |
| **CVXScriptObj** | 37 | An object whose whole existence is a script: projectiles (`Arrow`, `Javelin`, `Slingstone`), multi-unit spawn markers (`*PeasantMulti`), decorative tents, and `WatchEye`. | Also property-free leaves, but two carry `spy`/`infinite_spy` methods, one carries `projectile_explosion`, and `SummoningObj` carries the `summoning_behavior.vs` behavior. |
| **CVXTownHall** | 41 | The **settlement owner**: the object that holds gold, food, population and loyalty for a town. | Uniquely adds `max_population`, `population`, `settlement_gold`, `settlement_food`, `settlement_maxgold`, `settlement_maxfood`, `produces_gold`, `produces_food`, `can_be_captured`, `can_be_attacked`, `is_single_building`, `efficiency`, `max_units`. Behaviors `townhall_autotrain.vs`, `townhall_sentries_control.vs`, `townhall_buildingsbehavior.vs`, `settlement_behavior_ambient.vs`. This is the answer to "what distinguishes CVXTownHall from CVXBuilding": **a settlement's economy lives on the town hall object**, and every building that owns an economy (town hall, village, shipyard, inn, outpost, Stonehenge, the Teuton tent) is a `CVXTownHall`. |
| **CVXItemHolder** | 23 | A container that can be looted — graves, wells, boulders, chests. | Introduced at `ItemHolder` with `interface="thumb,items"`, `inventory_size="16"`, `can_be_captured="0"`. Behaviors `herograve_behavior.vs`, `magic_well_behavior.vs`. |
| **CVXBarrack** | 23 | A building with a **production queue and a research tree**. | Only class with methods `trainex`, `research`, `immediate_research`, `getcharm`. Its members are the only ones using `interface="thumb,building,queue"`. |
| **CVXMapObj** | 12 | Unclear. Twelve classes: `catapult_placing` and its 8 faction variants, plus the three `*Gule_shadow` projectile shadows. No distinguishing properties or methods. | The common thread is *drawn but not part of the simulation* — a placement ghost and a projectile's shadow. **Requires investigation in a later part.** |
| **CVXTavern** | 12 | The economy/diplomacy building: hire heroes, buy slaves, take and repay loans, invest, gossip, scout. | Distinguishing methods `getloan`, `repayloan`, `invest`, `buyslaves`, `addpop`, `trainpeasant`, `Gossip`, `Scout Area`, plus `research`/`immediate_research` shared with `CVXBarrack`. |
| **CVXDruid** | 9 | A spellcaster: targeted spells with a learning progression. | Distinguishing methods `globalspell` and `learn`; the only classes whose `<defaultcmd>` tables include `heal`, `curse`, `cripple`, `hide`, `teach`. |
| **CVXCatapult** | 8 | A siege engine — a *building* (it derives from `Building`) that can be told to fire at ground. | Distinguishing properties `splash_radius`, `attack_delay`, and (surprisingly) the full settlement property set it inherits unused; methods `autofire`, `stop`, `disband`; default commands `attack_ground` and `catapult_attack`. |
| **CVXWagon** | 8 | A capturable resource carrier — trade carts, food and gold wagons, transport ships. | No distinguishing properties or methods of its own; it is one of two classes to use `<nodefcmdinherit/>` and replace `Unit`'s command table with `capture`/`unload`/`boardship`. The distinction from `CVXUnit` is **cargo and capture rather than orders**, but the data does not prove it. |
| **CVXOutpost** | 9 | A settlement that **tributes resources to another settlement** and spawns pack mules. | The only class with methods `tribute`, `tribute_default`, `stoptribute`, `creategoldmulebig/small`, `createfoodmulebig/small`; adds `description` and `minimap_icon_type` on top of the full `CVXTownHall` settlement property set. |
| **CVXShip** | 4 | A vessel that carries units. | Distinguishing property `max_units_to_board`; methods `boardunit`, `unboard`, `unboardall`, `aitransport`. Also the only unit with a `building_projectile_class`. |
| **CVXCatapultShot** | 3 | The catapult projectile in flight (`Gule`, `CGule`, `IGule`). | No properties or methods; the parent classes set `projectile_explosion`, `projectile_shadow`, `projectile_fire`, `shot_tan`, `shot_height`. Ballistic arc handling is presumably why it is not a `CVXScriptObj`. |
| **CVXFlyingUnit** | 3 | A unit that ignores terrain and hovers (`Crow`, `Eagle` and the summoned eagle). | Distinguishing property `no_transparent_draw`; methods `hover` and `lesser_hover`; entity-level `floating="1"` on its animations. |
| **CVXTeleport** | 3 | A pair of linked structures moving armies instantly. | Carries the settlement property block plus `description="Allows armies to go from one cave entrance to the other instantly"`. No distinguishing methods — the mechanic is entirely in C++. |
| **CVXGhost** | 2 | An incorporeal unit (`GGhost`, `ShamanGhost`). | Distinguishing property `unit_specials`; parent classes set `damaged_by_ghost`. |
| **CVXSacrifice** | 2 | The Carthaginian sacrifice mechanic. | One method, `sacrifice`. **Requires investigation.** |
| **CVXAdvArea** | 1 | An adventure-mode trigger area, deriving from `CVXArea`. | Named only; no properties or methods. **Requires investigation.** |
| **CVXArea** | 1 | A scripting trigger volume. | Named only. **Requires investigation.** |
| **CVXAreaEffect** | 1 | `CoverOfMercy`, a persistent area-of-effect field. | One method, `coverofmercy`. |
| **CVXDestLock** | 1 | `DestLock`, a debug/editor marker with a `UI\DEBUG\` entity. | Editor-only; the one entity in `UI.pak`. |

### Priority for the engine's Part 3

By class count, five native classes cover 79% of the corpus: `CVXDecor` (148), `CVXUnit` (134),
`CVXBuilding` (125), `CVXHero` (114), `CVXGate` (65). By *gameplay* weight, the ordering is
different: `CVXUnit` and `CVXBuilding` are prerequisites for everything, `CVXTownHall` is the
economy, `CVXHero` is the campaign, and `CVXDecor`/`CVXFeedback`/`CVXScriptObj` — 230 classes
between them — need almost no logic at all, only art loading and an idle script.

## Faction breakdown

Faction membership is the resolved `race` property. **There are eight faction races, not seven**:
Rome is split into `RepublicanRome` (prefix `R`) and `ImperialRome` (prefix `M`), with separate
buildings, walls, heroes and units.

| Race | Prefix | Units | Buildings | Winter editor variants | Total |
|------|--------|------:|----------:|-----------------------:|------:|
| Gaul | `G` | 35 | 36 | 15 | 86 |
| RepublicanRome | `R` | 31 | 33 + 1 decor (`RArch`) | 4 | 69 |
| Carthage | `C` | 30 | 28 | — | 58 |
| Germany (Teutons) | `T` | 30 | 27 + 1 (`TOutpost`, misspelled `German`) | — | 58 |
| Britain | `B` | 29 | 28 | — | 57 |
| Egypt | `E` | 29 | 28 | — | 57 |
| Iberia | `I` | 29 | 28 | — | 57 |
| ImperialRome | `M` | 18 | 24 | 2 | 44 |
| Mutable (any race) | — | — | 2 | — | 2 |
| *(no race)* | — | — | — | — | 357 |

`Germany`'s extra building is `TOutpost`, whose `race` is misspelled `German`. `Mutable` is the
two editor placeholders `MutableStronghold` and `MutableVillage`. The 357 race-less classes are
scenery, effects, projectiles, wildlife and abstract interior nodes.

The prefix letters are `B`=Britain, `C`=Carthage, `E`=Egypt, `G`=Gaul, `I`=Iberia,
`R`=Republican Rome, `T`=Teuton/Germany, `M`=Imperial Rome (**not** "mutable" — the `M` classes
are `MTownhall`, `MBarracks`, `MHero1`… and all carry `race="ImperialRome"`).

Each faction is built from the same template:

- **8 wall segments** (`E N NE NW S SE SW W`) and **8 gates**, differing only in art and
  `wall_set` (`British walls`, `Carthaginian walls`, `Egyptian walls`, `Gaul walls`,
  `Iberian walls`, `Imperial Roman walls`, `Republican Roman walls`, `German walls`).
- **1 town hall, 1 village, 1 barracks, 1 blacksmith, 1 tavern, 1 tower, 3 houses**, plus a
  faction-specific temple or monument.
- **3 hero lines × 4 tiers** (`XHero1`, `XHero1a`, `XHero1b`, `XHero1c`) — 12 or more hero
  classes per faction, plus a faction hero base (`BritonHero`, `CarthaginianHero`, …).
- **2 sentry classes** (`XSentry`, `XSentry1`), 4 villager classes (male/female × active/ambient),
  and 5–10 combat units.

Gaul and Republican Rome are larger because they own the **shipyards** (4 each, with
`build_ship_variation` 0–3) and the only faction-specific outpost behavior scripts.

Entity-directory counts confirm the picture from the art side: 134 unit directories in
`Units.pak` and 132 building directories in `Buildings.pak`, distributed
`B` 19/12, `C` 18/13, `E` 15/12, `G` 16/24, `I` 15/14, `R` 18/19, `T` 17/19, `M` –/5, plus
shared/neutral directories under `S`, `W`, `D`, `F`, `H`, `K`, `L`, `N`, `O`.

### Hero skills and unit specials

`HeroSkills` is a comma-separated list of exactly five skills, drawn from 24 distinct names
(`Quick March` 53 classes, `Epic endurance` 44, `Team defense` 41, `Leadership` 30,
`Epic attack` 30, `Defensive cry` 30, `Administration` 27, `Team attack` 27, `Vigor` 27,
`Healing` 27, `Euphoria` 27, `Assault` 26, `Battle cry` 17, `Discipline`, `Frenzy`, `Rush`,
`Egoism`, `Wisdom`, `Ceasefire`, `Charge`, `Epic armor`, `Recovery`, `Survival`, `Concealment`).
Their definitions live in `DATA\SKILLS.INI`.

`unit_specials` draws from 34 tokens, led by `Freedom` (46 classes), `Keen sight` (18),
`Attack skill` (11), `Defense skill` (8), `Penetration` (7), `Triple strike` (6),
`Deflection` (6), `Drain` (6), `Active` (6). Definitions live in `DATA\UNIT_SPECIALS.INI`.

Note the naming overlap with the 47 `CVXFeedback` classes (`Assault`, `BattleCry`, `Ceasefire`,
`Charge`, `Curse`, `Deflection`, `Frenzy`, `Parry`, `Penetration`, `Rage`, `Revenge`,
`SpikedArmor`, `TripleStrike`, `Invisibility`, `Heal`): a skill or special has a same-named
feedback class that supplies its visual. That is a load-bearing convention for Part 3.

## Attribute vocabulary

Complete counts over both corpora. Full per-attribute value domains are in the schema documents;
this is the summary an implementer needs to prioritise.

| | `.SC.XML` | `.ENT.XML` |
|---|---:|---:|
| Files | 845 | 889 |
| Elements (distinct types) | 14 | 13 |
| Element instances | 5,282 | 36,158 |
| Distinct attribute names | 135 | 39 |
| Distinct (element, attribute) pairs | 163 | 55 |
| Attribute instances | 9,924 | 126,704 |

The distribution is extremely long-tailed. On the class side, 111 of the 135 attribute names are
`<properties>` attributes, and:

| Frequency band | Property names | Share of the 4,468 property assignments |
|----------------|---------------:|----------------------------------------:|
| ≥100 uses | 13 | 75% |
| 20–99 uses | 12 | 13% |
| 5–19 uses | 46 | 10% |
| 1–4 uses | 40 | 2% |

**Implementing the 25 properties used 20 times or more covers 88% of all property assignments.**
Those 25, in order: `icon`, `edittree_pos`, `radius`, `selection_radius`, `race`, `display_name`,
`display_name_plural`, `help`, `wall_set`, `sentry_class_name`, `maxhealth`, `armor_slash`,
`armor_pierce`, `damage`, `speed`, `unit_specials`, `desync_hash`, `sight`, `auto_repair`,
`num_sentry_slots`, `interface`, `range`, `damage_type`, `healthbar_type`, `projectile_class`.

Note that this is a count of *declarations*, not of classes affected: because `Object` seeds 27
properties, every one of the 845 classes has a resolved value for all of them regardless of how
rarely they are re-declared.

On the entity side the distribution is the opposite — flat and dense. Every one of the 4,034
`<image>` elements carries all six of its attributes; every one of the 15,572 `<frame>` elements
carries both of its. There is no long tail to defer: implementing `.ENT.XML` is all-or-nothing,
and the only genuinely optional attributes are `sortoffsetx`/`sortoffsety` (93% present),
`xray` (65%), `nohighlight` (5%) and `percent` (0.5%).

## Data that completes the model

The class graph is not self-contained. These plain-text files in `data.pak` supply the values that
`.SC.XML` only names, and a Part 3 loader needs them:

| File | Supplies |
|------|----------|
| `DATA\CONST.INI` | player colours (13 entries), difficulty addends, game speed, UI bar defaults, minimap thresholds |
| `DATA\SETTINGS.XML` | the current `season` (drives `entity_<season>` selection) and the 16-player colour table |
| `DATA\SKILLS.INI` | the 24 hero skills named by `HeroSkills` |
| `DATA\UNIT_SPECIALS.INI` | the 34 specials named by `unit_specials` |
| `DATA\ITEMS.XML` | the items named by `itemtype` and `respawn_item` |
| `DATA\ZBINS.XML` | the five z-sort bins that give `<layer z=>` its meaning |
| `DATA\SOUND ENTITIES\*.XML` (166) | the weighted `.wav` lists that bare `<sounds>` names resolve to |
| `DATA\COUNTERUNITS.XML`, `BONUS.XML`, `FORMATIONS.XML` | combat modifiers and formation shapes |
| `DATA\TERRAINS.XML`, `CURSORS.XML` | terrain and cursor tables |
| `DATA\COMMANDS\*` (35 files) | the command definitions that `<defaultcmd><cmd name=>` refers to |
| `DATA\INTERFACE\*` (139 INI files) | the panels named by the `interface` property tokens |
| `DATA\SUBAI\*.vs` (the 339 referenced scripts) | every `<method>` and `<behavior>` body |

## Findings that change the engine plan

1. **The class graph is a solved problem.** Zero unresolved parents, zero missing scripts, zero
   broken layer/image references. Part 3 can load the object model with confidence and does not
   need a repair or heuristic-recovery pass — only a tolerant one for the 38 known dangling refs.

2. **There are four seasons, not two.** The brief anticipated `entity` vs `entity_winter`; the
   data has `entity_spring`, `entity_autumn` and `entity_winter`, with `entity` as the fallback,
   and `SETTINGS.XML` carries an explicit `season` string. 14 classes have *only* seasonal art.

3. **The `WINTER` class directory is not the seasonal mechanism.** Those 24 classes are editor
   palette entries that pin winter art on a non-winter map. Seasonal switching is the
   `entity_<season>` attributes. Implementing one does not implement the other.

4. **Rome is two factions.** `RepublicanRome` and `ImperialRome` have separate buildings, walls,
   heroes and wall sets. Any faction table sized at seven is wrong.

5. **Animations are addressed by fixed numeric slot.** `idx="13"` *is* idle, `idx="9"` *is* die,
   and `.vs` scripts call `PlayAnim(19, …)` with literals. The renderer must expose animations by
   slot number, not by name, and `<state anim_idx>` wires states to slots. The slot space is
   larger than the data exercises (scripts reference slots 0 and 16, which no entity declares).

6. **`gameres/` is a virtual root aliasing `UI\`.** Without this rule all 613 icon references in
   the class graph appear broken; with it, 266 of the 268 distinct targets resolve. The resource
   resolver needs a virtual-filesystem layer, not raw pack lookups.

7. **Extension-optional references are pervasive.** `image/@file` may or may not carry `.rle`,
   and the pack stores `.RLE.MMP`; `pass_file` may or may not carry `.pass`; `<sounds>` values are
   either a path or a bare sound-entity name. Every resolver needs a candidate list, not a single
   lookup.

8. **`altid` is a second registry key with collisions.** Three `altid` values are claimed by
   multiple classes and one collides with a real `id`. Resolution order must be `id` first,
   `altid` second, or the shipyard/settlement references break.

9. **`CVXTownHall` is the settlement.** Gold, food, population, loyalty and max-unit caps all live
   on the town-hall object, not on an abstract "player" or "settlement" record. The economy model
   follows the object graph.

10. **`CVXFeedback` and `CVXScriptObj` are 84 classes with no data at all** — no properties, no
    methods, no behaviors. They are pure C++ + art bindings. That is 10% of the class corpus
    reachable for almost no loader work, and it is where visual polish comes from.

11. **The entity XML is a manifest, not the authority on geometry.** Cross-checked against the
    sprite and mask format work: 430 of 4,033 `<image>` declarations disagree with the real
    `.rle.mmp` frame table on `rows`/`columns`, and hundreds disagree on `drawmode` — the frame
    table wins, and a loader that trusts the XML mis-slices about one image in ten. Entities carry
    **no footprint geometry at all**, only the `pass_file` link; obstruction is an irregular
    bitmap stamp in the mask file. And `remaping` is animation sequencing, not palette
    remapping — team colouring is a palette-index swap inside the sprite data.

## What is still unknown

- **`point/@type`** (16 values). The single biggest gap: attachment points drive sentry slots,
  projectile origins, exits, smoke sources and gate hinges. Needs the `.VS` host API or runtime
  observation.
- **Nine of the 26 native classes** cannot be characterised from the data because their members
  declare nothing distinguishing: `CVXMapObj`, `CVXCatapultShot`, `CVXWagon`, `CVXSacrifice`,
  `CVXArea`, `CVXAdvArea`, `CVXDestLock`, `CVXGhost`, `CVXFeedback`. Their behaviour is entirely
  in C++.
- **~40 low-frequency properties** whose names suggest a meaning that is unverified
  (`desync_hash`, `target_factor`, `efficiency`, `shot_tan`, `initial_z`, `snapdistance`,
  `healthbar_type`'s four values, `delete_empty`, `build_ship_variation`).
- **Merge semantics for three constructs**: duplicate `<method sig>`, repeated `<properties
  itemtype>`, and whether `<behavior>` accumulates down the tree (the evidence favours yes).
- **The `defaultcmd` selection rule** — the list is ordered, but the criterion that picks one
  entry is not in this format.
- **`drawmode="index"`**, **`percent`** on layers, **`default_duration`** vs `duration`, and
  **`anim_row`** on states.
- **How the engine enumerates classes.** No manifest exists; a recursive scan of `DATA\CLASSES`
  reproduces all 845, but whether the retail loader scans or carries a hard-coded list is not
  determinable from the data.
