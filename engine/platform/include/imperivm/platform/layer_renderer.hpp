#pragma once

/// Putting CPU-composited RGBA layers on the screen, blended.
///
/// The interface is drawn on the CPU (`imperivm/core/ui/paint.hpp`) into
/// RGBA8 canvases: the two bars, a tooltip, and the menus. The bars are
/// opaque strips and a blit would do for them; a menu is not -- its dark
/// frame halves the world beneath (`CUIDarkFrame`, 0x00663c70) and its frame
/// art is colour-keyed around the corners -- so the layer has to be
/// **blended** over what is already in the target, and `SDL_BlitGPUTexture`
/// copies. This is the second pipeline `ui/paint.hpp` said the bars did not
/// need, and the menus do: the sprite vertex stage as it is, a fragment
/// stage that samples the texel and nothing else, one instanced quad per
/// layer, source-over.
///
/// Textures are the caller's; this draws them. `draw` queues, `render`
/// records one render pass over the queue and clears it.

#include <cstdint>
#include <string>
#include <vector>

struct SDL_GPUDevice;
struct SDL_GPUCommandBuffer;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct SDL_GPUBuffer;
struct SDL_GPUGraphicsPipeline;

namespace imperivm::platform {

class LayerRenderer {
 public:
  LayerRenderer() = default;
  ~LayerRenderer();

  LayerRenderer(const LayerRenderer&) = delete;
  LayerRenderer& operator=(const LayerRenderer&) = delete;

  bool create(SDL_GPUDevice* device, std::uint32_t target_format, std::string* error = nullptr);
  void destroy();
  [[nodiscard]] bool ready() const noexcept { return pipeline_ != nullptr; }

  /// Queue `texture` (an RGBA8 sampler texture of `width` x `height`) with
  /// its top-left at `(x, y)` in target pixels, drawn at its own size.
  void draw(SDL_GPUTexture* texture, std::uint32_t width, std::uint32_t height, std::int32_t x,
            std::int32_t y);
  /// Queue `texture` stretched over `draw_width` x `draw_height` at
  /// `(x, y)`, sampled linearly when `smooth`. Nothing draws through it
  /// today: the fog overlay that did is shaded into the ground now, since a
  /// picture stretched flat cannot follow a ground the height lifts.
  void draw_scaled(SDL_GPUTexture* texture, std::int32_t x, std::int32_t y,
                   std::int32_t draw_width, std::int32_t draw_height, bool smooth);

  /// Record the pass over `target`, sized `viewport_width` x
  /// `viewport_height`, loading what is there. Empties the queue.
  bool render(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
              std::uint32_t viewport_width, std::uint32_t viewport_height,
              std::string* error = nullptr);

 private:
  struct Instance {
    float dst[4];
    float src[4];
    float modulate[4];
    float palette;
  };
  struct Queued {
    SDL_GPUTexture* texture;
    Instance instance;
    bool smooth = false;
  };

  bool build_pipeline(std::string* error);
  bool ensure_capacity(std::size_t instances);

  SDL_GPUDevice* device_ = nullptr;
  std::uint32_t target_format_ = 0;
  SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
  SDL_GPUSampler* sampler_ = nullptr;
  SDL_GPUSampler* smooth_sampler_ = nullptr;
  SDL_GPUBuffer* instance_buffer_ = nullptr;
  std::size_t instance_capacity_ = 0;
  std::vector<Queued> queue_;
};

}  // namespace imperivm::platform
