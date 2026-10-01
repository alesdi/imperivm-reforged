#pragma once

#include <functional>

namespace imperivm::platform {

/// Runs one frame. Return false to stop.
using FrameFn = std::function<bool()>;

/// Drives `frame` until it returns false.
///
/// The web is the reason this is a function rather than a `while` in main.
/// A browser tab has one thread and one event loop, and a C++ loop that never
/// returns to it never repaints, never delivers input and eventually gets the
/// tab killed. Emscripten's answer is `emscripten_set_main_loop`: hand the
/// runtime one function, return, and let the browser call it per animation
/// frame.
///
/// So the shape that serves both is "give the platform a callback". Natively it
/// is called in a loop that this function owns; on the web it is registered and
/// this function returns immediately.
///
/// That difference is visible to the caller in exactly one way, and pretending
/// otherwise causes a use-after-free the first time the loop touches anything
/// main owns: see `frame_loop_returns_early`.
void run_frame_loop(FrameFn frame);

/// True when `run_frame_loop` returns before the loop has finished -- that is,
/// on the web, where the browser drives it afterwards.
///
/// A caller must keep everything the frame callback captures alive past its own
/// return when this is true. `engine/app/main.cpp` shows the one-line way:
/// build the application on the heap and, in this case, deliberately leak it,
/// because on the web the page's lifetime *is* the process lifetime.
[[nodiscard]] bool frame_loop_returns_early() noexcept;

}  // namespace imperivm::platform
