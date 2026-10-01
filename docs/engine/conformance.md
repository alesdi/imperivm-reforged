# The determinism harness

**Companion:** [`tick.md`](tick.md) (why there is no fixed timestep),
[`state-vector.md`](state-vector.md) (which channels the original hashes),
[`architecture.md`](architecture.md) (why the core is freestanding)

**Code:** `engine/core/include/imperivm/core/sim/conformance.hpp`,
`engine/core/src/sim/conformance.cpp`, `engine/tests/test_conformance.cpp`,
`engine/tools/imconform.cpp`

Lockstep determinism is the property this architecture is bent around. The
freestanding core, the no-floating-point rule, the ordered containers, the
single seeded RNG and the CI boundary checker all exist to buy it. Every domain
tested its own hash; nothing ran the pipeline twice and compared. This is that
thing.

## The four checks

### 1. Self-consistency

Run one scenario from one seed over one schedule, twice, and require
**byte-identical per-turn hash sequences**.

This is the cheap check and it catches the whole class of bug where the
simulation reads something the run does not own: an address folded into a hash,
an iteration over an unordered container, an uninitialised field, a static that
survived the last run.

It reports the **first** turn at which the two diverge and **which channel** did
it, because "they differ" sends somebody bisecting by hand and

```
channel `slots` first differs at turn 412 (game time 164800): run 1 0x…, run 2 0x…
```

points at one turn of one system. Channels are scanned in declaration order so
that `slots` is named ahead of the `hash_of_hashes` it caused — reporting the
roll-up first would bury the cause under its own symptom.

### 2. Partition invariance

The same total game time, delivered as **different turn-length sequences**, must
end in the same state: `[800, 800]`, `[400 × 4]`, `[200 × 8]`, and a ragged
sequence that changes length mid-run.

`tick.md` measures turn lengths of 200, 400, 799 and 800 in the retail dumps,
with one session renegotiating mid-game against measured latency. **There is no
fixed timestep.** So a system that accidentally depends on the *number of turns*
rather than the *elapsed time* is not a theoretical bug: it is a desync the
first time two peers agree on a different length. `test_combat.cpp` asserts a
smaller version of this over one system; the harness generalises it to a whole
pipeline, and found a real one within an hour of existing (see below).

Every entry point takes an explicit `Schedule`. There is deliberately no
overload taking a turn count and a constant — a harness with a fixed `dt` would
be testing a game nobody shipped.

**The clock is excluded, and only the clock.** `World::state_hash()` folds in
the turn counter and the current turn length, and it is right to: two peers that
disagree about either are running different simulations. But those are exactly
what differs between `[800, 800]` and `[200 × 8]` when nothing is wrong, so the
check calls `Run::freeze_clock()` on both finished runs — which zeroes the turn
counter and pins the turn length — and then compares `World::state_hash()` as it
stands. Elapsed game time *is* partition-invariant and is compared separately,
before the hashes.

The harness does **not** grow its own copy of the world's hashing to do this. A
second implementation of `state_hash` would drift from the first, and the day it
did the harness would be checking a projection of the state vector that nothing
else in the engine uses.

#### It only holds with the scripts switched off

**Partition invariance is a property of the systems, not of the engine.**
`imconform map` therefore runs it only under `--no-scripts`, and says so.

`Scheduler::advance` is `now_ += delta; run_ready();`, with no interpolation. A
script due at 1000 first runs when `now_` reaches 1600 under 800-unit turns and
1200 under 400s, so **its wake count over a fixed total depends on the
partition** — and a shipped script draws from the world RNG when it wakes.
`CROW_IDLE.VS` sleeps for `rand(2000)`; `DEER_IDLE.VS` has nine `rand` sites. A
different number of draws desynchronises the shared generator and everything
downstream of it.

`Balcans` is the scenario that shows it, because it is the wildlife map.
`Crossroads` passes with scripts on at 400 turns and `Numantia` at 40 — that is
luck about which scripts reach a `rand` before their state stops mattering, and
it is not evidence.

**Lockstep does not need this invariant.** Every peer in a match runs the same
turn length, negotiated by the engine against measured latency, so what has to
hold is that the *same* partition gives the same result — which is check 1, and
which does hold on every scenario tried. Partition invariance earns its place
over the systems alone, where it is sound and where it caught a real bug the
first hour it existed.

**Movement keeps it although it now decides things inside a turn.** Unit-versus-
unit avoidance asks, before every step of a route, whether the step's end is
taken — by whoever is there at that instant — and a unit blocked by another that
is still moving draws from the world generator. A system that asked at turn ends
would decide at different instants under `[800]` and `[400, 400]` and draw a
different number of times. `MovementSystem::advance` instead plays each turn out
as a queue of decisions ordered by `(game time, id)`, every instant derived from
the exact progress accumulator or a whole-millisecond hold, so the decisions, the
draws and the positions are the same however the turn is cut. The harness's
`--no-scripts` runs issue no orders and so exercise none of it;
`test_avoidance.cpp`'s `a_turn_partition_does_not_move_a_crowd` is the check.

### 3. An oracle

Compare a recording against a reference. Today the reference is a golden trace;
tomorrow it is a stream recorded from the original engine (see *Attaching a
recorded command stream*).

### 4. The reserved channels stay zero

`state-vector.md` measures `pathfinder`, `exploration`, `scriptstate` and
`aihash` as **zero in all nine desync dumps**. The shipped build deliberately
kept pathfinding, fog of war, script state and the AI out of the determinism
contract.

That is a constraint to respect, not to improve on. A harness that hashed them
would report divergence the original tolerated. So `check_reserved_channels`
*fails* a run in which one of them went non-zero rather than comparing it, and
`write_trace` refuses to serialise such a trace at all.

## Writing a partition-invariant system

The discipline is one sentence: **nothing may do work "per turn".**

Every effect is driven by an accumulator over `turn.length`, drained with a
`while` loop:

```cpp
phase_ += turn.length;
while (phase_ >= kPeriod) {   // a `while`, never an `if`
  phase_ -= kPeriod;
  do_the_thing();
}
```

The number of effects over an interval is then a function of the interval and
not of how it was cut up, because `(a + b) mod c` does not depend on where the
sum was split — the same argument `advance_elapsed` rests on.

Three ways to break it, all of which the harness catches:

* **`if` instead of `while`.** One 800-unit turn fires once where four 200-unit
  turns fire four times.
* **Counting turns.** `++cooldown_` in `advance` is what you write without
  thinking. Invisible to self-consistency, because both runs count the same
  turns.
* **Drawing from the RNG once per turn.** Subtler: every object ends where it
  should, and only the generator's position in the stream — the dump's
  `syncseed` — depends on the partition. The original hashes that, so it is a
  real desync.

`conformance::make_defective_scenario` reproduces the last two on purpose, and
`test_conformance.cpp` asserts each is caught. `DriftSystem` in
`conformance.cpp` is a worked example of the discipline.

## The trace format

Line oriented, hexadecimal, `#` comments, small enough to commit:

```
# imperivm conformance trace v1
scenario reference
seed 2a
systems drift
# turn length time slots threads netcmds extrahash ('=' repeats the line above)
t 1 400 400 1225459b861795c9 0 0 0
t 2 400 800 9e884455c4a7ee70 = = =
```

Deliberately not a binary blob: the whole value of a golden file is that the
diff is readable, and "turn 412 changed, `slots` only" tells a reviewer
something a changed checksum does not.

* `=` repeats the previous line's value for that channel, which is what keeps a
  quiet scenario cheap to commit.
* The four reserved channels are **not encoded at all**. Their only legal value
  is zero, and a field whose only legal value is zero invites somebody to change
  it.
* `hash_of_hashes` is not encoded either — it is a pure function of the four
  that are, and is recomputed on read, so a hand-edited golden file cannot
  contradict itself.
* `systems` records the run order. **This is load-bearing**, not decoration:
  `World::state_hash` folds each system's index and name in before calling its
  `hash`, so adding a system or moving one changes every hash in the trace from
  turn one. Without that line, a golden that met a new system would report
  "channel `slots` first differs at turn 1" — true, and pointed at the wrong
  thing entirely. With it, the harness says the pipeline changed and names the
  position. This is not hypothetical; it is what happened to this file's own
  golden the first time `feeder` landed between `economy` and `hero`.

An empty `systems` list means *not recorded*, not *no systems*, and is not
compared — so a golden written before the field existed still checks its hashes,
and so will an oracle recorded from the original engine, whose system list is
not ours and never will be.

### Regenerating a golden is a claim, not a fix

`kGoldenReferenceTrace` in `test_conformance.cpp` is a committed literal rather
than a file on disk, because the core test binary cannot open a file — the same
freestanding rule the harness itself obeys. `imconform trace 12 400` regenerates
it; `imconform map --record PATH` writes the on-disk equivalent for a real map.

When that test goes red, the harness is not broken and the build is not flaky.
It is reporting that **simulation results changed**. Regenerating the literal is
therefore a statement in the diff, in your name:

> I meant to change the simulation, and here is exactly what moved.

Treat it that way. Before pasting new hashes in, know which change caused them
and be able to say so in the commit message. A reviewer seeing twelve hashes
move with no explanation should push back, and a reviewer seeing them move
alongside "seeded the economy's settlement table from the loaded world" has
learned something real. The one thing this file must never become is a rubber
stamp that gets regenerated because CI was red.

The two failure modes to keep apart:

| The harness says | It means |
|---|---|
| `the system run order changed at position 3` | A system landed or moved. Expected; regenerate. |
| `channel slots first differs at turn 1` | Every hash moved. Something changed the state vector or its hashing. Find out what *before* regenerating. |
| `channel slots first differs at turn 412` | One turn, deep in the run. Almost never intentional. Investigate. |

## What the harness has actually found

Its first run against retail data, on `Crossroads`, `Balcans` and `Island War`:

* Self-consistency **passes** on all three, scripts on and scripts off, with up
  to 1,360 objects. Two runs of one map produce byte-identical per-turn hash
  sequences.
* Partition invariance **fails** on all three, from the very first interval:
  one 800-unit turn does not equal two 400-unit turns.

Bisected: object state, animation cursors, the RNG and the command-id counter
are all identical; the entire difference is in `EconomySystem`'s hash. Root
cause, in two halves:

1. `GameSession::Impl::seed_economy` creates settlements through
   `economy.settlements().create(init)` — the `SettlementStore` entry point —
   which bypasses `EconomySystem::create` and therefore **leaves all ten
   timers at zero**. On the system's own entry point they are seeded to their
   intervals. So on every retail map, every settlement timer starts at 0.
2. `EconomySystem::advance` chooses its sub-step as the minimum over timers with
   `remaining > 0`, then decrements, then fires whatever reached zero. A timer
   that is *already* zero on entry is not a candidate for that minimum, so the
   whole remaining turn is consumed before it fires, and the reset value is
   never charged for it. The state therefore differs by exactly the first turn's
   length: after 800 units, `[800]` leaves every timer at its full period and
   `[400, 400]` leaves it at period − 400.

Seeding the timers to their intervals makes the economy hash — and the whole
world hash — agree across partitions on all three scenarios. Fixing (1) is
sufficient, since a timer can otherwise only reach zero *inside* the loop and is
reset in the same iteration; fixing (2) as well would be defence in depth.

`test_economy.cpp`'s own `economy_agrees_under_any_turn_partition` passes
throughout, because its `run_scenario` builds settlements via
`EconomySystem::create`, which seeds the timers — so the test never exercises
the path every shipped map takes. That is the harness earning its keep: a
domain test can only test the world its fixture builds.

## Attaching a session

`SessionRun<>` is the adapter, and it is a template for one reason: a template
is not instantiated until it is used, so `engine/core` does not acquire a
link-time dependency on `session.cpp` merely by containing the header.

```cpp
class MapScenario final : public conformance::Scenario {
  std::string_view name() const noexcept override { return name_; }
  std::unique_ptr<conformance::Run> start(std::uint32_t seed) const override {
    auto session = GameSession::create(registry_, inputs_, seed);
    if (!session.ok()) return nullptr;
    auto run = std::make_unique<conformance::SessionRun<>>(std::move(session.value()));
    run->session().start_object_scripts();
    return run;
  }
};
```

`engine/tools/imconform.cpp` has the real one, including the installation
loading around it.

**`start()` must be a pure function of the seed.** The harness calls it
repeatedly and compares the results; a scenario that remembers anything between
calls makes the harness accuse the simulation of its own bug. Everything the map
scenario reads — the class graph, the host registry, the map bytes — is
immutable for the scenario's lifetime, which is what makes that true.

## Attaching a recorded command stream

This is the case the design is aimed at, and it decomposes into the two halves
the header already has.

* The **drive** is a `Scenario`. A recorded stream contains the map, the seed,
  and the commands each peer issued on each turn. A `RecordedScenario` builds
  the session from the recorded inputs and returns a `Run` whose `advance()`
  injects that turn's commands before calling `GameSession::advance(1, length)`.
  The recorded turn lengths become the `Schedule`. Nothing in the harness has to
  learn what a command is.
* The **oracle** is an `Oracle`. A per-turn hash log becomes a `Trace` and then a
  `TraceOracle`, and `check_against_oracle` is the function that already exists.

`Oracle::covers` is what makes this work against a *partial* recording: our
`threads` channel stays zero until the scheduler folds itself in, so an oracle
that knows the original's value for it declares it covered and one that does
not declares it not. Neither has to lie. `Oracle::systems()` returns empty for
such a recording, for the same reason.

## The fourth check: lockstep

`check_self_consistency` proves the simulation is a function of its seed and its
schedule. It says nothing about **input**, because it feeds none — and a desync
in a real match is almost never the world drifting on its own. It is two peers
resolving one right click differently, and the whole order-resolution path (the
per-actor candidate walk, the share-control filter, the verifier) runs only when
somebody clicks.

`check_lockstep` is the same comparison with both runs *driven*: the same orders,
on the same turns, from one `Driver`. `sim/netcmds.hpp` has the stream, the
canonical order every peer applies it in, and the fold.

**The `netcmds` channel comes from the driver, not from the world.** The command
queue deliberately hashes nothing (`sim/command.hpp` says why), so two peers
handed *different orders* have identical world hashes until those orders take
effect — which may be many turns later, in a system that has no idea why. The
shipped build hashed the command stream as its own channel for exactly this
reason: it is non-zero in seven of the nine dumps and zero in precisely the two
whose `cmdsprocessed` is zero. So `Driver::drive` returns the stream's hash
through that turn, `record` files it under `Channel::netcmds`, and
`compare_traces` reports it like any other channel.

It is **not** the original's fold and cannot be: no dump prints a command
payload, which is the same fact that makes the dumps unreplayable. What it buys
is the real half — two peers that disagree about a turn's orders say so on that
turn.

A run with no driver files zero, which is what every trace in this project was
recorded against, so no golden moves.

## The fifth check: netplay

`check_lockstep` hands two peers **one** stream. A real match hands each peer its
own player's orders and leaves the protocol to assemble the turn, and that
protocol is where a whole class of desync lives: a peer that ran a turn before
it had heard from everybody, two peers that computed different lengths for the
same turn, a packet decoded differently from how it was encoded.
`sim/lockstep.hpp` is that protocol — the turn packet and its wire format, the
input delay, the stall, the refusal of a peer that says two different things
about one turn, and the negotiated length — and `check_netplay` runs it.

Every player the stream names gets a peer with its own session. Every packet
goes over a loopback that delivers it up to `max_delay` rounds late,
independently per recipient, and then **delivers it again**; every peer proposes
its own turn length every turn. Then two comparisons, both through
`compare_traces`:

* each peer against the first, turn by turn, every channel — `netcmds` is
  `stream_hash` over the *agreed* history;
* the first peer against `record` of the same scenario, driven by a
  `StreamDriver` over that agreed history and schedule, with no network at all.

The second is what proves the transport transparent, and it is not redundant
with the first: peers that all make the *same* mistake — advancing by the
agreed real milliseconds instead of the game-time length they convert to, at
speed 999 — agree with each other perfectly. Only the unnetworked run can see
it, and `test_lockstep.cpp` has the case; the fault was injected and caught by
that comparison alone.

**A network that is never late tests nothing.** A packet is sent as its sender
starts turn `n` and is needed for turn `n + 1 + delay`, so the protocol has
`delay + 1` rounds of slack. The first run of the tool delivered at most three
rounds late against a delay of two, and so reported `0 stalls` and passed, on
both maps: the one path that makes lockstep lockstep, not running a turn you
have not heard about, had never run. The default lateness is past the slack
now, and the tool fails outright when no peer waited.

**And through a network that loses things.** The negotiator tolerates late,
repeated and reordered packets but stalls forever on a lost one, and UDP loses
them. `sim/netlink.hpp` is the layer between: every datagram carries every turn
packet the far side has not acknowledged, acknowledgement is per origin and
contiguous, and a packet is forwarded on every link but the one it came from, so
a node linked to everyone is a relay. `NetplayOptions::relay` runs the check
through it on a star, losing whole datagrams at `loss_per_mille`, with each
peer's proposal optionally the length its measured round trips ask for.
`test_netlink.cpp` plays four peers through a quarter of their datagrams lost,
and a link that loses everything is reported as a deadlock, never a pass.

**And between processes.** `imconform udp-host` and `udp-join` play the same
match over a real socket: the lobby seats each joiner only if its `data.pak` and
map hash the same as the host's, then the link layer carries the turns, with
`--drop` losing datagrams on purpose because localhost never does. Each process
checks itself against an unnetworked run of what it agreed, and prints a world
hash and a stream hash; `tests/test_corpus_udp.py` compares host with joiner.
Only within one match, though. Turn lengths now come from measured round trips,
which is real time, so two matches legitimately differ, and a test that compared
them would be testing the clock.

**And when a peer leaves.** Lockstep runs a turn only with every peer's
packet, so a departed peer stalls everyone -- and dropping it at different
turns on different peers is a desync. `sim/netdepart.hpp` is the agreement:
every peer that learns of the departure freezes at what it holds of the
departed peer's packets and reports that bound, the coordinator (the host)
decides the largest report once every peer still playing has reported, and
every peer applies exactly the departed peer's packets below it. The first
turn without it carries a `departed` order the negotiator writes in, and the
computer takes the seat there -- the original's drop routine does the same
(0x00406840 into the body behind `AIStart`, 0x00434ca0); the turn is this
engine's. The reports and the decision ride in every link datagram, so loss
repairs them. `NetplayOptions::departures` runs it: the peers that stay are
compared over every turn, the one that left against them over the turns it
ran below its agreed end, and the first that stayed against an unnetworked
replay of the history, takeover included. A departed coordinator ends the
match, reported rather than deadlocked. `test_netdepart.cpp` has the cases,
including a mesh where the coordinator holds none of the departed peer's
packets and the end must come from another peer's report;
`engine/net/tests` drops a joiner of three over real sockets, saying so and
going silent; `tests/test_corpus_udp.py` does it between three processes on
Zama, through loss, and requires both survivors to name the same turn and the
computer to take the seat on it.

**And when the speed changes.** `SetSpeed` is an order (`NetOrderKind::set_speed`,
turn packet version 3), as it is in the original: `SetSpeed` posts a
`CVXCmdSetSpeed` with the local player as issuer (0x004c6ce0) and every peer
executes it (0x004e67c0, clamping to 1..100000). Applying it writes the world
clock's speed; the negotiator converts every turn *after* the one it was agreed
for at the new speed, the last in canonical order winning, so the length of
turn `t` never depends on turn `t`'s own orders. A match with a fixed speed
(the lobby's `variable_speed` off) takes none, on any peer. `test_netspeed.cpp`
runs speed changes through `check_netplay`, direct and relayed; the unnetworked
replay, which only the history carries the speed to, agrees.
`tests/test_corpus_udp.py` and `test_corpus_app_net.py` change the speed from
each side of a two-process match and require both to apply it on the same
turn.

**And when a player joins late.** This engine's own feature -- the original admits nobody once
a match has started (`netjoin.md`). A joiner takes a seat the computer took from a player who left;
the host decides the first turn of its part past every packet it has sent and says so in every link
datagram, the negotiator writes a `joined` order into that turn (the seat's AI stops there), and the
joiner starts from the host's save with its stream hash resuming from the host's value.
`NetplayOptions::joins` runs it: a late joiner is built from `RunSnapshots::save` of the
coordinator's run and compared with the first peer on **every turn it ran, every channel**, and
must reach the end. `test_netjoin.cpp` has the rules and codecs, joiners through loss, one seated
during a speed change and one that leaves again; `imconform netjoin` does it on real sessions with
the session's sink, so the takeover and the hand-back really run, and the unnetworked replay
applies them too; `engine/net/tests` joins a match of three over real sockets; and
`tests/test_corpus_udp.py` and `test_corpus_app_net.py` play four processes -- host, stayer, leaver,
late joiner -- and require the late joiner's per-turn hashes (`trace from K:`) to equal the host's
from its first turn, through loss, through a speed change agreed for the turn before it, and in
the app through the shipped screens.

**Nor is talking.** Chat (`sim/netchat.hpp`) rides the link datagram beside the
turn packets and the lobby's roster and choice, and never reaches the negotiator.
`NetplayOptions::chat_every` makes every peer talk through a lossy star:
every line must arrive once and in its speaker's order, and every per-turn hash,
the schedule and the agreed orders must equal those of the same match played in
silence.

**Looking is not an input.** A peer's interface runs scripts no other peer
runs: every refresh of the command bar runs each row's `groupverifier`, the info
bar runs its own, and the cursor resolves a default order through the `verify=`
scripts. `imconform observe` runs two sessions from one seed and one local player
and watches only one of them, selecting, describing both bars and resolving the
cursor for real every turn, then requires every per-turn hash to match. An
injected verifier that draws from the RNG is caught at turn 1. So a screen is
free to look, and the command bar can ask `CommandBar::check` at the click
without the click becoming an input before its turn.

**The nine `Logs/*/desync.txt` files are not this.** Each is one divergence event
plus a world snapshot, with no command payloads at all. They specify the state
vector; they are not an oracle for a replay, and an API implying otherwise would
be built on a misreading.

## Running it

```
imconform self  [turns] [turn-length]     # the synthetic scenario; no game data
imconform trace [turns] [turn-length]     # print its trace, in golden-file form
imconform map   <game-dir> <map.bfhp> [options]
imconform lockstep <game-dir> <map.bfhp> [turns] [length]
imconform netplay  <game-dir> <map.bfhp> [turns] [delay] [net-seed]
imconform netjoin  <game-dir> <map.bfhp> [turns] [net-seed]
imconform observe  <game-dir> <map.bfhp> [turns]
imconform udp-host <game-dir> <map.bfhp> [--port P] [--turns N] [--delay D] [--drop M]
                   [--peers N] [--silence MS] [--wait-join-at N] [--speed-on-join SPEED]
imconform udp-join <game-dir> <map.bfhp> <host:port> [--drop M] [--leave-after N]
                   [--crash-after N] [--silence MS] [--speed-at K:SPEED]

  --turns N        turns to run (default 50)
  --length N       game-time units per turn (default 800)
  --seed N         world seed (default 1)
  --golden PATH    compare the run against this trace
  --record PATH    write the run's trace here instead of comparing
  --no-scripts     run the systems with the script VM switched off
```

`--no-scripts` is the first bisection to reach for: if a map is deterministic
without the VM and not with it, the divergence is in the VM. `session.hpp` calls
a null script resolver a legitimate configuration for exactly this reason.

`imconform self` needs no installation and runs in CI on a machine that has
never seen the game. It proves the *harness* works. Only `imconform map` proves
the *simulation* is deterministic, which is the thing anybody actually cares
about.

## A note on tests that pass for the wrong reason

Every check in `test_conformance.cpp` appears twice: once proving it passes on a
correct scenario, once proving it **fails** on a scenario broken in exactly the
way that check exists to catch. The second half is the one that matters. A check
that has never been observed to fail is not a check.

Three of the guards are there because the obvious version of this harness would
have been green and worthless:

* `the_reference_scenario_actually_simulates_something` — a scenario whose state
  never changes is trivially self-consistent, trivially partition-invariant and
  worth nothing. It asserts every single turn moves the hash, and that a
  different seed gives a different simulation.
* `partition_invariance_refuses_to_compare_unequal_intervals` — the way this
  check would silently pass is by comparing two runs that were never the same
  interval. Caught before either one runs.
* `a_pass_that_ran_no_checks_is_not_a_pass` — `Report::ok()` is false when
  `checks == 0`, and a schedule whose total cannot be repartitioned from the
  observed turn lengths is reported as a failure rather than skipped. A check
  that quietly does not run is the failure mode this whole harness exists to
  prevent.

One further trap, met while building it: `tiny_trace()`, the hand-built fixture,
originally left `hash_of_hashes` at zero — a trace no real run could produce.
Round-tripping it through the format, which recomputes that field, then
"diverged" for a reason that said nothing about the harness. Hand-authored
fixtures have accused correct readers in this project four times; that was the
fifth in miniature, and it is why the fixture now derives the field exactly as
`World::hashes()` does.
