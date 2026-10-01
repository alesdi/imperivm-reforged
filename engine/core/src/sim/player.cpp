// The sixteen players and the relations between them.
// See include/imperivm/core/sim/player.hpp.
//
// ## The relation bits are no longer inferred
//
// The header calls `kRelationBits` inferred and asks not to be believed. It was
// wrong, and `gbr.exe` says so in two independent places. `docs/formats/map.md`
// carries the full derivation; the short version, with addresses so it can be
// checked:
//
//   * The five script getters each load one dword out of the relations row and
//     test one bit of it. `DiplGetCeaseFire` (0x00564990) does `and al, 1`;
//     `DiplGetShareSupport` (0x00564a30) does `shr eax, 2; and al, 1`;
//     `DiplGetShareView` (0x00564ad0) does `shr eax, 4`; `DiplGetShareControl`
//     (0x00564b70) does `shr eax, 5`. `DiplAreAllied` (0x00564c10) tests **bit
//     0 in both directions** -- it is the mutual reading of the same bit
//     `DiplGetCeaseFire` reads one-directionally, not a bit of its own.
//   * The five setters agree, from the other side. `_PlayersAlly` (0x006a4a90)
//     does `or 1` on both rows, `_PlayersMakeEnemies` (0x006a4c90) does
//     `and ~1` on both, `_PlayersShareSupport` (0x006a4c10) `or 4`,
//     `_PlayersShareView` (0x006a4b90) `or 0x10`, `_PlayersShareControl`
//     (0x006a4b10) `or 0x20`.
//
// So four bits carry five named relations, because `allied` and `ceasefire`
// are the same bit. Bits 1 and 3 -- the two the header's competing "three
// two-bit fields" reading needed -- are read by nothing and written by nothing.
//
// `kRelationBits` in the header is therefore **superseded and must be corrected
// to `{0, 0, 4, 5, 2}`**. Nothing reads it any more: `bit_of` below is the only
// mapping in the build, so there is one source of truth rather than two that
// can drift. Changing the header is not this file's to make.

#include "imperivm/core/sim/player.hpp"

namespace imperivm::core::sim {
namespace {

[[nodiscard]] int hex_digit(char c) noexcept {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/// Bit position of each relation, in `Relation` order. **Proven**, from the
/// getters and the setters both; see the file header for the addresses.
///
/// `allied` and `ceasefire` share bit 0 on purpose. That is not a placeholder:
/// the engine has no separate alliance bit, `_PlayersAlly` and
/// `_PlayersMakeEnemies` set and clear this one bit, and the per-player
/// `enemyflags` mask is maintained as its exact complement.
constexpr std::array<std::uint8_t, static_cast<std::size_t>(Relation::count)> kProvenBits{
    0,  // ceasefire      DiplGetCeaseFire:     and al, 1
    0,  // allied         DiplAreAllied:        bit 0, both directions
    4,  // share_view     DiplGetShareView:     shr eax, 4
    5,  // share_control  DiplGetShareControl:  shr eax, 5
    2,  // share_support  DiplGetShareSupport:  shr eax, 2
};

[[nodiscard]] constexpr std::uint32_t bit_of(Relation relation) noexcept {
  return std::uint32_t{1} << kProvenBits[static_cast<std::size_t>(relation)];
}

// The four words the retail corpus contains, decoded under the proven bits.
// They nest exactly, which is the check that the assignment is not accidental:
// 0x35 = ceasefire + support + view + control (a player to itself), 0x15 drops
// control, 0x11 drops support as well, 0x01 leaves only the ceasefire.
static_assert(kRelationSelf ==
              ((1u << 0) | (1u << 2) | (1u << 4) | (1u << 5)));
static_assert(kRelationAllied == ((1u << 0) | (1u << 2) | (1u << 4)));
static_assert(kRelationFriendly == ((1u << 0) | (1u << 4)));
static_assert(kRelationCeasefireOnly == (1u << 0));

}  // namespace

PlayerControl parse_player_control(std::string_view text) noexcept {
  if (text == "Human") return PlayerControl::human;
  if (text == "Computer") return PlayerControl::computer;
  if (text == "Both") return PlayerControl::both;
  // `Disabled` and anything unrecognised. A misspelling that silently became a
  // playing faction would change the outcome of a match, so the default is the
  // one that takes no part.
  return PlayerControl::disabled;
}

PlayerTable::PlayerTable() {
  // Every shipped map sets the diagonal to `kRelationSelf`, in all 304 rows
  // across all sixteen containers. Seed it so a table nobody loaded a map into
  // still answers `are_allied(p, p)` and `is_enemy(p, p)` correctly.
  for (std::size_t i = 0; i < kPlayerCount; ++i) relations_[i][i] = kRelationSelf;
}

std::uint32_t PlayerTable::relation_word(PlayerId from, PlayerId to) const noexcept {
  if (!is_valid(from) || !is_valid(to)) return 0;
  return relations_[from][to];
}

void PlayerTable::set_relation_word(PlayerId from, PlayerId to, std::uint32_t word) noexcept {
  if (!is_valid(from) || !is_valid(to)) return;
  relations_[from][to] = word;
}

bool PlayerTable::has(PlayerId from, PlayerId to, Relation relation) const noexcept {
  if (relation == Relation::count) return false;
  return (relation_word(from, to) & bit_of(relation)) != 0;
}

void PlayerTable::set(PlayerId from, PlayerId to, Relation relation, bool on) noexcept {
  if (!is_valid(from) || !is_valid(to) || relation == Relation::count) return;
  const std::uint32_t bit = bit_of(relation);
  if (on) {
    relations_[from][to] |= bit;
  } else {
    relations_[from][to] &= ~bit;
  }
}

bool PlayerTable::is_enemy(PlayerId a, PlayerId b) const noexcept {
  // **One-directional, and that is not a simplification.** An earlier revision
  // of the header specified the mutual rule -- "true when neither player grants
  // the other a ceasefire" -- and `gbr.exe` does not do that. The difference is
  // observable in the shipped data, which contains one-sided truces.
  //
  // `Obj::IsEnemy` (0x005aa480) resolves the receiver's owner record and the
  // parameter's, then evaluates
  //
  //     dl = [receiver_owner + param_owner_id*4 + 0x24]; not dl; and dl, 1
  //
  // -- bit 0 of the *asking* player's row alone. `Settlement::IsEnemy`
  // (0x00425250) does the same, with the row of the player named in its integer
  // argument. Neither consults the other direction, and `SetRelation`
  // (0x005652f0) maintains a per-player `enemyflags` mask as the exact
  // complement of this one bit, which a mutual rule could not be stored as.
  //
  // It matters: in `3_Great_Losses_Egypt.bfhp` player 3's row holds 0x11 for
  // player 5 while player 5's row holds zero for player 3. Under the mutual rule
  // neither is the other's enemy; under the engine's rule player 5 is hostile to
  // player 3 and player 3 is not hostile to player 5 -- a one-sided truce, which
  // is what an asymmetric matrix exists to express.
  //
  // A player is never its own enemy: every shipped row sets its own diagonal to
  // `kRelationSelf`, whose bit 0 is set, so the general rule already answers
  // that. The explicit check only covers a table nobody loaded a map into.
  if (!is_valid(a) || !is_valid(b) || a == b) return false;
  return !has(a, b, Relation::ceasefire);
}

bool PlayerTable::are_allied(PlayerId a, PlayerId b) const noexcept {
  // `DiplAreAllied` (0x00564c10) tests bit 0 of each player's row for the other
  // and requires both, so this stays as written -- but only because
  // `Relation::allied` now resolves to bit 0 rather than to bit 2. Under the
  // header's superseded `kRelationBits` this read the share-support bit, which
  // happened to agree on all shipped data (`0x15` sets it, `0x11` does not) and
  // would have disagreed the moment a script called `_PlayersShareSupport`.
  //
  // The engine additionally requires a per-player dword at CVXPlayer+0x64 to be
  // non-zero on both sides. That field is not among the nine the serialiser
  // names, nothing in the shipped data reaches it, and its meaning is unknown,
  // so it is not modelled. See docs/formats/map.md.
  if (!is_valid(a) || !is_valid(b)) return false;
  if (a == b) return true;
  return has(a, b, Relation::allied) && has(b, a, Relation::allied);
}

bool PlayerTable::set_row_from_hex(PlayerId from, std::string_view hex) noexcept {
  constexpr std::size_t kDigitsPerWord = 8;
  constexpr std::size_t kDigits = kDigitsPerWord * kPlayerCount;
  if (!is_valid(from) || hex.size() != kDigits) return false;

  // Decoded into a scratch row first: a malformed digit halfway through must
  // leave the table as it was rather than half-written.
  std::array<std::uint32_t, kPlayerCount> row{};
  for (std::size_t to = 0; to < kPlayerCount; ++to) {
    std::uint32_t word = 0;
    for (std::size_t d = 0; d < kDigitsPerWord; ++d) {
      const int digit = hex_digit(hex[to * kDigitsPerWord + d]);
      if (digit < 0) return false;
      word = (word << 4) | static_cast<std::uint32_t>(digit);
    }
    row[to] = word;
  }
  relations_[from] = row;
  return true;
}

void PlayerTable::hash(std::uint64_t& accumulator) const noexcept {
  // Index order, both dimensions. Iteration order is state.
  for (std::size_t from = 0; from < kPlayerCount; ++from) {
    for (std::size_t to = 0; to < kPlayerCount; ++to) {
      accumulator = accumulator * 1099511628211ull ^ relations_[from][to];
    }
  }
}

}  // namespace imperivm::core::sim
