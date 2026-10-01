#pragma once

/// The GAIKA node table -- **an approximation, deliberately taken**.
///
/// `sim/gaika.hpp` carries the decision, what it costs, and how to go back.
/// Read that first; this file is only the construction.
///
/// ## The rule
///
/// One node per settlement, then one node per connected region that no
/// settlement stands in.
///
///   * A **settlement node** takes the settlement's central building as its
///     `center` -- which is what `Settlement::pos` and `NearestStronghold`
///     already measure a settlement from -- its `lsa` from the area under that
///     point, and its `settlement` back-link from the settlement itself. That
///     makes `Settlement::GetGaika` and `GAIKA::settlement` exact inverses,
///     which is the one relation `CVXGlobalAI::Persist`'s `gaikaset`/`setgaika`
///     pair says the original also keeps both ways.
///   * A **region node** covers an area with nothing built in it -- open
///     country, an empty island, a lake -- and takes the area's centroid as its
///     `center`, `kNoObject` as its settlement.
///
/// Nodes are numbered from **1**, because `kNoGaika` is 0 and both shipped
/// walks over the id space (`PRIORITIZE.VS`, `GAIKAMONITOR.VS`) start there.
///
/// ## Why this shape and not another
///
/// The AI's own scripts say what a node is *for*: `GetGAIKAStrat.vs` asks
/// `g.settlement`, whether it is a stronghold, whether it can be captured;
/// `GetArmyNeed.vs` asks whether it is explored and what garrison it needs;
/// `SQUADMONITOR.VS` sends armies to one. Every one of those questions is about
/// a *place worth fighting over*, and on a shipped map that is a settlement or
/// it is the ground between settlements. A node per settlement therefore gets
/// the case the scripts actually branch on right, and the region nodes keep the
/// space total so that `GetGAIKA(point)` always has an answer.
///
/// **It is not the original's cut.** The original derives its nodes from a slot
/// grid at `slotresx` x `slotresy` (0x0044e060), which is finer than this and
/// which nothing recovered so far reproduces. The consequences are listed in
/// `sim/gaika.hpp`; the important one is that node *ids* differ, so no retail
/// save could be read against this table.
///
/// ## Standing
///
/// **Map-derived configuration, not turn state.** The table is a pure function
/// of the terrain layer and the settlement list, both of which a load rebuilds
/// before anything runs -- so it is built once, never saved and never hashed,
/// exactly as `GroupTable` and the class graph are. The per-player state a node
/// carries (priority, what has been explored) is a different thing entirely and
/// belongs to `AiSystem`, which is where it changes.

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

/// One node. Three fields, and each is a thing the original's `Persist` names.
struct GaikaNode {
  /// `GAIKA::Center` / `GetGaikaCenter`.
  Point center{};
  /// `GAIKA::LSA`.
  LsaId lsa = kNoLsa;
  /// `GAIKA::settlement`, and `kNoObject` for a region node. This is the
  /// settlement's own object handle, which is what `Settlement` values carry.
  ObjectId settlement = kNoObject;
};

class GaikaTable {
 public:
  /// Build the table. `settlements` may be empty, which yields region nodes
  /// only; `areas` may be empty, which yields settlement nodes only. Both empty
  /// is a table with no nodes, and every accessor answers "nothing".
  void build(const World& world, const LsaPartition& areas,
             const SettlementStore& settlements);

  /// The nodes in id order -- element `i` is node `i + 1` -- which is what a
  /// save writes.
  [[nodiscard]] std::span<const GaikaNode> nodes() const noexcept { return nodes_; }

  /// Adopt the nodes a save wrote and link them over `areas`, as `build`
  /// links the nodes it makes.
  ///
  /// **A load restores the nodes rather than rebuilding them**, because a
  /// settlement node's centre is where its central building stood when the
  /// table was built, and it stays there: `GAIKA::Center` is one of the
  /// fields the original's `Persist` names, so a town hall razed in play
  /// leaves its node where it was. `build` asks `World::resolve_position` of
  /// the anchor, and of an anchor that is gone that answers `kHeldPosition`,
  /// (-1,-1), in no area. A save taken after a stronghold fell loaded into a
  /// table with that node in the map's corner, and a squad `revalue_squads`
  /// files by nearest centre was filed under another node on the restored
  /// side: Crossroads saved at 1,600 turns diverged three turns after the
  /// load, when a priest marching on the fallen town lost its route.
  void restore(std::vector<GaikaNode> nodes, const LsaPartition& areas);

  void clear() noexcept {
    nodes_.clear();
    neighbour_starts_.clear();
    neighbours_.clear();
    slot_nodes_.clear();
    slot_columns_ = 0;
  }

  /// The number of **real** nodes, so the valid ids are `1 .. count()`.
  ///
  /// **This is not `GAIKACount()`**, and the comment that used to stand here
  /// said it was. The original's vector carries a reserved element 0 that is
  /// not a place, so `GAIKACount()` is one more than this and the shipped
  /// `for (i = 1; i < GAIKACount(); …)` walks visit every node -- rather than
  /// stopping one short, which is what this table's own count would have made
  /// them do. `sim/ai.cpp`'s `f_gaika_count` adds the one; see it for why a
  /// table with no nodes at all still answers 0.
  [[nodiscard]] std::int32_t count() const noexcept {
    return static_cast<std::int32_t>(nodes_.size());
  }

  [[nodiscard]] const GaikaNode* find(GaikaId id) const noexcept {
    return (id >= 1 && static_cast<std::size_t>(id) <= nodes_.size())
               ? &nodes_[static_cast<std::size_t>(id) - 1]
               : nullptr;
  }

  /// `Settlement::GetGaika`. `kNoGaika` for a settlement with no node, which
  /// cannot happen for a settlement the table was built over.
  [[nodiscard]] GaikaId for_settlement(ObjectId settlement) const noexcept;

  /// `GetGAIKA(point)`. The nearest node centre **within the point's own
  /// area**, so a point on an island is never answered with a node across the
  /// water; failing that -- the point is off the layer, or its area has no node
  /// -- the nearest node overall. Distance is the squared distance, and a tie
  /// keeps the lower id.
  [[nodiscard]] GaikaId at(const LsaPartition& areas, Point where) const noexcept;

  /// The nodes adjacent to `id`, ascending. Empty for an id that names no node.
  ///
  /// **Measured on the same 128-unit slot grid the original measures it on**,
  /// which `sim/lsa.hpp` already keeps and names "the original's GAIKA slot".
  /// 0x0044a3c0 walks every labelled slot and, for each of its **eight**
  /// neighbours (the offset table at 0x007b1678, in that order), makes the two
  /// slots' nodes adjacent when they differ and neither is zero; a pair is
  /// counted once, both ways, through an n-by-n matrix it then reads off in
  /// ascending order into each node's own list. Unlike the *area* adjacency
  /// one level down, there is no type test: a land node and a sea node that
  /// touch on the grid are neighbours.
  ///
  /// **What differs is only who owns a slot.** The original's partitioner
  /// stamps a node id into every slot; this table has no such pass, so a slot
  /// is credited to the node its centre falls in -- `at`, the same function
  /// `GetGAIKA(point)` answers with. A slot no area claimed links nothing, as
  /// it does there.
  [[nodiscard]] std::span<const GaikaId> neighbours(GaikaId id) const noexcept;

  /// Which node each 128-unit slot is credited to, row by row, `kNoGaika` for
  /// a slot no area claimed -- the same crediting `neighbours` is measured on,
  /// kept rather than thrown away after the link. Derived from the node list
  /// and the terrain, never saved: `restore` rebuilds it.
  [[nodiscard]] std::span<const GaikaId> slot_nodes() const noexcept { return slot_nodes_; }
  [[nodiscard]] std::int32_t slot_columns() const noexcept { return slot_columns_; }

 private:
  void link(const LsaPartition& areas);

  std::vector<GaikaNode> nodes_;
  /// Concatenated neighbour lists; node `i` owns `[starts_[i-1], starts_[i])`.
  std::vector<std::uint32_t> neighbour_starts_;
  std::vector<GaikaId> neighbours_;
  std::vector<GaikaId> slot_nodes_;
  std::int32_t slot_columns_ = 0;
};

}  // namespace imperivm::core::sim
