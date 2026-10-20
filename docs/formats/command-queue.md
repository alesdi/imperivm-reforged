# The command queue

**Status:** read from `gbr.exe` for the host audit's twelfth round; the engine models the
slot's end, the stop flag, the synchronous `onfinish`, the `idle` as the queue's head from
the object's start, `SneakCommand`'s ids and the issue's group-move set-up
(`engine/core/src/sim/command.cpp`, `sim/session.cpp`, `script/scheduler.cpp`,
`script/vm.cpp`). What it does not model is listed at the end.
**Not a file format.** Nothing here is on disk; it is the runtime model behind every
`SetCommand`, `AddCommand`, `KillCommand`, `SneakCommand` and `ExecCmd` the shipped scripts
write, and behind every order a player gives, kept beside the host API
([`vs-host-api.md`](vs-host-api.md)).

Prose only, with addresses so each claim can be checked; [`../legal.md`](../legal.md) rule 2.

## One queue, one slot, one thread

Every script object keeps a deque of command objects (`[obj+0x8c..+0x98]`, the count at
`+0x98`) and an array of script slots. Slot 0 is the **command slot**: the thread running
the head command's method. Slots 1.. are the class's behaviours. A command object carries
its id (drawn from the command-id counter by the constructor, 0x00599302 through
0x0051d4b0), its verb, its argument by kind -- none, a point, an object, ... , and kind 5,
a list and a point -- and, when it came through a `<cmd>` row, the row.

Nothing in the queue runs except through slot 0, and slot 0 is only ever started from one
place: the command advance 0x005b4f20. With the queue empty it builds the class's `idle`
(0x005aef20 with `"idle"`, which draws an id), pushes it (0x005b4960) and starts it; with
something queued it starts the head. Either way the started command's name is written on
the object (`[obj+0x10c]`/`[obj+0x110]`, what `command` and `WaitIdle` read) and the stand
position rebuilt (`vtbl+0xc4`).

## A command's end starts the next one, there and then

The thread's message handler (0x0069f7a0) runs a slice of the interpreter (0x0069d4c0) and,
after it, asks the owner whether the object is to be erased (`vtbl+0x24`); if not and the
thread has finished or failed, it deletes the thread's message and calls the owner's
script-end virtual `vtbl+0x1c(slot, failed)`. For slot 0 that is 0x005b5160:

1. 0x005b19f0: with the queue non-empty, the head's row is made ambient (the four
   globals at 0x0081b2d0 and the row's three strings), its `onfinish` method is run **to
   its end, synchronously** (0x0059af00 looks the verb up in the class's method map and
   hands the path to the synchronous launcher 0x006a0360 with the object and the cancel
   flag `[0x009bdb18]` as `bCanceled`), the ambient row is cleared, the progress end
   `[obj+0x12c]` reset, and an object in script-action modes 2, 4, 5, 6 or 7
   (`[obj+0x130]`) has its animation stopped (0x0053d730) and its mode cleared.
2. The head is popped (0x00564770 lowers the player's research count for a research
   row) and deleted. The flag that would skip this (`[0x009bdb10]`, read at 0x005b5182)
   is never written.
3. If the object is not dead (`vtbl+0x50`), 0x005b4f20: the next command, or a fresh
   `idle`, starts now.

The new thread is armed at the wheel's clock, so it runs in the same pass. So a command
whose script returns is followed by the next one before any other script has run, and a
queue that runs dry is given its `idle` at once.

**Ending a command from outside.** 0x005b07d0(index) is every removal: it does nothing
unless the index is in the queue and slot 0 has a live thread (0x0069f950); it refunds a
costed row (`vtbl+0x90`); for index 0 it raises the cancel flag, ends the thread through
0x0069fee0(0, notify=1), lowers the flag and resets `[obj+0x12c]`; any other index is
erased and destroyed. 0x0069fee0 has two branches. When the thread being ended **is the
running one** (`[0x00a77ea8]`), it only sets the context's stop flag `[ctx+0x40]`: the
interpreter, which clears that flag at the start of every slice, turns the answer of the
host call that set it into "stop", so the script ends as the call returns and the end in
0x0069f7a0 runs then, not cancelled. Otherwise it deletes the thread and calls
`vtbl+0x1c(slot, arg)` at once -- the `onfinish` told it was cancelled, the head popped
and the next command started, all inside the `KillCommand` or `SetCommand` that asked.

`Obj::KillCommand` (0x005b0d30), `ObjList::KillCommand` (0x00562d20) and
`Query::KillCommand` (0x00578a80) are 0x005b07d0(0) per object. `SetCommand`
(0x005b0b30 point/0x005b0c30 object; the list forms 0x00561c10, 0x00562050, 0x00562a70 and
the query forms 0x00577db0, 0x005780d0, 0x00578870) builds the command and hands it to the
order insert 0x005b4e90 with the replace flag: the queued tail removed last first
(`vtbl+0xc0(0)`, 0x005b1120), the new command pushed, then the old head ended through
0x005b07d0(0) -- so the new command starts inside the call, or, when the head is the
calling script's own, when that script's slice ends. `AddCommand`'s insert (0x005b5c30)
puts the command at index 1 with `front`, at the back without, after the accept test
(`vtbl+0x8c`); it never starts anything.

## The `idle` is the head from the object's start

The object start virtual (0x005aec40; the overrides 0x0053c730, 0x004d64d0, 0x004db2d0 and
0x005d3090 call it first), called by every factory as an object is made and skipped while a
save is loading (`[0x00a87390]`) or in the editor (`[0x00a8734c]`), opens with
`vtbl+0x1c(0, 0)`: the slot-0 end with nothing queued, which has no `onfinish` to run and
nothing to pop, and goes straight to 0x005b4f20 -- the `idle` is pushed and started. Then
it sizes the script slots to the class's behaviour count plus one and spawns behaviour `i`
into slot `i + 1` (0x006a07e0). So every started object's queue holds its running `idle`,
object by object, the `idle` ahead of the object's behaviours; an `AddCommand` waits behind
it until something ends it (the shipped idiom `AddCommand(..); KillCommand();`), and
`CmdCount` counts it.

## `SneakCommand`

0x005b0930 does nothing in the editor or on an empty queue. It builds the new command
first (0x005aef20, drawing an id), then clones the running one (`vtbl+0x14`, 0x00599c40):
the clone's constructor (0x00599210) draws an id as every command's does, and the clone then
takes the runner's id and row over it (0x00599cdb). Both are inserted with `front` (the
clone first, then the new one ahead of it) and the head is ended (0x005b07d0(0)). So
`[run, rest]` becomes `[verb, run', rest]`, the resumed command keeps its id, and the
counter moves by two.

## The issue's group-move set-up

The per-object issue (0x004ef3d0; from `Obj::ExecCmd` 0x005b5470, `ObjList::ExecCmd`
0x0055e240, the right click and the command bar) takes, before its loop, the list's first
member: when it is a unit in a holder (`[unit+0x154]`) whose holder belongs to an object
(0x005319a0, `[holder+0xe]` -- a ship; a settlement's holder belongs to none), that object
is remembered (0x004e9100). In the loop, at the first actor for which a command was made
of an ordinary row (not immediate, not a research), and only then, 0x004e8ae0 is asked with
the form the actor's method resolved to:

- no argument: nobody gets the order;
- a point: nobody when its terrain index, half a cell rounded (0x004e8a80), is deep water
  (13); otherwise the ship is to put them ashore;
- an object: nobody when its class is a water unit (`[class+0xb30]`), or when it is a unit
  held by anything but this ship; held by this ship, everybody gets the order and the ship
  nothing; otherwise the ship is to put them ashore.

"Nobody" means every remaining actor's command is made -- its id drawn -- and dropped.
"Ashore" is 0x004ef1c0: the passengers are the issue's list, in order, followed by each
hero's army members not already in it; the ship gets an `unboard` of kind 5 with them and
the point (or the target's position), through the order insert with replace when the order
replaces (`vtbl+0xb8` with 1), else appended by the `AddCommand` insert (`vtbl+0xbc` with
0). It is made after the first actor's command and inserted before it. `SHIP_UNBOARD.VS`
(`// void, Obj This, ObjList ol, point pt`) sails there and drops them.

## What the engine does not model

- **The script-action mode at the end** (`[obj+0x130]`, step 1 above): the engine keeps no
  per-object action mode, so a command ended while its script was in `PlayAnim` or an
  idle/taunt activity leaves the animation to run out.
- **A holder that is not a ship's** passes the original a null `unboard` command when its
  class binds none; the engine gives it nothing (labelled at `group_move_set_up`).
- **An offset row's per-actor spread** (0x004e81f0, rows with `[def+0x1e0]`).
- **`command` with no argument on an empty queue** answers `idle`, where the original's
  string still holds the last command started (labelled at `command_impl`; the queue is
  empty only for an object whose class binds no `idle`, or a dying one).
