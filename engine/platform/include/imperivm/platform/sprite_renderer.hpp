#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct SDL_GPUBuffer;
struct SDL_GPUGraphicsPipeline;

namespace imperivm::platform {

class Window;

/// A rectangle of the index atlas. Zero width means "nothing was uploaded",
/// which is what an empty sprite frame gets.
struct AtlasRegion {
  std::uint32_t page = 0;
  std::uint16_t x = 0;
  std::uint16_t y = 0;
  std::uint16_t width = 0;
  std::uint16_t height = 0;

  [[nodiscard]] bool valid() const noexcept { return width != 0 && height != 0; }
};

/// A row of the palette lookup texture: 256 RGBA entries. One image drawn for
/// four players is one atlas region and four of these.
struct PaletteRow {
  std::uint16_t row = 0;
  bool valid = false;
};

struct Rgba {
  float red = 1.0F;
  float green = 1.0F;
  float blue = 1.0F;
  float alpha = 1.0F;
};

/// The sprite renderer: an 8-bit index atlas, a palette lookup texture, and one
/// instanced draw per atlas page.
///
/// **Why the indirection exists.** Every sprite in the game is 8-bit palette
/// indexed, and team colour is a swap of palette slots 0..63 for the owning
/// player's ramp. Flattening a sprite to RGBA therefore bakes in one player's
/// colours: four players means four copies of the same art. The retail sprite
/// store is 400 MB compressed and expands to roughly 2.8 GB of RGBA even once,
/// so "once per player" is not a rounding error, it is the difference between
/// possible and impossible.
///
/// Keeping the atlas in index form fixes both at once. The atlas is R8 -- one
/// byte per pixel, the same size as the source -- and it is shared by every
/// player. What varies per player is 1 KB of palette. The fragment shader does
/// the lookup that the original's software rasteriser did with a table in L1.
///
/// **Transparency.** The format has no alpha channel; gaps in the run encoding
/// are the only transparency, and a frame records a `color_key`, the index the
/// artist painted the background in, which the specification proves never
/// appears in the compressed data. The atlas is cleared to that index and its
/// palette entry is given alpha 0. So transparency arrives through the same
/// lookup as colour, and the atlas stays one byte per pixel.
///
/// **Shadows** are separate 1-bit mask images sharing the body's canvas and
/// offsets. They go through the identical path: covered pixels become index 1,
/// uncovered index 0, and a two-entry palette makes 0 transparent and 1 black.
/// No second pipeline, no second shader.
class SpriteRenderer {
 public:
  SpriteRenderer() = default;
  ~SpriteRenderer();

  SpriteRenderer(const SpriteRenderer&) = delete;
  SpriteRenderer& operator=(const SpriteRenderer&) = delete;

  bool create(Window& window, std::string* error = nullptr);
  /// Same, but targeting an explicit texture format instead of the swapchain's.
  bool create(SDL_GPUDevice* device, std::uint32_t target_format,
              std::string* error = nullptr);
  void destroy();

  [[nodiscard]] bool ready() const noexcept { return pipeline_ != nullptr; }

  // -- resources ---------------------------------------------------------

  /// Copies a `width * height` block of palette indices into the atlas. The
  /// bytes are staged; call `commit_uploads` before the first draw that uses
  /// the region.
  AtlasRegion upload_indices(std::span<const std::uint8_t> indices, std::uint32_t width,
                             std::uint32_t height);

  /// Adds one palette row. `rgba` holds up to 256 entries as 0xAABBGGRR (the
  /// byte order of an RGBA8 texture on a little-endian host). Entries beyond
  /// the end of the span are fully transparent.
  ///
  /// **A row is its contents.** A request for entries a row already holds is
  /// answered with that row: rows are never rewritten, so two sheets whose
  /// palettes agree -- the animation sheets of one unit type, or one sheet
  /// prepared by both the map's renderer and the live world's -- share one.
  /// Without that the texture filled on a sixteen-player map late in a match,
  /// and every sheet prepared after it had no palette, so no art, and the
  /// units drawn from it were not drawn at all. Once `kPaletteRows` distinct
  /// rows are taken an invalid row still comes back; the first refusal is
  /// logged and `palette_rows_refused` counts them.
  PaletteRow upload_palette(std::span<const std::uint32_t> rgba);

  /// Distinct palette rows in the lookup texture, staged or uploaded.
  [[nodiscard]] std::uint32_t palette_rows() const noexcept { return palette_rows_used_; }
  /// Requests `upload_palette` answered with a row already holding them.
  [[nodiscard]] std::size_t palette_rows_shared() const noexcept { return palette_rows_shared_; }
  /// Requests refused because the texture was full. Zero, or art is missing.
  [[nodiscard]] std::size_t palette_rows_refused() const noexcept { return palette_rows_refused_; }
  [[nodiscard]] static constexpr std::uint32_t palette_capacity() noexcept { return kPaletteRows; }

  /// Uploads everything staged since the last call. Blocking: it submits its
  /// own command buffer and waits, which is what load-time code wants.
  bool commit_uploads(std::string* error = nullptr);

  [[nodiscard]] std::size_t atlas_pages() const noexcept { return pages_.size(); }
  [[nodiscard]] std::uint32_t atlas_page_size() const noexcept { return kPageSize; }
  /// Bytes of atlas actually allocated, for the memory budget this design exists
  /// to defend.
  [[nodiscard]] std::size_t atlas_bytes() const noexcept;

  // -- drawing -----------------------------------------------------------

  /// Starts a draw list in a `width x height` pixel space, origin top-left.
  void begin(std::uint32_t width, std::uint32_t height);

  /// Queues one sprite. `x`/`y` are the top-left corner in pixels; `modulate`
  /// multiplies the palette colour, which is how a shadow is drawn as a
  /// partially transparent black pass and how selection tints are applied.
  void draw(const AtlasRegion& region, float x, float y, PaletteRow palette,
            const Rgba& modulate = Rgba{});
  /// As above, scaled.
  void draw_scaled(const AtlasRegion& region, float x, float y, float width, float height,
                   PaletteRow palette, const Rgba& modulate = Rgba{});

  // -- primitives --------------------------------------------------------
  //
  // What the original drew straight into its 16-bit back buffer with plain
  // lines and pixels -- the in-world health bar -- and the 8-bit coverage
  // images it tints with one colour -- the selection ring -- go through this
  // same pipeline and so the same queue order as the sprites around them. The
  // art behind both is staged by `create`: one opaque texel, and a palette row
  // whose entry `i` is white at alpha `i`.

  /// A solid rectangle in `colour`, in the same pixel space as `draw`.
  void fill_rect(float x, float y, float width, float height, const Rgba& colour);

  /// An 8-bit coverage image -- indices that are alphas, 0 transparent and
  /// 255 opaque -- drawn in `colour` at its top-left corner.
  void draw_mask(const AtlasRegion& region, float x, float y, const Rgba& colour);

  [[nodiscard]] std::size_t queued() const noexcept { return instances_.size(); }

  /// Records the render pass. `clear` is applied with a clear load op; pass
  /// nullptr to draw over whatever is already in the target.
  bool render(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
              const Rgba* clear = nullptr);

 private:
  /// 2048 is the smallest maximum guaranteed by every backend SDL GPU targets,
  /// so a page always fits. At one byte per pixel a page is 4 MB.
  static constexpr std::uint32_t kPageSize = 2048;
  static constexpr std::uint32_t kPaletteEntries = 256;
  /// One row per distinct palette a scene needs: an image's own, and its
  /// retint for each player that owns something drawn from it. A map draws a
  /// few hundred distinct sheets and up to sixteen players, so 256 rows --
  /// which was enough for a single sprite sheet -- runs out on the first real
  /// map, and 2048 ran out on Balcans' sixteen seats by turn 10 while a row
  /// was spent per (sheet, player) rather than per palette. Shared, that
  /// match takes about 920 rows at its start, and a playtest save of it about
  /// 1,120 at turn 3,600; 4096 is the headroom, and the smallest 2D texture
  /// height Vulkan guarantees. At 256 RGBA entries a row, this texture is 4 MB.
  static constexpr std::uint32_t kPaletteRows = 4096;

  /// A shelf packer: rows of uniform height, filled left to right. Sprite
  /// frames of one animation are near enough the same height that shelves waste
  /// very little, and the packer stays twenty lines long.
  struct Page {
    SDL_GPUTexture* texture = nullptr;
    std::uint32_t shelf_y = 0;
    std::uint32_t shelf_height = 0;
    std::uint32_t pen_x = 0;
  };

  struct PendingTexture {
    std::uint32_t page = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t staged_at = 0;  ///< offset into staging_
  };

  struct Instance {
    float dst[4];
    float src[4];
    float modulate[4];
    float palette;
  };

  bool build_pipeline(std::string* error);
  bool add_page(std::string* error);
  bool ensure_instance_capacity(std::size_t instances);
  [[nodiscard]] bool fail(std::string* error, const char* what);

  SDL_GPUDevice* device_ = nullptr;
  std::uint32_t target_format_ = 0;

  SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
  SDL_GPUSampler* sampler_ = nullptr;
  SDL_GPUTexture* palette_texture_ = nullptr;
  SDL_GPUBuffer* instance_buffer_ = nullptr;
  std::size_t instance_capacity_ = 0;

  std::vector<Page> pages_;
  std::vector<PendingTexture> pending_textures_;
  std::vector<std::uint8_t> staging_;
  std::vector<std::uint32_t> pending_palettes_;  ///< kPaletteEntries per row
  std::uint32_t palette_rows_used_ = 0;
  std::uint32_t palette_rows_uploaded_ = 0;
  std::size_t palette_rows_shared_ = 0;
  std::size_t palette_rows_refused_ = 0;
  /// Every row's entries, kept so a request can be matched against what is
  /// already there: `kPaletteEntries` per row, in row order.
  std::vector<std::uint32_t> palette_contents_;
  /// Rows by a hash of their entries. A hit is confirmed against
  /// `palette_contents_`, so a collision costs a comparison, never a wrong row.
  std::unordered_multimap<std::uint64_t, std::uint16_t> palette_by_hash_;
  std::size_t atlas_bytes_ = 0;

  std::vector<Instance> instances_;
  std::vector<std::uint32_t> instance_pages_;
  /// The primitives' art; see `fill_rect`.
  AtlasRegion solid_;
  PaletteRow alpha_ramp_;
  std::uint32_t viewport_width_ = 0;
  std::uint32_t viewport_height_ = 0;
};

}  // namespace imperivm::platform
