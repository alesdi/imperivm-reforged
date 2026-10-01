#pragma once

#include <cstddef>
#include <cstdint>

namespace imperivm::sound {

/// Where the player is looking, in world units: the view's four screen
/// corners unprojected and boxed, and the world x of its left and right
/// edges (the projection keeps x, so these are the screen's own edges).
struct View {
  std::int32_t left = 0;
  std::int32_t top = 0;
  std::int32_t right = 0;
  std::int32_t bottom = 0;
  std::int32_t screen_left = 0;
  std::int32_t screen_right = 0;
  bool reverse_speakers = false;
};

/// Hundredths of a decibel, DirectSound's unit, which the original computes
/// in throughout (0x006b0650, 0x006b0b22). 0 is the sound as recorded;
/// -6000 is the quietest a slider or distance takes it.
inline constexpr std::int32_t kSilentMillibels = -6000;

/// A positioned sound's distance attenuation (0x006b0650): nothing inside
/// the view; outside, five millibels per world unit of the Chebyshev
/// distance from the view's centre beyond 500, and -6000 once that is past
/// 1,200. Not clamped above: a point outside a narrow view but within 500 of
/// its centre comes out positive, as the original's does, and the mixer
/// plays anything above 0 at 0.
[[nodiscard]] std::int32_t attenuation(const View& view, std::int32_t x, std::int32_t y) noexcept;

/// A positioned sound's pan (0x006b0650): centred while `x` is between the
/// screen's edges, otherwise ten millibels per unit past the edge, at most
/// 10,000, towards that side -- reversed when the options swap the speakers.
/// Negative is left, as DirectSound's is.
[[nodiscard]] std::int32_t pan(const View& view, std::int32_t x) noexcept;

/// The volume a sound plays at (0x006b0b22): the attenuation lifted onto a
/// 60 dB scale, scaled by the call's own percentage and the type's slider,
/// and lowered again. `((attenuation + 6000) * percent / 100) * slider / 100
/// - 6000`, in integers; a slider of 68 plays a sound on screen at -1920.
[[nodiscard]] std::int32_t volume(std::int32_t attenuation, std::int32_t percent,
                                  std::int32_t slider) noexcept;

/// Where a live sound was played from, kept so it can follow the view.
struct Placed {
  bool positioned = false;  ///< false: x = -1, a sound with no place
  std::int32_t x = 0;
  std::int32_t y = 0;
};

/// What a live sound becomes when the view moves (0x006b0810, run from the
/// view setter 0x006265c0 and after an options change).
struct Followed {
  std::int32_t volume = 0;
  std::int32_t pan = 0;
  /// Whether the pan is set at all: an unpositioned sound keeps its own.
  bool pans = false;
};

/// The original re-applies, to each of its first 64 live channels, the
/// level `volume(attenuation, 100, slider)` and the pan from the sound's
/// stored point against the view as it now is; to an unpositioned sound,
/// `volume(0, 100, slider)` and no pan. **The call's own percentage is not
/// kept**: 0x006b0810 scales by the slider alone. Every caller of the play
/// entry point passes 100, so nothing sounds different for it.
[[nodiscard]] Followed follow(const View& view, const Placed& placed, std::int32_t slider) noexcept;

/// How many live channels the re-application walks (0x006b081b).
inline constexpr std::size_t kFollowedChannels = 64;

}  // namespace imperivm::sound
