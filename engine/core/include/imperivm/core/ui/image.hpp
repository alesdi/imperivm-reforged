#pragma once

/// The interface art: Windows `.bmp`, decoded to 8-bit RGBA.
///
/// The sprite store is palette indexed and goes to the GPU that way, but the
/// interface is not: all 4,041 bitmaps in `UI.pak` are plain uncompressed
/// `BITMAPINFOHEADER` images, 3,626 of them 16 bits per pixel, 415 at 24 and
/// four at 8. There is no palette to swap and no team colour, so decoding
/// straight to RGBA is both simpler and smaller than an indexed path would be.
///
/// **16 bits per pixel is X1R5G5B5.** `biCompression` is `BI_RGB` in every
/// shipped bitmap, which is what Windows means by 555, and the colour key
/// confirms it: the pixel the info bar art nominates as transparent reads
/// `0x03E0`, which is full green under 555 and half green under 565, and the
/// 24-bit art nominates `(0, 255, 0)` for the same purpose.
///
/// **Transparency is a colour key, not an alpha channel.** A widget names the
/// coordinates of a pixel whose colour is the transparent one — see
/// `ImageRef` — and every pixel of that exact colour decodes to alpha 0.
///
/// Rows are bottom-up unless the height is negative, and padded to four bytes,
/// both of which are ordinary BMP.

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "imperivm/core/formats/result.hpp"

namespace imperivm::core::ui {

/// A decoded bitmap: tightly packed RGBA8, top row first.
struct Image {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> rgba;

  [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
  [[nodiscard]] const std::uint8_t* pixel(std::uint32_t x, std::uint32_t y) const noexcept {
    return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
  }
  [[nodiscard]] std::uint8_t* pixel(std::uint32_t x, std::uint32_t y) noexcept {
    return rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
  }
};

/// Decode a `.bmp`. 8, 16, 24 and 32 bits per pixel, `BI_RGB` only.
[[nodiscard]] Result<Image> decode_bmp(std::span<const std::byte> bytes);

/// An 8-bit bitmap kept as its indices: one byte per pixel, top row first.
///
/// The campaign map's `territories.bmp` is one: a 1024 x 768 mask whose
/// pixel value is the `index` attribute of the territory standing there
/// (2..8 in the shipped conquest) and 1 where there is none. Its palette
/// is a colour per index for the editor's eyes and means nothing here.
struct IndexImage {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> indices;

  [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }
  [[nodiscard]] std::uint8_t at(std::uint32_t x, std::uint32_t y) const noexcept {
    return x < width && y < height ? indices[static_cast<std::size_t>(y) * width + x] : 0;
  }
};

/// Decode an 8-bit `.bmp` to its indices. Anything but 8 bits is refused.
[[nodiscard]] Result<IndexImage> decode_bmp_indices(std::span<const std::byte> bytes);

/// A recolouring of the pixels under one index of a mask: the hue set to
/// `hue`, the saturation scaled by `saturation`, the value kept.
///
/// The scales are the conquest's `territories.xml`'s: `owned_hue="560"`,
/// `enemy_hue="0"`, `owned_sat="1360"`, `enemy_sat="1000"`,
/// `disabled_sat="1024"`. **Reading, labelled:** hue on a scale of 1536 --
/// six sextants of 256, the integer HSV a 16-bit rasteriser keeps -- puts
/// 560 at 131 degrees, the green the legend paints for "your territory",
/// and 0 at red for "territory you can conquer"; saturation on a scale
/// where 1024 is unchanged, so owned is boosted and enemy nearly kept.
/// Neither scale is in the data or was read from the executable.
struct IndexTint {
  std::uint8_t index = 0;
  std::int32_t hue = 0;         ///< 0..1535
  std::int32_t saturation = 1024;  ///< 1024 = as it is
};

/// Apply `tints` to `image` wherever `mask` (the same size) holds a tinted
/// index. Pixels under other indices are left alone.
void tint_by_index(Image& image, const IndexImage& mask, std::span<const IndexTint> tints);

/// `source` resampled to `width` x `height`, each output pixel the mean of
/// the source block it covers (a box filter; exact integer arithmetic). For
/// a 1024 x 768 campaign map shown in a 600 x 400 widget.
[[nodiscard]] Image resample_box(const Image& source, std::uint32_t width, std::uint32_t height);

/// Zero the alpha of every pixel matching the colour at `(key_x, key_y)`.
///
/// Applied after decoding rather than during it because the key is a property
/// of the *widget* that names the bitmap, not of the file: the same bitmap can
/// be drawn keyed by one control and opaque by another.
void apply_color_key(Image& image, std::uint32_t key_x, std::uint32_t key_y) noexcept;

/// The `column`th of `columns` equal vertical slices, and the `row`th of `rows`
/// horizontal ones. This is what an `ImageType` letter and `Rows` select.
///
/// Returns an empty image when the grid does not divide the bitmap exactly,
/// which never happens in the retail art — the frame count implied by
/// `ImageType` divides the bitmap width in 139 of 139 multi-frame sections —
/// and would mean a mod had got its strip wrong.
[[nodiscard]] Image sub_image(const Image& source, std::uint32_t column, std::uint32_t columns,
                              std::uint32_t row, std::uint32_t rows);

/// The number of frames an `ImageType` code implies: the highest letter it
/// uses, plus one. `AAAAA` is 1, `ABCCC` is 3. Zero for an empty or malformed
/// code.
[[nodiscard]] std::uint32_t image_type_frames(std::string_view code) noexcept;

/// The frame index a control in state `state` draws, from the same code.
/// Out-of-range states fall back to the first letter, which is the resting
/// state in every code the corpus uses.
[[nodiscard]] std::uint32_t image_type_frame(std::string_view code, std::uint32_t state) noexcept;

}  // namespace imperivm::core::ui
