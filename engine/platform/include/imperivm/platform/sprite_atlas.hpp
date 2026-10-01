#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "imperivm/core/formats/color.hpp"
#include "imperivm/core/formats/rle.hpp"
#include "imperivm/platform/bytes.hpp"
#include "imperivm/platform/sprite_renderer.hpp"

namespace imperivm::platform {

/// One frame of an uploaded sprite.
///
/// `left`/`top` are the frame's position on the *shared canvas*, straight out
/// of the file. Every frame of an image, and every frame of its shadow image,
/// is expressed in that one coordinate system, which is why a body and its
/// shadow composite by bounding box with no alignment arithmetic at all.
struct SpriteFrame {
  AtlasRegion region;
  std::int32_t left = 0;
  std::int32_t top = 0;
};

/// A whole `.rle.mmp` living in the atlas.
struct Sprite {
  std::uint32_t columns = 0;  ///< the facing direction, in unit and building art
  std::uint32_t rows = 0;     ///< the animation step
  std::vector<SpriteFrame> frames;  ///< rows * columns, row major

  /// The palette exactly as shipped. For a player-colour image that means the
  /// neutral default the artist painted into slots 0..63.
  PaletteRow neutral;

  /// True when the image is class 2 and slots 0..63 are the team block, so a
  /// per-player row from `upload_team_palette` is meaningful.
  bool player_color = false;
  /// True when the image is a 1-bit coverage mask, i.e. a shadow.
  bool mask = false;

  /// Union of every populated frame's bounding box, in canvas coordinates.
  std::int32_t canvas_left = 0;
  std::int32_t canvas_top = 0;
  std::uint32_t canvas_width = 0;
  std::uint32_t canvas_height = 0;

  /// The index the atlas was cleared to, whose palette entry has alpha 0.
  std::uint8_t transparent_index = 0;

  [[nodiscard]] const SpriteFrame* at(std::uint32_t row, std::uint32_t column) const {
    const std::size_t i = static_cast<std::size_t>(row) * columns + column;
    return i < frames.size() ? &frames[i] : nullptr;
  }
};

/// Decodes every frame of `image` into the renderer's index atlas and adds the
/// shipped palette as one lookup row.
///
/// Nothing is expanded to RGBA: the atlas receives the palette indices the file
/// stores, one byte per pixel, which is what keeps the whole sprite store
/// affordable and what makes a per-player palette swap possible at all.
///
/// `store` is the memory-mapped `rle.mmp`; frames name absolute offsets into
/// it. Call `SpriteRenderer::commit_uploads` afterwards.
/// `first_frame_only` uploads frame (0, 0) and nothing else. A map draws several
/// hundred distinct sheets in one scene and holds every entity in its first
/// pose; uploading every facing and every animation step of all of them costs
/// tens of megabytes of atlas for pixels no frame will sample. Pose and facing
/// selection is a simulation question and will want the whole sheet back.
bool upload_sprite(SpriteRenderer& renderer, const core::RleImage& image, ByteSpan store,
                   Sprite& out, std::string* error = nullptr,
                   bool first_frame_only = false);

/// Sets `out` up for **on-demand** frame upload, and uploads no pixels at all.
///
/// A live scene cannot afford either of `upload_sprite`'s two settings. Whole
/// sheets are far too much -- an eight-facing, fourteen-step walk cycle is 112
/// frames, and a map's several hundred sheets come to hundreds of megabytes of
/// atlas -- while frame (0, 0) alone freezes every unit in one pose facing one
/// way. What a simulation actually samples is a handful of (row, column) cells
/// per sheet, discovered as objects animate and turn.
///
/// So this does everything that is a property of the *whole image* and has to
/// be settled before any frame is staged: it chooses the transparent index by
/// scanning every frame's runs (an index that is free in frame (0, 0) but used
/// by frame 40 would punch holes in frame 40, and the choice cannot be revised
/// once anything is in the atlas), adds the neutral palette row, sizes the
/// frame table, and measures the shared canvas from the frame boxes.
/// `upload_sprite_frame` then stages single cells as they are asked for.
bool prepare_sprite(SpriteRenderer& renderer, const core::RleImage& image, ByteSpan store,
                    Sprite& out, std::string* error = nullptr);

/// Stages one frame of a sheet prepared by `prepare_sprite`, if it is not
/// already resident, and returns it. Null for an out-of-range cell, an empty
/// frame, or an RGB555 image, which an index atlas cannot hold.
///
/// The pixels reach the GPU at the next `SpriteRenderer::commit_uploads`, so a
/// frame first asked for while a draw list is being built would be sampled
/// before it existed. Ask for what the frame needs, commit, then draw.
const SpriteFrame* upload_sprite_frame(SpriteRenderer& renderer, const core::RleImage& image,
                                       ByteSpan store, Sprite& sprite, std::uint32_t row,
                                       std::uint32_t column);

/// Adds a palette row for `image` with slots 0..63 retinted towards `team`.
///
/// This is the whole of team colour. The atlas is untouched; one kilobyte of
/// palette is added, and every player draws the same index texels.
///
/// A caveat the specification is explicit about: the real per-player ramps are
/// not in the sprite files -- they live in the executable or the balance data
/// and have not been sourced. What is in the file is the neutral block the
/// artist painted, and slots 0..63 are *not* a sorted gradient, so laying a
/// generated dark-to-light ramp over the indices would scramble the shading.
/// So this keeps each slot's own brightness and takes only hue and saturation
/// from `team`: folds and highlights survive, the cloth changes colour. It is a
/// faithful preview of the mechanism, not a reproduction of a particular
/// player's palette, and it matches what the Python exporter does.
PaletteRow upload_team_palette(SpriteRenderer& renderer, const core::RleImage& image,
                               core::Rgb888 team, std::uint8_t transparent_index);

/// The palette a 1-bit shadow mask is drawn through: index 0 transparent,
/// index 1 opaque black. Modulate alpha at draw time decides how dark it lands.
PaletteRow upload_mask_palette(SpriteRenderer& renderer);

}  // namespace imperivm::platform
