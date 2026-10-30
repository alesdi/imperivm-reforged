#pragma once

/// The GAIKA node table: the original's partition of the map into nodes.
///
/// `docs/formats/gaika.md`, "How the table is built", describes the build
/// 0x0044e050 makes when the AI manager starts, step by step; `build` below is
/// written from that description and follows its numbering. In one paragraph:
/// a 128-unit slot grid the size of the passability areas' (`sim/lsa.hpp`);
/// seeds from every settlement's buildings, snapped to the best ground slot
/// near their land entrance, then from every slot no settlement reached; a
/// weighted claim of each slot by the nearest, heaviest seed; one node per
/// settlement over the slots its seeds won in the central building's area,
/// and one per connected piece of every other seed's; a smoothing pass over
/// the node grid; small region nodes folded into a neighbour; then each
/// node's coverage, neighbours and, for a region, its centre.
///
/// Node ids, rectangles, order, coverage and neighbours are the original's for
/// a map. The table is integer-only and walks nothing unordered.
///
/// ## Standing
///
/// **Turn state that is built once.** The partition is cut over the world as
/// the match starts -- its passability, terrain, and its settlements' buildings
/// and entrances -- and is then kept as it is: a town hall razed in play leaves
/// its node and its slots where they were. A save therefore writes the records
/// and the slot grid (the original's `CVXGlobalAI::Persist` names `GAIKAs`,
/// `slotresx`, `slotresy` and `GAIKASlots`), and a load restores them rather
/// than cutting the restored world again. It is not hashed; what is filed
/// under a node is.

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/sim/gaika.hpp"
#include "imperivm/core/sim/lsa.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

class World;
class SettlementStore;

/// A node's rectangle on the slot grid, inclusive, as the record keeps it:
/// four 16-bit coordinates (`+0x00`..`+0x06`). Empty when `right < left` or
/// `bottom < top`.
struct GaikaRect {
  std::int16_t left = 0;
  std::int16_t top = 0;
  std::int16_t right = -1;
  std::int16_t bottom = -1;

  friend bool operator==(const GaikaRect&, const GaikaRect&) = default;
};

/// One node record (`0x24` bytes in the original, `docs/formats/gaika.md`).
struct GaikaNode {
  /// `GAIKA::Center` / `GetGaikaCenter` (`+0x08`), in world units.
  Point center{};
  /// `GAIKA::LSA` (`+0x10`).
  LsaId lsa = kNoLsa;
  /// `GAIKA::settlement` (`+0x14`), `kNoObject` for a region node. This is the
  /// settlement's own object handle, which is what `Settlement` values carry.
  ObjectId settlement = kNoObject;
  /// The node's rectangle in slots (`+0x00`).
  GaikaRect rect{};
  /// How many slots of the grid carry the node's id (`+0x1c`).
  std::int32_t coverage = 0;
};

class GaikaTable {
 public:
  /// Cut the map (0x0044e050). `areas` must have been built over the same
  /// world; its slot grid is the table's. An empty partition -- a world with
  /// no terrain or no passability layer -- yields no nodes.
  void build(const World& world, const LsaPartition& areas,
             const SettlementStore& settlements);

  /// The real nodes in id order -- element `i` is node `i + 1` -- which is
  /// what a save writes.
  [[nodiscard]] std::span<const GaikaNode> nodes() const noexcept { return nodes_; }

  /// Adopt the nodes and the slot grid a save wrote. Coverage and neighbours
  /// are a function of the slot grid (0x0044a3c0) and are counted again from
  /// it. False, and the table left empty, when the grid does not fit its
  /// dimensions or names a node that is not there.
  [[nodiscard]] bool restore(std::vector<GaikaNode> nodes, std::int32_t columns,
                             std::int32_t rows, std::vector<std::uint16_t> slots);

  void clear() noexcept {
    nodes_.clear();
    neighbour_starts_.clear();
    neighbours_.clear();
    slots_.clear();
    columns_ = 0;
    rows_ = 0;
    built_ = false;
  }

  /// The number of **real** nodes, so the valid ids are `1 .. count()`.
  ///
  /// **This is not `GAIKACount()`**: the original's vector carries the record
  /// 0 that is not a place, so `GAIKACount()` is one more than this and the
  /// shipped `for (i = 1; i < GAIKACount(); ...)` walks visit every node.
  /// `sim/ai.cpp`'s `f_gaika_count` adds the one; see it for why a table with
  /// no nodes at all still answers 0.
  [[nodiscard]] std::int32_t count() const noexcept {
    return static_cast<std::int32_t>(nodes_.size());
  }

  [[nodiscard]] const GaikaNode* find(GaikaId id) const noexcept {
    return (id >= 1 && static_cast<std::size_t>(id) <= nodes_.size())
               ? &nodes_[static_cast<std::size_t>(id) - 1]
               : nullptr;
  }

  /// `find`, and record 0 for id 0 once the table has been built or restored
  /// -- the placeholder the original keeps at the head of its vector, whose
  /// centre is `(-1, -1)` and which names no area and no settlement. A reader
  /// that goes through 0x0044e8e0 with id 0 reads that record; before the AI
  /// manager exists the readers answer from their own "no manager" arm.
  [[nodiscard]] const GaikaNode* record(GaikaId id) const noexcept {
    if (id == kNoGaika) return built_ ? &kRecordZero : nullptr;
    return find(id);
  }
  [[nodiscard]] bool built() const noexcept { return built_; }

  /// Record 0 as 0x0044cff0 makes it.
  static constexpr GaikaNode kRecordZero{Point{-1, -1}, kNoLsa, kNoObject, GaikaRect{0, 0, -1, -1}, 0};

  /// `Settlement::GetGaika` (0x0044e9e0, the manager's per-settlement entry).
  /// `kNoGaika` for a settlement the build placed no node for: one whose
  /// central building is none of the five settlement classes, or whose
  /// central building found no ground slot to stand on.
  [[nodiscard]] GaikaId for_settlement(ObjectId settlement) const noexcept;

  /// `GetGAIKA(point)`: the node id the slot grid holds under `where`
  /// (0x0044e3f0). `kNoGaika` when `x` is -1 or either coordinate is negative.
  ///
  /// The original tests neither far edge: it reads `(y >> 7) * columns +
  /// (x >> 7)`, each shift cut to 16 bits, so a point past the right edge
  /// reads a slot of the next row, which this reproduces. An index past the
  /// whole grid reads beyond the original's array; here it is `kNoGaika`.
  ///
  /// `areas` is not read; it is kept so the callers need not change.
  [[nodiscard]] GaikaId at(const LsaPartition& areas, Point where) const noexcept;
  [[nodiscard]] GaikaId at(Point where) const noexcept;

  /// The nodes adjacent to `id`, ascending (0x0044a3c0). Empty for an id that
  /// names no node.
  [[nodiscard]] std::span<const GaikaId> neighbours(GaikaId id) const noexcept;

  /// The slot grid, row by row: each 128-unit slot's node id, 0 for none.
  [[nodiscard]] std::span<const std::uint16_t> slot_nodes() const noexcept { return slots_; }
  [[nodiscard]] std::int32_t slot_columns() const noexcept { return columns_; }
  [[nodiscard]] std::int32_t slot_rows() const noexcept { return rows_; }

 private:
  /// Step 9: coverage and neighbour lists from the slot grid.
  void link();

  std::vector<GaikaNode> nodes_;
  /// Concatenated neighbour lists; node `i` owns `[starts_[i-1], starts_[i])`.
  std::vector<std::uint32_t> neighbour_starts_;
  std::vector<GaikaId> neighbours_;
  std::vector<std::uint16_t> slots_;
  std::int32_t columns_ = 0;
  std::int32_t rows_ = 0;
  bool built_ = false;
};

}  // namespace imperivm::core::sim
