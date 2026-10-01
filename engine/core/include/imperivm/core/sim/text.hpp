#pragma once

/// The string and localisation slice of the host surface.
///
/// Seven entry points over roughly 450 call sites, and they have one thing in
/// common that makes them worth keeping together and away from everything
/// else: **none of them may influence hashed state.**
///
/// `Translate` returns whatever the installed language pack says. Two players
/// in the same lockstep match can be running different packs, so a simulation
/// that compared a translated string, or folded one into a hash, would
/// desynchronise on locale alone. `pr` is a debug print and is not state at
/// all. `game/localization.hpp` says the same thing from the other side, and
/// `sim/host_context.hpp` keeps the table and the debug sink off `World` so
/// that the rule is structural rather than a convention to remember.
///
/// | Entry point | Sites | Notes |
/// |---|---:|---|
/// | `pr` /1 | 155 | debug print; a no-op without a sink |
/// | `Translate` /1 | 142 | table lookup, falls back to the source string |
/// | `Translatef` /2, /3 | 76 | translate, then substitute `%s1`-style slots |
/// | `ParseStr` /2 | 71 | split one comma-separated token off, in place |
/// | `Str2Int` /1 | -- | |
/// | `StrLen` /1 | -- | |
/// | `SS_STR` /1 | -- | squad-state name; needs the AI profile |
/// | `ClearDebug` /0 | -- | |

#include <cstddef>

namespace imperivm::core::script {
class HostRegistry;
}

namespace imperivm::core::sim {

/// Implement the string and localisation entry points on `registry`.
///
/// Returns the number defined, so a caller can assert the count rather than
/// trust it -- the convention the other domains follow.
std::size_t register_text_host(script::HostRegistry& registry);

}  // namespace imperivm::core::sim
