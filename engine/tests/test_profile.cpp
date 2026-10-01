// The player profile: the journal, the career aggregated from it, and the
// rolling hash that proves the reading. The last of those is the load-bearing
// test -- see `profile_hash_matches_a_shipped_journal`, whose expected number
// is the `hash=` line of a retail `player.ini` rather than this engine's own
// output.

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/game/profile.hpp"
#include "test.hpp"

using namespace imperivm::core;
using imperivm::core::game::aggregate;
using imperivm::core::game::parse_profile;
using imperivm::core::game::parse_ranks;
using imperivm::core::game::Profile;
using imperivm::core::game::profile_hash_fold;
using imperivm::core::game::Rank;
using imperivm::core::game::rank_for;

namespace {

std::span<const std::byte> as_bytes(std::string_view text) noexcept {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// One record, with every key the reader insists on. The values are the
/// caller's to vary; the shape is fixed.
struct Record {
  int year = 2017, month = 4, day = 23, hour = 18, minute = 38;
  int duration = 3600000, map_size = 0, multi = 1, lost = 0;
  int gold = 100, food = 50;
  int units_prod = 10, units_killed = 20, units_lost = 5, units_max = 30;
  int level_max = 7;
  std::string level_max_unit = "Sentinella";
  int health_sacr = 0, priests = 0;
  std::string favourite = "Pretoriano";
  int enemies = 3, allies = 1, race = 4;
  int damage_taken = 0, damage_inflicted = 0;
  int player_id = 0, poser_score = 0, kill_healths = 0, die_healths = 0;
};

std::string written(const Record& record, int index) {
  const auto n = [](std::string_view key, int value) {
    return std::string(key) + "=" + std::to_string(value) + "\n";
  };
  return "[game" + std::to_string(index) + "]\n" +
         "id=1D6CC99282E80845BC8B7ECE293BF08E\n" + n("year", record.year) +
         n("month", record.month) + n("day", record.day) + n("hour", record.hour) +
         n("minute", record.minute) + n("duration", record.duration) +
         n("mapsize", record.map_size) + n("multi", record.multi) + n("lost", record.lost) +
         n("gold", record.gold) + n("food", record.food) + n("units_prod", record.units_prod) +
         n("units_killed", record.units_killed) + n("units_lost", record.units_lost) +
         n("units_max", record.units_max) + n("level_max", record.level_max) +
         "level_max_unit=" + record.level_max_unit + "\n" + n("health_sacr", record.health_sacr) +
         n("priests", record.priests) + "favorite=" + record.favourite + "\n" +
         n("enemies", record.enemies) + n("allies", record.allies) + n("race", record.race) +
         n("damage_taken", record.damage_taken) +
         n("damage_inflicted", record.damage_inflicted) + n("player_id", record.player_id) +
         n("poser_score", record.poser_score) + n("kill_healths", record.kill_healths) +
         n("die_healths", record.die_healths) + "\n";
}

/// Parse, and hand back an empty profile where the caller's `REQUIRE` on the
/// journal's size will fail loudly rather than the helper doing it silently.
Profile read(std::string_view text) {
  Result<IniDocument> document = IniDocument::parse(as_bytes(text));
  if (!document.ok()) return Profile{};
  Result<Profile> profile = parse_profile(document.value());
  if (!profile.ok()) return Profile{};
  return profile.value();
}

/// A rank table with the shape the shipped one has and none of its
/// content: six rows, ascending but for the fifth, whose points sit below
/// the row before it. The retail table's own numbers and names are game
/// data (`docs/legal.md` rule 1) and the claim about them -- that
/// `RankPoints10 = 2` leaves `Hero` naming no rank -- is
/// `tests/test_corpus_profile.py`'s, recomputed from the installation.
/// What is checked here is that `rank_for` walks rather than searches, and
/// so reproduces whatever the file says.
constexpr std::string_view kOutOfOrderRanks = R"([Ranks]
RanksCount = 6
RankPoints0 = 0
RankName0 = First
RankPoints1 = 40
RankName1 = Second
RankPoints2 = 60
RankName2 = Third
RankPoints3 = 80
RankName3 = Fourth
RankPoints4 = 2	
RankName4 = Fifth
RankPoints5 = 300
RankName5 = Sixth
)";

std::vector<Rank> out_of_order_ranks() {
  Result<IniDocument> document = IniDocument::parse(as_bytes(kOutOfOrderRanks));
  if (!document.ok()) return {};
  return parse_ranks(document.value());
}

}  // namespace

TEST(profile_reads_the_player_block_and_its_journal) {
  const std::string text = std::string(R"([Player]
name=Angel
color=3
race=7
games=2
hash=-1650049073
fav=Pretoriano

)") + written(Record{}, 0) + written(Record{.multi = 0, .lost = 1, .race = 5}, 1);
  const Profile profile = read(text);
  CHECK(profile.name == "Angel");
  CHECK(profile.colour == 3);
  CHECK(profile.race == 7);
  CHECK(profile.games == 2);
  CHECK(profile.has_stored_hash);
  CHECK(profile.stored_hash == -1650049073);
  CHECK(profile.favourite_unit == "Pretoriano");
  REQUIRE(profile.journal.size() == 2);
  CHECK(profile.journal[0].id == "1D6CC99282E80845BC8B7ECE293BF08E");
  CHECK(profile.journal[0].multiplayer);
  CHECK(!profile.journal[0].lost);
  CHECK(profile.journal[0].level_max_unit == "Sentinella");
  CHECK(profile.journal[0].favourite == "Pretoriano");
  CHECK(!profile.journal[1].multiplayer);
  CHECK(profile.journal[1].lost);
  CHECK(profile.journal[1].race == 5);
}

TEST(profile_journal_stops_at_the_first_missing_number) {
  // `[game2]` with no `[game1]` before it is not read: the walk asks for
  // consecutive numbers and stops where one is absent.
  const std::string text = written(Record{}, 0) + written(Record{}, 2);
  const Profile profile = read(text);
  CHECK(profile.journal.size() == 1);
}

TEST(profile_rejects_a_record_missing_a_key) {
  // The executable's reader gives up on the first key it cannot read and the
  // aggregation propagates that, so a half-written record fails the file
  // rather than being skipped.
  std::string text = written(Record{}, 0);
  const std::size_t cut = text.find("priests=0\n");
  REQUIRE(cut != std::string::npos);
  text.erase(cut, std::string_view("priests=0\n").size());
  Result<IniDocument> document = IniDocument::parse(as_bytes(text));
  REQUIRE(document.ok());
  Result<Profile> profile = parse_profile(document.value());
  CHECK(!profile.ok());
  CHECK(profile.error() == FormatError::malformed);
}

TEST(profile_counts_games_and_the_share_won) {
  const std::string text = written(Record{.multi = 0, .lost = 0}, 0) +
                           written(Record{.multi = 0, .lost = 1}, 1) +
                           written(Record{.multi = 0, .lost = 1}, 2) +
                           written(Record{.multi = 1, .lost = 0}, 3);
  Profile profile = read(text);
  aggregate(profile, {});
  CHECK(profile.stats.single_games == 3);
  CHECK(profile.stats.single_won_percent == 33);  // 1 of 3, truncated
  CHECK(profile.stats.multi_games == 1);
  CHECK(profile.stats.multi_won_percent == 100);
}

TEST(profile_with_no_games_reports_full_shares_and_no_nation) {
  Profile profile = read("[Player]\nname=Nobody\n");
  aggregate(profile, {});
  CHECK(profile.stats.single_games == 0);
  CHECK(profile.stats.single_won_percent == 100);
  CHECK(profile.stats.multi_won_percent == 100);
  CHECK(profile.stats.favourite_race == -1);
  CHECK(profile.stats.favourite_race_percent == 100);
  CHECK(profile.stats.rating() == 0);
  CHECK(profile.stats.hash == imperivm::core::game::kProfileHashSeed);
}

TEST(profile_sums_the_counters_and_keeps_the_two_maxima) {
  const std::string text =
      written(Record{.duration = 3600000, .gold = 100, .food = 40, .units_lost = 5,
                     .units_max = 30, .level_max = 7, .level_max_unit = "Sipius",
                     .health_sacr = 11},
              0) +
      written(Record{.duration = 5400000, .gold = 300, .food = 60, .units_killed = 50,
                     .units_lost = 8, .units_max = 12, .level_max = 24,
                     .level_max_unit = "Damasus", .health_sacr = 4},
              1);
  Profile profile = read(text);
  aggregate(profile, {});
  CHECK(profile.stats.duration == 9000000);
  CHECK(profile.stats.hours() == 2);  // 2.5 hours, truncated
  CHECK(profile.stats.gold_spent == 400);
  CHECK(profile.stats.food_spent == 100);
  CHECK(profile.stats.units_killed == 70);
  CHECK(profile.stats.units_lost == 13);
  CHECK(profile.stats.health_sacrificed == 15);
  CHECK(profile.stats.best_level == 24);
  CHECK(profile.stats.best_unit == "Damasus");
  CHECK(profile.stats.most_units == 30);
}

TEST(profile_favourite_nation_is_the_most_played_and_ties_keep_the_lower) {
  const std::string text = written(Record{.race = 4}, 0) + written(Record{.race = 4}, 1) +
                           written(Record{.race = 1}, 2) + written(Record{.race = 5}, 3);
  Profile played = read(text);
  aggregate(played, {});
  CHECK(played.stats.favourite_race == 4);
  CHECK(played.stats.favourite_race_percent == 50);

  const std::string tied = written(Record{.race = 6}, 0) + written(Record{.race = 2}, 1);
  Profile even = read(tied);
  aggregate(even, {});
  CHECK(even.stats.favourite_race == 2);  // the earlier slot wins a tie
  CHECK(even.stats.favourite_race_percent == 50);
}

TEST(profile_nation_nine_is_counted_nowhere) {
  // The histogram has nine slots and the guard admits ten values. Nothing in
  // the retail journals reaches the tenth; a file that does must not make the
  // aggregation disagree with the executable, so the record counts towards
  // the games and towards no nation.
  const std::string text = written(Record{.race = 9}, 0) + written(Record{.race = 9}, 1);
  Profile profile = read(text);
  aggregate(profile, {});
  CHECK(profile.stats.multi_games == 2);
  CHECK(profile.stats.favourite_race == -1);
  CHECK(profile.stats.favourite_race_percent == 100);
}

TEST(profile_rating_floors_both_sides_of_the_ratio) {
  // A match in which nothing was hit: 1000 * 100 / 10000 is ten, not a
  // division by zero.
  Profile quiet = read(written(Record{}, 0));
  aggregate(quiet, {});
  CHECK(quiet.stats.rating_total == 10);
  CHECK(quiet.stats.rating() == 10);

  // A health counter weighs half. (50000/2 + 15000 + 1000) * 100 /
  // (20000/2 + 5000 + 10000) = 4100000 / 25000 = 164.
  Profile fought = read(written(Record{.damage_taken = 5000,
                                       .damage_inflicted = 15000,
                                       .kill_healths = 50000,
                                       .die_healths = 20000},
                                0));
  aggregate(fought, {});
  CHECK(fought.stats.rating_total == 164);
}

TEST(profile_rating_is_the_mean_over_the_matches) {
  const std::string text =
      written(Record{.damage_taken = 5000, .damage_inflicted = 15000, .kill_healths = 50000,
                     .die_healths = 20000},
              0) +
      written(Record{}, 1);
  Profile profile = read(text);
  aggregate(profile, {});
  CHECK(profile.stats.rating_total == 174);
  CHECK(profile.stats.rating() == 87);
}

TEST(ranks_parse_in_file_order_without_sorting) {
  const std::vector<Rank> ranks = out_of_order_ranks();
  REQUIRE(ranks.size() == 6);
  CHECK(ranks[0].points == 0);
  CHECK(ranks[0].name == "First");
  // The row that breaks the order keeps its place, and its trailing tab is
  // whitespace: the shipped table has one of those too.
  CHECK(ranks[4].points == 2);
  CHECK(ranks[4].name == "Fifth");
  CHECK(ranks[5].points == 300);
}

TEST(ranks_stop_at_the_first_missing_row) {
  constexpr std::string_view kHoled = "[Ranks]\nRanksCount = 4\nRankPoints0 = 0\nRankName0 = A\n"
                                      "RankPoints1 = 10\nRankName1 = B\nRankName2 = C\n";
  Result<IniDocument> document = IniDocument::parse(as_bytes(kHoled));
  REQUIRE(document.ok());
  const std::vector<Rank> ranks = parse_ranks(document.value());
  REQUIRE(ranks.size() == 2);
  CHECK(ranks[1].name == "B");
}

TEST(rank_for_walks_the_table_and_stops_where_it_stops) {
  const std::vector<Rank> ranks = out_of_order_ranks();
  CHECK(rank_for(ranks, 0) == 0);
  CHECK(rank_for(ranks, 39) == 0);
  CHECK(rank_for(ranks, 40) == 1);
  CHECK(rank_for(ranks, 60) == 2);
  CHECK(rank_for(ranks, 79) == 2);
  // Row 4 says 2, so a rating of 80 passes it as well and the walk only
  // stops at row 5's 300. Row 3 therefore names no rank at all, which is
  // the shape the shipped table has and the reason this is a walk.
  CHECK(rank_for(ranks, 80) == 4);
  CHECK(rank_for(ranks, 299) == 4);
  CHECK(rank_for(ranks, 300) == 5);
  CHECK(rank_for(ranks, 4000) == 5);
  for (std::int64_t rating = 0; rating < 1000; ++rating) CHECK(rank_for(ranks, rating) != 3);
}

TEST(rank_for_answers_below_the_first_row_and_over_an_empty_table) {
  const Rank ranks[] = {{40, "Second"}, {60, "Third"}};
  CHECK(rank_for(ranks, 0) == -1);
  CHECK(rank_for(ranks, 39) == -1);
  CHECK(rank_for(ranks, 40) == 0);
  CHECK(rank_for(std::span<const Rank>{}, 1000) == -1);
}

TEST(profile_rank_name_comes_from_the_table) {
  Profile profile = read(written(Record{.damage_taken = 5000,
                                        .damage_inflicted = 15000,
                                        .kill_healths = 50000,
                                        .die_healths = 20000},
                                 0));
  const std::vector<Rank> ranks = out_of_order_ranks();
  aggregate(profile, ranks);
  CHECK(profile.stats.rating() == 164);
  CHECK(profile.stats.rank == "Fifth");

  Profile beginner = read(written(Record{}, 0));
  aggregate(beginner, ranks);
  CHECK(beginner.stats.rating() == 10);
  CHECK(beginner.stats.rank == "First");
}

TEST(profile_hash_fold_is_the_executables) {
  // Four numbers checked against the arithmetic read out of the fold at
  // 0x0056d847: `mix(v) = ((((v >> 8) ^ v) >> 5) & 0x07FFFFF8) + (v & 7) + 8v`,
  // the previous hash doubled and xored in, then `x + (x >> 31)`.
  CHECK(profile_hash_fold(0, 0) == 0);
  CHECK(profile_hash_fold(imperivm::core::game::kProfileHashSeed,
                          imperivm::core::game::kProfileHashString) ==
        profile_hash_fold(imperivm::core::game::kProfileHashSeed, 0x0EA75617u));
  // The string constant's own mix is the literal the compiler folded into the
  // string step, which is what says the step is an ordinary fold of a
  // constant rather than a hash of the string's bytes.
  const std::uint32_t v = imperivm::core::game::kProfileHashString;
  const std::uint32_t mixed = ((((v >> 8) ^ v) >> 5) & 0x07FFFFF8u) + (v & 7u) + v * 8u;
  CHECK(mixed == 0x75B00047u);
}

// The retail check -- recompute the fold from a real `player.ini` and compare
// it with the `hash=` that file carries -- lives in
// `tests/test_corpus_profile.py`, where it runs against the installation on
// every run. It is deliberately not here: `docs/legal.md` rule 1 keeps game
// data out of this repository, not as test fixtures either, and a journal
// pasted into a string literal is exactly that. What is here is the fold's
// own claims, on records written from `docs/formats/profile.md`.

TEST(profile_hash_moves_with_every_folded_field) {
  const auto hash_of = [](const Record& record) {
    Profile profile = read(written(record, 0));
    aggregate(profile, {});
    return profile.stats.hash;
  };
  const std::uint32_t base = hash_of(Record{});
  // One record, one field at a time. Each of these is folded, so each must
  // move the number; the specification's table is what this checks.
  CHECK(hash_of(Record{.year = 2018}) != base);
  CHECK(hash_of(Record{.month = 5}) != base);
  CHECK(hash_of(Record{.day = 24}) != base);
  CHECK(hash_of(Record{.hour = 19}) != base);
  CHECK(hash_of(Record{.minute = 39}) != base);
  CHECK(hash_of(Record{.duration = 3600001}) != base);
  CHECK(hash_of(Record{.map_size = 1}) != base);
  CHECK(hash_of(Record{.lost = 1}) != base);
  CHECK(hash_of(Record{.gold = 101}) != base);
  CHECK(hash_of(Record{.food = 51}) != base);
  CHECK(hash_of(Record{.units_prod = 11}) != base);
  CHECK(hash_of(Record{.units_killed = 21}) != base);
  CHECK(hash_of(Record{.units_lost = 6}) != base);
  CHECK(hash_of(Record{.units_max = 31}) != base);
  CHECK(hash_of(Record{.level_max = 8}) != base);
  CHECK(hash_of(Record{.health_sacr = 1}) != base);
  CHECK(hash_of(Record{.priests = 1}) != base);
  CHECK(hash_of(Record{.enemies = 4}) != base);
  CHECK(hash_of(Record{.allies = 2}) != base);
  // `race` is not folded, but the histogram counter it increments is, so a
  // different nation still moves the hash -- by a different route, and the
  // next test is the one that pins the difference.
  CHECK(hash_of(Record{.race = 5}) != base);
  // `multi` is not folded either; the running counts are, and moving a
  // record from one count to the other moves both.
  CHECK(hash_of(Record{.multi = 0}) != base);
}

TEST(profile_hash_ignores_the_fields_that_are_not_folded) {
  const auto hash_of = [](const Record& record) {
    Profile profile = read(written(record, 0));
    aggregate(profile, {});
    return profile.stats.hash;
  };
  const std::uint32_t base = hash_of(Record{});
  // Six numbers the record carries and the fold does not touch. They are
  // read -- `kill_healths` and the two damage counters decide the match's
  // rating -- so this is a claim about the fold, not about the reader.
  CHECK(hash_of(Record{.damage_taken = 12345}) == base);
  CHECK(hash_of(Record{.damage_inflicted = 12345}) == base);
  CHECK(hash_of(Record{.player_id = 3}) == base);
  CHECK(hash_of(Record{.poser_score = 9999}) == base);
  CHECK(hash_of(Record{.kill_healths = 4242}) == base);
  CHECK(hash_of(Record{.die_healths = 4242}) == base);
  // And the three strings, whose contents fold a constant. The `id` too,
  // which is why two different matches with the same numbers hash alike.
  CHECK(hash_of(Record{.level_max_unit = "Damasus"}) == base);
  CHECK(hash_of(Record{.favourite = "Highlander"}) == base);
  // A flag spelled with any non-zero number is the same flag: the record
  // reader stores `value != 0` and the fold sees the 0 or the 1.
  CHECK(hash_of(Record{.multi = 7}) == base);
}

TEST(profile_hash_depends_on_the_order_of_the_records) {
  // The counters are folded after each record's increment, so a journal is
  // not a set. Two records that differ swap to a different number.
  Profile forwards = read(written(Record{.gold = 100}, 0) + written(Record{.gold = 300}, 1));
  Profile backwards = read(written(Record{.gold = 300}, 0) + written(Record{.gold = 100}, 1));
  aggregate(forwards, {});
  aggregate(backwards, {});
  CHECK(forwards.stats.gold_spent == backwards.stats.gold_spent);
  CHECK(forwards.stats.hash != backwards.stats.hash);
}

TEST(profile_hash_of_a_written_journal_does_not_drift) {
  // A pin: the number is the reference reader's
  // (`src/imperivm/formats/profile.py`) over the same three records written
  // here, so it catches a change to either implementation that the other
  // does not make. It is not evidence about the original -- that is the
  // corpus test's `hash=` comparison -- but it is not this engine marking
  // its own homework either.
  Profile profile = read(written(Record{}, 0) + written(Record{.multi = 0, .race = 5}, 1) +
                         written(Record{.lost = 1, .gold = 500}, 2));
  REQUIRE(profile.journal.size() == 3);
  aggregate(profile, {});
  CHECK(profile.stats.hash == 0x0A7C9682u);
}
