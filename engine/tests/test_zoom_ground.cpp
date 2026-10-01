// The zoom map's ground (`core::compose_zoom_ground`), on synthetic cells.
//
// Playtest report #20: the roads between towns did not show on the zoom map.
// It coloured a cell by one texture -- the base, replaced only by a layer
// holding all four corners -- and a road one vertex wide holds two corners of
// the cells either side of it, never four, so it was never drawn. The
// original composes the zoom map as it composes the ground (0x006180c0):
// every layer at a corner, through its mask. These pin that rule down with
// flat colours and masks whose answer can be worked out by hand.
//
// Built from bytes, as every core test is: no installation, no assets.

#include <array>
#include <cstdint>
#include <vector>

#include "builder.hpp"
#include "imperivm/core/world/map.hpp"
#include "imperivm/core/world/zoom_ground.hpp"
#include "test.hpp"

using namespace imperivm::core;
using imperivm::test::Builder;

namespace {

constexpr std::int32_t kGrass = 3;
constexpr std::int32_t kRoad = 7;

/// An 8-bit grid of `size` x `size` cells of `cell` world units each.
Builder grid_of(std::uint32_t size, std::uint32_t cell, const std::vector<std::uint8_t>& cells) {
  Builder grid;
  grid.text("DIRG").u32(cell).u32(8).u32(size * cell).u32(size * cell);
  for (const std::uint8_t value : cells) grid.u8(value);
  return grid;
}

ui::Image solid(std::uint32_t width, std::uint32_t height, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  ui::Image image;
  image.width = width;
  image.height = height;
  image.rgba.reserve(static_cast<std::size_t>(width) * height * 4);
  for (std::uint32_t i = 0; i < width * height; ++i) {
    image.rgba.insert(image.rgba.end(), {r, g, b, 255});
  }
  return image;
}

/// A mask the size of a zoom-16 cell at twice the scale (8 x 6), opaque
/// wherever the pixel's nearest corner is one the overlay holds -- the
/// corners *not* named by `code`.
ui::Image quadrant_mask(std::uint8_t code) {
  ui::Image image = solid(8, 6, 0, 0, 0);
  for (std::uint32_t y = 0; y < 6; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) {
      const bool left = x < 4;
      const bool top = y < 3;
      const std::uint8_t corner = top ? (left ? kCornerTopLeft : kCornerTopRight)
                                      : (left ? kCornerBottomLeft : kCornerBottomRight);
      const std::uint8_t alpha = (code & corner) != 0 ? 0 : 255;
      std::uint8_t* pixel = image.pixel(x, y);
      pixel[0] = pixel[1] = pixel[2] = alpha;
    }
  }
  return image;
}

struct Art {
  std::vector<ui::Image> tiles = std::vector<ui::Image>(16);
  std::vector<const ui::Image*> tile_of = std::vector<const ui::Image*>(16, nullptr);
  std::array<ui::Image, 32> masks;
  ZoomGroundArt art;

  Art() {
    tiles[kGrass] = solid(16, 16, 0, 160, 0);
    tiles[kRoad] = solid(16, 16, 200, 120, 40);
    tile_of[kGrass] = &tiles[kGrass];
    tile_of[kRoad] = &tiles[kRoad];
    art.tiles = tile_of;
    for (std::size_t row = 0; row < 2; ++row) {
      for (std::uint8_t code = 1; code < 15; ++code) {
        masks[row * 16 + code] = quadrant_mask(code);
        art.masks[row * 16 + code] = &masks[row * 16 + code];
      }
    }
  }
};

/// Grass everywhere but a road one vertex wide down column `road_x`.
std::vector<std::uint8_t> road_column(std::uint32_t size, std::uint32_t road_x) {
  std::vector<std::uint8_t> cells(static_cast<std::size_t>(size) * size, kGrass);
  for (std::uint32_t y = 0; y < size; ++y) cells[y * size + road_x] = kRoad;
  return cells;
}

}  // namespace

TEST(the_zoom_tile_orders_its_layers_by_number_alone) {
  // The ground lifts the waters above every land layer (`terrain_tile`); the
  // zoom map's scan does not (0x006180c0): shallow water is the base under a
  // road here, and the road is drawn over it.
  const Builder bytes = grid_of(2, 64, {kShallowWaterLayer, 20, 20, 20});
  const auto terrain = Grid::parse(bytes.span());
  REQUIRE(terrain.ok());
  const TerrainTile zoom = zoom_terrain_tile(terrain.value(), 0, 0);
  CHECK(zoom.base == kShallowWaterLayer);
  REQUIRE(zoom.overlay_count == 1);
  CHECK(zoom.overlays[0].type == 20);
  CHECK(zoom.overlays[0].corners == (kCornerTopRight | kCornerBottomRight | kCornerBottomLeft));
  CHECK(terrain_tile(terrain.value(), 0, 0).base == 20);
}

TEST(the_zoom_tile_keeps_every_layer_at_a_corner_in_ascending_order) {
  const Builder bytes = grid_of(2, 64, {20, kDeepWaterLayer, kGrass, kRoad});
  const auto terrain = Grid::parse(bytes.span());
  REQUIRE(terrain.ok());
  // Corners TL TR BR BL: 20, 13, 7 (clamped from (1,1)), 3.
  const TerrainTile zoom = zoom_terrain_tile(terrain.value(), 0, 0);
  CHECK(zoom.base == kGrass);
  REQUIRE(zoom.overlay_count == 3);
  CHECK(zoom.overlays[0].type == kRoad);
  CHECK(zoom.overlays[1].type == kDeepWaterLayer);
  CHECK(zoom.overlays[2].type == 20);
}

TEST(a_road_one_vertex_wide_is_drawn_on_the_zoom_map) {
  // Eight cells square at a sixteenth: 32 x 23 pixels, composed at 64 x 46.
  // The road down vertex column 4 holds the right-hand corners of cell
  // column 3 and the left-hand ones of column 4 -- never all four, so the
  // old rule drew no road at all. Through the masks it is composed columns
  // 28..35 wide, pixel columns 14..17 once halved.
  const Builder bytes = grid_of(8, 64, road_column(8, 4));
  const auto terrain = Grid::parse(bytes.span());
  REQUIRE(terrain.ok());
  Art art;
  std::array<bool, 16> counted{};
  counted[kRoad] = true;

  ui::Image out;
  const ZoomGroundStats stats = compose_zoom_ground(terrain.value(), Grid{}, 16, art.art, out, counted);
  REQUIRE(out.width == 32);
  REQUIRE(out.height == static_cast<std::uint32_t>(world_to_screen_y(8 * 64) / 16));
  for (std::uint32_t y = 0; y < out.height; ++y) {
    for (std::uint32_t x = 0; x < out.width; ++x) {
      const std::uint8_t* pixel = out.pixel(x, y);
      // An opaque mask pixel is 255 of 256, so the road keeps a 256th of
      // the grass under it: red 200 * 255 >> 8.
      const bool road = x >= 14 && x <= 17;
      CHECK(pixel[0] == (road ? (200 * 255) >> 8 : 0));
      CHECK(pixel[1] == (road ? (120 * 255 + 160) >> 8 : 160));
      CHECK(pixel[3] == 255);
    }
  }
  // Four columns of road, the picture's height tall.
  CHECK(stats.counted_pixels == 4 * out.height);
}

TEST(an_overlay_blends_by_its_mask_over_256) {
  // One overlay pixel at half opacity: `(over * a + under * (256 - a)) >> 8`
  // (0x00617a50), so a mask of 128 over 0 and 200 gives 100 exactly, and the
  // 2 x 2 mean carries it into the picture unchanged when the block is
  // uniform.
  const Builder bytes = grid_of(2, 64, {kGrass, kRoad, kGrass, kGrass});
  const auto terrain = Grid::parse(bytes.span());
  REQUIRE(terrain.ok());
  Art art;
  // The road holds the top-right corner of cell (0, 0): the mask whose code
  // names the other three.
  const std::uint8_t code = transition_mask_code(kCornerTopRight);
  art.masks[code] = solid(8, 6, 128, 128, 128);
  art.art.masks[code] = &art.masks[code];
  ui::Image out;
  (void)compose_zoom_ground(terrain.value(), Grid{}, 16, art.art, out);
  const std::uint8_t* pixel = out.pixel(0, 0);
  CHECK(pixel[0] == 100);
  CHECK(pixel[1] == (120 * 128 + 160 * 128) / 256);
  CHECK(pixel[2] == 20);
}

TEST(the_zoom_map_is_lit_by_the_baked_light) {
  // Level 6 everywhere is a gain of (6 + 4) / 20, a half: the grass's 160
  // green is 80. Level 16 leaves it alone.
  const Builder bytes = grid_of(4, 64, std::vector<std::uint8_t>(16, kGrass));
  const auto terrain = Grid::parse(bytes.span());
  REQUIRE(terrain.ok());
  const Builder dim = grid_of(8, 32, std::vector<std::uint8_t>(64, 6));
  const auto light = Grid::parse(dim.span());
  REQUIRE(light.ok());
  Art art;
  ui::Image out;
  (void)compose_zoom_ground(terrain.value(), light.value(), 16, art.art, out);
  REQUIRE(!out.empty());
  CHECK(out.pixel(5, 5)[1] == 80);

  const Builder neutral = grid_of(8, 32, std::vector<std::uint8_t>(64, 16));
  const auto flat = Grid::parse(neutral.span());
  REQUIRE(flat.ok());
  (void)compose_zoom_ground(terrain.value(), flat.value(), 16, art.art, out);
  CHECK(out.pixel(5, 5)[1] == 160);
}

TEST(a_layer_with_no_minimap_tile_is_left_undrawn) {
  // The original's draws return early on a layer with no picture; the
  // shipped waves layer (10) names a tile that does not ship.
  const Builder bytes = grid_of(2, 64, std::vector<std::uint8_t>(4, 10));
  const auto terrain = Grid::parse(bytes.span());
  REQUIRE(terrain.ok());
  Art art;
  ui::Image out;
  (void)compose_zoom_ground(terrain.value(), Grid{}, 16, art.art, out);
  REQUIRE(!out.empty());
  CHECK(out.pixel(0, 0)[0] == 0);
  CHECK(out.pixel(0, 0)[1] == 0);
  CHECK(out.pixel(0, 0)[3] == 255);
}
