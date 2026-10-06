// The region partition and the AI's node table over it.
//
// No game data anywhere in this file: every terrain layer here is written out
// by hand, cell by cell, so the properties asserted are properties of the
// algorithm rather than of any map.
//
// `sim/gaika.hpp` carries the decision these two files exist because of, and
// says which of them is a measurement and which is an approximation. The tests
// follow that split: the LSA cases assert exact areas, and the node cases
// assert the *rule* -- one node per settlement, then one per empty region --
// rather than any particular cut.

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/script/ast.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/gaika.hpp"
#include "imperivm/core/sim/gaika_table.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/lsa.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "builder.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::sim;

namespace {

constexpr std::uint32_t kTerrainCell = 64;
constexpr std::uint32_t kTerrainCells = 16;              // a 1024-unit map
constexpr std::uint32_t kPassCells = kTerrainCells * 4;  // 16-unit cells

std::span<const std::byte> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A terrain layer from a picture, one character per 64-unit cell. `~` is deep
/// water (index 13) and anything else is land.
///
/// **The pictures here are 16 cells on a side and their features are five
/// cells across**, which is not decoration: an area is discarded below 256
/// passability cells, and a passability cell is a quarter of a terrain cell on
/// each axis -- so the smallest area the partition keeps is 16 terrain cells,
/// and a 2x2 island would vanish.
std::vector<std::byte> terrain_from(std::initializer_list<std::string_view> rows) {
  imperivm::test::Builder out;
  const auto height = static_cast<std::uint32_t>(rows.size());
  const auto width = static_cast<std::uint32_t>(rows.begin()->size());
  out.text(kGridMagic).u32(kTerrainCell).u32(8).u32(kTerrainCell * width).u32(kTerrainCell * height);
  for (const std::string_view row : rows) {
    for (const char c : row) out.u8(c == '~' ? 13 : 1);
  }
  return {out.span().begin(), out.span().end()};
}

/// A passability layer that blocks every 16-unit cell whose terrain is deep
/// water. **Not what a real map does** -- see `pass_blocking_shore` below --
/// but it is the simplest way to give the node cases two separate landmasses
/// and no sea to be in.
std::vector<std::byte> pass_blocking_water(std::initializer_list<std::string_view> rows) {
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(16).u32(1).u32(16 * kPassCells).u32(16 * kPassCells);
  const std::uint32_t stride = (kPassCells + 7) / 8;
  std::vector<std::string_view> picture(rows);
  for (std::uint32_t y = 0; y < kPassCells; ++y) {
    for (std::uint32_t byte = 0; byte < stride; ++byte) {
      std::uint32_t value = 0;
      for (std::uint32_t bit = 0; bit < 8; ++bit) {
        const std::uint32_t x = byte * 8 + bit;
        if (x >= kPassCells) continue;
        if (picture[y / 4][x / 4] == '~') value |= 1u << bit;
      }
      out.u8(value);
    }
  }
  return {out.span().begin(), out.span().end()};
}

/// **What a real map does**: block the shoreline, one 16-unit cell wide, and
/// leave the open water passable.
///
/// Measured over the shipped scenarios and it is exact: every cell with a
/// four-neighbour of the opposite deep-water-ness is blocked -- 11,162 of
/// 11,162 on `Island War`, 21,998 of 21,998 on `Balcans` -- while deep water
/// itself is only 5% blocked, which is indistinguishable from land's 10% of
/// trees and cliffs. That one-cell line is the whole reason the fill can
/// ignore terrain and still produce areas that are pure land or pure water.
std::vector<std::byte> pass_blocking_shore(std::initializer_list<std::string_view> rows) {
  std::vector<std::string_view> picture(rows);
  const auto wet = [&](std::int32_t x, std::int32_t y) {
    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(kTerrainCells) ||
        y >= static_cast<std::int32_t>(kTerrainCells)) {
      return false;
    }
    return picture[static_cast<std::size_t>(y)][static_cast<std::size_t>(x)] == '~';
  };
  // Off the map is not a coastline: without this the whole border walls itself
  // and the sea is cut off from its own edge.
  const auto same_or_edge = [&](std::int32_t x, std::int32_t y, bool here) {
    if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(kTerrainCells) ||
        y >= static_cast<std::int32_t>(kTerrainCells)) {
      return here;
    }
    return wet(x, y);
  };
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(16).u32(1).u32(16 * kPassCells).u32(16 * kPassCells);
  const std::uint32_t stride = (kPassCells + 7) / 8;
  for (std::uint32_t y = 0; y < kPassCells; ++y) {
    for (std::uint32_t byte = 0; byte < stride; ++byte) {
      std::uint32_t value = 0;
      for (std::uint32_t bit = 0; bit < 8; ++bit) {
        const auto x = static_cast<std::int32_t>(byte * 8 + bit);
        if (x >= static_cast<std::int32_t>(kPassCells)) continue;
        const bool here = wet(x / 4, static_cast<std::int32_t>(y) / 4);
        // A shoreline cell is one whose four-neighbour disagrees about water.
        const auto ty = static_cast<std::int32_t>(y) / 4;
        const bool shore = same_or_edge(x / 4 - 1, ty, here) != here ||
                           same_or_edge(x / 4 + 1, ty, here) != here ||
                           same_or_edge(x / 4, ty - 1, here) != here ||
                           same_or_edge(x / 4, ty + 1, here) != here;
        if (shore) value |= 1u << bit;
      }
      out.u8(value);
    }
  }
  return {out.span().begin(), out.span().end()};
}

/// A passability layer with nothing blocked, unless `wall_x` names a column of
/// 16-unit cells to block from top to bottom.
std::vector<std::byte> pass_layer(std::int32_t wall_x = -1) {
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(16).u32(1).u32(16 * kPassCells).u32(16 * kPassCells);
  const std::uint32_t stride = (kPassCells + 7) / 8;
  for (std::uint32_t y = 0; y < kPassCells; ++y) {
    for (std::uint32_t byte = 0; byte < stride; ++byte) {
      std::uint32_t value = 0;
      for (std::uint32_t bit = 0; bit < 8; ++bit) {
        if (wall_x >= 0 && static_cast<std::int32_t>(byte * 8 + bit) == wall_x) value |= 1u << bit;
      }
      out.u8(value);
    }
  }
  return {out.span().begin(), out.span().end()};
}

ObstructionGrid obstruction(const std::vector<std::byte>& bytes) {
  const Result<Grid> parsed = Grid::parse(bytes);
  CHECK(parsed.ok());
  if (!parsed.ok()) return ObstructionGrid{};
  Result<ObstructionGrid> grid = ObstructionGrid::from_grid(parsed.value());
  CHECK(grid.ok());
  if (!grid.ok()) return ObstructionGrid{};
  return std::move(grid.value());
}

/// The world point at the middle of a 64-unit terrain cell. The partition
/// itself works in 16-unit cells, so a terrain cell's middle is unambiguous
/// where its corner sits on a boundary.
Point in_cell(std::int32_t cx, std::int32_t cy) {
  return Point{static_cast<std::int32_t>(cx * kTerrainCell + kTerrainCell / 2),
               static_cast<std::int32_t>(cy * kTerrainCell + kTerrainCell / 2)};
}

/// Two five-cell islands in one sea, touching nowhere.
constexpr std::initializer_list<std::string_view> kTwoIslands = {
    "~~~~~~~~~~~~~~~~", "~.....~~~~~~~~~~", "~.....~~~~~~~~~~", "~.....~~~~~~~~~~",
    "~.....~~~~~~~~~~", "~.....~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
    "~~~~~~~~~~~~~~~~", "~~~~~~~~~.....~~", "~~~~~~~~~.....~~", "~~~~~~~~~.....~~",
    "~~~~~~~~~.....~~", "~~~~~~~~~.....~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
};

/// The node cases' map: a small western island and a **tall** eastern one whose
/// centroid is a long way south. That shape is what makes the region rule
/// measurable -- a point on the eastern island's north-west corner is nearer to
/// the western island's node than to its own island's centroid, so a plain
/// nearest-centre rule would answer across the water.
constexpr std::initializer_list<std::string_view> kNodeIslands = {
    "~~~~~~~~~~~~~~~~", "~.....~.....~~~~", "~.....~.....~~~~", "~.....~.....~~~~",
    "~.....~.....~~~~", "~.....~.....~~~~", "~~~~~~~.....~~~~", "~~~~~~~.....~~~~",
    "~~~~~~~.....~~~~", "~~~~~~~.....~~~~", "~~~~~~~.....~~~~", "~~~~~~~.....~~~~",
    "~~~~~~~.....~~~~", "~~~~~~~.....~~~~", "~~~~~~~.....~~~~", "~~~~~~~~~~~~~~~~",
};

}  // namespace

// --------------------------------------------------------------------------
// the partition
// --------------------------------------------------------------------------

/// Four-connected components of the passability surface, in 16-unit cells,
/// with the terrain deciding which of the two passes may seed where.
///
/// **The layer here blocks deep water**, as a real map's does -- land units
/// cannot walk on it -- and that matters for what this test can claim. On such
/// a map the two readings of 0x00452250 agree, so what is asserted below holds
/// either way. Whether the fill *also* tests terrain per neighbour, and so
/// whether a map with passable water yields one area or three, is the one thing
/// about this routine still open; see `sim/lsa.hpp`.
TEST(lsa_labels_the_passable_map_into_connected_regions) {
  const std::vector<std::byte> terrain = terrain_from(kTwoIslands);
  const std::vector<std::byte> pass = pass_blocking_water(kTwoIslands);
  const Result<Grid> layer = Grid::parse(terrain);
  REQUIRE(layer.ok());
  LsaPartition areas;
  areas.build(layer.value(), obstruction(pass));

  REQUIRE(areas.size() == 2);
  CHECK(!areas.water(1));
  CHECK(!areas.water(2));

  CHECK(areas.at(in_cell(3, 3)) == 1);
  CHECK(areas.at(in_cell(11, 11)) == 2);
  CHECK(areas.at(in_cell(3, 3)) != areas.at(in_cell(11, 11)));
  // The sea is impassable, so it is in no area -- which is also what makes
  // `IsWaterLsa` answer false for it without a special case.
  CHECK(areas.at(in_cell(0, 0)) == kNoLsa);

  // Each island is 5x5 terrain cells, which is 20x20 passability cells.
  CHECK(areas.find(1)->cells == 400);
  CHECK(areas.find(2)->cells == 400);

  // A centroid lands back in its own area, which is the property it is for.
  CHECK(areas.at(areas.find(1)->centroid) == 1);
  CHECK(areas.at(areas.find(2)->centroid) == 2);

  // Off the map, and ids nobody minted.
  CHECK(areas.at(Point{-500, -500}) == kNoLsa);
  CHECK(areas.at(Point{100000, 0}) == kNoLsa);
  CHECK(areas.find(0) == nullptr);
  CHECK(areas.find(3) == nullptr);
  CHECK(!areas.water(0));
}

/// **The arrangement a real map actually has**, and the reason the fill can
/// ignore terrain: the shoreline is one impassable cell wide, the open water is
/// not, and the areas come out pure anyway.
///
/// This is the case that settles what `0x00452250` does. The fill has no
/// terrain test at all -- four neighbour blocks, each a bounds check and one
/// passability bit -- and read on its own it should flood the whole map from
/// the first water seed. It does not, because the coast is walled in the asset.
/// The numbers behind that are in `sim/lsa.hpp`.
TEST(lsa_keeps_land_and_sea_apart_because_the_map_walls_the_coast) {
  // One seven-cell island, so that what is left inside its walled shore is a
  // 5x5 block -- 400 passability cells, comfortably over the threshold. A
  // five-cell island would lose its whole interior to the ring.
  static constexpr std::initializer_list<std::string_view> kBigIsland = {
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~.......~~~~~~",
      "~~~.......~~~~~~", "~~~.......~~~~~~", "~~~.......~~~~~~", "~~~.......~~~~~~",
      "~~~.......~~~~~~", "~~~.......~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
  };
  const std::vector<std::byte> terrain = terrain_from(kBigIsland);
  const std::vector<std::byte> pass = pass_blocking_shore(kBigIsland);
  const Result<Grid> layer = Grid::parse(terrain);
  REQUIRE(layer.ok());
  LsaPartition areas;
  areas.build(layer.value(), obstruction(pass));

  // Open water is passable, and the two islands still do not join it.
  REQUIRE(areas.at(Point{16, 16}) != kNoLsa);
  const LsaId sea = areas.at(Point{16, 16});
  CHECK(areas.water(sea));

  // The island's interior is its own area: its outer ring is the walled shore,
  // so what survives is the 5x5 of terrain cells inside it.
  const LsaId island = areas.at(in_cell(6, 6));
  CHECK(island != kNoLsa);
  CHECK(island != sea);
  CHECK(!areas.water(island));
  CHECK(areas.find(island)->cells == 400);
  // And the shore itself belongs to neither.
  CHECK(areas.at(in_cell(3, 6)) == kNoLsa);

  REQUIRE(areas.size() == 2);
}

/// And an all-water map with everything passable is one water area, which is
/// the half of the type census that a land-only map cannot show.
TEST(lsa_types_an_area_by_the_terrain_under_it) {
  const std::vector<std::byte> sea = terrain_from({
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
  });
  const Result<Grid> layer = Grid::parse(sea);
  REQUIRE(layer.ok());
  LsaPartition areas;
  areas.build(layer.value(), obstruction(pass_layer()));
  REQUIRE(areas.size() == 1);
  CHECK(areas.water(1));
  CHECK(areas.find(1)->cells == 64 * 64);
}

/// **A wall cuts an area in two, and a coastline inside one pass does not.**
///
/// This is the case that separates the passability reading from the terrain
/// reading, and they disagree here in both directions: the blocked column
/// splits one landmass into two areas, while the shallow-water strip -- any
/// terrain that is not index 13 -- leaves its area whole.
TEST(lsa_splits_on_what_blocks_movement_and_not_on_the_coastline) {
  // All land, so there is one pass and one area unless something splits it.
  const std::vector<std::byte> terrain = terrain_from({
      "................", "................", "................", "................",
      "................", "................", "................", "................",
      "................", "................", "................", "................",
      "................", "................", "................", "................",
  });
  const Result<Grid> layer = Grid::parse(terrain);
  REQUIRE(layer.ok());
  LsaPartition areas;

  areas.build(layer.value(), obstruction(pass_layer()));
  REQUIRE(areas.size() == 1);
  CHECK(areas.find(1)->cells == 64 * 64);

  // Now block passability column 32 -- world x 512..527 -- from top to bottom.
  areas.build(layer.value(), obstruction(pass_layer(32)));
  REQUIRE(areas.size() == 2);
  CHECK(areas.at(Point{100, 100}) == 1);
  CHECK(areas.at(Point{900, 900}) == 2);
  CHECK(areas.at(Point{520, 100}) == kNoLsa);  // the wall itself is in no area
  CHECK(areas.find(1)->cells + areas.find(2)->cells == 64 * 64 - 64);
}

/// An area under 256 cells is thrown away, and its id is never issued -- so the
/// numbering stays dense and a pond is not a place.
TEST(lsa_discards_an_area_too_small_to_be_a_place) {
  // One 2x2-cell island: 8x8 = 64 passability cells, well under the threshold.
  const std::vector<std::byte> terrain = terrain_from({
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~..~~~~~~~~", "~~~~~~..~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
  });
  const Result<Grid> layer = Grid::parse(terrain);
  REQUIRE(layer.ok());
  LsaPartition areas;
  areas.build(layer.value(), obstruction(pass_blocking_water({
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~..~~~~~~~~", "~~~~~~..~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
  })));

  // Nothing survives: the sea is impassable and the island is too small, so
  // the id was never issued.
  CHECK(areas.size() == 0);
  CHECK(areas.at(in_cell(6, 4)) == kNoLsa);
  CHECK(areas.at(in_cell(0, 0)) == kNoLsa);
}

/// The size threshold is measured against **coverage**, not the cell count --
/// `cells - slots touched` -- so a long thin area has to be bigger to survive
/// than a compact one.
///
/// The strip below is one terrain cell wide and the full height of the map:
/// exactly 256 passability cells, which clears a plain count, and 8 slots, so
/// its coverage is 248 and it does not.
TEST(lsa_measures_the_size_threshold_against_coverage_and_not_cells) {
  static constexpr std::initializer_list<std::string_view> kStrip = {
      ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~",
      ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~",
      ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~",
      ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~", ".~~~~~~~~~~~~~~~",
  };
  const std::vector<std::byte> terrain = terrain_from(kStrip);
  const Result<Grid> layer = Grid::parse(terrain);
  REQUIRE(layer.ok());
  LsaPartition areas;
  // Water blocked, so the strip is the only candidate and nothing else can
  // absorb it.
  areas.build(layer.value(), obstruction(pass_blocking_water(kStrip)));

  // 4 x 64 = 256 cells over 8 slots: coverage 248, and it is thrown away.
  CHECK(areas.size() == 0);
  CHECK(areas.at(in_cell(0, 8)) == kNoLsa);
}

/// An area's type is a **majority** of its slot centres, not a single vote.
///
/// With nothing blocked the whole map is one area -- the fill never tests
/// terrain, so it crosses the coast freely -- and a third of it is deep water.
/// One water slot is not enough to call the whole thing a sea.
TEST(lsa_types_an_area_by_a_majority_of_its_slots) {
  const std::vector<std::byte> terrain = terrain_from({
      "~~~~~...........", "~~~~~...........", "~~~~~...........", "~~~~~...........",
      "~~~~~...........", "~~~~~...........", "~~~~~...........", "~~~~~...........",
      "~~~~~...........", "~~~~~...........", "~~~~~...........", "~~~~~...........",
      "~~~~~...........", "~~~~~...........", "~~~~~...........", "~~~~~...........",
  });
  const Result<Grid> layer = Grid::parse(terrain);
  REQUIRE(layer.ok());
  LsaPartition areas;
  areas.build(layer.value(), obstruction(pass_layer()));

  REQUIRE(areas.size() == 1);
  // Slot centres land on terrain columns 1, 3, 5, 7, ... -- two of the eight
  // are water, so the area is ground.
  CHECK(!areas.water(1));
  // And it really does span the coast, which is what makes the vote necessary.
  CHECK(areas.at(in_cell(1, 1)) == 1);
  CHECK(areas.at(in_cell(14, 14)) == 1);
}

/// A world missing either layer has no areas, and every accessor says so
/// rather than falling over. Every synthetic test in the suite is that world.
TEST(lsa_with_no_layers_is_empty_and_answers_nothing) {
  LsaPartition areas;
  areas.build(Grid{}, ObstructionGrid{});
  CHECK(areas.size() == 0);
  CHECK(areas.at(Point{0, 0}) == kNoLsa);
  CHECK(!areas.water(1));
  CHECK(areas.find(1) == nullptr);

  // A passability grid with no terrain beside it is the same nothing: the
  // seeding test has no answer without a terrain byte.
  const std::vector<std::byte> pass = pass_layer();
  areas.build(Grid{}, obstruction(pass));
  CHECK(areas.size() == 0);
}

/// **`at_or_near`: the cell's own area, or the nearest ring's first labelled
/// cell, out to 512 units.** For a point standing on its own obstruction --
/// every central building -- where `at` answers no area.
///
/// Checked against an independent statement of the same rule over every cell
/// of the map and forty cells of margin around it: among the labelled cells no
/// more than 32 cells away in either axis, the one with the least Chebyshev
/// distance, then the least `dy`, then the least `dx`. The ring walk is one
/// way to compute that; a minimum over a set is another.
namespace {

/// Two islands side by side along one shore line, so that a cell below the
/// strait finds both on the top row of one ring: the order *within* a row is
/// what decides it.
constexpr std::initializer_list<std::string_view> kSideBySide = {
    "~~~~~~~~~~~~~~~~", "~.....~.....~~~~", "~.....~.....~~~~", "~.....~.....~~~~",
    "~.....~.....~~~~", "~.....~.....~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
    "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
    "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
};

LsaPartition blocking_water_partition(std::initializer_list<std::string_view> picture) {
  const std::vector<std::byte> terrain = terrain_from(picture);
  const Result<Grid> layer = Grid::parse(terrain);
  CHECK(layer.ok());
  const std::vector<std::byte> pass = pass_blocking_water(picture);
  LsaPartition areas;
  if (layer.ok()) areas.build(layer.value(), obstruction(pass));
  return areas;
}

}  // namespace

TEST(lsa_at_or_near_takes_the_nearest_rings_first_labelled_cell) {
  constexpr std::int32_t kCells = 64;  // 1,024 units of 16-unit cells
  constexpr std::int32_t kReach = kLsaNearReach / kCollisionCellSize;
  REQUIRE(kReach == 32);
  const auto centre = [](std::int32_t cx, std::int32_t cy) {
    return Point{cx * 16 + 8, cy * 16 + 8};
  };

  for (const auto picture : {kTwoIslands, kSideBySide}) {
    const LsaPartition areas = blocking_water_partition(picture);
    REQUIRE(areas.size() == 2);
    const auto oracle = [&](std::int32_t cx, std::int32_t cy) {
      LsaId best = kNoLsa;
      std::int32_t best_r = 0;
      std::int32_t best_dy = 0;
      std::int32_t best_dx = 0;
      for (std::int32_t y = 0; y < kCells; ++y) {
        for (std::int32_t x = 0; x < kCells; ++x) {
          const LsaId label = areas.at(centre(x, y));
          if (label == kNoLsa) continue;
          const std::int32_t dx = x - cx;
          const std::int32_t dy = y - cy;
          const std::int32_t r = std::max(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy);
          if (r > kReach) continue;
          if (best == kNoLsa || r < best_r || (r == best_r && dy < best_dy) ||
              (r == best_r && dy == best_dy && dx < best_dx)) {
            best = label;
            best_r = r;
            best_dy = dy;
            best_dx = dx;
          }
        }
      }
      return best;
    };

    std::int32_t fallen_back = 0;
    std::int32_t out_of_reach = 0;
    std::int32_t mismatches = 0;
    for (std::int32_t cy = -40; cy < kCells + 40; ++cy) {
      for (std::int32_t cx = -40; cx < kCells + 40; ++cx) {
        const LsaId got = areas.at_or_near(centre(cx, cy));
        if (got != oracle(cx, cy)) ++mismatches;
        if (areas.at(centre(cx, cy)) == kNoLsa && got != kNoLsa) ++fallen_back;
        if (got == kNoLsa) ++out_of_reach;
      }
    }
    CHECK(mismatches == 0);
    CHECK(fallen_back > 0);
    CHECK(out_of_reach > 0);
  }

  // **Two ties, both settled by the walk**, which is the tie-break and so is
  // named here rather than left to the oracle above.
  //
  // Between rows: cell (29, 30) of `kTwoIslands` is seven cells from the west
  // island's corner (23, 23) and seven from the east island's (36, 36). The
  // ring is walked from its top row, so the west island wins.
  {
    const LsaPartition areas = blocking_water_partition(kTwoIslands);
    const LsaId west = areas.at(centre(20, 20));
    const LsaId east = areas.at(centre(40, 40));
    REQUIRE(west != kNoLsa);
    REQUIRE(east != kNoLsa);
    REQUIRE(west != east);
    REQUIRE(areas.at(centre(23, 23)) == west);
    REQUIRE(areas.at(centre(36, 36)) == east);
    REQUIRE(areas.at(centre(29, 30)) == kNoLsa);
    CHECK(areas.at_or_near(centre(29, 30)) == west);
    // A labelled cell is its own answer, whatever is nearer on the ring.
    CHECK(areas.at_or_near(centre(23, 23)) == west);
  }
  // Within a row: below the strait of `kSideBySide`, ten cells down from the
  // islands' shared shore line at y = 23, the top row of ring 10 holds both
  // islands; a row is walked from the left, so the west one wins.
  {
    const LsaPartition areas = blocking_water_partition(kSideBySide);
    const LsaId west = areas.at(centre(10, 10));
    const LsaId east = areas.at(centre(40, 10));
    REQUIRE(west != kNoLsa);
    REQUIRE(east != kNoLsa);
    REQUIRE(west != east);
    REQUIRE(areas.at(centre(23, 23)) == west);
    REQUIRE(areas.at(centre(28, 23)) == east);
    REQUIRE(areas.at(centre(25, 33)) == kNoLsa);
    CHECK(areas.at_or_near(centre(25, 33)) == west);
  }

  // An empty partition has nothing near anything.
  LsaPartition none;
  CHECK(none.at_or_near(Point{100, 100}) == kNoLsa);
}

// --------------------------------------------------------------------------
// the node table
// --------------------------------------------------------------------------

namespace {

/// A world with the picture above as its terrain, plus whatever settlements a
/// case plants on it.
struct NodeBench {
  ClassGraph graph;
  std::vector<std::byte> terrain = terrain_from(kNodeIslands);
  std::vector<std::byte> pass = pass_blocking_water(kNodeIslands);
  World world;
  EconomySystem economy;

  NodeBench() {
    graph.add(bytes_of(
                  R"(<class id="Townhall" cpp_class="CVXTownHall"><properties maxhealth="1000"/></class>)"),
              "townhall.sc.xml");
    graph.link();
    world.set_class_graph(&graph);
    const Result<Grid> layer = Grid::parse(terrain);
    REQUIRE(layer.ok());
    world.set_terrain(layer.value());
    REQUIRE(world.add_system(&economy));
    economy.start(world);
    world.mutable_lsa().build(world.terrain(), obstruction(pass));
  }

  /// A settlement whose central building stands on a cell.
  ///
  /// The kind is a parameter because `GetDistToPlayers` measures only
  /// strongholds -- the original filters on the central building being a heir
  /// of `BaseTownhall` -- so a test of that filter needs a settlement that is
  /// not one. Every other caller here wants the default and says nothing.
  SettlementId plant(std::int32_t cx, std::int32_t cy, PlayerId owner = 1,
                     SettlementKind kind = SettlementKind::stronghold) {
    const ObjectId anchor = world.spawn(NativeClass::town_hall, nullptr, graph.find("Townhall"));
    CHECK(world.set_position(anchor, in_cell(cx, cy)));
    SettlementInit init;
    init.kind = kind;
    init.anchor = anchor;
    init.owner = owner;
    return economy.create(world, init);
  }

  void rebuild() { world.mutable_gaika().build(world, world.lsa(), economy.settlements()); }
};

/// `NodeBench` with squads on it. Derived rather than folded in, on
/// `SpawnFeedFixture`'s precedent: a system added to a shared fixture changes
/// what a save carries, and the node tests above have nothing to do with heroes.
struct ArmyBench : NodeBench {
  HeroSystem heroes;

  ArmyBench() {
    // Two unit classes under one base, so `IsHeirOf` has something to be right
    // about: the entry point matches by descent, not by name.
    graph.add(bytes_of(R"(<class id="Military" cpp_class="CVXUnit" parent=""/>)"),
              "military.sc.xml");
    graph.add(bytes_of(R"(<class id="Legionary" cpp_class="CVXUnit" parent="Military"/>)"),
              "legionary.sc.xml");
    graph.add(bytes_of(R"(<class id="Archer" cpp_class="CVXUnit" parent="Military"/>)"),
              "archer.sc.xml");
    // A *building* under the same base, so the "is it a unit" test can be made
    // to bite on its own: with a building class outside the branch the class
    // filter would refuse it first and the flag test would never be reached.
    graph.add(bytes_of(R"(<class id="Fort" cpp_class="CVXBuilding" parent="Military"/>)"),
              "fort.sc.xml");
    graph.link();
    REQUIRE(world.add_system(&heroes));
  }

  ObjectId soldier(const char* class_name, PlayerId owner) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, graph.find(class_name));
    CHECK(world.set_owner(id, owner));
    CHECK(world.set_health(id, 100));
    return id;
  }

  ObjectId fort(PlayerId owner) {
    const ObjectId id = world.spawn(NativeClass::building, nullptr, graph.find("Fort"));
    CHECK(world.set_owner(id, owner));
    CHECK(world.set_health(id, 100));
    return id;
  }

  /// A squad in `node`, with strength, owned by `owner`, holding `members`.
  SquadKey band(PlayerId owner, GaikaId node, std::vector<ObjectId> members) {
    const SquadKey key = heroes.squads().create(owner);
    for (const ObjectId id : members) CHECK(heroes.squads().join(key, id));
    Squad* squad = heroes.squads().find(key);
    CHECK(squad != nullptr);
    if (squad != nullptr) {
      squad->gaika_in = node;
      // Strength is the first filter and it is stored-but-unfilled in this
      // engine, so every test here has to set it by hand. See the body.
      squad->eval = 10;
    }
    return key;
  }
};

}  // namespace

/// One node per settlement, then one per region nothing was built in.
///
/// This is the approximation, and the test asserts the *rule* rather than any
/// particular cut -- `sim/gaika.hpp` says why the cut itself is not a claim.
/// What is claimed, and what breaks if it stops holding: node ids are stable
/// (settlement order, then area order), every settlement has exactly one node,
/// and `GetGaika` and `GAIKA::settlement` are inverses.
TEST(gaika_gives_every_settlement_a_node_and_every_empty_region_one_too) {
  NodeBench b;
  // Water is impassable here, so the areas are the two islands and nothing
  // else -- the sea is not a place an army can be.
  REQUIRE(b.world.lsa().size() == 2);
  const SettlementId west = b.plant(3, 3);   // on the western island
  b.rebuild();
  const GaikaTable& table = b.world.gaika();

  // One settlement node, and the eastern island gets a region node because
  // nothing was built on it.
  REQUIRE(table.count() == 2);
  const Settlement* w = b.economy.settlements().find(west);
  REQUIRE(w != nullptr);

  CHECK(table.for_settlement(w->object) == 1);
  REQUIRE(table.find(1) != nullptr);
  CHECK(table.find(1)->settlement == w->object);
  CHECK(table.find(1)->center == in_cell(3, 3));
  CHECK(table.find(1)->lsa == b.world.lsa().at(in_cell(3, 3)));

  REQUIRE(table.find(2) != nullptr);
  CHECK(table.find(2)->settlement == kNoObject);
  CHECK(table.find(2)->lsa == b.world.lsa().at(in_cell(9, 9)));
  CHECK(table.find(2)->lsa != table.find(1)->lsa);

  // A second settlement, on the island that had only a region node: it becomes
  // node 2 and the region node is gone, because the region is no longer empty.
  const SettlementId east = b.plant(9, 9);
  b.rebuild();
  REQUIRE(table.count() == 2);
  const Settlement* e = b.economy.settlements().find(east);
  REQUIRE(e != nullptr);
  CHECK(table.for_settlement(e->object) == 2);
  CHECK(table.find(2)->settlement == e->object);

  // Ids are 1-based and out of range is nothing, which is `kNoGaika`'s rule.
  CHECK(table.find(0) == nullptr);
  CHECK(table.find(3) == nullptr);
  CHECK(table.for_settlement(kNoObject) == kNoGaika);
  CHECK(table.for_settlement(static_cast<ObjectId>(9999)) == kNoGaika);
}

/// `GetGAIKA(point)` prefers a node in the point's own area, and the map is
/// built so that a plain nearest-centre rule would answer across the water.
///
/// The eastern island is tall, so its region node sits far to the south. Its
/// north-west corner is **nearer to the western island's settlement node** than
/// to its own island's centroid -- and the answer is still its own island's.
TEST(gaika_at_a_point_stays_in_the_points_own_region) {
  NodeBench b;
  b.plant(5, 5);  // node 1, on the western island's south-east corner
  b.rebuild();
  const GaikaTable& table = b.world.gaika();
  REQUIRE(table.count() == 2);

  const Point corner = in_cell(7, 1);  // the eastern island's north-west corner
  const Point west_node = table.find(1)->center;
  const Point east_node = table.find(2)->center;
  const auto d2 = [](Point a, Point c) {
    const std::int64_t dx = a.x - c.x;
    const std::int64_t dy = a.y - c.y;
    return dx * dx + dy * dy;
  };
  // The premise, asserted rather than assumed: without the region rule this
  // point would answer node 1.
  REQUIRE(d2(corner, west_node) < d2(corner, east_node));
  CHECK(table.at(b.world.lsa(), corner) == 2);
  CHECK(table.find(table.at(b.world.lsa(), corner))->settlement == kNoObject);

  // And a point on the western island answers its own node.
  CHECK(table.at(b.world.lsa(), in_cell(2, 2)) == 1);

  // Out at sea there is no area to prefer -- water is impassable, so the sea is
  // in no area at all -- and the nearest node overall wins rather than nothing.
  // `GetGAIKA` has no "no answer" for the corpus to test.
  REQUIRE(b.world.lsa().at(in_cell(6, 1)) == kNoLsa);
  CHECK(table.at(b.world.lsa(), in_cell(6, 1)) != kNoGaika);

  // **Two nodes in one area**, which is the case that pins "nearest" rather
  // than merely "in the same region": with one node per area the region rule
  // alone would answer everything.
  NodeBench two;
  two.plant(2, 2);
  two.plant(4, 4);
  two.rebuild();
  const GaikaTable& pair = two.world.gaika();
  REQUIRE(pair.count() == 3);  // two settlements, plus the empty eastern island
  REQUIRE(pair.find(1)->lsa == pair.find(2)->lsa);
  CHECK(pair.at(two.world.lsa(), in_cell(1, 1)) == 1);
  CHECK(pair.at(two.world.lsa(), in_cell(5, 5)) == 2);
}

/// A map with no settlements is all region nodes, and a map with no terrain is
/// all settlement nodes. Neither is a degenerate case that answers nothing.
TEST(gaika_covers_a_map_with_only_regions_and_one_with_only_settlements) {
  NodeBench b;
  b.rebuild();
  // Two islands, no settlements: two region nodes.
  CHECK(b.world.gaika().count() == 2);
  CHECK(b.world.gaika().find(1)->settlement == kNoObject);
  CHECK(b.world.gaika().find(2)->settlement == kNoObject);

  World bare;
  LsaPartition none;
  none.build(Grid{}, ObstructionGrid{});
  EconomySystem economy;
  REQUIRE(bare.add_system(&economy));
  economy.start(bare);
  SettlementInit init;
  init.kind = SettlementKind::stronghold;
  init.owner = 1;
  (void)economy.create(bare, init);
  GaikaTable table;
  table.build(bare, none, economy.settlements());
  CHECK(table.count() == 1);
  CHECK(table.find(1)->lsa == kNoLsa);
  // And a table over nothing at all still answers rather than reading past its
  // own end.
  GaikaTable empty;
  CHECK(empty.count() == 0);
  CHECK(empty.find(1) == nullptr);
  CHECK(empty.at(none, Point{0, 0}) == kNoGaika);
  CHECK(empty.for_settlement(static_cast<ObjectId>(1)) == kNoGaika);
}

// --------------------------------------------------------------------------
// the entry points
// --------------------------------------------------------------------------

/// The six readers, through the registry, on a map with two islands and one
/// settlement -- which is the smallest world in which every one of them has
/// something to say.
TEST(gaika_entry_points_read_the_table_the_way_the_scripts_do) {
  NodeBench b;
  const SettlementId west = b.plant(3, 3);
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 2);
  const Settlement* w = b.economy.settlements().find(west);
  REQUIRE(w != nullptr);

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const auto call = [&](script::CallKind kind, const char* name,
                        std::vector<script::Value> args) {
    const std::uint32_t index =
        registry.find(kind, name,
                      static_cast<std::uint16_t>(kind == script::CallKind::member
                                                     ? args.size() - 1
                                                     : args.size()));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    CHECK(registry.entry(index).fn != nullptr);
    if (registry.entry(index).fn == nullptr) {
      return script::HostOutcome::failed("not implemented");
    }
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    return registry.entry(index).fn(ctx);
  };
  const auto gaika = [](GaikaId id) { return script::Value::integer(id); };

  // **`GAIKACount` is one more than the number of real nodes**, because the
  // original counts a reserved node 0 that is not a place -- which is what
  // makes the shipped `for (i = 1; i < GAIKACount; i += 1)` visit every node
  // and `GAIKACount() - 1` name the last one.
  CHECK(call(script::CallKind::free_function, "GAIKACount", {}).value.as_integer() == 3);
  CHECK(b.world.gaika().count() == 2);  // and the table still counts only real ones

  // `set.GetGaika` and `g.settlement` are inverses.
  const script::HostOutcome mine = call(script::CallKind::member, "GetGaika",
                                        {script::Value::object(kTypeSettlement, w->object)});
  CHECK(mine.value.as_integer() == 1);
  const script::HostOutcome back = call(script::CallKind::member, "settlement", {gaika(1)});
  REQUIRE(back.value.is_object());
  CHECK(back.value.as_object().id == w->object);
  CHECK(back.value.as_object().type == kTypeSettlement);
  // The region node has no settlement, and `GetGAIKAStrat.vs` branches on it.
  const script::HostOutcome none = call(script::CallKind::member, "settlement", {gaika(2)});
  CHECK(none.value.is_object());
  CHECK(none.value.as_object().type == script::kNoType);
  // And an *object* receiver still reaches `Building::settlement`, which shares
  // the key: one body, two registrations, told apart by the argument.
  const script::HostOutcome building =
      call(script::CallKind::member, "settlement",
           {script::Value::object(kTypeObj, w->anchor)});
  CHECK(building.value.is_object());
  CHECK(building.value.as_object().id == w->object);

  // `g.Center` is the node's own centre: the settlement's central building for
  // a settlement node, the area's centroid for a region node.
  const script::HostOutcome centre = call(script::CallKind::member, "Center", {gaika(1)});
  CHECK(is_point(centre.value));
  CHECK(unpack_point(centre.value) == in_cell(3, 3));
  CHECK(unpack_point(call(script::CallKind::member, "Center", {gaika(2)}).value) ==
        b.world.gaika().find(2)->center);
  // A GAIKA that names no node answers (0,0) rather than a sentinel: every
  // shipped site feeds the result into a distance or a `Goto`.
  CHECK(unpack_point(call(script::CallKind::member, "Center", {gaika(99)}).value) ==
        (Point{0, 0}));

  // `g.LSA` is the node's area, and `IsWaterLsa` reads it. Water is impassable
  // on this map, so both islands are ground.
  const std::int32_t lsa = call(script::CallKind::member, "LSA", {gaika(1)}).value.as_integer();
  CHECK(lsa == b.world.lsa().at(in_cell(3, 3)));
  CHECK(lsa != kNoLsa);
  CHECK(!call(script::CallKind::free_function, "IsWaterLsa",
              {script::Value::integer(lsa)})
             .value.truthy_scalar());
  // Nothing is water here, so the *positive* case needs its own world.
  CHECK(!call(script::CallKind::free_function, "IsWaterLsa", {script::Value::integer(0)})
             .value.truthy_scalar());
  CHECK(call(script::CallKind::member, "LSA", {gaika(99)}).value.as_integer() == kNoLsa);

  // `GetGAIKA` is three registrations at one arity. An integer is the identity;
  // a point and an object both ask which node covers a place.
  CHECK(call(script::CallKind::free_function, "GetGAIKA", {script::Value::integer(7)})
            .value.as_integer() == 7);
  CHECK(call(script::CallKind::free_function, "GetGAIKA", {pack_point(in_cell(3, 3))})
            .value.as_integer() == 1);
  CHECK(call(script::CallKind::free_function, "GetGAIKA", {pack_point(in_cell(9, 9))})
            .value.as_integer() == 2);
  CHECK(call(script::CallKind::free_function, "GetGAIKA",
             {script::Value::object(kTypeObj, w->anchor)})
            .value.as_integer() == 1);
}

/// `g.GetDistToPlayers(player, &own, &ally, &enemy)` -- the three nearest town
/// halls by side, with a crossing penalty and a sentinel that is not zero.
TEST(get_dist_to_players_measures_town_halls_by_side_and_doubles_a_crossing) {
  NodeBench b;
  // The receiver is a **village**, so it has a node like every settlement does
  // and is itself not measured: the walk keeps only strongholds. Planted first
  // so that its node is id 1.
  b.plant(3, 3, 0, SettlementKind::village);
  // **Planted in an order the answers can see.** Each side's nearest home is
  // not the last one the walk meets -- player 0's is at 64 units and its second
  // is at 181, planted afterwards -- so a walk that kept the newest rather than
  // the least would be visibly wrong. And the three answers are three different
  // numbers, so a pair of them being swapped is visible too. Both were
  // survivors of the fault sweep before this layout replaced one where own and
  // ally were equidistant.
  b.plant(2, 3, 0);  // own,   64
  b.plant(1, 3, 1);  // ally, 128
  b.plant(9, 9, 2);  // enemy, 543 on the other island
  b.plant(1, 1, 0);  // own,  181 -- farther, and later
  b.rebuild();

  PlayerTable& players = b.world.players();
  // One-directional, as `is_enemy` is: player 0 grants player 1 a ceasefire and
  // grants player 2 nothing. The transposes are left at zero deliberately --
  // the original reads the *asking* player's row and nothing else.
  players.set_relation_word(0, 1, kRelationFriendly);
  players.set_relation_word(0, 2, 0);

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  // `(own, ally, enemy)` for a receiver and a 1-based player number.
  struct Answer {
    std::int32_t own = 0;
    std::int32_t ally = 0;
    std::int32_t enemy = 0;
  };
  const std::uint32_t index =
      registry.find(script::CallKind::member, "GetDistToPlayers", 4);
  REQUIRE(index != script::kUnresolvedHost);
  REQUIRE(registry.entry(index).fn != nullptr);
  const auto ask = [&](GaikaId node, std::int32_t player) {
    std::vector<script::Value> args{script::Value::integer(node),
                                    script::Value::integer(player),
                                    script::Value::integer(7),
                                    script::Value::integer(7),
                                    script::Value::integer(7)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "GetDistToPlayers";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return Answer{args[2].as_integer(), args[3].as_integer(), args[4].as_integer()};
  };

  // The receiver's centre is `in_cell(3, 3)`. `isqrt` floors: 64 straight along
  // one axis, 128 along one axis, and 384 on each axis is 543 -- charged twice,
  // because that home is on the other island.
  const Answer seen = ask(1, 1);
  CHECK(seen.own == 64);
  CHECK(seen.ally == 128);
  CHECK(seen.enemy == 2 * 543);

  // The penalty is per settlement rather than per answer: an enemy home on the
  // receiver's own island is measured straight and wins the minimum.
  b.plant(4, 4, 2);
  b.rebuild();
  const Answer nearer = ask(1, 1);
  CHECK(nearer.enemy == 90);
  CHECK(nearer.own == 64);
  CHECK(nearer.ally == 128);

  // A village is not a home. These two are nearer than either side's nearest
  // stronghold and change nothing.
  b.plant(3, 4, 1, SettlementKind::village);
  b.plant(3, 2, 2, SettlementKind::village);
  b.rebuild();
  const Answer unchanged = ask(1, 1);
  CHECK(unchanged.own == nearer.own);
  CHECK(unchanged.ally == nearer.ally);
  CHECK(unchanged.enemy == nearer.enemy);

  // **&minus;1, not zero, for a side with no home.** Player 4 owns nothing and
  // is allied to nobody, and `ESH_GOLDTRADE.VS`'s `if (nEDist < 0) nEDist =
  // 32000` is what reads this: a zero would make "no enemy anywhere" the most
  // attractive answer its ranking can produce rather than the least.
  const Answer stranger = ask(1, 4);
  CHECK(stranger.own == -1);
  CHECK(stranger.ally == -1);
  CHECK(stranger.enemy == 64);

  // A player number outside 1..16 classifies nothing, and neither does a
  // receiver that names no node. Neither traps.
  for (const Answer nothing : {ask(1, 0), ask(1, 17), ask(99, 1)}) {
    CHECK(nothing.own == -1);
    CHECK(nothing.ally == -1);
    CHECK(nothing.enemy == -1);
  }
}

/// `Settlement::BestToSupply` -- the nearest town hall of the **same owner** in
/// the **same area**, and the area is the reason it lives here.
///
/// `0x0042c800` compares `[node + 0x10]`, which is `GAIKA::LSA`, and not node
/// identity. That distinction is load-bearing in this engine and nowhere else:
/// the node partition here is an approximation with one node per settlement, so
/// a comparison by node would answer nothing on every map, while the LSA
/// partition is measured. The three exclusions below are each placed *nearer*
/// than the answer, so dropping any one of them changes it.
TEST(best_to_supply_takes_the_nearest_same_owner_town_hall_in_the_same_area) {
  NodeBench b;
  // The receiver sits on the west island's east edge, where an east-island
  // stronghold is nearer than a west-island one.
  const SettlementId from = b.plant(5, 3, 0, SettlementKind::village);
  const SettlementId answer = b.plant(1, 3, 0);                            // 256, west
  const SettlementId across = b.plant(7, 3, 0);                            // 128, east
  const SettlementId nearer_village = b.plant(4, 3, 0, SettlementKind::village);  // 64
  const SettlementId nearer_foreign = b.plant(5, 2, 1);                    // 64, player 1
  // A second settlement that passes every test, farther away, so that the walk
  // is choosing rather than taking the only survivor.
  const SettlementId farther = b.plant(1, 1, 0);                           // 286, west
  b.rebuild();

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index = registry.find(script::CallKind::member, "BestToSupply", 0);
  REQUIRE(index != script::kUnresolvedHost);
  REQUIRE(registry.entry(index).fn != nullptr);

  const auto object_of = [&](SettlementId id) -> ObjectId {
    const Settlement* s = b.economy.settlements().find(id);
    CHECK(s != nullptr);
    return s == nullptr ? kNoObject : s->object;
  };
  const auto best = [&](SettlementId id) -> ObjectId {
    std::vector<script::Value> args{script::Value::object(kTypeSettlement, object_of(id))};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "BestToSupply";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    if (!out.value.is_object() || out.value.as_object().type == script::kNoType) {
      return kNoObject;
    }
    return out.value.as_object().id;
  };

  CHECK(best(from) == object_of(answer));
  (void)across;
  (void)nearer_village;
  (void)nearer_foreign;
  (void)farther;

  // **The receiver is not excluded.** A stronghold asking this of itself is
  // zero units from itself and wins; the original has no self test and neither
  // shipped caller can reach the branch, because one is a village and the other
  // an outpost.
  CHECK(best(answer) == object_of(answer));

  // **A settlement whose centre stands on blocked ground takes the area of the
  // nearest labelled cell** (`LsaPartition::at_or_near`, a labelled reading):
  // every village's central building stands on its own footprint, and with
  // `at` alone every village had no area and no supply target, so no tribute
  // ever flowed. Cell (0, 1) is sea one cell off the west island, so the
  // village there supplies the west island's nearest town hall, `farther` at
  // (1, 1) -- and not `across`, on the other island.
  const SettlementId shore = b.plant(0, 1, 0, SettlementKind::village);
  b.rebuild();
  REQUIRE(b.world.lsa().at(in_cell(0, 1)) == kNoLsa);
  const GaikaNode* node = b.world.gaika().find(b.world.gaika().for_settlement(object_of(shore)));
  REQUIRE(node != nullptr);
  CHECK(node->lsa == b.world.lsa().at(in_cell(1, 1)));
  CHECK(node->lsa != kNoLsa);
  CHECK(best(shore) == object_of(farther));
}

/// `IsWaterLsa` answering **true**, which needs a map with navigable water --
/// and the type census deciding it, which needs an area that spans a coast.
TEST(gaika_is_water_lsa_answers_for_a_sea) {
  World world;
  const std::vector<std::byte> sea = terrain_from({
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
  });
  const Result<Grid> layer = Grid::parse(sea);
  REQUIRE(layer.ok());
  world.set_terrain(layer.value());
  const std::vector<std::byte> pass = pass_layer();
  world.mutable_lsa().build(world.terrain(), obstruction(pass));
  REQUIRE(world.lsa().size() == 1);

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &world;
  const auto is_water = [&](std::int32_t id) {
    const std::uint32_t index =
        registry.find(script::CallKind::free_function, "IsWaterLsa", 1);
    CHECK(index != script::kUnresolvedHost);
    script::CallContext ctx;
    std::vector<script::Value> args{script::Value::integer(id)};
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "IsWaterLsa";
    ctx.kind = script::CallKind::free_function;
    return registry.entry(index).fn(ctx).value.truthy_scalar();
  };
  CHECK(is_water(1));
  // 0 is the reserved non-area, and the original answers false for it without
  // a bounds check because its record is explicitly zeroed.
  CHECK(!is_water(0));
  CHECK(!is_water(2));
  CHECK(!is_water(-1));
}

// --------------------------------------------------------------------------
// GAIKA::GetAIControlledUnits
// --------------------------------------------------------------------------

namespace {

/// One call into `gaika.GetAIControlledUnits(class, player, num, peaceful)`,
/// answered as the list of object ids it filled.
std::vector<ObjectId> controlled(ArmyBench& b, GaikaId node, const char* class_name,
                                 std::int32_t player, std::int32_t num, bool peaceful) {
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index =
      registry.find(script::CallKind::member, "GetAIControlledUnits", 4);
  CHECK(index != script::kUnresolvedHost);
  if (index == script::kUnresolvedHost) return {};
  CHECK(registry.entry(index).fn != nullptr);
  if (registry.entry(index).fn == nullptr) return {};
  std::vector<script::Value> args{
      script::Value::integer(node), script::Value::string(class_name),
      script::Value::integer(player), script::Value::integer(num),
      script::Value::boolean(peaceful)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.script = 1;
  ctx.name = "GetAIControlledUnits";
  ctx.kind = script::CallKind::member;
  const script::HostOutcome out = registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(is_objlist(out.value));
  const std::span<const ObjectId> items =
      objlist_pool_of(b.world).items(objlist_of(out.value));
  return {items.begin(), items.end()};
}

}  // namespace

/// The four squad filters and the four unit filters, each made to bite alone.
///
/// The entry point walks the *squads* in a node and emits their *member units*.
/// Everything asserted here is the original's rule, recovered from 0x0043a920;
/// see the body in `sim/squad.cpp` for which instruction each one came from.
TEST(get_ai_controlled_units_walks_the_nodes_squads_and_emits_their_members) {
  ArmyBench b;
  b.plant(3, 3);
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 2);

  const ObjectId a = b.soldier("Legionary", 1);
  const ObjectId c = b.soldier("Archer", 1);
  b.band(1, 1, {a, c});

  // The plain case: both members come back, in member order.
  const std::vector<ObjectId> both{a, c};
  CHECK(controlled(b, 1, "Military", 2, 10, false) == both);
  // `IsHeirOf`, not equality: `Military` matched both, `Legionary` matches one.
  CHECK(controlled(b, 1, "Legionary", 2, 10, false) == std::vector<ObjectId>{a});
  // A class the graph does not know selects nothing rather than everything.
  CHECK(controlled(b, 1, "Trireme", 2, 10, false).empty());
  // Another node's squads are not this node's.
  CHECK(controlled(b, 2, "Military", 2, 10, false).empty());
  // A GAIKA naming no node is the original's silent empty answer.
  CHECK(controlled(b, 99, "Military", 2, 10, false).empty());
  CHECK(controlled(b, kNoGaika, "Military", 2, 10, false).empty());
}

TEST(get_ai_controlled_units_takes_only_the_players_own_squads) {
  ArmyBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId mine = b.soldier("Legionary", 1);
  const ObjectId theirs = b.soldier("Legionary", 2);
  b.band(1, 1, {mine});
  b.band(2, 1, {theirs});

  // The argument is 1-based, as every per-player AI accessor here is.
  CHECK(controlled(b, 1, "Military", 2, 10, false) == std::vector<ObjectId>{mine});
  CHECK(controlled(b, 1, "Military", 3, 10, false) == std::vector<ObjectId>{theirs});
  // An ally is refused as firmly as an enemy: the relation test accepts only
  // *same*, which is why nothing here consults the alliance table.
  b.world.players().set(1, 2, Relation::allied, true);
  b.world.players().set(2, 1, Relation::allied, true);
  REQUIRE(b.world.players().are_allied(1, 2));
  CHECK(controlled(b, 1, "Military", 2, 10, false) == std::vector<ObjectId>{mine});
  // Zero or less skips the owner test entirely -- the original decrements
  // first and branches around the call on a negative index.
  CHECK(controlled(b, 1, "Military", 0, 10, false).size() == 2);
  // And an index past the table matches nobody rather than everybody.
  CHECK(controlled(b, 1, "Military", 17, 10, false).empty());
}

/// **The boolean argument's whole meaning**, and the only thing that tells the
/// temple recruiter apart from the other three: `false` refuses a squad flagged
/// `SF_PEACEFUL`, `true` takes it.
TEST(get_ai_controlled_units_gates_peaceful_squads_on_its_boolean) {
  ArmyBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId calm = b.soldier("Legionary", 1);
  const SquadKey key = b.band(1, 1, {calm});
  Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  squad->flags |= kSquadFlagPeaceful;

  CHECK(controlled(b, 1, "Military", 2, 10, false).empty());
  CHECK(controlled(b, 1, "Military", 2, 10, true) == std::vector<ObjectId>{calm});
}

TEST(get_ai_controlled_units_counts_a_squad_that_is_heading_to_the_node) {
  ArmyBench b;
  b.plant(3, 3);
  b.plant(9, 9);
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 2);

  const ObjectId marching = b.soldier("Legionary", 1);
  const SquadKey key = b.band(1, 2, {marching});
  Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);

  // Standing in node 2 and going nowhere: node 1 does not want it.
  CHECK(controlled(b, 1, "Military", 2, 10, false).empty());
  // `DestGAIKA` is the fallback "heading to", so node 1 counts it now.
  squad->dest_gaika = 1;
  CHECK(controlled(b, 1, "Military", 2, 10, false) == std::vector<ObjectId>{marching});
  // And the order's node wins over `DestGAIKA` when there is one, which is the
  // one place this rule differs from `GetSquads`'s.
  squad->order_dest = 2;
  CHECK(controlled(b, 1, "Military", 2, 10, false).empty());
  CHECK(controlled(b, 2, "Military", 2, 10, false) == std::vector<ObjectId>{marching});
}

/// Buildings are not lent out, and the class filter is not what stops them:
/// `Fort` descends from `Military` here precisely so the `[obj+0x2c] &
/// 0x400000` test is the only thing left standing between it and the list.
TEST(get_ai_controlled_units_emits_units_and_not_buildings) {
  ArmyBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId soldier = b.soldier("Legionary", 1);
  const ObjectId keep = b.fort(1);
  b.band(1, 1, {keep, soldier});

  // Membership order puts the building first, so a walk that did not test the
  // flag would answer with it at the head.
  CHECK(controlled(b, 1, "Military", 2, 10, false) == std::vector<ObjectId>{soldier});
}

/// A player index at or past the table refuses outright -- the *invalid* arm of
/// the four-way relation -- so an out-of-range argument matches nobody rather
/// than reaching through to a squad that happens to carry the same number.
TEST(get_ai_controlled_units_refuses_a_player_index_past_the_table) {
  ArmyBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId stray = b.soldier("Legionary", kNoPlayer);
  const SquadKey key = b.heroes.squads().create(kNoPlayer);
  CHECK(b.heroes.squads().join(key, stray));
  Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  squad->gaika_in = 1;
  squad->eval = 10;

  // `kNoPlayer` is 255, so `asked` of 256 is the argument whose decrement lands
  // exactly on it. Both indices are past the table and both are refused.
  CHECK(controlled(b, 1, "Military", 256, 10, false).empty());
  // And it is still there for a call that skips the owner test altogether.
  CHECK(controlled(b, 1, "Military", 0, 10, false) == std::vector<ObjectId>{stray});
}

TEST(get_ai_controlled_units_skips_the_dead_the_pinned_and_the_strengthless) {
  ArmyBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId alive = b.soldier("Legionary", 1);
  const ObjectId dead = b.soldier("Legionary", 1);
  const ObjectId pinned = b.soldier("Legionary", 1);
  CHECK(b.world.set_health(dead, 0));
  b.world.find(pinned)->state.flags.no_ai = true;
  const SquadKey key = b.band(1, 1, {alive, dead, pinned});

  // `UNITFLAG_NOAI` is the test this entry point is named after: a unit a
  // mission script has pinned is not the AI's to lend out.
  CHECK(controlled(b, 1, "Military", 2, 10, false) == std::vector<ObjectId>{alive});

  // And a squad with no strength is skipped whole, which is the census's guard
  // and the reason every case here sets `eval` by hand.
  Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  squad->eval = 0;
  CHECK(controlled(b, 1, "Military", 2, 10, false).empty());
}

/// `num` is a countdown over **units**, and it abandons the rest of the squad
/// it is in the middle of -- not a per-squad quota and not a comparison.
TEST(get_ai_controlled_units_stops_the_walk_when_the_quota_is_met) {
  ArmyBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId one = b.soldier("Legionary", 1);
  const ObjectId two = b.soldier("Legionary", 1);
  const ObjectId three = b.soldier("Legionary", 1);
  b.band(1, 1, {one, two, three});

  const std::vector<ObjectId> pair{one, two};
  CHECK(controlled(b, 1, "Military", 2, 2, false) == pair);
  CHECK(controlled(b, 1, "Military", 2, 1, false) == std::vector<ObjectId>{one});
  // Never compared, only decremented: zero and below are "no limit" rather
  // than "take nothing". No shipped site passes one; the original still does it.
  CHECK(controlled(b, 1, "Military", 2, 0, false).size() == 3);
  CHECK(controlled(b, 1, "Military", 2, -1, false).size() == 3);
}

/// **It is a pure query.** The four `.vs` callers do their own `DetachFrom` and
/// `SetNoAIFlag` afterwards, which is only correct if this changes nothing --
/// and the same units come back on the next call until a caller does.
TEST(get_ai_controlled_units_changes_nothing_and_answers_the_same_twice) {
  ArmyBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId soldier = b.soldier("Legionary", 1);
  const SquadKey key = b.band(1, 1, {soldier});

  const std::vector<ObjectId> first = controlled(b, 1, "Military", 2, 10, false);
  CHECK(first == std::vector<ObjectId>{soldier});
  const Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  CHECK(squad->members.size() == 1);
  CHECK(squad->gaika_in == 1);
  CHECK(!b.world.find(soldier)->state.flags.no_ai);
  CHECK(controlled(b, 1, "Military", 2, 10, false) == first);
}

// --------------------------------------------------------------------------
// the target search over a node
// --------------------------------------------------------------------------

namespace {

/// `ArmyBench` with a combat system and a siege-engine class tree.
///
/// `BestTargetInGAIKA` is the only member of the target-selection family whose
/// candidate set is not a circle, so it needs squads (the primary walk),
/// settlements (the fallback) and combat (the score) alive in one world at
/// once. Derived rather than folded into `ArmyBench` for the reason `ArmyBench`
/// itself gives: a system added to a shared fixture changes what a save
/// carries, and the node tests above have nothing to fight about.
struct SiegeBench : ArmyBench {
  CombatSystem combat;

  SiegeBench() {
    // `Catapult` is a *building* that declares `is_central_building = 1`, so a
    // placed siege engine owns a settlement and turns up in the same vector a
    // town hall does. `BCatapult` under it is what makes the fallback's match a
    // descent rather than a name.
    graph.add(bytes_of(R"(<class id="Catapult" cpp_class="CVXCatapult" parent="Military"/>)"),
              "catapult.sc.xml");
    graph.add(bytes_of(R"(<class id="BCatapult" cpp_class="CVXCatapult" parent="Catapult"/>)"),
              "bcatapult.sc.xml");
    graph.link();
    combat.set_world_bound(true);
    REQUIRE(world.add_system(&combat));
  }

  /// A unit standing on a cell.
  ObjectId trooper(const char* class_name, PlayerId owner, std::int32_t cx, std::int32_t cy) {
    const ObjectId id = soldier(class_name, owner);
    CHECK(world.set_position(id, in_cell(cx, cy)));
    return id;
  }

  /// A settlement whose central building is of `class_name`. The fallback
  /// matches on the **anchor's class** and never on the settlement's kind, so
  /// everything here is planted as `other`.
  ObjectId emplacement(const char* class_name, std::int32_t cx, std::int32_t cy,
                       PlayerId owner) {
    const ObjectId anchor =
        world.spawn(NativeClass::building, nullptr, graph.find(class_name));
    CHECK(world.set_owner(anchor, owner));
    CHECK(world.set_health(anchor, 100));
    CHECK(world.set_position(anchor, in_cell(cx, cy)));
    SettlementInit init;
    init.kind = SettlementKind::other;
    init.anchor = anchor;
    init.owner = owner;
    CHECK(economy.create(world, init) != kNoSettlement);
    return anchor;
  }

  /// Profiles for every class here and the population handed to combat.
  /// `sight` is a parameter because the one property this entry point does
  /// **not** have is a reach, and the only way to see that is to make the
  /// circle-sweeping form of the same search fail on the same candidate.
  void arm(std::int32_t sight = 500) {
    CombatProfile p;
    p.damage = 10;
    p.damage_type = DamageType::slash;
    p.max_health = 200;
    p.range = 17;
    p.radius = 15;
    p.selection_radius = 15;
    p.sight = sight;
    p.attack_interval = 1000;
    for (const char* name : {"Legionary", "Archer", "Fort", "Catapult", "BCatapult"}) {
      combat.set_profile(graph.find(name), p);
    }
    combat.start(world);
  }
};

/// `u.BestTargetInGAIKA()`, through the registry, as a script would reach it.
ObjectId best_in_gaika(SiegeBench& b, ObjectId caster) {
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index =
      registry.find(script::CallKind::member, "BestTargetInGAIKA", 0);
  CHECK(index != script::kUnresolvedHost);
  if (index == script::kUnresolvedHost) return kNoObject;
  std::vector<script::Value> args{script::Value::object(script::TypeId{1}, caster)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "BestTargetInGAIKA";
  ctx.kind = script::CallKind::member;
  const script::HostOutcome out = registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  if (!out.value.is_object()) return kNoObject;
  return static_cast<ObjectId>(out.value.as_object().id);
}

}  // namespace

/// The primary walk: the squads standing in the receiver's node, scored by the
/// plain filter, least wins.
///
/// The two candidates are joined **far first**, so a body that kept the first
/// offered rather than the least would answer the wrong one. That is not
/// pedantry: the tie rule really is "first offered", so the ordering is the
/// only thing separating the two readings.
TEST(best_target_in_gaika_takes_the_nearest_enemy_out_of_the_squads_standing_here) {
  SiegeBench b;
  b.plant(3, 3);  // node 1 on the western island; the eastern one is node 2
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 2);

  const ObjectId caster = b.trooper("Legionary", 1, 2, 2);
  const ObjectId near_by = b.trooper("Archer", 2, 2, 3);
  const ObjectId far_off = b.trooper("Archer", 2, 5, 5);
  b.arm();
  REQUIRE(b.world.gaika().at(b.world.lsa(), in_cell(2, 2)) == 1);

  b.band(2, 1, {far_off, near_by});
  CHECK(best_in_gaika(b, caster) == near_by);

  // And a tie keeps the **first offered**, which is squad order and then join
  // order. There is no ascending sweep here to make it "lowest id", so the
  // pair below is joined highest-id first: under the family's usual tie rule
  // the other one would win.
  SiegeBench t;
  t.plant(3, 3);
  t.rebuild();
  const ObjectId watcher = t.trooper("Legionary", 1, 3, 3);
  const ObjectId left = t.trooper("Archer", 2, 2, 3);
  const ObjectId right = t.trooper("Archer", 2, 4, 3);
  t.arm();
  REQUIRE(left < right);
  t.band(2, 1, {right, left});
  CHECK(best_in_gaika(t, watcher) == right);
}

/// **There is no reach.** Every other member of the family gets its radius from
/// the grid sweep it rides on; this one has no sweep, so the premise here is
/// that the circle-sweeping form of the same search finds nothing at all.
TEST(best_target_in_gaika_has_no_radius_where_the_rest_of_the_family_has_sight) {
  SiegeBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId caster = b.trooper("Legionary", 1, 1, 1);
  const ObjectId across = b.trooper("Archer", 2, 5, 5);
  b.arm(/*sight=*/10);
  b.band(2, 1, {across});

  REQUIRE(b.combat.best_target(caster) == kNoObject);
  CHECK(best_in_gaika(b, caster) == across);
}

/// The four squad tests, applied one at a time to a squad that is otherwise the
/// only candidate in the world -- so nothing here can be carried by the order
/// two candidates happen to arrive in.
TEST(best_target_in_gaika_applies_the_squad_tests_one_at_a_time) {
  SiegeBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId caster = b.trooper("Legionary", 1, 2, 2);
  const ObjectId enemy = b.trooper("Archer", 2, 2, 3);
  b.arm();
  const SquadKey key = b.band(2, 1, {enemy});
  Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  REQUIRE(best_in_gaika(b, caster) == enemy);

  // 1. `Squad::Eval`. A squad that exists with nothing in it is not an army --
  //    the same guard the census applies, and the reason this walk is empty on
  //    every shipped map today.
  squad->eval = 0;
  CHECK(best_in_gaika(b, caster) == kNoObject);
  squad->eval = 10;

  // 2. `SF_PEACEFUL`. The boolean that would allow it is written in as false.
  squad->flags = static_cast<std::uint16_t>(squad->flags | kSquadFlagPeaceful);
  CHECK(best_in_gaika(b, caster) == kNoObject);
  squad->flags = static_cast<std::uint16_t>(squad->flags & ~kSquadFlagPeaceful);

  // 3. Here, not heading here. 0x0044e1f0 answers 1 for a squad that is only on
  //    its way and the caller masks with 6, so an order pointing at this node
  //    does not make a squad standing somewhere else a candidate.
  squad->gaika_in = 2;
  squad->order_dest = 1;
  CHECK(best_in_gaika(b, caster) == kNoObject);
  // ...and 2, "here but leaving", is kept: once the squad is standing here,
  // where it is going is not read at all.
  squad->gaika_in = 1;
  squad->order_dest = 2;
  squad->dest_gaika = 2;
  CHECK(best_in_gaika(b, caster) == enemy);
  squad->order_dest = kNoGaika;
  squad->dest_gaika = kNoGaika;

  // 4. The member tests, which are the two `0x004282c0` makes before offering
  //    anything: a garrisoned unit is not a target, and neither is a dead one.
  const ObjectId keep = b.fort(1);
  CHECK(b.world.set_position(keep, in_cell(4, 4)));
  REQUIRE(b.world.put_in_holder(enemy, keep));
  CHECK(best_in_gaika(b, caster) == kNoObject);
  REQUIRE(b.world.remove_from_holder(enemy, in_cell(2, 3)));
  CHECK(best_in_gaika(b, caster) == enemy);

  CHECK(b.world.set_health(enemy, 0));
  CHECK(best_in_gaika(b, caster) == kNoObject);
}

/// The relation read is the **squad's owner**, not the member's, and it is read
/// off the receiver's own diplomacy row.
///
/// The squad belongs to player 3 and its one member to player 2, and player 1
/// is at war with 2 and at truce with 3. If the walk asked about the member the
/// answer would be the member; it asks about the squad, so there is none.
TEST(best_target_in_gaika_reads_the_squads_owner_and_not_its_members) {
  SiegeBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId caster = b.trooper("Legionary", 1, 2, 2);
  const ObjectId lent = b.trooper("Archer", 2, 2, 3);
  b.arm();
  b.world.players().set_relation_word(1, 3, kRelationFriendly);
  b.band(3, 1, {lent});

  CHECK(best_in_gaika(b, caster) == kNoObject);

  // One-directional: player 3 declaring player 1 an enemy changes nothing,
  // because the row consulted is player 1's.
  b.world.players().set_relation_word(3, 1, 0);
  CHECK(best_in_gaika(b, caster) == kNoObject);

  // And player 1 revoking the truce brings the member back.
  b.world.players().set_relation_word(1, 3, 0);
  CHECK(best_in_gaika(b, caster) == lent);
}

/// The fallback: the first enemy catapult standing in the receiver's node.
///
/// **The node test is narrower here than in the original**, and the layout says
/// so out loud. Every settlement gets a node centred on its central building,
/// so the catapult *is* a node centre and the receiver shares its node exactly
/// when that catapult is the nearest node centre to it. The original's slot
/// grid makes a node a region instead; the reading is the same and the reach is
/// smaller, and `sim/ai.cpp` records that.
TEST(best_target_in_gaika_falls_back_to_an_enemy_catapult_standing_in_this_node) {
  SiegeBench b;
  // `BCatapult`, not `Catapult`: the match is by descent.
  const ObjectId siege = b.emplacement("BCatapult", 2, 2, 2);
  b.plant(5, 5, 1);  // a town hall of the receiver's own, and a second node
  b.rebuild();
  const ObjectId caster = b.trooper("Legionary", 1, 1, 1);
  b.arm();

  const GaikaTable& table = b.world.gaika();
  const GaikaId here = table.at(b.world.lsa(), in_cell(1, 1));
  REQUIRE(here != kNoGaika);
  REQUIRE(table.at(b.world.lsa(), in_cell(2, 2)) == here);
  CHECK(best_in_gaika(b, caster) == siege);

  // **The direction is the transpose**, and this is the assertion that pins it:
  // the receiver declaring a truce with the catapult's owner does *not* hide
  // it, because the row read is the catapult owner's.
  b.world.players().set_relation_word(1, 2, kRelationFriendly);
  CHECK(best_in_gaika(b, caster) == siege);
  b.world.players().set_relation_word(2, 1, kRelationFriendly);
  CHECK(best_in_gaika(b, caster) == kNoObject);
  b.world.players().set_relation_word(2, 1, 0);
  CHECK(best_in_gaika(b, caster) == siege);
  b.world.players().set_relation_word(1, 2, 0);

  // A different node, and it is gone: the receiver standing by its own town
  // hall is in that settlement's node and not the catapult's.
  CHECK(b.world.set_position(caster, in_cell(5, 5)));
  REQUIRE(table.at(b.world.lsa(), in_cell(5, 5)) != here);
  CHECK(best_in_gaika(b, caster) == kNoObject);
  CHECK(b.world.set_position(caster, in_cell(1, 1)));

  // And the squad walk outranks it: a candidate out of a squad standing here
  // is answered instead, fallback or no fallback.
  const ObjectId enemy = b.trooper("Archer", 2, 1, 2);
  b.arm();
  b.band(2, here, {enemy});
  CHECK(best_in_gaika(b, caster) == enemy);
}

/// The class test is a class test: a settlement whose central building is
/// anything else is not a target, however near and however hostile.
TEST(best_target_in_gaika_answers_nothing_for_a_settlement_that_is_not_a_catapult) {
  SiegeBench b;
  // A `Fort` -- a building under the same `Military` base as the catapults, so
  // a body that matched the base rather than the name would answer it.
  const ObjectId keep = b.emplacement("Fort", 2, 2, 2);
  b.rebuild();
  const ObjectId caster = b.trooper("Legionary", 1, 1, 1);
  b.arm();

  const GaikaTable& table = b.world.gaika();
  REQUIRE(table.at(b.world.lsa(), in_cell(1, 1)) ==
          table.at(b.world.lsa(), in_cell(2, 2)));
  REQUIRE(b.world.players().is_enemy(b.world.find(keep)->state.owner, 1));
  CHECK(best_in_gaika(b, caster) == kNoObject);
}

/// A receiver that is nowhere, or nothing, answers nothing rather than trapping.
TEST(best_target_in_gaika_answers_nothing_for_a_receiver_it_cannot_place) {
  SiegeBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId caster = b.trooper("Legionary", 1, 2, 2);
  b.arm();
  CHECK(best_in_gaika(b, caster) == kNoObject);
  CHECK(best_in_gaika(b, kNoObject) == kNoObject);
  CHECK(best_in_gaika(b, static_cast<ObjectId>(9999)) == kNoObject);
}

/// A receiver in no node answers nothing, and **that guard is load-bearing**.
///
/// `kNoGaika` is every squad's default `GAIKAIn`, so a body that carried it
/// into the walk instead of refusing it would read every squad in the world as
/// standing here. It is the same hazard `GetAIControlledUnits` refuses
/// `kNoGaika` for, and it is the one fault in this block that no other case
/// could see.
TEST(best_target_in_gaika_refuses_a_receiver_that_is_in_no_node) {
  SiegeBench b;  // nothing planted, so there is no node table to be in
  REQUIRE(b.world.gaika().count() == 0);
  const ObjectId caster = b.trooper("Legionary", 1, 2, 2);
  const ObjectId enemy = b.trooper("Archer", 2, 2, 3);
  b.arm();
  b.band(2, kNoGaika, {enemy});
  REQUIRE(b.world.gaika().at(b.world.lsa(), in_cell(2, 2)) == kNoGaika);
  CHECK(best_in_gaika(b, caster) == kNoObject);
}

/// `g.Empty()` -- true unless a squad is standing in the node.
///
/// The original tests whether the node's squad container has ever been
/// allocated (`[node + 0x18] == 0`); this engine keeps node membership on the
/// squad, so the question is asked from that side. What the two have in common
/// is every answer a script can observe.
TEST(gaika_empty_is_true_until_a_squad_stands_in_the_node) {
  ArmyBench b;
  b.plant(3, 3);  // node 1 on the western island; node 2 is the eastern region
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 2);

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index = registry.find(script::CallKind::member, "Empty", 0);
  REQUIRE(index != script::kUnresolvedHost);
  REQUIRE(registry.entry(index).fn != nullptr);
  const auto empty = [&](GaikaId node) {
    std::vector<script::Value> args{script::Value::integer(node)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "Empty";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.truthy_scalar();
  };

  CHECK(empty(1));
  CHECK(empty(2));
  // A node id the table does not have, and `kNoGaika` itself, are both empty --
  // and the second matters, because `kNoGaika` is every squad's default
  // `GAIKAIn` and must not match the squad planted below.
  CHECK(empty(0));
  CHECK(empty(99));

  const ObjectId soldier = b.soldier("Legionary", 1);
  b.band(1, 1, {soldier});
  CHECK(!empty(1));
  // ...and only that node.
  CHECK(empty(2));
  CHECK(empty(kNoGaika));

  // A squad standing in a node the table does not have. The original cannot
  // build this -- its container hangs off the node -- but this engine can,
  // because membership is a number on the squad, so the table lookup is what
  // stops a stale `GAIKAIn` from answering for an id that names nothing.
  const ObjectId stray = b.soldier("Legionary", 2);
  const SquadKey stale = b.band(2, 1, {stray});
  Squad* squad = b.heroes.squads().find(stale);
  REQUIRE(squad != nullptr);
  squad->gaika_in = 99;
  CHECK(empty(99));

  // A squad that is nowhere leaves every node empty.
  ArmyBench nowhere;
  nowhere.plant(3, 3);
  nowhere.rebuild();
  const ObjectId lost = nowhere.soldier("Legionary", 1);
  nowhere.band(1, kNoGaika, {lost});
  HostContext other;
  other.world = &nowhere.world;
  std::vector<script::Value> args{script::Value::integer(1)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &other;
  ctx.name = "Empty";
  ctx.kind = script::CallKind::member;
  CHECK(registry.entry(index).fn(ctx).value.truthy_scalar());
}

/// `MilitaryPresence` is `Empty`'s question asked from one player's side.
///
/// Same container, same "in the node" reading, three filters `Empty` does not
/// apply: strength, the relation, and the peaceful flag. The relation is the
/// one worth building carefully, because it is one-directional and reads the
/// *squad's owner* rather than its members -- the same distinction
/// `best_target_in_gaika_reads_the_squads_owner_and_not_its_members` draws.
TEST(military_presence_wants_a_friendly_armed_squad_standing_in_this_node) {
  ArmyBench b;
  b.plant(3, 3);  // node 1 on the western island; node 2 is the eastern region
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 2);

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index = registry.find(script::CallKind::member, "MilitaryPresence", 1);
  REQUIRE(index != script::kUnresolvedHost);
  REQUIRE(registry.entry(index).fn != nullptr);
  const auto presence = [&](GaikaId node, std::int32_t player) {
    std::vector<script::Value> args{script::Value::integer(node),
                                    script::Value::integer(player)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "MilitaryPresence";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.truthy_scalar();
  };

  // Player numbers are 1-based in a script. Player 0 is what the entry point's
  // `dec` turns negative, and it is the "anybody at all" reading.
  CHECK(!presence(1, 2));
  CHECK(!presence(1, 0));

  const ObjectId mine = b.soldier("Legionary", 1);
  const SquadKey key = b.band(1, 1, {mine});
  CHECK(presence(1, 2));   // script 2 is player 1: my own squad
  CHECK(presence(1, 0));   // and "anybody" sees it too
  CHECK(!presence(2, 2));  // only the node it stands in
  CHECK(!presence(99, 2));

  Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);

  // -- strength. A squad with no `eval` is not a presence, and `Empty` above
  // would still call the node occupied -- which is the whole reason the two
  // entry points are not one.
  squad->eval = 0;
  CHECK(!presence(1, 2));
  squad->eval = 10;
  CHECK(presence(1, 2));

  // -- the peaceful flag, `[squad+0x30] & 4`.
  squad->flags = static_cast<std::uint16_t>(squad->flags | kSquadFlagPeaceful);
  CHECK(!presence(1, 2));
  squad->flags = static_cast<std::uint16_t>(squad->flags & ~kSquadFlagPeaceful);
  CHECK(presence(1, 2));

  // -- the relation, and it is **one-directional**. Player 3 (script 4) is an
  // enemy of player 1 until its own row says otherwise; changing player 1's
  // row instead changes nothing, because 0x0044e250 reads the *viewer's*.
  CHECK(!presence(1, 4));
  b.world.players().set_relation_word(1, 3, kRelationFriendly);
  CHECK(!presence(1, 4));
  b.world.players().set_relation_word(3, 1, kRelationFriendly);
  CHECK(presence(1, 4));
  b.world.players().set_relation_word(3, 1, 0);
  CHECK(!presence(1, 4));
  // ...and a player is never its own enemy, which `PlayerTable::is_enemy`
  // guarantees rather than this entry point: 0x0044e250 short-circuits on
  // `viewer == owner` before it reads a row, and `is_enemy` carries the same
  // rule for the whole engine. Asserted here anyway, because a squad's own
  // player is the case both shipped callers ask about.
  CHECK(presence(1, 2));

  // -- a stale node id answers false, and it is the *table* that says so. The
  // original cannot build this -- its container hangs off the node -- but here
  // membership is a number on the squad, so a squad parked in an id the table
  // does not have would otherwise be a presence in a node that does not exist.
  const ObjectId stray = b.soldier("Legionary", 1);
  const SquadKey stale = b.band(1, 1, {stray});
  Squad* wandering = b.heroes.squads().find(stale);
  REQUIRE(wandering != nullptr);
  wandering->gaika_in = 99;
  CHECK(!presence(99, 2));
  b.heroes.squads().leave(stale, stray);

  // -- the owner is the squad's, not its members'. A soldier belonging to the
  // asking player inside a squad the enemy owns is not a friendly presence.
  const ObjectId defector = b.soldier("Legionary", 4);
  CHECK(b.heroes.squads().join(key, defector));
  CHECK(!presence(1, 5));
}

// --------------------------------------------------------------------------
// FindTeleport
// --------------------------------------------------------------------------

namespace {

/// Two islands on one long map: the western one forty-eight cells wide, so
/// a walk can pass the 2000-unit threshold within one area, and an eastern
/// one across a two-cell strait for the cases that cross areas.
constexpr std::initializer_list<std::string_view> kLongIsland = {
    "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~",
    "~................................................~~............................~",
    "~................................................~~............................~",
    "~................................................~~............................~",
    "~................................................~~............................~",
    "~................................................~~............................~",
    "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~",
};

/// `pass_blocking_water` for a picture that is not sixteen cells square.
std::vector<std::byte> pass_blocking_water_sized(std::initializer_list<std::string_view> rows) {
  imperivm::test::Builder out;
  const std::vector<std::string_view> picture(rows);
  const auto height = static_cast<std::uint32_t>(picture.size()) * 4;
  const auto width = static_cast<std::uint32_t>(picture.front().size()) * 4;
  out.text(kGridMagic).u32(16).u32(1).u32(16 * width).u32(16 * height);
  const std::uint32_t stride = (width + 7) / 8;
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t byte = 0; byte < stride; ++byte) {
      std::uint32_t value = 0;
      for (std::uint32_t bit = 0; bit < 8; ++bit) {
        const std::uint32_t x = byte * 8 + bit;
        if (x >= width) continue;
        if (picture[y / 4][x / 4] == '~') value |= 1u << bit;
      }
      out.u8(value);
    }
  }
  return {out.span().begin(), out.span().end()};
}

/// A `NodeBench` on the long island, with a way to plant a teleport pair.
struct TeleportBench {
  ClassGraph graph;
  std::vector<std::byte> terrain = terrain_from(kLongIsland);
  std::vector<std::byte> pass = pass_blocking_water_sized(kLongIsland);
  World world;
  EconomySystem economy;
  script::HostRegistry registry;
  HostContext context;

  TeleportBench() {
    graph.add(bytes_of(
                  R"(<class id="Townhall" cpp_class="CVXTownHall"><properties maxhealth="1000"/></class>)"),
              "townhall.sc.xml");
    graph.add(bytes_of(R"(<class id="Teleport" cpp_class="CVXTeleport"/>)"), "teleport.sc.xml");
    graph.link();
    world.set_class_graph(&graph);
    const Result<Grid> layer = Grid::parse(terrain);
    REQUIRE(layer.ok());
    world.set_terrain(layer.value());
    REQUIRE(world.add_system(&economy));
    economy.start(world);
    world.mutable_lsa().build(world.terrain(), obstruction(pass));
    (void)register_all_hosts(registry);
    context.world = &world;
  }

  /// A teleport settlement standing on a cell. Pairs are made by `pair`.
  ObjectId teleport(std::int32_t cx, std::int32_t cy) {
    const ObjectId object = world.spawn(NativeClass::teleport, nullptr, graph.find("Teleport"));
    CHECK(world.set_position(object, in_cell(cx, cy)));
    SettlementInit init;
    init.kind = SettlementKind::other;
    init.anchor = object;
    init.settlement_object = object;
    init.owner = 1;
    (void)economy.create(world, init);
    return object;
  }

  void pair(ObjectId a, ObjectId b) {
    world.mutable_state(a)->teleport_destination = b;
    world.mutable_state(b)->teleport_destination = a;
  }

  void rebuild() { world.mutable_gaika().build(world, world.lsa(), economy.settlements()); }

  ObjectId find(std::int32_t player, Point src, Point dst) {
    const std::uint32_t index = registry.find(script::CallKind::free_function, "FindTeleport", 3);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return kNoObject;
    std::vector<script::Value> args{script::Value::integer(player), pack_point(src),
                                    pack_point(dst)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    if (!out.value.is_object() || out.value.as_object().type == script::kNoType) return kNoObject;
    return out.value.as_object().id;
  }
};

}  // namespace

/// The cheapest pair under the budget, the short-walk refusal, the two-thirds
/// rule, and the two area tests.
TEST(find_teleport_picks_the_pair_that_beats_two_thirds_of_the_walk) {
  TeleportBench b;
  // Within the western island: A at cells 2 and 40, B at cells 6 and 36.
  // Across the strait: X from cell 3 to cell 60. And two decoys: D on the
  // west, cells 46 and 48; C on the east, cells 51 and 76.
  const ObjectId a_in = b.teleport(2, 3);
  const ObjectId a_out = b.teleport(40, 3);
  const ObjectId b_in = b.teleport(6, 3);
  const ObjectId b_out = b.teleport(36, 3);
  const ObjectId x_in = b.teleport(3, 3);
  const ObjectId x_out = b.teleport(60, 3);
  const ObjectId d_in = b.teleport(46, 3);
  const ObjectId d_out = b.teleport(48, 3);
  const ObjectId c_in = b.teleport(51, 3);
  const ObjectId c_out = b.teleport(76, 3);
  b.pair(a_in, a_out);
  b.pair(b_in, b_out);
  b.pair(x_in, x_out);
  b.pair(d_in, d_out);
  b.pair(c_in, c_out);
  b.rebuild();
  const LsaId west = b.world.lsa().at(in_cell(2, 3));
  const LsaId east = b.world.lsa().at(in_cell(60, 3));
  REQUIRE(west != kNoLsa);
  REQUIRE(east != kNoLsa);
  REQUIRE(west != east);

  // -- one area. From cell 1 to cell 44: 2752 units straight, budget 1834.
  // A costs 64 in and 256 out, 320; B 320 and 512, 832. A wins.
  CHECK(b.find(1, in_cell(1, 3), in_cell(44, 3)) == a_in);
  // From cell 5 the walk in favours B (64 against 192) but the walk out still
  // favours A: 448 against 576.
  CHECK(b.find(1, in_cell(5, 3), in_cell(44, 3)) == a_in);
  // From cell 7 to cell 39 the walk out favours A (64 against 192) and the
  // sum favours B: 256 against 384. Both halves count.
  CHECK(b.find(1, in_cell(7, 3), in_cell(39, 3)) == b_in);
  // Under 2000 units within one area nothing is answered, however cheap a
  // pair would be: cell 5 to cell 30 is 1600.
  CHECK(b.find(1, in_cell(5, 3), in_cell(30, 3)) == kNoObject);
  // Over it, a pair has to beat two thirds of the walk. Cell 10 to cell 44 is
  // 2176 and the budget 1450; A costs 768 and is taken. Re-pair B's near end
  // to A's and the only pair under budget is gone: A's exit is 256 from the
  // target and its entry 2816 back at cell 2 -- 3072, over the budget.
  CHECK(b.find(1, in_cell(10, 3), in_cell(44, 3)) == a_in);
  b.pair(b_in, a_in);
  b.world.mutable_state(a_out)->teleport_destination = kNoObject;
  // Now A's near end pairs with B's near end: from cell 10 the walk in is 256
  // to cell 6 and the walk out from cell 2 is 2688 -- 2944, over 1450.
  CHECK(b.find(1, in_cell(10, 3), in_cell(44, 3)) == kNoObject);
  b.pair(a_in, a_out);
  b.pair(b_in, b_out);

  // -- across the strait. From cell 47 to cell 52 is 320 units, and the short
  // walk is not refused when the areas differ; nor is the budget two thirds
  // of it -- X costs 2816 in and 512 out, 3328, and is taken. D would be far
  // cheaper, 64 and 256, but its exit is on the wrong island.
  CHECK(b.find(1, in_cell(47, 3), in_cell(52, 3)) == x_in);
  // From cell 47 to cell 78: C stands on the eastern island, 256 away, with
  // an exit 128 from the target -- and is not in the source's area, so X is
  // the answer at 3968.
  CHECK(b.find(1, in_cell(47, 3), in_cell(78, 3)) == x_in);

  // An unpaired teleport is never a candidate.
  b.world.mutable_state(x_in)->teleport_destination = kNoObject;
  CHECK(b.find(1, in_cell(47, 3), in_cell(52, 3)) == kNoObject);
  b.pair(x_in, x_out);

  // A player with no AI and one outside the table answer alike.
  CHECK(b.find(0, in_cell(1, 3), in_cell(44, 3)) == a_in);
  CHECK(b.find(9, in_cell(1, 3), in_cell(44, 3)) == a_in);
}

// --------------------------------------------------------------------------
// the area graph: CheckLsaPath, and two neighbours of it
// --------------------------------------------------------------------------

namespace {

/// Two islands in one sea, joined by a three-cell strait, with a pond on the
/// eastern one that is too small to be an area and poisons the slots it
/// touches. Thirty-two cells wide -- a one-bit grid's rows are whole bytes --
/// and thirteen tall.
constexpr std::initializer_list<std::string_view> kShores = {
    "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~",  // 0
    "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~",  // 1
    "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~",  // 2
    "~~~..........~~~..............~~",  // 3
    "~~~..........~~~..............~~",  // 4
    "~~~..........~~~.........~~~..~~",  // 5
    "~~~..........~~~.........~~~..~~",  // 6
    "~~~..........~~~.........~~~..~~",  // 7
    "~~~..........~~~..............~~",  // 8
    "~~~..........~~~..............~~",  // 9
    "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~",  // 10
    "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~",  // 11
    "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~",  // 12
};

/// **Two seas, so that a route can be longer than three.** Land on both edges
/// and in the middle, cut by two channels wide enough to survive the shoreline
/// walling and the 256-cell floor: left, sea, middle, sea, right. Five areas,
/// and the only picture here that gives `CheckLsaPath` an answer of 5.
constexpr std::initializer_list<std::string_view> kTwoSeas = {
    ".......~~~~~........~~~~~.......",  // 0
    ".......~~~~~........~~~~~.......",  // 1
    ".......~~~~~........~~~~~.......",  // 2
    ".......~~~~~........~~~~~.......",  // 3
    ".......~~~~~........~~~~~.......",  // 4
    ".......~~~~~........~~~~~.......",  // 5
    ".......~~~~~........~~~~~.......",  // 6
    ".......~~~~~........~~~~~.......",  // 7
    ".......~~~~~........~~~~~.......",  // 8
    ".......~~~~~........~~~~~.......",  // 9
    ".......~~~~~........~~~~~.......",  // 10
    ".......~~~~~........~~~~~.......",  // 11
    ".......~~~~~........~~~~~.......",  // 12
};

/// Land with a pond too small to keep in its corner -- so the pond fills
/// first, before the terrain rounding (`sim/lsa.hpp`) lets any land cell seed
/// in the water pass -- and what the pond touched, nobody gets.
constexpr std::initializer_list<std::string_view> kPond = {
    "~~~.............", "~~~.............", "~~~.............", "................",
    "................", "................", "................", "................",
    "................", "................", "................", "................",
    "................", "................", "................", "................",
};

/// Land with a lake big enough to keep -- nineteen cells on a side -- in the
/// corner, whose slot claims run one slot past its water on the right and
/// below, so five of its nine votes are cast over land and the census calls
/// it **ground**.
constexpr std::initializer_list<std::string_view> kLake = {
    "~~~~~...........", "~~~~~...........", "~~~~~...........", "~~~~~...........",
    "~~~~~...........", "................", "................", "................",
    "................", "................", "................", "................",
    "................", "................", "................", "................",
};

/// A sea in the corner, walled off by cliffs on both sides, whose only contact
/// with the land is one **diagonal** pair of slots: sea (3,1), land (4,2).
constexpr std::initializer_list<std::string_view> kCorner = {
    "~~~~~~~~##......", "~~~~~~~~##......", "~~~~~~~~##......", "~~~~~~~~##......",
    "########........", "########........", "................", "................",
    "................", "................", "................", "................",
    "................", "................", "................", "................",
};

/// Two lakes and a cliff band: the northern land touches both, the southern
/// land only the second, which straddles the band. Twenty-four by sixteen.
constexpr std::initializer_list<std::string_view> kTwoLakes = {
    "........................",  // 0
    "........................",  // 1
    "..~~~~~~................",  // 2
    "..~~~~~~................",  // 3
    "..~~~~~~................",  // 4
    "..~~~~~~................",  // 5
    "..~~~~~~......~~~~~~....",  // 6
    "..~~~~~~......~~~~~~....",  // 7
    "..............~~~~~~....",  // 8
    "..............~~~~~~....",  // 9
    "##############~~~~~~####",  // 10
    "##############~~~~~~####",  // 11
    "..............~~~~~~....",  // 12
    "..............~~~~~~....",  // 13
    "........................",  // 14
    "........................",  // 15
};

/// `pass_blocking_shore` at the **passability cell's** resolution and for a
/// picture of any size: the wall is one 16-unit cell on each side of the
/// coast, which is what the shipped grids carry and what lets a land slot and
/// a sea slot sit side by side. `pass_blocking_shore` walls the whole coastal
/// terrain cell, four passability cells deep, and on a grid like that no two
/// areas ever share or neighbour a slot. A `#` is land that is blocked -- a
/// cliff -- and, being land, walls no shore of its own.
std::vector<std::byte> pass_walling_shore_sized(std::initializer_list<std::string_view> rows) {
  const std::vector<std::string_view> picture(rows);
  const auto rows_n = static_cast<std::int32_t>(picture.size());
  const auto cols_n = static_cast<std::int32_t>(picture.front().size());
  const std::int32_t height = rows_n * 4;
  const std::int32_t width = cols_n * 4;
  const auto wet = [&](std::int32_t x, std::int32_t y, bool here) {
    if (x < 0 || y < 0 || x >= width || y >= height) return here;
    return picture[static_cast<std::size_t>(y / 4)][static_cast<std::size_t>(x / 4)] == '~';
  };
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(16).u32(1).u32(16 * static_cast<std::uint32_t>(width))
      .u32(16 * static_cast<std::uint32_t>(height));
  const std::int32_t stride = (width + 7) / 8;
  for (std::int32_t y = 0; y < height; ++y) {
    for (std::int32_t byte = 0; byte < stride; ++byte) {
      std::uint32_t value = 0;
      for (std::int32_t bit = 0; bit < 8; ++bit) {
        const std::int32_t x = byte * 8 + bit;
        if (x >= width) continue;
        const bool here = wet(x, y, false);
        const bool shore = wet(x - 1, y, here) != here || wet(x + 1, y, here) != here ||
                           wet(x, y - 1, here) != here || wet(x, y + 1, here) != here;
        const bool cliff =
            picture[static_cast<std::size_t>(y / 4)][static_cast<std::size_t>(x / 4)] == '#';
        if (shore || cliff) value |= 1u << static_cast<std::uint32_t>(bit);
      }
      out.u8(static_cast<std::uint8_t>(value));
    }
  }
  return {out.span().begin(), out.span().end()};
}

/// The shores, with the systems the three entry points read: settlements and
/// their nodes, squads, ships with commands, and the AI's ship-need counters.
struct ShoreBench {
  ClassGraph graph;
  std::vector<std::byte> terrain;
  std::vector<std::byte> pass;
  World world;
  EconomySystem economy;
  HeroSystem heroes;
  CommandSystem commands;
  AiSystem ai;
  script::HostRegistry registry;
  HostContext context;

  explicit ShoreBench(std::initializer_list<std::string_view> picture = kShores)
      : terrain(terrain_from(picture)), pass(pass_walling_shore_sized(picture)) {
    graph.add(bytes_of(
                  R"(<class id="Townhall" cpp_class="CVXTownHall"><properties maxhealth="1000"/></class>)"),
              "townhall.sc.xml");
    graph.add(bytes_of(R"(<class id="BaseVillage" cpp_class="CVXBuilding" parent=""/>)"),
              "basevillage.sc.xml");
    graph.add(bytes_of(R"(<class id="Village" cpp_class="CVXBuilding" parent="BaseVillage"/>)"),
              "village.sc.xml");
    graph.add(bytes_of(R"(<class id="Warship" cpp_class="CVXShip" parent=""/>)"),
              "warship.sc.xml");
    graph.link();
    world.set_class_graph(&graph);
    const Result<Grid> layer = Grid::parse(terrain);
    REQUIRE(layer.ok());
    world.set_terrain(layer.value());
    REQUIRE(world.add_system(&commands));
    REQUIRE(world.add_system(&heroes));
    REQUIRE(world.add_system(&economy));
    REQUIRE(world.add_system(&ai));
    economy.start(world);
    world.mutable_lsa().build(world.terrain(), obstruction(pass));
    (void)register_all_hosts(registry);
    context.world = &world;
  }

  /// A settlement whose central building stands on a cell, on `cls`.
  SettlementId plant(std::int32_t cx, std::int32_t cy, PlayerId owner, SettlementKind kind,
                     const char* cls) {
    const ObjectId anchor =
        world.spawn(kind == SettlementKind::stronghold ? NativeClass::town_hall : NativeClass::building,
                    nullptr, graph.find(cls));
    CHECK(world.set_position(anchor, in_cell(cx, cy)));
    SettlementInit init;
    init.kind = kind;
    init.anchor = anchor;
    init.owner = owner;
    return economy.create(world, init);
  }
  ObjectId object_of(SettlementId id) {
    const Settlement* s = economy.settlements().find(id);
    return s == nullptr ? kNoObject : s->object;
  }
  void rebuild() { world.mutable_gaika().build(world, world.lsa(), economy.settlements()); }

  /// A living ship of `owner` on a cell, running `verb` or nothing.
  ObjectId ship(std::int32_t cx, std::int32_t cy, PlayerId owner, const char* verb) {
    const ObjectId id = world.spawn(NativeClass::ship, nullptr, graph.find("Warship"));
    CHECK(world.set_owner(id, owner));
    CHECK(world.set_health(id, 100));
    CHECK(world.set_position(id, in_cell(cx, cy)));
    if (verb != nullptr) commands.set_command(world, id, verb, Command{});
    return id;
  }

  script::HostOutcome call(script::CallKind kind, const char* name,
                           std::vector<script::Value> args) {
    const std::uint32_t index =
        registry.find(kind, name,
                      static_cast<std::uint16_t>(kind == script::CallKind::member ? args.size() - 1
                                                                                  : args.size()));
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    return registry.entry(index).fn(ctx);
  }
  std::int32_t path(LsaId from, LsaId to, std::int32_t player) {
    const script::HostOutcome out =
        call(script::CallKind::free_function, "CheckLsaPath",
             {script::Value::integer(from), script::Value::integer(to), script::Value::integer(player)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  }
};

}  // namespace

/// The slot grid and the neighbour lists: a sea between two islands, joined to
/// both and to nothing else; a slot the sea touched first is the sea's even
/// where its centre is land; the pond claims and poisons its slots.
TEST(lsa_neighbours_are_the_other_type_across_the_slot_grid) {
  ShoreBench b;
  const LsaPartition& areas = b.world.lsa();
  REQUIRE(areas.size() == 3);
  const LsaId sea = areas.at(in_cell(1, 1));
  const LsaId west = areas.at(in_cell(6, 6));
  const LsaId east = areas.at(in_cell(20, 6));
  REQUIRE(sea == 1);
  REQUIRE(west == 2);
  REQUIRE(east == 3);
  CHECK(areas.water(sea));
  CHECK(!areas.water(west));
  CHECK(!areas.water(east));
  // The strait is the sea's, and the pond is not a place.
  CHECK(areas.at(in_cell(14, 6)) == sea);
  CHECK(areas.at(in_cell(26, 6)) == kNoLsa);

  // Slots are 128 units: two cells. Row 1 of slots straddles the coast and
  // the sea, filled first, claimed all of it -- its centres over the islands
  // are land, which is a land vote for the sea and the sea is water anyway.
  CHECK(areas.slot_columns() == 16);
  CHECK(areas.slot_rows() == 7);
  CHECK(areas.slot_label(0, 0) == sea);
  CHECK(areas.slot_label(3, 1) == sea);   // over the western island's north shore
  CHECK(areas.slot_label(3, 2) == west);
  CHECK(areas.slot_label(6, 2) == sea);   // the strait
  CHECK(areas.slot_label(8, 2) == east);
  CHECK(areas.slot_label(-1, 0) == kNoLsa);
  CHECK(areas.slot_label(16, 0) == kNoLsa);
  // The pond's free cells touch slots (12..13, 2..3). Both islands are
  // seeded in the *water* pass, before it -- the terrain test rounds by half
  // a cell, so a land cell in the last quarter of a coastal terrain cell
  // samples the sea beside it (the original's rounding, `sim/lsa.hpp`) and
  // the row-major scan reaches those seeds on row 13, above the pond -- so
  // the eastern island has three of the four already, and the discarded pond
  // poisons the one it alone touched: nobody's, though the island's cells
  // stand in it.
  CHECK(areas.at(in_cell(24, 4)) == east);
  CHECK(areas.slot_label(12, 2) == east);
  CHECK(areas.slot_label(12, 3) == east);
  CHECK(areas.slot_label(13, 3) == kNoLsa);
  CHECK(areas.at(Point{104 * 16 + 8, 24 * 16 + 8}) == kNoLsa);  // the pond itself

  const auto list = [&](LsaId id) {
    const std::span<const LsaId> span = areas.neighbours(id);
    return std::vector<LsaId>(span.begin(), span.end());
  };
  const std::vector<LsaId> both{west, east};
  const std::vector<LsaId> only_sea{sea};
  CHECK(list(sea) == both);
  CHECK(list(west) == only_sea);
  CHECK(list(east) == only_sea);
  CHECK(list(kNoLsa).empty());
  CHECK(list(4).empty());
}


/// `PrepareAiTransportShip(srcLsa, dstLsa, player, cmd, pt)` -- the crossing,
/// and the last name that stood between a reachable shipped script and its run.
///
/// It runs the same search `CheckLsaPath` runs, refuses anything but a length
/// of three, takes the **sea in the middle of the route**, finds a ship of the
/// player's standing in it, and writes the caller's order onto that ship.
TEST(prepare_ai_transport_ship_takes_the_sea_in_the_middle_and_tasks_a_ship) {
  ShoreBench b;
  const LsaPartition& areas = b.world.lsa();
  const LsaId sea = areas.at(in_cell(1, 1));
  const LsaId west = areas.at(in_cell(6, 6));
  const LsaId east = areas.at(in_cell(20, 6));
  REQUIRE(sea != kNoLsa && west != kNoLsa && east != kNoLsa);
  const Point beach = in_cell(20, 6);

  const auto prepare = [&](LsaId from, LsaId to, std::int32_t player, const char* verb) {
    const script::HostOutcome out =
        b.call(script::CallKind::free_function, "PrepareAiTransportShip",
               {script::Value::integer(from), script::Value::integer(to),
                script::Value::integer(player), script::Value::string(verb), pack_point(beach)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() ? out.value.as_object().id : kNoObject;
  };

  // **No ship in the sea: no crossing.** The route is the same one
  // `CheckLsaPath` refuses, so this refuses with it.
  CHECK(prepare(west, east, 1, "move") == kNoObject);

  // An idle ship of the player's in the sea is taken, and takes the order.
  const ObjectId idle = b.ship(14, 6, 0, "idle");
  CHECK(prepare(west, east, 1, "move") == idle);
  CHECK(b.ai.ship_transport(idle).order == "move");
  CHECK(b.ai.ship_transport(idle).where == beach);

  // **A length that is not three is refused**, which is the whole of the
  // shape: the same area is a march, and two areas of different types have no
  // route at all.
  CHECK(prepare(west, west, 1, "move") == kNoObject);
  CHECK(prepare(west, sea, 1, "move") == kNoObject);
  CHECK(prepare(0, east, 1, "move") == kNoObject);
  // A player number outside 1..16 names nobody and crosses nothing. **That is
  // an early-out rather than a guard**: the route search itself refuses to
  // cross water for a player it cannot name, so the length would be 0 anyway.
  CHECK(prepare(west, east, 0, "move") == kNoObject);
  CHECK(prepare(west, east, 40, "move") == kNoObject);

  // **A route of five is not a crossing**, which is what `cmp eax, 3` says and
  // the only shape that can show it: land, sea, land, sea, land, with a ship
  // in each sea. One hop this entry point can arrange; two it cannot.
  {
    ShoreBench two(kTwoSeas);
    const LsaPartition& cut = two.world.lsa();
    const LsaId left = cut.at(in_cell(2, 6));
    const LsaId middle = cut.at(in_cell(15, 6));
    const LsaId right = cut.at(in_cell(29, 6));
    REQUIRE(cut.size() == 5);
    (void)two.ship(9, 6, 0, "idle");
    (void)two.ship(22, 6, 0, "idle");
    const auto across = [&](LsaId from, LsaId to) {
      const script::HostOutcome out =
          two.call(script::CallKind::free_function, "PrepareAiTransportShip",
                   {script::Value::integer(from), script::Value::integer(to),
                    script::Value::integer(1), script::Value::string("move"), pack_point(beach)});
      CHECK(out.status == script::HostStatus::ok);
      return out.value.is_object() ? out.value.as_object().id : kNoObject;
    };
    // One sea each way: taken, and **the sea taken is the one on that side**,
    // which is what reading element 1 of the route means.
    const ObjectId west_ship = across(left, middle);
    const ObjectId east_ship = across(middle, right);
    CHECK(west_ship != kNoObject);
    CHECK(east_ship != kNoObject);
    CHECK(west_ship != east_ship);
    CHECK(cut.at(two.world.resolve_position(west_ship)) == cut.at(in_cell(9, 6)));
    CHECK(cut.at(two.world.resolve_position(east_ship)) == cut.at(in_cell(22, 6)));
    // Two seas: refused.
    CHECK(across(left, right) == kNoObject);
  }

  // **Whose ship, and where it is standing.** Someone else's, one with no
  // running command, a dead one and one on the beach are all refused; the
  // idle one in the sea is what answers.
  {
    ShoreBench c;
    (void)c.ship(14, 6, 1, "idle");   // another player's
    (void)c.ship(15, 6, 0, nullptr);  // no command at all
    const ObjectId sunk = c.ship(16, 6, 0, "idle");
    CHECK(c.world.set_health(sunk, 0));
    (void)c.ship(6, 5, 0, "idle");  // beached, and so not in the sea
    const script::HostOutcome out =
        c.call(script::CallKind::free_function, "PrepareAiTransportShip",
               {script::Value::integer(west), script::Value::integer(east),
                script::Value::integer(1), script::Value::string("move"), pack_point(beach)});
    CHECK(out.value.is_object());
    CHECK(out.value.as_object().id == kNoObject);
  }
}

/// The three verbs a candidate ship may be running, and the one of them the
/// transport order made real.
TEST(prepare_ai_transport_ship_reads_the_ships_running_command) {
  const Point beach = in_cell(20, 6);
  const auto crossing = [&](const char* verb, const char* order, Point where,
                            const char* wanted) {
    ShoreBench b;
    const LsaPartition& areas = b.world.lsa();
    const LsaId west = areas.at(in_cell(6, 6));
    const LsaId east = areas.at(in_cell(20, 6));
    const ObjectId boat = b.ship(14, 6, 0, verb);
    if (order != nullptr) b.ai.set_ship_transport(boat, order, where);
    const script::HostOutcome out =
        b.call(script::CallKind::free_function, "PrepareAiTransportShip",
               {script::Value::integer(west), script::Value::integer(east),
                script::Value::integer(1), script::Value::string(wanted), pack_point(beach)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_object() && out.value.as_object().id == boat;
  };

  // `idle` and `advance` are taken outright.
  CHECK(crossing("idle", nullptr, Point{}, "move"));
  CHECK(crossing("advance", nullptr, Point{}, "move"));
  // Anything else is not this ship.
  CHECK(!crossing("move", nullptr, Point{}, "move"));
  CHECK(!crossing("sneak", nullptr, Point{}, "move"));
  CHECK(!crossing("capture", nullptr, Point{}, "move"));

  // **`boardunit` is the clause the transport order made real.** A ship already
  // boarding for *this* crossing is reused; one boarding for a different verb
  // or a different destination is refused, and so is one boarding for nothing
  // at all. That last case used to be the only one: with no writer in the tree
  // every ship carried `""` and `(-1, -1)`, so a boarding ship always counted.
  CHECK(crossing("boardunit", "move", beach, "move"));
  CHECK(!crossing("boardunit", "advance", beach, "move"));
  CHECK(!crossing("boardunit", "move", in_cell(19, 6), "move"));
  CHECK(!crossing("boardunit", nullptr, Point{}, "move"));
  // ...and an empty order matches an empty request: the test is equality on
  // both fields and not "does it carry something".
  CHECK(crossing("boardunit", "", beach, ""));
  CHECK(!crossing("boardunit", "", beach, "move"));
}

/// `CheckLsaPath`: one for the same area, zero across types, and across the
/// water only with an idle, boarding or advancing ship of the player -- and a
/// refused crossing bumps the ship need on the sea that lacked one.
TEST(check_lsa_path_counts_the_areas_and_needs_a_ship_to_cross) {
  ShoreBench b;
  const LsaPartition& areas = b.world.lsa();
  const LsaId sea = areas.at(in_cell(1, 1));
  const LsaId west = areas.at(in_cell(6, 6));
  const LsaId east = areas.at(in_cell(20, 6));
  REQUIRE(sea != kNoLsa && west != kNoLsa && east != kNoLsa);

  // The same id answers 1 before anything is looked at.
  CHECK(b.path(west, west, 1) == 1);
  CHECK(b.path(0, 0, 1) == 1);
  CHECK(b.path(99, 99, 1) == 1);
  // Different types, or an id that names nothing: 0.
  CHECK(b.path(west, sea, 1) == 0);
  CHECK(b.path(sea, west, 1) == 0);
  CHECK(b.path(west, 99, 1) == 0);
  CHECK(b.path(0, west, 1) == 0);

  // No ship: no path, and the sea is where one was wanted.
  CHECK(b.ai.ship_needs(0, sea) == 0);
  CHECK(b.path(west, east, 1) == 0);
  CHECK(b.ai.ship_needs(0, sea) == 1);
  CHECK(b.path(east, west, 1) == 0);
  CHECK(b.ai.ship_needs(0, sea) == 2);
  // A player outside the table walks but neither crosses nor bumps.
  CHECK(b.path(west, east, 0) == 0);
  CHECK(b.path(west, east, 40) == 0);
  CHECK(b.ai.ship_needs(0, sea) == 2);

  // A ship of someone else, a ship with no command, one on a job, a dead
  // one, and one of the player's own on land: none of them is a crossing...
  const ObjectId theirs = b.ship(1, 1, 1, "idle");
  const ObjectId silent = b.ship(1, 2, 0, nullptr);
  const ObjectId busy = b.ship(2, 1, 0, "move");
  const ObjectId sunk = b.ship(2, 2, 0, "idle");
  CHECK(b.world.set_health(sunk, 0));
  const ObjectId beached = b.ship(6, 5, 0, "idle");
  // ...and the player *has* ships in the sea now, whatever they are doing,
  // so the sea is no longer short of one: nothing is bumped.
  CHECK(b.path(west, east, 1) == 0);
  CHECK(b.ai.ship_needs(0, sea) == 2);
  (void)theirs;
  (void)silent;
  (void)busy;
  (void)beached;

  // An idle ship of the player's in the sea: west, sea, east -- three areas.
  // The player has a ship there now, so nothing is bumped.
  const ObjectId idle = b.ship(14, 6, 0, "idle");
  CHECK(b.path(west, east, 1) == 3);
  CHECK(b.path(east, west, 1) == 3);
  CHECK(b.ai.ship_needs(0, sea) == 2);
  // Player 2's own count is untouched throughout.
  CHECK(b.ai.ship_needs(1, sea) == 0);

  // Boarding and advancing count too; a ship on any other job does not.
  b.commands.clear_commands(b.world, idle);
  b.commands.set_command(b.world, idle, "boardunit", Command{});
  CHECK(b.path(west, east, 1) == 3);
  b.commands.clear_commands(b.world, idle);
  b.commands.set_command(b.world, idle, "advance", Command{});
  CHECK(b.path(west, east, 1) == 3);
  b.commands.clear_commands(b.world, idle);
  b.commands.set_command(b.world, idle, "Idle", Command{});  // byte-for-byte
  CHECK(b.path(west, east, 1) == 0);
  // ...and with a ship present but busy, the sea is not short of ships.
  CHECK(b.ai.ship_needs(0, sea) == 2);
}

/// `SupplyCount`: the settlements of the same owner, on the class, whose own
/// `BestToSupply` is the receiver.
TEST(supply_count_is_the_villages_that_feed_this_stronghold) {
  ShoreBench b;
  const SettlementId hall = b.plant(4, 6, 1, SettlementKind::stronghold, "Townhall");
  const SettlementId far_hall = b.plant(11, 6, 1, SettlementKind::stronghold, "Townhall");
  const SettlementId near_one = b.plant(5, 4, 1, SettlementKind::village, "Village");
  const SettlementId near_two = b.plant(3, 5, 1, SettlementKind::village, "BaseVillage");
  const SettlementId theirs = b.plant(6, 5, 2, SettlementKind::village, "Village");
  const SettlementId far_one = b.plant(10, 4, 1, SettlementKind::village, "Village");
  const SettlementId overseas = b.plant(20, 6, 1, SettlementKind::village, "Village");
  // A twin hall one cell below a village that is one cell below the hall: a
  // tie, and the earlier settlement keeps it.
  const SettlementId tied = b.plant(4, 7, 1, SettlementKind::village, "BaseVillage");
  const SettlementId twin = b.plant(4, 8, 1, SettlementKind::stronghold, "Townhall");
  b.rebuild();
  (void)tied;
  (void)theirs;
  (void)far_one;
  (void)overseas;

  const auto count = [&](SettlementId of, const char* cls) {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "SupplyCount",
               {script::Value::object(kTypeSettlement, b.object_of(of)), script::Value::string(cls)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  // Three villages are nearer the western hall, one of them by the tie; one
  // is nearer the far hall; player 2's is not player 1's; the overseas one
  // has no hall in its area.
  CHECK(count(hall, "BaseVillage") == 3);
  CHECK(count(far_hall, "BaseVillage") == 1);
  CHECK(count(twin, "BaseVillage") == 0);
  CHECK(count(hall, "Village") == 1);  // descent: near_two and tied are the base class itself
  CHECK(count(hall, "Nonesuch") == 0);
  // A hall supplies itself, at distance zero, and counts when asked by class.
  CHECK(count(hall, "Townhall") == 1);
  // A village asked the question feeds nothing: it is nobody's target.
  CHECK(count(near_one, "BaseVillage") == 0);
  (void)near_one;
  (void)near_two;
}

/// `ApproachingSquads`: the player's strength bound for the node from
/// outside it, counted by the leader's distance, strictly within.
TEST(approaching_squads_sums_the_strength_on_its_way_to_the_node) {
  ShoreBench b;
  const SettlementId hall = b.plant(4, 6, 1, SettlementKind::stronghold, "Townhall");
  const SettlementId other = b.plant(10, 6, 1, SettlementKind::stronghold, "Townhall");
  b.rebuild();
  const GaikaId here = b.world.gaika().for_settlement(b.object_of(hall));
  const GaikaId there = b.world.gaika().for_settlement(b.object_of(other));
  REQUIRE(here != kNoGaika && there != kNoGaika && here != there);

  const auto leader = [&](PlayerId owner, Point at) {
    const ObjectId id = b.world.spawn(NativeClass::unit, nullptr);
    CHECK(b.world.set_owner(id, owner));
    CHECK(b.world.set_health(id, 100));
    CHECK(b.world.set_position(id, at));
    return id;
  };
  const auto band = [&](PlayerId owner, GaikaId in, GaikaId ai_dest, GaikaId dest, ObjectId lead,
                        std::int32_t eval) -> SquadKey {
    const SquadKey key = b.heroes.squads().create(owner);
    if (lead != kNoObject) CHECK(b.heroes.squads().join(key, lead));
    Squad* squad = b.heroes.squads().find(key);
    CHECK(squad != nullptr);
    if (squad == nullptr) return key;
    squad->leader = lead;
    squad->gaika_in = in;
    squad->ai_dest = ai_dest;
    squad->dest_gaika = dest;
    squad->eval = eval;
    return key;
  };
  const Point centre = b.world.gaika().find(here)->center;
  // 500 away, and 300 away, both bound here from the other node.
  band(0, there, here, kNoGaika, leader(0, Point{centre.x + 500, centre.y}), 10);
  band(0, there, here, kNoGaika, leader(0, Point{centre.x, centre.y + 300}), 20);
  // Already here; bound elsewhere; no leader; someone else's.
  band(0, here, here, kNoGaika, leader(0, Point{centre.x + 100, centre.y}), 100);
  band(0, there, there, kNoGaika, leader(0, Point{centre.x + 100, centre.y}), 100);
  band(0, there, here, kNoGaika, kNoObject, 100);
  band(1, there, here, kNoGaika, leader(1, Point{centre.x + 100, centre.y}), 100);
  // Bound here by `DestGAIKA` alone -- `AIDest`'s own fallback.
  const SquadKey fallback =
      band(0, there, kNoGaika, here, leader(0, Point{centre.x - 400, centre.y}), 40);

  const auto coming = [&](std::int32_t player, std::int32_t dist) {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "ApproachingSquads",
               {script::Value::integer(here), script::Value::integer(player),
                script::Value::integer(dist)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  };
  CHECK(coming(1, 1000) == 70);
  CHECK(coming(1, 501) == 70);
  CHECK(coming(1, 500) == 60);   // strictly within
  CHECK(coming(1, 400) == 20);
  CHECK(coming(1, 300) == 0);
  CHECK(coming(2, 1000) == 100); // someone else's counts for them alone
  CHECK(coming(0, 1000) == 0);
  CHECK(coming(1, -1) == 0);

  // The fallback is `AIDest`'s as well, so the two never disagree.
  const script::HostOutcome dest =
      b.call(script::CallKind::member, "AIDest", {pack_squad(fallback)});
  CHECK(dest.status == script::HostStatus::ok);
  CHECK(gaika_of(dest.value) == here);
}

/// A partition alone, for the slot-grid cases that need no systems.
static LsaPartition partition_of(std::initializer_list<std::string_view> picture) {
  const std::vector<std::byte> terrain = terrain_from(picture);
  const std::vector<std::byte> pass = pass_walling_shore_sized(picture);
  const Result<Grid> layer = Grid::parse(terrain);
  CHECK(layer.ok());
  LsaPartition areas;
  if (layer.ok()) areas.build(layer.value(), obstruction(pass));
  return areas;
}

/// A discarded pond keeps its claims: the land around it stands in four slots
/// it cannot own.
TEST(lsa_a_discarded_pond_poisons_the_slots_it_touched) {
  const LsaPartition areas = partition_of(kPond);
  REQUIRE(areas.size() == 1);
  CHECK(!areas.water(1));
  CHECK(areas.at(in_cell(1, 1)) == kNoLsa);  // the pond
  CHECK(areas.at(in_cell(8, 8)) == 1);
  // The pond's free cells run from passability cell 0 to 10 on both axes:
  // slots 0 and 1, both ways. The land stands in three of them and owns none;
  // the fourth is the pond's alone.
  CHECK(areas.at(Point{13 * 16 + 8, 1 * 16 + 8}) == 1);   // land in slot (1, 0)
  CHECK(areas.at(Point{1 * 16 + 8, 13 * 16 + 8}) == 1);   // land in slot (0, 1)
  CHECK(areas.slot_label(0, 0) == kNoLsa);
  CHECK(areas.slot_label(1, 0) == kNoLsa);
  CHECK(areas.slot_label(0, 1) == kNoLsa);
  CHECK(areas.slot_label(1, 1) == kNoLsa);
  CHECK(areas.slot_label(2, 0) == 1);
  CHECK(areas.slot_label(0, 2) == 1);
}

/// The census votes one slot per claim, at the slot's centre: a lake whose
/// claims reach a slot past its water on two sides is more coastline than
/// water, and it is typed ground -- and so is not a neighbour of the land.
TEST(lsa_a_lake_that_is_mostly_coastline_is_typed_ground) {
  const LsaPartition areas = partition_of(kLake);
  REQUIRE(areas.size() == 2);
  const LsaId lake = areas.at(in_cell(2, 2));
  const LsaId land = areas.at(in_cell(12, 12));
  REQUIRE(lake == 1);
  REQUIRE(land == 2);
  // Nine claims, four wet centres: (1,1) (3,1) (1,3) (3,3); the other five
  // sample land at column and row 5. Four is not more than half of nine.
  CHECK(areas.slot_label(0, 0) == lake);
  CHECK(areas.slot_label(2, 2) == lake);
  CHECK(areas.slot_label(2, 0) == lake);
  CHECK(areas.slot_label(3, 0) == land);
  CHECK(!areas.water(lake));
  CHECK(!areas.water(land));
  CHECK(areas.neighbours(lake).empty());
  CHECK(areas.neighbours(land).empty());
}

/// Adjacency looks at all eight neighbouring slots: a sea whose only contact
/// with the land is corner to corner is still its neighbour.
TEST(lsa_neighbours_touch_on_the_diagonal_too) {
  const LsaPartition areas = partition_of(kCorner);
  REQUIRE(areas.size() == 2);
  const LsaId sea = areas.at(in_cell(1, 1));
  const LsaId land = areas.at(in_cell(12, 12));
  REQUIRE(sea == 1);
  REQUIRE(land == 2);
  CHECK(areas.water(sea));
  CHECK(areas.slot_label(3, 1) == sea);
  CHECK(areas.slot_label(4, 1) == kNoLsa);  // the cliff
  CHECK(areas.slot_label(3, 2) == kNoLsa);  // the cliff
  CHECK(areas.slot_label(4, 2) == land);
  const std::vector<LsaId> only_land{land};
  const std::vector<LsaId> only_sea{sea};
  CHECK(std::vector<LsaId>(areas.neighbours(sea).begin(), areas.neighbours(sea).end()) == only_land);
  CHECK(std::vector<LsaId>(areas.neighbours(land).begin(), areas.neighbours(land).end()) == only_sea);
}

/// Where a ship would have helped: the first ship-less sea seen wins a tie,
/// nothing is bumped when the destination is reached, and a ship the player
/// does not own is not the player's.
TEST(check_lsa_path_bumps_the_first_shipless_sea_it_saw) {
  ShoreBench b(kTwoLakes);
  const LsaPartition& areas = b.world.lsa();
  REQUIRE(areas.size() == 4);
  const LsaId first = areas.at(in_cell(4, 4));
  const LsaId second = areas.at(in_cell(16, 9));
  const LsaId north = areas.at(in_cell(12, 1));
  const LsaId south = areas.at(in_cell(4, 14));
  // The lakes are seeded in scan order, the first above the second; which
  // ids the lands take is the rounding's business (`sim/lsa.hpp`).
  REQUIRE(first != kNoLsa && second != kNoLsa && north != kNoLsa && south != kNoLsa);
  REQUIRE(first < second);
  REQUIRE(north != south && !areas.water(north) && !areas.water(south));
  CHECK(areas.water(first) && areas.water(second));
  const auto list = [&](LsaId id) {
    return std::vector<LsaId>(areas.neighbours(id).begin(), areas.neighbours(id).end());
  };
  const std::vector<LsaId> both{first, second};
  const std::vector<LsaId> only_second{second};
  CHECK(list(north) == both);
  CHECK(list(south) == only_second);

  // No ships anywhere: both lakes are seen at once, unreached; the first is
  // taken and the second is not nearer.
  CHECK(b.path(north, south, 1) == 0);
  CHECK(b.ai.ship_needs(0, first) == 1);
  CHECK(b.ai.ship_needs(0, second) == 0);
  // A ship on a job in the second: still no crossing, and the second lake is
  // not short of ships, so the first is bumped again.
  const ObjectId busy = b.ship(16, 9, 0, "move");
  CHECK(b.path(north, south, 1) == 0);
  CHECK(b.ai.ship_needs(0, first) == 2);
  CHECK(b.ai.ship_needs(0, second) == 0);
  // Idle: north, second lake, south. The first lake is still ship-less and
  // still seen, and nothing is bumped because the walk arrived.
  b.commands.clear_commands(b.world, busy);
  b.commands.set_command(b.world, busy, "idle", Command{});
  CHECK(b.path(north, south, 1) == 3);
  CHECK(b.path(south, north, 1) == 3);
  CHECK(b.ai.ship_needs(0, first) == 2);
  // An unowned idle ship in the second lake is nobody's: a player outside
  // the table does not cross on it.
  (void)b.ship(16, 8, kNoPlayer, "idle");
  CHECK(b.path(north, south, 0) == 0);
  CHECK(b.path(north, south, 40) == 0);
  CHECK(b.ai.ship_needs(0, first) == 2);
}

// --------------------------------------------------------------------------
// what a squad is worth, where it stands, and the spell that reads both
// --------------------------------------------------------------------------

namespace {

/// `SiegeBench` with a turn to run, because the two fields under test are the
/// ones `HeroSystem::advance` writes.
struct StrengthBench : SiegeBench {
  std::uint64_t turns = 0;

  /// One turn, which is all `revalue_squads` needs: it is a pure function of
  /// the world, so a second turn changes nothing a first did not.
  void step() {
    ++turns;
    Turn turn;
    turn.index = turns;
    turn.length = 100;
    turn.start = static_cast<GameTime>((turns - 1) * 100 + 1);
    turn.end = static_cast<GameTime>(turns * 100);
    heroes.advance(world, turn);
  }

  /// What one unit is worth to the census, asked of the same function the sum
  /// is built from -- so the arithmetic is asserted once, in `test_match.cpp`,
  /// and these cases assert the *sum* rather than restating the formula.
  [[nodiscard]] std::int32_t worth(ObjectId id) {
    const WorldObject* slot = world.find(id);
    CHECK(slot != nullptr);
    return slot == nullptr ? 0 : object_power(world, &combat, *slot);
  }

  /// A squad with no fields written by hand, which is the point: `band` fills
  /// `eval` and `gaika_in` in because nothing used to, and these cases are
  /// about the thing that now does.
  SquadKey raw(PlayerId owner, std::vector<ObjectId> members) {
    const SquadKey key = heroes.squads().create(owner);
    for (const ObjectId id : members) CHECK(heroes.squads().join(key, id));
    return key;
  }
};

}  // namespace

/// `Squad::Eval` is the sum of its members' census valuations, and it tracks
/// every term of that valuation because the original re-runs it on every health
/// write and every stat recalc.
TEST(squad_eval_is_the_census_valuation_of_everyone_in_it) {
  StrengthBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId first = b.trooper("Legionary", 2, 2, 2);
  const ObjectId second = b.trooper("Archer", 2, 2, 3);
  b.arm();
  const SquadKey key = b.raw(2, {first, second});

  // Nothing has run yet, and that is the state every shipped map used to be
  // stuck in: the field exists, nine readers branch on it, and it is zero.
  REQUIRE(b.heroes.squads().find(key) != nullptr);
  CHECK(b.heroes.squads().find(key)->eval == 0);

  b.step();
  const std::int32_t both = b.worth(first) + b.worth(second);
  REQUIRE(both > 0);
  CHECK(b.heroes.squads().find(key)->eval == both);

  // Health is a term of the valuation, so a wounded member is worth less --
  // which in the original is 0x005d3a80 subtracting before the write and adding
  // after, and here is the sum being taken again.
  CHECK(b.world.set_health(first, 20));
  b.step();
  const std::int32_t hurt = b.worth(first) + b.worth(second);
  CHECK(hurt < both);
  CHECK(b.heroes.squads().find(key)->eval == hurt);

  // And a member who leaves takes its share with it.
  CHECK(b.heroes.squads().leave(key, second));
  b.step();
  CHECK(b.heroes.squads().find(key)->eval == b.worth(first));
}

/// A squad is filed under the node its **first member** stands in, and it stops
/// being filed anywhere when it empties.
TEST(squad_gaika_in_follows_the_first_member_and_src_gaika_is_stamped_once) {
  StrengthBench b;
  b.plant(3, 3);   // node 1, the western island
  b.plant(10, 3);  // node 2, the eastern one
  b.rebuild();
  REQUIRE(b.world.gaika().count() >= 2);
  const GaikaId west = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  const GaikaId east = b.world.gaika().at(b.world.lsa(), in_cell(11, 3));
  REQUIRE(west != east);
  REQUIRE(west != kNoGaika);

  const ObjectId lead = b.trooper("Legionary", 2, 2, 2);
  const ObjectId back = b.trooper("Archer", 2, 11, 3);
  b.arm();
  const SquadKey key = b.raw(2, {lead, back});

  b.step();
  const Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  // The *first* member decides it, and the second one standing on the far
  // island does not get a vote.
  CHECK(squad->gaika_in == west);
  CHECK(squad->src_gaika == west);

  // Move the first member and the squad moves with it.
  CHECK(b.world.set_position(lead, in_cell(11, 4)));
  b.step();
  CHECK(b.heroes.squads().find(key)->gaika_in == east);
  // `SrcGAIKA` is written the first time it is anything but zero and never
  // again: it is where the squad was raised, not where it is.
  CHECK(b.heroes.squads().find(key)->src_gaika == west);
}

namespace {

/// `GetGAIKA(u)` on an object, through the registry, as a script reaches it.
GaikaId gaika_of_object(SiegeBench& b, ObjectId id) {
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index = registry.find(script::CallKind::free_function, "GetGAIKA", 1);
  CHECK(index != script::kUnresolvedHost);
  if (index == script::kUnresolvedHost) return kNoGaika;
  std::vector<script::Value> args{script::Value::object(script::TypeId{1}, id)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "GetGAIKA";
  ctx.kind = script::CallKind::free_function;
  const script::HostOutcome out = registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  return out.value.is_integer() ? static_cast<GaikaId>(out.value.as_integer()) : kNoGaika;
}

}  // namespace

/// A squad whose leader is in a town's garrison is filed under **the town's
/// node**: its `posRH` (0x005d3db0) is the central building's position, and
/// that is what the node tracker 0x0041f530 is handed. So is `GetGAIKA(u)`
/// (0x0044e650) for the garrisoned unit itself.
///
/// The holder record a garrisoned unit is in stands at (0, 0), which here is
/// the western sea, nearest the western town's node. Filed by that, every
/// squad that went home on Crossroads was "elsewhere": `AIOSENDSQUAD.VS` sent
/// it to its own town -- `GetDestPoint`, 450 units outside the gate -- and the
/// units that came out to go there stood on the door they came out of.
TEST(a_garrisoned_squad_is_filed_under_its_towns_node) {
  StrengthBench b;
  b.plant(3, 3);                             // the western town, nearest the corner
  const SettlementId east = b.plant(9, 10);  // the eastern one, far from it
  b.rebuild();
  const GaikaId west_node = b.world.gaika().at(b.world.lsa(), in_cell(3, 3));
  const GaikaId east_node = b.world.gaika().at(b.world.lsa(), in_cell(9, 10));
  REQUIRE(west_node != kNoGaika);
  REQUIRE(east_node != kNoGaika);
  REQUIRE(west_node != east_node);
  // The control: where the holder record stands is the western node's.
  const Settlement* town = b.economy.settlements().find(east);
  REQUIRE(town != nullptr);
  REQUIRE(b.world.resolve_position(town->holder.object) == (Point{0, 0}));
  REQUIRE(b.world.gaika().at(b.world.lsa(), (Point{0, 0})) == west_node);

  const ObjectId lead = b.trooper("Legionary", 1, 9, 11);
  b.arm();
  const SquadKey key = b.raw(1, {lead});
  b.step();
  REQUIRE(b.heroes.squads().find(key) != nullptr);
  REQUIRE(b.heroes.squads().find(key)->gaika_in == east_node);
  CHECK(gaika_of_object(b, lead) == east_node);

  // In: still the eastern town's.
  REQUIRE(garrison_enter(b.world, east, lead, /*force=*/true));
  REQUIRE(b.world.state(lead)->is_held());
  CHECK(unit_pos_rh(b.world, lead) == in_cell(9, 10));
  b.step();
  CHECK(b.heroes.squads().find(key)->gaika_in == east_node);
  CHECK(gaika_of_object(b, lead) == east_node);

  // Out again, on the map: its own position decides once more.
  CHECK(garrison_exit(b.world, lead, in_cell(9, 13), b.world.time(), /*throttled=*/false) == 0);
  REQUIRE(!b.world.state(lead)->is_held());
  CHECK(unit_pos_rh(b.world, lead) == b.world.resolve_position(lead));
  b.step();
  CHECK(b.heroes.squads().find(key)->gaika_in == east_node);
}

namespace {

/// `u.BestMDPos(md, min, max, minEval, protect)`, through the registry.
Point best_md_pos(SiegeBench& b, ObjectId caster, std::int32_t md, std::int32_t min_range,
                  std::int32_t max_range, std::int32_t min_eval, bool protect) {
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index = registry.find(script::CallKind::member, "BestMDPos", 5);
  CHECK(index != script::kUnresolvedHost);
  if (index == script::kUnresolvedHost) return Point{-1, -1};
  std::vector<script::Value> args{script::Value::object(script::TypeId{1}, caster),
                                  script::Value::integer(md),
                                  script::Value::integer(min_range),
                                  script::Value::integer(max_range),
                                  script::Value::integer(min_eval),
                                  script::Value::boolean(protect)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "BestMDPos";
  ctx.kind = script::CallKind::member;
  const script::HostOutcome out = registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  return unpack_point(out.value);
}

constexpr Point kNoPos{-1, -1};

}  // namespace

/// The candidate set is the enemy squads filed under the **caster's own
/// squad's** node, and the point offered is each one's first member.
TEST(best_md_pos_offers_the_enemy_squads_filed_under_the_casters_node) {
  SiegeBench b;
  b.plant(3, 3);
  b.plant(10, 3);
  b.rebuild();
  const GaikaId west = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  const GaikaId east = b.world.gaika().at(b.world.lsa(), in_cell(11, 3));
  REQUIRE(west != east);

  const ObjectId druid = b.trooper("Legionary", 1, 2, 2);
  const ObjectId here = b.trooper("Archer", 2, 2, 4);
  const ObjectId elsewhere = b.trooper("Archer", 2, 2, 5);
  b.arm();
  b.band(1, west, {druid});
  b.band(2, west, {here});
  b.band(2, east, {elsewhere});

  // The one filed under this node wins, and the one filed under the other is
  // not offered at all -- even though it is nearer than half the map.
  CHECK(best_md_pos(b, druid, 500, 0, 5000, 1, false) == in_cell(2, 4));

  // A caster in no squad has no node to look in, and neither has one whose
  // squad is filed nowhere. Both are refusals before anything is measured.
  const ObjectId loner = b.trooper("Legionary", 1, 2, 2);
  CHECK(best_md_pos(b, loner, 500, 0, 5000, 1, false) == kNoPos);
  const SquadKey adrift = b.band(1, kNoGaika, {loner});
  CHECK(adrift.valid());
  // ...and an enemy filed nowhere alongside it is still not a candidate: node
  // zero is "no node", not a node that things can be found in.
  const ObjectId stray = b.trooper("Archer", 2, 2, 3);
  b.band(2, kNoGaika, {stray});
  CHECK(best_md_pos(b, loner, 500, 0, 5000, 1, false) == kNoPos);

  // A squad with no strength is not an army, which is the first test the walk
  // makes and the one that used to empty it on every map.
  Squad* enemy = b.heroes.squads().find(b.heroes.squads().squad_of(here));
  REQUIRE(enemy != nullptr);
  enemy->eval = 0;
  CHECK(best_md_pos(b, druid, 500, 0, 5000, 1, false) == kNoPos);
  enemy->eval = 10;

  // And an ally is not a target however strong it is.
  b.world.players().set_relation_word(1, 2, kRelationFriendly);
  CHECK(best_md_pos(b, druid, 500, 0, 5000, 1, false) == kNoPos);
}

/// The relation read is the **squad's owner**, and it is read off the caster's
/// own diplomacy row.
///
/// The squad belongs to player 3 and its one member to player 2, and player 1
/// is at war with 2 and at truce with 3. `Eval` still counts the member, so
/// the spot is worth hitting; what refuses it is the squad. Asking about the
/// member instead, or asking player 3's row instead of player 1's, both answer
/// the member's position.
TEST(best_md_pos_reads_the_squads_owner_off_the_casters_own_row) {
  SiegeBench b;
  b.plant(3, 3);
  b.rebuild();
  const GaikaId node = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  const ObjectId druid = b.trooper("Legionary", 1, 2, 2);
  const ObjectId lent = b.trooper("Archer", 2, 2, 4);
  b.arm();
  b.band(1, node, {druid});
  b.band(3, node, {lent});
  b.world.players().set_relation_word(1, 3, kRelationFriendly);

  CHECK(best_md_pos(b, druid, 500, 0, 5000, 1, false) == kNoPos);

  // One-directional: player 3 declaring player 1 an enemy changes nothing,
  // because the row consulted is player 1's.
  b.world.players().set_relation_word(3, 1, 0);
  CHECK(best_md_pos(b, druid, 500, 0, 5000, 1, false) == kNoPos);

  // And player 1 revoking the truce brings the whole squad back.
  b.world.players().set_relation_word(1, 3, 0);
  CHECK(best_md_pos(b, druid, 500, 0, 5000, 1, false) == in_cell(2, 4));
}

/// The distance band is inclusive at both ends, and the minimum is applied to
/// the score before the ranking bonus is added to it.
TEST(best_md_pos_bands_the_distance_and_floors_the_score) {
  SiegeBench b;
  b.plant(3, 3);
  b.rebuild();
  const GaikaId node = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  const ObjectId druid = b.trooper("Legionary", 1, 2, 2);
  const ObjectId mob = b.trooper("Archer", 2, 2, 4);
  b.arm();
  b.band(1, node, {druid});
  b.band(2, node, {mob});

  // Two cells apart, which is 128 world units.
  const Point target = in_cell(2, 4);
  CHECK(best_md_pos(b, druid, 500, 128, 128, 1, false) == target);
  CHECK(best_md_pos(b, druid, 500, 129, 5000, 1, false) == kNoPos);
  CHECK(best_md_pos(b, druid, 500, 0, 127, 1, false) == kNoPos);

  // The score is what `Eval` puts in the enemy bucket for a circle of `MDRange`
  // around the candidate -- one archer here, and nothing at all when the
  // circle is too small to reach itself is not a case: the candidate is always
  // inside its own circle, so the floor is what refuses it.
  const std::int32_t alone = object_power(b.world, &b.combat, *b.world.find(mob));
  REQUIRE(alone > 1);
  CHECK(best_md_pos(b, druid, 500, 0, 5000, alone, false) == target);
  CHECK(best_md_pos(b, druid, 500, 0, 5000, alone + 1, false) == kNoPos);

  // And the literal 3: the ranked value is the score plus `MaxRange - distance`
  // and it has to clear 3 whatever `MinEval` allowed. A unit with no attack at
  // all is worth exactly 1 to the census -- the `+ 1` the formula ends on --
  // so with `MaxRange` equal to the distance there is no bonus and 1 is
  // refused, while three units of bonus carry the same candidate over.
  CombatProfile harmless;
  harmless.max_health = 200;
  harmless.attack_interval = 1000;
  b.combat.set_profile(b.graph.find("Archer"), harmless);
  REQUIRE(object_power(b.world, &b.combat, *b.world.find(mob)) == 1);
  CHECK(best_md_pos(b, druid, 500, 0, 128, 0, false) == kNoPos);
  CHECK(best_md_pos(b, druid, 0, 0, 130, 0, false) == kNoPos);
  CHECK(best_md_pos(b, druid, 500, 0, 131, 0, false) == target);
}

/// Nearer wins between equals, the tie keeps the first offered, and
/// `bProtectFriendly` pays for what the blast would catch.
TEST(best_md_pos_prefers_the_nearer_crowd_and_pays_for_friendly_fire) {
  SiegeBench b;
  b.plant(3, 3);
  b.rebuild();
  const GaikaId node = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  const ObjectId druid = b.trooper("Legionary", 1, 2, 2);
  const ObjectId close = b.trooper("Archer", 2, 2, 3);
  const ObjectId far_off = b.trooper("Archer", 2, 2, 6);
  // Standing where the near crowd stands, and created before combat is
  // populated so that the census values it like anybody else.
  const ObjectId comrade = b.trooper("Legionary", 1, 2, 3);
  b.arm();
  b.band(1, node, {druid});
  // Joined far first, so a body that kept the first offered rather than the
  // best-ranked would answer the wrong one.
  b.band(2, node, {far_off});
  b.band(2, node, {close});
  // A circle small enough that neither candidate can see the other, so the two
  // scores are equal and only the distance bonus separates them.
  CHECK(best_md_pos(b, druid, 32, 0, 5000, 1, false) == in_cell(2, 3));

  // Equidistant, and the tie keeps the first offered -- squad order, which is
  // creation order, and not the lower id.
  SiegeBench t;
  t.plant(3, 3);
  t.rebuild();
  const GaikaId here = t.world.gaika().at(t.world.lsa(), in_cell(3, 3));
  const ObjectId caster = t.trooper("Legionary", 1, 3, 3);
  const ObjectId left = t.trooper("Archer", 2, 2, 3);
  const ObjectId right = t.trooper("Archer", 2, 4, 3);
  t.arm();
  t.band(1, here, {caster});
  REQUIRE(left < right);
  t.band(2, here, {right});
  t.band(2, here, {left});
  CHECK(best_md_pos(t, caster, 32, 0, 5000, 1, false) == in_cell(4, 3));

  // And the flag. The friendly standing where the near crowd stands is worth
  // exactly what the crowd is, so protecting it takes that spot's score to
  // zero and the far crowd wins instead -- while without the flag the near one
  // still does.
  CHECK(comrade != kNoObject);
  CHECK(object_power(b.world, &b.combat, *b.world.find(comrade)) ==
        object_power(b.world, &b.combat, *b.world.find(close)));
  CHECK(best_md_pos(b, druid, 32, 0, 5000, 1, false) == in_cell(2, 3));
  CHECK(best_md_pos(b, druid, 32, 0, 5000, 1, true) == in_cell(2, 6));

  // And an **ally** costs exactly what one's own men cost: the flag subtracts
  // both buckets, and a third player at truce standing on the far crowd takes
  // that spot's score to zero in its turn.
  SiegeBench a;
  a.plant(3, 3);
  a.rebuild();
  const GaikaId where = a.world.gaika().at(a.world.lsa(), in_cell(2, 2));
  const ObjectId caller = a.trooper("Legionary", 1, 2, 2);
  const ObjectId mob = a.trooper("Archer", 2, 2, 6);
  const ObjectId neutral = a.trooper("Legionary", 3, 2, 6);
  a.arm();
  a.world.players().set_relation_word(1, 3, kRelationFriendly);
  a.band(1, where, {caller});
  a.band(2, where, {mob});
  CHECK(best_md_pos(a, caller, 32, 0, 5000, 1, false) == in_cell(2, 6));
  CHECK(neutral != kNoObject);
  CHECK(best_md_pos(a, caller, 32, 0, 5000, 1, true) == kNoPos);
}

/// The two halves meet: a squad valued and filed by the turn, read by the spell
/// without a single field written by hand.
TEST(best_md_pos_reads_the_squads_the_turn_itself_valued_and_filed) {
  StrengthBench b;
  b.plant(3, 3);
  b.rebuild();
  const ObjectId druid = b.trooper("Legionary", 1, 2, 2);
  const ObjectId mob = b.trooper("Archer", 2, 2, 4);
  b.arm();
  const SquadKey ours = b.raw(1, {druid});
  const SquadKey theirs = b.raw(2, {mob});

  // Before the turn both squads are worth nothing and filed nowhere, so there
  // is no node to search and no army to find in it.
  CHECK(best_md_pos(b, druid, 500, 0, 5000, 1, false) == kNoPos);

  b.step();
  CHECK(b.heroes.squads().find(ours)->gaika_in != kNoGaika);
  CHECK(b.heroes.squads().find(theirs)->eval > 0);
  CHECK(best_md_pos(b, druid, 500, 0, 5000, 1, false) == in_cell(2, 4));
}

// --------------------------------------------------------------------------
// whom to shield
// --------------------------------------------------------------------------

namespace {

/// `SiegeBench` with a sacrifice class and the helpers `BestProtPos` needs.
///
/// A `Sacrifice` is the one branch of this entry point no shipped map can
/// reach -- `sim/world_host.cpp` records why -- so the only place it can be
/// exercised is a world built by hand, which is this one.
struct ShieldBench : SiegeBench {
  ShieldBench() {
    graph.add(bytes_of(R"(<class id="Ritual" cpp_class="CVXSacrifice" parent=""/>)"),
              "ritual.sc.xml");
    graph.link();
  }

  /// `n` units of `owner` standing on one cell, none of them in any squad.
  void crowd(const char* class_name, PlayerId owner, std::int32_t cx, std::int32_t cy,
             std::int32_t n, bool shielded = false) {
    for (std::int32_t i = 0; i < n; ++i) shield(trooper(class_name, owner, cx, cy), shielded);
  }

  /// `ObjectFlags::half_damage`, which is what a cover of mercy leaves behind.
  void shield(ObjectId id, bool on) {
    ObjectState* state = world.mutable_state(id);
    CHECK(state != nullptr);
    if (state != nullptr) state->flags.half_damage = on;
  }

  ObjectId ritual(PlayerId owner, std::int32_t cx, std::int32_t cy) {
    const ObjectId id = world.spawn(NativeClass::sacrifice, nullptr, graph.find("Ritual"));
    CHECK(world.set_owner(id, owner));
    CHECK(world.set_position(id, in_cell(cx, cy)));
    return id;
  }
};

/// `u.BestProtPos(range, min, max, minEval)`, through the registry.
ObjectId best_prot_pos(SiegeBench& b, ObjectId caster, std::int32_t range,
                       std::int32_t min_range, std::int32_t max_range, std::int32_t min_eval) {
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index = registry.find(script::CallKind::member, "BestProtPos", 4);
  CHECK(index != script::kUnresolvedHost);
  if (index == script::kUnresolvedHost) return kNoObject;
  std::vector<script::Value> args{script::Value::object(script::TypeId{1}, caster),
                                  script::Value::integer(range),
                                  script::Value::integer(min_range),
                                  script::Value::integer(max_range),
                                  script::Value::integer(min_eval)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "BestProtPos";
  ctx.kind = script::CallKind::member;
  const script::HostOutcome out = registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  if (!out.value.is_object() || out.value.as_object().type == script::kNoType) return kNoObject;
  return static_cast<ObjectId>(out.value.as_object().id);
}

}  // namespace

/// The two-thirds rule, and what it really measures.
///
/// The census **assigns** the shielding of the caster's own units where it adds
/// the allies', so what reaches the rule is the last friendly the walk saw. One
/// unshielded man alone fails `3 * 1 <= 2 * 1`; shield him and he passes; put a
/// second man beside him and the pair passes whatever their shielding, because
/// `3 <= 4`. That is the original's arithmetic, slip and all.
TEST(best_prot_pos_applies_the_two_thirds_rule_to_the_last_friendly_it_saw) {
  ShieldBench b;
  b.plant(3, 3);
  b.rebuild();
  const GaikaId node = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  const ObjectId caster = b.trooper("Legionary", 1, 2, 2);
  const ObjectId mob = b.trooper("Archer", 2, 2, 4);
  const ObjectId ours = b.trooper("Legionary", 1, 2, 5);
  b.arm();
  b.band(1, node, {caster});
  b.band(2, node, {mob});

  // One friendly, unshielded: 3 > 2, refused.
  CHECK(best_prot_pos(b, caster, 100, 0, 5000, 1) == kNoObject);

  // The same man under a cover of mercy: 0 <= 2, and the crowd is answered by
  // its front member.
  b.shield(ours, true);
  CHECK(best_prot_pos(b, caster, 100, 0, 5000, 1) == mob);

  // Two friendlies, the second unshielded: 3 <= 4, so the pair passes where
  // one of them alone did not.
  b.shield(ours, false);
  b.crowd("Legionary", 1, 3, 4, 1);
  CHECK(best_prot_pos(b, caster, 100, 0, 5000, 1) == mob);
}

/// Neither side may be less than a tenth of the whole, which is how a rout is
/// told from a battle.
TEST(best_prot_pos_refuses_a_rout_on_either_side) {
  ShieldBench b;
  b.plant(3, 3);
  b.rebuild();
  const GaikaId node = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  const ObjectId caster = b.trooper("Legionary", 1, 2, 2);
  const ObjectId mob = b.trooper("Archer", 2, 2, 4);
  // Nine more enemies on the same cell, and one shielded friendly beside them,
  // so the two-thirds rule is out of the way.
  b.crowd("Archer", 2, 2, 4, 9);
  b.crowd("Legionary", 1, 2, 5, 1, /*shielded=*/true);
  b.arm();
  b.band(1, node, {caster});
  b.band(2, node, {mob});

  // Ten against one: the tenth of eleven is one, and one friendly clears it.
  CHECK(best_prot_pos(b, caster, 100, 0, 5000, 1) == mob);

  // Twenty against one: the tenth of twenty-one is two, and the friendly side
  // is below it.
  b.crowd("Archer", 2, 2, 4, 10);
  CHECK(best_prot_pos(b, caster, 100, 0, 5000, 1) == kNoObject);

  // And the same rule from the other end: one enemy against twenty friends.
  ShieldBench t;
  t.plant(3, 3);
  t.rebuild();
  const GaikaId here = t.world.gaika().at(t.world.lsa(), in_cell(2, 2));
  const ObjectId who = t.trooper("Legionary", 1, 2, 2);
  const ObjectId lone = t.trooper("Archer", 2, 2, 4);
  t.crowd("Legionary", 1, 2, 5, 10, /*shielded=*/true);
  t.arm();
  t.band(1, here, {who});
  t.band(2, here, {lone});
  // Eleven in all, a tenth of one, and the single enemy clears it.
  CHECK(best_prot_pos(t, who, 100, 0, 5000, 1) == lone);
  t.crowd("Legionary", 1, 3, 4, 10, /*shielded=*/true);
  CHECK(best_prot_pos(t, who, 100, 0, 5000, 1) == kNoObject);
}

/// The score is the size of the engagement, `enemies * friends`, floored by
/// `MinEval`; the ranking adds `MaxRange - distance`, and the winner still has
/// to clear the literal 3.
TEST(best_prot_pos_scores_the_size_of_the_engagement) {
  ShieldBench b;
  b.plant(3, 3);
  b.rebuild();
  const GaikaId node = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  const ObjectId caster = b.trooper("Legionary", 1, 2, 2);
  const ObjectId mob = b.trooper("Archer", 2, 2, 4);
  b.crowd("Archer", 2, 2, 4, 1);
  b.crowd("Legionary", 1, 2, 5, 2);
  b.arm();
  b.band(1, node, {caster});
  b.band(2, node, {mob});

  // Two against two: the score is four.
  CHECK(best_prot_pos(b, caster, 100, 0, 5000, 4) == mob);
  CHECK(best_prot_pos(b, caster, 100, 0, 5000, 5) == kNoObject);

  // The distance band is the shared walk's, and it is inclusive at both ends:
  // the crowd is two cells away.
  CHECK(best_prot_pos(b, caster, 100, 128, 128, 4) == mob);
  CHECK(best_prot_pos(b, caster, 100, 129, 5000, 4) == kNoObject);
  CHECK(best_prot_pos(b, caster, 100, 0, 127, 4) == kNoObject);

  // And the literal 3, tested on the ranked value. The smallest engagement the
  // three tests allow is one against one -- the friendly shielded, or the
  // two-thirds rule refuses it -- and its score is 1, so with `MaxRange` equal
  // to the distance there is no bonus and 1 is refused. Three units of bonus
  // carry the same crowd over.
  ShieldBench t;
  t.plant(3, 3);
  t.rebuild();
  const GaikaId here = t.world.gaika().at(t.world.lsa(), in_cell(2, 2));
  const ObjectId who = t.trooper("Legionary", 1, 2, 2);
  const ObjectId lone = t.trooper("Archer", 2, 2, 4);
  t.crowd("Legionary", 1, 2, 5, 1, /*shielded=*/true);
  t.arm();
  t.band(1, here, {who});
  t.band(2, here, {lone});
  CHECK(best_prot_pos(t, who, 100, 0, 128, 0) == kNoObject);
  CHECK(best_prot_pos(t, who, 100, 0, 130, 0) == kNoObject);
  CHECK(best_prot_pos(t, who, 100, 0, 131, 0) == lone);
}

/// A crowd is preferred to a sacrifice, and a sacrifice only answers when no
/// crowd cleared the bar. A sacrifice belonging to somebody else is not one.
TEST(best_prot_pos_keeps_the_sacrifice_as_a_fallback_and_only_the_casters_own) {
  ShieldBench b;
  b.plant(3, 3);
  b.rebuild();
  const GaikaId node = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  const ObjectId caster = b.trooper("Legionary", 1, 3, 3);

  // Two crowds, one on either side of the caster. The near one stands beside
  // the caster's own ritual; the far one does not.
  const ObjectId near_mob = b.trooper("Archer", 2, 3, 4);
  b.crowd("Archer", 2, 3, 4, 1);
  b.crowd("Legionary", 1, 3, 5, 2, /*shielded=*/true);
  const ObjectId far_mob = b.trooper("Archer", 2, 1, 1);
  b.crowd("Legionary", 1, 1, 2, 1, /*shielded=*/true);
  b.arm();
  const ObjectId altar = b.ritual(1, 3, 4);
  b.band(1, node, {caster});
  b.band(2, node, {near_mob});
  b.band(2, node, {far_mob});

  // The near crowd scores 2 * 2 = 4 and the far one 1 * 1 = 1, and the near one
  // is nearer -- but it is the one holding the ritual, so the *far* crowd wins
  // the crowd pair and is answered.
  CHECK(best_prot_pos(b, caster, 100, 0, 5000, 1) == far_mob);

  // Take the far crowd's friend away and no crowd qualifies at all, and the
  // ritual answers in its place.
  CHECK(b.world.set_health(far_mob, 0));
  CHECK(b.heroes.squads().leave(b.heroes.squads().squad_of(far_mob), far_mob));
  CHECK(best_prot_pos(b, caster, 100, 0, 5000, 1) == altar);

  // A ritual belonging to somebody else is not the caster's fallback, so the
  // near crowd is recorded against itself again.
  CHECK(b.world.set_owner(altar, 2));
  CHECK(best_prot_pos(b, caster, 100, 0, 5000, 1) == near_mob);
}

/// Allies count on the friendly side and **add** to the shield tally where the
/// caster's own men assign to it; the circle takes its own edge; and a
/// garrisoned unit is off the grid and so out of the census.
///
/// Three allies, two of them unshielded, put the shield tally at exactly two
/// thirds of the friendly side -- the boundary the rule allows -- which the
/// caster's own men could never produce on their own.
TEST(best_prot_pos_counts_allies_and_the_edge_and_not_the_garrison) {
  ShieldBench b;
  b.plant(3, 3);
  b.rebuild();
  const GaikaId node = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  const ObjectId caster = b.trooper("Legionary", 1, 2, 2);
  const ObjectId mob = b.trooper("Archer", 2, 2, 4);
  // Exactly one cell away, which is exactly the radius asked for below.
  const ObjectId edge = b.trooper("Archer", 2, 3, 4);
  b.world.players().set_relation_word(1, 3, kRelationFriendly);
  b.crowd("Legionary", 3, 2, 5, 1, /*shielded=*/true);
  b.crowd("Legionary", 3, 2, 5, 2);
  b.arm();
  b.band(1, node, {caster});
  b.band(2, node, {mob});
  CHECK(edge != kNoObject);

  // Two enemies against three allies: the score is six, and the enemy standing
  // exactly on the radius is needed to make it so.
  CHECK(best_prot_pos(b, caster, 64, 0, 5000, 6) == mob);
  CHECK(best_prot_pos(b, caster, 64, 0, 5000, 7) == kNoObject);

  // And the two-thirds rule at its boundary: two unshielded out of three is
  // allowed, three out of three is not.
  ShieldBench t;
  t.plant(3, 3);
  t.rebuild();
  const GaikaId here = t.world.gaika().at(t.world.lsa(), in_cell(2, 2));
  const ObjectId who = t.trooper("Legionary", 1, 2, 2);
  const ObjectId lone = t.trooper("Archer", 2, 2, 4);
  t.world.players().set_relation_word(1, 3, kRelationFriendly);
  t.crowd("Legionary", 3, 2, 5, 3);
  t.arm();
  t.band(1, here, {who});
  t.band(2, here, {lone});
  CHECK(best_prot_pos(t, who, 100, 0, 5000, 1) == kNoObject);

  // And the garrison. A held unit's position is the held sentinel just outside
  // the map's corner, so a crowd standing in the first cell has it well inside
  // its circle -- and it is still not censused, because a garrisoned unit is
  // off the object grid.
  ShieldBench g;
  g.plant(3, 3);
  g.rebuild();
  const ObjectId asking = g.trooper("Legionary", 1, 2, 2);
  const ObjectId corner = g.trooper("Archer", 2, 0, 0);
  g.crowd("Archer", 2, 0, 0, 1);
  g.crowd("Legionary", 1, 0, 0, 1, /*shielded=*/true);
  const ObjectId keep = g.fort(1);
  CHECK(g.world.set_position(keep, in_cell(0, 0)));
  const ObjectId garrisoned = g.trooper("Legionary", 1, 0, 0);
  g.arm();
  REQUIRE(g.world.put_in_holder(garrisoned, keep));
  REQUIRE(g.world.resolve_position(garrisoned) == in_cell(0, 0));
  g.band(1, g.world.gaika().at(g.world.lsa(), in_cell(2, 2)), {asking});
  g.band(2, g.world.gaika().at(g.world.lsa(), in_cell(2, 2)), {corner});
  // Two enemies against the one friendly the sweep can see.
  CHECK(best_prot_pos(g, asking, 100, 0, 5000, 2) == corner);
  CHECK(best_prot_pos(g, asking, 100, 0, 5000, 3) == kNoObject);
}

/// A tie keeps the first offered, and what is answered is the winning squad's
/// **front** member rather than any other one.
TEST(best_prot_pos_breaks_a_tie_by_squad_order_and_answers_the_front_member) {
  ShieldBench b;
  b.plant(3, 3);
  b.rebuild();
  const GaikaId node = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  // The caster sits between the two crowds and is the only friendly either of
  // them can see, so both circles score alike.
  const ObjectId caster = b.trooper("Legionary", 1, 3, 3);
  const ObjectId left_front = b.trooper("Archer", 2, 2, 3);
  const ObjectId left_back = b.trooper("Archer", 2, 1, 3);
  const ObjectId right_front = b.trooper("Archer", 2, 4, 3);
  const ObjectId right_back = b.trooper("Archer", 2, 5, 3);
  b.arm();
  b.shield(caster, true);
  b.band(1, node, {caster});
  // Offered right first, and the ids ascend left first, so neither "lowest id"
  // nor "last offered" can be mistaken for the rule.
  REQUIRE(left_front < right_front);
  b.band(2, node, {right_front, right_back});
  b.band(2, node, {left_front, left_back});

  CHECK(best_prot_pos(b, caster, 64, 0, 5000, 1) == right_front);
  CHECK(left_back != kNoObject);
  CHECK(right_back != kNoObject);
}

/// The sacrifice has to clear the same literal 3 the crowd does.
TEST(best_prot_pos_holds_the_sacrifice_to_the_same_bar) {
  ShieldBench b;
  b.plant(3, 3);
  b.rebuild();
  const GaikaId node = b.world.gaika().at(b.world.lsa(), in_cell(2, 2));
  const ObjectId caster = b.trooper("Legionary", 1, 2, 2);
  const ObjectId lone = b.trooper("Archer", 2, 2, 4);
  b.crowd("Legionary", 1, 2, 5, 1, /*shielded=*/true);
  b.arm();
  const ObjectId altar = b.ritual(1, 2, 4);
  b.band(1, node, {caster});
  b.band(2, node, {lone});

  // One against one is a score of 1, and with no distance bonus the sacrifice
  // is refused exactly as a crowd would be.
  CHECK(best_prot_pos(b, caster, 100, 0, 128, 0) == kNoObject);
  CHECK(best_prot_pos(b, caster, 100, 0, 131, 0) == altar);
}

// --------------------------------------------------------------------------
// which nodes are next door
// --------------------------------------------------------------------------

namespace {

/// One seven-cell island in open water, which is the smallest island whose
/// interior survives its own walled shore -- `kBigIsland`'s own comment says
/// why -- and the only picture here that has a **sea to be in**.
constexpr std::initializer_list<std::string_view> kIslandAndSea = {
    "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~.......~~~~~~",
    "~~~.......~~~~~~", "~~~.......~~~~~~", "~~~.......~~~~~~", "~~~.......~~~~~~",
    "~~~.......~~~~~~", "~~~.......~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
    "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
};

/// `pass_blocking_water` for a picture that need not be square, because the
/// one thing a square slot grid can never show is whether its rows and its
/// columns have been kept apart.
std::vector<std::byte> pass_blocking_water_for(std::initializer_list<std::string_view> rows) {
  const auto height = static_cast<std::uint32_t>(rows.size()) * 4;
  const auto width = static_cast<std::uint32_t>(rows.begin()->size()) * 4;
  imperivm::test::Builder out;
  out.text(kGridMagic).u32(16).u32(1).u32(16 * width).u32(16 * height);
  const std::uint32_t stride = (width + 7) / 8;
  std::vector<std::string_view> picture(rows);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t byte = 0; byte < stride; ++byte) {
      std::uint32_t value = 0;
      for (std::uint32_t bit = 0; bit < 8; ++bit) {
        const std::uint32_t x = byte * 8 + bit;
        if (x >= width) continue;
        if (picture[y / 4][x / 4] == '~') value |= 1u << bit;
      }
      out.u8(value);
    }
  }
  return {out.span().begin(), out.span().end()};
}

/// `NodeBench`'s world over a picture of the caller's choosing, and with the
/// choice of passability layer the two questions here need: a walled shore,
/// which leaves the open water passable and so gives the sea a node of its
/// own, or blocked water, which gives two landmasses and no sea at all.
struct LinkBench {
  ClassGraph graph;
  std::vector<std::byte> terrain;
  std::vector<std::byte> pass;
  World world;
  EconomySystem economy;

  /// `shore` picks the passability layer; `rect` builds the blocked-water one
  /// from the picture's own extent instead of assuming a square map.
  LinkBench(std::initializer_list<std::string_view> picture, bool shore, bool rect = false)
      : terrain(terrain_from(picture)),
        pass(rect ? pass_blocking_water_for(picture)
                  : (shore ? pass_blocking_shore(picture) : pass_blocking_water(picture))) {
    graph.add(bytes_of(
                  R"(<class id="Townhall" cpp_class="CVXTownHall"><properties maxhealth="1000"/></class>)"),
              "townhall.sc.xml");
    graph.link();
    world.set_class_graph(&graph);
    const Result<Grid> layer = Grid::parse(terrain);
    REQUIRE(layer.ok());
    world.set_terrain(layer.value());
    REQUIRE(world.add_system(&economy));
    economy.start(world);
    world.mutable_lsa().build(world.terrain(), obstruction(pass));
  }

  SettlementId plant(std::int32_t cx, std::int32_t cy, PlayerId owner = 1) {
    const ObjectId anchor = world.spawn(NativeClass::town_hall, nullptr, graph.find("Townhall"));
    CHECK(world.set_position(anchor, in_cell(cx, cy)));
    CHECK(world.set_owner(anchor, owner));
    SettlementInit init;
    init.kind = SettlementKind::stronghold;
    init.anchor = anchor;
    init.owner = owner;
    return economy.create(world, init);
  }

  void rebuild() { world.mutable_gaika().build(world, world.lsa(), economy.settlements()); }

  [[nodiscard]] bool touching(GaikaId a, GaikaId b) const {
    for (const GaikaId id : world.gaika().neighbours(a)) {
      if (id == b) return true;
    }
    return false;
  }

  /// Every claim that holds of any adjacency, asserted over the whole table:
  /// symmetric, ascending, no duplicates, and nothing is its own neighbour.
  void check_shape() const {
    const GaikaTable& table = world.gaika();
    for (GaikaId a = 1; a <= table.count(); ++a) {
      GaikaId previous = kNoGaika;
      for (const GaikaId b : table.neighbours(a)) {
        CHECK(b != a);
        CHECK(b > previous);
        previous = b;
        CHECK(touching(b, a));
      }
    }
    // And an id that names no node has no neighbours.
    CHECK(table.neighbours(kNoGaika).empty());
    CHECK(table.neighbours(table.count() + 1).empty());
  }
};

}  // namespace

/// **A land node and a sea node that touch are neighbours.** The node graph
/// has no type test where the area graph one level down has one, so the water
/// around an island is next door to it -- and to a second settlement on the
/// same island, which is next door to the first.
TEST(gaika_neighbours_pair_a_land_node_with_the_water_around_it) {
  LinkBench b(kIslandAndSea, /*shore=*/true);
  // Both inside the walled shore, which leaves the five-cell block at 4..8.
  b.plant(4, 4);
  b.plant(8, 8);
  b.rebuild();
  // Two settlements, and the open water nothing was built in.
  REQUIRE(b.world.gaika().count() == 3);
  b.check_shape();

  const GaikaId first = b.world.gaika().at(b.world.lsa(), in_cell(4, 4));
  const GaikaId second = b.world.gaika().at(b.world.lsa(), in_cell(8, 8));
  const GaikaId sea = b.world.gaika().at(b.world.lsa(), Point{16, 16});
  REQUIRE(first != second);
  REQUIRE(sea != first);
  REQUIRE(sea != second);
  CHECK(b.world.lsa().water(b.world.lsa().at(Point{16, 16})));

  CHECK(b.touching(first, second));
  CHECK(b.touching(first, sea));
  CHECK(b.touching(second, sea));
}

/// Water wider than a slot separates two islands, and neither is the other's
/// neighbour.
TEST(gaika_neighbours_stop_at_water_wider_than_a_slot) {
  LinkBench b(kTwoIslands, /*shore=*/false);
  b.plant(3, 3);
  b.plant(11, 11);
  b.rebuild();
  // Blocked water is nobody's area, so there are two nodes and no sea.
  REQUIRE(b.world.gaika().count() == 2);
  b.check_shape();
  CHECK(!b.touching(1, 2));
  CHECK(b.world.gaika().neighbours(1).empty());
  CHECK(b.world.gaika().neighbours(2).empty());
}

/// **The slot grid cannot see water narrower than a slot.** One terrain cell of
/// sea is 64 units and a slot is 128, so no slot centre lands in the channel
/// and the two islands come out as neighbours -- a property of the original's
/// resolution as much as of this one, because it is the same grid at the same
/// 128 units.
TEST(gaika_neighbours_cannot_see_a_channel_narrower_than_a_slot) {
  LinkBench b(kNodeIslands, /*shore=*/false);
  b.plant(3, 3);
  b.plant(9, 9);
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 2);
  b.check_shape();
  CHECK(b.touching(1, 2));
}

namespace {

/// `g.ControlledNeighbors(player)`, through the registry.
std::int32_t controlled_neighbors(LinkBench& b, GaikaId node, std::int32_t player) {
  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index =
      registry.find(script::CallKind::member, "ControlledNeighbors", 1);
  CHECK(index != script::kUnresolvedHost);
  if (index == script::kUnresolvedHost) return -1;
  std::vector<script::Value> args{gaika_value(node), script::Value::integer(player)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "ControlledNeighbors";
  ctx.kind = script::CallKind::member;
  const script::HostOutcome out = registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  return out.value.is_integer() ? out.value.as_integer() : -1;
}

}  // namespace

/// How many of the places next door this player holds -- which is what a
/// defensive AI asks before deciding it is surrounded.
///
/// The island carries two settlements of different players and the water
/// around them carries none, so each of the three counts one thing and misses
/// two: a node is never its own neighbour, and a region node has no settlement
/// to have an owner.
///
/// **The argument is a script player number, one-based**, and the settlements
/// here belong to player *indices* 1 and 2 -- so they answer to 2 and 3, and
/// asking for 1 finds neither. That is the whole of the original's `dec`.
TEST(controlled_neighbors_counts_the_settlements_next_door) {
  LinkBench b(kIslandAndSea, /*shore=*/true);
  b.plant(4, 4, /*owner=*/1);
  b.plant(8, 8, /*owner=*/2);
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 3);
  const GaikaId first = b.world.gaika().at(b.world.lsa(), in_cell(4, 4));
  const GaikaId second = b.world.gaika().at(b.world.lsa(), in_cell(8, 8));
  const GaikaId sea = b.world.gaika().at(b.world.lsa(), Point{16, 16});
  REQUIRE(b.touching(first, second));
  REQUIRE(b.touching(first, sea));

  CHECK(controlled_neighbors(b, first, 3) == 1);
  // ...and the first settlement is not next door to itself.
  CHECK(controlled_neighbors(b, first, 2) == 0);
  CHECK(controlled_neighbors(b, second, 2) == 1);
  CHECK(controlled_neighbors(b, second, 3) == 0);
  // The water is next door to both, and holds neither.
  CHECK(controlled_neighbors(b, sea, 2) == 1);
  CHECK(controlled_neighbors(b, sea, 3) == 1);
  CHECK(controlled_neighbors(b, sea, 4) == 0);
  // And player number 1 is player index 0, who holds nothing -- which is what
  // a body that forgot to subtract the one would get wrong.
  CHECK(controlled_neighbors(b, sea, 1) == 0);

  // A player number outside 1..16 matches no owner and answers zero without a
  // trap, which is what the raw `dec` and compare do -- and an *unowned*
  // settlement is not the one it matches, which is the only way to tell the
  // refusal from a comparison against "no player".
  const SettlementId nobodys = b.plant(6, 6, kNoPlayer);
  b.rebuild();
  CHECK(nobodys != kNoSettlement);
  REQUIRE(b.world.gaika().count() == 4);
  const GaikaId water = b.world.gaika().at(b.world.lsa(), Point{16, 16});
  CHECK(controlled_neighbors(b, water, 0) == 0);
  CHECK(controlled_neighbors(b, water, 17) == 0);
  // And so does a GAIKA that names no node.
  CHECK(controlled_neighbors(b, kNoGaika, 2) == 0);
  CHECK(controlled_neighbors(b, b.world.gaika().count() + 1, 2) == 0);
  // The unowned settlement is still next door and still not counted for
  // anybody, so the walk did reach it.
  CHECK(controlled_neighbors(b, water, 2) == 1);
}

/// Two settlements of the same player, and the water between them counts them
/// both.
TEST(controlled_neighbors_counts_every_neighbour_that_qualifies) {
  LinkBench b(kIslandAndSea, /*shore=*/true);
  b.plant(4, 4, /*owner=*/1);
  b.plant(8, 8, /*owner=*/1);
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 3);
  const GaikaId first = b.world.gaika().at(b.world.lsa(), in_cell(4, 4));
  const GaikaId sea = b.world.gaika().at(b.world.lsa(), Point{16, 16});

  CHECK(controlled_neighbors(b, first, 2) == 1);
  CHECK(controlled_neighbors(b, sea, 2) == 2);
}

/// **Corner to corner counts.** The offset table is the eight neighbours, so
/// two landmasses whose slots meet only on a diagonal are next door to each
/// other -- which a four-neighboured walk would miss.
TEST(gaika_neighbours_take_the_diagonal) {
  // Two five-cell islands, one at the north-west and one south-east of it, so
  // that the slots they own meet at exactly one corner and nowhere else.
  static constexpr std::initializer_list<std::string_view> kStaircase = {
      "~~~~~~~~~~~~~~~~", "~.....~~~~~~~~~~", "~.....~~~~~~~~~~", "~.....~~~~~~~~~~",
      "~.....~~~~~~~~~~", "~.....~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~.....~~~~",
      "~~~~~~~.....~~~~", "~~~~~~~.....~~~~", "~~~~~~~.....~~~~", "~~~~~~~.....~~~~",
      "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
  };
  LinkBench b(kStaircase, /*shore=*/false);
  b.plant(3, 3);
  b.plant(9, 9);
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 2);
  b.check_shape();
  CHECK(b.touching(1, 2));
}

/// The slot grid's rows and its columns are not the same number, and a walk
/// that confused them would read the wrong slot -- which no square map can
/// show. Sixteen cells wide by eight tall is eight slot columns by four rows.
TEST(gaika_neighbours_keep_the_slot_grids_rows_and_columns_apart) {
  static constexpr std::initializer_list<std::string_view> kWide = {
      "~~~~~~~~~~~~~~~~", "~.....~.....~~~~", "~.....~.....~~~~", "~.....~.....~~~~",
      "~.....~.....~~~~", "~.....~.....~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
  };
  LinkBench b(kWide, /*shore=*/false, /*rect=*/true);
  REQUIRE(b.world.lsa().slot_columns() == 8);
  REQUIRE(b.world.lsa().slot_rows() == 4);
  b.plant(3, 3);
  b.plant(9, 3);
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 2);
  b.check_shape();
  // The two are neighbours over the one-cell channel, which is a *positive*
  // claim on purpose: a walk that read the wrong slot would find no pair.
  CHECK(b.touching(1, 2));
}

// --------------------------------------------------------------------------
// whether there is a way in
// --------------------------------------------------------------------------

namespace {

/// `LinkBench` with the AI's per-player node view on it, and teleports.
struct ExploreBench : LinkBench {
  AiSystem ai;
  ClassIndex teleport_class = kNoClass;

  ExploreBench(std::initializer_list<std::string_view> picture, bool shore)
      : LinkBench(picture, shore) {
    graph.add(bytes_of(R"(<class id="Teleport" cpp_class="CVXTeleport"/>)"), "teleport.sc.xml");
    graph.link();
    teleport_class = graph.find("Teleport");
    REQUIRE(world.add_system(&ai));
  }

  /// Give `player` a view over the table as it stands, which is what
  /// `AIStart` does before any of this is asked.
  void see(PlayerId player) { ai.seed_gaika_view(player, world.gaika().count()); }

  void mark_explored(PlayerId player, GaikaId node, bool on) {
    GaikaView* view = ai.gaika_view(player);
    REQUIRE(view != nullptr);
    Laika* record = view == nullptr ? nullptr : view->find(node);
    REQUIRE(record != nullptr);
    if (record == nullptr) return;
    if (on) {
      record->flags = static_cast<std::uint16_t>(record->flags | kLaikaExplored);
    } else {
      record->flags = static_cast<std::uint16_t>(record->flags & ~kLaikaExplored);
    }
  }

  /// A teleport standing on a cell, as its own settlement -- which is how this
  /// engine keeps one, and the vector the original walks.
  ObjectId teleport(std::int32_t cx, std::int32_t cy) {
    const ObjectId object = world.spawn(NativeClass::teleport, nullptr, teleport_class);
    CHECK(world.set_position(object, in_cell(cx, cy)));
    SettlementInit init;
    init.kind = SettlementKind::other;
    init.anchor = object;
    init.settlement_object = object;
    init.owner = 1;
    (void)economy.create(world, init);
    return object;
  }

  void pair(ObjectId a, ObjectId b) {
    world.mutable_state(a)->teleport_destination = b;
    world.mutable_state(b)->teleport_destination = a;
  }

  [[nodiscard]] bool can_explore(GaikaId node, std::int32_t player) {
    script::HostRegistry registry;
    (void)register_all_hosts(registry);
    HostContext context;
    context.world = &world;
    const std::uint32_t index = registry.find(script::CallKind::member, "CanExplore", 1);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return false;
    std::vector<script::Value> args{gaika_value(node), script::Value::integer(player)};
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = "CanExplore";
    ctx.kind = script::CallKind::member;
    const script::HostOutcome out = registry.entry(index).fn(ctx);
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() && out.value.as_integer() != 0;
  }
};

}  // namespace

/// The first of the two grounds: **a neighbour is explored**.
///
/// Two settlements on one island are neighbours and both are next door to the
/// water, so marking any one of the three lights up the other two -- and
/// nothing lights up a node whose neighbours are all still dark.
TEST(can_explore_says_yes_when_a_neighbour_has_been_explored) {
  ExploreBench b(kIslandAndSea, /*shore=*/true);
  b.plant(4, 4);
  b.plant(8, 8);
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 3);
  b.see(0);
  const GaikaId first = b.world.gaika().at(b.world.lsa(), in_cell(4, 4));
  const GaikaId second = b.world.gaika().at(b.world.lsa(), in_cell(8, 8));
  const GaikaId sea = b.world.gaika().at(b.world.lsa(), Point{16, 16});
  REQUIRE(b.touching(first, second));

  // Nothing is explored, which is the state every shipped map is in: this
  // engine runs no visibility sweep, so the answer is a uniform no.
  CHECK(!b.can_explore(first, 1));
  CHECK(!b.can_explore(second, 1));
  CHECK(!b.can_explore(sea, 1));

  b.mark_explored(0, second, true);
  CHECK(b.can_explore(first, 1));
  CHECK(b.can_explore(sea, 1));
  // ...and a node is judged by its *neighbours* and never by itself, so the
  // one that was explored still answers no while both of its own are dark.
  CHECK(!b.can_explore(second, 1));
  b.mark_explored(0, first, true);
  CHECK(b.can_explore(second, 1));
  b.mark_explored(0, first, false);

  // Another player has its own view and has explored nothing.
  b.see(2);
  CHECK(!b.can_explore(first, 3));

  // A player number outside the window, and a GAIKA that names no node.
  CHECK(!b.can_explore(first, 0));
  CHECK(!b.can_explore(first, 17));
  CHECK(!b.can_explore(kNoGaika, 1));
  CHECK(!b.can_explore(b.world.gaika().count() + 1, 1));
}

/// The second ground, on its own: **a teleport standing here comes out
/// somewhere explored**.
///
/// The two islands are further apart than a slot, so neither is the other's
/// neighbour and the first ground can say nothing at all. Only the teleport
/// can, and it is the far end's node that decides.
TEST(can_explore_says_yes_through_a_teleport_that_lands_somewhere_explored) {
  ExploreBench b(kTwoIslands, /*shore=*/false);
  b.plant(3, 3);
  b.plant(11, 11);
  const ObjectId mouth = b.teleport(2, 2);
  const ObjectId exit = b.teleport(12, 12);
  b.rebuild();
  b.see(0);
  const GaikaId west = b.world.gaika().at(b.world.lsa(), in_cell(3, 3));
  const GaikaId east = b.world.gaika().at(b.world.lsa(), in_cell(11, 11));
  // The node the far end of the pair actually stands in, which is what the
  // routine asks about and which is not the settlement's node beside it.
  const GaikaId landing = b.world.gaika().at(b.world.lsa(), b.world.resolve_position(exit));
  // A teleport is a settlement here, so it is a node centre and the node it
  // stands in is its own. That is the partition showing through, and it is
  // narrower than the original's region: there a teleport shares the node of
  // whatever region it sits in.
  const GaikaId doorway = b.world.gaika().at(b.world.lsa(), b.world.resolve_position(mouth));
  REQUIRE(west != east);
  // Nothing on the western island is next door to anything on the eastern one,
  // so the first of the two grounds can say nothing here at all.
  CHECK(!b.touching(west, east));
  CHECK(!b.touching(doorway, landing));

  // The far side is explored, and with no pair between them that is nothing
  // to the near one.
  b.mark_explored(0, landing, true);
  CHECK(!b.can_explore(doorway, 1));

  // Paired, it is everything.
  b.pair(mouth, exit);
  CHECK(b.can_explore(doorway, 1));

  // And it is the *far* end's node that is asked: darken it and the answer
  // goes back to no, even though the near end is standing right here.
  b.mark_explored(0, landing, false);
  CHECK(!b.can_explore(doorway, 1));
  b.mark_explored(0, doorway, true);
  CHECK(!b.can_explore(doorway, 1));
}

// --------------------------------------------------------------------------
// the two that finished `GS_SIEGE.VS`
// --------------------------------------------------------------------------

namespace {

/// `ArmyBench` with a command system and the two verbs an invading squad is
/// given, plus a gate class to be very broken.
///
/// The two islands are doing the work here: `LsaPartition` puts them in
/// different areas, and "inside the walls" is exactly "in the same area as the
/// central building". A besieger stands on the other island.
/// A teleport with two doors, so that "the door nearest the *leader*" is a
/// claim a case can fail. The offsets are screen pixels, which is what
/// `<point x= y=>` authors; `sim/entrance.hpp` owns the round trip.
constexpr std::string_view kPadEntity =
    "<entity name=\"pad\" type=\"vx/building\" variations=\"1\" pass_file=\"\" radius=\"1\">"
    "<points>"
    "<point idx=\"0\" type=\"1\" x=\"-128\" y=\"0\"/>"
    "<point idx=\"1\" type=\"1\" x=\"128\" y=\"0\"/>"
    "</points>"
    "</entity>";

struct WallBench : ArmyBench {
  CommandSystem commands;
  MatchSystem match;
  Result<Entity> pad_entity = Entity::parse(bytes_of(kPadEntity));
  script::HostRegistry registry;
  HostContext context;

  WallBench() {
    graph.add(bytes_of(R"(<class id="Gate" cpp_class="CVXGate" parent="">
        <properties maxhealth="1000"/>
      </class>)"),
              "gate.sc.xml");
    // A town hall under the base the entry point actually tests. `NodeBench`'s
    // `Townhall` descends from nothing, and `GAIKA::GetDestPoint` asks
    // `IsHeirOf("BaseTownhall")` before it will look for a gate at all.
    graph.add(bytes_of(R"(<class id="BaseTownhall" cpp_class="CVXTownHall" parent=""/>)"),
              "basetownhall.sc.xml");
    graph.add(bytes_of(R"(<class id="RTownhall" cpp_class="CVXTownHall" parent="BaseTownhall">
        <properties maxhealth="5000"/>
      </class>)"),
              "rtownhall.sc.xml");
    // `maxhealth` is declared so that a unit can be made to *pass* the
    // brokenness test: without it `IsVeryBroken` reads `1 < 0` and refuses for
    // the wrong reason, and the `kSyncBuilding` guard beside it never bites.
    graph.add(bytes_of(R"(<class id="Invader" cpp_class="CVXUnit" parent="Military">
        <properties maxhealth="80"/>
        <method sig="advance"  vs="data/subai/unit_advance.vs"/>
        <method sig="capture"  vs="data/subai/unit_capture.vs"/>
        <method sig="move"     vs="data/subai/unit_move.vs"/>
        <method sig="sneak"    vs="data/subai/unit_sneak.vs"/>
        <method sig="teleport" vs="data/subai/unit_teleport.vs"/>
      </class>)"),
              "invader.sc.xml");
    graph.link();
    REQUIRE(world.add_system(&commands));
    // `GAIKA::GetDestPoint` asks the match for the map rectangle before it will
    // send a squad outside a gate.
    REQUIRE(world.add_system(&match));
    (void)register_all_hosts(registry);
    (void)register_squad_host(registry);
    context.world = &world;
  }

  /// A settlement whose central building is a real `BaseTownhall` heir.
  SettlementId plant_town(std::int32_t cx, std::int32_t cy, PlayerId owner = 1) {
    const ObjectId anchor = world.spawn(NativeClass::town_hall, nullptr, graph.find("RTownhall"));
    CHECK(world.set_position(anchor, in_cell(cx, cy)));
    SettlementInit init;
    init.kind = SettlementKind::stronghold;
    init.anchor = anchor;
    init.owner = owner;
    return economy.create(world, init);
  }

  /// A gate belonging to `id`, at full health until a case breaks it.
  ObjectId gate_of(SettlementId id, std::int32_t cx, std::int32_t cy) {
    const ObjectId g = world.spawn(NativeClass::gate, nullptr, graph.find("Gate"));
    CHECK(world.set_position(g, in_cell(cx, cy)));
    CHECK(world.set_health(g, 1000));
    CHECK(economy.add_building(world, id, g, 1000));
    return g;
  }

  ObjectId invader(std::int32_t cx, std::int32_t cy) {
    const ObjectId u = world.spawn(NativeClass::unit, nullptr, graph.find("Invader"));
    CHECK(world.set_position(u, in_cell(cx, cy)));
    CHECK(world.set_owner(u, 2));
    CHECK(world.set_health(u, 100));
    return u;
  }

  script::HostOutcome call(script::CallKind kind, const char* name, std::uint16_t arity,
                   std::vector<script::Value> args) {
    const std::uint32_t index = registry.find(kind, name, arity);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    script::CallContext ctx;
    ctx.arguments = args;
    ctx.user = &context;
    ctx.name = name;
    ctx.kind = kind;
    return registry.entry(index).fn(ctx);
  }
};

}  // namespace

/// `WallBench` with the movement system `Gate::Inside`'s predicate routes on.
struct InsideBench : WallBench {
  MovementSystem movement;
  InsideBench() {
    movement.set_class_graph(&graph);
    movement.set_grid(obstruction(pass));
    REQUIRE(world.add_system(&movement));
  }
};

/// `Gate::Inside(squad)` -- **every** member has to be inside, and a member in
/// a holder is not.
///
/// The predicate, `inside_walls`, is the original's -- a member is inside when
/// its route to the town centre, every gate open, crosses no gate -- and
/// `test_gate.cpp` holds it to a wall and a gate. Here the walk: these gates
/// have no axis, so no route crosses one and a member in the streets is
/// inside; a member in a holder is the one that is not.
TEST(gate_inside_wants_every_member_of_the_squad_inside) {
  InsideBench b;
  const SettlementId town = b.plant(3, 3);
  const Settlement* set = b.economy.settlements().find(town);
  REQUIRE(set != nullptr);
  const ObjectId gate = b.gate_of(town, 4, 4);

  const auto inside = [&](ObjectId at, SquadKey key) {
    return b.call(script::CallKind::member, "Inside", 1,
                  {script::Value::object(kTypeObj, at), pack_squad(key)})
        .value.truthy_scalar();
  };

  const ObjectId within = b.invader(2, 2);    // western island, with the town
  const ObjectId garrison = b.invader(2, 2);
  CHECK(inside(gate, b.band(2, 1, {within})));
  CHECK(inside(gate, b.band(2, 1, {garrison})));

  // A member **in a holder** is not inside: it is in a building, not standing
  // in the streets. `[unit+0x154] != 0xffff` is the original's own first test.
  CHECK(b.world.put_in_holder(garrison, set->anchor));
  CHECK(!inside(gate, b.band(2, 1, {garrison})));
  // **Every member**, so one decides for the whole squad.
  CHECK(!inside(gate, b.band(2, 1, {within, garrison})));
  // ...and an empty squad is inside, because every member of nothing is.
  CHECK(inside(gate, b.band(2, 1, {})));

  // A member with no route to the centre at all -- the eastern island, across
  // water -- lists no crossing, and the original reads an empty list as
  // inside.
  const ObjectId beyond = b.invader(10, 10);
  CHECK(inside(gate, b.band(2, 1, {beyond})));

  // A gate that belongs to no settlement answers false rather than guessing.
  const ObjectId stray = b.world.spawn(NativeClass::gate, nullptr, b.graph.find("Gate"));
  CHECK(b.world.set_position(stray, in_cell(2, 2)));
  CHECK(!inside(stray, b.band(2, 1, {within})));

  // A handle naming no squad walks no members, which is the same answer an
  // empty squad gets and not the opposite one.
  CHECK(inside(gate, SquadKey{99, 9}));
}

/// `squad.InvadeThroughGate(gate, nState)` -- five guards, then two orders per
/// member, and it stops at a hero.
TEST(invade_through_gate_orders_every_member_to_advance_and_then_capture) {
  WallBench b;
  const SettlementId town = b.plant(3, 3);
  const Settlement* set = b.economy.settlements().find(town);
  REQUIRE(set != nullptr);
  const ObjectId anchor = set->anchor;
  const ObjectId gate = b.gate_of(town, 4, 4);

  const ObjectId first = b.invader(10, 10);
  const ObjectId second = b.invader(10, 11);
  const SquadKey key = b.band(2, 1, {first, second});

  const auto invade = [&](std::int32_t state) {
    return b.call(script::CallKind::member, "InvadeThroughGate", 2,
                  {pack_squad(key), script::Value::object(kTypeObj, gate),
                   script::Value::integer(state)})
        .status;
  };
  const auto verbs = [&](ObjectId id) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < b.commands.command_count(id); ++i) {
      out.emplace_back(b.commands.command_name(id, i));
    }
    return out;
  };

  // **A sound gate is not a hole.** `IsVeryBroken` is `health < maxhealth / 20`
  // and the gate is at full health, so nothing happens at all -- not even the
  // state, which the original writes only after this guard.
  CHECK(invade(9) == script::HostStatus::ok);
  CHECK(b.heroes.squads().find(key)->state == 0);
  CHECK(verbs(first).empty());

  // A twentieth exactly is still not broken enough: the comparison is strict.
  CHECK(b.world.set_health(gate, 50));
  CHECK(invade(9) == script::HostStatus::ok);
  CHECK(b.heroes.squads().find(key)->state == 0);

  // One below it is.
  CHECK(b.world.set_health(gate, 49));
  CHECK(invade(9) == script::HostStatus::ok);
  CHECK(b.heroes.squads().find(key)->state == 9);
  CHECK(b.heroes.squads().find(key)->state_time == b.world.time());
  // `advance` on the gate's own position, then `capture` on the settlement's
  // central building -- the queue replaced, not appended to.
  CHECK(verbs(first) == std::vector<std::string>({"advance", "capture"}));
  CHECK(verbs(second) == std::vector<std::string>({"advance", "capture"}));
  {
    const CommandQueue* queue = b.commands.find(first);
    REQUIRE(queue != nullptr);
    REQUIRE(queue->entries.size() == 2);
    CHECK(queue->entries[0].point == b.world.resolve_position(gate));
    CHECK(queue->entries[1].object == anchor);
  }

  // **The lock does not stay up.** The original raises `kSquadLocked` for the
  // duration of the walk and writes the pre-walk flags back on every exit.
  CHECK((b.heroes.squads().find(key)->flags & kSquadLocked) == 0);

  // **`SF_NOAI` refuses**: a squad a mission script owns is not marched
  // anywhere by the AI.
  const SquadKey theirs = b.band(2, 1, {b.invader(10, 12)});
  b.heroes.squads().find(theirs)->flags = kSquadFlagNoAi;
  CHECK(invade(9) == script::HostStatus::ok);
  {
    const ObjectId only = b.heroes.squads().find(theirs)->members.front();
    b.commands.clear_commands(b.world, only);
    CHECK(b.call(script::CallKind::member, "InvadeThroughGate", 2,
                 {pack_squad(theirs), script::Value::object(kTypeObj, gate),
                  script::Value::integer(5)})
              .status == script::HostStatus::ok);
    CHECK(b.heroes.squads().find(theirs)->state == 0);
    CHECK(verbs(only).empty());
  }

  // **It stops at a hero.** After ordering a member that carries `kSyncHero`,
  // the walk ends and whoever is behind it in the deque gets nothing.
  const ObjectId champion = b.world.spawn(NativeClass::hero, nullptr, b.graph.find("Invader"));
  CHECK(b.world.set_position(champion, in_cell(10, 10)));
  CHECK(b.world.set_owner(champion, 2));
  CHECK(b.world.set_health(champion, 100));
  const ObjectId behind = b.invader(10, 13);
  const SquadKey led = b.band(2, 1, {champion, behind});
  CHECK(b.call(script::CallKind::member, "InvadeThroughGate", 2,
               {pack_squad(led), script::Value::object(kTypeObj, gate),
                script::Value::integer(4)})
            .status == script::HostStatus::ok);
  CHECK(b.heroes.squads().find(led)->state == 4);
  CHECK(verbs(champion) == std::vector<std::string>({"advance", "capture"}));
  CHECK(verbs(behind).empty());

  // **The queue is replaced, not appended to.** A member already carrying an
  // order loses it: the original aborts through `vtbl+0xc0` before it queues
  // anything, and with an empty queue the two spellings look identical.
  {
    const ObjectId busy = b.invader(10, 15);
    Command idle;
    CHECK(b.commands.set_command(b.world, busy, "idle", idle) != 0);
    CHECK(verbs(busy) == std::vector<std::string>({"idle"}));
    const SquadKey one = b.band(2, 1, {busy});
    CHECK(b.call(script::CallKind::member, "InvadeThroughGate", 2,
                 {pack_squad(one), script::Value::object(kTypeObj, gate),
                  script::Value::integer(2)})
              .status == script::HostStatus::ok);
    CHECK(verbs(busy) == std::vector<std::string>({"advance", "capture"}));
  }

  // **A gate has to be a building.** A unit at one health would pass the
  // brokenness test on its own, and the `kSyncBuilding` guard is what refuses
  // it before anything is ordered.
  {
    const ObjectId impostor = b.invader(4, 4);
    CHECK(b.world.set_health(impostor, 1));  // `Invader` declares maxhealth 80
    REQUIRE(b.world.find(impostor) != nullptr);
    CHECK(!b.world.find(impostor)->state.flags.is_building);
    CHECK(b.economy.add_building(b.world, town, impostor, 80));
    const ObjectId victim = b.invader(10, 16);
    const SquadKey one = b.band(2, 1, {victim});
    CHECK(b.call(script::CallKind::member, "InvadeThroughGate", 2,
                 {pack_squad(one), script::Value::object(kTypeObj, impostor),
                  script::Value::integer(6)})
              .status == script::HostStatus::ok);
    CHECK(b.heroes.squads().find(one)->state == 0);
    CHECK(verbs(victim).empty());
  }

  // A member whose class binds neither verb gets neither, and the walk goes on.
  const ObjectId mute = b.soldier("Legionary", 2);
  CHECK(b.world.set_position(mute, in_cell(10, 10)));
  const ObjectId talker = b.invader(10, 14);
  const SquadKey mixed = b.band(2, 1, {mute, talker});
  CHECK(b.call(script::CallKind::member, "InvadeThroughGate", 2,
               {pack_squad(mixed), script::Value::object(kTypeObj, gate),
                script::Value::integer(3)})
            .status == script::HostStatus::ok);
  CHECK(verbs(mute).empty());
  CHECK(verbs(talker) == std::vector<std::string>({"advance", "capture"}));
}

/// `GAIKA::GetDestPoint(unit)` -- where a squad ordered to a node is actually
/// sent, which is **not** the node's centre.
///
/// An army sent at a walled town wants the ground outside its nearest gate, and
/// the unit argument is what decides which gate is nearest. `sim/ai.cpp`
/// carries the branch list and the one branch that is not read to the end.
TEST(gaika_dest_point_aims_a_squad_at_the_ground_outside_the_nearest_gate) {
  WallBench b;
  const SettlementId town = b.plant_town(3, 3);
  const Settlement* set = b.economy.settlements().find(town);
  REQUIRE(set != nullptr);
  const Point centre = b.world.resolve_position(set->anchor);
  b.rebuild();
  const GaikaId node = b.world.gaika().for_settlement(set->object);
  REQUIRE(node != kNoGaika);

  const auto ask = [&](GaikaId g, ObjectId unit) {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "GetDestPoint", 1,
               {gaika_value(g), script::Value::object(kTypeObj, unit)});
    CHECK(out.status == script::HostStatus::ok);
    return unpack_point(out.value);
  };

  const ObjectId scout = b.invader(1, 1);

  // **No gates yet: the answer is the central building**, which is the running
  // answer every branch either improves on or leaves.
  CHECK(ask(node, scout) == centre);

  // A `GAIKA` naming no node is `(0, 0)`: the accumulator is zeroed before the
  // table is asked and nothing else writes it.
  const Point origin{0, 0};
  CHECK(ask(kNoGaika, scout) == origin);
  CHECK(ask(99, scout) == origin);

  // Two gates on opposite sides of the town. The nearest one **to the unit**
  // wins, so the same node answers differently for two different squads.
  const ObjectId north = b.gate_of(town, 3, 1);
  const ObjectId south = b.gate_of(town, 3, 5);
  const Point north_at = b.world.resolve_position(north);
  const Point south_at = b.world.resolve_position(south);

  // The answer is 450 units beyond the gate, on the ray out of the town: it is
  // further from the centre than the gate is, and on the same side.
  {
    const Point out = ask(node, scout);  // the scout is north-west
    const bool moved = out.x != centre.x || out.y != centre.y;
    CHECK(moved);
    CHECK(out.y < north_at.y);   // beyond the northern gate, not short of it
    CHECK(out.x == north_at.x);  // straight out, the gate being due north
    CHECK(north_at.y - out.y == 450);
  }
  {
    const ObjectId southerner = b.invader(3, 6);
    const Point out = ask(node, southerner);
    CHECK(out.y > south_at.y);
    CHECK(out.y - south_at.y == 450);
  }

  // A unit argument that names nothing leaves the running answer alone -- it
  // does not fall back to steering from the town centre, which would pick a
  // gate rather than none. Both spellings of "nothing": a value that is not an
  // object at all, and a handle whose object is gone.
  CHECK(unpack_point(b.call(script::CallKind::member, "GetDestPoint", 1,
                            {gaika_value(node), script::Value::integer(0)})
                         .value) == centre);
  CHECK(unpack_point(b.call(script::CallKind::member, "GetDestPoint", 1,
                            {gaika_value(node), script::Value::object(kTypeObj, 999999)})
                         .value) == centre);

  // **Only gates count.** A building standing nearer the scout than either gate
  // is not a way in and must not be chosen.
  {
    const ObjectId barn = b.fort(1);
    CHECK(b.world.set_position(barn, in_cell(2, 1)));
    CHECK(b.economy.add_building(b.world, town, barn, 100));
    const Point out = ask(node, scout);
    CHECK(out.x == north_at.x);
    CHECK(north_at.y - out.y == 450);
  }

  // **A gate standing on the town centre steps nowhere**, which is the
  // original's own `len <= 0` arm: there is no direction to leave by.
  {
    WallBench c;
    const SettlementId one = c.plant_town(3, 3);
    const Settlement* s = c.economy.settlements().find(one);
    REQUIRE(s != nullptr);
    const Point middle = c.world.resolve_position(s->anchor);
    const ObjectId on_top = c.gate_of(one, 3, 3);
    (void)on_top;
    c.rebuild();
    const GaikaId n = c.world.gaika().for_settlement(s->object);
    const ObjectId who = c.invader(1, 1);
    CHECK(unpack_point(c.call(script::CallKind::member, "GetDestPoint", 1,
                              {gaika_value(n), script::Value::object(kTypeObj, who)})
                           .value) == middle);
  }

  // **A gate further than 200,000 units away is not found at all**, which is
  // the original's own initial best rather than an unbounded one.
  {
    WallBench c;
    const SettlementId one = c.plant_town(3, 3);
    const Settlement* s = c.economy.settlements().find(one);
    REQUIRE(s != nullptr);
    const Point middle = c.world.resolve_position(s->anchor);
    const ObjectId remote = c.world.spawn(NativeClass::gate, nullptr, c.graph.find("Gate"));
    CHECK(c.world.set_position(remote, Point{900000, 900000}));
    CHECK(c.economy.add_building(c.world, one, remote, 1000));
    c.rebuild();
    const GaikaId n = c.world.gaika().for_settlement(s->object);
    const ObjectId who = c.invader(1, 1);
    CHECK(unpack_point(c.call(script::CallKind::member, "GetDestPoint", 1,
                              {gaika_value(n), script::Value::object(kTypeObj, who)})
                           .value) == middle);
  }

  // **The class test is `BaseTownhall` and it is not decoration.** A settlement
  // whose central building descends from nothing has gates and is still
  // answered with its centre: the search never runs.
  {
    WallBench c;
    const SettlementId plain = c.plant(3, 3);  // `NodeBench`'s `Townhall`, no base
    const Settlement* s = c.economy.settlements().find(plain);
    REQUIRE(s != nullptr);
    const Point middle = c.world.resolve_position(s->anchor);
    (void)c.gate_of(plain, 3, 1);
    c.rebuild();
    const GaikaId n = c.world.gaika().for_settlement(s->object);
    const ObjectId who = c.invader(1, 1);
    CHECK(unpack_point(c.call(script::CallKind::member, "GetDestPoint", 1,
                              {gaika_value(n), script::Value::object(kTypeObj, who)})
                           .value) == middle);
  }

  // **An offset that leaves the map is refused, not clamped**: the original
  // tests all four edges and falls back to the central building. The northern
  // gate's offset goes *negative*, so the low edges are what refuse it -- a
  // test of the far edges alone would let it through.
  {
    MatchSystem* match = match_system_of(b.world);
    REQUIRE(match != nullptr);
    // A world with no match rectangle at all does **not** refuse: there is
    // nothing to be outside of. That is this engine's departure and it is why
    // the offset above came back at all.
    CHECK(match->rules().map_size == 0);
    MatchRules tight;
    tight.map_size = 100000;  // wide enough that only the low edge can refuse
    match->configure(tight, 0, false);
    CHECK(ask(node, scout) == centre);
    // ...and a rectangle the offset fits inside lets it through again. It has
    // to be the southern gate: 450 north of the northern one is off the top of
    // any map, the town standing three cells from the edge.
    MatchRules wide;
    wide.map_size = 100000;
    match->configure(wide, 0, false);
    const ObjectId southerner = b.invader(3, 6);
    CHECK(ask(node, southerner).y == south_at.y + 450);
  }

}

/// A node with **no settlement** answers its own centre, which is the branch
/// above the whole gate search.
TEST(gaika_dest_point_answers_a_region_nodes_own_centre) {
  WallBench b;
  (void)b.plant(3, 3);
  b.rebuild();
  // The eastern island has no settlement on it, so its node is a region node.
  const GaikaId region = b.world.gaika().at(b.world.lsa(), in_cell(10, 10));
  const GaikaNode* other = b.world.gaika().find(region);
  REQUIRE(other != nullptr);
  REQUIRE(other->settlement == kNoObject);
  const ObjectId scout = b.invader(10, 10);
  const script::HostOutcome out =
      b.call(script::CallKind::member, "GetDestPoint", 1,
             {gaika_value(region), script::Value::object(kTypeObj, scout)});
  CHECK(out.status == script::HostStatus::ok);
  CHECK(unpack_point(out.value) == other->center);
}

/// `squad.UseTeleport(tel, nState, cmd, pt)` -- the one entry point in this
/// family that gives a squad a **three-order itinerary**: walk to the
/// teleport, step through it, walk on from the far side.
///
/// `AIOSENDSQUAD.VS` reaches it as the shortcut arm of a pair whose other arm
/// is a plain `SetCmd(state, 0, SF_ADVCHOOSER, cmd, pt)`, which is why the two
/// share `state`, `cmd` and `pt`.
TEST(use_teleport_walks_a_squad_to_the_gate_through_it_and_on) {
  WallBench b;
  REQUIRE(b.pad_entity.ok());
  const ObjectId pad =
      b.world.spawn(NativeClass::teleport, &b.pad_entity.value(), b.graph.find("Gate"));
  CHECK(b.world.set_position(pad, in_cell(5, 5)));
  CHECK(b.world.set_health(pad, 1000));
  const Point pad_at = b.world.resolve_position(pad);

  const ObjectId first = b.invader(1, 1);
  const ObjectId second = b.invader(1, 2);
  const SquadKey key = b.band(2, 1, {first, second});
  const Point onward = in_cell(12, 12);

  const auto go = [&](const char* verb, std::int32_t state) {
    return b.call(script::CallKind::member, "UseTeleport", 4,
                  {pack_squad(key), script::Value::object(kTypeObj, pad),
                   script::Value::integer(state), script::Value::string(verb),
                   pack_point(onward)});
  };
  const auto verbs = [&](ObjectId id) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < b.commands.command_count(id); ++i) {
      out.emplace_back(b.commands.command_name(id, i));
    }
    return out;
  };

  // Every member gets the three orders, in order.
  CHECK(go("move", 8).value.truthy_scalar());
  CHECK(b.heroes.squads().find(key)->state == 8);
  CHECK(b.heroes.squads().find(key)->state_time == b.world.time());
  CHECK(verbs(first) == std::vector<std::string>({"move", "teleport", "move"}));
  CHECK(verbs(second) == std::vector<std::string>({"move", "teleport", "move"}));
  {
    const CommandQueue* queue = b.commands.find(first);
    REQUIRE(queue != nullptr);
    REQUIRE(queue->entries.size() == 3);
    // The door, which here is the pad's own position: this bench has no height
    // layer, so `sim/entrance.hpp`'s round trip discards every authored point
    // and the list comes back empty. **Which unit the door is measured from is
    // therefore not observable here** -- what is, is that the order carries the
    // door rather than the caller's `pt`. The picker itself, and the water flag
    // `UseTeleport` passes false for, are pinned where the layers exist:
    // `entrance_enter_point_near_picks_the_nearest_of_the_chosen_list`.
    CHECK(queue->entries[0].point == enter_point_near(b.world, pad, first, false));
    CHECK(queue->entries[0].point == pad_at);
    CHECK(queue->entries[1].object == pad);
    CHECK(queue->entries[2].point == onward);
  }

  // **The lock does not stay up**, as in `InvadeThroughGate`.
  CHECK((b.heroes.squads().find(key)->flags & kSquadLocked) == 0);

  // **Only `move`, `advance` and `sneak` are walked to the teleport.** Any
  // other verb skips the first order entirely: the squad is teleported without
  // being sent to the pad first, which is 0x004298ac's three fixed-length
  // comparisons and nothing else.
  for (const char* verb : {"advance", "sneak"}) {
    b.commands.clear_commands(b.world, first);
    CHECK(b.commands.kill_command(b.world, first) || true);
    CHECK(go(verb, 1).value.truthy_scalar());
    CHECK(verbs(first) == std::vector<std::string>({verb, "teleport", verb}));
  }
  CHECK(go("capture", 1).value.truthy_scalar());
  CHECK(verbs(first) == std::vector<std::string>({"teleport", "capture"}));

  // **The receiver has to be a teleport**, which is a class test rather than
  // "does this handle name an object" -- `GetEnterPoint` next door takes any.
  {
    const ObjectId notpad = b.world.spawn(NativeClass::gate, nullptr, b.graph.find("Gate"));
    CHECK(b.world.set_position(notpad, in_cell(5, 5)));
    b.commands.clear_commands(b.world, first);
    const script::HostOutcome out =
        b.call(script::CallKind::member, "UseTeleport", 4,
               {pack_squad(key), script::Value::object(kTypeObj, notpad),
                script::Value::integer(3), script::Value::string("move"), pack_point(onward)});
    CHECK(out.status == script::HostStatus::ok);
    CHECK(!out.value.truthy_scalar());
    CHECK(b.heroes.squads().find(key)->state != 3);
  }

  // A teleport handle whose object is gone answers false rather than reading
  // through it -- the guard the class test below cannot stand in for.
  {
    const script::HostOutcome out =
        b.call(script::CallKind::member, "UseTeleport", 4,
               {pack_squad(key), script::Value::object(kTypeObj, 999999),
                script::Value::integer(6), script::Value::string("move"), pack_point(onward)});
    CHECK(out.status == script::HostStatus::ok);
    CHECK(!out.value.truthy_scalar());
    CHECK(b.heroes.squads().find(key)->state != 6);
  }

  // A handle naming no squad, and a squad with nobody in it, both answer false
  // and leave everything alone.
  {
    const script::HostOutcome gone =
        b.call(script::CallKind::member, "UseTeleport", 4,
               {pack_squad(SquadKey{77, 7}), script::Value::object(kTypeObj, pad),
                script::Value::integer(3), script::Value::string("move"), pack_point(onward)});
    CHECK(!gone.value.truthy_scalar());
    const SquadKey hollow = b.band(2, 1, {});
    const script::HostOutcome empty =
        b.call(script::CallKind::member, "UseTeleport", 4,
               {pack_squad(hollow), script::Value::object(kTypeObj, pad),
                script::Value::integer(3), script::Value::string("move"), pack_point(onward)});
    CHECK(!empty.value.truthy_scalar());
    CHECK(b.heroes.squads().find(hollow)->state == 0);
  }

  // **It stops at a hero**, as `InvadeThroughGate` does.
  {
    const ObjectId champion = b.world.spawn(NativeClass::hero, nullptr, b.graph.find("Invader"));
    CHECK(b.world.set_position(champion, in_cell(1, 1)));
    CHECK(b.world.set_owner(champion, 2));
    CHECK(b.world.set_health(champion, 100));
    const ObjectId behind = b.invader(1, 3);
    const SquadKey led = b.band(2, 1, {champion, behind});
    CHECK(b.call(script::CallKind::member, "UseTeleport", 4,
                 {pack_squad(led), script::Value::object(kTypeObj, pad),
                  script::Value::integer(4), script::Value::string("move"), pack_point(onward)})
              .value.truthy_scalar());
    CHECK(verbs(champion) == std::vector<std::string>({"move", "teleport", "move"}));
    CHECK(verbs(behind).empty());
  }
}

// --------------------------------------------------------------------------
// Squad::CalcGoAround
// --------------------------------------------------------------------------

namespace {

/// Open country, so that five settlements make five nodes on one area and the
/// slot grid joins each to the ones around it.
constexpr std::initializer_list<std::string_view> kOpenField = {
    "................", "................", "................", "................",
    "................", "................", "................", "................",
    "................", "................", "................", "................",
    "................", "................", "................", "................",
};

/// Parse and compile one script into `scheduler`, printing why if it fails.
std::uint32_t build_script(script::Scheduler& scheduler, const script::HostRegistry& registry,
                           std::string_view source, const char* name) {
  script::Diagnostic diagnostic;
  const auto parsed = script::parse(bytes_of(source), name, &diagnostic);
  if (!parsed.ok()) {
    std::printf("  parse %s:%u: %.*s\n", name, diagnostic.line,
                static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
    return script::kNoChunk;
  }
  script::CompileError error;
  auto chunk = script::compile(parsed.value(), &registry, &error);
  if (!chunk.ok()) {
    std::printf("  compile %s:%u: %s\n", name, error.line, error.message.c_str());
    return script::kNoChunk;
  }
  return scheduler.add_chunk(std::move(chunk.value()));
}

/// Five towns in a cross -- the node a squad stands in at the centre, its
/// destination to the east, and one each to the north, south and west -- with
/// everything a detour needs: squads, a command system for the orders, an AI
/// and a scheduler for the script the detour consults, and an `EnvSystem` for
/// that script to leave a trace in.
struct DetourBench : LinkBench {
  HeroSystem heroes;
  CommandSystem commands;
  AiSystem ai;
  EnvSystem env;
  script::HostRegistry registry;
  HostContext context;
  WorldHost host{world};
  script::Scheduler scheduler;
  GaikaId here = kNoGaika, east = kNoGaika, north = kNoGaika, south = kNoGaika, west = kNoGaika;

  DetourBench() : LinkBench(kOpenField, /*shore=*/false) {
    graph.add(bytes_of(R"(<class id="Walker" cpp_class="CVXUnit" parent="">
        <method sig="advance" vs="data/subai/unit_advance.vs"/>
        <method sig="move"    vs="data/subai/unit_move.vs"/>
        <method sig="attack"  vs="data/subai/unit_attack.vs"/>
      </class>)"),
              "walker.sc.xml");
    // A class that binds no `move` at all, so the walk has a member it must
    // pass over without stopping.
    graph.add(bytes_of(R"(<class id="Mule" cpp_class="CVXUnit" parent=""/>)"), "mule.sc.xml");
    graph.link();
    REQUIRE(world.add_system(&heroes));
    REQUIRE(world.add_system(&commands));
    REQUIRE(world.add_system(&ai));
    REQUIRE(world.add_system(&env));
    (void)register_all_hosts(registry);
    (void)register_squad_host(registry);
    context.world = &world;
    context.object_type = kTypeObj;
    scheduler.set_registry(&registry);
    scheduler.set_host(&host);
    scheduler.set_user(&context);
    install_objlist_teardown(scheduler);

    // Planted in this order so the ids are known: the centre is 1 and its
    // neighbour list runs east, north, south, west.
    (void)plant(7, 7);
    (void)plant(13, 7);
    (void)plant(7, 1);
    (void)plant(7, 13);
    (void)plant(1, 7);
    rebuild();
    REQUIRE(world.gaika().count() == 5);
    here = world.gaika().at(world.lsa(), in_cell(7, 7));
    east = world.gaika().at(world.lsa(), in_cell(13, 7));
    north = world.gaika().at(world.lsa(), in_cell(7, 1));
    south = world.gaika().at(world.lsa(), in_cell(7, 13));
    west = world.gaika().at(world.lsa(), in_cell(1, 7));
    REQUIRE(here == 1);
    REQUIRE(east == 2);
    REQUIRE(north == 3);
    REQUIRE(south == 4);
    REQUIRE(west == 5);
    const std::span<const GaikaId> around = world.gaika().neighbours(here);
    REQUIRE(std::vector<GaikaId>(around.begin(), around.end()) ==
            std::vector<GaikaId>({east, north, south, west}));
  }

  /// A stand-in `CheckMAIKA.vs` with the shipped signature. It refuses the
  /// node `refused` names and records every candidate it was shown, in order,
  /// as decimal digits in AI variable 7 of the asking player -- node ids here
  /// are single digits -- and the source and destination it was told in 8
  /// and 9. `path` is where the copy lives: the root `DATA/AI`, or a profile's
  /// own directory.
  void check_maika(GaikaId refused, const char* path = "DATA/AI/CHECKMAIKA.VS") {
    std::string source =
        "// bool, int idPlayer, GAIKA gSrc, GAIKA gDst, GAIKA gMAIKA\n"
        "SetAIVar(idPlayer, 7, AIVar(idPlayer, 7) * 10 + gMAIKA.ID);\n"
        "SetAIVar(idPlayer, 8, gSrc.ID);\n"
        "SetAIVar(idPlayer, 9, gDst.ID);\n"
        "if (gMAIKA.ID == " + std::to_string(refused) + ") return false;\n"
        "return true;\n";
    REQUIRE(build_script(scheduler, registry, source, path) != script::kNoChunk);
  }
  [[nodiscard]] std::int32_t shown(PlayerId owner) { return env.ai_vars().get(owner + 1, 7); }
  [[nodiscard]] std::int32_t told_src(PlayerId owner) { return env.ai_vars().get(owner + 1, 8); }
  [[nodiscard]] std::int32_t told_dst(PlayerId owner) { return env.ai_vars().get(owner + 1, 9); }

  ObjectId unit(const char* cls, Point at, PlayerId owner = 2) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, graph.find(cls));
    CHECK(world.set_position(id, at));
    CHECK(world.set_owner(id, owner));
    CHECK(world.set_health(id, 100));
    return id;
  }
  ObjectId hero(Point at, PlayerId owner = 2) {
    const ObjectId id = world.spawn(NativeClass::hero, nullptr, graph.find("Walker"));
    CHECK(world.set_position(id, at));
    CHECK(world.set_owner(id, owner));
    CHECK(world.set_health(id, 100));
    return id;
  }

  /// A squad of `owner`'s standing in the centre node, bound for `dest`, that
  /// came from `src`.
  SquadKey band(PlayerId owner, std::vector<ObjectId> members, GaikaId dest, GaikaId src) {
    const SquadKey key = heroes.squads().create(owner);
    for (const ObjectId id : members) CHECK(heroes.squads().join(key, id));
    Squad* squad = heroes.squads().find(key);
    CHECK(squad != nullptr);
    if (squad == nullptr) return key;
    squad->gaika_in = here;
    squad->ai_dest = dest;
    squad->dest_gaika = dest;
    squad->src_gaika = src;
    return key;
  }

  script::HostOutcome go_around(SquadKey key) {
    const std::uint32_t index = registry.find(script::CallKind::member, "CalcGoAround", 0);
    CHECK(index != script::kUnresolvedHost);
    if (index == script::kUnresolvedHost) return script::HostOutcome::failed("not declared");
    std::vector<script::Value> receiver = {pack_squad(key)};
    script::CallContext ctx;
    ctx.arguments = receiver;
    ctx.host = &host;
    ctx.scheduler = &scheduler;
    ctx.user = &context;
    ctx.name = "CalcGoAround";
    ctx.kind = script::CallKind::member;
    return registry.entry(index).fn(ctx);
  }
  std::vector<std::string> verbs(ObjectId id) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < commands.command_count(id); ++i) {
      out.emplace_back(commands.command_name(id, i));
    }
    return out;
  }
};

}  // namespace

/// The side test, the score, the script, and what a detour does to the squad.
///
/// The leader stands east of the centre and a little north of the line to the
/// destination, so the centre lies **south** of its line of march: the
/// northern neighbour is on the wrong side and is dropped although it scores
/// best of all, the destination is on the line itself and is dropped because
/// zero is not negative, and the west scores worse than the south. The south
/// is the detour.
TEST(calc_go_around_picks_the_sideways_neighbour_on_the_centres_side_of_the_march) {
  DetourBench b;
  b.check_maika(kNoGaika);
  const Point leader_at{600, 470};
  const ObjectId mule = b.unit("Mule", leader_at);
  const ObjectId first = b.unit("Walker", leader_at);
  const ObjectId champion = b.hero(leader_at);
  const ObjectId behind = b.unit("Walker", leader_at);
  // The first walker is on its way somewhere: that order has to survive.
  Command advance;
  advance.arg_kind = CommandArgKind::point;
  advance.point = in_cell(12, 12);
  (void)b.commands.set_command(b.world, first, "advance", advance);
  const SquadKey key = b.band(2, {mule, first, champion, behind}, b.east, b.west);
  b.heroes.squads().find(key)->flags = kSquadFlagPeaceful;

  const script::HostOutcome out = b.go_around(key);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(out.value.truthy_scalar());

  // Only the south reached the script: the north and the destination fell to
  // the side test and the west scored worse than the best so far. It was told
  // where the squad came from and where it is going.
  CHECK(b.shown(2) == b.south);
  CHECK(b.told_src(2) == b.west);
  CHECK(b.told_dst(2) == b.east);
  const Squad* squad = b.heroes.squads().find(key);
  REQUIRE(squad != nullptr);
  // The detour is recorded as where the squad now comes from, and the lock
  // did not stay up.
  CHECK(squad->src_gaika == b.south);
  CHECK(squad->flags == kSquadFlagPeaceful);
  CHECK(squad->ai_dest == b.east);

  // The mule binds no `move` and is passed over without ending the walk; the
  // walker steps aside and then resumes its advance, at the same point; the
  // hero steps aside and ends the walk; whoever is behind it gets nothing.
  CHECK(b.verbs(mule).empty());
  CHECK(b.verbs(first) == std::vector<std::string>({"move", "advance"}));
  {
    const CommandQueue* queue = b.commands.find(first);
    REQUIRE(queue != nullptr && queue->entries.size() == 2);
    CHECK(queue->entries[0].arg_kind == CommandArgKind::point);
    CHECK(queue->entries[0].point == b.world.gaika().find(b.south)->center);
    CHECK(queue->entries[1].arg_kind == CommandArgKind::point);
    CHECK(queue->entries[1].point == in_cell(12, 12));
  }
  CHECK(b.verbs(champion) == std::vector<std::string>({"move"}));
  CHECK(b.verbs(behind).empty());
}

/// A tie in the score goes to the later neighbour, and the script can refuse
/// it -- in which case the earlier one, which the script also saw, is the
/// detour. The leader on the centre's own line to the destination is what
/// makes a tie: the side test is off, and north and south are mirror images.
TEST(calc_go_around_keeps_the_later_of_two_equal_scores_unless_the_script_refuses_it) {
  {
    DetourBench b;
    b.check_maika(kNoGaika);
    const ObjectId only = b.unit("Walker", Point{608, 480});
    const SquadKey key = b.band(2, {only}, b.east, b.west);
    CHECK(b.go_around(key).value.truthy_scalar());
    CHECK(b.heroes.squads().find(key)->src_gaika == b.south);
    // The east is asked first because nothing has been kept yet, then the
    // north beats it, then the south ties; the west scored worse than the
    // best so far and the script never saw it.
    CHECK(b.shown(2) == (b.east * 10 + b.north) * 10 + b.south);
  }
  {
    DetourBench b;
    b.check_maika(b.south);
    const ObjectId only = b.unit("Walker", Point{608, 480});
    const SquadKey key = b.band(2, {only}, b.east, b.west);
    CHECK(b.go_around(key).value.truthy_scalar());
    CHECK(b.heroes.squads().find(key)->src_gaika == b.north);
    CHECK(b.shown(2) == (b.east * 10 + b.north) * 10 + b.south);
  }
  // The script is asked with the owner 1-based -- a squad of player 5's
  // records under AI player 6 -- and **on the owner's search path**: with a
  // profile of its own carrying a second copy of the file, player 5's squad
  // is answered by that copy and player 2's by the root one. The caller here
  // is no script at all, so the owner is the only player in sight.
  {
    DetourBench b;
    AiProfile profile;
    const auto parsed = AiProfile::parse(bytes_of("[Scripts]\nMain.vs = void\n"));
    REQUIRE(parsed.ok());
    profile = parsed.value();
    b.ai.add_profile("DEFENSIVE", &profile, "DATA\\AI\\DEFENSIVE\\");
    REQUIRE(build_script(b.scheduler, b.registry, "// void\n", "DATA/AI/DEFENSIVE/MAIN.VS") !=
            script::kNoChunk);
    REQUIRE(b.ai.start(5, "DEFENSIVE", AiDifficulty::normal, b.scheduler) == AiStartStatus::ok);
    b.check_maika(kNoGaika);
    b.check_maika(b.south, "DATA/AI/DEFENSIVE/CHECKMAIKA.VS");

    const ObjectId theirs = b.unit("Walker", Point{608, 480}, 5);
    const SquadKey key = b.band(5, {theirs}, b.east, b.west);
    CHECK(b.go_around(key).value.truthy_scalar());
    CHECK(b.heroes.squads().find(key)->src_gaika == b.north);
    CHECK(b.shown(5) == (b.east * 10 + b.north) * 10 + b.south);
    CHECK(b.shown(2) == 0);

    const ObjectId ours = b.unit("Walker", Point{608, 480}, 2);
    const SquadKey root = b.band(2, {ours}, b.east, b.west);
    CHECK(b.go_around(root).value.truthy_scalar());
    CHECK(b.heroes.squads().find(root)->src_gaika == b.south);
  }
}

/// Every way of answering false, and none of them touches the squad.
TEST(calc_go_around_refuses_the_way_back_the_way_there_and_a_squad_going_nowhere) {
  const auto untouched = [](DetourBench& b, SquadKey key, ObjectId member, GaikaId src) {
    const script::HostOutcome out = b.go_around(key);
    CHECK(out.status == script::HostStatus::ok);
    CHECK(!out.value.truthy_scalar());
    const Squad* squad = b.heroes.squads().find(key);
    REQUIRE(squad != nullptr);
    CHECK(squad->src_gaika == src);
    CHECK((squad->flags & kSquadLocked) == 0);
    if (member != kNoObject) CHECK(b.verbs(member).empty());
  };
  // The best candidate is where the squad came from.
  {
    DetourBench b;
    b.check_maika(kNoGaika);
    const ObjectId only = b.unit("Walker", Point{600, 470});
    untouched(b, b.band(2, {only}, b.east, b.south), only, b.south);
    CHECK(b.shown(2) == b.south);
  }
  // The best candidate is the destination itself: the leader stands almost on
  // the line and close to the east, so the east scores best and lies on the
  // centre's side of the line.
  {
    DetourBench b;
    b.check_maika(kNoGaika);
    const ObjectId only = b.unit("Walker", Point{800, 481});
    untouched(b, b.band(2, {only}, b.east, b.west), only, b.west);
    CHECK(b.shown(2) == b.east);
  }
  // The script refuses everyone.
  {
    DetourBench b;
    b.check_maika(b.south);
    const ObjectId only = b.unit("Walker", Point{600, 470});
    untouched(b, b.band(2, {only}, b.east, b.west), only, b.west);
  }
  // No destination, the destination is here, or here is nowhere.
  {
    DetourBench b;
    b.check_maika(kNoGaika);
    const ObjectId only = b.unit("Walker", Point{600, 470});
    untouched(b, b.band(2, {only}, kNoGaika, b.west), only, b.west);
    untouched(b, b.band(2, {only}, b.here, b.west), only, b.west);
    const SquadKey lost = b.band(2, {only}, b.east, b.west);
    b.heroes.squads().find(lost)->gaika_in = kNoGaika;
    untouched(b, lost, only, b.west);
    // `AIDest` falls back to `DestGAIKA` when it is zero, as `squad_ai_dest`
    // reads it everywhere -- and wins over it otherwise.
    const SquadKey fallback = b.band(2, {only}, b.east, b.west);
    b.heroes.squads().find(fallback)->ai_dest = kNoGaika;
    CHECK(b.go_around(fallback).value.truthy_scalar());
    CHECK(b.shown(2) == b.south);
    CHECK(b.told_dst(2) == b.east);
    const SquadKey ahead = b.band(2, {only}, b.east, b.west);
    b.heroes.squads().find(ahead)->dest_gaika = b.north;
    CHECK(b.go_around(ahead).value.truthy_scalar());
    CHECK(b.told_dst(2) == b.east);
  }
  // An empty squad, a squad whose front member is gone -- the original
  // reads the deque's front through the object table and a dead slot is a
  // null -- and a handle naming no squad.
  {
    DetourBench b;
    b.check_maika(kNoGaika);
    untouched(b, b.band(2, {}, b.east, b.west), kNoObject, b.west);
    const ObjectId fallen = b.unit("Walker", Point{600, 470});
    const ObjectId behind = b.unit("Walker", Point{600, 470});
    // It came from the north, so that a body reading the dead slot's
    // `(-1, -1)` as a position would find a west that is not the way back.
    const SquadKey leaderless = b.band(2, {fallen, behind}, b.east, b.north);
    CHECK(b.world.despawn(fallen));
    untouched(b, leaderless, behind, b.north);
    const script::HostOutcome none = b.go_around(SquadKey{9, 2});
    CHECK(none.status == script::HostStatus::ok);
    CHECK(!none.value.truthy_scalar());
  }
}

/// The trampoline answers true when there is no script to run, and a script
/// that cannot finish is this call's failure.
TEST(calc_go_around_without_check_maika_accepts_and_with_a_broken_one_fails) {
  {
    DetourBench b;
    const ObjectId only = b.unit("Walker", Point{600, 470});
    const SquadKey key = b.band(2, {only}, b.east, b.west);
    CHECK(b.go_around(key).value.truthy_scalar());
    CHECK(b.heroes.squads().find(key)->src_gaika == b.south);
    CHECK(b.verbs(only) == std::vector<std::string>({"move"}));
  }
  {
    DetourBench b;
    REQUIRE(build_script(b.scheduler, b.registry,
                         "// bool, int idPlayer, GAIKA gSrc, GAIKA gDst, GAIKA gMAIKA\n"
                         "Sleep(10);\nreturn true;\n",
                         "DATA/AI/CHECKMAIKA.VS") != script::kNoChunk);
    const ObjectId only = b.unit("Walker", Point{600, 470});
    const SquadKey key = b.band(2, {only}, b.east, b.west);
    CHECK(b.go_around(key).status == script::HostStatus::error);
    CHECK(b.heroes.squads().find(key)->src_gaika == b.west);
    CHECK(b.verbs(only).empty());
  }
}

// --------------------------------------------------------------------------
// NearestHospital
// --------------------------------------------------------------------------

namespace {

/// `ShoreBench` -- two islands and the sea between them -- with squads on it
/// and settlements that can be hospitals: a holder with room and a store with
/// food, both of which `ShoreBench::plant` leaves at zero.
struct HospitalBench : ShoreBench {
  HospitalBench() {
    graph.add(bytes_of(R"(<class id="Walker" cpp_class="CVXUnit" parent=""/>)"), "walker.sc.xml");
    graph.link();
  }

  /// A settlement of `owner`'s on a cell, with `room` free places in its
  /// holder and `food` in its store. `cls` picks the central building's
  /// class, which is what makes it a village or not.
  SettlementId town(std::int32_t cx, std::int32_t cy, PlayerId owner, const char* cls = "Townhall",
                    std::int32_t room = 10, std::int32_t food = 1000) {
    const SettlementKind kind =
        std::string_view(cls) == "Village" ? SettlementKind::village : SettlementKind::stronghold;
    const ObjectId anchor =
        world.spawn(kind == SettlementKind::stronghold ? NativeClass::town_hall : NativeClass::building,
                    nullptr, graph.find(cls));
    CHECK(world.set_position(anchor, in_cell(cx, cy)));
    SettlementInit init;
    init.kind = kind;
    init.anchor = anchor;
    init.owner = owner;
    init.max_units = room;
    init.max_food = 100000;
    init.food = food;
    return economy.create(world, init);
  }
  GaikaId node_of(SettlementId id) {
    const Settlement* s = economy.settlements().find(id);
    return s == nullptr ? kNoGaika : world.gaika().for_settlement(s->object);
  }

  ObjectId walker(std::int32_t cx, std::int32_t cy, PlayerId owner) {
    const ObjectId id = world.spawn(NativeClass::unit, nullptr, graph.find("Walker"));
    CHECK(world.set_position(id, in_cell(cx, cy)));
    CHECK(world.set_owner(id, owner));
    CHECK(world.set_health(id, 100));
    return id;
  }

  /// A squad of `owner`'s, worth `eval`, standing in `in` and going nowhere.
  SquadKey band(PlayerId owner, std::vector<ObjectId> members, GaikaId in, std::int32_t eval) {
    const SquadKey key = heroes.squads().create(owner);
    for (const ObjectId id : members) CHECK(heroes.squads().join(key, id));
    Squad* squad = heroes.squads().find(key);
    CHECK(squad != nullptr);
    if (squad == nullptr) return key;
    squad->gaika_in = in;
    squad->eval = eval;
    return key;
  }

  GaikaId hospital(SquadKey key) {
    const script::HostOutcome out =
        call(script::CallKind::free_function, "NearestHospital", {pack_squad(key)});
    CHECK(out.status == script::HostStatus::ok);
    return out.value.is_integer() ? out.value.as_integer() : -1;
  }
};

}  // namespace

/// The four gates, and the distance score with its two multipliers. The
/// leader stands mid-island; a town 286 away and a village 271 away compete,
/// and the village's half-again makes it 406.
TEST(nearest_hospital_wants_an_own_town_with_room_and_food_and_scores_by_distance) {
  HospitalBench b;
  const SettlementId hall = b.town(10, 4, 2);
  const SettlementId village = b.town(3, 9, 2, "Village");
  // Three nearer ones that fail a gate each: the wrong owner, no room, not
  // enough food. (5,4) is 143 from the leader, against the hall's 286.
  (void)b.town(7, 4, 1);
  (void)b.town(8, 8, 2, "Townhall", /*room=*/0);
  const SettlementId near = b.town(5, 4, 2, "Townhall", 10, /*food=*/299);
  b.rebuild();
  const GaikaId at_hall = b.node_of(hall);
  const GaikaId at_village = b.node_of(village);
  const GaikaId at_near = b.node_of(near);
  REQUIRE(at_hall != kNoGaika && at_village != kNoGaika && at_near != kNoGaika);

  const ObjectId leader = b.walker(6, 6, 2);
  const SquadKey key = b.band(2, {leader}, b.world.gaika().at(b.world.lsa(), in_cell(6, 6)), 100);
  CHECK(b.hospital(key) == at_hall);

  // 300 food is exactly enough: the comparison is `>=`.
  Settlement* nearest = b.economy.settlements().find(near);
  REQUIRE(nearest != nullptr);
  nearest->warehouse.food = 300;
  CHECK(b.hospital(key) == at_near);

  // A holder with exactly one place left has room; the full one has none.
  nearest->holder.max_units = 1;
  CHECK(b.hospital(key) == at_near);
  CHECK(nearest->holder.force_add(b.walker(5, 4, 2)));
  CHECK(b.hospital(key) == at_hall);

  // Without the village's half-again, 271 beats 286; with it, 406 does not.
  Settlement* town = b.economy.settlements().find(hall);
  REQUIRE(town != nullptr);
  town->warehouse.food = 0;
  CHECK(b.hospital(key) == at_village);
}

/// The enemy strength standing in a hospital's node: past three quarters of
/// the squad's own it is no hospital, past half it is twice as far, and the
/// peaceful squads of players 14 and 15 -- and only those -- do not count.
TEST(nearest_hospital_weighs_the_enemies_standing_in_the_node) {
  HospitalBench b;
  const SettlementId hall = b.town(10, 4, 2);
  const SettlementId village = b.town(3, 9, 2, "Village");
  b.rebuild();
  const GaikaId at_hall = b.node_of(hall);
  const GaikaId at_village = b.node_of(village);
  const ObjectId leader = b.walker(6, 6, 2);
  const SquadKey key = b.band(2, {leader}, b.world.gaika().at(b.world.lsa(), in_cell(6, 6)), 100);
  // Everyone is an enemy until told otherwise -- bit 0 clear is war -- so
  // only the ally and the neutral need setting: 4 is allied both ways, 5 is
  // at peace and nothing more.
  b.world.players().set(2, 4, Relation::ceasefire, true);
  b.world.players().set(4, 2, Relation::ceasefire, true);
  b.world.players().set(2, 4, Relation::allied, true);
  b.world.players().set(4, 2, Relation::allied, true);
  b.world.players().set(2, 5, Relation::ceasefire, true);

  // Player 1's squad standing at the hall, worth `eval`.
  const ObjectId foe = b.walker(10, 4, 1);
  const SquadKey raiders = b.band(1, {foe}, at_hall, 50);
  const auto raiders_worth = [&](std::int32_t eval) { b.heroes.squads().find(raiders)->eval = eval; };
  // Half exactly is not more than half: the hall at 286 still beats 406.
  CHECK(b.hospital(key) == at_hall);
  // One past half doubles it to 572, and the village wins.
  raiders_worth(51);
  CHECK(b.hospital(key) == at_village);
  // Three quarters exactly is still a hospital -- doubled, but there.
  Settlement* v = b.economy.settlements().find(village);
  REQUIRE(v != nullptr);
  v->warehouse.food = 0;
  raiders_worth(75);
  CHECK(b.hospital(key) == at_hall);
  // One past it is none at all.
  raiders_worth(76);
  CHECK(b.hospital(key) == kNoGaika);
  v->warehouse.food = 1000;
  CHECK(b.hospital(key) == at_village);

  // Only a squad *staying* in the node counts: one heading for it from the
  // village does not, and one standing in it but leaving for the village
  // does not either -- `AI_STAYING` alone, not the census's "in this node".
  raiders_worth(1000);
  b.heroes.squads().find(raiders)->gaika_in = at_village;
  b.heroes.squads().find(raiders)->dest_gaika = at_hall;
  CHECK(b.hospital(key) == at_hall);
  b.heroes.squads().find(raiders)->gaika_in = at_hall;
  b.heroes.squads().find(raiders)->dest_gaika = at_village;
  CHECK(b.hospital(key) == at_hall);
  b.heroes.squads().find(raiders)->dest_gaika = kNoGaika;
  CHECK(b.hospital(key) == at_village);
  // A strengthless squad does not count, whoever it is.
  raiders_worth(0);
  CHECK(b.hospital(key) == at_hall);
  raiders_worth(1000);

  raiders_worth(0);

  // One squad at a time standing at the hall, worth more than the whole
  // squad, of each owner in turn. A key is the table's order, so each is a
  // new squad rather than a renamed one, and the last is silenced by
  // strength before the next -- which the case above says is enough.
  const auto standing = [&](PlayerId owner, std::uint16_t flags) {
    const SquadKey them = b.band(owner, {b.walker(10, 4, owner)}, at_hall, 1000);
    b.heroes.squads().find(them)->flags = flags;
    const GaikaId answer = b.hospital(key);
    b.heroes.squads().find(them)->eval = 0;
    return answer;
  };
  // An ally's squad and the squad's own do not count; a mere non-enemy's
  // does not either.
  CHECK(standing(4, 0) == at_hall);
  CHECK(standing(2, 0) == at_hall);
  CHECK(standing(5, 0) == at_hall);

  // **Peaceful, and whose.** Player 14's peaceful squad does not count, nor
  // 15's; player 3's peaceful squad does, and player 14's warlike one does.
  CHECK(standing(14, kSquadFlagPeaceful) == at_hall);
  CHECK(standing(15, kSquadFlagPeaceful) == at_hall);
  CHECK(standing(3, kSquadFlagPeaceful) == at_village);
  CHECK(standing(14, 0) == at_village);
}

/// Across the water the distance is multiplied by the length of the area
/// path, and no path is no hospital. The leader stands on the west island's
/// east coast: the east island's town is 256 away as the crow flies and 768
/// as the ship sails, against a town of its own island at 607.
TEST(nearest_hospital_multiplies_by_the_area_path_and_needs_a_ship_to_cross) {
  HospitalBench b;
  const SettlementId home = b.town(3, 3, 2);
  const SettlementId abroad = b.town(16, 6, 2);
  b.rebuild();
  const GaikaId at_home = b.node_of(home);
  const GaikaId at_abroad = b.node_of(abroad);
  REQUIRE(at_home != kNoGaika && at_abroad != kNoGaika);
  const ObjectId leader = b.walker(12, 6, 2);
  const GaikaId standing = b.world.gaika().at(b.world.lsa(), in_cell(12, 6));
  REQUIRE(standing == at_home);
  const SquadKey key = b.band(2, {leader}, standing, 100);

  // No ship of the player's in the sea: the near town across the water is
  // unreachable and the far one at home is the answer.
  CHECK(b.hospital(key) == at_home);
  Settlement* h = b.economy.settlements().find(home);
  REQUIRE(h != nullptr);
  h->warehouse.food = 0;
  CHECK(b.hospital(key) == kNoGaika);
  // Somebody else's ship does not help.
  (void)b.ship(14, 6, 1, "idle");
  CHECK(b.hospital(key) == kNoGaika);
  // The player's own idle ship makes the crossing a route of three.
  (void)b.ship(14, 7, 2, "idle");
  CHECK(b.hospital(key) == at_abroad);
  // And three times 256 is more than 607, so with the home town fed again it
  // wins although it is further as the crow flies.
  h->warehouse.food = 1000;
  CHECK(b.hospital(key) == at_home);

  // A squad standing in no node has no area to start from and no hospital.
  b.heroes.squads().find(key)->gaika_in = kNoGaika;
  CHECK(b.hospital(key) == kNoGaika);

  // An empty squad, and a handle naming none, answer 0 without a walk.
  CHECK(b.hospital(b.band(2, {}, at_home, 100)) == kNoGaika);
  CHECK(b.hospital(SquadKey{9, 2}) == kNoGaika);
}

/// The two multipliers are three halves and two, not the other way round,
/// and a tie in the score keeps the earlier node. The leader is at (6,6):
/// the towns at (10,4) and (10,8) are 286 each, the town at (12,9) is 429,
/// the village at (3,9) is 271 and so 406.
TEST(nearest_hospital_ranks_by_the_multiplied_distance_and_breaks_ties_by_id) {
  HospitalBench b;
  const SettlementId first = b.town(10, 4, 2);
  const SettlementId second = b.town(10, 8, 2);
  const SettlementId far = b.town(12, 9, 2);
  const SettlementId village = b.town(3, 9, 2, "Village");
  b.rebuild();
  const ObjectId leader = b.walker(6, 6, 2);
  const SquadKey key = b.band(2, {leader}, b.world.gaika().at(b.world.lsa(), in_cell(6, 6)), 100);
  Settlement* one = b.economy.settlements().find(first);
  Settlement* two = b.economy.settlements().find(second);
  Settlement* v = b.economy.settlements().find(village);
  REQUIRE(one != nullptr && two != nullptr && v != nullptr);

  // Equal at 286: the first planted, which is the lower node id.
  CHECK(b.hospital(key) == b.node_of(first));
  one->warehouse.food = 0;
  CHECK(b.hospital(key) == b.node_of(second));

  // The village's 271 is 406 at three halves and would be 542 at two: the
  // 429 town sits between, so which multiplier it is decides.
  two->warehouse.food = 0;
  CHECK(b.hospital(key) == b.node_of(village));
  v->warehouse.food = 0;
  CHECK(b.hospital(key) == b.node_of(far));

  // And the enemy doubling is two, not three halves: the first town under
  // pressure is 572, and would be 429 -- a tie with the far town that the
  // lower id would win.
  one->warehouse.food = 1000;
  const SquadKey raiders = b.band(1, {b.walker(10, 4, 1)}, b.node_of(first), 60);
  (void)raiders;
  CHECK(b.hospital(key) == b.node_of(far));
}

/// The refusal behind the fallback: a node with **no** area -- nothing labelled
/// within reach at all -- answers an invalid handle at 0x0042c82e, before the
/// walk. A stronghold is its own nearest town hall, so without that guard one
/// with an area of `kNoLsa` would match itself.
TEST(best_to_supply_refuses_a_node_with_no_area_within_reach) {
  LinkBench b({"~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
               "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
               "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~",
               "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~", "~~~~~~~~~~~~~~~~"},
              /*shore=*/false);
  REQUIRE(b.world.lsa().size() == 0);
  const SettlementId adrift = b.plant(3, 3, 0);
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 1);
  CHECK(b.world.gaika().find(1)->lsa == kNoLsa);

  script::HostRegistry registry;
  (void)register_all_hosts(registry);
  HostContext context;
  context.world = &b.world;
  const std::uint32_t index = registry.find(script::CallKind::member, "BestToSupply", 0);
  REQUIRE(index != script::kUnresolvedHost);
  const Settlement* s = b.economy.settlements().find(adrift);
  REQUIRE(s != nullptr);
  std::vector<script::Value> args{script::Value::object(kTypeSettlement, s->object)};
  script::CallContext ctx;
  ctx.arguments = args;
  ctx.user = &context;
  ctx.name = "BestToSupply";
  ctx.kind = script::CallKind::member;
  const script::HostOutcome out = registry.entry(index).fn(ctx);
  CHECK(out.status == script::HostStatus::ok);
  CHECK(!out.value.is_object() || out.value.as_object().type == script::kNoType);
}

namespace {

/// Forty cells of open land by sixteen: 2,560 units by 1,024, one area, three
/// 1,024-unit fog cells across and one down. Not square, so that a slot's row
/// and column cannot be confused without it showing.
constexpr std::initializer_list<std::string_view> kOpenLand = {
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................",
    "........................................"};

}  // namespace

/// **A node is `Explored` once any slot credited to it is**, not only its
/// centre (`AiSystem::advance`, the second reading). The node here is the
/// whole of one open field and its centre is in the top-left fog cell; seeing
/// the far corner of the field is seeing the node.
TEST(a_node_is_explored_once_any_slot_credited_to_it_is) {
  LinkBench b(kOpenLand, /*shore=*/false, /*rect=*/true);
  FogSystem fog;
  REQUIRE(b.world.add_system(&fog));
  fog.map().resize(2560);
  b.plant(5, 5);
  b.rebuild();
  REQUIRE(b.world.gaika().count() == 1);
  const GaikaTable& table = b.world.gaika();
  // Every slot of the field is credited to the one node.
  REQUIRE(table.slot_columns() == 20);
  REQUIRE(table.slot_nodes().size() == 160);
  for (const GaikaId id : table.slot_nodes()) CHECK(id == 1);

  AiSystem ai;
  ai.seed_gaika_view(1, table.count());
  ai.seed_gaika_view(2, table.count());
  const auto explored = [&](PlayerId player) {
    return (ai.gaika_view(player)->find(1)->flags & kLaikaExplored) != 0;
  };
  const auto sweep = [&](GameTime time) {
    Turn turn;
    turn.time = time;
    ai.advance(b.world, turn);
  };
  sweep(1000);
  CHECK(!explored(1));
  // Player 1 sees the far end, two fog cells east of the centre.
  fog.map().explore(Point{2400, 500}, 1);
  sweep(2000);
  CHECK(explored(1));
  CHECK(ai.gaika_view(1)->find(1)->last_seen == 2000);
  CHECK(!explored(2));
  // And the stamp follows it on later passes, as a centre-explored node's does.
  sweep(3000);
  CHECK(ai.gaika_view(1)->find(1)->last_seen == 3000);
}
