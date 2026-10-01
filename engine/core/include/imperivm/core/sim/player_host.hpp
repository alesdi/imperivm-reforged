#pragma once

/// The diplomacy half of the `.vs` host API: who is whose enemy, and what a
/// script may ask about a player.
///
/// `sim/player.hpp` owns the state -- sixteen players and a 16x16 relations
/// matrix -- and this header attaches the 21 entry points the corpus calls to
/// reach it. Combat targeting, tribute, and every AI objective bottom out in
/// `IsEnemy` (159 sites) and `EnemyObjs` (63), so this is the seam those depend
/// on rather than on a private notion of hostility of their own.
///
/// ## Script player numbers are 1..16, not 0..15
///
/// **This is proven and it is easy to get wrong.** `PlayerId` is 0..15 through
/// the whole simulation, but every number a `.vs` script passes or receives is
/// one more than that:
///
///   * `Obj::player` (`gbr.exe` 0x005ab290) loads the owner record's `ID` field
///     and returns `ID + 1`; an unowned object yields -1.
///   * `Settlement::IsOwn` (0x004252c0) does `dec` on its argument before
///     comparing it with the owner's `ID`. `Settlement::IsAlly` (0x004251e0)
///     indexes the relations table at `0xfd0 + arg*0x320`, which is the row of
///     player `arg - 1`.
///   * `Outposts` (0x0042d770), `Strongholds` (0x0042d2f0) and `MilUnits`
///     (0x00421ee0) all `dec` their argument and then bounds-check against 16.
///   * Five error strings in the executable say so in words: "Function
///     DiplGetCeaseFire: Player number should be between 1 and 16", and the
///     same for `DiplAreAllied`, `DiplShareSupport`, `EnemyObjs` and
///     `ControllableObjs`.
///   * The corpus agrees without the executable: `STONEHENGE_WISDOM.VS` walks
///     every player as `for (k = 1; k <= 16; k += 1) GetPlayerUnits(k)`, and
///     `ESH_MARKET.VS` reads the two engine neutrals as `Outposts(15) +
///     Outposts(16)` with the comment `VX_PLAYER_NEUTRAL + VX_PLAYER_RESCUE` --
///     which are `PlayerId` 14 and 15, the two `sim/system.hpp` names.
///
/// Every entry point here converts with `player_from_script` and
/// `player_to_script` and never open-codes the adjustment.
///
/// **`sim/world_host.cpp`'s `player` member does not do this**: it returns the
/// raw 0-based owner. That is 531 call sites disagreeing with these by one, and
/// it has to be fixed there, not worked around here -- a compensating
/// off-by-one in this file would be invisible until the two were assembled.
///
/// ## What is not defined here, and why
///
/// `player`/0 and `ClassPlayerObjs`/2 belong to `register_world_host`, and
/// `IsEnemy`/1 is also defined by `define_combat_host`. A second `define()` of
/// the same (kind, name, arity) silently replaces the first, so the first two
/// are deliberately left alone. `IsEnemy` is defined here anyway because the
/// combat version answers only the object-object form against a private
/// alliance set, and the settlement-and-integer form -- `set.IsEnemy(AIPlayer)`
/// -- is in the corpus too; whichever registration runs last wins, which is a
/// hazard the report names.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/player.hpp"

namespace imperivm::core::sim {

/// The number a script uses for `id`. Never negative for a valid player.
[[nodiscard]] constexpr std::int32_t player_to_script(PlayerId id) noexcept {
  return PlayerTable::is_valid(id) ? static_cast<std::int32_t>(id) + 1 : -1;
}

/// The `PlayerId` a script's 1..16 means, or `kNoPlayer` when out of range.
[[nodiscard]] constexpr PlayerId player_from_script(std::int32_t number) noexcept {
  if (number < 1 || number > static_cast<std::int32_t>(kPlayerCount)) return kNoPlayer;
  return static_cast<PlayerId>(number - 1);
}

/// `CVXPlayerFlagsQuery.type`, the discriminator carried on the query object.
///
/// **Proven**, from the three constructors: `EnemyObjs` (0x005756b0) pushes 2,
/// `FriendlyObjs` (0x00575ab0) pushes 1 and `ControllableObjs` (0x005758b0)
/// pushes 3 into the same query factory at 0x004fe990. `sim/query.hpp` records
/// the field's domain as {1, 2} because those are the only values the nine
/// desync dumps happen to contain; 3 is real and simply never instantiated in
/// them.
///
/// Which per-player mask each type consults is *not* proven -- the names line
/// up with `enemyflags` and `controlflags` and nothing contradicts that, but the
/// match function was not disassembled. See docs/formats/map.md.
inline constexpr std::int32_t kPlayerFlagsFriendly = 1;
inline constexpr std::int32_t kPlayerFlagsEnemy = 2;
inline constexpr std::int32_t kPlayerFlagsControllable = 3;

/// The race a class or a player carries, as the integer the scripts compare
/// against. Not a `PlayerId`, not an index into anything here.
///
/// **Proven**: the eight race constants are registered one after another at
/// `gbr.exe` 0x005b70b2 with these literal values. Note that `Rome` and
/// `RepublicanRome` are two names for 1, and that the shipped `player<i>.xml`
/// files spell the two Romes apart.
enum class Race : std::int32_t {
  gaul = 0,
  republican_rome = 1,
  carthage = 2,
  iberia = 3,
  imperial_rome = 4,
  britain = 5,
  egypt = 6,
  germany = 7,
};

/// What a raceless class or an unresolved player race reads as.
///
/// **Inferred.** `Obj::race` (0x005ada20) returns a field of the class
/// descriptor verbatim and the shipped class XML writes `race="None"` for the
/// generic classes, but what integer the engine stores for `None` was not
/// recovered. -1 is chosen because it can never equal a real race, so a script
/// comparing `.race == Carthage` cannot accidentally match.
inline constexpr std::int32_t kNoRace = -1;

/// `"Carthage"` -> 2. `kNoRace` for `None`, `Random`, `Mutable`, `Select`, an
/// empty string, or anything unrecognised: those are match-setup markers rather
/// than races, and resolving them needs the RNG.
[[nodiscard]] std::int32_t race_from_name(std::string_view name) noexcept;

/// The one-letter class-name prefix `GetRaceStrPref` answers -- `G`, `R`, `C`,
/// `I`, `M`, `B`, `E`, `T` for races 0..7, lower-cased on request, and empty for
/// anything else. See the definition for why `M` and `T`.
[[nodiscard]] std::string race_prefix(std::int32_t race, bool lower) noexcept;

/// The spelling the shipped data uses, or an empty view. `1` gives
/// `"RepublicanRome"`; the `Rome` alias is accepted on the way in only.
[[nodiscard]] std::string_view race_to_name(std::int32_t race) noexcept;

/// Define every entry point this domain owns.
///
/// Call after `declare_shipped_surface`. Returns the number defined, so a
/// caller can assert the count rather than trust it -- the convention
/// `register_world_host` and `register_economy_hosts` already follow.
std::size_t register_player_host(script::HostRegistry& registry);

/// The number `register_player_host` defines, for that assertion.
[[nodiscard]] std::size_t player_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
