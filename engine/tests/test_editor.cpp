// The editor's brushes over synthetic grids: the disc, a terrain stroke and
// the water rules, the passability rebuild -- the terrain's rules, a mask
// stamped through the projection, the frame -- the three height tools and
// the light they leave behind, and the decoration stamps.

#include <cstdint>
#include <string>
#include <vector>

#include "builder.hpp"
#include "imperivm/core/formats/grid.hpp"
#include "imperivm/core/world/editor.hpp"
#include "imperivm/core/world/map.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::edit;
using imperivm::test::Builder;

namespace {

OwnedGrid grid8(std::uint32_t cell, std::uint32_t cells, std::uint32_t fill) {
  auto owned = OwnedGrid::create(cell, 8, cell * cells, cell * cells);
  CHECK(owned.ok());
  (void)owned->grid().fill(fill);
  return std::move(owned.value());
}

TerrainTable table() {
  Builder xml;
  xml.text(
      "<terrain>"
      "<layer z=\"3\" type=\"1\" display=\"Grass 1\" image=\"g.vq\"/>"
      "<layer z=\"4\" type=\"1\" display=\"Grass 2\" image=\"g.vq\"/>"
      "<layer z=\"6\" type=\"5\" display=\"Rocks 1\" passable=\"0\" image=\"r.vq\"/>"
      "<layer z=\"12\" type=\"4\" display=\"Shallow\" image=\"s.vq\"/>"
      "<layer z=\"13\" type=\"4\" display=\"Deep\" passable_water=\"1\" image=\"d.vq\"/>"
      "</terrain>");
  auto parsed = TerrainTable::parse(xml.span());
  CHECK(parsed.ok());
  return std::move(parsed.value());
}

}  // namespace

TEST(editor_brush_disc_and_slot_radii) {
  // The five slots: 0, 1, 2, 3, 5 cells for terrain and decor; 1, 3, 5, 7, 9
  // for height.
  CHECK(brush_radius(0, false) == 0);
  CHECK(brush_radius(3, false) == 3);
  CHECK(brush_radius(4, false) == 5);
  CHECK(brush_radius(0, true) == 1);
  CHECK(brush_radius(4, true) == 9);
  CHECK(brush_radius(9, false) == 5);  // clamped
  // i^2 + j^2 <= r^2: radius 0 is one cell, 1 is the plus, 2 is 13 cells.
  CHECK(brush_disc(0).size() == 1);
  CHECK(brush_disc(1).size() == 5);
  CHECK(brush_disc(2).size() == 13);
  CHECK(brush_disc(5).size() == 81);
  // The cell of a stroke: the one whose corner is the grid corner nearest
  // the pointer -- (100, 100) is nearer 128 than 64, so cell 2.
  CHECK(terrain_brush_cell({100, 100}) == (sim::Point{2, 2}));
  CHECK(terrain_brush_cell({95, 30}) == (sim::Point{1, 0}));
  CHECK(terrain_brush_cell({31, 31}) == (sim::Point{0, 0}));
  CHECK(terrain_brush_cell({32, 32}) == (sim::Point{1, 1}));
  CHECK(!terrain_stroke_moved({100, 100}, {120, 120}));
  CHECK(terrain_stroke_moved({100, 100}, {133, 100}));
}

TEST(editor_terrain_stroke_paints_a_disc_and_rebakes_the_passability_under_it) {
  OwnedGrid terrain = grid8(64, 16, 3);
  auto pass = OwnedGrid::create(16, 1, 1024, 1024);
  REQUIRE(pass.ok());
  sim::Rng rng(7);
  const std::int32_t rocks[] = {6};
  const CellRect changed = paint_terrain(terrain.grid(), 8, 8, 1, rocks, false, false, rng);
  CHECK(!changed.empty());
  CHECK(changed.x0 == 7 && changed.x1 == 9 && changed.y0 == 7 && changed.y1 == 9);
  CHECK(terrain.grid().cell(8, 8) == 6);
  CHECK(terrain.grid().cell(7, 8) == 6);
  CHECK(terrain.grid().cell(7, 7) == 3);  // the corner is outside radius 1
  // Rocks are passable="0": the passability cells that *sample* a rock cell
  // are blocked, the grass beside them free. The bake samples each 16-unit
  // cell 32 units on from its corner, so the window of terrain cell 8 (world
  // 512..575) is the four pass cells at 480..543 -- 30..33, not 32..35.
  const WorldRect map = WorldRect::of_map(terrain.grid());
  bake_terrain_passability(pass->grid(), terrain.grid(), table(), terrain_stroke_rect(8, 8, 1), map);
  CHECK(pass->grid().cell(30, 30) == 1);
  CHECK(pass->grid().cell(33, 33) == 1);
  CHECK(pass->grid().cell(34, 34) == 0);
  CHECK(pass->grid().cell(29, 29) == 0);
  CHECK(pass->grid().cell(28, 32) == 1);  // 28 samples terrain cell 7, in the disc
  CHECK(pass->grid().cell(28, 28) == 0);  // (7, 7) is the corner, outside it
  CHECK(pass->grid().count_set() == 5 * 16);
  // A group draws one of its layers per cell: every cell ends up grass 1 or 2.
  const std::int32_t grasses[] = {3, 4};
  (void)paint_terrain(terrain.grid(), 8, 8, 1, grasses, false, false, rng);
  for (const sim::Point d : brush_disc(1)) {
    const std::uint32_t z = terrain.grid().cell(static_cast<std::uint32_t>(8 + d.x), static_cast<std::uint32_t>(8 + d.y));
    CHECK(z == 3 || z == 4);
  }
  // Painting an unchanged value reports nothing.
  const std::int32_t grass[] = {3};
  (void)paint_terrain(terrain.grid(), 2, 2, 0, grass, false, false, rng);
  CHECK(paint_terrain(terrain.grid(), 2, 2, 0, grass, false, false, rng).empty());
  // Off the grid is clipped, not written.
  CHECK(!paint_terrain(terrain.grid(), 0, 0, 2, rocks, false, false, rng).empty());
}

TEST(editor_deep_water_needs_water_all_around_unless_shift) {
  OwnedGrid terrain = grid8(64, 24, 3);
  sim::Rng rng(1);
  const std::int32_t deep[] = {13};
  // Radius 2 on grass: the first pass writes shallow water everywhere, and no
  // cell has water four deep around it, so nothing goes deep.
  (void)paint_terrain(terrain.grid(), 12, 12, 2, deep, false, false, rng);
  CHECK(terrain.grid().cell(12, 12) == 12);
  CHECK(terrain.grid().cell(10, 12) == 12);
  // No single brush covers the nine-by-nine square the rule asks for --
  // radius 5 misses its corners -- so a lake is deepened by overlapping
  // strokes: four shallow ones around the centre, then deep at it.
  const std::int32_t shallow[] = {12};
  (void)paint_terrain(terrain.grid(), 8, 12, 5, shallow, false, false, rng);
  (void)paint_terrain(terrain.grid(), 16, 12, 5, shallow, false, false, rng);
  (void)paint_terrain(terrain.grid(), 12, 8, 5, shallow, false, false, rng);
  (void)paint_terrain(terrain.grid(), 12, 16, 5, shallow, false, false, rng);
  (void)paint_terrain(terrain.grid(), 12, 12, 0, deep, false, false, rng);
  CHECK(terrain.grid().cell(12, 12) == 13);
  (void)paint_terrain(terrain.grid(), 12, 12, 1, deep, false, false, rng);
  CHECK(terrain.grid().cell(12, 11) == 13);
  CHECK(terrain.grid().cell(12, 7) == 12);
  // Shift writes deep water regardless.
  (void)paint_terrain(terrain.grid(), 2, 2, 1, deep, false, true, rng);
  CHECK(terrain.grid().cell(2, 2) == 13);
  CHECK(terrain.grid().cell(1, 2) == 13);
  // The Water group is deep water whatever the group lists.
  const std::int32_t waters[] = {12, 13};
  (void)paint_terrain(terrain.grid(), 20, 20, 0, waters, true, true, rng);
  CHECK(terrain.grid().cell(20, 20) == 13);
  // Land painted beside a deep cell takes its depth: the centre of the lake
  // loses its deep neighbours-all-round.
  const std::int32_t grass[] = {3};
  (void)paint_terrain(terrain.grid(), 12, 9, 0, grass, false, false, rng);
  CHECK(terrain.grid().cell(12, 9) == 3);
  CHECK(terrain.grid().cell(12, 12) == 12);
}

TEST(editor_height_tools_set_add_once_per_stroke_and_smooth) {
  CHECK(height_of_level(100) == 253);
  CHECK(height_of_level(0) == 0);
  CHECK(level_of_height(253) == 99);
  CHECK(height_delta(100) == 64);
  CHECK(height_delta(-100) == -64);
  CHECK(height_delta(0) == 0);
  CHECK(height_delta(50) == 32);
  OwnedGrid heights = grid8(32, 16, 100);
  HeightStroke stroke;
  stroke.begin(heights.grid());
  // Set: every cell of the disc to the level's height.
  CellRect changed = apply_height(heights.grid(), stroke, HeightTool::kSet, 8, 8, 1, 50);
  CHECK(!changed.empty());
  CHECK(heights.grid().cell(8, 8) == static_cast<std::uint32_t>(height_of_level(50)));
  CHECK(heights.grid().cell(7, 7) == 100);
  // Raise: +32 once; the same cell again in the same stroke is left alone,
  // a new stroke adds again.
  stroke.begin(heights.grid());
  (void)apply_height(heights.grid(), stroke, HeightTool::kRaiseLower, 2, 2, 0, 50);
  CHECK(heights.grid().cell(2, 2) == 132);
  CHECK(apply_height(heights.grid(), stroke, HeightTool::kRaiseLower, 2, 2, 0, 50).empty());
  CHECK(heights.grid().cell(2, 2) == 132);
  stroke.begin(heights.grid());
  (void)apply_height(heights.grid(), stroke, HeightTool::kRaiseLower, 2, 2, 0, 50);
  CHECK(heights.grid().cell(2, 2) == 164);
  // Lower clamps at the floor.
  stroke.begin(heights.grid());
  (void)apply_height(heights.grid(), stroke, HeightTool::kRaiseLower, 12, 12, 0, -100);
  CHECK(heights.grid().cell(12, 12) == 36);
  stroke.begin(heights.grid());
  (void)apply_height(heights.grid(), stroke, HeightTool::kRaiseLower, 12, 12, 0, -100);
  CHECK(heights.grid().cell(12, 12) == 0);
  // Smooth: a spike of 200 among 100s becomes (64*200 + 26*400) / (64 + 104).
  (void)heights.grid().set_cell(5, 5, 200);
  (void)apply_height(heights.grid(), stroke, HeightTool::kSmooth, 5, 5, 0, 0);
  CHECK(heights.grid().cell(5, 5) == (64 * 200 + 26 * 400) / 168);
}

TEST(editor_light_is_the_slope_of_the_height) {
  OwnedGrid heights = grid8(32, 8, 100);
  OwnedGrid light = grid8(32, 8, 0);
  // Flat ground is the neutral 16 everywhere, the far edge included.
  rebake_light(light.grid(), heights.grid(), CellRect{0, 0, 7, 7});
  CHECK(light.grid().cell(3, 3) == 16);
  CHECK(light.grid().cell(7, 7) == 16);
  CHECK(light.grid().cell(7, 0) == 16);
  // A step up to the right and below darkens the cell before it and lights
  // the cell on the step: 100 - (110 + 110) / 2 + 16 = 6; 110 - 100 + 16 = 26 -> 21.
  for (std::uint32_t y = 0; y < 8; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      if (x >= 4 || y >= 4) (void)heights.grid().set_cell(x, y, 110);
    }
  }
  rebake_light(light.grid(), heights.grid(), CellRect{3, 3, 4, 4});
  CHECK(light.grid().cell(3, 3) == 6);
  CHECK(light.grid().cell(2, 3) == 16 - 5);  // 100 - (100 + 110) / 2 + 16
  CHECK(light.grid().cell(4, 4) == 16);
  // The cells left of and above the changed ones are recomputed too.
  CHECK(light.grid().cell(2, 2) == 16);
}

TEST(editor_decorations_stamp_scatter_and_clear) {
  auto owned = OwnedGrid::create(64, 16, 1024, 1024);
  REQUIRE(owned.ok());
  Grid& decor = owned->grid();
  // An exact stamp keeps the position to four units: (70, 200) is cell (1, 3)
  // with nibbles round(6/4) = 2 and round(8/4) = 2.
  CellRect changed = stamp_decor(decor, {70, 200}, 31);
  CHECK(changed.x0 == 1 && changed.y0 == 3);
  DecorCell cell;
  REQUIRE(decor_unpack(decor.cell(1, 3), cell));
  CHECK(cell.kind == 31);
  CHECK(cell.offset_x == 8);
  CHECK(cell.offset_y == 8);
  // A remainder that rounds to sixteen carries into the next cell.
  changed = stamp_decor(decor, {126, 0}, 5);
  CHECK(changed.x0 == 2);
  REQUIRE(decor_unpack(decor.cell(2, 0), cell));
  CHECK(cell.offset_x == 0);
  // An occupied cell is overwritten by an exact stamp.
  (void)stamp_decor(decor, {70, 200}, 40);
  REQUIRE(decor_unpack(decor.cell(1, 3), cell));
  CHECK(cell.kind == 40);
  // Scatter at density 100 fills every empty cell of the disc from the
  // kinds, at density 0 nothing; an occupied cell is skipped.
  sim::Rng rng(3);
  const std::int32_t kinds[] = {7, 8};
  CHECK(scatter_decor(decor, 8, 8, 2, kinds, 0, rng).empty());
  changed = scatter_decor(decor, 8, 8, 2, kinds, 100, rng);
  CHECK(!changed.empty());
  std::size_t filled = 0;
  for (const sim::Point d : brush_disc(2)) {
    DecorCell c;
    if (decor_unpack(decor.cell(static_cast<std::uint32_t>(8 + d.x), static_cast<std::uint32_t>(8 + d.y)), c)) {
      ++filled;
      CHECK(c.kind == 7 || c.kind == 8);
    }
  }
  CHECK(filled == 13);
  (void)scatter_decor(decor, 1, 3, 0, kinds, 100, rng);
  REQUIRE(decor_unpack(decor.cell(1, 3), cell));
  CHECK(cell.kind == 40);  // still the stamp: scatter does not overwrite
  // Clear takes the disc out and reports only what held something.
  changed = clear_decor(decor, 8, 8, 2);
  CHECK(changed.x0 == 6 && changed.x1 == 10);
  CHECK(decor.cell(8, 8) == 0);
  CHECK(clear_decor(decor, 8, 8, 2).empty());
}

namespace {

/// A blank `.pass`: the shipped geometry, 128 cells of 16 units, no cell set.
OwnedGrid blank_mask() {
  auto owned = OwnedGrid::create(16, 1, 2048, 2048);
  CHECK(owned.ok());
  return std::move(owned.value());
}

OwnedGrid pass_for(std::uint32_t cells) {
  auto owned = OwnedGrid::create(16, 1, cells * 16, cells * 16);
  CHECK(owned.ok());
  return std::move(owned.value());
}

std::size_t set_in_column(const Grid& pass, std::uint32_t cx) {
  std::size_t n = 0;
  for (std::uint32_t y = 0; y < pass.height(); ++y) n += pass.cell(cx, y) != 0;
  return n;
}

}  // namespace

TEST(editor_stamp_is_mirrored_and_lands_through_the_projection) {
  // Three cells in one column of the mask: rows 63, 64 and 66 of column 64.
  OwnedGrid mask = blank_mask();
  (void)mask.grid().set_cell(64, 63, 1);
  (void)mask.grid().set_cell(64, 64, 1);
  (void)mask.grid().set_cell(64, 66, 1);
  OwnedGrid terrain = grid8(64, 64, 3);  // 4096 units, flat: no height layer
  OwnedGrid pass = pass_for(256);
  const WorldRect map = WorldRect::of_map(terrain.grid());
  Grid no_height;
  const PassMask footprint{&mask.grid(), mask_bounds(mask.grid())};
  CHECK(footprint.bounds.x0 == 64 && footprint.bounds.x1 == 64);
  CHECK(footprint.bounds.y0 == 63 && footprint.bounds.y1 == 66);
  stamp_footprint(pass.grid(), footprint, {1000, 1000}, no_height, map, map);
  // Column 64 of the mask is `16 * 64 - 1016 = 8` units right of the anchor:
  // world x 1008, cell 63.
  CHECK(set_in_column(pass.grid(), 63) == 3);
  CHECK(set_in_column(pass.grid(), 62) == 0);
  CHECK(set_in_column(pass.grid(), 64) == 0);
  // Mirrored: row 63 (above the centre in the file) lands *below* the anchor
  // in world y, row 66 above it. The anchor's row, 1000, is cell 62.
  CHECK(pass.grid().cell(63, 63) == 1);  // row 63 -> world 1022
  CHECK(pass.grid().cell(63, 62) == 1);  // row 64 -> world 1007
  CHECK(pass.grid().cell(63, 61) == 1);  // row 66 -> world 976
  CHECK(pass.grid().cell(63, 64) == 0);
  // The flat extent 0x005fe080 answers is mirrored the same way.
  const WorldRect box = footprint_rect(footprint, {1000, 1000});
  CHECK(box.x0 == 1008 && box.x1 == 1008);
  CHECK(box.y0 == 1000 + 1032 - 66 * 16);
  CHECK(box.y1 == 1000 + 1032 - 63 * 16);
  OwnedGrid blank = blank_mask();
  const PassMask nothing{&blank.grid(), mask_bounds(blank.grid())};
  CHECK(nothing.empty());
  CHECK(footprint_rect(nothing, {0, 0}).empty());
  // Only cells inside the limit are written.
  OwnedGrid limited = pass_for(256);
  stamp_footprint(limited.grid(), footprint, {1000, 1000}, no_height, map, WorldRect{0, 0, 4095, 1010});
  CHECK(limited.grid().cell(63, 62) == 1);
  CHECK(limited.grid().cell(63, 63) == 0);
  // A mask that is not a 16-unit one-bit grid stamps nothing.
  OwnedGrid wrong = grid8(64, 4, 1);
  OwnedGrid untouched = pass_for(256);
  stamp_footprint(untouched.grid(), PassMask{&wrong.grid(), CellRect{0, 0, 3, 3}}, {1000, 1000}, no_height, map, map);
  CHECK(untouched.grid().count_set() == 0);
}

TEST(editor_stamp_fills_the_column_a_slope_pulls_apart) {
  // A solid column of nine rows, on ground that rises steeply with y: the
  // inverse projection spreads consecutive rows more than a cell apart, and
  // the stamp fills between them so the footprint stays solid.
  OwnedGrid mask = blank_mask();
  for (std::uint32_t row = 60; row <= 68; ++row) (void)mask.grid().set_cell(64, row, 1);
  OwnedGrid terrain = grid8(64, 64, 3);
  auto height = OwnedGrid::create(32, 8, 4096, 4096);
  REQUIRE(height.ok());
  for (std::uint32_t y = 0; y < 128; ++y) {
    for (std::uint32_t x = 0; x < 128; ++x) (void)height->grid().set_cell(x, y, std::min(255u, y * 4));
  }
  OwnedGrid pass = pass_for(256);
  const WorldRect map = WorldRect::of_map(terrain.grid());
  stamp_footprint(pass.grid(), PassMask{&mask.grid(), mask_bounds(mask.grid())}, {1000, 1500}, height->grid(), map, map);
  // Whatever the spread, the set cells of the column are contiguous.
  std::int32_t first = -1;
  std::int32_t last = -1;
  for (std::uint32_t y = 0; y < 256; ++y) {
    if (pass.grid().cell(63, y) == 0) continue;
    if (first < 0) first = static_cast<std::int32_t>(y);
    last = static_cast<std::int32_t>(y);
  }
  REQUIRE(first >= 0);
  CHECK(static_cast<std::size_t>(last - first + 1) == set_in_column(pass.grid(), 63));
  // And the slope did spread them: nine rows on flat ground cover eight or
  // nine cells; here more.
  CHECK(last - first + 1 > 9);
}

TEST(editor_terrain_bake_blocks_rock_and_shores_deep_water) {
  // A 16-cell map: rock at (4, 4); a deep lake at 8..11 x 8..11 with a
  // shallow ring around it, and grass elsewhere.
  OwnedGrid terrain = grid8(64, 16, 3);
  (void)terrain.grid().set_cell(4, 4, 6);
  for (std::uint32_t y = 7; y <= 12; ++y) {
    for (std::uint32_t x = 7; x <= 12; ++x) (void)terrain.grid().set_cell(x, y, 12);
  }
  for (std::uint32_t y = 8; y <= 11; ++y) {
    for (std::uint32_t x = 8; x <= 11; ++x) (void)terrain.grid().set_cell(x, y, 13);
  }
  OwnedGrid pass = pass_for(64);
  const WorldRect map = WorldRect::of_map(terrain.grid());
  bake_terrain_passability(pass.grid(), terrain.grid(), table(), map, map);
  // The rock: the four-by-four window sampling cell 4 is pass cells 14..17.
  CHECK(pass.grid().cell(14, 14) == 1);
  CHECK(pass.grid().cell(17, 17) == 1);
  CHECK(pass.grid().cell(13, 14) == 0);
  CHECK(pass.grid().cell(18, 17) == 0);
  // Deep water's interior is free; its rim against the shallow ring is
  // blocked: cell 8's window is pass cells 30..33, and 30 is the rim.
  CHECK(pass.grid().cell(30, 36) == 1);   // left rim, mid-lake row
  CHECK(pass.grid().cell(31, 36) == 0);   // inside
  CHECK(pass.grid().cell(36, 36) == 0);   // the middle of the lake
  CHECK(pass.grid().cell(45, 36) == 1);   // right rim: cell 11's window is 42..45
  CHECK(pass.grid().cell(36, 30) == 1);   // top rim
  CHECK(pass.grid().cell(30, 30) == 1);   // a corner: the diagonal too
  // The shallow cell beside the lake carries the shore: its pass cell next
  // to a deep-sampling one is blocked, the one further out is not.
  CHECK(pass.grid().cell(29, 36) == 1);
  CHECK(pass.grid().cell(28, 36) == 0);
  // The last two columns and rows are never written by the bake.
  OwnedGrid rock = grid8(64, 16, 6);
  OwnedGrid edge = pass_for(64);
  bake_terrain_passability(edge.grid(), rock.grid(), table(), map, map);
  CHECK(edge.grid().cell(61, 61) == 1);
  CHECK(edge.grid().cell(62, 30) == 0);
  CHECK(edge.grid().cell(30, 63) == 0);
}

TEST(editor_frame_blocks_the_edges_and_the_bottom_strip) {
  OwnedGrid terrain = grid8(64, 256, 3);  // a 16384 map, flat
  OwnedGrid pass = pass_for(1024);
  Grid no_height;
  frame_passability(pass.grid(), no_height, WorldRect::of_map(terrain.grid()));
  // Two columns either side on every row.
  CHECK(pass.grid().cell(0, 500) == 1 && pass.grid().cell(1, 500) == 1);
  CHECK(pass.grid().cell(1022, 500) == 1 && pass.grid().cell(1023, 500) == 1);
  CHECK(pass.grid().cell(2, 500) == 0 && pass.grid().cell(1021, 500) == 0);
  // Four rows at the top on flat ground, and the strip from row 996 down.
  CHECK(pass.grid().cell(500, 3) == 1 && pass.grid().cell(500, 4) == 0);
  CHECK(pass.grid().cell(500, 995) == 0 && pass.grid().cell(500, 996) == 1);
  CHECK(pass.grid().cell(500, 1023) == 1);
}

TEST(editor_rebuild_puts_objects_and_decorations_back) {
  OwnedGrid terrain = grid8(64, 64, 3);
  OwnedGrid pass = pass_for(256);
  Grid no_height;
  // A tree of a kind with a one-cell mask at the mask's centre, and a
  // building with a two-cell one.
  OwnedGrid tree = blank_mask();
  (void)tree.grid().set_cell(64, 64, 1);
  OwnedGrid house = blank_mask();
  (void)house.grid().set_cell(64, 64, 1);
  (void)house.grid().set_cell(65, 64, 1);
  auto decor = OwnedGrid::create(64, 16, 4096, 4096);
  REQUIRE(decor.ok());
  (void)decor->grid().set_cell(10, 10, 5u | (4u << 8) | (8u << 12));  // kind 5 at (656, 672)
  const PassMask tree_mask{&tree.grid(), mask_bounds(tree.grid())};
  const PassMask house_mask{&house.grid(), mask_bounds(house.grid())};
  std::vector<const PassMask*> kinds(256, nullptr);
  kinds[5] = &tree_mask;
  std::vector<Footprint> objects{{&house_mask, {2000, 2000}}, {&house_mask, {3900, 3900}}};
  const WorldRect whole = WorldRect::of_map(terrain.grid());
  rebuild_passability(pass.grid(), whole, terrain.grid(), table(), no_height, objects, decor->grid(), kinds);
  // The decoration: (656, 672) -> x 664, y 672 + 1032 - 1024 = 680 -> cell (41, 42).
  CHECK(decor_position(10, 10, decor->grid().cell(10, 10)) == (sim::Point{656, 672}));
  CHECK(pass.grid().cell(41, 42) == 1);
  // The house at (2000, 2000): x 2008 and 2024, y 2000 + 8 -> cells (125, 125), (126, 125).
  CHECK(pass.grid().cell(125, 125) == 1);
  CHECK(pass.grid().cell(126, 125) == 1);
  CHECK(pass.grid().cell(127, 125) == 0);
  const std::size_t all = pass.grid().count_set();
  // A rebuild over a rectangle around the house with the house gone clears
  // it and leaves the tree and the far house alone; the frame comes back.
  std::vector<Footprint> without{{&house_mask, {3900, 3900}}};
  rebuild_passability(pass.grid(), WorldRect{1900, 1900, 2100, 2100}, terrain.grid(), table(), no_height,
                      without, decor->grid(), kinds);
  CHECK(pass.grid().cell(125, 125) == 0);
  CHECK(pass.grid().cell(41, 42) == 1);
  CHECK(pass.grid().count_set() == all - 2);
  // The rectangles the tools hand the rebuild.
  const WorldRect stroke = terrain_stroke_rect(10, 10, 1);
  CHECK(stroke.x0 == 640 - 64 - 127 && stroke.x1 == 640 + 64 + 128);
  CHECK(stroke.y0 == stroke.x0 && stroke.y1 == stroke.x1);
  const WorldRect leaf = water_leaf_rect(10, 10, 0);
  CHECK(leaf.x0 == 640 - 63 && leaf.x1 == 640 + 64);
  const WorldRect inverted{10, 10, 5, 20};
  CHECK(inverted.empty());
  const WorldRect wide{-10, -10, 5000, 20};
  const WorldRect cut{0, 0, 4095, 20};
  CHECK(wide.clamped(whole) == cut);
}

TEST(editor_water_leaf_levels_the_ground_and_clears_the_decorations) {
  auto height = OwnedGrid::create(32, 8, 4096, 4096);
  REQUIRE(height.ok());
  (void)height->grid().fill(100);
  auto decor = OwnedGrid::create(64, 16, 4096, 4096);
  REQUIRE(decor.ok());
  (void)decor->grid().set_cell(10, 10, 5);
  (void)decor->grid().set_cell(20, 20, 5);
  const WorldRect square = water_leaf_rect(10, 10, 1);  // 640 +- 127
  const CellRect levelled = level_height(height->grid(), square, 0);
  CHECK(!levelled.empty());
  CHECK(height->grid().cell(20, 20) == 0);   // 640 / 32
  CHECK(height->grid().cell(16, 20) == 0);   // 513 / 32
  CHECK(height->grid().cell(15, 20) == 100); // 480 is outside 513..768
  CHECK(height->grid().cell(24, 20) == 0);   // 768 is the far edge, inside
  CHECK(height->grid().cell(25, 20) == 100); // 800 is outside
  const CellRect cleared = clear_decor_in(decor->grid(), square);
  CHECK(cleared.x0 == 10 && cleared.x1 == 10);
  CHECK(decor->grid().cell(10, 10) == 0);
  CHECK(decor->grid().cell(20, 20) == 5);
  CHECK(clear_decor_in(decor->grid(), square).empty());
}

TEST(editor_mask_library_keeps_what_it_read_and_refuses_the_rest) {
  PassMaskLibrary library;
  OwnedGrid mask = blank_mask();
  (void)mask.grid().set_cell(64, 64, 1);
  const PassMask* kept = library.load("BUILDINGS\\A\\A.PASS", mask.bytes());
  REQUIRE(kept != nullptr);
  CHECK(kept->grid->blocked(64, 64));
  CHECK(kept->bounds.x0 == 64 && kept->bounds.y1 == 64);
  CHECK(library.find("BUILDINGS\\A\\A.PASS") == kept);
  CHECK(library.known("BUILDINGS\\A\\A.PASS"));
  CHECK(!library.known("BUILDINGS\\B\\B.PASS"));
  // A byte layer is not a mask; the path is remembered as having none.
  OwnedGrid bytes = grid8(64, 4, 1);
  CHECK(library.load("X\\TERRAIN.GRID", bytes.bytes()) == nullptr);
  CHECK(library.known("X\\TERRAIN.GRID"));
  CHECK(library.find("X\\TERRAIN.GRID") == nullptr);
  CHECK(library.size() == 2);
  // Loading a known path again answers what it has.
  CHECK(library.load("BUILDINGS\\A\\A.PASS", bytes.bytes()) == kept);
}

TEST(editor_undo_takes_back_a_stroke_and_redo_puts_it_again) {
  OwnedGrid terrain = grid8(64, 16, 3);
  auto height = OwnedGrid::create(32, 8, 1024, 1024);
  REQUIRE(height.ok());
  (void)height->grid().fill(10);
  auto decor = OwnedGrid::create(64, 16, 1024, 1024);
  REQUIRE(decor.ok());
  OwnedGrid pass = pass_for(64);
  UndoStack::Layers layers{&terrain.grid(), &height->grid(), &decor->grid(), &pass.grid()};
  UndoStack undo;
  CHECK(!undo.can_undo() && !undo.can_redo());

  // A stroke of two steps: press, then drag, each snapshotting before it
  // paints; the second overlaps the first.
  undo.snapshot(layers, undo_rect({256, 256}, 128), UndoStack::kTerrain | UndoStack::kPass, true);
  (void)terrain.grid().set_cell(4, 4, 6);
  (void)pass.grid().set_cell(16, 16, 1);
  undo.snapshot(layers, undo_rect({320, 256}, 128), UndoStack::kTerrain | UndoStack::kPass, false);
  (void)terrain.grid().set_cell(5, 4, 6);
  (void)terrain.grid().set_cell(4, 4, 8);  // the second step paints over the first
  CHECK(undo.records() == 2);
  CHECK(undo.can_undo() && !undo.can_redo());

  // One undo takes the whole stroke back, and reports where.
  const WorldRect taken = undo.undo(layers);
  CHECK(!taken.empty());
  CHECK(taken.x0 == 128 && taken.x1 == 448);
  CHECK(terrain.grid().cell(4, 4) == 3);
  CHECK(terrain.grid().cell(5, 4) == 3);
  CHECK(pass.grid().cell(16, 16) == 0);
  CHECK(!undo.can_undo() && undo.can_redo());
  CHECK(undo.undo(layers).empty());

  // Redo puts it all back, last step last.
  CHECK(!undo.redo(layers).empty());
  CHECK(terrain.grid().cell(4, 4) == 8);
  CHECK(terrain.grid().cell(5, 4) == 6);
  CHECK(pass.grid().cell(16, 16) == 1);
  CHECK(undo.can_undo() && !undo.can_redo());
  // And undo again, then a fresh stroke drops the redo list.
  (void)undo.undo(layers);
  CHECK(undo.can_redo());
  undo.snapshot(layers, undo_rect({700, 700}, 64), UndoStack::kHeight, true);
  CHECK(!undo.can_redo());
  (void)height->grid().set_cell(22, 22, 200);
  (void)undo.undo(layers);
  CHECK(height->grid().cell(22, 22) == 10);

  // Layers a record does not carry are left alone by its undo: the height
  // record put back the height, not the terrain painted meanwhile.
  undo.snapshot(layers, undo_rect({700, 700}, 64), UndoStack::kHeight, true);
  (void)terrain.grid().set_cell(11, 11, 6);
  (void)undo.undo(layers);
  CHECK(terrain.grid().cell(11, 11) == 6);
  undo.clear();
  CHECK(!undo.can_undo() && undo.bytes() == 0);
}

TEST(editor_undo_drops_the_oldest_records_past_its_cap) {
  // A 16,384-unit map's passability is 1,024 x 1,024 cells; a record of the
  // whole layer is 4 MB of cells here, so two of them pass the cap and the
  // first goes.
  OwnedGrid pass = pass_for(1024);
  UndoStack::Layers layers{nullptr, nullptr, nullptr, &pass.grid()};
  UndoStack undo;
  const WorldRect whole{0, 0, 16383, 16383};
  undo.snapshot(layers, whole, UndoStack::kPass, true);
  CHECK(undo.records() == 1);
  CHECK(undo.bytes() == 1024u * 1024u * sizeof(std::uint32_t));
  undo.snapshot(layers, whole, UndoStack::kPass, true);
  CHECK(undo.records() == 1);  // the older one evicted, the newest kept
  CHECK(undo.bytes() <= UndoStack::kByteCap);
}
