# A sacrifice's stop flag

**Status:** read from `gbr.exe`; the flag is reproduced in
`engine/core/src/sim/world_host.cpp` (`m_consume`, `m_query_is_empty`) as
`ObjectFlags::sacrifice_stopped`. The druid list behind it is not modelled
(see the end). This is a runtime behaviour, not a file; nothing here is
code from the executable (`docs/legal.md`).

A `CVXSacrifice` is the ritual object a druid starts. Its scripts read two
of its fields: the druid list at `+0x14c` (with its count at `+0x150`) and
an int at `+0x15c`, the **stop flag**.

## Who writes the flag

* **The constructor** (0x00595290) sets it to 0, and reads
  `GamePlay/SacrificeThroughput` into `+0x158`.
* **The druid-list prune** (0x00595150) is the only other writer. It walks
  the list and drops every entry whose druid
  * no longer resolves,
  * has health (`[druid+0xc0]`) at or below a tenth of its maximum
    (`[druid+0xc8] / 10`), or
  * holds a lease whose deadline is before the clock it is given,

  and writes `0xffff` into that druid's sacrifice handle `[druid+0x1cc]`.
  Then it sets the flag to 1 if the list is now empty and to 0 if not.
* The prune has one caller: the core of `Consume` (0x00596050), which runs
  it first. When the flag is then set, the core answers 0 without
  spreading the price over anybody; otherwise it takes the price from the
  druids left on the list.

The flag is therefore "no druid was left the last time the ritual was
paid for". A sacrifice reads 0 from its making until its first `Consume`.

## Who reads it

* `Sacrifice::IsEmpty` (0x00594eb0): the handle check, which answers true
  when the handle does not resolve, then the flag.
* `Consume`'s core, as above.

## What this engine models

No druid can join a sacrifice's list here (`AddDruid` does not attach, for
the reason given beside `m_add_druid`), so every `Consume` finds the list
empty. `Consume` therefore raises the flag on its receiver when that is a
sacrifice, and answers 0. `IsEmpty` on a sacrifice answers the flag. The
flag is saved and hashed in the object's flag word (state vector 53).

## What differs, and why

* The druid list, its leases and the prune's three tests are not
  modelled, because nothing can put a druid on the list. So the flag is
  only ever raised, never lowered: in the original a `Consume` that found
  a living druid on the list would lower it.
* As in the original, a `Consume` that waits runs the core when it
  resumes, so the flag goes up after the wait, not when the call is made.
