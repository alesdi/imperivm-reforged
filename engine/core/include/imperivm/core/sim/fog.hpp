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
/// Two stamps write the states, and both act on 0 and on 2 and leave 1 and 3
/// alone. What the states are was read off the stamps and the fog manager's
/// feed (0x00516f80) together:
///
///   * **0, never seen.** The fog manager paints the cell's ground black.
///   * **2, partially explored.** The cell carries a **fine record**: a
///     32 x 32 lattice of 4-bit values at 32-unit spacing (a map keyed by cell
///     at `[expl + 0x4c + player * 12]`, data at `rec + 8`). The fog manager
///     reads the lattice back as a cap on its light (`sim/fog_light.hpp`), and
///     the zoom map reads a 32-unit sub-cell as explored iff its value is not
///     zero. So the middle state is **spatial** -- the soft rim of every
///     circle ever stamped -- and not a phase of a fade.
///   * **3, fully explored.** Only the large stamp writes it.
///   * **1** is written by nothing that was found. This engine never writes it.
///
/// **The unit stamp, 0x00517ad0** -- a unit's move (0x005a8660) and owner
/// change (0x005aa0a0), the share-view merge (0x00564cd0), the notes' pins.
/// A radius past 1024 is handed whole to the large stamp. Otherwise, with
/// `R = r + 48`, it walks the three by three cells about the point's own
/// (coordinates divided toward zero), keeps those inside the map rectangle
/// that meet the box `pt ± R`, and for each slot of the mask's low byte gives
/// a cell at 0 a zeroed record and state 2 and writes the **brush** into the
/// record of a cell at 2 (0x00514fc0). It never writes 3; a cell the box
/// grazes keeps an empty record. The brush is a table (0x005f8810, the cache
/// at `map + 0xe4`, built on first use for each `n = R / 16 + 2` and kept) of
/// `(2n + 1)²` samples 16 units apart about the centre: 15 nearer than
/// `n - w` samples, `trunc((n - d) * 15 / w)` out to `n`, 0 beyond, with
/// `w = min(96 / 16 + 1, n)`. Each lattice point of the cell within the box
/// `16n - 16` of the centre reads its four neighbouring samples and blends
/// them bilinearly at its offset from the table's origin, in shifts of 9,
/// truncating; the result is `max`ed in. So a unit's circle is flat to about
/// `r - 32` and fades out to `r + 80`.
///
/// **The large stamp, 0x00516450** -- `ExploreCircle`, `ExploreArea`,
/// `SetSight` and the scout skill when the sight widens, and the unit stamp
/// past 1024. It walks every cell from the one `pt - R` falls in to the one
/// `pt + R` does and classifies each against the circle (0x005146a0: 1 when
/// all four corners are within the radius, -1 when they do not meet, 0
/// otherwise): first against `R`, skipping -1, then against `r - 48`. On 1 a
/// cell becomes 3 and its record is erased; on 0 the **rim** is written into
/// its record (0x00514b80); on -1 -- a cell only the outer ring reaches --
/// **nothing is written**. The rim evaluates the ramp (0 past `R`, 15 short of
/// `R - 96`, `(R - d) * 15 / 96` between, `d` the floored root) at the four
/// corners of the 32-unit square each lattice point heads and blends them at
/// the point's offset, modulo 16, from the origin of the box `16 (R / 16 + 2)`
/// about the centre. All of it is 32-bit arithmetic, wrapping, signed.
///
/// **The host reader looks inside a partial cell.** 0x00515950 answers 0 as
/// false and 3 as true, and for 1 and 2 finds the slot's record holding the
/// point and answers whether the 4-bit value of the 32-unit sub-cell the point
/// falls in is non-zero (0x00514a60). So `IsExplored` and
/// `GetUnexploredPoint` see the soft rim at 32-unit resolution: a point in a
/// rim cell beyond the circle's reach is *not* explored. (This header used to
/// say every reader was `state != 0`, and the engine answered true anywhere
/// in a partial cell.) **One equivalence is taken and labelled:**
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
///   * **The cadence.** The original stamps at every position change; the
///     sweep stamps once a turn, after movement, for every object that has
///     moved or changed owner since -- the same stamps for a unit that moves
///     once a turn (labelled in `FogSystem::sweep`).
///   * **The second grid at `map + 0xb4`** (stride `map + 0xc4`): both
///     stamps set bit `2 * slot` there for every record they write, and the
///     large stamp clears it when it erases one. No reader was found; it is
///     not kept.
///   * **Bit 0x40000000 of `[obj+0x2c]`**, which the move stamp refuses
///     beside the spawn-template bit. Its meaning was not identified.
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
struct WorldObject;

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

  /// Whether `slot` has explored the point `at`: 0x00515950.
  ///
  /// A `full` cell is explored and a `never` one is not; in a `partial` cell
  /// the answer is the fine value of the 32-unit sub-cell `at` falls in, non-
  /// zero. A slot outside `0..kSlots-1` -- compared **unsigned**, so a
  /// negative slot too -- is **explored everywhere**, which is the original's
  /// short circuit; an empty map is explored nowhere, which is its
  /// missing-manager path. A point outside the grid is not explored (the
  /// original reads outside its array).
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

  /// The large stamp, 0x00516450, for one slot. Every cell between the cells
  /// the box `pt ± (r + 48)` begins and ends in is classified (0x005146a0)
  /// against the circle `R = r + 48` and, if it meets it, against `r - 48`:
  /// a cell entirely inside the inner circle becomes fully explored (its
  /// record dropped); one that meets the inner circle has the rim written into
  /// its record (0x00514b80); one that meets only the outer circle is left
  /// alone. States 1 and 3 are never touched. A radius of zero stamps nothing;
  /// a negative one runs the same arithmetic, in 32 bits.
  void explore_circle(Point centre, std::int32_t radius, std::int32_t slot);

  /// The large stamp as its callers make it: every slot of `mask`'s low byte.
  /// `ExploreCircle`, `ExploreArea`, `SetSight` and the scout skill.
  void explore_circle_mask(Point centre, std::int32_t radius, std::uint32_t mask);

  /// The unit stamp, 0x00517ad0: the large stamp for a radius past 1024, and
  /// otherwise the small one -- the three by three cells about the point's
  /// cell, each that meets the box `pt ± (r + 48)` given a record (a cell at
  /// state 0 gets a zeroed one and becomes 2) into which the cached brush is
  /// read bilinearly (0x00514fc0, 0x005f8810). It never writes state 3. A
  /// move, an owner change, the share-view merge and the catapult's site.
  void explore_sight(Point centre, std::int32_t radius, std::uint32_t mask);

  /// The small stamp's brush for `n = (r + 48) / 16 + 2` (0x005f8810):
  /// `(2n + 1)` by `(2n + 1)` samples 16 units apart, centred, each a value
  /// 0..15 shifted left 9. Built on first use and kept.
  [[nodiscard]] const std::vector<std::int32_t>& brush(std::int32_t n) const;

  /// `ExploreAll()`. Every cell, every slot, fully, and the map's
  /// explored-everywhere flag (`[map+0xf0]`, 0x005157c0's first write).
  void explore_all();

  /// The explored-everywhere flag: 0 from the map's constructor (0x005179d0)
  /// and 1 from `ExploreAll` on; nothing else writes it. `GAIKA::Explored`
  /// reads it (0x0041cb80). The original's `Exploration` persist (0x00517860)
  /// does not write it and its load puts it down; neither does this engine's
  /// save, and the load raises it again only by the game's start
  /// (`GameSession::load`).
  [[nodiscard]] bool explored_everywhere() const noexcept { return everywhere_; }
  void set_explored_everywhere(bool everywhere) noexcept { everywhere_ = everywhere; }

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
  /// 0x00514fc0: the brush into one cell's record.
  void stamp_brush(FineRecord& record, std::int32_t x0, std::int32_t y0, Point centre,
                   std::int32_t outer) const;

  std::int32_t side_ = 0;
  std::vector<std::uint16_t> cells_;
  bool everywhere_ = false;
  /// Per slot, the partial cells' records, keyed by cell index. **Iteration
  /// order is state**: this is a `std::map` so that the save and every walk are
  /// in index order.
  std::array<std::map<std::int32_t, FineRecord>, kSlots> fine_;
  /// The brush cache (`map + 0xe4`, 130 slots). **Not state**: each entry is
  /// a function of its index.
  mutable std::array<std::vector<std::int32_t>, 130> brush_;
};

/// The system that owns the map and keeps it up to date.
class FogSystem final : public System {
 public:
  [[nodiscard]] std::string_view name() const noexcept override { return "fog"; }

  /// Size the grid from the match's map extent and reveal what is already
  /// standing, so a script that asks on turn zero gets an honest answer.
  void start(World& world) override;

  /// Stamp every object that has moved or changed owner since it was last
  /// seen, as the original's move (0x005a8660) and owner change (0x005aa0a0)
  /// do: through `explore_sight`, at the object's `sight`, for its owner's
  /// `viewflags`.
  void advance(World& world, const Turn& turn) override;

  /// Take every object as already stamped where it stands, writing nothing.
  /// A load calls it: the original's load stamps nothing, and a restamp here
  /// would write circles its map never held.
  void prime(const World& world);

  /// Stamp `object` at `sight` through `explore_sight` now, and take it as
  /// stamped where it stands -- the catapult's site, revealed at
  /// `CatapultBuildSight` for the one `SetPos` it is placed with.
  void stamp_now(World& world, ObjectId object, std::int32_t sight);

  /// **Deliberately empty.** `exploration` is one of the four channels the
  /// shipped build leaves out of the sync hash; see the header.
  void hash(std::uint64_t& accumulator) const override { (void)accumulator; }

  [[nodiscard]] ExplorationMap& map() noexcept { return map_; }
  [[nodiscard]] const ExplorationMap& map() const noexcept { return map_; }

  void serialize(std::vector<std::byte>& out) const;
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

 private:
  /// Stamp every object that has moved or changed owner since it was last
  /// stamped.
  void sweep(World& world);

  ExplorationMap map_;
  /// Where each object stood, and for whom, when it was last stamped. A sight
  /// change is not here: the original stamps a widening itself (`SetSight`,
  /// the scout skill) and a narrowing not at all. Rebuilt by `prime` on a
  /// load; empty, it stamps everything, which is a new game's start.
  struct Stamp {
    Point position;
    PlayerId owner = kNoPlayer;
    friend bool operator==(const Stamp&, const Stamp&) = default;
  };
  [[nodiscard]] static Stamp stamp_of(const WorldObject& object) noexcept;
  std::unordered_map<ObjectId, Stamp> stamped_;
};

/// The world's fog system, or null.
[[nodiscard]] FogSystem* fog_system_of(World& world) noexcept;

/// The fog slot a script's **1-based** player number names, or -1.
///
/// 1..16 in, 0..15 out, the same conversion `player_from_script` makes; the
/// caller decides what to do with a slot at or past `ExplorationMap::kSlots`.
[[nodiscard]] std::int32_t fog_slot_from_script(std::int32_t player) noexcept;

/// The fog slots `player`'s stamps write: its record's `viewflags`
/// (`record + 0x14`), low byte. Its own slot (the record initialiser 0x00564660
/// writes it) plus every player its own relations row grants share-view
/// (bit 4) -- `SetRelation` (0x005652f0) keeps the word that way. Zero for an
/// invalid player.
[[nodiscard]] std::uint32_t fog_view_mask(const World& world, PlayerId player) noexcept;

/// The exploration merge a grant of shared view makes: 0x00564fa0.
///
/// `DiplShareView(granter, receiver, true)` (0x00565640) calls it after the
/// relation bit, and so does the Diplomacy screen's order when the word it
/// writes carries share-view (0x004e5f80); clearing the bit merges nothing.
/// It does not copy the granter's explored cells: it walks the object grid
/// over the whole map (0x00564cd0) and, for every object of **either** player
/// that is not a spawn template, stamps the circle of the object's sight about
/// its point into the **receiver's** slot (0x00517ad0, mask `1 << receiver`).
/// So the receiver sees at once what the granter's units see now -- not what
/// they saw before -- and its own units' circles again. Nothing is written for
/// the granter, and a receiver past the eighth player has no slot to write.
/// It runs on every call that sets the bit, whether the bit was already set or
/// not, and on `DiplShareView(p, p, true)`, whose relation write
/// `SetRelation` refuses.
void merge_shared_view(World& world, PlayerId granter, PlayerId receiver);

/// `IsExplored/2`, `ExploreArea/2`, `ExploreCircle/3`, `ExploreAll/0`,
/// `GetUnexploredPoint/1`. Returns how many were newly implemented.
std::size_t register_fog_host(script::HostRegistry& registry);

/// The number of `define` calls `register_fog_host` makes.
[[nodiscard]] std::size_t fog_host_entry_count() noexcept;

}  // namespace imperivm::core::sim
