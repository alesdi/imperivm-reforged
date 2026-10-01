#include "imperivm/platform/frame_loop.hpp"

#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace imperivm::platform {
namespace {

#ifdef __EMSCRIPTEN__
/// The registered callback outlives every stack frame in the program, so it
/// cannot live on one. Emscripten calls the trampoline from JavaScript long
/// after `run_frame_loop` has returned.
FrameFn& web_frame() {
  static FrameFn frame;
  return frame;
}

void web_tick() {
  if (!web_frame() || !web_frame()()) {
    emscripten_cancel_main_loop();
    web_frame() = nullptr;
  }
}
#endif

}  // namespace

void run_frame_loop(FrameFn frame) {
  if (!frame) return;

#ifdef __EMSCRIPTEN__
  web_frame() = std::move(frame);
  // fps 0 means "use requestAnimationFrame", which is the only rate a browser
  // actually wants to be driven at. simulate_infinite_loop is 0 so that this
  // returns normally rather than unwinding the stack with an exception; the
  // caller is told what that implies by frame_loop_returns_early().
  emscripten_set_main_loop(web_tick, 0, 0);
#else
  while (frame()) {
  }
#endif
}

bool frame_loop_returns_early() noexcept {
#ifdef __EMSCRIPTEN__
  return true;
#else
  return false;
#endif
}

}  // namespace imperivm::platform
