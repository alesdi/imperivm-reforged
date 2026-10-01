#pragma once

/// The three keyed stores the scripts talk to: the environment, the AI
/// variables, and the research/unit-availability tables.
///
/// Together these are roughly 1,800 call sites, the largest single block of
/// host surface in `docs/formats/vs-host-api.md`. They are grouped here because
/// they are one mechanism wearing three hats: the retail engine keeps all of
/// them in the same string-keyed registry (`CVXEnvironment`, which `gbr.exe`
/// dumps under the banner `--------[ Script environment ]--------`), and the
/// research system is nothing but a convention over it.
///
/// ## The environment store is a path-keyed registry of strings
///
/// `gbr.exe` carries the exact key format strings, and they settle the shape of
/// the store beyond argument:
///
/// ```
///   /%s/Player%d/%s              /%s/Settlement%d/%s
///   /%s/Settlement%d/Bld%d/%s    /%s/Player%d/maxtrainlevel
///   /%s/Settlement%d/Reserve%d   /%s/Settlement%d/ReserveFor%d
/// ```
///
/// and, alongside them, the six-way overload set with its declared signatures:
///
/// ```
///   int,  str key                      int,  Settlement set, str key
///   void, str key, int val             void, Settlement set, str key, int val
///   ...                                (and the same six for Building and int plr)
/// ```
///
/// Four things follow, and all four are load-bearing.
///
///   1. **The scope is part of the key, not a separate table.** A player, a
///      settlement and a building are three different path prefixes into one
///      flat namespace. There is also a *root* scope — the one-argument
///      overloads — which the containers call 403 times; see "the root scope"
///      below.
///   2. **The value is a string.** The registry's dump format (`env-item`,
///      ` = "`) and the object encoding `$$%d$$` sit in the same string block.
///      That is why `SetsPlr, maxtrainlevel, 4` can be written by
///      `ONFINISH_RESEARCH.VS` with `EnvWriteString` and read back by
///      `UNIT_TRAIN.VS` with `EnvReadInt`: it is one slot, and `EnvReadInt`
///      parses it. A design with separate int/str namespaces would silently
///      disable every arena training upgrade in the game.
///   3. **A missing key reads as zero / empty / invalid.** Live scripts test
///      `EnvReadInt(set, "AIV_NoRepair") == 0` and
///      `EnvReadInt(this, "no_villagers") == 1` against keys nothing ever
///      writes.
///   4. **An invalid scope handle is an error.** `gbr.exe` carries
///      `Parameter #Settlement in function 'EnvWriteInt' is uninitialized or
///      invalid object.` verbatim, once per overload. We trap; see the unknowns
///      section for what we cannot tell from the binary.
///
/// ## The root scope, and why nothing normalises the path
///
/// The one-argument overloads are **not** dead. They are called 403 times, and
/// all 403 sites are in the 24 `.bfhp` containers — 191 `EnvReadInt/1`, 164
/// `EnvWriteInt/2`, 34 `EnvReadString/1`, 14 `EnvWriteString/2`, every one of
/// them inside a `Sequences/` script. `data.pak` never calls the root form,
/// which is exactly why it read as unreachable while the inventory was built
/// from the 577 pack scripts alone.
///
/// 356 of those sites pass a string literal, and it was tempting to read them
/// as needing normalisation, because two keys appear in both spellings:
/// `Conquests/mediterranean.BFHP` writes `"/Bonus"` and reads `"Bonus"`, and
/// `3_Great_Losses_Egypt.bfhp` writes `"En_Direction"` once where four other
/// sites say `"/En_Direction"`. **The binary settles it, and the answer is
/// that there is no normalisation.**
///
/// `gbr.exe`'s six root thunks pass the script's string straight to the
/// registry — no `sprintf`, no prefix (0x006ac890, `EnvWriteString(str, str)`).
/// The registry's get and set both funnel the path through one helper,
/// `MakePath` at 0x006ab0e0, which is four instructions of decision:
///
/// ```
///   0x6ab0ff  mov  al, byte ptr [edi]     ; the path's first character
///   0x6ab103  cmp  al, 0x2f               ; '/'
///   0x6ab109  je   0x6ab113               ; absolute: prefix = ""
///   0x6ab10b  mov  esi, [esp + 0x44]      ; else prefix = the thread's Context
/// ```
///
/// and then returns `prefix + path`. A leading `/` means *absolute*; anything
/// else is relative to the running script thread's `Context` string, which is a
/// `char*` field the thread serialises under exactly that attribute name
/// (0x0069cc58). It is null by default, and the only code that ever sets it is
/// the object-attached script launcher, which sets it to the object's id
/// formatted `"%d"` (0x0041ccb0). So `"/Bonus"` and `"Bonus"` are two different
/// slots under every possible context, and a sequence — which carries no
/// context at all — simply gets its literal back.
///
/// The corpus agrees once it is counted rather than sampled. Of 77 distinct
/// root literals, **75 use one spelling consistently**: 73 always absolute, and
/// `"Waves"` always relative on both its write and its seven read sites, which
/// works fine. Only the two named above mix, and both mix *one-sidedly* — the
/// signature of a typo, not of a convention. Reproducing the retail engine
/// therefore means storing the key verbatim, which is what `EnvScope::root()`
/// plus the literal key already does.
///
/// The consequence is a real defect in shipped content, and it is worth naming
/// because it will look like our bug: `mediterranean.BFHP`'s `Sequences/seq0.vs`
/// writes `EnvWriteString("/Bonus", ConquestBonus())` and dispatches on it
/// correctly, but the three bonus sequences that need to *keep* running —
/// `seq1.vs` (rBritain), `seq3.vs` (rEgypt), `seq6.vs` (rIberia) — each open
/// `while (EnvReadString("Bonus") == "r…")`, read a key nobody writes, get the
/// empty string and fall straight through. Three of the seven conquest bonuses
/// do nothing in the retail game. We reproduce that rather than repair it.
///
/// ## Iteration order is state
///
/// These stores serialise with the world and fold into the world hash, so their
/// iteration order is part of the simulation's definition (`sim/system.hpp`,
/// rule 3). A `std::unordered_map<std::string, …>` is therefore **forbidden**:
/// its order depends on the hash function, the bucket count and the insertion
/// history, none of which is world state, and two peers that walked the same
/// entries in a different order would hash differently.
///
/// `EnvStore` is a `std::vector` kept sorted by `(scope, key)` with binary
/// search, so iteration order is a pure function of the *contents* and not of
/// how they got there. `AiVarStore` is a sorted vector of players, each holding
/// a dense array indexed by variable id — index order, again content-determined.
///
/// ## No floating point, no clock, no allocation surprises
///
/// Everything here is integers and bytes. Percentages, parsing and formatting
/// are integral; `read_int` is a hand-written `Str2Int`, not `strtol` with a
/// locale.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/ai_profile.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;

// --------------------------------------------------------------------------
// the environment store
// --------------------------------------------------------------------------

/// Which path prefix a key hangs off.
///
/// One enumerator per overload family in `gbr.exe`'s registration block. The
/// numbering is ours; it orders the store and is therefore world state, so it
/// must not be reshuffled.
enum class EnvScopeKind : std::uint8_t {
  root = 0,     ///< the key verbatim — the one-argument overloads. 403 container sites.
  player = 1,   ///< `/<map>/Player<n>/<key>`
  settlement = 2,  ///< `/<map>/Settlement<id>/<key>`
  building = 3,    ///< `/<map>/Settlement<id>/Bld<n>/<key>`
};

/// A scope, as a sortable pair.
///
/// `id` is the player number for `player`, the `SettlementId` for `settlement`,
/// and the **object id** for `building`.
///
/// INFERRED, and the one place we knowingly differ from the retail key: the
/// engine addresses a building by its slot within its settlement (`Bld%d`),
/// which is a 1:1 relabelling of the object as long as a slot is never reused
/// for a different building. Keying by object id removes that "as long as", and
/// no script can observe the difference because the path is never exposed to
/// script. It would matter only for save-file interoperability with the retail
/// engine, which is not a goal.
struct EnvScope {
  EnvScopeKind kind = EnvScopeKind::root;
  std::uint32_t id = 0;

  [[nodiscard]] static EnvScope root() noexcept { return {}; }
  [[nodiscard]] static EnvScope for_player(std::int32_t player) noexcept {
    return {EnvScopeKind::player, static_cast<std::uint32_t>(player)};
  }
  [[nodiscard]] static EnvScope for_settlement(SettlementId id) noexcept {
    return {EnvScopeKind::settlement, id};
  }
  [[nodiscard]] static EnvScope for_building(ObjectId object) noexcept {
    return {EnvScopeKind::building, object};
  }

  friend constexpr bool operator==(const EnvScope&, const EnvScope&) noexcept = default;
};

/// One stored key.
///
/// The value is a string because the retail store is a string store; the typed
/// accessors coerce. See the header note.
struct EnvEntry {
  EnvScope scope;
  std::string key;
  std::string value;
};

/// `Str2Int`-shaped parse: optional sign, then decimal digits, stopping at the
/// first character that is not one. Everything else is zero, which is what a
/// script that reads `"researched"` as an int must get.
[[nodiscard]] std::int32_t env_parse_int(std::string_view text) noexcept;

/// Decimal, with a leading `-`. The inverse of `env_parse_int` on its range.
[[nodiscard]] std::string env_format_int(std::int32_t value);

/// The `$$<id>$$` encoding `gbr.exe` uses for an object inside the registry.
/// INFERRED: the literal `$$%d$$` sits next to `environment` and `env-item` in
/// the binary's string pool, and no other consumer for it is visible.
[[nodiscard]] std::string env_format_object(ObjectId id);
/// Returns `kNoObject` when `text` is not an encoded object.
[[nodiscard]] ObjectId env_parse_object(std::string_view text) noexcept;

/// The flat, sorted, scope-and-key-addressed store.
///
/// Sorted vector rather than a hash map on purpose: iteration order is world
/// state (see the header). Lookup is a binary search over `(scope, key)`.
class EnvStore {
 public:
  /// The raw slot, or null. Reads never create.
  [[nodiscard]] const std::string* find(const EnvScope& scope, std::string_view key) const noexcept;

  [[nodiscard]] std::int32_t read_int(const EnvScope& scope, std::string_view key) const noexcept;
  [[nodiscard]] std::string_view read_string(const EnvScope& scope,
                                             std::string_view key) const noexcept;
  [[nodiscard]] ObjectId read_object(const EnvScope& scope, std::string_view key) const noexcept;

  void write_int(const EnvScope& scope, std::string_view key, std::int32_t value);
  void write_string(const EnvScope& scope, std::string_view key, std::string_view value);
  void write_object(const EnvScope& scope, std::string_view key, ObjectId object);

  /// Drop a slot. Nothing in the corpus removes — `ONFINISH_RESEARCH.VS`
  /// cancels a research by writing `""` — but the store is also what a save
  /// reload rebuilds, so it needs a way back to empty.
  bool erase(const EnvScope& scope, std::string_view key);
  void clear() noexcept { entries_.clear(); }

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  /// In `(scope, key)` order, which is the order the hash folds them in.
  [[nodiscard]] std::span<const EnvEntry> entries() const noexcept { return entries_; }

  [[nodiscard]] std::uint64_t hash() const noexcept;

  // -- the saved game ----------------------------------------------------

  /// Append the whole store to `out`, with its own magic and version.
  ///
  /// A pair rather than a replay of `write_string`, because `entries()` is the
  /// hashed order and `assign` decides where a new key lands. Reproducing that
  /// order by re-running the writes would make the file's meaning depend on an
  /// insertion rule that is private and is allowed to change.
  ///
  /// Definitions in `src/sim/save_systems.cpp`. Layout: docs/formats/save.md.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace it with the one in `bytes`. **Atomic**: decoded into a local and
  /// moved in only once every row has read cleanly.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  /// Index of the entry, or of the insertion point; `found` says which.
  struct Slot {
    std::size_t index = 0;
    bool found = false;
  };
  [[nodiscard]] Slot locate(const EnvScope& scope, std::string_view key) const noexcept;
  void assign(const EnvScope& scope, std::string_view key, std::string value);

  std::vector<EnvEntry> entries_;
};

// --------------------------------------------------------------------------
// AI variables
// --------------------------------------------------------------------------

/// The `AIVar` / `SetAIVar` store: one integer array per player.
///
/// `gbr.exe` declares the entry points as
///
/// ```
///   AIVar      int,  int idPlayer, int var
///              bool, int idPlayer, int var, int bit
///   SetAIVar   void, int idPlayer, int var, int value
///              void, int idPlayer, int var, int bit, bool value
/// ```
///
/// so the key is an **integer variable id**, not a name: the `AIV_*` and
/// `AIMV_*` globals resolve to indices, and the index space is the order of the
/// `[Vars.*]` sections of the AI profile's `DATA\AI\AI.INI`. Two profiles ship
/// and their tables differ, so an id is only meaningful relative to a profile;
/// nothing here hard-codes a name or a default. Seeding the defaults is the
/// profile loader's job (`sim/ai_profile.*`).
///
/// The arity-3 forms are the "player mask" variables. `AI.INI` documents them
/// in as many words:
///
/// ```
///   SetAIVar(<AIPlayerNum>, <Variable>, <PlayerNum>, <true/false>);
///   AIVar(<AIPlayerNum>, <Variable>, <PlayerNum>); // returns bool
///   ;   SetAIVar(2, AIMV_NoAttack, 1, true);  // player 2 AI won't attack player 1
///   AIMV_NoAttack=0   ; player mask variable, setting bit i disables attacking player i
/// ```
///
/// and all seven arity-3 `AIVar` call sites in the corpus pass an `AIMV_*`
/// variable and a player number. `RECRUITER.VS` reads the same variable both
/// ways in the same function — `AIVar(p, AIMV_NoAttack) != 0` for "any bit set"
/// and `AIVar(p, AIMV_NoAttack, j)` for "bit j" — which pins the bit index to
/// the player number with no scaling.
class AiVarStore {
 public:
  /// Unset reads as zero. Negative ids and players are refused.
  [[nodiscard]] std::int32_t get(std::int32_t player, std::int32_t var) const noexcept;
  /// Bit `bit` of the variable, as `AIVar`'s arity-3 form returns it.
  [[nodiscard]] bool bit(std::int32_t player, std::int32_t var, std::int32_t bit) const noexcept;

  bool set(std::int32_t player, std::int32_t var, std::int32_t value);
  bool set_bit(std::int32_t player, std::int32_t var, std::int32_t bit, bool value);

  void clear() noexcept { players_.clear(); }
  [[nodiscard]] std::size_t player_count() const noexcept { return players_.size(); }
  [[nodiscard]] std::uint64_t hash() const noexcept;

  /// The engine rejects player numbers outside 1..16 (`Function "AIStart":
  /// Player number should be between 1 and 16`), and `RECRUITER.VS` walks
  /// exactly that range when it scans a mask. We accept 0 as well, because the
  /// meaning of the integer a script passes is the object model's business and
  /// not ours to reinterpret, and cap the table so a wild id cannot allocate.
  static constexpr std::int32_t kMaxPlayer = 64;
  /// A defensive ceiling only. `AI.INI` declares 118 variables.
  static constexpr std::int32_t kMaxVar = 4096;

  // -- the saved game ----------------------------------------------------

  /// Append the whole store to `out`, with its own magic and version.
  ///
  /// A pair rather than a replay of `set`, because `hash` folds the whole dense
  /// `values` vector, trailing zeros included, and the vector's length is a
  /// function of the highest variable ever written -- which a replay of only
  /// the non-zero entries would not reproduce.
  ///
  /// Definitions in `src/sim/save_systems.cpp`. Layout: docs/formats/save.md.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace it with the one in `bytes`. **Atomic**: decoded into a local and
  /// moved in only once every row has read cleanly.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  struct PlayerVars {
    std::int32_t player = 0;
    std::vector<std::int32_t> values;  ///< dense, indexed by variable id
  };
  [[nodiscard]] const PlayerVars* find(std::int32_t player) const noexcept;

  std::vector<PlayerVars> players_;  ///< sorted by `player`
};

// --------------------------------------------------------------------------
// research
// --------------------------------------------------------------------------

/// Whose ledger a research is written to.
///
/// `DATA\COMMANDS\*.XML` says which for every research command, in its `param`
/// attribute, and `ONFINISH_RESEARCH.VS` is the code that acts on it:
/// `NameSet` writes `EnvWriteString(.settlement, name, "researched")`,
/// `NamePlr` writes `EnvWriteString(.player, name, "researched")`.
enum class ResearchScope : std::uint8_t {
  none = 0,
  settlement,
  player,
};

/// The literal the ledger holds. `gbr.exe` carries both strings.
inline constexpr std::string_view kResearched = "researched";
inline constexpr std::string_view kResearching = "researching";

/// One key/value the command writes when it completes: `SetsSet` / `SetsPlr`.
struct ResearchAssignment {
  ResearchScope scope = ResearchScope::none;
  std::string key;
  std::string value;
};

/// One prerequisite: `ReqSet` / `ReqPlr` / `NReqSet`.
struct ResearchRequirement {
  ResearchScope scope = ResearchScope::none;
  std::string name;
  /// `NReqSet`: satisfied when the named research is *not* done. The Carthage
  /// specialisations use it to make five commands mutually exclusive.
  bool negated = false;
};

/// A `<cmd method="research">` from `DATA\COMMANDS\*.XML`, reduced to what the
/// simulation needs.
///
/// **This is where the research tree lives.** There are 145 such commands
/// across the 35 command files, and their `param` attribute is the whole graph:
/// `ReqSet`/`ReqPlr`/`NReqSet` are the edges, `NameSet`/`NamePlr` is the node,
/// `SetsSet`/`SetsPlr` is the payload. The two scripts that interpret it,
/// `VERIFY_RESEARCH.VS` and `ONFINISH_RESEARCH.VS`, both open with the grammar
/// written out as a comment.
struct ResearchCommand {
  std::string name;      ///< `<cmd name="…">`, and the string `IsResearched` is called with
  ResearchScope scope = ResearchScope::none;  ///< from `NameSet` / `NamePlr`
  /// The name recorded in the ledger, which is the `NameSet`/`NamePlr`
  /// argument and is **not** always the command name: `<cmd name="Gaul
  /// Training 1" … param="… NamePlr, Training, ">` records `Training`.
  std::string records;
  std::vector<ResearchRequirement> requires_;
  std::vector<ResearchAssignment> assigns;
  std::int32_t cost_gold = 0;  ///< `costgold`
  std::int32_t cost_food = 0;  ///< `costfood`
};

/// Parse a `<cmd param="…">` into a `ResearchCommand`.
///
/// The grammar is comma-separated triples `<verb>, <name>, <value>`, exactly as
/// `VERIFY_RESEARCH.VS` parses it with `ParseStr`. Recognised verbs are
/// `NameSet`, `NamePlr`, `ReqSet`, `ReqPlr`, `NReqSet`, `SetsSet`, `SetsPlr`;
/// anything else is skipped, and so is the third field, which is a UI
/// explanation (`default`, or a translatable refusal message) rather than
/// simulation state.
///
/// Lives in core rather than in a loader because it is pure string work with no
/// filesystem in it, and because the two scripts that define the grammar are
/// the specification: keeping the parser next to the store keeps them together.
[[nodiscard]] ResearchCommand parse_research_command(std::string_view name,
                                                     std::string_view param);

/// The 145 research commands, by name.
///
/// Empty by default and filled by `build_from`, which `EnvSystem::start` calls
/// with the command system's table. With an empty catalog every entry point
/// here still answers, by falling back to the scope-agnostic reading
/// `VERIFY_ISRESEARCHED.VS` uses (look in the settlement ledger, then the
/// player's); what is lost is the prerequisite graph, so `CanResearch` degrades
/// to "not already researched or researching".
class ResearchCatalog {
 public:
  /// Build from the merged `DATA\COMMANDS\*.XML` that `sim/command.hpp`
  /// already parses.
  ///
  /// This is the whole loading path, and it is why the tree is not hard-coded
  /// anywhere: a `CommandDef` whose `method` is `research` or
  /// `immediate_research` **is** a node of the research graph, its `param` is
  /// its edges, and its `costgold`/`costfood` are the same numbers
  /// `GetCmdCost` reports. Replaces whatever was here.
  void build_from(const CommandTable& table);

  /// Later definitions of the same name replace earlier ones, so a mod's file
  /// can be loaded over the shipped one.
  void add(ResearchCommand command);
  [[nodiscard]] const ResearchCommand* find(std::string_view name) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return commands_.size(); }
  [[nodiscard]] std::span<const ResearchCommand> all() const noexcept { return commands_; }
  void clear() noexcept { commands_.clear(); }

 private:
  std::vector<ResearchCommand> commands_;  ///< sorted by `name`
};

// --------------------------------------------------------------------------
// unit availability
// --------------------------------------------------------------------------

/// One row of the engine's per-race unit table.
///
/// `gbr.exe` declares the four entry points that read it:
///
/// ```
///   UType      str, int nType, int nRace
///   UTech      str, int nType, int nRace
///   UTrainCmd  str, int nType, int nRace
///   RUType     int, str strClass, int nRace
/// ```
///
/// and carries the table itself as three parallel string runs in its pool.
struct UnitRow {
  std::string type;       ///< the unit class, e.g. `GSwordsman`
  std::string tech;       ///< the research command that enables it, or empty
  std::string train_cmd;  ///< the barrack command that builds it
};

/// One race's rows, plus its two special recruits.
struct RaceUnits {
  std::string name;  ///< diagnostics only
  std::vector<UnitRow> rows;
  UnitRow arena;   ///< `ArenaUType` / `ArenaUTech` / `ArenaTrainCmd`
  UnitRow temple;  ///< `TempleUType` / `TempleUTrain`; `tech` is unused
};

/// The whole table, indexed by the race constant.
///
/// **The race index is inferred.** The eight races appear in `gbr.exe`'s string
/// pool in the order Gaul, RepublicanRome, Carthage, Iberia, ImperialRome,
/// Britain, Egypt, Germany — five parallel runs agree on it (the unit classes,
/// the train commands, the arena techs, the arena types and the temple building
/// classes), and `ESH_NEEDTECH.VS` names the arena techs against races in its
/// comments, which matches. What is *not* proven is that the race globals
/// `Gaul`, `RepublicanRome`, … are literally 0..7 in that order; nothing in the
/// shipped data states their numeric values. See "what is still unknown".
class UnitCatalog {
 public:
  [[nodiscard]] const RaceUnits* race(std::int32_t race) const noexcept;
  [[nodiscard]] const UnitRow* row(std::int32_t race, std::int32_t type) const noexcept;
  /// The index of `type` within `race`'s rows, or -1.
  [[nodiscard]] std::int32_t index_of(std::int32_t race, std::string_view type) const noexcept;

  void set_races(std::vector<RaceUnits> races) { races_ = std::move(races); }
  [[nodiscard]] std::span<const RaceUnits> races() const noexcept { return races_; }
  [[nodiscard]] bool empty() const noexcept { return races_.empty(); }

 private:
  std::vector<RaceUnits> races_;
};

/// The table as `gbr.exe` carries it.
///
/// Every string below is in the retail binary, and every column is corroborated
/// by a second shipped source:
///
///   * the **class** column against `DATA\COUNTERUNITS.XML`, whose 52 `<unit>`
///     elements are in exactly this order, and against the `AIV_Max*` variable
///     each index is paired with in `DATA\AI\ESH_ENABLEDUNITS.VS`;
///   * the **tech** column against the second field of the barrack train
///     commands' `param` and against `DATA\COMMANDS\BLACKSMITH.XML`, where all
///     of them are `<cmd name="…" method="research">`;
///   * the **train command** column against `<cmd name="train…">` in
///     `DATA\COMMANDS\*BARRACKS.XML`.
///
/// Where the barrack file's *order* disagrees with the binary's — Iberia ships
/// its slinger before its defender in `IBARRACKS.XML` — the binary and
/// `COUNTERUNITS.XML` agree with each other and with `ESH_ENABLEDUNITS.VS`, so
/// the file order is a display order and this is the table.
[[nodiscard]] UnitCatalog shipped_unit_catalog();

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

/// The stores, hung on the world as a `System`.
///
/// It has no per-turn behaviour: `advance` does nothing. It is a `System`
/// because that is how a domain gets state onto the world and into the world
/// hash without `World` growing a field, which is the seam `sim/system.hpp`
/// documents.
class EnvSystem final : public System {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "env"; }

  /// Build the research catalog from the command system's table, if there is
  /// one. Idempotent, and safe to call again after more commands are merged.
  void start(World& world) override;

  /// Nothing to do per turn. The stores change only when a script writes.
  void advance(World& world, const Turn& turn) override;

  /// Environment and AI variables are hashed; the catalogues are configuration
  /// loaded from shipped data and identical on every peer, so they are not.
  void hash(std::uint64_t& accumulator) const override;

  [[nodiscard]] EnvStore& env() noexcept { return env_; }
  [[nodiscard]] const EnvStore& env() const noexcept { return env_; }
  [[nodiscard]] AiVarStore& ai_vars() noexcept { return ai_vars_; }
  [[nodiscard]] const AiVarStore& ai_vars() const noexcept { return ai_vars_; }
  [[nodiscard]] ResearchCatalog& research() noexcept { return research_; }
  [[nodiscard]] const ResearchCatalog& research() const noexcept { return research_; }
  [[nodiscard]] UnitCatalog& units() noexcept { return units_; }
  [[nodiscard]] const UnitCatalog& units() const noexcept { return units_; }

  // -- AI variables ------------------------------------------------------

  /// Seed one player's variables from a profile at a difficulty.
  ///
  /// **The variable id is the index into `AiProfile::variables(difficulty)`**,
  /// which is `[Vars.All]`'s declaration order. `gbr.exe` declares `AIVar` as
  /// `int, int idPlayer, int var` — the key is an integer, not a name — and the
  /// `AIV_*` names live only in `AI.INI`, not in the binary, so the numbering
  /// can only be the file's own order. An overlay profile keeps positions and
  /// replaces values (`sim/ai_profile.hpp`), so the ids are stable across the
  /// `DEFENSIVE` overlay.
  ///
  /// Nothing here hard-codes a default. A world whose profile was never loaded
  /// reads every variable as zero, which is loud in the right way: an AI that
  /// does nothing is easier to notice than one tuned to invented numbers.
  void seed_ai_vars(std::int32_t player, const AiProfile& profile, AiDifficulty difficulty);

  /// The id of an `AIV_*` / `AIMV_*` name, or -1.
  ///
  /// This is the answer `script::Host::global` owes for those 118 names, and
  /// the only place the mapping exists. `sim/world_host.cpp` does not implement
  /// `global` yet; when it does, this is what it must call.
  [[nodiscard]] std::int32_t ai_var_id(std::string_view name) const noexcept;
  /// The name of an id, or empty. For `gbr.exe`'s own `AIVars for player %d`
  /// dump, and for diagnostics.
  [[nodiscard]] std::string_view ai_var_name(std::int32_t id) const noexcept;
  /// Install the name table without seeding any player, for a test.
  void set_ai_var_names(std::vector<std::string> names);

  // -- research ---------------------------------------------------------

  /// Where a research is recorded, and under what name.
  struct ResearchSlot {
    EnvScope scope;
    std::string_view key;
    bool known = false;  ///< false when the catalog has never heard of the name
  };

  /// Resolve `name` against a settlement: the catalog decides whether the
  /// ledger is the settlement's or its owner's, exactly as `IsResearched`'s two
  /// `gbr.exe` overloads do.
  [[nodiscard]] ResearchSlot slot_for(const Settlement& s, std::string_view name) const;

  /// `IsResearched(Settlement, str)` and `IsResearched(int, str)`.
  ///
  /// With the catalog loaded this reads one ledger. Without it, it reads the
  /// settlement's and then the owner's, which is what `VERIFY_ISRESEARCHED.VS`
  /// does by hand and what makes `ESH_NEEDTECH.VS` work: that script asks a
  /// settlement about `"Fights"` (a `NameSet` research) and about
  /// `"Battle tactics"` (a `NamePlr` one) through the same call.
  [[nodiscard]] bool is_researched(const Settlement& s, std::string_view name) const;
  [[nodiscard]] bool is_researching(const Settlement& s, std::string_view name) const;
  [[nodiscard]] bool is_researched_for_player(std::int32_t player, std::string_view name) const;
  [[nodiscard]] bool is_researching_for_player(std::int32_t player, std::string_view name) const;

  /// `Settlement::CanResearch(str)`.
  ///
  /// Not already researched, not already researching, every `ReqSet`/`ReqPlr`
  /// satisfied and every `NReqSet` not satisfied — the exact set of tests
  /// `VERIFY_RESEARCH.VS` runs, minus the ones it makes about the building
  /// (`EnvReadString(this, "researching") == "yes"`) and minus cost, which
  /// `ESH_NEEDTECH.VS` checks separately with `CanAfford`.
  [[nodiscard]] bool can_research(const Settlement& s, std::string_view name) const;

  /// `RESEARCH.VS`: write `researching` into the ledger. Returns false when the
  /// name resolves to no ledger at all.
  bool begin_research(const Settlement& s, std::string_view name);
  /// `ONFINISH_RESEARCH.VS`, the non-cancelled branch: write `researched`, then
  /// apply every `SetsSet` / `SetsPlr`.
  bool finish_research(const Settlement& s, std::string_view name);
  /// `ONFINISH_RESEARCH.VS`, the cancelled branch: write `""` back.
  bool cancel_research(const Settlement& s, std::string_view name);

  // -- balance constants -------------------------------------------------

  /// Load `DATA\CONST.INI`'s `[GamePlay]` section. Returns how many keys were
  /// taken.
  ///
  /// **Parsed, not transcribed.** The section holds 362 keys — 269 whose value
  /// is entirely an integer, 93 whose value is not, and no bare lines. The
  /// alternative was to compile that table into this file, and it was the wrong
  /// answer twice over: it is a second source of truth that a patched or modded
  /// `CONST.INI` silently contradicts, and transcribing `CONST.INI` by eye is
  /// exactly how `ProductionInterval` came to be recorded as 20 when it is
  /// 2000. `imperivm/core/formats/ini.hpp` reads the real file and its
  /// `value_int` refuses a half-parsed number rather than returning the prefix,
  /// which is the guard that mistake needed.
  ///
  /// The corpus passes 74 distinct string literals to `GetConst`. 73 are keys
  /// of this section; the 74th, `RecallDistance`, is in no section of the file
  /// at all. (`docs/formats/vs-host-api.md` records 73 and says "all 73
  /// observed keys resolve there" — both counted by a scan that missed the
  /// dangling one.)
  ///
  /// A key whose value is not entirely an integer is kept as a string and is
  /// reachable through `constant_string` only — which is what `TributeTimes`
  /// (`0,   10,   20,   30`) and `TributeGold` (`500, 1000, 1500, 2000`), the
  /// corpus's only two `GetConstStr` keys, are.
  std::size_t load_constants(const IniDocument& document);

  /// `GetConst(k)` and `GetConstStr(k)`. Returns false when the key was not
  /// loaded.
  [[nodiscard]] bool constant(std::string_view key, std::int32_t& out) const;
  [[nodiscard]] bool constant_string(std::string_view key, std::string_view& out) const;
  /// Set one directly, for a test that does not want to build an INI document
  /// to say `MinPopulation = 10`.
  void set_constant(std::string_view key, std::int32_t value);
  void set_constant_string(std::string_view key, std::string_view value);
  [[nodiscard]] std::size_t constant_count() const noexcept { return int_constants_.size(); }

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
  /// **Written:** the environment store and the AI-variable store, which are
  /// the two things a script can write. Both are folded into the world hash.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** the research catalog (built from the command table), the
  /// unit catalog (`shipped_unit_catalog`), the AI-variable name table and the
  /// `CONST.INI` constants. All four are load-time: nothing in the engine
  /// writes them after `start`, and `EnvSystem` exposes no way for a script to
  /// try.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  EnvStore env_;
  AiVarStore ai_vars_;
  ResearchCatalog research_;
  UnitCatalog units_;
  /// `[Vars.All]` declaration order: index is the `AIVar` variable id.
  std::vector<std::string> ai_var_names_;
  /// `[GamePlay]`, split by whether the value parses as a whole integer.
  /// Sorted by key; lookup is a binary search and iteration order does not
  /// depend on file order. Not hashed: configuration, not state.
  std::vector<std::pair<std::string, std::int32_t>> int_constants_;
  std::vector<std::pair<std::string, std::string>> str_constants_;
};

/// The env system registered on `world`, or null. Same shape as `economy_of`:
/// systems are found by `name()`, which is already part of the run-order
/// manifest, rather than cached in `HostContext`.
[[nodiscard]] EnvSystem* env_of(World& world) noexcept;

/// Define this domain's slice of the host API and return how many entry points
/// were defined, so a caller can assert against the count.
///
/// Every name and arity below is already declared by `declare_shipped_surface`.
/// **`GetConst`/1 moved here from `sim/economy.cpp`**: a second `define()` of
/// the same (kind, name, arity) silently replaces the first, and
/// `sim/host_setup.cpp` runs env after economy, so the economy's definition
/// would have become live-looking dead code. It still answers the economy's
/// slice through `economy_constant`, so `EconomySystem::set_rules` stays
/// authoritative for the keys the economy holds as fields.
std::size_t register_env_host(script::HostRegistry& registry);

/// How many entry points `register_env_host` defines.
[[nodiscard]] std::size_t env_host_entry_count() noexcept;

// --------------------------------------------------------------------------
// what is still unknown
// --------------------------------------------------------------------------
//
// * **What the retail engine does after "uninitialized or invalid object".**
//   The message is in the binary once per `Env*` overload, but whether it
//   aborts the script, returns a default, or only prints is not recoverable
//   from strings. We return `HostOutcome::failed`. Settling it needs either a
//   disassembly of the six thunks or a live trace with a deliberately invalid
//   handle.
//
// * **`EnvReadObj/1` and `EnvWriteObj/2`.** The other four root arities are now
//   registered; these two are not. `gbr.exe` declares them at 0x006ae823 and
//   0x006ae80d — `Obj, str key` and `void, str key, Obj o` — and no script in
//   the installation calls either, so they follow the `SetAIVar/4` precedent
//   and stay declared-only. `env_target` already handles them; adding the two
//   rows is a one-line change if a witness ever turns up.
//
// * **`int` → `str` coercion.** `EnvWriteString` then `EnvReadInt` is proven by
//   the arena training upgrades. The reverse — `EnvWriteInt` then
//   `EnvReadString` — has no corpus witness. We format decimally, which is the
//   only reading consistent with a string-valued store.
//
// * **The `$$%d$$` object encoding.** Inferred from the string pool's
//   neighbourhood, not from a dump of a live registry. The only two object keys
//   in the corpus are `TributeBuilding` and three siblings, and none of them is
//   ever read as a string, so nothing observable depends on the spelling.
//
// * **Whether `CheckUEnabled` consults anything but its `max` argument.** Its
//   signature is `bool, int *mask, int idPlayer, int max, int nType, int nRace`
//   and we set the bit iff `max != 0` — which is what `AI.INI`'s own comment
//   (`0 - disable, -1: no limit`) says the value means, and what
//   `ESH_BUILDARMY.VS`'s `CheckUEnabled(nEnabled, AIPlayer, 0, 5, Carthage)`
//   uses it for. `idPlayer` and `nRace` are then unused, which is suspicious.
//   What it is *not* is a research test: `ESH_BUILDARMY.VS` line 327 asks
//   `UEnabled(nEnabled, i) && !IsResearched(set, UTech(i, nRace))`, which no
//   unit could satisfy if the mask already required the tech.
//
// * **The numeric values of the race constants.** The unit table is ordered
//   Gaul, RepublicanRome, Carthage, Iberia, ImperialRome, Britain, Egypt,
//   Germany, corroborated five ways, but the constants themselves are host
//   globals whose values no shipped file states. If they are not 0..7 in that
//   order, `UnitCatalog` needs a remap and nothing else changes.
//
// * **Germany's unit techs, and the temple/arena prerequisites.** `UTech(i,
//   Germany)` is empty for every `i` in the shipped table — the four German
//   research names (`Axemen production`, `Javelin production`, `Horseshoes
//   production`, `Macemen production`) are not in `gbr.exe` at all, and
//   `ESH_BUILDGERMANARMY.VS` carries them in a script-side `StrArray` instead
//   of calling `UTech`. That is consistent, but it means the table has a hole
//   we are reproducing rather than filling.
//
// * **`Settlement::TSResearch`.** The retail engine implements it by running
//   `data/ai/TSH_Research.vs` (`gbr.exe`: `TSResearch: Error running script!` /
//   `TSH_Research.vs`). That script is six lines and we transcribe it, but it
//   calls `set.Research(tech)`, which queues a command on a research lab. We
//   mark the ledger `researching` directly and charge nothing, because
//   `Settlement::Research/1` is declared and implemented by nobody. When it
//   lands, `TSResearch` should become a script dispatch and this native path
//   should be deleted.
//
// * **Who answers `script::Host::global` for `AIV_*`.** `AIVar`'s second
//   argument is an integer variable id, and `ai_var_id` is the only place the
//   name-to-id mapping exists -- but nothing implements `global` yet, so no
//   script can currently reach an `AIV_*` constant at all. Until it does, the
//   757 `AIVar`/`SetAIVar` call sites are reachable only from C++.
//
// * **`CanResearch` and the command queue.** `ESH_NEEDTECH.VS` comments its
//   failure as "researched, researching or impossible to research **or the
//   Queue is not empty**". We cannot test the queue, so `can_research` is
//   optimistic by exactly that one condition.

}  // namespace imperivm::core::sim
