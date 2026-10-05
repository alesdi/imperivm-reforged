# Class definitions (`.SC.XML`)

**Status:** schema fully enumerated over the retail corpus; a few attribute *semantics* remain open
**Reference reader:** [`src/imperivm/formats/gamedata.py`](../../src/imperivm/formats/gamedata.py)
**Companion:** [`ent-xml.md`](ent-xml.md) (the art side), [`pak.md`](pak.md) (the container)

`.SC.XML` files are the game's object model. Every placeable, spawnable or scriptable thing in
Imperivm — a swordsman, a gate, a lightning-bolt sprite, an outpost, a bridge — is one `<class>`
element. There are **845** of them in `data.pak` under `DATA\CLASSES`, and they form a single
rooted inheritance tree.

A class is a triple:

1. a **native class** (`cpp_class`) that supplies engine behaviour written in C++,
2. a **property bag** inherited down the tree and overridden per class,
3. a **method table** of `.vs` script entry points, likewise inherited and overridable.

Plus a reference to the art (`entity`, an [`.ENT.XML`](ent-xml.md)) and some UI wiring.

## File location and identity

Files live at `DATA\CLASSES\*.SC.XML` (821 files) and `DATA\CLASSES\WINTER\*.SC.XML` (24 files).
Each file contains exactly one `<class>` element; there is no envelope element and no manifest
listing the classes, so the loader must enumerate the directory recursively.

**The file name is not the identity.** `DATA\CLASSES\WINTER\GARENA 1.SC.XML` declares
`id="GArena1Winter"`, and `DATA\CLASSES\WINTER\GBARRACKS.SC.XML` and
`DATA\CLASSES\GBARRACKS.SC.XML` are two different classes sharing a base name. Index by `id`.

All 845 `id` values are unique across both directories. File names may contain spaces
(`SHIPYARD 1.SC.XML`, `COLUMN ALONE1.SC.XML`).

Encoding is cp1252 in practice (ASCII in every shipped file). Some files carry an XML declaration
and an `<?xml-stylesheet href="class.xsl"?>` processing instruction, most do not; both parse the
same. Every one of the 845 files is well-formed XML.

## Document shape

```xml
<class id="BaseBarracks" cpp_class="CVXBarrack" parent="BaseTownBuilding"
    entity="Buildings/RBarracks/RBarracks_as.ent.xml"
    entity_winter="Buildings/RBarracks/RBarracks_w.ent.xml">
    <behavior script="data/subai/arena_behavior.vs"/>
    <properties radius="245" selection_radius="250"/>
    <method sig="idle"  vs="data/subai/barrack_idle.vs"/>
    <method sig="train" vs="data/subai/barrack_train.vs"/>
    <properties interface="thumb,building,queue"/>
    <properties auto_repair="no"/>
    <sounds select="BarracksSelect"/>
</class>
```

Child element order is **not** significant, and elements interleave freely, as above. The
complete element census over all 845 files:

| Element | Occurrences | Files that use it | Cardinality |
|---------|------------:|------------------:|-------------|
| `<class>` | 845 | 845 | exactly 1, the document root |
| `<properties>` | 3,426 | 800 | 0..21, merged (see below) |
| `<method>` | 409 | 100 | 0..60 |
| `<sounds>` | 159 | 143 | 0..4, merged like `<properties>` |
| `<defaultcmd>` | 108 | 32 | 0..14 |
| `<defaultcmd>/<cmd>` | 173 | 32 | 1..n per `<defaultcmd>` |
| `<behavior>` | 49 | 41 | 0..4 |
| `<value0>`…`<value5>` | 111 | 38 | 0..1 each |
| `<nodefcmdinherit>` | 2 | 2 | 0..1, empty element |

No element ever carries text content. There are **135 distinct attribute names** across
**163 distinct (element, attribute) pairs**.

## `<class>` attributes

| Attribute | Count | Required | Domain |
|-----------|------:|----------|--------|
| `id` | 845 | yes | unique string; the registry key |
| `cpp_class` | 845 | yes | one of 26 `CVX*` names, see [data-model.md](../data-model.md) |
| `parent` | 845 | yes (may be empty) | an `id`; 93 distinct non-empty values, empty only on `Object` |
| `entity` | 821 | no | path to an `.ENT.XML`, or `""` |
| `altid` | 100 | no | a second registry key (95 distinct) |
| `entity_winter` | 59 | no | path to an `.ENT.XML`, or `""` |
| `entity_autumn` | 43 | no | path to an `.ENT.XML` |
| `entity_spring` | 43 | no | path to an `.ENT.XML` |

Only eight attribute-presence combinations occur:

| Count | Attributes present |
|------:|--------------------|
| 694 | `id cpp_class parent entity` |
| 82 | + `altid` |
| 18 | `id cpp_class parent entity entity_autumn entity_spring entity_winter` |
| 14 | `id cpp_class parent entity_autumn entity_spring entity_winter` (no `entity`) |
| 11 | the seven-attribute form + `altid` |
| 10 | `id cpp_class parent` (no entity at all) |
| 9 | `id cpp_class parent entity entity_winter` |
| 7 | that + `altid` |

### `parent` and the root

`parent` is present on all 845 classes. It is empty (`parent=""`) on exactly one class, `Object`,
which is the single root. **Every non-empty `parent` resolves** to a declared `id`; there are no
dangling parents and no cycles. Maximum chain length is 7, e.g.
`Object → Unit → Military → Ranged → Sentry → BSentry → BSentry1`.

`parent` is never resolved through `altid` in the shipped data — always through `id`.

### `altid`

100 classes carry an `altid`; 95 values are distinct. It is a *second* name the class answers to,
used where data outside the class graph wants to name a class generically:

```xml
<class id="GShipyard1" altid="Shipyard 1" cpp_class="CVXTownHall" parent="BaseShipyard" …>
    <properties importsettlement="Shipyard 1"/>
```

Three `altid` values are claimed by more than one class — `Tavern` (3), `Village` (3),
`Townhall` (2) — so an `altid` lookup is not unique and the engine presumably takes the first or
last registration. One `altid` (`Inn`) collides with a real `id`. **A loader must therefore resolve
`id` first and only fall back to `altid`.** With that rule, every `projectile_class`,
`projectile_explosion`, `projectile_shadow`, `projectile_fire`, `building_projectile_class`,
`sentry_class_name`, `select_class` and `defender_cls_*` value in the corpus resolves to a class.

`importsettlement` is the exception and is **not** a class reference: only three of its eight
values (`Shipyard 1`, `Shipyard 2`, `Shipyard 3`) happen to match an `altid`, while `Shipyard 4`
and `RShipyard 1`..`RShipyard 4` match nothing. Read it as a settlement-template name — the same
namespace as `Packs/RandomMapSettlements.bfhp` — not a class id.

### `entity` and the seasonal variants

Four attributes select the [entity](ent-xml.md) (the sprite/animation definition):

- `entity` — the default,
- `entity_spring`, `entity_autumn`, `entity_winter` — seasonal overrides.

The current season is a game setting: `DATA\SETTINGS.XML` carries
`<settings … season="spring" …/>`. The observed selection rule is

> use `entity_<season>` if the attribute is present and non-empty, otherwise fall back to `entity`.

Evidence and caveats:

- 776 classes declare only `entity`. Season has no effect on them.
- 29 declare all four; 16 declare `entity` + `entity_winter`; 14 declare the three seasonal
  attributes and *no* `entity` at all — so `entity` cannot be treated as mandatory, and a class
  with only seasonal art must still resolve in whatever season is not covered. In all 14 such
  cases the three seasonal paths are non-empty, so the only uncovered season is summer. **The
  summer fallback for these 14 classes is unknown** — the engine may reuse `entity_autumn`, or
  there may be no summer season at runtime at all (only `spring` is observed in shipped settings).
- `entity_autumn == entity_spring` in 33 of the 43 classes that declare both, and the shared file
  is conventionally named `*_as.ent.xml` ("autumn/spring"). Winter files are `*_w.ent.xml`.
- 61 classes have `entity=""`. These are abstract: `Object`, `Unit`, `Building`, `ItemHolder`,
  `Hero`, `Melee`, `Ranged`, and similar interior nodes. An empty string means "no art of my own";
  it does **not** inherit the parent's entity in any observed case, because every concrete
  descendant sets its own.

`entity` is **not** inherited in practice — but nothing in the data proves it is not inheritable,
because no concrete class relies on it. Treat "inherit `entity` from the parent when absent" as
the safe reading: it is consistent with all 845 files, and the 10 classes with no entity attribute
at all are all abstract interior nodes.

### The `WINTER` subdirectory is an editor palette, not a season mechanism

The 24 classes under `DATA\CLASSES\WINTER` are ordinary classes that inherit from their summer
counterpart and pin the winter art:

```xml
<class cpp_class="CVXBarrack" entity="Buildings/GBarracks/GBarracks_w.ent.xml"
       id="GBarracksWinter" parent="GBarracks">
    <properties edittree_pos="Structures/Stronghold (Gaul)/Winter/Barracks"/>
</class>
```

Their `id`s all end in `Winter`, none of them collides with a non-winter `id`, and their only
own property is an `edittree_pos` under a `…/Winter/…` node. They exist so a map author can place
a permanently-winter building on a non-winter map. Runtime seasonal switching is the
`entity_<season>` attributes, described above; these two mechanisms are independent.

## `<properties>` — the property bag

`<properties>` may appear any number of times (0 to 21 in one file; 45 classes have none) and
**all occurrences merge into a single flat dictionary**. There is no nesting, no grouping
semantics, and no order dependence beyond last-wins:

```xml
<properties radius="245" selection_radius="250"/>
<properties interface="thumb,building,queue"/>
<properties auto_repair="no"/>
```

is exactly equivalent to

```xml
<properties radius="245" selection_radius="250"
            interface="thumb,building,queue" auto_repair="no"/>
```

The authors used repetition purely for readability — grouping related settings on their own line
with blank lines between them. `Object` uses 19 separate `<properties>` elements to set 27
properties.

`<sounds>` merges by exactly the same rule, and for exactly the same reason
(`CWarElephant` writes one `<sounds>` element per channel).

**Within one class, later wins.** 16 attribute re-declarations inside a single file exist, all in
`BASERUINS.SC.XML`, where `itemtype` is written 11 times:

```xml
<properties itemtype="Elephant Tusk"/>
<properties itemtype="Boar teeth"/>
…
<properties itemtype="Gem of Wisdom"/>
```

Under last-wins only `Gem of Wisdom` survives, which makes the other ten dead data. This is
probably a list the authors expected to accumulate. **Whether the engine keeps a list or the last
value is unknown** and matters only for `BaseRuins`; flagged for the Part 3 loader.

### Inheritance

Resolution is a straightforward walk to the root, applying each class's merged dictionary over
its ancestors':

```
resolved(C) = merge(resolved(parent(C)), own_properties(C))
```

Every property therefore has a value on every class, because `Object` seeds 24 of them
(`race="None"`, `maxhealth="100"`, `radius="0"`, `interface="thumb"`, …). A property that is never
set on any ancestor has no default in the data; `<properties health="100"/>` on three classes with
no `Object`-level `health` is such a case, and the engine's own default applies.

### Property vocabulary

111 distinct property names. Values are always strings; integers, booleans as `0`/`1` (with
`auto_repair` idiosyncratically `yes`/`no`), and comma-separated token lists.

| Property | Count | Domain |
|----------|------:|--------|
| `icon` | 512 | `gameres/icons/*.bmp`, 258 distinct |
| `edittree_pos` | 423 | `Structures/…` or `Units/…` path; 422 distinct — the map-editor placement tree |
| `radius` | 382 | int 0..1000 |
| `selection_radius` | 378 | int 0..1000 |
| `race` | 364 | 11 values, see below |
| `display_name` | 315 | localisation key, 232 distinct |
| `display_name_plural` | 196 | 67 distinct |
| `help` | 185 | `/contents/…` help-topic path, 151 distinct |
| `wall_set` | 136 | 8 values, one per faction (`British walls`, …) |
| `sentry_class_name` | 130 | a class `id`, 16 distinct |
| `maxhealth` | 127 | int 20..15000 |
| `armor_slash` / `armor_pierce` | 102 / 102 | int 0..22 |
| `damage` | 99 | int 0..300 |
| `speed` | 92 | int 0..240 |
| `unit_specials` | 77 | comma list, 34 distinct tokens |
| `desync_hash` | 52 | `0` / `1` |
| `sight` | 44 | int 0..1400 |
| `auto_repair` | 43 | `yes` / `no` |
| `num_sentry_slots` | 33 | `4` / `5` |
| `interface` | 31 | comma list of 8 tokens, see below |
| `range` | 31 | int 15..1000 |
| `damage_type` | 30 | `slash`, `pierce`, `siege`, `none` |
| `healthbar_type` | 24 | `0`..`3` |
| `projectile_class` | 21 | a class `id`: `Arrow`, `Javelin`, `Axe`, `Slingstone`, `Big_Arrow`, `Gule`, `CGule`, `IGule` |
| `foodperpop` | 17 | `100` / `45`; read by nothing — `gbr.exe` holds no such string and no script reads it |
| `settlement_food` / `settlement_gold` | 16 / 16 | int |
| `description` | 16 | English prose (outpost/teleport tooltips) |
| `min_range` | 15 | int 0..301 |
| `HeroSkills` | 15 | comma list of 5 skills, 24 distinct tokens |
| `formation_priority` | 15 | int 100..2000 |
| `formation_radius` | 15 | `26` / `35` / `45` |
| `itemtype` | 12 | item name (see `DATA\ITEMS.XML`) |
| `water_unit` | 12 | `0` / `1` |

The remaining 78 properties occur 11 times or fewer. In descending order:
`feeds`, `target_factor`, `is_central_building`, `can_be_captured`, `produces_gold`,
`produces_food`, `healthbaroffset`, `can_be_attacked`, `is_single_building`, `settlement_maxfood`,
`settlement_maxgold`, `population`, `efficiency`, `max_population`, `max_units`,
`decor_always_animate`, `building_projectile_class`, `exit_vector_x`, `exit_vector_y`, `max_food`,
`minimap_icon_type`, `importsettlement`, `build_ship_variation`, `respawn_item`, `respawn_time`,
`stamina_dec_time`, `settlement_icon_name`, `defender_cls_1`, `defenders_max_1`, `defenders_out_1`,
`start_level_1`, `end_level_1`, `repair_rate`, `projectile_explosion`, `does_not_regenerate`,
`health`, `damaged_by_ghost`, `always_visible_on_minimap`, `inventory_size`, `shot_tan`,
`shot_height`, `projectile_shadow`, `projectile_fire`, `no_transparent_draw`, `select_class`,
`house_pop_bonus`, `target_priority`, `delete_empty`, `non_selectable`, `hides_units`,
`can_be_invisible`, `capture_health_percent`, `exit_interval`, `attack_delay`, `maxstamina`,
`can_be_cloned`, `ignore_passability`, `itemmanage`, `minlevel`, `levelperitem`, `snapdistance`,
`splash_radius`, `shaman_food_gain`, `minrange`, `initial_z`, `heroarmyexpgain`, `max_army`,
`signt`, `defender_cls_2`, `defenders_max_2`, `defenders_out_2`, `start_level_2`, `end_level_2`,
`max_units_to_board`, `attack`, `max_load`.

Four of these are almost certainly **typos in the shipped data**, each appearing exactly once and
shadowed by a correctly-spelled sibling: `signt` (vs `sight`), `minrange` (vs `min_range`),
`attack` on `<properties>` (no such property elsewhere), and `rolloveer` on `<value1>`
(vs `rollover`). A loader should keep unknown properties rather than reject them.

#### What keeps two units apart: `radius`, `water_unit`, `ignore_passability`

Unit-versus-unit avoidance reads three class properties, each from a slot the class reader
fills by name:

| Property | Slot | Stored at | Read by |
|---|---|---|---|
| `radius` | `[class+0x2dc]` | `0x005a1a8c` | every overlap test: a unit is a circle of this radius, so two block each other when their centres are nearer than the two summed |
| `water_unit` | `[class+0xb30]` | `0x005a4f18` | the sidestep, which lets a water unit give way only onto deep water (terrain index 13) and a land unit only off it |
| `ignore_passability` | `[class+0x31c]` | `0x005a5317` | the owned destination lock: a route whose owner's class sets it gets none |

`Unit` declares `radius="15"`; the `Object` root declares `radius="0"`, which is what the
`DestLock` class (`cpp_class="CVXDestLock"`, parent `Object`) inherits and which the
sidestep then measures a lock by — the lock's own radius lives on the object, not the
class. `sim/avoidance.hpp` has the mechanism.

#### The outpost defender block, and what an undeclared property is worth

Eleven properties travel together, and they are the reason "undeclared" is not the same as "zero"
anywhere in this format.

| Property | Slot | Declared by |
|---|---|---|
| `defender_cls_1` / `defender_cls_2` | 1 / 2 | the class of unit the outpost garrisons itself with |
| `defenders_max_1` / `defenders_max_2` | 1 / 2 | how many to spawn |
| `defenders_out_1` / `defenders_out_2` | 1 / 2 | how many to keep outside when an enemy is near |
| `start_level_1` / `start_level_2` | 1 / 2 | the level they start at |
| `end_level_1` / `end_level_2` | 1 / 2 | the level they reach after an hour of game time |
| `settlement_food` | — | the food a captured outpost is stocked with |

Six classes declare them — `BOutpost`, `COutpost`, `GOutpost`, `IOutpost`, `ROutpost`, `TOutpost` —
and **only `IOutpost` declares a second slot**. Their common base `Outpost` declares none of the ten
slotted ones and no `settlement_food` either. `gbr.exe` copies the whole block onto every
`CVXOutpost` instance at construction (`0x00563f70`), into two `std::string`s at `+0x208`/`+0x224`
and nine dwords at `+0x240 .. +0x260`, and the six `Building::Get*` entry points read exactly one
field each.

**The sentinel, and the one property that keeps it.** `CVXClass`'s constructor (`0x005a6820`) fills
every numeric property slot in a fresh descriptor with `0xff1b1e40` — read as a signed int, exactly
**-15,000,000** — and every string slot with `"**Invalid**"`. Those are not what a script sees.
`ResolveInheritance` (`0x0059c310`) runs two passes per class, both inside its `if (parent != null)`
branch:

1. **inherit** — a slot still holding the sentinel takes the parent's value;
2. **normalise** — a slot *still* holding the sentinel is written to `0`, and a string still holding
   `"**Invalid**"` is assigned `""`.

The normalise pass covers descriptor `+0xa4c .. +0xa68`, which is the ten slotted numbers. It does
**not** cover `+0xa8c`, which is `settlement_food`. So an outpost whose ancestry never declares a
defender slot answers `0` and `""`, and one whose ancestry never declares `settlement_food` answers
**-15,000,000**. A root class — one with no `parent` — skips both passes and keeps every sentinel;
no shipped class is one.

This is what makes `DATA\SUBAI\OUTPOST_BEHAVIOR.VS` correct on the five outposts with a single slot.
It guards the second slot with `if (sDefenderCls2 != "")`, and the guard is **false**, so the spawn
loop never runs. Had the string sentinel survived the guard would have passed and the loop would
have tried to place `defenders_max_2` units of a class called `**Invalid**`.

A reader that resolves properties by walking the ancestry reproduces all of this with two fallbacks:
`0` and `""` for the ten slotted properties, and `-15,000,000` for `settlement_food`.

#### `race`

| Value | Classes (resolved) |
|-------|-------------------:|
| `None` | 357 |
| `Gaul` | 86 |
| `RepublicanRome` | 69 |
| `Carthage` | 58 |
| `Britain` | 57 |
| `Egypt` | 57 |
| `Germany` | 57 |
| `Iberia` | 57 |
| `ImperialRome` | 44 |
| `Mutable` | 2 |
| `German` | 1 |

`German` is a typo for `Germany` on a single class (`TOutpost`). `Mutable` marks the two
race-agnostic editor placeholders (`MutableStronghold`, `MutableVillage`). `None` is the
`Object` default and covers decor, effects, projectiles and neutral wildlife.

Note that **Rome is two races**, `RepublicanRome` and `ImperialRome`, with distinct buildings,
heroes and wall sets. The engine therefore has eight faction races, not seven.

#### `interface`

A comma-separated token list naming the UI panels the object's selection shows. Eight tokens:

| Token | Classes |
|-------|--------:|
| `thumb` | 844 |
| `building` | 295 |
| `items` | 292 |
| `unit` | 169 |
| `holder` | 161 |
| `hero` | 114 |
| `queue` | 74 |
| `empty` | 1 |

Only 12 distinct strings occur; `thumb` alone (243), `thumb,unit,items` (153),
`thumb, building` (151 — note the space, so the parser must trim), `thumb,hero,items,holder` (114),
`thumb,building,queue` (74). `empty` appears once, on the `Empty` class, and is the only value
that does not include `thumb`.

## `<method>` — script binding

```xml
<method sig="attack" vs="data/subai/unit_attack.vs" verify="data/subai/unit_attack_verify.vs"/>
<method sig="boardshipcommon" vs="data/subai/unit_board_common.vs"
        onfinish="data/subai/unit_board_common_onfinish.vs"/>
```

| Attribute | Count | Meaning |
|-----------|------:|---------|
| `sig` | 409 | the method name; 184 distinct |
| `vs` | 409 | required; the `.vs` script that implements it |
| `verify` | 44 | a second script consulted before the method may run; 24 distinct |
| `onfinish` | 10 | a script run on completion; 6 distinct |

409 `<method>` elements across just 100 classes bind 270 distinct `.vs` files. Methods are
inherited and overridden by `sig`, exactly like properties:

```
methods(C) = merge(methods(parent(C)), own_methods(C))   # keyed on sig
```

`Unit` alone declares 58 distinct signatures in 60 elements; `Hero` declares 40. Resolution gives
a plain unit like `BBowman` a 65-entry method table and a hero like `BHero1` an 80-entry one.

**Duplicate `sig` within one class occurs 14 times** — most conspicuously in `UNIT.SC.XML`:

```xml
<method sig="attack" vs="data/subai/unit_attack.vs" verify="data/subai/unit_attack_verify.vs"/>
<method sig="engage" vs="data/subai/unit_engage.vs"/>
<method sig="attack" vs="data/subai/unit_advance.vs"/>
```

Under last-wins, `Unit.attack` binds `unit_advance.vs` and loses its `verify` script. Whether the
engine takes the last, the first, or builds an overload list keyed on something not in the XML is
**unknown**. The classes affected are `Unit` (`attack`, `onenter`), `Hero` (`attack`),
`BDruid` (`hide`), `GDruid`/`RPriest` (`heal`), `Bear`/`Boar`/`LionF`/`LionM`/`Peaceful`/`GGhost`
(`attack`) — i.e. it clusters on `attack`, which suggests a deliberate but undocumented
mechanism rather than pure sloppiness. Flagged for Part 3.

`sig` values that are not lowercase identifiers exist: `Scout Area`, `Gossip`,
`form-ranged-wings`. Treat `sig` as an opaque string.

### Three `sig` values the engine calls rather than the content

Almost every `sig` is invoked *by name* — by a command, a verifier, or another script. Three are
not: `ondie`, `onkill` and `onenter` are hooks the engine fires at fixed moments, and nothing in
the 885 shipped `.vs` files names any of them.

| `sig` | Declared on | Fired on | Parameters |
|-------|------------:|----------|------------|
| `ondie` | 14 classes, 14 elements | the object, when it dies or is erased | `Obj this` |
| `onkill` | `Unit`, 1 element | the attacker, on a fatal blow | `Obj this, Obj Victim` |
| `onenter` | `Unit`, 2 elements | the object, on entering a holder | `Obj this, Settlement sett` |

**Fifteen classes declare one, in seventeen elements.** `onenter`'s two are one of the fourteen
duplicate-`sig` cases above and both name the same file, so the ambiguity is inert there.
Inheritance is what makes the numbers matter: after resolution **203 of the 845 classes answer
`ondie`** — `Military` is the base of every soldier — and **271 answer each of `onkill` and
`onenter`**, which is everything under `Unit`.

The parameter lists are the scripts' own header comments, which is the only declaration of a
`.vs` signature that exists; see the `.VS` specification. `MILITARY_ONDIE.VS` pays out the
Warrior Tales research and tracks `BestHeroLevel`, `UNIT_ON_KILL.VS` books the kill and the
Egyptian and British level-up research, and `UNIT_ON_ENTER.VS` converts a unit's carried Spoils
of War into the settlement's gold.

The fourth member of the family is the `onfinish` **attribute** rather than a `sig` — 10
elements, 6 distinct files — and how the engine fires it is not established here.

What the bound scripts can *do* is outside this document; see the `.VS` specification.

## `<behavior>` — the ambient script

```xml
<behavior script="data/subai/arena_behavior.vs"/>
```

One attribute, `script`. 49 occurrences across 41 classes. Unlike a `<method>`, a behavior has no
`sig` and is not invoked by name — it is the always-running script attached to the instance.

Five classes declare more than one: `BaseTownhall` (4), `BaseVillage` (3), `Outpost` (3),
`EagleSummoned` (2), `TTent` (2) — so concurrent behaviors within a class are clearly intended.

Twelve classes declare a `<behavior>` *and* inherit one (`BOutpost`, `COutpost`, `IOutpost`,
`ETownhall`, `TTownhall`, `TBarracks`, `BTempleOfThor`, `ETempleOfOsiris`, `TTempleOfNeptus`,
`ITemple`, `Shrine`, `MutableStronghold`). The pattern is consistently *base declares the generic
behaviours, subclass adds one faction-specific script* — `Outpost` declares
`outpost_behavior_guard.vs` + `outpost_behavior.vs`, and `COutpost` adds `coutpost_behavior.vs`.

**Accumulation, settled from `gbr.exe`.** The class loader appends each `<behavior>` to the
class's list as it parses the element (`0x005a046b`; a script that does not compile is reported
and left out, `0x005a059c`), and only once every class file is read does it resolve
inheritance, appending the parent's resolved list *behind* the class's own (`0x005a5fd0`,
recursive). Neither step looks for a duplicate, and both stop at eight. So a class's list is
its own behaviours in document order, then its parent's list, then its grandparent's, and so on:
`COutpost` runs `coutpost_behavior.vs` in slot 1 and the two `Outpost` scripts after it.
`MutableStronghold` is the one shipped class that declares a script it also inherits
(`settlement_behavior_ambient.vs`), and it runs it twice. No shipped class comes near eight;
the most is five.

**Each instance runs every entry of its class's list at once**, from the moment it exists. The
object's start virtual (`vtbl+0x2c`, `0x005aec40`), which the factory calls on every object it
builds, spawns entry `i` into the object's script slot `i + 1` (slot 0 is the command slot)
with the object as the script's one argument — every behaviour in the corpus opens
`// void, Obj This`. It does nothing while a save is loading, because the save restores the
coroutines, or in the editor. The scripts are the object's: they are saved with it and end with
it, and nothing restarts them when the object changes hands.

39 distinct behavior scripts, e.g. `arena_behavior.vs`, `gate_patrol.vs`, `sneak_behavior.vs`,
`summoning_behavior.vs`, `townhall_autotrain.vs`, `outpost_behavior_guard.vs`.

## `<sounds>` — audio binding

```xml
<sounds select="BarracksSelect"/>
<sounds command="data/sound entities/VoiceGHero.xml" attack="UnitSwordFight" die="UnitDeath"/>
```

Six attributes, all optional: `command` (100), `attack` (34), `select` (24), `die` (22),
`walk` (15), `idle` (4). 159 elements across 143 classes; repeated elements merge attribute-wise
exactly like `<properties>`, and inherit the same way.

The value is **either a path or a bare name**:

- if it contains a separator, it is a resource path — a `.wav` in `Sounds.pak` or a sound-entity
  XML in `data.pak` (`data/sound entities/VoiceGHero.xml`);
- otherwise it names a sound entity and resolves to `DATA\SOUND ENTITIES\<name>.XML` in
  `data.pak`. `select="BarracksSelect"` → `DATA\SOUND ENTITIES\BARRACKSSELECT.XML`, which is a
  weighted list of four `.wav` files.

`Object` declares `<sounds select="" command=""/>`; empty means silent.

## `<defaultcmd>` — the right-click command table

```xml
<defaultcmd target="">
    <cmd name="move"/>
    <cmd name="advance" ctrl="1"/>
</defaultcmd>
<defaultcmd target="ShipBattle">
    <cmd name="boardship"/>
    <cmd name="approach"/>
</defaultcmd>
```

Each `<defaultcmd>` binds a *target class* to an ordered list of commands. `target=""` is the
no-target case (right-click on terrain). 108 `<defaultcmd>` blocks across only 32 classes —
this is declared high in the tree (`Unit` has 13, `Hero` 14, `Wagon` 10) and inherited.

| Element | Attribute | Count | Domain |
|---------|-----------|------:|--------|
| `defaultcmd` | `target` | 108 | `""` or a class `id`: `Unit`, `ShipBattle`, `Hero`, `Building`, `Inn`, `BaseTownhall`, `BaseVillage`, `Outpost`, `BaseShipyard`, `TTent`, `Military`, `Tower`, `Gate`, `Wall`, `Teleport`, `Wagon`, `ItemHolder`, `Catapult`, `ShipS` (20 values) |
| `cmd` | `name` | 173 | a method `sig`, 28 distinct |
| `cmd` | `ctrl` | 13 | always `1` — the modifier-held variant |

`target` names a *base* class (`Unit`, `Building`, `Military`), so matching is by subtype: a
right-click on any descendant of `Building` uses the `target="Building"` block. The list order is
presumably "first command whose `verify` passes", but nothing in the class XML states that.

`<cmd name>` vocabulary, by frequency: `approach` 46, `attack` 22, `tribute_default` 13,
`attach` 10, `boardship` 10, `enter` 9, `move` 8, `attack_ground` 8, `advance` 5, `transport` 5,
`attack_independent` 4, `capture` 4, `catapult_attack` 4, `unitsout` 3, `teleport` 3,
`stay_hidden` 3, `curse` 2, `heal` 2, `attack_unit_type` 2, `getitems` 2, and singletons
`hide`, `cripple`, `moveinfight`, `boardshiphero`, `teach`, `boardunit`, `unload`, `follow`.

### `<nodefcmdinherit/>` and the default merge rule

An empty marker element, present on exactly two classes (`Sentry`, `Wagon`). It appears
immediately before that class's own `<defaultcmd>` blocks and means **do not inherit the parent's
`<defaultcmd>` table** — build this class's table from its own blocks only. Both users are classes
whose interaction verbs differ sharply from `Unit`'s (a sentry cannot be ordered around; a wagon
is captured rather than commanded).

Its existence settles the default: **`<defaultcmd>` blocks accumulate down the tree**, keyed by
`target`, and `<nodefcmdinherit/>` is the opt-out. 21 classes declare `<defaultcmd>` blocks while
also inheriting some, and only these two opt out — so a subclass adding a `target="Wagon"` block
keeps the inherited `target="Unit"` and `target=""` blocks. Whether a same-`target` block in a
subclass replaces or appends to the inherited one is **unknown**; replacement is the natural
reading and no corpus case distinguishes them.

## `<value0>` … `<value5>` — the info-bar slots

Six optional, singular, positional elements defining what the selection info bar shows:

```xml
<value0
    icon="gameres/infobar/common/level ico.bmp"
    script="return .AsUnit.level;"
    flags="-1"
    help="/contents/stats/level"
    rollover="Level"/>
```

| Attribute | Meaning |
|-----------|---------|
| `icon` | a `.bmp` under `gameres/infobar/common/` |
| `script` | a **one-line inline `.vs` expression**, not a file path — the only place in the format where script source is embedded rather than referenced |
| `rollover` | tooltip label (localisation key) |
| `help` | `/contents/…` help topic opened on click |
| `flags` | `-1`, `0`, or `5`; **meaning unknown** |

Per-slot counts: `value1` 27, `value2` 24, `value3` 24, `value0` 22, `value5` 8, `value4` 6,
spread over 38 classes. `<value4/>` appears once as an empty element in `BASEBUILDING.SC.XML`,
which is how a class blanks an inherited slot.

The six slots are inherited and overridden per index — `BaseBuilding` replaces `Unit`'s six with
settlement statistics (population, loyalty, gold, food, health). 15 classes override a slot that
an ancestor already set.

## Path conventions

Paths inside `.SC.XML` use `/` and mixed case; pack entries use `\` and uppercase. Comparison must
normalise both. One virtual root exists:

> **`gameres/` maps to `UI\` in `UI.pak`.**

`gameres/icons/BBarracks.bmp` is the pack entry `UI\ICONS\BBARRACKS.BMP`;
`gameres/infobar/common/pop ico.bmp` is `UI\INFOBAR\COMMON\POP ICO.BMP`. Every other path
(`data/subai/…`, `Buildings/…`, `Units/…`, `Sounds/…`, `data/sound entities/…`) is a literal pack
path. Applying that one alias, 266 of 268 icon references resolve.

## Reference integrity

Measured across all 845 classes against the 14,203 entries of the retail packs. See
[data-model.md](../data-model.md) for the full report; the headline is that the graph is
essentially clean:

| Reference kind | Distinct | Dangling |
|----------------|---------:|---------:|
| `parent` | 94 | **0** |
| `entity` / `entity_*` | 684 | 6 |
| `vs` / `verify` / `onfinish` | 339 | **0** |
| `icon` | 268 | 2 |
| `<sounds>` | 148 | 8 |
| class-name properties (`projectile_class`, `sentry_class_name`, `importsettlement`, …) | — | **0** |

## What is still unknown

- **`flags` on `<value0..5>`.** Only `-1`, `0` and `5` occur. Probably a visibility or
  formatting bitmask; not inferable from the data.
- **Duplicate `<method sig>`.** 14 cases, clustered on `attack`. Last-wins is assumed but
  unproven, and under last-wins the `verify` script on `Unit.attack` is discarded, which looks
  wrong.
- **Repeated `<properties itemtype>` on `BaseRuins`.** Eleven values; last-wins discards ten.
  A list semantics is plausible and would need a different merge rule for that one property.
- **The `onfinish` attribute.** 10 elements over 6 distinct files, and the fourth member of the
  engine-fired family the three `on*` sigs belong to. When the engine runs it, what it is handed,
  and whether it fires on a method that was interrupted as well as one that ran out, are all
  unread. The three `sig` hooks beside it are settled; this one is not.
- **`entity` inheritance and the summer fallback.** 14 classes declare only spring/autumn/winter
  art. Whether `entity` is inherited from the parent when absent cannot be settled from the data
  because every concrete class sets its own.
- **The `defaultcmd` selection rule.** The command list is ordered, but the criterion that picks
  one entry (presumably each command's `verify` script) is not in this format.
- **Which numeric properties the normalise pass covers.** The outpost block above is settled:
  `+0xa4c .. +0xa68` is normalised and `+0xa8c` (`settlement_food`) is not, and both were read out
  of `0x0059c310`. The rest of the descriptor's numeric slots were not walked, so it is not known
  whether the pass is contiguous over the whole block or a hand-written list with more than one
  hole in it. Anything outside the outpost block should be treated as unmeasured rather than as
  normalised by analogy.
- **Semantics of ~40 low-frequency properties.** `desync_hash`, `target_factor`, `efficiency`,
  `shot_tan`, `initial_z`, `snapdistance`, `healthbar_type`'s four values, `delete_empty`,
  `build_ship_variation` and similar are named suggestively but unverified. They are enumerated
  above with their observed domains; assigning meaning requires either the `.VS` host API
  specification or runtime observation.
- **How the engine discovers classes.** No manifest exists in `data.pak`. A recursive scan of
  `DATA\CLASSES` reproduces the 845, but whether the retail loader scans or has a hard-coded
  list is not determinable from the data files.
