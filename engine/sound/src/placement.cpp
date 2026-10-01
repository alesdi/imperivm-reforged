#include "imperivm/sound/placement.hpp"

#include <algorithm>
#include <cstdlib>

namespace imperivm::sound {

std::int32_t attenuation(const View& view, std::int32_t x, std::int32_t y) noexcept {
  if (x >= view.left && x <= view.right && y >= view.top && y <= view.bottom) return 0;
  const std::int32_t cx = view.left + (view.right - view.left) / 2;
  const std::int32_t cy = view.top + (view.bottom - view.top) / 2;
  const std::int32_t beyond = std::max(std::abs(x - cx), std::abs(y - cy)) - 500;
  if (beyond > 1200) return kSilentMillibels;
  return -5 * beyond;
}

std::int32_t pan(const View& view, std::int32_t x) noexcept {
  if (x > view.screen_left && x < view.screen_right) return 0;
  const std::int32_t edge = x <= view.screen_left ? view.screen_left : view.screen_right;
  const std::int64_t raw = static_cast<std::int64_t>(edge - x) * 10;
  const std::int32_t clamped = static_cast<std::int32_t>(std::clamp<std::int64_t>(raw, -10000, 10000));
  return view.reverse_speakers ? clamped : -clamped;
}

std::int32_t volume(std::int32_t attenuation, std::int32_t percent, std::int32_t slider) noexcept {
  return (attenuation + 6000) * percent / 100 * slider / 100 - 6000;
}

}  // namespace imperivm::sound

namespace imperivm::sound {

Followed follow(const View& view, const Placed& placed, std::int32_t slider) noexcept {
  Followed out;
  if (!placed.positioned) {
    out.volume = volume(0, 100, slider);
    return out;
  }
  out.volume = volume(attenuation(view, placed.x, placed.y), 100, slider);
  out.pan = pan(view, placed.x);
  out.pans = true;
  return out;
}

}  // namespace imperivm::sound
