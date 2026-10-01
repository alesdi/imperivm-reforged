# Joining a running match, changing speed mid-match, and losing a peer

**Status:** research, then built. The original's behaviour is read from `gbr.exe` and the retail
install. The proposal at the end is kept as it was written; the speed, the departure and **the
late join are now built**, and [the last section](#the-late-join-as-built) records the late join
as it was built and where it departs from the proposal. All of the late join is this engine's.
**Companion:** [`state-vector.md`](state-vector.md) (what the original hashed, and the claim that
`cmdidseed` and `syncseed` belong in a join handshake), [`tick.md`](tick.md) (turn lengths and
`gamespeed`), [`conformance.md`](conformance.md) (`check_netplay`, and what peers compare).
**Code this is about:** `engine/core/include/imperivm/core/sim/lockstep.hpp`, `netlobby.hpp`,
`netcmds.hpp`, `netlink.hpp`; `engine/net/include/imperivm/net/match.hpp`;
`engine/app/netplay.hpp`. Wire protocol at the time of writing: **`kNetProtocol = 3`**, turn
packet version 2.

Part 7 has four open items. This document answers four questions for two of them, joining a match
that is already running and negotiating `SetSpeed`, and records what the executable does when a
peer disappears, for the third (dropping a departed player). It answers them from the executable
rather than guessing:

1. Does the original let a player join a match in progress, and if so, what does the joiner receive?
2. How are `cmdidseed` and `syncseed` produced and used, and what else must a joiner hold to hash
   identically from its first turn?
3. How does the original change game speed in a networked match: who may, how it is agreed, and how
   every peer applies it on the same turn?
4. What does the original do when a peer disconnects mid-match: who takes the seat, and how the last
   turn is agreed?

The short answers are: **no**; **settings at the start, then the save**; **a command in the stream,
from any player, unless the match fixed the speed**; and **the computer takes the seat, on a turn
that every peer reaches by being stuck on it**. The rest of this document gives the evidence.

## How to read the evidence

Every claim is tagged with one of these labels:

* **Read.** The behaviour was followed instruction by instruction at the cited address with
  `tools/re/gbr.py` or `tools/re/hostregs.py`, or read from the named retail file.
* **Inferred.** It follows from something read but was not itself followed to the end. The
  alternatives are listed next to the claim.
* **This engine's.** A decision, not a finding.

Addresses are virtual addresses in the retail `gbr.exe`. Following the house rule in
[`../legal.md`](../legal.md), nothing here reproduces instructions, and no retail message text is
quoted: messages are paraphrased, and only identifiers (class names, INI keys, serialisation tags)
appear verbatim, as they do in the rest of `docs/engine/`.

## The machinery the four answers share

The original has three layers. Knowing which layer does what explains most of the answers.

### The transport: a custom UDP lockstep, not DirectPlay's session

**Read.** The strings list both DirectPlay 8 error names and a transport of the original's own. The
transport's configuration is the `[Network]` section of the player's `config.ini`, which is
LZIS-compressed; `src/imperivm/formats/lzis.py` reads it. The reader starts at `0x004072f0`, and
the keys map to globals as follows:

| key | where it lands | retail value |
|---|---|---:|
| `MinTickSize`, `MaxTickSize` | the transport object, `+0x3f0` / `+0x3f4` (built-in defaults 100 / 2000 at `0x00407282`) | 200 / 800 |
| `TransPort` | `0x0081a2f8`, the UDP port | 40447 |
| `DelayPercent`, `LambdaPercent`, `LambdaDelayPercent`, `BoundPercent`, `TickPercent` | `0x0081a2fc` … `0x0081a30c` | 20, 50, 50, 80, 130 |
| `mSecondsBeforeQuitInitial` | `0x0081a310` | 90000 |
| `mSecondsBeforeQuitSubsequent` | `0x0081a314` | 30000 |
| `MinNackTime` | `0x0081a318` | 100 |

`MinTickSize`/`MaxTickSize` are **200 and 800**, which are exactly the extremes `tick.md` observed
in the dumps (`kMinObservedTurnLength`, `kMaxObservedTurnLength`). The negotiator's bounds were
labelled as observations used as limits. They are the original's configured limits.

**Read.** The transport object is built **once**, when a multiplayer game starts.
`CVXGameMultiplayer`'s start override at `0x005504b0` runs the common game start (`0x00524460`),
then constructs the transport (`0x004071b0`, called from `0x0055053e`). The constructor gets three
things from the game settings object: the eight peer addresses at settings `+0xdc8`, the local
seat at `+0xde8`, and the command pump at game `+0x1054`. The transport's per-peer receive state is
an array of **eight seats × three turns** (a 0x20-byte slot per seat per turn, indexed by turn mod
3). That is where the "not more than two turns ahead" window comes from.

The transport has two masks, and they matter for questions 1 and 4:

* **Connected mask.** The session byte at `+0x108`, written at `0x00403fa9`, is set only when the
  session is constructed. It is **cleared** in two places and set nowhere else: the timeout split
  (`0x004085e1`) and the graceful quit (`0x004e6169`). `IsMultiplayer` (`0x004c6500`) is "any bit
  set".
* **Start mask.** The transport's byte at `+0x400` is written once, at `0x004076bf`. It records who
  was in the match when it started.

**Read, and it corrects `tick.md`.** The tick length is computed per tick in the function that ends
in the `AdvanceTick` log line (`0x00407bd0`–`0x00407cf0`, log at `0x00407eba`). Every peer reads the
same values: the u16 timing samples carried in every seat's packet for that tick. It takes the
largest, scales it by `TickPercent`, smooths it exponentially against the previous length with
`LambdaPercent`, and clamps it to `[MinTickSize, MaxTickSize]`. A second quantity, the delay, is
computed from the same samples with `DelayPercent` and `LambdaDelayPercent` and clamped to
`[0, length/2]`. **The original's length has memory** (the previous length at transport `+0x70`, the
previous delay at `+0x74`). This engine's rule is "largest proposal, clamped", which has none.
**Inferred:** the samples are round-trip or latency measurements. The code that fills them was not
followed.

### The command pump: every player action is a serialised command

**Read.** Player actions that change the simulation are `CmdBase` objects. The class-name table at
`0x0081b278`–`0x0081b2c8` names them: `CVXCmdImmediate`, `CVXCmdCancelCmd`, `CVXCmdSetSpeed`,
`CVXCmdSelFilter`, `CVXCmdSelAdd`, `CVXCmdSelRemove`, `CVXCmdSelShortcut`, `CVXCmdSelSetNum`,
`CVXCmdUseItem`, `CVXCmdGiveItem`, `CVXCmdToggleFog`, `CVXCmdSetFog`, `CVXCmdSummonObject`,
`CVXCmdMessage`, `CVXCmdUIMultiple`, `CVXChatMessage`, `CVXDiplomacyChange`,
`CVXPlayerWantsToQuit`, `CVXNetworkSplit`, `CVXCmdPlayerSurrender` and `CVXCmdSkillPlus`, with
`CVXCmdBase`/`CVXCmdUser`/`CVXCmdUI`/`CVXCmdDefault` after them. Each class's vtable has six slots:

* a class id;
* an is-kind-of test;
* a destructor;
* "set issuer" at `0x004e78d0`, which stores the issuing player's record and hands the command to a
  global sink at `0x009969dc`;
* execute;
* serialise.

The base serialiser `0x004e78f0` writes the section `CmdBase` and the issuer's seat as `playerid`.

**Read.** A locally issued command goes through `0x0051d3c0` into the pump's **local** queue
(`0x004f6e30`, one list per issuing seat at pump `+0xf4`). The pump also has a **net** queue: the
lists at pump `+0x4`/`+0x8`, filled through `0x004f6d50`. The desync dump's "Local Commands" and
"Net Commands" are these two.

**Read, and it settles an open question in `state-vector.md`.** The pump's tick (`0x004f79d0`)
executes the net queue as follows:

* It walks **sixteen lists in seat order**, and each list first-in-first-out.
* For every command the sink accepts, other than `CVXCmdImmediate`, it folds the `netcmds` hash:
  rotate the running value left by one, then XOR it with the command's `+8` field shifted left by
  eight and XORed with the issuer's seat (`0x004f7b73`–`0x004f7b92`, into `0x009a7068`).
* It logs `Cmd: <name>` for a UI command, and otherwise `CmdNotUI, idx:` followed by that same `+8`
  field.
* It executes the command and counts `cmdsprocessed`.

`0x009a7068` is the `netcmds` channel: the `[HASHES]` printer at `0x00528900` reads the eight
channels from `0x009a7054` (`slots`) to `0x009a7070` (`aihash`) in dump order.
`hashofthehashes` is the XOR of the eight.

So the original's execution order is **by issuer seat, then by issue order**, which is the rule
`netcmds.hpp` chose and labelled as this engine's. The dumps' `idx` is **not** the seat: it is the
command's `+8` field. The constructor sets that field to −1, and it is filled later by code that was
not followed (see the unknowns).

### The game settings object: what every peer starts from

**Read.** A match is built from a `TVXGame*Settings` object that every peer holds (the current one
is `0x00a87c48`). Its serialiser (`0x005bdb60`, then `0x005bdd40`) names the fields:

* `basicgamesettings`: `gamepath`, `animatedecors`, `random`, `rmsettings`, `playerdata`;
* `gamedata`: `worldpop`, `startinggold`, `gamespeed` (`+0xd98`), `fogofwar`, `exploration`,
  `season`, `difficulty`, `loadgame` (`+0xdac`), `sizex`, `sizey`;
* `gamesettings`: `colorize`, `normalseed` (`+0xdc0`), `syncseed` (`+0xdbc`), `playdemo` (`+0xdc4`).

`0x005bde80` answers "is this a replay being played back": the settings are the single-player kind
with `playdemo` set. Several of the code paths below branch on it.

## 1. Joining a match in progress

**Answer: the original does not allow it.** Nothing in the executable admits a peer after the
match starts, so the join design below has no original to reproduce.

The evidence, all **read**:

* **Membership is fixed when the transport is built.** The only write that sets connected-mask bits
  is the session constructor, called once from the game start at `0x0055053e`. Afterwards the mask
  only loses bits, at `0x004085e1` and `0x004e6169`. The start mask is written once, at
  `0x004076bf`. There is no code path by which a ninth, or a returning, address enters a running
  session.
* **No text for it.** The executable has no string for joining late, rejoining, reconnecting,
  spectating or observing. The lobby's refusals are "different protocol version" and "different
  game version" (`0x006e302c`, `0x007036e8`), plus the firewall one. The in-game messages about
  players concern leaving.
* **The one "has joined the game" message is GameSpy's, at the start.** `0x004058c0` prints a
  player's name and GameSpy level when the Online Battle match is assembled. Its only caller is
  `0x00550c04`, next to the message that some players could not connect to the Online Battle game.
* **The lobby list distinguishes an open game from a started one.** It shows one status text for a
  game that is open to join (`0x006e0f8b`) and another for one that is starting (`0x006e0cac`).
  **Inferred:** a started game is listed but not joinable. The click handler that would refuse it
  was not followed.
* **Loading a save does not go through the network.** The `loadgame` flag is raised only for the
  duration of the single-player load path (`0x004d18bf` sets it, `0x004d1905` clears it). No
  multiplayer screen sets it.

So a joiner in the original receives nothing mid-match. What every peer receives **before** the
match is the settings object above, which includes the two seeds (next section). This is also the
closest thing the original has to a joiner's payload. **A replay is settings plus the command
stream.** The `playdemo` path in `0x004f79d0` executes recorded commands in the same seat order
without the transport, and the save and replay loaders are separate. **Inferred:** a replay file is
that settings object followed by the serialised commands. The replay writer was not followed.

## 2. `cmdidseed`, `syncseed`, and what hashes

### `syncseed`: the synchronised generator's state

**Read.**

* **Two generators.** The game object holds two random generator objects, the synchronised one at
  game `+0x12a0` and a second one at `+0x12b4`. At game start (`0x00524522`–`0x0052454e`) the first
  is seeded from settings `syncseed` and the second from settings `normalseed`.
* **What the dump prints.** The desync dump's `[SEEDS]` block (`0x0051e650`, format at
  `0x007beeb0`) prints `cmdidseed` from game `+0x24c` and `syncseed` by asking the first generator
  for its state (its vtable slot `+0x10`). So `syncseed` in a dump is the generator's **current**
  state, not the value it was seeded with, which is why the nine dump values look unrelated.
* **The save stores both states.** The game object's save writes both generators' states under the
  tag `seed`: sync first, then normal (`0x005258b1`–`0x0052593f`). Loading re-seeds both.
* **Where a networked match's seeds come from.** The LAN/Internet lobby (`0x006dbcdf`,
  `0x006dbceb`) sets `normalseed` to **zero** and `syncseed` to a field of the lobby's game record
  (`+0x38`). The GameSpy Online Battle setup (`0x0070295b`, `0x00702961`) does the same. The
  settings constructors' defaults are 111 and 22 (`0x0059732c`/`0x00597336`, and the same pair
  at `0x005be4a4`/`0x005be4ae`). One other path
  (`0x0074c1c1`–`0x0074c25a`) seeds the C runtime from the time and draws both seeds from `rand()`.
  It also sets the speed to 100000, the clamp maximum. **Inferred:** that is a test or benchmark
  mode, not a networked one.
* **Not read:** how the host fills the lobby record's seed field.

### `cmdidseed`: the unit-command id counter, not the network's

**Read.** `cmdidseed` is game `+0x24c`:

* it is reset at game start (`0x00524551`);
* it is saved in its own section, `CmdIdSeed`, under the tag `seed` (`0x005257c2`–`0x005257ec`);
* it is advanced by a single allocator (`0x0051d4b0`, post-increment), whose **only** caller
  (`0x00599302`) stamps the id into a newly constructed unit command-queue entry.

This confirms `state-vector.md`'s "same counter family" claim and sharpens it. `cmdidseed` counts
**simulation** command-queue entries, not network commands. It must agree across peers because
`CVXCmdCancelCmd` names its target by that id (its serialiser writes `cmdid`, at `0x004e56d0`). This
engine keeps the counter as `World::next_command_id` (`world.hpp`), in the save and in the state
hash.

### What a joiner would need to hash identically from its first turn

The original never needed to answer this. The list below is assembled from what it hashes and
saves. The source of each item is marked.

| item | original's home | why it matters | in this engine |
|---|---|---|---|
| object slots, script threads | the `slots`/`threads` channels | hashed | `World::hashes()`; in the save |
| synchronised generator state | game `+0x12a0`, saved | every `rand()` | `World::rng()`; in the save |
| `cmdidseed` | game `+0x24c`, saved | unit-command ids; `CancelCmd` names them | `World::next_command_id`; hashed and saved |
| the clock: time, turn window, **speed** | clock at game `+0x1238`, speed at clock `+0xc` = game `+0x1244` | the next turn's game-time length | `TickConfig`; world state, hashed (`world.cpp` hashes `game_speed`) |
| the `netcmds` running value | `0x009a7068` | it is a running fold, so a joiner starting mid-stream needs the value so far | **not** world state here: `stream_hash` over the negotiator's history |
| pending net commands for future turns | pump net queue | orders already agreed for turns the joiner has not run | the negotiator's pending packets |
| tick-length memory | transport `+0x70`/`+0x74` | the original's next length depends on the previous one | none (the "largest proposal" rule is memoryless) |
| membership and who controls each seat | connected and start masks, per-seat computer controller | who must send, and who the computer is playing | `LockstepConfig::peers`; the session's AI seats |
| the normal generator | game `+0x12b4`, saved | **Inferred:** cosmetic only; it is not a hashed channel | none known |

## 3. Changing speed in a networked match

### `SetSpeed` is a command, and so is everything that changes speed

**Read.** `hostregs.py`:

| entry point | body | signature |
|---|---|---|
| `SetSpeed` | `0x004c6ce0` | `void, int speed` |
| `GetSpeed` | `0x004c6220` | `int` |
| `TogglePause` | `0x004c6680` | `void` |
| `Pause` | `0x006a45d0` | `void` |
| `IsMultiplayer` | `0x004c6500` | `bool` |

* **`SetSpeed`** does **nothing if game `+0x230` is set**. Otherwise it constructs a
  `CVXCmdSetSpeed` (vtable `0x007b806c`), gives it the **local player** as issuer, stores the
  requested speed at `+0xc`, and posts it through `0x0051d3c0`, the ordinary local-command path.
  **It changes no state directly.**
* **`CVXCmdSetSpeed`'s execute (`0x004e67c0`)** clamps the speed to **[1, 100000]** and writes it
  to game `+0x1244`, which is the clock's speed field. In a `CVXGameMultiplayer` game whose
  settings list more than one peer address (`0x006cc1c0` counts the non-zero entries among the
  eight at settings `+0xdc8`), it then prints a message naming the issuer and the new speed as a
  percentage: speed × 100 / `[VXTIME] GameSpeed`, the CONST.INI constant that `tick.md` reads as
  1000.
* **`CVXCmdSetSpeed`'s serialiser (`0x004e5740`)** writes the section `CmdSetSpeed` with the field
  `speed` after the base's `playerid`.
* **`GetSpeed`** returns game `+0x1244`.
* **The clock applies it on the next window.** `0x00528a80`, called by the transport after the pump
  has run the tick's commands (`0x00407fe7`, after `0x00407f82`), computes the next turn's
  game-time end as the negotiated real length × speed / 1000, and only when the game is not paused.
  That is `tick.md`'s `floor(800 × 999 / 1000) = 799`, now read rather than inferred. A speed
  command executed during turn *t* therefore changes the length of turn *t + 1*, on every peer,
  because every peer executes the same commands in the same order before computing the same
  window.

**Read.** Three more places construct `CVXCmdSetSpeed` (vtable references at `0x004c669d`,
`0x004e805e`, `0x006e8067`):

* **`TogglePause` (`0x004c6680`).** It pauses by sending speed **1**, after remembering the current
  speed in the global `0x00996850`. It unpauses, when the speed is 1, by sending the remembered
  speed. So a script-driven pause is a speed command.
* **The options screen (`0x006e7ff0`).** When options are applied during a game **and the match
  speed is variable**, it sends `700 + option × 2301 / 100` (`0x006e8085`–`0x006e80a4`), where
  `option` is `Settings.ini`'s `[Options] GameSpeed`. When the match speed is fixed, the slider is
  disabled (`0x006e90b0`). The player's own `Settings.ini` has `GameSpeed=13`, which gives
  **999**, the one off-speed dump. That dump was a player at the default options position, not a
  negotiated speed.
* **The command factory (`0x004e805e`).** It deserialises the command from the stream or a replay.
  **Inferred** from its place among the other classes' factory references.

**This corrected the app, and the app now follows it.** `engine/app/main.cpp` read `GameSpeed`
as a 0..25 turn-interval knob, with 13 as normal and a labelled guess. The executable treats it
as a speed position that maps linearly to per-mille speed, with 13 → 999, and the start of a
match writes the position back from the speed it starts at (0x006e71e0: 1000 → 13), so the
shipped 13 is `NormalSpeed` on its way back. Both directions are `sim::game_speed_from_option`
and `sim::option_from_game_speed` in `sim/tick.hpp`, the one place either formula is written;
`tick.md` ("Where the 999 came from") has the whole round trip. The slider's range was not
read; **inferred** as 0..100 like the volume sliders beside it, which spans 700..3001, `SlowSpeed`
to CONST.INI's `Speed5` 3000 within a unit.

### Who may change it

**Read.**

* **The match decides whether speed is variable.** At game start (`0x00526833`–`0x005268cf`), a
  settings `gamespeed` of **−1** means variable: the starting speed comes from the `[GamePlay]`
  `NormalSpeed` key, and game `+0x230` stays clear. Any other value fixes the speed: game `+0x230`
  is set, and the value is clamped and used. CONST.INI declares `VariableSpeed = -1` beside
  `SlowSpeed`/`NormalSpeed`/`FastSpeed`/`FastestSpeed` (700/1000/1400/2000). The LAN/Internet
  lobby's record defaults to −1 (`0x006dc74c`). GameSpy's Online Battle forces `FastSpeed`
  (`0x007029bc`–`0x007029ea`).
* **Any player, when it is variable.** Nothing on the issuing path (`0x004c6ce0`, `0x006e7ff0`) or
  the executing path (`0x004e67c0`) checks for the host. The issuer is whoever pressed the key or
  applied the options. **The executing side has no gate at all**: a `CVXCmdSetSpeed` that arrives
  is applied even in a fixed-speed match, because the fixed-speed check is only on the issuing side.
* **The shipped call sites.** `DATA\SCDEBUG.XML` binds keypad `+` and `−` to step up and down
  through CONST.INI's `Speed1`–`Speed5` (700, 1000, 1400, 2000, 3000) with `GetSpeed`/`SetSpeed`.
  It binds keypad `*` to toggle 10000, but only when not `IsMultiplayer()`, and binds `Pause` to
  `Pause()`. The four other occurrences, in `DATA\AI\AIOSENDSQUAD.VS` and `SQUADMONITOR.VS`, are
  commented out. Nothing in the campaign containers calls it (`vs-host-api.md`'s census agrees).

### Pause in multiplayer is a different mechanism

**Read.** `CVXGameMultiplayer` (vtable `0x007c2858`) overrides pause, unpause and toggle
(`0x005503b0`, `0x00550400`, `0x00550450`). None of them touches the speed. They set a pending
request on the transport (`+0x3f8`, `+0x3fc`). The request travels as a flag bit in this seat's
next tick packet. When a tick is advanced, the transport collects every seat's flag for that tick
into a mask (`0x00407ced`–`0x00407d9b`), toggles the game's pause state, and prints which players
paused or unpaused (`0x00406320`). A pause is therefore **agreed on a tick** like a command, but it
is carried by the transport rather than the pump. It is refused after the game is over (unless in a
replay), and in a GameSpy game whose flag at `+0x80` is set (`0x00712c80`). **Inferred:** that flag
means a rated or Online Battle match.

## 4. When a peer disappears

Another agent is implementing this now, so this section records evidence only.

### Two ways out, and one routine that takes the seat

**Read: the graceful leave.** Quitting a networked game (`0x006b47d0`–`0x006b486a`) opens
`MenuIni/EndMultiplayer.ini`. That dialog tells the player to wait until the others know they have
left, and offers to exit at once. The quit also posts a `CVXPlayerWantsToQuit` (vtable
`0x007bbcd0`) through the ordinary command path, with the fields `player` and `player_to_quit` both
set to the local seat (serialiser `0x004e6180`). The quit is therefore **a command in the stream**,
executed at its agreed turn on every peer. Its execute (`0x004e60c0`) does the following:

* **Replay.** It hands `player_to_quit` to the computer directly, if that seat is a human seat with
  no computer controller yet.
* **Live, on the quitter.** The quitter executes it too, and when `player` is the local seat it
  calls the game's leave routine (vtable slot `+0x68`, `0x005504a0`).
* **Live, everywhere else.** It clears the quitter's bit from the connected mask, then runs the drop
  routine below.

**Read: the timeout, "an attempt to split the network".** The transport's wait state
(`0x004083ef`–`0x0040849c`) measures how long it has been stuck on the current tick. It allows
`mSecondsBeforeQuitInitial` (90 s) while the tick number is **≤ 5** and
`mSecondsBeforeQuitSubsequent` (30 s) after that. **Inferred:** the longer allowance covers peers
still loading the map. While it waits, the "waiting for player" message counts the seconds
(`0x00405f56`–`0x0040603b`): at 3, 6 and 11 seconds, then every 5 seconds after 15.

On expiry it runs the split (`0x00408520`) for the stuck tick *t*. For every seat in the connected
mask whose slot for *t* holds no data, the split:

1. discards that seat's buffered data for *t*;
2. removes the seat from the connected mask;
3. if the seat is a human seat with no computer controller, adds it to a `forget_mask`.

If the `forget_mask` is non-empty, the split constructs a `CVXNetworkSplit` (vtable `0x007ab6f4`,
field `forget_mask`, serialiser `0x004e61f0`) and puts it **straight into this peer's own net
queue** (`0x004f6d50`). **It is not sent.** The split then re-checks whether tick *t* is now
complete with the reduced membership (`0x004080d0`, type 0) and, if so, advances it. The split
command's execute (`0x004e68c0`) calls the drop routine too. In a replay, it hands each seat in
`forget_mask` to the computer directly and plays the dropped-player sound.

**Read: the drop routine (`0x00406840`).** It walks seats 0..7. For each seat that meets all four
conditions:

* the seat is not local;
* it is a human seat (player record `+0x290 == 1`);
* it is absent from the connected mask but present in the start mask;
* it has no computer controller yet (player record `+0x88 == 0`);

the routine does three things, and a fourth once for the whole walk:

1. prints that the player dropped, with the player's name: the string `Player %s1 dropped`
   passes through the translation table (the object at `0x00a77e94`, `0x00689c90`) with the name
   as `%s1`, and goes to the in-game message list;
2. **appends "(AI)" to the player's name** (a string append at `0x00406980` onto the name at
   player record `+0x90`). The appended text is the table's translation of `(AI)` with an empty
   context (`0x00689130`); the shipped table has the entry, commented "Added to names of AI
   players" (the Italian pack's result is `(IA)`). It is appended verbatim, with no separator,
   after the message has printed the name without it. This routine is the only reference to
   the `(AI)` string in the executable, and nothing removes the marker;
3. **gives the seat to the computer** by calling `0x00434ca0`, the body behind `AIStart(player)`
   (`hostregs.py`: `AIStart` at `0x00434e20` calls it), with no profile and no difficulty. That
   body destroys any existing controller in player record `+0x88` and allocates a new one;
4. after the walk, **if it handed over at least one seat**, plays `Sounds/UI/PlayerDropped.wav`
   once (`0x006b0910`, with arguments whose meaning, a volume and a category among them, was not
   traced).

This engine does all four (`sim/netdepart.hpp`, *What the players are shown*), with the marker
kept as presentation beside the match rather than in the world's player names, so it is in no
hash and no save. A late join, which the original does not have, hands the seat back and takes
the marker off, with no sound.

The exception is when the GameSpy object's flag is set and the game state (game `+0x1264`) is at
least 2. The routine then calls the game's vtable slot `+0x48` (`0x00524090`) with the seat and 1
**instead of** starting the computer. **Inferred:** that call records the player as defeated, as a
rated match should. The routine evaluates end-of-game state, and the executable carries a rating
penalty message for disconnecting.

### How the last turn is agreed

**Read, then inferred.**

* **A graceful quit** is agreed exactly like any command. Every peer executes the quit on the same
  turn, and from then on the connected mask excludes the quitter, so no later tick waits for it. The
  quitter's own execution is what lets it leave. The dialog's "exit now" gives up waiting for that.
  **Inferred:** a quitter who takes it is then handled by the others' timeout.
* **A timeout** is agreed **implicitly**: every surviving peer is stuck on the same tick *t*,
  because lockstep cannot pass *t* without the missing packet. Each peer times out independently,
  drops whoever it lacks data from for *t*, runs *t* without their orders, and executes its own
  locally inserted split command in that tick's pump. The seats change hands on every peer at the
  same tick because every peer is stuck at the same place.
* **The hole, and what the original does about it.** If peer A holds the vanished player's packet
  for *t* and peer B does not, A will not drop that player at *t* and B will. The transport tries to
  close the hole: it has negative acknowledgements "regarding" a third party (the log format at
  `0x007abc68`), which reads as one peer asking another for a third peer's data. **Inferred:** the
  purpose is exactly this repair. It narrows the hole but cannot close it, since nothing re-checks
  agreement. The log line's own name for the operation, an *attempt* to split the network, suggests
  the authors knew. The dumps cannot say whether any of the nine desyncs came from it.

### What this means for the departed-player work

The work is in progress elsewhere; these are the facts it can lean on.

1. **The original never ends the match for everyone.** The seat passes to the computer through
   `AIStart`'s body with no profile, and the player's name is marked "(AI)". A rated Online Battle
   match is the exception: there the player is (inferred) declared defeated.
2. **The drop is itself an event executed in the pump**, on every peer, at the tick being completed.
   A graceful leave is a command in the stream. A timeout is a locally synthesised command that
   every peer synthesises for the same tick.
3. **Only human seats that were present at the start and have no computer controller are handed
   over**, and never the local seat.
4. **The timeouts are 90 s for ticks ≤ 5 and 30 s after.**
5. **The original's agreement on the last turn is "the tick everyone is stuck on".** That is not a
   proof of agreement, and this engine can do better, because its link layer already relays every
   packet and acknowledges it per origin.

## What is still unknown

* **What fills a command's `+8` field**, the `idx` in the dumps' `CmdNotUI` lines and the value the
  `netcmds` fold uses. It is −1 at construction. Candidates are the sink at `0x009969dc` (its vtable
  slot `+0x28` is called from "set issuer", and its slot `+0x30` decides whether the pump hashes a
  command) and the transport on receipt. Until that is read, the original's `netcmds` fold is known
  in form but cannot be reproduced.
* **What the sink at `0x009969dc` is.** It is created in the pump's setup (`0x004f6247`), is called
  for every command, and can veto hashing. A replay recorder and an id registry are both consistent
  with that.
* **The initial value of `netcmds`** and where `0x009a7068` is reset.
* **How the host draws the lobby's `syncseed`** (the lobby record's `+0x38`).
* **What the tick-length samples measure.** They are u16 values in each seat's tick packet. The
  producer was not followed. So neither the `sug.` and `del.` quantities in the `AdvanceTick` log
  nor the use of the computed delay is fully read.
* **Whether a started game is refused at the lobby's click, or only labelled.** Only the label was
  read.
* **What `0x00524090` does for a dropped seat in a rated game**, beyond evaluating the end of the
  game. "Declared defeated" is inferred.
* **The options slider's range** for `GameSpeed`. Only the mapping from the stored value to a speed
  was read.
* **Whether the normal generator (game `+0x12b4`) ever touches hashed state.** It is seeded to zero
  on every networked peer, so it would not desync even if it did.
* **The replay file's layout.** Settings plus commands is inferred from the `playdemo` paths; the
  writer was not followed.

## A proposed design, fitted to protocol 3

**Everything in this section is this engine's.** The original offers nothing to reproduce for the
join, and offers a mechanism with known weaknesses for speed. The goal is the smallest change to
`lockstep.hpp`/`netlobby.hpp`/`NetMatch` that keeps the property the current protocol is built on:
**every change to what a turn means is itself agreed in the stream, and applied at a turn number
every peer computes**. Both features bump `kNetProtocol` to **4** and the turn packet to version
**3**.

### Speed: an order kind, applied at apply time, effective next turn

This follows the original: a command from any player, executed in stream order, changing the next
window.

1. **A new `NetOrderKind::speed`** carrying an `int32` per-mille value. Its wire form adds an
   explicit `i32 value` after the order's kind byte. It does not reuse `target`, because a field
   that means two things is the kind of ambiguity the decoder exists to refuse. Actors: none.
2. **Who may.** Any seated human, as in the original. The `Start` gains **`speed`**: `-1`
   (`kVariableSpeed`, CONST.INI's own value) or a fixed per-mille. The lobby's default is `-1`, as
   the original's is (`0x006dc74c`).
3. **Refused on every peer alike, not only at the issuer.** With a fixed speed, `apply_turn` drops a
   speed order and reports it. This deliberately departs from the original, whose executing side
   does not check. A hostile or out-of-date peer must not be able to change a fixed-speed match. The
   refusal is deterministic, so it cannot desync.
4. **Applied in canonical order, last wins.** `apply_turn` clamps to `[1, 100000]` (the original's
   bounds) and calls `World::set_game_speed`. Two speed orders in one turn resolve by issuer, then
   sequence: the original's seat-then-FIFO order, and the one `netcmds.hpp` already uses.
5. **Effective from the next turn.** The negotiator converts real milliseconds to game time in
   `take()`. Today it uses the constant `LockstepConfig::game_speed`, and `lockstep.hpp` notes that
   speed changes are not handled. It gains `set_game_speed(int32)`. The caller calls it after
   `apply_turn(t)` and before `take()` of *t + 1*, and `check_netplay` does the same, so the
   unnetworked comparison stays exact. Because the speed is already `TickConfig` world state and
   hashed, a peer that applied it on the wrong turn is caught on that turn.
6. **Pause.** The original pauses a networked game with a transport flag, and pauses a scripted one
   with speed 1. The smallest faithful option here is a separate `NetOrderKind::pause` toggle,
   applied like speed. Speed 1 is the other option, and it would also make "unpause" restore a
   remembered speed that is per-machine in the original (`0x00996850`) and would have to become
   world state. This is out of scope for the speed item, and noted because the plan currently
   refuses pause in networked play.
7. **The message.** Print the issuer's name and speed × 100 / 1000 as a percentage, in a match with
   more than one networked seat, as `0x006cc1c0` gates it.

### Join: admitted by an order, effective after the delay, from the host's save

*Superseded in its mechanism by [the late join as built](#the-late-join-as-built): the admission
became a link-layer fact rather than an order, and the joiner starts at the admission turn itself.
The save, the header and the resumable stream hash are as proposed.*

The rule that makes it lockstep-safe is that **membership is a function of the agreed stream**.
This is the same rule the departed-player work needs in reverse, so both should share one mechanism
in `TurnNegotiator`:

    membership(t) = initial peers
                    + admits applied at turns <= t - (input_delay + 1)
                    - drops  applied at turns <= t - (input_delay + 1)

A change applied at turn *J* takes effect at **K = J + input_delay + 1**. That is the first turn no
peer had already scheduled when it learned of the change: after taking *J*, a peer submits for
*J + 1 + delay*.

The sequence:

1. **Ask.** A joiner sends `Hello` as today. `NetMatch` already lets lobby traffic through
   mid-match. The host answers with `Refuse` (`full`, `protocol`, `content`), or with a new
   **`Offer`**: the running match's `Start` with the seat it will get (`you`), `running = true`, and
   the match id. **Which seats are joinable is this engine's choice.** The proposal is a human seat
   whose player departed and which the computer now plays (the departed-player work creates these),
   or a row the host marked "open for late joiners". A joiner that lacks the map leaves, as today.
2. **Agree.** The host submits a **`NetOrderKind::admit`** order `{slot, name}` in its next packet,
   for turn *J*. Only the host's admits are honoured. The host is already the lobby's authority, and
   the check is deterministic because the issuer is restored from the sender on the wire. Every peer
   applies it at *J*. From *K* on, `ready()` requires the slot's packet, and `receive` stops
   refusing that slot's packets as `unknown_peer` for turns ≥ *K*.
3. **Hand over the seat, at *K*, on every peer.** The seat stops being played by the computer at the
   start of *K*: its AI coroutines are ended and the seat is marked human. The original has no
   inverse of `AIStart` to copy, so how the AI is stopped is this engine's. It must be a function of
   the world alone, like everything else applied at a turn.
4. **The joiner's first turns are agreed empty.** At match start, turns `0 .. delay-1` are agreed in
   advance for everyone. In the same way, the joiner's turns *K* .. *K + delay − 1* are agreed empty
   **for the joiner only**, and contribute no proposal to the length. No peer waits for the joiner
   until *K + delay*, and the joiner can submit that packet as soon as it knows *K*, before it holds
   any state.
5. **Send the state.** After applying turn *K − 1*, the host takes `GameSession::save()`: the world,
   the generator, `next_command_id`, the clock with its speed, the scripts and the AI. It streams the
   save as **`JoinState`** chunks of about 1 KiB. The header carries the match id, *K*, the total
   size, a hash of the whole, the `Start` as amended by the admit, and the **`netcmds` running value
   through *K − 1***. A save is a few hundred KiB (`imsave` on Danube at 200 turns is 369 KB), so
   this is a few hundred datagrams. They are resent until the joiner's **`JoinAck`**, a contiguous
   offset, covers them. The chunk layer is separate from the link layer on purpose: it is bulk
   transfer, and its size would break the link layer's per-datagram budget.
6. **Resume.** The joiner builds the session with `create` from the `Start` (the same
   `SessionInputs` every peer used, and the same `data.pak` checked by `Hello`), then `load(bytes)`.
   The save's own hash verification refuses a bad transfer. It starts a `TurnNegotiator` at
   `next_turn = K` and seeds its `stream_hash` with the transmitted value, which needs a resumable
   `stream_hash(seed, …)`, because today's fold starts from the whole history. It links to the host
   only; the host relays. Packets for turns below *K* are refused as `stale`.
7. **Stall, visibly.** Peers reach *K + delay + 1* and need the joiner's packet for it, which the
   joiner can only send after running turn *K*. The match therefore waits for the transfer, as a
   named wait (the original shows who it waits for, and for how long). The timeout that drops a
   silent peer also covers a joiner that never arrives. The joiner is dropped by the same stream
   mechanism, at an agreed turn.

**The alternatives** where this design chose:

* **When to take the snapshot.** At *K − 1* (proposed): simplest, and it costs a stall the length of
  one transfer. At *J* instead: the transfer overlaps turns *J + 1* .. *K − 1*, and the joiner
  replays those agreed turns before joining. The stall is shorter, but the host must keep and
  forward agreed packets back to *J*, and the joiner must run a catch-up loop. Pausing the match
  (agreed, like the original's pause) during the transfer is a third option.
* **Who may admit.** Host only (proposed), or any peer. Any peer adds nothing for a hub topology.
* **What the joiner loads.** A save (proposed), which the project already round-trips on every map
  (`tools/save_sweep.py`). The alternative is the original's replay model: the `Start` plus every
  agreed turn since turn 0, replayed. That needs no snapshot code, but its cost grows with match
  length, and it re-runs the AI for every seat, which is both slow and a proof obligation.
* **The tick-length memory.** Not needed today, because the negotiator's rule is memoryless. If the
  rule ever adopts the original's smoothing (transport `+0x70`/`+0x74`), the previous length must
  join the `JoinState` header.

### What each piece checks, and where it would be caught

| fault | caught by |
|---|---|
| a peer applies a speed order a turn late | the world hash: `game_speed` is hashed, and the next turn's length differs |
| a fixed-speed match accepts a speed order | a test: a peer that refuses and one that does not diverge |
| membership changes at a turn other than *J + delay + 1* | `check_netplay` with an admit in the stream: a stall that never resolves, or a turn run on a guess |
| the joiner's world differs after load | `GameSession::load`'s own hash check, then the per-turn comparison |
| the joiner's `netcmds` restarts from zero | the per-turn `netcmds` comparison in `check_netplay`, extended with a peer that joins |

`check_netplay` can prove the whole join in the core with no socket, as it proves everything else.
A peer that exists only from turn *K*, built from another peer's save at *K − 1*, has to agree turn
by turn with the peers that ran from turn 0 and with the unnetworked record.

## The late join as built

**Everything here is this engine's**; question 1 says the original has nothing to copy. Code:
`sim/netjoin.hpp` (the rules, the header, chunk and acknowledgement, the transfer),
`sim/lockstep.hpp` (stints, `admit`, `ResumePoint`), `sim/netlink.hpp` (admissions and
stint-tagged departures in the datagram, `add_link`), `sim/netdepart.hpp` (`apply_membership`),
`sim/netcmds.hpp` (`joined`, the resumable `CommandStream`), `engine/net/match.hpp` (the host's
seating and the transfer), `engine/app/netplay.hpp`. Protocol **7**, link datagram version **4**;
the turn packet is unchanged at version 3, because nothing a peer sends changed.

### The three decisions the task asked for

* **Which seat: one the computer took over after its player left.** Not an empty open seat: a
  lobby's open row that nobody took is closed when the match starts (`HostLobby::finish`), and a
  closed seat has nothing on the map to play. So a join is exactly the reverse of a departure, on
  the same `PlayerId`, and `config.peers` never changes during a match -- which is what keeps every
  slot index, window and wire field as it was. With several such seats the lowest is given; the
  joiner does not choose.
* **The seat's AI stops** on the first turn of the joiner's part, on every peer: the negotiator
  writes a `joined` order into that turn (never on the wire, like `departed`), and applying it is
  `AIStop`'s body (0x00422720) for the seat, through `NetCommandSink::hand_back`. Using `AIStop` as
  the inverse of the takeover's `AIStart` body is this engine's. Units keep whatever the computer
  last ordered them to do.
* **What the others see:** the match plays on while the save travels. Every peer prints and shows
  (in the corner where chat lines appear) who is joining and from which turn; the host alone knows
  how much of the save has arrived. The match waits only when it needs the joiner's orders, from
  turn *S + delay + 1*, and then the wait is the ordinary lockstep stall.

### How it departs from the proposal, and why

| proposal | as built | why |
|---|---|---|
| an `admit{slot}` **order** in the host's packet for *J*, membership from *K = J + delay + 1* | a coordinator's **fact** "seat *P* plays again from *S*", carried by every link datagram beside the drops; the negotiator writes a `joined` order into *S* | The joiner's first packet is relayed to peers that may not yet have *taken* turn *J*, so an order learned at apply time cannot tell a peer's link layer or negotiator to accept it; the packet would be refused and, having been acknowledged, never resent. A datagram fact precedes every packet that depends on it. It is also the departure's mechanism run backwards: one path for both, and no "only the host's admits count" check on the wire, because no peer can send one. |
| *K = J + delay + 1* | *S = max(next_submit + 1, dropped_from + 1)* on the host | No peer can run *S* without the host's packet for *S*, which the host sends only after deciding, and every datagram carrying it carries the admission. The `+ 1` keeps the joiner's first packet (*S + delay*) past any packet the departed player can still have in flight, which then arrives late rather than as a rival. |
| snapshot after *K − 1*, joiner starts at *K* | the same, with *S* | -- |
| the joiner links to the host; packets below *K* stale | the host's `add_link` re-holds everything its negotiator holds from *S* on -- the link layer may already have forgotten it -- and sends nothing older | A packet every existing link had acknowledged was pruned; the joiner still needs it. |
| membership = initial + admits − drops | per seat, a list of **stints** (`from`, `until`); every departure fact names the stint it is about (`since`) | A seat can be dropped, joined, dropped and joined again; without the tag an old report about the departed player reads as a new one about the joiner, and a joiner would read its seat's old drop as its own. |
| the joiner's turns *K .. K+delay−1* agreed empty | the same, and it sends its first packet from the header alone, before the save arrives | So the others run *S .. S + delay* during the transfer. |
| (not in the proposal) | the chat numbering: the header carries where the host's stands for each speaker; the joiner continues its seat's | Lines are taken strictly in order; a joiner starting from zero would never show a line again. |
| one join at a time (implicit) | explicit: a second joiner is not answered until the first has its save, and none while a departure is undecided | The coordinator would otherwise need the joiner's report on a departure it has no state to make. |

A joiner that never arrives is timed out like any silent peer; its stint is dropped by the ordinary
agreement and the computer takes the seat back on that turn, after which the seat can be joined
again. `NetMatch::set_late_join(false)` refuses every late joiner as `full`.

### The state

After applying *S − 1* the host takes `GameSession::save()` (a Zama save is about 375 KB from
`imconform`, 718 KB from the app) and sends a `JoinHeader` -- the running match's `Start` made the
joiner's (its seat, the host as its only link, the **per-mille speed in force** as
`config.game_speed`), *S*, the save's size and FNV-1a hash, `stream_hash` through *S − 1*, every
seat's stints, each speaker's chat numbering -- then 1 KiB `JoinChunk`s in a window of 64,
acknowledged cumulatively by `JoinAck` and resent every 40 ms until acknowledged. The joiner
builds its session as every peer did and loads the save, whose own hash check refuses a bad
transfer; `CommandStream::first`/`prior` let its `netcmds` fold resume from the host's value, over
the match's turn numbers. The host holds turn *S* (`NetMatch::take` returns nothing) until it has
handed the save over; the joiner holds it until it has loaded it.

### Where each fault is caught

| fault | caught by |
|---|---|
| the joiner's stream hash restarts from the basis, or folds its own positions | `test_netjoin` (the resumed fold), the per-turn `netcmds` in `check_netplay` |
| the admission reaches a negotiator after the joiner's packet | `test_netjoin` (the relay test), `net_tests` |
| a facts about an earlier stint read as the current one | `test_netjoin` (stints), `imconform netjoin` (a seat joined twice) |
| the joiner's first turn not `joined`, or its AI not stopped | `test_netjoin`, the unnetworked replay in `imconform netjoin` and `udp-join`, which applies the sink |
| the save or the speed at *S* wrong | the joiner's per-turn comparison with the host, every channel, in `check_netplay`, `net_tests`, `test_corpus_udp.py` and `test_corpus_app_net.py` |

**Faults injected.** 52 single-point faults across the negotiator, the stream hash, the link layer,
the membership step, the join codecs and transfer, the host's seating and the app sink (one
first-draft mutant touched only a comment and was redone). The first sweep caught 42 of 51. The
nine survivors named six missing tests -- a joiner that has run nothing accepting an end before its
first turn, `hand_back` really stopping the AI, a new link sent nothing from before its first turn
(packets or chat), a second departure keeping the seat shut, a joiner running nothing before its
world loads -- and one real bug: `apply_membership` put a stint's end before its admission when
both arrived together, so a joiner that came and went between two datagrams was irreconcilable; the
mutant that "broke" it was the fix. Receiving a datagram also moved into one core function
(`deliver`) that `check_netplay` and `NetMatch` share, so the ordering it enforces is tested in the
core. Three survive, each said so where it lives: removing `admit`'s open-stint check makes it read
an empty `optional` (undefined, and never reached by an honest caller); not refilling the transfer
window on an acknowledgement only slows the transfer; and moving admissions after departures in the
link layer cannot be told apart.
