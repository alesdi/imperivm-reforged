#include "imperivm/platform/fog_view.hpp"

#include <algorithm>

namespace imperivm::platform {

using core::sim::FogLight;

FogLight::Rect FogView::view_rect(const Camera& camera) noexcept {
  const core::sim::Point top_left = camera.unproject(0, 0);
  const core::sim::Point bottom_right = camera.unproject(camera.width, camera.height);
  return FogLight::Rect{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
}

void FogView::update(const core::sim::World& world, const core::sim::FogSystem* fog,
                     core::PlayerId local, bool fog_of_war, bool exploration,
                     std::int32_t map_size, const Camera& camera, std::uint64_t now_ms) {
  world_ = &world;
  map_ = fog != nullptr ? &fog->map() : nullptr;
  local_ = local;
  setup_.fog_of_war = fog_of_war;
  setup_.exploration = exploration && map_ != nullptr;
  if (map_size <= 0 || (!setup_.fog_of_war && !setup_.exploration) || camera.width <= 0 ||
      camera.height <= 0) {
    map_size_ = 0;
    light_.resize(0);
    return;
  }
  if (map_size != map_size_) {
    map_size_ = map_size;
    light_.resize(map_size);
    last_tick_ms_ = now_ms;
  }
  const FogLight::Rect view = view_rect(camera);
  // What the camera newly exposes is snapped; the first call snaps it all.
  // Either half may move the grid, so the generation counts the call.
  ++generation_;
  light_.show(world, map_, local, setup_, view);
  // The timer: one tick a period, and one at most a frame, which is what a
  // coalescing timer delivers to a slow frame.
  if (now_ms - last_tick_ms_ >= static_cast<std::uint64_t>(FogLight::period_ms(0))) {
    last_tick_ms_ = now_ms;
    light_.tick(world, map_, local, setup_, view);
  }
}

bool FogView::hides(const core::sim::WorldObject& object, core::PlayerId local) const noexcept {
  if (light_.empty() || world_ == nullptr) return false;
  return light_.hides(*world_, object, local, map_, setup_);
}

std::int32_t FogView::factor_at(core::sim::Point at) const noexcept {
  if (light_.empty()) return 32;
  return FogLight::factor_of(light_.sample(at));
}


}  // namespace imperivm::platform
