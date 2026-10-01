# Sound

**Status:** scoped, and stages 1 and 5 built on top of the first slice: a mixer on SDL3, the
sound entities, the unit's acknowledgement when the player gives an order, the interface's
click, the options, a building's select sound, live sounds following the camera, and the
music, the menus' and the match's, streamed from Ogg Vorbis through the vendored
`stb_vorbis`. Combat, deaths, walking, gates, ambience, notifications and conversation speech
are the stages [at the end](#the-rest-in-stages).
**Code this is about:** `engine/sound/` (decoders, entities, mixer, music; no SDL),
`engine/third_party/stb_vorbis/` (the Ogg Vorbis decoder, [`../legal.md`](../legal.md)),
`engine/platform/src/audio.cpp` (the device), the sound hooks in `engine/app/main.cpp`.
**Companion:** [`../formats/sc-xml.md`](../formats/sc-xml.md) for the class files whose
`<sounds>` block names what a class plays, [`architecture.md`](architecture.md) for why none
of this is in core.

Sound is presentation. Nothing here is simulation state: nothing is hashed, saved, sent to a
peer or read back by the simulation, and the random numbers that choose a variant are the
presentation's own, never the world's RNG. Core does not include a line of it
(`tools/check_core_boundary.py`).

## How to read the evidence

As in [`netjoin.md`](netjoin.md):

* **Read.** Followed at the cited address with `tools/re/gbr.py`, or counted in the retail
  files by the census scripts described beside each table.
* **Inferred.** Follows from something read but was not itself followed to the end; the
  alternatives are named.
* **This engine's.** A decision, not a finding.

Addresses are virtual addresses in the retail `gbr.exe`. Nothing here reproduces
instructions or ships a byte of the installation ([`../legal.md`](../legal.md)); the
examples are written from the format, not copied from a file.

## The data

### What the installation holds

| where | what | count | format |
|---|---|---:|---|
| `Packs/Sounds.pak` | effects, ambience, fights, walks, selections, notifications, UI | 314 `.WAV` | PCM 16-bit: 138 mono 44,100 Hz, 97 mono 22,050, 76 stereo 44,100, 2 stereo 22,050, 1 stereo 48,000 |
| `local/<language>.pak` (`italian.pak` here), `CURRENTLANG\VOICES\<unit>\` | the units' voices | 388 `.WAV` | Microsoft ADPCM (tag 2), 4-bit mono 44,100 Hz, 1,024-byte blocks of 2,036 samples, the seven standard coefficient pairs |
| `Packs/data.pak`, `DATA\SOUND ENTITIES\` | sound entities | 165 `.XML` and `AMBIENT SOUNDS.TXT` | XML, below |
| `music/` | music | 8 `.ogg` | Ogg Vorbis, stereo 44,100 Hz, 128 kbit/s (one at 160), encoder `libVorbis I 20020717`; seven `GBR_TRACK_<n>` of 222--329 s and `_menu.ogg` of 87 s |
| `config.ini` (LZIS) | `[SoundConfig]`, `[SoundChannels]` | -- | INI, below |
| `Settings.ini` | the options: `SoundVolume`, `MusicVolume`, `SpeechVolume`, `SoundFX`, `NatureSounds`, `Speech`, `Music`, `ReverseSpeakers`, `Conversations` | -- | INI |

The report's "316 entries" is the pack's 314 WAVs; nothing else is in it. `Sounds.pak` holds about
973 seconds of PCM. The voices are 76 unit directories of five files and 12 villager directories of
three (one enchantress has six).

**The decoder.** `engine/sound`'s `decode_wav` reads PCM of 8 and 16 bits and Microsoft ADPCM, and
refuses anything else. Over all 702 shipped WAVs it agrees with SDL's own decoder sample for sample.
The one difference is length. SDL drops the short last block of 227 voices, up to about 200 samples.
This decoder plays that block. The `fact` count never asks for less than the data holds: 111 voices
count exactly what is there, 116 count one sample more, and the 161 whose data ends on a block
boundary count a whole block that is not there.

**The music's decoder is `stb_vorbis`.** `gbr.exe` links one statically: there is an `OggS` capture at
`0x00753202` and a `vorbis` header check at `0x00753a77`, but no vendor string to identify it. The
owner chose to vendor `stb_vorbis.c` v1.22 (public domain or MIT), the project's first third-party
source: `engine/third_party/stb_vorbis/` holds it unchanged with its licence and a README naming the
upstream commit, it builds as its own C library with its warnings off, and `engine/sound` alone links
it ([`../legal.md`](../legal.md), "Third-party code"). Over the eight shipped files it decodes
exactly the length each file's last page names, 87 to 329 seconds of stereo 44,100 Hz.

### The sound-entity file

The census below covers all 165 files. The example is written from the format and is not a shipped
file:

```xml
<entity description="Example Voice" priority="UnitOrder" type="UnitOrder">
  <files>
    <sound file="CurrentLang/voices/Example/1.wav" frequency="50"/>
    <sound file="CurrentLang/voices/Example/2.wav" frequency="50"/>
  </files>
</entity>
```

| element | attribute | seen | values |
|---|---|---:|---|
| `<entity>` | `description` | 165 | free text; **read by nothing** (stored at entity+0xcc by 0x006b0fe4, never read) |
| `<entity>` | `priority` | 144 | `UnitOrder` 97, `UnitFight` 21, `UI` 12, `UnitIdle` 5, `UnitWalk` 5, `Event` 4 |
| `<entity>` | `type` | 144 | `UnitOrder` 99, `UnitFight` 23, `UI` 12, `UnitIdle` 5, `UnitWalk` 5 |
| `<files>` | -- | 165 | one per file |
| `<sound>` | `file` | 837 | a path in the installation; 16 name nothing shipped (four horse-walk WAVs, eight `.ogg` ambient entries and others) |
| `<sound>` | `frequency` | 837 | 1 to 100; 20 is the most common (358) |

`priority` and `type` agree in 140 files. The four that differ are `GateOpen`, `GateClose`,
`CatapultRotate` and `GuleExplosion`, which have priority `Event` and type `UnitFight` or `UnitOrder`.
The 21 files with neither attribute are the ambient themes (`Ambient*`, `Sound*`). One file opens with
an XML Spy comment. Files per entity range from 0 to 37. The weights sum to 100 in 115 files and to
99 in 19. Other sums occur too, from 0 (an empty entity) to 250.

**Read: the name of an entity is its file's stem** (`_splitpath`, 0x006b13e7). It is not its
`description`, which differs from the stem in about 130 files. At start-up the loader reads every
`*.xml` under `data/sound entities`, up to 4,000 (0x006af9c0, 0x006b1350).

**Read: the weights are evened out at load** (0x006b1400--0x006b1517). Let *S* be the sum and *Z* the
number of zero weights. When *Z* > 0 and *S* < 100, each zero becomes (100 − *S*) / *Z*, in integers.
If the sum is still short, a silent filler entry takes the rest. So an entity summing to 70 is silent
three times in ten, and the empty entity is always silent. Sums over 100 are left alone, and entries
past a running total of 100 can never be drawn. The `AMBIENT SOUNDS.TXT` note says the sum "must"
be 100; the loader is what makes that so.

**Read: the type table** (0x0082e6a0). It gives each type an id and a default channel count. `Event`
and `UnitWork` are priorities, not types.

| type | id | default channels | shipped `[SoundChannels]` | steals when full | switch, slider (0x006e7210) |
|---|---:|---:|---:|---|---|
| `Music` | 1 | 1 | 1 | yes | `Music`, `MusicVolume` |
| `Ambient` | 2 | 1 | 1 | yes | `NatureSounds`, `SoundVolume` |
| `UnitOrder` | 3 | 1 | 1 | no | `Speech`, `SpeechVolume` |
| `UnitFight` | 4 | 4 | 5 | no | `SoundFX`, `SoundVolume` |
| `UnitIdle` | 5 | 1 | 1 | no | `Speech`, `SpeechVolume` |
| `UI` | 6 | 1 | 1 | no | `SoundFX`, `SoundVolume` |
| `Select` | 7 | 1 | 1 | no | `SoundFX`, `SoundVolume` |
| `Ambient2` | 8 | 1 | 1 | yes | `NatureSounds`, `SoundVolume` |
| `ConvSpeech` | 9 | 1 | 1 | yes | none: always on, at 100 |
| `UnitWalk` | 10 | 2 | 2 | no | `SoundFX`, `SoundVolume` |

**Read: the priority table** (0x0082e600). Highest 1, UI 300, Music 500, Ambient 700, Ambient2 800,
Event 2000, UnitOrder 3000, UnitFight 4000, UnitWalk 4750, UnitWork 5000, UnitIdle 6000, Lowest
65535. A number is also accepted, and so is `Name + N` / `Name − N`, which no shipped file uses.
Absent or unknown means Lowest. **Priority is inert.** It is stored on every sound (+0x11e, setter
0x006affa0), and no reader was found in the sound modules. Channel allocation does not use it.

### A class's `<sounds>`

Of the 845 classes, 159 `<sounds>` blocks carry 199 values: `command` 100, `attack` 34, `select` 24,
`die` 22, `walk` 15, `idle` 4. They come in three shapes:

| shape | count | example (made up) |
|---|---:|---|
| a bare entity name | 59 | `ExampleSelect` |
| a path to an entity file | 104 | `data/sound entities/VoiceExample.xml` |
| a path to a WAV | 33 | `Sounds/selection/example.wav` |
| `nothing` / empty | 1 / 2 | -- |

**Read: resolution happens at play time** (0x006b0910). The value is split as a path. An extension
other than `.xml` makes it a file. Otherwise its **stem** is looked up among the entities without
regard to case (0x006afb40). The directory is ignored, which is how the four
`Sounds/entities/Voice*.xml` values still find their voices: that directory does not exist. A stem
that names no entity is played as a file, and a file must end in `.wav` or `.ogg`. Anything else is
refused (`UNKNOWNSOUNDFILEEXT`). Three `select` WAVs name nothing shipped.

The class loader (0x005a58c1--0x005a5e7b) stores the values as follows. `select` and `command` get one
string each (class+0xaf4, +0xb10). `walk`, `attack`, `die`, `idle` and `taunt` each fill four slots of
a table of 28 indexed by the unit's current action (class+0x34c, stride 0x1c). An empty value becomes
`**DontInherit**`, and inheritance runs at 0x0059d233. `ClassGraph::resolved_sounds` gives the
inherited bag, and the acknowledgement reads `command` from it.

## What `gbr.exe` does

### The sound manager

**Read.** The backend is DirectMusic 8, created through COM (0x00637980). Each channel is an
audiopath, with volume set through `SetVolume` (0x00637260) and pan through the buffer's `SetPan`
(0x006371c0). The manager is the global at `[0xa78420]`, constructed at 0x006b0080. It reads
`config.ini [SoundConfig]`:

* `Sound`: 0 never starts the manager.
* `Music`: gates music.
* `ChannelsNumber`: clamped to 8..32, then overwritten.
* `ReverseSpeakers`.

Then, for each key of `[SoundChannels]`, it creates that many channels of the type the key names. A
type that is not listed gets no channels and never plays. The shipped file gives 15.

**Read: one entry point plays everything** (0x006b0910). Its arguments are the name, priority, type,
x, y, flags and a volume percentage; every caller passes 100. A priority or type of 0 means "the
entity's own". Each type has its own enable switch (+0x1ec), checked before and after resolution.

**Read: channels are pools per type** (0x006b0450), and no type takes another's channels. The manager
counts the type's busy channels, polls the finished ones free, and counts again. If the pool is
still full, `Music`, `Ambient`, `Ambient2` and `ConvSpeech` take over their **last** channel: the
steal flag passed to 0x006b0450 is set for types 1, 2, 8 and 9 (0x006b09f4--0x006b0a43). This
document's first version left `ConvSpeech` out; nothing plays it yet. Every other type **drops the new
sound**. For a type that drops, the full pool is detected before the variant is drawn (0x006b09f4), so
a dropped sound leaves the no-repeat state unchanged.

**Read: music is DirectMusic's primary segment.** Every sound is played with flag 0x80 set
(0x006b0c70--0x006b0c7e) and that flag toggled on its way to `PlaySegmentEx` (0x00636df0), so every
type but `Music` plays as a secondary segment and `Music` as the primary. **Inferred** from the value,
which is `DMUS_SEGF_SECONDARY`: nothing on the path read makes music repeat; a track ends and its
channel goes idle, which is what both music loops below wait for.

**Read: everything stops** at 0x006aff00, which stops channels 0 to 100 one by one. It is called as
the game's own loop starts (0x0074a132, behind a flag at `[0xa8734c]` not followed), in 0x0051d430
(**inferred**: the match's teardown, beside the game globals it clears), and at 0x004c7e2d and
0x00709e0c, not followed.

**Read: the variant** (0x006b0da0). The draw uses the C runtime's `rand()` (0x007638f2, the MSVC
LCG), not either of the game's generators:

* An entity with one entry plays it with no draw.
* With two or more entries, `rand() % 100` walks the running weights, and the first entry whose running
  total exceeds the roll is picked.
* With three or more, a pick equal to the last one (entity+0x19e) moves on to the next, wrapping. The
  filler counts as an entry here.
* The filler, or a roll past every weight, plays nothing.

**Read: volume and position.** Everything is in millibels, hundredths of a decibel.

* **Distance** (0x006b0650). The view's four screen corners are unprojected to the world and boxed.
  Inside the box the attenuation is 0. Outside, *d* is the Chebyshev distance to the box's centre,
  and the attenuation is −5·(*d* − 500). It becomes −6000 once *d* − 500 passes 1,200. There is no
  upper clamp, so a point outside a narrow view but near its centre comes out positive. x = −1 means
  "not positioned".
* **Pan** (same function). A sound whose x is between the screen's left and right edges is centred,
  so everything horizontally on screen is centred. Off screen, the pan is ten per unit past the edge,
  up to ±10,000, towards that side. `ReverseSpeakers` reverses it.
* **Level** (0x006b0b22): `((attenuation + 6000) · percent / 100) · slider / 100 − 6000`. A slider
  is therefore linear on a 60 dB scale. 0 means −60 dB, not silence, and the shipped `SoundVolume=68`
  is −19.2 dB.
* **Following the camera.** The manager is not what culls off-screen sounds. 0x006b0810 walks the
  first 64 channels and re-applies level and pan to each that plays. A sound with no place
  (+0x10c clear) takes `slider · 6000 / 100 − 6000` and keeps its pan. A placed one takes the
  attenuation and pan of its stored point (+0x110, +0x114) against the view as it now is, at
  `(attenuation + 6000) · slider / 100 − 6000`: **the call's percentage is not kept**, which changes
  nothing while every caller passes 100. It runs from the view setter 0x006265c0 (the call at
  0x00626c56, just before the ambience is told the new view), from the `ReverseSpeakers` setter
  (0x006b08fa) and from the options' apply (0x006e7377). The object and animation callers do their
  own culling (below). The voices do none.

**Read: the options** (0x006e7210). They are applied at start-up, at game start and from the options
screen. The mapping is the table's last column. **Unit acknowledgements follow `Speech` and
`SpeechVolume`, not `SoundFX`.** Switching a type off stops its channels (0x006af960).

### The acknowledgement

**Read.** The input handler (0x005e8d10) acts only for a human local seat. It acknowledges in two
cases:

* a right-click default order (0x005e6050);
* a command-bar row completed on its target (0x005e7090).

Both draw `Random(0, count − 1)` over the **whole selection** on the game's **unsynchronised**
generator (0x005e61db, 0x005e71ae). They call `Talk` (0x005e36e0) on the chosen object, which is also
the script host's `Unit::Talk`, 0x005d8900. `Talk` plays the class's `command` sound at the object's
position, with priority 3000, type `UnitOrder` and volume 100.

When the chosen object is a squad-like object with members (flag 0x1000000 at +0x2c, count at +0x1e4),
`Talk` first rolls `Random(0, 100)`. Above 40, which is 60 chances in 101, a random member speaks its
own class's voice at the group's position.

**There is no timer.** The rate limit is the single `UnitOrder` channel, which drops a new voice
while one speaks.

A row with no target does not acknowledge, and nor does selecting. **Inferred** from a census of
call sites: `Talk` has no other caller.

**This engine's:**

* The speaker is drawn on a presentation generator of the app's own. The original's unsynchronised
  generator is cosmetic, and here nothing about a sound may reach the world.
* The squad-with-members test is read as a hero with a non-empty army. In this engine a hero's squad
  is his army (`sim/hero.hpp`). **Inferred.**
* The local right click acknowledges when at least one order is issued. A networked click
  acknowledges as it is queued, because the original speaks in its input layer, not when the turn
  runs.

### Selection, combat, walking, gates (for the stages)

**Read.**

* **Select** (`PlaySelectSound`, 0x005e2f80). It plays the object's class `select` value
  (class+0xaf4; nothing when it is empty) at the object's position (vtable +0x3c), at priority 4500
  on type `Select`, at 100 percent. It has three callers:
  * **0x005e7dc6**, inside 0x005e7d80, the insert of one object into the local selection, which
    twenty-four sites call: the holder strip at 0x006d3469 among them, and, **inferred** from where
    they sit, the click and the band (twelve sites in the input handler, 0x005e7f18--0x005e9f1e)
    and script selections (0x0041faa9--0x004cd3d4, not followed). Before
    it adds the object it asks the object's visibility virtual (vtable +0x74, given the players'
    table), refuses a spawn template (`[obj+0x2c]` bit 27), and plays the sound only when the
    owner's relation word towards the local player (`[obj+0x70]+0x24+4·local`) has bit 5 set:
    **`share_control`, which every player grants itself** (`player<i>.xml`'s self-word 0x35). So
    each object is heard as it goes in, and with the one `Select` channel dropping, a band is heard
    by the first that speaks.
  * **0x005e84e2**, recalling a selection group: the first object of the restored selection, with
    no test at all.
  * **0x005e31ca**: the first object of a list at `+0x98` of the input handler, in a function that
    resets the handler's state. Not followed.

  Only buildings and decor declare `select` (24 values, 133 classes by inheritance), so selecting a
  unit is silent.
* **Animation sounds** (0x005d9550, a virtual in seven unit vtables). It plays the class table slot of
  the unit's current action `[unit+0x44]`, with priority 4000 and type `UnitFight`. A moving squad
  leader uses `UnitWalk` and 4750 instead. It plays only inside the view grown by 2,048 px on every
  side, and only for objects the local player sees. **Inferred:** called as an animation starts.
* **Object sounds** (`Obj::PlaySound` 0x005aba90 → 0x005a93a0). These are culled the same way and use
  the entity's priority and type. Gates play `GateOpen` (0x0052914a) and `GateClose` (0x00529254) at
  `Event` priority on `UnitFight`, so they share the five fight channels. The same path carries items
  dropped, taken and put (0x005acc6a--0x005acef2), and `GuleExplosion` (0x004e45fe).
* **Item use** (0x0053f3d8): 4000, `UnitFight`, positioned.
* **Notifications** (0x0055cde7): `DATA\VXNOTIFICATIONS.XML`'s `sound=`, 2000, `UI`, not positioned.
* **Conversation speech** (0x0050bac9): `%s%s_Phrase%d.wav`, priority 1, `ConvSpeech`.
* **Ambience** (0x006af630, 0x006af6c0). The entity is named base plus season, as
  `AMBIENT SOUNDS.TXT` says. `Ambient*` plays at 700 on `Ambient` and `Sound*` at 800 on `Ambient2`,
  and each is chained from the finished-sound callback (0x006afe60).
* **Script host:** `PlaySound(str)` (0x004c60d0, UI, 300), `PlaySound(player, str)` (0x004c6150,
  local player only, entity's own), `PlayMusic(str)` (0x004c6050: a non-empty name at 500 on
  `Music`, unplaced, at 100) and `MusicPlaying()` (0x004c61e0: whether any `Music` channel is busy),
  registered at 0x004caaa7 and 0x004caae9. `RESEARCH.VS`'s `UpgradeComplete` is named through a
  misspelt directory, which the stem rule forgives.
* **Fixed files:** `PlayerDropped.wav` at 300 on **`Ambient2`** (0x00406a3b, 0x004e69aa), so the
  nature switch governs it. `ChatMessage.wav` at 300 on `UI` (0x004e7518).

### Interface clicks

**Read.** 0x006b0d10 is a global listener on the UI manager (installed at 0x0074d4d9). On the
"control activated" message 0x12345604 it plays `Sounds/UI/click.wav` at 300 on `UI`, unpositioned.
A button posts that message when released over itself (0x00661136), and a dialog posts it for its
default or cancel control on Enter or Escape (0x00662eb6, 0x00662ef9). One control, id 0x0100BEDA,
is exempt (0x006b0d2a). With one `UI` channel that drops, rapid clicks are dropped. `click2.wav` and
`Sounds/beep.wav` are referenced by nothing. **Inferred:** unused.

### Music

**Read: the match's music.** The in-game player (constructor 0x00551150) lists `music/` and keeps every
file whose name does not start with `_` (0x005511e4), which leaves the seven tracks; the last track
starts as none (0x005512ac). Its handler (0x00550f90) asks whether any `Music` channel is busy and
whether the game clock `[game+0x1258]` is past 100. That clock is in milliseconds, the one
`World::time()` is (`sim/economy.cpp` reads 60,000 of it as a minute). If both allow, it picks a track
(0x00550e80) and plays it through 0x00550df0: the play entry point with priority 500, type `Music`,
x and y −1, flags 0, 100 percent. Then it schedules itself 2,000 ms on (0x00550fbb) on the game's
scheduler `[game+0x129c]`, the one the simulation's timers use. **Inferred:** two seconds of game
time, so it stops while the game is paused.

The pick: no tracks, nothing. One, that one, with no draw and the last left as it was (0x00550ed9).
More, `Random(0, n − 1)` on the unsynchronised generator (`[game+0x12b4]`), drawn again while it
equals the last (0x00550f4d), and the last set to it after the play whether or not that played
(0x00550f82). There is no choice by nation or situation and no crossfade.

**Read: the menus' music plays once each time the menus are entered.** The menus are a loop of their
own (0x00748600, called by the main loop at 0x0074d90e each time the program goes back to them). On
entry it raises a flag (0x00748620). Each pass of the loop counts a countdown down (0x0074886a), and
when it is zero and the flag stands, the countdown is set to 500 (0x00748812) and, if no `Music`
channel is busy, `CONST.INI [GamePlay] PregameUIMusic` (`music/_menu.ogg`) is read (0x00748839) and
played at 500 on `Music`, unplaced, at 100 (0x00748855). A play that succeeds lowers the flag
(0x0074885e), and so does a `CONST.INI` without the key. So the 87-second piece plays once each time
the menus are entered, retried every 500 passes while something else holds the channel, and is not
started again when it ends. This document's first version read "restarted whenever the channel is
idle" and inferred a loop; the flag is what that reading missed.

**Inferred:** a pass of the menus' loop is a frame; its body pumps messages and draws.

## What is built

| piece | where | what it does |
|---|---|---|
| WAV decoder | `engine/sound/wav.{hpp,cpp}` | PCM 8/16, Microsoft ADPCM; refuses the rest |
| entities | `engine/sound/sound_entity.{hpp,cpp}` | the parser with the loader's evening-out, the type and priority tables, `choose_variant` with the CRT `rand()` and the no-repeat rule, stem resolution (`SoundBank`), `config.ini`'s sections |
| placement | `engine/sound/placement.{hpp,cpp}` | distance attenuation, pan, the slider formula; `follow`, 0x006b0810's re-application |
| mixer | `engine/sound/mixer.{hpp,cpp}` | pools per type, drop or steal-the-last, millibels to Q15, linear rate conversion, integer mixing with clipping; streams decoded inside `mix` as far as the frames mixed need; `set_voice` on a live voice |
| Ogg Vorbis | `engine/sound/ogg.{hpp,cpp}` | `OggStream`: the compressed file held whole, samples handed out as asked, through the vendored `stb_vorbis` |
| music | `engine/sound/music.{hpp,cpp}` | which files are tracks, `next_track` (0x00550e80), the 2,000 ms look, the 100 ms start, the menus' 500 passes |
| device | `engine/platform/src/audio.cpp` | one SDL3 stream at 44,100 Hz stereo, fed from SDL's callback under the stream's lock; clips decoded once and kept, Ogg streamed and never kept; each voice's name and point kept for `live` and `set_voice`; the dummy driver when headless |
| hooks | `engine/app/main.cpp` | `start_sound` (config.ini → pools), `play_sound_value`, `acknowledge_order` (right click and aimed row, local and networked), `play_click` (menu controls and bar buttons), `play_select_sounds` (band and click, `select:`, the party, the holder strip), `follow_view` (the camera, and the options' apply), `tick_music` and `play_music` (the menus, the match), `stop_sounds` (a match starting and ending), the options → types, `PlayerDropped.wav` on `Ambient2` |

**This engine's:**

* The mixer's arithmetic is integer, and the gain table is computed once.
* Millibels at or above 0 play at 0. −10,000 is silence.
* Pan is DirectSound's: the far channel is attenuated by the pan.
* Bar buttons click, on the reading that they are controls like any other.
* The options' `ReverseSpeakers` is the one used; `config.ini`'s is read but overridden.
* A stream is decoded in the audio callback, a block of at least 2,048 frames at a time; the
  headers are read on the game's thread when it starts.
* The select sound: the selection made afresh (a click, a band, `select:`, the party, the holder
  strip) is all new, so a building clicked again speaks again; Shift adds only what it adds. The
  visibility virtual is not asked: an object the player controls is one the player sees.
  **Inferred**, as is that the band goes through 0x005e7d80 one object at a time. There is no group
  recall in the app yet, so 0x005e84e2's path has no caller here.
* The live sounds follow the view on the frame the camera moved, and on every options apply.
* The match's music player starts with the match: `music/` is listed then, sorted by name (the order
  Windows lists an NTFS directory in; the draw is uniform either way), and the first look is at
  2,000 ms of game time. When the original schedules its first look was not read.
* Every sound stops as a match starts (0x0074a132) and as the app goes back to the menus
  (0x0051d44e, inferred teardown); the menus then ask for their music again.
* `PlayMusic` and `MusicPlaying` are not bound. No shipped script calls either (none of the 885
  scripts names them, and `core/src/script/host_surface.cpp`, generated from those scripts, does
  not declare them), and the script host is in core, which may not reach the device.
  `MusicPlaying` would also hand presentation state to the simulation: a peer with music off would
  answer differently from one with it on. A mod that needed them would want `PlayMusic` as a request
  on an unhashed presentation queue (the one stage 2 needs), and `MusicPlaying` answered by the
  simulation alone, which this engine cannot do faithfully.

**`IMPERIVM_SOUND_LOG=1`** prints one line for each sound played or moved:

```
sound: <entity> <file> vol <millibels> pan <millibels>
music: <file> vol <millibels>
repan: <file> vol <millibels> pan <millibels>
```

`repan:` is a live voice whose level or pan the view changed. A sound or music that was attempted
and failed prints `not played: <why>` instead; a type switched off prints nothing. Names with a space
are quoted.

**Tests:**

* `engine/sound/tests/sound_tests.cpp` (`sound_tests`, in `tools/verify.py`'s quick tier, 10,279
  checks). It covers the decoder, the parser, the evening-out, the generator's sequence, the variant
  rule, stem resolution, `config.ini`, placement, the gains, the mixing arithmetic, rate conversion,
  the pools' drop and steal, streams read a block at a time and interpolated across blocks,
  `set_voice`, `follow`, and the track draw over 5,000 picks. All of it runs on synthetic buffers:
  the Ogg Vorbis tests build a stream bit by bit from the Vorbis I specification and RFC 3533 (two
  codebooks, floor 1, residue 1, one short-block mode) and decode it in pieces, to its last
  granule, mono and stereo, silent and not, and through the mixer.
* `tests/test_corpus_app_sound.py`:
  * a right click on Numantia's `RHastatus` logs one voice from the class's own entity, at the speech
    slider's level and centred;
  * three orders in quick succession are acknowledged once;
  * a bar button clicks at the sound slider;
  * without the variable, nothing is logged;
  * selecting Crossroads' barracks logs its `select` entity, and selecting units logs nothing;
  * a camera move after it logs a `repan:` of that voice, now panned left and no louder;
  * the menus log `music/_menu.ogg` once at the music slider, and nothing with music off;
  * an unpaused match logs one `GBR_TRACK`, never `_menu`;
  * `sound_tests --decode` reads each of the installation's eight `.ogg` files in place: stereo
    44,100 Hz, its whole length in many blocks, not silent.
  The music runs give the app a temporary user directory whose `settings.ini` turns music on, since
  the installation's own may have it off; nothing is written into the installation.

Nothing is in core, and nothing is hashed or saved.

## The rest, in stages

Each stage is presentation only. Where it needs to know that something happened in the simulation,
it should use **one event queue on the world that is never hashed, saved or sent**. Core appends
records of the form `{kind, object, class, position}`, and the app drains the queue after each turn.
The rule is the same as for anything cosmetic: a peer that never reads the queue plays the same match.
Such a queue does not exist yet, and stage 2 is the first stage that needs it.

1. **Selection and re-pan.** Built. Left: a group recall's sound (0x005e84e2), when the app has
   group recall.
2. **Combat, deaths, walking, idle.** Needs the event queue and the meaning of the 28 action slots
   (`anim%d`, four slots per attribute, still unknown).
   * Animation starts, with the ±2,048 px and visibility culls.
   * `UnitWalk` for a moving leader only.
3. **Gates, items, explosions, notifications, chat.** The calls are named above.
   * Needs the queue for gates and items.
   * The notification screen already exists in the app.
   * `ChatMessage.wav` hangs off `show_chat`.
4. **Ambience.** The terrain under the view centre picks a theme: Plains, Forest or Sea, with season
   appended. A building's `ambient_sound1`/`2`, or its town hall's, takes precedence. The two chains run
   on `Ambient` and `Ambient2`. The rule that picks the terrain needs reading (0x006af630 and its
   callers).
5. **Music.** Built, on the vendored `stb_vorbis` (the owner's choice over `libvorbisfile` and
   SDL3_mixer). Left: `PlayMusic` and `MusicPlaying`, which no shipped script calls (see "This
   engine's" above).
6. **Speech in conversations** (`ConvSpeech`, `%s%s_Phrase%d.wav`) and the script host's `PlaySound`
   and `PlaySound(player, …)`.

## What is still unknown

* Whether anything reads a sound's priority (+0x11e) outside the sound modules scanned.
* What DirectMusic does with a positive volume, or with one below −9,600.
* The 28 action slots of the class sound table: which animation goes with which of the four slots
  per attribute, and when `idle` and `taunt` fire.
* What the object's visibility virtual (vtable +0x74) asks before a selected object is inserted.
* What the input handler's list at `+0x98` is, whose first object 0x005e31ca makes speak.
* When the match's music player schedules its first look, and whether its scheduler's clock is the
  game clock (read as such).
* Whether 0x0051d430, which stops every sound, is the match's teardown, and what the flag at
  `[0xa8734c]` guarding the stop at the game loop's start is.
* That DirectMusic's flag 0x80 here means `DMUS_SEGF_SECONDARY`, and that nothing repeats a music
  segment.
* Which control is 0x0100BEDA, the one that does not click.
* How `Conversations` gates `ConvSpeech`, and where `ConvSpeech` gets its volume.
* Whether the unsynchronised generator's `Random(lo, hi)` includes `hi`. The 60-in-101 figure assumes
  it does, as the synchronised one does.
* What the original does with the 16 entity files that name nothing shipped, beyond logging "Error
  creating sound" (0x006b0cc6).
* Which Ogg Vorbis decoder the original links.
