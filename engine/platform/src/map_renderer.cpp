#include "imperivm/platform/map_renderer.hpp"

#include "imperivm/core/sim/flying.hpp"
#include "imperivm/core/world/zoom_ground.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace imperivm::platform {
namespace {

namespace core = imperivm::core;

constexpr std::int32_t kCellWidth = core::kTileWidth;    // 64
constexpr std::int32_t kCellHeight = core::kTileHeight;  // 46

/// How far outside the viewport an object's origin may sit and still be drawn.
/// A tower sprite is 230 px tall and hangs a long way above its origin, so a
/// tight cull pops the tops of buildings in at the top edge.
constexpr std::int32_t kSpriteMargin = 512;

/// The shadow pass's opacity. `DATA\CONST.INI` records `XRayTranslucency` for
/// a different effect and nothing at all for this one, so it is chosen to
/// match the reference render rather than recovered.
constexpr float kShadowAlpha = 110.0F / 255.0F;

/// The light lookup the ground warp reads per pixel: `kLightLevels` levels by
/// 256 channel values, built from `core::terrain_light_channel` -- the table
/// the original builds at 0x0061e210, over 8-bit channels instead of 5-bit.
constexpr std::int32_t kLightLevels = 32;
constexpr std::int32_t kNeutralLight = 16;

const std::array<std::uint8_t, kLightLevels * 256>& light_table() {
  static const std::array<std::uint8_t, kLightLevels * 256> table = [] {
    std::array<std::uint8_t, kLightLevels * 256> out{};
    for (std::int32_t level = 0; level < kLightLevels; ++level) {
      for (std::int32_t channel = 0; channel < 256; ++channel) {
        out[static_cast<std::size_t>(level * 256 + channel)] =
            static_cast<std::uint8_t>(core::terrain_light_channel(channel, level));
      }
    }
    return out;
  }();
  return table;
}

std::uint64_t cell_key(std::int32_t cx, std::int32_t cy) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cy)) << 32) |
         static_cast<std::uint32_t>(cx);
}

/// Decode an 8-bit Windows BMP, which is what the 56 terrain transition masks
/// are. Nothing else in the engine ships as a BMP that a renderer must read, so
/// this handles the one variant they use -- 8 bits, no compression, bottom-up --
/// and refuses everything else rather than growing a general decoder.
bool decode_bmp8(ByteSpan bytes, std::uint32_t& width, std::uint32_t& height,
                 std::vector<std::uint8_t>& grey) {
  const auto u16 = [&](std::size_t at) -> std::uint32_t {
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8;
  };
  const auto u32 = [&](std::size_t at) -> std::uint32_t {
    return u16(at) | u16(at + 2) << 16;
  };

  if (bytes.size() < 54 || bytes[0] != 'B' || bytes[1] != 'M') return false;
  const std::uint32_t offset = u32(10);
  const std::uint32_t info_size = u32(14);
  const auto declared_height = static_cast<std::int32_t>(u32(22));
  width = u32(18);
  height = static_cast<std::uint32_t>(declared_height < 0 ? -declared_height : declared_height);
  const std::uint32_t bits = u16(28);
  const std::uint32_t compression = u32(30);
  if (bits != 8 || compression != 0 || width == 0 || height == 0) return false;

  // The palette is greyscale in every mask, but reading it costs four lines and
  // removes the assumption.
  const std::size_t palette_at = 14 + info_size;
  std::array<std::uint8_t, 256> palette{};
  for (std::size_t i = 0; i < 256; ++i) {
    const std::size_t at = palette_at + i * 4;
    palette[i] = at + 2 < bytes.size() ? bytes[at + 2] : static_cast<std::uint8_t>(i);
  }

  const std::size_t stride = (static_cast<std::size_t>(width) + 3) & ~std::size_t{3};
  if (offset + stride * height > bytes.size()) return false;

  grey.assign(static_cast<std::size_t>(width) * height, 0);
  for (std::uint32_t y = 0; y < height; ++y) {
    // A positive height means the rows are stored bottom up.
    const std::uint32_t source = declared_height < 0 ? y : height - 1 - y;
    const std::size_t row = offset + static_cast<std::size_t>(source) * stride;
    for (std::uint32_t x = 0; x < width; ++x) {
      grey[static_cast<std::size_t>(y) * width + x] = palette[bytes[row + x]];
    }
  }
  return true;
}

}  // namespace

MapRenderer::~MapRenderer() {
  if (device_ != nullptr && ground_texture_ != nullptr) {
    SDL_ReleaseGPUTexture(device_, ground_texture_);
  }
}

// --------------------------------------------------------------------------
// load
// --------------------------------------------------------------------------

bool MapRenderer::create(Vfs& vfs, SpriteRenderer& renderer, SDL_GPUDevice* device,
                         std::string* error) {
  vfs_ = &vfs;
  renderer_ = &renderer;
  device_ = device;

  // The class graph: 845 documents addressed by directory and suffix, because
  // the data ships no index of them.
  for (const std::string& name : vfs.list()) {
    if (name.size() < 21) continue;
    if (name.compare(0, 13, "DATA\\CLASSES\\") != 0) continue;
    if (name.compare(name.size() - 7, 7, ".SC.XML") != 0) continue;
    const ByteSpan document = vfs.read(name);
    if (document.empty()) continue;
    (void)classes_.add(as_core_bytes(document), name);
  }
  classes_.link();
  stats_.classes = classes_.size();
  if (classes_.empty()) {
    if (error != nullptr) *error = "no class definitions under DATA\\CLASSES";
    return false;
  }

  const ByteSpan terrains = vfs.read("DATA\\TERRAINS.XML");
  auto table = core::TerrainTable::parse(as_core_bytes(terrains));
  if (!table) {
    if (error != nullptr) *error = "DATA\\TERRAINS.XML did not parse";
    return false;
  }
  terrain_ = std::move(table.value());
  stats_.terrain_layers = terrain_.layers().size();

  const ByteSpan zbins = vfs.read("DATA\\ZBINS.XML");
  if (auto bins = core::ZBins::parse(as_core_bytes(zbins)); bins) {
    zbins_ = std::move(bins.value());
  } else if (error != nullptr) {
    *error = "DATA\\ZBINS.XML did not parse";
    return false;
  }

  // Decorations are optional: six shipped maps carry none, and a container
  // without the palette should still draw its objects.
  const ByteSpan ini = vfs.read("MAPOBJECTS\\DECORS\\DECORS.INI");
  if (auto decors = core::DecorTable::parse(as_core_bytes(ini)); decors) {
    decors_ = std::move(decors.value());
  }
  return true;
}

bool MapRenderer::load(const core::WorldMap& map, std::string_view season,
                       std::span<const core::Rgb888> players, std::string* error) {
  map_ = &map;
  players_.assign(players.begin(), players.end());
  season_name_ = season.empty() ? "spring" : std::string(season);
  season_ = core::season_from_name(season_name_);

  // Art caches are keyed on the entity path, not the map, so they survive a
  // map change; the composited ground cache does not, because a cell's
  // contents are a property of the map.
  tiles_.clear();
  frame_valid_ = false;

  stats_.objects = map.objects().objects().size();
  stats_.placed = 0;
  stats_.unresolved = 0;

  for (const core::MapObject& object : map.objects().objects()) {
    const EntityArt* art = art_for_class(object.class_name);
    if (art == nullptr) {
      ++stats_.unresolved;
      continue;
    }
    ++stats_.placed;
    // Team palettes are staged, not uploaded, so every (sheet, player) pairing
    // the scene can ask for has to be claimed now: a row created during a draw
    // would be sampled a frame before it exists and the sprite would come out
    // of the shader as whatever the lookup texture happened to hold.
    for (const ArtLayer& layer : art->layers) {
      (void)palette_for(layer.sheet, object.player, true);
    }
  }

  // Decorations: one entity per distinct kind present in the layer, resolved
  // now so the first frame does not stall on 47,000 cells.
  stats_.decorations = 0;
  if (map.decor().cell_size() != 0) {
    std::array<bool, 256> seen{};
    const std::uint32_t cells = map.decor().width();
    for (std::uint32_t cy = 0; cy < map.decor().height(); ++cy) {
      for (std::uint32_t cx = 0; cx < cells; ++cx) {
        core::DecorCell decor;
        if (!core::decor_unpack(map.decor().cell(cx, cy), decor)) continue;
        ++stats_.decorations;
        if (seen[static_cast<std::size_t>(decor.kind) & 0xFF]) continue;
        seen[static_cast<std::size_t>(decor.kind) & 0xFF] = true;
        if (const core::DecorKind* kind = decors_.find(decor.kind); kind != nullptr) {
          (void)art_for_entity(kind->entity);
        }
      }
    }
  }

  stats_.entities = entities_.size();
  stats_.sheets = sheets_.size();

  std::string upload_error;
  if (!renderer_->commit_uploads(&upload_error)) {
    if (error != nullptr) *error = "atlas upload failed: " + upload_error;
    return false;
  }

  if (map.geometry().has_start) {
    look_at(map.geometry().start_x, map.geometry().start_y, 0, 0);
  } else {
    look_at(map.geometry().size_x / 2, map.geometry().size_y / 2, 0, 0);
  }
  return true;
}

// --------------------------------------------------------------------------
// art resolution
// --------------------------------------------------------------------------

std::int32_t MapRenderer::sheet_for(const std::string& path) {
  const auto found = sheet_by_path_.find(path);
  if (found != sheet_by_path_.end()) return found->second;

  std::int32_t result = -1;
  const ByteSpan table = vfs_->read(path);
  if (!table.empty()) {
    auto parsed = core::RleImage::parse(as_core_bytes(table));
    if (parsed) {
      Sheet sheet;
      sheet.image = std::move(parsed.value());
      std::string error;
      if (upload_sprite(*renderer_, sheet.image, vfs_->pixel_store(), sheet.sprite, &error,
                        /*first_frame_only=*/true)) {
        sheets_.push_back(std::move(sheet));
        result = static_cast<std::int32_t>(sheets_.size()) - 1;
        if (SDL_getenv("IMPERIVM_DEBUG_SHEETS") != nullptr) {
          const Sheet& s = sheets_.back();
          SDL_Log("sheet %s class=%u mask=%d pal=%zu pc=%d ti=%u", path.c_str(),
                  s.image.raw_image_class(), s.sprite.mask ? 1 : 0, s.image.palette_size(),
                  s.sprite.player_color ? 1 : 0, s.sprite.transparent_index);
        }
      }
    }
  }
  sheet_by_path_.emplace(path, result);
  return result;
}

const MapRenderer::EntityArt* MapRenderer::art_for_entity(std::string_view entity_path) {
  if (entity_path.empty()) return nullptr;
  const std::string key = core::normalise_resource_path(entity_path);
  const auto found = art_by_entity_.find(key);
  if (found != art_by_entity_.end()) {
    return found->second.layers.empty() ? nullptr : &found->second;
  }

  EntityArt art;
  const core::Entity* entity = entities_.find(key);
  if (entity == nullptr) {
    const ByteSpan document = vfs_->read(key);
    if (!document.empty()) {
      auto loaded = entities_.load(key, as_core_bytes(document));
      entity = loaded.ok() ? loaded.value() : nullptr;
    }
  }

  if (entity != nullptr) {
    for (const std::uint32_t index : entity->draw_order()) {
      const core::EntityLayer& layer = entity->layers()[index];
      const core::EntityImage* image = entity->image(layer.image);
      if (image == nullptr) continue;

      std::int32_t sheet = -1;
      for (const std::string& candidate :
           core::image_path_candidates(entity->path(), image->file)) {
        if (!vfs_->contains(candidate)) continue;
        sheet = sheet_for(candidate);
        if (sheet >= 0) break;
      }
      if (sheet < 0) continue;

      const SpriteFrame* frame = sheets_[static_cast<std::size_t>(sheet)].sprite.at(0, 0);
      if (frame == nullptr || !frame->region.valid()) continue;

      ArtLayer out;
      out.sheet = sheet;
      out.z = layer.z;
      out.sort_offset_x = layer.sortoffsetx;
      out.sort_offset_y = layer.sortoffsety;
      out.offset_x = layer.offsetx + frame->left;
      out.offset_y = layer.offsety + frame->top;
      art.layers.push_back(out);
    }
  }

  const auto inserted = art_by_entity_.emplace(key, std::move(art)).first;
  return inserted->second.layers.empty() ? nullptr : &inserted->second;
}

const MapRenderer::EntityArt* MapRenderer::art_for_class(const std::string& class_name) {
  const auto found = art_by_class_.find(class_name);
  if (found != art_by_class_.end()) return found->second;

  const EntityArt* art = nullptr;
  const core::ClassIndex index = classes_.lookup(class_name);
  if (index != core::kNoClass) {
    art = art_for_entity(classes_.entity_path(index, season_));
  }
  art_by_class_.emplace(class_name, art);
  return art;
}

PaletteRow MapRenderer::palette_for(std::int32_t sheet, std::int32_t player,
                                   bool create) {
  Sheet& stored = sheets_[static_cast<std::size_t>(sheet)];
  if (!stored.sprite.player_color || player <= 0) return stored.sprite.neutral;

  const std::uint64_t key =
      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sheet)) << 8) |
      static_cast<std::uint32_t>(player & 0xFF);
  const auto found = palettes_.find(key);
  if (found != palettes_.end()) return found->second;
  if (!create) return stored.sprite.neutral;

  // The map's own player colours, from `player<i>.xml`. That attribute is the
  // only per-player colour anywhere in the shipped data; the palette ramps the
  // engine really swaps into slots 0..63 are in the executable and have not
  // been sourced, so this retints by hue and keeps each slot's brightness --
  // see sprite_atlas.hpp. A faithful preview of the mechanism, not a
  // reproduction of a particular army.
  const std::size_t slot = static_cast<std::size_t>(player - 1);
  if (slot >= players_.size()) return stored.sprite.neutral;
  const core::Rgb888 team = players_[slot];
  if (team.red == 0 && team.green == 0 && team.blue == 0) return stored.sprite.neutral;

  PaletteRow row = upload_team_palette(*renderer_, stored.image, team,
                                       stored.sprite.transparent_index);
  if (!row.valid) row = stored.sprite.neutral;
  palettes_.emplace(key, row);
  return row;
}

// --------------------------------------------------------------------------
// the ground
// --------------------------------------------------------------------------

const MapRenderer::GroundTexture* MapRenderer::ground_texture(std::int32_t layer_z) {
  const auto found = ground_.find(layer_z);
  if (found != ground_.end()) {
    return found->second.rgb.empty() ? nullptr : &found->second;
  }

  GroundTexture texture;
  if (const core::TerrainLayerDef* layer = terrain_.layer(layer_z); layer != nullptr) {
    const std::string path = core::TerrainTable::texture_path(layer->image, season_name_);
    const ByteSpan bytes = vfs_->read(path);
    if (!bytes.empty()) {
      if (auto image = core::VqImage::parse(as_core_bytes(bytes)); image) {
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(image->width()) *
                                      image->height() * 3);
        if (image->decode_rgb888({reinterpret_cast<std::byte*>(rgb.data()), rgb.size()})) {
          texture.width = image->width();
          texture.height = image->height();
          // The two waters are one image of fifteen stacked frames. Animation
          // is not wired up yet, so the field is the first frame.
          if (layer->frames > 1 && texture.height % static_cast<std::uint32_t>(layer->frames) == 0) {
            texture.height /= static_cast<std::uint32_t>(layer->frames);
            rgb.resize(static_cast<std::size_t>(texture.width) * texture.height * 3);
          }
          texture.rgb = std::move(rgb);
        }
      }
    }
  }
  const auto inserted = ground_.emplace(layer_z, std::move(texture)).first;
  return inserted->second.rgb.empty() ? nullptr : &inserted->second;
}

const std::vector<std::uint8_t>* MapRenderer::transition_mask(char style, std::uint8_t code) {
  const auto key = static_cast<std::uint16_t>(static_cast<std::uint8_t>(style) << 8 | code);
  const auto found = masks_.find(key);
  if (found != masks_.end()) {
    return found->second.empty() ? nullptr : &found->second;
  }

  // The mask is the overlay's opacity as it stands: `code` names the corners
  // the overlay does *not* hold, a set bit is black, so the file is white
  // where the overlay is (0x0061ffc1). The original quantises it to five bits
  // (0x0061e350, `>> 3`) because it blends in 16-bit colour; this keeps eight.
  std::vector<std::uint8_t> alpha;
  if (code != 0 && code != 0xF) {
    const std::string path =
        "TERRAIN\\TRANSITIONS\\" + core::transition_mask_name(style, code) + ".BMP";
    const ByteSpan bytes = vfs_->read(path);
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!bytes.empty() && decode_bmp8(bytes, width, height, alpha) &&
        (width != static_cast<std::uint32_t>(kCellWidth) ||
         height != static_cast<std::uint32_t>(kCellHeight))) {
      alpha.clear();
    }
  }
  const auto inserted = masks_.emplace(key, std::move(alpha)).first;
  return inserted->second.empty() ? nullptr : &inserted->second;
}

void MapRenderer::invalidate_ground(std::int32_t cx0, std::int32_t cy0, std::int32_t cx1,
                                    std::int32_t cy1) {
  if (map_ == nullptr) return;
  const auto cells = static_cast<std::int32_t>(map_->terrain_cells());
  cx0 = std::max(0, std::min(cx0, cx1) - 1);
  cy0 = std::max(0, std::min(cy0, cy1) - 1);
  cx1 = std::min(cells - 1, std::max(cx0, cx1) + 1);
  cy1 = std::min(cells - 1, std::max(cy0, cy1) + 1);
  for (std::int32_t cy = cy0; cy <= cy1; ++cy) {
    for (std::int32_t cx = cx0; cx <= cx1; ++cx) tiles_.erase(cell_key(cx, cy));
  }
  frame_valid_ = false;
}

const std::vector<std::uint8_t>* MapRenderer::cell_tile(std::int32_t cx, std::int32_t cy) {
  const std::uint64_t key = cell_key(cx, cy);
  const auto found = tiles_.find(key);
  if (found != tiles_.end()) {
    return found->second.empty() ? nullptr : &found->second;
  }
  // Panning walks the map, so the cache has to be bounded. A 1500 x 1000
  // viewport holds about 600 cells; four thousand is a comfortable working set
  // and 47 MB, and dropping the lot is cheaper than tracking recency.
  if (tiles_.size() > 4000) tiles_.clear();

  // The tile over this cell, as the original composes it (0x0061f8a0): the
  // terrain byte is a vertex, and the tile blends the four at its corners --
  // see `core::terrain_tile`.
  const core::TerrainTile layers = core::terrain_tile(map_->terrain(), cx, cy);
  const GroundTexture* base = ground_texture(layers.base);

  std::vector<std::uint8_t> tile;
  if (base != nullptr) {
    tile.assign(static_cast<std::size_t>(kCellWidth) * kCellHeight * 4, 255);

    // The texture is wallpaper, not a tile: it is sampled continuously in
    // screen space so that it runs across cell boundaries unbroken.
    const auto paint = [&](const GroundTexture& texture, std::uint8_t* out) {
      for (std::int32_t row = 0; row < kCellHeight; ++row) {
        const std::uint32_t v =
            static_cast<std::uint32_t>(cy * kCellHeight + row) % texture.height;
        const std::uint8_t* source = texture.rgb.data() +
                                     static_cast<std::size_t>(v) * texture.width * 3;
        for (std::int32_t column = 0; column < kCellWidth; ++column) {
          const std::uint32_t u =
              static_cast<std::uint32_t>(cx * kCellWidth + column) % texture.width;
          const std::size_t at =
              (static_cast<std::size_t>(row) * kCellWidth + column) * 4;
          out[at + 0] = source[u * 3 + 0];
          out[at + 1] = source[u * 3 + 1];
          out[at + 2] = source[u * 3 + 2];
          out[at + 3] = 255;
        }
      }
    };
    paint(*base, tile.data());

    // Each overlay through its mask, in the order the tile lists them. The
    // mask row alternates by the tile's row (C on even rows, D on odd for
    // every shipped layer), and the file is the one naming the corners the
    // overlay does not hold.
    std::vector<std::uint8_t> over;
    for (std::size_t i = 0; i < layers.overlay_count; ++i) {
      const core::TerrainTileLayer& layer = layers.overlays[i];
      const GroundTexture* texture = ground_texture(layer.type);
      if (texture == nullptr) continue;
      const core::TerrainLayerDef* def = terrain_.layer(layer.type);
      const char style = core::transition_style(
          def != nullptr ? def->transition : core::kDefaultTransition, cy);
      const std::vector<std::uint8_t>* mask =
          transition_mask(style, core::transition_mask_code(layer.corners));
      if (mask == nullptr) continue;
      over.assign(static_cast<std::size_t>(kCellWidth) * kCellHeight * 4, 255);
      paint(*texture, over.data());
      for (std::size_t p = 0; p < mask->size(); ++p) {
        const std::uint32_t a = (*mask)[p];
        for (std::size_t channel = 0; channel < 3; ++channel) {
          const std::size_t at = p * 4 + channel;
          tile[at] = static_cast<std::uint8_t>(
              (static_cast<std::uint32_t>(tile[at]) * (255 - a) +
               static_cast<std::uint32_t>(over[at]) * a) / 255);
        }
      }
    }
    // The light is not applied here: the original applies it in the warp,
    // per pixel, interpolated across each 32-unit quad (`compose_ground`).
  }

  const auto inserted = tiles_.emplace(key, std::move(tile)).first;
  return inserted->second.empty() ? nullptr : &inserted->second;
}

MapRenderer::ZoomStats MapRenderer::compose_zoom(std::uint32_t divisor, core::ui::Image& out) {
  ZoomStats stats;
  out = core::ui::Image{};
  if (map_ == nullptr || divisor == 0 || map_->terrain_cells() == 0) return stats;

  // The art is the minimap's own, for this zoom (0x006183b0): each layer's
  // `minimap` tile, `%season%` resolved as its ground texture's is, under
  // `MINIMAP\ZOOM<divisor>\TERRAIN\`, and the C and D masks beside it.
  // Loaded for the call: the picture is composed once per map and zoom.
  const std::string root = "MINIMAP\\ZOOM" + std::to_string(divisor) + "\\TERRAIN\\";
  const auto load = [&](const std::string& path, core::ui::Image& image) {
    const ByteSpan bytes = vfs_->read(path);
    if (bytes.empty()) return false;
    auto decoded = core::ui::decode_bmp(as_core_bytes(bytes));
    if (!decoded) return false;
    image = std::move(decoded.value());
    return !image.empty();
  };
  std::int32_t top = 0;
  for (const core::TerrainLayerDef& layer : terrain_.layers()) top = std::max(top, layer.z);
  std::vector<core::ui::Image> tiles(static_cast<std::size_t>(top) + 1);
  std::vector<const core::ui::Image*> tile_of(tiles.size(), nullptr);
  // The roads, `type` 6, for the tally.
  const std::unique_ptr<bool[]> road(new bool[tiles.size()]());
  for (const core::TerrainLayerDef& layer : terrain_.layers()) {
    if (layer.z < 0 || layer.minimap.empty()) continue;
    const auto z = static_cast<std::size_t>(layer.z);
    road[z] = layer.type == 6;
    if (load(root + core::TerrainTable::texture_path(layer.minimap, season_name_), tiles[z])) {
      tile_of[z] = &tiles[z];
      ++stats.tiles;
    }
  }
  core::ZoomGroundArt art;
  art.tiles = tile_of;
  std::array<core::ui::Image, 32> masks;
  for (std::size_t row = 0; row < 2; ++row) {
    for (std::uint8_t code = 1; code < 15; ++code) {
      const std::size_t at = row * 16 + code;
      const std::string name = core::transition_mask_name(row == 0 ? 'C' : 'D', code);
      if (load(root + "TRANSITIONS\\" + name + ".BMP", masks[at])) {
        art.masks[at] = &masks[at];
        ++stats.masks;
      }
    }
  }
  const core::ZoomGroundStats drawn =
      core::compose_zoom_ground(map_->terrain(), map_->light(), divisor, art, out,
                                std::span<const bool>(road.get(), tiles.size()));
  stats.road_pixels = drawn.counted_pixels;
  return stats;
}

bool MapRenderer::compose_ground(std::uint32_t width, std::uint32_t height) {
  if (frame_valid_ && frame_width_ == width && frame_height_ == height &&
      frame_view_x_ == view_x_ && frame_view_y_ == view_y_) {
    return false;
  }
  frame_width_ = width;
  frame_height_ = height;
  frame_view_x_ = view_x_;
  frame_view_y_ = view_y_;
  frame_valid_ = true;
  frame_.assign(static_cast<std::size_t>(width) * height * 4, 0);
  frame_world_.assign(static_cast<std::size_t>(width) * height * 2, 0);
  shaded_generation_ = ~0ull;

  // The ground is a mesh: each cell is drawn as four quads of 32 world
  // units whose corners are lifted by the height layer at the corner
  // (`Terrain.height.grid` holds the height at 32-unit corners, as the
  // bilinear sample in `sim/flying.hpp` reads it), one pixel of screen up
  // per unit of height. A quad's left and right edges stay vertical -- x
  // passes through the projection unchanged -- so each screen column of it
  // is the tile's column stretched between the top edge and the bottom
  // edge interpolated at that column, which is the bilinear surface the
  // sample answers and the join between neighbouring quads has no seam.
  // A map with no height layer draws flat, which is the same mesh at zero.
  const core::Grid& elevation = map_->height();
  const bool lifted = elevation.cell_size() == 32;
  const auto corner = [&](std::int32_t hx, std::int32_t hy) -> std::int32_t {
    if (!lifted) return 0;
    hx = std::clamp(hx, 0, static_cast<std::int32_t>(elevation.width()) - 1);
    hy = std::clamp(hy, 0, static_cast<std::int32_t>(elevation.height()) - 1);
    return static_cast<std::int32_t>(elevation.cell(static_cast<std::uint32_t>(hx), static_cast<std::uint32_t>(hy)));
  };

  // The baked light, which the original applies here and not to the flat
  // tile (0x006217f0): the level at the quad's four 32-unit corners, the same
  // vertices as the height, interpolated in 8.8 fixed point along the top and
  // bottom edges per column and then down the column per screen row, and the
  // integer part of the result looks up the gain. So the light is bilinear per
  // pixel across a quad and continuous between quads, and it moves in whole
  // levels: a band is one step of the 22, 5% of brightness.
  const core::Grid& light = map_->light();
  const bool lit = light.cell_size() == 32;
  const auto light_corner = [&](std::int32_t lx, std::int32_t ly) -> std::int32_t {
    if (!lit) return kNeutralLight;
    lx = std::clamp(lx, 0, static_cast<std::int32_t>(light.width()) - 1);
    ly = std::clamp(ly, 0, static_cast<std::int32_t>(light.height()) - 1);
    const auto level = static_cast<std::int32_t>(light.cell(static_cast<std::uint32_t>(lx), static_cast<std::uint32_t>(ly)));
    return std::min(level, kLightLevels - 1);
  };
  const std::uint8_t* gains = light_table().data();

  const auto cells = static_cast<std::int32_t>(map_->terrain_cells());
  const std::int32_t first_x = std::max(0, view_x_ / kCellWidth - 1);
  const std::int32_t first_y = std::max(0, view_y_ / kCellHeight - 1);
  const std::int32_t last_x =
      std::min(cells, (view_x_ + static_cast<std::int32_t>(width)) / kCellWidth + 1);
  // Rows below the view lift into it: the tallest ground lifts 255 rows,
  // which is twelve cells.
  const std::int32_t reach = lifted ? 255 / kCellHeight + 2 : 1;
  const std::int32_t last_y =
      std::min(cells, (view_y_ + static_cast<std::int32_t>(height)) / kCellHeight + reach);

  constexpr std::int32_t kHalfWidth = kCellWidth / 2;    // 32
  constexpr std::int32_t kHalfHeight = kCellHeight / 2;  // 23
  const auto frame_width = static_cast<std::int32_t>(width);
  const auto frame_height = static_cast<std::int32_t>(height);

  for (std::int32_t cy = first_y; cy < last_y; ++cy) {
    for (std::int32_t cx = first_x; cx < last_x; ++cx) {
      const std::vector<std::uint8_t>* tile = cell_tile(cx, cy);
      if (tile == nullptr) continue;
      const std::int32_t x = cx * kCellWidth - view_x_;
      const std::int32_t y = cy * kCellHeight - view_y_;
      for (std::int32_t qy = 0; qy < 2; ++qy) {
        for (std::int32_t qx = 0; qx < 2; ++qx) {
          // The quad's four corner heights, and its flat screen box.
          const std::int32_t hx = cx * 2 + qx;
          const std::int32_t hy = cy * 2 + qy;
          const std::int32_t h_tl = corner(hx, hy);
          const std::int32_t h_tr = corner(hx + 1, hy);
          const std::int32_t h_bl = corner(hx, hy + 1);
          const std::int32_t h_br = corner(hx + 1, hy + 1);
          // And its four light levels, in 8.8 fixed point; a quad lit
          // neutral at every corner is a copy, as it is in the original.
          const std::int32_t l_tl = light_corner(hx, hy) << 8;
          const std::int32_t l_tr = light_corner(hx + 1, hy) << 8;
          const std::int32_t l_bl = light_corner(hx, hy + 1) << 8;
          const std::int32_t l_br = light_corner(hx + 1, hy + 1) << 8;
          const bool neutral = l_tl == kNeutralLight << 8 && l_tr == l_tl && l_bl == l_tl && l_br == l_tl;
          const std::int32_t qx0 = x + qx * kHalfWidth;
          const std::int32_t qy0 = y + qy * kHalfHeight;
          // Off the frame horizontally: nothing of this quad shows.
          if (qx0 >= frame_width || qx0 + kHalfWidth <= 0) continue;
          if (!lifted && (qy0 >= frame_height || qy0 + kHalfHeight <= 0)) continue;
          const std::int32_t col0 = std::max(0, qx0);
          const std::int32_t col1 = std::min(frame_width, qx0 + kHalfWidth);
          for (std::int32_t column = col0; column < col1; ++column) {
            const std::int32_t u = column - qx0;  // 0..31 across the quad
            // The top and bottom edges at this column, in 1/32 of a pixel.
            const std::int32_t top = qy0 * kHalfWidth - (h_tl * (kHalfWidth - u) + h_tr * u);
            const std::int32_t bottom =
                (qy0 + kHalfHeight) * kHalfWidth - (h_bl * (kHalfWidth - u) + h_br * u);
            const std::int32_t rows = bottom - top;  // > 0: a 23-row quad lifts at most 255
            if (rows <= 0) continue;
            // The screen rows this column covers, and the tile row each one
            // samples: `v` runs 0..22 down the quad's half of the tile.
            const std::int32_t row_first = std::max(0, (top + kHalfWidth - 1) / kHalfWidth);
            const std::int32_t row_last = std::min(frame_height, (bottom + kHalfWidth - 1) / kHalfWidth);
            const std::int32_t tile_x = qx * kHalfWidth + u;
            // The quad's world column, and its world rows: 32 units over
            // the quad's rows, from the corner.
            const std::int32_t world_x = (cx * 2 + qx) * kHalfWidth + u;
            const std::int32_t world_y0 = (cy * 2 + qy) * kHalfWidth;
            // The light along the top and bottom edges at this column: the
            // original steps each by `(right - left) << 3` a column, which is
            // this over the quad's 32.
            const std::int32_t light_top = l_tl + (l_tr - l_tl) * u / kHalfWidth;
            const std::int32_t light_bottom = l_bl + (l_br - l_bl) * u / kHalfWidth;
            for (std::int32_t row = row_first; row < row_last; ++row) {
              const std::int32_t along = row * kHalfWidth - top;  // 0..rows, in 1/32 px
              std::int32_t v = along * kHalfHeight / rows;
              v = std::clamp(v, 0, kHalfHeight - 1);
              const std::int32_t tile_y = qy * kHalfHeight + v;
              const std::size_t source = (static_cast<std::size_t>(tile_y) * kCellWidth + tile_x) * 4;
              const std::size_t pixel = static_cast<std::size_t>(row) * width + static_cast<std::size_t>(column);
              std::uint8_t* out = frame_.data() + pixel * 4;
              std::memcpy(out, tile->data() + source, 4);
              if (!neutral) {
                const std::int32_t level =
                    (light_top + (light_bottom - light_top) * std::clamp(along, 0, rows) / rows) >> 8;
                if (level != kNeutralLight) {
                  const std::uint8_t* gain = gains + static_cast<std::size_t>(std::clamp(level, 0, kLightLevels - 1)) * 256;
                  out[0] = gain[out[0]];
                  out[1] = gain[out[1]];
                  out[2] = gain[out[2]];
                }
              }
              const std::int32_t world_y = world_y0 + std::clamp(along * kHalfWidth / rows, 0, kHalfWidth - 1);
              frame_world_[pixel * 2] = static_cast<std::uint16_t>(std::clamp(world_x, 0, 65535));
              frame_world_[pixel * 2 + 1] = static_cast<std::uint16_t>(std::clamp(world_y, 0, 65535));
            }
          }
        }
      }
    }
  }
  return true;
}

bool MapRenderer::ensure_ground_texture(std::uint32_t width, std::uint32_t height) {
  if (ground_texture_ != nullptr && ground_texture_width_ == width &&
      ground_texture_height_ == height) {
    return true;
  }
  if (ground_texture_ != nullptr) {
    SDL_ReleaseGPUTexture(device_, ground_texture_);
    ground_texture_ = nullptr;
  }
  // A fresh texture holds nothing, whatever the composite thinks.
  ground_uploaded_ = false;

  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
  info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
  info.width = width;
  info.height = height;
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  ground_texture_ = SDL_CreateGPUTexture(device_, &info);
  ground_texture_width_ = width;
  ground_texture_height_ = height;
  return ground_texture_ != nullptr;
}

bool MapRenderer::draw_terrain(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
                               std::uint32_t width, std::uint32_t height) {
  if (map_ == nullptr || commands == nullptr || target == nullptr) return false;
  if (width == 0 || height == 0) return false;
  if (!ensure_ground_texture(width, height)) return false;

  // The ground is a still image between camera moves, and the texture holding
  // it survives the frame. Upload only what has changed: a frame that neither
  // panned nor resized keeps the copy already on the GPU and goes straight to
  // the blit.
  if (compose_ground(width, height)) ground_uploaded_ = false;
  if (shade_ground()) ground_uploaded_ = false;
  const std::vector<std::uint8_t>& pixels = fog_ != nullptr ? frame_shaded_ : frame_;

  if (!ground_uploaded_) {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = static_cast<Uint32>(pixels.size());
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &info);
    if (transfer == nullptr) return false;

    void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false);
    if (mapped == nullptr) {
      SDL_ReleaseGPUTransferBuffer(device_, transfer);
      return false;
    }
    std::memcpy(mapped, pixels.data(), pixels.size());
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transfer;
    source.pixels_per_row = width;
    source.rows_per_layer = height;
    SDL_GPUTextureRegion destination{};
    destination.texture = ground_texture_;
    destination.w = width;
    destination.h = height;
    destination.d = 1;
    SDL_UploadToGPUTexture(copy, &source, &destination, true);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    ground_uploaded_ = true;
  }

  SDL_GPUBlitInfo blit{};
  blit.source.texture = ground_texture_;
  blit.source.w = width;
  blit.source.h = height;
  blit.destination.texture = target;
  blit.destination.w = width;
  blit.destination.h = height;
  blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
  blit.filter = SDL_GPU_FILTER_NEAREST;
  SDL_BlitGPUTexture(commands, &blit);
  return true;
}

// --------------------------------------------------------------------------
// objects
// --------------------------------------------------------------------------

void MapRenderer::add_sprite(const EntityArt& art, std::int32_t world_x,
                             std::int32_t world_y, std::int32_t player, std::int32_t width,
                             std::int32_t height) {
  const std::int32_t x = core::world_to_screen_x(world_x) - view_x_;
  const std::int32_t y = core::world_to_screen_y(world_y, ground_height(world_x, world_y)) - view_y_;
  if (x < -kSpriteMargin || x > width + kSpriteMargin) return;
  if (y < -kSpriteMargin || y > height + kSpriteMargin) return;

  for (const ArtLayer& layer : art.layers) {
    Sheet& sheet = sheets_[static_cast<std::size_t>(layer.sheet)];
    const SpriteFrame* frame = sheet.sprite.at(0, 0);
    if (frame == nullptr || !frame->region.valid()) continue;

    DrawItem item;
    item.bin = static_cast<std::uint32_t>(zbins_.bin_for(layer.z));
    item.order = static_cast<std::uint32_t>(items_.size());
    item.sort_y = y + layer.sort_offset_y;
    item.sort_x = x + layer.sort_offset_x;
    item.x = static_cast<float>(x + layer.offset_x);
    item.y = static_cast<float>(y + layer.offset_y);
    item.region = frame->region;

    // A shadow decodes to a coverage mask with no colour of its own; it is
    // drawn as translucent black through the two-entry mask palette.
    const bool shadow = sheet.sprite.mask ||
                        sheet.image.image_class() == core::RleImageClass::shadow;
    if (shadow) {
      item.palette = sheet.sprite.neutral;
      item.modulate = Rgba{1.0F, 1.0F, 1.0F, kShadowAlpha};
    } else {
      item.palette = palette_for(layer.sheet, player, false);
    }
    items_.push_back(item);
  }
}

std::size_t MapRenderer::queue_objects(std::uint32_t width, std::uint32_t height) {
  if (map_ == nullptr) return 0;
  items_.clear();

  const auto w = static_cast<std::int32_t>(width);
  const auto h = static_cast<std::int32_t>(height);

  for (const core::MapObject& object : map_->objects().objects()) {
    const EntityArt* art = art_for_class(object.class_name);
    if (art == nullptr) continue;
    add_sprite(*art, object.x, object.y, object.player, w, h);
  }

  // Decorations: one entity per non-zero cell of `Terrain.decor.grid`, placed
  // at the cell corner plus the two sub-cell nibbles, four world units a step.
  const core::Grid& decor = map_->decor();
  if (decor.cell_size() != 0) {
    const auto cells = static_cast<std::int32_t>(decor.width());
    const std::int32_t first_x = std::max(0, view_x_ / kCellWidth - 8);
    const std::int32_t first_y = std::max(0, view_y_ / kCellHeight - 12);
    const std::int32_t last_x = std::min(cells, (view_x_ + w) / kCellWidth + 8);
    const std::int32_t last_y = std::min(cells, (view_y_ + h) / kCellHeight + 12);
    for (std::int32_t cy = first_y; cy < last_y; ++cy) {
      for (std::int32_t cx = first_x; cx < last_x; ++cx) {
        core::DecorCell cell;
        if (!core::decor_unpack(decor.cell(static_cast<std::uint32_t>(cx),
                                           static_cast<std::uint32_t>(cy)),
                                cell)) {
          continue;
        }
        const core::DecorKind* kind = decors_.find(cell.kind);
        if (kind == nullptr) continue;
        const EntityArt* art = art_for_entity(kind->entity);
        if (art == nullptr) continue;
        add_sprite(*art, cx * kCellWidth + cell.offset_x, cy * kCellWidth + cell.offset_y,
                   0, w, h);
      }
    }
  }

  // Bin order first. Inside a sorted bin the key is the layer's own ground
  // contact point in screen space, with x as a tie-break; inside an unsorted
  // bin the emission order stands.
  std::vector<std::uint32_t> order(items_.size());
  for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [this](std::uint32_t a, std::uint32_t b) {
    const DrawItem& left = items_[a];
    const DrawItem& right = items_[b];
    if (left.bin != right.bin) return left.bin < right.bin;
    const bool sorted = left.bin < zbins_.bins().size() && zbins_.bins()[left.bin].sorted;
    if (!sorted) return left.order < right.order;
    if (left.sort_y != right.sort_y) return left.sort_y < right.sort_y;
    if (left.sort_x != right.sort_x) return left.sort_x < right.sort_x;
    return left.order < right.order;
  });

  for (const std::uint32_t index : order) {
    const DrawItem& item = items_[index];
    renderer_->draw(item.region, item.x, item.y, item.palette, item.modulate);
  }
  stats_.last_queued = items_.size();
  return items_.size();
}

// --------------------------------------------------------------------------
// camera
// --------------------------------------------------------------------------

void MapRenderer::set_fog(const core::sim::FogLight* fog, std::uint64_t generation) noexcept {
  if (fog != nullptr && fog->empty()) fog = nullptr;
  if (fog_ == fog && fog_generation_ == generation) return;
  fog_ = fog;
  fog_generation_ = generation;
}

/// The fog laid over the composed ground: each pixel darkened by the fog
/// sampled at the world point under it. Redone when the frame or the fog
/// changed; true when the upload has to follow.
bool MapRenderer::shade_ground() {
  if (fog_ == nullptr) {
    const bool was = !frame_shaded_.empty();
    frame_shaded_.clear();
    shaded_generation_ = ~0ull;
    return was;
  }
  if (shaded_generation_ == fog_generation_ && frame_shaded_.size() == frame_.size()) return false;
  shaded_generation_ = fog_generation_;
  frame_shaded_.resize(frame_.size());
  const std::size_t pixels = frame_.size() / 4;
  const std::span<const std::uint16_t> words = fog_->displayed();
  const std::int32_t columns = fog_->columns();
  const std::int32_t rows = fog_->rows();
  constexpr std::int32_t kCell = core::sim::FogLight::kCell;
  for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
    const std::uint8_t* in = frame_.data() + pixel * 4;
    std::uint8_t* out = frame_shaded_.data() + pixel * 4;
    if (in[3] == 0) {
      std::memcpy(out, in, 4);
      continue;
    }
    // `FogLight::sample`, inline: the four corner words blended, then the
    // factor over 32.
    const std::int32_t wx = frame_world_[pixel * 2];
    const std::int32_t wy = frame_world_[pixel * 2 + 1];
    const std::int32_t cx = wx / kCell;
    const std::int32_t cy = wy / kCell;
    std::int32_t factor = 0;
    if (cx < columns && cy < rows) {
      const std::int32_t nx = std::min(cx + 1, columns - 1);
      const std::int32_t ny = std::min(cy + 1, rows - 1);
      const std::int64_t tx = wx % kCell;
      const std::int64_t ty = wy % kCell;
      const std::int64_t w00 = words[static_cast<std::size_t>(cy) * columns + cx];
      const std::int64_t w10 = words[static_cast<std::size_t>(cy) * columns + nx];
      const std::int64_t w01 = words[static_cast<std::size_t>(ny) * columns + cx];
      const std::int64_t w11 = words[static_cast<std::size_t>(ny) * columns + nx];
      const std::int64_t blend = w00 * (kCell - tx) * (kCell - ty) + w10 * tx * (kCell - ty) +
                                 w01 * (kCell - tx) * ty + w11 * tx * ty;
      factor = core::sim::FogLight::factor_of(static_cast<std::uint16_t>(blend / (kCell * kCell)));
    }
    if (factor >= 32) {
      std::memcpy(out, in, 4);
      continue;
    }
    out[0] = static_cast<std::uint8_t>(in[0] * factor / 32);
    out[1] = static_cast<std::uint8_t>(in[1] * factor / 32);
    out[2] = static_cast<std::uint8_t>(in[2] * factor / 32);
    out[3] = in[3];
  }
  return true;
}

std::int32_t MapRenderer::ground_height(std::int32_t world_x, std::int32_t world_y) const noexcept {
  if (map_ == nullptr || map_->height().cell_size() == 0 || world_x < 0 || world_y < 0) return 0;
  const core::Grid& elevation = map_->height();
  if (world_x >= static_cast<std::int32_t>(elevation.extent_x()) ||
      world_y >= static_cast<std::int32_t>(elevation.extent_y())) {
    return 0;
  }
  return core::sim::sample_height(elevation, core::sim::Point{world_x, world_y});
}

void MapRenderer::set_view(std::int32_t x, std::int32_t y) noexcept {
  // Only a view that actually moved invalidates the composited ground. Saying
  // "invalid" unconditionally is not conservative here, it is wrong by a whole
  // frame's work: `clamp_view` runs every frame from the play loop, so the
  // ground was recomposited and re-uploaded on every frame of a camera that
  // was standing still.
  if (view_x_ == x && view_y_ == y) return;
  view_x_ = x;
  view_y_ = y;
  frame_valid_ = false;
}

void MapRenderer::move_view(std::int32_t dx, std::int32_t dy) noexcept {
  set_view(view_x_ + dx, view_y_ + dy);
}

void MapRenderer::look_at(std::int32_t world_x, std::int32_t world_y, std::int32_t width,
                          std::int32_t height) noexcept {
  set_view(core::world_to_screen_x(world_x) - width / 2,
           core::world_to_screen_y(world_y, ground_height(world_x, world_y)) - height / 2);
  clamp_view(width, height);
}

void MapRenderer::clamp_view(std::int32_t width, std::int32_t height) noexcept {
  if (map_ == nullptr) return;
  const std::int32_t map_width = core::world_to_screen_x(map_->geometry().size_x);
  const std::int32_t map_height = core::world_to_screen_y(map_->geometry().size_y);
  set_view(std::clamp(view_x_, 0, std::max(0, map_width - width)),
           std::clamp(view_y_, 0, std::max(0, map_height - height)));
}

}  // namespace imperivm::platform
