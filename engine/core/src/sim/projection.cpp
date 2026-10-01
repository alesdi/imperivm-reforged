#include "imperivm/core/sim/projection.hpp"

#include "imperivm/core/sim/flying.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::sim {

Point world_to_screen(const World& world, Point at) noexcept {
  return Point{at.x, project_scale(at.y) - terrain_height(world, at)};
}

Point screen_to_world(const World& world, Point screen) noexcept {
  return screen_to_world_over(
      screen, [&](Point at) { return terrain_height(world, at); });
}

}  // namespace imperivm::core::sim
