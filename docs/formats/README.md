# File format specifications

The formats used by the HMMSYS engine behind Imperivm: Great Battles of Rome.

Each specification has a reference reader of the same name in [`src/imperivm/formats/`](../../src/imperivm/formats/).
See [`CONTRIBUTING.md`](../../CONTRIBUTING.md) for how these documents are written and what
standard of evidence they are held to.

| Format | Spec | What it holds |
|--------|------|---------------|
| `HMMSYS PackFile` (`.pak`) | [pak.md](pak.md) | the top-level archives; everything ships inside one |
| `LZIS` | [lzis.md](lzis.md) | whole-file compression codec |
| `IMGRLE` (`.rle.mmp`, `rle.mmp`) | [rle.md](rle.md) | sprite frames and the memory-mapped pixel store |
| `vqbm` (`.vq`) | [vq.md](vq.md) | vector-quantised terrain textures |
| `HPFS` (`.bfhp`) | [bfhp.md](bfhp.md) | maps, scenarios, campaigns, saved games |
| `.apf` | [apf.md](apf.md) | bitmap fonts |
| `.pass` | [pass.md](pass.md) | per-entity passability masks |
| interface `.ini` | [interface-ini.md](interface-ini.md) | the command bar, the info bar, the menus, the editor |
| `.ini` | [ini.md](ini.md) | configuration: the interface, the AI profiles, the tuning constants |
| `AI.INI` | [ai-ini.md](ai-ini.md) | AI profiles, and the script constants they declare |
| `.sc.xml` | [sc-xml.md](sc-xml.md) | the game class graph |
| `.ent.xml` | [ent-xml.md](ent-xml.md) | entity, sprite and animation definitions |
| `.vs` | [vs-language.md](vs-language.md), [vs-host-api.md](vs-host-api.md) | the scripting language and its host API |
| `.conv.xml` | [conv-xml.md](conv-xml.md) | conversations: what a mission says, and which of the things it could say it picks |
| `player.ini` | [profile.md](profile.md) | a player profile: the journal of every match finished, and the career aggregated from it |
| map container contents | [map.md](map.md) | what one playable map's directory tree holds |
| campaign container contents | [adventure.md](adventure.md) | what makes a sequence of maps a campaign: flavours, the territory graph, what crosses a mission boundary |
| `ISAV` | [save.md](save.md) | **ours, not the original's** — the saved session we write, and the checklist `gbr.exe` gives for it |

The resolved object model — class tree, native class responsibilities, faction inventory —
is in [`../data-model.md`](../data-model.md).

## Conventions

Unless a specification says otherwise:

- integers are little-endian and unsigned
- offsets are absolute from the start of the file
- strings are byte strings in cp1252, using `\` as the path separator, conventionally uppercase
- the engine is a 16-bit software rasteriser, so pixel data is generally 16 bits per pixel
