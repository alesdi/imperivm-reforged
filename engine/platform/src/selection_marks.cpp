#include "imperivm/platform/selection_marks.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string_view>

namespace imperivm::platform {
namespace {

std::uint32_t u32_at(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[at + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

std::uint32_t u16_at(std::span<const std::uint8_t> bytes, std::size_t at) noexcept {
  return static_cast<std::uint32_t>(bytes[at]) | (static_cast<std::uint32_t>(bytes[at + 1]) << 8);
}

// The rgb555 colours 0x0062ac80 draws with.
constexpr std::uint16_t kWhite = 0x7FFF;  // pushed as -1 into a 16-bit buffer
constexpr std::uint16_t kBlack = 0x0000;
constexpr std::uint16_t kGreen = 0x03E0;
constexpr std::uint16_t kYellow = 0x7FE0;
constexpr std::uint16_t kRed = 0x7C00;
// The stamina pips, two rows of a three-pixel cycle (0x007d7a30, 0x007d7a28).
constexpr std::uint16_t kPipUpper[3] = {0x0B5C, 0x0077, 0x0000};
constexpr std::uint16_t kPipLower[3] = {0x4BDF, 0x0B5C, 0x0000};
// The item gem, 5 x 5, zero transparent (0x007d7a38).
constexpr std::uint16_t kGem[25] = {
    0x0000, 0x7FF9, 0x7FF9, 0x568F, 0x0000,  //
    0x7FF9, 0x7FF9, 0x7C00, 0x4000, 0x568F,  //
    0x6312, 0x7C00, 0x7C00, 0x4000, 0x6312,  //
    0x6312, 0x7C00, 0x7C00, 0x7FF9, 0x6312,  //
    0x0000, 0x568F, 0x6312, 0x7FF9, 0x0000,
};

/// A horizontal line from `x0` to `x1` inclusive, as the original's line
/// primitive draws one (reading: its end points are both drawn).
void hline(SpriteRenderer& renderer, std::int32_t x0, std::int32_t x1, std::int32_t y,
           std::uint16_t colour) {
  if (x1 < x0) return;
  renderer.fill_rect(static_cast<float>(x0), static_cast<float>(y),
                     static_cast<float>(x1 - x0 + 1), 1.0F, rgb555(colour));
}

void pixel(SpriteRenderer& renderer, std::int32_t x, std::int32_t y, std::uint16_t colour) {
  renderer.fill_rect(static_cast<float>(x), static_cast<float>(y), 1.0F, 1.0F, rgb555(colour));
}

}  // namespace

Rgba rgb555(std::uint16_t colour) noexcept {
  const auto channel = [](std::uint32_t five) { return static_cast<float>(five) / 31.0F; };
  return Rgba{channel((colour >> 10) & 31u), channel((colour >> 5) & 31u), channel(colour & 31u), 1.0F};
}

bool decode_coverage_image(std::span<const std::uint8_t> file, std::uint32_t frame,
                           std::vector<std::uint8_t>& out, std::uint32_t& width,
                           std::uint32_t& height) {
  // The frame table of docs/formats/rle.md, with one difference that is the
  // whole of class 6: after a populated frame's record the payload itself
  // follows, `data_size` bytes, where the other classes have `pamm` and an
  // offset into `rle.mmp`.
  if (file.size() < 26 || std::memcmp(file.data(), "IMGRLE", 6) != 0) return false;
  if (u32_at(file, 6) != 6) return false;
  const std::uint32_t count = u32_at(file, 18) * u32_at(file, 22);
  if (frame >= count) return false;
  std::size_t at = 26;
  for (std::uint32_t index = 0; index < count; ++index) {
    if (at + 16 > file.size()) return false;
    const std::uint32_t left = u32_at(file, at);
    const std::uint32_t top = u32_at(file, at + 4);
    const std::uint32_t right = u32_at(file, at + 8);
    const std::uint32_t bottom = u32_at(file, at + 12);
    at += 16;
    if (right < left || bottom < top) {
      if (index == frame) return false;
      continue;
    }
    if (at + 30 > file.size() || std::memcmp(file.data() + at, "RLE2", 4) != 0) return false;
    const std::uint32_t w = u32_at(file, at + 4);
    const std::uint32_t h = u32_at(file, at + 8);
    const std::uint32_t format = u16_at(file, at + 12);
    const std::uint32_t size = u32_at(file, at + 14);
    const bool wide = u32_at(file, at + 22) != 0;
    at += 30;
    if (at + size > file.size()) return false;
    if (index != frame) {
      at += size;
      continue;
    }
    if (format != 1 || w == 0 || h == 0 || w > 4096 || h > 4096) return false;
    const std::span<const std::uint8_t> payload = file.subspan(at, size);
    const std::size_t entry = wide ? 4 : 2;
    if (static_cast<std::size_t>(h) * entry > payload.size()) return false;
    out.assign(static_cast<std::size_t>(w) * h, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
      const std::size_t start = wide ? u32_at(payload, y * 4) : u16_at(payload, y * 2);
      const std::size_t end = y + 1 < h ? (wide ? u32_at(payload, (y + 1) * 4) : u16_at(payload, (y + 1) * 2))
                                        : payload.size();
      if (start > end || end > payload.size()) return false;
      std::size_t p = start;
      std::uint32_t x = 0;
      while (p + 1 < end) {
        x += payload[p];
        const std::uint32_t run = payload[p + 1];
        p += 2;
        if (p + run > end || x + run > w) return false;
        std::memcpy(out.data() + static_cast<std::size_t>(y) * w + x, payload.data() + p, run);
        p += run;
        x += run;
      }
    }
    width = w;
    height = h;
    return true;
  }
  return false;
}

bool SelectionRings::load(const Vfs& vfs, SpriteRenderer& renderer, std::string* error) {
  rings_.clear();
  constexpr std::string_view kPrefix = "UI\\SELECTIONS\\";
  constexpr std::string_view kSuffix = ".RLE";
  std::vector<std::uint8_t> coverage;
  for (const std::string& name : vfs.list()) {
    const std::string_view path(name);
    if (path.size() <= kPrefix.size() + kSuffix.size() || !path.starts_with(kPrefix) ||
        !path.ends_with(kSuffix)) {
      continue;
    }
    const std::string_view stem =
        path.substr(kPrefix.size(), path.size() - kPrefix.size() - kSuffix.size());
    Ring ring;
    const auto parsed = std::from_chars(stem.data(), stem.data() + stem.size(), ring.size);
    if (parsed.ec != std::errc{} || parsed.ptr != stem.data() + stem.size()) continue;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    if (!decode_coverage_image(vfs.read(name), 0, coverage, width, height)) continue;
    ring.region = renderer.upload_indices(coverage, width, height);
    if (!ring.region.valid()) continue;
    ring.width = static_cast<std::int32_t>(width);
    ring.height = static_cast<std::int32_t>(height);
    rings_.push_back(ring);
  }
  std::sort(rings_.begin(), rings_.end(), [](const Ring& a, const Ring& b) { return a.size < b.size; });
  if (rings_.empty() && error != nullptr) *error = "no UI\\SELECTIONS\\<n>.RLE in the packs";
  return !rings_.empty();
}

const SelectionRings::Ring* SelectionRings::for_radius(std::int32_t selection_radius) const noexcept {
  if (rings_.empty()) return nullptr;
  const auto it = std::lower_bound(rings_.begin(), rings_.end(), selection_radius,
                                   [](const Ring& ring, std::int32_t radius) { return ring.size < radius; });
  return it == rings_.end() ? &rings_.back() : &*it;
}

bool queue_health_bar(SpriteRenderer& renderer, std::int32_t x, std::int32_t anchor_y,
                      const HealthBar& bar) {
  if (bar.type <= 0 || bar.max_health <= 0) return false;
  // The span, per type (0x0062ad8a): 1 is the class radius either side, 3 is
  // 31 left and 30 right of the anchor, and every other value 17 and 16.
  std::int32_t left = x - 17;
  std::int32_t right = x + 16;
  if (bar.type == 1) {
    left = x - bar.radius;
    right = x + bar.radius - 1;
  } else if (bar.type == 3) {
    left = x - 31;
    right = x + 30;
  }
  if (right - left < 4) return false;
  const std::int32_t y = anchor_y + bar.offset;
  const bool gem = bar.item && bar.type != 1;

  // The frame: a white outer pixel and a black inner one at each end of the
  // two coloured rows; with the gem, the right end is three full-height lines.
  pixel(renderer, left, y, kWhite);
  pixel(renderer, left, y + 1, kWhite);
  pixel(renderer, left + 1, y, kBlack);
  pixel(renderer, left + 1, y + 1, kBlack);
  if (gem) {
    renderer.fill_rect(static_cast<float>(right - 1), static_cast<float>(y), 1.0F, 5.0F, rgb555(kBlack));
    renderer.fill_rect(static_cast<float>(right), static_cast<float>(y), 1.0F, 5.0F, rgb555(kWhite));
    renderer.fill_rect(static_cast<float>(right + 1), static_cast<float>(y), 1.0F, 5.0F, rgb555(kBlack));
    for (std::int32_t row = 0; row < 5; ++row) {
      for (std::int32_t column = 0; column < 5; ++column) {
        const std::uint16_t colour = kGem[row * 5 + column];
        if (colour != 0) pixel(renderer, right + 2 + column, y + row, colour);
      }
    }
  } else {
    pixel(renderer, right - 1, y, kBlack);
    pixel(renderer, right - 1, y + 1, kBlack);
    pixel(renderer, right, y, kWhite);
    pixel(renderer, right, y + 1, kWhite);
  }

  // The fill: green, yellow under half, red under a quarter; the rest of the
  // two rows black, and a black row under them.
  const std::int32_t health = std::clamp(bar.health, 0, bar.max_health);
  std::uint16_t colour = kGreen;
  if (2 * static_cast<std::int64_t>(health) < bar.max_health) colour = kYellow;
  if (4 * static_cast<std::int64_t>(health) < bar.max_health) colour = kRed;
  const std::int32_t fill =
      left + 2 +
      static_cast<std::int32_t>(static_cast<std::int64_t>(right - left - 4) * health / bar.max_health);
  hline(renderer, left + 2, fill, y, colour);
  hline(renderer, left + 2, fill, y + 1, colour);
  if (fill < right - 2) {
    hline(renderer, fill, right - 2, y, kBlack);
    hline(renderer, fill, right - 2, y + 1, kBlack);
  }
  hline(renderer, left + 2, right - 2, y + 2, kBlack);

  // Stamina: three pixels a point from the fill's left edge, two rows.
  if (bar.type != 1 && bar.stamina > 0) {
    const std::int32_t start = left + 2;
    const std::int32_t end = start + 3 * bar.stamina;
    for (std::int32_t px = start; px < end; ++px) {
      const auto phase = static_cast<std::size_t>((px - start) % 3);
      pixel(renderer, px, y + 3, kPipUpper[phase]);
      pixel(renderer, px, y + 4, kPipLower[phase]);
    }
  }
  return true;
}

}  // namespace imperivm::platform
