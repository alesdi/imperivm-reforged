#include "imperivm/core/world/zoom_ground.hpp"

#include <algorithm>
#include <vector>

namespace imperivm::core {

namespace {

/// The light levels the lookup covers, and the one that leaves a channel as
/// it is (`terrain_light_channel`).
constexpr std::int32_t kLightLevels = 32;
constexpr std::int32_t kNeutralLight = 16;

const ui::Image* usable(const ui::Image* image) {
  if (image == nullptr || image->empty()) return nullptr;
  if (image->rgba.size() < static_cast<std::size_t>(image->width) * image->height * 4) return nullptr;
  return image;
}

}  // namespace

TerrainTile zoom_terrain_tile(const Grid& terrain, std::int32_t cx, std::int32_t cy) {
  TerrainTile tile;
  if (terrain.cell_size() == 0) return tile;
  const auto width = static_cast<std::int32_t>(terrain.width());
  const auto height = static_cast<std::int32_t>(terrain.height());
  if (width <= 0 || height <= 0) return tile;

  const auto sample = [&](std::int32_t x, std::int32_t y) -> std::int32_t {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return static_cast<std::int32_t>(
        terrain.cell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)));
  };
  tile.corners = {sample(cx, cy), sample(cx + 1, cy), sample(cx + 1, cy + 1), sample(cx, cy + 1)};
  static constexpr std::array<std::uint8_t, 4> kBits{kCornerTopLeft, kCornerTopRight,
                                                     kCornerBottomRight, kCornerBottomLeft};

  // The scan over 0..255 (0x00618210, 0x0061824e) finds the corners' types
  // in ascending order; the first is the base, each after it an overlay.
  std::array<std::int32_t, 4> present = tile.corners;
  std::sort(present.begin(), present.end());
  const auto end = std::unique(present.begin(), present.end());
  tile.base = present[0];
  for (auto it = present.begin() + 1; it != end; ++it) {
    std::uint8_t bits = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      if (tile.corners[i] == *it) bits = static_cast<std::uint8_t>(bits | kBits[i]);
    }
    tile.overlays[tile.overlay_count++] = TerrainTileLayer{*it, bits};
  }
  return tile;
}

ZoomGroundStats compose_zoom_ground(const Grid& terrain, const Grid& light,
                                    std::uint32_t divisor, const ZoomGroundArt& art,
                                    ui::Image& out, std::span<const bool> counted) {
  ZoomGroundStats stats;
  out = ui::Image{};
  if (divisor == 0 || terrain.cell_size() == 0) return stats;
  const auto cells = static_cast<std::int32_t>(terrain.width());
  if (cells <= 0) return stats;

  // The picture, sized as the zoom map places things on it.
  const std::int32_t cell_w = std::max<std::int32_t>(1, kTileWidth / static_cast<std::int32_t>(divisor));
  out.width = static_cast<std::uint32_t>(cells * cell_w);
  out.height = static_cast<std::uint32_t>(
      world_to_screen_y(cells * kTileWidth) / static_cast<std::int32_t>(divisor));
  out.rgba.assign(static_cast<std::size_t>(out.width) * out.height * 4u, 0);
  if (out.width == 0 || out.height == 0) return stats;

  // Composed at twice the picture's scale -- the original builds the zoom
  // at level `L - 1` and halves it (0x0061710e, 0x00617188) -- unless the
  // divisor is odd or one, when there is no half to compose at.
  const std::int32_t factor = divisor >= 2 && divisor % 2 == 0 ? 2 : 1;
  const std::int32_t scale = static_cast<std::int32_t>(divisor) / factor;
  const std::int32_t big_cell_w = cell_w * factor;
  const std::int32_t big_w = static_cast<std::int32_t>(out.width) * factor;
  const std::int32_t big_h = static_cast<std::int32_t>(out.height) * factor;

  // Each composed row's cell row, and each cell row's first composed row:
  // the rows whose world point projects into the cell.
  std::vector<std::int32_t> row_start(static_cast<std::size_t>(cells) + 1, big_h);
  for (std::int32_t py = 0; py < big_h; ++py) {
    const std::int32_t world_y = screen_to_world_y(py * scale);
    const std::int32_t cy = std::max(0, world_y) / kTileWidth;
    if (cy < cells && row_start[static_cast<std::size_t>(cy)] == big_h) row_start[static_cast<std::size_t>(cy)] = py;
  }
  for (std::int32_t cy = cells; cy-- > 0;) {
    if (row_start[static_cast<std::size_t>(cy)] == big_h) {
      row_start[static_cast<std::size_t>(cy)] = row_start[static_cast<std::size_t>(cy) + 1];
    }
  }

  const auto tile_of = [&](std::int32_t type) -> const ui::Image* {
    if (type < 0 || static_cast<std::size_t>(type) >= art.tiles.size()) return nullptr;
    return usable(art.tiles[static_cast<std::size_t>(type)]);
  };
  const auto is_counted = [&](std::int32_t type) {
    return type >= 0 && static_cast<std::size_t>(type) < counted.size() && counted[static_cast<std::size_t>(type)];
  };

  // The composed picture, RGB, and how much of each pixel is a counted
  // layer, out of 256.
  std::vector<std::uint8_t> big(static_cast<std::size_t>(big_w) * static_cast<std::size_t>(big_h) * 3, 0);
  std::vector<std::uint16_t> share(counted.empty() ? 0 : static_cast<std::size_t>(big_w) * static_cast<std::size_t>(big_h), 0);

  for (std::int32_t cy = 0; cy < cells; ++cy) {
    const std::int32_t y0 = row_start[static_cast<std::size_t>(cy)];
    const std::int32_t y1 = row_start[static_cast<std::size_t>(cy) + 1];
    if (y0 >= y1) continue;
    for (std::int32_t cx = 0; cx < cells; ++cx) {
      const TerrainTile tile = zoom_terrain_tile(terrain, cx, cy);
      const std::int32_t x0 = cx * big_cell_w;
      // The base, whole: the tile sampled at the picture's own pixel, so a
      // layer is one continuous field across cells (0x00617990).
      if (const ui::Image* base = tile_of(tile.base); base != nullptr) {
        const std::uint16_t whole = is_counted(tile.base) ? 256 : 0;
        for (std::int32_t py = y0; py < y1; ++py) {
          const std::uint32_t v = static_cast<std::uint32_t>(py) % base->height;
          for (std::int32_t px = x0; px < x0 + big_cell_w; ++px) {
            const std::uint8_t* source = base->pixel(static_cast<std::uint32_t>(px) % base->width, v);
            std::uint8_t* target = big.data() + (static_cast<std::size_t>(py) * big_w + px) * 3;
            target[0] = source[0];
            target[1] = source[1];
            target[2] = source[2];
            if (!share.empty()) share[static_cast<std::size_t>(py) * big_w + px] = whole;
          }
        }
      }
      // Each overlay through its mask: C on even cell rows, D on odd, the
      // file naming the corners the overlay does not hold (0x00617a50).
      for (std::size_t i = 0; i < tile.overlay_count; ++i) {
        const TerrainTileLayer& layer = tile.overlays[i];
        const ui::Image* over = tile_of(layer.type);
        if (over == nullptr) continue;
        const std::size_t mask_index = static_cast<std::size_t>(cy & 1) * 16 + transition_mask_code(layer.corners);
        const ui::Image* mask = usable(art.masks[mask_index]);
        if (mask == nullptr) continue;
        const std::uint16_t whole = is_counted(layer.type) ? 256 : 0;
        for (std::int32_t py = y0; py < y1; ++py) {
          const std::uint32_t v = static_cast<std::uint32_t>(py) % over->height;
          const auto mask_y = std::min<std::uint32_t>(static_cast<std::uint32_t>(py - y0), mask->height - 1);
          for (std::int32_t px = x0; px < x0 + big_cell_w; ++px) {
            const auto mask_x = std::min<std::uint32_t>(static_cast<std::uint32_t>(px - x0), mask->width - 1);
            const std::uint32_t a = mask->pixel(mask_x, mask_y)[0];
            if (a == 0) continue;
            const std::uint8_t* source = over->pixel(static_cast<std::uint32_t>(px) % over->width, v);
            std::uint8_t* target = big.data() + (static_cast<std::size_t>(py) * big_w + px) * 3;
            for (int c = 0; c < 3; ++c) {
              target[c] = static_cast<std::uint8_t>((source[c] * a + target[c] * (256 - a)) >> 8);
            }
            if (!share.empty()) {
              std::uint16_t& s = share[static_cast<std::size_t>(py) * big_w + px];
              s = static_cast<std::uint16_t>((whole * a + s * (256 - a)) >> 8);
            }
          }
        }
      }
    }
  }

  // The light: the level at the 32-unit vertices, bilinear to the pixel's
  // world point, its integer part choosing the gain (0x00617bd0).
  const std::int32_t spacing = static_cast<std::int32_t>(light.cell_size());
  if (spacing > 0 && light.width() > 0 && light.height() > 0) {
    const auto lw = static_cast<std::int32_t>(light.width());
    const auto lh = static_cast<std::int32_t>(light.height());
    const auto level_at = [&](std::int32_t lx, std::int32_t ly) -> std::int32_t {
      lx = std::clamp(lx, 0, lw - 1);
      ly = std::clamp(ly, 0, lh - 1);
      return std::min<std::int32_t>(
          static_cast<std::int32_t>(light.cell(static_cast<std::uint32_t>(lx), static_cast<std::uint32_t>(ly))),
          kLightLevels - 1);
    };
    for (std::int32_t py = 0; py < big_h; ++py) {
      const std::int32_t world_y = std::max(0, screen_to_world_y(py * scale));
      const std::int32_t ly = world_y / spacing;
      const std::int32_t fy = world_y % spacing;
      for (std::int32_t px = 0; px < big_w; ++px) {
        const std::int32_t world_x = px * scale;
        const std::int32_t lx = world_x / spacing;
        const std::int32_t fx = world_x % spacing;
        const std::int32_t top = level_at(lx, ly) * (spacing - fx) + level_at(lx + 1, ly) * fx;
        const std::int32_t bottom = level_at(lx, ly + 1) * (spacing - fx) + level_at(lx + 1, ly + 1) * fx;
        const std::int32_t level = (top * (spacing - fy) + bottom * fy) / (spacing * spacing);
        if (level == kNeutralLight) continue;
        std::uint8_t* target = big.data() + (static_cast<std::size_t>(py) * big_w + px) * 3;
        for (int c = 0; c < 3; ++c) {
          target[c] = static_cast<std::uint8_t>(terrain_light_channel(target[c], level));
        }
      }
    }
  }

  // Halved: each pixel the mean of its block (0x00617820).
  const auto count = static_cast<std::uint32_t>(factor * factor);
  for (std::uint32_t y = 0; y < out.height; ++y) {
    for (std::uint32_t x = 0; x < out.width; ++x) {
      std::uint32_t sum[3] = {0, 0, 0};
      std::uint32_t shared = 0;
      for (std::int32_t dy = 0; dy < factor; ++dy) {
        for (std::int32_t dx = 0; dx < factor; ++dx) {
          const std::size_t at = static_cast<std::size_t>(static_cast<std::int32_t>(y) * factor + dy) * big_w +
                                 static_cast<std::size_t>(static_cast<std::int32_t>(x) * factor + dx);
          sum[0] += big[at * 3 + 0];
          sum[1] += big[at * 3 + 1];
          sum[2] += big[at * 3 + 2];
          if (!share.empty()) shared += share[at];
        }
      }
      std::uint8_t* pixel = out.pixel(x, y);
      pixel[0] = static_cast<std::uint8_t>(sum[0] / count);
      pixel[1] = static_cast<std::uint8_t>(sum[1] / count);
      pixel[2] = static_cast<std::uint8_t>(sum[2] / count);
      pixel[3] = 255;
      if (!share.empty() && shared * 2 >= count * 256) ++stats.counted_pixels;
    }
  }
  return stats;
}

}  // namespace imperivm::core
