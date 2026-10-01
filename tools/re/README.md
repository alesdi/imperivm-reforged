# Reverse-engineering tools

Three contributor tools that were being **rewritten from scratch every session**
before they were committed here. None of them ships game data, and none of them
is needed to build, test or run the engine.

| | |
|---|---|
| `gbr.py` | read the player's own `gbr.exe`: disassemble by address, find strings, find cross-references |
| `hostregs.py` | every host entry point the executable registers, with body address and argument types |
| `faultrun.py` | run a fault-injection sweep across N git worktrees in parallel |

## The rule that governs all of this

`docs/legal.md` rule 2: **do not paste disassembler output into this repository,
in any language, including as comments.** These tools read; what they print stays
on the contributor's machine. Describe the behaviour in prose — in a
`docs/formats/` specification or in the doc comment of the thing that implements
it — and implement independently from that description. Naming an address so a
reader can check the claim is fine and is what the rest of this tree does;
reproducing the instructions at it is not.

## `gbr.py` and `hostregs.py` need `capstone`

`pip install capstone`. **This is the only third-party dependency anywhere in the
project**, and it is confined to this directory on purpose: nothing that builds,
tests or runs the engine imports it, and `README.md`'s "Python 3.11+ with no
third-party dependencies" stays true of everything a user touches.

Both find the executable through `IMPERIVM_GBR`, then `IMPERIVM_GAME_DIR`, then
`~/Downloads/Imperivm/gbr.exe`.

## Why `hostregs.py` earns its place

`docs/formats/vs-host-api.md` is inferred from call sites and says at the top
that the executable wins where the two disagree. It already has, repeatedly —
receivers on the wrong type, arities that do not exist, a family of five entry
points where this project's census had three and two phantoms. One line of
`hostregs.py` settles the receiver, the arity and the return type of an entry
point, and it is the first thing to run before implementing one.

It was itself wrong until the conversation layer needed it, in two ways that
cost it **389 of the 1,498 registrations, a quarter of the table**:

* it required the registry to arrive as `push reg`, which is the cdecl form.
  The thiscall form passes it in `ecx` and never pushes it, and *everything*
  registered through the textual registrar uses that form -- so the entire
  signature-carrying third of the table was invisible, `BlockUserInput`,
  `View`, `ExploreCircle` and `SetShortcutSel` among them; and
* it demanded four pushed items, which a textual registration -- body, name,
  signature -- never has.

A third bug was in the walk itself. Reading a push list backwards is ambiguous,
because the last byte of one instruction can start another: `push 0x6a6d00`
ends in the byte `0x6a`, which is `push imm8`. Trying the narrow form first
turned every body address containing that byte into a two-byte push and
desynchronised everything before it. `push imm32` is now tried first, which
makes the two items that must be right -- the body and the name -- unambiguous,
because both are always pointers.

## Why `faultrun.py` earns its place

See its module docstring. The short version: the sweep used to be serial, a
one-file change gives `ninja` a single compile job, and twenty-four faults was
**ten minutes of one core** on a ten-core machine. Six worktrees brought that to
160 seconds; fixing `HostRegistry::declare`'s quadratic build (see
`docs/plan.html`) brought it to **33**, because a sweep runs the whole test
suite once per fault and the suite went from 26 seconds to 0.6. The worktrees
persist for the session, so only the first sweep pays for the builds.

It also keeps a sweep out of the working tree entirely — you can keep editing
while one runs — and lets a fault target a *header*, which the serial driver
could not. The first time that mattered, it caught a fault that had been
silently failing to apply.
