#pragma once

/// The fog of war as the local player sees it: presentation, never hashed.
///
/// `gbr.exe` keeps two grids. The **exploration map** -- 1024-unit cells, two
/// bits per player, the partial cells' fine records, what `IsExplored` and
/// `ExploreCircle` read -- is `sim/fog.hpp`'s `ExplorationMap`, and it is
/// saved. The **light grid** is the fog manager's (`CVXFog`, 0x00606970):
/// 16-unit cells for the local player only, rebuilt from the exploration map
/// and the objects on the local player's side, slid to its target over four
/// ticks, and read by the draw and the hide test. That grid, its rules and
/// its numbers are `sim/fog_light.hpp`'s `FogLight`; this is the application's
/// hold on one -- the camera's rect fed to it, the timer, the overlay the
/// renderer draws, and the hide predicate the world view asks.
///
/// The overlay is one pixel per 16-unit cell over the cells the view touches,
/// black at an alpha of `(32 - f) / 32` for the cell's darkening factor `f`
/// (`FogLight::factor_of`): nothing where the word is lit, solid where it is
/// black, 15/32 at the explored floor. Drawn stretched so each texel's centre
/// lands on its cell's corner -- the word is the corner's -- and sampled
/// linearly between, which stands in for the draw's own per-pixel
/// interpolation (0x00604bb0) and leaves out its dither.

#include <cstdint>
#include <vector>

#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/fog_light.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/ui/paint.hpp"
#include "imperivm/platform/world_view.hpp"

namespace imperivm::platform {

class FogView {
 public:
  static constexpr std::int32_t kCell = core::sim::FogLight::kCell;

  /// Once a frame. Sizes the grid to the map, snaps what the camera newly
  /// exposes, and runs the timer: a tick every `FogLight::period_ms` of
  /// `now_ms`, at most one a frame. A map extent of zero, or both switches
  /// off, empties the view -- nothing drawn, nothing hidden. Without a fog
  /// system the exploration switch reads as off.
  void update(const core::sim::World& world, const core::sim::FogSystem* fog,
              core::PlayerId local, bool fog_of_war, bool exploration, std::int32_t map_size,
              const Camera& camera, std::uint64_t now_ms);

  /// Forget the grid: the next `update` sizes and snaps it afresh. For a
  /// new game or a loaded one, whose ground has nothing to fade from.
  void reset() noexcept { map_size_ = 0; }

  [[nodiscard]] bool empty() const noexcept { return light_.empty(); }
  [[nodiscard]] const core::sim::FogLight& light() const noexcept { return light_; }
  /// Counts every `update` that could have moved the displayed grid -- a
  /// snap or a tick -- so a renderer holding a shade of it knows to redo it.
  [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
  /// The darkening factor over 32 at a world point: the displayed grid
  /// sampled bilinearly, the way the ground draw and the sprites take it
  /// (0x00604a40). 32 with no fog.
  [[nodiscard]] std::int32_t factor_at(core::sim::Point at) const noexcept;

  /// Whether the view hides `object` for `local`: `FogLight::hides` over the
  /// world `update` was last given.
  [[nodiscard]] bool hides(const core::sim::WorldObject& object, core::PlayerId local) const noexcept;


 private:
  [[nodiscard]] static core::sim::FogLight::Rect view_rect(const Camera& camera) noexcept;

  core::sim::FogLight light_;
  core::sim::FogLight::Setup setup_;
  const core::sim::World* world_ = nullptr;
  const core::sim::ExplorationMap* map_ = nullptr;
  core::PlayerId local_ = core::kNoPlayer;
  std::int32_t map_size_ = 0;
  std::uint64_t last_tick_ms_ = 0;
  std::uint64_t generation_ = 0;
};

}  // namespace imperivm::platform
