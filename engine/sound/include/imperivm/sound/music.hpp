#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/sound/sound_entity.hpp"

namespace imperivm::sound {

/// When the in-game music is looked at, in milliseconds of game time: every
/// two seconds (0x00550fbb schedules the next look 2,000 ms on, on the
/// game's own scheduler at `[game+0x129c]`).
inline constexpr std::int64_t kMusicCheckMs = 2000;
/// The game clock must be past this before a track starts (0x00550fab:
/// `[game+0x1258]`, the clock `World::time()` is, compared with 100).
inline constexpr std::int64_t kMusicStartMs = 100;
/// The menus look at their music once in this many passes of their loop
/// (0x00748812 sets the countdown, 0x0074886a counts it down).
inline constexpr std::int32_t kMenuMusicPasses = 500;

/// Whether a file of `music/` is an in-game track: every name that does not
/// begin with `_` (0x00551150 skips those at 0x005511e4), which in the
/// shipped installation is the seven `GBR_TRACK_<n>.ogg` and not `_menu.ogg`.
[[nodiscard]] bool is_music_track(std::string_view file_name) noexcept;

/// The in-game player's choice of the next track (0x00550e80).
///
/// No tracks: nothing. One: that one, with no draw. More: `Random(0, n - 1)`
/// redrawn until it differs from `last`, so no track plays twice in a row.
/// The draws are the original's unsynchronised generator's; here they are
/// the presentation's own `SoundRandom`, never the world's.
[[nodiscard]] std::optional<std::size_t> next_track(std::size_t tracks, std::optional<std::size_t> last,
                                                    SoundRandom& random);

}  // namespace imperivm::sound
