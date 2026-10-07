// The match: setup, the victory rule, and the entry points it needs.
// See include/imperivm/core/sim/match.hpp for what is proven and what is not.

#include "imperivm/core/sim/match.hpp"

#include <utility>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/xml.hpp"

namespace imperivm::core::sim {
namespace {

using script::CallContext;
using script::HostOutcome;
using script::Value;

constexpr script::CallKind kFree = script::CallKind::free_function;

/// The FNV prime, the same mixer `EnvSystem::hash` and `PlayerTable::hash` use.
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

/// The three `race` values that are match-setup markers rather than races.
/// `race_from_name` already answers `kNoRace` for all of them; this exists so
/// that an unrecognised *spelling* can be told apart from a marker, because the
/// two want different diagnostics even though they resolve the same way.
[[nodiscard]] bool is_race_marker(std::string_view name) noexcept {
  return name == "Random" || name == "Mutable" || name == "Select";
}

/// A 1..16 script player number as a `PlayerId`, or `kNoPlayer`. The same
/// conversion `sim/player_host.hpp` insists every entry point uses.
[[nodiscard]] PlayerId player_arg(const Value& value) noexcept {
  if (!value.is_integer()) return kNoPlayer;
  return player_from_script(value.as_integer());
}

[[nodiscard]] MatchSystem* host_match(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  return world == nullptr ? nullptr : match_system_of(*world);
}

// -- the entry points ------------------------------------------------------

/// `EndGame(int player, bool lose)`.
///
/// `gbr.exe` 0x004c9610 pops the bool, pops the player, subtracts one, and
/// forwards the pair. It does not inspect the world, and neither does this.
HostOutcome fn_end_game(CallContext& ctx) {
  MatchSystem* match = host_match(ctx);
  if (match == nullptr) return HostOutcome::failed("EndGame: no match");
  const PlayerId player = player_arg(ctx.arg(0));
  if (player == kNoPlayer) return HostOutcome::ok_void();
  // Any non-zero integer is `true`; the language has no separate bool value.
  const bool lost = ctx.arg(1).is_integer() && ctx.arg(1).as_integer() != 0;
  match->end_game(player, lost);
  return HostOutcome::ok_void();
}

/// `EndGame(int player, bool lose, str message)` -- 0x004c9660, the second
/// registration. See the header for why there are two.
///
/// The 61 shipped call sites are every container's victory and defeat
/// sequence, so this is the form that actually ends a mission; the two-argument
/// form is only reached from `data.pak`'s four skirmish victory scripts.
///
/// The player is *not* rejected before the message is taken. 0x004c9660 pops
/// all three regardless and compares indices; an out-of-range player simply
/// fails the comparison, which `end_game`'s own `player == human_` reproduces.
HostOutcome fn_end_game_message(CallContext& ctx) {
  MatchSystem* match = host_match(ctx);
  if (match == nullptr) return HostOutcome::failed("EndGame: no match");
  const PlayerId player = player_arg(ctx.arg(0));
  const bool lost = ctx.arg(1).is_integer() && ctx.arg(1).as_integer() != 0;
  // A non-string third argument is not an error: the binary reads the slot as
  // a string pointer whatever the script put there, and the shipped sites all
  // pass `Translate(...)`, which is a string.
  //
  // Written as a branch and not a conditional expression. `as_string()` returns
  // `const std::string&` and the other arm is a `const char[1]`, so the common
  // type of `cond ? as_string() : ""` is `std::string` **by value** -- the
  // expression materialises a temporary, and a `string_view` initialised from
  // it dangles the moment the statement ends. `-Wdangling-gsl` caught it; the
  // message it produced was plausible garbage rather than a crash.
  if (ctx.arg(2).is_string()) {
    match->end_game(player, lost, ctx.arg(2).as_string());
  } else {
    match->end_game(player, lost, std::string_view{});
  }
  return HostOutcome::ok_void();
}

/// `IsMultiplayer()`.
HostOutcome fn_is_multiplayer(CallContext& ctx) {
  MatchSystem* match = host_match(ctx);
  if (match == nullptr) return HostOutcome::failed("IsMultiplayer: no match");
  return HostOutcome::ok_with(Value::boolean(match->multiplayer()));
}

/// `GetDifficulty()` -- 136 call sites, the fourth most-used name in the
/// installation. 0x004c6630, and it is a bare field read: the game object's
/// `+0x254`, with no range check and no fallback. A build that never set it
/// answers zero.
HostOutcome fn_get_difficulty(CallContext& ctx) {
  MatchSystem* match = host_match(ctx);
  if (match == nullptr) return HostOutcome::failed("GetDifficulty: no match");
  return HostOutcome::ok_with(Value::integer(match->difficulty()));
}

/// `SetDifficulty(n)` -- 0x004c65d0, and **zero call sites in the whole
/// installation**.
///
/// Registered anyway, and implemented rather than trapped, because the body is
/// read to the instruction and the pair only makes sense together: the value
/// 136 sites read has to be settable by something. Out of range is not a
/// refusal -- the original prints and returns, leaving the field alone -- so
/// this succeeds and changes nothing, which is the same observable.
HostOutcome fn_set_difficulty(CallContext& ctx) {
  MatchSystem* match = host_match(ctx);
  if (match == nullptr) return HostOutcome::failed("SetDifficulty: no match");
  if (!ctx.arg(0).is_integer()) return HostOutcome::failed("SetDifficulty: expected an integer");
  (void)match->set_difficulty(ctx.arg(0).as_integer());
  return HostOutcome::ok_void();
}

/// `GetTime()` -- game time in milliseconds.
///
/// `GameTime` is 64-bit and the language's integer is 32, so this saturates
/// rather than wrapping. Three hours -- the longest time limit `CONST.INI`
/// offers -- is 10,800,000, so the clamp is unreachable in any real match and
/// is here only so that a runaway conformance run reports a stuck clock rather
/// than a negative one.
///
/// **The scripts' clock, not the world's.** 0x004c64e0 reads the game's
/// running time (`+0x1258`), which the millisecond loop (0x00528b40) moves
/// one step at a time while the scripts' wheel fires, so a script reads the
/// millisecond it woke on. Here that is the scheduler's `now` -- the running
/// script's wake time inside a pass -- while the world stands at the turn's
/// end. A bare run with no scheduler falls back to the world's.
HostOutcome fn_get_time(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetTime: no world");
  GameTime time = ctx.scheduler != nullptr ? ctx.scheduler->now() : world->time();
  if (time < 0) time = 0;
  if (time > 0x7fffffff) time = 0x7fffffff;
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(time)));
}

/// `GetSpeed()` -- the game speed, per mille. 0x004c6220 reads the game's
/// `+0x1244`, the field `CVXCmdSetSpeed` writes and the clock converts with,
/// which here is the clock's own `game_speed`. World state, so every peer
/// answers alike.
///
/// Its only callers are `DATA\SCDEBUG.XML`'s keypad bindings, which step
/// `GetSpeed()` up and down CONST.INI's `Speed1`-`Speed5`.
HostOutcome fn_get_speed(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetSpeed: no world");
  return HostOutcome::ok_with(Value::integer(world->clock().config().game_speed));
}

/// `SetSpeed(speed)` -- **posts, and changes nothing itself.** 0x004c6ce0
/// builds a `CVXCmdSetSpeed` with the local player as issuer and the argument
/// as it came, and hands it to the local command path; execution (0x004e67c0)
/// clamps it and writes the clock, on every peer, on the turn the stream
/// says. Here that command is `NetOrderKind::set_speed`, and the path is
/// `HostContext::outbox`, so a script's speed change reaches a networked
/// match exactly as the options screen's does.
///
/// Two refusals, both returns rather than traps:
///
///   * **the match fixed its speed** (game `+0x230`): the original tests it
///     first and posts nothing. `OrderOutbox::speed_fixed` answers it.
///   * **no outbox**: a headless run has no local player and no command path.
///     The original always has both; posting nowhere is the reading that
///     leaves such a run unchanged, and writing the clock directly -- the
///     other reading -- would change a replay's world from a script's side
///     effect alone, which is what making it an order exists to prevent.
///
/// The argument is not validated here, as it is not in the original: a
/// negative speed travels as asked and is clamped to 1 where it is applied.
HostOutcome fn_set_speed(CallContext& ctx) {
  HostContext* context = host_context_of(ctx);
  if (context == nullptr || context->world == nullptr) return HostOutcome::failed("SetSpeed: no world");
  if (!ctx.arg(0).is_integer()) return HostOutcome::failed("SetSpeed: expected an integer");
  if (context->outbox == nullptr || context->outbox->speed_fixed()) return HostOutcome::ok_void();
  NetOrder order;
  order.kind = NetOrderKind::set_speed;
  order.issuer = context->local_player;
  order.speed = ctx.arg(0).as_integer();
  context->outbox->post(std::move(order));
  return HostOutcome::ok_void();
}

/// `MapName()`.
HostOutcome fn_map_name(CallContext& ctx) {
  MatchSystem* match = host_match(ctx);
  if (match == nullptr) return HostOutcome::failed("MapName: no match");
  return HostOutcome::ok_with(Value::string(match->rules().map_name));
}

/// `MapSize()` -- **the largest legal coordinate, which is one less than the
/// extent.** Body 0x0051b120, four instructions: it reads `[map+0xbc]`, the
/// map rectangle's `right`, and returns it. The rectangle is inclusive and is
/// built as `(0, 0, w - 1, h - 1)` at 0x00541f25, so this is `w - 1`.
///
/// Its sibling `GetMapSize()` (0x0053dac0) computes `right - left + 1` and so
/// answers the extent. **The two differ by one on purpose**, and this returned
/// the wrong one of them: `map_size` is `map.xml`'s `size/@x` verbatim.
///
/// The corpus settles which reading is right without needing the disassembly
/// at all -- `CROW_MOVE.VS` clamps with
/// `if (x > MapSize()) x = MapSize();`, which is only correct if the value is
/// a legal coordinate rather than one past the end.
HostOutcome fn_map_size(CallContext& ctx) {
  MatchSystem* match = host_match(ctx);
  if (match == nullptr) return HostOutcome::failed("MapSize: no match");
  const std::int32_t extent = match->rules().map_size;
  return HostOutcome::ok_with(Value::integer(extent > 0 ? extent - 1 : 0));
}

/// The shipped basenames, indexed by `VictoryCondition`. Index 0 is `none`.
constexpr std::string_view kScriptNames[] = {
    "",
    "1 Elimination",
    "2 Score limit",
    "3 Time limit (military rating)",
    "4 Time limit (score)",
};

}  // namespace

// --------------------------------------------------------------------------
// the rule
// --------------------------------------------------------------------------

VictoryCondition parse_victory_condition(std::string_view attribute) noexcept {
  std::size_t i = 0;
  while (i < attribute.size() && (attribute[i] == ' ' || attribute[i] == '\t')) ++i;
  if (i >= attribute.size() || attribute[i] < '0' || attribute[i] > '9') {
    return VictoryCondition::none;
  }
  std::int32_t index = 0;
  while (i < attribute.size() && attribute[i] >= '0' && attribute[i] <= '9') {
    // A field this long is not an index; refuse rather than overflow.
    if (index > 100) return VictoryCondition::none;
    index = index * 10 + (attribute[i] - '0');
    ++i;
  }
  if (index < 1 || index > 4) return VictoryCondition::none;
  return static_cast<VictoryCondition>(index);
}

std::string_view victory_script_name(VictoryCondition condition) noexcept {
  const auto index = static_cast<std::size_t>(condition);
  if (index >= sizeof(kScriptNames) / sizeof(kScriptNames[0])) return {};
  return kScriptNames[index];
}

std::string victory_script_path(VictoryCondition condition) {
  const std::string_view name = victory_script_name(condition);
  if (name.empty()) return {};
  std::string path = "data/GameScripts/";
  path.append(name);
  path.append(".vs");
  return path;
}

Result<MatchRules> MatchRules::parse_game_properties(std::span<const std::byte> xml) {
  Result<XmlDocument> doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  const NodeIndex root = doc->root();
  if (root == kNoNode || doc->node(root).name != "game") return FormatError::malformed;
  const NodeIndex properties = doc->child(root, "properties");
  if (properties == kNoNode) return FormatError::malformed;

  MatchRules out;
  out.condition = parse_victory_condition(doc->attribute(properties, "victory_condition"));
  out.param = std::string(doc->attribute(properties, "victory_threshold"));
  const std::int32_t start = doc->attribute_int(properties, "start_player", 0);
  out.start_player = (start >= 0 && static_cast<std::size_t>(start) < kPlayerCount)
                         ? static_cast<PlayerId>(start)
                         : 0;
  out.single_only = doc->attribute_int(properties, "single_only", 0) != 0;
  return out;
}

// --------------------------------------------------------------------------
// options
// --------------------------------------------------------------------------

MatchOptions::MatchOptions() noexcept {
  for (std::size_t i = 0; i < kPlayerCount; ++i) {
    races[i] = kNoRace;
    controls[i] = PlayerControl::disabled;
    control_set[i] = false;
  }
}

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

void MatchSystem::advance(World& world, const Turn& turn) {
  (void)world;
  (void)turn;
}

void MatchSystem::hash(std::uint64_t& accumulator) const {
  const auto mix = [&accumulator](std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      accumulator ^= static_cast<std::uint8_t>(value >> (i * 8));
      accumulator *= kFnvPrime;
    }
  };
  // Index order. Iteration order is state.
  mix(static_cast<std::uint64_t>(human_) + 1);
  mix(multiplayer_ ? 1u : 0u);
  mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(difficulty_)));
  for (std::size_t i = 0; i < kPlayerCount; ++i) {
    const MatchPlayer& slot = players_[i];
    mix(static_cast<std::uint64_t>(slot.control));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(slot.race)));
    mix(slot.participates ? 1u : 0u);
    mix(static_cast<std::uint64_t>(slot.outcome));
    // The four combat counters. Hashed because a script reads them through
    // `GetTeamMilitaryScore` and ends the match on the answer, so two peers
    // that disagreed about who has dealt what would end it differently.
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(slot.score.gold)));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(slot.score.damage_taken)));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(slot.score.damage_inflicted)));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(slot.score.kill_healths)));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(slot.score.die_healths)));
  }
}

bool MatchSystem::set_difficulty(std::int32_t value) noexcept {
  // The three explicit comparisons the original makes, not a range test: the
  // fall-through is the diagnostic path and it leaves the field untouched.
  if (value != 0 && value != 1 && value != 2) return false;
  difficulty_ = value;
  return true;
}

void MatchSystem::configure(const MatchRules& rules, PlayerId human, bool multiplayer) {
  rules_ = rules;
  human_ = human;
  multiplayer_ = multiplayer;
  // Fresh rows, with the counters the census at `start` took kept: the
  // original's statistics records are the game's, not the setup's, and the
  // soldiers the map placed were booked as it was loaded.
  for (MatchPlayer& slot : players_) {
    const PlayerScoreCounters score = slot.score;
    slot = MatchPlayer{};
    slot.score = score;
  }
}

void MatchSystem::set_player(PlayerId id, const MatchPlayer& player) {
  if (!PlayerTable::is_valid(id)) return;
  players_[id] = player;
}

const MatchPlayer& MatchSystem::player(PlayerId id) const noexcept {
  static const MatchPlayer none{};
  if (!PlayerTable::is_valid(id)) return none;
  return players_[id];
}

std::size_t MatchSystem::participant_count() const noexcept {
  std::size_t count = 0;
  for (const MatchPlayer& slot : players_) {
    if (slot.participates) ++count;
  }
  return count;
}

void MatchSystem::end_game(PlayerId player, bool lost) {
  if (!PlayerTable::is_valid(player)) return;
  MatchPlayer& slot = players_[player];
  if (!slot.participates) return;
  if (slot.outcome != MatchOutcome::undecided) return;
  slot.outcome = lost ? MatchOutcome::lost : MatchOutcome::won;
}

void MatchSystem::end_game(PlayerId player, bool lost, std::string_view message) {
  // The message is taken before the outcome, and taken whether or not the
  // outcome sticks. 0x004c9660 assigns the string on the index comparison
  // alone; it has not looked at the outcome table at that point and does not
  // look afterwards. A second `EndGame` from the human's script therefore
  // replaces the sentence while leaving the outcome at whatever the first one
  // said -- which is the retail behaviour, however odd it reads.
  if (player != kNoPlayer && player == human_) end_message_ = message;
  end_game(player, lost);
}

MatchOutcome MatchSystem::outcome(PlayerId id) const noexcept { return player(id).outcome; }

MatchOutcome MatchSystem::human_outcome() const noexcept {
  if (human_ == kNoPlayer) return MatchOutcome::undecided;
  return outcome(human_);
}

bool MatchSystem::over(const World& world) const {
  // One human's result ends a match only when there is one human. In a
  // networked game every seat is a human and `human_` is merely the lowest --
  // hashed, so every peer holds the same one -- and one player losing a
  // three-way game ends it for nobody else.
  if (!multiplayer_ && human_ != kNoPlayer && PlayerTable::is_valid(human_) &&
      players_[human_].participates && players_[human_].outcome != MatchOutcome::undecided) {
    return true;
  }
  // At most one alliance group standing. Ascending index, both loops: the
  // answer must not depend on which side of a pair is asked first.
  for (std::size_t a = 0; a < kPlayerCount; ++a) {
    if (!players_[a].participates || players_[a].outcome == MatchOutcome::lost) continue;
    for (std::size_t b = a + 1; b < kPlayerCount; ++b) {
      if (!players_[b].participates || players_[b].outcome == MatchOutcome::lost) continue;
      if (!world.players().are_allied(static_cast<PlayerId>(a), static_cast<PlayerId>(b))) {
        return false;
      }
    }
  }
  return true;
}

PlayerId MatchSystem::winner(const World& world) const {
  if (!over(world)) return kNoPlayer;
  for (std::size_t i = 0; i < kPlayerCount; ++i) {
    if (players_[i].participates && players_[i].outcome != MatchOutcome::lost) {
      return static_cast<PlayerId>(i);
    }
  }
  return kNoPlayer;
}

MatchSystem* match_system_of(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "match") {
      return static_cast<MatchSystem*>(system);
    }
  }
  return nullptr;
}

const MatchSystem* match_system_of(const World& world) noexcept {
  return match_system_of(const_cast<World&>(world));
}

MatchStatus match_status(const World& world) {
  const MatchSystem* match = match_system_of(world);
  return match_status(world, match == nullptr ? kNoPlayer : match->human());
}

MatchStatus match_status(const World& world, PlayerId viewer) {
  MatchStatus status;
  const MatchSystem* match = match_system_of(world);
  if (match == nullptr) return status;

  status.over = match->over(world);
  status.winner = match->winner(world);
  status.human = viewer;
  status.human_outcome =
      PlayerTable::is_valid(viewer) ? match->outcome(viewer) : MatchOutcome::undecided;
  // The script's closing sentence is addressed to the match's human only.
  status.message = viewer == match->human() ? match->end_message() : std::string_view{};

  if (status.human == kNoPlayer) return status;
  // A win the human's own script reported stands on its own: `2`, `3` and `4`
  // all call `EndGame(player, false)` and mean it.
  if (status.human_outcome == MatchOutcome::won) {
    status.human_won = true;
    return status;
  }
  if (status.human_outcome == MatchOutcome::lost || !status.over) return status;
  // Otherwise the human won by outlasting everyone -- the elimination case,
  // where no script ever reports a win and the survivor is derived.
  status.human_won = status.winner != kNoPlayer &&
                     (status.winner == status.human ||
                      world.players().are_allied(status.winner, status.human));
  return status;
}

// --------------------------------------------------------------------------
// setup
// --------------------------------------------------------------------------

std::size_t setup_match(World& world, MatchSystem& match, const MatchRules& rules,
                        const MatchOptions& options) {
  PlayerId human = options.human;
  if (human == kNoPlayer) human = rules.start_player;
  if (!PlayerTable::is_valid(human)) human = kNoPlayer;

  MatchRules chosen = rules;
  if (options.condition_set) {
    chosen.condition = options.condition;
    chosen.param = options.threshold;
  }
  chosen.world_population = options.world_population;
  chosen.starting_gold = options.starting_gold;
  chosen.fog_of_war = options.fog_of_war;
  chosen.exploration = options.exploration;
  match.configure(chosen, human, options.multiplayer);

  PlayerTable& table = world.players();
  std::size_t participants = 0;

  // One ascending walk. The RNG draws happen inside it, so this order is world
  // state and not a convenience.
  for (std::size_t i = 0; i < kPlayerCount; ++i) {
    const auto id = static_cast<PlayerId>(i);
    PlayerSetup& setup = table.setup(id);

    if (options.control_set[i]) setup.control = options.controls[i];

    MatchPlayer slot;
    slot.control = setup.control;
    slot.participates = setup.control != PlayerControl::disabled && id != kNeutralWildlife &&
                        id != kNeutralPassive;
    // The counters the census at `start` took stay: the original's statistics
    // records are the game's, not the setup's, and the soldiers the map
    // placed were booked as it was loaded.
    slot.score = match.player(id).score;

    if (!slot.participates) {
      // A slot that takes no part keeps whatever the file said and draws
      // nothing: burning a draw on it would make the stream depend on how many
      // empty slots a map happens to declare.
      slot.race = kNoRace;
      match.set_player(id, slot);
      continue;
    }
    ++participants;

    if (id == human) {
      slot.control = PlayerControl::human;
    } else if (setup.control == PlayerControl::both) {
      slot.control = PlayerControl::computer;
    } else if (setup.control == PlayerControl::human && !options.multiplayer) {
      slot.control = PlayerControl::computer;
    }
    setup.control = slot.control;

    std::int32_t race = options.races[i];
    if (race < 0 || race >= kRaceCount) race = race_from_name(setup.race);
    if (race < 0 || race >= kRaceCount) {
      // `Random`, `Mutable`, `Select`, an empty attribute, or a spelling this
      // build does not know: all of them draw. See the header on the
      // distribution being an assumption and the source not being one.
      (void)is_race_marker(setup.race);
      race = world.rng().below(kRaceCount);
    }
    slot.race = race;
    // `GetPlayerRace` reads the string, so the resolution has to land there.
    setup.race = std::string(race_to_name(race));

    match.set_player(id, slot);
  }
  return participants;
}

std::size_t start_victory_scripts(World& world, script::Scheduler& scheduler) {
  MatchSystem* match = match_system_of(world);
  if (match == nullptr) return 0;
  const std::string path = victory_script_path(match->rules().condition);
  if (path.empty()) return 0;

  std::size_t started = 0;
  for (std::size_t i = 0; i < kPlayerCount; ++i) {
    const auto id = static_cast<PlayerId>(i);
    if (!match->player(id).participates) continue;
    const Value args[] = {Value::integer(player_to_script(id)), Value::string(match->rules().param)};
    if (scheduler.spawn_by_name(path, args) != script::kNoScript) ++started;
  }
  return started;
}

/// `GetTeamMilitaryScore(player)` -- 2 sites, both in
/// `3 TIME LIMIT (MILITARY RATING).VS`, which compares two teams' scores when
/// the clock runs out and ends the game on the larger.
///
/// The argument is **1-based**: 0x004c6710 pops it and `dec`s it before the
/// call, like every other player argument in the family.
HostOutcome fn_team_military_score(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetTeamMilitaryScore: no world");
  const MatchSystem* match = match_system_of(*world);
  if (match == nullptr) return HostOutcome::failed("GetTeamMilitaryScore: no match");
  const PlayerId player =
      ctx.arg(0).is_integer() ? player_from_script(ctx.arg(0).as_integer()) : kNoPlayer;
  return HostOutcome::ok_with(Value::integer(match->team_military_score(*world, player)));
}

/// `GetTeamOverallScore(player)` -- 4 sites, and the sole blocker of
/// `2 Score limit.VS` and `4 Time limit (score).VS`, which are 158 call sites
/// between them.
///
/// The argument is 1-based; 0x004c67a0 `dec`s it before the call, like every
/// other player argument in this family. Non-const `World&` because the census
/// behind it reaches for the economy and combat systems over `World::systems()`.
HostOutcome fn_team_overall_score(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetTeamOverallScore: no world");
  MatchSystem* match = match_system_of(*world);
  if (match == nullptr) return HostOutcome::failed("GetTeamOverallScore: no match");
  const PlayerId player =
      ctx.arg(0).is_integer() ? player_from_script(ctx.arg(0).as_integer()) : kNoPlayer;
  return HostOutcome::ok_with(Value::integer(match->team_overall_score(*world, player)));
}

/// `EvalGroup(name)` -- 1 site. 0x004350e0 looks the group up by name in the
/// group table, and answers 0 for a name it does not hold; otherwise it walks
/// the group's members and sums `object_power` -- the census valuation, 16
/// bits of it -- over those that are units. A member that is not a unit
/// contributes nothing, and a member the world no longer holds is skipped.
///
/// `World::group_index` interns rather than finds, because `Group("X")` on an
/// undeclared name has to keep answering; here a name nobody declared is
/// looked up with `find` and answers 0 without minting a row.
HostOutcome fn_eval_group(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("EvalGroup: no world");
  if (!ctx.arg(0).is_string()) return HostOutcome::ok_with(Value::integer(0));
  const std::int32_t group = world->groups().find(ctx.arg(0).as_string());
  if (group < 0) return HostOutcome::ok_with(Value::integer(0));
  const CombatSystem* combat = combat_system_of(*world);
  std::int64_t total = 0;
  for (const ObjectId id : world->groups().members(group)) {
    const WorldObject* slot = world->find(id);
    if (slot == nullptr || (world->sync_flags(id) & kSyncUnit) == 0) continue;
    total += object_power(*world, combat, *slot);
  }
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(total)));
}

// --------------------------------------------------------------------------
// the scores
// --------------------------------------------------------------------------

std::int32_t military_rating(const PlayerScoreCounters& counters) noexcept {
  // Unsigned throughout, as the original is: `div` at 0x0056c700, not `idiv`.
  const auto numerator = static_cast<std::uint32_t>(
      ((static_cast<std::uint32_t>(counters.kill_healths) >> 1) +
       static_cast<std::uint32_t>(counters.damage_inflicted) + 1000u) * 100u);
  const std::uint32_t denominator = (static_cast<std::uint32_t>(counters.die_healths) >> 1) +
                                    static_cast<std::uint32_t>(counters.damage_taken) + 10000u;
  if (denominator == 0) return 0;  // unreachable: the +10000 cannot be cancelled
  return static_cast<std::int32_t>(numerator / denominator);
}

namespace {

/// Whether a blow between these two owners is counted at all.
///
/// See `MatchSystem::record_damage`: the original routes an allied blow into a
/// capped non-lethal branch that books nothing, and `PlayerTable::are_allied`
/// is mutual bit 0 and true for a player against itself.
[[nodiscard]] bool blow_is_counted(const World& world, PlayerId attacker,
                                   PlayerId victim) noexcept {
  if (attacker == kNoPlayer || victim == kNoPlayer) return true;
  if (!PlayerTable::is_valid(attacker) || !PlayerTable::is_valid(victim)) return true;
  return !world.players().are_allied(attacker, victim);
}

}  // namespace

void MatchSystem::record_damage(const World& world, PlayerId attacker, PlayerId victim,
                                std::int32_t amount) noexcept {
  if (amount <= 0) return;
  if (!blow_is_counted(world, attacker, victim)) return;
  if (PlayerTable::is_valid(attacker)) players_[attacker].score.damage_inflicted += amount;
  if (PlayerTable::is_valid(victim)) players_[victim].score.damage_taken += amount;
}

void MatchSystem::record_gold_spent(PlayerId player, std::int32_t amount) noexcept {
  if (!PlayerTable::is_valid(player)) return;
  players_[player].score.gold += amount;
}

void MatchSystem::record_kill(const World& world, PlayerId killer, PlayerId victim,
                              std::int32_t max_health) noexcept {
  if (max_health <= 0) return;
  if (!blow_is_counted(world, killer, victim)) return;
  if (PlayerTable::is_valid(killer)) {
    // One kill and its health, in the same guarded branch (0x00511905 and
    // 0x0051192d are eleven instructions apart).
    players_[killer].score.kill_healths += max_health;
    players_[killer].score.units_killed += 1;
  }
  if (PlayerTable::is_valid(victim)) players_[victim].score.die_healths += max_health;
}

void MatchSystem::record_food_spent(PlayerId player, std::int32_t amount) noexcept {
  if (!PlayerTable::is_valid(player)) return;
  players_[player].score.food += amount;
}

void MatchSystem::record_gold_captured(PlayerId player, std::int32_t amount) noexcept {
  if (!PlayerTable::is_valid(player)) return;
  players_[player].score.gold_captured += amount;
}

void MatchSystem::record_gold_produced(PlayerId player, std::int32_t amount) noexcept {
  if (!PlayerTable::is_valid(player) || amount <= 0) return;
  players_[player].score.gold_townhall += amount;
}

void MatchSystem::record_gold_converted(PlayerId player, std::int32_t amount) noexcept {
  if (!PlayerTable::is_valid(player)) return;
  players_[player].score.gold_outpost += amount;
}

namespace {

/// The unit statistics count `Military` and nothing else: 0x00540130 is an
/// is-heir-of against that one class, cached in a global, and both the
/// producer's and the destructor's counts are gated on it.
[[nodiscard]] bool counts_as_soldier(const World& world, const WorldObject& slot) noexcept {
  if (!slot.state.flags.is_unit || slot.class_index == kNoClass) return false;
  if (world.class_graph() == nullptr) return false;
  const ClassFilter filter = ClassFilter::parse("Military", world.class_graph());
  if (filter.match_all) return false;
  return world.matches_filter(slot, filter);
}

}  // namespace

void MatchSystem::start(World& world) {
  world.set_observer(this);
  for (const WorldObject& slot : world.objects()) {
    if (slot.state.flags.unspawned || slot.state.owner == kNoPlayer) continue;
    if (!counts_as_soldier(world, slot)) continue;
    owner_set(world, slot.id, kNoPlayer, slot.state.owner);
  }
}

void MatchSystem::owner_set(World& world, ObjectId id, PlayerId previous, PlayerId now) {
  (void)previous;
  if (!PlayerTable::is_valid(now)) return;
  const WorldObject* slot = world.find(id);
  if (slot == nullptr || !counts_as_soldier(world, *slot)) return;
  PlayerScoreCounters& score = players_[now].score;
  score.units_produced += 1;
  // 0x005dc4fc: with `produced >= lost`, the units standing are their
  // difference, and the maximum climbs by one when they exceed it.
  if (score.units_produced >= score.units_lost &&
      score.units_produced - score.units_lost > score.units_max) {
    score.units_max += 1;
  }
}

void MatchSystem::despawning(World& world, const WorldObject& object) {
  if (object.state.flags.unspawned || !PlayerTable::is_valid(object.state.owner)) return;
  if (!counts_as_soldier(world, object)) return;
  players_[object.state.owner].score.units_lost += 1;
}

const PlayerScoreCounters& MatchSystem::score(PlayerId id) const noexcept {
  static const PlayerScoreCounters kNone;
  return PlayerTable::is_valid(id) ? players_[id].score : kNone;
}

namespace {

/// `0x004439d0` -- what one object is worth to the census, truncated to the
/// sixteen bits its two callers read it through.
///
///     is-a `RamUnit` ? 1
///                    : (attack + armor_slash) * (level + 13)
///                      * (maxhealth / 4 + health) / 1000 + 1
///
/// Every term is measured: `[obj+0xc0]`/`[obj+0xc8]` are `Obj::health` and
/// `Obj::maxhealth` (`sim/wait.cpp` identified that pair), `[obj+0xdc]` and
/// `[obj+0xe4]` are `Obj::attack` (0x005add80) and `Obj::armor_slash`
/// (0x005adc00), and `vtbl+0x114` is the effective level. The `+13` is
/// `inc eax` then `add eax, 0xc` at 0x00443a16, and the division by 1000 is the
/// `0x10624dd3` magic at 0x00443a22 followed by `+ 1`.
///
/// **The ram is worth one and nothing else**, which is the whole of
/// 0x00540460: a cached class lookup of `"RamUnit"` and an ancestry test. A
/// siege ram carries a huge health pool and no attack, and counting it by the
/// formula would make a battering ram the most valuable thing on the map.
}  // namespace

std::uint16_t object_power(const World& world, const CombatSystem* combat,
                           const WorldObject& slot) {
  const ClassGraph* graph = world.class_graph();
  const ClassIndex ram = graph == nullptr ? kNoClass : graph->lookup("RamUnit");
  // `World::class_is_a` rather than a walk over `ancestry`, which returns a
  // fresh vector: the same test, and this is now called for every member of
  // every squad once a turn (`revalue_squads`).
  if (ram != kNoClass && world.class_is_a(slot.id, ram)) return 1;
  if (combat == nullptr) return 1;
  const Combatant* unit = combat->find(slot.id);
  if (unit == nullptr) return 1;
  const CombatProfile profile = combat->profile(unit->class_index);

  // Signed division truncating toward zero, which is what `cdq / and edx,3 /
  // sar 2` performs -- and it matters for nothing on shipped data, where a
  // maximum health is never negative.
  // All four terms are the *object's* cached numbers there, not the class's:
  // `0x004439e6` reads `[obj+0xc8]`, `[obj+0xc0]`, `[obj+0xdc]` and
  // `[obj+0xe4]`. So `Unit::AddBonus`'s record reaches the power census, which
  // is why the maximum and the attack come through `CombatSystem`'s readers.
  const std::int64_t health_term = combat->max_health_of(*unit) / 4 + slot.state.health;
  // `vtbl+0x114`. `effective_level` is the system's own private helper, so the
  // two halves it adds are taken here: the combatant's floored level and the
  // owner's difficulty addend.
  const std::int64_t level_term =
      unit->base_effective_level() + combat->player_level_addend(unit->owner) + 13;
  // `armour_bonus` -- what a hero's Team Defense puts on its army -- is
  // deliberately *not* folded in here. Whether the original's skills reach
  // `[obj+0xe4]` through the same rebuild the record does is a separate
  // question this block does not open; only the record is added.
  const std::int64_t attack_term = static_cast<std::int64_t>(combat->attack_of(*unit)) +
                                   profile.armor_slash + unit->bonus.armour_slash;
  const std::int64_t value = attack_term * level_term * health_term / 1000 + 1;
  return static_cast<std::uint16_t>(static_cast<std::uint64_t>(value) & 0xFFFFu);
}

namespace {

/// Both passes apply this before anything else: not a spawn template, and a
/// unit rather than a building or a piece of scenery.
[[nodiscard]] bool counts_towards_power(const WorldObject& slot, PlayerId player) noexcept {
  if (slot.state.owner != player) return false;
  if (slot.state.flags.unspawned) return false;   // `[obj+0x2c] & 0x8000000`
  return slot.state.flags.is_unit;                // `[obj+0x2c] & 0x400000`
}

}  // namespace

std::int32_t MatchSystem::power_score(World& world, PlayerId player) const {
  if (!PlayerTable::is_valid(player)) return 0;
  const CombatSystem* combat = combat_system_of(world);

  std::int64_t building_points = 0;
  std::int64_t gold = 0;
  std::int64_t food = 0;
  std::int64_t unit_power = 0;

  // -- the settlement walk (0x0056a6e0) ------------------------------------
  if (const EconomySystem* economy = economy_of(world); economy != nullptr) {
    for (const Settlement& settlement : economy->settlements().all()) {
      if (settlement.owner != player) continue;
      gold += settlement.warehouse.gold;
      food += settlement.warehouse.food;
      // 20 for a town hall, 3 for anything else. `SettlementKind::stronghold`
      // is exactly "`BaseTownhall` and its races", which is the ancestry test
      // 0x0056a7ee performs on the central building's class.
      building_points += settlement.kind == SettlementKind::stronghold ? 20 : 3;
      for (const ObjectId id : settlement.holder.units) {
        const WorldObject* slot = world.find(id);
        if (slot == nullptr || !counts_towards_power(*slot, player)) continue;
        unit_power += object_power(world, combat, *slot);
      }
    }
  }

  // -- the object-grid walk (0x0056a4a0, functor 0x0056a410) ----------------
  //
  // Held objects are skipped, and that is the grid's own shape rather than an
  // extra rule: an object inside a holder occupies no cell, so the original's
  // rectangle sweep never offers one. It is also what keeps a garrison from
  // being counted twice, since the walk above has already had it.
  for (const WorldObject& slot : world.objects()) {
    if (slot.state.holder != kNoObject) continue;
    if (!counts_towards_power(slot, player)) continue;
    unit_power += object_power(world, combat, slot);
    // A wagon's load lands in the same two totals the warehouses fill, split by
    // `restype`: `[wagon+0x1d4] == 0` is gold and anything else is food.
    if (slot.state.cargo != 0) {
      if (slot.state.cargo_resource == static_cast<std::int32_t>(Resource::gold)) {
        gold += slot.state.cargo;
      } else {
        food += slot.state.cargo;
      }
    }
  }

  const std::int64_t total = building_points + (gold + food / 2 + unit_power) / 100;
  return static_cast<std::int32_t>(total);
}

std::int32_t MatchSystem::team_overall_score(World& world, PlayerId player) const {
  if (!PlayerTable::is_valid(player)) return 0;
  // The same team the military score averages over, member for member -- see
  // its own note on why the bound is eight and why the asking player is on the
  // list from any slot.
  const auto term = [&](PlayerId id) {
    const PlayerScoreCounters& counters = players_[id].score;
    const auto rating = static_cast<std::uint32_t>(military_rating(counters));
    const auto power = static_cast<std::uint32_t>(power_score(world, id));
    return static_cast<std::uint32_t>(counters.gold) / 1000u + power * rating / 100u;
  };

  std::uint32_t total = term(player);
  std::uint32_t members = 1;
  for (PlayerId other = 0; other < kTeamScanSlots; ++other) {
    if (other == player) continue;
    if (!players_[other].participates) continue;
    if (!world.players().are_allied(player, other)) continue;
    total += term(other);
    ++members;
  }
  return static_cast<std::int32_t>(total / members);
}

std::int32_t MatchSystem::team_military_score(const World& world, PlayerId player) const {
  if (!PlayerTable::is_valid(player)) return 0;
  // The player itself is on the list before any test but the range one, and it
  // is on the list from any slot; only the *others* are drawn from the first
  // eight. See the header on why the bound is eight.
  std::uint32_t total = static_cast<std::uint32_t>(military_rating(players_[player].score));
  std::uint32_t members = 1;
  for (PlayerId other = 0; other < kTeamScanSlots; ++other) {
    if (other == player) continue;
    if (!players_[other].participates) continue;
    if (!world.players().are_allied(player, other)) continue;
    total += static_cast<std::uint32_t>(military_rating(players_[other].score));
    ++members;
  }
  return static_cast<std::int32_t>(total / members);
}

// --------------------------------------------------------------------------
// registration
// --------------------------------------------------------------------------

std::size_t register_match_host(script::HostRegistry& registry) {
  std::size_t defined = 0;
  const auto def = [&](script::CallKind kind, std::string_view name, std::uint16_t arity,
                       script::HostFn fn) {
    registry.define(kind, name, arity, fn);
    ++defined;
  };

  def(kFree, "EndGame", 2, &fn_end_game);
  def(kFree, "EndGame", 3, &fn_end_game_message);
  def(kFree, "IsMultiplayer", 0, &fn_is_multiplayer);
  def(kFree, "GetTime", 0, &fn_get_time);
  def(kFree, "MapName", 0, &fn_map_name);
  def(kFree, "MapSize", 0, &fn_map_size);
  def(kFree, "GetDifficulty", 0, &fn_get_difficulty);  // 136
  def(kFree, "SetDifficulty", 1, &fn_set_difficulty);  // 0
  // `DATA\SCDEBUG.XML`'s keypad bindings, and no `.vs` file.
  def(kFree, "GetSpeed", 0, &fn_get_speed);
  def(kFree, "SetSpeed", 1, &fn_set_speed);

  // 2 sites, and the sole blocker of `3 TIME LIMIT (MILITARY RATING).VS`.
  def(kFree, "GetTeamMilitaryScore", 1, &fn_team_military_score);  // 2
  def(kFree, "GetTeamOverallScore", 1, &fn_team_overall_score);    // 4
  def(kFree, "EvalGroup", 1, &fn_eval_group);                      // 1

  // `SetPlayerStatus`/3 and /4 are deliberately absent: they belong to
  // `register_player_host`, and defining them again here would silently replace
  // its versions with these.
  //
  // `GetTeamOverallScore/1` (4 sites) and `GetTeamPowerScore/1` (0) are absent
  // for a different reason: both multiply the military rating by a **power
  // score** (0x0056a8c0) this engine cannot compute yet. Two of that number's
  // four terms are settled -- the total combat power of the player's field army
  // and of its garrisons, through 0x004439d0, and a settlement count weighted
  // 20 for a town hall and 3 for anything else -- and two are not: a pair of
  // headcount buckets read off a per-object component at `+0x1cc`, split on a
  // mode flag at `+0x1d4` whose meaning was not settled. Half a formula is not
  // a score, and `2 SCORE LIMIT.VS` ends matches on this number.

  return defined;
}

std::size_t match_host_entry_count() noexcept { return 13; }

}  // namespace imperivm::core::sim
