#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/rle.hpp"
#include "imperivm/core/formats/vq.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/ui/image.hpp"
#include "imperivm/core/sim/fog_light.hpp"
#include "imperivm/core/world/map.hpp"
#include "imperivm/platform/sprite_atlas.hpp"
#include "imperivm/platform/sprite_renderer.hpp"
#include "imperivm/platform/vfs.hpp"

struct SDL_GPUDevice;
struct SDL_GPUTexture;
struct SDL_GPUCommandBuffer;

namespace imperivm::platform {

/// Draws a `core::WorldMap`: the textured ground, then every object standing
/// on it, through the projection in docs/engine/projection.md.
///
/// Two passes, because the ground and the sprites are two different problems.
///
/// **The ground is composited on the CPU**, as the original's is (it has no
/// GPU path: 0x0061f8a0 blends into a 16-bit back buffer). It is not a tile
/// atlas: each terrain type is one large tileable field sampled continuously in
/// screen space, and the terrain byte is a *vertex* -- the tile over a cell
/// blends the four vertices at its corners through 64 x 46 masks, C and D
/// alternating by row (`core::terrain_tile`); `Terrain.trans.grid` is zero in
/// every cell of every shipped map and is not read. So a cell is a small
/// composite of one to four texture samples and it never changes; caching the
/// finished 64 x 46 cell makes
/// panning a row copy, and the finished viewport goes to the GPU as one texture
/// and one blit. A shader could do this, but not without an atlas of every
/// terrain texture and every mask bound at once, and the cache is a dozen lines.
///
/// **The sprites go through the existing index atlas**, which already does the
/// palette indirection and team colour. What this class adds is placement (the
/// projection and the layer offsets), the shadow pass, and the ZBINS depth sort.
///
/// The ground is flat. `Terrain.height.grid` is loaded and ignored: displacing
/// rigid cells by their own height tears the ground into disjoint blocks, and
/// the warped ground mesh that would not is out of scope — see
/// docs/engine/projection.md, "Elevation".
class MapRenderer {
 public:
  MapRenderer() = default;
  ~MapRenderer();

  MapRenderer(const MapRenderer&) = delete;
  MapRenderer& operator=(const MapRenderer&) = delete;

  /// Loads the game-wide tables: the class graph, the terrain table, the
  /// decoration palette and the depth bins. Expensive once, never again.
  bool create(Vfs& vfs, SpriteRenderer& renderer, SDL_GPUDevice* device,
              std::string* error = nullptr);

  /// Binds a map and uploads the art every object on it needs. `season`
  /// selects both the `%season%` half of a terrain texture path and the class
  /// graph's seasonal entity override.
  /// `players` is the sixteen `player<i>.xml` colours in slot order, or empty
  /// for the neutral palettes the artist painted. An object's `player` is
  /// 1-based, so slot `player - 1` is the one that colours it.
  bool load(const core::WorldMap& map, std::string_view season,
            std::span<const core::Rgb888> players = {}, std::string* error = nullptr);

  [[nodiscard]] bool ready() const noexcept { return map_ != nullptr; }
  /// The decoration palette `create` loaded, for the views that draw the
  /// layer from it.
  [[nodiscard]] const core::DecorTable& decors() const noexcept { return decors_; }
  /// `DATA\TERRAINS.XML` as `create` loaded it: the editor's Terrains tree
  /// and the passability it re-bakes read the same table the ground draws from.
  [[nodiscard]] const core::TerrainTable& terrain_table() const noexcept { return terrain_; }

  // -- camera ------------------------------------------------------------
  //
  // The view is in *screen* pixels, which is the space the ground rectangles
  // and the sprite offsets are already in. World coordinates only ever enter
  // through the projection.

  /// The fog of war to darken the ground by: each pixel by the fog sampled
  /// at its own world point, bilinear over the 16-unit light grid, as the
  /// original's ground draw does (0x00604a40). `generation` says when the
  /// grid has changed so the shade is redone; null clears the fog.
  void set_fog(const core::sim::FogLight* fog, std::uint64_t generation) noexcept;

  /// The height under a world point, bilinear over the map's layer; zero
  /// off the map or with no layer. What lifts a sprite onto the mesh.
  [[nodiscard]] std::int32_t ground_height(std::int32_t world_x, std::int32_t world_y) const noexcept;

  void set_view(std::int32_t x, std::int32_t y) noexcept;
  void move_view(std::int32_t dx, std::int32_t dy) noexcept;
  /// Centres the view on a world position.
  void look_at(std::int32_t world_x, std::int32_t world_y, std::int32_t width,
               std::int32_t height) noexcept;
  /// Keeps the viewport inside the map. Called by `set_view` and `move_view`
  /// once a viewport size is known.
  void clamp_view(std::int32_t width, std::int32_t height) noexcept;

  [[nodiscard]] std::int32_t view_x() const noexcept { return view_x_; }
  [[nodiscard]] std::int32_t view_y() const noexcept { return view_y_; }

  // -- drawing -----------------------------------------------------------

  /// Composites the ground for the current view and blits it into `target`.
  /// Must precede the sprite render pass, which loads rather than clears.
  bool draw_terrain(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
                    std::uint32_t width, std::uint32_t height);

  /// Queues every visible object layer into the sprite renderer, in the order
  /// `DATA\ZBINS.XML` prescribes. Returns the number of layers queued.
  std::size_t queue_objects(std::uint32_t width, std::uint32_t height);

  /// The ground under terrain cells `[cx0, cx1] x [cy0, cy1]` has changed --
  /// the editor painted a terrain type or the light under it -- so their
  /// cached tiles are dropped, and their neighbours' too, because a cell's
  /// transitions are composed from the eight around it. The next frame
  /// recomposites the view.
  void invalidate_ground(std::int32_t cx0, std::int32_t cy0, std::int32_t cx1, std::int32_t cy1);

  /// What `compose_zoom` drew.
  struct ZoomStats {
    /// Picture pixels at least half road: the layers of `type` 6.
    std::uint32_t road_pixels = 0;
    /// Layers with a minimap tile, and masks, that loaded.
    std::uint32_t tiles = 0;
    std::uint32_t masks = 0;
  };

  /// The whole map's ground at `1 / divisor` of the screen scale, into
  /// `out` (RGBA8), as the original's zoom map composes it
  /// (`core::compose_zoom_ground`): from `Minimap.pak`'s own art --
  /// each layer's `minimap` tile under `MINIMAP\ZOOM<divisor>\TERRAIN\`
  /// and the C and D masks beside it -- blended by the terrain vertices at
  /// twice the scale, lit, and halved. So a road is drawn wherever its
  /// vertices are, one vertex wide or not.
  ZoomStats compose_zoom(std::uint32_t divisor, core::ui::Image& out);

  struct Stats {
    std::size_t classes = 0;
    std::size_t entities = 0;
    std::size_t sheets = 0;
    std::size_t objects = 0;
    std::size_t placed = 0;      ///< objects whose class resolved to art
    std::size_t unresolved = 0;  ///< and those that did not
    std::size_t decorations = 0;
    std::size_t terrain_layers = 0;
    std::size_t last_queued = 0;
  };
  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

 private:
  /// One `.rle.mmp` in the atlas, frame (0, 0) only.
  struct Sheet {
    core::RleImage image;
    Sprite sprite;
  };

  /// One `<layer>` of an entity, resolved to a sheet and a screen offset.
  struct ArtLayer {
    std::int32_t sheet = -1;
    std::int32_t z = 0;
    std::int32_t sort_offset_x = 0;
    std::int32_t sort_offset_y = 0;
    std::int32_t offset_x = 0;  ///< layer offset plus the frame's own left
    std::int32_t offset_y = 0;
  };

  struct EntityArt {
    std::vector<ArtLayer> layers;
  };

  /// A ground texture, decoded once and kept as tightly packed RGB.
  struct GroundTexture {
    std::vector<std::uint8_t> rgb;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
  };

  struct DrawItem {
    std::uint32_t bin = 0;
    std::int32_t sort_y = 0;
    std::int32_t sort_x = 0;
    std::uint32_t order = 0;
    float x = 0.0F;
    float y = 0.0F;
    AtlasRegion region;
    PaletteRow palette;
    Rgba modulate;
  };

  // resource resolution
  [[nodiscard]] const GroundTexture* ground_texture(std::int32_t layer_z);
  /// The 64 x 46 opacity of `TERRAIN\TRANSITIONS\<style><code>.BMP`, or
  /// null when that file does not ship.
  [[nodiscard]] const std::vector<std::uint8_t>* transition_mask(char style, std::uint8_t code);
  [[nodiscard]] std::int32_t sheet_for(const std::string& path);
  [[nodiscard]] const EntityArt* art_for_entity(std::string_view entity_path);
  [[nodiscard]] const EntityArt* art_for_class(const std::string& class_name);
  /// The palette row a sheet is drawn through for a player. Rows are staged
  /// into the lookup texture and only reach the GPU at `commit_uploads`, so
  /// every row a scene can ask for must exist before the first frame:
  /// `create` is true only during `load`, and false while drawing.
  [[nodiscard]] PaletteRow palette_for(std::int32_t sheet, std::int32_t player,
                                       bool create);

  // the ground
  [[nodiscard]] const std::vector<std::uint8_t>* cell_tile(std::int32_t cx, std::int32_t cy);
  /// Recomposites `frame_` when the view or the size has moved under it.
  /// Returns true when it did, which is what tells `draw_terrain` the GPU copy
  /// of the ground is out of date.
  bool compose_ground(std::uint32_t width, std::uint32_t height);
  bool ensure_ground_texture(std::uint32_t width, std::uint32_t height);

  void add_sprite(const EntityArt& art, std::int32_t world_x, std::int32_t world_y,
                  std::int32_t player, std::int32_t width, std::int32_t height);

  Vfs* vfs_ = nullptr;
  SpriteRenderer* renderer_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;

  core::ClassGraph classes_;
  core::EntityLibrary entities_;
  core::TerrainTable terrain_;
  core::DecorTable decors_;
  core::ZBins zbins_;
  core::Season season_ = core::Season::spring;
  std::string season_name_ = "spring";

  const core::WorldMap* map_ = nullptr;

  std::deque<Sheet> sheets_;  ///< stable addresses: art layers hold indices
  std::map<std::string, std::int32_t> sheet_by_path_;
  std::map<std::string, EntityArt> art_by_entity_;
  std::map<std::string, const EntityArt*> art_by_class_;
  std::map<std::uint64_t, PaletteRow> palettes_;
  std::vector<core::Rgb888> players_;

  std::map<std::int32_t, GroundTexture> ground_;
  std::map<std::uint16_t, std::vector<std::uint8_t>> masks_;  ///< by style << 8 | code
  std::map<std::uint64_t, std::vector<std::uint8_t>> tiles_;

  std::vector<std::uint8_t> frame_;  ///< the composited ground, RGBA
  /// The world point under each pixel of `frame_`, x then y, 16 bits each:
  /// what the fog shade reads to darken a pixel by the fog at its own point.
  std::vector<std::uint16_t> frame_world_;
  /// `frame_` darkened by the fog, when a fog is set; what is uploaded then.
  std::vector<std::uint8_t> frame_shaded_;
  const core::sim::FogLight* fog_ = nullptr;
  std::uint64_t fog_generation_ = 0;
  std::uint64_t shaded_generation_ = ~0ull;
  bool shade_ground();
  std::uint32_t frame_width_ = 0;
  std::uint32_t frame_height_ = 0;
  std::int32_t frame_view_x_ = 0;
  std::int32_t frame_view_y_ = 0;
  bool frame_valid_ = false;

  SDL_GPUTexture* ground_texture_ = nullptr;
  std::uint32_t ground_texture_width_ = 0;
  std::uint32_t ground_texture_height_ = 0;
  /// Whether `ground_texture_` already holds `frame_`. The composite only
  /// changes when the camera or the window does, so re-uploading a screen of
  /// RGBA every frame was paying the cost of a pan on every frame that was not
  /// one -- at 1100x850 that is 3.7 MB of transfer buffer, allocated, filled
  /// and copied, and it was the largest single item in the frame.
  bool ground_uploaded_ = false;

  std::vector<DrawItem> items_;

  std::int32_t view_x_ = 0;
  std::int32_t view_y_ = 0;
  Stats stats_;
};

}  // namespace imperivm::platform
