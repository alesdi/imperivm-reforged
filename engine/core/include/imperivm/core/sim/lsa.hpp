#pragma once

/// **LSA**: a connected region of the map, and the one thing in the GAIKA
/// family that is a measurement rather than an approximation.
///
/// `sim/gaika.hpp` records the decision this file half of. In short: the AI's
/// *node* partition is an approximation, because the original cuts it with a
/// slot grid this project cannot reproduce; the *area* partition is not,
/// because an LSA is a connected component of the map under a movement domain
/// and that is a computation, not a guess.
///
/// ## Why the numbering does not have to match
///
/// No shipped script ever writes an LSA id down or compares one against a
/// literal. Every one of the 65 uses passes an id straight from a producer to a
/// consumer -- `IsWaterLsa(g.LSA)`, `CheckLsaPath(sq.GAIKAIn.LSA, g.LSA, p)` --
/// so what the scripts read is the *relation* between ids (same area, different
/// area, this area is water) and never the id itself. That makes the labelling
/// free to be this engine's own, the way `ObjListId` and `ScriptId` are.
///
/// ## The rule, and it is the original's
///
/// `gbr.exe` builds the partition at map load in `0x00453130`, and this is what
/// it does:
///
///   1. Take a **private copy of the passability bitmap** -- 16-unit cells, one
///      bit each, a set bit meaning blocked. The fill *sets* bits as it
///      consumes cells, so "blocked" doubles as "already claimed" and one
///      surface serves both jobs.
///   2. Run the labelling **twice**: the **water pass first**, then the ground
///      pass. A free cell seeds an area only if the terrain byte under it
///      matches the pass -- deep water is terrain index **13**, and shallow
///      water counts as ground.
///   3. Each seed grows by **four-connected** flood fill over the *passability*
///      surface, in world order, ignoring terrain entirely from that point on.
///      So an area is bounded by what blocks movement, not by the coastline;
///      the terrain test only decides which pass may start there.
///   4. **An area whose *coverage* falls below 256 is discarded** -- freed, its
///      cells released, and its id not consumed. Coverage is not the cell
///      count: the counter advances once per visited cell **after the first one
///      in each 128-unit slot**, so it is `cells - slots touched`. A long thin
///      area spread over many slots therefore has to be larger to survive than
///      a compact one.
///   5. An area's **type is a majority vote**, not the pass that made it: the
///      terrain is sampled at the centre of every 128-unit slot the area
///      covers, and the area is water when more than half of them are deep
///      water. So the pass proposes and the census disposes.
///
/// The scan is row-major with x innermost from the origin, and an area takes
/// the next id, so the labelling is a function of the two layers alone.
///
/// ## The fill never tests terrain, and the coastline is in the asset
///
/// This looks wrong until you measure the data, and the measurement is the
/// interesting part. `0x00452250` has **no** comparison against 13, no
/// reference to the terrain layer, and does not receive the pass flag: each of
/// its four neighbour blocks is a bounds check and one passability bit, and
/// nothing else. Read literally, the water pass should seed at the first
/// deep-water cell and flood the whole map.
///
/// It does not, because **the shipped passability grids wall the coast**. Over
/// the scenarios that have one, *every* shoreline cell -- every 16-unit cell
/// with a four-neighbour of the opposite deep-water-ness -- is blocked:
/// 11,162 of 11,162 on `Island War`, 21,998 of 21,998 on `Balcans`. Deep water
/// itself is **not** blocked (5.1% of its cells, against 10.2% of land's, which
/// is trees and cliffs), so the barrier is a one-cell line and not a region.
/// Simulating the algorithm over that data gives areas that are 0% or 100%
/// deep water with no exceptions -- four seas and two landmasses on `Balcans`,
/// which is the shape `IsWaterLsa`, `Ships` and `CheckLsaPath` are written
/// against.
///
/// So the two passes do not keep land and sea apart; the map does. What they
/// decide is which component is claimed first, and the type census then has
/// almost nothing left to disagree about -- its real job is the coastal
/// *slot*, which any touching cell can claim while its centre samples the other
/// side.
///
/// **Ids are dense from 1 and 0 is a reserved sentinel** whose record the
/// original explicitly zeroes -- which is why `IsWaterLsa(0)` answers false
/// without a bounds check. The type field's domain is exactly `{0 sentinel,
/// 1 water, 2 ground}`, and `LsaStr` prints those as `"water"` and `"ground"`.
///
/// ## The slot grid, and who owns a slot
///
/// Beside the cell labels the original keeps a second labelling at **128-unit
/// slot** resolution, and it is the one the type census and the adjacency
/// lists are computed over. A slot belongs to **the first area whose fill
/// touches any cell in it** -- the fill (0x00452250) ends by walking every
/// slot it touched and claiming each one that no earlier fill has claimed --
/// and since every fill runs to completion before the next seed is tried,
/// "first" is the lowest id. Two things follow that are easy to get wrong:
///
///   * **A discarded area still claims its slots.** The pass driver
///     (0x00452800) clears the slot *labels* that carry the dead id, but the
///     claim marks stay set, so a slot a too-small pond touched first is owned
///     by nobody for the rest of the build and no landmass around it ever
///     gets it. Reproduced, because the adjacency below is read off these
///     labels and a poisoned slot is a slot that links nothing.
///   * The census votes one slot per label, sampling the terrain at the slot's
///     centre -- so a coastal slot the sea claimed first casts a *land* vote
///     for the sea, and a lake small enough to be mostly coastline can be
///     typed ground. That is the original's, and this partition now does the
///     same rather than voting by whichever area owns the slot centre.
///
/// ## Adjacency is measured on the slot grid, between types
///
/// 0x00452a60 builds the per-area neighbour lists after the census: for every
/// labelled slot and each of its **eight** neighbouring slots, the two labels
/// are made adjacent when they differ, both are live, and **their types
/// differ**. Two ground areas are never neighbours, and neither are two seas;
/// the graph `CheckLsaPath` walks alternates land and water by construction,
/// and the walled coastline (see above) is what lets a land slot and a sea
/// slot sit side by side at all. Lists are ascending by id, which is the order
/// the original's matrix-to-list pass produces.
///
/// ## What this is not
///
/// It is not the original's *numbering* -- see above on why that is free -- and
/// it does not carry the area-to-area distance matrix the original keeps
/// beside each record (`dists`): nothing in the assigned entry points reads
/// it. Neither are the per-player `shipneed` / `shipcount` arrays here; those
/// live on `sim/ai.hpp`.
///
/// It is **not saved**: it is a pure function of the terrain and passability
/// layers, so a load rebuilds it exactly, the way the class graph and the group
/// table are rebuilt rather than restored.
///
/// **`SlotLSA.bmp` and `DetailLSA.bmp` are not read and do not ship.** Each has
/// exactly one cross-reference in the executable, both as arguments to
/// 0x00453130, which is `ret 8` in the retail build and never touches either --
/// the residue of a debug dump. No such file exists anywhere in an install.
/// That is what settles "computed" against "authored".

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

/// An LSA id. **0 is "no area"**, matching every other index space in this
/// engine that a script can see.
using LsaId = std::int32_t;
inline constexpr LsaId kNoLsa = 0;

/// Deep water, in the terrain layer's own numbering.
inline constexpr std::uint32_t kDeepWaterTerrain = 13;

/// The smallest *coverage* the original keeps: below this an area is erased
/// back to "no area" (0x00452800). Coverage is `cells - slots touched`; see
/// the header.
inline constexpr std::int32_t kMinLsaCoverage = 256;

/// The slot the type census votes over -- 128 world units, eight passability
/// cells or two terrain cells on a side. It is also the original's GAIKA slot.
inline constexpr std::int32_t kLsaSlotSize = 128;

/// How far `LsaPartition::at_or_near` looks for walkable ground: four slots,
/// thirty-two passability cells. The largest central building in the shipped
/// classes is well inside it; the number is this engine's.
inline constexpr std::int32_t kLsaNearReach = 512;

/// One connected region.
struct LsaArea {
  /// Deep water, decided by the majority of the area's slot centres rather
  /// than by the pass that built it.
  bool water = false;
  /// The mean of the area's cell centres, truncated -- what a node built over
  /// this area uses for its `Center`. Not one of the original's fields; the
  /// original picks a representative point during the fill instead, and that
  /// choice is inside the part of the pipeline this project does not
  /// reproduce.
  Point centroid{};
  std::int32_t cells = 0;
};

/// The whole partition.
class LsaPartition {
 public:
  /// Label the map into areas. Needs both layers: the passability surface the
  /// fill spreads over, and the terrain layer that decides which pass may seed
  /// where and how the type census votes.
  ///
  /// A world with either layer missing produces nothing, which is what every
  /// synthetic test that has neither gets.
  void build(const Grid& terrain, const ObstructionGrid& passability);

  [[nodiscard]] std::size_t size() const noexcept { return areas_.size(); }
  /// Areas are `1 .. size()`; `kNoLsa` is not one.
  [[nodiscard]] const LsaArea* find(LsaId id) const noexcept {
    return (id >= 1 && static_cast<std::size_t>(id) <= areas_.size())
               ? &areas_[static_cast<std::size_t>(id) - 1]
               : nullptr;
  }
  /// The area a world point falls in, or `kNoLsa` off the map, on a blocked
  /// cell, or in an area that was too small to keep. Cells are the
  /// passability grid's 16-unit ones, floored -- `ObstructionGrid::cell_of`,
  /// which is the same mapping every path in this engine uses.
  [[nodiscard]] LsaId at(Point where) const noexcept;
  /// `at`, or -- when that is `kNoLsa` -- the first labelled cell on square
  /// rings of growing radius around `where`'s cell, out to `kLsaNearReach`
  /// world units.
  ///
  /// **For a point standing on its own obstruction**, which is where every
  /// building is: a village's or an outpost's central building sits on its
  /// blocked footprint, so `at` of its position is `kNoLsa`. The original
  /// never asks the question this way -- a node's LSA comes from the slot the
  /// partitioner stamped it into (`[node+0x10]`), and a slot is labelled by the
  /// ground around the building, not under it -- so this is a stand-in for
  /// that, and a labelled one.
  ///
  /// **The scan order is fixed and is the tie-break.** Ring `r` (in 16-unit
  /// cells) is walked row by row from the top, `dy` from `-r` to `r` and `dx`
  /// from `-r` to `r` within a row, perimeter cells only, and the first
  /// labelled cell wins. That is the nearest ring and not the nearest cell: a
  /// corner of ring 3 beats an edge of ring 4, and two labelled cells on one
  /// ring are decided by position in that walk. Deterministic either way,
  /// which is what a node's LSA has to be.
  [[nodiscard]] LsaId at_or_near(Point where) const noexcept;
  /// `IsWaterLsa(id)`. An id that names no area is not water.
  [[nodiscard]] bool water(LsaId id) const noexcept {
    const LsaArea* area = find(id);
    return area != nullptr && area->water;
  }

  /// The areas adjacent to `id`, ascending -- see the header: neighbours are
  /// of the other type, and touch on the slot grid. Empty for an id that names
  /// no area.
  [[nodiscard]] std::span<const LsaId> neighbours(LsaId id) const noexcept;

  /// The 128-unit slot grid: the area that claimed slot `(sx, sy)`, or
  /// `kNoLsa` for a slot nothing claimed, a slot a discarded area poisoned, or
  /// one off the grid.
  [[nodiscard]] LsaId slot_label(std::int32_t sx, std::int32_t sy) const noexcept;
  [[nodiscard]] std::int32_t slot_columns() const noexcept { return slot_columns_; }
  [[nodiscard]] std::int32_t slot_rows() const noexcept { return slot_rows_; }

 private:
  std::int32_t width_ = 0;   ///< in 16-unit passability cells
  std::int32_t height_ = 0;
  std::vector<LsaId> labels_;  ///< row-major, one per passability cell
  std::vector<LsaArea> areas_;
  std::int32_t slot_columns_ = 0;  ///< in 128-unit slots
  std::int32_t slot_rows_ = 0;
  std::vector<LsaId> slot_labels_;  ///< row-major, one per slot
  /// Concatenated neighbour lists; area `i` owns `[starts_[i-1], starts_[i])`.
  std::vector<std::uint32_t> neighbour_starts_;
  std::vector<LsaId> neighbours_;
};

}  // namespace imperivm::core::sim
