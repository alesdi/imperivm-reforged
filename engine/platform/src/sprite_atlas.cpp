#include "imperivm/platform/sprite_atlas.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace imperivm::platform {
namespace {

using core::RleFrame;
using core::RleImage;
using core::RleImageClass;
using core::RlePixelFormat;

/// RGBA8 texel on a little-endian host: 0xAABBGGRR.
constexpr std::uint32_t rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                             std::uint8_t a) {
  return static_cast<std::uint32_t>(r) | static_cast<std::uint32_t>(g) << 8 |
         static_cast<std::uint32_t>(b) << 16 | static_cast<std::uint32_t>(a) << 24;
}

/// Pick the index the atlas is cleared to and the palette makes transparent.
///
/// The frame's own `color_key` is the natural answer -- it is the colour the
/// artist painted the background in, and the specification establishes it never
/// appears in the compressed data -- but only if this image really does not use
/// it, which is checked rather than assumed. A player-colour image additionally
/// must not reserve a slot below 64: a swapped-in team ramp would land on a
/// hole and the unit would develop transparent patches.
std::uint8_t choose_transparent_index(const RleImage& image,
                                      const std::array<bool, 256>& used) {
  const std::size_t size = image.palette_size();
  const bool team_block = image.image_class() == RleImageClass::player_color;

  std::uint32_t key = 0;
  bool one_key = true;
  bool have_key = false;
  for (const RleFrame& frame : image.frames()) {
    if (frame.empty) continue;
    if (!have_key) {
      key = frame.color_key;
      have_key = true;
    } else if (frame.color_key != key) {
      one_key = false;
    }
  }

  if (have_key && one_key && key < size && !used[key] &&
      !(team_block && key < core::kRlePlayerColorSlots)) {
    return static_cast<std::uint8_t>(key);
  }

  for (std::size_t i = core::kRlePlayerColorSlots; i < 256; ++i) {
    if (i >= size || !used[i]) return static_cast<std::uint8_t>(i);
  }
  for (std::size_t i = 0; i < core::kRlePlayerColorSlots; ++i) {
    if (!used[i]) return static_cast<std::uint8_t>(i);
  }
  // Every index is in use. Nothing correct is possible without a second
  // channel, so give up the colour of the least-used slot rather than the
  // sprite: index 0 of a class 1 image is the conventional background.
  return 0;
}

std::vector<std::uint32_t> palette_rows(const RleImage& image, std::uint8_t transparent,
                                        const core::Rgb888* team) {
  std::vector<std::uint32_t> out(256, 0);
  const std::size_t size = image.palette_size();
  for (std::size_t i = 0; i < size && i < 256; ++i) {
    const core::Rgb888 colour = image.palette_color(i);
    out[i] = rgba(colour.red, colour.green, colour.blue, 255);
  }
  if (team != nullptr && image.image_class() == RleImageClass::player_color) {
    // Keep each slot's brightness, take hue and saturation from the team
    // colour. See the header for why a generated ramp would be wrong.
    float team_h = 0.0F;
    float team_s = 0.0F;
    {
      const float r = static_cast<float>(team->red) / 255.0F;
      const float g = static_cast<float>(team->green) / 255.0F;
      const float b = static_cast<float>(team->blue) / 255.0F;
      const float hi = std::max({r, g, b});
      const float lo = std::min({r, g, b});
      const float span = hi - lo;
      team_s = hi > 0.0F ? span / hi : 0.0F;
      if (span > 0.0F) {
        if (hi == r) {
          team_h = std::fmod((g - b) / span, 6.0F);
        } else if (hi == g) {
          team_h = (b - r) / span + 2.0F;
        } else {
          team_h = (r - g) / span + 4.0F;
        }
        team_h /= 6.0F;
        if (team_h < 0.0F) team_h += 1.0F;
      }
    }

    for (std::size_t i = 0; i < core::kRlePlayerColorSlots && i < size; ++i) {
      const core::Rgb888 colour = image.palette_color(i);
      const float value = static_cast<float>(std::max({colour.red, colour.green, colour.blue})) /
                          255.0F;

      const float h = team_h * 6.0F;
      const int sector = static_cast<int>(h) % 6;
      const float f = h - std::floor(h);
      const float p = value * (1.0F - team_s);
      const float q = value * (1.0F - team_s * f);
      const float t = value * (1.0F - team_s * (1.0F - f));
      float r = 0.0F;
      float g = 0.0F;
      float b = 0.0F;
      switch (sector) {
        case 0: r = value; g = t; b = p; break;
        case 1: r = q; g = value; b = p; break;
        case 2: r = p; g = value; b = t; break;
        case 3: r = p; g = q; b = value; break;
        case 4: r = t; g = p; b = value; break;
        default: r = value; g = p; b = q; break;
      }
      out[i] = rgba(static_cast<std::uint8_t>(std::lround(r * 255.0F)),
                    static_cast<std::uint8_t>(std::lround(g * 255.0F)),
                    static_cast<std::uint8_t>(std::lround(b * 255.0F)), 255);
    }
  }

  // The transparency the format does not have an alpha channel for.
  out[transparent] = 0;
  return out;
}

/// Which palette indices this image actually uses. The answer decides which
/// index can safely be given away to transparency, and it is a scan of the
/// compressed runs -- no buffer is allocated and nothing is uploaded.
void scan_used_indices(const RleImage& image, ByteSpan store, bool first_frame_only,
                       std::array<bool, 256>& used, bool& any_mask) {
  const auto store_bytes = as_core_bytes(store);
  for (const RleFrame& frame : image.frames()) {
    if (first_frame_only && frame.index != 0) continue;
    if (frame.empty) continue;
    if (frame.pixel_format == RlePixelFormat::mask) {
      any_mask = true;
      continue;
    }
    if (frame.pixel_format != RlePixelFormat::indexed8) continue;
    const auto payload = core::rle_frame_payload(frame, store_bytes);
    if (!payload) continue;
    core::rle_for_each_span(frame, payload.value(),
                            [&](std::uint32_t, std::uint32_t, std::uint32_t length,
                                std::span<const std::byte> run) {
                              for (std::uint32_t i = 0; i < length && i < run.size(); ++i) {
                                used[static_cast<std::uint8_t>(run[i])] = true;
                              }
                            });
  }
}

/// Decode one frame into a tight index buffer and stage it in the atlas.
///
/// `pixels` is the caller's scratch buffer, reused across frames so a sheet of
/// 112 frames does not allocate 112 times. Returns false only when the frame's
/// bytes are unusable, which is a corrupt file rather than an ordinary miss; an
/// RGB555 frame is skipped and reported as success, because such an image is
/// legitimately not representable in an index atlas.
bool stage_frame(SpriteRenderer& renderer, const RleFrame& frame, ByteSpan store,
                 std::uint8_t transparent, std::vector<std::uint8_t>& pixels,
                 SpriteFrame& out, std::string* error) {
  const auto payload = core::rle_frame_payload(frame, as_core_bytes(store));
  if (!payload) {
    if (error != nullptr) *error = "frame payload is outside the pixel store";
    return false;
  }

  const bool mask = frame.pixel_format == RlePixelFormat::mask;
  if (!mask && frame.pixel_format != RlePixelFormat::indexed8) {
    // RGB555 frames (classes 0 and 5: cursors, clouds, fire) have no palette
    // and cannot go in an index atlas. They need a second, truecolour path,
    // which is not written yet -- see docs/engine/rendering.md.
    return true;
  }

  const std::size_t count = static_cast<std::size_t>(frame.width) * frame.height;
  pixels.assign(count, transparent);

  const std::uint32_t width = frame.width;
  const auto status = core::rle_for_each_span(
      frame, payload.value(),
      [&](std::uint32_t y, std::uint32_t x, std::uint32_t length,
          std::span<const std::byte> run) {
        const std::size_t at = static_cast<std::size_t>(y) * width + x;
        if (at + length > pixels.size()) return;
        if (mask) {
          std::fill_n(pixels.begin() + static_cast<long>(at), length, std::uint8_t{1});
        } else {
          std::memcpy(pixels.data() + at, run.data(), length);
        }
      });
  if (!status) {
    if (error != nullptr) *error = "frame rows do not tile their bounding box";
    return false;
  }

  out.region = renderer.upload_indices(pixels, frame.width, frame.height);
  out.left = static_cast<std::int32_t>(frame.left);
  out.top = static_cast<std::int32_t>(frame.top);
  return true;
}

/// The union of every populated frame's bounding box, straight from the frame
/// table. No pixels are touched: the boxes are in the table.
void measure_canvas(const RleImage& image, Sprite& out) {
  std::int64_t min_left = std::numeric_limits<std::int64_t>::max();
  std::int64_t min_top = std::numeric_limits<std::int64_t>::max();
  std::int64_t max_right = std::numeric_limits<std::int64_t>::min();
  std::int64_t max_bottom = std::numeric_limits<std::int64_t>::min();
  for (const RleFrame& frame : image.frames()) {
    if (frame.empty) continue;
    min_left = std::min<std::int64_t>(min_left, frame.left);
    min_top = std::min<std::int64_t>(min_top, frame.top);
    max_right = std::max<std::int64_t>(max_right, frame.right);
    max_bottom = std::max<std::int64_t>(max_bottom, frame.bottom);
  }
  if (min_left > max_right) return;
  out.canvas_left = static_cast<std::int32_t>(min_left);
  out.canvas_top = static_cast<std::int32_t>(min_top);
  out.canvas_width = static_cast<std::uint32_t>(max_right - min_left + 1);
  out.canvas_height = static_cast<std::uint32_t>(max_bottom - min_top + 1);
}

}  // namespace

bool upload_sprite(SpriteRenderer& renderer, const RleImage& image, ByteSpan store,
                   Sprite& out, std::string* error, bool first_frame_only) {
  out = Sprite{};
  out.columns = image.columns();
  out.rows = image.rows();
  out.player_color = image.image_class() == RleImageClass::player_color;

  // Pass one: which palette indices does this image actually use? The answer
  // decides which index can safely be given away to transparency.
  std::array<bool, 256> used{};
  bool any_mask = false;
  scan_used_indices(image, store, first_frame_only, used, any_mask);
  out.mask = any_mask && image.palette_size() == 0;

  // A mask has no palette, so it defines its own two-entry one: 0 uncovered,
  // 1 covered. Everything downstream then treats it as an ordinary indexed
  // image, which is why shadows need no second code path.
  out.transparent_index = out.mask ? 0 : choose_transparent_index(image, used);

  // Pass two: decode each frame into its own tight index buffer and stage it.
  std::vector<std::uint8_t> pixels;
  out.frames.resize(static_cast<std::size_t>(image.rows()) * image.columns());

  std::int64_t min_left = std::numeric_limits<std::int64_t>::max();
  std::int64_t min_top = std::numeric_limits<std::int64_t>::max();
  std::int64_t max_right = std::numeric_limits<std::int64_t>::min();
  std::int64_t max_bottom = std::numeric_limits<std::int64_t>::min();

  for (const RleFrame& frame : image.frames()) {
    if (first_frame_only && frame.index != 0) continue;
    if (frame.index >= out.frames.size()) continue;
    if (frame.empty) continue;

    SpriteFrame& slot = out.frames[frame.index];
    if (!stage_frame(renderer, frame, store, out.transparent_index, pixels, slot, error)) {
      return false;
    }
    if (!slot.region.valid()) continue;

    min_left = std::min<std::int64_t>(min_left, frame.left);
    min_top = std::min<std::int64_t>(min_top, frame.top);
    max_right = std::max<std::int64_t>(max_right, frame.right);
    max_bottom = std::max<std::int64_t>(max_bottom, frame.bottom);
  }

  if (min_left <= max_right) {
    out.canvas_left = static_cast<std::int32_t>(min_left);
    out.canvas_top = static_cast<std::int32_t>(min_top);
    out.canvas_width = static_cast<std::uint32_t>(max_right - min_left + 1);
    out.canvas_height = static_cast<std::uint32_t>(max_bottom - min_top + 1);
  }

  out.neutral = out.mask ? upload_mask_palette(renderer)
                         : renderer.upload_palette(
                               palette_rows(image, out.transparent_index, nullptr));
  return out.neutral.valid;
}

bool prepare_sprite(SpriteRenderer& renderer, const RleImage& image, ByteSpan store,
                    Sprite& out, std::string* error) {
  out = Sprite{};
  out.columns = image.columns();
  out.rows = image.rows();
  out.player_color = image.image_class() == RleImageClass::player_color;

  // The whole image is scanned, not just the frames a caller happens to ask
  // for first: the transparent index has to be safe for every frame that may
  // later be uploaded, and it cannot be revised afterwards without re-staging
  // everything already in the atlas.
  std::array<bool, 256> used{};
  bool any_mask = false;
  scan_used_indices(image, store, /*first_frame_only=*/false, used, any_mask);
  out.mask = any_mask && image.palette_size() == 0;
  out.transparent_index = out.mask ? 0 : choose_transparent_index(image, used);

  out.frames.resize(static_cast<std::size_t>(image.rows()) * image.columns());
  measure_canvas(image, out);

  out.neutral = out.mask ? upload_mask_palette(renderer)
                         : renderer.upload_palette(
                               palette_rows(image, out.transparent_index, nullptr));
  if (!out.neutral.valid && error != nullptr) *error = "palette row could not be staged";
  return out.neutral.valid;
}

const SpriteFrame* upload_sprite_frame(SpriteRenderer& renderer, const RleImage& image,
                                       ByteSpan store, Sprite& sprite, std::uint32_t row,
                                       std::uint32_t column) {
  if (sprite.columns == 0 || row >= sprite.rows || column >= sprite.columns) return nullptr;
  const std::size_t index = static_cast<std::size_t>(row) * sprite.columns + column;
  if (index >= sprite.frames.size()) return nullptr;

  SpriteFrame& slot = sprite.frames[index];
  if (slot.region.valid()) return &slot;

  const auto frame = image.frame(row, column);
  if (!frame || frame->empty) return nullptr;

  std::vector<std::uint8_t> pixels;
  if (!stage_frame(renderer, frame.value(), store, sprite.transparent_index, pixels, slot,
                   nullptr)) {
    return nullptr;
  }
  return slot.region.valid() ? &slot : nullptr;
}

PaletteRow upload_team_palette(SpriteRenderer& renderer, const RleImage& image,
                               core::Rgb888 team, std::uint8_t transparent_index) {
  return renderer.upload_palette(palette_rows(image, transparent_index, &team));
}

PaletteRow upload_mask_palette(SpriteRenderer& renderer) {
  std::vector<std::uint32_t> entries(256, 0);
  entries[0] = 0;                      // uncovered
  entries[1] = rgba(0, 0, 0, 255);     // covered; alpha comes from the modulate
  return renderer.upload_palette(entries);
}

}  // namespace imperivm::platform
