#include "imperivm/sound/music.hpp"

namespace imperivm::sound {

bool is_music_track(std::string_view file_name) noexcept {
  return !file_name.empty() && file_name.front() != '_' && file_name != "." && file_name != "..";
}

std::optional<std::size_t> next_track(std::size_t tracks, std::optional<std::size_t> last, SoundRandom& random) {
  if (tracks == 0) return std::nullopt;
  if (tracks == 1) return 0;
  const auto hi = static_cast<std::int32_t>(tracks - 1);
  std::size_t pick = 0;
  do {
    pick = static_cast<std::size_t>(random.between(0, hi));
  } while (last.has_value() && pick == *last);
  return pick;
}

}  // namespace imperivm::sound
