#pragma once

/// The player profile: `Profiles/<name>/player.ini`, and the twelve lines the
/// *Change player* screen computes from it.
///
/// A profile is a directory under `Profiles/`. `PROFILE.INI` lists them in one
/// pane and shows the selected one's career in the other; the career is not
/// stored, it is **aggregated on every open** from a journal the executable
/// appends to after every match:
///
/// ```ini
/// [Player]
/// name=Angel
/// race=7
/// games=57
/// hash=-1650049073
/// fav=Pretoriano
/// fval=3395600
///
/// [game0]
/// id=1D6CC99282E80845BC8B7ECE293BF08E
/// year=2017   month=4   day=23   hour=18   minute=38
/// duration=3955488   mapsize=0   multi=1   lost=1
/// gold=119320   food=47908
/// units_prod=554   units_killed=192   units_lost=394   units_max=335
/// level_max=24   level_max_unit=Sentinella
/// health_sacr=0   priests=0   favorite=Pretoriano
/// enemies=8   allies=1   race=4
/// damage_taken=128432   damage_inflicted=52253
/// player_id=1   poser_score=739
/// kill_healths=48300   die_healths=104700
/// ```
///
/// The records are `[game0]`, `[game1]`, ... consecutively; the walk stops at
/// the first number with no section, so a hole truncates the journal. Every
/// key above must be present: the reader (0x0056b340) gives up on the first
/// one missing and the aggregation (0x0056d760) propagates that as a failure
/// rather than skipping the record.
///
/// ## The hash is reproduced, and that is the proof
///
/// `[Player] hash` is a rolling fold over the journal, and the shipped
/// profile's `hash=-1650049073` comes back **bit for bit** from 57 records
/// and 1,824 folds. That one number settles the whole reading at once -- the
/// record's field order, which fields are folded and in what order, the seed,
/// the nine-slot nation histogram, and that a string field folds a constant.
/// Nothing else here is guesswork sitting behind a plausible-looking screen.
///
/// The fold is `h = normalise(v + (mix(v) ^ 2h))`, where
/// `mix(v) = ((((v >> 8) ^ v) >> 5) & 0x07FFFFF8) + (v & 7) + 8v` and
/// `normalise(x) = x + (x >> 31)`, everything unsigned 32-bit. The seed is
/// `0x48564849`.
///
/// ## This engine reads profiles and does not write them
///
/// `docs/legal.md` keeps the installation read-only, so a journal this engine
/// appended to would be its own file. Profiles under the installation are read
/// and shown; a profile this engine creates or renames lives beside the saves.
/// Nothing here writes a hash, which is why the fold is a *check* and not a
/// serialiser.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/formats/result.hpp"

namespace imperivm::core::game {

/// One `[game<n>]` record, in the order `0x0056b340` reads the keys -- which
/// is also the order the record's fields sit in, and is not the order they
/// fold in.
struct ProfileGame {
  std::string id;             ///< a 32-hex-digit match id; the record's first key
  std::uint16_t year = 0;
  std::uint16_t month = 0;
  std::uint16_t day = 0;
  std::uint16_t hour = 0;
  std::uint16_t minute = 0;
  std::int32_t duration = 0;  ///< milliseconds
  std::int32_t map_size = 0;
  bool multiplayer = false;   ///< `multi`, stored as `value != 0`
  bool lost = false;          ///< `lost`, likewise
  std::int32_t gold = 0;      ///< gold spent
  std::int32_t food = 0;      ///< food spent
  std::int32_t units_produced = 0;
  std::int32_t units_killed = 0;
  std::int32_t units_lost = 0;
  std::int32_t units_max = 0;  ///< the most standing at once
  std::int32_t level_max = 0;
  std::string level_max_unit;
  std::int32_t health_sacrificed = 0;
  std::int32_t priests = 0;
  std::string favourite;  ///< `favorite`, the unit's display name
  std::int32_t enemies = 0;
  std::int32_t allies = 0;
  std::int32_t race = 0;
  std::int32_t damage_taken = 0;
  std::int32_t damage_inflicted = 0;
  std::int32_t player_id = 0;
  std::int32_t poser_score = 0;
  std::int32_t kill_healths = 0;
  std::int32_t die_healths = 0;
};

/// One row of `DATA\CONST.INI`'s `[Ranks]`: the rating at which the rank is
/// reached, and its name. The file says the rows must ascend by `points`; see
/// `parse_ranks` for the shipped file's one that does not.
struct Rank {
  std::int32_t points = 0;
  std::string name;
};

/// `[Ranks]`: `RanksCount` rows of `RankPoints<k>` and `RankName<k>`.
///
/// The walk stops at the first `k` with no `RankPoints`, as the executable's
/// does, so a hole shortens the table rather than leaving a gap in it.
[[nodiscard]] std::vector<Rank> parse_ranks(const IniDocument& document);

/// The career the screen shows. Every field is recomputed from the journal;
/// none of it is stored in the file.
struct ProfileStats {
  std::string rank;                       ///< the `[Ranks]` name, empty below the first
  std::uint32_t single_games = 0;
  std::uint32_t single_won_percent = 100;  ///< 100 when there are no games
  std::uint32_t multi_games = 0;
  std::uint32_t multi_won_percent = 100;
  std::int64_t duration = 0;               ///< milliseconds, summed
  std::int64_t rating_total = 0;           ///< the per-match scores, summed
  std::int32_t favourite_race = -1;        ///< the most played, -1 with no games
  std::uint32_t favourite_race_percent = 100;
  std::int64_t gold_spent = 0;
  std::int64_t food_spent = 0;
  std::int64_t units_killed = 0;
  std::int64_t units_lost = 0;
  std::int64_t health_sacrificed = 0;
  std::string best_unit;                   ///< the highest level reached, and by whom
  std::int32_t best_level = 0;
  std::int32_t most_units = 0;
  std::uint32_t hash = 0;                  ///< the fold; compare with `[Player] hash`

  /// The military rating: the summed per-match scores over the number of
  /// matches, with the divisor floored at one.
  [[nodiscard]] std::int64_t rating() const noexcept;
  /// `duration` in whole hours, which is the only unit the screen shows.
  [[nodiscard]] std::int64_t hours() const noexcept { return duration / 3600000; }
};

/// A profile: its `[Player]` block, its journal, and the career that follows.
struct Profile {
  std::string directory;  ///< the subdirectory of `Profiles/`, which names it
  std::string name;       ///< `[Player] name`; the directory when the key is absent
  std::string favourite_unit;   ///< `[Player] fav`, empty for a profile with no match
  std::int32_t colour = 0;      ///< `[Player] color`
  std::int32_t race = 0;        ///< `[Player] race`, the nation last chosen
  std::int32_t games = 0;       ///< `[Player] games`, the journal's own count
  std::int32_t stored_hash = 0; ///< `[Player] hash`, as the file spells it
  bool has_stored_hash = false;
  std::vector<ProfileGame> journal;
  ProfileStats stats;
};

/// Read one `player.ini`. `malformed` when a `[game<n>]` section is missing a
/// key the record needs, which is what the executable does with it.
///
/// `[Player]` itself is optional: a directory whose file has only a journal
/// still aggregates, and one with neither is a profile with no career.
[[nodiscard]] Result<Profile> parse_profile(const IniDocument& document);

/// Recompute `profile.stats` from `profile.journal`.
///
/// `ranks` picks the rank name; pass an empty span to leave it empty. The
/// hash lands in `stats.hash` whether or not the file carried one.
void aggregate(Profile& profile, std::span<const Rank> ranks);

/// The rank whose `points` the rating has reached: the last row, in file
/// order, whose `points` is at or below `rating`, stopping at the first that
/// is above. `-1` when even the first row is out of reach.
///
/// **The shipped table is not sorted and this is not a sort.** `CONST.INI`
/// carries `RankPoints10 = 2` where the ascending order its own comment
/// demands wants something above 200, so the walk stops at row 11 and every
/// rating from 200 to 299 answers `Legend`. `Hero`, row 9, is unreachable in
/// the retail game. Reproduced rather than repaired.
[[nodiscard]] std::int32_t rank_for(std::span<const Rank> ranks, std::int64_t rating) noexcept;

/// The rolling fold, exposed because the test pins it against the shipped
/// profile's stored hash and because nothing else in the tree hashes this way.
[[nodiscard]] std::uint32_t profile_hash_fold(std::uint32_t hash, std::uint32_t value) noexcept;

/// What the seed is, and what a string field folds. Both are the executable's
/// literals: a string contributes the same constant whatever it holds, which
/// is why two journals differing only in a unit's name hash alike.
inline constexpr std::uint32_t kProfileHashSeed = 0x48564849u;
inline constexpr std::uint32_t kProfileHashString = 0x0EA75617u;

}  // namespace imperivm::core::game
