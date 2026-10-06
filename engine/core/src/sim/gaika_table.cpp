#include "imperivm/core/sim/gaika_table.hpp"

#include <utility>
#include <vector>

#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

void GaikaTable::build(const World& world, const LsaPartition& areas,
                       const SettlementStore& settlements) {
  nodes_.clear();

  // Settlements first, in store order, so a node's id is a function of the map
  // document rather than of anything that happens in play.
  std::vector<bool> covered(areas.size() + 1, false);
  for (const Settlement& s : settlements.all()) {
    const Point centre = world.resolve_position(s.anchor);
    // `at_or_near`, not `at`: a village's or an outpost's central building
    // stands on its own blocked footprint, so the cell under the anchor is no
    // area at all. With `at`, every such node had no LSA, `BestToSupply`
    // found no path to one and answered invalid, and no tribute flowed. The
    // original takes the LSA from the slot its partitioner stamped the node
    // into (`[node+0x10]`); the nearest labelled cell stands in for that, and
    // `LsaPartition::at_or_near` says how it is found.
    const LsaId lsa = areas.at_or_near(centre);
    if (lsa != kNoLsa) covered[static_cast<std::size_t>(lsa)] = true;
    nodes_.push_back(GaikaNode{centre, lsa, s.object});
  }

  // Then one node for every area nothing was built in, in area order.
  for (std::size_t i = 1; i <= areas.size(); ++i) {
    if (covered[i]) continue;
    const LsaArea* area = areas.find(static_cast<LsaId>(i));
    if (area == nullptr) continue;
    nodes_.push_back(GaikaNode{area->centroid, static_cast<LsaId>(i), kNoObject});
  }

  link(areas);
}

void GaikaTable::restore(std::vector<GaikaNode> nodes, const LsaPartition& areas) {
  nodes_ = std::move(nodes);
  link(areas);
}

/// 0x0044a3c0, on the slot grid `sim/lsa.hpp` keeps. See the header for what
/// carries over and what does not.
void GaikaTable::link(const LsaPartition& areas) {
  neighbour_starts_.assign(nodes_.size(), 0);
  neighbours_.clear();
  slot_nodes_.clear();
  slot_columns_ = 0;
  if (nodes_.empty()) return;

  // Every slot the fill labelled, credited to the node its centre falls in.
  // A slot nothing claimed -- off the map, all blocked, or poisoned by a
  // discarded area -- links nothing, exactly as an unstamped slot does there.
  //
  // **The centre is a choice, and a labelled one.** The original reads a node
  // id the partitioner stamped into the slot and never asks where in the slot
  // anything is; this table has no such stamp, so it asks `at`, and it asks at
  // the point the type census one level down samples (`sim/lsa.hpp`). Asking
  // at the slot's corner instead was injected as a fault and could not be made
  // to change a single pair on any picture tried -- a slot is 128 units and
  // the things it is deciding between are far larger.
  const std::int32_t columns = areas.slot_columns();
  const std::int32_t rows = areas.slot_rows();
  std::vector<GaikaId> slots(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows),
                             kNoGaika);
  for (std::int32_t sy = 0; sy < rows; ++sy) {
    for (std::int32_t sx = 0; sx < columns; ++sx) {
      if (areas.slot_label(sx, sy) == kNoLsa) continue;
      const Point centre{sx * kLsaSlotSize + kLsaSlotSize / 2,
                         sy * kLsaSlotSize + kLsaSlotSize / 2};
      slots[static_cast<std::size_t>(sy) * static_cast<std::size_t>(columns) +
            static_cast<std::size_t>(sx)] = at(areas, centre);
    }
  }

  // The eight offsets, in the original's own order -- which cannot matter,
  // because a pair is recorded in a matrix and read back out ascending.
  static constexpr std::int32_t kAround[8][2] = {{-1, 1}, {-1, 0}, {-1, -1}, {0, -1},
                                                 {1, -1}, {1, 0},  {1, 1},   {0, 1}};
  const std::size_t count = nodes_.size();
  std::vector<char> paired(count * count, 0);
  for (std::int32_t sy = 0; sy < rows; ++sy) {
    for (std::int32_t sx = 0; sx < columns; ++sx) {
      const GaikaId here =
          slots[static_cast<std::size_t>(sy) * static_cast<std::size_t>(columns) +
                static_cast<std::size_t>(sx)];
      if (here == kNoGaika) continue;
      for (const auto& step : kAround) {
        const std::int32_t nx = sx + step[0];
        const std::int32_t ny = sy + step[1];
        if (nx < 0 || ny < 0 || nx >= columns || ny >= rows) continue;
        const GaikaId there =
            slots[static_cast<std::size_t>(ny) * static_cast<std::size_t>(columns) +
                  static_cast<std::size_t>(nx)];
        if (there == kNoGaika || there == here) continue;
        const std::size_t a = static_cast<std::size_t>(here) - 1;
        const std::size_t b = static_cast<std::size_t>(there) - 1;
        if (paired[a * count + b] != 0) continue;
        // Both halves, as the original writes them. Writing only one is an
        // equivalence here and was injected as such: the offsets are
        // symmetric and both slots are labelled, so every pair is seen from
        // both sides anyway. It is not an equivalence *there*, where the same
        // loop also increments each node's count as it goes.
        paired[a * count + b] = 1;
        paired[b * count + a] = 1;
      }
    }
  }

  slot_columns_ = columns;
  slot_nodes_ = slots;

  // The matrix read off row by row, which is ascending by id.
  for (std::size_t a = 0; a < count; ++a) {
    for (std::size_t b = 0; b < count; ++b) {
      if (paired[a * count + b] != 0) neighbours_.push_back(static_cast<GaikaId>(b + 1));
    }
    neighbour_starts_[a] = static_cast<std::uint32_t>(neighbours_.size());
  }
}

std::span<const GaikaId> GaikaTable::neighbours(GaikaId id) const noexcept {
  if (id < 1 || static_cast<std::size_t>(id) > nodes_.size()) return {};
  const std::size_t at_index = static_cast<std::size_t>(id) - 1;
  const std::uint32_t from = at_index == 0 ? 0u : neighbour_starts_[at_index - 1];
  const std::uint32_t to = neighbour_starts_[at_index];
  return std::span<const GaikaId>(neighbours_).subspan(from, to - from);
}

GaikaId GaikaTable::for_settlement(ObjectId settlement) const noexcept {
  if (settlement == kNoObject) return kNoGaika;
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (nodes_[i].settlement == settlement) return static_cast<GaikaId>(i + 1);
  }
  return kNoGaika;
}

GaikaId GaikaTable::at(const LsaPartition& areas, Point where) const noexcept {
  // 0x0044e3f0 answers node 0 for either coordinate negative, before it
  // indexes its slot grid. See the header.
  if (where.x < 0 || where.y < 0) return kNoGaika;
  const LsaId here = areas.at(where);
  GaikaId best = kNoGaika;
  std::int64_t nearest = 0;
  GaikaId best_anywhere = kNoGaika;
  std::int64_t nearest_anywhere = 0;
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const std::int64_t dx = static_cast<std::int64_t>(nodes_[i].center.x) - where.x;
    const std::int64_t dy = static_cast<std::int64_t>(nodes_[i].center.y) - where.y;
    const std::int64_t d2 = dx * dx + dy * dy;
    if (best_anywhere == kNoGaika || d2 < nearest_anywhere) {
      best_anywhere = static_cast<GaikaId>(i + 1);
      nearest_anywhere = d2;
    }
    if (here == kNoLsa || nodes_[i].lsa != here) continue;
    if (best == kNoGaika || d2 < nearest) {
      best = static_cast<GaikaId>(i + 1);
      nearest = d2;
    }
  }
  return best != kNoGaika ? best : best_anywhere;
}

}  // namespace imperivm::core::sim
