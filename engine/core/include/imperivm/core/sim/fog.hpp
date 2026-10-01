#pragma once

/// The explored map: which square of the world each player has seen.
///
/// ## The encoding, read off `gbr.exe`
///
/// `IsExplored(pt, player)` (0x004c6820) hands the point and `player - 1` to
/// 0x00515950, whose whole body is address arithmetic:
///
///     if (slot >= 8) return true;                  // 0x00515958
///     dword = data[(y >> 10) * stride + (x >> 11)]; // 0x0051597b
///     half  = (dword >> (((x >> 10) & 1) << 4)) & 0xFFFF;
///     return ((half >> (2 * slot)) & 3) != 0;      // 0x005159a3
///
/// So a cell is **1024 by 1024 world units**, two cells share a `dword`, and a
/// cell holds **two bits for each of eight players**. Sixteen players and eight
/// fog slots is not a mistake: a player above the eighth has no fog at all, and
/// the short circuit at the top says so -- `IsExplored` answers **true** for
/// every point for players 9 to 16.
///
/// ## Four states, and what the middle one carries
///
/// The circle stamp (0x00516450) is a state machine over the two bits: it
/// acts on 0 and on 2 and leaves 1 and 3 alone. What the states are was read
/// off the stamp and the fog manager's feed (0x00516f80) together:
///
///   * **0, never seen.** The fog manager paints the cell's ground black.
///   * **2, partially explored.** The cell carries a **fine record**: a
///     32 x 32 lattice of 4-bit values at 32-unit spacing (a map keyed by cell
///     at `[expl + 0x4c + player * 12]`, data at `rec + 8`). The stamp writes
///     the circle's **edge** into it: `R = r + 48`, and a lattice point at
///     distance `d` from the centre gets `0` beyond `R`, `15` inside `R - 96`,
///     `(R - d) * 15 / 96` between, `max`ed with what was there. The fog
///     manager reads the lattice back as a cap on its light (`sim/fog_light.hpp`),
///     and the zoom map reads a 32-unit sub-cell as explored iff its value is
///     not zero. So the middle state is **spatial** -- the soft rim of every
///     circle ever stamped -- and not a phase of a fade.
///   * **3, fully explored.** The stamp writes it into every cell that lies
///     entirely inside `r - 48`, erasing the fine record if there was one.
///   * **1** is written by nothing that was found: the stamp never produces it
///     and the feed gives it no value. This engine never writes it either.
///
/// Every host reader is `!= 0`: `IsExplored` cannot tell 2 from 3, and
/// `GetUnexploredPoint` looks for 0. **One equivalence is taken and labelled:**
/// a fine record whose thirty-two by thirty-two values are all 15 is promoted
/// to state 3 and dropped. Every reader agrees -- the light cap is then
/// `0x3c00`, which draws nothing and hides nothing, exactly as no cap does;
/// the zoom map's test is *non-zero*; the hosts test *non-zero* -- and it keeps
/// the records to the rim, where the information is. A cell a unit's sight
/// circle can never contain whole (a 1024-unit square needs a radius of 725
/// past the 48 the stamp keeps back) would otherwise carry a full record of
/// fifteens forever.
///
/// ## The player argument is 1-based, and the original's own bounds check is
/// not
///
/// `ExploreCircle` and `ExploreArea` read a mask at
/// `[globals + 0xfc0 + player * 0x320]` and bounds-check `0 <= player < 16`.
///
/// This paragraph used to name `RevealHiddenEnemyUnits` as a third member of
/// that group. It is not one, in either half. It reads the word *after* that
/// mask -- `+0xfc4`, `record[p - 1] + 0x18` -- which is the player's **side**
/// mask and not its exploration mask, and it has **no bounds check at all**:
/// 0x004c8460 multiplies and indexes. Two adjacent words of one record, two
/// different questions, and only one of them belongs to this file.
/// That looks 0-based and is not: 0x0051d5b0 computes the local player's record
/// as `globals + 0x12cc + player * 0x320`, so the record array begins at
/// `0x12cc` with stride `0x320`, and `0xfc0 + p * 0x320` is
/// **`record[p - 1] + 0x14`**. The writers index the same 1-based way
/// `IsExplored`'s `dec` does, and the bounds check is off by one at both ends:
/// it admits 0, which reads one record before the array, and rejects 16, which
/// is a real player. No shipped site hits either -- `.player` is 1..16 and the
/// maps that explore use players 1 to 8 -- and the check is reproduced as the
/// *indexing* rather than as the arithmetic, because a reader out of bounds is
/// a fault and not a behaviour.
///
/// ## Not hashed. Saved.
///
/// `exploration` is one of the four channels the shipped build leaves at zero
/// in all nine desync dumps (`docs/engine/state-vector.md`), beside
/// `pathfinder`, `scriptstate` and `aihash`, and this engine keeps that. It is
/// **saved** anyway, for `WorldObject::sight`'s reason: recomputing it on load
/// would make a save's meaning depend on where every unit happened to be
/// standing, and a reloaded game whose map had gone dark is a different game.
///
/// ## What is still unknown
///
///   * **The reveal cadence.** The stamp's six sim call sites (through its
///     small-radius front end 0x00517ad0) were not read, so how often the
///     original stamps a unit's own surroundings is not established.
///     `FogSystem::advance` does it once a turn at `sight`, which is the
///     reading `UNIT_EXPLORE.VS` needs and the smallest claim that makes the
///     entry points answer.
///   * **The rectangle-against-circle tests** the stamp classifies a cell with
///     (0x005146a0) are read as *nearest point beyond `R`* for "entirely
///     outside" and *farthest corner within `r - 48`* for "entirely inside";
///     the helper's own arithmetic was not transcribed.
///   * **The lattice value.** A fine value is computed here at its lattice
///     point; the original evaluates the ramp at the four corners of the
///     32-unit sub-cell and blends them at the sub-cell's own offset, which
///     for a record aligned to its cell is the corner itself.
///   * **`RecreateExploration/0`** (0x00565070) is registered with zero call
///     sites. It presumably rebuilds the map from scratch after a load; there
///     is nothing here for it to do that a load does not already do.
///   * **`RevealHiddenEnemyUnits/3`** is not here, and it is not unknown
///     either -- it is in `sim/player_host.cpp`, where the diplomacy matrix is.
///     It is not an exploration entry point: it clears the hidden bit on enemy
///     units inside a circle and touches no cell of this grid. This bullet used
///     to claim it "writes into the low sixteen bits of `[obj+0x2c]`, which are
///     a *per-player visibility mask* rather than the owner this engine reads
///     them as", and conclude that it "needs a field on the object". It reads
///     those bits, they are the owner, and it needed no new field; the entry
///     point stayed blocked on the sentence rather than on the work. See the
///     note above `fn_reveal_hidden_enemy_units`.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <unordered_map>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;

/// Two bits per player per cell, eight players, 1024-unit cells, and the
/// fine records of the partially explored cells.
class ExplorationMap {
 public:
  /// World units per cell, off `y >> 10` and `x >> 11` with a half-word select.
  static constexpr std::int32_t kCellSize = 1024;
  /// Fog slots. Sixteen players share eight; see the header.
  static constexpr std::int32_t kSlots = 8;
  /// The fine lattice: 32-unit spacing, 32 x 32 points per cell.
  static constexpr std::int32_t kFineSpacing = 32;
  static constexpr std::int32_t kFineSide = kCellSize / kFineSpacing;
  static constexpr std::int32_t kFineMax = 15;
  /// The stamp's margins: fully explored to `r - kInner`, the ramp out to
  /// `r + kInner`, `kRamp` wide.
  static constexpr std::int32_t kInner = 48;
  static constexpr std::int32_t kRamp = 96;

  enum class State : std::uint8_t { never = 0, unwritten = 1, partial = 2, full = 3 };

  /// A partially explored cell's lattice, row-major, one value 0..15 a point.
  struct FineRecord {
    std::array<std::uint8_t, static_cast<std::size_t>(kFineSide) * kFineSide> values{};
  };

  /// Size the grid for a square map `extent` world units on a side. An extent
  /// of zero leaves the map empty, which reads as *nothing is explored* -- the
  /// answer the original gives when there is no fog manager (0x004c684f).
  void resize(std::int32_t extent);

  [[nodiscard]] std::int32_t cells() const noexcept { return side_; }
  [[nodiscard]] bool empty() const noexcept { return side_ == 0; }

  /// Whether `slot` has seen the cell `at` falls in: its state is not `never`.
  ///
  /// A slot at or past `kSlots` is **explored everywhere**, which is the
  /// original's short circuit; an empty map is explored nowhere, which is its
  /// missing-manager path. A point outside the grid is not explored.
  [[nodiscard]] bool explored(Point at, std::int32_t slot) const noexcept;

  /// The state of cell `(cx, cy)` for `slot`; `never` off the grid or for a
  /// slot outside 0..7.
  [[nodiscard]] State state(std::int32_t cx, std::int32_t cy, std::int32_t slot) const noexcept;
  /// The fine record of a `partial` cell, or null.
  [[nodiscard]] const FineRecord* fine(std::int32_t cx, std::int32_t cy,
                                       std::int32_t slot) const noexcept;
  /// The exploration value 0..15 at lattice point `(fx, fy)` -- in fine units
  /// from the grid's origin, so `cx * kFineSide + i` -- for `slot`: 15 in a
  /// `full` cell, 0 in a `never` one, the record's value in a `partial` one.
  /// The fog manager's neighbour rule (0x00516ab0): a lattice point on a
  /// cell's far edge belongs to the next cell. `unwritten` reads as 15, which
  /// is a choice where the original leaves an argument uninitialised.
  [[nodiscard]] std::int32_t fine_at(std::int32_t fx, std::int32_t fy,
                                     std::int32_t slot) const noexcept;

  /// Mark the cell `at` falls in fully explored by `slot`.
  void explore(Point at, std::int32_t slot);

  /// The circle stamp, 0x00516450: fully explored inside `radius - 48`, the
  /// 96-unit ramp written into the fine records of the cells the annulus
  /// `[radius - 48, radius + 48]` meets, nothing beyond. A negative radius
  /// stamps nothing.
  void explore_circle(Point centre, std::int32_t radius, std::int32_t slot);

  /// `ExploreAll()`. Every cell, every slot, fully.
  void explore_all();

  /// The centre of the nearest never-seen cell within `radius` of `from`, or
  /// `found = false` when there is none.
  ///
  /// **Nearest, with ties going to the lower cell index** -- row-major order,
  /// and therefore a function of the grid alone rather than of anything a
  /// caller can vary. What the original picks is *not* established beyond "an
  /// unexplored point inside the radius"; see the header's unknowns.
  struct Search {
    Point point;
    bool found = false;
  };
  [[nodiscard]] Search nearest_unexplored(Point from, std::int32_t radius,
                                          std::int32_t slot) const noexcept;

  /// The cells as the original packs them: one `uint16` a cell, two bits a
  /// slot, slot `s` at bits `2s..2s+1`.
  [[nodiscard]] std::span<const std::uint16_t> raw() const noexcept { return cells_; }
  /// The fine records, in cell-index order within slot order -- the order the
  /// save writes them in.
  struct FineEntry {
    std::int32_t slot = 0;
    std::int32_t index = 0;
    const FineRecord* record = nullptr;
  };
  [[nodiscard]] std::vector<FineEntry> fine_entries() const;
  /// Install a grid and its records. A record for a cell that is not
  /// `partial`, a `partial` cell without one, a value past 15 or a count that
  /// is not `side * side` is refused, and the map is left as it was.
  [[nodiscard]] bool set_raw(std::int32_t side, std::span<const std::uint16_t> cells,
                             std::span<const FineEntry> records);

 private:
  [[nodiscard]] std::int32_t index_of(Point at) const noexcept;
  [[nodiscard]] State state_at(std::int32_t index, std::int32_t slot) const noexcept;
  void set_state(std::int32_t index, std::int32_t slot, State state) noexcept;
  [[nodiscard]] FineRecord& record_for(std::int32_t index, std::int32_t slot);
  void drop_record(std::int32_t index, std::int32_t slot);
  /// Promote a record of fifteens to `full`; see the header's equivalence.
  void normalise(std::int32_t index, std::int32_t slot);

  std::int32_t side_ = 0;
  std::vector<std::uint16_t> cells_;
  /// Per slot, the partial cells' records, keyed by cell index. **Iteration
  /// order is state**: this is a `std::map` so that the save and every walk are
  /// in index order.
  std::array<std::map<std::int32_t, FineRecord>, kSlots> fine_;
};

/// The system that owns the map and keeps it up to date.
class FogSystem final : public System {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "fog"; }

  /// Size the grid from the match's map extent and reveal what is already
  /// standing, so a script that asks on turn zero gets an honest answer.
  void start(World& world) override;

  /// Stamp every object's owner's slot within its own `sight`.
  void advance(World& world, const Turn& turn) override;

  /// **Deliberately empty.** `exploration` is one of the four channels the
  /// shipped build leaves out of the sync hash; see the header.
  void hash(std::uint64_t& accumulator) const override { (void)accumulator; }

  [[nodiscard]] ExplorationMap& map() noexcept { return map_; }
  [[nodiscard]] const ExplorationMap& map() const noexcept { return map_; }

  void serialize(std::vector<std::byte>& out) const;
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  /// Stamp every object that has moved, or changed owner or sight, since it
  /// was last stamped; see the body for why that is every object's stamp.
  void sweep(World& world);

  ExplorationMap map_;
  /// What each object was last stamped as. **Not state**: an empty table
  /// re-stamps everything and the map comes out the same, which is what a load
  /// relies on.
  struct Stamp {
    Point position;
    std::int32_t sight = 0;
    PlayerId owner = kNoPlayer;
    friend bool operator==(const Stamp&, const Stamp&) = default;
  };
  std::unordered_map<ObjectId, Stamp> stamped_;
};

/// The world's fog system, or null.
[[nodiscard]] FogSystem* fog_system_of(World& world) noexcept;

/// The fog slot a script's **1-based** player number names, or -1.
///
/// 1..16 in, 0..15 out, the same conversion `player_from_script` makes; the
/// caller decides what to do with a slot at or past `ExplorationMap::kSlots`.
[[nodiscard]] std::int32_t fog_slot_from_script(std::int32_t player) noexcept;

/// `IsExplored/2`, `ExploreArea/2`, `ExploreCircle/3`, `ExploreAll/0`,
/// `GetUnexploredPoint/1`. Returns how many were newly implemented.
std::size_t register_fog_host(script::HostRegistry& registry);

/// The number of `define` calls `register_fog_host` makes.
[[nodiscard]] std::size_t fog_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
