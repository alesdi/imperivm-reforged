#include "imperivm/core/game/profile.hpp"

#include <algorithm>
#include <array>

namespace imperivm::core::game {
namespace {

/// `game%d`, built the way the walk asks for it.
std::string game_section(std::int32_t index) { return "game" + std::to_string(index); }

/// One key, required. `false` when it is absent or is not wholly a number,
/// which is the reader's own standard: `value_int` answers its fallback for a
/// half-numeric value and a record must not be allowed to read one half-way.
bool need_int(const IniDocument& document, SectionIndex section, std::string_view key,
              std::int32_t& out) {
  const std::string_view text = document.value(section, key);
  if (text.empty()) return false;
  return parse_int(text, out);
}

/// One key whose value is text. Present with an empty value is still present:
/// the shipped `[Player] victorytreshold` is empty and nothing rejects it.
bool need_text(const IniDocument& document, SectionIndex section, std::string_view key,
               std::string& out) {
  for (const IniEntry& entry : document.entries_of(section)) {
    if (!entry.has_key) continue;
    if (entry.key.size() != key.size()) continue;
    bool same = true;
    for (std::size_t i = 0; i < key.size() && same; ++i) {
      const char a = entry.key[i];
      const char b = key[i];
      same = (a == b) || (a | 0x20) == (b | 0x20);
    }
    if (!same) continue;
    out.assign(entry.value);
    return true;
  }
  return false;
}

}  // namespace

std::uint32_t profile_hash_fold(std::uint32_t hash, std::uint32_t value) noexcept {
  const std::uint32_t mixed =
      ((((value >> 8) ^ value) >> 5) & 0x07FFFFF8u) + (value & 7u) + value * 8u;
  const std::uint32_t folded = value + (mixed ^ (hash * 2u));
  return folded + (folded >> 31);
}

std::int64_t ProfileStats::rating() const noexcept {
  const std::uint32_t matches = single_games + multi_games;
  return rating_total / (matches < 1 ? 1 : static_cast<std::int64_t>(matches));
}

std::vector<Rank> parse_ranks(const IniDocument& document) {
  std::vector<Rank> ranks;
  const SectionIndex section = document.section("Ranks");
  if (section == kNoSection) return ranks;
  const std::int32_t count = document.value_int(section, "RanksCount", 0);
  for (std::int32_t index = 0; index < count; ++index) {
    Rank rank;
    // The executable formats both keys and stops at the first `RankPoints`
    // that will not read; a missing name leaves the rank nameless rather than
    // ending the table, because the name is only ever displayed.
    if (!need_int(document, section, "RankPoints" + std::to_string(index), rank.points)) break;
    (void)need_text(document, section, "RankName" + std::to_string(index), rank.name);
    ranks.push_back(std::move(rank));
  }
  return ranks;
}

std::int32_t rank_for(std::span<const Rank> ranks, std::int64_t rating) noexcept {
  std::size_t reached = 0;
  while (reached < ranks.size() && ranks[reached].points <= rating) ++reached;
  return static_cast<std::int32_t>(reached) - 1;
}

Result<Profile> parse_profile(const IniDocument& document) {
  Profile profile;
  if (const SectionIndex player = document.section("Player"); player != kNoSection) {
    (void)need_text(document, player, "name", profile.name);
    (void)need_text(document, player, "fav", profile.favourite_unit);
    profile.colour = document.value_int(player, "color", 0);
    profile.race = document.value_int(player, "race", 0);
    profile.games = document.value_int(player, "games", 0);
    profile.has_stored_hash = need_int(document, player, "hash", profile.stored_hash);
  }
  for (std::int32_t index = 0;; ++index) {
    const SectionIndex section = document.section(game_section(index));
    if (section == kNoSection) break;
    ProfileGame game;
    std::int32_t scratch = 0;
    const auto u16 = [&](std::string_view key, std::uint16_t& out) {
      if (!need_int(document, section, key, scratch)) return false;
      out = static_cast<std::uint16_t>(scratch);
      return true;
    };
    const auto flag = [&](std::string_view key, bool& out) {
      if (!need_int(document, section, key, scratch)) return false;
      out = scratch != 0;
      return true;
    };
    const bool complete =
        need_text(document, section, "id", game.id) && u16("year", game.year) &&
        u16("month", game.month) && u16("day", game.day) && u16("hour", game.hour) &&
        u16("minute", game.minute) && need_int(document, section, "duration", game.duration) &&
        need_int(document, section, "mapsize", game.map_size) && flag("multi", game.multiplayer) &&
        flag("lost", game.lost) && need_int(document, section, "gold", game.gold) &&
        need_int(document, section, "food", game.food) &&
        need_int(document, section, "units_prod", game.units_produced) &&
        need_int(document, section, "units_killed", game.units_killed) &&
        need_int(document, section, "units_lost", game.units_lost) &&
        need_int(document, section, "units_max", game.units_max) &&
        need_int(document, section, "level_max", game.level_max) &&
        need_text(document, section, "level_max_unit", game.level_max_unit) &&
        need_int(document, section, "health_sacr", game.health_sacrificed) &&
        need_int(document, section, "priests", game.priests) &&
        need_text(document, section, "favorite", game.favourite) &&
        need_int(document, section, "enemies", game.enemies) &&
        need_int(document, section, "allies", game.allies) &&
        need_int(document, section, "race", game.race) &&
        need_int(document, section, "damage_taken", game.damage_taken) &&
        need_int(document, section, "damage_inflicted", game.damage_inflicted) &&
        need_int(document, section, "player_id", game.player_id) &&
        need_int(document, section, "poser_score", game.poser_score) &&
        need_int(document, section, "kill_healths", game.kill_healths) &&
        need_int(document, section, "die_healths", game.die_healths);
    if (!complete) return FormatError::malformed;
    profile.journal.push_back(std::move(game));
  }
  return profile;
}

void aggregate(Profile& profile, std::span<const Rank> ranks) {
  ProfileStats stats;
  stats.hash = kProfileHashSeed;
  std::uint32_t single_won = 0;
  std::uint32_t multi_won = 0;
  // Nine counters, and a tenth slot nothing ever reads. The executable admits
  // a `race` of 0 to 9 into a nine-element array and then sums and ranks only
  // the first nine, so a record claiming nation 9 lands in a local that is not
  // part of the histogram. No shipped record does -- the nations are 0 to 7
  // with 8 for random -- so the slot is kept here to hold the same write
  // rather than to change the answer.
  std::array<std::uint32_t, 10> nations{};
  const auto fold = [&](std::uint32_t value) { stats.hash = profile_hash_fold(stats.hash, value); };

  for (const ProfileGame& game : profile.journal) {
    if (game.multiplayer) {
      if (!game.lost) ++multi_won;
      ++stats.multi_games;
    } else {
      if (!game.lost) ++single_won;
      ++stats.single_games;
    }
    fold(stats.multi_games);
    fold(stats.single_games);
    stats.duration += game.duration;
    fold(static_cast<std::uint32_t>(game.duration));
    if (game.race >= 0 && game.race <= 9) ++nations[static_cast<std::size_t>(game.race)];
    for (std::size_t nation = 0; nation < 9; ++nation) fold(nations[nation]);

    stats.gold_spent += game.gold;
    fold(static_cast<std::uint32_t>(game.gold));
    stats.food_spent += game.food;
    fold(static_cast<std::uint32_t>(game.food));
    stats.units_killed += game.units_killed;
    fold(static_cast<std::uint32_t>(game.units_killed));
    stats.units_lost += game.units_lost;
    fold(static_cast<std::uint32_t>(game.units_lost));
    stats.health_sacrificed += game.health_sacrificed;
    fold(static_cast<std::uint32_t>(game.health_sacrificed));
    fold(static_cast<std::uint32_t>(game.units_max));
    fold(game.lost ? 1u : 0u);
    fold(static_cast<std::uint32_t>(game.allies));
    fold(game.year);
    fold(game.month);
    fold(game.day);
    fold(game.hour);
    fold(game.minute);
    fold(static_cast<std::uint32_t>(game.enemies));
    fold(static_cast<std::uint32_t>(game.level_max));
    fold(kProfileHashString);  // level_max_unit
    fold(static_cast<std::uint32_t>(game.map_size));
    fold(static_cast<std::uint32_t>(game.priests));
    fold(static_cast<std::uint32_t>(game.units_produced));
    fold(kProfileHashString);  // favorite
    fold(kProfileHashString);  // id

    // The match's own score, and the two rules it is built on: a health
    // counter counts half, and both sides of the ratio carry a floor -- a
    // thousand on the numerator, ten thousand on the denominator -- so a
    // match in which nothing happened scores ten rather than dividing by
    // zero. The arithmetic is unsigned and 32-bit, and the sum is not.
    const std::uint32_t scored =
        (static_cast<std::uint32_t>(game.kill_healths) / 2u +
         static_cast<std::uint32_t>(game.damage_inflicted) + 1000u) * 100u;
    const std::uint32_t against = static_cast<std::uint32_t>(game.die_healths) / 2u +
                                  static_cast<std::uint32_t>(game.damage_taken) + 10000u;
    stats.rating_total += static_cast<std::int32_t>(scored / against);

    // The two maxima are compared differently and both readings are kept:
    // the level's test is unsigned (0x0056dfad) and the unit count's is
    // signed (0x0056dfd5). They agree on everything a match can produce --
    // neither counter goes negative -- and disagree only on a hand-edited
    // file, where a negative level would win and a negative count would not.
    if (static_cast<std::uint32_t>(game.level_max) > static_cast<std::uint32_t>(stats.best_level)) {
      stats.best_level = game.level_max;
      stats.best_unit = game.level_max_unit;
    }
    if (game.units_max > stats.most_units) stats.most_units = game.units_max;
  }

  stats.single_won_percent =
      stats.single_games == 0 ? 100u : single_won * 100u / stats.single_games;
  stats.multi_won_percent = stats.multi_games == 0 ? 100u : multi_won * 100u / stats.multi_games;

  std::uint32_t played = 0;
  for (std::size_t nation = 0; nation < 9; ++nation) played += nations[nation];
  if (played == 0) {
    stats.favourite_race = -1;
    stats.favourite_race_percent = 100;
  } else {
    std::size_t best = 0;
    for (std::size_t nation = 1; nation < 9; ++nation) {
      if (nations[best] < nations[nation]) best = nation;
    }
    stats.favourite_race = static_cast<std::int32_t>(best);
    stats.favourite_race_percent = nations[best] * 100u / played;
  }

  if (const std::int32_t rank = rank_for(ranks, stats.rating()); rank >= 0) {
    stats.rank = ranks[static_cast<std::size_t>(rank)].name;
  }
  profile.stats = std::move(stats);
}

}  // namespace imperivm::core::game
