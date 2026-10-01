# The saved game (`ISAV`)

**Status:** ours, not the original's — see the warning below
**Reference reader:** [`engine/core/src/sim/save.cpp`](../../engine/core/src/sim/save.cpp),
[`World::serialize`](../../engine/core/src/sim/world.cpp),
[`save_systems.cpp`](../../engine/core/src/sim/save_systems.cpp)
**Header:** [`engine/core/include/imperivm/core/sim/save.hpp`](../../engine/core/include/imperivm/core/sim/save.hpp)
**Session pair:** [`GameSession::save` / `GameSession::load`](../../engine/core/include/imperivm/core/sim/session.hpp)
**The file on disk:** [`core/sim/save_file.hpp`](../../engine/core/include/imperivm/core/sim/save_file.hpp), [`gamedata/save_file.hpp`](../../engine/gamedata/include/imperivm/gamedata/save_file.hpp)
**Harness:** [`engine/tools/imsave`](../../engine/tools/imsave.cpp), `imrun --load`

> **This is not the retail save format.** Every other document in this directory specifies a
> format the original engine wrote, decoded from shipped bytes. This one specifies a format
> *we* write. The retail installation ships no saved game to decode — `Adventures/`,
> `Scenarios/` and `Conquests/` hold maps, and `currentadv.bfhp` is adventure progress, not a
> session — so there is nothing to reverse. What `gbr.exe` does give us is a **checklist**: the
> field names its persist stream registers, which say what the original considered part of a
> saved game. Those are quoted throughout, and the section
> [What the original saved](#what-the-original-saved) collects them. They constrain *what* a
> save must contain. They say nothing about the byte layout below, which is our own.

A save is a small container of named sections. It is written by
`imperivm::core::sim::write_save` and read by `SaveReader` / `read_save`, both over byte spans:
`engine/core` cannot open a file, so putting the result inside a [`.bfhp`](bfhp.md) container —
which is where the original kept its saves — is the app's job.

## Why a container of sections and not one blob

Three of the four pieces already existed before this format did:

- the [`.bfhp`](bfhp.md) block container reads and writes byte-identically (23 of 23 retail
  containers rebuild bit for bit), and the original stored saved games in it;
- `script::Execution` already round-trips one suspended coroutine, and
  `script::Scheduler::serialize` already round-trips the whole set of them, with a magic and a
  version;
- `sim::SelectionTable` already has a serialise pair.

What was missing was the statement of **what a saved game contains**. So this format is a
frame around the pieces rather than a rewrite of them, and the section table is the extension
point: a subsystem that grows a serialise pair adds a section and nothing in `save.cpp`
changes.

## Layout

All integers little-endian, unsigned unless noted. Offsets absolute from the start of the save.

```
offset  size  field
------  ----  -----------------------------------------------------------
0x00     4    magic          "ISAV"
0x04     4    format_version u32   the framing; 1
0x08     4    state_vector   u32   the meaning of the section bytes; 17
0x0C     4    section_count  u32   >= 1; the meta section is always the first
0x10     ..   sections, laid end to end
```

Each section:

```
size  field
----  -------------------------------------------------------------------
 1    name_length   u8    1..255
 n    name                printable ASCII (0x20..0x7E), unique within a save
 4    payload_length u32  <= 0x20000000
 m    payload
```

There is no offset table. Sections are a stream, walked once — the same shape
[`pak.md`](pak.md)'s name table has, and for the same reason: a table of offsets is a second
copy of the framing that can disagree with the first. A reader that wants one section walks to
it; `SaveReader` walks once and keeps the spans.

A save whose sections do not account for exactly every byte is refused. So is a duplicate
section name, a name outside printable ASCII, and a meta section that is not first.

### Two version numbers, two meanings

`format_version` is the framing above. `state_vector` is **the meaning of the bytes inside
every section**, including a section owned by a system, and it is the one that will move. Bump
`kStateVectorVersion` whenever a field is added to, removed from, or reinterpreted in any
section. Both mismatches are refused as `FormatError::unsupported` at the header, before a
single object is decoded.

It has moved to **2**, when the world section grew the `ObjList` pool and all eleven systems
grew sections of their own; a version-1 save described a strictly smaller game and there is no
honest way to widen one, so it is refused. **4** was the hero section growing
`army_attacked_at` and `army_attacked_unit`, when a hero's army was last struck and which
member, which `Hero::TimePastLastAttack` reads. **5** was the selection section below. It
**6** was two fields on the object header: `ObjectState::ui_target`, the player's manual
target for a tower, and `ObjectFlags::summoning_death`, which marks a druid's death as a
transformation rather than a casualty. **7** was `Squad::eval`, the squad's stored strength. **8** was the teleport pair and its
traversal mask on the object header. **9** was the five per-player score counters in the match
section — the four `CombatSystem` credits on every blow it lands, and the gold counter nothing
maintains yet. **10** was `UnitRecord::kills`, the tally `Unit::IncKills` writes. **11** was
`Squad::last_attacker` — whoever last struck a member of a squad, stamped beside
`last_fight_time` and saved with it. **12** to **16** are logged beside `kStateVectorVersion`
in `sim/save.hpp`, which is the one place every bump is recorded and the place this paragraph
copies from. **17** was the `session` section below — the first section that belongs to
neither the world nor a system, and the one whose absence no hash could report — and **18**
the match section's report counters. **19** was the movement section's avoidance state (see
the paragraph under the section table), and **20** the AI's nodes in the session section.
**21**: a settlement's timer array lost the two timers that were the economy's
copies of `TOWNHALL_SENTRIES_CONTROL.VS` and `VILLAGE_BEHAVIOR_SUPPLY.VS`, which run as the
town hall's and the village's class behaviours instead. The class behaviours themselves added
nothing to the format: they are coroutines owned by their object, and the script section
already carries every coroutine with its owner. It stands at **22** now: a script value that
is an object is written as its type (u16), its id (u32) and a second word (u32), because a VS
point is two 32-bit integers — `x` in the id, `y` in the second word — where it had been two
sixteen-bit halves of the id (the coroutine payload's own version went from 3 to 4).

The original did exactly this and said so: `gbr.exe` carries
`Savegame version of this build: %d`, `Last savegame loaded version: %d` and, for the replay
stream, *"This replay is in invalid format and cannot be loaded. It has probably been saved
with a different version of the game!"*.

### `meta`

Always the first section. It exists so that a save browser can list a save, and a loader can
refuse one, without decoding an object table.

```
    string  map            the map identity the writer supplied; may be empty
    u64     turns          Clock::turns()
    i64     time           Clock::time()
    u64     slots          ] World::hashes() at the moment of the save
    u64     threads        ]
    u64     netcmds        ]
    u64     extrahash      ]
    u32     system_count
    string  system_name    x system_count, in registration order
```

Strings are a u32 length followed by that many bytes; not NUL-terminated, because a name with
an embedded zero is hostile input rather than a truncation point.

The four hash channels that are zero in all nine retail desync dumps — `pathfinder`,
`exploration`, `scriptstate`, `aihash` — are **not encoded**, for the reason
`conformance::write_trace` gives for the same omission: a field whose only legal value is zero
is an invitation. `hash_of_hashes` is not encoded either, being a pure function of the four
that are.

`map` is checked against `LoadOptions::map` when the caller supplies one. The original refused
the same way, with its own message: *"The map you played this game on when the game was saved
(%s1) is missing or has been changed"*.

### `world`

**`World::deferred_erase` is deliberately absent, and so is the latch it models.** `Erase` on the
object whose script is running sets a bit (`gbr.exe`'s `[0x009bdb14]`) that the runner consumes the
moment that script's slice ends. It is armed and drained inside one script step, so no save and no
between-turn hash can see it set; there is nothing to carry and nothing a loader could disagree
about.

`World::serialize`. Its own magic `"IWLD"` and version, in the shape
`Scheduler::serialize` established, so the section is self-describing even outside the
envelope.

```
    u32     magic          "IWLD"
    u32     version        23

    -- the clock (all of it is world state; sim/tick.hpp) --
    i32     turn_length    TickConfig::turn_length: the length the NEXT turn takes
    i32     game_speed     TickConfig::game_speed, the CONST.INI per-mille scale
    i64     time           Clock::time(): the running sum of declared lengths
    u64     turn_index     Clock::turns(); the dumps' `Tick` is this plus one
    i32     turn_length    the length the LAST turn actually ran at

    -- the seeds, which the dumps' [SEEDS] block records --
    u32     syncseed       Rng::state()
    u32     cmdidseed      World::next_command_id()
    u32     next_id        the next handle; never reused, so this is state

    -- the object table, in spawn order, which is tick order --
    u32     object_count
    per object:
      u32   id             strictly ascending; a table out of order is refused
      u8    internal       InternalKind
      u8    native_class   NativeClass, or 0xFF for a slot with no native object
      u32   class_index    ClassIndex, or 0xFFFFFFFF
      u32   settlement     the settlement this object belongs to
      i32   sight          resolved from the class at spawn; see below
      u32   query          index into the query table, for InternalKind::query
      i32   position.x
      i32   position.y
      u8    owner          PlayerId, 0xFF for none
      u32   flags          bit 0 is_unit, 1 is_building, 2 is_hero,
                           3 has_active_path, 4 hidden, 5 in_party,
                           6 unspawned, 7 no_ai, 8 built, 9 in_air,
                           10 noselect, 11 building, 12 autocast,
                           13 on_minimap, 14 enemies_near, 15 friends_near,
                           16 gate_open, 17 messenger, 18 landing,
                           19 cursed; any other bit is
                           refused. Two bytes until `gate_open` made it
                           seventeen. Bit 17 is on the **`+0x2c`** word in
                           `gbr.exe`, where 9 to 16 are on `+0x194`; here they
                           are one word because this engine has one.
                           A byte until `built` made it nine bits.
                           Bit 18 is `Flying::IsLanding`'s half of a two-bit
                           state machine: `[obj+0x1d4]`, written by every
                           `Flying::PlayAnim` as `z == -1`, and `IsLanding` is
                           it **and** `in_air`. Version 16.
                           Bit 19 is `Unit::IsCursed` -- bit 27 of the same
                           `[obj+0x194]` word. `SHAMAN_IDLE.VS` is the whole
                           corpus for it and round-trips it itself; where the
                           *original* writes it is not established. Version 17.
                           Bits 9 to 11 are read by `Flying::IsInAir`,
                           `Unit::SetNoselectFlag` and `Ship::IsBuilding`, and
                           two of the three are on `gbr.exe`'s **second** flag
                           word `[obj+0x194]` rather than the `+0x2c`
                           `SyncFlags` one -- which is why `noselect`'s mask
                           there is the same `0x00800000` the Building bit uses
                           here.
      u32   holder         kNoObject, or the object this one is inside
      i32   health
      i32   stamina
      i32   user           `Unit::user`, the script scratch int. Version 15.
                           Nothing in the engine reads it; it is saved and
                           hashed because scripts steer on it -- a crow that
                           reloaded having forgotten it had decided to land
                           would rejoin the flock going the wrong way.
      i32   z_from         `Flying::z` interpolates from here. Version 16.
      i32   z_to           ...to here, over the running animation's clock, in
                           the height layer's own 0..255 units. `[obj+0x1cc]`
                           and `[obj+0x1d0]`, written only by
                           `Flying::PlayAnim` -- `z_from` from `Flying::z`
                           itself, so each animation starts where the last one
                           left off. A bird has no stored altitude and these
                           two are what stands in for one.
      -- one more bit joined `flags` above at version 21: `entering`, which is
      -- `Unit::SetEntering`'s bit 20 of `[obj+0x194]`. Write-only from script
      -- and stored for the reason `noselect` beside it is. Version 22 added
      -- `summoning_death`: `Druid::SetSummoningDeath`'s `[obj+0x1d4]`, which
      -- marks a death as a transformation rather than a casualty. Unlike
      -- `entering` it is read back, by `IsSummoningDeath`.
      i32   parry_mode     `Unit::GetParryMode` -- `[unit+0x1a0]`, 0 or 1.
                           Version 20. Both the setter and the getter are gated
                           on the class carrying the `Parry` special (bit 0 of
                           the 64-bit mask at `[unit+0x198]`), so a unit that
                           cannot parry reads 0 whatever this holds; the field
                           is saved anyway because the gate is the class's and
                           the value is the object's.
      i32   damage_state   `Building::IsBroken` -- `[building+0x204]`, a tier
                           from 0 to 3. Version 20. **State rather than a
                           derived value, because it has hysteresis**: the tier
                           depends on the tier before it, so two buildings at
                           the same health can legitimately differ. The four
                           `[GamePlay]` keys that produce it are configuration
                           and are not saved. `IsVeryBroken` is *not* this
                           field: that one is `health < maxhealth / 10`,
                           computed on the spot.
      u32   ui_target      `Building::GetUITarget` -- the *player's* manual
                           target for a tower, `[building+0x1fc]`. Version 22.
                           Written by the interface and read by the simulation:
                           `CATAPULT_TOWER_GUARD.VS` re-reads it inside its
                           volley loop, which is what lets a player retarget a
                           tower mid-shot. Hashed for that reason -- what a
                           tower fires at is not something peers may differ on.
      u32   teleport_dest  `Teleport::destination` -- the far end of a teleport
                           pair, `[teleport+0x208]`. Version 23. **The map
                           authors it**, through the paired teleport's
                           *settlement*: a teleport's `<scriptobj>` carries
                           `destination_set`, naming that settlement by its `id`
                           attribute rather than its index. 54 teleports ship
                           across 28 maps; every one is inside a settlement, no
                           settlement holds two, and the relation is symmetric
                           in 54 of 54 -- see `docs/formats/map.md`.
                           Saved rather than rebuilt on load, because nothing
                           after load keeps the map's settlement id table, and
                           for `sight`'s reason: a rebuild would make the file's
                           meaning depend on the map data staying put.
      u32   traversed_by   `Teleport::Traverse(player)` -- a 16-bit-wide mask of
                           which players have been through this teleport,
                           `[teleport+0x214]`, bit `player - 1`. Version 23.
                           The entry point moves nothing; the six shipped sites
                           move the unit themselves and then call it on **both**
                           ends of the pair. What reads the mask is not settled;
                           `FindTeleport` is the likely consumer.
      u8    animating
      u8    repeat         AnimRepeat
      -- only when native_class != 0xFF --
      i32   anim.state_idx
      i32   anim.anim_slot
      i32   anim.elapsed_ms
      u32   anim.step
      u32   anim.variation

    -- query specs, by index, written whole including dead slots --
    u32     query_count
    per spec: u8 kind, u32 subject, i32 center.x, i32 center.y, i32 radius,
              i32 left, i32 top, i32 right, i32 bottom, u8 area_is_rect,
              u8 settlement_scope,
              u8 player, i32 flags_type, i32 group, u8 visible_only,
              u32 lhs, u32 rhs, u8 op, u8 filter.match_all, u8 filter.count,
              u32 filter.classes[count]

    -- the sixteen players --
    u32     player_count   must equal 16
    per player: string name, string race, string allowed_races, u8 control,
                i32 difficulty, u16 colour, i32 start.x, i32 start.y,
                i32 bonus, u8 allied_flag, string ai_script
    u32     relation[16][16]   the relation words, row-major from..to

    -- the two name tables, in index order --
    u32     group_count
    per group: string name, u32 member_count, u32 member_id[member_count]
    u32     named_count
    per name: string name, u32 object

    -- the system run order: a manifest, not state --
    u32     system_count
    string  system_name x system_count

    -- the script-owned collections, length-prefixed --
    u32     objlist_bytes
    u32     magic          "IOBL"
    u32     version        2
    u32     entry_count    every slot, dead ones included
    per entry: u32 script, u32 slot, u8 live, u32 alias, u32 item_count,
               u32 item_id[item_count]

    u32     array_bytes
    ... the `IntArray`/`StrArray` pool, "ARRY" version 1 ...

    u32     squadlist_bytes
    u32     magic          "SLST"
    u32     version        1
    u32     entry_count    every slot, dead ones included
    per entry: u32 script, u32 slot, u8 live, u32 cursor, u32 member_count,
               (i32 index, u8 player) x member_count

    u32     conversation_bytes
    u32     magic          "CONV"
    u32     version        1
    u32     entry_count    every slot, dead ones included
    per entry: u32 script, u32 slot, u8 live, string name, u32 actor_count,
               (string role, u32 object, string fallback_class) x actor_count
```

Five of those deserve their reasons written down.

- **The clock stores five numbers, not eight.** `Turn::start`, `Turn::end` and `Turn::time` are
  pure functions of the time a turn began at and its length — `gametimetickstart = gametime + 1`
  and `gametimetickend = start + length` hold exactly in all nine dumps — so they are derived on
  load rather than stored. The derivation lives in one place, `restore_clock` in
  `world.cpp`, which builds a `Turn` and hands it to
  [`Clock::restore(GameTime, const Turn&)`](../../engine/core/include/imperivm/core/sim/tick.hpp).
  That method is the **one operation in the engine that moves game time backwards**, and the save
  is the only reason it exists: nothing in a running simulation may rewind, which is why `time_`
  has no setter, but a load is not a running simulation. The turn has to come back as well as the
  time, because **turn length is not constant** — 200, 400, 799 and 800 all occur and two dumps
  renegotiate it mid-session — so neither the index nor the last length is a function of the
  time. This used to be a loop that replayed `turn_index` turns whose lengths summed to `time`,
  inventing an intermediate schedule that never happened; the restore is now O(1) and invents
  nothing.
- **`sight` is written although nothing but the spawn ever writes it.** Recomputing it from the
  class graph on load would make a save's meaning depend on a `sight` property in the game data
  staying put, which is the silent drift a versioned save exists to prevent.
- **The player table is written although `state_hash` does not cover it.** Combat cannot pick a
  target and the economy cannot decide who a tribute may go to without the relation matrix, so a
  save that lost it would diverge on the first fight rather than at the load. This is the
  general rule: *hashed* and *saved* are different sets, and saved is the larger one.
- **The run order is a manifest, not state.** Systems own their own sections. It is here so a
  load into a differently-ordered pipeline is refused by name, because run order is folded into
  `slots` ahead of every system's contribution and would explain every hash difference under it.
- **The `ObjList` pool is written although nothing hashes it.** `scriptstate` is zero in all nine
  dumps, so the pool is deliberately outside the determinism contract — and a suspended script's
  local slot can hold an `ObjList` handle, which the shipped AI does constantly
  (`ol = Group("GoldMules" + idPlayer).GetObjList()` and hundreds of sites like it). A load that
  dropped the pool would hand every one of them an empty list and **no hash would notice**. Dead
  slots are written too: `ObjListPool::emplace` reuses the lowest dead index before it grows, so
  where the holes are decides which handle the next acquire returns, and a suspended script's
  handle refers to that numbering.
- **The `IntArray`/`StrArray` pool is written on exactly the same terms**, and for exactly the
  same reason: it is a script's own storage, `scriptstate` is zero in all nine dumps, and a
  suspended script's local slot can hold an array handle. Forty declarations across 38 shipped
  files, every one of them written past its own end on the line after it is declared. Dead slots
  are written for the same reason as the list pool's. Nothing hashes it and therefore everything
  depends on the save carrying it.

- **The `SquadList` pool is the third on those terms, and it carries a cursor.** A `SquadList`
  is a list *and* a position in it: `Cur`, `Next`, `EOL` and `Rewind` are the whole of how the
  AI scripts read one, and the position lives in the list object rather than in the caller.
  `DATA\AI\GS_GUARD.VS` sleeps in the loop above its walk, so a save can be taken part way
  through one — and a load that rewound would order every squad in the list a second time. The
  cursor is clamped to the member count on read *and* on load, because a cursor past the end is
  what "walked to the end" already means. Version 18 added the section.

### `script`

`script::Scheduler::serialize`, unchanged. Scripts are stored **by source name**, not by chunk
index: the library is rebuilt from `data.pak` on load, and a rebuild that disagrees must be
caught rather than silently resumed against the wrong script. The original does the same and
fails the same way — `sequence from savegame does not compile : %s`.

### `selection`

`sim::SelectionTable::serialize`, at its own version **2**.

```
    u32     magic          "ISEL"
    u32     version        2
    u32     players        16, and a file that says otherwise is refused
    per player, ascending:
      u32   count
      u32   id             count of them, in selection order
    u32     stamped
    per stamp, ascending by object:
      u32   object
      i64   when           game time this object was last in a selection that changed
```

Version 1 carried a per-player *latch* instead of the stamps — one bit meaning "this player has
had something selected at some point", which is what `WasSelectionAssigned` was thought to
answer. It is not: that entry point reads the environment key
`/Player<n>/SelectionAssigned`, and the bit is set by the input layer when the player assigns a
numbered control group, not by anything about the selection. So the latch went, the stamps
`Obj::_LastSelectionTime` reads arrived, and a version-1 file is refused rather than widened —
it records a different set of facts. See `sim/orders.hpp`.

The stamps must arrive ascending and unique; a file whose stamps are out of order is
`malformed` rather than quietly sorted. Ordering is what every read of the table relies on, and
a save is not a promise.

### `session`

`GameSession`'s own bookkeeping, at its own version **3**. One number, the layer the grid
was built from, and the templates laid.

```
    u32     magic          "ISES"
    u32     version        3
    u32     script_watermark
    u32     layer_size         bytes that follow; 0 when the map's own layer is in use
    u8[]    layer              a GRID container: `Terrain.pass.grid` as rebuilt
    u32     stamp_count        templates laid at match start, in the order they landed
    struct  stamps[]           u32 template index into RandomMapSettlements, i32 x, i32 y
```

`stamps` is what a load needs to lay the templates' ground again: each template copies its
own terrain onto the map, levels the height under it and takes out the shore where it
landed (`sim/mutable_settlement.hpp`, `stamp_template_ground`), and those layers are not
otherwise in the save -- the terrain, height and decoration layers a session plays on are
the map's plus that ground. The load lays them before the passability layer goes in and
before any system is restored, in the saved order, which is the order that decides where
the slope limiter's marks fall.

`layer` is the passability layer `start_match` rebuilt over the world once the `Mutable`
placeholders had become a race's towns — the original's own step (0x00552e95), and the reason
the grid a match is played on is not the layer the map ships: the templates' footprints are
in it and the placeholders' are gone (`docs/formats/pass.md`, "The map's passability
layer"). It is written as the `GRID` container it is, 128 KB for a 16,384-unit map, and it is
put back *before* the movement section, whose `grid_generation` would otherwise be bumped by
the new grid and send every restored route back to the pathfinder. A load that adopted the
shipped layer instead would route the restored side through the towns the running one walks
around — which the save sweep's per-turn comparison catches on the ten skirmish maps. Absent
(`layer_size` 0) when the rebuild did not run, which a session built without
`SessionInputs::masks` is; then the grid is the map's on both sides.

`script_watermark` is the first object id `GameSession::start_object_scripts` has not
considered yet. Object ids are never reused, so this one word is the exact record of which
objects have been offered their class's `idle` script, and it is what lets the per-turn sweep
pick up a `SpawnGroup` copy without keeping a bit per object. It is not hashed — a script's
existence is in no channel until it does something — and no system owns it, so it had no
section, and a load left it where `start_object_scripts` on the fresh session had put it: at
the *map's* `next_id()`. The first turn after the load then offered `idle` to every object
spawned since the match began. A trained unit walking into its town under an `enter` command,
whose own idle had been retired, got a second `UNIT_IDLE.VS`, and that script's `Stop(1000)`
dropped the route the original was still walking — the save sweep's Balcans and Crossroads
divergence, one turn after a save taken at turn 40. `test_group.cpp` holds it in place.

Required. A save without the section is refused as `malformed`, not defaulted: every save this
build writes has it, and the default it would take is the wrong answer above. A watermark past
`World::next_id()` is refused for the same reason — nothing this build writes can produce one.

### `system:<name>`

One per system, whose bytes the system produces. `System` has no `serialize` in its interface
and does not get one here: that would put a virtual on a seam eleven domains share, and
`sim/system.hpp` is not this format's to change. So the bytes arrive from whoever owns the
system and leave the same way — `SaveInputs::systems` going in, `SaveReader::system_section`
coming out, with `GameSession::save`/`GameSession::load` as the routing in between.

**A registered system with no section is not saved.** `write_save` reports it
(`SaveReport::unsaved_systems`) and `read_save` reports it again
(`LoadReport::unrestored_systems`) rather than producing a save that quietly reloads into a
different game. `GameSession::save` refuses outright if either list is non-empty, so the
report is a wiring assertion rather than a condition an embedder has to check.

All eleven systems in `kSystemOrder` now have a pair. Each section opens with its own
four-byte magic and a shared `kSectionVersion` (**24**, since the movement section grew
unit-versus-unit avoidance; **23** gave the match section's player rows the end-of-match
report's eight counters; **22** gave the fog section its two bits
a slot and the partial cells' fine records; **21** made the hero's skill-point balance derived
and took it out of the section; **20** gave the match section the setup's four rules; it read **7**
here for six bumps after the squad section gained `last_attacker`, and **13** for seven more,
which is what a number quoted in prose does — read the constant in `save_systems.cpp`);
the pairs are declared in each domain's header
and defined together in
[`engine/core/src/sim/save_systems.cpp`](../../engine/core/src/sim/save_systems.cpp), so that
what a save contains is one file to audit rather than eleven.

**Every section carries turn-mutable state and nothing that a load rebuilds anyway.** That is
the same rule `World::serialize` follows when it declines to write the class graph, and it is
what makes the contract of a load statable: *a save is applied to a session built from the same
game data.* `GameSession::load` says so and enforces what it can — the state-vector version,
the map identity, the system run order.

| section | carries | left to `GameSession::create` to rebuild |
|---|---|---|
| `movement` | the per-object `MoveState` table in id order, waypoints included — and with each row, the avoidance state: the route's stride, the hold and when it ends, the step being walked with its two sidestep offsets, `CVXPathCoop`'s wait, and the march it belongs to — then `grid_generation_`, and last the ownerless destination locks with the flag that says they were made | the obstruction grid, the formation table, the node budget, `PathFinder`, and the per-turn decision queue and bucket grids, which live only inside `advance` |
| `combat` | combatants — including `Unit::AddBonus`'s five per-object addends, which nothing but the entry point produces and so nothing could rebuild — projectiles in flight, the undrained event list, per-player level addends, alliance overrides, `now_`, `death_duration_`, `world_bound_` | `CombatConstants`, the class graph, the counter table and mode, the `PlayerTable` pointer, the lazily built `profiles_` cache |
| `economy` | the settlement store (nested `ISET`), wagons in transit, the wagon id counter, per-settlement statistics, the `start`ed flag | `EconomyRules`, `auto_deliver_` |
| `feeder` | the chain, the hungry list, `next_tick_`, both accumulators, both cursors | `FeederRules`, `world_bound_`, `draws_from_warehouse_` |
| `hero` | unit records, hero records — including `army_attacked_at` and `army_attacked_unit`, when this hero's army was last struck and which member — `now_`, the squad table (nested `ISQD`) and the item store (nested `IITM`) | `HeroConstants`, the borrowed `ItemCatalog*` |
| `env` | the environment store (nested `INVS`) and the AI-variable store (nested `IVAR`) | the research catalog, the unit catalog, the AI-variable name table, the `CONST.INI` constants |
| `command` | every object's queue in id order, and the default verb | the `CommandTable`, the scheduler pointer, `method_cache_` |
| `match` | the rules, the sixteen player rows — each its control, race, participation, outcome, and thirteen counters: the five score counters the team-score entry points read, and the eight the Statistics screen shows (food spent, units produced, killed, lost and most at once, gold captured, produced and converted) — the human, the multiplayer flag, the difficulty, `end_message_` | nothing |
| `ai` | the sixteen per-player rows, the script-owner table, per-settlement economy/tactic bindings, the prune counter, the AI-helper name→script table | the profile table, whose entries hold borrowed `const AiProfile*` |
| `campaign` | `CampaignProgress`, applied through `CampaignSystem::restore` so a save from a different conquest is refused; the note board (nested `NOTS`); the conversation results (nested `CRES`) | the territory id and bonus tables, from `configure(ConquestMap)`; the note catalogue and the conversation catalogue, both rebuilt from the container |
| `areas` | the area table | nothing — but see below |
| `fog` | the explored map as the original packs it: one `u16` per 1024-unit cell, two bits per player for eight players, then the partially explored cells' fine records — slot, cell index, and 32 × 32 nibbles packed two to a byte — in slot order and cell order | the grid's size, which `start` takes from the match's map extent, and the "already stamped" table the sweep keeps — but the *contents* are saved, see below |

Two of those rows need their reasons stated rather than tabulated.

**`match` carries eight report counters that are not hashed.** Nothing a script can reach
reads them — they feed `STATISTICS.INI` and nothing else — so, like the economy's
per-settlement statistics, two peers may disagree about them without disagreeing about the
game. The five score counters beside them *are* hashed, because `GetTeamMilitaryScore` ends a
match on them. State vector **18**.

**`movement` carries avoidance state that nothing hashes.** A unit blocked by another holds
for 50, 75 or 100 ms, gives way sideways, and walks back to its route; a building's doorway
and a tree's footprint are circles no unit may give way into. That is `CVXPathCoop` and the
ownerless `CVXDestLock`s in the original — path media and objects that clear their own
sync-set bit — and `pathfinder` is zero in all nine dumps, so none of it is folded into
`MovementSystem::hash`. It is saved because it is not recomputable: a unit reloaded without
its hold steps at once, one reloaded without its sidestep jumps back onto its route, and
locks rebuilt at load would forget that the original makes them once, when the map loads,
and carries them in the save thereafter. `test_avoidance.cpp`'s
`a_save_taken_mid_step_resumes_exactly` knocks every one of these fields off before a load
and requires the reloaded crowd to end where the uninterrupted one does. State vector **19**.

**`match` carries `end_message_`, which is display text and is deliberately not hashed.**
`EndGame`'s three-argument form assigns the string only when the acting player is the local
one, so it is one player's sentence and never part of the simulation. It is saved anyway: a
game reloaded after its victory condition fired should still be able to say why.

**`fog` carries a map that nothing hashes, and that is the shipped build's own
decision.** `exploration` is one of the four channels the nine desync dumps
leave at zero, beside `pathfinder`, `scriptstate` and `aihash`; folding it here
would put this engine's determinism contract somewhere the original's is not. It
is *saved* for `WorldObject::sight`'s reason turned around: the map is a
function of where every unit has ever stood, so rebuilding it on load would need
a history no save carries, and a reloaded game whose map had gone dark is a
different game. `sim/fog.hpp` has the encoding and what is still unknown about
it.

**`areas` carries a table that nothing writes after load and nothing hashes.** It is derived
from `map.obj.xml` by `load_areas` during `GameSession::create`, so a load would rebuild it
correctly without the section. It is written for the same reason `WorldObject::sight` is
written: recomputing it would make a save's meaning depend on the map data staying put, which is
the silent drift a versioned save exists to prevent — and it puts the system inside the
byte-level check below rather than outside it.

**The environment store carries keys no script ever wrote, and two of them are named here
because nothing else names them.** `Env*` looks like a script-only store and is not: several
host entry points read and write it directly, so a key can be live state that no `EnvWriteInt`
in the corpus produced. `Settlement::SpentGoldOnArmy` and `Settlement::SpentGoldOnTech` are the
clearest case — `gbr.exe` formats
`/<root>/Settlement<id>/GoldSpentOnArmy<owner>` and `.../GoldSpentOnTech<owner>` and does a
read-add-write against it, with the owner as its **zero-based** index. This engine's `EnvScope`
is the path rather than a string, so the settlement segment is the scope and the trailing name
and suffix are the key. There are 17 writes across the installation and **no reads at all**: the
readers are `Settlement::GoldSpentOnArmy` / `GoldSpentOnTech`, registered beside the writers and
called by nothing, which is why the writers are bound here and the readers are not. The other
key of this kind already documented above is `/Player<n>/SelectionAssigned`.

**The stores are nested, length-prefixed, inside their systems' sections.** `SettlementStore`,
`SquadTable`, `ItemStore`, `EnvStore` and `AiVarStore` each grew a serialise pair of their own
rather than being rebuilt through their systems' public API, and each declaration says why. The
common reason is that a store's *order* is state — `lower_bound` binary-searches it, `hash`
folds it, the AI-variable vectors are dense and their length is a function of the highest
variable ever written — and reproducing that order by replaying public mutations would make the
file's meaning depend on an insertion rule that is private and allowed to change.

## The session pair

[`GameSession::save`](../../engine/core/include/imperivm/core/sim/session.hpp) returns the
bytes; `GameSession::load` puts them back. Neither touches a file — the core cannot — so
wrapping the result in a [`.bfhp`](bfhp.md) container is `engine/gamedata`'s or `engine/app`'s
job, as it is for everything else the core produces.

`load` does three things nothing under it can:

1. **It routes the system sections.** `read_save` cannot reach inside a `System`, so `load`
   walks `kSystemOrder` and hands each section to its owner. The dispatch is a copy of the one
   in `create`, deliberately, so that both read the run order from the one line that states it.
2. **It primes the script library.** `Scheduler::deserialize` resolves a saved coroutine's code
   *by source name* and refuses one it cannot find. A session that has only been `create`d has
   an empty library, so `load` restores the world first, compiles from it the three sets that a
   normal start would have compiled — the victory condition, the AI profile's `[Scripts]`
   manifest, and each class's `idle` binding — and only then applies the scheduler's section.
   `AIRun` and the command pump look a script up and never compile one, so nothing else can be
   running. A session that *has* already been started is equally fine: the saved coroutines
   replace whatever was there.
3. **It puts back its own watermark, the templates' ground and the passability layer**, from
   the `session` section, the ground and the layer ahead of the systems and the watermark
   before any turn can run the sweep that reads it. This is the half of "a started session is equally fine" that was not
   true until the section existed: the coroutines came back and the sweep's memory of which
   objects had one did not.

`load` refuses, before touching anything, a save whose state-vector version, map identity or
system run order is not this session's; and afterwards, as `FormatError::malformed`, one whose
recorded hashes do not match. **A session that failed a load is not usable** and the caller must
discard it — a half-restored session that kept running would be the exact silent divergence this
format exists to prevent.

## The check that makes a save correct

A save is correct when loading it reproduces the simulation exactly. Three checks, and the third
one exists because the first two have a blind spot.

1. **`verify_hashes` at the load.** `meta` records `World::hashes()` as it stood at the save;
   after every section has been applied, `verify_hashes` recomputes them and compares channel by
   channel in `conformance::Channel` order, so `slots` is reported ahead of anything it caused.

2. **The per-turn hash sequences agree.** Save, load into a fresh session, then advance both on
   the same ragged schedule and compare every channel at *every* turn — not only at the end. A
   field restored to a value that happens to hash the same at turn zero and drifts afterwards
   shows up here.

3. **The re-save is byte-identical.** Save, load, save again: the two blobs must match byte for
   byte.

**Check 3 is the one that sees `command` and `ai`.** Nine of the eleven systems fold into
`slots`, so losing one is caught immediately by check 1. `CommandSystem` has no `hash` override
and `AiSystem::hash` is a no-op, and `areas` is deliberately unhashed, so a load that dropped a
command queue, an AI script binding or the area table passes checks 1 and 2 unchanged and then
misbehaves later with nothing to point at. That asymmetry is asserted rather than believed:
`save_command_and_ai_are_invisible_to_the_hash_and_caught_by_the_bytes` in
`engine/tests/test_save.cpp` first proves the world hash really cannot see either system's
state, then proves the byte comparison can.

Check 3 has a blind spot of its own, and it is worth stating: it compares a *re-save* against
the save, so it catches a **loader** that dropped a section and not a **writer** that never
wrote one. A writer-side omission round-trips perfectly, as a zero. That case is covered by the
eleven per-system tests in `test_save.cpp`, which mutate a system, encode, decode into a fresh
instance, encode again, and require the blob both to match *and* to differ from the blob a
default-constructed system produces. Verified by injecting the fault: making
`CommandSystem::serialize` write an empty queue list fails
`save_command_section_round_trips` and `save_command_and_ai_are_invisible…`, and passes
`imsave` — which is exactly the division of labour above.

## The file on disk

Everything above is the envelope: the simulation's own bytes, which `GameSession::save` returns
and cannot put anywhere. The file the application writes is the envelope inside a
[`.bfhp`](bfhp.md) container beside a manifest, and the shape is the original's: `gbr.exe`
mounts `currentadv.bfhp` as `AdvSave/` and persists the whole mounted filesystem
([adventure.md](adventure.md)), with a second mount, `ConquestTempFolder/`, for the
`territories.xml` a conquest rewrites between missions. A container is what lets a save carry
that document beside the session, where the original keeps it.

A 512-byte-block container, entries created in this order — and the order is the block
layout, which is why it is stated:

```
save.ini          the manifest
session.isav      the envelope, verbatim
```

Nothing else: a conquest mission's progress is in the envelope, in the campaign system's own
section. What crosses *between* missions is a file of its own, because it outlives every
session — see [The campaign between missions](#the-campaign-between-missions).

`save.ini`:

```
[Save]
container=Conquests/mediterranean.BFHP    installation-relative, /-separated
map=7                                     the Maps/<n> number played; empty for a root map
seed=1                                    the World seed the session was built from
turns=30                                  SaveMeta::turns, for a browser
time=14995                                SaveMeta::time, likewise
engine=0.1.0                              who wrote it
```

**The manifest names the game; the envelope carries it.** `GameSession::load` must be applied
to a session built from the same game data, and the envelope says nothing about which data —
`SaveMeta::map` is a spelling the writer chose. The manifest is what a loader builds that
session from: `imrun --load` and the application's `--load` open the container it names, read
the map it numbers, seed the world as it says, start the session as for a fresh game and apply
the envelope over it. `map` is the number *played*, never the one requested: "the first in walk
order" and `game.xml`'s `start_map` are two ways of asking and one way of having played.

The map identity the envelope records, and `load` checks, is one spelling everywhere:
`<container>/Maps/<n>`, or `<container>` for a root map. A save of the conquest's seventh map
refuses to load into its third by that string before any object is decoded.

**The container writer is the reference's.** `BlockFileBuilder` in
[`formats/bfhp.hpp`](../../engine/core/include/imperivm/core/formats/bfhp.hpp) is a port of
`bfhp.py`'s builder, which rebuilds every retail container byte for byte from its own
extraction; `tests/test_corpus_imsave.py` takes a save `imsave --out` wrote apart with the
Python reader and rebuilds it with the Python writer, and requires the same bytes. A save's
session is several hundred kilobytes, so that one check crosses the direct-block limit, the
promotion and a run of index blocks.

`imsave --out FILE` writes the file; `imrun <game> --load FILE [turns]` resumes it headless;
the application resumes it with `--load FILE`, writes `<installation>/Saves/quicksave.bfhp` on
**F5** and reads it back on **F9** — refusing a quicksave of another map by its identity, and
ending rather than continuing if a load fails, because a session that failed a load is not
usable.

## The campaign between missions

The other thing that outlives a session. A conquest mission's own progress is in the envelope;
what the *next* mission starts from has to survive the session ending, so it is a file of its
own — `<installation>/Saves/<conquest>.campaign.ini` — and it is the engine's spelling of what
the original keeps as the conquest's `territories.xml`, rewritten under the
`ConquestTempFolder/` mount ([adventure.md](adventure.md)). An INI rather than the XML because
an INI is the shipped configuration format, a person can read it, and there is no conquest in
progress in a retail install to decode.

```
[Campaign]
container=Conquests/mediterranean.BFHP                    which conquest this belongs to
territories=Spain,Britain,Gaul,Italy,Carthage,Egypt,Germany   the ids, in table order
states=1,1,1,1,1,1,1                                      TerritoryState per territory
conquered=0                                               indices, in the order conquered
active_bonus=rIberia                                      what ConquestBonus() answers next
```

`territories` is carried so that a restore can refuse a file written for another conquest, or
for this one re-authored, by name rather than by a count that happens to match.

**What crosses, and when.** `CampaignSystem::carry` is the finished mission's answer: the
territory states as its victory sequence left them, the mission's territory appended to
`conquered` if the sequence did not do it itself, and the active bonus set to **the most
recently conquered territory's** — which is the inference [`sim/campaign.hpp`](../../engine/core/include/imperivm/core/sim/campaign.hpp)
labels (`ConqueredOrder` is ordered; a player choosing at the campaign map fits the same
evidence), acted on in that one line and nowhere else. A lost mission carries the progress
exactly as it stood. `GameSession::restore_campaign` installs a carry, and it has to run
**before `start_sequences`**: the container-root `StartBonuses` reads `ConquestBonus()` on its
first pass, and a carry installed after that is a reward that never runs.

Measured on the conquest, headless: map 10 (Spain) won → `conquered=0`, `active_bonus=rIberia`
→ a fresh session on map 4 with the file restores it and its `seq0.vs` reads
`ConquestBonus() -> "rIberia"` and runs that sequence. `tests/test_corpus_imrun.py` holds it.
`imrun <game> <conquest> [turns] [len] [map] --campaign FILE [--human N] [--declare-won]` is the
harness — `--declare-won` ends the human's match the way a victory sequence would, because no
headless run wins Spain in a minute, and the output says it was declared and by what. The
application reads the file at the start of any conquest map and writes it when the human wins,
then prints which `--map-index` values the carry opens, because the campaign map that would
show them is Part 6's.

## What is saved, measured

[`engine/tools/imsave`](../../engine/tools/imsave.cpp) runs the whole thing over a real
installation, because `core_tests` links `imperivm_core` alone and so cannot open a map.

**25 map runs across the 19 map-bearing containers in a retail install** (the extra six are
`mediterranean`'s further `Maps/<n>` directories), each: a full session with scripts, the match
and the AI started, advanced 100 turns on a ragged 400/400/800/200/799/400/800/200 schedule,
saved, loaded into a *fresh* session built from the same inputs, then 20 more turns on both.
Every one passes all three checks. Saves run from 16,338 bytes to 534,595
(`3_Great_Battles_Alesia`). Two saves of one session are byte-identical, which is asserted
rather than assumed.

### And a check that the checks work

`imsave --verify-faults` re-runs the round trip once per system, each time **deleting that
system's section from the save** before loading it, and requires every deletion to be caught.
On `Scenarios/Balcans.BFHP` at 300 turns:

```
  movement   caught by load refused
  combat     nothing to lose: unchanged from turn zero on this map
  economy    caught by load refused
  feeder     caught by load refused
  hero       caught by re-save differs
  env        caught by load refused
  command    caught by re-save differs
  match      caught by load refused
  ai         caught by re-save differs
  campaign   nothing to lose: unchanged from turn zero on this map
  areas      nothing to lose: unchanged from turn zero on this map
```

`command` and `ai` are caught by the byte check and by nothing else, which is the whole point of
having it.

**"Nothing to lose" is an honest answer, not a pass.** Before injecting a fault, `imsave`
compares the system's section at turn N against its section at turn 0; if they are byte-equal,
deleting it loses nothing and no check could notice. That is a fact about the scenario, not
about the save. Three systems are in that position in a headless run and each for its own
reason:

- **`campaign`** — nothing calls `SetTerritoryState` inside a mission.
- **`areas`** — load-time by construction; the table is rebuilt identically by `create`.

**`combat` used to be a third, and it no longer is.** This list carried it with the reading that
`CombatSystem` is *empty* in a `GameSession` — that `start` binds the player table, nothing ever
enrols a combatant, and `advance` returns at its first line. That was a wiring gap in
`session.cpp` and it has been closed: `create` calls `set_class_graph`, binds the world and lets
`reconcile` enrol, and `MovementSystem::set_grid` and `CommandSystem::set_scheduler` are called
beside it. On `6_Great_Battles_Danube` a 200-turn session enrols hundreds of combatants, lands
six thousand blows and kills four hundred of them, and deleting the combat section is **caught by
`load refused`** rather than reported as nothing to lose. The synthetic tests are no longer
carrying that domain alone.

All three are covered instead by their own tests in `test_save.cpp`, which mutate the system
directly and require the mutation to survive the round trip.

## What the original saved

Recovered from `gbr.exe`'s persist stream, which registers each field by name. Used as a
checklist for what a save must contain, not as a layout. The engine's own machinery is
`CVXPersistStream` with `Save`, `Load`, `SaveXML`, `HashSave` and `Dump` variants, and a
`CVXPersistException` for when it goes wrong.

| Group | Registered field names |
|---|---|
| `Game` | `seed`, `CmdIdSeed`, `Timestats`, `frame`, `lastprintframe`, `lastprinttime`, `lastcmdstarttime`, `party`, `viewlocked`, `minimap_blocked`, `followed_unit`, `signature`, `game_over`, `animations`, `colorize`, `Settings`, `Player`, `nextseason`, `hashes`, `explvisible`, `fogvisible` |
| `GameTime` | `gametime`, `gametimetickstart`, `gametimetickend`, `ticksize` |
| `GameHashes` | `slots`, `threads`, `exploration`, `scriptstate`, `netcmds`, `extra` |
| `gamesettings` | `syncseed`, `normalseed`, `fogofwar`, `gamepath`, `loadgame`, `rmsettings`, `animatedecors`, `playdemo` |
| `Feeder` | `unit_chain`, `total_unit_count`, `next_tick`, `ch_quant`, `cf_quant`, `curr_quant` |
| `CVXAI::Persist` | `player`, `profile`, `difficulty`, `SettlementES`, `SettlementTS`, `GAIKAMap`, `LAIKAs`, `strat`, `enemies`, `LastSeen`, `priority`, `Optimism` |
| `CVXGlobalAI::Persist` | `GAIKAs`, `slotresx`, `slotresy`, `GAIKASlots`, `MAIKAs`, `gaikaset`, `setgaika`, `settlement`, `squads`, `coverage`, `adjcount`, `dists`, `shipneed`, `shipcount`, `ships`, `maika` |
| `CExecutionAI::Persist` | `CivEval`, `CivUnits`, `Squads`, `ClassCount`, `SrcGAIKA`, `GAIKAIn`, `DestGAIKA`, `order`, `state`, `LastFight`, `FirstFree` |
| `sequencemanager` | `curid`, `source`, `local`, `autorun`, `lastinstrexceedtime`, `loading-local`, `compiled-successfully` |
| `CVXSPath` | `success`, `pathleft`, `segment`, `steps`, `stepsdone`, `steplen`, `stepsize`, `pointcount`, `flags`, `sample_num`, `sample_end`, `gate_checked`, `range`, `minrange`, `dest_handle` |
| `GlobalSpell` / `GlobalSpells` | `spell_type`, `player_id`, `time_begin`, `stonehenge`, `spells_count`, `is_thread_running` |
| `Feedback` | `next_in_chain`, `time_end`, `Offset` |
| `Exploration` | `explslot`, `slotgrid`, `dwsize` |
| `CVXGate` | `enemiesaround`, `friendsaround`, `vtmovestart`, `vtmovetime`, `cmovestart`, `cmovedist`, `bmovestarted`, `bPassAplied`, `bFirstInit`, `savedpass` |

Four things follow from that table, and all four are load-bearing.

1. **Persisted is a wider set than hashed.** `CVXSPath` — the whole pathfinding state — is
   persisted field by field, and the `pathfinder` hash channel is zero in all nine dumps. So is
   `Exploration`, whose channel is also zero. Determinism excluded them; the save did not. A
   save that carries only what `state_hash` covers is not a save of the game.
2. **The save carries its own hashes.** `Game.hashes` and the `GameHashes` group are exactly
   what `meta` reproduces, and they are the reason `verify_hashes` is not an invention.
3. **The original had two generators.** `gamesettings` registers `syncseed` *and* `normalseed`.
   We have one (`sim/rng.hpp`), which is the synchronised one; see the unknowns below.
4. **The feeder's saved fields line up with ours almost exactly.** `unit_chain` /
   `total_unit_count` / `next_tick` / `ch_quant` / `cf_quant` / `curr_quant` against
   `FeederSystem`'s `chain_`, `next_tick_`, `food_quant_`, `health_quant_`, `food_cursor_`,
   `health_cursor_`. That is a name-level cross-check on a system built from other evidence
   entirely.

## What is deliberately left out

- **`AnimTimeline`, `Entity*` and the `ClassGraph*`.** Load-time state, re-resolved from the
  class graph and the entity resolver exactly as `populate_from_map` and `play_anim` resolve
  them. An address in a save is a save that only loads in the process that wrote it, and a
  copied timeline is a copy of game data that can disagree with the game data.
- **`NativeObject::position`, `owner`, `health`, `max_health`, `alive`.** Pre-Part-5
  placeholders that the simulation neither reads nor writes; the authoritative copies are in
  `WorldObject::state`. Writing them would create a second answer to a question that has one.
- **The four reserved hash channels.** See `meta`.
- **Every system's load-time configuration** — the rules structs, the command table, the class
  graph, the AI profiles, the shipped catalogs, the obstruction grid, the item catalog. See the
  table under `system:<name>` for the per-system list. A save is applied to a session built from
  the same game data; that is the contract, and it is what `GameSession::load` enforces as far as
  it can.
- **`PathFinder` and `CommandSystem::method_cache_`.** Scratch and memo respectively; both refill
  themselves and neither is state.
- **The `ObjList` pool's alias resolver.** The pool's entries are in the `world` section; the
  function pointer that turns an alias into a settlement's garrison is a seam
  `EconomySystem::start` installs on the live pool, and it is the embedder's, not the save's.
  Worth naming because it was lost once: `World::deserialize` decoded the pool into a fresh
  object and move-assigned it over the live one, resolver and all, so after every load
  `Settlement::Units` handed out aliases that read as empty over a full garrison, and
  `UNIT_ENTER.VS`'s `.Units().Contains(this)` walked a unit back into a town it was already
  in — Balcans at turn 104, a hundred turns in and four after the load. The pool now adopts
  the decoded entries and keeps its own resolver (`ObjListPool::adopt_entries`), and
  `test_economy.cpp` round-trips a world across an alias to hold it there.

## What is still unknown

- **The original's byte layout.** Nothing here reproduces it, because there is no retail saved
  game in the installation to reproduce. If one turns up, the `CVXPersistStream` field names
  above are the map into it, and this document should be replaced by a decode rather than
  extended.
- **`ObjList` lifetime across a spawn.** The pool round-trips (see the `world` section), but
  `sim/objlist.hpp` records an open question the save does not settle: a list can outlive the
  script that owns it, because `TS_CARTHAGETACTIC.VS` fills a local `ol` and passes it to
  `AIRun("TS_AttackAtWill.vs", set, ol, ...)`. `release_script` does not know that, so a child
  outliving its parent is left naming a stale handle. A save reproduces whatever the live pool
  held, faithfully, including that state — it neither causes the problem nor fixes it. The
  experiment `objlist.hpp` asks for is a save/reload replay in which a parent tactic script ends
  while its child still runs, and `imsave` is now the tool that could run it.
- **`campaign` and `areas` are not exercised by any real-map round trip**, because their state
  does not move in a headless session — nothing calls `SetTerritoryState`, and the area table is
  load-time. Their sections are covered only by the synthetic per-system tests. That is a gap in
  the *evidence*, not in the format. **`combat` was the third name on this line and has left it**:
  a headless session can fight now, which is what the line said would close it.
- **`normalseed`.** The original carried a second, unsynchronised generator alongside
  `syncseed`. What it drove is unknown — presentation, plausibly, since it is not in the
  determinism contract — and we do not have one to save. If one is ever added it is world state
  for saving purposes even though it is not for hashing purposes, which is the same distinction
  `CVXSPath` makes.
- **What the original did about a save whose *class graph* had changed.** It checked the map
  (`"...is missing or has been changed"`) and the build version. Whether it also checked the
  game data, and what it did when a class an object was spawned from no longer existed, is not
  recoverable from the strings. This format stores `class_index` — a graph-order index — so a
  reordered class graph would silently reassign classes. Storing the class *name* instead would
  cost roughly 20 bytes an object and remove the failure mode; it is not done yet because
  nothing in the tree renumbers the graph, which is an argument that will stop being true.
- **`SaveMeta::map` is whatever the embedder passes.** There is no canonical map identity in
  this codebase yet — a container path, a `game.xml` name and a `Maps/<n>` index are all
  candidates — so the check is exact-string and the choice is the app's. The original clearly
  had one, since it names the map in its own error message.
