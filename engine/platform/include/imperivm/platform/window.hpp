#pragma once

#include <cstdint>
#include <string>

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUTexture;
struct SDL_GPUCommandBuffer;

namespace imperivm::platform {

/// An SDL window plus the GPU device that owns its swapchain -- or, headless,
/// the device alone and a texture where the swapchain would be.
///
/// The original engine drove DirectDraw surfaces at a fixed 16-bit colour depth
/// and refused to start unless the *desktop* matched, which is the single
/// biggest reason it is fragile on modern systems. Nothing here has a
/// resolution list, a colour depth requirement, or an opinion about the
/// desktop: the window is whatever size it is, the swapchain follows it, and
/// the pixel format comes from the driver.
///
/// The device is created for the shader formats this repository ships (SPIR-V
/// and MSL), so SDL picks Vulkan or Metal accordingly. See
/// docs/engine/rendering.md for why those two and what is missing.
class Window {
 public:
  struct Options {
    const char* title = "Imperivm Reforged";
    int width = 1280;
    int height = 720;
    bool resizable = true;
    bool borderless = false;
    bool fullscreen = false;
    /// Ask for the backing-store resolution on a HiDPI display. The swapchain
    /// is then in pixels, not points, and `pixel_width()` reports the former.
    bool high_dpi = true;
    /// VSYNC presents on the refresh; IMMEDIATE does not wait.
    bool vsync = true;
    /// Ask SDL for backend validation layers and shader debug info.
    bool debug = false;
    /// No window at all: the GPU device alone, and a texture of `width` x
    /// `height` pixels standing in for the swapchain. Everything above draws
    /// exactly as it would into a window -- the frame is still rendered into
    /// the `RenderTarget` and blitted onward -- but nothing is shown, nothing
    /// takes focus, and `begin_frame` paces itself to `headless_hz` instead of
    /// waiting on a display's refresh. For test runs and scripted captures.
    ///
    /// SDL's video subsystem must still be initialised with the platform's
    /// real driver: SDL will not create a GPU device without one, and the
    /// `dummy` and `offscreen` drivers offer no GPU backend. See
    /// `headless_requested` and `prepare_headless` for the rest.
    bool headless = false;
    /// The frame rate a headless window keeps. A windowed run is held to the
    /// display's refresh by the swapchain; this is the same bound, so that
    /// `--frames N` spans about as much real time -- and so as many turns --
    /// with or without a window.
    int headless_hz = 60;
  };

  /// One frame in flight: the command buffer, and the texture to draw into.
  ///
  /// `swapchain` is null when the window is minimised or the swapchain is
  /// otherwise unavailable. That is not an error -- it is the normal way a
  /// compositor says "there is nowhere to put this frame" -- so the caller
  /// should skip its render pass and still submit.
  struct Frame {
    SDL_GPUCommandBuffer* commands = nullptr;
    SDL_GPUTexture* swapchain = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool drawable() const noexcept { return swapchain != nullptr; }
  };

  Window() = default;
  ~Window();

  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;

  /// Creates the window and its GPU device. Returns false and leaves the object
  /// unusable on failure; call error() for the reason.
  bool open(const Options& options);
  /// Convenience for the common case.
  bool open(const char* title, int width, int height);
  void close();

  [[nodiscard]] bool is_open() const noexcept { return window_ != nullptr || headless_; }
  [[nodiscard]] bool headless() const noexcept { return headless_; }
  [[nodiscard]] const char* error() const noexcept { return error_.c_str(); }
  /// Null in a headless run.
  [[nodiscard]] SDL_Window* handle() const noexcept { return window_; }
  [[nodiscard]] SDL_GPUDevice* device() const noexcept { return device_; }

  /// The swapchain's texture format, for building pipelines that target it.
  /// Returned as the underlying integer so this header need not include
  /// SDL_gpu.h; callers cast it back to SDL_GPUTextureFormat.
  [[nodiscard]] std::uint32_t swapchain_format() const noexcept;

  /// Size of the drawable in *pixels*, which on a HiDPI display is not the
  /// size in window coordinates. Everything the renderer does is in pixels.
  [[nodiscard]] int pixel_width() const noexcept;
  [[nodiscard]] int pixel_height() const noexcept;

  void set_fullscreen(bool on);
  void set_borderless(bool on);
  /// Resize the window, in window coordinates; the options dialog's
  /// resolution. No effect while fullscreen.
  void set_size(int width, int height);

  /// Acquires a command buffer and the swapchain texture. SDL resizes the
  /// swapchain here, so a resize needs no handling beyond reading the width and
  /// height back out of the returned frame every time.
  [[nodiscard]] bool begin_frame(Frame& frame);

  /// Submits the frame. Always call it, even for a frame that was not
  /// drawable, or the command buffer leaks.
  void end_frame(Frame& frame);

 private:
  bool open_headless(const Options& options);

  SDL_Window* window_ = nullptr;
  SDL_GPUDevice* device_ = nullptr;
  std::string error_;
  // -- headless: the texture that stands in for the swapchain, its size, and
  // when the next frame is due.
  bool headless_ = false;
  SDL_GPUTexture* stand_in_ = nullptr;
  std::uint32_t stand_in_width_ = 0;
  std::uint32_t stand_in_height_ = 0;
  std::uint64_t frame_period_ns_ = 0;
  std::uint64_t next_frame_ns_ = 0;
};

/// Whether this run asked for no window: `--headless` among `argv`, or
/// `IMPERIVM_HEADLESS` set to anything but empty or `0`.
[[nodiscard]] bool headless_requested(int argc, char** argv);

/// What a headless run must settle before `SDL_Init`: the process stays a
/// background process (on macOS SDL otherwise activates it, and with it the
/// Dock, taking focus from whatever the user was in, even with no window), and
/// audio goes to SDL's `dummy` driver, so no sound device is opened.
void prepare_headless();

}  // namespace imperivm::platform
