#include "imperivm/platform/window.hpp"

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>

#include "imperivm/platform/render_target.hpp"

namespace imperivm::platform {
namespace {

// Ask for both forms we ship. SDL picks the backend that can consume one of
// them: Metal on Apple platforms, Vulkan elsewhere. Adding DXIL here would
// enable D3D12 as soon as there is a Windows job to compile it.
constexpr SDL_GPUShaderFormat kShaderFormats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL;

}  // namespace

bool headless_requested(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--headless") == 0) return true;
  }
  const char* env = std::getenv("IMPERIVM_HEADLESS");
  return env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

void prepare_headless() {
  // Overrides, not defaults: a variable left in the environment by some other
  // run must not be able to put a sound on the speakers or take the focus.
  SDL_SetHintWithPriority(SDL_HINT_MAC_BACKGROUND_APP, "1", SDL_HINT_OVERRIDE);
  SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
}

Window::~Window() { close(); }

bool Window::open(const char* title, int width, int height) {
  Options options;
  options.title = title;
  options.width = width;
  options.height = height;
  return open(options);
}

bool Window::open(const Options& options) {
  close();
  if (options.headless) return open_headless(options);

  SDL_WindowFlags flags = 0;
  if (options.resizable) flags |= SDL_WINDOW_RESIZABLE;
  if (options.borderless) flags |= SDL_WINDOW_BORDERLESS;
  if (options.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
  if (options.high_dpi) flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;

  window_ = SDL_CreateWindow(options.title, options.width, options.height, flags);
  if (window_ == nullptr) {
    error_ = std::string("SDL_CreateWindow: ") + SDL_GetError();
    return false;
  }

  device_ = SDL_CreateGPUDevice(kShaderFormats, options.debug, nullptr);
  if (device_ == nullptr) {
    error_ = std::string("SDL_CreateGPUDevice: ") + SDL_GetError();
    close();
    return false;
  }

  if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
    error_ = std::string("SDL_ClaimWindowForGPUDevice: ") + SDL_GetError();
    close();
    return false;
  }

  // Present mode is a request, not a demand: ask, and fall back to the one
  // every driver must support rather than failing to start over it.
  const SDL_GPUPresentMode wanted =
      options.vsync ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;
  const SDL_GPUPresentMode present =
      SDL_WindowSupportsGPUPresentMode(device_, window_, wanted) ? wanted
                                                                : SDL_GPU_PRESENTMODE_VSYNC;
  SDL_SetGPUSwapchainParameters(device_, window_, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present);

  SDL_Log("gpu: %s, swapchain %dx%d px", SDL_GetGPUDeviceDriver(device_), pixel_width(),
          pixel_height());
  return true;
}

bool Window::open_headless(const Options& options) {
  if (options.width <= 0 || options.height <= 0) {
    error_ = "headless: the frame needs a width and a height";
    return false;
  }
  // No window to claim: the device is created on its own, which Metal and
  // Vulkan both allow. SDL still wants its video subsystem up to pick the
  // backend, and only a real platform driver offers one.
  device_ = SDL_CreateGPUDevice(kShaderFormats, options.debug, nullptr);
  if (device_ == nullptr) {
    error_ = std::string("SDL_CreateGPUDevice (headless; SDL_VIDEO_DRIVER must be the "
                         "platform's own, not dummy or offscreen): ") +
             SDL_GetError();
    return false;
  }

  // The stand-in for the swapchain. Nothing reads it; it is here so that the
  // frame goes through exactly the passes a windowed frame goes through,
  // including the final blit, and a headless run exercises the same code.
  SDL_GPUTextureCreateInfo info{};
  info.type = SDL_GPU_TEXTURETYPE_2D;
  info.format = static_cast<SDL_GPUTextureFormat>(render_target_format());
  info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
  info.width = static_cast<Uint32>(options.width);
  info.height = static_cast<Uint32>(options.height);
  info.layer_count_or_depth = 1;
  info.num_levels = 1;
  stand_in_ = SDL_CreateGPUTexture(device_, &info);
  if (stand_in_ == nullptr) {
    error_ = std::string("SDL_CreateGPUTexture (headless frame): ") + SDL_GetError();
    close();
    return false;
  }
  headless_ = true;
  stand_in_width_ = info.width;
  stand_in_height_ = info.height;
  frame_period_ns_ = options.headless_hz > 0
                        ? static_cast<std::uint64_t>(SDL_NS_PER_SECOND) / static_cast<std::uint64_t>(options.headless_hz)
                        : 0;
  next_frame_ns_ = 0;

  SDL_Log("gpu: %s, headless %ux%u px at %d Hz, no window", SDL_GetGPUDeviceDriver(device_),
          stand_in_width_, stand_in_height_, options.headless_hz);
  return true;
}

void Window::close() {
  if (device_ != nullptr && stand_in_ != nullptr) {
    SDL_WaitForGPUIdle(device_);
    SDL_ReleaseGPUTexture(device_, stand_in_);
  }
  stand_in_ = nullptr;
  headless_ = false;
  stand_in_width_ = 0;
  stand_in_height_ = 0;
  if (device_ != nullptr) {
    // The device may still be chewing on submitted work; releasing the window
    // out from under it is how you get a crash on quit.
    SDL_WaitForGPUIdle(device_);
    if (window_ != nullptr) SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
}

std::uint32_t Window::swapchain_format() const noexcept {
  if (headless_) return render_target_format();
  if (device_ == nullptr || window_ == nullptr) {
    return static_cast<std::uint32_t>(SDL_GPU_TEXTUREFORMAT_INVALID);
  }
  return static_cast<std::uint32_t>(SDL_GetGPUSwapchainTextureFormat(device_, window_));
}

int Window::pixel_width() const noexcept {
  if (headless_) return static_cast<int>(stand_in_width_);
  int w = 0;
  int h = 0;
  if (window_ != nullptr) SDL_GetWindowSizeInPixels(window_, &w, &h);
  return w;
}

int Window::pixel_height() const noexcept {
  if (headless_) return static_cast<int>(stand_in_height_);
  int w = 0;
  int h = 0;
  if (window_ != nullptr) SDL_GetWindowSizeInPixels(window_, &w, &h);
  return h;
}

void Window::set_fullscreen(bool on) {
  if (window_ != nullptr) SDL_SetWindowFullscreen(window_, on);
}

void Window::set_size(int width, int height) {
  if (window_ != nullptr && width > 0 && height > 0) SDL_SetWindowSize(window_, width, height);
}

void Window::set_borderless(bool on) {
  if (window_ != nullptr) SDL_SetWindowBordered(window_, !on);
}

bool Window::begin_frame(Frame& frame) {
  frame = Frame{};
  if (headless_ && device_ != nullptr) {
    // Where a windowed frame waits for the display, this one waits for its
    // slot. A frame that is already late runs at once, and one that is more
    // than a whole period late restarts the schedule rather than letting the
    // next few run back to back to catch up -- a display does not do that
    // either.
    if (frame_period_ns_ > 0) {
      const std::uint64_t now = SDL_GetTicksNS();
      if (next_frame_ns_ == 0 || now > next_frame_ns_ + frame_period_ns_) {
        next_frame_ns_ = now;
      } else if (now < next_frame_ns_) {
        SDL_DelayPrecise(next_frame_ns_ - now);
      }
      next_frame_ns_ += frame_period_ns_;
    }
    frame.commands = SDL_AcquireGPUCommandBuffer(device_);
    if (frame.commands == nullptr) {
      error_ = std::string("SDL_AcquireGPUCommandBuffer: ") + SDL_GetError();
      return false;
    }
    frame.swapchain = stand_in_;
    frame.width = stand_in_width_;
    frame.height = stand_in_height_;
    return true;
  }
  if (device_ == nullptr || window_ == nullptr) return false;

  frame.commands = SDL_AcquireGPUCommandBuffer(device_);
  if (frame.commands == nullptr) {
    error_ = std::string("SDL_AcquireGPUCommandBuffer: ") + SDL_GetError();
    return false;
  }

  // This is where a resize is absorbed: SDL rebuilds the swapchain to match the
  // window and reports the new size, so nothing above needs a resize event.
  // A null texture with a true return means "minimised, nowhere to draw".
  if (!SDL_WaitAndAcquireGPUSwapchainTexture(frame.commands, window_, &frame.swapchain,
                                             &frame.width, &frame.height)) {
    error_ = std::string("SDL_WaitAndAcquireGPUSwapchainTexture: ") + SDL_GetError();
    SDL_CancelGPUCommandBuffer(frame.commands);
    frame.commands = nullptr;
    return false;
  }
  return true;
}

void Window::end_frame(Frame& frame) {
  if (frame.commands == nullptr) return;
  SDL_SubmitGPUCommandBuffer(frame.commands);
  frame = Frame{};
}

}  // namespace imperivm::platform
