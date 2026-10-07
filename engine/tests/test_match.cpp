// The match: setup, the victory rule, and the entry points it needs.
//
// Three things this file is built to catch, none of which is visible from
// inside `sim/match.cpp` alone:
//
//   1. **The RNG is the world's.** `race="Random"` is part of the simulation's
//      starting state, so two worlds seeded alike must produce the same eight
//      races and a world whose players all name a race must consume no draws at
//      all. A generator of match.cpp's own would pass every other assertion
//      here and fail both of those.
//
//   2. **No entry point is taken from another domain.** `HostRegistry::define`
//      replaces silently. `register_match_host` runs last, so a name it claims
//      that another domain already implemented would disappear without a
//      diagnostic -- and both domains' own tests would keep passing.
//      `SetPlayerStatus` is the live example: the player domain holds it and
//      this domain must not.
//
//   3. **A victory script actually reaches `EndGame`.** The last two tests
//      compile a script shaped like the shipped one -- the same thirteen entry
//      points in the same shapes -- run it against a real world through the
//      real registry, and assert the match ends. That is the only assertion
//      here that could not be satisfied by a stub.
//
//      That the *shipped* `DATA/GAMESCRIPTS/1 ELIMINATION.VS` does the same is
//      `tests/test_corpus_match.py`'s job, through `imcheck victory`, against
//      the file in the player's own `data.pak`. It used to be this file's, by
//      way of 2,138 bytes of it pasted into a string literal, which
//      `docs/legal.md` rule 1 forbids.

#include <array>
#include <cstddef>
#include <memory>
#include <vector>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "domains.hpp"
#include "test.hpp"

namespace {

using namespace imperivm::core;
using namespace imperivm::core::sim;

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A world with the two systems a match needs on it, in a fixed order.
struct MatchWorld {
  World world;
  MatchSystem match;
  EnvSystem env;

  explicit MatchWorld(std::uint32_t seed) {
    world.seed(seed);
    world.add_system(&env);
    world.add_system(&match);
  }

  /// Fill slots `[0, n)` with an enabled computer player of a named race, and
  /// make every one of them everybody else's enemy (relation word zero, which
  /// is what a fresh table already holds).
  void enable(std::size_t n, std::string_view race, PlayerControl control) {
    for (std::size_t i = 0; i < n; ++i) {
      PlayerSetup& setup = world.players().setup(static_cast<PlayerId>(i));
      setup.control = control;
      setup.race = std::string(race);
    }
  }
};

MatchRules elimination_rules() {
  MatchRules rules;
  rules.condition = VictoryCondition::elimination;
  rules.param = "0";
  rules.start_player = 0;
  return rules;
}

}  // namespace

// --------------------------------------------------------------------------
// the rule, as game.xml spells it
// --------------------------------------------------------------------------

TEST(match_victory_condition_parses_on_the_leading_integer) {
  // The two spellings that actually occur.
  CHECK(parse_victory_condition("0") == VictoryCondition::none);
  CHECK(parse_victory_condition("1 Elimination") == VictoryCondition::elimination);

  // The three that do not, keyed on the index rather than on the display half,
  // because the display half is unobserved.
  CHECK(parse_victory_condition("2 Score limit") == VictoryCondition::score_limit);
  CHECK(parse_victory_condition("3 anything at all") == VictoryCondition::time_limit_military);
  CHECK(parse_victory_condition("4") == VictoryCondition::time_limit_score);

  // Nothing else is a rule.
  CHECK(parse_victory_condition("") == VictoryCondition::none);
  CHECK(parse_victory_condition("Elimination") == VictoryCondition::none);
  CHECK(parse_victory_condition("5 Something new") == VictoryCondition::none);
  CHECK(parse_victory_condition("99999999999999999999") == VictoryCondition::none);
}

TEST(match_victory_script_paths_are_the_shipped_basenames) {
  CHECK(victory_script_path(VictoryCondition::none).empty());
  CHECK(victory_script_path(VictoryCondition::elimination) ==
        "data/GameScripts/1 Elimination.vs");
  CHECK(victory_script_path(VictoryCondition::score_limit) ==
        "data/GameScripts/2 Score limit.vs");
  CHECK(victory_script_path(VictoryCondition::time_limit_military) ==
        "data/GameScripts/3 Time limit (military rating).vs");
  CHECK(victory_script_path(VictoryCondition::time_limit_score) ==
        "data/GameScripts/4 Time limit (score).vs");
}

TEST(match_rules_parse_out_of_a_game_xml) {
  // The shape `docs/formats/map.md` documents, with the one victory condition
  // the hand-made scenarios carry.
  const std::string_view xml =
      "<game><properties game_type=\"0\" name=\"Balcans\" start_map=\"1\""
      " victory_condition=\"1 Elimination\" victory_threshold=\"0\""
      " single_only=\"0\" start_player=\"1\" season=\"spring\"/></game>";
  const Result<MatchRules> rules = MatchRules::parse_game_properties(bytes_of(xml));
  REQUIRE(rules.ok());
  CHECK(rules->condition == VictoryCondition::elimination);
  CHECK(rules->param == "0");
  CHECK(rules->start_player == 1);
  CHECK(!rules->single_only);

  // A `<map>` document is not a `<game>` one.
  CHECK(!MatchRules::parse_game_properties(bytes_of("<map/>")).ok());
  // A `<game>` with no `<properties>` is a container this reader does not
  // understand rather than a match with default rules.
  CHECK(!MatchRules::parse_game_properties(bytes_of("<game/>")).ok());
}

// --------------------------------------------------------------------------
// setup
// --------------------------------------------------------------------------

TEST(match_setup_resolves_control_from_the_declared_slots) {
  MatchWorld w(1);
  w.enable(4, "Gaul", PlayerControl::both);
  // Two slots the map disables, and one of the engine neutrals set to Computer
  // to prove it is still kept out.
  w.world.players().setup(9).control = PlayerControl::disabled;
  w.world.players().setup(kNeutralWildlife).control = PlayerControl::computer;
  w.world.players().setup(kNeutralWildlife).race = "Gaul";

  MatchOptions options;
  options.human = 2;
  const std::size_t participants = setup_match(w.world, w.match, elimination_rules(), options);

  CHECK(participants == 4);
  CHECK(w.match.participant_count() == 4);
  CHECK(w.match.human() == 2);
  CHECK(w.match.player(2).control == PlayerControl::human);
  CHECK(w.match.player(0).control == PlayerControl::computer);
  CHECK(w.match.player(1).control == PlayerControl::computer);
  CHECK(w.match.player(3).control == PlayerControl::computer);
  CHECK(!w.match.player(9).participates);
  // The neutral declared `Computer` and is still out: the victory scripts walk
  // players 1..8 and the neutrals hold the map's animals.
  CHECK(!w.match.player(kNeutralWildlife).participates);
  CHECK(!w.match.player(kNeutralPassive).participates);

  // The resolution lands on the world, which is what `IsAIPlayer` reads.
  CHECK(w.world.players().setup(2).control == PlayerControl::human);
  CHECK(w.world.players().setup(0).control == PlayerControl::computer);
}

TEST(match_setup_takes_the_human_from_start_player_when_the_caller_names_none) {
  MatchWorld w(1);
  w.enable(2, "Gaul", PlayerControl::both);
  MatchRules rules = elimination_rules();
  rules.start_player = 1;

  (void)setup_match(w.world, w.match, rules, MatchOptions{});
  CHECK(w.match.human() == 1);
  CHECK(w.match.player(1).control == PlayerControl::human);
  CHECK(w.match.player(0).control == PlayerControl::computer);
}

TEST(match_setup_keeps_a_declared_race_and_spends_no_randomness_on_it) {
  MatchWorld w(0x1234);
  w.enable(8, "Carthage", PlayerControl::computer);
  const std::uint32_t before = w.world.rng().state();

  (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});

  // The control the header demands: a map that names every race must leave the
  // stream exactly where it found it.
  CHECK(w.world.rng().state() == before);
  for (std::size_t i = 0; i < 8; ++i) {
    CHECK(w.match.player(static_cast<PlayerId>(i)).race ==
          static_cast<std::int32_t>(Race::carthage));
    CHECK(w.world.players().setup(static_cast<PlayerId>(i)).race == "Carthage");
  }
}

TEST(match_setup_draws_random_races_from_the_world_rng) {
  MatchWorld w(0x1234);
  w.enable(8, "Random", PlayerControl::computer);
  const std::uint32_t before = w.world.rng().state();

  (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});

  // Eight participants, eight unresolved races, eight draws -- and not one
  // more, so a slot that takes no part cannot shift the stream.
  Rng expected(before);
  std::array<std::int32_t, 8> drawn{};
  for (std::size_t i = 0; i < 8; ++i) drawn[i] = expected.below(kRaceCount);
  CHECK(w.world.rng().state() == expected.state());

  for (std::size_t i = 0; i < 8; ++i) {
    const std::int32_t race = w.match.player(static_cast<PlayerId>(i)).race;
    CHECK(race == drawn[i]);
    CHECK(race >= 0 && race < kRaceCount);
    // `GetPlayerRace` reads the string, so the resolution has to land there.
    CHECK(w.world.players().setup(static_cast<PlayerId>(i)).race == race_to_name(race));
  }
}

TEST(match_setup_treats_mutable_and_select_as_markers_too) {
  MatchWorld w(7);
  w.enable(3, "Mutable", PlayerControl::both);
  w.world.players().setup(1).race = "Select";
  w.world.players().setup(2).race = "";  // an attribute this build cannot read

  (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});
  for (std::size_t i = 0; i < 3; ++i) {
    const std::int32_t race = w.match.player(static_cast<PlayerId>(i)).race;
    CHECK(race >= 0 && race < kRaceCount);
  }
}

TEST(match_setup_is_deterministic_and_the_seed_is_what_moves_it) {
  const auto races_for = [](std::uint32_t seed) {
    MatchWorld w(seed);
    w.enable(8, "Random", PlayerControl::computer);
    (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});
    std::array<std::int32_t, 8> out{};
    for (std::size_t i = 0; i < 8; ++i) out[i] = w.match.player(static_cast<PlayerId>(i)).race;
    return out;
  };
  CHECK(races_for(99) == races_for(99));
  // The control: if this held for two different seeds the draw would not be
  // coming from the seeded stream at all.
  CHECK(races_for(99) != races_for(100));
}

TEST(match_setup_lets_the_caller_override_a_race) {
  MatchWorld w(5);
  w.enable(2, "Random", PlayerControl::both);
  MatchOptions options;
  options.races[0] = static_cast<std::int32_t>(Race::egypt);
  const std::uint32_t before = w.world.rng().state();

  (void)setup_match(w.world, w.match, elimination_rules(), options);

  CHECK(w.match.player(0).race == static_cast<std::int32_t>(Race::egypt));
  CHECK(w.world.players().setup(0).race == "Egypt");
  // One override, one marker left: exactly one draw.
  Rng expected(before);
  (void)expected.below(kRaceCount);
  CHECK(w.world.rng().state() == expected.state());
}

TEST(match_setup_lets_the_caller_override_control) {
  MatchWorld w(1);
  w.enable(3, "Gaul", PlayerControl::both);
  MatchOptions options;
  options.human = 0;
  options.control_set[2] = true;
  options.controls[2] = PlayerControl::disabled;

  const std::size_t participants = setup_match(w.world, w.match, elimination_rules(), options);
  CHECK(participants == 2);
  CHECK(!w.match.player(2).participates);
  CHECK(w.match.player(2).race == kNoRace);
}

// --------------------------------------------------------------------------
// the outcome
// --------------------------------------------------------------------------

TEST(match_end_game_records_the_first_report_and_keeps_it) {
  MatchWorld w(1);
  w.enable(2, "Gaul", PlayerControl::computer);
  (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});

  CHECK(w.match.outcome(0) == MatchOutcome::undecided);
  w.match.end_game(0, true);
  CHECK(w.match.outcome(0) == MatchOutcome::lost);
  // A second report does not change it.
  w.match.end_game(0, false);
  CHECK(w.match.outcome(0) == MatchOutcome::lost);

  // A slot that takes no part cannot report anything.
  w.match.end_game(7, true);
  CHECK(w.match.outcome(7) == MatchOutcome::undecided);
}

TEST(match_is_over_when_one_side_is_left_standing) {
  MatchWorld w(1);
  w.enable(3, "Gaul", PlayerControl::computer);
  MatchOptions options;
  options.human = 0;
  (void)setup_match(w.world, w.match, elimination_rules(), options);

  // Three mutual enemies: three groups, nobody has won.
  CHECK(!w.match.over(w.world));
  CHECK(w.match.winner(w.world) == kNoPlayer);

  w.match.end_game(2, true);
  CHECK(!w.match.over(w.world));

  w.match.end_game(1, true);
  CHECK(w.match.over(w.world));
  CHECK(w.match.winner(w.world) == 0);
  CHECK(w.match.human_outcome() == MatchOutcome::undecided);
}

TEST(match_is_over_the_moment_the_human_reports) {
  MatchWorld w(1);
  w.enable(3, "Gaul", PlayerControl::computer);
  MatchOptions options;
  options.human = 0;
  (void)setup_match(w.world, w.match, elimination_rules(), options);

  // The human loses while two enemies are still fighting each other. There is
  // no single side left, but the match this process is playing is decided.
  w.match.end_game(0, true);
  CHECK(w.match.over(w.world));
  CHECK(w.match.human_outcome() == MatchOutcome::lost);
  // And the survivor the derivation names is not the human.
  CHECK(w.match.winner(w.world) == 1);
}

TEST(match_in_a_networked_game_is_not_over_when_one_human_reports) {
  // The same three players, networked, two of them human. Every seat is a
  // human there, and `human` is only the lowest one -- the value every peer
  // agrees on because it is hashed. That seat losing ends the game for
  // nobody else.
  MatchWorld w(1);
  w.enable(3, "Gaul", PlayerControl::computer);
  MatchOptions options;
  options.human = 0;
  options.multiplayer = true;
  options.control_set[1] = true;
  options.controls[1] = PlayerControl::human;
  (void)setup_match(w.world, w.match, elimination_rules(), options);
  CHECK(w.world.players().setup(1).control == PlayerControl::human);

  w.match.end_game(0, true);
  CHECK(!w.match.over(w.world));
  // Each seat asks about itself: 0 has lost and 1 has not won yet.
  CHECK(match_status(w.world, 0).human_outcome == MatchOutcome::lost);
  CHECK(!match_status(w.world, 1).human_won);
  w.match.end_game(2, true);
  CHECK(w.match.over(w.world));
  CHECK(w.match.winner(w.world) == 1);
  const MatchStatus one = match_status(w.world, 1);
  const MatchStatus zero = match_status(w.world, 0);
  CHECK(one.over && zero.over);
  CHECK(one.human_won);
  CHECK(!zero.human_won);
  CHECK(one.winner == zero.winner);
}

TEST(match_allies_count_as_one_side) {
  MatchWorld w(1);
  w.enable(3, "Gaul", PlayerControl::computer);
  // Players 0 and 1 allied both ways -- `are_allied` is mutual bit 0, which is
  // what `DiplAreAllied` answers and what the four scripts test.
  w.world.players().set(0, 1, Relation::allied, true);
  w.world.players().set(1, 0, Relation::allied, true);
  (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});

  CHECK(!w.match.over(w.world));
  w.match.end_game(2, true);
  // Two standing players, allied: one side.
  CHECK(w.match.over(w.world));
  CHECK(w.match.winner(w.world) == 0);
}

TEST(match_with_nobody_left_has_no_winner) {
  MatchWorld w(1);
  w.enable(2, "Gaul", PlayerControl::computer);
  (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});
  w.match.end_game(0, true);
  w.match.end_game(1, true);
  CHECK(w.match.over(w.world));
  CHECK(w.match.winner(w.world) == kNoPlayer);
}

TEST(match_status_is_the_one_call_a_play_loop_makes) {
  MatchWorld w(1);
  w.enable(3, "Gaul", PlayerControl::computer);
  MatchOptions options;
  options.human = 0;
  (void)setup_match(w.world, w.match, elimination_rules(), options);

  MatchStatus status = match_status(w.world);
  CHECK(!status.over);
  CHECK(!status.human_won);
  CHECK(status.human == 0);
  CHECK(status.winner == kNoPlayer);
  CHECK(status.human_outcome == MatchOutcome::undecided);

  // The elimination case, and the reason this is worth a function: both
  // computers report their own defeat, nobody reports a win, and the human's
  // victory is entirely the host's derivation.
  w.match.end_game(1, true);
  CHECK(!match_status(w.world).human_won);
  w.match.end_game(2, true);

  status = match_status(w.world);
  CHECK(status.over);
  CHECK(status.winner == 0);
  CHECK(status.human_won);
  // And the human never said so itself.
  CHECK(status.human_outcome == MatchOutcome::undecided);
}

TEST(match_status_says_defeat_when_the_human_is_eliminated) {
  MatchWorld w(1);
  w.enable(2, "Gaul", PlayerControl::computer);
  MatchOptions options;
  options.human = 0;
  (void)setup_match(w.world, w.match, elimination_rules(), options);

  w.match.end_game(0, true);
  const MatchStatus status = match_status(w.world);
  CHECK(status.over);
  CHECK(!status.human_won);
  CHECK(status.human_outcome == MatchOutcome::lost);
  CHECK(status.winner == 1);
}

TEST(match_status_counts_an_allys_survival_as_the_humans_win) {
  MatchWorld w(1);
  w.enable(3, "Gaul", PlayerControl::computer);
  // The human is player 1, allied with player 0. Player 0 is the lower index
  // and so is the derived winner -- the human still won.
  w.world.players().set(0, 1, Relation::allied, true);
  w.world.players().set(1, 0, Relation::allied, true);
  MatchOptions options;
  options.human = 1;
  (void)setup_match(w.world, w.match, elimination_rules(), options);

  w.match.end_game(2, true);
  const MatchStatus status = match_status(w.world);
  CHECK(status.over);
  CHECK(status.winner == 0);
  CHECK(status.human == 1);
  CHECK(status.human_won);
}

TEST(match_status_on_a_world_with_no_match_is_a_game_that_is_not_over) {
  // What a conformance run polls, and what `--play` polls before setup has
  // happened. Neither should have to check for null.
  World bare;
  const MatchStatus status = match_status(bare);
  CHECK(!status.over);
  CHECK(!status.human_won);
  CHECK(status.human == kNoPlayer);
  CHECK(status.winner == kNoPlayer);
}

TEST(match_state_is_hashed_and_an_outcome_moves_it) {
  MatchWorld a(1);
  MatchWorld b(1);
  a.enable(2, "Gaul", PlayerControl::computer);
  b.enable(2, "Gaul", PlayerControl::computer);
  (void)setup_match(a.world, a.match, elimination_rules(), MatchOptions{});
  (void)setup_match(b.world, b.match, elimination_rules(), MatchOptions{});

  std::uint64_t ha = 0;
  std::uint64_t hb = 0;
  a.match.hash(ha);
  b.match.hash(hb);
  CHECK(ha == hb);

  a.match.end_game(1, true);
  ha = 0;
  a.match.hash(ha);
  CHECK(ha != hb);
}

// --------------------------------------------------------------------------
// the scores
// --------------------------------------------------------------------------

TEST(match_military_rating_is_the_recovered_arithmetic) {
  // A player that has neither dealt nor taken anything: (0 + 0 + 1000) * 100
  // over (0 + 0 + 10000) = 10, which is the floor of the whole scale.
  CHECK(military_rating(PlayerScoreCounters{}) == 10);

  PlayerScoreCounters c;
  c.kill_healths = 4000;   // >> 1 -> 2000
  c.damage_inflicted = 3000;
  c.die_healths = 2000;    // >> 1 -> 1000
  c.damage_taken = 4000;
  // (2000 + 3000 + 1000) * 100 / (1000 + 4000 + 10000) = 600000 / 15000 = 40.
  CHECK(military_rating(c) == 40);

  // The halving is a shift, not a divide, and the truncation is the original's.
  PlayerScoreCounters odd;
  odd.kill_healths = 3;  // >> 1 -> 1
  CHECK(military_rating(odd) == (1 + 0 + 1000) * 100 / 10000);
}

/// The team a score averages over is the alliance matrix, in both directions,
/// and the caller is always on it.
///
/// 0x00522e20 inserts the player itself before any test, then scans the *first
/// eight slots* for others that are mutually allied and pass a membership
/// filter. The eight is not a typo of sixteen on my part: the loop bound is
/// `esi < 0x1900` over a `0x320` stride and there is a dead `cmp esi, 0x3200`
/// inside the body, which is what a sixteen-slot version left behind.
TEST(match_team_military_score_averages_over_mutual_allies_in_the_first_eight) {
  MatchWorld w(1);
  w.enable(16, "Roman", PlayerControl::computer);
  (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});

  const auto give = [&](PlayerId id, std::int32_t inflicted, std::int32_t taken) {
    // Through the recorder, not through the field, so the test measures the
    // thing combat calls. Player 15 is nobody's ally and nobody's enemy, so
    // these are all counted.
    w.match.record_damage(w.world, id, 15, inflicted);
    w.match.record_damage(w.world, 15, id, taken);
  };
  give(0, 3000, 4000);   // rating 40 with the kill/die halves below
  w.match.record_kill(w.world, 0, 15, 8000);   // kill_healths 8000 -> >> 1 = 4000
  CHECK(military_rating(w.match.score(0)) == (4000 + 3000 + 1000) * 100 / (0 + 4000 + 10000));

  // Alone: the team is the player, and the average is its own rating.
  const std::int32_t alone = w.match.team_military_score(w.world, 0);
  CHECK(alone == military_rating(w.match.score(0)));

  // One-directional alliance is not a team, **either way round**. The shipped
  // data contains one-sided truces, and a test that only granted the
  // caller-to-candidate direction would pass against a predicate that read
  // only the other one.
  w.world.players().set(1, 0, Relation::allied, true);
  CHECK(w.match.team_military_score(w.world, 0) == alone);
  w.world.players().set(1, 0, Relation::allied, false);
  w.world.players().set(0, 1, Relation::allied, true);
  CHECK(w.match.team_military_score(w.world, 0) == alone);

  // Mutual, and now the average moves: player 1 has dealt and taken nothing,
  // so it rates the floor of 10.
  w.world.players().set(1, 0, Relation::allied, true);
  CHECK(military_rating(w.match.score(1)) == 10);
  CHECK(w.match.team_military_score(w.world, 0) == (alone + 10) / 2);
  // And symmetrically, because the matrix is.
  CHECK(w.match.team_military_score(w.world, 1) == (alone + 10) / 2);

  // A mutual ally in slot 8 is not on the team, because the scan stops at
  // eight. If it ran to sixteen this would be `(alone + 10 + 10) / 3`.
  w.world.players().set(0, 8, Relation::allied, true);
  w.world.players().set(8, 0, Relation::allied, true);
  CHECK(w.match.team_military_score(w.world, 0) == (alone + 10) / 2);
  // **And the relation is not symmetric even though the alliance is**, which
  // is the odd consequence of a scan narrower than the table: player 8 asks
  // about the first eight and finds player 0 there, so 0 is on 8's team while
  // 8 is not on 0's.
  CHECK(w.match.team_military_score(w.world, 8) == (10 + alone) / 2);

  // A slot that takes no part is not on anyone's team either, which is what
  // stops sixteen empty rows dragging every average to 10.
  w.world.players().set(0, 2, Relation::allied, true);
  w.world.players().set(2, 0, Relation::allied, true);
  CHECK(w.match.team_military_score(w.world, 0) == (alone + 10 + 10) / 3);
  w.match.set_player(2, MatchPlayer{});  // participates = false
  CHECK(w.match.team_military_score(w.world, 0) == (alone + 10) / 2);

  // Out of range answers 0. The original faults here: all four score entry
  // points hand 0x00522e20 a null record and it reads `+0x64` off it.
  CHECK(w.match.team_military_score(w.world, kNoPlayer) == 0);
  CHECK(w.match.team_military_score(w.world, 200) == 0);
}

/// The counters are hashed and they round-trip.
TEST(match_the_score_counters_survive_a_save_and_move_the_hash) {
  MatchWorld w(1);
  w.enable(4, "Roman", PlayerControl::computer);
  (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});

  // Each counter moves the hash on its own. Two peers that disagreed about any
  // one of them would answer `GetTeamMilitaryScore` differently and end the
  // match on different turns, so none of the four may be left out -- and a
  // single "the hash moved" check would not notice if three were.
  const auto hash_of = [&] {
    std::uint64_t value = 0;
    w.match.hash(value);
    return value;
  };
  const std::uint64_t empty = hash_of();
  w.match.record_damage(w.world, 0, kNoPlayer, 250);  // inflicted only
  const std::uint64_t inflicted = hash_of();
  CHECK(inflicted != empty);
  w.match.record_damage(w.world, kNoPlayer, 1, 250);  // taken only
  const std::uint64_t taken = hash_of();
  CHECK(taken != inflicted);
  w.match.record_kill(w.world, 0, kNoPlayer, 900);  // kill only
  const std::uint64_t killed = hash_of();
  CHECK(killed != taken);
  w.match.record_kill(w.world, kNoPlayer, 1, 900);  // died only
  const std::uint64_t after = hash_of();
  CHECK(after != killed);

  std::vector<std::byte> saved;
  w.match.serialize(saved);
  MatchSystem restored;
  REQUIRE(restored.deserialize(saved).ok());
  CHECK(restored.score(0).damage_inflicted == 250);
  CHECK(restored.score(1).damage_taken == 250);
  CHECK(restored.score(0).kill_healths == 900);
  CHECK(restored.score(1).die_healths == 900);
  std::uint64_t round_tripped = 0;
  restored.hash(round_tripped);
  CHECK(round_tripped == after);

  // A section written by the previous shape is refused rather than read short.
  // The literal 4 is the point: if `kSectionVersion` had not moved, the bytes
  // above would already carry it and this patch would be a no-op.
  std::vector<std::byte> older = saved;
  REQUIRE(older.size() > 8);
  older[4] = std::byte{4};
  older[5] = std::byte{0};
  older[6] = std::byte{0};
  older[7] = std::byte{0};
  MatchSystem refused;
  CHECK(!refused.deserialize(older).ok());

  // `gold` is saved even though nothing maintains it, so that the day
  // something does there is no hole to widen.
  MatchPlayer rich = w.match.player(3);
  rich.score.gold = 12345;
  w.match.set_player(3, rich);
  std::vector<std::byte> again;
  w.match.serialize(again);
  MatchSystem back;
  REQUIRE(back.deserialize(again).ok());
  CHECK(back.score(3).gold == 12345);
}

/// The entry point, called the way `3 TIME LIMIT (MILITARY RATING).VS` calls
/// it: with a **1-based** player number.
TEST(match_get_team_military_score_takes_a_one_based_player) {
  MatchWorld w(1);
  w.enable(4, "Roman", PlayerControl::computer);
  (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});
  w.match.record_damage(w.world, 0, 1, 4000);

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &w.world;
  const auto call = [&](script::Value arg) -> script::HostOutcome {
    const std::uint32_t index =
        registry.find(script::CallKind::free_function, "GetTeamMilitaryScore", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    script::CallContext ctx;
    std::vector<script::Value> args{arg};
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "GetTeamMilitaryScore";
    ctx.kind = script::CallKind::free_function;
    return registry.entry(index).fn(ctx);
  };

  // 1 is player 0, and it is the one that has dealt the damage.
  const script::HostOutcome first = call(script::Value::integer(1));
  CHECK(first.status == script::HostStatus::ok);
  CHECK(first.value.as_integer() == w.match.team_military_score(w.world, 0));
  CHECK(first.value.as_integer() > 10);

  // 2 is player 1, which has only taken it.
  const script::HostOutcome second = call(script::Value::integer(2));
  CHECK(second.value.as_integer() == w.match.team_military_score(w.world, 1));
  CHECK(second.value.as_integer() < 10);

  // 0 and 17 are outside 1..16 and answer 0 rather than faulting.
  CHECK(call(script::Value::integer(0)).value.as_integer() == 0);
  CHECK(call(script::Value::integer(17)).value.as_integer() == 0);
  CHECK(call(script::Value::string("2")).value.as_integer() == 0);
}

TEST(match_answers_both_team_scores_now_that_the_census_exists) {
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  (void)register_match_host(registry);

  // **This test used to assert the opposite**, and the comment under it said
  // `GetTeamOverallScore` was left trapping because the power score behind it
  // was only half recovered -- the settlement weighting settled, the object
  // pass's per-object body not. That body is decoded now (see
  // `MatchSystem::power_score`), so the refusal has no subject left.
  // (`GetTeamPowerScore` and `GetTeamAchievementsScore` are registered in
  // `gbr.exe` beside it and have no call site anywhere, so they are not on the
  // shipped surface at all and are still unbound.)
  const std::uint32_t overall =
      registry.find(script::CallKind::free_function, "GetTeamOverallScore", 1);
  REQUIRE(overall != script::kUnresolvedHost);
  CHECK(registry.entry(overall).fn != nullptr);

  // The military one is answered, because the four counters behind it are
  // maintained.
  const std::uint32_t military =
      registry.find(script::CallKind::free_function, "GetTeamMilitaryScore", 1);
  REQUIRE(military != script::kUnresolvedHost);
  CHECK(registry.entry(military).fn != nullptr);
}

// --------------------------------------------------------------------------
// the host surface
// --------------------------------------------------------------------------

TEST(match_host_takes_no_entry_point_from_another_domain) {
  script::HostRegistry alone;
  script::declare_shipped_surface(alone);
  const std::size_t defined = register_match_host(alone);
  CHECK(defined == match_host_entry_count());
  CHECK(alone.implemented() == match_host_entry_count());

  // The whole table, with this domain running last -- where the manifest puts
  // it and where an overwrite would be silent. If any of the five were already
  // implemented, the gain here would be smaller than the count above.
  script::HostRegistry all;
  imperivm::test::define_all_except("match", all);
  const std::size_t before = all.implemented();
  (void)register_match_host(all);
  CHECK(all.implemented() - before == match_host_entry_count());
}

TEST(match_host_leaves_set_player_status_to_the_player_domain) {
  // Both arities, both already implemented before this domain runs, and both
  // still pointing at the same function afterwards. `SetPlayerStatus` is the
  // one entry point the four game scripts hammer -- 60 call sites between them
  // -- and it belongs to `register_player_host`, which documents why it is a
  // validated no-op.
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  for (const std::uint16_t arity : {std::uint16_t{3}, std::uint16_t{4}}) {
    const std::uint32_t index =
        registry.find(script::CallKind::free_function, "SetPlayerStatus", arity);
    REQUIRE(index != script::kUnresolvedHost);
    const script::HostFn before = registry.entry(index).fn;
    REQUIRE(before != nullptr);
    (void)register_match_host(registry);
    CHECK(registry.entry(index).fn == before);
  }
}

TEST(match_host_entry_points_answer_from_the_match) {
  MatchWorld w(1);
  w.enable(2, "Gaul", PlayerControl::computer);
  MatchRules rules = elimination_rules();
  rules.map_name = "Balcans";
  rules.map_size = 16384;
  MatchOptions options;
  options.human = 0;
  options.multiplayer = true;
  (void)setup_match(w.world, w.match, rules, options);

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  (void)register_match_host(registry);

  HostContext context;
  context.world = &w.world;
  context.object_type = kTypeObj;

  const auto call = [&](const char* name, std::uint16_t arity,
                        std::span<script::Value> args) -> script::HostOutcome {
    const std::uint32_t index = registry.find(script::CallKind::free_function, name, arity);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostFn fn = registry.entry(index).fn;
    if (fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.user = &context;
    ctx.arguments = args;
    ctx.name = name;
    return fn(ctx);
  };

  CHECK(call("IsMultiplayer", 0, {}).value.as_integer() == 1);
  CHECK(call("MapName", 0, {}).value.as_string() == "Balcans");
  // **16383, not 16384.** `MapSize()` is the largest legal coordinate, not the
  // extent: 0x0051b120 returns the map rectangle's inclusive `right`, and the
  // rectangle is `(0, 0, w - 1, h - 1)`. Its sibling `GetMapSize()` returns
  // `right - left + 1` and is the one that answers 16384. This asserted the
  // extent for as long as nothing read the two apart.
  CHECK(call("MapSize", 0, {}).value.as_integer() == 16383);

  // `GetTime` is milliseconds -- `ESH_BUILDARMY.VS` compares it against
  // `600000` for ten minutes. Called with no scheduler, as here, it is the
  // world's clock; inside a pass it is the running script's wake time
  // (`test_wakeup.cpp`).
  CHECK(call("GetTime", 0, {}).value.as_integer() == 0);
  w.world.advance(800);
  w.world.advance(800);
  CHECK(call("GetTime", 0, {}).value.as_integer() == 1600);

  // `EndGame(int player, bool lose)`, 1-based like every other player argument.
  script::Value lose[] = {script::Value::integer(2), script::Value::boolean(true)};
  (void)call("EndGame", 2, lose);
  CHECK(w.match.outcome(1) == MatchOutcome::lost);
  CHECK(w.match.outcome(0) == MatchOutcome::undecided);
}

TEST(difficulty_reads_back_what_was_set_and_refuses_everything_else) {
  MatchWorld w(1);
  (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  (void)register_match_host(registry);

  HostContext context;
  context.world = &w.world;
  context.object_type = kTypeObj;

  const auto call = [&](const char* name, std::uint16_t arity,
                        std::span<script::Value> args) -> script::HostOutcome {
    const std::uint32_t index = registry.find(script::CallKind::free_function, name, arity);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    const script::HostFn fn = registry.entry(index).fn;
    if (fn == nullptr) return script::HostOutcome::failed("not implemented");
    script::CallContext ctx;
    ctx.user = &context;
    ctx.arguments = args;
    ctx.name = name;
    return fn(ctx);
  };

  // A match nobody configured answers zero, which is what the game object's
  // field holds before the options screen writes it.
  REQUIRE(call("GetDifficulty", 0, {}).value.as_integer() == 0);

  // All three legal settings, and each one has to be *read back* rather than
  // merely accepted: a setter that validated and dropped the value would pass
  // a test that only checked the return.
  for (const std::int32_t legal : {2, 1, 0, 1, 2}) {
    script::Value args[] = {script::Value::integer(legal)};
    CHECK(call("SetDifficulty", 1, args).status == script::HostStatus::ok);
    CHECK(call("GetDifficulty", 0, {}).value.as_integer() == legal);
  }

  // 0x004c65d0 tests 0, 1 and 2 by name and falls through to a diagnostic. The
  // field keeps its previous value -- it is not clamped, not zeroed, and the
  // call is not a failure. `-1` and `3` are the two neighbours a range check
  // written the obvious way would let through if either bound were wrong.
  CHECK(w.match.difficulty() == 2);
  for (const std::int32_t illegal : {-1, 3, 4, 100, -2147483647 - 1, 2147483647}) {
    script::Value args[] = {script::Value::integer(illegal)};
    CHECK(call("SetDifficulty", 1, args).status == script::HostStatus::ok);
    CHECK(w.match.difficulty() == 2);
  }
  CHECK(!w.match.set_difficulty(3));
  CHECK(w.match.set_difficulty(1));
  CHECK(w.match.difficulty() == 1);
}

TEST(difficulty_moves_the_match_hash) {
  // Zama seq4 spawns Siphax at `30 + GetDifficulty() * 10`. Two peers that
  // disagree about the difficulty disagree about that unit's level on the turn
  // it appears, so the value has to be in the hash and not beside it.
  MatchWorld easy(1);
  MatchWorld hard(1);
  (void)setup_match(easy.world, easy.match, elimination_rules(), MatchOptions{});
  (void)setup_match(hard.world, hard.match, elimination_rules(), MatchOptions{});

  std::uint64_t a = 0;
  std::uint64_t b = 0;
  easy.match.hash(a);
  hard.match.hash(b);
  REQUIRE(a == b);

  REQUIRE(hard.match.set_difficulty(2));
  a = 0;
  b = 0;
  easy.match.hash(a);
  hard.match.hash(b);
  CHECK(a != b);

  // And a rejected value must not move it either, which is the same claim as
  // "the field did not change" seen from the side the netcode cares about.
  const std::uint64_t before = b;
  CHECK(!hard.match.set_difficulty(7));
  b = 0;
  hard.match.hash(b);
  CHECK(b == before);
}

TEST(match_host_entry_points_refuse_without_a_world) {
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  (void)register_match_host(registry);

  for (const char* name : {"EndGame", "IsMultiplayer", "GetTime", "MapName", "MapSize"}) {
    const std::uint32_t index = registry.find(script::CallKind::free_function, name, 0);
    const std::uint32_t index2 = registry.find(script::CallKind::free_function, name, 2);
    const std::uint32_t found = index != script::kUnresolvedHost ? index : index2;
    REQUIRE(found != script::kUnresolvedHost);
    script::CallContext ctx;
    ctx.user = nullptr;
    const script::HostOutcome outcome = registry.entry(found).fn(ctx);
    CHECK(outcome.status == script::HostStatus::error);
  }
}

// --------------------------------------------------------------------------
// the shipped script
// --------------------------------------------------------------------------

namespace {

/// A victory script written for this test, using every entry point the shipped
/// one uses.
///
/// **The retail `DATA/GAMESCRIPTS/1 ELIMINATION.VS` used to be here**, all
/// 2,138 bytes of it. `docs/legal.md` rule 1 forbids that -- no game assets in
/// the repository, "not as test fixtures" -- and `tools/check_fixtures.py` now
/// refuses it.
///
/// The claim that needed the retail bytes was *"the shipped script compiles
/// against this registry and reaches `EndGame`"*, and that claim did not
/// survive as a fixture -- it survived as
/// `tests/test_corpus_match.py::test_the_shipped_elimination_script_ends_the_game`,
/// which runs the real file out of `data.pak` through `imcheck victory`. That
/// is a better test of it: a copy can drift from the file it was taken from,
/// and this one cannot.
///
/// What is left here is the wiring, and it is written to keep every property
/// the copy had. It calls the **same nine entry points in the same shapes** --
/// `ClassPlayerObjs/2`, `EnvWriteInt/3`, `EnvReadInt/3`, `SetPlayerStatus/4`
/// and `/3`, `DiplAreAllied/2`, `MilUnits/1`, `GetConst/1`, `Translate/1`,
/// `Translatef/3`, `EndGame/2`, `Query::IsEmpty/0` and `Sleep/1` -- so a
/// missing or mis-aritied one still fails to compile here, which is the failure
/// this test was built to catch first.
constexpr std::string_view kEliminationScript = R"VS(//void, int player, str param
Query qHalls;
int rival, countdown, worst, losing;

qHalls = ClassPlayerObjs("BaseTownhall", player);
EnvWriteInt(player, "elimination", 0);
SetPlayerStatus(player, 0, Translate("Elimination"), false);

while (1) {
	Sleep(1000);
	SetPlayerStatus(player, 0, false);

	// How far along every rival's own countdown is. `losing` counts the ones
	// that have started; `worst` is the longest any of them has left. The
	// point of reading it out of the environment store rather than off a
	// system is that this is how the eight copies of this script talk to one
	// another -- so the arity of `EnvReadInt/3` is load-bearing here.
	worst = 0;
	losing = 0;
	rival = 1;
	while (rival <= 8) {
		int theirs;
		theirs = 0;
		if (rival != player) {
			if (!DiplAreAllied(rival, player)) {
				theirs = EnvReadInt(rival, "elimination");
				if (theirs == 0) losing = losing - 1;
				else losing = losing + 1;
				if (theirs > worst) worst = theirs;
			}
		}
		rival = rival + 1;
	}
	if (losing > 0 && worst > 1) {
		SetPlayerStatus(player, 1, Translatef("win %s1:%s2", worst / 60, worst % 60), true);
		SetPlayerStatus(player, 0, true);
	}

	// Still holding a hall: nothing to count down.
	if (!qHalls.IsEmpty()) continue;

	countdown = GetConst("EliminationTimeout");
	while (countdown > 0) {
		if (MilUnits(player) == 0) countdown = 0;
		else {
			EnvWriteInt(player, "elimination", countdown);
			SetPlayerStatus(player, 1, Translatef("lose %s1:%s2", countdown / 60, countdown % 60), true);
			Sleep(1000);
			if (!qHalls.IsEmpty()) break;
			countdown = countdown - 1;
		}
	}
	if (countdown == 0) {
		SetPlayerStatus(player, 1, Translate("out"), true);
		SetPlayerStatus(player, 0, true);
		EnvWriteInt(player, "elimination", 1);
		EndGame(player, true);
		return;
	}

	// Recaptured before the clock ran out; tell the rivals to stop hoping.
	EnvWriteInt(player, "elimination", 0);
}
)VS";

}  // namespace

TEST(match_the_shipped_elimination_script_runs_and_ends_the_game) {
  MatchWorld w(1);
  w.enable(2, "Gaul", PlayerControl::computer);
  MatchOptions options;
  options.human = 0;
  (void)setup_match(w.world, w.match, elimination_rules(), options);

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  (void)register_match_host(registry);

  // The shipped bytes have to parse and compile against the real registry.
  // A trap in either is the interesting failure, so both say why.
  script::Diagnostic diagnostic;
  const auto parsed = script::parse(bytes_of(kEliminationScript), "1 Elimination.vs", &diagnostic);
  if (!parsed.ok()) {
    std::printf("  parse 1 Elimination.vs:%u: %.*s\n", diagnostic.line,
                static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
  }
  REQUIRE(parsed.ok());
  script::CompileError error;
  auto chunk = script::compile(parsed.value(), &registry, &error);
  if (!chunk.ok()) {
    std::printf("  compile 1 Elimination.vs:%u: %s\n", error.line, error.message.c_str());
  }
  REQUIRE(chunk.ok());

  WorldHost host(w.world);
  script::Scheduler scheduler;
  scheduler.set_registry(&registry);
  scheduler.set_host(&host);
  scheduler.set_user(&w.env);  // replaced below; see the context, not this
  HostContext context;
  context.world = &w.world;
  context.object_type = kTypeObj;
  scheduler.set_user(&context);
  install_objlist_lifetime(scheduler);

  const std::uint32_t index = scheduler.add_chunk(std::move(chunk.value()));
  const script::Value args[] = {script::Value::integer(player_to_script(0)),
                                script::Value::string("0")};
  REQUIRE(scheduler.spawn(index, args) != script::kNoScript);

  // The map has no town halls and the player has no units, so the elimination
  // timer runs out. `GetConst("EliminationTimeout")` answers zero without a
  // `CONST.INI`, which takes the `nTime == 0` branch on the first pass rather
  // than after 180 seconds -- the same branch, reached sooner.
  for (int pass = 0; pass < 8 && scheduler.live_count() > 0; ++pass) {
    const script::RunReport report = scheduler.advance(1000);
    for (const script::FailedScript& trap : report.traps) {
      std::printf("  trap %s:%u: %s\n", trap.source_name.c_str(), trap.trap.line,
                  trap.trap.detail.c_str());
    }
    CHECK(report.traps.empty());
  }

  CHECK(scheduler.live_count() == 0);
  CHECK(w.match.outcome(0) == MatchOutcome::lost);
  CHECK(w.match.human_outcome() == MatchOutcome::lost);
  CHECK(w.match.over(w.world));
  // The script's own bookkeeping landed in the environment store, which is
  // what every other player's copy reads to decide whether it has won.
  CHECK(w.env.env().read_int(EnvScope::for_player(player_to_script(0)), "elimination") == 1);
}

TEST(match_starts_one_victory_script_per_participant) {
  MatchWorld w(1);
  w.enable(3, "Gaul", PlayerControl::computer);
  MatchOptions options;
  options.human = 0;
  (void)setup_match(w.world, w.match, elimination_rules(), options);

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  (void)register_match_host(registry);

  // The path has to outlive the compile: `Script::source_name` is a view, and
  // the chunk's copy of it is what `spawn_by_name` looks up.
  const std::string path = victory_script_path(VictoryCondition::elimination);
  script::Diagnostic diagnostic;
  const auto parsed = script::parse(bytes_of(kEliminationScript), path, &diagnostic);
  REQUIRE(parsed.ok());
  script::CompileError error;
  auto chunk = script::compile(parsed.value(), &registry, &error);
  REQUIRE(chunk.ok());

  WorldHost host(w.world);
  script::Scheduler scheduler;
  scheduler.set_registry(&registry);
  scheduler.set_host(&host);
  HostContext context;
  context.world = &w.world;
  context.object_type = kTypeObj;
  scheduler.set_user(&context);
  install_objlist_lifetime(scheduler);
  (void)scheduler.add_chunk(std::move(chunk.value()));

  CHECK(start_victory_scripts(w.world, scheduler) == 3);
  CHECK(scheduler.live_count() == 3);

  // And a match with no victory condition starts nothing, which is 19 of the
  // 22 shipped containers.
  MatchWorld none(1);
  none.enable(3, "Gaul", PlayerControl::computer);
  (void)setup_match(none.world, none.match, MatchRules{}, MatchOptions{});
  CHECK(start_victory_scripts(none.world, scheduler) == 0);
}

// ==========================================================================
// map.xml
// ==========================================================================

namespace {

/// `Maps/2/map.xml` from `2_Great_loses_Spain`, trimmed to what is parsed.
constexpr std::string_view kSpainMapXml = R"(<map name="Spain" displayname="" descr="" persist_state="1">
  <size x="16384" y="16384"/>
  <start_pt x="0" y="0"/>
</map>)";

/// One placeable unit class, which is all `Place` needs to mint an object.
constexpr std::string_view kPlaceableClass =
    R"(<class id="RHastatus" cpp_class="CVXUnit"><properties sight="100" maxhealth="50"/></class>)";

[[nodiscard]] std::span<const std::byte> map_bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A session built with or without `map.xml`, and a way to place something.
struct MapExtentBench {
  script::HostRegistry registry;
  ClassGraph graph;
  SessionInputs inputs;
  std::unique_ptr<GameSession> session;

  explicit MapExtentBench(bool with_map_xml) : MapExtentBench(with_map_xml ? kSpainMapXml
                                                                          : std::string_view{}) {}

  explicit MapExtentBench(std::string_view map_xml) {
    register_all_hosts(registry);
    (void)graph.add(map_bytes_of(kPlaceableClass), "rhastatus.sc.xml");
    graph.link();
    inputs.classes = &graph;
    if (!map_xml.empty()) inputs.map_properties = map_bytes_of(map_xml);
    auto made = GameSession::create(registry, inputs, /*seed=*/1);
    REQUIRE(made.ok());
    if (made.ok()) session = std::move(made.value());
    if (session != nullptr) (void)session->start_match(MatchOptions{});
  }

  [[nodiscard]] const MatchRules* rules() {
    MatchSystem* match = match_system_of(session->world());
    return match == nullptr ? nullptr : &match->rules();
  }

  /// `Place("RHastatus", (x, y), 3)`, returning the object id or `kNoObject`.
  ObjectId place(std::int32_t x, std::int32_t y) {
    const std::uint32_t index = registry.find(script::CallKind::free_function, "Place", 3);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return kNoObject;
    HostContext context;
    context.world = &session->world();
    std::vector<script::Value> args{
        script::Value::string("RHastatus"),
        pack_point(Point{x, y}),
        script::Value::integer(3)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "Place";
    ctx.kind = script::CallKind::free_function;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    if (!out.value.is_object() || out.value.as_object().type != kTypeObj) return kNoObject;
    return out.value.as_object().id;
  }
};

}  // namespace

/// `map.xml` reaches `MatchRules`, and **without it nothing can be placed
/// anywhere**.
///
/// This is the wiring half of the failure this project keeps finding, and it is
/// the widest instance of it so far. `MatchRules` has carried `map_name` and
/// `map_size` since the match domain was written, with a comment saying they
/// come from `map.xml` and *the caller fills them in* -- and no caller ever
/// did. So `map_size` was zero in every session the engine has ever run.
///
/// A zero extent is not a small error. `Place`'s rectangle is
/// `0 .. map_size - 1`, which at zero collapses to the single point `(0, 0)`,
/// so **every one of its 131 call
/// sites refused every position on every real map**; `_PlaceEx` clamped its 89
/// into a degenerate rectangle; and `GetMapRect`, `IntoRect`, `ClampToMap` and
/// `MapSize` all answered about a map that was not there. Nothing failed, no
/// test went red, and the trap tally could not see it -- a refusal hands back
/// an invalid handle rather than trapping, so the cost showed up two statements
/// later as somebody else's receiver problem. On `2_Great_loses_Spain` map 2
/// that was nine sequences dying on a `SetFeeding` whose unit had never been
/// placed.
TEST(match_the_map_extent_comes_from_map_xml_and_place_needs_it) {
  MapExtentBench with(true);
  const MatchRules* rules = with.rules();
  REQUIRE(rules != nullptr);
  CHECK(rules->map_size == 16384);
  CHECK(rules->map_name == "Spain");

  // A real coordinate off that map -- the outpost at 11424, 9216 that
  // `OUTPOST_BEHAVIOR.VS` places its garrison at.
  CHECK(with.place(11424, 9216) != kNoObject);
  // Both edges, because `Place` includes them.
  CHECK(with.place(0, 0) != kNoObject);
  CHECK(with.place(16383, 16383) != kNoObject);
  // And one past the far edge, which it must still refuse.
  CHECK(with.place(16384, 0) == kNoObject);

  // The control, and the bug: no `map.xml`, no extent, nothing placeable.
  MapExtentBench without(false);
  const MatchRules* bare = without.rules();
  REQUIRE(bare != nullptr);
  CHECK(bare->map_size == 0);
  CHECK(bare->map_name.empty());
  CHECK(without.place(11424, 9216) == kNoObject);
  CHECK(without.place(1, 0) == kNoObject);
  CHECK(without.place(0, 1) == kNoObject);
  // The rectangle a zero extent leaves is not empty -- `high` is clamped up to
  // 0 rather than left at -1 -- so it is the single point `(0, 0)`, and that
  // one place still works. A degenerate map is worse than an empty one for
  // exactly this reason: it looks like a map that simply refuses a lot.
  CHECK(without.place(0, 0) != kNoObject);
  // A session with no map document is still legal -- every conformance fixture
  // is one -- so this degrades rather than failing, and `(-1, -1)`, the held
  // position, is exempt from the bounds test either way.
  CHECK(without.place(-1, -1) != kNoObject);
}

/// The extent is `size/@x`, and it is **not** `size/@y`.
///
/// Every shipped map declares the two equal, so no retail document can tell the
/// two readings apart -- which is exactly why this is asserted on a synthetic
/// one. `MapSize` (0x0051b120) returns a single field, so the engine has one
/// number and has to choose which attribute fills it; `x` is the choice, and a
/// choice nothing checks is a coin flip somebody will re-flip.
TEST(match_the_map_extent_is_size_x_and_not_size_y) {
  // `std::string_view`, spelled out: a `const char*` converts to `bool` by a
  // *standard* conversion and to `std::string_view` by a user-defined one, so a
  // bare literal here silently picks the other constructor and this test
  // measures the Spain fixture instead. It did, once.
  MapExtentBench oblong(
      std::string_view(R"(<map name="Oblong"><size x="8192" y="32768"/></map>)"));
  const MatchRules* rules = oblong.rules();
  REQUIRE(rules != nullptr);
  CHECK(rules->map_size == 8192);
  CHECK(rules->map_size != 32768);
  // And the rectangle follows it: the far corner of the *x* extent is inside,
  // one past it is not, and a point only `y` would have allowed is not either.
  CHECK(oblong.place(8191, 8191) != kNoObject);
  CHECK(oblong.place(8192, 0) == kNoObject);
  CHECK(oblong.place(0, 20000) == kNoObject);
}

/// A `map.xml` that does not parse leaves the extent at zero and the session
/// alive.
///
/// Degrading rather than failing is the rule every optional input in
/// `SessionInputs` follows, and it has to hold here too: a session with no map
/// document at all is legal -- every conformance fixture is one -- so a
/// *broken* document must not be the one case that takes the session down with
/// it. The cost of getting this wrong is not a wrong number, it is no session.
TEST(match_a_map_xml_that_does_not_parse_leaves_the_extent_at_zero) {
  for (const std::string_view broken : {
           std::string_view("<map name=\"Half\"><size x=\"16384\""),  // truncated
           std::string_view("<notamap><size x=\"16384\" y=\"16384\"/></notamap>"),
           std::string_view("not xml at all"),
       }) {
    MapExtentBench bench(broken);
    const MatchRules* rules = bench.rules();
    REQUIRE(rules != nullptr);
    CHECK(rules->map_size == 0);
    CHECK(rules->map_name.empty());
  }
}

/// The terrain-type layer reaches the session, and without it nothing is water.
///
/// **The fourth map payload, and each of the first three arrived the same way:**
/// something in the simulation turned out to be reading a number nobody had ever
/// handed it. `IsPointInWater` is the only consumer -- 127 hits in a corpus
/// pass across the lion's, the wolf's, the bear's and the fish's idle scripts
/// and a ship's unboarding verify -- and with no layer they all answer "not
/// water" and walk into the sea.
///
/// This is the wiring half, and it has its own test because the wiring is the
/// half that keeps going missing: a fault that reads the payload and never binds
/// it survives every test written against `World::set_terrain` directly. It did.
TEST(match_the_terrain_layer_reaches_the_session) {
  // A 2x2 layer of 64-unit cells with water at (1, 1), built by hand.
  std::vector<std::byte> layer;
  const auto put = [&layer](std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
      layer.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
    }
  };
  for (const char c : std::string_view("DIRG")) layer.push_back(static_cast<std::byte>(c));
  put(64);   // cell size
  put(8);    // bits per cell
  put(128);  // extent x
  put(128);  // extent y
  for (const std::uint32_t value : {3u, 3u, 3u, 13u}) {
    layer.push_back(static_cast<std::byte>(value));
  }

  const auto answers = [&](bool with_layer) {
    script::HostRegistry registry;
    register_all_hosts(registry);
    ClassGraph graph;
    SessionInputs inputs;
    inputs.classes = &graph;
    if (with_layer) inputs.terrain = layer;
    auto made = GameSession::create(registry, inputs, /*seed=*/1);
    CHECK(made.ok());
    if (!made.ok()) return false;
    HostContext context;
    context.world = &made.value()->world();
    const std::uint32_t index =
        registry.find(script::CallKind::free_function, "IsPointInWater", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return false;
    script::CallContext ctx;
    // Cell (1, 1) with the half-cell bias: x and y in 32..95.
    std::vector<script::Value> args{
        pack_point(Point{64, 64})};
    ctx.arguments = args;
    ctx.user = &context;
    ctx.kind = script::CallKind::free_function;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  };

  CHECK(answers(true));
  CHECK(!answers(false));
}

// --------------------------------------------------------------------------
// the power census
// --------------------------------------------------------------------------

namespace {

/// A world with the three systems `power_score` reaches for, a four-class
/// graph, and no packs anywhere.
///
/// The stats are deliberately large. A retail footman is worth about 59 to this
/// formula and the census divides by 100, so a test built on realistic numbers
/// would compare 0 against 0 and pass whatever the code did.
struct CensusWorld {
  World world;
  MatchSystem match;
  EnvSystem env;
  EconomySystem economy;
  CombatSystem combat;
  ClassGraph graph;

  CensusWorld() {
    world.seed(1);
    const std::string docs[] = {
        R"(<class id="Object" cpp_class="CVXDecor"><properties sight="0"/></class>)",
        R"(<class id="Heavy" parent="Object" cpp_class="CVXUnit">
             <properties maxhealth="4000" damage="200" armor_slash="100"/></class>)",
        R"(<class id="RamUnit" parent="Object" cpp_class="CVXUnit">
             <properties maxhealth="4000" damage="200" armor_slash="100"/></class>)",
        // A *child* of the ram, so the exclusion is an ancestry test rather
        // than a name comparison in disguise -- and it inherits the same stats,
        // so the only difference between it and `Heavy` is where it sits.
        R"(<class id="BigRam" parent="RamUnit" cpp_class="CVXUnit"/>)",
        // Tuned so the valuation's trailing `+ 1` is the whole difference
        // between 0 and 1 after the census divides by 100: one attack, no
        // armour, and 28,000/4 + 100 = 7,100, so 1 * 14 * 7100 / 1000 = 99.
        R"(<class id="Whelp" parent="Object" cpp_class="CVXUnit">
             <properties maxhealth="28000" damage="1" armor_slash="0"/></class>)",
        // The soldiers' base, and one soldier under it: the report's unit
        // counters count `Military` heirs and nothing else.
        R"(<class id="Military" parent="Object" cpp_class="CVXUnit">
             <properties maxhealth="4000" damage="200" armor_slash="100"/></class>)",
        R"(<class id="Legionary" parent="Military" cpp_class="CVXUnit"/>)",
    };
    const char* names[] = {"object.sc.xml", "heavy.sc.xml", "ram.sc.xml", "bigram.sc.xml",
                           "whelp.sc.xml", "military.sc.xml", "legionary.sc.xml"};
    for (int i = 0; i < 7; ++i) {
      graph.add(std::span<const std::byte>{reinterpret_cast<const std::byte*>(docs[i].data()),
                                           docs[i].size()},
                names[i]);
    }
    graph.link();
    world.set_class_graph(&graph);
    // The stats come down the class graph rather than from `set_profile`, so
    // `BigRam` inherits exactly what `Heavy` declares and the ram's exclusion
    // cannot be mistaken for a difference in its numbers.
    combat.set_class_graph(&graph);
    world.add_system(&env);
    world.add_system(&economy);
    world.add_system(&combat);
    world.add_system(&match);
    for (std::size_t i = 0; i < 4; ++i) {
      PlayerSetup& setup = world.players().setup(static_cast<PlayerId>(i));
      setup.control = PlayerControl::computer;
      setup.race = "Roman";
    }
  }

  ObjectId soldier(const char* class_name, PlayerId owner, std::int32_t health) {
    return spawn_as(NativeClass::unit, class_name, owner, health);
  }

  /// The same, as whatever native class is asked for -- so a building can be
  /// given a combat profile and still be excluded, which is the only way the
  /// `is_unit` test can be made to bite on its own.
  ObjectId spawn_as(NativeClass native, const char* class_name, PlayerId owner,
                    std::int32_t health) {
    const ClassIndex cls = graph.find(class_name);
    const ObjectId id = world.spawn(native, nullptr, cls);
    CHECK(world.set_owner(id, owner));
    CHECK(world.set_health(id, health));
    Combatant c;
    c.id = id;
    c.class_index = cls;
    c.owner = owner;
    c.health = health;
    combat.add(c);
    return id;
  }

  SettlementId plant(PlayerId owner, SettlementKind kind, ObjectId& anchor_out) {
    const ObjectId anchor = world.spawn(NativeClass::building, nullptr, graph.find("Object"));
    SettlementInit init;
    init.settlement_object = anchor;
    init.anchor = anchor;
    init.owner = owner;
    init.kind = kind;
    init.max_units = 100;
    anchor_out = anchor;
    return economy.settlements().create(init);
  }
};

/// The formula, written out here so the test measures the arithmetic rather
/// than agreeing with whatever the implementation does.
[[nodiscard]] std::int64_t expect_power(std::int64_t attack, std::int64_t armour,
                                        std::int64_t level, std::int64_t max_health,
                                        std::int64_t health) {
  return (attack + armour) * (level + 13) * (max_health / 4 + health) / 1000 + 1;
}

/// A fresh `Combatant` is level 1, and this file does not raise it -- so the
/// level term is 1 + 13 rather than 13, which is worth saying because the
/// `+13` is the part of the formula a reader would take for a constant.
constexpr std::int64_t kLevel = 1;

/// One full-health `Heavy`: 300 * 14 * 5000 / 1000 + 1.
constexpr std::int64_t kHeavy = 21001;

}  // namespace

/// `power_score` is `building_points + (gold + food/2 + unit_power) / 100`, and
/// the two object passes between them see each object once.
TEST(match_power_score_is_the_two_pass_census) {
  CensusWorld w;
  CHECK(expect_power(200, 100, kLevel, 4000, 4000) == kHeavy);
  // Nothing owned is nothing scored -- and 0, not the 10 the military rating
  // floors at, because this term has no `+1000` in it.
  CHECK(w.match.power_score(w.world, 0) == 0);

  // -- the settlement half -------------------------------------------------
  ObjectId hall = kNoObject;
  const SettlementId home = w.plant(0, SettlementKind::stronghold, hall);
  REQUIRE(home != kNoSettlement);
  CHECK(w.match.power_score(w.world, 0) == 20);  // 20 for a town hall

  ObjectId farm = kNoObject;
  const SettlementId village = w.plant(0, SettlementKind::village, farm);
  REQUIRE(village != kNoSettlement);
  CHECK(w.match.power_score(w.world, 0) == 23);  // ...and 3 for anything else

  // A settlement somebody else owns is not on my census at all.
  ObjectId theirs = kNoObject;
  REQUIRE(w.plant(1, SettlementKind::stronghold, theirs) != kNoSettlement);
  CHECK(w.match.power_score(w.world, 0) == 23);
  CHECK(w.match.power_score(w.world, 1) == 20);

  // Warehouse stores. **Food is halved and gold is not**, which is the one
  // asymmetry in the sum.
  w.economy.settlements().find(home)->warehouse.gold = 1000;
  CHECK(w.match.power_score(w.world, 0) == 23 + 1000 / 100);
  w.economy.settlements().find(home)->warehouse.food = 1000;
  CHECK(w.match.power_score(w.world, 0) == 23 + (1000 + 500) / 100);

  // -- one soldier, garrisoned; the settlement pass has it -----------------
  const ObjectId guard = w.soldier("Heavy", 0, 4000);
  REQUIRE(w.world.put_in_holder(guard, hall));
  w.economy.settlements().find(home)->holder.units.push_back(guard);
  CHECK(w.match.power_score(w.world, 0) == 23 + (1500 + kHeavy) / 100);

  // -- and one in the field; the *other* pass has that one -----------------
  const ObjectId scout = w.soldier("Heavy", 0, 4000);
  CHECK(w.match.power_score(w.world, 0) == 23 + (1500 + 2 * kHeavy) / 100);

  // **Neither pass sees it twice.** Walking the field soldier into the town
  // moves it from one pass to the other and leaves the total exactly where it
  // was -- which is the property that makes two passes safe at all.
  REQUIRE(w.world.put_in_holder(scout, hall));
  w.economy.settlements().find(home)->holder.units.push_back(scout);
  CHECK(w.match.power_score(w.world, 0) == 23 + (1500 + 2 * kHeavy) / 100);

  // Another player's soldier standing in my town counts for neither of us: the
  // mask both passes test is the *object's* owner, not the settlement's.
  const ObjectId intruder = w.soldier("Heavy", 1, 4000);
  REQUIRE(w.world.put_in_holder(intruder, hall));
  w.economy.settlements().find(home)->holder.units.push_back(intruder);
  CHECK(w.match.power_score(w.world, 0) == 23 + (1500 + 2 * kHeavy) / 100);
  CHECK(w.match.power_score(w.world, 1) == 20);
}

/// The rules inside the per-object valuation, each made to bite on its own.
TEST(match_power_census_values_a_ram_at_one_and_reads_health_not_maxhealth) {
  CensusWorld w;
  // Health, not maximum health: a soldier at a quarter strength is worth less.
  const ObjectId hurt = w.soldier("Heavy", 0, 1000);
  const std::int64_t hurt_value = expect_power(200, 100, kLevel, 4000, 1000);
  CHECK(hurt_value < kHeavy);
  CHECK(w.match.power_score(w.world, 0) == hurt_value / 100);
  (void)hurt;

  // **A ram is worth 1**, whatever it is carrying. Same profile as the
  // `Heavy` above, so the only difference is its ancestry -- and it is a
  // *grand*child of `RamUnit`, which a name comparison would miss.
  const std::int64_t before = w.match.power_score(w.world, 0);
  (void)w.soldier("BigRam", 0, 4000);
  CHECK(w.match.power_score(w.world, 0) == (hurt_value + 1) / 100);
  CHECK(w.match.power_score(w.world, 0) == before);  // 19,501 would have moved it

  // A building is not a unit and is valued at nothing -- `[obj+0x2c] & 0x400000`
  // is required by both passes. Given the `Heavy` class *and a combatant
  // record*, so neither the class nor a missing profile is what excludes it:
  // counted, it would be worth 210 on its own.
  (void)w.spawn_as(NativeClass::building, "Heavy", 0, 4000);
  CHECK(w.match.power_score(w.world, 0) == before);

  // Nor is an unspawned template, which is the other bit both passes test --
  // and the same object counts the moment the bit clears.
  const ObjectId templ = w.soldier("Heavy", 0, 4000);
  w.world.find(templ)->state.flags.unspawned = true;
  CHECK(w.match.power_score(w.world, 0) == before);
  w.world.find(templ)->state.flags.unspawned = false;
  CHECK(w.match.power_score(w.world, 0) == (hurt_value + 1 + kHeavy) / 100);
}

/// A wagon's load joins the warehouses' totals, split by `restype`.
TEST(match_power_census_counts_the_gold_and_food_on_the_road) {
  CensusWorld w;
  const ObjectId mule = w.soldier("Heavy", 0, 4000);
  CHECK(w.match.power_score(w.world, 0) == kHeavy / 100);

  // Gold rides at face value...
  w.world.find(mule)->state.cargo = 1000;
  w.world.find(mule)->state.cargo_resource = static_cast<std::int32_t>(Resource::gold);
  CHECK(w.match.power_score(w.world, 0) == (1000 + kHeavy) / 100);

  // ...and food at half, the same asymmetry the warehouse totals have.
  w.world.find(mule)->state.cargo_resource = static_cast<std::int32_t>(Resource::food);
  CHECK(w.match.power_score(w.world, 0) == (500 + kHeavy) / 100);

  // An empty wagon adds nothing, and `restype` alone is not cargo: the field is
  // not cleared when a load is spent, in either engine, so `amount` is what
  // says whether anything is there.
  w.world.find(mule)->state.cargo = 0;
  CHECK(w.match.power_score(w.world, 0) == kHeavy / 100);
}

/// The valuation's trailing `+ 1`, on the one object built to show it.
///
/// 0x00443a32's `lea eax, [edx + ecx + 1]` adds one to every object's value
/// after the divide by 1000, and on any realistic unit that one disappears
/// inside the census's own divide by 100. `Whelp` is picked so it does not:
/// its formula body is exactly 99, so the `+ 1` is the difference between a
/// power score of 0 and one of 1.
TEST(match_power_census_adds_one_to_every_objects_value) {
  CensusWorld w;
  CHECK(expect_power(1, 0, kLevel, 28000, 100) == 100);
  (void)w.soldier("Whelp", 0, 100);
  CHECK(w.match.power_score(w.world, 0) == 1);
}

/// `GetTeamOverallScore` averages `gold_used/1000 + power*rating/100` over the
/// same team `GetTeamMilitaryScore` averages over.
TEST(match_team_overall_score_is_the_military_team_over_a_different_term) {
  CensusWorld w;
  (void)setup_match(w.world, w.match, elimination_rules(), MatchOptions{});
  (void)w.soldier("Heavy", 0, 4000);
  const std::int32_t power = w.match.power_score(w.world, 0);
  CHECK(power == static_cast<std::int32_t>(kHeavy / 100));
  CHECK(power == 210);

  // A fresh player rates 10, so the whole term is `power * 10 / 100`.
  CHECK(military_rating(w.match.score(0)) == 10);
  const std::int32_t alone = w.match.team_overall_score(w.world, 0);
  CHECK(alone == 210 * 10 / 100);
  CHECK(alone == 21);

  // `gold_used` is the other half, and it is a thousandth. `Settlement::
  // GoldSpent` is its one writer here.
  w.match.record_gold_spent(0, 5000);
  CHECK(w.match.team_overall_score(w.world, 0) == 5 + 21);

  // The team is the same set, and one-directional alliance is not a team --
  // the property `team_military_score` is tested for, asserted again because
  // the two share the rule rather than the code.
  w.world.players().set(1, 0, Relation::allied, true);
  CHECK(w.match.team_overall_score(w.world, 0) == 5 + 21);
  w.world.players().set(0, 1, Relation::allied, true);
  // Player 1 owns nothing, so its power is 0 and its whole term is 0.
  CHECK(w.match.power_score(w.world, 1) == 0);
  CHECK(w.match.team_overall_score(w.world, 0) == (5 + 21) / 2);

  // Out of range answers 0 rather than faulting, as the military one does.
  CHECK(w.match.team_overall_score(w.world, kNoPlayer) == 0);

  // **The rating is a real factor rather than a constant**, and it is the one
  // this term shares with `GetTeamMilitaryScore`: damage taken halves it, and
  // the power term halves with it. Last, because it is not undoable -- the
  // counters only go up.
  w.match.record_damage(w.world, 15, 0, 10000);
  CHECK(military_rating(w.match.score(0)) == (0 + 0 + 1000) * 100 / (0 + 10000 + 10000));
  CHECK(military_rating(w.match.score(0)) == 5);
  CHECK(w.match.team_overall_score(w.world, 0) == (5 + 210 * 5 / 100) / 2);
}

/// `EvalGroup(name)` sums the census valuation over the named group's units:
/// a member that is not a unit adds nothing, a member that is gone is skipped,
/// and a name no group carries answers 0 without minting one.
TEST(match_eval_group_sums_the_power_of_the_groups_units) {
  CensusWorld w;
  const ObjectId a = w.soldier("Heavy", 0, 4000);
  const ObjectId b = w.soldier("Heavy", 1, 4000);
  const ObjectId tower = w.spawn_as(NativeClass::building, "Heavy", 0, 4000);
  const ObjectId gone = w.soldier("Heavy", 0, 4000);
  const std::int32_t group = w.world.group_index("Guard");
  CHECK(w.world.groups().add(group, a));
  CHECK(w.world.groups().add(group, tower));
  CHECK(w.world.groups().add(group, b));
  CHECK(w.world.groups().add(group, gone));
  w.world.despawn(gone);

  script::HostRegistry registry;
  (void)register_match_host(registry);
  HostContext context;
  context.world = &w.world;
  const std::uint32_t index = registry.find(script::CallKind::free_function, "EvalGroup", 1);
  REQUIRE(index != script::kUnresolvedHost);
  const auto eval = [&](const char* name) -> std::int32_t {
    std::vector<script::Value> args = {script::Value::string(name)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  // Two units of both owners, and neither the building nor the dead id.
  CHECK(eval("Guard") == 2 * kHeavy);
  CHECK(eval("Nonesuch") == 0);
  CHECK(w.world.groups().find("Nonesuch") < 0);
}

/// `Settlement::UnitsInHolderEval` sums the census valuation over the
/// garrison and nothing else.
TEST(match_units_in_holder_eval_sums_the_garrison) {
  CensusWorld w;
  ObjectId anchor = kNoObject;
  const SettlementId id = w.plant(0, SettlementKind::village, anchor);
  Settlement* s = w.economy.settlements().find(id);
  REQUIRE(s != nullptr);
  const ObjectId a = w.soldier("Heavy", 0, 4000);
  const ObjectId b = w.soldier("Heavy", 0, 4000);
  const ObjectId outside = w.soldier("Heavy", 0, 4000);
  REQUIRE(s->holder.add(a));
  REQUIRE(s->holder.add(b));
  (void)outside;

  script::HostRegistry registry;
  (void)register_economy_hosts(registry);
  HostContext context;
  context.world = &w.world;
  const std::uint32_t index =
      registry.find(script::CallKind::member, "UnitsInHolderEval", 0);
  REQUIRE(index != script::kUnresolvedHost);
  const auto eval = [&](ObjectId settlement_object) -> std::int32_t {
    std::vector<script::Value> args = {script::Value::object(kTypeSettlement, settlement_object)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  CHECK(eval(s->object) == 2 * kHeavy);
  CHECK(eval(9999) == 0);
}

/// **`Unit::AddBonus`'s record reaches the power census**, because the census
/// reads the object's cached numbers rather than its class's: `0x004439e6`
/// takes `[obj+0xc8]`, `[obj+0xc0]`, `[obj+0xdc]` and `[obj+0xe4]`, and the
/// first, third and fourth are exactly what the record adds to.
TEST(match_power_score_sees_a_stat_bonus) {
  CensusWorld w;
  const ObjectId heavy = w.soldier("Heavy", 0, 4000);
  const std::int64_t bare = expect_power(200, 100, kLevel, 4000, 4000);
  CHECK(object_power(w.world, &w.combat, *w.world.find(heavy)) == bare);

  StatBonus put;
  put.attack = 100;        // `[obj+0xdc]`
  put.armour_slash = 50;   // `[obj+0xe4]`
  put.max_health = 4000;   // `[obj+0xc8]`
  CHECK(w.combat.add_bonus(heavy, put));
  // Every term moves, and the health term moves by the *maximum* only: the
  // unit is still standing at 4,000.
  CHECK(object_power(w.world, &w.combat, *w.world.find(heavy)) ==
        expect_power(300, 150, kLevel, 8000, 4000));

  // The pierce addend is not in this formula at all -- the original reads
  // `[obj+0xe4]`, the slash channel, and nothing else.
  StatBonus pierce;
  pierce.armour_pierce = 10000;
  CHECK(w.combat.add_bonus(heavy, pierce));
  CHECK(object_power(w.world, &w.combat, *w.world.find(heavy)) ==
        expect_power(300, 150, kLevel, 8000, 4000));
}

/// The end-of-match report's counters, as `gbr.exe` books them: a `Military`
/// unit produced for every owner it takes (0x005dc3f0), the most standing at
/// once, one lost for every one that leaves the world (0x005db4c0), one
/// killed beside every kill the combat tally counts (0x00511905); gold and
/// food spent with a charge and back with a refund; gold captured moving with
/// a settlement, produced with a tick, converted with `GoldConverted`. None
/// of it is hashed, all of it is saved.
TEST(match_report_counters_follow_the_units_the_gold_and_the_food) {
  CensusWorld w;
  const auto hash_of = [&] {
    std::uint64_t value = 0;
    w.match.hash(value);
    return value;
  };
  // Two soldiers standing before the match starts are the census `start`
  // takes -- produced, and two at once -- and a deer is not a soldier.
  const ObjectId first = w.soldier("Legionary", 0, 4000);
  const ObjectId second = w.soldier("Legionary", 0, 4000);
  const ObjectId deer = w.soldier("Heavy", 0, 4000);
  const std::uint64_t before = hash_of();
  w.world.start();
  CHECK(w.match.score(0).units_produced == 2);
  CHECK(w.match.score(0).units_max == 2);
  CHECK(w.match.score(0).units_lost == 0);
  CHECK(hash_of() == before);  // the report is not hashed

  // From here the world tells the match as it goes: a third soldier raises
  // the maximum, a soldier changing sides is produced for its new owner and
  // not lost by its old, and one leaving the world is lost by its owner.
  const ObjectId third = w.soldier("Legionary", 1, 4000);
  CHECK(w.match.score(1).units_produced == 1 && w.match.score(1).units_max == 1);
  REQUIRE(w.world.set_owner(third, 0));
  CHECK(w.match.score(0).units_produced == 3 && w.match.score(0).units_max == 3);
  CHECK(w.match.score(1).units_lost == 0 && w.match.score(1).units_max == 1);
  REQUIRE(w.world.set_owner(third, 0));  // the same owner again books nothing
  CHECK(w.match.score(0).units_produced == 3);
  REQUIRE(w.world.despawn(first));
  CHECK(w.match.score(0).units_lost == 1);
  CHECK(w.match.score(0).units_max == 3);  // the maximum stands
  // Standing: 3 produced - 1 lost = 2; one more makes 3, not past the maximum.
  const ObjectId fourth = w.soldier("Legionary", 0, 4000);
  CHECK(w.match.score(0).units_produced == 4 && w.match.score(0).units_max == 3);
  REQUIRE(w.world.despawn(deer));
  CHECK(w.match.score(0).units_lost == 1);  // not a soldier
  (void)second;
  (void)fourth;

  // A counted kill is one killed beside its health; an allied blow is neither.
  w.match.record_kill(w.world, 1, 0, 4000);
  CHECK(w.match.score(1).units_killed == 1 && w.match.score(1).kill_healths == 4000);
  w.world.players().set(0, 1, Relation::ceasefire, true);
  w.world.players().set(1, 0, Relation::ceasefire, true);
  w.match.record_kill(w.world, 1, 0, 4000);
  CHECK(w.match.score(1).units_killed == 1);

  // Gold and food spent, and back. `gold` is one of the five hashed counters
  // (a script reads it through the team scores); `food` is not.
  const std::uint64_t tallied = hash_of();
  w.match.record_gold_spent(0, 60);
  w.match.record_food_spent(0, 20);
  w.match.record_gold_spent(0, -60);
  w.match.record_food_spent(0, -20);
  w.match.record_food_spent(0, 35);
  CHECK(w.match.score(0).gold == 0 && w.match.score(0).food == 35);
  CHECK(hash_of() == tallied);
  // Gold produced, converted, and moving with a settlement.
  w.match.record_gold_produced(0, 12);
  w.match.record_gold_produced(0, 0);
  w.match.record_gold_converted(0, 7);
  w.match.record_gold_captured(1, -500);
  w.match.record_gold_captured(0, 500);
  CHECK(w.match.score(0).gold_townhall == 12 && w.match.score(0).gold_outpost == 7);
  CHECK(w.match.score(0).gold_captured == 500 && w.match.score(1).gold_captured == -500);
  w.match.record_gold_produced(16, 5);  // out of range: ignored
  CHECK(hash_of() == tallied);

  // All eight survive a save.
  std::vector<std::byte> saved;
  w.match.serialize(saved);
  MatchSystem restored;
  REQUIRE(restored.deserialize(saved).ok());
  CHECK(restored.score(0).food == 35);
  CHECK(restored.score(0).units_produced == 4);
  CHECK(restored.score(1).units_killed == 1);
  CHECK(restored.score(0).units_lost == 1);
  CHECK(restored.score(0).units_max == 3);
  CHECK(restored.score(0).gold_captured == 500);
  CHECK(restored.score(0).gold_townhall == 12);
  CHECK(restored.score(0).gold_outpost == 7);
}
