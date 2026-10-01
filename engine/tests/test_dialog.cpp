// The menus: a screen shaped like `GAMEMENU.INI` running as a `Dialog` --
// buttons pressed with the mouse and the keyboard, a list, an edit, a
// combobox -- and the menu widgets' pixels, over synthetic art small enough
// that one pixel says which piece of which bitmap landed where.

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/ui/dialog.hpp"
#include "imperivm/core/ui/image.hpp"
#include "imperivm/core/ui/interface.hpp"
#include "imperivm/core/ui/layout.hpp"
#include "imperivm/core/ui/markup.hpp"
#include "imperivm/core/ui/paint.hpp"
#include "test.hpp"

using namespace imperivm::core;
using namespace imperivm::core::ui;

namespace {

std::span<const std::byte> as_bytes(std::string_view text) noexcept {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

/// A `width` x `height` 16-bit BMP (X1R5G5B5) from row-major pixels.
std::string bmp16(std::uint32_t width, std::uint32_t height, const std::vector<std::uint16_t>& pixels) {
  const std::size_t stride = (width * 2 + 3) / 4 * 4;
  std::string out(14 + 40 + stride * height, '\0');
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
  put32(18, width);
  put32(22, height);
  put16(26, 1);
  put16(28, 16);
  put32(30, 0);
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint32_t row = height - 1 - y;  // bottom-up
    for (std::uint32_t x = 0; x < width; ++x) {
      put16(54 + row * stride + x * 2, pixels[y * width + x]);
    }
  }
  return out;
}

constexpr std::uint16_t kRed = 0x7C00;
constexpr std::uint16_t kGreen = 0x03E0;
constexpr std::uint16_t kBlue = 0x001F;
constexpr std::uint16_t kWhite = 0x7FFF;
constexpr std::uint16_t kYellow = 0x7FE0;
constexpr std::uint16_t kCyan = 0x03FF;
constexpr std::uint16_t kMagenta = 0x7C1F;
constexpr std::uint16_t kBlack = 0x0000;
constexpr std::uint16_t kGrey = 0x4210;

Color rgb(std::uint16_t x1r5g5b5) {
  const auto expand = [](std::uint16_t five) {
    return static_cast<std::uint8_t>((five << 3) | (five >> 2));
  };
  return Color{expand((x1r5g5b5 >> 10) & 31), expand((x1r5g5b5 >> 5) & 31), expand(x1r5g5b5 & 31), 255};
}

/// A 6 x 6 frame: each of the nine pieces of a `2, 2, 2, 2` nine-slice its
/// own colour, so a composited pixel names the piece.
std::string frame_bmp() {
  const std::uint16_t piece[3][3] = {
      {kRed, kGreen, kBlue}, {kYellow, kGrey, kCyan}, {kMagenta, kWhite, kBlack}};
  std::vector<std::uint16_t> pixels(36);
  for (std::uint32_t y = 0; y < 6; ++y) {
    for (std::uint32_t x = 0; x < 6; ++x) pixels[y * 6 + x] = piece[y / 2][x / 2];
  }
  return bmp16(6, 6, pixels);
}

/// An 8 x 2 button grid: four 2 x 1 columns by two rows, every cell its own
/// colour: row 0 red/green/blue/yellow, row 1 cyan/magenta/white/grey.
std::string button_bmp() {
  const std::uint16_t cells[2][4] = {{kRed, kGreen, kBlue, kYellow}, {kCyan, kMagenta, kWhite, kGrey}};
  std::vector<std::uint16_t> pixels(16);
  for (std::uint32_t y = 0; y < 2; ++y) {
    for (std::uint32_t x = 0; x < 8; ++x) pixels[y * 8 + x] = cells[y][x / 2];
  }
  return bmp16(8, 2, pixels);
}

/// An arrow strip: three 2 x 2 frames.
std::string arrow_bmp() {
  return bmp16(6, 2, {kRed, kRed, kGreen, kGreen, kBlue, kBlue, kRed, kRed, kGreen, kGreen, kBlue, kBlue});
}

constexpr std::string_view kTemplate = R"([Params]
MenuRes=menures
ID_CAPTION=0x1007f
ID_LBULLET=0x75ee
ID_RBULLET=0x75ef

[StdDlg]
Type = Dialog
MinSize = 320, 200
MaxSize = 1024, 768
RectWH = 0, 0, 320, 200

[ShadowFrame2]
Type = Frame
Style = TRANSPARENT
Image = %MenuRes%/frame.bmp, 2, 2
Dividers=2,2,2,2

[StaticText]
Type = TextW
TextColor = 255,255,255

[ImgButton200]
Type = ImageButton
ImageType = ABBCD
Rows = 2
XFrames = 4
YFrames = 2
Image = %MenuRes%/button.bmp, -1, -1
FontColor=0,0,0

[LeftBullet]
Style = TRANSPARENT
Type = ImageButton
ImageType = AAAAA
Image = %MenuRes%/tapal.bmp, -1, -1
Id = %ID_LBULLET%

[TextListWithSelect]
Type = List
Style = TRANSPARENT, TEXTONLY, AUTOCALC, ROWS, SINGLE, TIGHTSCROLL
ItemHeight = 4

[VScroll]
Type = Scroll
Style = VSCROLL, AUTODISABLE, AUTOMOVE
ImageType = AABBC
Thumb = %MenuRes%/Scroll.BMP, -1, -1

[ScrollUp2]
Type = Button
ImageType = AABBC
Image = %MenuRes%/UpArrow.BMP

[ScrollDown2]
Type = Button
ImageType = AABBC
Image = %MenuRes%/DownArrow.BMP

[SingleLineEdit]
Type = EditW
Bufsize = 512
Style = TRANSPARENT, TABSTOP

[ComboBox]
Type = Combobox
Style = TRANSPARENT, AUTOSIZE
BkColor = 32, 32, 32
FrameColor1 = 220, 189, 129
ButtonImage = %MenuRes%/DownArrow.bmp
)";

constexpr std::string_view kGameMenu = R"([gamemenu]
Template = %TmplIni%, StdDlg
Style = TRANSPARENT, MODAL
RectWH = 0, 0, 64, 48
Esc = Close

[gamemenu Objects]
DialogBk
DialogFrame
Heading
LoadGame
Quit
Close
LBullet

[gamemenu Params]
Template=%TmplIni%, Params
TmplIni=Menuini/template.ini
DialogRect = 10,4,40,40
Button1Rect = 14, 10, 8, 4
Button2Rect = 14, 20, 8, 4
Button3Rect = 14, 30, 8, 4

[DialogBk]
Type = DarkFrame
RectWH = %DialogRect%

[DialogFrame]
Template = %TmplIni%, ShadowFrame2
RectWH = #left(DialogBk) - 2#,#top(DialogBk) - 2#,#width(DialogBk) + 4#,#height(DialogBk) + 4#

[Heading]
Template = %TmplIni%, StaticText
RectWH = #left(DialogBk)+4#, #top(DialogBk)+1#, #width(DialogBk)-8#, 4
Style = ALIGN_CENTER
Text = Main menu
Id = %ID_CAPTION%

[LoadGame]
Template = %TmplIni%, ImgButton200
RectWH = %Button1Rect%
Text = Load game
Id = 0x1001

[Quit]
Template = %TmplIni%, ImgButton200
RectWH = %Button2Rect%
Text = Quit
Id = 0x1006

[Close]
Template = %TmplIni%, ImgButton200
RectWH = %Button3Rect%
Text = Close
Id = 0x1007

[LBullet]
Template = %TmplIni%, LeftBullet
)";

constexpr std::string_view kSaveGame = R"([SaveGame]
Template = %TmplIni%, StdDlg
Style = TRANSPARENT
RectWH = 0, 0, 64, 64
Enter = SaveBtn
Esc = CancelBtn
Focus = NameEdit

[SaveGame Objects]
ChatFrame
NameEdit
NameLabel
CancelBtn
SaveBtn
List
List.ScrollUp
List.ScrollDown
List.VScroll
GameType
Browser
Picture

[SaveGame Params]
Template=%TmplIni%, Params
TmplIni=Menuini/template.ini
ListTop = 20

[NameLabel]
Template = %TmplIni%, StaticText
Text=Name:
RectWH = 2, 2, 10, 4

[ChatFrame]
Template = %TmplIni%, ShadowFrame2
Rect = #right(NameLabel)+2#, 2, #width(SaveGame)-4#, #2+4#

[NameEdit]
Template = %TmplIni%, SingleLineEdit
RectWH = #left(ChatFrame) + 1#, #top(ChatFrame)#, #width(ChatFrame) - 2#, #height(ChatFrame)#
id = 0x1004
Bufsize = 6

[SaveBtn]
Template = %TmplIni%, ImgButton200
RectWH = 40, 50, 8, 4
Text=Save
id = 0x1001

[CancelBtn]
Template = %TmplIni%, ImgButton200
RectWH = 50, 50, 8, 4
Text=Cancel
id = 0x1003

[List]
Template = %TmplIni%, TextListWithSelect
RectWH = 2, #ListTop#, 30, 12
ScrollID = List.VScroll
Id = 0x1015

[List.ScrollUp]
Template=%TmplIni%, ScrollUp2
RectWH = #right(List)+1#, #top(List)#, 2, 2
TargetId = List.VScroll
Id = #id(List) * 0x100 + 3#

[List.ScrollDown]
Template=%TmplIni%, ScrollDown2
RectWH = #left(List.ScrollUp)#, #bottom(List)-2#, 2, 2
TargetId = List.VScroll
Id = #id(List) * 0x100 + 4#

[List.VScroll]
Template=%TmplIni%, VScroll
Rect = #left(List.ScrollUp)#, #bottom(List.ScrollUp)#, #right(List.ScrollUp)#, #top(List.ScrollDown)#
TargetId = List
BackID = List.ScrollUp
ForwardID = List.ScrollDown
Id = #id(List) * 0x100 + 6#

[GameType]
Template = %TmplIni%, ComboBox
Rect = 2, 36, 30, 60
id=0x100A

[Browser]
Type = Control
RectWH = 34, 40, 28, 12
ItemHeight = 4
Id = 0x100FA

[Picture]
Type = Button
STYLE = INACTIVE
RectWH = 34, 54, 28, 8
ImageType = AAAAA
Id = 0x100FB
)";

struct Files {
  std::map<std::string, std::string> files{
      {"data/interface/menu/template.ini", std::string{kTemplate}},
      {"data/interface/menu/gamemenu.ini", std::string{kGameMenu}},
      {"data/interface/menu/savegame.ini", std::string{kSaveGame}},
      {"data/interface/editor/tools.ini", std::string{kSaveGame}},
      {"ui/menu/frame.bmp", frame_bmp()},
      {"ui/menu/button.bmp", button_bmp()},
      {"ui/menu/tapal.bmp", bmp16(2, 2, {kRed, kRed, kRed, kRed})},
      {"ui/menu/Scroll.BMP", arrow_bmp()},
      {"ui/menu/UpArrow.BMP", arrow_bmp()},
      {"ui/menu/DownArrow.BMP", arrow_bmp()},
      {"ui/menu/DownArrow.bmp", arrow_bmp()},
  };
  FileProvider provider = [this](std::string_view path) -> std::span<const std::byte> {
    for (const auto& [name, bytes] : files) {
      if (name.size() == path.size()) {
        bool same = true;
        for (std::size_t i = 0; i < name.size(); ++i) {
          const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
          const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(path[i])));
          if (a != b) {
            same = false;
            break;
          }
        }
        if (same) return as_bytes(bytes);
      }
    }
    return {};
  };
  ResourceCache cache{provider};

  Dialog open(std::string_view path) {
    Result<Screen> screen = load_screen(path, {}, provider);
    if (!screen.ok()) return Dialog(Screen{}, cache);
    return Dialog(std::move(screen.value()), cache);
  }
};

}  // namespace

/// An 8-bit BMP: `width` x `height` indices, a palette of 256 greys.
std::string bmp8(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& indices) {
  const std::size_t stride = (width + 3) / 4 * 4;
  const std::size_t offset = 14 + 40 + 256 * 4;
  std::string out(offset + stride * height, '\0');
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
  put32(10, static_cast<std::uint32_t>(offset));
  put32(14, 40);
  put32(18, width);
  put32(22, height);
  put16(26, 1);
  put16(28, 8);
  put32(30, 0);
  put32(46, 256);
  for (std::uint32_t i = 0; i < 256; ++i) {
    out[54 + 4 * i] = out[55 + 4 * i] = out[56 + 4 * i] = static_cast<char>(i);
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    const std::uint32_t row = height - 1 - y;
    for (std::uint32_t x = 0; x < width; ++x) {
      out[offset + row * stride + x] = static_cast<char>(indices[y * width + x]);
    }
  }
  return out;
}

TEST(campaign_map_art_decodes_tints_and_scales) {
  // A 4 x 2 index map: territory 3 on the left half, 5 on the right.
  const std::string mask_bmp = bmp8(4, 2, {3, 3, 5, 5, 3, 3, 5, 5});
  Result<IndexImage> mask = decode_bmp_indices(as_bytes(mask_bmp));
  REQUIRE(mask.ok());
  CHECK(mask->width == 4 && mask->height == 2);
  CHECK(mask->at(0, 0) == 3);
  CHECK(mask->at(3, 1) == 5);
  CHECK(mask->at(9, 9) == 0);
  // A 16-bit one is not an index map.
  CHECK(!decode_bmp_indices(as_bytes(bmp16(2, 1, {kRed, kRed}))).ok());

  // A grey picture the same size; tint territory 3 red at full saturation
  // (hue 0, saturation boosted from grey's zero -- stays grey, as the
  // saturation is a multiplier) and territory 5 from a coloured pixel.
  Image picture;
  picture.width = 4;
  picture.height = 2;
  picture.rgba.assign(4 * 2 * 4, 0);
  for (std::uint32_t y = 0; y < 2; ++y) {
    for (std::uint32_t x = 0; x < 4; ++x) {
      std::uint8_t* pixel = picture.pixel(x, y);
      pixel[0] = x < 2 ? 200 : 100;   // grey on the left, blue-ish on the right
      pixel[1] = x < 2 ? 200 : 120;
      pixel[2] = x < 2 ? 200 : 240;
      pixel[3] = 255;
    }
  }
  const IndexTint tints[] = {IndexTint{3, 0, 1024}, IndexTint{5, 512, 1024}};
  tint_by_index(picture, mask.value(), tints);
  // Grey has no saturation to keep: unchanged.
  CHECK(picture.pixel(0, 0)[0] == 200 && picture.pixel(0, 0)[2] == 200);
  // The blue-ish pixel keeps its value (240) and spread, with the hue at
  // 512 / 1536 = 120 degrees: green is the high channel now.
  CHECK(picture.pixel(3, 0)[1] == 240);
  CHECK(picture.pixel(3, 0)[0] < picture.pixel(3, 0)[1]);
  CHECK(picture.pixel(3, 0)[2] < picture.pixel(3, 0)[1]);

  // Scaled to 2 x 1: each output pixel the mean of a 2 x 2 block.
  const Image small = resample_box(picture, 2, 1);
  REQUIRE(small.width == 2 && small.height == 1);
  CHECK(small.pixel(0, 0)[0] == 200);
  CHECK(small.pixel(1, 0)[1] == 240);
  CHECK(resample_box(picture, 0, 1).empty());
}

TEST(dialog_loader_reads_the_menu_keys) {
  Files f;
  Result<Screen> screen = load_screen("menuini/gamemenu.ini", {}, f.provider);
  REQUIRE(screen.ok());
  CHECK(has(screen->style, Style::kModal));
  CHECK(screen->escape == "Close");
  CHECK(screen->design.width == 64);
  const Widget* frame = screen->find("DialogFrame");
  REQUIRE(frame != nullptr);
  CHECK(frame->kind == WidgetType::kFrame);
  CHECK(frame->has_dividers);
  CHECK(frame->divider_left == 2 && frame->divider_middle == 2);
  CHECK((frame->design == Rect{8, 2, 44, 44}));
  const Widget* button = screen->find("Quit");
  REQUIRE(button != nullptr);
  CHECK(button->kind == WidgetType::kImageButton);
  CHECK(button->xframes == 4 && button->yframes == 2);
  CHECK(button->has_id && button->id == 0x1006);
  CHECK(screen->find("DialogBk")->kind == WidgetType::kDarkFrame);
  CHECK(screen->warnings.empty());
}

TEST(dialog_loader_resolves_forward_self_and_parameter_references) {
  Files f;
  Result<Screen> screen = load_screen("menuini/savegame.ini", {}, f.provider);
  REQUIRE(screen.ok());
  CHECK(screen->enter == "SaveBtn");
  CHECK(screen->focus == "NameEdit");
  // `ChatFrame` is placed from `NameLabel`, listed after it, and from
  // `width(SaveGame)`, the dialog itself.
  const Widget* chat = screen->find("ChatFrame");
  REQUIRE(chat != nullptr);
  CHECK((chat->design == Rect{14, 2, 60 - 14, 4}));
  // `#ListTop#` is a bare parameter.
  const Widget* list = screen->find("List");
  REQUIRE(list != nullptr);
  CHECK((list->design == Rect{2, 20, 30, 12}));
  // `#id(List) * 0x100 + 3#`.
  CHECK(screen->find("List.ScrollUp")->id == 0x1015 * 0x100 + 3);
  CHECK(screen->warnings.empty());
}

TEST(dialog_loader_takes_the_leading_digits_of_a_number) {
  // A rectangle whose last field runs into the next key -- the shipped
  // `ADVCURMAP.INI` -- and one with a stray letter, `TERRAINSETTINGS.INI`.
  Files f;
  f.files["data/interface/menu/junk.ini"] =
      "[Junk]\nRectWH = 0, 0, 20, 10\n[Junk Objects]\nA\nB\n"
      "[A]\nType = TextW\nRectWH = 1, 2, 3, 4Text = Delete\n"
      "[B]\nType = TextW\nRectWH = 9, 19, 300, 44f\n";
  Result<Screen> screen = load_screen("menuini/junk.ini", {}, f.provider);
  REQUIRE(screen.ok());
  CHECK((screen->find("A")->design == Rect{1, 2, 3, 4}));
  CHECK((screen->find("B")->design == Rect{9, 19, 300, 44}));
  CHECK(screen->warnings.empty());
}

TEST(dialog_loader_keeps_the_local_rectangle_and_unions_the_styles) {
  Files f;
  f.files["data/interface/menu/tmpl2.ini"] =
      "[Params]\n"
      "[Multiline]\nType = TextW\nStyle = TRANSPARENT, MULTILINE, ALIGN_CENTER\n"
      "[Listing]\nType = List\nRect = 17, 50, 492, 529\nStyle = ROWS\n";
  f.files["data/interface/menu/over.ini"] =
      "[Over]\nRectWH = 0, 0, 100, 100\n[Over Objects]\nText\nList\nPlain\n"
      "[Over Params]\nTmplIni=menuini/tmpl2.ini\n"
      // `SELECTMAP.INI`'s shape: a local alignment over a multiline template.
      "[Text]\nTemplate = %TmplIni%, Multiline\nRectWH = 1, 2, 30, 40\nStyle = ALIGN_LEFT\n"
      // `ADVENTUREMENU.INI`'s: `RectWH` over a template's `Rect`.
      "[List]\nTemplate = %TmplIni%, Listing\nRectWH = 5, 6, 7, 8\n"
      "[Plain]\nTemplate = %TmplIni%, Multiline\nRectWH = 0, 0, 1, 1\n";
  Result<Screen> screen = load_screen("menuini/over.ini", {}, f.provider);
  REQUIRE(screen.ok());
  const Widget* text = screen->find("Text");
  REQUIRE(text != nullptr);
  CHECK(has(text->style, Style::kMultiline));
  CHECK(has(text->style, Style::kTransparent));
  CHECK(has(text->style, Style::kAlignLeft));
  CHECK(!has(text->style, Style::kAlignCenter));
  const Widget* list = screen->find("List");
  REQUIRE(list != nullptr);
  CHECK((list->design == Rect{5, 6, 7, 8}));
  CHECK(has(list->style, Style::kRows));
  // With no local style, the template's alignment stands.
  CHECK(has(screen->find("Plain")->style, Style::kAlignCenter));
}

TEST(dialog_presses_a_button_on_release_over_it) {
  Files f;
  Dialog dialog = f.open("menuini/gamemenu.ini");
  dialog.place(100, 100);
  CHECK(dialog.modal());
  // Down on Quit, up on Quit: the command.
  CHECK(dialog.mouse_down(100 + 15, 100 + 21).kind == DialogEvent::Kind::kNone);
  CHECK(dialog.content().pressed == "Quit");
  DialogEvent event = dialog.mouse_up(100 + 15, 100 + 21);
  CHECK(event.kind == DialogEvent::Kind::kCommand);
  CHECK(event.id == 0x1006);
  CHECK(event.widget == "Quit");
  CHECK(dialog.content().pressed.empty());
  // Down on Quit, up elsewhere: nothing.
  (void)dialog.mouse_down(100 + 15, 100 + 21);
  CHECK(dialog.mouse_up(100 + 15, 100 + 31).kind == DialogEvent::Kind::kNone);
  // Over the dark frame, nothing is pressed.
  (void)dialog.mouse_down(100 + 12, 100 + 6);
  CHECK(dialog.content().pressed.empty());
  // A disabled button is passed over.
  dialog.set_enabled("Close", false);
  CHECK(dialog.widget_at(100 + 15, 100 + 31) == nullptr);
  dialog.set_enabled("Close", true);
  CHECK(dialog.widget_at(100 + 15, 100 + 31)->name == "Close");
  // Hover.
  (void)dialog.mouse_move(100 + 15, 100 + 11);
  CHECK(dialog.content().hovered == "LoadGame");
  (void)dialog.mouse_move(0, 0);
  CHECK(dialog.content().hovered.empty());
}

TEST(dialog_escape_presses_the_named_widget_or_reports_itself) {
  Files f;
  Dialog dialog = f.open("menuini/gamemenu.ini");
  DialogEvent event = dialog.key(DialogKey::kEscape);
  CHECK(event.kind == DialogEvent::Kind::kCommand);
  CHECK(event.id == 0x1007);
  // With the Close button disabled, Escape has nothing to press.
  dialog.set_enabled("Close", false);
  CHECK(dialog.key(DialogKey::kEscape).kind == DialogEvent::Kind::kEscape);
  // Enter names nothing on this screen.
  CHECK(dialog.key(DialogKey::kEnter).kind == DialogEvent::Kind::kNone);
}

TEST(dialog_paints_the_dark_frame_the_nine_slice_and_the_button_cells) {
  Files f;
  Dialog dialog = f.open("menuini/gamemenu.ini");
  Canvas canvas;
  dialog.paint(canvas);
  CHECK(canvas.width() == 64 && canvas.height() == 48);
  // Outside everything: nothing.
  CHECK((canvas.at(0, 0) == Color{0, 0, 0, 0}));
  // The dark frame at (10,4)-(50,44): black at half cover, under the frame's
  // 2-pixel border which sits from (8,2) to (52,46).
  CHECK((canvas.at(30, 8) == Color{0, 0, 0, 128}));
  // The nine-slice: corners as they are, edges from the middle pieces.
  CHECK(canvas.at(8, 2) == rgb(kRed));        // top-left corner
  CHECK(canvas.at(51, 2) == rgb(kBlue));      // top-right corner
  CHECK(canvas.at(8, 45) == rgb(kMagenta));   // bottom-left corner
  CHECK(canvas.at(51, 45) == rgb(kBlack));    // bottom-right corner
  CHECK(canvas.at(30, 2) == rgb(kGreen));     // top edge, tiled
  CHECK(canvas.at(30, 45) == rgb(kWhite));    // bottom edge
  CHECK(canvas.at(8, 20) == rgb(kYellow));    // left edge
  CHECK(canvas.at(51, 20) == rgb(kCyan));     // right edge
  // The middle piece is the colour key -- as every shipped frame's is --
  // and the dark frame shows through it.
  CHECK((canvas.at(10, 4) == Color{0, 0, 0, 128}));
  // A resting button: column A of row 0, the 2 x 1 red cell centred in its
  // 8 x 4 rectangle at (14,10): drawn at (17, 11).
  CHECK(canvas.at(17, 11) == rgb(kRed));
  CHECK(canvas.at(18, 11) == rgb(kRed));
  CHECK((canvas.at(16, 11) == Color{0, 0, 0, 128}));  // beside the cell, the dark
  // Hovered: column B (green). Pressed: column B too (`ABBCD`).
  (void)dialog.mouse_move(15, 21);
  dialog.paint(canvas);
  CHECK(canvas.at(17, 21) == rgb(kGreen));
  (void)dialog.mouse_down(15, 21);
  dialog.paint(canvas);
  CHECK(canvas.at(17, 21) == rgb(kGreen));
  (void)dialog.mouse_up(0, 0);
  (void)dialog.mouse_move(0, 0);
  // Disabled: column C (blue).
  dialog.set_enabled("Quit", false);
  dialog.paint(canvas);
  CHECK(canvas.at(17, 21) == rgb(kBlue));
  // Row 1 of the grid: cyan.
  dialog.set_enabled("Quit", true);
  dialog.set_row("Quit", 1);
  dialog.paint(canvas);
  CHECK(canvas.at(17, 21) == rgb(kCyan));
  // The bullet with no rectangle stands beside the caption -- with no font
  // to measure the caption, nowhere -- and draws nothing.
  CHECK((dialog.widget_rect("LBullet") == Rect{}));
}

TEST(dialog_list_selects_on_click_activates_on_a_second_and_scrolls_by_its_arrows) {
  Files f;
  Dialog dialog = f.open("menuini/savegame.ini");
  dialog.set_items("List", {"one", "two", "three", "four", "five"});
  // Rows are 4 tall (`ItemHeight`), 12 tall list: three visible.
  DialogEvent event = dialog.mouse_down(5, 20 + 5);
  CHECK(event.kind == DialogEvent::Kind::kSelect);
  CHECK(event.widget == "List");
  CHECK(event.index == 1);
  CHECK(dialog.selected("List") == 1);
  CHECK(dialog.focused() == "List");
  event = dialog.mouse_down(5, 20 + 5);
  CHECK(event.kind == DialogEvent::Kind::kActivate);
  CHECK(event.index == 1);
  // The keyboard walks it, scrolling the selection into view.
  CHECK(dialog.key(DialogKey::kDown).index == 2);
  CHECK(dialog.key(DialogKey::kDown).index == 3);
  CHECK(dialog.content().state_of("List")->scroll == 1);
  CHECK(dialog.key(DialogKey::kEnd).index == 4);
  CHECK(dialog.content().state_of("List")->scroll == 2);
  // Enter on a selected row activates it.
  event = dialog.key(DialogKey::kEnter);
  // `Enter = SaveBtn` wins on this screen: a command.
  CHECK(event.kind == DialogEvent::Kind::kCommand);
  CHECK(event.id == 0x1001);
  // The scroll-up arrow is `List.VScroll`'s `BackID`: it moves the list.
  const Rect up = dialog.widget_rect("List.ScrollUp");
  (void)dialog.mouse_down(up.x, up.y);
  event = dialog.mouse_up(up.x, up.y);
  CHECK(event.kind == DialogEvent::Kind::kCommand);
  CHECK(event.id == 0x1015 * 0x100 + 3);
  CHECK(dialog.content().state_of("List")->scroll == 1);
  const Rect down = dialog.widget_rect("List.ScrollDown");
  (void)dialog.mouse_down(down.x, down.y);
  (void)dialog.mouse_up(down.x, down.y);
  CHECK(dialog.content().state_of("List")->scroll == 2);
  // And no further than the last page.
  (void)dialog.mouse_down(down.x, down.y);
  (void)dialog.mouse_up(down.x, down.y);
  CHECK(dialog.content().state_of("List")->scroll == 2);
}

TEST(dialog_edit_takes_typed_text_up_to_its_buffer) {
  Files f;
  Dialog dialog = f.open("menuini/savegame.ini");
  // `Focus = NameEdit` at open.
  CHECK(dialog.focused() == "NameEdit");
  DialogEvent event = dialog.text_input("ab");
  CHECK(event.kind == DialogEvent::Kind::kChange);
  CHECK(dialog.text("NameEdit") == "ab");
  (void)dialog.text_input("cdefgh");
  // `Bufsize = 6`: five characters and the terminator.
  CHECK(dialog.text("NameEdit") == "abcde");
  CHECK(dialog.key(DialogKey::kBackspace).kind == DialogEvent::Kind::kChange);
  CHECK(dialog.text("NameEdit") == "abcd");
  (void)dialog.key(DialogKey::kHome);
  (void)dialog.text_input("X");
  CHECK(dialog.text("NameEdit") == "Xabcd");
  (void)dialog.key(DialogKey::kRight);
  (void)dialog.key(DialogKey::kDelete);
  CHECK(dialog.text("NameEdit") == "Xacd");
  // Set from outside, read back.
  dialog.set_text("NameEdit", "quick");
  CHECK(dialog.text("NameEdit") == "quick");
  // Escape presses Cancel.
  CHECK(dialog.key(DialogKey::kEscape).id == 0x1003);
}

TEST(dialog_open_combobox_list_takes_the_hit_over_later_widgets) {
  // The setup screen: a row's nation list drops over the rows declared
  // after it, and a click in the list chooses a nation rather than pressing
  // what lies under it.
  Files f;
  f.files["data/interface/menu/rows.ini"] =
      "[Rows]\nRectWH = 0, 0, 64, 64\n[Rows Objects]\nNation\nBelow\n"
      "[Rows Params]\nTemplate=%TmplIni%, Params\nTmplIni=menuini/template.ini\n"
      "[Nation]\nTemplate = %TmplIni%, ComboBox\nRectWH = 2, 2, 40, 12\nId = 0x1022\n"
      "[Below]\nTemplate = %TmplIni%, ImgButton200\nRectWH = 2, 8, 40, 40\nId = 0x2012\n";
  Dialog dialog = f.open("menuini/rows.ini");
  dialog.set_items("Nation", {"Random", "Gaul", "Carthage"});
  dialog.select("Nation", 0);
  // Closed: the point below the box is the button's.
  CHECK(dialog.widget_at(10, 20)->name == "Below");
  (void)dialog.mouse_down(10, 3);
  REQUIRE(dialog.content().state_of("Nation")->open);
  // Open (`AUTOSIZE`, three 16-tall rows): the same point is the list's.
  CHECK(dialog.widget_at(10, 20)->name == "Nation");
  const DialogEvent event = dialog.mouse_down(10, 4 + 1 + 16 + 3);
  CHECK(event.kind == DialogEvent::Kind::kChange);
  CHECK(event.index == 1);
  CHECK(dialog.selected("Nation") == 1);
  CHECK(dialog.content().pressed.empty());
}

TEST(dialog_slider_follows_the_pointer_and_a_check_button_flips) {
  Files f;
  f.files["data/interface/menu/opts.ini"] =
      "[Opts]\nRectWH = 0, 0, 100, 40\n[Opts Objects]\nVolume\nCheck\n"
      "[Opts Params]\nTemplate=%TmplIni%, Params\nTmplIni=menuini/template.ini\n"
      // A slider: `HScroll` with no `TargetId`; the thumb strip is 6 x 2, so
      // the thumb is 2 wide and the run 40.
      "[Volume]\nType = Scroll\nStyle = HSCROLL, TRANSPARENT\nImageType = AABBC\n"
      "Thumb = %MenuRes%/Scroll.BMP, -1, -1\nRectWH = 10, 10, 42, 4\n"
      "[Check]\nType = Button\nStyle = TRISTATE, TRANSPARENT\nImageType = AAAA\nRows = 3\n"
      "Image = %MenuRes%/button.bmp, -1, -1\nRectWH = 10, 20, 8, 4\nId = 0x100B\n";
  Dialog dialog = f.open("menuini/opts.ini");
  dialog.set_value("Volume", 30);
  CHECK(dialog.value("Volume") == 30);
  // A press at the middle of the run: 50.
  DialogEvent event = dialog.mouse_down(10 + 1 + 20, 12);
  CHECK(event.kind == DialogEvent::Kind::kChange);
  CHECK(event.widget == "Volume");
  CHECK(event.index == 50);
  // Dragging past the end pins it at 100, even off the widget.
  event = dialog.mouse_move(90, 30);
  CHECK(event.kind == DialogEvent::Kind::kChange);
  CHECK(event.index == 100);
  CHECK(dialog.mouse_up(90, 30).kind == DialogEvent::Kind::kNone);
  CHECK(dialog.value("Volume") == 100);
  // Released, the pointer no longer moves it.
  CHECK(dialog.mouse_move(11, 12).kind == DialogEvent::Kind::kNone);
  // The check button flips its row and reports its id.
  (void)dialog.mouse_down(12, 22);
  event = dialog.mouse_up(12, 22);
  CHECK(event.kind == DialogEvent::Kind::kCommand);
  CHECK(event.id == 0x100B);
  CHECK(dialog.content().state_of("Check")->row == 1);
  (void)dialog.mouse_down(12, 22);
  (void)dialog.mouse_up(12, 22);
  CHECK(dialog.content().state_of("Check")->row == 0);
}

TEST(dialog_combobox_opens_on_click_and_chooses_a_row) {
  Files f;
  Dialog dialog = f.open("menuini/savegame.ini");
  dialog.set_items("GameType", {"Skirmish", "Adventure"});
  dialog.select("GameType", 0);
  // Closed, only its box (the arrow's height plus two) takes the click.
  const Rect rect = dialog.widget_rect("GameType");
  CHECK((rect == Rect{2, 36, 28, 24}));
  CHECK(dialog.widget_at(5, 36 + 10) == nullptr);
  (void)dialog.mouse_down(5, 36 + 1);
  CHECK(dialog.content().state_of("GameType")->open);
  // Open, the rows below the box: the second row (font-less rows are 16
  // tall) is at box bottom + 1 + 16.
  const std::int32_t closed = combobox_closed_height(f.cache, *dialog.screen().find("GameType"));
  CHECK(closed == 4);
  DialogEvent event = dialog.mouse_down(5, 36 + closed + 1 + 16);
  CHECK(event.kind == DialogEvent::Kind::kChange);
  CHECK(event.index == 1);
  CHECK(dialog.selected("GameType") == 1);
  CHECK(!dialog.content().state_of("GameType")->open);
  // A click elsewhere closes an open list without choosing.
  (void)dialog.mouse_down(5, 36 + 1);
  CHECK(dialog.content().state_of("GameType")->open);
  (void)dialog.mouse_down(60, 60);
  CHECK(!dialog.content().state_of("GameType")->open);
  CHECK(dialog.selected("GameType") == 1);
}

TEST(dialog_tab_buttons_show_their_tab_and_radio_buttons_clear_their_group) {
  // The toolkit's two id-keyed rules (0x0066d515, 0x0066d4f0), as the
  // editor's `AdvObjProps.ini` relies on them: `0x0003000N` is the tab
  // button of tab N, a top byte is the tab a widget belongs to, and
  // `0x0002xxNN` is a radio button of the group `0x0002xx00`.
  Files f;
  f.files["data/interface/menu/tabs.ini"] =
      "[Tabs]\nRectWH = 0, 0, 100, 60\n[Tabs Objects]\nTab1\nTab2\nOnOne\nOnTwo\nAlways\nGold\nFood\n"
      "[Tabs Params]\nTemplate=%TmplIni%, Params\nTmplIni=menuini/template.ini\n"
      "[Tab1]\nType = Button\nStyle = TRANSPARENT\nImageType = AAAA\nRows = 2\n"
      "Image = %MenuRes%/button.bmp, -1, -1\nRectWH = 0, 0, 8, 4\nId = 0x30001\n"
      "[Tab2]\nType = Button\nStyle = TRANSPARENT\nImageType = AAAA\nRows = 2\n"
      "Image = %MenuRes%/button.bmp, -1, -1\nRectWH = 10, 0, 8, 4\nId = 0x30002\n"
      "[OnOne]\nType = Button\nStyle = TRANSPARENT\nImageType = AAAA\n"
      "Image = %MenuRes%/button.bmp, -1, -1\nRectWH = 0, 10, 8, 4\nId = 0x01000007\n"
      "[OnTwo]\nType = Button\nStyle = TRANSPARENT\nImageType = AAAA\n"
      "Image = %MenuRes%/button.bmp, -1, -1\nRectWH = 10, 10, 8, 4\nId = 0x02000007\n"
      "[Always]\nType = Button\nStyle = TRANSPARENT\nImageType = AAAA\n"
      "Image = %MenuRes%/button.bmp, -1, -1\nRectWH = 20, 10, 8, 4\nId = 0x1234\n"
      "[Gold]\nType = Button\nStyle = TRANSPARENT\nImageType = AAAA\nRows = 2\n"
      "Image = %MenuRes%/button.bmp, -1, -1\nRectWH = 0, 20, 8, 4\nId = 0x4020021\n"
      "[Food]\nType = Button\nStyle = TRANSPARENT\nImageType = AAAA\nRows = 2\n"
      "Image = %MenuRes%/button.bmp, -1, -1\nRectWH = 10, 20, 8, 4\nId = 0x4020022\n";
  Dialog dialog = f.open("menuini/tabs.ini");
  // Opened on tab 1 (0x0066d5f0): tab 2's widget is hidden, tab 1's and
  // the untabbed one show, and the first tab button is the pressed one.
  CHECK(dialog.tab() == 1);
  CHECK(dialog.widget_at(2, 12) != nullptr && dialog.widget_at(2, 12)->name == "OnOne");
  CHECK(dialog.widget_at(12, 12) == nullptr);
  CHECK(dialog.widget_at(22, 12) != nullptr && dialog.widget_at(22, 12)->name == "Always");
  CHECK(dialog.content().state_of("Tab1")->row == 1);
  CHECK(dialog.content().state_of("Tab2")->row == 0);
  // The second tab button: its tab's widgets, and the command still posts.
  (void)dialog.mouse_down(12, 2);
  DialogEvent event = dialog.mouse_up(12, 2);
  CHECK(event.kind == DialogEvent::Kind::kCommand);
  CHECK(event.id == 0x30002);
  CHECK(dialog.tab() == 2);
  CHECK(dialog.widget_at(2, 12) == nullptr);
  CHECK(dialog.widget_at(12, 12) != nullptr && dialog.widget_at(12, 12)->name == "OnTwo");
  CHECK(dialog.widget_at(22, 12) != nullptr);
  CHECK(dialog.content().state_of("Tab1")->row == 0);
  CHECK(dialog.content().state_of("Tab2")->row == 1);
  // The radio pair (on tab 4 -- switch there first): pressing one puts its
  // row at 1 and its partner's at 0.
  dialog.set_tab(4);
  (void)dialog.mouse_down(12, 22);
  event = dialog.mouse_up(12, 22);
  CHECK(event.kind == DialogEvent::Kind::kCommand && event.id == 0x4020022);
  CHECK(dialog.content().state_of("Food")->row == 1);
  CHECK(dialog.content().state_of("Gold")->row == 0);
  (void)dialog.mouse_down(2, 22);
  (void)dialog.mouse_up(2, 22);
  CHECK(dialog.content().state_of("Gold")->row == 1);
  CHECK(dialog.content().state_of("Food")->row == 0);
}

TEST(dialog_an_edit_combobox_takes_typed_text_in_place_of_its_choice) {
  // `GRP_Combo` is `Style = EDIT`: a group that does not exist yet is
  // typed. Typing starts from the shown item and un-chooses it.
  Files f;
  f.files["data/interface/menu/grp.ini"] =
      "[Grp]\nRectWH = 0, 0, 100, 60\n[Grp Objects]\nCombo\n"
      "[Grp Params]\nTemplate=%TmplIni%, Params\nTmplIni=menuini/template.ini\n"
      "[Combo]\nType = Combobox\nStyle = TRANSPARENT, EDIT\nRectWH = 2, 2, 60, 40\nId = 0x2000001\n";
  Dialog dialog = f.open("menuini/grp.ini");
  dialog.set_items("Combo", {"Alpha", "Beta"});
  dialog.select("Combo", 1);
  dialog.focus("Combo");
  DialogEvent event = dialog.text_input("s");
  CHECK(event.kind == DialogEvent::Kind::kChange);
  CHECK(dialog.text("Combo") == "Betas");
  CHECK(dialog.selected("Combo") == -1);
  (void)dialog.key(DialogKey::kBackspace);
  (void)dialog.key(DialogKey::kBackspace);
  CHECK(dialog.text("Combo") == "Bet");
  // Choosing a row again drops the typed text.
  dialog.select("Combo", 0);
  CHECK(dialog.selected("Combo") == 0);
}

// --------------------------------------------------------------------------
// Inline markup: `<color r g b>` and `<imagetransp path>`.
// --------------------------------------------------------------------------

namespace {

/// The smallest APF that `Font::parse` accepts: one range, `first..last`
/// inclusive, every glyph a solid `width` x `height` block with no bearing
/// and no kerning. Height 4 = ascent 3 + descent 1.
std::string apf(std::uint32_t first, std::uint32_t last, std::uint32_t width = 3) {
  const std::uint32_t count = last - first + 1;
  const std::uint32_t height = 4;
  const std::size_t metrics = 0x20;
  const std::size_t table = metrics + 14 * 4;
  const std::size_t block = table + 4 + 16;
  const std::uint32_t kern_offset = 16 + 32 * count;
  const std::uint32_t pixel_offset = kern_offset;
  // One run byte per glyph row: alpha 7, run `width`.
  const std::uint32_t pixel_size = count * height;
  const std::uint32_t block_size = pixel_offset + pixel_size;
  std::string out(block + block_size, '\0');
  auto put32 = [&](std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
  };
  out[0] = 'A';
  out[1] = 'B';
  out[2] = 'C';
  out[3] = 'F';
  put32(4, static_cast<std::uint32_t>(metrics));
  put32(metrics + 0, height);
  put32(metrics + 5 * 4, 3);
  put32(metrics + 6 * 4, 1);
  put32(table, 1);
  put32(table + 4 + 0, static_cast<std::uint32_t>(block));
  put32(table + 4 + 4, block_size);
  put32(table + 4 + 8, first);
  put32(table + 4 + 12, count);
  put32(block + 0, kern_offset);
  put32(block + 4, 0);
  put32(block + 8, pixel_offset);
  put32(block + 12, pixel_size);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t record = block + 16 + 32 * i;
    put32(record + 0, 0);      // abc_a
    put32(record + 4, width);  // ink width
    put32(record + 8, 0);      // abc_c
    put32(record + 16, 0);     // top
    put32(record + 24, height - 1);  // bottom
    put32(record + 28, i * height);  // pixel start
    for (std::uint32_t row = 0; row < height; ++row) {
      out[block + pixel_offset + i * height + row] =
          static_cast<char>(0xE0u | static_cast<std::uint32_t>(width - 1));
    }
  }
  return out;
}

}  // namespace

TEST(markup_parses_colours_images_and_breaks) {
  // The command tooltip's title, as 0x004f3590 formats it.
  const std::vector<MarkedLine> title = parse_markup(
      "<color 255 255 0>Attack<color 255 255 255>  <imagetransp gameres/infobar/common/hotkey.bmp> "
      "<color 255 255 255>A",
      Color{});
  REQUIRE(title.size() == 1);
  REQUIRE(title[0].spans.size() == 4);
  CHECK(title[0].spans[0].text == "Attack");
  CHECK((title[0].spans[0].ink == Color{255, 255, 0, 255}));
  CHECK(title[0].spans[1].text == "  ");
  CHECK((title[0].spans[1].ink == Color{255, 255, 255, 255}));
  CHECK(title[0].spans[2].image == "ui/infobar/common/hotkey.bmp");  // alias resolved
  CHECK(title[0].spans[2].keyed);
  CHECK(title[0].spans[3].text == " A");

  // The bonus help text: the two characters `\n` break, a colour holds across
  // the break, and a trailing separator space is dropped.
  const std::vector<MarkedLine> bonus = parse_markup(
      "<color 255 255 0>Select:<color 255 0 0>\\n<imagetransp menures/smallgold.bmp> 2000 gold \n"
      "<image x.bmp>", Color{});
  REQUIRE(bonus.size() == 3);
  CHECK(bonus[0].spans.size() == 1);
  REQUIRE(bonus[1].spans.size() == 2);
  CHECK(bonus[1].spans[0].image == "ui/menu/smallgold.bmp");
  CHECK(bonus[1].spans[1].text == " 2000 gold");
  CHECK((bonus[1].spans[1].ink == Color{255, 0, 0, 255}));
  REQUIRE(bonus[2].spans.size() == 1);
  CHECK(!bonus[2].spans[0].keyed);

  // An unknown tag is text (`<None>` is an item's text); a lone `<` is text;
  // a malformed colour is ignored.
  const std::vector<MarkedLine> odd = parse_markup("a <None> < c<color 1 2>d", Color{});
  REQUIRE(odd.size() == 1);
  REQUIRE(odd[0].spans.size() == 1);
  CHECK(odd[0].spans[0].text == "a <None> < cd");
  CHECK((odd[0].spans[0].ink == Color{}));
  CHECK(strip_markup("<color 255 0 0>x\\ny <imagetransp a.bmp>z") == "x\ny z");
  CHECK(parse_markup("", Color{}).empty());
}

TEST(markup_draws_text_in_its_colour_beside_the_keyed_image) {
  Files f;
  f.files["fonts/test.apf"] = apf(' ', 'z');
  // A 2 x 2 bitmap keyed at its top-left: green key, red elsewhere.
  f.files["ui/menu/gold.bmp"] = bmp16(2, 2, {kGreen, kRed, kRed, kRed});
  const Font* font = f.cache.font("fonts/test.apf");
  REQUIRE(font != nullptr);
  CHECK(font->height() == 4);
  CHECK(font->measure("ab") == 6);

  const std::vector<MarkedLine> lines =
      parse_markup("<color 255 0 0>ab<imagetransp menures/gold.bmp>c", Color{});
  REQUIRE(lines.size() == 1);
  CHECK(marked_line_width(f.cache, *font, lines[0]) == 6 + 2 + 3);
  CHECK(marked_line_height(f.cache, *font, lines[0]) == 4);
  // A 20-pixel icon grows the line to hold it.
  f.files["ui/menu/tall.bmp"] = bmp16(1, 20, std::vector<std::uint16_t>(20, kRed));
  CHECK(marked_line_height(f.cache, *font,
                           parse_markup("<image menures/tall.bmp>", Color{})[0]) == 20);

  Canvas canvas;
  canvas.resize(16, 6);
  canvas.clear(Color{0, 0, 0, 255});
  const Rect clip{0, 0, 16, 6};
  CHECK(draw_marked_line(canvas, f.cache, *font, lines[0], 1, 1, clip) == 11);
  // `a` in red at x 1..3, the image at 7..8 with its keyed corner leaving
  // the black, and `c` at 9..11 still red: the colour holds past the image.
  CHECK((canvas.at(1, 1) == Color{255, 0, 0, 255}));
  CHECK((canvas.at(7, 1) == Color{0, 0, 0, 255}));   // keyed pixel
  CHECK((canvas.at(8, 1) == Color{255, 0, 0, 255}));  // red pixel of the icon
  CHECK((canvas.at(9, 1) == Color{255, 0, 0, 255}));
  CHECK((canvas.at(12, 1) == Color{0, 0, 0, 255}));
}

TEST(dialog_brush_picker_chooses_a_frame_and_a_spin_counts_its_edit) {
  // The editor's tool panes: `BrushSize` offers the columns `Frames` names
  // and reports the chosen one as its value; `Spin` counts its `TargetId`
  // number edit up on the top half and down on the bottom, and the change
  // is the edit's.
  Files f;
  // A 3 x 2 strip of 1 x 1 brushes: row 0 red/green/blue, row 1 the chosen
  // look, cyan/magenta/white.
  f.files["ui/editor/brushes.bmp"] = bmp16(3, 2, {kRed, kGreen, kBlue, kCyan, kMagenta, kWhite});
  f.files["ui/editor/spin.bmp"] = bmp16(1, 2, {kYellow, kGrey});
  f.files["ui/editor/spinup.bmp"] = bmp16(1, 2, {kBlack, kBlack});
  f.files["data/interface/editor/pane.ini"] =
      "[Pane]\nRectWH = 0, 0, 40, 20\n[Pane Objects]\nBrushes\nAmount\nSpin\n"
      "[Pane Params]\nTemplate=%TmplIni%, Params\nTmplIni=menuini/template.ini\n"
      "[Brushes]\nType = BrushSize\nItemWidth = 1\nItemHeight = 1\nImage = ui/editor/brushes.bmp\n"
      "Frames = 13\nRectWH = 2, 2, 3, 1\nId = 16\n"
      "[Amount]\nType = EditW\nStyle = NUMBER\nBufsize = 8\nRectWH = 10, 2, 10, 4\nText = 5\n"
      "[Spin]\nType = Spin\nButtons = ui/editor/spin.bmp\nUpButtonPressed = ui/editor/spinup.bmp\n"
      "TargetId = Amount\nRectWH = 30, 2, 1, 2\nId = 0x12\n";
  Dialog dialog = f.open("editorini/pane.ini");
  // Nothing chosen: both offered frames from row 0, in `Frames` order.
  Canvas canvas;
  dialog.paint(canvas);
  CHECK(canvas.at(2, 2) == rgb(kRed));
  CHECK(canvas.at(3, 2) == rgb(kBlue));
  CHECK(canvas.at(4, 2).alpha == 0);  // two frames offered, not three
  // The second offered column is frame 3.
  DialogEvent event = dialog.mouse_down(3, 2);
  CHECK(event.kind == DialogEvent::Kind::kChange);
  CHECK(event.widget == "Brushes");
  CHECK(event.index == 3);
  CHECK(dialog.value("Brushes") == 3);
  (void)dialog.mouse_up(3, 2);
  dialog.paint(canvas);
  CHECK(canvas.at(2, 2) == rgb(kRed));
  CHECK(canvas.at(3, 2) == rgb(kWhite));  // the chosen look
  // Choosing it again is not a change; set from outside, read back.
  CHECK(dialog.mouse_down(3, 2).kind == DialogEvent::Kind::kNone);
  dialog.set_value("Brushes", 1);
  CHECK(dialog.value("Brushes") == 1);
  // The spin: the top half counts up, the bottom down, and the pressed half
  // wears its own picture while held.
  event = dialog.mouse_down(30, 2);
  CHECK(event.kind == DialogEvent::Kind::kChange);
  CHECK(event.widget == "Amount");
  CHECK(event.index == 6);
  CHECK(dialog.text("Amount") == "6");
  dialog.paint(canvas);
  CHECK(canvas.at(30, 2) == rgb(kBlack));
  // A release over it is not a command, whatever its `Id`.
  CHECK(dialog.mouse_up(30, 2).kind == DialogEvent::Kind::kNone);
  dialog.paint(canvas);
  CHECK(canvas.at(30, 2) == rgb(kYellow));
  event = dialog.mouse_down(30, 3);
  CHECK(event.index == 5);
  (void)dialog.mouse_up(30, 3);
  CHECK(dialog.text("Amount") == "5");
  // A number edit keeps only digits and the sign.
  dialog.focus("Amount");
  (void)dialog.text_input("a-1");
  CHECK(dialog.text("Amount") == "5-1");
}

TEST(dialog_a_control_filled_with_items_acts_as_a_list_and_the_editors_ink_is_black) {
  // The editor's `Browser` (`MAPTOOLSDLG.INI`) is a bare `Control` the exe
  // attaches a native tree to; filled with rows it draws, selects and takes
  // the keyboard as a `List`, in the screen's first font, and -- under an
  // editor path -- in black.
  Files f;
  Dialog dialog = f.open("editorini/tools.ini");
  CHECK(dialog.content().default_ink == (Color{0, 0, 0, 255}));
  CHECK(dialog.content().default_font.empty());  // the fixture names no font
  // Empty, it is inert: a click on it is nothing.
  CHECK(dialog.mouse_down(40, 42).kind == DialogEvent::Kind::kNone);
  dialog.set_items("Browser", {"+ Structures", "+ Units"});
  DialogEvent event = dialog.mouse_down(40, 40 + 5);
  CHECK(event.kind == DialogEvent::Kind::kSelect);
  CHECK(event.widget == "Browser");
  CHECK(event.index == 1);
  CHECK(dialog.focused() == "Browser");
  CHECK(dialog.key(DialogKey::kUp).index == 0);
  Dialog menu = f.open("menuini/savegame.ini");
  CHECK(menu.content().default_ink == (Color{255, 255, 255, 255}));
}

TEST(dialog_a_composed_bitmap_on_an_inactive_button_draws_and_takes_the_click) {
  // `MapPlace.ini`'s `BMP` is an `INACTIVE` button the exe hangs the
  // minimap on and reads the click off. Bare, the button is nothing to the
  // mouse; carrying a picture it answers a click with the point local to it.
  Files f;
  Dialog dialog = f.open("editorini/tools.ini");
  CHECK(dialog.mouse_down(40, 56).kind == DialogEvent::Kind::kNone);
  Image picture;
  picture.width = 28;
  picture.height = 8;
  picture.rgba.assign(28 * 8 * 4, std::uint8_t{0});
  for (std::size_t i = 0; i < 28 * 8; ++i) {
    picture.rgba[i * 4 + 0] = 255;
    picture.rgba[i * 4 + 3] = 255;
  }
  dialog.content().state("Picture").bitmap = &picture;
  DialogEvent event = dialog.mouse_down(40, 56);
  CHECK(event.kind == DialogEvent::Kind::kClick);
  CHECK(event.widget == "Picture");
  CHECK(event.x == 6);
  CHECK(event.y == 2);
  CHECK(dialog.mouse_down(40, 50).kind == DialogEvent::Kind::kNone);  // beside it: nothing
  Canvas canvas(64, 64);
  canvas.clear(Color{0, 0, 0, 255});
  dialog.paint(canvas);
  const std::size_t at = (static_cast<std::size_t>(56) * 64 + 40) * 4;
  CHECK(canvas.pixels()[at] == 255 && canvas.pixels()[at + 1] == 0);
  dialog.content().state("Picture").hidden = true;
  dialog.content().state("Picture").has_hidden = true;
  CHECK(dialog.mouse_down(40, 56).kind == DialogEvent::Kind::kNone);
}
