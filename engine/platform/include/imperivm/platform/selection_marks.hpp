#pragma once

/// What marks an object in the world: the ring under a selected unit and the
/// health bar over it. Both are read off `gbr.exe`; what is inferred is said
/// where it is used.
///
/// **The ring is art from the data.** `UI\SELECTIONS\<n>.RLE` holds eighteen
/// ellipses, `20` to `150`, and the object's visual looks one up by its
/// class's `selection_radius` when it is set up (0x005a837d, through the
/// manager at `[0x009dcce8]` that 0x0061bd20 loads from `UI/selections`): the
/// first file whose number is at least the radius, or the largest when none
/// is (0x0061b2e0 over a sorted table, 0x0061b160). A file is an `IMGRLE`
/// frame table of **image class 6**, which `docs/formats/rle.md` did not
/// know: 8-bit frames whose payload follows the frame record in the file
/// instead of a `pamm` offset into `rle.mmp`, and no palette. The bytes are
/// coverage, 1 to 255, and the draw tints them with one colour
/// (0x0062b5b0 passes `[visual+0x5e8]` to the image's draw).
///
/// **The health bar is not art.** 0x0062ac80 draws it with pixels and lines
/// into the 16-bit back buffer; `queue_health_bar` reproduces those pixels.
/// Which objects carry one, and when, is the application's (the global mode
/// the backtick key toggles); this file is the picture.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "imperivm/platform/sprite_renderer.hpp"
#include "imperivm/platform/vfs.hpp"

namespace imperivm::platform {

/// An RGB555 pixel -- the original's back-buffer format -- as a modulate
/// colour.
[[nodiscard]] Rgba rgb555(std::uint16_t colour) noexcept;

/// Frame `frame` of an image-class-6 `IMGRLE` file as `width * height`
/// coverage bytes, 0 where the run encoding leaves a gap. False when the file
/// is not class 6 or does not parse.
bool decode_coverage_image(std::span<const std::uint8_t> file, std::uint32_t frame,
                           std::vector<std::uint8_t>& out, std::uint32_t& width,
                           std::uint32_t& height);

/// The eighteen rings, resident in a sprite renderer's atlas.
class SelectionRings {
 public:
  struct Ring {
    std::int32_t size = 0;  ///< the file's number, the radius it is for
    AtlasRegion region;
    std::int32_t width = 0;
    std::int32_t height = 0;
  };

  /// Reads every `UI\SELECTIONS\<n>.RLE` the packs hold and stages frame 0 of
  /// each in `renderer`'s atlas (committed by the next `commit_uploads`).
  ///
  /// **Frame 0 only, which is a reading.** The nine files up to `60` carry a
  /// second frame -- the same ellipse drawn dotted -- and which of the two the
  /// image's draw picks, and when, has not been read. Frame 0 is the solid
  /// one, and the only one the larger nine have.
  bool load(const Vfs& vfs, SpriteRenderer& renderer, std::string* error = nullptr);

  /// The ring for a class's `selection_radius`: the smallest at least that
  /// size, else the largest. Null when none is loaded.
  [[nodiscard]] const Ring* for_radius(std::int32_t selection_radius) const noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return rings_.size(); }

 private:
  std::vector<Ring> rings_;  ///< ascending by size
};

/// One object's health bar, in the terms of 0x0062ac80.
struct HealthBar {
  /// The class's `healthbar_type`: 1 is as wide as twice the class `radius`,
  /// 3 is 62 pixels, anything else positive 34. Zero and below draw nothing.
  std::int32_t type = 0;
  std::int32_t radius = 0;
  /// The class's `healthbaroffset`, added to the anchor's screen y: `Object`
  /// ships -70 and the animals -30.
  std::int32_t offset = 0;
  std::int32_t health = 0;
  std::int32_t max_health = 0;
  /// Drawn as a row of pips under the bar, three pixels a point, on every
  /// type but 1.
  std::int32_t stamina = 0;
  /// The small gem at the bar's right end (0x005a8436: the unit carries an
  /// item whose record says so). Not on type 1.
  bool item = false;
};

/// Queues `bar` for an object whose anchor -- its projected ground point, the
/// one its sprites are placed from -- is at `(x, y)`. Returns false, queuing
/// nothing, for a type that draws no bar or an object with no health to show.
bool queue_health_bar(SpriteRenderer& renderer, std::int32_t x, std::int32_t y,
                      const HealthBar& bar);

}  // namespace imperivm::platform
