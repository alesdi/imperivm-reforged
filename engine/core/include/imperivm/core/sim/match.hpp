#pragma once

/// The match: who the human is, who the computers are, what rule decides the
/// game, and who won.
///
/// ## Why this is a separate slice
///
/// The simulation could already run a retail map deterministically without
/// knowing any of that. A map declares its players and a victory rule, and
/// `DATA/GAMESCRIPTS` ships the four rules as ordinary `.vs` -- so almost none
/// of this is behaviour to invent. It is **wiring**, plus the handful of host
/// entry points the four scripts reach that no domain had claimed.
///
/// ## What the shipped data says
///
/// `game.xml` carries `victory_condition`, and it is a **path fragment, not an
/// integer**. `gbr.exe` holds the literal `data/GameScripts/` (0x007b8844)
/// immediately beside the save-game field name `victorycond`, and reports
/// `Error compiling victory condition script %s` when the file behind it does
/// not build. The four files in `DATA/GAMESCRIPTS` are named
///
///     1 ELIMINATION.VS
///     2 SCORE LIMIT.VS
///     3 TIME LIMIT (MILITARY RATING).VS
///     4 TIME LIMIT (SCORE).VS
///
/// and every one opens with the comment `//void, int player, str param` -- the
/// same signature `gbr.exe` stores beside `data/GameScripts/`. So a victory
/// condition is one script, started **once per player**, with that player's
/// number and `victory_threshold` as a string.
///
/// `DATA/CONST.INI` confirms the naming from the other side: its threshold
/// menus are keyed `2_Score_limit0..5`, `3_Time_limit_(military_rating)0..7`
/// and `4_Time_limit_(score)0..7` -- the script basenames with spaces turned
/// into underscores. It also holds the four constants the scripts read:
/// `SuddenDeathTimeout = 300`, `EliminationTimeout = 180`,
/// `NearScorePercent = 80`, `ScoreDelta = 1000`.
///
/// Only `"0"` and `"1 Elimination"` occur in the 22 shipped containers, so the
/// exact spelling of the other three attribute values is **not** observed.
/// Parsing therefore keys on the leading integer, which `docs/formats/map.md`
/// already recommends, and never on the whole string.
///
/// ## Elimination never declares a win
///
/// `1 ELIMINATION.VS` calls `EndGame(player, true)` and nothing else: a player
/// whose town halls are gone and whose army is gone reports its own defeat, and
/// no script anywhere reports the survivor's victory. `2`, `3` and `4` do call
/// `EndGame(player, false)`, but `1` -- the only condition the retail maps
/// actually use -- does not.
///
/// So **the winner is the host's to derive**, and `over()` / `winner()` below
/// are that derivation. It is marked as a derivation everywhere it appears
/// because it is one: `EndGame`'s two-argument body (0x004c9610) only forwards
/// the pair `(player - 1, lose != 0)` through the game object's virtual slot
/// 0x48, and what that slot does with it was not recovered.
///
/// ## `EndGame` is registered twice, and the corpus uses both
///
/// The string pool's C signature -- `"void, int player, bool lose"` at
/// 0x007b86f8 -- describes only the pair, and for a long time this header said
/// that was the whole story. It is not. The **numeric** registrar
/// (0x00699bb0), which carries no signature strings, registers the *same name*
/// a second time at 0x006aebaa:
///
///     push 0x0b   ; str
///     push 0x07   ; bool
///     push 0x01   ; int
///     push 0x03   ; three arguments
///     push 0x00   ; returns void
///     push 0x007b86f8   ; "EndGame"  -- the same string
///     push 0x004c9660   ; a *different* body
///
/// So the surface is `EndGame/2` **and** `EndGame/3`, and the split is exactly
/// where the content sits: all 24 `EndGame` sites in `data.pak` pass two
/// arguments and all 61 in the 24 shipped containers pass three. Across all
/// 885 `.vs` files in the install, `EndGame` is the **only** name called with
/// more arguments than some other site gives it -- so this is a second
/// registration, not a VM that tolerates surplus arguments, and the count was
/// checked both ways before either was written down.
///
/// 0x004c9660 pops `str`, `bool`, `int`, subtracts one from the player, and
/// then computes `(current_player - (game + 0x12cc)) / 800` -- the local
/// player's index, 800 being the player stride -- and assigns the string into
/// the game object **only when the two indices agree**. It then frees the
/// string and forwards `(player - 1, lose != 0)` through the same slot 0x48 as
/// the two-argument form. The message is one player's, and it is display text.
///
/// ## Match state is world state
///
/// It serialises and it hashes, so it is a `System` on the world rather than a
/// field on the session -- the seam `sim/system.hpp` documents and `EnvSystem`
/// already uses. Every walk over it is over `PlayerId` 0..15 ascending, and the
/// one place that draws from the RNG (`setup_match`) draws in that same order.
///
/// ## The two team scores, and what they used to refuse
///
/// `GetTeamMilitaryScore/1` and `GetTeamOverallScore/1` are both bound now, and
/// this paragraph used to say neither would be. The refusal was right at the
/// time and it named its own condition: the formulas were recovered exactly,
/// and the counters they read were tracked by nothing, so an entry point
/// answering `(0 + 0 + 1000) * 100 / (0 + 0 + 10000)` = 10 for every player
/// forever would have been a plausible number and a silent divergence -- and in
/// `3` and `4` a table of equal scores is `nTop1 == nTop2`, permanent sudden
/// death rather than a decided game.
///
/// What lifted it was the counters arriving one at a time: the four combat
/// counters when `CombatSystem` began crediting them, `gold_used` when
/// `Settlement::GoldSpent` was written, and `power_score` when the last
/// unidentified piece of its census -- the object-grid pass's per-object body
/// -- was decoded. See `PlayerScoreCounters` for that history in full.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/system.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

class CombatSystem;

// --------------------------------------------------------------------------
// the rule
// --------------------------------------------------------------------------

/// The four shipped victory conditions, plus the absence of one.
///
/// The enumerator values are the leading integers of the attribute, so
/// `static_cast<int>` round-trips through `victory_condition`'s first field.
enum class VictoryCondition : std::uint8_t {
  none = 0,             ///< `victory_condition="0"` -- 19 of the 22 containers
  elimination = 1,      ///< `"1 Elimination"` -- the three hand-made scenarios
  score_limit = 2,      ///< `"2 Score limit"`
  time_limit_military = 3,  ///< `"3 Time limit (military rating)"`
  time_limit_score = 4,     ///< `"4 Time limit (score)"`
};

/// How many of the eight races `Random` may resolve to. See `Race` in
/// `sim/player_host.hpp`, whose eight values are proven from the executable's
/// constant table.
inline constexpr std::int32_t kRaceCount = 8;

/// Parse `game.xml`'s `victory_condition`.
///
/// Reads the leading run of digits and ignores everything after it, because
/// the attribute is `"<index> <display name>"` in one field and only two of
/// the five possible spellings occur in the retail data. Anything that does not
/// begin with a digit, and any index above 4, reads as `none`.
[[nodiscard]] VictoryCondition parse_victory_condition(std::string_view attribute) noexcept;

/// The `DATA/GAMESCRIPTS` basename, without extension, or an empty view for
/// `none`. Taken from the shipped filenames verbatim; lookup is
/// case-insensitive, so the case here does not matter and the shipped spelling
/// is used so a diagnostic reads like the file it names.
[[nodiscard]] std::string_view victory_script_name(VictoryCondition condition) noexcept;

/// The path a `ScriptResolver` is asked for: `data/GameScripts/<name>.vs`.
/// Empty for `none`.
[[nodiscard]] std::string victory_script_path(VictoryCondition condition);

// --------------------------------------------------------------------------
// what the map declares
// --------------------------------------------------------------------------

/// The match half of `game.xml`, plus the two `map.xml` fields the host API
/// exposes to scripts.
///
/// `world/map.hpp`'s `GameProperties` keeps only what the renderer needs (the
/// season and the start map); these are the rest. Kept here rather than added
/// there because a victory rule is not a rendering input, and because the two
/// map fields exist in this struct only to answer `MapName()` and `MapSize()`
/// -- see the note on those two entry points in `register_match_host`.
struct MatchRules {
  VictoryCondition condition = VictoryCondition::none;

  /// `victory_threshold`, verbatim, as the `str param` the script receives.
  ///
  /// Passed through unchanged and never interpreted here. `2`, `3` and `4` do
  /// `Str2Int(param)` on it and want a score or a number of seconds;
  /// `CONST.INI`'s `2_Score_limit0..5` and `3_/4_Time_limit_*0..7` lists are
  /// the menu of legal values, and turning a menu index into one of them is
  /// the lobby's job, not the simulation's. It is `"0"` or empty in every
  /// retail container.
  std::string param;

  /// `start_player`: the slot the human takes unless the caller overrides it.
  /// 0 or 1 in the retail data.
  PlayerId start_player = 0;

  /// `single_only`. `1` never occurs.
  bool single_only = false;

  /// `map.xml`'s `name`, for `MapName()`.
  std::string map_name;

  /// `map.xml`'s `size/@x`, for `MapSize()`. 8192, 16384 or 32768; `size_y` is
  /// always equal and the engine keeps only one number (`MapSize` at
  /// 0x0051b120 returns a single field).
  std::int32_t map_size = 0;

  /// The skirmish setup's four rules, as `gbr.exe`'s game object carries them
  /// beside the victory condition (0x006dbc30 copies the settings record
  /// into `+0xd90 worldpop`, `+0xd94 startinggold`, `+0xd9c fogofwar`,
  /// `+0xda0 exploration`; 0x005bdb80 serialises the four into
  /// `<basicgamesettings><gamedata ...>`). Saved with the rules; the
  /// automatch path (0x00702906) states the defaults independently:
  /// `NormalPop`, gold -1, fog on, exploration on.
  ///
  /// `world_population` is a percent of every non-wildlife settlement's
  /// `max_population` (0x005267dd, C integer division; `population` itself
  /// is untouched); `starting_gold` of -1 is "Default", the map's own, and
  /// anything else is `SetGold` on every settlement with a warehouse
  /// (0x005267b3) -- every one, not only the players'. `exploration` off
  /// reveals the whole map at load (0x00526813) and answers every explored
  /// query with yes (0x0041cb80); `fog_of_war` off makes a cell visible when
  /// it is explored (0x0041cbd0), which is a rule for the renderer this
  /// engine's fog does not draw yet, kept here because the original keeps it
  /// as match state.
  std::int32_t world_population = 100;
  std::int32_t starting_gold = -1;
  bool fog_of_war = true;
  bool exploration = true;

  /// Parse the match fields out of a `game.xml` document.
  ///
  /// Does not touch `map_name` or `map_size`: those come from `map.xml`, which
  /// `MapGeometry::parse` already reads, and the caller fills them in.
  [[nodiscard]] static Result<MatchRules> parse_game_properties(std::span<const std::byte> xml);
};

// --------------------------------------------------------------------------
// what the caller chooses
// --------------------------------------------------------------------------

/// The lobby's side of setup: everything the map does not decide.
///
/// Defaults describe a single-player skirmish that takes the map at its word:
/// the human is `MatchRules::start_player`, every other enabled slot is a
/// computer, and every unresolved race is drawn.
struct MatchOptions {
  MatchOptions() noexcept;

  /// Which slot the human takes. `kNoPlayer` means "use `start_player`".
  /// A value of `kNoPlayer` after `start_player` is applied is legal and means
  /// an all-computer match, which is what a conformance run wants.
  PlayerId human = kNoPlayer;

  /// True only for a networked game. `IsMultiplayer()` answers it directly.
  bool multiplayer = false;

  /// Per-slot race, or `kNoRace` to take the map's and, if that is a marker,
  /// draw. Indexed by `PlayerId`.
  std::array<std::int32_t, kPlayerCount> races{};

  /// Per-slot control override, applied only where `control_set[i]` is true.
  /// `PlayerControl` has no "unset" enumerator, hence the parallel array.
  std::array<PlayerControl, kPlayerCount> controls{};
  std::array<bool, kPlayerCount> control_set{};

  /// The setup screen's victory rule: `"Map Default"` -- the map's own
  /// `victory_condition` and `victory_threshold` -- unless `condition_set`,
  /// in which case `condition` and `threshold` (the limit's number, the
  /// `str param` the script receives) replace them. That the chosen rule
  /// replaces the map's is INFERRED: 0x006f50d4 writes the choice into the
  /// settings record and 0x006dbc30 into the game object, and the hop from
  /// there to the loaded rule was not located.
  bool condition_set = false;
  VictoryCondition condition = VictoryCondition::none;
  std::string threshold;

  /// See `MatchRules`. Applied by `setup_match` to the rules and by
  /// `GameSession::start_match` to the settlements and the exploration map.
  std::int32_t world_population = 100;
  std::int32_t starting_gold = -1;
  bool fog_of_war = true;
  bool exploration = true;
};

// --------------------------------------------------------------------------
// the state
// --------------------------------------------------------------------------

/// One player's standing in the match.
enum class MatchOutcome : std::uint8_t {
  undecided = 0,
  won = 1,   ///< `EndGame(player, false)`
  lost = 2,  ///< `EndGame(player, true)`
};

// --------------------------------------------------------------------------
// the scores
// --------------------------------------------------------------------------

/// The five per-player counters the two team-score entry points read.
///
/// **Recovered from `gbr.exe`, by name.** The player's game-stats record is
/// serialised twice, at 0x0056b0e0 and 0x0056cb00, and both walks register the
/// same offsets under names:
///
/// | Offset | 0x0056b0e0 | 0x0056cb00 |
/// |---|---|---|
/// | `+0x38` | `gold` | `gold_used` |
/// | `+0xa0` | `damage_taken` | `damage_taken` |
/// | `+0xa4` | `damage_inflicted` | `damage_inflicted` |
/// | `+0xb8` | `poser_score` | `power_score` |
/// | `+0xbc` | `kill_healths` | `kill_healths` |
/// | `+0xc0` | `die_healths` | `die_healths` |
///
/// (`poser_score` is the same field as `power_score`, spelt wrong in the older
/// of the two serialisers. The longer walk names eleven more -- `units_killed`,
/// `units_lost`, `units_produced`, `gold_captured`, `gold_townhall`,
/// `gold_outpost`, `health_sacrificed`, `priests` and the rest -- and none of
/// them is read by anything a script can reach.)
///
/// ## `GetTeamOverallScore` is bound, and this is what it took
///
/// `0x004c67a0` decrements the 1-based player and calls `0x0056c900`, which
/// averages over the same team `GetTeamMilitaryScore` averages over and whose
/// per-member term is
///
///     gold_used / 1000  +  (power_score * military_rating) / 100
///
/// -- `military_rating` being the very expression above, reused verbatim as the
/// middle factor. Two of the three were here from the start. The other two are
/// now:
///
///   * **`gold_used`**, whose three writers in `gbr.exe` are the production
///     payment (0x004df1a7), the cancellation refund (0x004df49f) and
///     `Settlement::GoldSpent` -- and the third is here, as
///     `record_gold_spent`. The other two are the economy's to add when
///     production starts charging through this counter.
///   * **`power_score`**, a whole-map census, and `MatchSystem::power_score`
///     below is what it comes to. It was half-recovered for a long time: the
///     settlement walk was decoded and the object-grid pass's per-object body
///     was not. That body (0x0056a410) turned out to be eleven instructions of
///     wagon cargo and one call to the same per-unit valuation the settlement
///     walk uses, and that valuation (0x004439d0) is
///
///         is-a `RamUnit` ? 1
///                        : (attack + armor_slash) * (level + 13)
///                          * (maxhealth / 4 + health) / 1000 + 1
///
///     truncated to sixteen bits by its caller. `ObjectState::cargo` and
///     `cargo_resource` -- added for `Wagon::amount` and `Wagon::restype` --
///     are what made the wagon half recoverable at all.
///
/// Its two shipped callers are `2 Score limit.VS` and
/// `4 Time limit (score).VS`, which rank all eight players and end the match on
/// the top two. Both were unreachable until this existed.
///
/// **`gold` is gold *spent*, not held and not earned**, which the older
/// serialiser's shorter name hides. Its three writers are the production
/// payment at 0x004df1a7, the cancellation refund at 0x004df49f, and the
/// script entry point `Settlement::GoldSpent` -- income is counted separately
/// in `gold_captured`, `gold_townhall` and `gold_outpost`. Nothing here
/// maintains it yet: it is the `GetTeamAchievementsScore` and
/// `GetTeamOverallScore` term, and neither of those is bound.
///
/// The four combat counters **are** maintained -- `CombatSystem` credits them
/// on every blow it lands. See `MatchSystem::record_damage`.
struct PlayerScoreCounters {
  /// Gold **spent**, cumulative: the production charge when a command is
  /// queued (0x004df1a7), less the refund when it is cancelled (0x004df49f),
  /// plus `Settlement::GoldSpent`. `record_gold_spent`.
  std::int32_t gold = 0;              ///< +0x38 `gold_used`
  std::int32_t damage_taken = 0;      ///< +0xa0
  std::int32_t damage_inflicted = 0;  ///< +0xa4
  /// The **maximum** health of everything this player has killed, one lump per
  /// death, read off the victim's class rather than off the victim.
  std::int32_t kill_healths = 0;      ///< +0xbc
  /// And of everything this player has lost, the same way.
  std::int32_t die_healths = 0;       ///< +0xc0

  // -- the end-of-match report's counters ---------------------------------
  //
  // The rest of the record the two serialisers name (see the table above),
  // as `STATISTICS.INI` shows them: *Resources* is `gold_used` and
  // `food_used`; *Units* is `units_killed`, `units_lost`, `units_max`;
  // *Gold production* is `gold_townhall`, `gold_outpost`, `gold_captured`.
  // Nothing a script can reach reads any of them, so they are **saved and
  // not hashed**, like the economy's per-settlement statistics.

  /// Food spent the way `gold` is: the production charge and its refund
  /// (0x004df1d2, 0x004df4fd).
  std::int32_t food = 0;              ///< +0x3c `food_used`
  /// `Military` units that took this owner: at spawn, and on every change of
  /// owner (0x005dc3f0 counts on `SetPlayer` when the owner differs).
  std::int32_t units_produced = 0;    ///< +0x40
  /// Units this player killed, one per counted kill -- the same guarded
  /// branch as `kill_healths` (0x00511905).
  std::int32_t units_killed = 0;      ///< +0x44
  /// `Military` units of this owner that left the world, whatever took them
  /// (the destructor, 0x005db4c0).
  std::int32_t units_lost = 0;        ///< +0x48
  /// The most `Military` units this owner had at once: raised by one when a
  /// unit produced makes `produced - lost` exceed it (0x005dc4dc).
  std::int32_t units_max = 0;         ///< +0x4c
  /// Gold that changed hands with a settlement: `Settlement::SetPlayer`
  /// (0x005c4f03) moves the warehouse's gold from the old owner's count to
  /// the new one's, so a player's is net -- captured less lost.
  std::int32_t gold_captured = 0;     ///< +0xac
  /// *From taxes*: the gold the player's settlements produced. **Reading,
  /// labelled:** the writer found (0x005c424f) stores a new settlement's
  /// starting gold into the owner's count; the running increment was not
  /// found, and crediting every production tick is the reading this engine
  /// takes.
  std::int32_t gold_townhall = 0;     ///< +0xb0
  /// *Other*: `Settlement::GoldConverted` (0x005c29d3), the outposts' food
  /// sold and interest earned -- what the shipped outpost scripts report.
  std::int32_t gold_outpost = 0;      ///< +0xb4
};

/// How many player slots `0x00522e20` scans for teammates.
///
/// Eight, not sixteen. See `MatchSystem::team_military_score`.
inline constexpr PlayerId kTeamScanSlots = 8;

/// One player's military rating -- the per-member term of
/// `GetTeamMilitaryScore`.
///
/// **Proven**, from `_TeamMilitaryScore` at 0x0056c640, which computes exactly
///
///     ((kill_healths >> 1) + damage_inflicted + 1000) * 100
///     ---------------------------------------------------------
///     ((die_healths  >> 1) + damage_taken     + 10000)
///
/// as an unsigned divide, and averages it over the team's members with an
/// unsigned divide by the member count. `GetTeamOverallScore` (0x0056c900)
/// reuses the same expression verbatim as its middle factor.
///
/// A player who has neither dealt nor taken anything rates 10.
[[nodiscard]] std::int32_t military_rating(const PlayerScoreCounters& counters) noexcept;

/// One slot, after setup resolved it.
struct MatchPlayer {
  PlayerControl control = PlayerControl::disabled;
  /// 0..7 once resolved, `kNoRace` for a slot that takes no part.
  std::int32_t race = kNoRace;
  /// Takes part in the victory rule: enabled, and not one of the two engine
  /// neutrals. Only participants get a victory script and only participants
  /// count towards `over()`.
  bool participates = false;
  MatchOutcome outcome = MatchOutcome::undecided;
  /// The running combat tally the two team-score entry points read. Written by
  /// `CombatSystem`; see `record_damage` and `record_kill`.
  PlayerScoreCounters score;
};

/// The match, hung on the world as a `System`.
///
/// No per-turn behaviour: `advance` does nothing. The state changes only when a
/// script calls `EndGame`, exactly as `EnvSystem`'s stores change only when a
/// script writes.
class MatchSystem final : public System, public WorldObserver {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "match"; }
  void advance(World& world, const Turn& turn) override;
  /// Installs the system as the world's observer and takes the census the
  /// original takes as it places the map -- every in-play `Military` unit
  /// with an owner counts as produced, and each owner's *most at once* is
  /// what stands. A load replaces the counters with the saved ones after.
  void start(World& world) override;
  void owner_set(World& world, ObjectId id, PlayerId previous, PlayerId now) override;
  void despawning(World& world, const WorldObject& object) override;

  /// The outcomes and the human's identity are hashed: two peers that disagree
  /// about who has been eliminated have desynchronised. The rules are load-time
  /// configuration, identical on every peer, and are not.
  void hash(std::uint64_t& accumulator) const override;

  // -- setup -------------------------------------------------------------

  /// Installed by `setup_match`. Not called directly.
  void configure(const MatchRules& rules, PlayerId human, bool multiplayer);
  void set_player(PlayerId id, const MatchPlayer& player);

  [[nodiscard]] const MatchRules& rules() const noexcept { return rules_; }
  [[nodiscard]] PlayerId human() const noexcept { return human_; }
  [[nodiscard]] bool multiplayer() const noexcept { return multiplayer_; }

  /// The game difficulty `GetDifficulty()` reports: **0 easy, 1 medium, 2
  /// hard**, and `SetDifficulty`'s own diagnostic is where those three names
  /// come from (`SetDifficulty: The difficulties allowed are 0, 1 and 2
  /// (easy/medium/hard)`, 0x007b8088). The same three settings
  /// `sim/combat.hpp`'s `Difficulty` names, kept here as the plain `int` the
  /// original stores rather than as that enum, because `GetDifficulty` returns
  /// the field verbatim and `SetDifficulty` is the only thing that constrains
  /// it.
  ///
  /// Match-level rather than map-level. `GetDifficulty` (0x004c6630) reads the
  /// game object's field at `+0x254`, and the only writers are
  /// `SetDifficulty` and 0x006e80a0, which copies it out of a 60-byte settings
  /// block at 0x0082ef58 -- the options screen, not the map. It is *not*
  /// `game.xml`'s `playerdata/@difficulty`, which is per player and is the AI
  /// profile overlay `sim/ai_profile.hpp` already models as `AiDifficulty`.
  ///
  /// Hashed, because a script can move it and two peers at different
  /// difficulties would spawn units at different levels. **No shipped script
  /// moves it**: `SetDifficulty` has zero call sites in the whole
  /// installation, while `GetDifficulty` has 136 -- almost all of them
  /// arithmetic on a level, a count or a timer, as in Zama seq4's
  /// `SetLevel(30 + (GetDifficulty() * 10))` and seq6's
  /// `Sleep(120000 - (GetDifficulty() * 15000))`.
  [[nodiscard]] std::int32_t difficulty() const noexcept { return difficulty_; }

  /// `SetDifficulty(n)` -- 0x004c65d0.
  ///
  /// Returns false and changes nothing for anything outside 0..2, which is
  /// exactly what the original does: it tests the three values, prints the
  /// diagnostic on the fall-through and returns without touching the field.
  bool set_difficulty(std::int32_t value) noexcept;
  [[nodiscard]] const MatchPlayer& player(PlayerId id) const noexcept;
  [[nodiscard]] std::size_t participant_count() const noexcept;

  // -- the rule ----------------------------------------------------------

  /// `EndGame(player, lose)`.
  ///
  /// Records and never overwrites: the first outcome a player reports is the
  /// one it keeps. `2 SCORE LIMIT.VS` returns immediately after calling it and
  /// so does `1 ELIMINATION.VS`, so a second report is a script restarted by
  /// hand rather than a change of mind. Out-of-range and non-participating
  /// players are ignored.
  void end_game(PlayerId player, bool lost);

  /// `EndGame(player, lose, message)` -- the three-argument form.
  ///
  /// Same outcome rule, plus the sentence the end screen shows. The message is
  /// kept **only when `player` is the human**, which is what 0x004c9660 does:
  /// it divides `(current_player - players_base)` by the 800-byte player
  /// stride, compares that index against `player - 1`, and assigns the string
  /// into the game object only on a match. Every other player's message is
  /// evaluated, freed, and dropped.
  void end_game(PlayerId player, bool lost, std::string_view message);

  /// The sentence the last `EndGame/3` naming the human carried, or empty.
  ///
  /// Not hashed and not simulation state: it is a line of display text, it
  /// exists on one peer only, and hashing it would make two peers with
  /// different `local/*.pak` translations desynchronise.
  [[nodiscard]] std::string_view end_message() const noexcept { return end_message_; }

  // -- the scores --------------------------------------------------------

  /// Credit one blow: `amount` to the attacker's `damage_inflicted` and the
  /// same number to the victim's `damage_taken`.
  ///
  /// **One number, two rows.** The four writer pairs in `gbr.exe` -- the main
  /// resolver at 0x0051183a/0x00511865 and the three effect loops -- all add
  /// the identical value to both, and the value is the victim's health *delta*
  /// (0x0051151d), so it is the health actually removed rather than the attack
  /// roll.
  ///
  /// **Allied blows are not counted, and that is the original's rule rather
  /// than a tidy-up.** When both sides are units and the attacker's owner
  /// grants the victim's owner bit 0, 0x00511550 takes a separate capped
  /// non-lethal branch at 0x0051164f and jumps past every counter. This engine
  /// has no such branch, so the exclusion is stated here instead. A player is
  /// allied to itself, so friendly fire and self-damage are covered by the same
  /// test.
  ///
  /// Out-of-range players are ignored on either side, independently: an
  /// unowned object's attacker still credits the victim.
  void record_damage(const World& world, PlayerId attacker, PlayerId victim,
                     std::int32_t amount) noexcept;

  /// Credit one death: `max_health` to the killer's `kill_healths` and to the
  /// victim's `die_healths`.
  ///
  /// The number is the victim's **class** maximum health (`[victim + 0x3c] +
  /// 0x294`, the field the object constructor copies into both current and
  /// maximum health at spawn), not its health when it died and not the blow
  /// that killed it. A full-health kill and a finishing blow on a sliver are
  /// worth the same.
  ///
  /// `die_healths` is credited whether or not there is a killer -- the store at
  /// 0x00511954 is outside the `attacker valid` branch that guards
  /// 0x0051192d -- and the same alliance rule applies as above.
  void record_kill(const World& world, PlayerId killer, PlayerId victim,
                   std::int32_t max_health) noexcept;

  /// `Settlement::GoldSpent(n)` -- add to a player's cumulative **gold spent**.
  ///
  /// 0x005c29f0 resolves the settlement's own player record, walks to that
  /// player's score block and adds the argument to `[+0x38]`. It is one of the
  /// three writers of `gold_used` this file's `GetTeamOverallScore` note names,
  /// and the only one a script can reach; the other two are the production
  /// payment and its cancellation refund, inside the executable.
  ///
  /// No sign check and no alliance rule: unlike `record_damage` and
  /// `record_kill`, this is a script saying what it spent, not a blow that has
  /// to be worth counting. An out-of-range player is ignored.
  void record_gold_spent(PlayerId player, std::int32_t amount) noexcept;
  /// The report's counters (see `PlayerScoreCounters`). Each ignores an
  /// out-of-range player; none is hashed.
  void record_food_spent(PlayerId player, std::int32_t amount) noexcept;
  void record_gold_captured(PlayerId player, std::int32_t amount) noexcept;
  void record_gold_produced(PlayerId player, std::int32_t amount) noexcept;
  void record_gold_converted(PlayerId player, std::int32_t amount) noexcept;

  [[nodiscard]] const PlayerScoreCounters& score(PlayerId id) const noexcept;

  /// `GetTeamMilitaryScore(player)` -- the average of `military_rating` over
  /// the player's team, or 0 for a team with no members.
  ///
  /// **"Team" is the alliance matrix, and it wants both directions.**
  /// 0x00522e20 puts the player itself in the list unconditionally and first,
  /// then scans for others whose row grants this player bit 0 *and* whose bit 0
  /// this player grants back -- which is `PlayerTable::are_allied`. It is not a
  /// team id and there is no such field.
  ///
  /// **It scans eight slots, not sixteen**, and that is transcribed rather than
  /// corrected: the loop bound is `esi < 0x1900` over a `0x320` stride, and a
  /// dead `cmp esi, 0x3200` inside the body is the leftover of a sixteen-slot
  /// version. So a mutual ally in slot 8 or above is not on anyone's team,
  /// while the caller itself is on its own team from any slot.
  ///
  /// The membership filter on the others is `CVXPlayer + 0x64`, **the same
  /// unmodelled field `PlayerTable::are_allied` already declines to require** --
  /// `DiplAreAllied` (0x00564c10) wants it non-zero on both sides and this scan
  /// wants it non-zero on the candidate. It is not among the nine the
  /// serialiser names and nothing in the shipped data reaches it. `participates`
  /// stands in: it excludes disabled slots and the two engine neutrals, which
  /// is the same set that gets a victory script. Without some such filter every
  /// empty slot would join every team and rate 10, which is plainly what the
  /// original's filter is for.
  ///
  /// A player number outside 1..16 **faults** in the original: the four score
  /// entry points pass a null record into 0x00522e20, which dereferences it at
  /// `+0x64` with no check. Answering 0 is a guard, not a transcription.
  [[nodiscard]] std::int32_t team_military_score(const World& world, PlayerId player) const;

  /// `power_score`, the `+0xb8` field, recomputed from the map.
  ///
  /// A whole-map census rather than a counter, and it is the reason
  /// `GetTeamOverallScore` was refused for so long. What it comes to:
  ///
  ///     building_points + (gold + food / 2 + unit_power) / 100
  ///
  /// where the three sums are gathered by **two passes that between them see
  /// every object exactly once**:
  ///
  ///   * the settlement walk (0x0056a6e0), over settlements this player owns:
  ///     the warehouse's stored gold and food, **20 points for a `BaseTownhall`
  ///     settlement and 3 for any other**, and `unit_power` over the garrison;
  ///   * the object-grid walk (0x0056a4a0, functor at 0x0056a410), over every
  ///     positioned object this player owns: `unit_power` again, plus a
  ///     wagon's load added to the gold or the food total by its `restype`.
  ///
  /// A garrisoned unit is inside a holder and therefore has no grid cell, so
  /// the two passes do not double-count -- and a unit inside a *ship* is
  /// counted by neither, which is the original's behaviour and not a gap here.
  ///
  /// Both passes skip an object whose spawn-template bit is set and require the
  /// `kSyncUnit` bit, and both test ownership against the same one-hot mask
  /// `pack_sync_flags` builds.
  ///
  /// **The cache and its gate are not reproduced.** 0x0056a8c0 returns the
  /// stored `+0xb8` unless `[player record + 0x290]` is 1, and recomputes and
  /// stores otherwise. That field is a player-slot lifecycle number -- its
  /// writers put 2 and 3 into it, and 0x00523341 sets 3 immediately after
  /// reading a player's score for the last time -- so `== 1` reads as *still
  /// playing*, and a player who is out keeps the last score it had. That is a
  /// labelled inference rather than a measurement; this recomputes every time,
  /// which agrees with the original for every player still in the match and
  /// differs only for one already eliminated, whose score both engines are
  /// about to stop reading.
  [[nodiscard]] std::int32_t power_score(World& world, PlayerId player) const;

  /// `GetTeamOverallScore(player)`.
  ///
  /// The same team average `team_military_score` takes, over the per-member
  /// term 0x0056c900 computes:
  ///
  ///     gold_used / 1000  +  (power_score * military_rating) / 100
  ///
  /// -- `military_rating` being `team_military_score`'s own per-member term,
  /// reused verbatim as the middle factor rather than recomputed differently.
  [[nodiscard]] std::int32_t team_overall_score(World& world, PlayerId player) const;

  [[nodiscard]] MatchOutcome outcome(PlayerId id) const noexcept;

  /// The human's own outcome, or `undecided` when there is no human.
  ///
  /// Under `elimination` this is the only outcome any script ever reports for
  /// the human, and it is only ever `lost`: see the header note.
  [[nodiscard]] MatchOutcome human_outcome() const noexcept;

  /// Is the match decided?
  ///
  /// **A derivation, not a measurement.** True when either
  ///
  ///   * the human participates and has reported an outcome, or
  ///   * at most one alliance group of participants is still standing, where
  ///     "standing" means an outcome of `undecided` or `won` and a group is a
  ///     set of participants allied to one another (`PlayerTable::are_allied`,
  ///     which is mutual bit 0 and is what `DiplAreAllied` answers -- the same
  ///     predicate the four scripts use to decide who their enemies are).
  ///
  /// The second clause is what makes an elimination match winnable at all,
  /// because no elimination script ever reports a win.
  [[nodiscard]] bool over(const World& world) const;

  /// The lowest-numbered standing participant once `over`, or `kNoPlayer` for
  /// a match that is not over, or one nobody survived.
  ///
  /// A single player, not a team: the caller that wants the winning side asks
  /// `PlayerTable::are_allied` against this one.
  [[nodiscard]] PlayerId winner(const World& world) const;

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
  /// **Written:** the rules, the sixteen player rows, the human player, the
  /// multiplayer flag and `end_message_`.
  ///
  /// `end_message_` is display text and is deliberately **not** hashed --
  /// `EndGame`'s message reaches a dialog, not the simulation -- and it is
  /// saved all the same, because a game reloaded after its victory condition
  /// fired should still be able to say why.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace this system's state with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into locals and moved in only once every
  /// field has read cleanly, so a truncated or malformed save leaves the system
  /// exactly as it was.
  ///
  /// **Not restored:** nothing. This system is small enough to be saved
  /// whole.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  MatchRules rules_;
  std::array<MatchPlayer, kPlayerCount> players_{};
  PlayerId human_ = kNoPlayer;
  bool multiplayer_ = false;
  /// Zero until something sets it, which is what the game object's field holds
  /// before the settings block is applied.
  std::int32_t difficulty_ = 0;
  std::string end_message_;
};

/// The match system on a world, or null. The same shape as `combat_system_of`
/// and `hero_system_of`, and for the same reason: registration order is part of
/// the simulation's definition, so a name lookup is the deterministic one.
[[nodiscard]] MatchSystem* match_system_of(World& world) noexcept;
[[nodiscard]] const MatchSystem* match_system_of(const World& world) noexcept;

/// One frame's answer to "is it over, and who won".
///
/// A snapshot rather than four calls, because a render loop asks every frame
/// and the four answers have to agree with one another: `winner` is meaningless
/// until `over` is true, and `human_won` is a reading of `winner` against the
/// human's alliance that a caller should not have to reproduce.
struct MatchStatus {
  bool over = false;
  /// The lowest-numbered standing participant, or `kNoPlayer`.
  PlayerId winner = kNoPlayer;
  /// Which slot the human holds, or `kNoPlayer` for an all-computer match.
  PlayerId human = kNoPlayer;
  /// What the human's own victory script reported, if anything. Under
  /// `elimination` this is only ever `undecided` or `lost`, because that script
  /// never reports a win -- see the header.
  MatchOutcome human_outcome = MatchOutcome::undecided;
  /// Is this the screen that says "Victory"? False whenever `over` is false,
  /// whenever there is no human, and whenever nobody survived.
  bool human_won = false;
  /// What `EndGame/3` said, when the human's own script used that form. Empty
  /// otherwise -- including for every `EndGame/2`, which carries no sentence.
  std::string_view message;
};

/// The status of the match on a world.
///
/// A world with no `MatchSystem` -- a conformance run, or a map loaded without
/// a match -- reports a default, which is a game that is not over. That is the
/// right answer for a caller that polls unconditionally.
[[nodiscard]] MatchStatus match_status(const World& world);

/// The same question asked from `viewer`'s seat: `human`, `human_outcome` and
/// `human_won` are `viewer`'s rather than the match's own human.
///
/// For a networked game, where every seat is a human and the match's `human`
/// is merely the lowest -- the same slot on every peer, because it is hashed --
/// so "did I win" has to be asked of the seat at this screen, which is the one
/// thing peers legitimately disagree about. `over` and `winner` do not depend
/// on the viewer and are the same on every peer.
[[nodiscard]] MatchStatus match_status(const World& world, PlayerId viewer);

// --------------------------------------------------------------------------
// setup
// --------------------------------------------------------------------------

/// Resolve a skirmish from the map's declared players plus the caller's
/// choices, and install it on `match`.
///
/// Returns the number of participants.
///
/// ## What it decides, in this order
///
/// A single ascending walk over `PlayerId` 0..15, because the walk draws from
/// `world.rng()` and the order of the draws is world state.
///
///   1. **Who takes part.** `control="Disabled"` stays out. The two engine
///      neutrals (`kNeutralWildlife`, `kNeutralPassive`) stay out whatever they
///      declare -- they hold the map's animals and its unowned scenery, and the
///      victory scripts walk players 1..8 only.
///   2. **Who the human is.** `MatchOptions::human`, or `start_player` when
///      that is `kNoPlayer`. That slot becomes `PlayerControl::human`.
///   3. **What everyone else is.** `Both` becomes `Computer`. A slot that
///      declares `Human` and is not the chosen human becomes `Computer` in a
///      single-player match and is left alone in a multiplayer one.
///   4. **What race each ends up as.** A caller override wins; otherwise the
///      map's `race`, unless that is one of the three markers `Random`,
///      `Mutable` or `Select`, which draw `world.rng().below(kRaceCount)`.
///      The resolved name is written back into `PlayerSetup::race`, which is
///      what `GetPlayerRace` reads.
///
/// ## What it does not do
///
/// `AllowedRaces` is not applied. `"All"` is its only value in all 304 shipped
/// `player<i>.xml` documents, so there is nothing to test a filter against; if
/// a container with a narrower list ever turns up, this is where it goes.
///
/// The distribution of a `Random` draw is **assumed uniform over the eight
/// races**. Nothing in the data or the executable was found to settle it. What
/// is not assumed is where the randomness comes from: `World::rng()`, because
/// the resolution is part of the simulation's starting state and a second
/// generator is a second state to serialise.
std::size_t setup_match(World& world, MatchSystem& match, const MatchRules& rules,
                        const MatchOptions& options);

/// Spawn the victory-condition script, once per participating player.
///
/// Ascending `PlayerId`, arguments `(player_to_script(id), rules.param)` --
/// the `void, int player, str param` the four scripts declare and `gbr.exe`
/// stores beside `data/GameScripts/`.
///
/// Returns how many started. Zero is not an error: `VictoryCondition::none` is
/// 19 of the 22 shipped containers, and a chunk the scheduler's library does
/// not hold yields `kNoScript` exactly as `spawn_by_name` documents. The caller
/// compiles `victory_script_path(...)` into the library first.
std::size_t start_victory_scripts(World& world, script::Scheduler& scheduler);

// --------------------------------------------------------------------------
// the host surface
// --------------------------------------------------------------------------

/// Define every entry point this domain owns.
///
/// Call after `declare_shipped_surface`. Returns the number defined, so a
/// caller can assert the count rather than trust it -- the convention
/// `register_player_host` and `register_world_host` already follow.
///
/// ## What it claims, and why each one is here
///
///   * `EndGame/2` -- the only entry point the four scripts reach that decides
///     anything. `gbr.exe` gives its signature literally:
///     `void, int player, bool lose`.
///   * `IsMultiplayer/0` -- `bool`. The original (0x004c6500) answers "is there
///     a network session with at least one peer"; here it is the match's own
///     flag, which is the same question asked of state this engine has.
///   * `GetTime/0` -- `int`, **milliseconds**. The original returns one field
///     of the game object; the corpus settles the unit beyond doubt
///     (`GetTime() > nLastResearchTime + 600000` for ten minutes,
///     `GetTime() < 10 * 60000`, `+ 15000` for fifteen seconds). This answers
///     `World::time()`, saturated into 32 bits.
///   * `MapName/0` -- `str`, and `MapSize/0` -- `int`. Neither is reached by
///     the four game scripts; `MapSize()` is reached by `CROW_MOVE.VS`, which
///     clamps a coordinate to it, so it is a world-unit extent. **Both are
///     lodgers.** They belong to the world once `World` knows its own bounds
///     and its own name, and they are here only because `MatchRules` is the
///     one structure that currently carries either.
///
/// ## What it deliberately does not claim
///
///   * `SetPlayerStatus/3` and `/4` -- already defined by `register_player_host`
///     as a validated no-op. `define` replaces silently, so redefining them
///     here would take them away from a domain that has documented why it holds
///     them. The four scripts call them 60 times between them and only ever to
///     write HUD text.
///   * `GetTeamOverallScore/1` and `GetTeamMilitaryScore/1` -- see the header.
std::size_t register_match_host(script::HostRegistry& registry);

/// One object's worth to the power census: 0x004439d0's
/// `(damage + attack_bonus + armor_slash) * (level + 13) * (maxhealth / 4 +
/// health) / 1000 + 1`, truncated to 16 bits, with a ram worth exactly 1. See
/// `MatchSystem::power_score`, whose census this is the per-object term of.
/// Exported because `EvalGroup` sums the same valuation over a named group,
/// and a second copy of the formula is how the two would drift.
[[nodiscard]] std::uint16_t object_power(const World& world, const CombatSystem* combat,
                                         const WorldObject& slot);

/// The number `register_match_host` defines, for that assertion.
[[nodiscard]] std::size_t match_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
