# A settlement's garrison holder: `health` and `stamina`

**Status:** read from `gbr.exe`; reproduced in
`engine/core/src/sim/world_host.cpp` (`m_health`, `m_stamina`). This is a
runtime behaviour, not a file; nothing here is code from the executable
(`docs/legal.md`).

A settlement owns one **garrison holder**, the object its 16-bit handle at
`[settlement+0x5e]` names. `UnitsCount`, `max_units` and the units inside
are read from it. Its record holds a health at `+0x1c` and a stamina at
`+0x24`, inside the block `+0x14` to `+0x24` that also carries its maximum
units, maximum health and maximum stamina.

## The two readers

`Settlement::health` (0x005c1e40) and `Settlement::stamina` (0x005c1f00)
resolve the settlement, resolve its holder, and answer `+0x1c` and `+0x24`.
Either miss prints and answers 0.

## The writers

* The holder's constructor (0x00531de0) zeroes `+0x14` to `+0x24`.
* The holder's archive exchange (0x00531fb0) is the only other code that
  writes the two fields. It reads and writes a `Holder` record: the
  settlement, `maxunits`, the units, `maxhealth`, `health`, `maxstamina`
  and `stamina`. A saved game therefore writes them and loads them back.
* No map file carries a `Holder` record: the shipped `map.obj.xml` files
  hold only `mapobject`, `settlement` and `scriptobj` elements
  ([`map.md`](map.md)). So no game ever puts a value other than 0 into
  either field, and a save writes 0 and loads 0.

So **both answer 0 for every settlement in every game.** No shipped script
asks either of a settlement.

## What this engine models

A receiver of the `Settlement` type answers 0 for both. The `Obj` forms of
`health` and `stamina` read the object, as before.
