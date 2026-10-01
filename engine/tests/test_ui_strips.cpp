// The strips: `UIHolder`, `UIInventory`, `Combiner`, `HeroSkills`,
// `UnitSpecials` and the portrait, composited from `BarContent` over
// synthetic art -- 2 x 2 bitmaps in memory, one colour each, so that a pixel
// says which bitmap landed where. The geometry keys are the Roman info
// bar's; the paths and art are not the game's.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "imperivm/core/ui/image.hpp"
#include "imperivm/core/ui/interface.hpp"
#include "imperivm/core/ui/layout.hpp"
#include "imperivm/core/ui/paint.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::ui;

namespace {

std::span<const std::byte> as_bytes(std::string_view text) noexcept {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A 2 x 2 16-bit BMP (X1R5G5B5): top-left, top-right, bottom-left,
/// bottom-right.
std::string bmp4(std::uint16_t a, std::uint16_t b, std::uint16_t c, std::uint16_t d) {
  std::string out(14 + 40 + 16, '\0');
  auto put16 = [&](std::size_t at, std::uint16_t v) {
    out[at] = static_cast<char>(v & 0xFF);
    out[at + 1] = static_cast<char>(v >> 8);
  };
  auto put32 = [&](std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
  };
  out[0] = 'B';
  out[1] = 'M';
  put32(2, static_cast<std::uint32_t>(out.size()));
  put32(10, 54);
  put32(14, 40);
  put32(18, 2);
  put32(22, 2);
  put16(26, 1);
  put16(28, 16);
  put32(30, 0);
  // Bottom-up rows.
  put16(54, c);
  put16(56, d);
  put16(58, a);
  put16(60, b);
  return out;
}

/// One colour throughout.
std::string solid_bmp(std::uint16_t colour) { return bmp4(colour, colour, colour, colour); }

constexpr std::uint16_t kRed = 0x7C00;
constexpr std::uint16_t kGreen = 0x03E0;   // the interface's key colour
constexpr std::uint16_t kBlue = 0x001F;
constexpr std::uint16_t kWhite = 0x7FFF;
constexpr std::uint16_t kYellow = 0x7FE0;

// The frames are keyed at (0, 0), as the shipped ones are at (20, 20): the
// keyed pixel is the interface's green and the rest is the frame's colour,
// so a frame shows at a cell's top-right and lets the icon through at its
// top-left.
const std::string kFrameNormal = bmp4(kGreen, kRed, kRed, kRed);
const std::string kFrameSelected = bmp4(kGreen, kBlue, kBlue, kBlue);
const std::string kFrameTrain = bmp4(kGreen, kYellow, kYellow, kYellow);
/// Two rows of one 2 x 1 cell: red on top, blue below.
const std::string kSwitch = bmp4(kRed, kRed, kBlue, kBlue);
const std::string kPortrait = solid_bmp(kWhite);
const std::string kGreenPortrait = solid_bmp(kGreen);
const std::string kRamp = solid_bmp(kGreen);
const std::string kBack = solid_bmp(kBlue);
const std::string kGlow = solid_bmp(kYellow);
const std::string kPlus = solid_bmp(kRed);

constexpr std::string_view kScreen = R"([Strips]
RectWH = 0, 0, 1024, 80

[Strips Objects]
Thumbnail
Holder
Inventory
Comb
Skills
Specials
Blinker
Steady

[Thumbnail]
Type = Thumbnail
RectWH = 0, 0, 10, 10

[Holder]
Type = UIHolder
Style = TRANSPARENT
Rect = 100, 0, 300, 10
IconWidth = 10
IconHeight = 10
MinIconSpace = 0
MaxIconSpace = 6
IconYPosition = 0
HealthBarYPosition = 8
HealthWidth = 6
GradientImage = art/ramp.bmp
NoSelFrameImage = art/frame.bmp, 0, 0
SelFrameImage = art/selected.bmp, 0, 0
TrainFrameImage = art/train.bmp, 0, 0
WaitFrameImage = art/frame.bmp, 0, 0
holder

[Inventory]
Type = UIInventory
Style = TRANSPARENT
Rect = 400, 0, 500, 10
IconWidth = 10
IconHeight = 10
MinIconSpace = 0
MaxIconSpace = 0
BackImage = art/back.bmp
BackGlowImage = art/glow.bmp
ReverseDraw
items

[Comb]
Type = Combiner
Style = TRANSPARENT
Rect = 600, 0, 700, 10
Id1 = Inventory
Id2 = Holder
items
holder

[Skills]
Type = HeroSkills
Style = TRANSPARENT
Rect = 700, 20, 900, 30
PlusSign = art/plus.bmp
IconFrame = art/frame.bmp, 0, 0
IconSpace = 2
IconTextColor = 255, 255, 255
IconTextOffset = -5, 5
hero

[Specials]
Type = UnitSpecials
Style = TRANSPARENT
RectWH = 900, 40, 100, 40
IconFrame = art/frame.bmp, 0, 0
Icon0RectWH = 0, 0, 2, 2
Text0RectWH = 4, 0, 50, 10
Icon1RectWH = 0, 10, 2, 2
Text1RectWH = 4, 10, 50, 10
unit

[Blinker]
Type = Switch
Style = TRANSPARENT
Image = art/switch.bmp
Rows = 2
InitialRow = 0
RectWH = 1000, 60, 2, 1
SwitchToTab = 1
BlinkTime = 500
hero

[Steady]
Type = Switch
Style = TRANSPARENT
Image = art/switch.bmp
Rows = 2
InitialRow = 0
RectWH = 1010, 60, 2, 1
SwitchToTab = 0
hero
)";

std::span<const std::byte> provider(std::string_view path) {
  if (path == "data/interface/strips.ini") return as_bytes(kScreen);
  if (path == "art/frame.bmp") return as_bytes(kFrameNormal);
  if (path == "art/selected.bmp") return as_bytes(kFrameSelected);
  if (path == "art/train.bmp") return as_bytes(kFrameTrain);
  if (path == "art/portrait.bmp") return as_bytes(kPortrait);
  if (path == "art/green.bmp") return as_bytes(kGreenPortrait);
  if (path == "art/ramp.bmp") return as_bytes(kRamp);
  if (path == "art/back.bmp") return as_bytes(kBack);
  if (path == "art/glow.bmp") return as_bytes(kGlow);
  if (path == "art/plus.bmp") return as_bytes(kPlus);
  if (path == "art/switch.bmp") return as_bytes(kSwitch);
  return {};
}

struct Painted {
  Canvas canvas{1024, 80};
  ResourceCache cache{provider};
  Result<Screen> screen = load_screen("data/interface/strips.ini", {}, provider);

  bool paint(const BarContent& content) {
    if (!screen.ok()) return false;
    const Layout layout = layout_screen(*screen, 1024, 80);
    canvas.clear();
    paint_screen(canvas, layout, cache, content);
    return true;
  }
  [[nodiscard]] bool is(std::int32_t x, std::int32_t y, std::uint8_t r, std::uint8_t g,
                        std::uint8_t b) const {
    const Color c = canvas.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
    return c.red == r && c.green == g && c.blue == b && c.alpha == 255;
  }
  [[nodiscard]] bool blank(std::int32_t x, std::int32_t y) const {
    return canvas.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)).alpha == 0;
  }
};

}  // namespace

TEST(ui_strips_draw_nothing_for_an_empty_strip) {
  Painted p;
  BarContent content;
  content.tags = SelectionTag::kHolder | SelectionTag::kItems | SelectionTag::kHero |
                 SelectionTag::kUnit;
  REQUIRE(p.paint(content));
  CHECK(p.screen->warnings.empty());
  // Every strip is visible and every strip is empty: no frame, no furniture.
  CHECK(p.blank(100, 0));
  CHECK(p.blank(490, 0));
  CHECK(p.blank(600, 0));
  CHECK(p.blank(700, 20));
  CHECK(p.blank(900, 40));
}

TEST(ui_strips_lay_cells_out_with_the_gap_the_row_allows) {
  Painted p;
  BarContent content;
  content.tags = SelectionTag::kHolder;
  StripCell cell;
  cell.icon = "art/portrait.bmp";
  content.holder.assign(3, cell);
  content.holder[1].frame = StripCell::Frame::kSelected;
  content.holder[2].frame = StripCell::Frame::kTrain;
  content.holder[0].health = 100;
  REQUIRE(p.paint(content));
  // Three ten-wide cells in a 200-wide row: the gap is `MaxIconSpace`, 6.
  // A frame's keyed pixel is its top-left; its top-right shows its colour.
  CHECK(p.is(101, 0, 255, 0, 0));      // cell 0: the normal frame
  CHECK(p.is(117, 0, 0, 0, 255));      // cell 1 at 100 + 16: selected
  CHECK(p.is(133, 0, 255, 255, 0));    // cell 2 at 100 + 32: train
  CHECK(p.blank(110, 0));              // the gap
  // The icon, 2 x 2, centred across the ten-wide cell: at x 104..105.
  CHECK(p.is(104, 0, 255, 255, 255));
  CHECK(p.is(120, 1, 255, 255, 255));
  // The bar: `HealthWidth` 6 centred in the 10-wide cell, at row 8, in the
  // ramp's colour -- green at full health.
  CHECK(p.is(102, 8, 0, 255, 0));
  CHECK(p.is(107, 9, 0, 255, 0));
  CHECK(!p.is(101, 8, 0, 255, 0));
  CHECK(p.blank(118, 8));              // cell 1 has no bar
}

TEST(ui_strips_squeeze_the_gap_when_the_row_is_full) {
  Painted p;
  BarContent content;
  content.tags = SelectionTag::kHolder;
  StripCell cell;
  cell.icon = "art/portrait.bmp";
  // Twenty cells of ten in two hundred: no gap at all.
  content.holder.assign(20, cell);
  REQUIRE(p.paint(content));
  for (int i = 0; i < 20; ++i) CHECK(p.is(101 + i * 10, 0, 255, 0, 0));
  // And a twenty-first would not fit; nothing is drawn past the row.
  content.holder.push_back(cell);
  REQUIRE(p.paint(content));
  CHECK(p.blank(301, 0));
}

TEST(ui_strips_reverse_draw_fills_from_the_right_and_glows) {
  Painted p;
  BarContent content;
  content.tags = SelectionTag::kItems;
  StripCell item;
  item.icon = "art/green.bmp";  // a green portrait is a keyed one: transparent
  content.items.assign(2, item);
  content.items[1].glow = true;
  REQUIRE(p.paint(content));
  // The row is 400..500; the first cell sits against the right edge and the
  // icon, being all key, shows the back image through: blue, then the glow.
  CHECK(p.is(490, 0, 0, 0, 255));
  CHECK(p.is(480, 0, 255, 255, 0));
  CHECK(p.blank(400, 0));
}

TEST(ui_strips_combiner_continues_the_first_strip_with_the_second) {
  Painted p;
  BarContent content;
  content.tags = SelectionTag::kItems | SelectionTag::kHolder;
  StripCell item;
  item.icon = "art/portrait.bmp";
  content.items.assign(1, item);
  content.holder.assign(2, item);
  REQUIRE(p.paint(content));
  // The combiner's row is 600..700. The inventory draws from the right by its
  // own `ReverseDraw`: its one cell at 690. The holder continues in the
  // same row, from the left, one cell on: 600 + 16 and 600 + 32.
  CHECK(p.is(690, 0, 0, 0, 255));      // the item's back image
  CHECK(p.is(617, 0, 255, 0, 0));      // the first garrison cell, one slot on
  CHECK(p.is(633, 0, 255, 0, 0));
  CHECK(p.blank(600, 0));
}

TEST(ui_strips_skills_and_specials_draw_their_frames) {
  Painted p;
  BarContent content;
  content.tags = SelectionTag::kHero | SelectionTag::kUnit;
  StripCell skill;
  skill.icon = "art/portrait.bmp";
  skill.number = "2";
  content.skills.assign(2, skill);
  content.skills[1].plus = true;
  StripCell special;
  special.icon = "art/portrait.bmp";
  special.text = "Parry";
  content.specials.assign(2, special);
  REQUIRE(p.paint(content));
  // Skills: the frame is 2 x 2, the row is 700..900 and 10 tall, the cell is
  // centred vertically (y = 24) and the second sits `IconSpace` on.
  CHECK(p.is(701, 24, 255, 0, 0));
  CHECK(p.is(705, 24, 255, 0, 0));
  CHECK(p.blank(702, 24));
  // Under the frame's keyed corner, the icon shows: white at each cell's
  // top-left.
  CHECK(p.is(700, 24, 255, 255, 255));
  // Specials: two framed icons at the control's `Icon0RectWH` and `Icon1RectWH`.
  CHECK(p.is(901, 40, 255, 0, 0));
  CHECK(p.is(901, 50, 255, 0, 0));
  CHECK(p.blank(901, 45));
}

TEST(ui_strips_portrait_by_path_is_keyed_when_its_corner_is_green) {
  Painted p;
  BarContent content;
  content.tags = SelectionTag::kThumb;
  content.thumbnail_path = "art/portrait.bmp";
  REQUIRE(p.paint(content));
  // A 2 x 2 portrait centred in a 10-wide thumbnail: at x 4..5.
  CHECK(p.is(4, 0, 255, 255, 255));
  CHECK(p.blank(0, 0));
  content.thumbnail_path = "art/green.bmp";
  REQUIRE(p.paint(content));
  CHECK(p.blank(4, 0));
}

TEST(ui_strips_a_switch_with_a_blink_time_shows_its_other_row_while_blinking) {
  // 0x006c1032: the blink toggles `1 - row`; a switch that declares no
  // `BlinkTime` never leaves its `InitialRow`, whatever the content says.
  Painted p;
  BarContent content;
  content.tags = SelectionTag::kHero;
  REQUIRE(p.paint(content));
  CHECK(p.is(1000, 60, 255, 0, 0));
  CHECK(p.is(1010, 60, 255, 0, 0));
  content.blink = true;
  REQUIRE(p.paint(content));
  CHECK(p.is(1000, 60, 0, 0, 255));
  CHECK(p.is(1010, 60, 255, 0, 0));
}

TEST(ui_strips_a_cell_is_hit_where_it_is_drawn) {
  Painted p;
  REQUIRE(p.screen.ok());
  const Layout layout = layout_screen(*p.screen, 1024, 80);
  BarContent content;
  content.tags = SelectionTag::kHolder;
  StripCell cell;
  cell.icon = "art/portrait.bmp";
  content.holder.assign(3, cell);
  // The cells of `ui_strips_lay_cells_out_with_the_gap_the_row_allows`: ten
  // wide at 100, 116 and 132.
  StripCellHit hit = strip_cell_at(layout, content, 120, 5);
  REQUIRE(hit.strip != nullptr);
  CHECK(hit.strip->name == "Holder");
  CHECK(hit.index == 1);
  CHECK(strip_cell_at(layout, content, 111, 5).strip == nullptr);  // the gap
  CHECK(strip_cell_at(layout, content, 150, 5).strip == nullptr);  // past the last cell
  const Rect third = strip_cell_rect(layout, content, "Holder", 2);
  CHECK(third.x == 132 && third.y == 0 && third.width == 10 && third.height == 10);
  CHECK(strip_cell_rect(layout, content, "Holder", 3).width == 0);
  // A strip the selection does not show is not hit.
  content.tags = SelectionTag::kUnit;
  CHECK(strip_cell_at(layout, content, 120, 5).strip == nullptr);
}

TEST(ui_strips_a_combiners_cells_are_hit_as_its_two_strips) {
  Painted p;
  REQUIRE(p.screen.ok());
  const Layout layout = layout_screen(*p.screen, 1024, 80);
  BarContent content;
  content.tags = SelectionTag::kItems | SelectionTag::kHolder;
  StripCell item;
  item.icon = "art/portrait.bmp";
  content.items.assign(1, item);
  content.holder.assign(2, item);
  // The combiner's row: the item at 690, the garrison from 616.
  StripCellHit hit = strip_cell_at(layout, content, 620, 5);
  REQUIRE(hit.strip != nullptr);
  CHECK(hit.strip->name == "Holder");
  CHECK(hit.index == 0);
  hit = strip_cell_at(layout, content, 695, 5);
  REQUIRE(hit.strip != nullptr);
  CHECK(hit.strip->name == "Inventory");
  CHECK(hit.index == 0);
  CHECK(strip_cell_at(layout, content, 605, 5).strip == nullptr);
}
