#pragma once

/// The campaign layer: what turns a sequence of maps into a story with state
/// carried between them.
///
/// Specification: docs/formats/adventure.md. The container itself — `game.xml`,
/// `Maps/<n>/`, `Sequences/sequences.xml`, `player<i>.xml` — is
/// docs/formats/map.md, and `world/map.hpp` reads it. Nothing here re-reads a
/// map.
///
/// ## What the retail install actually ships
///
/// Three container flavours share one layout and are told apart only by
/// `game.xml`'s `game_type`: **scenario** (0), **adventure** (1), **conquest**
/// (2). `Packs/emptyscn.bfhp`, `Packs/emptyadv.bfhp` and
/// `Packs/emptyconquest.bfhp` are the editor's three blank templates and carry
/// exactly those three values, which is the cleanest possible confirmation that
/// the number is the flavour and not a coincidence.
///
/// **Every shipped adventure holds exactly one map.** All thirteen of them —
/// six `GreatBattles`, six `GreatChallenges`, the Tutorial — have a single
/// `Maps/<n>` directory, numbered 1 to 7 and never 1 by default. So an
/// adventure is *one mission in a container*, and the campaign it belongs to is
/// not declared inside it at all: `gbr.exe`'s `CVXUIPreAdventureMenu` carries
/// the two literals `adventures/GreatBattles/` and `adventures/GreatChallenges/`
/// and enumerates each directory. The mission order is the `1_`..`6_` prefix on
/// the file name; see `adventure_order` below.
///
/// **The one conquest holds seven maps** — `Maps/3, 4, 6, 7, 8, 9, 10`, not
/// contiguous — plus one document no other container has: `territories.xml`.
/// That document is the campaign structure. It declares seven territories, each
/// naming a map, a set of neighbours, and a *bonus*, and the seven form a graph
/// rather than a list. `Conquests/mediterranean.BFHP` is the only conquest in
/// the retail install, so every statement below about conquests rests on a
/// corpus of one.
///
/// ## What crosses a mission boundary, and how we know
///
/// This is the question the layer exists to answer, and the shipped scripts
/// answer it flatly. Every conquest mission's victory sequence ends the same
/// way — this is `Maps/10/Sequences/seq6.vs` verbatim:
///
/// ```
///     SetTerritoryState("Spain", tsOwned);
///     EndGame(1, false, Translate("You have conquered Hispania!"));
/// ```
///
/// Six of the other maps do the same for their own territory. And every mission
/// *begins* by running the container-root sequence `StartBonuses`
/// (`Sequences/seq0.vs`), which is this, verbatim and entire in its first half:
///
/// ```
///     set.SetFood(2000);
///     set.SetGold(6000);
///     set.AddToPopulation(40);
///     EnvWriteString("/Bonus", ConquestBonus());
///     if (EnvReadString("/Bonus") == "rIberia") RunSequence("rIberia");
///     ... six more, one per territory ...
/// ```
///
/// Read those two together and the answer is unambiguous, and it is not the
/// answer one expects:
///
///   * **No hero, no unit, no item and no gold crosses a mission boundary.**
///     Every conquest mission starts its town hall at exactly 6000 gold, 40
///     population and 2000 food, unconditionally, by assignment. There is no
///     roster to carry and nothing reads one. `persist_state` is `0` on all
///     seven conquest maps.
///   * **What crosses is territory state**, written by the finished mission's
///     own script through `SetTerritoryState` and read back by the engine.
///   * **And one string.** `ConquestBonus()` returns the name of exactly one
///     container-root sequence, and `StartBonuses` runs exactly that one. The
///     bonuses are not cumulative in the shipped data: `seq0.vs` is a chain of
///     seven independent `if`s over a single value, and each bonus script
///     itself loops on `while (EnvReadString("Bonus") == "rX")`. One conquered
///     territory's reward is live at a time.
///
/// The rewards are spawned, not transferred: `rRepublicanRome` *places* a fresh
/// `RHero3` at level 20 and a `Trader` wagon holding 6000 gold beside the town
/// hall; `rGaul` places four fresh `GTridentWarrior` at level 12. A level-20
/// hero appears in mission two whether or not you had one in mission one.
///
/// ## The adventure carry mechanism exists, and the shipped content cut it
///
/// The other half of the answer, and it is the more interesting half.
///
/// `gbr.exe` implements a **party carry-over** for adventures — heroes and the
/// units attached to them, moved from one `Maps/<n>` to another inside one
/// container. The class is `CVXPartyList`, it sits between `CVXGameConq` and
/// `CVXGameAdv` in the string pool, and its trace strings spell the whole
/// mechanism out:
///
/// ```
///     Storing party before moving to another map:
///     Party unit of class %s, this ptr is 0x%08x, handle is %d
///     attaching unit %d to hero %d
///     -- AIP unit %d -> hero %d
///     --- unit found in storebin, object is %08x, stored handle is %d
///     Placing party on new map:
///     Entering new map...
///     mapchangecounter
/// ```
///
/// It is driven by the host entry point `ChangeMap`, which the binary guards
/// with `ChangeMap: Maps can be changed only in adventure mode` and
/// `ChangeMap: No map is named %s` — so its first argument is a map's `name`,
/// the same string `map.xml` carries, and the feature is adventure-only.
/// `WaitForMapChange` and `SaveAdventure` carry the matching
/// `... called in a non-adventure game!` guards.
///
/// **Nothing in the shipped content uses it.** Across all 308 `.vs` files in
/// every container in the install there are exactly **12** `ChangeMap` call
/// sites and **0** of them are outside a comment block. All twelve are in the
/// six `GreatBattles` adventures, all twelve have the same shape, and all
/// twelve sit immediately after the `EndGame` that replaced them:
///
/// ```
///     EndGame(1, false, Translate("You have defeated Hannibal at Zama."));
///     /*
///     EnvWriteString("/LastMap","WinZama1");
///     ChangeMap("MainMap", "A_MainMap");
///     */
/// ```
///
/// Eleven distinct outcome tokens are written to `/LastMap` in those comments —
/// `WinZama1`, `LoseZama1`, `WinNumantia1`, `WinAlesia1`, `LoseAlesia1`,
/// `WinBattleForEG1`, `LoseBattleForEG1`, `WinBritainConquest1`,
/// `LoseBritainConquest1`, `WinGerman1`, `LoseGerman1` — and every one of them
/// is also inside a comment. So the Great Battles were built as **one container
/// with a `MainMap` hub** that each battle returned to, carrying its party and
/// an outcome token in the root-scope environment store, and the shipping build
/// cut the hub: each battle became a standalone container that ends at
/// `EndGame`, and no `MainMap` exists anywhere in the install.
///
/// That is why every adventure holds one map, and it is the reason this layer
/// models a mission chain at all rather than a single map: the engine's own
/// mechanism is a chain, and the content is the degenerate case of one.
///
/// **This engine does not implement `ChangeMap` or the party carry.** Loading a
/// second map mid-session is `sim/session.*`'s business, not this file's, and
/// there is no shipped content to test it against — the surest way to get a
/// mechanism wrong. `MissionResult` records what a finished mission produced so
/// that a session which does implement it has somewhere to put the answer.
///
/// ## `EndGame` has a third argument
///
/// Not this domain's entry point, but this domain's evidence.
/// `docs/formats/vs-host-api.md` and `sim/match.hpp` both record `EndGame` at
/// arity 2, `(int player, bool lose)`, from the four `DATA/GAMESCRIPTS` victory
/// conditions — the only call sites `data.pak` contains. **All 61 `EndGame`
/// call sites in the 24 shipped containers pass three arguments**, and not one
/// passes two:
///
/// ```
///     EndGame(1, false, Translate("You have conquered Hispania!"));
///     EndGame(1, true,  Translate("You have failed to conquer Hispania!"));
/// ```
///
/// The third is the message the end-game screen shows. `register_match_host`
/// defines `EndGame/2` and nothing defines `EndGame/3`, so every one of those
/// 61 sites traps today. The fix belongs in `sim/match.hpp`, not here.
///
/// ## Where the state is kept
///
/// `currentadv.bfhp` in the install root **is the adventure save**. `gbr.exe`
/// mounts it as the virtual folder `AdvSave/` while an adventure runs, and
/// copies it to and from `AdvSaveGame/currentadv.bfhp` when a named slot is
/// saved or loaded. It is an HPFS container, not a flat record: the whole
/// mounted filesystem is what gets persisted. `ConquestTempFolder/` is the
/// conquest's equivalent mount, and `SetTerritoryState` writes through it into
/// `territories.xml` — the `state` attribute and `ConqueredOrder` both have
/// paired read and write sites in the binary's XML serialiser.
///
/// In *this* install `currentadv.bfhp` is two 4096-byte blocks holding an empty
/// root directory: **no campaign is in progress**, which is why the file is
/// evidence of the mechanism and not of its contents.
///
/// `tempadv.sdw` is zero bytes here and is **not** campaign state. It has one
/// write site in the whole binary (`0x495210`, opened with create/read-write in
/// the editor path) and no read site at all — a scratch shadow file.
///
/// `Profiles/lastconquest.usr` and `Profiles/lastsettings.usr` are **not**
/// campaign progress despite the names. Both are LZIS streams wrapping a
/// `CVXPersistStream` whose header is `magic` = `'gsxv'`, `build` and
/// `version`; the payload is a lobby setup — a container path, sixteen player
/// slots, and the `game.xml` property strings. `Profiles/<name>/player.ini` is
/// a settings block plus 57 `[game<n>]` post-match statistics records, none of
/// which mentions an adventure. See docs/formats/adventure.md.
///
/// ## What this refuses
///
///   * **What the shipped `state="1"` means as a starting position.** The value
///     is `tsOwned` — see `TerritoryState` — and the container gives it to all
///     seven territories at once, which cannot be the state a conquest begins
///     in. Either the engine overrides the attribute when a conquest starts, or
///     the shipped file carries the author's last saved play state. Nothing
///     distinguishes the two, so `configure` takes the file at its word and
///     says so.
///   * **Which territory `ConquestBonus()` names.** That it returns one bonus
///     name is proven by its only call site. That the one it returns is the
///     most recently conquered territory's is inference from `ConqueredOrder`
///     being an *ordered* list; a player choice at the campaign map would fit
///     the same evidence. `last_conquered_bonus` implements the inference and
///     is not wired to anything.
///   * **`ConqueredOrder`'s element numbering.** `gbr.exe` holds the format
///     string `, %d`, so it is a comma-separated list of integers, and the
///     attribute is empty in the one shipped conquest. Whether the integer is a
///     territory's `index` attribute (2..8 in the retail file) or its position
///     in the document (0..6) is not established.
///   * **Whether finishing adventure *n* unlocks *n+1*.** Nothing in the
///     install records which adventures a profile has completed — not
///     `player.ini`, not `profiles.ini`, not either `.usr`. `adventure_order`
///     gives the menu's ordering and claims nothing about gating.
///
/// ## Serialisation
///
/// `CampaignProgress` is the whole of it: an ordered vector of (id, state)
/// pairs, an ordered vector of conquered indices, and one string. Nothing else
/// in this header is state — the rest is the immutable declaration parsed out
/// of the container. Iteration order is document order everywhere, and there is
/// no unordered container in the file.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/conversation.hpp"
#include "imperivm/core/sim/note.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core {
class XmlDocument;
}

namespace imperivm::core::sim {

class World;

// --------------------------------------------------------------------------
// what kind of container this is
// --------------------------------------------------------------------------

/// `game.xml`'s `game_type`.
///
/// Proven by the three blank templates in `Packs/`, which carry 0, 1 and 2 and
/// are named `emptyscn`, `emptyadv` and `emptyconquest` respectively. The
/// retail content agrees: six `Scenarios/` and `Packs/randommap` are 0, the
/// thirteen `Adventures/` are 1, `Conquests/mediterranean` is 2.
enum class CampaignKind : std::uint8_t {
  scenario = 0,   ///< a standalone skirmish map; no campaign layer at all
  adventure = 1,  ///< one scripted mission, part of a numbered series
  conquest = 2,   ///< a territory graph over several maps, with carried state
};

/// `game_type` as a `CampaignKind`, or an error for a value outside 0..2.
///
/// Refuses rather than defaulting: a fourth flavour would mean a container this
/// reader does not understand, and treating it as a scenario would silently
/// drop whatever campaign structure it carries.
[[nodiscard]] Result<CampaignKind> campaign_kind(std::int32_t game_type);

/// The campaign half of `game.xml`'s `<properties>`.
///
/// `world/map.hpp`'s `GameProperties` keeps what the renderer needs and
/// `sim/match.hpp`'s `MatchRules` keeps what the victory rule needs; these are
/// the rest, and the split is the one `MatchRules` already documents. Nothing
/// here is duplicated from either.
struct CampaignProperties {
  CampaignKind kind = CampaignKind::scenario;
  std::string name;         ///< title in the campaign browser
  std::string author;       ///< `Haemimont Games` in 15 of 22
  std::string description;  ///< the briefing; embeds literal `\n` two-character escapes
  /// `start_map`: the `Maps/<n>` number play begins on. **Not an index**: the
  /// conquest starts on `Maps/10` and Zama's only map is `Maps/6`.
  std::int32_t start_map = 1;
  /// `last_edited_map`. An editor bookmark with no run-time effect, kept only
  /// so that a round-trip through this struct does not lose it.
  std::int32_t last_edited_map = 1;

  [[nodiscard]] static Result<CampaignProperties> parse(std::span<const std::byte> xml);
  [[nodiscard]] static Result<CampaignProperties> from_document(const XmlDocument& doc);
};

// --------------------------------------------------------------------------
// the adventure series
// --------------------------------------------------------------------------

/// The ordinal a campaign container's file name carries, or -1.
///
/// `gbr.exe`'s `CVXUIPreAdventureMenu` holds `adventures/GreatBattles/` and
/// `adventures/GreatChallenges/` as literals and enumerates each directory;
/// there is no manifest anywhere in the install. The twelve files in those two
/// directories are named `<n>_<title>.bfhp` with `n` a 1-based run of 1..6 in
/// each, and that prefix is the only ordering the data provides.
///
/// Takes a bare file name or a path (the last `/` or `\` separated component is
/// what is read) and returns the leading run of decimal digits when it is
/// followed by `_`. `Tutorial.BFHP` has no prefix and yields -1, which is
/// correct: it sits in neither series.
///
/// Claims nothing about unlocking. See the header.
[[nodiscard]] std::int32_t adventure_order(std::string_view file_name) noexcept;

// --------------------------------------------------------------------------
// sequences.xml — where the campaign layer actually lives
// --------------------------------------------------------------------------

/// One `<sequence>` of a `sequences.xml`: a named script the map can run.
///
/// This is the missing link between a container and a running campaign, and it
/// was missing in the literal sense: nothing in the engine read a
/// `sequences.xml` at all, so every mission script in the installation was
/// unreachable. `imrun` started 1,119 scripts on Zama and not one of them was
/// the mission — they were all `idle` behaviours out of `data.pak`. All 61
/// `EndGame` sites, all 403 root-scope `Env*` sites, `ConquestBonus`,
/// `SetTerritoryState` and every `RunAIHelper` live in these files.
///
/// `gbr.exe` names the two manifests verbatim — `CurrentGame/Sequences/
/// sequences.xml` and `CurrentMap/Sequences/sequences.xml` — and the container
/// holds them at `Sequences/sequences.xml` and `Maps/<n>/Sequences/
/// sequences.xml`. 49 manifests across the 24 containers declare 308 sequences.
struct SequenceRef {
  std::string name;    ///< what `RunSequence` is called with
  std::string script;  ///< `CurrentGame/…` or `CurrentMap/…`
  std::string wizard;  ///< editor metadata; empty in all 308
  /// `autorunallowed`, defaulting to true when the attribute is absent.
  ///
  /// **What starts at map start.** 262 of the 308 allow autorun and 46 do not,
  /// and the association with `RunSequence` is decisive: 32 of the 46 are named
  /// by a `RunSequence` / `IsRunning` / `IsWaiting` / `IsFinished` literal
  /// somewhere in their own container, against 5 of the 262. A sequence that
  /// forbids autorun is one another script starts.
  ///
  /// This sentence used to search for `RunSequence` / `StopSequence` /
  /// `IsSequenceRunning` and report 31 against 2. It reproduced exactly,
  /// because neither of the two phantom names exists anywhere -- see the note
  /// above `add_sequence`. A search set with dead entries in it is a search
  /// set nobody can audit from its own output.
  bool autorun_allowed = true;
};

/// Every `<sequence>` of one `sequences.xml`, in document order.
[[nodiscard]] Result<std::vector<SequenceRef>> parse_sequences(std::span<const std::byte> xml);

/// The container entry a `<sequence script="…">` names.
///
/// The attribute is a virtual path whose first component is the root —
/// `CurrentGame` for the container, `CurrentMap` for the map — and `base` is
/// the directory that root stands for (`""` and `Maps/<n>` respectively). The
/// rest is taken verbatim, including its case: the shipped conquest writes
/// `CurrentGame/sequences/seq0.vs` where the entry is `Sequences/seq0.vs`, and
/// the container index is case-insensitive, so nothing needs to fold here.
[[nodiscard]] std::string sequence_entry_path(std::string_view script, std::string_view base);

// --------------------------------------------------------------------------
// territories.xml
// --------------------------------------------------------------------------

/// A territory's `state` attribute.
///
/// **The three values are proven**, read out of `gbr.exe`'s registration block
/// at VA `0x503800`, which pushes each literal beside its name and calls
/// `RegisterConstant` (`0x69c3a0`, a `__thiscall` taking `(const char* name,
/// int value)`):
///
/// ```
///     push 1 ; push 0x7BD1E8 "tsOwned"    ; call RegisterConstant
///     push 0 ; push 0x7BD1E0 "tsEnemy"    ; call RegisterConstant
///     push 2 ; push 0x7BD1D4 "tsDisabled" ; call RegisterConstant
/// ```
///
/// **The registration order is not the string-pool order.** The pool lists them
/// `tsDisabled`, `tsEnemy`, `tsOwned`, which reads as 0, 1, 2 and is exactly
/// wrong for two of the three. Reading values off adjacency would have made
/// `tsDisabled` the value the shipped file carries.
enum class TerritoryState : std::int32_t {
  enemy = 0,     ///< `tsEnemy`
  owned = 1,     ///< `tsOwned` — what all seven map scripts write on victory
  disabled = 2,  ///< `tsDisabled`
};

/// A raw `state` attribute as a `TerritoryState`, or an error outside 0..2.
[[nodiscard]] Result<TerritoryState> territory_state(std::int32_t value);

/// What the retail conquest's `territories.xml` gives every one of its seven
/// territories: `state="1"`, which is `tsOwned`.
///
/// A fact about the file, not an interpretation of it. Seven owned territories
/// cannot be the position a conquest starts from, and the authoring copy at
/// `ConquestMaps/1 - GBR europe/territories.xml` disagrees with it anyway — it
/// marks Spain and Italy `tsOwned` and the other five `tsEnemy`. See the
/// header's refusal list.
inline constexpr TerritoryState kShippedInitialTerritoryState = TerritoryState::owned;

/// One `<territory>` of `territories.xml`.
struct Territory {
  /// `id`: the handle `SetTerritoryState` and `neighbours` use. Seven distinct
  /// values in the retail file, all plain ASCII.
  std::string id;
  /// `index`: 2..8 in the retail file, distinct, and **not** the `Maps/<n>`
  /// number of the territory's map. What it indexes was not established; the
  /// campaign-map art in `ConquestMaps/<data>/` is the obvious candidate.
  std::int32_t index = 0;
  /// `state`, as authored. Live state lives in `CampaignProgress`, not here.
  TerritoryState state = TerritoryState::enemy;
  /// `visualname`: the Latin name shown to the player (`Hispania`, `Roma`).
  std::string visual_name;
  /// `mapname`: matches the `name` attribute of exactly one `Maps/<n>/map.xml`
  /// in the container. All seven resolve; `resolve_maps` does the matching.
  std::string map_name;
  std::string description;
  /// `bonus`: **the name of a `<sequence>` in the container-root
  /// `Sequences/sequences.xml`**, not a class name, despite reading exactly
  /// like one (`rIberia`, `rRepublicanRome`). All seven are marked
  /// `autorunallowed="no"` so that they run only when invoked by name.
  std::string bonus;
  std::string bonus_description;
  /// `neighbours`: a comma-separated list of territory `id`s, split and trimmed,
  /// in the order written. All 22 references in the retail file resolve.
  std::vector<std::string> neighbours;
  /// `interface`: the territory's **race**, as the race enumeration numbers
  /// it (`race_from_name`: Gaul 0, RepublicanRome 1, Carthage 2, Iberia 3,
  /// ImperialRome 4, Britain 5, Egypt 6, Germany 7). 0x00506560 keeps the
  /// value only when `0 <= v < 8`, the eight races; and all seven shipped
  /// territories agree with their `bonus` sequence's `r<Race>` name
  /// (`rIberia` is 3, `rBritain` 5, `rGaul` 0 ...). It is what the interface
  /// skin -- `INFOBAR_<RACE>.INI`, hence the name -- and the human's race are
  /// when a conquest starts from the territory; -1 when the file has none.
  std::int32_t interface_id = -1;
};

/// `territories.xml`: a conquest's campaign map.
///
/// One document, in one container, in the whole retail install. Everything the
/// element carries is kept, because a corpus of one gives no basis for deciding
/// a field is uninteresting.
class ConquestMap {
 public:
  [[nodiscard]] static Result<ConquestMap> parse(std::span<const std::byte> xml);
  [[nodiscard]] static Result<ConquestMap> from_document(const XmlDocument& doc);

  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  /// `data`: a path to a directory *outside* the container holding the campaign
  /// map bitmaps — `ConquestMaps/1 - GBR Europe` in the retail file, which
  /// exists on disk as `ConquestMaps/1 - GBR europe` with a lower-case `e`.
  /// Resolve it case-insensitively.
  [[nodiscard]] const std::string& data_path() const noexcept { return data_; }
  /// `choose`. `1` in the retail file. Reads as "the player picks a starting
  /// territory", which the seven identical `state` values are consistent with;
  /// not proven.
  [[nodiscard]] bool choose() const noexcept { return choose_; }
  /// `ConqueredOrder`, verbatim. Empty in the retail file. `gbr.exe` writes it
  /// with the format string `, %d`; `parse_conquered_order` splits it.
  [[nodiscard]] const std::string& conquered_order() const noexcept { return conquered_order_; }
  [[nodiscard]] std::int32_t interface_id() const noexcept { return interface_id_; }

  [[nodiscard]] const std::vector<Territory>& territories() const noexcept {
    return territories_;
  }

  /// The twelve colourisation knobs, in document order, as (name, value).
  /// `owned_colorize`, `enemy_colorize`, `disabled_colorize` and their `_hue`
  /// and `_sat` companions. Kept as written rather than as fields because they
  /// are display parameters this engine does not yet use and the retail file is
  /// the only sample; `disable_colorize` (sic) appears in the authoring copy
  /// where the container spells it `disabled_colorize`.
  [[nodiscard]] const std::vector<std::pair<std::string, std::int32_t>>& display() const noexcept {
    return display_;
  }

  /// Index of the territory with this `id`, or -1. Case-sensitive: every
  /// reference in the retail data matches exactly.
  [[nodiscard]] std::int32_t find(std::string_view id) const noexcept;

  /// Every `neighbours` entry resolves to a declared territory, and no `id`
  /// repeats. Both hold for the retail file. A conquest whose graph does not
  /// close is a container this reader does not understand.
  [[nodiscard]] Status validate() const;

  /// Every territory's `bonus` names one of `sequence_names`.
  ///
  /// Separate from `validate` because it needs the container-root
  /// `Sequences/sequences.xml`, which `territories.xml` alone does not carry.
  /// `sequence_names` is a span so the caller keeps its own ordering.
  [[nodiscard]] Status validate_bonuses(std::span<const std::string> sequence_names) const;

 private:
  std::string name_;
  std::string data_;
  std::string conquered_order_;
  std::vector<Territory> territories_;
  std::vector<std::pair<std::string, std::int32_t>> display_;
  std::int32_t interface_id_ = -1;
  bool choose_ = false;
};

/// Split a `ConqueredOrder` attribute into its integers.
///
/// The format string in `gbr.exe` is `, %d` — comma, space, integer — and the
/// attribute is empty in the one shipped conquest, so this accepts commas with
/// or without surrounding space and ignores empty fields. It does **not** say
/// what the integers index; see the header.
[[nodiscard]] std::vector<std::int32_t> parse_conquered_order(std::string_view text);

/// One territory paired with the `Maps/<n>` number its `mapname` resolves to.
struct TerritoryMap {
  std::int32_t territory = -1;  ///< index into `ConquestMap::territories()`
  std::int32_t map_number = -1; ///< the `Maps/<n>` directory number, or -1
};

/// Resolve every territory's `mapname` against the maps a container holds.
///
/// `map_names` is `(number, name)` for each `Maps/<n>/map.xml`, in whatever
/// order the caller enumerated the container; the result is in *territory*
/// document order, so it is deterministic regardless. All seven resolve in the
/// retail conquest. A territory whose `mapname` matches nothing gets
/// `map_number == -1` rather than an error: the caller decides whether a
/// conquest with an unplayable territory is loadable.
[[nodiscard]] std::vector<TerritoryMap> resolve_maps(
    const ConquestMap& conquest,
    std::span<const std::pair<std::int32_t, std::string>> map_names);

// --------------------------------------------------------------------------
// the state that crosses a mission boundary
// --------------------------------------------------------------------------

/// Everything a finished mission hands to the next one.
///
/// This is the whole of the campaign's persistent state, and it is small
/// because the shipped scripts make it small — see the header. It is a value
/// type with ordered members and no handles, so `sim/save.hpp` can write it
/// field by field without reaching into this file's internals.
struct CampaignProgress {
  /// Live territory states, parallel to `ConquestMap::territories()` and in the
  /// same order. Empty for an adventure or a scenario.
  std::vector<TerritoryState> states;
  /// Territory indices in the order they were conquered, appended once each.
  /// Corresponds to `ConqueredOrder`; the numbering this engine uses is the
  /// index into `ConquestMap::territories()`, which is document order.
  std::vector<std::int32_t> conquered;
  /// What `ConquestBonus()` answers on the next mission: the `bonus` of one
  /// territory, or empty for none. Set by the host, never derived here.
  std::string active_bonus;

  [[nodiscard]] bool operator==(const CampaignProgress&) const = default;
};

/// What one finished mission produces.
///
/// A conquest mission reports its own territory and its own outcome, because
/// its victory sequence does exactly that: `SetTerritoryState("Spain", tsOwned)`
/// then `EndGame(1, false, ...)`. The pair is recorded rather than derived so
/// that a caller can replay a campaign from a list of these.
struct MissionResult {
  /// The `Maps/<n>` number that was played.
  std::int32_t map_number = -1;
  /// Index into `ConquestMap::territories()`, or -1 for an adventure mission.
  std::int32_t territory = -1;
  /// True when the human's side finished the mission alive and victorious.
  /// `sim/match.hpp`'s `MatchStatus::human_won` is what fills it.
  bool won = false;
};

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

struct CampaignCarry;

/// The campaign, hung on the world as a `System`.
///
/// No per-turn behaviour: `advance` does nothing, exactly as `MatchSystem`'s
/// does nothing. The state changes only when a script calls
/// `SetTerritoryState`, which is a handful of times per mission.
///
/// It is a `System` rather than a field on the session for the reason
/// `sim/system.hpp` gives and `MatchSystem` follows: it is state a script
/// mutates, so it has to serialise and it has to hash. Two peers that disagree
/// about which territories are owned have desynchronised in a way that shows up
/// only at the *next* mission's start, which is far too late to diagnose.
///
/// The immutable declaration — the territory table itself — is configuration,
/// identical on every peer, and is not hashed.
class CampaignSystem final : public System {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "campaign"; }
  void advance(World& world, const Turn& turn) override;
  void hash(std::uint64_t& accumulator) const override;

  // -- setup -------------------------------------------------------------

  /// Install a conquest's territory table and take its authored states as the
  /// starting progress. Replaces anything already installed.
  void configure(const ConquestMap& conquest);

  /// Install progress carried in from an earlier mission. Refuses a
  /// `states` vector whose length does not match the configured table, because
  /// a short one would silently leave the tail at its authored value.
  [[nodiscard]] Status restore(const CampaignProgress& progress);

  /// The state to hand to the next mission, or to a save.
  [[nodiscard]] const CampaignProgress& progress() const noexcept { return progress_; }

  /// Install a carry from an earlier mission. Refuses one whose territory
  /// ids are not this table's, in this order -- a file written for another
  /// conquest, or for this one re-authored -- and then refuses what
  /// `restore` refuses. The active bonus comes with it.
  [[nodiscard]] Status restore(const CampaignCarry& carry);

  /// What this mission leaves for the next one, given how it ended.
  ///
  /// The progress as it stands -- the victory sequence has already written
  /// its territory's state -- with `result` folded in by
  /// `apply_mission_result`, and the active bonus set to the most recently
  /// conquered territory's. **That last step is the inference
  /// `last_conquered_bonus` labels**, wired here and nowhere else, so that
  /// whoever establishes the answer has one line to change. A lost mission
  /// leaves the progress and the bonus exactly as they were.
  [[nodiscard]] CampaignCarry carry(std::string_view container, const MissionResult& result) const;

  /// The territory ids, in `ConquestMap::territories()` order. Empty until
  /// `configure` runs.
  [[nodiscard]] const std::vector<std::string>& territory_ids() const noexcept { return ids_; }

  /// Index of `id`, or -1.
  [[nodiscard]] std::int32_t find(std::string_view id) const noexcept;

  /// The `bonus` of one territory, by index, or empty for an index out of
  /// range. Kept alongside the ids so that a host which has to answer
  /// `ConquestBonus()` between missions does not need the `ConquestMap` itself,
  /// which is a whole document it would otherwise have to hold open.
  [[nodiscard]] std::string_view bonus_of(std::int32_t territory) const noexcept;

  // -- the sequences a mission is made of ---------------------------------
  //
  // `gbr.exe` registers **five** entry points here, in one block at
  // 0x005bcb90: `RunSequence` (0x005bc170), `IsRunning` (0x005ba220),
  // `IsWaiting` (0x005ba420), `IsFinished` (0x005ba620) and the debug dump
  // `_SequencesStatus` (0x005ba0e0).
  //
  // **There is no `StopSequence` and no `IsSequenceRunning`.** Neither string
  // occurs anywhere in the image, and neither has a call site in the 885
  // shipped scripts. Three places in this tree named them as the family, this
  // header among them; they are a documentation error rather than a deferred
  // feature, and adding them would invent both a name and a semantics.
  //
  // **Status is not a field.** The original keeps it as a string in the
  // environment registry under `/SequenceStatus/<name>` -- the getter
  // (0x005b9a90) and setter (0x005b9990) both build that key from the literal
  // at 0x007ccef8 and the sequence's own name -- with three values compared
  // byte for byte against `"Running"`, `"Waiting"` and `"Finished"`. Anything
  // else, which in practice means an absent key, is written back as
  // `"Waiting"`. So this needs no save section of its own: `EnvStore` already
  // hashes and serialises, and the status rides in it.
  //
  // The name -> running-script association *is* new, and is rebuilt rather
  // than saved: no two `<sequence>` entries in any container resolve to the
  // same script path, so it is derivable from the manifest, which is
  // configuration. It is not hashed, for the same reason `ids_` and `bonuses_`
  // are not.

  /// One `<sequence>`, as `RunSequence` needs it: what the manifest called it
  /// and where its script lives.
  struct SequenceEntry {
    std::string name;    ///< the `name` attribute, and `RunSequence`'s argument
    std::string script;  ///< the entry path, already resolved against the base
    /// The script currently running it, or `script::kNoScript`. **Saved**, by
    /// sequence name, in the campaign section: the manifest is rebuilt on
    /// load, but the thread a sequence is waiting on is not configuration.
    /// It used to be left to the rebuild, which holds the ids the *fresh*
    /// session spawned before the load replaced its scripts -- so a sequence
    /// running at the save was never marked `"Finished"` when its script
    /// ended, or was marked so at once when a stale id named a dead one,
    /// and `RunSequence` then refused or restarted it accordingly.
    script::ScriptId running = script::kNoScript;
  };

  /// Install one manifest entry. Manifest order, because a script id is world
  /// state and the order they are declared is the order they are spawned.
  ///
  /// The original merges a container's root manifest and its map's into one
  /// map keyed by name and reports `%s duplicate sequence name` on a
  /// collision, the map's entry winning. No shipped container has such a
  /// collision -- zero across all 24 -- so a merged table and a scope-local
  /// one answer identically on every shipped site, and merged is the faithful
  /// one.
  void add_sequence(std::string_view name, std::string_view script);

  /// The entry `RunSequence(name)` names, or null. Case-sensitive, as the
  /// original's `std::map<std::string, CVXSequence*>` is.
  [[nodiscard]] SequenceEntry* find_sequence(std::string_view name) noexcept;

  /// Every declared sequence, in manifest order.
  [[nodiscard]] const std::vector<SequenceEntry>& sequences() const noexcept {
    return sequences_;
  }

  // -- the notes a mission pins on the player's list ----------------------
  //
  // `GiveNote/1` (115 sites), `RemoveNote/1` (65), `IsNoteActive/1` (24) and
  // `ClearNotes/0` (16) — 220 sites, all inside map containers. The split is
  // the same one the sequences have: the **catalogue** is configuration, read
  // from the container's `Notes.xml` documents and rebuilt on both sides of a
  // save, and the **board** of active ids is world state, because
  // `IsNoteActive` is a predicate a running script branches on. See
  // `sim/note.hpp` for the rule that a note nothing declares cannot be given,
  // and for the two shipped call sites where that fires.
  //
  // `ShowNotes/0` is `gbr.exe`'s fifth entry point in the same registration
  // block (0x00557c60) and is not bound: it opens the interface's note panel
  // and has zero call sites in the installation — the rectangle family's rule.

  [[nodiscard]] NoteCatalogue& notes() noexcept { return notes_; }
  [[nodiscard]] const NoteCatalogue& notes() const noexcept { return notes_; }
  [[nodiscard]] NoteBoard& note_board() noexcept { return board_; }
  [[nodiscard]] const NoteBoard& note_board() const noexcept { return board_; }

  // -- the conversations a mission plays ----------------------------------
  //
  // `RunConv/1` (74 sites), `Conversation::SetActor/2` (63),
  // `Conversation::Run/0` (54), `Conversation::Init/1` (54) and
  // `ConvResult/1` (5) -- 250 sites, all inside map containers, and the same
  // split the notes have. The **catalogue** is configuration, read from the
  // container's `Maps/<n>/Conversations/*.conv.xml` and rebuilt on both sides
  // of a save; the **results** are world state, because `ConvResult` is a
  // predicate a running script branches on and one of the five sites branches
  // all the way to `ChangeMap`.
  //
  // They live here rather than in a system of their own for the reason the
  // notes do: it is the same shape, it is the same lifetime, and a system per
  // catalogue would be a save section and a hash contributor per catalogue.
  // See `sim/conversation.hpp`.

  [[nodiscard]] ConversationCatalogue& conversations() noexcept { return conversations_; }
  [[nodiscard]] const ConversationCatalogue& conversations() const noexcept {
    return conversations_;
  }
  [[nodiscard]] ConversationResults& results() noexcept { return results_; }
  [[nodiscard]] const ConversationResults& results() const noexcept { return results_; }

  /// Mark every sequence whose script has ended `"Finished"`, and clear its id.
  ///
  /// The original does this from a completion callback (0x005b9f50) that finds
  /// the sequence whose recorded thread matches the one that ended. Called once
  /// a turn from `GameSession::advance`, which is the only place that holds
  /// both this table and the scheduler.
  void reap_sequences(World& world, const script::Scheduler& scheduler);

  // -- the script surface ------------------------------------------------

  /// `GetTerritoryState(str id)`. Returns `fallback` for an unknown id.
  [[nodiscard]] TerritoryState state_of(
      std::string_view id, TerritoryState fallback = TerritoryState::enemy) const noexcept;

  /// `SetTerritoryState(str id, int state)`.
  ///
  /// Writes the state and, when it is `TerritoryState::owned`, appends the
  /// territory to `conquered` if it is not already there — which is how
  /// `ConqueredOrder` is maintained, and the only place it is.
  ///
  /// Returns false for an unknown id, which is what the entry point reports.
  bool set_state(std::string_view id, TerritoryState state);

  /// `ConquestBonus()`. Returns `CampaignProgress::active_bonus` verbatim.
  [[nodiscard]] std::string_view active_bonus() const noexcept {
    return progress_.active_bonus;
  }
  void set_active_bonus(std::string_view bonus);

  // -- the saved game ----------------------------------------------------
  //
  // Layout and rationale: docs/formats/save.md. The definitions live together
  // in `src/sim/save_systems.cpp` rather than in this domain's own `.cpp`, so
  // that the ten systems' state vectors are one file to audit: adding a field
  // here and forgetting its section shows up as a diff that does not touch the
  // one place every section is written.

  /// Append this system's state to `out`, as one self-describing section with
  /// its own magic and version -- the shape `Scheduler::serialize` established.
  ///
  /// **Written:** `CampaignProgress`, which is the whole of what a mission can
  /// change. `progress()` and `restore()` already do the work; this is the
  /// byte layer over them.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** the territory id and bonus tables, which come from the
  /// `ConquestMap` through `configure` and are the same on both sides of a
  /// load. `restore` refuses progress whose territory count disagrees with
  /// them, so a save from a different conquest is caught rather than loaded.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  std::vector<std::string> ids_;
  /// Configuration, not state: rebuilt from the manifest, never hashed, never
  /// serialised. See the note above `add_sequence`.
  std::vector<SequenceEntry> sequences_;
  std::vector<std::string> bonuses_;
  /// Configuration, like `ids_` and `sequences_`: rebuilt from the container.
  NoteCatalogue notes_;
  /// State. Hashed by `hash` and written by `serialize`.
  NoteBoard board_;
  /// Configuration again: the container's `.conv.xml` documents.
  ConversationCatalogue conversations_;
  /// State again, and for the same reason the board is.
  ConversationResults results_;
  CampaignProgress progress_;
};

/// The campaign system on a world, or null. Name lookup, like
/// `match_system_of`, because registration order is part of the simulation's
/// definition.
/// Write one sequence's `/SequenceStatus/<name>` key.
///
/// Free functions rather than members because the status lives in `EnvStore`,
/// not on `CampaignSystem` -- that is where `gbr.exe` keeps it, and it is what
/// lets the whole feature ride in a save section that already exists.
void set_sequence_waiting(World& world, std::string_view name);
void set_sequence_running(World& world, std::string_view name);

[[nodiscard]] CampaignSystem* campaign_system_of(World& world) noexcept;
[[nodiscard]] const CampaignSystem* campaign_system_of(const World& world) noexcept;

// --------------------------------------------------------------------------
// applying a result
// --------------------------------------------------------------------------

/// Fold a finished mission into a campaign's progress.
///
/// A lost mission changes nothing: no shipped script writes a territory state
/// on defeat, and the four defeat sequences call `EndGame(1, true, ...)` and
/// stop. A won mission is expected to have written its own state already —
/// that is what its victory sequence does — so this only appends the territory
/// to `conquered` when the mission's own script did not, and never overwrites a
/// state.
///
/// Returns true when anything changed.
bool apply_mission_result(CampaignProgress& progress, const MissionResult& result);

/// The `bonus` of the most recently conquered territory, or empty.
///
/// **Inference, and deliberately not wired to anything.** `ConquestBonus()`
/// returns one bonus name and `ConqueredOrder` is an ordered list, which is the
/// whole of the evidence for "most recent"; a player choosing their reward at
/// the campaign map fits it equally well. Provided so that whoever establishes
/// the answer has one place to change, and so that a test can pin the reading
/// rather than leave it undocumented in a comment.
[[nodiscard]] std::string_view last_conquered_bonus(const ConquestMap& conquest,
                                                    const CampaignProgress& progress) noexcept;

// --------------------------------------------------------------------------
// the campaign between missions
// --------------------------------------------------------------------------
//
// What a finished mission leaves on disk for the next one to start from. The
// original keeps it as the conquest's `territories.xml`, rewritten, under
// the `ConquestTempFolder/` mount (`docs/formats/adventure.md`); this engine
// keeps the same facts in an INI document, `<installation>/Saves/<conquest>.
// campaign.ini`, because an INI is the shipped configuration format, a
// person can read it, and nothing decodes the original's -- there is no
// conquest in progress in a retail install to decode.
//
//     [Campaign]
//     container=Conquests/mediterranean.BFHP     which conquest this belongs to
//     territories=Spain,Italy,Gaul,...           the ids, in table order
//     states=1,0,0,...                           `TerritoryState` per territory
//     conquered=0,3                              indices, in the order conquered
//     active_bonus=rIberia                       what `ConquestBonus()` answers next
//
// `territories` is carried so that a restore can refuse a file written for a
// different conquest, or the same conquest re-authored, by name rather than
// by a matching count.

/// The campaign between missions.
struct CampaignCarry {
  /// The conquest container, in the manifest spelling `save_file.hpp` uses.
  std::string container;
  /// `CampaignSystem::territory_ids()` at the time of writing.
  std::vector<std::string> territories;
  CampaignProgress progress;

  [[nodiscard]] bool operator==(const CampaignCarry&) const = default;
};

/// The carry as the INI text above.
[[nodiscard]] std::vector<std::byte> encode_campaign_carry(const CampaignCarry& carry);
/// Parse one. Refuses a document without a `[Campaign]` section or a
/// `container`, a `states` list whose length differs from `territories`, a
/// state outside 0..2, a conquered index outside the table, and a field that
/// is not entirely numbers.
[[nodiscard]] Result<CampaignCarry> decode_campaign_carry(std::span<const std::byte> ini);

// --------------------------------------------------------------------------
// the host surface
// --------------------------------------------------------------------------

/// Define the three entry points this domain owns.
///
/// **None of the three is in `declare_shipped_surface`.** That inventory is
/// built from the 577 `.vs` files in `data.pak`, and all three of these are
/// reached only from scripts stored *inside* a conquest container, which no
/// pack carries. They are new names, and `gbr.exe` is the only source for them:
///
/// All three are registered at VA `0x503800` through `gbr.exe`'s **numeric**
/// registrar (`0x699bb0`), not the signature-string one (`0x699d20`), so there
/// is no signature string for any of them and no argument names exist in the
/// binary to recover. The types come from the pushed type codes, against the
/// table the 35 `RegisterType` sites define (`str` = 0x0B, `int` = 0x01):
///
///   * `ConquestBonus` — return `0x0b`, 0 arguments: **`str ConquestBonus()`**.
///     One call site in the install, `EnvWriteString("/Bonus",
///     ConquestBonus())` in the conquest's `Sequences/seq0.vs`.
///   * `SetTerritoryState` — return void, 2 arguments `[0x0b, 0x01]`:
///     **`void SetTerritoryState(str, int)`**. Seven call sites, one per
///     conquest map, all of the form `SetTerritoryState("<id>", tsOwned)`.
///   * `GetTerritoryState` — return `0x01`, 1 argument `[0x0b]`:
///     **`int GetTerritoryState(str)`**. Reached by **nothing** in the install;
///     the arity is the binary's, not a guess from symmetry.
///
/// The constants `tsDisabled`, `tsEnemy` and `tsOwned` are **not** registered
/// here: they are ambient globals and go through `Host::global`, which
/// `sim/globals.cpp` owns. Their values are now proven — see `TerritoryState`
/// — so that file can answer them; this one cannot reach it.
///
/// Returns the number defined, so a caller can assert the count.
std::size_t register_campaign_host(script::HostRegistry& registry);

/// The number `register_campaign_host` defines.
[[nodiscard]] std::size_t campaign_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
