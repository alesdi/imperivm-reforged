# Entity definitions (`.ENT.XML`)

**Status:** schema fully enumerated over the retail corpus; several numeric enumerations remain unresolved
**Reference reader:** [`src/imperivm/formats/gamedata.py`](../../src/imperivm/formats/gamedata.py)
**Companion:** [`sc-xml.md`](sc-xml.md) (the class graph that references these), [`rle.md`](rle.md) (the sprite frame tables)

An entity is the **art and animation contract** for a class. Where a `.SC.XML` says *what a
Briton bowman is*, its `.ENT.XML` says *which sprite sheets he is drawn from, in what layer order,
which named poses he can be in, and how he animates between them*.

> **The entity XML is a manifest, not the authority.** Two things it appears to declare are
> actually owned elsewhere and must be read from there:
>
> - **Sprite geometry.** `<image rows= columns= drawmode=>` disagrees with the real frame table in
>   the `.rle.mmp` for **430 of the 4,033 resolvable images** on `rows` or `columns`, and for
>   hundreds more on `drawmode`. **The frame table wins.** A loader that slices animations from
>   the XML's grid will mis-slice roughly one image in ten. See [`rle.md`](rle.md).
> - **Footprint geometry.** There is none here — not a width, not a height, not a tile extent.
>   The only footprint information in an entity is the `pass_file` *link*. Static obstruction is
>   an irregular bitmap stamp, interior holes and all, living in the mask file. See
>   [`pass.md`](pass.md).
>
> What the entity XML *is* authoritative for: which sheets exist and their handles, layer stacking
> and offsets, the state machine, the animation slot numbers, and all timing.

There are **889** of them across four packs:

| Pack | Entities | Kind |
|------|---------:|------|
| `MapObjects.pak` | 367 | trees, rocks, ruins, camp props, bridges |
| `Buildings.pak` | 291 | buildings, walls, gates, siege engines |
| `Units.pak` | 148 | units, heroes, animals |
| `Visuals.pak` | 82 | spell and combat effects |
| `UI.pak` | 1 | `UI\DEBUG\DESTLOCK.ENT.XML`, a debug marker |

Every one parses as well-formed XML. Encoding is cp1252 (ASCII in every shipped file). Files
carry an XML declaration and an `<?xml-stylesheet href="../entity.xsl"?>` PI — the stylesheet
itself is *not* in the packs, so the authors previewed these in a browser from a working
directory that never shipped.

## Document shape

```xml
<entity name="BBowman" type="vx/unit" variations="8" pass_file="">
  <images>
    <image idx="1" file="Attack.rle" drawmode="player_color" remaping="none" rows="15" columns="8"/>
    <image idx="2" file="Attack_shadow.rle" drawmode="shadow" remaping="none" rows="15" columns="8"/>
    …
  </images>
  <points>
    <point idx="1" type="2" x="-70" y="74"/>
  </points>
  <layers>
    <layer idx="1" name="unit" image="1" z="1000" offsetx="-93" offsety="-114"
           sortoffsetx="0" sortoffsety="0" xray="0"/>
  </layers>
  <states>
    <state idx="1" name="idle" image_idx="0" image_row="1" offsetx="-93" offsety="-114"
           anim_idx="13" anim_frame="1"/>
  </states>
  <anims>
    <anim idx="1" name="walk" startstate="1" endstate="1" frames="16" duration="924"
          default_duration="0" action_time="0" step="57">
      <replace layer="1" image="11" offsetx="-93" offsety="-114"/>
      <replace layer="2" image="12" offsetx="-93" offsety="-114"/>
      <frame idx="1" duration="0"/>
      <frame idx="2" duration="66"/>
      …
    </anim>
  </anims>
</entity>
```

The five section elements always appear in this order, and each appears at most once:

| Element | Occurrences | Files | Notes |
|---------|------------:|------:|-------|
| `<entity>` | 889 | 889 | root |
| `<images>` | 889 | 889 | always present, may be empty |
| `<layers>` | 889 | 889 | always present |
| `<states>` | 889 | 889 | always present, never empty |
| `<points>` | 888 | 888 | absent from one file, often empty |
| `<anims>` | 886 | 886 | absent from three files, often empty |
| `<images>/<image>` | 4,034 | — | 0..25 per entity |
| `<points>/<point>` | 3,607 | — | 0..41 per entity |
| `<layers>/<layer>` | 2,430 | — | 1..25 per entity |
| `<states>/<state>` | 1,822 | — | 1..5 per entity |
| `<anims>/<anim>` | 1,168 | — | 0..8 per entity |
| `<anim>/<replace>` | 2,195 | — | 0..6 per anim |
| `<anim>/<frame>` | 15,572 | — | 1..34 per anim |

885 of the 889 files carry all five sections in exactly that order; three omit `<anims>` and one
omits `<points>`. No element carries text content. **39 distinct attribute names** across
**55 distinct (element, attribute) pairs** — the format is much narrower than `.SC.XML`, but far
denser in instances.

## `<entity>` attributes

| Attribute | Count | Domain |
|-----------|------:|--------|
| `name` | 889 | 754 distinct; 12 files leave it empty |
| `type` | 889 | `vx/building` 329, `tree` 214, `""` 172, `vx/unit` 156, `tobj` 16, `gaul building` 1, `vx/animal` 1 |
| `variations` | 889 | `1` 730, `8` 108, `12` 45, `32` 5, `36` 1 |
| `pass_file` | 885 | 402 distinct; `""` for entities with no footprint |
| `radius` | 124 | `0` ×120, `150`, `90` |
| `selection_radius` | 124 | `0` ×120, `152`, `92` |
| `floating_turnspeed` | 3 | `100`, on `SHIP` and `SHIPL` |

`name` is **not** an identity — 52 names are claimed by more than one file (`Tower`, `Wall`,
`Catapult_Placing`, …) and twelve entities have none at all. Entities are addressed by their pack
path, which is what a class's `entity=` attribute carries. Treat `name` as a comment.

`type` is a loose editor tag rather than a discriminator: it does not drive the schema (a
`vx/unit` and a `vx/building` entity have identical structure), and it does not agree with the
class's `cpp_class` — `BTaran`, a battering ram, is `vx/unit` inside `Buildings.pak`. `gaul
building` (one file) and `vx/animal` (one file) are one-off typos of `vx/building` and `vx/unit`.
It does correlate with which `<point>` types appear (see below), so the engine probably keys some
footprint logic off it.

`radius` / `selection_radius` here are **per-entity overrides of the class properties of the same
name**. 120 of the 124 uses set both to `0`. Which wins when the class also sets them is
**unknown**; the class value is set far more often and is the one balance data varies, so the
entity value is most likely a default the class overrides rather than the reverse.

`pass_file` names a passability mask (see [`pass.md`](pass.md)) relative to the entity's own
directory. This link is **the entity's only footprint information** — the mask is an irregular
bitmap stamp that may contain interior holes, and nothing in this file records its extent.

The extension is frequently omitted: of the 597 masks in the packs, 51 are stored under the bare
name `PASS` with no extension at all, which is what `pass_file="pass"` refers to. Resolution must
try both the literal name and the name with `.PASS` appended. Note also that the engine matches
masks by *content*, not by filename, so a reference that dangles by name may still be satisfied at
runtime — the 16 dangling `pass_file` references counted below are name-level misses.

### `variations`

The number of distinct renderings of the same entity — for a unit, the number of **facing
directions**; for a static prop, the number of interchangeable random looks.

The corpus shows `variations` is the same number as the sprite sheet's `columns` in 867 of 889
entities:

| Entities | `variations` | `columns` observed on its images |
|---------:|-------------:|----------------------------------|
| 715 | 1 | `1` |
| 103 | 8 | `8` |
| 43 | 12 | `12` |
| 12 | 1 | `6` |
| 5 | 8 | `1` and `8` |
| 5 | 32 | `32` |
| 2 | 12 | `4` and `12` |
| 1 | 36 | `36` |
| 2 | 1 | (no images) |

So **`variations` selects the sprite-grid column**, and the 8-direction / 12-direction split is
the game's two facing granularities (8 for infantry, 12 for cavalry and large units; 32 and 36
appear only on a handful of ships and wheels). The mismatched rows — 12 entities with
`variations=1` but `columns=6`, and the mixed-column cases — mean the column count is a per-image
property and `variations` is the entity-wide default, not a hard constraint.

## `<image>` — the sprite sheets

```xml
<image idx="1" file="Attack.rle" drawmode="player_color" remaping="none" rows="15" columns="8"/>
```

All six attributes are present on all 4,034 images.

| Attribute | Domain |
|-----------|--------|
| `idx` | 1..25, unique within the entity; the handle `<layer>` and `<replace>` refer to |
| `file` | a `.rle` name, 1,288 distinct |
| `drawmode` | `player_color` 2,005, `shadow` 1,228, `normal` 589, `index` 162, `clouds` 49, `playercol` 1 |
| `remaping` | `none` 3,816, `reverse` 135, `pingpong` 83 |
| `rows` | 1..32 |
| `columns` | `1` 2,152, `8` 1,311, `12` 538, `32` 16, `6` 12, `4` 4, `36` 1 |

### `file` resolution

The name is relative to the directory holding the `.ENT.XML`, and the extension is frequently
omitted (`file="Building"`, `file="tobj23"`). In the packs the pixel data is stored as
`.RLE.MMP`. Resolution must try, in order:

```
<dir>\<file>            <dir>\<file>.MMP            <dir>\<file>.RLE.MMP
```

With that rule, 4,033 of 4,034 references resolve. The one failure is
`UI\DEBUG\DESTLOCK.ENT.XML` referencing a `destlock` sheet that was never shipped.

### The row/column grid

An `.rle` sheet is a **`rows` × `columns` grid of frames**. The convention throughout the corpus:

- **column = variation** (facing direction, or random look),
- **row = animation frame**.

`BBowman`'s `walk.rle` is `rows="14" columns="8"` — fourteen steps of the walk cycle in eight
facings. Its `<anim idx="1" name="walk" frames="16">` plays those 14 rows (see below).

**Read the grid from the frame table, not from here.** 430 of the 4,033 resolvable `<image>`
declarations state a `rows` or `columns` that the corresponding `.rle.mmp` frame table
contradicts, and `drawmode` disagrees in hundreds more. The XML values are a stale authoring-time
copy; the sheet is authoritative. Keep the XML values only as a cross-check.

`drawmode` names the blit path:

| Value | Images | Reading |
|-------|-------:|---------|
| `player_color` | 2,005 | the sheet participates in team colouring. Used for every unit and most building layers. |
| `shadow` | 1,228 | drawn as a flat shadow, not coloured. Always paired with a `player_color`/`normal` sheet of the same dimensions. |
| `normal` | 589 | drawn with the sprite's own palette. |
| `index` | 162 | **meaning unknown.** Possibly a raw-palette-index blit. |
| `clouds` | 49 | all on `Visuals.pak` effects and fog. Presumably an additive/translucent blend. |
| `playercol` | 1 | a typo for `player_color`. |

**Team colouring is not driven from this file.** It is a palette-index swap over indices 0–63
inside the sprite data itself; `drawmode="player_color"` only flags that the sheet takes part.
See [`rle.md`](rle.md) for the mechanism — and note that the entity XML's `drawmode` disagrees
with the sheet in hundreds of cases, so the sheet decides here too.

### `remaping` is animation sequencing, not palette remapping

Despite the name (and the game's spelling), `remaping` has nothing to do with colour. It is the
**row playback order** when a sheet is animated:

| Value | Images | Order | Where used |
|-------|-------:|-------|------------|
| `none` | 3,816 | 1 → N | everything ordinary |
| `reverse` | 135 | N → 1 | rotating catapults and ballistae (`Rotation.rle`, `Catapult_Rotation`) |
| `pingpong` | 83 | 1 → N → 1 | fires (`TempleFire1.rle`, `EBlacksmithFire.rle`), swaying trees (`3LTREES\TR*`), crops |

The pairing is consistent: every `reverse` sheet is a rotation, every `pingpong` sheet is a loop
that must not snap back. The three values are also independent of `drawmode` — `reverse` and
`pingpong` both occur on `player_color`, `shadow`, `normal`, `index` and `clouds` sheets alike,
which is itself evidence that the attribute is not a colour operation.

## `<point>` — attachment and footprint markers

```xml
<point idx="1" type="2" x="-70" y="74"/>
```

All four attributes always present. `idx` is 1..41 and unique within the entity; `x` and `y` are
signed pixel offsets from the entity origin, ranging −450..454 and −404..391.

`type` is a small integer with 16 observed values. It is clearly a **role tag** — the same entity
often has several points of one type (`EBarracks` has five `type="16"` points). Its distribution
is strongly correlated with the entity's `type`, which is the main evidence available:

| `point/@type` | Total | Occurs on |
|--------------:|------:|-----------|
| 4 | 796 | `vx/building` 396, untyped 400 |
| 2 | 499 | buildings 322, untyped 156, units 16, trees 5 |
| 3 | 489 | buildings 361, untyped 88, units 40 |
| 10 | 272 | buildings 207, untyped 52, trees 13 |
| 11 | 264 | untyped 168, buildings 73, trees 23 |
| 7 | 258 | buildings 241, untyped 16, units 1 |
| 1 | 247 | untyped 144, buildings 82, units 16, trees 5 |
| 16 | 155 | buildings **only** |
| 6 | 132 | untyped 112, buildings 20 |
| 17 | 130 | buildings 118, untyped 12 |
| 12 | 122 | buildings 68, untyped 54 |
| 8 | 116 | buildings 115, untyped 1 |
| 5 | 54 | untyped **only** |
| 9 | 25 | trees 9, untyped 12, buildings 4 |
| 13 | 24 | untyped 18, buildings 6 |
| 14 | 24 | untyped 18, buildings 6 |

Type 15 is never used; the numbering is not contiguous.

**Four of the sixteen point types are named**, and the host API is where they
came from — which is what the note below predicted. `Building::GetPoint(type, n)`
(`gbr.exe` 0x004dd150) walks a 64-entry table of `{x, y, type}` triples on the
class's visual record — count at `+0x338`, entries from `+0x33c`, a bitmask of
which types are present at `+0x63c` — and returns the *n*th entry of the type
asked for, or the `(-32768, …)` sentinel. Four registered entry points are that
function with a constant:

| type | entry point |
|---:|---|
| 12 | `Building::GetEnterExit()` (0x005c54d0) |
| 13 | `Building::GetWaterSource()` (0x005c5540) |
| 14 | `Building::GetGoodsSource()` (0x005c55b0) |
| 15 | `Building::GetFixSite()` (0x005c5620) |

The stored `x`/`y` are **screen offsets, not world ones**: 0x005c11a0 converts
the owning object's world position into screen space as
`(pos.x, (pos.y * 181) >> 8 - terrain_height(pos))` before adding the offset,
and 181/256 is 0.707 — the isometric y-scale.

**That does not mean reading one back needs the inverse.** `Building::GetPoint`
does not project at all: it returns `x` verbatim and `y` scaled by **1448/1024**,
as a *relative* offset for the caller to add to the object's own position. The
height term cancels because an offset is a difference — the point and the object
that owns it stand on the same ground — so the inverse of the projection is one
multiplication. Every shipped caller adds the position back
(`aa1 = .pos + .GetPoint(3, inds[0])` in `GATE_PATROL.VS`), and `SENTRY_GUARD.VS`
uses the result as a *direction*, which is consistent only with a relative
vector. The four constant-type entry points above are the ones that want the
point snapped onto the ground it will be stood on, and those do need the
height-aware round trip.

**Three more are named by the executable's own reader**, and they are the ones
that keep units out of doorways. The map-object loader (`0x00540a20`) ends by
walking the same table (`0x00540780`) and making an ownerless destination lock —
a circle no unit's cooperative step may give way into — for each point of these
types, after the same screen-offset round trip:

| type | makes |
|---:|---|
| 9 | one lock of radius **20** |
| 10 | one lock of radius **40** — `RAM_ATTACK.VS`'s comment calls this type `etDestLock2` |
| 11 | **six** locks of radius 40, on a ring of radius 40 at steps of `1.0466666…` radians (the double at `0x007c2498`, `3.14 / 3`), each axis truncated |

Trees carry them as well as buildings (see the census above), which is what makes a
wood something a column walks round rather than through. `sim/avoidance.hpp` has the
mechanism; the locks are not made while a saved game is restored, because the save
carries them.

**Six more types are named from the corpus**, by what asks for them. This is
weaker evidence than a registered entry point — it is what the scripts do with
the answer — and it is marked as such:

| type | asked for by | evidently |
|---:|---|---|
| 1 | `RAM_ATTACK.VS`, walked to exhaustion as a last resort | enter points; agrees with the enter/exit baker |
| 2 | `GATE_PATROL.VS`, exactly two: `(2,0)` and `(2,1)` | the two ends of a gate's line |
| 3 | `GATE_PATROL.VS`, four, permuted into a patrol path | patrol stations around a gate |
| 8 | `SENTRY_GUARD.VS`, one, used as a **direction** | a facing marker |
| 10 | `RAM_ATTACK.VS`, three: `(10,0..2)` | ram impact points |
| 17 | `RAM_ATTACK.VS`, guarded by `IsHeirOf("Gate")` | gate hinge or breach points |

**The meaning of the remaining five is unknown**: 4, 5, 6, 7 and 16. What the data supports: type 16 is
building-exclusive and plausibly the sentry slots (`EBarracks` has five type-16 points and a
class `num_sentry_slots` of 4–5); type 5 never appears on a building at all. Confirming any
of this needs runtime observation. Do not guess in the loader — carry `(idx, type, x, y)`
through verbatim.

**One reader detail that matters to a loader.** `Building::GetPoint` and the enter/exit baker
walk all **64** table slots rather than stopping at the `+0x338` count, so the slots past the
count must be zero-filled — they read as type 0 at `(0, 0)`. No shipped script asks for
type 0, so a table holding only the authored points answers identically everywhere the
content goes.

## `<layer>` — the draw stack

```xml
<layer idx="1" name="unit"        image="1" z="1000" offsetx="-93" offsety="-114"
       sortoffsetx="0" sortoffsety="0" xray="0"/>
<layer idx="2" name="unit_shadow" image="2" z="800"  offsetx="-93" offsety="-114"
       sortoffsetx="0" sortoffsety="0" xray="0"/>
```

| Attribute | Count | Required | Domain |
|-----------|------:|----------|--------|
| `idx` | 2,430 | yes | 1..25, unique within the entity |
| `name` | 2,430 | yes | 131 distinct; `shadow` 611, `layer1` 324, `layer2` 185, `base` 181, `styblo` 171, `unit` 156, … |
| `image` | 2,430 | yes | an `<image idx>` |
| `z` | 2,430 | yes | 18 values: `1000` 1,478, `800` 667, `500` 114, `1500` 44, `20` 25, `900` 22, and singles |
| `offsetx` | 2,430 | yes | int −938..116 |
| `offsety` | 2,430 | yes | int −809..76 |
| `sortoffsetx` | 2,264 | no | int −310..367 |
| `sortoffsety` | 2,264 | no | int −243..266 |
| `xray` | 1,584 | no | `0` 1,432, `1` 148, `226` ×4 |
| `nohighlight` | 126 | no | `1` 112, `0` 14 |
| `percent` | 13 | no | `50`, `100`, `-90` |

Several layer names are Bulgarian, the developers' language: `styblo` (trunk), `korona` (crown),
`podlojka` (underlay), `naves` (canopy), `plashilo` (scarecrow), `kofi`/`nosi` (carry). They are
labels only.

`offsetx`/`offsety` position the sprite relative to the entity origin — always negative-ish
because they place the sheet's top-left corner given a centred origin. Every layer of a given
entity normally shares one offset, and `<state>` and `<replace>` repeat the same values.

**Every `layer/@image` in the corpus resolves to a declared `<image idx>`** — 0 dangling.

### `z` and the sort bins

`z` is not a plain painter's-algorithm depth. `DATA\ZBINS.XML` partitions the z range into bins
and flags each as sorted or not:

```xml
<zbins>
    <zbin startz="0"     sort="1"/>
    <zbin startz="750"   sort="0"/>
    <zbin startz="900"   sort="1"/>
    <zbin startz="1080"  sort="0"/>
    <zbin startz="10000" sort="1"/>
</zbins>
```

Mapping the observed `z` values onto those bins:

| `z` | Bin | `sort` | Typical layer |
|----:|-----|-------:|---------------|
| 20 | [0, 750) | 1 | `tobj` ground decals |
| 500 | [0, 750) | 1 | building base / rear layers |
| 800 | [750, 900) | 0 | **shadows** |
| 900, 950, 1000, 1001, 1050 | [900, 1080) | 1 | the main sprite |
| 1100, 1200, 1500, 2000 | [1080, 10000) | 0 | overlays, fog, lightning |

So the intended reading is: *layers inside a `sort="1"` bin are y-sorted against every other
object in the scene; layers inside a `sort="0"` bin are drawn in a fixed order within the bin.*
Shadows (z=800) and full-screen effects (z=1500) sit in unsorted bins, the sprite body (z=1000)
in a sorted one. This is an inference from the `ZBINS.XML` boundaries lining up exactly with the
observed `z` clusters — **`sort`'s precise semantics are unconfirmed.**

`sortoffsetx`/`sortoffsety` shift the point used for the y-sort without moving the sprite. Present
on 2,264 of 2,430 layers (93%). `EBarracks` uses `sortoffsety="80"` on two of four layers so the
building's upper storeys sort behind units that its base sorts in front of.

`xray` is a boolean flag on 1,584 layers (`1` on 148, mostly building roofs and tree crowns) — the
layer that fades or draws as a silhouette when a unit stands behind it. Four layers carry
`xray="226"`, which is not a boolean and is presumably corrupt.

`nohighlight="1"` on 112 layers excludes them from the selection highlight / outline pass — the
Outlines.pak silhouettes.

`percent` occurs on 13 layers only. `50` on every water reflection layer
(`SHIP\reflection`, `BOAT_WRECK\Boat_wreck_ref`, `FISH\unit`) and on `BIG_ARROW\shadow`; `100` on
three `fog` layers; `-90` on `DEATH_MAGIC2\Lightning`. Read as a **blend/alpha percentage**; the
negative value is unexplained.

## `<state>` — the poses

```xml
<state idx="1" name="idle" image_idx="0" image_row="1" offsetx="-93" offsety="-114"
       anim_idx="13" anim_frame="1"/>
```

A state is the resting configuration the entity holds when not mid-animation. Every entity has at
least one; most have one to four, one has five.

| Attribute | Count | Domain |
|-----------|------:|--------|
| `idx` | 1,822 | `1` 889, `2` 412, `3` 262, `4` 258, `5` 1 |
| `name` | 1,822 | 71 distinct; `idle` 614, `"2"` 259, `"3"` 259, `"0"` 197, `"1"` 184, `attack` 85, `fight` 45 |
| `offsetx` / `offsety` | 1,726 | int, plus a sentinel (below) |
| `image_row` | 1,695 | `0` 1,504, `1` 190, `7` ×1 |
| `image_idx` | 1,694 | `0` 1,665, `1` 17, `2` 11, `3` ×1 |
| `anim_idx` | 1,168 | `65536` 987, `5` 92, `13` 80, `17` 7, `18` ×1, `0` ×1 |
| `anim_frame` | 1,150 | `65536` 875, `1` 269, `2` 5, `3` ×1 |
| `sound_id` | 123 | always `""` |
| `anim_row` | 17 | `1` 13, `0` 4 |

Two idioms coexist:

- **Units** name their states (`idle`, `attack`) and point `anim_idx` at the looping animation to
  play while in that state — `BBowman`'s `idle` state has `anim_idx="13"` (its `idle` anim) and
  `attack` has `anim_idx="5"` (its `Attack` anim). `image_idx="0"` means "no still frame of my
  own; the layers keep the images they were given".
- **Buildings** number their states (`0`,`1`,`2`,`3`) with no animation at all
  (`anim_idx="65536"`), and the state index selects the construction/damage stage. `EBarracks`
  declares four such states and every one of its images has `rows="4"` — one row per stage.
- **Effects** set `image_idx` to a specific sheet and `image_row` to a specific row, giving a
  still pose. `VISUALS\SMOKE` has three states differing only in `image_idx`.

### `65536` is "none"

`anim_idx="65536"` (987 states) and `anim_frame="65536"` (875) are the sentinel for *no
animation*: 65536 is `0x10000`, i.e. the low 16 bits are zero in a value the engine treats as a
16.16 fixed-point or a packed index. Any loader must special-case it. **`state/@anim_idx` never
dangles** once 65536 is excluded, except in three states in two files (`ICATAPULT`,
`IMOUNTAINEER`).

### Uninitialised memory in the shipped data

Seven states carry `offsetx="-842150451"` and `offsety="-842150451"`. That is `0xCDCDCDCD`
interpreted as a signed 32-bit integer — the MSVC debug-heap fill pattern for uninitialised
memory. The exporter wrote out a struct field it never set. Four layers carry `xray="226"`, likely
the same class of bug.

A loader should clamp or ignore these rather than trusting them; the affected entities render
correctly in the retail game, so the engine evidently ignores the field for those states.

## `<anim>` — the animations

```xml
<anim idx="1" name="walk" startstate="1" endstate="1" frames="16" duration="924"
      default_duration="0" action_time="0" step="57">
    <replace layer="1" image="11" offsetx="-93" offsety="-114"/>
    <replace layer="2" image="12" offsetx="-93" offsety="-114"/>
    <frame idx="1"  duration="0"/>
    <frame idx="2"  duration="66"/>
    …
    <frame idx="16" duration="0"/>
</anim>
```

All eight `<anim>` attributes are present on all 1,168 animations; `floating` and `sound_id` are
occasional extras.

| Attribute | Count | Domain |
|-----------|------:|--------|
| `idx` | 1,168 | 1,2,3,5,9,13,14,17,18,19,20,21,22,23,24 — a **fixed slot number**, see below |
| `name` | 1,168 | 158 distinct, decorative |
| `startstate` | 1,168 | a `<state idx>`; `1` 931, `2` 235 |
| `endstate` | 1,168 | a `<state idx>`; `1` 926, `2` 238 |
| `frames` | 1,168 | 1..34 |
| `duration` | 1,168 | ms, 0..26,358 |
| `default_duration` | 1,168 | ms, 0..1,500 |
| `action_time` | 1,168 | ms, 0..1,950 |
| `step` | 1,168 | 0..400 |
| `sound_id` | 43 | always `""` |
| `floating` | 16 | `1` ×15, `0` ×1 |

`startstate` / `endstate` make the animation an edge in a state machine: `walk` runs 1→1,
`Attack` runs 2→2, `toattack` runs 1→2 and `toidle` runs 2→1. This is why units have exactly two
states and four transition animations.

`step` is the distance advanced per animation cycle in world units — non-zero only on locomotion
animations (`walk` is `step="57"`, `Attack` is `step="0"`). `action_time` is the offset within the
animation at which the gameplay effect fires: `BBowman`'s `Attack` has `duration="990"` and
`action_time="726"`, i.e. the arrow leaves at 73% through the swing. `default_duration` is
non-zero on 40 distinct values and is presumably the fallback when `duration` is 0; the
distinction is **unconfirmed**.

`floating="1"` appears on 15 anims (ships, `Eagle`, `Crow`), paired with the entity-level
`floating_turnspeed`. Presumably continuous heading interpolation rather than snapping to a
variation column.

### `idx` is a slot number, not a serial

The engine addresses animations **numerically**, from scripts: `.vs` code contains literal calls
like `PlayAnim(19, .pos)` and `Anim(17, tgt.pos)`. `idx` values are therefore a fixed, sparse
vocabulary and `name` is documentation. The corpus assignment:

| `idx` | Dominant name | Count | Reading |
|------:|---------------|------:|---------|
| 1 | `walk` | 144 (of 309) | locomotion; on non-units, "the animation" |
| 2, 3 | `anim2`, `anim3`, `fog2` | 10, 8 | extra generic loops |
| 5 | `attack` | 117 (of 142) | attack; also `defence`/`defend` |
| 9 | `die` | 147 (of 154) | death; also `destroy` on buildings |
| 13 | `idle` | 146 (of 147) | idle loop |
| 14 | — | 1 | second idle (`crow_idle_02`) |
| 17 | `heal` / `carry` | 28 | secondary action |
| 18 | `toidle` | 100 (of 127) | transition attack→idle |
| 19 | `toattack` | 100 (of 116) | transition idle→attack |
| 20 | `taunt` | 102 (of 107) | taunt / work |
| 21–24 | `defence`, `talk`, `work`, `rotating` | 4–6 each | per-entity extras |

Slots 4, 6, 7, 8, 10, 11, 12, 15, 16 are never used in these files — but `.vs` code calls
`PlayAnim(16, …)` and `PlayAnim(0, …)`, so the slot space is larger than the data exercises.
**The authoritative slot table is not in the data**; the mapping above is empirical.

### `<replace>` — the sheet swap

An animation does not have its own images; it **re-points existing layers at different sheets**
for its duration:

```xml
<replace layer="1" image="11" offsetx="-93" offsety="-114"/>
```

All four attributes always present. `layer` is a `<layer idx>` (2,195 uses, 1,156 of them layer 1
and 1,029 layer 2 — the body/shadow pair), `image` is an `<image idx>`. **Both resolve for every
one of the 2,195 `<replace>` elements in the corpus.** `offsetx`/`offsety` override the layer's
offsets while the animation runs, and in practice always repeat them unchanged.

### `<frame>` — the timing strip

```xml
<frame idx="1" duration="0"/>
<frame idx="2" duration="66"/>
```

Two attributes, both always present. `idx` is 1..34; `duration` is milliseconds, 0..14,000, with
66 ms (≈15 fps) overwhelmingly the common value.

The frame list is **bookended by two zero-duration sentinels**. In 1,114 of 1,168 animations the
first and last `<frame>` both have `duration="0"`, and

```
anim/@frames == replaced_image/@rows + 2
```

holds for 1,116 of the 1,168 animations that have a `<replace>`. So frames 2 … `rows+1` are the
`rows` real sprite rows, each held for its `duration`, and frames 1 and `rows+2` are entry/exit
markers of zero length. `BBowman`'s `walk`: `rows=14`, `frames=16`, fourteen 66 ms holds,
`duration = 14 × 66 = 924`.

`anim/@duration` should therefore equal the sum of the `<frame>` durations. It does in 1,041 of
1,168 animations; **127 disagree**, e.g. `BTEMPLEOFTHOR\TEMPLEFIRE1` declares `duration="1200"`
against a frame sum of 2,200. Which value the engine honours is unknown; the safe implementation
recomputes from the frames and treats `@duration` as advisory.

## Reference integrity

Measured across all 889 entities against the 14,203 entries of the retail packs:

| Reference | Count | Dangling |
|-----------|------:|---------:|
| `image/@file` → `.rle.mmp` | 4,034 | **1** (`UI\DEBUG\DESTLOCK.ENT.XML` → `destlock`) |
| `entity/@pass_file` → mask | 885 | 16 (by name; the engine matches by content) |
| `layer/@image` → `<image idx>` | 2,430 | **0** |
| `replace/@layer` → `<layer idx>` | 2,195 | **0** |
| `replace/@image` → `<image idx>` | 2,195 | **0** |
| `anim/@startstate`/`@endstate` → `<state idx>` | 2,336 | 2 (`UNITS\EPRIEST`) |
| `state/@anim_idx` → `<anim idx>` (excluding 65536) | 181 | 3 (`UNITS\ICATAPULT`, `UNITS\IMOUNTAINEER`) |

The 16 name-level misses are: `BUILDINGS\VILLAGE\TEST`, `UNITS\BDRUID`, `UNITS\EWVILLAGER`,
`MAPOBJECTS\DECORS\KAMANAK4\KAMANAK4_ASW`, and twelve of the `MAPOBJECTS\DECORS\SKELETONS`
props. Because masks are matched by content rather than filename, some of these may resolve at
runtime; the engine must in any case tolerate a missing passability mask (skeletons are
walk-through decor and plainly always were).

## What is still unknown

- **Why 430 `<image>` declarations contradict their frame table** on `rows`/`columns`, and
  hundreds more on `drawmode`. The frame table is authoritative so this costs nothing to
  implement, but it means the XML cannot be used to validate the sheets, and it is not known
  whether the divergence is systematic (an export that stopped being re-run) or per-file.
- **`point/@type`.** 16 values, and **four of them now have names** — 12, 13, 14 and 15 are
  the enter/exit, water source, goods source and fix site, read off the four `Building::Get*`
  entry points that are `GetPoint(type, 0)` with a constant. See the `<point>` section. The
  other twelve are still unnamed, and this is still the largest gap in the format: attachment
  points drive sentry placement, projectile origins, unit exits, smoke sources and gate hinges.
  The route that worked was the one this entry named — the `.VS` host API — so the remaining
  twelve are a matter of finding the entry points that read them, not of runtime observation.
  Reading any of them back as a *world* point additionally needs the inverse of the
  height-dependent screen projection at 0x005c11a0.
- **`drawmode="index"`.** 162 images. The other five values have a defensible reading; this one
  does not.
- **`percent` on `<layer>`.** Alpha is the obvious reading, but `-90` is not an alpha.
- **`default_duration` vs `duration`.** Both present on every anim; only one can drive playback.
- **The 127 animations whose `@duration` disagrees with their frame sum.**
- **The full animation slot table.** Fifteen slots are exercised by this data; `.vs` code calls
  at least slots 0 and 16 as well.
- **Whether entity-level `radius`/`selection_radius` override or are overridden by the class
  properties of the same name.**
- **`sound_id`.** Present on 123 states and 43 anims, empty in every single case. The mechanism
  it was meant to drive is unused in the retail data.
- **`anim_row` on `<state>`.** 17 uses, values `0`/`1`. No hypothesis.
- **`ZBINS.XML` `sort` semantics.** The bin boundaries align convincingly with the observed `z`
  clusters, but "y-sorted vs. fixed order" is an inference.
