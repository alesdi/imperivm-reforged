#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct SDL_GPUDevice;
struct SDL_GPUTexture;
struct SDL_GPUCommandBuffer;

namespace imperivm::platform {

/// An offscreen colour target the size of the drawable.
///
/// The renderer draws here rather than straight into the swapchain, for two
/// reasons. A swapchain texture is not reliably readable, and being able to
/// read the frame back is what makes an automated visual check possible at all
/// -- rendering something and then *looking at it* is the only test that
/// distinguishes a correct sprite from a plausible one. It also decouples the
/// internal resolution from the window, which the game will want later.
///
/// The cost is one full-screen blit per frame, which is nothing.
class RenderTarget {
 public:
  RenderTarget() = default;
  ~RenderTarget();

  RenderTarget(const RenderTarget&) = delete;
  RenderTarget& operator=(const RenderTarget&) = delete;

  /// Creates or resizes the texture. Cheap and idempotent when the size has
  /// not changed, so it can be called every frame with the swapchain's size.
  bool ensure(SDL_GPUDevice* device, std::uint32_t width, std::uint32_t height);
  void destroy();

  [[nodiscard]] SDL_GPUTexture* texture() const noexcept { return texture_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

  /// Scales this target onto `destination`, which is normally the swapchain.
  void blit_to(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* destination,
               std::uint32_t destination_width, std::uint32_t destination_height) const;

  /// Reads the texture back as tightly packed RGBA8. Blocking; for screenshots
  /// and tests, not for the frame loop.
  bool download(std::vector<std::uint8_t>& rgba, std::string* error = nullptr) const;

 private:
  SDL_GPUDevice* device_ = nullptr;
  SDL_GPUTexture* texture_ = nullptr;
  std::uint32_t width_ = 0;
  std::uint32_t height_ = 0;
};

/// The format `RenderTarget` uses, as the SDL enumerator's underlying value,
/// for handing to `SpriteRenderer::create`.
[[nodiscard]] std::uint32_t render_target_format() noexcept;

/// Writes tightly packed RGBA8 as a PNG.
///
/// Deliberately stores rather than compresses: a PNG's zlib stream may use
/// uncompressed deflate blocks, which costs a few percent of file size on a
/// screenshot and saves the project a compression library. SDL3 has no PNG
/// writer, and pulling one in for a debug feature would be a poor trade.
bool save_png(const char* path, const std::uint8_t* rgba, std::uint32_t width,
              std::uint32_t height, std::string* error = nullptr);

}  // namespace imperivm::platform
