#pragma once

// A node table for a test world that has no terrain.
//
// The partition (`GaikaTable::build`, docs/formats/gaika.md) is cut over the
// passability areas, and a world with no terrain layer has none, so it cuts
// nothing. The tests that only need *some* node per settlement -- to file a
// squad under, to send one to, to read a node's settlement back -- restore
// this table instead: one node per settlement, centred on its central
// building, in settlement order, and a slot grid that gives every slot to the
// node whose centre is nearest the slot's middle (the lower id on a tie).
// It is a fixture, not a claim about the original's cut; the partition's own
// tests are in test_gaika.cpp and test_corpus_gaika.py.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "imperivm/core/sim/gaika_table.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/world.hpp"
#include "test.hpp"

namespace imperivm::test {

inline void stand_in_nodes(core::sim::World& world, const core::sim::SettlementStore& settlements) {
  using namespace core::sim;
  std::vector<GaikaNode> nodes;
  std::int32_t columns = 64;
  std::int32_t rows = 64;
  for (const Settlement& s : settlements.all()) {
    GaikaNode node;
    node.center = world.resolve_position(s.anchor);
    node.settlement = s.object;
    const auto sx = static_cast<std::int16_t>(std::max(node.center.x, 0) / kLsaSlotSize);
    const auto sy = static_cast<std::int16_t>(std::max(node.center.y, 0) / kLsaSlotSize);
    node.rect = GaikaRect{sx, sy, sx, sy};
    columns = std::max(columns, sx + 2);
    rows = std::max(rows, sy + 2);
    nodes.push_back(node);
  }
  std::vector<std::uint16_t> slots(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows), 0);
  for (std::int32_t y = 0; y < rows; ++y) {
    for (std::int32_t x = 0; x < columns; ++x) {
      std::int64_t nearest = -1;
      std::uint16_t best = 0;
      for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::int64_t dx = static_cast<std::int64_t>(x) * kLsaSlotSize + kLsaSlotSize / 2 - nodes[i].center.x;
        const std::int64_t dy = static_cast<std::int64_t>(y) * kLsaSlotSize + kLsaSlotSize / 2 - nodes[i].center.y;
        const std::int64_t d2 = dx * dx + dy * dy;
        if (nearest < 0 || d2 < nearest) {
          nearest = d2;
          best = static_cast<std::uint16_t>(i + 1);
        }
      }
      slots[static_cast<std::size_t>(y) * static_cast<std::size_t>(columns) + static_cast<std::size_t>(x)] = best;
    }
  }
  REQUIRE(world.mutable_gaika().restore(std::move(nodes), columns, rows, std::move(slots)));
}

}  // namespace imperivm::test
