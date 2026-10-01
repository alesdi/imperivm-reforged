#pragma once

/// The sixteen players, and the relations between them.
///
/// ## Sixteen, always
///
/// Every `.bfhp` in the retail install carries exactly sixteen `player<i>.xml`
/// files, from a two-player skirmish map to the campaign containers. The table
/// is fixed-size and fully populated; an unused player is one whose `control`
/// is `Disabled`, not one that is absent. `PlayerId` 14 and 15 are the
/// engine-reserved neutrals `sim/system.hpp` describes.
///
/// ## The relations matrix
///
/// Each `player<i>.xml` carries `relations="<128 hex digits>"`: sixteen
/// big-endian `uint32` words, one per player, MSB nibble first. Word `j` of
/// player `i`'s row is player `i`'s relation *to* player `j`.
///
/// **The matrix is not symmetric.** Three asymmetric pairs occur across the
/// shipped containers, all in `3_Great_Losses_Egypt.bfhp`: player 2's row holds
/// `0x01` at index 3 while player 3's row holds `0x11` at index 2, and so on.
/// Store the full 16x16; do not fold it. Three maps set any off-diagonal
/// relation at all -- Egypt, `Tutorial.BFHP` and `mediterranean.BFHP`.
///
/// Only four distinct words occur, over 4,864 relation words in 19 map sets
/// (4,544 of them zero):
///
/// | Word   | Bits set | Occurrences | Where |
/// |--------|----------|------------:|-------|
/// | `0x35` | 0, 2, 4, 5 |       304 | the diagonal, in every row of every map |
/// | `0x11` | 0, 4       |         9 | -- |
/// | `0x15` | 0, 2, 4    |         6 | pairs whose `playerdata/@allied` is `1` |
/// | `0x01` | 0          |         1 | player 2 to player 3, Egypt |
///
/// The census had to be redone once: `Packs/RandomMapSettlements.bfhp` is an
/// LZIS stream wrapping the container, so sniffing the magic misses it and the
/// first count was 288 documents in 18 sets rather than 304 in 19.
///
/// ## What the bits mean, read out of the executable
///
/// This was inferred once and the inference was wrong in two of five entries.
/// It is now **settled by disassembly**, from two independent sites that agree.
///
/// Each getter loads one dword from the relations row and tests one bit:
///
/// | Function | Address | Test | Bit |
/// |---|---|---|---:|
/// | `DiplGetCeaseFire`     | `0x00564990` | `and al, 1`          | 0 |
/// | `DiplGetShareSupport`  | `0x00564a30` | `shr eax, 2; and 1`  | 2 |
/// | `DiplGetShareView`     | `0x00564ad0` | `shr eax, 4`         | 4 |
/// | `DiplGetShareControl`  | `0x00564b70` | `shr eax, 5`         | 5 |
/// | `DiplAreAllied`        | `0x00564c10` | bit 0, both rows     | 0 |
///
/// The setters agree from the other side: `_PlayersAlly` ors 1,
/// `_PlayersMakeEnemies` ands out 1, `_PlayersShareSupport` ors 4,
/// `_PlayersShareView` ors `0x10`, `_PlayersShareControl` ors `0x20`. All ten
/// index `[player_record + other_id * 4 + 0x24]`, and the serialiser at
/// `0x00564843` registers exactly `record + 0x24`, length `0x40`, under the
/// attribute name `relations` -- so the file's 128 hex digits and the dwords
/// the getters read are the same sixteen words.
///
/// **Five named relations sit on four bits: `allied` and `ceasefire` are the
/// same bit.** There is no separate alliance bit. The competing reading of
/// three two-bit fields is refuted outright: it needs bits 1 and 3, and no code
/// path in any of the ten functions touches them.
///
/// `SetRelation` (`0x005652f0`) confirms the semantics by maintaining the five
/// per-player masks the executable's field names advertise -- `seeflags`,
/// `viewflags`, `enemyflags`, `controlflags`, `supportflags` -- as it writes.
/// Bit 0 *clear* sets the other player in `enemyflags`, making that mask the
/// exact complement of bit 0.
///
/// Still unknown: the upper 24 bits of a word, and bits 1 and 3, which are
/// never set and never read. Do not invent a meaning for them.
///
/// ## This is world state
///
/// Diplomacy changes at run time -- the exe logs "diplomacy changed" and
/// exposes setters -- so the matrix is hashed, serialised, and iterated in
/// index order like everything else in `sim/system.hpp`.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

/// Every map ships exactly this many.
inline constexpr std::size_t kPlayerCount = 16;

/// Who drives a player. The spelling is the `playerdata/@control` attribute's.
enum class PlayerControl : std::uint8_t {
  disabled,  ///< `Disabled` -- the slot exists but takes no part
  human,     ///< `Human`
  computer,  ///< `Computer`
  both,      ///< `Both` -- human or computer, decided at match setup
};

[[nodiscard]] PlayerControl parse_player_control(std::string_view text) noexcept;

/// The five named relations, in the order the exe lists their mask fields.
///
/// `ceasefire` is placed first because it is the only relation every observed
/// word sets, which makes it the weakest of the four nested values.
enum class Relation : std::uint8_t {
  ceasefire = 0,
  allied,
  share_view,
  share_control,
  share_support,
  count,
};

/// Bit position of each relation inside a packed relations word.
///
/// **Proven by disassembly**, not inferred; see the header note for the ten
/// addresses. `ceasefire` and `allied` deliberately share bit 0 -- the engine
/// has no separate alliance bit, and `DiplAreAllied` is bit 0 tested in both
/// directions.
inline constexpr std::array<std::uint8_t, static_cast<std::size_t>(Relation::count)>
    kRelationBits{0, 0, 4, 5, 2};

/// The four relation words the retail corpus contains, for tests to assert on.
inline constexpr std::uint32_t kRelationSelf = 0x35;
inline constexpr std::uint32_t kRelationAllied = 0x15;
inline constexpr std::uint32_t kRelationFriendly = 0x11;
inline constexpr std::uint32_t kRelationCeasefireOnly = 0x01;

/// One player's declared setup, as `player<i>.xml` gives it.
struct PlayerSetup {
  std::string name;
  /// `Mutable`, `Random`, or a race name. Left as written: resolving `Random`
  /// needs the RNG and therefore belongs to match setup, not to the loader.
  std::string race;
  std::string allowed_races = "All";
  PlayerControl control = PlayerControl::disabled;
  std::int32_t difficulty = 0;
  /// The 16-bit packed colour, verbatim. RGB555, like every other pixel in the
  /// engine; `docs/formats/rle.md` has the unpacking.
  std::uint16_t colour = 0;
  Point start;
  /// `playerdata/@bonus`, `-1` for none.
  std::int32_t bonus = -1;
  /// `playerdata/@allied`. A per-player marker, **not** the relation source:
  /// in `3_Great_Losses_Egypt.bfhp` players 0 and 1 hold `0x15` to each other
  /// and only player 1 has `allied="1"`. Kept because match setup reads it.
  bool allied_flag = false;
  /// `playerdata/@AI`, usually empty.
  std::string ai_script;
};

/// The players and the relations between them.
///
/// Fixed-size on purpose: `kPlayerCount` is a property of the format, the
/// indices are the `PlayerId`s the rest of the simulation uses, and iteration
/// in index order is the deterministic order.
class PlayerTable {
 public:
  PlayerTable();

  [[nodiscard]] static constexpr bool is_valid(PlayerId id) noexcept {
    return id < kPlayerCount;
  }

  [[nodiscard]] const PlayerSetup& setup(PlayerId id) const noexcept { return setup_[id]; }
  [[nodiscard]] PlayerSetup& setup(PlayerId id) noexcept { return setup_[id]; }

  /// The raw word `from`'s row holds for `to`. Out-of-range gives zero.
  [[nodiscard]] std::uint32_t relation_word(PlayerId from, PlayerId to) const noexcept;
  void set_relation_word(PlayerId from, PlayerId to, std::uint32_t word) noexcept;

  /// Does `from` grant `to` this relation?
  ///
  /// One-directional by design; the matrix is not symmetric. A predicate that
  /// wants mutuality asks twice and says so at the call site.
  [[nodiscard]] bool has(PlayerId from, PlayerId to, Relation relation) const noexcept;
  void set(PlayerId from, PlayerId to, Relation relation, bool on) noexcept;

  /// Whether `viewer` treats `other` as an enemy: `viewer`'s own row, bit 0
  /// clear.
  ///
  /// **One-directional, and proven so.** `Obj::IsEnemy` (`0x005aa480`) tests
  /// bit 0 of the *receiver's* owner row and `Settlement::IsEnemy`
  /// (`0x00425250`) tests the row of the player in its integer argument;
  /// neither consults the transpose. An earlier revision made this mutual,
  /// which would misplay the one-sided truces the shipped data contains.
  ///
  /// A player is never its own enemy.
  [[nodiscard]] bool is_enemy(PlayerId viewer, PlayerId other) const noexcept;

  /// Mutual `allied`, or the same player.
  [[nodiscard]] bool are_allied(PlayerId a, PlayerId b) const noexcept;

  /// Parse a 128-hex-digit `relations` attribute into `from`'s row.
  ///
  /// Rejects anything that is not exactly `2 * 4 * kPlayerCount` hex digits:
  /// a short or malformed attribute silently zeroing a row would make a map
  /// load and play wrong, which is worse than refusing to load it.
  [[nodiscard]] bool set_row_from_hex(PlayerId from, std::string_view hex) noexcept;

  /// Fold the matrix into the world hash. The setups are load-time data and are
  /// deliberately not hashed; the relations are, because they change in play.
  void hash(std::uint64_t& accumulator) const noexcept;

 private:
  std::array<PlayerSetup, kPlayerCount> setup_{};
  /// `relations_[from][to]`. Row-major, iterated in index order.
  std::array<std::array<std::uint32_t, kPlayerCount>, kPlayerCount> relations_{};
};

}  // namespace imperivm::core::sim
