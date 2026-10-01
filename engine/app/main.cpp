// The windowed application: the thin wiring between platform services and the
// freestanding core. Real work belongs in one of those two, not here.
//
// Two modes, selected by the command line.
//
//   --map Scenarios/Crossroads.BFHP   open a shipped map and render it: the
//                                     textured, corner-blended ground, every
//                                     object placed on it, shadows, and the
//                                     ZBINS depth sort. Pan with the arrow keys
//                                     or by dragging.
//   (default)                         the sprite renderer's acceptance test:
//                                     one unit sheet drawn through four player
//                                     palettes over one atlas of indices.
//
// Both are wiring. The map is loaded by `core::WorldMap` from byte spans the
// platform hands it, and drawn by `platform::MapRenderer`; nothing below
// interprets a byte or places a pixel.

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "imperivm/core/formats/color.hpp"
#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/game/help.hpp"
#include "imperivm/core/game/localization.hpp"
#include "imperivm/core/game/profile.hpp"
#include "imperivm/core/formats/rle.hpp"
#include "imperivm/core/version.hpp"
#include "imperivm/core/script/ast.hpp"
#include "imperivm/core/script/compiler.hpp"
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/host_setup.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/netcmds.hpp"
#include "imperivm/core/sim/tick.hpp"
#include "imperivm/core/sim/cmdbar.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/infobar.hpp"
#include "imperivm/core/sim/session.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/ui/dialog.hpp"
#if IMPERIVM_HAVE_NET
#include "netplay.hpp"
#endif
#include "imperivm/core/ui/markup.hpp"
#include "imperivm/core/world/editor.hpp"
#include "imperivm/core/xml.hpp"
#include "imperivm/core/xml_patch.hpp"
#include "imperivm/core/world/map.hpp"
#include "imperivm/core/world/map_writer.hpp"
#include "imperivm/platform/audio.hpp"
#include "imperivm/core/formats/lzis.hpp"
#include "imperivm/sound/music.hpp"
#include "imperivm/sound/placement.hpp"
#include "imperivm/sound/sound_entity.hpp"
#include "imperivm/platform/fog_view.hpp"
#include "imperivm/platform/frame_loop.hpp"
#include "imperivm/platform/map_renderer.hpp"
#include "imperivm/gamedata/installation.hpp"
#include "imperivm/gamedata/map_source.hpp"
#include "imperivm/gamedata/map_writer.hpp"
#include "imperivm/gamedata/campaign_file.hpp"
#include "imperivm/gamedata/save_file.hpp"
#include "imperivm/core/version.hpp"
#include "imperivm/platform/render_target.hpp"
#include "imperivm/platform/sprite_atlas.hpp"
#include "imperivm/platform/sprite_renderer.hpp"
#include "imperivm/platform/vfs.hpp"
#include "imperivm/platform/ui_renderer.hpp"
#include "imperivm/platform/window.hpp"
#include "imperivm/platform/world_view.hpp"

namespace {

namespace platform = imperivm::platform;
namespace core = imperivm::core;
namespace gamedata = imperivm::gamedata;

struct Arguments {
  std::string game;
  std::string map;     ///< a container inside the installation; empty = sheet demo
  int map_index = -1;  ///< which `Maps/<n>` to open; -1 uses game.xml's start_map
  std::string sprite = "UNITS\\RPRAETORIAN\\WALK.RLE.MMP";
  /// `--sprite` was given: the sheet viewer rather than the front.
  bool sprite_given = false;
  std::string shadow;  ///< defaults to the sprite with _SHADOW before .RLE.MMP
  std::string screenshot;
  /// One entry per player to draw. The default is the single red the Python
  /// exporter uses, so a screenshot is directly comparable with its output;
  /// `--players` swaps in four, which is what demonstrates that one index
  /// atlas serves every player at a kilobyte of palette each.
  std::vector<core::Rgb888> teams{core::Rgb888{255, 0, 0}};
  bool players = false;
  int width = 1100;
  int height = 850;
  int frames = 0;  ///< quit after this many; 0 means run until closed
  int at_x = -1;   ///< world position to open the view on; -1 uses the map's own
  int at_y = -1;
  bool borderless = false;
  bool fullscreen = false;
  bool animate = false;
  /// Run the simulation, not just draw the map. See `start_play`.
  bool play = false;
  /// `--edit`: the map opens in the editor -- placed, not played.
  bool edit = false;
  /// `--no-fog`: the whole map drawn, for a screenshot or a debugging run.
  bool no_fog = false;
  /// `--declare-won`: end the human's match won on the first frame it runs,
  /// the way a victory sequence would -- `imrun`'s knob of the same name,
  /// for driving what follows a win (the end-game menu, the campaign carry,
  /// the conquest's victory) without winning.
  bool declare_won = false;
  /// A save file to resume. Its manifest names the map, the map number and
  /// the seed, so `--map` and `--map-index` are taken from it and `--play` is
  /// implied. See `start`.
  std::string load;
  /// The campaign file to carry a conquest across its missions. Defaults to
  /// `<user data>/<conquest>.campaign.ini` for a container that holds a
  /// `territories.xml`, and to nothing for any other.
  std::string campaign;
  /// `--user-dir DIR`, over `$IMPERIVM_USER_DIR`: where everything the app
  /// writes goes, and where it reads back what it wrote. See
  /// `choose_user_directory`.
  std::string user_dir;
  /// Object ids to put in the local player's selection at the start, comma
  /// separated -- `--select 12,13`. What a screenshot of the info bar needs
  /// and a mouse cannot give a headless run.
  std::string select;
  /// The info bar's starting tab, for a screenshot of the skills strip.
  int tab = 0;
  /// A pointer position to hold, `--hover X,Y`, for a screenshot of a tooltip.
  int hover_x = -1;
  int hover_y = -1;
  /// A menu to open at start, `--menu game|confirm|save|load|options`, for a
  /// screenshot of it.
  std::string menu;
  /// Set when the map was chosen from the front rather than the command
  /// line: the game menu's Quit goes back there instead of out.
  bool from_front = false;
  /// `--language NAME`: the `local/<NAME>.pak` to mount, over `Settings.ini`'s
  /// `[Language] Default=`. `none` mounts nothing and the keys show as
  /// themselves, which is English.
  std::string language;
  /// Input to play, one step per frame from the third: `--input
  /// "click:640,400;text:name;key:Return;wait:5"`, and `press:<widget>`,
  /// `press:<list>@<row>`, `press:<list>@=<text>` for a click aimed at a
  /// named widget of an open dialog. The steps are posted as SDL events and
  /// take the same path a hand's would, so a headless run can press a
  /// menu's buttons and a screenshot can show what happened.
  std::string input;
  /// `--turn-length N`: every unnetworked turn is N game-time units,
  /// whatever the speed. Zero, the default, converts `turn_interval` at the
  /// clock's speed instead, which is what the original's clock does
  /// (0x00528a80: real length x speed / 1000). A fast-forward for a scripted
  /// run, and nothing a player can reach.
  int turn_length = 0;
  /// Which player slot the mouse commands. 0-based, like `PlayerId`.
  int player = 0;
  /// `--difficulty N`, 0 easy to 2 hard: this match's, over the options'
  /// `Difficulty`, which it neither reads nor writes. -1, the default, takes
  /// the options'. `imrun` sets none, so its matches run at the match's own
  /// 0; a run that is to be compared with one says `--difficulty 0`.
  int difficulty = -1;
  /// The real length of an unnetworked turn, in milliseconds: one turn runs
  /// every `turn_interval` ms and is worth that many ms of game time at the
  /// clock's speed. **This engine's number**: a networked turn's real length
  /// is agreed (200..800 in the dumps), a single-player one was not read, and
  /// 100 keeps a turn short enough that the world moves smoothly.
  int turn_interval = 100;
  /// Whether `--width`/`--height` were given: the options' resolution
  /// yields to the command line.
#if IMPERIVM_HAVE_NET
  /// `--host PORT` or `--join HOST:PORT`, and what goes with them. See
  /// `netplay.hpp`.
  imperivm::app::NetPlayOptions net;
#endif
  bool size_given = false;
  /// `--headless`, or `IMPERIVM_HEADLESS=1`: no window, no sound device, no
  /// focus taken; the frame is drawn at `--width` x `--height` into a texture
  /// and a screenshot reads it back as it would from a window. See `kUsage`.
  bool headless = false;
};

/// `--help`. Not every flag -- `parse_arguments` is the list -- but the ones a
/// person starts from, and the one thing a test run changes.
constexpr const char* kUsage = R"(usage: imperivm [--game DIR] [--map CONTAINER [--map-index N]] [options]

  --game DIR            the installation (Packs/, rle.mmp); without --map, the front
  --map CONTAINER       a map inside it, e.g. Scenarios/Crossroads.BFHP
  --play                run the simulation; --player N picks the seat,
                        --difficulty N (0..2) the match's, over the options'
  --edit                open the map in the editor
  --load SAVE           resume a save
  --user-dir DIR        keep saves, settings, profiles and campaigns in DIR;
                        also $IMPERIVM_USER_DIR. See "User data" below
  --host PORT           host a networked match; --join HOST:PORT joins one
  --width W --height H  the window's size (the headless frame's, in pixels)
  --frames N            quit after N frames; --screenshot PNG shoots the last
  --input SCRIPT        scripted input, one step a frame: click:X,Y rclick:X,Y
                        drag:X1,Y1,X2,Y2 hold:X1,Y1,X2,Y2 release:X,Y
                        move:X,Y key:NAME text:S press:WIDGET wait:N shot:PNG
                        turn:N over select:WHAT look:WHAT, separated by ';' (turn:N waits for the N-th turn,
                        over for the end; WHAT is --select's words)
  --turn-length N       every turn N game-time units, whatever the speed
  --headless            no window: see below
  --help                this

Headless. --headless, or IMPERIVM_HEADLESS=1 in the environment, runs with no
window: the GPU device renders into a texture of --width x --height pixels,
shot: and --screenshot read that texture back, sound goes to SDL's dummy
driver, the process never takes focus, and frames are paced at 60 Hz as a
display would pace them. Everything else -- the simulation, input, netplay --
is unchanged. The test suite sets IMPERIVM_HEADLESS=1 (tests/conftest.py), so
a run started from a shell that inherited it is headless too. To see a run in a
window, clear it:

  IMPERIVM_HEADLESS=0 ./build/engine/app/imperivm --game $GAME --map ... --play

User data. What the app writes -- the quick save (F5, read back by F9), the
save dialog's slots, settings.ini, Profiles/, <conquest>.campaign.ini, the
editor's maps -- and reads back of its own goes to one directory. It is
--user-dir DIR if given, else $IMPERIVM_USER_DIR, else, in a window, the
installation's Saves/ (the editor's maps: its Scenarios/). Headless with
neither, it is a fresh temporary directory, removed at exit: a scripted run
never reaches the player's saves. The test suite sets IMPERIVM_USER_DIR to a
directory of its own. The start-up output names the directory in use.
)";

bool ends_with(const std::string& text, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

Arguments parse_arguments(int argc, char** argv) {
  Arguments args;
  bool size_given = false;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
    if (flag == "--game") {
      args.game = next();
    } else if (flag == "--map") {
      args.map = next();
    } else if (flag == "--play") {
      args.play = true;
    } else if (flag == "--edit") {
      args.play = true;
      args.edit = true;
    } else if (flag == "--no-fog") {
      args.no_fog = true;
    } else if (flag == "--declare-won") {
      args.declare_won = true;
    } else if (flag == "--load") {
      args.load = next();
    } else if (flag == "--campaign") {
      args.campaign = next();
    } else if (flag == "--user-dir") {
      args.user_dir = next();
    } else if (flag == "--select") {
      args.select = next();
    } else if (flag == "--tab") {
      args.tab = SDL_atoi(next().c_str());
    } else if (flag == "--hover") {
      const std::string value = next();
      int x = 0;
      int y = 0;
      if (SDL_sscanf(value.c_str(), "%d,%d", &x, &y) == 2) {
        args.hover_x = x;
        args.hover_y = y;
      }
    } else if (flag == "--menu") {
      args.menu = next();
    } else if (flag == "--input") {
      args.input = next();
    } else if (flag == "--language") {
      args.language = next();
    } else if (flag == "--player") {
      args.player = SDL_atoi(next().c_str());
    } else if (flag == "--difficulty") {
      args.difficulty = std::clamp(SDL_atoi(next().c_str()), 0, 2);
    } else if (flag == "--turn-length") {
      args.turn_length = SDL_atoi(next().c_str());
    } else if (flag == "--turn-interval") {
      args.turn_interval = SDL_atoi(next().c_str());
    } else if (flag == "--map-index") {
      args.map_index = SDL_atoi(next().c_str());
    } else if (flag == "--at") {
      // `--at X,Y`, in world units.
      const std::string value = next();
      int x = 0;
      int y = 0;
      if (SDL_sscanf(value.c_str(), "%d,%d", &x, &y) == 2) {
        args.at_x = x;
        args.at_y = y;
      }
    } else if (flag == "--sprite") {
      args.sprite = next();
      args.sprite_given = true;
    } else if (flag == "--shadow") {
      args.shadow = next();
    } else if (flag == "--screenshot") {
      args.screenshot = next();
    } else if (flag == "--width") {
      args.width = SDL_atoi(next().c_str());
      size_given = true;
    } else if (flag == "--height") {
      args.height = SDL_atoi(next().c_str());
      size_given = true;
    } else if (flag == "--frames") {
      args.frames = SDL_atoi(next().c_str());
    } else if (flag == "--borderless") {
      args.borderless = true;
    } else if (flag == "--fullscreen") {
      args.fullscreen = true;
    } else if (flag == "--animate") {
      args.animate = true;
    } else if (flag == "--players") {
      args.players = true;
      args.animate = true;
      args.teams = {core::Rgb888{255, 0, 0}, core::Rgb888{40, 70, 200},
                    core::Rgb888{30, 150, 50}, core::Rgb888{225, 200, 40}};
#if IMPERIVM_HAVE_NET
    } else if (flag == "--host") {
      args.net.host_port = SDL_atoi(next().c_str());
    } else if (flag == "--join") {
      args.net.join = next();
    } else if (flag == "--seats") {
      // `--seats 1,3`: the slots joiners take, in the order they arrive.
      const std::string value = next();
      std::size_t at = 0;
      while (at < value.size()) {
        const std::size_t comma = value.find(',', at);
        const std::string item = value.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!item.empty()) args.net.seats.push_back(static_cast<core::PlayerId>(SDL_atoi(item.c_str())));
        if (comma == std::string::npos) break;
        at = comma + 1;
      }
    } else if (flag == "--name") {
      args.net.name = next();
    } else if (flag == "--net-delay") {
      args.net.delay = static_cast<std::uint32_t>(SDL_atoi(next().c_str()));
    } else if (flag == "--net-drop") {
      args.net.drop = static_cast<std::uint32_t>(std::clamp(SDL_atoi(next().c_str()), 0, 999));
    } else if (flag == "--net-turns") {
      args.net.stop_after = static_cast<std::uint32_t>(SDL_atoi(next().c_str()));
    } else if (flag == "--net-speed") {
      // TURN:SPEED -- ask for SPEED per mille once TURN turns have run.
      const std::string value = next();
      args.net.speed_turn = static_cast<std::uint32_t>(SDL_atoi(value.c_str()));
      const std::size_t colon = value.find(':');
      args.net.speed = colon == std::string::npos ? 0 : SDL_atoi(value.c_str() + colon + 1);
    } else if (flag == "--net-timeout") {
      args.net.timeout_ms = static_cast<std::uint32_t>(SDL_atoi(next().c_str()));
#endif
    } else if (flag == "--headless") {
      args.headless = true;
    } else if (flag == "--team") {
      const std::string value = next();
      int r = 0;
      int g = 0;
      int b = 0;
      if (SDL_sscanf(value.c_str(), "%d,%d,%d", &r, &g, &b) == 3) {
        args.teams = {core::Rgb888{static_cast<std::uint8_t>(r),
                                   static_cast<std::uint8_t>(g),
                                   static_cast<std::uint8_t>(b)}};
      }
    }
  }
  if (args.shadow.empty() && ends_with(args.sprite, ".RLE.MMP")) {
    args.shadow = args.sprite.substr(0, args.sprite.size() - 8) + "_SHADOW.RLE.MMP";
  }
  // A map wants a window shaped like a play view, not like a sprite sheet.
  if (!args.map.empty() && !size_given) {
    args.width = 1500;
    args.height = 1000;
  }
  args.size_given = size_given;
  return args;
}

/// Where the app's own files go: see "User data" in `kUsage`.
///
/// **Why a temporary directory for headless, rather than refusing writes**: a
/// headless run is a test or a tool driving the real code, and a save that
/// is refused takes a different path from one that is made -- F5 then F9
/// would stop being testable, and OK on the options screen would print a
/// failure the player never sees. A directory of its own keeps every path
/// the player's run takes, and cannot reach the installation, because it
/// is not under it. It is removed at exit, so nothing is left behind.
struct UserDirectory {
  std::filesystem::path path;
  /// The installation's `Saves/`, which is where a person's run keeps it.
  bool installation = false;
  /// Made here, and removed with everything in it at exit.
  bool temporary = false;
  /// For the start-up line.
  std::string why;
};

std::optional<UserDirectory> choose_user_directory(const Arguments& args,
                                                   const std::filesystem::path& root) {
  UserDirectory chosen;
  std::error_code failed;
  const char* env = std::getenv("IMPERIVM_USER_DIR");
  if (!args.user_dir.empty() || (env != nullptr && env[0] != '\0')) {
    chosen.path = args.user_dir.empty() ? std::filesystem::path(env) : std::filesystem::path(args.user_dir);
    chosen.why = args.user_dir.empty() ? "$IMPERIVM_USER_DIR" : "--user-dir";
    chosen.path = std::filesystem::absolute(chosen.path, failed);
    std::filesystem::create_directories(chosen.path, failed);
    if (!std::filesystem::is_directory(chosen.path, failed)) {
      std::fprintf(stderr, "user data: %s (%s) is not a directory and cannot be made one\n",
                   chosen.path.string().c_str(), chosen.why.c_str());
      return std::nullopt;
    }
    return chosen;
  }
  if (!args.headless) {
    chosen.path = root / "Saves";
    chosen.installation = true;
    chosen.why = "the installation's";
    return chosen;
  }
  const std::filesystem::path base = std::filesystem::temp_directory_path(failed);
  if (failed) {
    std::fprintf(stderr, "user data: no temporary directory (%s); give --user-dir DIR\n",
                 failed.message().c_str());
    return std::nullopt;
  }
  // `create_directory` answers false for one that already exists, so a
  // name another process took is skipped rather than shared.
  const std::uint64_t stamp = SDL_GetTicksNS() ^ (static_cast<std::uint64_t>(SDL_GetCurrentThreadID()) << 20);
  for (std::uint64_t attempt = 0; attempt < 1000; ++attempt) {
    const std::filesystem::path candidate =
        base / ("imperivm-user-" + std::to_string(stamp + attempt));
    if (std::filesystem::create_directory(candidate, failed)) {
      chosen.path = candidate;
      chosen.temporary = true;
      chosen.why = "headless with no --user-dir or $IMPERIVM_USER_DIR: a fresh temporary "
                   "directory, removed at exit; the installation's Saves/ is neither read nor written";
      return chosen;
    }
  }
  std::fprintf(stderr, "user data: could not make a temporary directory under %s; give --user-dir DIR\n",
               base.string().c_str());
  return std::nullopt;
}

/// Removes a temporary user directory when the application goes.
struct TemporaryDirectory {
  std::filesystem::path path;
  TemporaryDirectory() = default;
  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
  ~TemporaryDirectory() {
    if (path.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

/// For the sound's log lines and the music's files (defined beside
/// `play_music`).
std::string sound_word(std::string_view text);
std::filesystem::path find_loose_file(const std::filesystem::path& root, std::string_view relative);

/// Everything the frame callback touches. It lives on the heap because on the
/// web `run_frame_loop` returns before the loop has run -- see
/// `frame_loop_returns_early`.
class Application {
 public:
  bool start(const Arguments& args);
  bool tick();

 private:
  bool start_map();
  bool start_play();
  bool look_at_local_player();
  bool start_sheet();
  void draw_sheet();
  void tick_map(platform::Window::Frame& frame);
  void tick_play(platform::Window::Frame& frame);
  void draw_status();
  void handle_play_mouse(const SDL_Event& event);
  void tick_sheet(platform::Window::Frame& frame);
  void take_screenshot(const std::string& path);

  Arguments args_;
  platform::Vfs vfs_;
  platform::Window window_;
  platform::SpriteRenderer renderer_;
  platform::RenderTarget target_;
  /// The sound device and its mixer, one pool of channels per sound type
  /// (docs/engine/sound.md).
  platform::Audio audio_;
  /// The installation's sound entities, read as they are first played.
  imperivm::sound::SoundBank sound_bank_{[this](std::string_view path) { return vfs_.read(path); }};
  /// `config.ini`'s `[SoundConfig]` and `[SoundChannels]`.
  imperivm::sound::SoundConfig sound_config_ = imperivm::sound::default_sound_config();
  /// The C runtime's `rand()`, which draws variants in the original. Both
  /// generators here are the presentation's own: a sound is chosen on one
  /// screen, and nothing about it may reach the world or its RNG.
  imperivm::sound::SoundRandom variant_random_;
  /// The draws the original makes on its unsynchronised generator: which
  /// selected unit speaks, and whether a hero's army speaks for him.
  imperivm::sound::SoundRandom speaker_random_{0x5EED};
  /// `IMPERIVM_SOUND_LOG=1`: one `sound:` line per sound played, which is
  /// what tests assert on -- never on the audio.
  bool sound_log_ = false;
  /// The view the live sounds were last placed against (`follow_view`).
  std::optional<imperivm::sound::View> sound_view_placed_;
  /// The menus' music (0x00748600): wanted from the moment the menus are
  /// entered until a play of it succeeds, looked at once in
  /// `kMenuMusicPasses` frames; `CONST.INI [GamePlay] PregameUIMusic`, read
  /// once.
  bool menu_music_wanted_ = false;
  std::int32_t menu_music_countdown_ = 0;
  std::optional<std::string> menu_music_;
  /// The match's music (0x00551150): `music/`'s tracks as the match found
  /// them, the last one drawn, and the game time of the next look. The draw
  /// is the presentation's own generator, never the world's.
  std::vector<std::string> music_tracks_;
  std::optional<std::size_t> music_last_;
  std::int64_t music_next_look_ = imperivm::sound::kMusicCheckMs;
  imperivm::sound::SoundRandom music_random_{0x0517};

  // -- map mode
  gamedata::MapContainer container_;
  /// The `Maps/<n>` directory `start_map` chose, which the simulation and
  /// every save name too.
  std::string map_directory_;
  /// The container's scripts ahead of the pack's; see the session build.
  std::unique_ptr<gamedata::ContainerScripts> scripts_;
  core::WorldMap world_;
  platform::MapRenderer map_;
  bool map_mode_ = false;
  bool dragging_ = false;
  /// The view is centred on the first drawable frame, not at load: on a HiDPI
  /// display the drawable is not the window, and centring against the wrong
  /// one puts the camera half a screen out.
  bool look_pending_ = false;
  std::int32_t look_x_ = 0;
  std::int32_t look_y_ = 0;
  /// The scripts' camera (`HostContext::view`): what `View` writes and
  /// `ViewPos` reads. `view_seen_` is where this app last put the camera, so
  /// a difference between the two is a `View` the frame has not followed yet.
  core::sim::ViewState view_state_;
  core::sim::Point view_seen_;

  // -- play mode
  //
  // Deliberately thin, and deliberately easy to delete. The interface is 150
  // declarative INI files and a separate effort is building the interpreter for
  // the 24 in-game ones; everything here that draws is scaffolding that gets
  // replaced by the real command bar. What is *not* scaffolding is the loop:
  // advancing turns on a real clock and letting somebody watch is how bugs a
  // test cannot phrase get found, and it is the reason to build this at all.
  /// Loads an entity on demand out of the mounted packs, and hands each one to
  /// the view so it can correct the sprite grid from the frame table -- the
  /// entity XML disagrees with it for about one image in ten, and the frame
  /// table wins.
  class AppEntities final : public core::sim::EntityResolver, public core::sim::PassMaskResolver {
   public:
    AppEntities(platform::Vfs& vfs, platform::WorldView& view) : vfs_(&vfs), view_(&view) {}

    /// The entity's `.pass`, read once out of the packs and kept: what the
    /// session's rebuild at match start and the editor's stamps use.
    const core::edit::PassMask* mask_of(const core::Entity* entity) override {
      return masks_.resolve(entity, [this](std::string_view path) {
        return platform::as_core_bytes(vfs_->read(path));
      });
    }

    const core::Entity* resolve(std::string_view path) const override {
      if (path.empty()) return nullptr;
      const std::string key(path);
      if (const core::Entity* found = registry_.find(key)) return found;
      const platform::ByteSpan document = vfs_->read(key);
      if (document.empty()) return nullptr;
      auto loaded = registry_.load(key, platform::as_core_bytes(document));
      if (!loaded.ok()) return nullptr;
      // `load` returns a const pointer into the registry; the view needs to
      // rewrite the geometry, and the registry owns the storage either way.
      if (core::Entity* mutable_entity = registry_.find_mutable(key)) {
        view_->adopt_frame_tables(*mutable_entity);
      }
      return loaded.value();
    }

   private:
    platform::Vfs* vfs_;
    platform::WorldView* view_;
    mutable core::EntityLibrary registry_;
    core::edit::PassMaskLibrary masks_;
  };

  platform::WorldView world_view_;
  /// The fog of war as the local player sees it: the fog manager's light
  /// grid (`sim/fog_light.hpp`), fed the camera's rect and the clock every
  /// frame -- its own timer slides it to what the world shows. `--edit` and
  /// `--no-fog` leave it empty.
  platform::FogView fog_view_;
  bool fog_hiding_ = false;
  void refresh_fog();
  /// The templates' ground, taken from the session into the map the view
  /// draws from -- the app keeps its own copy of the layers -- and the
  /// light re-baked over it. After `start_match` and after a load.
  void sync_session_ground();
  platform::UiRenderer ui_;
  /// Whose screen this is. Defaults to the map's own `start_player`.
  core::PlayerId local_player_ = 0;
  /// A drag box in window coordinates, live while the left button is down.
  bool band_select_ = false;
  std::int32_t band_x0_ = 0;
  std::int32_t band_y0_ = 0;
  std::int32_t band_x1_ = 0;
  std::int32_t band_y1_ = 0;
  /// Whether the last frame drew the band: what the `drawn:` line counts.
  bool band_drawn_ = false;
  void draw_band();
  /// The selection rings, `UI\SELECTIONS\<n>.RLE`, and what marks each
  /// object in view (`platform::WorldView::Marks`).
  platform::SelectionRings rings_;
  platform::WorldView::Marks marks_for(const core::sim::WorldObject& object);
  /// The class properties the marks read, by class index, read once.
  struct ClassMarks {
    bool read = false;
    std::int32_t selection_radius = 0;
    std::int32_t radius = 0;
    std::int32_t healthbar_type = 0;
    std::int32_t healthbar_offset = 0;
    std::int32_t max_health = 0;
  };
  std::vector<ClassMarks> class_marks_;
  const ClassMarks& class_marks(core::ClassIndex index);
  /// The in-world health bars' global mode, `[0x009edd20]`: 0 none -- the
  /// start, the word being in `.bss` -- 1 everyone's, 2 the local player's
  /// and those it holds ceasefire with, 3 the others. The backtick key
  /// toggles it between 0 and the mode it last left (`[0x008250c8]`, which
  /// ships as 1); Ctrl+backtick steps it 1, 3, 2, 1; a backtick held past
  /// 500 ms turns it off when released (0x00627ef0).
  std::int32_t bar_mode_ = 0;
  std::int32_t bar_last_mode_ = 1;
  std::uint64_t bar_key_ticks_ = 0;
  void bar_key(const SDL_Event& event);
  std::vector<core::ObjectId> picked_;
  bool reported_outcome_ = false;
  /// Trap messages and verbs already printed, so each is said once.
  std::set<std::string> reported_;
  platform::Camera camera_;
  /// `game.xml`'s season: which entity a class stands as, and so which
  /// mask it stamps -- `Crops1` has three seasonal entities and no other.
  core::Season season_ = core::Season::base;
  /// The container's `game.xml`, for the editor's caption and root node.
  core::GameProperties game_;
  std::unique_ptr<AppEntities> entities_;
  std::unique_ptr<gamedata::Installation> install_;
  gamedata::MapPayloads payloads_;
  core::script::HostRegistry registry_;
  std::unique_ptr<core::sim::GameSession> session_;
  /// What the info bar shows, read from the data through the session. See
  /// `sim/infobar.hpp`; the app only carries its answer to the renderer.
  std::unique_ptr<core::sim::InfoBar> infobar_;
  std::uint64_t infobar_turn_ = ~0ull;
  std::size_t infobar_selection_ = 0;
  std::uint32_t infobar_generation_ = 0;
  void refresh_infobar();
  /// The command bar's buttons, from the same session. See `sim/cmdbar.hpp`.
  std::unique_ptr<core::sim::CommandBar> cmdbar_;
  std::vector<core::sim::CommandButton> buttons_;
  /// Which button the pointer is over and which is held, by row name, and
  /// the row waiting for a click on the map.
  std::string hovered_button_;
  std::string pressed_button_;
  std::string pending_command_;
  bool bars_dirty_ = true;
  /// The info bar's tab -- 0 for the army and items, 1 for the skills -- and
  /// when a `Switch` with a `SwitchBackTime` will flip it back to 0.
  std::uint32_t infobar_tab_ = 0;
  std::uint64_t tab_back_at_ = 0;
  /// The skills switch's blink: on while the selection's head is a hero of
  /// the local player's with points to spend, its phase from the clock and
  /// the widget's `BlinkTime` (`BarContent::blink`).
  bool blink_active_ = false;
  bool blink_on_ = false;
  /// Where the pointer last was, so a bar refresh can re-read the tooltip
  /// under it: a cost that turns affordable while the pointer rests would
  /// otherwise stay red until the next motion.
  std::int32_t pointer_x_ = -1;
  std::int32_t pointer_y_ = -1;
  void refresh_cmdbar();
  /// `keys`: Shift appends, Ctrl is the order's `bModifier` -- the two keys
  /// the original's bar reads when it posts (0x005e39f0).
  void press_button(std::int32_t index, core::sim::CommandBar::Keys keys);
  /// The object's queue on one line, running command first: what an order
  /// the player just gave left there.
  void print_queue(core::ObjectId id) const;
  void update_hover(std::int32_t x, std::int32_t y);
  [[nodiscard]] std::vector<std::string> command_tooltip(const core::sim::CommandButton& button) const;
  /// A widget's `HelpText` (or `Rollover`), translated in its screen's context; empty for none.
  [[nodiscard]] std::vector<std::string> widget_tooltip(const core::ui::Widget* widget) const;
  bool play_mode_ = false;
  bool paused_ = false;
  /// Whether `PAUSED.INI` is the menu on the stack.
  bool paused_menu_ = false;
  /// Take the pause word down so that another menu can open over the game.
  void dismiss_paused() {
    if (!paused_menu_) return;
    close_all_menus();
    paused_menu_ = false;
  }
  /// The map identity every save this app writes records, and every load it
  /// applies is checked against: the container's installation-relative
  /// spelling plus the map number played. See `gamedata/save_file.hpp`.
  std::string identity_;
  /// The world seed the session was built from, and the one a save records.
  std::uint32_t seed_ = 1;
#if IMPERIVM_HAVE_NET
  /// The networked match, when `--host` or `--join` made one. Null otherwise,
  /// and every branch on it is the difference between the two kinds of game.
  std::unique_ptr<imperivm::app::NetPlay> net_;
  bool start_net();
  /// The start's rows and rules as the setup screen's own, on every peer.
  void apply_net_rows(const core::sim::Start& start);
#endif
  /// Whether this is a networked match -- the one question the rest of the
  /// file asks, and false in a build with no sockets.
  [[nodiscard]] bool networked() const noexcept {
#if IMPERIVM_HAVE_NET
    return net_ != nullptr;
#else
    return false;
#endif
  }
  /// `--load`'s file, read before the map opens and applied once the session
  /// has started.
  std::optional<core::sim::SaveFileContents> pending_load_;
  /// Where F5 writes and F9 reads: `<user data>/quicksave.bfhp`. For a
  /// person's run the user data is `<installation>/Saves/`: inside the
  /// installation because that is where the original keeps its slots
  /// (`AdvSaveGame/`), and a directory of its own because none of the
  /// original's names is a format this engine writes. See
  /// `choose_user_directory`.
  std::filesystem::path quicksave_;
  /// Every file this app writes, and reads back as its own, is under this.
  std::filesystem::path user_dir_;
  /// It is the installation's `Saves/`, which is a person's run.
  bool user_dir_is_installation_ = true;
  TemporaryDirectory temporary_user_dir_;
  bool quicksave();
  bool quickload();
  /// Every save this app writes and reads goes through these; the quick
  /// slot and the save dialog's named slots are files in the same directory.
  bool save_to(const std::filesystem::path& file);
  bool load_from(const std::filesystem::path& file);
  [[nodiscard]] std::filesystem::path saves_directory() const;
  [[nodiscard]] std::vector<std::string> save_names() const;

  // -- menus
  //
  // The menus are the shipped `MENU/*.INI` screens, opened on the interface
  // renderer's dialog stack and driven by `core::ui::Dialog`; what a button
  // *does* is decided here, one handler per open menu, by the `Id` the file
  // gives it. `GAMEMENU.INI` is the in-game menu (F10, the bar's button,
  // Escape); it opens `CONFIRM.INI`, `SAVEGAME.INI`, `LOADGAME.INI` and
  // `GAMEOPTIONS.INI`. The simulation stands still while any menu is open,
  // which is this engine's choice for a single-player game rather than a
  // reading of the original.
  using MenuHandler = std::function<void(const core::ui::DialogEvent&, core::ui::Dialog&)>;
  /// One per open dialog, **indexed by its position in the stack**. Every
  /// operation on the stack has to be mirrored here or a dialog gets
  /// another's handler: open pushes, close erases at the same index, and
  /// anything that *reorders* has to rotate this the same way, which is
  /// what `raise_menu` is for. Nothing reordered the stack until the
  /// editor's `nextwnd`, and the first version of that did not know this
  /// vector was here -- a raise left every handler one place out, so a
  /// click on the palette ran the explorer's.
  std::vector<MenuHandler> menu_handlers_;
  bool quit_requested_ = false;
  /// The dialog a button went down on, so its release goes to the same one.
  std::size_t mouse_menu_ = 0;
  core::ui::Dialog* open_menu(std::string_view path, MenuHandler handler, std::string_view section = {});
  void close_menu();
  void close_menu_at(std::size_t index);
  void close_menu(core::ui::Dialog* dialog);
  void close_all_menus();
  /// Close every front screen but `MENUBACK.INI`, the bottom of the stack.
  void close_to_background();
  /// Move a dialog to the top of the stack, handlers with it. The only way
  /// the app may reorder the stack: the renderer's own raise moves the
  /// dialogs and knows nothing about `menu_handlers_`.
  void raise_menu(core::ui::Dialog* dialog);
  /// Routes an event to the top menu. True when the menu took it.
  bool menus_take(const SDL_Event& event);
  void open_game_menu();
  void open_confirm(std::string question, std::function<void()> yes);
  void open_save_menu();
  void open_load_menu();
  void open_options_menu();
  /// The settings `GAMEOPTIONS.INI` edits: the game's `Settings.ini`
  /// `[Options]` keys, read from the installation's file for their
  /// defaults and written to `Saves/settings.ini`, the engine's own, so the
  /// original's file is never touched. What the engine acts on: the game
  /// speed, the scroll speed (the keyboard pan), the resolution (the
  /// window), the animation switches (kept), and the sound switches and
  /// sliders, which govern the sound types as `sound_type_on` and
  /// `sound_type_volume` say (docs/engine/sound.md).
  ///
  /// **`game_speed` is a speed position, read**: the options screen sends
  /// `sim::game_speed_from_option(game_speed)` per mille as a `set_speed`
  /// order when OK is pressed during a match (0x006e7ff0), and the start of
  /// a match writes the position back from the speed it starts at
  /// (0x006e71e0), so the shipped `GameSpeed=13` is `NormalSpeed` 1000 on
  /// its way back, and 999 on its way out again. It does **not** set the
  /// speed a match starts at; `NormalSpeed` does (0x0052683b).
  struct Settings {
    int resolution = 0;
    int sound_volume = 68;
    int music_volume = 80;
    int speech_volume = 52;
    bool reverse_speakers = false;
    bool no_object_animations = false;
    bool no_water_animation = false;
    bool music = false;
    bool sound_fx = true;
    bool nature_sounds = true;
    bool speech = true;
    bool conversations = true;
    int game_speed = 13;
    int scroll_speed = 50;
    int difficulty = 1;
  };
  Settings settings_;
  /// The skirmish setup's rules (`SETTINGS.INI` beside the players'
  /// screen), kept between games under the keys `gbr.exe` persists in
  /// `Profiles/<name>/player.ini [Player]` (0x0056ba60): `nofogofwar`,
  /// `noexploration`, `sharedcontrol`, `sharedsupport`, `nobonus`,
  /// `startinggold` (-1 = Default), `worldpop` (a percent), `victorycond`
  /// (a game script's basename, empty = `Map Default`), `victorytreshold`
  /// (the limit's number, sic). The shipped profile reads `startinggold=-1
  /// worldpop=150 victorycond=1 Elimination sharedsupport=1`; the defaults
  /// here are the automatch path's (0x00702906): Normal, Default, fog and
  /// exploration on.
  struct SetupRules {
    bool no_fog = false;
    bool no_exploration = false;
    bool shared_support = false;
    bool shared_control = false;
    bool no_bonuses = false;
    int starting_gold = -1;
    int world_population = 100;
    std::string victory;    ///< game script basename, e.g. `1 ELIMINATION`
    std::string threshold;  ///< the limit's number as text
  };
  SetupRules rules_;

  /// The map editor -- the tools palette over a loaded map, `gbr.exe`'s
  /// native mode with no root screen. The palette (`MAPTOOLSDLG.INI`)
  /// carries a native tree the exe builds at 0x004a29a0, which is a
  /// `Control` here filled with one row per visible node: the roots in the
  /// exe's order -- Decorations from `DECORS.INI`'s `group`/`subgroup`/
  /// `name`, Delete decorations, Height with Raise/lower, Smooth and Set
  /// height, Terrains by `TERRAINS.XML` type with a leaf per layer, the
  /// classes' `edittree_pos` tree, Areas, Edit objects. A node chosen
  /// docks its own settings dialog where `SettingsPosCtl` stands and makes
  /// its tool the map's: the brushes (`core/world/editor.hpp` has their
  /// arithmetic and the addresses), placing, the areas, and the default
  /// tool that selects, drags, deletes and turns. The keys are the shipped
  /// `DATA\vxAction.xml`. **Readings, labelled:** the tree's `+`/`-`
  /// markers, F2 for the save dialog, the brushes' starting slot and the
  /// panes' starting numbers are this engine's; the exe's Escape is
  /// `tooldefault`, and leaving the editor from the default tool is ours.
  enum class EditorTool : std::uint8_t {
    kEdit,         ///< DefaultTool.ini: select, drag, delete, turn
    kPlace,        ///< PlaceObj.ini: a class leaf
    kTerrain,      ///< TerrainSettings.ini
    kHeight,       ///< the three height panes
    kDecor,        ///< DecorSettings.ini
    kDecorDelete,  ///< DecorDelete.ini
    kPlaceArea,    ///< a circular or rectangular area at the click
    kEditArea,     ///< move and resize areas by their handles
  };
  struct EditorNode {
    std::string label;     ///< translated `<leaf>@editortree/<path>`
    std::string path;      ///< `Structures/Stronghold (Gaul)`
    std::string class_id;  ///< a class leaf's class, empty otherwise
    std::int32_t parent = -1;
    std::vector<std::int32_t> children;
    bool expanded = false;
    /// What choosing the node does. A branch with no tool only folds.
    bool is_tool = false;
    EditorTool tool = EditorTool::kEdit;
    std::string pane;                  ///< `editorini/<X>.ini`, or none
    std::vector<std::int32_t> values;  ///< terrain layers, or decor kinds
    bool water_group = false;          ///< the Terrains/Water group: deep water
    core::edit::HeightTool height_tool = core::edit::HeightTool::kRaiseLower;
    bool circle = false;               ///< kPlaceArea: a circle, else a rectangle
  };
  /// An area the editor knows: the object, its shape, and its name -- the
  /// type-0 group that names the object, `Unnamed` for a fresh one.
  struct EditorArea {
    core::ObjectId id = core::kNoObject;
    core::MapArea shape;
    std::string name;
  };
  /// One shipped shortcut of `vxAction.xml`.
  struct EditorKey {
    std::string action;
    SDL_Keycode key = SDLK_UNKNOWN;
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
  };
  /// What the property sheet changed on one object, for the save: laid
  /// over the authored record on top of the world's position and
  /// direction, the way `Editor::directions` is. The world is changed as
  /// the sheet is worked, so the map shows it; this is the part the world
  /// does not carry back into `map.obj.xml` on its own.
  struct PropertyEdits {
    std::optional<std::int32_t> player;          ///< 1-based, 0 for no owner
    std::optional<std::int32_t> health_percent;
    std::optional<std::int32_t> stamina;
    std::optional<std::int32_t> level;           ///< the attribute's own, earned level - 1
    std::uint32_t flags_set = 0;                 ///< `flags` bits raised
    std::uint32_t flags_clear = 0;               ///< `flags` bits lowered
    std::uint32_t unit_flags_set = 0;            ///< `UnitFlags` bits raised
    std::uint32_t unit_flags_clear = 0;          ///< `UnitFlags` bits lowered
    std::optional<std::string> script_name;      ///< the type-0 group; empty unnames
    std::optional<std::string> display_name;
    std::vector<std::string> groups_added;       ///< type-1 groups, in order
    std::vector<std::string> groups_removed;
    std::optional<std::vector<std::string>> items;  ///< `slot0..3`
    /// The skills tab: the five rows, `hs<Skill>="points"`; an unset row
    /// is `count`.
    std::optional<std::vector<std::pair<core::sim::HeroSkill, std::int32_t>>> skills;
    std::optional<std::string> icon;  ///< the `Icon` attribute, a bitmap path
    /// The settlement tab, on the building's settlement record.
    std::optional<std::string> settlement_name;
    std::optional<std::int32_t> settlement_player;  ///< 1-based
    std::optional<std::int32_t> settlement_gold;
    std::optional<std::int32_t> settlement_food;
    std::optional<std::int32_t> settlement_population;
    std::optional<std::int32_t> settlement_max_population;
    std::optional<std::int32_t> settlement_sentries;
  };
  /// A node of the explorer's tree (see `open_explorer`).
  enum class ExplorerKind : std::uint8_t {
    kBranch,     ///< folds and unfolds, opens nothing
    kPane,       ///< opens `pane` docked in the explorer, unfilled
    kArea,       ///< an area: `key` is its index in `Editor::areas`
    kNamedUnit,  ///< a named unit: `key` is its object id
    kGroup,      ///< a type-1 group: `key` is its name, `AdvGrpProps.ini` its dialog
    kNew,        ///< *New note*, *New sequence*, ...: `key` says which; choosing it makes one
  };
  struct ExplorerNode {
    std::string label;
    ExplorerKind kind = ExplorerKind::kBranch;
    std::string pane;
    std::string key;
    std::int32_t parent = -1;
    std::vector<std::int32_t> children;
    bool expanded = false;
  };
  struct Editor {
    bool active = false;
    std::vector<EditorNode> nodes;
    std::vector<std::int32_t> rows;  ///< the node each palette row shows
    std::int32_t chosen = -1;        ///< the node whose tool is the map's
    EditorTool tool = EditorTool::kEdit;
    std::string placing;             ///< the class a click places, or empty
    core::PlayerId player = 0;       ///< `PlaceObj`'s player combo
    core::ObjectId selected = core::kNoObject;
    bool dragging = false;
    std::vector<core::ObjectId> object_ids;  ///< authored index -> object
    std::vector<core::MapObject> added;      ///< placed here
    std::set<std::int32_t> removed;          ///< authored indices deleted
    /// The direction the right button set on an object, by object, for the
    /// save: the sim keeps a facing, not the authored vector.
    std::map<core::ObjectId, core::sim::Point> directions;
    /// Brush slots, 0..4, one per family: terrain, decoration, height.
    std::int32_t brush[3] = {2, 2, 2};
    std::int32_t amount = 10;   ///< HeightPaintSettings' `Amount`, -100..100
    std::int32_t level = 50;    ///< HeightSetSettings' `Level`, 0..100
    std::int32_t density = 50;  ///< DecorSettings' `Density`, 0..100
    bool painting = false;
    core::sim::Point last_paint;  ///< where the terrain brush last painted
    core::sim::Point last_cell;   ///< the height/decor cell last applied
    core::edit::HeightStroke stroke;
    core::sim::Rng rng{0x1d2c3b4a};
    std::vector<EditorArea> areas;
    std::int32_t area_selected = -1;
    /// The handle being dragged: 0 none, 1 the centre, 2..9 a rectangle's
    /// grips (left, right, top, bottom, then the four corners), 2 the
    /// circle's rim.
    std::int32_t area_handle = 0;
    bool windows_hidden = false;  ///< `togglewnd`
    std::vector<EditorKey> keys;
    core::sim::Point pointer;  ///< the last pointer position, in world units
    bool pointer_on_map = false;
    /// The passability layer's inputs: the mask each class stamps, once
    /// resolved through the app's entities, and the decoration kinds' masks
    /// by kind. See `core/world/editor.hpp`, "passability".
    std::map<core::ClassIndex, const core::edit::PassMask*> class_masks;
    std::vector<const core::edit::PassMask*> decor_masks;
    /// Where the dragged object stood at the press, for the drop.
    core::sim::Point drag_from;
    /// The brushes' undo (`core::edit::UndoStack`): a stroke is a generation.
    core::edit::UndoStack undo;
    /// The property sheet (`AdvObjProps.ini`): the objects it is about, its
    /// parent window and the sheet docked in it -- null when none is open.
    std::vector<core::ObjectId> props_ids;
    core::ui::Dialog* props_parent = nullptr;
    core::ui::Dialog* props_sheet = nullptr;
    /// The sheet's edits by object, for the save.
    std::map<core::ObjectId, PropertyEdits> props;
    /// The explorer (`AdvExplorerDlg.ini`): its tree, the window and the
    /// dialog docked in it (null when closed), the node chosen, and the
    /// group whose properties are docked.
    std::vector<ExplorerNode> explorer_nodes;
    std::vector<std::int32_t> explorer_rows;
    std::int32_t explorer_chosen = -1;
    core::ui::Dialog* explorer = nullptr;
    core::ui::Dialog* explorer_pane = nullptr;
    std::string explorer_group;
    /// The overlay-text labels: the row chosen on the Labels tab, the
    /// *Place Label* screen while it is open, and the picture it shows.
    std::int32_t label_selected = -1;
    core::ui::Dialog* place_label = nullptr;
    core::ui::Image place_picture;
    core::ui::Rect place_fitted;  ///< where the map stands in the `BMP`
    /// The tools palette (`MapToolsDlg.ini`) and the tool's pane docked
    /// under it, or null. The palette used to be found as dialog 0, which
    /// held until `nextwnd` gave the stack a reason to reorder.
    core::ui::Dialog* palette = nullptr;
    core::ui::Dialog* tool_pane = nullptr;
    /// Group edits for the save: a type-1 group renamed (old name to new)
    /// or deleted, by name.
    std::map<std::string, std::string> group_renames;
    std::set<std::string> groups_deleted;
    /// The container's documents as this run holds them, by path: read
    /// from the source on first use and patched in place by the explorer's
    /// dialogs; every one goes into the container written.
    std::map<std::string, std::string> documents;
    /// Documents this run made that the source has no file for -- a new
    /// sequence's `.vs`, a new `.conv.xml` -- written even when empty,
    /// where a document merely read and found missing is not.
    std::set<std::string> created;
    /// The Maps tab: the row chosen, and the `Maps/<n>` directories deleted
    /// this run, left out of the container written.
    std::int32_t map_selected = -1;
    std::set<std::string> removed_maps;
    /// The open file dialog's rows (Import ... maps): the containers listed.
    std::vector<std::filesystem::path> import_files;
    /// Windows rolled up by their Collapse button (id 0x5000), by dialog,
    /// with the height each had; a docked pane stays out of sight while
    /// its window is rolled up.
    std::map<core::ui::Dialog*, std::int32_t> collapsed;
    /// The container the editor was opened on, by stem: the save dialog's
    /// default name, kept while the editor works on a copy of its own
    /// (`editor_switch_map`).
    std::string origin_stem;
    /// The diplomacy sheet's current player, whose relations its rows show.
    core::PlayerId diplomacy_player = 0;
    /// A window being dragged by its caption (`Move`, `%ID_MOVE%`): which
    /// dialog, and where the pointer took hold of it.
    std::size_t moving = ~std::size_t{0};
    std::int32_t move_dx = 0;
    std::int32_t move_dy = 0;
  };
  Editor editor_;
  void start_editor();
  void apply_select_argument();
  /// `--select`'s grammar resolved against the world now: see the definition.
  [[nodiscard]] std::vector<core::ObjectId> resolve_objects(const std::string& spec) const;
  void build_editor_tree();
  void refresh_editor_palette();
  void editor_choose(std::int32_t node);
  void open_editor_pane(const EditorNode& node);
  void close_editor_pane();
  void editor_load_keys();
  bool editor_key(const SDL_KeyboardEvent& key);
  /// `nextwnd` (Ctrl+Tab): the editor's windows, top-level ones only, in
  /// the order the cycle visits them. A docked pane is not one of them --
  /// it follows the window it is docked in -- and neither is a window that
  /// is not open.
  [[nodiscard]] std::vector<core::ui::Dialog*> editor_windows();
  /// Make `window` the active one: it and its docked pane go to the top of
  /// the stack, so it draws over the others and takes the next click.
  void editor_activate_window(core::ui::Dialog* window);
  [[nodiscard]] std::int32_t editor_brush_slot(const core::ui::Dialog& pane, std::int32_t frame) const;
  void editor_place(std::int32_t mx, std::int32_t my);
  void editor_select(std::int32_t mx, std::int32_t my);
  void editor_delete();
  void editor_turn(core::sim::Point towards, bool opposite);
  void editor_paint(core::sim::Point world, bool first);
  /// The passability layer, kept the way the original keeps it: rebuilt
  /// over a rectangle after every change (`core::edit::rebuild_passability`).
  [[nodiscard]] const core::edit::PassMask* editor_mask_of(core::ClassIndex index);
  [[nodiscard]] const core::edit::PassMask* editor_mask_of_kind(std::uint32_t cell) const;
  [[nodiscard]] std::vector<core::edit::Footprint> editor_footprints();
  void editor_rebuild_passability(const core::edit::WorldRect& rect);
  void editor_stamp(const core::edit::PassMask* mask, core::sim::Point at);
  void editor_unstamp(const core::edit::PassMask* mask, core::sim::Point at);
  void editor_drop();
  [[nodiscard]] core::edit::UndoStack::Layers editor_layers();
  void editor_snapshot(const core::edit::WorldRect& rect, std::uint32_t flags, bool first);
  void editor_undo(bool redo);
  void editor_pick(core::sim::Point world);
  void editor_place_area(core::sim::Point world, bool circle);
  void editor_area_press(core::sim::Point world);
  void editor_area_drag(core::sim::Point world);
  void open_area_dialog();
  /// The property sheet: `AdvObjProps.ini`'s parent window with the
  /// section for the objects' kind docked in it, opened on the selected
  /// object by the right button or `objprops`, filled from the world, and
  /// applied to the world and to `Editor::props` as it is worked.
  void open_object_properties(std::vector<core::ObjectId> ids, std::int32_t tab = 1);
  void close_object_properties();
  void fill_object_properties();
  void object_properties_event(const core::ui::DialogEvent& event, core::ui::Dialog& sheet);
  [[nodiscard]] core::ui::Dialog* object_properties_sheet() noexcept;
  [[nodiscard]] std::string object_display_line(core::ObjectId id) const;
  [[nodiscard]] std::vector<std::string> group_names() const;
  /// The authored record behind an object -- the map's, or this run's
  /// placement -- or null for an object the editor does not know.
  [[nodiscard]] const core::MapObject* editor_record(core::ObjectId id) const;
  /// The five skill rows a hero's sheet shows: the record's `hs*`
  /// attributes in order, then the class's offered skills, then blanks.
  [[nodiscard]] std::vector<std::pair<core::sim::HeroSkill, std::int32_t>> hero_skill_rows(core::ObjectId id) const;
  /// The explorer: the Adventure Palette's tree and the dialogs it docks.
  void open_explorer();
  void close_explorer();
  void build_explorer_tree();
  void refresh_explorer();
  void explorer_choose(std::int32_t index);
  void close_explorer_pane();
  [[nodiscard]] core::ui::Dialog* explorer_dialog() noexcept;
  [[nodiscard]] core::ui::Dialog* palette_dialog() noexcept;
  [[nodiscard]] core::ui::Dialog* explorer_pane() noexcept;
  void open_group_properties(const std::string& name);
  void fill_group_properties();
  void dock_in_explorer(core::ui::Dialog* pane);
  void redock_windows();
  /// The Collapse button (id 0x5000) of an editor window: rolled up to the
  /// caption and the first strip, or unrolled to the height it had.
  void editor_toggle_collapse(core::ui::Dialog* dialog);
  /// The explorer's document dialogs, over the container's own text.
  [[nodiscard]] std::string& editor_document_text(const std::string& path);
  [[nodiscard]] std::string editor_map_prefix() const;
  [[nodiscard]] core::sim::Point editor_view_centre() const;
  [[nodiscard]] std::string note_document_of(const std::string& id) const;
  [[nodiscard]] std::string sequence_script_path(const std::string& name);
  /// The overlay-text labels (`Maps/<n>/labels.xml`): the Labels tab of
  /// `AdvCurMap.ini` and `AdvScenario.ini`, and the *Place Label* screen.
  [[nodiscard]] std::string labels_document() const;
  [[nodiscard]] std::string& labels_text();
  /// The Maps tab of `AdvAdventure.ini` and `AdvCurMap.ini`: the container's
  /// maps, New from the blank template, Edit (a switch), Delete, Import.
  [[nodiscard]] std::vector<std::string> editor_map_list();
  [[nodiscard]] std::string editor_map_name(const std::string& directory);
  void fill_maps(core::ui::Dialog& pane);
  void maps_event(const core::ui::DialogEvent& event, core::ui::Dialog& pane);
  [[nodiscard]] int editor_free_map_number() const;
  int editor_copy_map(const gamedata::MapContainer& from, const std::string& directory);
  void editor_new_map();
  void editor_switch_map(int number);
  void open_import_maps_dialog(bool adventure);
  void fill_labels(core::ui::Dialog& pane);
  void labels_event(const core::ui::DialogEvent& event, core::ui::Dialog& pane);
  void open_place_label();
  void refresh_place_label(core::ui::Dialog& menu);
  void close_place_label();
  void fill_explorer_pane(const ExplorerNode& node, core::ui::Dialog& pane);
  /// The *New ...* leaves: make the thing, rebuild the tree, open its node.
  void explorer_create(const std::string& what);
  void explorer_reselect(std::string_view pane, const std::string& key);
  [[nodiscard]] std::string unique_name(std::string base, const std::function<bool(const std::string&)>& taken) const;
  void fill_diplomacy(core::ui::Dialog& pane);
  void fill_item_type(const std::string& id, core::ui::Dialog& pane);
  [[nodiscard]] std::string conversation_document_of(const std::string& name);
  void fill_conversation(const std::string& name, core::ui::Dialog& pane);
  void conversation_event(const std::string& name, const core::ui::DialogEvent& event, core::ui::Dialog& pane);
  [[nodiscard]] static std::string unescape_item_script(std::string_view stored);
  [[nodiscard]] static std::string escape_item_script(std::string_view source);
  [[nodiscard]] std::int32_t resources_filter(const core::ui::Dialog& pane) const;
  void fill_resources(core::ui::Dialog& pane, std::int32_t filter);
  void set_settlement_number(core::sim::SettlementId id, std::string_view field, std::int32_t value);
  void explorer_pane_event(ExplorerNode& node, const core::ui::DialogEvent& event, core::ui::Dialog& pane);
  void editor_repaint(const core::edit::CellRect& terrain_cells);
  void refresh_editor_overlay(std::uint32_t width, std::uint32_t height);
  void open_editor_save_dialog();
  /// Where the editor's Save writes: the installation's `Scenarios/` for a
  /// person's run, as the original's editor does, else `Scenarios/` under
  /// the user data, so a test or a tool never writes a map into the
  /// installation.
  [[nodiscard]] std::filesystem::path editor_maps_directory() const {
    return user_dir_is_installation_ ? vfs_.root() / "Scenarios" : user_dir_ / "Scenarios";
  }
  bool editor_save(const std::filesystem::path& out);
  [[nodiscard]] core::MapObjectList editor_document() const;
  [[nodiscard]] static bool is_editor_pane(const core::ui::Dialog& dialog) noexcept;
  /// Names this run of the editor wrote, folded: those may be overwritten.
  std::set<std::string> written_by_editor_;
  [[nodiscard]] static std::string fold_name(std::string name) {
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return name;
  }
  /// The limit combo's rows for the chosen game type: `CONST.INI`'s
  /// `<basename with _><i> = number, label` keys, read until one is missing
  /// (0x006c31a0). Empty for Elimination, which has none.
  struct LimitRow {
    std::string number;
    std::string label;
  };
  [[nodiscard]] std::vector<LimitRow> limit_rows(std::string_view victory) const;
  /// `[GamePlay] LowPop/NormalPop/HighPop`, in that order (0x0082f598).
  [[nodiscard]] std::array<int, 3> population_levels() const;
  void refresh_setup_rules(core::ui::Dialog& settings);
  /// The installation's packs, opened once: at play, or for the setup's
  /// game-script and constant lists before it.
  bool ensure_installation();
  void load_settings();
  void load_profile_choice();
  bool save_settings() const;
  void apply_settings();
  [[nodiscard]] std::filesystem::path settings_file() const;
  void open_notes_menu();
  /// `ENDGAMEMENU.INI` when the human's match is decided: the picture
  /// (`WINLOSEF.BMP`'s first row for a win, its third for a loss -- a
  /// *reading*; the strip is three rows and the file draws two), the word,
  /// and the game menu's buttons plus Continue, which plays on.
  void open_endgame_menu(bool won);
  /// `STATISTICS.INI`, the end-of-match report: four tabs of three columns
  /// over the match's per-player counters, a row per *real* player (see
  /// `statistics_rows`).
  void open_statistics_menu();
  void fill_statistics(core::ui::Dialog& menu, int tab);
  /// The players the Statistics screen lists, in slot order, at most eight.
  [[nodiscard]] std::vector<core::PlayerId> statistics_rows() const;
  int statistics_tab_ = 0;
  /// `DIPLOMACY.INI`: one row per other *real* player -- its colour and
  /// name, four toggles for the local player's policy toward it (cease fire,
  /// share view, share support, share control: the relation bits) and the
  /// same four, inert, for its policy toward the local player, and the
  /// Allied victory box. OK posts what changed as `diplomacy` orders; Cancel
  /// drops it. See the definition.
  void open_diplomacy_menu();
  /// The other side's four toggles, from the table as it now stands: after
  /// every turn while the screen is open (0x006cb8c0 after each execution).
  void refresh_diplomacy(core::ui::Dialog& dialog);
  /// OK's orders: a word per row that changed, and the flag if it did.
  void post_diplomacy(const core::ui::Dialog& menu, const std::vector<core::PlayerId>& others);
  /// `diplomacy:` on stdout for an applied order, from the table.
  void print_diplomacy(const core::sim::NetOrder& order, std::uint64_t turn) const;
  /// The players the open Diplomacy screen's rows show, in row order.
  std::vector<core::PlayerId> diplomacy_rows_;
  /// A toggle column: its widget suffix and the relation it shows.
  struct DiplomacyColumn {
    const char* suffix;
    core::sim::Relation relation;
  };
  static constexpr std::array<DiplomacyColumn, 4> kDiplomacyColumns{{
      {"CF", core::sim::Relation::ceasefire},
      {"SV", core::sim::Relation::share_view},
      {"SS", core::sim::Relation::share_support},
      {"SC", core::sim::Relation::share_control},
  }};
  /// `Select party (F7)`: the selection becomes the local player's party --
  /// every live object of theirs with the `in_party` flag, which is what
  /// `PartyQuery()` collects. Nothing happens when there is none.
  void select_party();
  /// A click on portrait `index` of the info bar's `UIHolder` -- a
  /// garrison's, a hero's army's, a multiple selection's (`CVXUIHolder`,
  /// handler 0x006d3550). See the definition.
  void holder_click(std::size_t index, bool shift, bool ctrl);
  /// A click on the training queue's cell `index`, released over the cell it
  /// went down on: a cancel of that entry, or with Ctrl of it and every one
  /// behind it (0x006bfc20). Posted as orders, not done here.
  void queue_click(std::size_t index, bool ctrl);
  /// What the queue strip shows, by command id, cell for cell; and the ids a
  /// click has posted a cancel for, which the strip leaves out until the
  /// command is gone -- as the original takes the cell out at once
  /// (0x006ee640) rather than wait for the order to run.
  std::vector<std::uint32_t> queue_cells_;
  std::vector<std::uint32_t> queue_cancelled_;
  /// The queue cell the left button went down on, or -1.
  std::int32_t pressed_queue_cell_ = -1;
  /// Select what the marked portraits hold, in place (0x006d3250).
  void apply_holder_marks();
  /// One `UIHolder` portrait the bar shows now, from the last
  /// `refresh_infobar`: its key and what a click on it selects.
  struct HolderCell {
    std::uint64_t key = 0;
    std::vector<core::ObjectId> objects;
  };
  std::vector<HolderCell> holder_cells_;
  /// The portraits marked and not yet applied, by key, each with how many
  /// of its units to take (a plain click all of them, 0x10000; each Ctrl
  /// click one more): Shift-clicks wait for Shift to be released.
  std::vector<std::pair<std::uint64_t, std::size_t>> holder_marks_;
  /// `Minimap (Space)`: `ZOOMMAP.INI` with the whole map reduced to 1024
  /// pixels across -- the ground from the map renderer, every building and
  /// map object as its `Minimap.pak` `ZOOM<n>` picture (named after its
  /// entity path, `BUILDINGS-BBARRACKS-BBARRACKS.BMP`), every unit a dot of
  /// its owner's colour, and the camera's frame. The screen declares no
  /// widget: the map is the dialog's backdrop, sized to it.
  ///
  /// Read off the exe's zoom map (constructed at 0x0060f850, one per
  /// session): the zoom is the map's width over 1024 -- the outlines
  /// loader at 0x0061d03b switches on the map's right edge, 0x1FFF to 8,
  /// 0x3FFF to 16, 0x7FFF to 32, and the terrain composer at 0x00618459
  /// asks for `zoom%d` with `2 << level` -- so a 16,384-unit map fits
  /// 1024 x 736 at a sixteenth and the tutorial's 8,192 at an eighth.
  /// Space is a tap-or-hold key (0x0060e3b0): the press opens the map, or
  /// closes it if it was open, and the release closes it only when the key
  /// was held longer than `Const.ini`'s `[zoommap] ToggleTreshold`
  /// milliseconds -- a tap leaves it open, a hold is a peek. It is a live
  /// child window, not a modal: a 500 ms tick (0x00610260) recomposes the
  /// picture while it is shown and the game's input keeps running, so the
  /// turns run under it here. **Reading, labelled:** whether the original
  /// pauses the simulation under it was not established. A right click
  /// centres the camera on the point and closes the map (0x006103c0 --
  /// "go there and close"; its exception for a point an ally stands on is
  /// not reproduced). The left button, pressed or dragged, hands the point
  /// to the view's click dispatcher in cursor mode 0x11, **inferred** to be
  /// a camera drag and not established: here it moves the camera and keeps
  /// the map open, its frame following.
  void open_zoom_map();
  /// `Help (F1)`: `HELP.INI` over the language pack's `HELP.XML`. The topic
  /// shown is the list -- an entry a row, its image beside it, headings in
  /// the bold face and centred -- a row with a link opens its topic, and
  /// the six buttons walk the hypertext: Backward and Forward through the
  /// history, Up to the parent, Previous and Next among the siblings, Home
  /// to the contents.
  void open_help();
  void show_help_topic(core::ui::Dialog& menu, std::int32_t topic, bool record);
  std::unique_ptr<core::game::HelpDocument> help_;
  /// The main menu's Tips frame over `CurrentLang\TIPS.XML`, and the
  /// Credits roll over `CurrentLang/credits.txt`.
  std::vector<core::game::Tip> tips_;
  bool tips_loaded_ = false;
  std::int32_t tips_last_ = -1;  ///< `[Tips] LastTip` in settings.ini
  bool tips_all_shown_ = false;  ///< `[Tips] AllShown`
  void show_next_tip(core::ui::Dialog& menu);
  void show_tip(core::ui::Dialog& menu) const;
  void open_credits_menu();
  /// `PROFILE.INI`, the main menu's *Change player*: the profiles in one
  /// pane and the selected one's career in the other. See
  /// `docs/formats/profile.md` and `core/game/profile.hpp`.
  void open_profile_menu();
  /// Every profile the app can see, in directory order: the installation's
  /// `Profiles/` first and this engine's own beneath the saves after,
  /// the later shadowing the earlier by folded name. Re-scanned on every
  /// call, because *New*, *Rename* and *Delete* change it.
  struct ProfileRow {
    std::string directory;         ///< the subdirectory's name, which is the key
    std::filesystem::path path;    ///< its `player.ini`
    bool writable = false;         ///< false under the installation, which is read-only
    core::game::Profile profile;
  };
  [[nodiscard]] std::vector<ProfileRow> scan_profiles() const;
  /// Give a profile another name: the directory it lives in and the
  /// `[Player] name` inside it, which is what the list shows. The file is
  /// rewritten line by line rather than reparsed and re-emitted, because
  /// everything else in it -- the journal, the roster, `[favmap]` -- has to
  /// survive untouched. False when it could not be written.
  bool rename_profile(const ProfileRow& row, const std::string& name) const;
  /// Where a profile this engine makes lives: `Saves/Profiles/`.
  /// `docs/legal.md` rule 1 keeps the installation read-only, and a journal
  /// or a rename written into it would break that, so the engine's own
  /// profiles sit beside `Saves/settings.ini` exactly as the settings do.
  [[nodiscard]] std::filesystem::path profiles_directory() const;
  /// The twelve lines of the info pane, in the language, built the way
  /// 0x006ec090 builds them.
  [[nodiscard]] std::vector<std::string> profile_info_lines(const core::game::Profile& profile) const;
  void show_profile(core::ui::Dialog& menu, const std::vector<ProfileRow>& rows,
                    std::int32_t index) const;
  /// `DATA\CONST.INI`'s `[Ranks]`, read once.
  [[nodiscard]] std::span<const core::game::Rank> ranks() const;
  mutable std::vector<core::game::Rank> ranks_;
  mutable bool ranks_loaded_ = false;
  /// The profile directory in use. `Profiles/profiles.ini`'s `default=` at
  /// start, then whatever *Select* last chose, kept in `Saves/settings.ini`.
  std::string profile_;
  core::ui::Dialog* credits_ = nullptr;
  std::uint64_t credits_started_ticks_ = 0;
  std::int32_t credits_pixels_per_second_ = 100;
  std::int32_t credits_height_ = 0;
  void tick_credits();
  std::vector<std::int32_t> help_back_;
  std::vector<std::int32_t> help_forward_;
  std::int32_t help_topic_ = -1;
  void refresh_zoom_map(core::ui::Dialog& menu);
  /// The open zoom map's dialog, or null.
  [[nodiscard]] core::ui::Dialog* zoom_dialog() noexcept;
  void close_zoom_map();
  /// Space, down or up, in play: the tap-or-hold rule above.
  void zoom_key(const SDL_Event& event);
  /// The pointer on the open zoom map; true when the event was the map's.
  bool zoom_mouse(const SDL_Event& event, core::ui::Dialog& dialog);
  /// A point of the map's picture, back to the world: x is 1:1 at the
  /// divisor, y through the 46/64 projection.
  void zoom_look(std::int32_t local_x, std::int32_t local_y);
  core::ui::Image zoom_ground_;
  core::ui::Image zoom_view_;
  bool zoom_open_ = false;
  /// 8, 16 or 32: the map's width over 1024; what `zoom_ground_` was
  /// composed at.
  std::uint32_t zoom_divisor_ = 16;
  /// `[zoommap] ToggleTreshold`, milliseconds.
  std::int32_t zoom_toggle_threshold_ = 1200;
  /// `[zoommap] MinimapEmptyColor`, RGB555.
  std::uint16_t zoom_empty_colour_ = 0x3dc9;
  /// When Space went down for the open map; 0 once released.
  std::uint64_t zoom_press_ticks_ = 0;
  /// The last recompose, for the 500 ms tick.
  std::uint64_t zoom_refresh_ticks_ = 0;
  bool zoom_dragging_ = false;
  bool restart_play();

  // -- the front
  //
  // With no `--map`, the app opens on `MENUBACK.INI` and `MAINMENU.INI`,
  // and the buttons lead to the adventure and conquest lists, the scenario
  // list, the saved games and the options, each the shipped screen. A
  // choice starts the map the way `--map --play` does; the game menu's
  // Quit comes back here.
  bool front_mode_ = false;
  /// `local/<language>.pak`, mounted at the root for `CurrentLang/`, and its
  /// `TRANSLATION.LOC.XML`, which every display string goes through --
  /// the menus here, the bars and the notes through the session.
  void mount_language();
  std::unique_ptr<core::game::TranslationTable> translations_;
  std::vector<std::byte> translation_bytes_;
  std::string language_;
  /// The current container's own tables (`Local/<language>/`), kept for the
  /// session's lifetime because the session views them.
  std::vector<std::vector<std::byte>> localisation_;
  std::vector<std::span<const std::byte>> localisation_spans_;
  /// A container's `Local/<language>/` tables as one table, for the front's
  /// lists and the campaign map, which show its strings before a session
  /// exists.
  [[nodiscard]] core::game::TranslationTable container_table(const gamedata::MapContainer& container) const;
  [[nodiscard]] std::string localised(const core::game::TranslationTable& table, std::string_view text) const;
  /// A combobox item by the table's own contextual key -- `Republican
  /// Rome@race`, `easy@ai`, `Low@population`, `Default@gold` -- through the
  /// language pack; the text itself when the pack has no row.
  [[nodiscard]] std::string item_label(std::string_view text, std::string_view context) const;
  /// The eight races' display names as the table keys them (`Gaul@race`,
  /// `Republican Rome@race`), from the race enumeration.
  [[nodiscard]] static std::string_view race_display_name(std::int32_t race) noexcept;
  [[nodiscard]] std::function<std::string_view(std::string_view)> translator() const;
  bool start_front();
  void tick_front(platform::Window::Frame& frame);
  void open_main_menu();
  /// `ADVENTUREMENU.INI` over `Adventures/GreatBattles/`, or
  /// `CONQUESTMENU.INI` over `Conquests/`: one row per container, its
  /// `game.xml` name and description, its picture beside it.
  /// One container of a front list: its file, its `game.xml` name and
  /// description in the language, and its picture beside it.
  struct ContainerRow {
    std::filesystem::path path;
    std::string name;
    std::string description;
    std::string picture;  // a virtual path, or empty
  };
  [[nodiscard]] std::vector<ContainerRow> scan_containers(const std::filesystem::path& directory);
  /// `PREADVENTUREMENU.INI` in its Great Battles shape: Rome's victories and
  /// Rome's enemies as two lists, the chosen one's picture and words.
  void open_great_battles_menu();
  void open_container_menu(std::string_view screen, const std::filesystem::path& directory,
                           std::string_view list_widget);
  /// `SELECTMAP.INI` over `Scenarios/`.
  void open_scenario_menu(bool to_edit = false);
  /// One row of the setup screen: a slot the scenario declares.
  struct SetupPlayer {
    core::PlayerId slot = 0;
    std::string name;
    /// 0 human, 1 computer, 2 closed -- and the row of `PlayerButton`'s
    /// bitmap it shows (0, 3, 4). **Reading, labelled:** the five rows of
    /// `MMENBUT.BMP` are a monitor, a face, a blank, a hand and a cross;
    /// this takes the monitor for this computer's player, the hand for the
    /// AI and the cross for a closed slot, and leaves the face and the
    /// blank -- a remote human, an open seat -- to multiplayer.
    int type = 1;
    std::int32_t race = -1;  ///< `kNoRace` is Random
    int difficulty = 1;      ///< 0 easy, 1 normal, 2 hard
    int team = 0;            ///< 0 none, 1..4 -- `TEAMS.BMP`'s rows
    /// 0 none, 1 wealth, 2 riches, 3 hero -- `BONUSES.BMP`'s rows and the
    /// leading numbers of `DATA/BonusScripts/`, in the order the button's
    /// `HelpText` lists them.
    int bonus = 0;
    core::Rgb888 colour{};
    /// A networked lobby's ready mark: `PlayerReady_Pn`.
    bool ready = false;
  };
  std::vector<SetupPlayer> setup_players_;
  /// Which players' screen `MPGAMEMENU.INI` is showing: the skirmish setup,
  /// a host's networked lobby, or a joiner's view of one. Types 3 (an open
  /// seat, the blank) and 4 (another peer, the face) occur only in the last
  /// two, which is `MMENBUT.BMP`'s two rows the skirmish leaves unused.
  enum class SetupMode : std::uint8_t { local, host, join };
  SetupMode setup_mode_ = SetupMode::local;
  /// Set by the setup screen's Start and read by `start_play`: who is
  /// human, what everyone plays, who is allied.
  bool setup_pending_ = false;
  /// `MPGAMEMENU.INI` with `SETTINGS.INI` beside it, over a scenario.
  void open_setup_menu(const std::filesystem::path& scenario, SetupMode mode = SetupMode::local);
  /// The scenario list is choosing a map to host rather than to play.
  bool scenario_for_host_ = false;
  /// `Sounds/UI/PlayerDropped.wav`, said on the console either way. Type
  /// `Ambient2` at priority 300 (0x00406a3b), so the nature-sounds switch and
  /// the sound slider govern it.
  void play_sound(std::string_view path);
  /// Play what a class's `<sounds>` value, an entity's name or a file names,
  /// as the original's sound manager does (0x006b0910): `type` none takes
  /// the entity's own; a switched-off type, a full pool or the silent filler
  /// plays nothing; `at`, a world point, attenuates and pans it by the view.
  /// Whether it played.
  bool play_sound_value(std::string_view value, std::uint16_t priority, imperivm::sound::SoundType type,
                        const core::sim::Point* at, std::int32_t percent = 100);
  /// An order was given to the local selection: one of it acknowledges it
  /// (0x005e61db, 0x005e71ae, `Talk` 0x005e36e0).
  void acknowledge_order();
  /// A control was activated: `Sounds/UI/click.wav` (0x006b0d10).
  void play_click(std::int32_t control_id);
  /// The options' switch and slider for a type (0x006e7210).
  [[nodiscard]] bool sound_type_on(imperivm::sound::SoundType type) const;
  [[nodiscard]] std::int32_t sound_type_volume(imperivm::sound::SoundType type) const;
  /// `config.ini`, and the mixer's pools from it.
  void start_sound();
  /// The view as the sound manager measures from it, in world units.
  [[nodiscard]] imperivm::sound::View sound_view() const;
  /// The selection changed: each object it gained, in order, plays its
  /// class's `select` sound if the local player controls it (0x005e7d80,
  /// 0x005e2f80). `before` is the selection as it was.
  void play_select_sounds(const std::vector<core::ObjectId>& before);
  /// The live sounds follow the view (0x006b0810): re-applied when it has
  /// moved since they were last placed, or always when `force`.
  void follow_view(bool force = false);
  /// The music, once a frame: the menus' (0x00748808) and the match's
  /// (0x00550f90).
  void tick_music();
  /// A file of the installation on the `Music` type, at 500, unplaced and at
  /// 100 percent (0x00550df0, 0x00748855). Whether it played.
  bool play_music(std::string_view path);
  /// Every sound stops (0x006aff00): a match starting or ending.
  void stop_sounds();
  /// A seat's name as a screen shows it: the setup's, or `Player N`, and in
  /// a networked match marked while the computer holds it for a player who
  /// left (`sim/netdepart.hpp`).
  [[nodiscard]] std::string seat_name(core::PlayerId seat, const core::sim::PlayerSetup& setup) const;
#if IMPERIVM_HAVE_NET
  /// The networked front: `MPMENU.INI`'s list, a host's lobby, a joiner's.
  /// Stepped every frame by `step_net_lobby` while the front is up.
  struct NetFront {
    enum class Role : std::uint8_t { none, browse, host, join } role = Role::none;
    std::unique_ptr<imperivm::net::UdpSocket> socket;
    std::unique_ptr<imperivm::net::HostLobby> host;
    std::unique_ptr<imperivm::net::JoinLobby> join;
    std::unique_ptr<imperivm::net::LanBrowser> browser;
    std::uint32_t last_refresh = 0;
    std::string address;       ///< the host's, shown and copied
    std::string listed;        ///< what the game list shows, to redraw on change
    bool seated = false;       ///< a joiner past its first roster
    bool startable = false;
  };
  NetFront net_front_;
  void open_mp_menu();
  void open_multi_box(const std::string& message, std::function<void()> ok);
  void step_net_lobby();
  void end_net_front();
  /// Hand a settled lobby to a match, as the command line's does.
  void play_net_lobby(std::unique_ptr<imperivm::net::UdpSocket> socket, imperivm::net::Lobby lobby);
  /// A late joiner: wait for the host's save and load it when the session
  /// starts. True, doing nothing, for any other peer.
  bool receive_late_state(const std::filesystem::path& root, std::string* error);
  /// The host's rows from the screen's, and the joiners' from the lobby's.
  void push_host_rows();
  void pull_lobby_rows(const std::vector<core::sim::SeatRow>& rows, core::PlayerId you);
  /// `MPCHAT.INI`'s log from the lobby's.
  void show_lobby_chat(const std::vector<core::sim::ChatLine>& log, core::ui::Dialog* dialog);
  bool net_departed_shown_ = false;
  /// `INGAMECHAT.INI`, opened by Enter in a networked match.
  void open_ingame_chat();
  /// A line of chat on screen, for a while; see `UiRenderer::set_messages`.
  void show_chat(std::string text);
  void refresh_chat();
  /// What the lobby's chat log last showed, to redraw on change.
  std::size_t lobby_chat_shown_ = 0;
  struct ShownChat {
    std::string text;
    std::uint32_t until = 0;
  };
  std::vector<ShownChat> chat_shown_;
#endif
  void refresh_setup_menu(core::ui::Dialog& menu);
  /// `LOADGAMEFROMMAINMENU.INI`.
  void open_front_load_menu();

  /// `CONQUESTGAME.INI`: the campaign map of a conquest. The `ConquestMap`
  /// widget shows `ConquestMaps/<data>/global.bmp` -- 1024 x 768 -- scaled
  /// into its 600 x 400, the territories under `territories.bmp`'s indices
  /// tinted by their state -- owned, or an enemy the player can reach -- a
  /// click on one selects it and shows its description and bonus, and
  /// Start plays its map. A fresh conquest with `choose="1"` asks for a starting territory
  /// first: every territory is offered, Select makes the chosen one owned
  /// and the others enemy, and the campaign file is written. The shields
  /// above the map are the road of conquest: the first territory, an arrow,
  /// then every one conquered after it, each its `Shield<id>.bmp`.
  /// **Readings, labelled:** that the art is scaled into the widget rather
  /// than scrolled under it (the shields' frame sits over the widget's top
  /// 90 pixels, which is open sea on the scaled map and land on a
  /// scrolled one), what the shields show, and that unreachable territories
  /// are left untinted (the file turns `disabled_colorize` off and the
  /// legend's third entry is commented out of the screen).
  struct CampaignScreen {
    std::filesystem::path container;
    std::string relative;
    core::sim::ConquestMap map;
    core::sim::CampaignProgress progress;
    bool choosing = false;
    std::int32_t selected = -1;
    core::ui::Image global;
    core::ui::IndexImage mask;
    core::ui::Image view;
    std::filesystem::path file;
    std::string data_prefix;  ///< `ConquestMaps/<dir>/`, a virtual path
    core::game::TranslationTable table;  ///< the container's `Local/<language>/`
  };
  std::unique_ptr<CampaignScreen> campaign_;
  bool from_campaign_ = false;
  /// The nation the campaign's start territory gave the player, applied to
  /// the conquest's `Mutable` human slot: the territory's `interface` is its
  /// race index (`Territory::interface_id`), and the `bonus` sequence's
  /// `r<Race>` name says the same thing for every shipped territory and is
  /// the fallback when a file has no `interface`. **Reading, labelled:** what
  /// else "choose a starting area" could decide for a `Mutable` slot is not
  /// in the data.
  std::int32_t campaign_race_ = -1;
  void open_campaign_screen(const std::filesystem::path& container);
  void refresh_campaign_screen(core::ui::Dialog& menu);
  bool save_campaign_screen();
  /// Leave the front for `map` (installation-relative), on the map it names
  /// or its `start_map`, as the container's `start_player`.
  bool play_from_front(const std::string& map, int map_index);
  /// Leave the front by resuming a save.
  bool load_from_front(const std::filesystem::path& file);
  /// Back to the front from a game: the session, the map and the bars go.
  void return_to_front();
  /// The map renderer, the world view and the host table are built once;
  /// a second map reuses them.
  bool renderers_ready_ = false;
  bool hosts_registered_ = false;
  /// The containers the front's lists show, in row order.
  std::vector<std::filesystem::path> listed_containers_;
  /// The campaign file this mission reads at start and writes on a win, or
  /// empty for a map that is not a conquest's.
  std::filesystem::path campaign_file_;
  bool campaign_written_ = false;
  void write_campaign_carry();
  /// Real milliseconds owed to the simulation, so a slow frame catches up
  /// rather than dropping game time.
  double owed_ms_ = 0.0;
  std::uint64_t last_ticks_ = 0;
  std::uint64_t turns_run_ = 0;
  /// The local command path of an unnetworked match: what the options
  /// screen posts waits here for the next turn (`sim::LocalOrders`).
  core::sim::LocalOrders local_orders_;
#if IMPERIVM_HAVE_NET
  /// A networked match's local command path: a script's `SetSpeed` becomes
  /// an order for an agreed turn, as the options screen's does.
  class NetOutbox final : public core::sim::OrderOutbox {
   public:
    explicit NetOutbox(imperivm::app::NetPlay& net) noexcept : net_(&net) {}
    /// The start says whether the match fixed its speed; the negotiator
    /// refuses a `set_speed` in a fixed match on every peer anyway.
    [[nodiscard]] bool speed_fixed() const noexcept override {
      return !net_->start().config.variable_speed;
    }
    void post(core::sim::NetOrder order) override { net_->queue(std::move(order)); }

   private:
    imperivm::app::NetPlay* net_;
  };
  std::unique_ptr<NetOutbox> net_outbox_;
#endif
  /// Point the session's host context at this match's command path, where
  /// a script's `SetSpeed` posts. No script the app runs calls it today --
  /// its only callers are `SCDEBUG.XML`'s key bindings, which the app does
  /// not run -- so dropping this was fault-injected and survived; the core
  /// tests hold what posting does.
  void attach_outbox();
  /// The game-time length of the next unnetworked turn: `--turn-length`,
  /// or `turn_interval` converted at the clock's speed.
  [[nodiscard]] std::int32_t local_turn_length() const;
  /// One unnetworked turn: its length converted, then the posted orders
  /// applied, then the turn run -- the order `TurnNegotiator` keeps, so a
  /// speed applied with turn t converts turn t + 1 onwards, as it does in a
  /// networked match.
  void run_local_turn();
  /// The options screen's speed position, posted as a `set_speed` order
  /// through whichever command path this match has.
  void post_speed_option(int option);
  /// Where a match starts its speed: `NormalSpeed`, and the options'
  /// position written back from it.
  void start_speed();

  // -- sheet mode
  core::RleImage body_image_;
  core::RleImage shadow_image_;
  platform::Sprite body_;
  platform::Sprite shadow_;
  std::vector<platform::PaletteRow> team_palettes_;

  std::uint32_t sheet_width_ = 0;
  std::uint32_t sheet_height_ = 0;
  std::int32_t canvas_left_ = 0;
  std::int32_t canvas_top_ = 0;
  std::uint32_t cell_width_ = 0;
  std::uint32_t cell_height_ = 0;

  std::uint64_t frame_number_ = 0;
  bool screenshot_taken_ = false;
  /// A `shot:PATH` step's file, written once the frame after it has rendered.
  std::string pending_shot_;
  /// `--input`, as far as it has been played.
  std::size_t input_at_ = 0;
  std::uint64_t input_wait_until_ = 0;
  /// `turn:N`: hold the script until the world has run N turns, or the
  /// match is decided first. `over`: hold it until the match is decided.
  /// What a frame count cannot say, since how many turns a frame runs
  /// depends on how fast the machine is.
  std::uint64_t input_until_turn_ = 0;
  bool input_until_over_ = false;
  void play_input();

  /// Frames drawn since the last status line, so the status line can say what
  /// the frame rate actually is. Guessing at it from the outside is how a
  /// simulation that had stopped catching up looked like a slow renderer.
  std::uint64_t frames_since_status_ = 0;
};

bool Application::start(const Arguments& args) {
  args_ = args;

  const auto root = platform::Vfs::find_installation(args.game);
  if (!root) {
    std::fprintf(stderr,
                 "no game installation found. Point at one with --game DIR or "
                 "$IMPERIVM_GAME_DIR; it is the directory holding rle.mmp and Packs/.\n");
    return false;
  }
  std::string error;
  if (!vfs_.mount_installation(*root, &error)) {
    std::fprintf(stderr, "cannot mount %s: %s\n", root->string().c_str(), error.c_str());
    return false;
  }
  std::printf("installation: %s\n", root->string().c_str());
  std::printf("pixel store:  %zu bytes mapped\n", vfs_.pixel_store().size());
  const std::optional<UserDirectory> user = choose_user_directory(args_, *root);
  if (!user) return false;
  user_dir_ = user->path;
  user_dir_is_installation_ = user->installation;
  if (user->temporary) temporary_user_dir_.path = user->path;
  std::printf("user data:    %s (%s)\n", user_dir_.string().c_str(), user->why.c_str());
  quicksave_ = user_dir_ / "quicksave.bfhp";
  mount_language();
  start_sound();
  load_settings();

  if (!args_.load.empty()) {
    // The file says which game it is a save of; the flags do not get a vote.
    core::sim::SaveFileContents contents;
    if (!gamedata::read_save_file(args_.load, contents, &error)) {
      std::fprintf(stderr, "%s\n", error.c_str());
      return false;
    }
    args_.map = contents.manifest.container;
    args_.map_index =
        contents.manifest.map_index.empty() ? -1 : SDL_atoi(contents.manifest.map_index.c_str());
    args_.play = true;
    seed_ = contents.manifest.seed;
    std::printf("resuming:     %s -- %s, turn %llu\n", args_.load.c_str(),
                gamedata::map_identity(contents.manifest).c_str(),
                static_cast<unsigned long long>(contents.manifest.turns));
    pending_load_ = std::move(contents);
  }

#if IMPERIVM_HAVE_NET
  if (args_.net.enabled() && !start_net()) return false;
#endif

  platform::Window::Options options;
  options.title = "Imperivm Reforged";
  options.width = args.width;
  options.height = args.height;
  options.borderless = args.borderless;
  options.fullscreen = args.fullscreen;
  // The play view is 1:1 -- one world unit across is one pixel across, and
  // there is no zoom in it. Asking for the backing-store resolution on a HiDPI
  // display would not magnify anything, it would show twice as much world at
  // half the apparent size, so a map renders at window resolution and is
  // upscaled by the compositor, which is what the original did.
  options.high_dpi = args.map.empty() && args.sprite_given;
  options.headless = args.headless;
  if (!window_.open(options)) {
    std::fprintf(stderr, "%s\n", window_.error());
    return false;
  }

  // The renderer targets the offscreen texture, not the swapchain, so the
  // frame can be read back and looked at.
  if (!renderer_.create(window_.device(), platform::render_target_format(), &error)) {
    std::fprintf(stderr, "sprite renderer: %s\n", error.c_str());
    return false;
  }

  map_mode_ = !args_.map.empty();
  if (!map_mode_ && !args_.sprite_given && args_.load.empty()) return start_front();
  if (!map_mode_) return start_sheet();
  if (!start_map()) return false;
  // Play mode is map mode plus a running simulation: the terrain, the camera
  // and the input are the same, so it builds on top rather than beside.
  play_mode_ = args_.play;
  return play_mode_ ? start_play() : true;
}

// --------------------------------------------------------------------------
// play mode
// --------------------------------------------------------------------------

bool Application::ensure_installation() {
  if (install_ != nullptr) return true;
  std::string error;
  install_ = std::make_unique<gamedata::Installation>();
  if (!install_->open(vfs_.root(), &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    install_.reset();
    return false;
  }
  return true;
}

#if IMPERIVM_HAVE_NET
/// The lobby, before the window: who plays which seat, on what seed, at what
/// difficulty. Blocking, with a timeout -- this is the command line's form;
/// the shipped lobby screen steps the same exchange from the frame loop.
bool Application::start_net() {
  if (!args_.load.empty() || args_.edit) {
    std::fprintf(stderr, "net: a networked match starts fresh; not with --load or --edit\n");
    return false;
  }
  const bool hosting = args_.net.host_port >= 0;
  if (hosting && args_.map.empty()) {
    std::fprintf(stderr, "net: --host needs --map; a joiner is told the host's\n");
    return false;
  }
  args_.play = true;
  const std::filesystem::path root = vfs_.root();
  // The game: the lobby refuses a joiner whose data.pak hashes differently.
  // The map is checked apart, against the hash the start carries.
  const std::uint64_t content = imperivm::app::hash_file(root / "Packs" / "data.pak");
  net_ = std::make_unique<imperivm::app::NetPlay>(args_.net);
  std::string error;
  if (hosting) {
    core::sim::Start base;
    base.you = static_cast<core::PlayerId>(args_.player);
    // A fresh game each time; every peer is told it, so nothing depends on
    // whose clock it came from.
    base.seed = imperivm::net::now_ms() | 1u;
    base.map = gamedata::container_relative(root, root / args_.map);
    base.map_hash = imperivm::app::hash_file(root / args_.map);
    base.map_index = static_cast<std::uint32_t>(args_.map_index);
    base.difficulty = static_cast<std::uint32_t>(std::clamp(settings_.difficulty, 0, 2));
    if (!net_->host(base, content, &error)) {
      std::fprintf(stderr, "net: %s\n", error.c_str());
      return false;
    }
  } else if (!net_->join(content, root, &error)) {
    std::fprintf(stderr, "net: %s\n", error.c_str());
    return false;
  }
  if (!receive_late_state(root, &error)) {
    std::fprintf(stderr, "net: %s\n", error.c_str());
    return false;
  }
  seed_ = net_->start().seed;
  args_.player = net_->seat();
  args_.map = net_->start().map;
  args_.map_index = static_cast<int>(net_->start().map_index);
  return true;
}
#endif

#if IMPERIVM_HAVE_NET
bool Application::receive_late_state(const std::filesystem::path& root, std::string* error) {
  // A match already running (`sim/netjoin.hpp`, this engine's): the session
  // is built as every peer's was, then loads the host's save over it -- the
  // path a save file takes, with the bytes from the network.
  if (net_ == nullptr || !net_->late()) return true;
  core::sim::SaveFileContents contents;
  if (!net_->receive_state(root, contents.session, error)) return false;
  std::printf("net:          the host's save arrived, %zu bytes\n", contents.session.size());
  std::fflush(stdout);
  pending_load_ = std::move(contents);
  return true;
}

void Application::apply_net_rows(const core::sim::Start& start) {
  setup_players_.clear();
  for (const core::sim::SeatRow& row : start.rows) {
    SetupPlayer player;
    player.slot = row.slot;
    // This machine's seat is the monitor; another peer's, the face.
    player.type = row.type == core::sim::SeatRow::Type::human ? (row.slot == start.you ? 0 : 4)
                  : row.type == core::sim::SeatRow::Type::computer ? 1
                                                                   : 2;
    player.name = row.name;
    player.race = row.race;
    player.difficulty = row.difficulty;
    player.team = row.team;
    player.bonus = row.bonus;
    setup_players_.push_back(std::move(player));
  }
  const core::sim::LobbyRules& r = start.rules;
  rules_.victory = r.victory;
  rules_.threshold = r.threshold;
  rules_.world_population = r.world_population;
  rules_.starting_gold = r.starting_gold;
  rules_.no_fog = !r.fog_of_war;
  rules_.no_exploration = !r.exploration;
  rules_.no_bonuses = !r.bonuses;
  rules_.shared_support = r.shared_support;
  rules_.shared_control = r.shared_control;
  setup_pending_ = true;
}
#endif

bool Application::start_play() {
  if (!ensure_installation()) return false;
  std::string error;

  // Every sound stops as the game's loop starts (0x0074a132), the menus'
  // music with it, and the match's music player lists `music/` afresh
  // (0x00551150): every file not starting with `_`, by name, as Windows
  // lists them (**this engine's** order; the draw is uniform either way).
  stop_sounds();
  menu_music_wanted_ = false;
  music_tracks_.clear();
  music_last_.reset();
  music_next_look_ = imperivm::sound::kMusicCheckMs;
  if (const std::filesystem::path dir = find_loose_file(vfs_.root(), "music"); !dir.empty()) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
      const std::string name = entry.path().filename().string();
      if (entry.is_regular_file(ec) && imperivm::sound::is_music_track(name)) music_tracks_.push_back(name);
    }
    std::sort(music_tracks_.begin(), music_tracks_.end());
  }

  // The map `start_map` chose, by number. **This read no index for as long
  // as it existed**, so on the conquest `--map-index 7` drew the seventh
  // map's terrain over a simulation of the third: `read_payloads` with no
  // index takes the first `Maps/<n>` in walk order, and nothing compared the
  // two until a save of the seventh map had to name which map it was of.
  payloads_ = gamedata::read_payloads(container_, gamedata::map_number(map_directory_));
  if (!payloads_.ok()) {
    std::fprintf(stderr, "%s holds no map.obj.xml\n", args_.map.c_str());
    return false;
  }

  // Built once per process: the table is world-invariant, `HostFn` is a plain
  // function pointer, and there is nothing per-match to capture.
  std::size_t implemented = registry_.size();
  if (!hosts_registered_) {
    implemented = core::sim::register_all_hosts(registry_);
    hosts_registered_ = true;
  }

  if (!world_view_.ready() && !world_view_.create(vfs_, renderer_, &error)) {
    std::fprintf(stderr, "world view: %s\n", error.c_str());
    return false;
  }
  if (rings_.size() == 0) {
    std::string rings_error;
    if (!rings_.load(vfs_, renderer_, &rings_error)) {
      std::fprintf(stderr, "selection rings: %s\n", rings_error.c_str());
    }
  }
  class_marks_.clear();
  // A match marks what is selected and, in the bar mode, what has health.
  // The editor marks neither: its selection is its own and drawn by it.
  if (args_.edit) {
    world_view_.set_marks(nullptr, nullptr);
  } else {
    world_view_.set_marks(&rings_, [this](const core::sim::WorldObject& object) { return marks_for(object); });
  }
  // The editor shows the map as placed, spawn templates included; a match
  // shows what is in play.
  world_view_.set_show_templates(args_.edit);
  if (entities_ == nullptr) entities_ = std::make_unique<AppEntities>(vfs_, world_view_);

  core::sim::SessionInputs inputs = gamedata::session_inputs(*install_, payloads_);
  // Without this every object spawns with no art and the view draws nothing.
  inputs.entities = entities_.get();
  inputs.masks = entities_.get();
  // A container's own scripts are in no pack. Without this chain every
  // `Maps/<n>/Sequences/seq*.vs` fails to resolve and the mission layer never
  // runs -- which is what `imrun` found for itself, and what this app did
  // for as long as it resolved against `data.pak` alone.
  scripts_ = std::make_unique<gamedata::ContainerScripts>(container_, install_->scripts());
  inputs.scripts = scripts_.get();
  // The language pack's table: what the bars, the notes and every host
  // `Translate` show. Never reached a session from the installation before.
  // And the container's own, under `Local/<language>/`, merged over it.
  if (!translation_bytes_.empty()) inputs.translations = translation_bytes_;
  localisation_ = gamedata::read_localisation(container_, language_);
  localisation_spans_.clear();
  for (const std::vector<std::byte>& document : localisation_) localisation_spans_.emplace_back(document);
  inputs.localisations = localisation_spans_;
  auto session = core::sim::GameSession::create(registry_, inputs, seed_);
  if (!session.ok()) {
    std::fprintf(stderr, "cannot build a session from %s (error %d)\n", args_.map.c_str(),
                 static_cast<int>(session.error()));
    return false;
  }
  session_ = std::move(session.value());
  // The camera the scripts see and move: `View`, `ViewPos`, `SetFog`. Unset,
  // every one of them was a no-op here -- a mission's cutscene played to a
  // camera that stayed where it was.
  view_state_ = core::sim::ViewState{};
  view_seen_ = core::sim::Point{};
  session_->host_context().view = &view_state_;

  // Slot 0 unless `--player` says otherwise. `game.xml` declares a
  // `start_player` and that is the right default, but `core::GameProperties`
  // does not parse the attribute yet, so guessing from it here would be
  // inventing a value; the flag is honest until the reader carries it.
  local_player_ = static_cast<core::PlayerId>(args_.player);
  session_->set_local_player(local_player_);
  if (args_.edit) {
    // The editor: the authored map as placed, nothing started, no turns.
    if (!ui_.ready() &&
        !ui_.create(vfs_, window_.device(), platform::render_target_format(), &error)) {
      std::fprintf(stderr, "interface: %s\n", error.c_str());
      return false;
    }
    start_editor();
    return true;
  }

  // Order matters and is deliberate: the match resolves races and starts the
  // victory scripts, then the map's own objects wake, then the AI. Each draws
  // script ids from the same counter, and a script id is world state.
  core::sim::MatchOptions match;
  match.human = local_player_;
  if (from_campaign_ && campaign_race_ >= 0) match.races[local_player_] = campaign_race_;
#if IMPERIVM_HAVE_NET
  // Every peer builds the same options, whoever's screen this is. From the
  // players' screen, the rows the host started from -- every peer rebuilds
  // `setup_players_` and `rules_` from the start and runs the setup below.
  // From the command line, the map's own setup, every seat human.
  if (net_ != nullptr) {
    setup_pending_ = false;
    if (net_->has_rows()) {
      apply_net_rows(net_->start());
    } else {
      match = net_->match_options();
    }
  }
#endif
  if (setup_pending_) {
    // The setup screen's rows: the human, each slot's race and control. In a
    // networked game every human row is a peer's -- this machine's or a
    // joiner's -- and the match's own `human` is the lowest of them, the one
    // value every peer holds alike because it is hashed.
    if (networked()) {
      match.multiplayer = true;
      match.human = core::kNoPlayer;
    }
    for (const SetupPlayer& row : setup_players_) {
      if (row.slot >= core::sim::kPlayerCount) continue;
      const bool human = row.type == 0 || row.type == 4;
      match.races[row.slot] = row.race;
      match.control_set[row.slot] = true;
      match.controls[row.slot] = human           ? core::sim::PlayerControl::human
                                 : row.type == 1 ? core::sim::PlayerControl::computer
                                                 : core::sim::PlayerControl::disabled;
      if (!human) continue;
      if (!networked() || match.human == core::kNoPlayer || row.slot < match.human) match.human = row.slot;
    }
    if (!networked()) local_player_ = match.human;
    session_->set_local_player(local_player_);
    // The settings screen's rules.
    if (!rules_.victory.empty()) {
      match.condition_set = true;
      match.condition = core::sim::parse_victory_condition(rules_.victory);
      match.threshold = rules_.threshold;
    }
    match.world_population = rules_.world_population;
    match.starting_gold = rules_.starting_gold;
    match.fog_of_war = !rules_.no_fog;
    match.exploration = !rules_.no_exploration;
  }
  const std::size_t victory = session_->start_match(match);
  // The game's difficulty, which `GetDifficulty`'s 136 readers multiply into
  // levels, counts and timers: the options' setting, or what the adventure
  // and campaign screens' combos last chose. It reached no match before.
  if (core::sim::MatchSystem* system = core::sim::match_system_of(session_->world()); system != nullptr) {
    int difficulty = args_.difficulty >= 0 ? args_.difficulty : settings_.difficulty;
#if IMPERIVM_HAVE_NET
    // The host's, not this machine's options: 136 readers multiply it in.
    if (net_ != nullptr) difficulty = static_cast<int>(net_->start().difficulty);
#endif
    (void)system->set_difficulty(std::clamp(difficulty, 0, 2));
  }
  start_speed();
  attach_outbox();
  sync_session_ground();
  if (setup_pending_) {
    // Names, and teams as mutual alliance: `0x15` is the word the shipped
    // maps hold between allies. **Reading, labelled:** what the original's
    // team button writes into the relations was not read.
    core::sim::PlayerTable& players = session_->world().players();
    for (const SetupPlayer& row : setup_players_) {
      if (row.slot >= core::sim::kPlayerCount) continue;
      if ((row.type == 0 || row.type == 4) && !row.name.empty()) players.setup(row.slot).name = row.name;
      players.setup(row.slot).difficulty = row.difficulty;
      if (row.type != 2 && row.bonus > 0) players.setup(row.slot).bonus = row.bonus;
      for (const SetupPlayer& other : setup_players_) {
        if (other.slot == row.slot || other.slot >= core::sim::kPlayerCount) continue;
        if (row.team != 0 && other.team == row.team && row.type != 2 && other.type != 2) {
          // Allies share view; shared support and shared control are the
          // settings' two checks, the profile's `sharedsupport` /
          // `sharedcontrol` (bits 2 and 5, `kRelationBits`). INFERRED: the
          // record's two fields are persisted and no reader of them was
          // found; the bits are what the words `0x15` and `0x35` carry.
          std::uint32_t word = core::sim::kRelationAllied;
          if (rules_.shared_support) word |= 1u << core::sim::kRelationBits[static_cast<std::size_t>(core::sim::Relation::share_support)];
          if (rules_.shared_control) word |= 1u << core::sim::kRelationBits[static_cast<std::size_t>(core::sim::Relation::share_control)];
          players.set_relation_word(row.slot, other.slot, word);
        }
      }
    }
    setup_pending_ = false;
  }
  const std::size_t started = session_->start_object_scripts();
  const std::size_t ai = session_->start_ai();
  // The setup screen's bonuses: `001 WEALTH.VS` and its two siblings, one
  // per player who chose one, on their first stronghold.
  // Not in a networked match: which bonus each player chose is the setup
  // screen's, and the lobby does not carry it yet. Every peer skips them alike.
  // From the players' screen the rows carry each player's choice; from the
  // command line nothing does, and every peer skips them alike.
  bool net_rowless = false;
#if IMPERIVM_HAVE_NET
  net_rowless = net_ != nullptr && !net_->has_rows();
#endif
  const std::size_t bonuses = (rules_.no_bonuses && !from_campaign_) || net_rowless
                                  ? 0
                                  : session_->start_bonuses(install_->bonus_scripts());
  if (bonuses > 0) std::printf("bonuses:      %zu bonus script(s) started\n", bonuses);
  {
    core::sim::SaveFileManifest manifest;
    manifest.container = gamedata::container_relative(vfs_.root(), vfs_.root() / args_.map);
    manifest.map_index = gamedata::map_number(payloads_.map_directory);
    identity_ = gamedata::map_identity(manifest);
    // The campaign between missions, before the sequences: the conquest's
    // root sequence reads `ConquestBonus()` on its first pass, and a carry
    // installed after that is a reward that never runs.
    // A campaign in progress is this machine's file; a networked match plays
    // the mission from its start on every peer.
    if (!payloads_.conquest.empty() && !networked()) {
      campaign_file_ = args_.campaign.empty()
                           ? gamedata::campaign_file_path(saves_directory(), manifest.container)
                           : std::filesystem::path(args_.campaign);
      core::sim::CampaignCarry carry;
      std::string why;
      if (!gamedata::read_campaign_file(campaign_file_, carry, &why)) {
        std::printf("campaign:     %s (%s)\n",
                    why.empty() ? "no campaign in progress" : why.c_str(),
                    campaign_file_.string().c_str());
      } else if (carry.container != manifest.container) {
        std::printf("campaign:     %s is a campaign of %s, not of this container\n",
                    campaign_file_.string().c_str(), carry.container.c_str());
      } else if (!session_->restore_campaign(carry).ok()) {
        std::printf("campaign:     %s refused by this conquest's table\n",
                    campaign_file_.string().c_str());
      } else {
        std::printf("campaign:     %s restored, %zu conquered, bonus \"%s\"\n",
                    campaign_file_.string().c_str(), carry.progress.conquered.size(),
                    carry.progress.active_bonus.c_str());
      }
    }
  }
  // Then the mission: the container's own sequence manifest first, then the
  // map's, which is the order the retail engine reaches them and the order
  // `imrun` starts them in. **This app never started a sequence before**, so
  // no campaign mission -- no narration, no objective, no `EndGame` -- ever
  // ran in it; it played every map as a skirmish.
  const std::size_t sequences =
      session_->start_sequences(payloads_.game_sequences, /*base=*/"") +
      session_->start_sequences(payloads_.map_sequences, payloads_.map_directory);

  // The real command bars, from the game's own 24 declarative INI files. The
  // faction is the local player's resolved race -- `setup_match` writes it back
  // into `PlayerSetup::race`, turning `Mutable` or `Random` into a name.
  if (!ui_.ready() &&
      !ui_.create(vfs_, window_.device(), platform::render_target_format(), &error)) {
    std::fprintf(stderr, "interface: %s\n", error.c_str());
    return false;
  }
  ui_.show_bars(true);
  const std::string faction(session_->world().players().setup(local_player_).race);
  if (!ui_.load_faction(faction, &error)) {
    // Not fatal: the world still draws, and a missing skin is worth seeing
    // rather than exiting over.
    std::fprintf(stderr, "interface: no skin for \"%s\": %s\n", faction.c_str(),
                 error.c_str());
  }
  std::printf("simulation:   %zu objects, %zu scripts started, %zu host entry points\n",
              session_->world().objects().size(), started, implemented);
  std::printf("match:        player %u (%s), %zu victory script(s), AI started for %zu player(s)\n",
              static_cast<unsigned>(local_player_), faction.c_str(), victory, ai);
  if (const core::sim::MatchSystem* rules_of = core::sim::match_system_of(session_->world())) {
    const core::sim::MatchRules& r = rules_of->rules();
    std::printf("rules:        %s \"%s\", population %d%%, gold %s, fog %s, exploration %s, bonuses %s\n",
                std::string(core::sim::victory_script_name(r.condition)).c_str(), r.param.c_str(),
                r.world_population, r.starting_gold < 0 ? "default" : std::to_string(r.starting_gold).c_str(),
                r.fog_of_war ? "on" : "off", r.exploration ? "on" : "off", bonuses > 0 ? "run" : "none");
  }
  std::printf("mission:      %zu sequence(s) started of %zu declared\n", sequences,
              payloads_.game_sequences.size() + payloads_.map_sequences.size());
  if (!networked()) {
    std::printf("              one turn every %d ms, %d game-time units at speed %d\n",
                args_.turn_interval, local_turn_length(),
                session_->world().clock().config().game_speed);
  }
  std::printf("keys:         P pauses, . steps one turn while paused, space is the map, "
              "F5 saves, F9 loads, F10 the menu\n");
  // Look at what the player owns rather than at the map's authored start
  // point. On Crossroads those are half a map apart and the opening frame is
  // bare rock. It has to be here, after the match is set up: `start_map` runs
  // before there is a session on every path, and it used to make this call
  // there, where `session_` was always null -- so it never fired.
  if (args_.at_x < 0 && look_at_local_player()) {
    look_pending_ = true;
    std::printf("view:         player %u's holding at %d, %d\n", static_cast<unsigned>(local_player_),
                look_x_, look_y_);
  }
  infobar_ = std::make_unique<core::sim::InfoBar>(*session_, install_->skills(),
                                                  install_->unit_specials());
  cmdbar_ = std::make_unique<core::sim::CommandBar>(*session_);
  infobar_tab_ = static_cast<std::uint32_t>(std::max(0, args_.tab));
  apply_select_argument();
#if IMPERIVM_HAVE_NET
  if (net_ != nullptr) {
    // The marker a seat the computer took is shown with: the language pack's
    // `(AI)`, context empty, as 0x00406840 asks for it (`sim/netdepart.hpp`).
    if (translations_ != nullptr) net_->set_computer_marker(std::string(translations_->translate("(AI)")));
    net_->begin(*session_, *cmdbar_, identity_);
    std::printf("net:          playing as player %u; pause, saves and the editor are off\n",
                static_cast<unsigned>(local_player_));
  }
#endif

  if (!args_.menu.empty()) {
    if (args_.menu == "game") {
      open_game_menu();
    } else if (args_.menu == "confirm") {
      open_confirm("Quit the game?", [] {});
    } else if (args_.menu == "save") {
      open_save_menu();
    } else if (args_.menu == "load") {
      open_load_menu();
    } else if (args_.menu == "options") {
      open_options_menu();
    } else if (args_.menu == "notes") {
      open_notes_menu();
    } else if (args_.menu == "diplomacy") {
      open_diplomacy_menu();
    } else if (args_.menu == "help") {
      open_help();
    } else if (args_.menu == "statistics") {
      open_statistics_menu();
    } else if (args_.menu == "endgame") {
      open_endgame_menu(true);
    } else {
      open_menu(args_.menu, [](const core::ui::DialogEvent&, core::ui::Dialog&) {});
    }
  }

  if (pending_load_.has_value()) {
    // Over the started session: the saved coroutines replace what was just
    // started, and a save whose hashes do not come back is refused rather
    // than run, because a half-restored session is a game that never existed.
    core::sim::SessionLoadReport loaded;
    const core::Status status = session_->load(pending_load_->session, identity_, &loaded);
    if (!status.ok()) {
      if (!loaded.hashes.ok) {
        std::fprintf(stderr, "%s: load refused, channel %.*s differs\n", args_.load.c_str(),
                     static_cast<int>(loaded.hashes.channel.size()),
                     loaded.hashes.channel.data());
      } else if (const std::string why = core::sim::save_version_refusal(pending_load_->session);
                 !why.empty()) {
        std::fprintf(stderr, "%s: load refused, %s\n", args_.load.c_str(), why.c_str());
      } else {
        std::fprintf(stderr, "%s: load refused (error %d)\n", args_.load.c_str(),
                     static_cast<int>(status.error()));
      }
      return false;
    }
    std::printf("loaded:       %zu objects, %zu scripts, turn %llu, hashes verified\n",
                loaded.objects, loaded.scripts, static_cast<unsigned long long>(loaded.meta.turns));
    sync_session_ground();
    turns_run_ = loaded.meta.turns;
    pending_load_.reset();
  }

  last_ticks_ = SDL_GetTicks();
  return true;
}

bool Application::quicksave() { return save_to(quicksave_); }

std::filesystem::path Application::saves_directory() const { return user_dir_; }

/// The `.bfhp` files in the saves directory, by name without the extension,
/// newest first -- which is the order the original's list shows its slots
/// in, as far as a directory listing can say.
std::vector<std::string> Application::save_names() const {
  std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
  std::error_code ignored;
  for (const auto& entry : std::filesystem::directory_iterator(saves_directory(), ignored)) {
    if (!entry.is_regular_file(ignored)) continue;
    const std::filesystem::path& path = entry.path();
    std::string extension = path.extension().string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension != ".bfhp") continue;
    found.emplace_back(entry.last_write_time(ignored), path.stem().string());
  }
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<std::string> names;
  for (auto& [time, name] : found) names.push_back(std::move(name));
  return names;
}

bool Application::save_to(const std::filesystem::path& file) {
  const core::Result<std::vector<std::byte>> saved = session_->save(identity_);
  if (!saved.ok()) {
    std::printf("save refused (error %d)\n", static_cast<int>(saved.error()));
    return false;
  }
  core::sim::SaveFileContents contents;
  contents.manifest.container = gamedata::container_relative(vfs_.root(), vfs_.root() / args_.map);
  contents.manifest.map_index = gamedata::map_number(payloads_.map_directory);
  contents.manifest.seed = seed_;
  contents.manifest.turns = session_->world().turns();
  contents.manifest.time = session_->world().time();
  contents.manifest.engine = core::version_string();
  contents.session = saved.value();
  std::string error;
  if (!gamedata::write_save_file(file, contents, &error)) {
    std::printf("%s\n", error.c_str());
    return false;
  }
  std::printf("saved: %s (turn %llu, %zu bytes)\n", file.string().c_str(),
              static_cast<unsigned long long>(contents.manifest.turns), contents.session.size());
  return true;
}

void Application::write_campaign_carry() {
  if (campaign_file_.empty() || campaign_written_) return;
  campaign_written_ = true;
  core::sim::MissionResult result;
  result.map_number = SDL_atoi(gamedata::map_number(payloads_.map_directory).c_str());
  result.won = true;
  if (const auto conquest = core::sim::ConquestMap::parse(payloads_.conquest); conquest.ok()) {
    result.territory =
        gamedata::territory_of_map(container_, *conquest, payloads_.map_directory);
  }
  const std::string container =
      gamedata::container_relative(vfs_.root(), vfs_.root() / args_.map);
  const core::sim::CampaignCarry carry = session_->campaign_carry(container, result);
  std::string error;
  if (!gamedata::write_campaign_file(campaign_file_, carry, &error)) {
    std::printf("%s\n", error.c_str());
    return;
  }
  std::printf("campaign: wrote %s -- territory %d conquered, next reward \"%s\"\n",
              campaign_file_.string().c_str(), result.territory,
              carry.progress.active_bonus.c_str());
  // Which maps the carry opens: every territory not yet owned that neighbours
  // an owned one, printed because the campaign map that would show it is
  // Part 6's.
  if (const auto conquest = core::sim::ConquestMap::parse(payloads_.conquest); conquest.ok()) {
    std::printf("campaign: next --map-index candidates:");
    std::size_t printed = 0;
    for (const std::int32_t territory : gamedata::open_territories(*conquest, carry.progress)) {
      const int number = gamedata::map_number_of_territory(container_, *conquest, territory);
      if (number < 0) continue;
      std::printf(" %d (%s)", number, conquest->territories()[territory].id.c_str());
      ++printed;
    }
    std::printf("%s\n", printed == 0 ? " none -- every territory is owned" : "");
  }
}

bool Application::quickload() { return load_from(quicksave_); }

bool Application::load_from(const std::filesystem::path& file) {
  core::sim::SaveFileContents contents;
  std::string error;
  if (!gamedata::read_save_file(file, contents, &error)) {
    std::printf("%s\n", error.c_str());
    return true;  // nothing was touched
  }
  if (gamedata::map_identity(contents.manifest) != identity_) {
    std::printf("%s is a save of %s, not of this map\n", file.string().c_str(),
                gamedata::map_identity(contents.manifest).c_str());
    return true;
  }
  // A save another build wrote is refused before the session is touched, so
  // the game in progress goes on: the envelope's versions are read first.
  if (const std::string why = core::sim::save_version_refusal(contents.session); !why.empty()) {
    std::printf("%s: load refused, %s\n", file.string().c_str(), why.c_str());
    return true;  // nothing was touched
  }
  core::sim::SessionLoadReport loaded;
  const core::Status status = session_->load(contents.session, identity_, &loaded);
  // A loaded game's ground has nothing to fade from.
  fog_view_.reset();
  if (!status.ok()) {
    std::printf("load refused (error %d); the session is no longer usable\n",
                static_cast<int>(status.error()));
    return false;
  }
  sync_session_ground();
  turns_run_ = loaded.meta.turns;
  owed_ms_ = 0.0;
  std::printf("loaded: %s (turn %llu, hashes verified)\n", file.string().c_str(),
              static_cast<unsigned long long>(loaded.meta.turns));
  return true;
}

void Application::sync_session_ground() {
  if (session_ == nullptr) return;
  const core::sim::GroundReport& ground = session_->ground();
  if (ground.changed.empty()) return;
  // One layer at a time: the session's over the map's, cell for cell, where
  // both hold the layer at the same geometry.
  const auto copy = [](const core::Grid& from, core::Grid& into, const core::edit::WorldRect& rect,
                       core::edit::CellRect* cells) {
    if (from.cell_size() == 0 || into.cell_size() != from.cell_size() || !into.writable()) return;
    const auto size = static_cast<std::int32_t>(from.cell_size());
    const std::int32_t x0 = std::max(0, rect.x0 / size);
    const std::int32_t y0 = std::max(0, rect.y0 / size);
    const std::int32_t x1 = std::min(static_cast<std::int32_t>(from.width()) - 1, rect.x1 / size);
    const std::int32_t y1 = std::min(static_cast<std::int32_t>(from.height()) - 1, rect.y1 / size);
    for (std::int32_t y = y0; y <= y1; ++y) {
      for (std::int32_t x = x0; x <= x1; ++x) {
        const std::uint32_t value = from.cell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        if (into.cell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) == value) continue;
        (void)into.set_cell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), value);
        if (cells != nullptr) cells->add(x, y);
      }
    }
  };
  core::edit::CellRect terrain_cells;
  core::edit::CellRect height_cells;
  for (const core::edit::WorldRect& rect : ground.changed) {
    copy(session_->world().terrain(), world_.terrain_mut(), rect, &terrain_cells);
    copy(session_->world().height(), world_.height_mut(), rect, &height_cells);
    copy(session_->decor(), world_.decor_mut(), rect, nullptr);
  }
  if (!height_cells.empty()) {
    // A cell's light is its slope to the right and below, so the cells
    // beside a changed one change too.
    height_cells.add(std::max(0, height_cells.x0 - 1), std::max(0, height_cells.y0 - 1));
    height_cells.add(height_cells.x1 + 1, height_cells.y1 + 1);
    core::edit::rebake_light(world_.light_mut(), world_.height(), height_cells);
    terrain_cells.merge(core::edit::CellRect{height_cells.x0 / 2, height_cells.y0 / 2, height_cells.x1 / 2,
                                             height_cells.y1 / 2});
  }
  if (!terrain_cells.empty()) {
    map_.invalidate_ground(terrain_cells.x0, terrain_cells.y0, terrain_cells.x1, terrain_cells.y1);
  }
  std::printf("ground:       %zu template(s): %zu cells copied, %zu height cells levelled, %zu decorations "
              "bulldozed, %zu shore cells\n",
              ground.changed.size(), ground.cells_copied, ground.cells_levelled, ground.decorations_bulldozed,
              ground.shore_cells);
}

// --------------------------------------------------------------------------
// menus
// --------------------------------------------------------------------------

core::ui::Dialog* Application::open_menu(std::string_view path, MenuHandler handler, std::string_view section) {
  std::string error;
  core::ui::Dialog* dialog = ui_.push_dialog(path, &error, section);
  if (dialog == nullptr) {
    std::printf("menu: %s\n", error.c_str());
    return nullptr;
  }
  menu_handlers_.push_back(std::move(handler));
  dialog->content().translate = translator();
  if (translations_ != nullptr) {
    const core::game::TranslationTable* table = translations_.get();
    const std::string screen_path = dialog->screen().path;
    dialog->content().translate_widget = [table, screen_path](std::string_view text, std::string_view widget,
                                                              std::string_view attribute) {
      return table->translate_in_context(text, core::ui::translation_context(screen_path, widget, attribute));
    };
  }
  if (ui_.dialog_count() == 1) SDL_StartTextInput(window_.handle());
  // A menu takes the pointer away from the bars.
  ui_.set_tooltip({}, 0, 0);
  hovered_button_.clear();
  pressed_button_.clear();
  pressed_queue_cell_ = -1;
  bars_dirty_ = true;
  return dialog;
}

void Application::close_menu() {
  if (ui_.dialog_count() == 0) return;
  ui_.pop_dialog();
  menu_handlers_.pop_back();
  if (ui_.dialog_count() == 0) {
    SDL_StopTextInput(window_.handle());
    owed_ms_ = 0.0;
  }
}

/// Close one dialog wherever it stands in the stack, with its handler.
void Application::close_menu_at(std::size_t index) {
  if (index >= ui_.dialog_count()) return;
  ui_.close_dialog(index);
  menu_handlers_.erase(menu_handlers_.begin() + static_cast<std::ptrdiff_t>(index));
  if (mouse_menu_ == index) mouse_menu_ = ~std::size_t{0};
  if (ui_.dialog_count() == 0) {
    SDL_StopTextInput(window_.handle());
    owed_ms_ = 0.0;
  }
}

void Application::close_menu(core::ui::Dialog* dialog) {
  if (dialog != nullptr) close_menu_at(ui_.index_of(dialog));
}

void Application::raise_menu(core::ui::Dialog* dialog) {
  const std::size_t from = ui_.index_of(dialog);
  if (from >= ui_.dialog_count() || from + 1 == ui_.dialog_count()) return;
  ui_.raise_dialog(dialog);
  // The same rotate the stack just did, so that index `i` still names the
  // handler of dialog `i`.
  std::rotate(menu_handlers_.begin() + static_cast<std::ptrdiff_t>(from),
              menu_handlers_.begin() + static_cast<std::ptrdiff_t>(from) + 1,
              menu_handlers_.end());
}

void Application::close_to_background() {
  while (ui_.dialog_count() > 1) close_menu();
}

void Application::close_all_menus() {
  while (ui_.dialog_count() > 0) close_menu();
}

bool Application::menus_take(const SDL_Event& event) {
  if (ui_.dialog_count() == 0) return false;
  if (editor_.active) {
    // The editor's palettes are tool windows over the map, not menus: the
    // pointer outside every one of them is the map's, and the keys are the
    // map's unless something that is not a palette (the save dialog) is on
    // top.
    const auto is_palette = [](const core::ui::Dialog* dialog) { return is_editor_pane(*dialog); };
    std::int32_t px = -1;
    std::int32_t py = -1;
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
      px = static_cast<std::int32_t>(event.motion.x);
      py = static_cast<std::int32_t>(event.motion.y);
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
      px = static_cast<std::int32_t>(event.button.x);
      py = static_cast<std::int32_t>(event.button.y);
    }
    if (px >= 0) {
      bool inside = false;
      for (std::size_t i = 0; i < ui_.dialog_count(); ++i) {
        const core::ui::Rect rect = ui_.dialog(i)->rect();
        if (px >= rect.x && px < rect.right() && py >= rect.y && py < rect.bottom()) inside = true;
      }
      const bool release_of_press = event.type == SDL_EVENT_MOUSE_BUTTON_UP && mouse_menu_ != ~std::size_t{0};
      // A release anywhere ends the map's drag, whoever takes the event.
      if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
        editor_drop();
        editor_.dragging = false;
        editor_.painting = false;
        editor_.area_handle = 0;
      }
      if (event.type == SDL_EVENT_MOUSE_MOTION && inside) editor_.pointer_on_map = false;
      if (!inside && !release_of_press) {
        if (event.type == SDL_EVENT_MOUSE_MOTION) ui_.set_tooltip({}, 0, 0);
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) mouse_menu_ = ~std::size_t{0};
        return false;
      }
    } else if (is_palette(ui_.dialog(ui_.dialog_count() - 1))) {
      // The keys are the map's -- unless an edit has the focus, which the
      // original honours the same way (0x004cb7a0's lookup is skipped while
      // an edit, list or combo has it): then the typing is the edit's, and
      // only Escape leaves it. The dialog that has the focus takes it,
      // wherever it stands in the stack.
      if (event.type != SDL_EVENT_TEXT_INPUT && event.type != SDL_EVENT_KEY_DOWN) return false;
      std::size_t typing = ~std::size_t{0};
      for (std::size_t i = ui_.dialog_count(); i-- > 0;) {
        core::ui::Dialog* dialog = ui_.dialog(i);
        if (dialog == nullptr || dialog->focused().empty()) continue;
        const core::ui::Widget* widget = dialog->screen().find(dialog->focused());
        if (widget == nullptr) continue;
        if (widget->kind != core::ui::WidgetType::kEditW && widget->kind != core::ui::WidgetType::kCombobox) continue;
        typing = i;
        break;
      }
      if (typing == ~std::size_t{0}) return false;
      if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        // Escape leaves the edit and does nothing else -- not the map's
        // `tooldefault`, which would drop the selection under the sheet.
        ui_.dialog(typing)->focus("");
        return true;
      }
      mouse_menu_ = typing;
    }
  }
  // A window dragged by its caption: `Move` (`%ID_MOVE%`, 0x10015) is the
  // strip the frame's caption sits on, and the press takes hold of it.
  if (editor_.active) {
    if (event.type == SDL_EVENT_MOUSE_MOTION && editor_.moving < ui_.dialog_count()) {
      core::ui::Dialog* dragged = ui_.dialog(editor_.moving);
      const core::ui::Rect canvas = ui_.canvas_rect();
      ui_.place_dialog(dragged, static_cast<std::int32_t>(event.motion.x) - editor_.move_dx - canvas.x,
                       static_cast<std::int32_t>(event.motion.y) - editor_.move_dy - canvas.y);
      redock_windows();
      return true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && editor_.moving != ~std::size_t{0}) {
      editor_.moving = ~std::size_t{0};
      return true;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
      const auto px = static_cast<std::int32_t>(event.button.x);
      const auto py = static_cast<std::int32_t>(event.button.y);
      // The topmost window whose frame is under the point -- not
      // `dialog_at`, which looks for a widget and the strip is not one.
      std::size_t under = ui_.dialog_count();
      for (std::size_t i = ui_.dialog_count(); i-- > 0;) {
        const core::ui::Rect rect = ui_.dialog(i)->rect();
        if (px >= rect.x && px < rect.right() && py >= rect.y && py < rect.bottom()) {
          under = i;
          break;
        }
      }
      core::ui::Dialog* dialog = ui_.dialog(under);
      if (dialog != nullptr && is_editor_pane(*dialog)) {
        const core::ui::Rect rect = dialog->rect();
        for (const core::ui::Widget& widget : dialog->screen().widgets) {
          if (!widget.has_id || widget.id != 0x10015) continue;
          const core::ui::Rect grip = dialog->widget_rect(widget.name);
          if (px < rect.x + grip.x || px >= rect.x + grip.right() || py < rect.y + grip.y || py >= rect.y + grip.bottom()) continue;
          // Not over a button on the strip (Close, Collapse), which is theirs.
          if (const core::ui::Widget* hit = dialog->widget_at(px, py); hit != nullptr && hit->kind != core::ui::WidgetType::kControl) break;
          editor_.moving = under;
          editor_.move_dx = px - rect.x;
          editor_.move_dy = py - rect.y;
          return true;
        }
      }
    }
  }
  // Keys go to the top; the pointer to the topmost screen under it, and a
  // release to the screen its press went to.
  std::size_t which = ui_.dialog_count() - 1;
  if (event.type == SDL_EVENT_MOUSE_MOTION) {
    which = ui_.dialog_at(static_cast<std::int32_t>(event.motion.x), static_cast<std::int32_t>(event.motion.y));
  } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    which = ui_.dialog_at(static_cast<std::int32_t>(event.button.x), static_cast<std::int32_t>(event.button.y));
    mouse_menu_ = which;
    // One focus among the open screens: a press on one takes it from the
    // others, so typing goes where the last click went.
    for (std::size_t i = 0; i < ui_.dialog_count(); ++i) {
      if (i != which && ui_.dialog(i) != nullptr) ui_.dialog(i)->focus("");
    }
  } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
    // The release goes to the screen its press went to -- the explorer's
    // Collapse under a docked pane, say -- and the press is spent.
    which = std::min(mouse_menu_, ui_.dialog_count() - 1);
    if (editor_.active) mouse_menu_ = ~std::size_t{0};
  } else if ((event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_KEY_DOWN) && editor_.active &&
             is_editor_pane(*ui_.dialog(ui_.dialog_count() - 1)) && mouse_menu_ < ui_.dialog_count()) {
    which = mouse_menu_;  // the palette with the focused edit, found above
  }
  if (editor_.active && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    // A press outside every palette but over the top one's rect: the check
    // above kept it, so it is a palette's.
    if (ui_.dialog(which) == nullptr) return false;
  }
  core::ui::Dialog* dialog = ui_.dialog(which);
  if (dialog == nullptr) return false;
  // The zoom map's pointer is its own: both buttons and the drag.
  if (zoom_open_ && dialog == zoom_dialog() && zoom_mouse(event, *dialog)) return true;
  core::ui::DialogEvent out;
  switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
      // Every other screen loses its hover.
      for (std::size_t i = 0; i < ui_.dialog_count(); ++i) {
        if (i != which) (void)ui_.dialog(i)->mouse_move(-1, -1);
      }
      out = dialog->mouse_move(static_cast<std::int32_t>(event.motion.x),
                               static_cast<std::int32_t>(event.motion.y));
      // A menu widget's `HelpText` is its tooltip, on the front as in play.
      ui_.set_tooltip(widget_tooltip(ui_.widget_at(static_cast<std::int32_t>(event.motion.x),
                                                   static_cast<std::int32_t>(event.motion.y))),
                      static_cast<std::int32_t>(event.motion.x) + 12,
                      static_cast<std::int32_t>(event.motion.y) - 8);
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      if (event.button.button != SDL_BUTTON_LEFT) return true;
      out = dialog->mouse_down(static_cast<std::int32_t>(event.button.x),
                               static_cast<std::int32_t>(event.button.y));
      break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
      if (event.button.button != SDL_BUTTON_LEFT) return true;
      out = dialog->mouse_up(static_cast<std::int32_t>(event.button.x),
                             static_cast<std::int32_t>(event.button.y));
      break;
    case SDL_EVENT_TEXT_INPUT:
      out = dialog->text_input(event.text.text);
      break;
    case SDL_EVENT_KEY_DOWN: {
      core::ui::DialogKey key;
      switch (event.key.key) {
        case SDLK_ESCAPE: key = core::ui::DialogKey::kEscape; break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: key = core::ui::DialogKey::kEnter; break;
        case SDLK_BACKSPACE: key = core::ui::DialogKey::kBackspace; break;
        case SDLK_DELETE: key = core::ui::DialogKey::kDelete; break;
        case SDLK_LEFT: key = core::ui::DialogKey::kLeft; break;
        case SDLK_RIGHT: key = core::ui::DialogKey::kRight; break;
        case SDLK_HOME: key = core::ui::DialogKey::kHome; break;
        case SDLK_END: key = core::ui::DialogKey::kEnd; break;
        case SDLK_UP: key = core::ui::DialogKey::kUp; break;
        case SDLK_DOWN: key = core::ui::DialogKey::kDown; break;
        case SDLK_PAGEUP: key = core::ui::DialogKey::kPageUp; break;
        case SDLK_PAGEDOWN: key = core::ui::DialogKey::kPageDown; break;
        case SDLK_TAB: key = core::ui::DialogKey::kTab; break;
        default:
          // F11 still flips the window; every other key is the menu's.
          return event.key.key != SDLK_F11;
      }
      out = dialog->key(key);
      // Enter sends, in both chat screens. Neither file binds it -- the
      // in-game one's `Enter = SendBtn` is commented out, the lobby's has no
      // button -- so this is this engine's: the line an edit ends with is
      // the line said.
      if (key == core::ui::DialogKey::kEnter && out.kind == core::ui::DialogEvent::Kind::kNone) {
        const std::string path = fold_name(dialog->screen().path);
        if (path.find("ingamechat.ini") != std::string::npos) {
          out.kind = core::ui::DialogEvent::Kind::kCommand;
          out.id = 0x1001;
          out.widget = "SendBtn";
        } else if (path.find("mpchat.ini") != std::string::npos) {
          out.kind = core::ui::DialogEvent::Kind::kCommand;
          out.id = 0x9003;
          out.widget = "ChatEdit";
        }
      }
      break;
    }
    default:
      return false;
  }
  if (out.kind == core::ui::DialogEvent::Kind::kNone) return true;
  if (out.kind == core::ui::DialogEvent::Kind::kEscape) {
    // Escape with nothing to press closes the menu.
    if (zoom_open_ && ui_.dialog_count() == 1) {
      close_zoom_map();
    } else {
      close_menu();
    }
    return true;
  }
  // The handler may close its own menu (and open another), so it is copied
  // out first: the vector it lives in is about to change under it.
  const MenuHandler handler = menu_handlers_[which];
  if (out.kind == core::ui::DialogEvent::Kind::kCommand) play_click(out.id);
  handler(out, *dialog);
  return true;
}

/// F10, the bar's menu button and Escape. Once the match is decided for the
/// local player the menu they open is the end-game screen: the key and the
/// button toggle the menu the owner keeps (0x005e1a20, from 0x005e9398 and
/// 0x006ccd81), and the message that makes it (0x17050017, 0x005e8656)
/// builds `CVXUIEndGameMenu` (0x006cd4f0) rather than the game menu
/// (0x006ced00) when the local player's slot word `+0x290` is no longer 1.
/// That is how the end-game screen comes back after Statistics closed it.
/// **Reading, labelled:** that the word leaves 1 for a winner too; the end of
/// the match opens the end-game screen through the same message (0x0051d56e)
/// whichever way it went, which is only consistent with that.
void Application::open_game_menu() {
  dismiss_paused();
  if (session_ == nullptr || ui_.dialog_count() > 0) return;
  if (reported_outcome_) {
    const core::sim::MatchStatus match =
        networked() ? session_->match_status(local_player_) : session_->match_status();
    if (match.over && match.human != core::kNoPlayer) {
      open_endgame_menu(match.human_won);
      return;
    }
  }
  open_menu("menuini/gamemenu.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog&) {
    if (event.kind != core::ui::DialogEvent::Kind::kCommand) return;
    switch (event.id) {
      case 0x1001: open_load_menu(); break;
      case 0x1002: open_save_menu(); break;
      case 0x1003: open_options_menu(); break;
      case 0x1004:
        open_confirm("Restart the game?", [this] {
          close_all_menus();
          if (!restart_play()) quit_requested_ = true;
        });
        break;
      case 0x1005:
        open_confirm("Surrender?", [this] {
          close_all_menus();
#if IMPERIVM_HAVE_NET
          if (net_ != nullptr) {
            core::sim::NetOrder order;
            order.kind = core::sim::NetOrderKind::surrender;
            net_->queue(std::move(order));
            return;
          }
#endif
          session_->declare_match(local_player_, /*lost=*/true);
        });
        break;
      case 0x1006:
        open_confirm("Quit the game?", [this] {
          close_all_menus();
          if (args_.from_front) {
            return_to_front();
          } else {
            quit_requested_ = true;
          }
        });
        break;
      case 0x1007: close_menu(); break;
      default: break;
    }
  });
}

/// `CONFIRM.INI`: the question in its caption, Yes `0x1002`, No `0x1003`.
void Application::open_confirm(std::string question, std::function<void()> yes) {
  core::ui::Dialog* dialog = open_menu(
      "menuini/confirm.ini",
      [this, yes = std::move(yes)](const core::ui::DialogEvent& event, core::ui::Dialog&) {
        if (event.kind != core::ui::DialogEvent::Kind::kCommand) return;
        if (event.id == 0x1002) {
          close_menu();
          yes();
        } else if (event.id == 0x1003) {
          close_menu();
        }
      });
  if (dialog != nullptr) dialog->set_text("Text", question);
}

/// `SAVEGAME.INI`: the slots in `List` (`0x1015`), the name in `NameEdit`
/// (`0x1004`), Save `0x1001`, Delete `0x1002`, Cancel `0x1003`. Choosing a
/// slot puts its name in the edit, so Save over it is one click.
void Application::open_save_menu() {
  core::ui::Dialog* dialog = open_menu(
      "menuini/savegame.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        if (event.kind == Kind::kSelect && event.widget == "List") {
          const std::vector<std::string> names = save_names();
          if (event.index >= 0 && static_cast<std::size_t>(event.index) < names.size()) {
            menu.set_text("NameEdit", names[static_cast<std::size_t>(event.index)]);
          }
          return;
        }
        if (event.kind != Kind::kCommand) return;
        if (event.id == 0x1003) {
          close_menu();
          return;
        }
        const std::string name = menu.text("NameEdit");
        if (event.id == 0x1001) {
          if (name.empty() || name.find_first_of("/\\") != std::string::npos) return;
          if (save_to(saves_directory() / (name + ".bfhp"))) close_all_menus();
        } else if (event.id == 0x1002) {
          if (name.empty()) return;
          std::error_code ignored;
          std::filesystem::remove(saves_directory() / (name + ".bfhp"), ignored);
          menu.set_items("List", save_names());
          menu.set_text("NameEdit", "");
        }
      });
  if (dialog == nullptr) return;
  dialog->set_items("List", save_names());
  dialog->set_text("NameEdit", "");
}

/// `LOADGAME.INI`: the same list, Load `0x1001`, Delete `0x1002`, Cancel
/// `0x1003`; a double click on a slot loads it. The `GameType` combobox
/// (`0x100A`) that the original filters the list with is shown with the
/// one kind this app writes.
void Application::open_load_menu() {
  core::ui::Dialog* dialog = open_menu(
      "menuini/loadgame.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        const auto chosen = [&]() -> std::string {
          const std::vector<std::string> names = save_names();
          const std::int32_t index = menu.selected("List");
          if (index < 0 || static_cast<std::size_t>(index) >= names.size()) return std::string();
          return names[static_cast<std::size_t>(index)];
        };
        const auto load = [&]() {
          const std::string name = chosen();
          if (name.empty()) return;
          close_all_menus();
          if (!load_from(saves_directory() / (name + ".bfhp"))) quit_requested_ = true;
        };
        if (event.kind == Kind::kActivate && event.widget == "List") {
          load();
          return;
        }
        if (event.kind != Kind::kCommand) return;
        if (event.id == 0x1003) {
          close_menu();
        } else if (event.id == 0x1001) {
          load();
        } else if (event.id == 0x1002) {
          const std::string name = chosen();
          if (name.empty()) return;
          std::error_code ignored;
          std::filesystem::remove(saves_directory() / (name + ".bfhp"), ignored);
          menu.set_items("List", save_names());
        }
      });
  if (dialog == nullptr) return;
  dialog->set_items("List", save_names());
  dialog->set_items("GameType", {"Saved games"});
  dialog->select("GameType", 0);
  if (!save_names().empty()) dialog->select("List", 0);
}

/// `GAMEOPTIONS.INI`: shown as the file draws it; OK `0x1002` and Cancel
/// `0x1003` both close it, because nothing on it is wired to a setting yet.
std::filesystem::path Application::settings_file() const { return saves_directory() / "settings.ini"; }

void Application::load_settings() {
  // The installation's `Settings.ini` first, the engine's own over it.
  const auto read = [this](const std::filesystem::path& path) {
    std::vector<std::byte> bytes;
    if (std::ifstream file(path, std::ios::binary); file) {
      std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      bytes.resize(text.size());
      std::memcpy(bytes.data(), text.data(), text.size());
    }
    if (bytes.empty()) return;
    auto ini = core::IniDocument::parse(bytes);
    if (!ini.ok()) return;
    const core::SectionIndex section = ini->section("Options");
    for (const core::IniEntry& entry : ini->entries_of(section)) {
      if (!entry.has_key) continue;
      const int value = SDL_atoi(std::string(entry.value).c_str());
      const std::string_view key = entry.key;
      if (key == "Resolution") settings_.resolution = value;
      else if (key == "SoundVolume") settings_.sound_volume = value;
      else if (key == "MusicVolume") settings_.music_volume = value;
      else if (key == "SpeechVolume") settings_.speech_volume = value;
      else if (key == "ReverseSpeakers") settings_.reverse_speakers = value != 0;
      else if (key == "NoObjectAnimations") settings_.no_object_animations = value != 0;
      else if (key == "NoWaterAnimation") settings_.no_water_animation = value != 0;
      else if (key == "Music") settings_.music = value != 0;
      else if (key == "SoundFX") settings_.sound_fx = value != 0;
      else if (key == "NatureSounds") settings_.nature_sounds = value != 0;
      else if (key == "Speech") settings_.speech = value != 0;
      else if (key == "Conversations") settings_.conversations = value != 0;
      else if (key == "GameSpeed") settings_.game_speed = value;
      else if (key == "ScrollSpeed") settings_.scroll_speed = value;
      else if (key == "Difficulty") settings_.difficulty = value;
    }
    // The tips' place, as the original keeps it in its `settings.ini`
    // (0x006d80a4 reads `[Tips] AllShown` and `LastTip`).
    if (const core::SectionIndex tips = ini->section("Tips"); tips != core::kNoSection) {
      for (const core::IniEntry& entry : ini->entries_of(tips)) {
        if (!entry.has_key) continue;
        const int value = SDL_atoi(std::string(entry.value).c_str());
        if (entry.key == "LastTip") tips_last_ = value;
        else if (entry.key == "AllShown") tips_all_shown_ = value != 0;
      }
    }
    // The setup's rules, under the original profile's section and keys.
    const core::SectionIndex player = ini->section("Player");
    if (player == core::kNoSection) return;
    for (const core::IniEntry& entry : ini->entries_of(player)) {
      if (!entry.has_key) continue;
      const int value = SDL_atoi(std::string(entry.value).c_str());
      const std::string_view key = entry.key;
      if (key == "nofogofwar") rules_.no_fog = value != 0;
      else if (key == "noexploration") rules_.no_exploration = value != 0;
      else if (key == "sharedcontrol") rules_.shared_control = value != 0;
      else if (key == "sharedsupport") rules_.shared_support = value != 0;
      else if (key == "nobonus") rules_.no_bonuses = value != 0;
      else if (key == "startinggold") rules_.starting_gold = value;
      else if (key == "worldpop") rules_.world_population = value;
      else if (key == "victorycond") rules_.victory = std::string(entry.value);
      else if (key == "victorytreshold") rules_.threshold = std::string(entry.value);
    }
  };
  read(vfs_.root() / "Settings.ini");
  read(settings_file());
  load_profile_choice();
  apply_settings();
}

/// Which profile is in use: `Saves/settings.ini`'s `[Profiles] default` if
/// this engine has chosen one, else the installation's
/// `Profiles/profiles.ini` `default=profiles/<name>`, which is the line the
/// original keeps it on. The section there has an empty name, which is why
/// the whole document is walked for the key rather than one section asked
/// for it.
void Application::load_profile_choice() {
  const auto read = [this](const std::filesystem::path& path, bool strip_prefix) {
    std::vector<std::byte> bytes;
    if (std::ifstream file(path, std::ios::binary); file) {
      std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      bytes.resize(text.size());
      std::memcpy(bytes.data(), text.data(), text.size());
    }
    if (bytes.empty()) return;
    const auto document = core::IniDocument::parse(bytes);
    if (!document.ok()) return;
    for (const core::IniEntry& entry : document->entries()) {
      if (!entry.has_key || fold_name(std::string(entry.key)) != "default") continue;
      if (entry.value.empty()) continue;
      std::string value(entry.value);
      if (strip_prefix) {
        std::replace(value.begin(), value.end(), '\\', '/');
        if (const std::size_t slash = value.rfind('/'); slash != std::string::npos) {
          value.erase(0, slash + 1);
        }
      }
      if (!value.empty()) profile_ = std::move(value);
      return;
    }
  };
  read(vfs_.root() / "Profiles" / "profiles.ini", true);
  read(settings_file(), false);
}

bool Application::save_settings() const {
  std::error_code ignored;
  std::filesystem::create_directories(settings_file().parent_path(), ignored);
  std::ofstream file(settings_file(), std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file << "[Options]\n"
       << "Resolution=" << settings_.resolution << "\n"
       << "SoundVolume=" << settings_.sound_volume << "\n"
       << "MusicVolume=" << settings_.music_volume << "\n"
       << "SpeechVolume=" << settings_.speech_volume << "\n"
       << "ReverseSpeakers=" << (settings_.reverse_speakers ? 1 : 0) << "\n"
       << "NoObjectAnimations=" << (settings_.no_object_animations ? 1 : 0) << "\n"
       << "NoWaterAnimation=" << (settings_.no_water_animation ? 1 : 0) << "\n"
       << "Music=" << (settings_.music ? 1 : 0) << "\n"
       << "SoundFX=" << (settings_.sound_fx ? 1 : 0) << "\n"
       << "NatureSounds=" << (settings_.nature_sounds ? 1 : 0) << "\n"
       << "Speech=" << (settings_.speech ? 1 : 0) << "\n"
       << "Conversations=" << (settings_.conversations ? 1 : 0) << "\n"
       << "GameSpeed=" << settings_.game_speed << "\n"
       << "ScrollSpeed=" << settings_.scroll_speed << "\n"
       << "Difficulty=" << settings_.difficulty << "\n"
       << "\n[Player]\n"
       << "nofogofwar=" << (rules_.no_fog ? 1 : 0) << "\n"
       << "noexploration=" << (rules_.no_exploration ? 1 : 0) << "\n"
       << "sharedcontrol=" << (rules_.shared_control ? 1 : 0) << "\n"
       << "sharedsupport=" << (rules_.shared_support ? 1 : 0) << "\n"
       << "nobonus=" << (rules_.no_bonuses ? 1 : 0) << "\n"
       << "startinggold=" << rules_.starting_gold << "\n"
       << "worldpop=" << rules_.world_population << "\n"
       << "victorycond=" << rules_.victory << "\n"
       << "victorytreshold=" << rules_.threshold << "\n"
       << "\n[Tips]\n"
       << "LastTip=" << tips_last_ << "\n"
       << "AllShown=" << (tips_all_shown_ ? 1 : 0) << "\n"
       // Which profile the *Change player* screen last selected. The
       // original keeps this in `Profiles/profiles.ini` under the
       // installation, which is read-only here, so it rides the engine's
       // own settings file in the shape that file uses it.
       << "\n[Profiles]\n"
       << "default=" << profile_ << "\n";
  return static_cast<bool>(file);
}

/// The window sizes the resolution combobox offers, in the order the
/// original's `Resolution=` index counts them. **Reading, labelled:** the
/// shipped file says `0` and nothing says what 0 is; 1024 x 768 is the
/// size every menu is authored for, and the rest are the sizes that came
/// after it.
constexpr struct {
  int width;
  int height;
} kResolutions[] = {{1024, 768}, {1280, 800}, {1280, 1024}, {1440, 900}, {1600, 900}, {1680, 1050}, {1920, 1080}, {1920, 1200}};

void Application::apply_settings() {
  // A type switched off stops what it is playing (0x006af960), and what
  // still plays takes the sliders as they now are (0x006e7377 runs
  // 0x006b0810).
  for (std::size_t t = 1; t < imperivm::sound::kSoundTypes; ++t) {
    if (!sound_type_on(static_cast<imperivm::sound::SoundType>(t))) audio_.stop_pool(t);
  }
  follow_view(/*force=*/true);
  // The game speed is not applied here: in a match it is an order, posted
  // by the options screen's OK, and a match starts at `NormalSpeed`.
  if (window_.handle() != nullptr && !args_.fullscreen && !args_.size_given &&
      settings_.resolution >= 0 && static_cast<std::size_t>(settings_.resolution) < std::size(kResolutions)) {
    window_.set_size(kResolutions[settings_.resolution].width, kResolutions[settings_.resolution].height);
  }
}

std::int32_t Application::local_turn_length() const {
  if (args_.turn_length > 0) return args_.turn_length;
  const std::int32_t speed = session_ != nullptr ? session_->world().clock().config().game_speed
                                                 : core::sim::kDefaultGameSpeed;
  return core::sim::turn_length_from_real_ms(std::max(1, args_.turn_interval), speed);
}

void Application::run_local_turn() {
  if (session_ == nullptr) return;
  core::sim::ScriptOrderVerifier verifier(session_->scheduler(), session_->host_context());
  const core::sim::LocalTurn turn = core::sim::begin_local_turn(
      session_->world(), local_orders_, std::max(1, args_.turn_interval), &verifier);
  if (turn.report.cancels > 0 || std::any_of(turn.orders.orders.begin(), turn.orders.orders.end(),
                                              [](const core::sim::NetOrder& order) {
                                                return order.kind == core::sim::NetOrderKind::cancel_command;
                                              })) {
    std::printf("cancel:       %zu queued command(s) taken out on turn %llu\n", turn.report.cancels,
                static_cast<unsigned long long>(turns_run_ + 1));
    for (const core::sim::NetOrder& order : turn.orders.orders) {
      if (order.kind == core::sim::NetOrderKind::cancel_command) print_queue(order.target.object);
    }
    bars_dirty_ = true;
  }
  for (const core::sim::NetOrder& order : turn.orders.orders) {
    if (order.kind == core::sim::NetOrderKind::diplomacy) {
      print_diplomacy(order, turns_run_ + 1);
      continue;
    }
    if (order.kind != core::sim::NetOrderKind::set_speed) continue;
    // Applied with this turn, so the next is the first converted at it.
    std::printf("speed:        player %u asked %d, %d per mille from turn %llu: %d units a turn\n",
                static_cast<unsigned>(order.issuer), order.speed,
                session_->world().clock().config().game_speed,
                static_cast<unsigned long long>(turns_run_ + 2), local_turn_length());
    std::fflush(stdout);
  }
  session_->advance(1, args_.turn_length > 0 ? args_.turn_length : turn.length);
  ++turns_run_;
}

/// What an applied `diplomacy` order did, as the table now holds it: the
/// original logs `diplomacy changed` from `SetRelation` (0x0056533c).
void Application::print_diplomacy(const core::sim::NetOrder& order, std::uint64_t turn) const {
  if (session_ == nullptr) return;
  const core::sim::PlayerTable& players = session_->world().players();
  if (order.other == core::kNoPlayer) {
    std::printf("diplomacy:    player %u's allied victory %s on turn %llu\n",
                static_cast<unsigned>(order.issuer),
                players.setup(order.issuer).allied_flag ? "on" : "off",
                static_cast<unsigned long long>(turn));
  } else {
    std::printf("diplomacy:    player %u's word for player %u is 0x%02x on turn %llu\n",
                static_cast<unsigned>(order.issuer), static_cast<unsigned>(order.other),
                players.relation_word(order.issuer, order.other), static_cast<unsigned long long>(turn));
  }
  std::fflush(stdout);
}

void Application::post_speed_option(int option) {
#if IMPERIVM_HAVE_NET
  if (net_ != nullptr) {
    net_->ask_speed_option(option);
    return;
  }
#endif
  // A single-player match's speed is always variable: **inferred**, since
  // the settings' `gamespeed` (-1 variable, 0x00526833) was read for the
  // lobby's record and the single-player screens were not followed.
  if (local_orders_.speed_fixed()) return;
  core::sim::NetOrder order;
  order.kind = core::sim::NetOrderKind::set_speed;
  order.issuer = local_player_;
  order.speed = core::sim::game_speed_from_option(option);
  local_orders_.post(std::move(order));
}

void Application::attach_outbox() {
  if (session_ == nullptr) return;
  core::sim::OrderOutbox* outbox = &local_orders_;
#if IMPERIVM_HAVE_NET
  net_outbox_.reset();
  if (net_ != nullptr) {
    net_outbox_ = std::make_unique<NetOutbox>(*net_);
    outbox = net_outbox_.get();
  }
#endif
  session_->host_context().outbox = outbox;
}

void Application::start_speed() {
  if (session_ == nullptr) return;
  core::sim::World& world = session_->world();
  (void)local_orders_.take();
  if (!networked()) {
    // 0x0052683b: a variable-speed match starts at `[GamePlay] NormalSpeed`,
    // clamped to 1..100000. 1000 where the table has no such key, which is
    // the shipped value and the clock's default.
    std::int32_t normal = core::sim::kDefaultGameSpeed;
    if (core::sim::EnvSystem* env = core::sim::env_of(world); env != nullptr) {
      std::int32_t value = 0;
      if (env->constant("NormalSpeed", value)) normal = value;
    }
    world.clock().set_game_speed(core::sim::clamp_game_speed(normal));
  }
  // 0x005268e7 -> 0x006e71e0: the options' position, written back from the
  // speed the match starts at. The player's own `GameSpeed` does not survive
  // into a match; 1000 comes back as 13. (Fault-injected, and no test here
  // can tell: the installation's saved position is already 13, the output of
  // this very write-back, and a test may not rewrite the player's file.)
  settings_.game_speed = core::sim::option_from_game_speed(world.clock().config().game_speed);
}

/// `GAMEOPTIONS.INI`: the checks (`0x100B` music, `0x1010` sound, `0x1011`
/// nature, `0x1015` speech, `0x1016` conversations, `0x101B` reverse
/// speakers, `0x1007` no object animations, `0x1008` no water animation),
/// the sliders (`MVScroll` music, `SndScroll` sound, `SpScroll` speech,
/// `GScroll` game speed, `GSScroll` scroll speed), the resolution
/// (`0x1004`) and the difficulty comboboxes; OK `0x1002` applies and
/// writes, Cancel `0x1003` drops.
void Application::open_options_menu() {
  auto edited = std::make_shared<Settings>(settings_);
  core::ui::Dialog* dialog = open_menu(
      "menuini/gameoptions.ini", [this, edited](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        if (event.kind == Kind::kChange) {
          const int value = event.index;
          if (event.widget == "MVScroll") edited->music_volume = value;
          else if (event.widget == "SndScroll") edited->sound_volume = value;
          else if (event.widget == "SpScroll") edited->speech_volume = value;
          // The position itself, as the volumes are. **Inferred**: the
          // slider's range was not read; 0..100 spans 700..3001 per mille,
          // `SlowSpeed` to CONST.INI's `Speed5` 3000 within a unit, which a
          // range picked to reach the fastest preset would.
          else if (event.widget == "GScroll") edited->game_speed = value;
          else if (event.widget == "GSScroll") edited->scroll_speed = value;
          else if (event.widget == "ResolutionCombo") edited->resolution = value;
          else if (event.widget == "DifficultyCombo") edited->difficulty = value;
          return;
        }
        if (event.kind != Kind::kCommand) return;
        const auto checked = [&](const char* name) {
          const core::ui::WidgetState* state = menu.content().state_of(name);
          return state != nullptr && state->row == 1;
        };
        switch (event.id) {
          case 0x100B: edited->music = checked("PlayMusicCheck"); break;
          case 0x1010: edited->sound_fx = checked("SndCheck"); break;
          case 0x1011: edited->nature_sounds = checked("NSndCheck"); break;
          case 0x1015: edited->speech = checked("USCheck"); break;
          case 0x1016: edited->conversations = checked("CnvCheck"); break;
          case 0x101B: edited->reverse_speakers = checked("RSCheck"); break;
          case 0x1007: edited->no_object_animations = checked("TurnOffObjCheck"); break;
          case 0x1008: edited->no_water_animation = checked("TurnOffWaterCheck"); break;
          case 0x1002:
            // 0x006e7ff0: OK during a match sends the position's speed as a
            // `CVXCmdSetSpeed`, **moved or not** -- which is how a player who
            // never touched the slider ends up at 999. Through the local
            // command path, so a networked match agrees it on one turn.
            if (play_mode_ && session_ != nullptr) post_speed_option(edited->game_speed);
            settings_ = *edited;
            apply_settings();
            if (!save_settings()) std::printf("options: %s could not be written\n", settings_file().string().c_str());
            close_menu();
            break;
          case 0x1003: close_menu(); break;
          default: break;
        }
      });
  if (dialog == nullptr) return;
  dialog->set_row("PlayMusicCheck", settings_.music ? 1 : 0);
  dialog->set_row("SndCheck", settings_.sound_fx ? 1 : 0);
  dialog->set_row("NSndCheck", settings_.nature_sounds ? 1 : 0);
  dialog->set_row("USCheck", settings_.speech ? 1 : 0);
  dialog->set_row("CnvCheck", settings_.conversations ? 1 : 0);
  dialog->set_row("RSCheck", settings_.reverse_speakers ? 1 : 0);
  dialog->set_row("TurnOffObjCheck", settings_.no_object_animations ? 1 : 0);
  dialog->set_row("TurnOffWaterCheck", settings_.no_water_animation ? 1 : 0);
  dialog->set_value("MVScroll", settings_.music_volume);
  dialog->set_value("SndScroll", settings_.sound_volume);
  dialog->set_value("SpScroll", settings_.speech_volume);
  dialog->set_value("GScroll", std::clamp(settings_.game_speed, 0, 100));
  dialog->set_value("GSScroll", settings_.scroll_speed);
  std::vector<std::string> resolutions;
  for (const auto& size : kResolutions) {
    resolutions.push_back(std::to_string(size.width) + " x " + std::to_string(size.height));
  }
  dialog->set_items("ResolutionCombo", std::move(resolutions));
  dialog->select("ResolutionCombo", std::clamp<int>(settings_.resolution, 0, static_cast<int>(std::size(kResolutions)) - 1));
  dialog->set_items("DifficultyCombo", {item_label("Easy", "Adventure difficulty level"),
                                        item_label("Normal", "Adventure difficulty level"),
                                        item_label("Hard", "Adventure difficulty level")});
  dialog->select("DifficultyCombo", std::clamp(settings_.difficulty, 0, 2));
  // The four `@hints` rows are this combo's items; which `Settings.ini` key
  // holds the choice is not read (this install's file has none), so it is
  // shown and not kept.
  dialog->set_items("NotificationsCombo", {item_label("Notify", "hints"), item_label("Hints only", "hints"),
                                           item_label("Pop-up", "hints"), item_label("Off", "hints")});
  dialog->select("NotificationsCombo", 0);
}

void Application::open_help() {
  dismiss_paused();
  if (ui_.dialog_count() > 0 && !front_mode_) return;
  if (help_ == nullptr) {
    const platform::ByteSpan bytes = vfs_.read("CurrentLang\\HELP.XML");
    if (bytes.empty()) {
      std::printf("help: no CurrentLang/HELP.XML in the language pack\n");
      return;
    }
    auto parsed = core::game::HelpDocument::parse(platform::as_core_bytes(bytes));
    if (!parsed.ok()) {
      std::printf("help: HELP.XML would not parse\n");
      return;
    }
    help_ = std::make_unique<core::game::HelpDocument>(std::move(parsed.value()));
    help_back_.clear();
    help_forward_.clear();
    help_topic_ = -1;
  }
  core::ui::Dialog* dialog = open_menu(
      "gameini/help/help.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        const core::game::HelpDocument& help = *help_;
        if ((event.kind == Kind::kSelect || event.kind == Kind::kActivate) && event.widget == "HelpList") {
          if (help_topic_ < 0) return;
          const core::game::HelpTopic& topic = help.topics()[static_cast<std::size_t>(help_topic_)];
          if (event.index < 0 || static_cast<std::size_t>(event.index) >= topic.entries.size()) return;
          const std::int32_t target = help.resolve(topic.entries[static_cast<std::size_t>(event.index)].link, help_topic_);
          if (target >= 0) show_help_topic(menu, target, true);
          return;
        }
        if (event.kind != Kind::kCommand) return;
        const auto& topics = help.topics();
        const core::game::HelpTopic* current = help_topic_ >= 0 ? &topics[static_cast<std::size_t>(help_topic_)] : nullptr;
        switch (event.id) {
          case 0x1011: close_menu(); break;
          case 0x1021:  // Backward
            if (!help_back_.empty()) {
              help_forward_.push_back(help_topic_);
              const std::int32_t previous = help_back_.back();
              help_back_.pop_back();
              show_help_topic(menu, previous, false);
            }
            break;
          case 0x1022:  // Forward
            if (!help_forward_.empty()) {
              help_back_.push_back(help_topic_);
              const std::int32_t next = help_forward_.back();
              help_forward_.pop_back();
              show_help_topic(menu, next, false);
            }
            break;
          case 0x1023:  // Up
            if (current != nullptr && current->parent >= 0) show_help_topic(menu, current->parent, true);
            break;
          case 0x1024:
          case 0x1025: {  // Previous, Next: the siblings
            if (current == nullptr || current->parent < 0) break;
            const std::vector<std::int32_t>& siblings = topics[static_cast<std::size_t>(current->parent)].children;
            const auto at = std::find(siblings.begin(), siblings.end(), help_topic_);
            if (at == siblings.end()) break;
            const std::ptrdiff_t index = at - siblings.begin() + (event.id == 0x1025 ? 1 : -1);
            if (index >= 0 && static_cast<std::size_t>(index) < siblings.size()) {
              show_help_topic(menu, siblings[static_cast<std::size_t>(index)], true);
            }
            break;
          }
          case 0x1026: show_help_topic(menu, help.home(), true); break;
          default: break;
        }
      });
  if (dialog == nullptr) return;
  show_help_topic(*dialog, help_topic_ >= 0 ? help_topic_ : help_->home(), false);
}

void Application::show_help_topic(core::ui::Dialog& menu, std::int32_t topic, bool record) {
  if (help_ == nullptr || topic < 0 || static_cast<std::size_t>(topic) >= help_->topics().size()) return;
  if (record && help_topic_ >= 0 && help_topic_ != topic) {
    help_back_.push_back(help_topic_);
    help_forward_.clear();
  }
  help_topic_ = topic;
  const core::game::HelpTopic& shown = help_->topics()[static_cast<std::size_t>(topic)];
  std::vector<std::string> items;
  std::vector<std::string> icons;
  std::vector<std::uint8_t> flags;
  bool any_icon = false;
  for (const core::game::HelpEntry& entry : shown.entries) {
    items.push_back(entry.text.empty() ? " " : entry.text);
    icons.push_back(entry.image);
    if (!entry.image.empty()) any_icon = true;
    std::uint8_t flag = 0;
    if (entry.large) flag |= core::ui::WidgetState::kItemLarge;
    if (entry.centred) flag |= core::ui::WidgetState::kItemCentred;
    flags.push_back(flag);
  }
  menu.set_items("HelpList", std::move(items));
  core::ui::WidgetState& list = menu.content().state("HelpList");
  list.icons = any_icon ? std::move(icons) : std::vector<std::string>{};
  list.item_flags = std::move(flags);
  list.selected = -1;
  menu.set_enabled("BackwardBtn", !help_back_.empty());
  menu.set_enabled("ForwardBtn", !help_forward_.empty());
  menu.set_enabled("UpBtn", shown.parent >= 0);
  menu.set_enabled("PrevBtn", shown.parent >= 0);
  menu.set_enabled("NextBtn", shown.parent >= 0);
}

void Application::open_zoom_map() {
  dismiss_paused();
  if (session_ == nullptr || ui_.dialog_count() > 0) return;
  // The zoom is the map's width over 1024: 8, 16 or 32. Anything else --
  // no match, a size the pack has no zoom for -- composes at sixteen.
  std::uint32_t divisor = 16;
  if (const core::sim::MatchSystem* match = core::sim::match_system_of(session_->world()); match != nullptr) {
    const std::int32_t size = match->rules().map_size;
    if (size == 8192 || size == 16384 || size == 32768) divisor = static_cast<std::uint32_t>(size / 1024);
  }
  if (zoom_ground_.empty() || divisor != zoom_divisor_) {
    zoom_divisor_ = divisor;
    const platform::MapRenderer::ZoomStats drawn = map_.compose_zoom(zoom_divisor_, zoom_ground_);
    std::printf("zoom map:   %ux%u at 1/%u, %u tiles, %u masks, %u pixels of road\n", zoom_ground_.width,
                zoom_ground_.height, zoom_divisor_, drawn.tiles, drawn.masks, drawn.road_pixels);
  }
  if (zoom_ground_.empty()) return;
  // `Const.ini`'s `[zoommap] ToggleTreshold`, the misspelling included, and
  // `MinimapEmptyColor`, the RGB555 word the unexplored ground is painted
  // (0x0060faad, default 0x3dc9 -- the shipped 15817, a dark olive).
  zoom_toggle_threshold_ = 1200;
  zoom_empty_colour_ = 0x3dc9;
  if (install_ != nullptr) {
    if (const auto constants = core::IniDocument::parse(install_->constants()); constants.ok()) {
      const core::SectionIndex section = constants->section("zoommap");
      if (section != core::kNoSection) {
        const std::string_view value = constants->value(section, "ToggleTreshold");
        if (!value.empty()) zoom_toggle_threshold_ = std::atoi(std::string(value).c_str());
        const std::string_view colour = constants->value(section, "MinimapEmptyColor");
        if (!colour.empty()) {
          zoom_empty_colour_ = static_cast<std::uint16_t>(std::atoi(std::string(colour).c_str()));
        }
      }
    }
  }
  // The pointer is the map's -- `zoom_mouse` -- so the dialog's own click
  // has nothing left to do; Escape closes it through `menus_take`.
  core::ui::Dialog* dialog = open_menu(
      "commonini/zoommap.ini", [](const core::ui::DialogEvent&, core::ui::Dialog&) {});
  if (dialog == nullptr) return;
  zoom_open_ = true;
  zoom_dragging_ = false;
  dialog->resize(static_cast<std::int32_t>(zoom_ground_.width), static_cast<std::int32_t>(zoom_ground_.height));
  ui_.place_dialog(dialog, (1024 - static_cast<std::int32_t>(zoom_ground_.width)) / 2,
                   (768 - static_cast<std::int32_t>(zoom_ground_.height)) / 2);
  refresh_zoom_map(*dialog);
}

core::ui::Dialog* Application::zoom_dialog() noexcept {
  if (!zoom_open_) return nullptr;
  for (std::size_t i = 0; i < ui_.dialog_count(); ++i) {
    core::ui::Dialog* dialog = ui_.dialog(i);
    if (dialog == nullptr) continue;
    std::string path = dialog->screen().path;
    for (char& c : path) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (path.find("zoommap") != std::string::npos) return dialog;
  }
  return nullptr;
}

void Application::close_zoom_map() {
  if (!zoom_open_) return;
  close_all_menus();
  zoom_open_ = false;
  zoom_dragging_ = false;
  zoom_press_ticks_ = 0;
}

void Application::zoom_key(const SDL_Event& event) {
  if (event.type == SDL_EVENT_KEY_DOWN) {
    if (event.key.repeat) return;
    if (zoom_open_) {
      close_zoom_map();
      return;
    }
    open_zoom_map();
    zoom_press_ticks_ = zoom_open_ ? SDL_GetTicks() : 0;
    return;
  }
  // The release: a hold longer than the threshold was a peek.
  if (!zoom_open_ || zoom_press_ticks_ == 0) return;
  const std::uint64_t held = SDL_GetTicks() - zoom_press_ticks_;
  zoom_press_ticks_ = 0;
  if (held > static_cast<std::uint64_t>(std::max(0, zoom_toggle_threshold_))) close_zoom_map();
}

void Application::zoom_look(std::int32_t local_x, std::int32_t local_y) {
  const auto divisor = static_cast<std::int32_t>(zoom_divisor_);
  look_x_ = local_x * divisor;
  look_y_ = core::screen_to_world_y(local_y * divisor);
  look_pending_ = true;
  // The frame follows the camera on the next frame rather than the tick.
  zoom_refresh_ticks_ = 0;
}

bool Application::zoom_mouse(const SDL_Event& event, core::ui::Dialog& dialog) {
  const core::ui::Rect rect = dialog.rect();
  const auto inside = [&rect](std::int32_t x, std::int32_t y) {
    return x >= rect.x && y >= rect.y && x < rect.right() && y < rect.bottom();
  };
  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    const auto x = static_cast<std::int32_t>(event.button.x);
    const auto y = static_cast<std::int32_t>(event.button.y);
    if (!inside(x, y)) return true;
    if (event.button.button == SDL_BUTTON_RIGHT) {
      // Go there and close.
      zoom_look(x - rect.x, y - rect.y);
      close_zoom_map();
      return true;
    }
    if (event.button.button == SDL_BUTTON_LEFT) {
      zoom_dragging_ = true;
      zoom_look(x - rect.x, y - rect.y);
      return true;
    }
    return true;
  }
  if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
    if (event.button.button == SDL_BUTTON_LEFT) zoom_dragging_ = false;
    return true;
  }
  if (event.type == SDL_EVENT_MOUSE_MOTION && zoom_dragging_) {
    const auto x = static_cast<std::int32_t>(event.motion.x);
    const auto y = static_cast<std::int32_t>(event.motion.y);
    zoom_look(std::clamp(x - rect.x, 0, rect.width - 1), std::clamp(y - rect.y, 0, rect.height - 1));
    return true;
  }
  return false;
}

void Application::refresh_zoom_map(core::ui::Dialog& menu) {
  const auto kDivisor = static_cast<std::int32_t>(zoom_divisor_);
  zoom_refresh_ticks_ = SDL_GetTicks();
  zoom_view_ = zoom_ground_;
  const core::sim::World& world = session_->world();
  const core::ui::Rect all{0, 0, static_cast<std::int32_t>(zoom_view_.width), static_cast<std::int32_t>(zoom_view_.height)};
  core::ui::Canvas canvas(zoom_view_.width, zoom_view_.height);
  canvas.clear(core::ui::Color{0, 0, 0, 0});
  canvas.blit(zoom_view_, 0, 0, all);
  // The zoom map reads the exploration map and never the light grid: every
  // caller of the light sampler is inside the fog module. So an object is
  // shown wherever the ground is *explored*, seen or not (0x0060fd70: alive,
  // spawned, `IsExplored` at its position; a unit also not dead and, when
  // its owner is not on the local side, not carrying the hidden bit).
  const core::sim::MatchSystem* match = core::sim::match_system_of(world);
  const core::sim::FogSystem* fog = core::sim::fog_system_of(session_->world());
  const bool exploration = !args_.no_fog && !editor_.active && match != nullptr &&
                           match->rules().exploration && fog != nullptr &&
                           core::sim::PlayerTable::is_valid(local_player_);
  const core::sim::ExplorationMap* explored_map = exploration ? &fog->map() : nullptr;
  const auto slot = static_cast<std::int32_t>(local_player_);
  const auto explored = [&](core::sim::Point at) {
    return explored_map == nullptr || explored_map->explored(at, slot);
  };
  // Buildings and map objects as their zoom pictures, units as dots.
  core::ui::ResourceCache pictures([this](std::string_view path) { return platform::as_core_bytes(vfs_.read(path)); });
  for (const core::sim::WorldObject& object : world.objects()) {
    if (object.object == nullptr || object.class_index == core::kNoClass) continue;
    if (object.state.flags.unspawned || object.state.is_held()) continue;
    if (!explored(object.state.position)) continue;
    if (object.state.flags.is_unit) {
      if (object.state.health <= 0) continue;
      if (object.state.flags.hidden && object.state.owner != local_player_ &&
          !world.players().has(object.state.owner, local_player_, core::sim::Relation::share_view)) {
        continue;
      }
    }
    const std::int32_t x = object.state.position.x / kDivisor;
    const std::int32_t y = core::world_to_screen_y(object.state.position.y) / kDivisor;
    const core::Entity* entity = object.object->entity;
    bool drawn = false;
    if (entity != nullptr) {
      // `Buildings/BBarracks/BBarracks.ent.xml` -> `BUILDINGS-BBARRACKS-BBARRACKS`.
      std::string name;
      for (const char c : entity->path()) name.push_back(c == '/' || c == '\\' ? '-' : static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
      const std::size_t dot = name.find('.');
      if (dot != std::string::npos) name.erase(dot);
      core::ui::ImageRef ref;
      ref.path = "MINIMAP/ZOOM" + std::to_string(zoom_divisor_) + "/ENTITIES/" + name + ".BMP";
      if (const core::ui::Image* picture = pictures.image(ref); picture != nullptr && !picture->empty()) {
        canvas.blit(*picture, x - static_cast<std::int32_t>(picture->width) / 2,
                    y - static_cast<std::int32_t>(picture->height) / 2, all);
        drawn = true;
      }
    }
    if (!drawn) {
      const core::Rgb888 rgb = core::expand_x1r5g5b5(world.players().setup(object.state.owner).colour);
      canvas.fill_rect(core::ui::Rect{x - 1, y - 1, 2, 2}, core::ui::Color{rgb.red, rgb.green, rgb.blue, 255});
    }
  }
  // The fog over it, from the exploration map alone (0x006154a0 through the
  // column scan 0x00613b40): a never-seen 1024-cell is painted the empty
  // colour whole, a fully explored one not at all, and a partially explored
  // one by its 32-unit sub-cells, a sub-cell explored iff its nibble is not
  // zero -- the far edge of the 96-unit rim. **There is no explored-but-
  // unseen darkening here**: nothing outside the fog module can see the
  // light grid.
  if (explored_map != nullptr && !explored_map->empty()) {
    using State = core::sim::ExplorationMap::State;
    constexpr std::int32_t kBig = core::sim::ExplorationMap::kCellSize;
    constexpr std::int32_t kFine = core::sim::ExplorationMap::kFineSpacing;
    constexpr std::int32_t kSide = core::sim::ExplorationMap::kFineSide;
    const core::Rgb888 rgb = core::expand_x1r5g5b5(zoom_empty_colour_);
    const core::ui::Color empty{rgb.red, rgb.green, rgb.blue, 255};
    const auto width = static_cast<std::int32_t>(zoom_view_.width);
    const auto height = static_cast<std::int32_t>(zoom_view_.height);
    for (std::int32_t ey = 0; ey < explored_map->cells(); ++ey) {
      const std::int32_t py0 = std::clamp(core::world_to_screen_y(ey * kBig) / kDivisor, 0, height);
      const std::int32_t py1 = std::clamp(core::world_to_screen_y((ey + 1) * kBig) / kDivisor, 0, height);
      for (std::int32_t ex = 0; ex < explored_map->cells(); ++ex) {
        const State state = explored_map->state(ex, ey, slot);
        if (state == State::full || state == State::unwritten) continue;
        const std::int32_t px0 = std::clamp(ex * kBig / kDivisor, 0, width);
        const std::int32_t px1 = std::clamp((ex + 1) * kBig / kDivisor, 0, width);
        if (state == State::never) {
          canvas.fill_rect(core::ui::Rect{px0, py0, px1 - px0, py1 - py0}, empty);
          continue;
        }
        const core::sim::ExplorationMap::FineRecord* record = explored_map->fine(ex, ey, slot);
        if (record == nullptr) continue;
        for (std::int32_t py = py0; py < py1; ++py) {
          const std::int32_t j = std::clamp((core::screen_to_world_y(py * kDivisor) - ey * kBig) / kFine, 0, kSide - 1);
          std::int32_t run = -1;
          for (std::int32_t px = px0; px <= px1; ++px) {
            const std::int32_t i = std::clamp((px * kDivisor - ex * kBig) / kFine, 0, kSide - 1);
            const bool unexplored = px < px1 && record->values[static_cast<std::size_t>(j) * kSide + i] == 0;
            if (unexplored && run < 0) run = px;
            if (!unexplored && run >= 0) {
              canvas.fill_rect(core::ui::Rect{run, py, px - run, 1}, empty);
              run = -1;
            }
          }
        }
      }
    }
  }
  // The camera's frame.
  const core::ui::Color frame{255, 255, 255, 255};
  const core::ui::Rect view{camera_.x / kDivisor, camera_.y / kDivisor, camera_.width / kDivisor, camera_.height / kDivisor};
  canvas.fill_rect(core::ui::Rect{view.x, view.y, view.width, 1}, frame);
  canvas.fill_rect(core::ui::Rect{view.x, view.bottom() - 1, view.width, 1}, frame);
  canvas.fill_rect(core::ui::Rect{view.x, view.y, 1, view.height}, frame);
  canvas.fill_rect(core::ui::Rect{view.right() - 1, view.y, 1, view.height}, frame);
  zoom_view_.rgba.assign(canvas.pixels().begin(), canvas.pixels().end());
  menu.content().backdrop = &zoom_view_;
  menu.touch();
}

void Application::select_party() {
  if (session_ == nullptr) return;
  core::sim::World& world = session_->world();
  std::vector<core::ObjectId> party;
  for (const core::sim::WorldObject& object : world.objects()) {
    if (object.class_index == core::kNoClass || object.state.owner != local_player_) continue;
    if (!object.state.flags.in_party || object.state.flags.unspawned) continue;
    if (!core::sim::is_selectable(world, object.id)) continue;
    party.push_back(object.id);
  }
  if (party.empty()) {
    std::printf("party: none\n");
    return;
  }
  core::sim::Selection& selection = session_->selections().player(local_player_);
  selection.clear();
  for (const core::ObjectId id : party) selection.add(id);
  session_->selections().note_selection_changed(local_player_, session_->scheduler().now());
  play_select_sounds({});
  std::printf("party: %zu selected\n", party.size());
  bars_dirty_ = true;
}

/// `DIPLOMACY.INI` (0x006cbbf0 builds it, 0x006cb8f0 handles it).
///
/// **Rows.** The other players of the description's `real` list, in slot
/// order, at most the file's seven: 0x006cbca7 walks the sixteen records and
/// keeps each whose `+0x14` is set, skipping the local player -- the field
/// and the list `statistics_rows` reads. Unused rows are hidden (0x006cbe20,
/// ids `n0..nA`). With no row at all the screen closes itself before it is
/// seen (0x006cc04c). **Not modelled:** the original also pulls the legend,
/// OK, Cancel and the frame up by 40 pixels a missing row (0x006cbe54); here
/// the hidden rows leave their gap.
///
/// **The toggles show the relation *on* as row 0.** 0x006cb950 sets each of
/// the local player's four to `~bit & 1` of its word for the row's player
/// (cease fire bit 0, shared vision bit 4, shared support bit 2, shared
/// control bit 5), and 0x006cb500 the other side's four from the row's word
/// for the local player, which are then made inert (style 0x200, 0x006cbde9).
/// The art agrees: `cf.bmp`'s first row is the crossed-out sword, the others'
/// the plain picture. The Allied victory box is row 1 when the local
/// player's record `+0x64` -- `playerdata/@allied` -- is set.
///
/// **A press on a toggle does nothing but flip it** -- the handler has no
/// case for it. **OK** (0x1000, 0x006cb680) reads the rows back into a word
/// per player, and for each that differs from the word in the table posts a
/// `diplomacy` order; one more if the box differs from the flag; then closes.
/// **Cancel** (0x1001) closes. So nothing changes until the order is applied
/// on the next turn, on every peer, and a peer's change reaches an open
/// screen's other column when it is (0x006cb8c0, `refresh_diplomacy`).
/// A `UIHolder` portrait, clicked (`CVXUIHolder`'s cell handler, 0x006d3550;
/// the strip's dispatcher, 0x006ee27a, gives a left click code 0 plain, 1
/// with Ctrl, 3 with Shift, 4 with Alt, and a double click 8, all of which
/// take this path).
///
/// The strip is a garrison, a lone hero's army, or a multiple selection's
/// members and their heroes' armies, folded into one portrait per hero, per
/// class and owner, and for every sentry: `sim/infobar.hpp` has the rule.
///
/// **It selects, in place.** Nothing is posted and nothing leaves: the cell
/// is marked (Ctrl: one more of its units, 0x006d3aa8; otherwise its mark
/// flips, and a mark means all of them, 0x10000) and, unless Shift is held,
/// 0x006d3250 clears the selection and adds the marked cells' units through
/// the selection's own insert (0x005e7d80, the one `Select` uses) -- units
/// still inside their holder, or out in the field with their hero -- then
/// clears the marks. A hero's cell gives the hero alone; the army folded
/// into it stays out (0x006d342b). With Shift the marks gather and are
/// applied when Shift is released (0x006d34f0). `GROUP_UNITSOUT.VS` shows a
/// held unit being selected the same way before it is ordered out.
///
/// **The gate:** the selection's head must be owned by a player whose word
/// for the local player has cease fire or shared control (0x006d35d8 tests
/// `& 0x21`), which the local player's own word always has.
///
/// **Not modelled, labelled:** 0x006d3250 walks the list in list order and
/// takes each marked cell's units as it meets them; this takes them cell by
/// cell, which gives the same head and differs only in the order of a
/// selection gathered from several cells. A right click (code 7) opens the
/// unit's help, and with Shift posts an `attach` command; neither is here.
void Application::holder_click(std::size_t index, bool shift, bool ctrl) {
  if (session_ == nullptr || index >= holder_cells_.size()) return;
  core::sim::World& world = session_->world();
  const core::sim::Selection& selection = session_->selections().player(local_player_);
  if (selection.empty()) return;
  const core::sim::WorldObject* head = world.find(selection.ids().front());
  if (head == nullptr || !core::sim::PlayerTable::is_valid(head->state.owner) ||
      !core::sim::PlayerTable::is_valid(local_player_) ||
      (world.players().relation_word(head->state.owner, local_player_) & 0x21u) == 0) {
    return;
  }
  constexpr std::size_t kAll = 0x10000;
  const std::uint64_t key = holder_cells_[index].key;
  const auto marked = std::find_if(holder_marks_.begin(), holder_marks_.end(),
                                   [key](const auto& mark) { return mark.first == key; });
  if (ctrl) {
    if (marked == holder_marks_.end()) {
      holder_marks_.emplace_back(key, 1);
    } else {
      ++marked->second;
    }
  } else if (marked == holder_marks_.end()) {
    holder_marks_.emplace_back(key, kAll);
  } else {
    holder_marks_.erase(marked);
  }
  bars_dirty_ = true;
  if (!shift) apply_holder_marks();
}

/// **The gate** (0x006bfc20, before anything is posted): a first selected
/// object that is a building, whose running command is neither `idle` nor
/// `broken`, and whose owner grants the local player shared control (the
/// relation word's bit 0x20, `is_commandable`'s test). **The click**: released
/// over the cell it went down on, with Ctrl it cancels that entry and every
/// entry behind it -- one `CVXCmdCancelCmd` each, from the last back to the
/// clicked one (0x006bfebc) -- and otherwise, Shift or Alt or neither, that
/// entry alone (0x006bfee9). **Not here, labelled:** a right click on a cell
/// opens the trained unit's help page (0x006bfd18).
void Application::queue_click(std::size_t index, bool ctrl) {
  if (session_ == nullptr || index >= queue_cells_.size()) return;
  core::sim::World& world = session_->world();
  const core::sim::Selection& selection = session_->selections().player(local_player_);
  if (selection.empty()) return;
  const core::ObjectId building = selection.ids().front();
  const core::sim::WorldObject* slot = world.find(building);
  if (slot == nullptr || !slot->state.flags.is_building) return;
  const core::sim::CommandSystem* commands = core::sim::command_system(world);
  const core::sim::CommandQueue* queue = commands == nullptr ? nullptr : commands->find(building);
  const core::sim::Command* running = queue == nullptr ? nullptr : queue->running();
  if (running == nullptr) return;
  const auto is = [](std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
             return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
  };
  if (is(running->verb, "idle") || is(running->verb, "broken")) return;
  if (!core::sim::is_commandable(world, building, local_player_)) return;
  core::sim::OrderOutbox* outbox = session_->host_context().outbox;
  if (outbox == nullptr) return;
  const std::size_t last = ctrl ? queue_cells_.size() - 1 : index;
  for (std::size_t i = last + 1; i-- > index;) {
    core::sim::NetOrder order;
    order.kind = core::sim::NetOrderKind::cancel_command;
    order.issuer = local_player_;
    order.target.object = building;
    order.command_id = queue_cells_[i];
    outbox->post(std::move(order));
    queue_cancelled_.push_back(queue_cells_[i]);
    std::printf("queue:        cancel of command %u (cell %zu) on object %u posted\n", queue_cells_[i], i,
                static_cast<unsigned>(building));
  }
  std::fflush(stdout);
  bars_dirty_ = true;
}

void Application::apply_holder_marks() {
  if (session_ == nullptr) return;
  std::vector<std::pair<std::uint64_t, std::size_t>> marks;
  marks.swap(holder_marks_);
  bars_dirty_ = true;
  if (marks.empty()) return;
  core::sim::World& world = session_->world();
  core::sim::Selection& selection = session_->selections().player(local_player_);
  // The selection is cleared at the first unit a marked cell gives, so a
  // mark on a cell that has none left changes nothing (0x006d3446).
  bool cleared = false;
  std::size_t added = 0;
  std::size_t offered = 0;
  for (const HolderCell& cell : holder_cells_) {
    const auto mark = std::find_if(marks.begin(), marks.end(),
                                   [&cell](const auto& m) { return m.first == cell.key; });
    if (mark == marks.end()) continue;
    std::size_t left = mark->second;
    for (const core::ObjectId id : cell.objects) {
      if (left == 0) break;
      --left;
      ++offered;
      if (!cleared) {
        selection.clear();
        cleared = true;
      }
      // The insert's own gates, as `Select` has them: alive, not `noselect`.
      const core::sim::WorldObject* unit = world.find(id);
      if (unit == nullptr || unit->state.health <= 0 || unit->state.flags.noselect) continue;
      selection.add(id);
      ++added;
    }
  }
  if (!cleared) return;
  session_->selections().note_selection_changed(local_player_, session_->scheduler().now());
  // Each goes in through the same insert as a click's (0x006d3469 calls
  // 0x005e7d80), so with its select sound.
  play_select_sounds({});
  std::printf("holder: %zu of %zu selected in place:", added, offered);
  for (const core::ObjectId id : selection.ids()) std::printf(" %u", static_cast<unsigned>(id));
  std::printf("\n");
  std::fflush(stdout);
}

void Application::open_diplomacy_menu() {
  dismiss_paused();
  if (session_ == nullptr || ui_.dialog_count() > 0) return;
  const core::sim::PlayerTable& players = session_->world().players();
  std::vector<core::PlayerId> others;
  for (const core::PlayerId id : statistics_rows()) {
    if (id != local_player_ && others.size() < 7) others.push_back(id);
  }
  if (others.empty()) {
    std::printf("diplomacy: no other player\n");
    return;
  }
  diplomacy_rows_ = others;
  std::printf("diplomacy:    a row for player");
  for (const core::PlayerId id : others) std::printf(" %u", static_cast<unsigned>(id));
  std::printf("\n");
  core::ui::Dialog* dialog = open_menu(
      "menuini/diplomacy.ini", [this, others](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        if (event.kind != core::ui::DialogEvent::Kind::kCommand) return;
        if (event.id == 0x1000) {  // OK
          post_diplomacy(menu, others);
          close_menu();
        } else if (event.id == 0x1001) {  // Cancel
          close_menu();
        }
      });
  if (dialog == nullptr) return;
  for (std::size_t row = 0; row < 7; ++row) {
    const std::string prefix = "Pl" + std::to_string(row + 1) + ".";
    const bool used = row < others.size();
    for (const char* name : {"Frame", "Color", "Name", "CF1", "SV1", "SS1", "SC1", "CF2", "SV2", "SS2", "SC2"}) {
      dialog->set_hidden(prefix + name, !used);
    }
    if (!used) continue;
    const core::PlayerId other = others[row];
    const core::sim::PlayerSetup& setup = players.setup(other);
    const core::Rgb888 rgb = core::expand_x1r5g5b5(setup.colour);
    core::ui::WidgetState& colour = dialog->content().state(prefix + "Color");
    colour.has_color = true;
    colour.color = core::ui::Color{rgb.red, rgb.green, rgb.blue, 255};
    dialog->set_text(prefix + "Name", seat_name(other, setup));
    for (const DiplomacyColumn& column : kDiplomacyColumns) {
      dialog->set_row(prefix + column.suffix + "1", players.has(local_player_, other, column.relation) ? 0 : 1);
      dialog->set_enabled(prefix + column.suffix + "2", false);
    }
  }
  dialog->set_row("Allied", players.setup(local_player_).allied_flag ? 1 : 0);
  refresh_diplomacy(*dialog);
}

void Application::refresh_diplomacy(core::ui::Dialog& dialog) {
  if (session_ == nullptr) return;
  const core::sim::PlayerTable& players = session_->world().players();
  for (std::size_t row = 0; row < diplomacy_rows_.size() && row < 7; ++row) {
    const std::string prefix = "Pl" + std::to_string(row + 1) + ".";
    for (const DiplomacyColumn& column : kDiplomacyColumns) {
      const std::string name = prefix + column.suffix + "2";
      const std::int32_t wanted = players.has(diplomacy_rows_[row], local_player_, column.relation) ? 0 : 1;
      const core::ui::WidgetState* state = dialog.content().state_of(name);
      if (state == nullptr || state->row != wanted) dialog.set_row(name, wanted);
    }
  }
}

void Application::post_diplomacy(const core::ui::Dialog& menu, const std::vector<core::PlayerId>& others) {
  if (session_ == nullptr) return;
  core::sim::OrderOutbox* outbox = session_->host_context().outbox;
  if (outbox == nullptr) return;
  const core::sim::PlayerTable& players = session_->world().players();
  const auto row_of = [&menu](const std::string& name) {
    const core::ui::WidgetState* state = menu.content().state_of(name);
    return state == nullptr ? 0 : state->row;
  };
  for (std::size_t row = 0; row < others.size() && row < 7; ++row) {
    const std::string prefix = "Pl" + std::to_string(row + 1) + ".";
    std::uint32_t word = 0;
    for (const DiplomacyColumn& column : kDiplomacyColumns) {
      // Row 0 is the relation on (0x006cb738: `+0x7e == 0`).
      if (row_of(prefix + column.suffix + "1") == 0) {
        word |= 1u << core::sim::kRelationBits[static_cast<std::size_t>(column.relation)];
      }
    }
    // The whole word is compared, as 0x006cb77f compares it: a word with a
    // bit the screen does not show is rewritten without it.
    if (word == players.relation_word(local_player_, others[row])) continue;
    core::sim::NetOrder order;
    order.kind = core::sim::NetOrderKind::diplomacy;
    order.issuer = local_player_;
    order.other = others[row];
    order.relations = word;
    outbox->post(std::move(order));
  }
  const bool allied = row_of("Allied") == 1;
  if (allied != players.setup(local_player_).allied_flag) {
    core::sim::NetOrder order;
    order.kind = core::sim::NetOrderKind::diplomacy;
    order.issuer = local_player_;
    order.other = core::kNoPlayer;
    order.allied = allied;
    outbox->post(std::move(order));
  }
}

/// The Statistics screen (`STATISTICS.INI`, built at 0x006fc800). A row per
/// player still in the game's table -- the first eight, the rest of the
/// eight rows hidden -- with the colour square and the name; four tabs, one
/// fill each (0x006fc260 *Resources*, 0x006fae20 *Gold production*,
/// 0x006fbbb0 *Units*, 0x006fb4e0 *Scores*), each writing the title, the
/// three column headings (ids 1..3) and the cells (0x006faaa0: the value as
/// `%d`, its share `(%d%%)` of the column's maximum, the maximum's own row
/// in yellow); *Resources* has two columns and hides the third. The time is
/// `Game time: hh:mm:ss` of the game clock (0x006fabd0). Which players are
/// rows is `statistics_rows`.
///
/// Opened from the end-game screen it *replaces* that screen. The end menu's
/// `0x1005` (0x006ce1d5) has the menu's owner post 0x17050016, which destroys
/// the end menu (0x005e8636), before 0x006fcb40 opens this one; Load, Save
/// replay, Options and Close post the same message first. This Close
/// (0x006fcb12) closes this and nothing else -- unlike Options, whose close
/// brings the end menu back when the end menu opened it (0x006e7bac) -- and
/// F10 brings it back (`open_game_menu`). `STATISTICS.INI` itself asks for nothing
/// else: `Style = TRANSPARENT`, no `MODAL` (only `GAMEMENU.INI` among the
/// menus has one), a `Move` strip to drag it by, `Esc = 0.close`.
void Application::open_statistics_menu() {
  if (session_ == nullptr) return;
  core::ui::Dialog* dialog = open_menu(
      "menuini/statistics.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        if (event.kind != core::ui::DialogEvent::Kind::kCommand) return;
        if (event.id >= 4 && event.id <= 7) {
          statistics_tab_ = static_cast<int>(event.id - 4);
          fill_statistics(menu, statistics_tab_);
          return;
        }
        if (event.id == 8) close_menu();
      });
  if (dialog == nullptr) return;
  fill_statistics(*dialog, statistics_tab_);
}

/// The rows are the game description's *real* players. 0x006fc8b0 walks the
/// sixteen `playerdata` records of the description (`[0xa87c48] + 0x9c`, 0xc0
/// each) and takes the first eight whose `+0x14` is set; the description's
/// binary serialiser names that field `real` (0x00567686, beside `race`,
/// `control`, `bonusindex`). Nothing loaded from a map sets it -- the
/// constructor clears it (0x00567913) and `player<i>.xml`'s reader never
/// touches it -- and the players' screen's Start sets it (0x006dc039), as
/// the automatch's does (0x00702d86): over the first **eight** records only,
/// a record whose map `control` is `Computer` is skipped whole (0x006dbdf3),
/// and the rest are set real unless their row was closed. So a seat the map
/// declares `Computer` -- Crossroads' 4 to 13, with no holdings -- is never
/// a row, and neither is anything past the eighth. `Military 10` on such a
/// row was the rating of nothing: 0x0056a260 is (kill_healths/2 +
/// damage_inflicted + 1000) * 100 / (die_healths/2 + damage_taken + 10000),
/// which is 10 on all-zero counters -- not a starting value.
///
/// Here: a slot below eight whose `player<i>.xml` says `Human` or `Both`
/// and that takes part (a closed row does not). **Reading, labelled:** a
/// match that did not come through the players' screen -- a mission, an
/// adventure, `--play` -- is given the rule the screen applies; how the
/// original fills `real` on those paths was not read. `Rescue` parses as
/// disabled here and is not a row.
std::vector<core::PlayerId> Application::statistics_rows() const {
  std::vector<core::PlayerId> rows;
  if (session_ == nullptr) return rows;
  const core::sim::MatchSystem* match = core::sim::match_system_of(session_->world());
  if (match == nullptr) return rows;
  for (std::size_t i = 0; i < 8 && i < core::sim::kPlayerCount; ++i) {
    const auto id = static_cast<core::PlayerId>(i);
    if (!match->player(id).participates) continue;
    // The map's own word, not the setup's: setup turns `Both` into
    // `computer`, which would make it the same as a `Computer` seat.
    const auto document = container_.read("player" + std::to_string(i) + ".xml");
    if (document.empty()) continue;
    const auto slot = core::PlayerSlot::parse(document);
    if (!slot.ok()) continue;
    const core::sim::PlayerControl control = core::sim::parse_player_control(slot->control);
    if (control != core::sim::PlayerControl::human && control != core::sim::PlayerControl::both) continue;
    rows.push_back(id);
  }
  return rows;
}

void Application::fill_statistics(core::ui::Dialog& menu, int tab) {
  const core::sim::World& world = session_->world();
  const core::sim::MatchSystem* match = core::sim::match_system_of(world);
  if (match == nullptr) return;
  // The rows.
  const std::vector<core::PlayerId> rows = statistics_rows();
  static const char* const kCells[] = {"Color", "Name", "text1", "text2", "text3", "per1", "per2", "per3"};
  for (std::size_t row = 0; row < 8; ++row) {
    const std::string prefix = "pl" + std::to_string(row) + ".";
    const bool used = row < rows.size();
    for (const char* cell : kCells) menu.set_hidden(prefix + cell, !used);
    if (!used) continue;
    const core::sim::PlayerSetup& setup = world.players().setup(rows[row]);
    const core::Rgb888 rgb = core::expand_x1r5g5b5(setup.colour);
    core::ui::WidgetState& square = menu.content().state(prefix + "Color");
    square.has_color = true;
    square.color = core::ui::Color{rgb.red, rgb.green, rgb.blue, 255};
    menu.set_text(prefix + "Name", seat_name(rows[row], setup));
  }
  // The tab: its title, its headings, and the three numbers of each row.
  struct Column {
    const char* heading;
    std::int32_t (*value)(const core::sim::MatchSystem&, const core::sim::World&, core::PlayerId);
  };
  using Counters = core::sim::PlayerScoreCounters;
  const Column resources[] = {
      {"Gold spent", [](const core::sim::MatchSystem& m, const core::sim::World&, core::PlayerId p) { return m.score(p).gold; }},
      {"Food spent", [](const core::sim::MatchSystem& m, const core::sim::World&, core::PlayerId p) { return m.score(p).food; }},
      {nullptr, nullptr}};
  // *Captured* is the warehouse gold a settlement carries when it changes
  // hands, and it is booked on both sides: 0x005c4f08 takes it from the old
  // owner's `+0xac`, 0x005c4f29 gives it to the new one's. A player whose
  // town was taken reads negative -- Crossroads' p0 at -8,368 -- and with the
  // column's maximum found from nought upward (0x006fb25f) and the share as
  // value * 100 / maximum (0x006fab50, signed), that is `(-100%)` in the
  // original too, not a slip here. Both writes go through 0x0051cce0, which
  // books into a spare record the screen never reads (its reader, 0x0051cd20,
  // always takes the player's own) unless the player's slot word `+0x290` is
  // 1, *still playing*. The match does not keep that word. **Reading,
  // labelled:** on Crossroads it changes nothing, because the town changes
  // hands first and `1 ELIMINATION.VS` puts p0 out after.
  const Column gold[] = {
      {"From taxes", [](const core::sim::MatchSystem& m, const core::sim::World&, core::PlayerId p) { return m.score(p).gold_townhall; }},
      {"Other", [](const core::sim::MatchSystem& m, const core::sim::World&, core::PlayerId p) { return m.score(p).gold_outpost; }},
      {"Captured", [](const core::sim::MatchSystem& m, const core::sim::World&, core::PlayerId p) { return m.score(p).gold_captured; }}};
  const Column units[] = {
      {"Killed", [](const core::sim::MatchSystem& m, const core::sim::World&, core::PlayerId p) { return m.score(p).units_killed; }},
      {"Lost", [](const core::sim::MatchSystem& m, const core::sim::World&, core::PlayerId p) { return m.score(p).units_lost; }},
      {"Maximum", [](const core::sim::MatchSystem& m, const core::sim::World&, core::PlayerId p) { return m.score(p).units_max; }}};
  // *Scores*: the military rating (0x0056a260), *Development* as gold spent
  // over 500 (0x0056a2a0), and *Overall* as that plus the power census times
  // the rating over 100 (0x0056a970) -- the screen's own arithmetic, which
  // divides by 500 where `GetTeamOverallScore` divides by 1000.
  const Column scores[] = {
      {"Military", [](const core::sim::MatchSystem& m, const core::sim::World&, core::PlayerId p) { return core::sim::military_rating(m.score(p)); }},
      {"Development", [](const core::sim::MatchSystem& m, const core::sim::World&, core::PlayerId p) {
         return static_cast<std::int32_t>(static_cast<std::uint32_t>(m.score(p).gold) / 500u);
       }},
      {"Overall", [](const core::sim::MatchSystem& m, const core::sim::World& w, core::PlayerId p) {
         const Counters& c = m.score(p);
         const std::int32_t development = static_cast<std::int32_t>(static_cast<std::uint32_t>(c.gold) / 500u);
         const std::int64_t power = m.power_score(const_cast<core::sim::World&>(w), p);
         return static_cast<std::int32_t>(development + power * core::sim::military_rating(c) / 100);
       }}};
  static const char* const kTitles[] = {"Resources", "Gold production", "Units", "Scores"};
  const Column* columns = tab == 1 ? gold : tab == 2 ? units : tab == 3 ? scores : resources;
  menu.set_text("Text", item_label(kTitles[std::clamp(tab, 0, 3)], ""));
  // What the screen shows, a line a row on the console, for a script to read.
  std::vector<std::string> said(rows.size());
  for (std::size_t row = 0; row < rows.size(); ++row) {
    said[row] = std::string("statistics: ") + kTitles[std::clamp(tab, 0, 3)] + " | " +
                seat_name(rows[row], world.players().setup(rows[row]));
  }
  for (int c = 0; c < 3; ++c) {
    const Column& column = columns[c];
    const std::string heading = "0.text" + std::to_string(c + 1);
    menu.set_hidden(heading, column.heading == nullptr);
    if (column.heading != nullptr) menu.set_text(heading, item_label(column.heading, ""));
    std::int32_t maximum = 0;
    std::vector<std::int32_t> values;
    for (const core::PlayerId player : rows) {
      values.push_back(column.value != nullptr ? column.value(*match, world, player) : 0);
      maximum = std::max(maximum, values.back());
    }
    // 0x006fb2f1: a column whose maximum is nought shares against a hundred.
    const std::int32_t divisor = maximum == 0 ? 100 : maximum;
    for (std::size_t row = 0; row < rows.size(); ++row) {
      const std::string prefix = "pl" + std::to_string(row) + ".";
      const std::string value = prefix + "text" + std::to_string(c + 1);
      const std::string share = prefix + "per" + std::to_string(c + 1);
      if (column.heading == nullptr) {
        menu.set_hidden(value, true);
        menu.set_hidden(share, true);
        continue;
      }
      menu.set_text(value, std::to_string(values[row]));
      core::ui::WidgetState& ink = menu.content().state(value);
      ink.has_ink = true;
      ink.ink = values[row] == maximum ? core::ui::Color{255, 255, 0, 255} : core::ui::Color{255, 255, 255, 255};
      const std::string percent = "(" + std::to_string(static_cast<std::int64_t>(values[row]) * 100 / divisor) + "%)";
      menu.set_text(share, percent);
      said[row] += " | " + std::to_string(values[row]) + " " + percent;
    }
  }
  for (const std::string& line : said) std::printf("%s\n", line.c_str());
  std::fflush(stdout);
  // The four tab buttons show the current one pressed, and the time.
  for (int t = 0; t < 4; ++t) {
    static const char* const kButtons[] = {"0.resources", "0.gold", "0.units", "0.scores"};
    menu.set_row(kButtons[t], t == tab ? 1 : 0);
  }
  const std::int64_t seconds = static_cast<std::int64_t>(session_->scheduler().now()) / 1000;
  char clock[32];
  std::snprintf(clock, sizeof clock, "%02lld:%02lld:%02lld", static_cast<long long>(seconds / 3600),
                static_cast<long long>(seconds / 60 % 60), static_cast<long long>(seconds % 60));
  menu.set_text("0.time", item_label("Game time", "") + ": " + clock);
}

void Application::open_endgame_menu(bool won) {
  dismiss_paused();
  if (session_ == nullptr) return;
  // The end of the match is over whatever was open. This refused to open
  // over any dialog, and the zoom map is one that does not stop the clock:
  // Crossroads decided with it open showed no end-game screen, and the
  // decided match ran on under it (to turn 2,729 before anything noticed).
  // So did any menu in a networked match, whose clock no menu stops.
  close_zoom_map();
  close_all_menus();
  core::ui::Dialog* dialog = open_menu(
      "menuini/endgamemenu.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog&) {
        if (event.kind != core::ui::DialogEvent::Kind::kCommand) return;
        switch (event.id) {
          case 0x1001: open_load_menu(); break;
          case 0x1003: open_options_menu(); break;
          case 0x1005:
            // Statistics takes the end menu's place rather than stacking over
            // it: see `open_statistics_menu`.
            close_menu();
            open_statistics_menu();
            break;
          case 0x1004:
            close_all_menus();
            if (!restart_play()) quit_requested_ = true;
            break;
          case 0x1006:
            close_all_menus();
            if (args_.from_front) {
              return_to_front();
            } else {
              quit_requested_ = true;
            }
            break;
          case 0x1007:
          case 0x100B: close_menu(); break;
          default: break;
        }
      });
  if (dialog == nullptr) return;
  // The words are `WINLOSEDLG.INI`'s, keyed there in the table.
  const core::game::TranslationTable* table = translations_.get();
  const auto word = [table](std::string_view text, std::string_view widget) {
    if (table == nullptr) return std::string(text);
    return std::string(table->translate_in_context(
        text, core::ui::translation_context("menuini/winlosedlg.ini", widget, "Text")));
  };
  dialog->set_text("Heading", won ? word("You Win!", "WinText") : word("You lose", "LoseText"));
  dialog->set_text("Reason", "");
  dialog->set_row("WinLose", won ? 0 : 2);
  dialog->set_enabled("SaveDemo", false);
  // `Quit2` (Continue) shares Quit's rectangle; Close is the way to play on.
  for (const char* name : {"RatingText", "LevelText", "Quit2"}) dialog->set_hidden(name, true);
}

/// `NOTES.INI`: the active notes in `NotesList` (`0x1002`), each its icon
/// and its title over its text; `View location` (`0x1000`) puts the camera
/// on the selected note's pin; Close `0x1001`. `NoActiveNotes` (`0x1010`)
/// shows when the board is empty.
void Application::open_notes_menu() {
  dismiss_paused();
  if (session_ == nullptr) return;
  const core::sim::CampaignSystem* campaign = core::sim::campaign_system_of(session_->world());
  if (campaign == nullptr) return;
  std::vector<core::sim::Point> pins;
  std::vector<std::string> items;
  std::vector<std::string> icons;
  for (const std::string& id : campaign->note_board().active_notes()) {
    const core::sim::NoteDefinition* note = campaign->notes().find(id);
    if (note == nullptr) continue;
    const core::game::TranslationTable* table = session_->host_context().translations;
    const auto translate = [table](const std::string& key) {
      return table == nullptr ? key : std::string(table->translate(key));
    };
    std::string item = translate(note->title.empty() ? note->id : note->title);
    if (!note->text.empty()) {
      item += '\n';
      item += translate(note->text);
    }
    items.push_back(std::move(item));
    // 75 of the 82 shipped declarations carry no icon and the other seven
    // name `noteicons/triangle.bmp`, which no pack holds; the editor's own
    // table lists seven others. So the shipped notes show no icon, and the
    // list keeps the `TextOffs` column for one all the same.
    icons.push_back(note->icon);
    pins.push_back(note->location);
  }
  core::ui::Dialog* dialog = open_menu(
      "menuini/notes.ini",
      [this, pins](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        if (event.kind != core::ui::DialogEvent::Kind::kCommand) return;
        if (event.id == 0x1001) {
          close_menu();
        } else if (event.id == 0x1000) {
          const std::int32_t index = menu.selected("NotesList");
          if (index < 0 || static_cast<std::size_t>(index) >= pins.size()) return;
          const core::sim::Point pin = pins[static_cast<std::size_t>(index)];
          if (pin.x < 0 || pin.y < 0) return;
          look_x_ = pin.x;
          look_y_ = pin.y;
          look_pending_ = true;
          close_menu();
        }
      });
  if (dialog == nullptr) return;
  dialog->set_hidden("NoActiveNotes", !items.empty());
  dialog->set_items("NotesList", std::move(items));
  dialog->content().state("NotesList").icons = std::move(icons);
  if (!pins.empty()) dialog->select("NotesList", 0);
}


// --------------------------------------------------------------------------
// the front
// --------------------------------------------------------------------------

void Application::mount_language() {
  // `Settings.ini`'s `[Language] Default=` names the pack, unless
  // `--language` does. The pack holds `CurrentLang/`: the menu backgrounds,
  // the tips, and `TRANSLATION.LOC.XML`. Mounted at the root, because its
  // names are fully qualified.
  std::string language = args_.language;
  if (language.empty()) {
    language = "English";
    std::vector<std::byte> settings;
    if (std::ifstream file(vfs_.root() / "Settings.ini", std::ios::binary); file) {
      std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      settings.resize(text.size());
      std::memcpy(settings.data(), text.data(), text.size());
    }
    if (!settings.empty()) {
      if (auto ini = core::IniDocument::parse(settings); ini.ok()) {
        if (const core::SectionIndex section = ini->section("Language"); section != core::kNoSection) {
          for (const core::IniEntry& entry : ini->entries_of(section)) {
            if (entry.has_key && entry.key == "Default" && !entry.value.empty()) {
              language.assign(entry.value);
            }
          }
        }
      }
    }
  }
  if (language == "none") return;
  language_ = language;
  auto pack = std::make_unique<platform::Pack>();
  std::filesystem::path pack_path = vfs_.root() / "local" / (language + ".pak");
  if (!std::filesystem::exists(pack_path)) {
    // Case-blind on a case-sensitive filesystem: `Italian` names `italian.pak`.
    std::error_code ignored;
    for (const auto& entry : std::filesystem::directory_iterator(vfs_.root() / "local", ignored)) {
      std::string name = entry.path().filename().string();
      std::string wanted = language + ".pak";
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      for (char& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name == wanted) pack_path = entry.path();
    }
  }
  std::string error;
  if (!pack->open(pack_path, &error)) {
    std::printf("language:     %s -- no pack at %s\n", language.c_str(), pack_path.string().c_str());
    return;
  }
  vfs_.mount_pack("", std::move(pack));
  const platform::ByteSpan table = vfs_.read("CurrentLang\\TRANSLATION.LOC.XML");
  if (!table.empty()) {
    const std::span<const std::byte> core_bytes = platform::as_core_bytes(table);
    translation_bytes_.assign(core_bytes.begin(), core_bytes.end());
    if (auto parsed = core::game::TranslationTable::parse(translation_bytes_); parsed.ok()) {
      translations_ = std::make_unique<core::game::TranslationTable>(std::move(parsed.value()));
    }
  }
  std::printf("language:     %s (%s), %zu translations\n", language.c_str(),
              pack_path.string().c_str(), translations_ != nullptr ? translations_->size() : 0u);
}

core::game::TranslationTable Application::container_table(const gamedata::MapContainer& container) const {
  core::game::TranslationTable table;
  for (const std::vector<std::byte>& document : gamedata::read_localisation(container, language_)) {
    if (auto parsed = core::game::TranslationTable::parse(document); parsed.ok()) {
      (void)table.merge(parsed.value());
    }
  }
  return table;
}

std::string Application::localised(const core::game::TranslationTable& table, std::string_view text) const {
  // The container's table first (its keys are its own English), then the
  // language pack's; a miss is the text, which is English already.
  if (table.contains(text)) return std::string(table.translate(text));
  if (translations_ != nullptr && translations_->contains(text)) return std::string(translations_->translate(text));
  return core::ui::cp1252_from_utf8(text);
}

std::string Application::item_label(std::string_view text, std::string_view context) const {
  if (translations_ == nullptr) return std::string(text);
  return std::string(translations_->translate_in_context(text, context));
}

std::string_view Application::race_display_name(std::int32_t race) noexcept {
  // The table's `@race` keys, which are the display names rather than the
  // class-prefix spellings `race_to_name` answers (`RepublicanRome`).
  switch (race) {
    case 0: return "Gaul";
    case 1: return "Republican Rome";
    case 2: return "Carthage";
    case 3: return "Iberia";
    case 4: return "Imperial Rome";
    case 5: return "Britain";
    case 6: return "Egypt";
    case 7: return "Germany";
    default: return "unknown";
  }
}

std::function<std::string_view(std::string_view)> Application::translator() const {
  const core::game::TranslationTable* table = translations_.get();
  if (table == nullptr) return {};
  return [table](std::string_view key) { return table->translate(key); };
}

bool Application::start_front() {
  std::string error;
  if (!ui_.create(vfs_, window_.device(), platform::render_target_format(), &error)) {
    std::fprintf(stderr, "interface: %s\n", error.c_str());
    return false;
  }
  // The adventures' pictures sit beside their containers, outside every pack.
  vfs_.mount_directory("Adventures\\", vfs_.root() / "Adventures");
  vfs_.mount_directory("Conquests\\", vfs_.root() / "Conquests");
  vfs_.mount_directory("Scenarios\\", vfs_.root() / "Scenarios");

  front_mode_ = true;
  // Entering the menus asks for their music (0x00748620).
  menu_music_wanted_ = true;
  menu_music_countdown_ = 0;
  ui_.show_bars(false);
  open_main_menu();
  std::printf("front:        %zu menu(s) open\n", ui_.dialog_count());
  return true;
}

void Application::tick_front(platform::Window::Frame& frame) {
  if (!target_.ensure(window_.device(), frame.width, frame.height)) return;
  const platform::Rgba black{0.0F, 0.0F, 0.0F, 1.0F};
  renderer_.begin(frame.width, frame.height);
  renderer_.render(frame.commands, target_.texture(), &black);
  ui_.set_viewport(frame.width, frame.height);
  std::string error;
  ui_.render(frame.commands, target_.texture(), &error);
  target_.blit_to(frame.commands, frame.swapchain, frame.width, frame.height);
}

/// `MENUBACK.INI` under `MAINMENU.INI`. The buttons, by `Id`: Tutorial
/// `0x1002`, Great Battles `0x1009`, Conquest `0x1060`, Single player
/// `0x1010`, Change player `0x1011`, Load game `0x1004`, Options `0x1005`,
/// Credits `0x1006`, Quit `0x1007`. Multiplayer, Online Battle and News are
/// shown disabled, because all three are online and nothing behind them
/// exists here.
void Application::open_main_menu() {
  close_all_menus();
  open_menu("menuini/menuback.ini", [](const core::ui::DialogEvent&, core::ui::Dialog&) {});
  core::ui::Dialog* menu = open_menu(
      "menuini/mainmenu.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        if (event.kind != core::ui::DialogEvent::Kind::kCommand) return;
        switch (event.id) {
          case 0x1002: (void)play_from_front("Adventures/Tutorial.BFHP", -1); break;
          case 0x1009: open_great_battles_menu(); break;
          case 0x1060:
            open_container_menu("menuini/conquestmenu.ini", vfs_.root() / "Conquests", "List");
            break;
          case 0x1010: open_scenario_menu(false); break;
          // The editor: the scenario list, and the chosen one opens placed
          // rather than played. The original's `PreAdventureMenu.ini` front
          // is not reproduced; the list is the same screen.
          case 0x1008: open_scenario_menu(true); break;
          case 0x1004: open_front_load_menu(); break;
          case 0x1005: open_options_menu(); break;
          case 0x1006: open_credits_menu(); break;
          case 0x1011: open_profile_menu(); break;  // Change player
#if IMPERIVM_HAVE_NET
          case 0x1003: open_mp_menu(); break;  // Multiplayer
#endif
          case 0x1007: quit_requested_ = true; break;
          case 0x1054: show_next_tip(menu); break;  // Next tip
          case 0x1055: {                             // More Info: the tip's help topic
            if (tips_last_ < 0 || static_cast<std::size_t>(tips_last_) >= tips_.size()) break;
            const std::string link = tips_[static_cast<std::size_t>(tips_last_)].link;
            open_help();
            if (help_ == nullptr || link.empty()) break;
            const std::int32_t topic = help_->resolve(link, help_->home());
            if (topic < 0) break;
            for (std::size_t i = ui_.dialog_count(); i-- > 0;) {
              core::ui::Dialog* dialog = ui_.dialog(i);
              if (dialog != nullptr && fold_name(dialog->screen().path).find("help.ini") != std::string::npos) {
                show_help_topic(*dialog, topic, true);
                break;
              }
            }
            break;
          }
          default: break;
        }
      });
  if (menu == nullptr) return;
  // Online Battle and News were a service that no longer exists. Multiplayer
  // is the LAN and a typed address, which need nothing but the two machines.
  for (const char* name : {"OnlineBattle", "UpdateBtn"}) menu->set_enabled(name, false);
#if !IMPERIVM_HAVE_NET
  menu->set_enabled("MultiPlayer", false);
#endif
  menu->set_text("Version", core::version_string());
  menu->set_text("VersionShadow", core::version_string());
  // The Tips frame: `CurrentLang\TIPS.XML`'s tips, one shown, as the
  // original's main menu shows them (0x006d8050). Without the document
  // the frame stays down.
  if (!tips_loaded_) {
    tips_loaded_ = true;
    if (const platform::ByteSpan bytes = vfs_.read("CurrentLang\\TIPS.XML"); !bytes.empty()) {
      if (auto parsed = core::game::parse_tips(platform::as_core_bytes(bytes)); parsed.ok()) tips_ = std::move(parsed.value());
    }
  }
  const bool tips = !tips_.empty();
  for (const char* name : {"TipsBackFrame", "TipsText", "NextTipButton", "MoreInfoButton"}) menu->set_hidden(name, !tips);
  if (tips) show_next_tip(*menu);
}

/// The next tip, by the original's rule (0x006d8050): while not every tip
/// has been shown, the next in order -- `LastTip + 1`, and past the end
/// `AllShown` is set; once all have been shown, one drawn at random, and a
/// draw that repeats the tip standing moves on by one. Both numbers are
/// kept in `settings.ini` under `[Tips]`, written when the settings are.
void Application::show_next_tip(core::ui::Dialog& menu) {
  const auto count = static_cast<std::int32_t>(tips_.size());
  if (count == 0) return;
  if (!tips_all_shown_) {
    tips_last_ += 1;
    if (tips_last_ >= count) tips_all_shown_ = true;
  }
  if (tips_all_shown_) {
    const std::int32_t drawn = static_cast<std::int32_t>(SDL_rand(count));
    tips_last_ = drawn == tips_last_ ? (drawn + 1) % count : drawn;
  }
  tips_last_ = std::clamp(tips_last_, 0, count - 1);
  show_tip(menu);
  (void)save_settings();
}

void Application::show_tip(core::ui::Dialog& menu) const {
  if (tips_last_ < 0 || static_cast<std::size_t>(tips_last_) >= tips_.size()) return;
  const core::game::Tip& tip = tips_[static_cast<std::size_t>(tips_last_)];
  menu.set_text("TipsText", tip.text);
  menu.set_enabled("MoreInfoButton", !tip.link.empty());
}

/// `CREDITSMENU.INI`: `CurrentLang/credits.txt` rolled up through the
/// `Credits` block at the file's own `[CreditsDescription] PixPerSec`,
/// starting below the frame and ending when the last line has passed its
/// top, which returns to the main menu; Close (0x1001) returns at once.
/// `UniCode=0` says the text is single-byte, which the fonts index
/// directly. **Readings, labelled:** that the roll ends the screen rather
/// than stopping, and the `[IMG_...]` image cues the file's comments
/// describe, none of which the shipped file uses, are not drawn.
void Application::open_credits_menu() {
  const platform::ByteSpan text_bytes = vfs_.read("CurrentLang/credits.txt");
  if (text_bytes.empty()) {
    std::printf("credits: no CurrentLang/credits.txt in the language pack\n");
    return;
  }
  std::string text(reinterpret_cast<const char*>(text_bytes.data()), text_bytes.size());
  std::string normalised;
  normalised.reserve(text.size());
  for (const char c : text) {
    if (c != '\r') normalised.push_back(c);
  }
  close_to_background();
  core::ui::Dialog* dialog = open_menu(
      "menuini/creditsmenu.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog&) {
        if (event.kind == core::ui::DialogEvent::Kind::kCommand && event.id == 0x1001) {
          credits_ = nullptr;
          open_main_menu();
        }
      });
  if (dialog == nullptr) return;
  credits_ = dialog;
  credits_started_ticks_ = SDL_GetTicks();
  credits_pixels_per_second_ = 100;
  // The screen's own section names the file and the speed.
  if (const platform::ByteSpan ini = vfs_.read("menuini/creditsmenu.ini"); !ini.empty()) {
    if (const auto doc = core::IniDocument::parse(platform::as_core_bytes(ini)); doc.ok()) {
      const core::SectionIndex section = doc->section("CreditsDescription");
      if (section != core::kNoSection) {
        std::string speed(doc->value(section, "PixPerSec"));
        if (const std::size_t comment = speed.find(';'); comment != std::string::npos) speed.erase(comment);
        while (!speed.empty() && speed.back() == ' ') speed.pop_back();
        if (const int value = SDL_atoi(speed.c_str()); value > 0) credits_pixels_per_second_ = value;
      }
    }
  }
  dialog->set_text("Credits", normalised);
  const core::ui::Rect rect = dialog->widget_rect("Credits");
  credits_height_ = rect.height;
  core::ui::ResourceCache fonts([this](std::string_view path) { return platform::as_core_bytes(vfs_.read(path)); });
  if (const core::ui::Widget* widget = dialog->screen().find("Credits"); widget != nullptr) {
    if (const core::ui::Font* font = fonts.font(widget->attribute("Font")); font != nullptr) {
      credits_height_ = core::ui::text_block_height(*font, normalised, rect.width);
    }
  }
  // Starts below the frame: the first line enters from the bottom.
  dialog->content().state("Credits").text_scroll = -rect.height;
  dialog->set_hidden("CreditsImageIdInputField", true);
}

void Application::tick_credits() {
  if (credits_ == nullptr) return;
  if (ui_.index_of(credits_) >= ui_.dialog_count()) {
    credits_ = nullptr;
    return;
  }
  const core::ui::Rect rect = credits_->widget_rect("Credits");
  const std::uint64_t elapsed = SDL_GetTicks() - credits_started_ticks_;
  const auto scrolled = static_cast<std::int32_t>(elapsed * static_cast<std::uint64_t>(credits_pixels_per_second_) / 1000);
  const std::int32_t offset = scrolled - rect.height;
  core::ui::WidgetState& state = credits_->content().state("Credits");
  if (state.text_scroll != offset) {
    state.text_scroll = offset;
    credits_->touch();
  }
  if (offset >= credits_height_) {
    credits_ = nullptr;
    open_main_menu();
  }
}

// --------------------------------------------------------------------------
// the profiles screen
// --------------------------------------------------------------------------

std::filesystem::path Application::profiles_directory() const {
  return saves_directory() / "Profiles";
}

std::span<const core::game::Rank> Application::ranks() const {
  if (!ranks_loaded_) {
    ranks_loaded_ = true;
    if (const platform::ByteSpan bytes = vfs_.read("data/Const.ini"); !bytes.empty()) {
      if (const auto doc = core::IniDocument::parse(platform::as_core_bytes(bytes)); doc.ok()) {
        ranks_ = core::game::parse_ranks(doc.value());
      }
    }
  }
  return ranks_;
}

/// Every profile, the installation's and this engine's own.
///
/// The original enumerates `Profiles/` and takes every subdirectory
/// (0x005707d0); `profiles.ini` names a default and is not a list. Here the
/// same scan runs twice, over the installation and over `Saves/Profiles/`,
/// and a directory of the same name in the second shadows the first --
/// which is how a profile the player renames or edits stops being the
/// read-only one it started as.
std::vector<Application::ProfileRow> Application::scan_profiles() const {
  std::vector<ProfileRow> rows;
  const auto scan = [&](const std::filesystem::path& root, bool writable) {
    std::error_code ignored;
    std::vector<std::filesystem::path> directories;
    for (const auto& entry : std::filesystem::directory_iterator(root, ignored)) {
      if (!entry.is_directory(ignored)) continue;
      if (!std::filesystem::is_regular_file(entry.path() / "player.ini", ignored)) continue;
      directories.push_back(entry.path());
    }
    // The directory order a filesystem gives is not the one Windows gives, so
    // the list is sorted rather than left to the platform: the screen's rows
    // must not move between runs.
    std::sort(directories.begin(), directories.end());
    for (const std::filesystem::path& directory : directories) {
      ProfileRow row;
      row.directory = directory.filename().string();
      row.path = directory / "player.ini";
      row.writable = writable;
      std::vector<std::byte> bytes;
      if (std::ifstream file(row.path, std::ios::binary); file) {
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        bytes.resize(text.size());
        std::memcpy(bytes.data(), text.data(), text.size());
      }
      const auto document = core::IniDocument::parse(bytes);
      if (!document.ok()) continue;
      auto parsed = core::game::parse_profile(document.value());
      // A record missing a key fails the file, as it does in the original.
      // The profile is still listed, because a player must be able to see
      // and delete one the game left half-written.
      row.profile = parsed.ok() ? std::move(parsed.value()) : core::game::Profile{};
      row.profile.directory = row.directory;
      if (row.profile.name.empty()) row.profile.name = row.directory;
      core::game::aggregate(row.profile, ranks());
      const auto same = std::find_if(rows.begin(), rows.end(), [&](const ProfileRow& seen) {
        return fold_name(seen.directory) == fold_name(row.directory);
      });
      if (same == rows.end()) {
        rows.push_back(std::move(row));
      } else {
        *same = std::move(row);
      }
    }
  };
  scan(vfs_.root() / "Profiles", false);
  scan(profiles_directory(), true);
  return rows;
}

/// A profile renamed: the directory it lives in moves, and nothing inside
/// it is touched.
///
/// The directory **is** the name -- the original's list and its caption both
/// show it (0x006ed520 and 0x006ec0e6 read the same string, which the scan
/// filled from the directory entry) -- and `[Player] name` is a separate
/// thing the setup screen uses. The shipped profile proves they are
/// separate: its directory is `Nome` and its `name=` is `Angel`. So a
/// rename moves the directory and leaves `name=` where it is, rather than
/// inventing a rule that keeps two independent fields in step.
///
/// Only this engine's own profiles can be renamed; see `open_profile_menu`.
bool Application::rename_profile(const ProfileRow& row, const std::string& name) const {
  if (!row.writable) return false;
  std::error_code failed;
  std::filesystem::rename(row.path.parent_path(), profiles_directory() / name, failed);
  return !failed;
}

/// The twelve lines, in the order 0x006ec090 writes them and with its own
/// formats.
///
/// The heading table has fourteen slots and the screen draws twelve: slots 4
/// and 8 hold `OUT FOR LUNCH` and `OUT FOR DINNER`, are translated with the
/// rest and are never shown. They are left out here rather than reproduced,
/// because a line nothing draws is not a line.
///
/// **Readings, labelled:** that `Resources spent` puts gold before food (the
/// two are translated in the `short form` context and both orders read, but
/// the field order and the shipped numbers agree on gold first); and that a
/// games line with no games shows the bare `%s: %u` form, which the shipped
/// screen never reaches because it does not draw the line at all.
std::vector<std::string> Application::profile_info_lines(const core::game::Profile& profile) const {
  const core::game::ProfileStats& stats = profile.stats;
  std::vector<std::string> lines;
  const auto heading = [this](std::string_view text) { return item_label(text, "infolist"); };
  const auto line = [&lines](std::string text) { lines.push_back(std::move(text)); };

  line(heading("Rank") + ": " + (stats.rank.empty() ? std::string() : stats.rank) + " (" +
       item_label("military rating", {}) + " " + std::to_string(stats.rating()) + ")");
  if (stats.single_games != 0) {
    line(heading("Single player games") + ": " + std::to_string(stats.single_games) + " (" +
         std::to_string(stats.single_won_percent) + "% " + item_label("won", {}) + ")");
  }
  if (stats.multi_games != 0) {
    line(heading("Multiplayer games") + ": " + std::to_string(stats.multi_games) + " (" +
         std::to_string(stats.multi_won_percent) + "% " + item_label("won", {}) + ")");
  }
  line(heading("Game time") + ": " + std::to_string(stats.hours()) + " " +
       item_label("hours", "infolist"));
  // The nation's name comes from the `race` context, the same eight the
  // setup screen names, with `unknown` for a profile that has played none.
  const std::string nation =
      stats.favourite_race < 0
          ? item_label("unknown", "race")
          : item_label(race_display_name(stats.favourite_race), "race");
  if (stats.favourite_race < 0) {
    line(heading("Favorite nation") + ": " + nation);
  } else {
    line(heading("Favorite nation") + ": " + nation + " (" +
         std::to_string(stats.favourite_race_percent) + "%)");
  }
  line(heading("Favorite unit") + ": " +
       (profile.favourite_unit.empty() ? item_label("unknown", "favorite unit")
                                       : item_label(profile.favourite_unit, {})));
  line(heading("Resources spent") + ": " + std::to_string(stats.gold_spent) + " " +
       item_label("gold", "short form") + ", " + std::to_string(stats.food_spent) + " " +
       item_label("food", "short form"));
  line(heading("Units eliminated") + ": " + std::to_string(stats.units_killed));
  line(heading("Units lost") + ": " + std::to_string(stats.units_lost));
  line(heading("Health spent for rituals") + ": " + std::to_string(stats.health_sacrificed));
  if (stats.best_unit.empty()) {
    line(heading("Most experienced unit") + ": " + item_label("Unknown", "maxunits"));
  } else {
    line(heading("Most experienced unit") + ": " + stats.best_unit + " (" +
         item_label("Level", "maxlevel") + " " + std::to_string(stats.best_level) + ")");
  }
  line(heading("Maximum number of units") + ": " + std::to_string(stats.most_units));
  return lines;
}

void Application::show_profile(core::ui::Dialog& menu, const std::vector<ProfileRow>& rows,
                               std::int32_t index) const {
  const bool have = index >= 0 && static_cast<std::size_t>(index) < rows.size();
  menu.set_text("InfoBackFrame", have ? rows[static_cast<std::size_t>(index)].directory : "");
  menu.set_items("InfoList", have ? profile_info_lines(rows[static_cast<std::size_t>(index)].profile)
                                  : std::vector<std::string>{});
  const bool mine = have && rows[static_cast<std::size_t>(index)].writable;
  menu.set_enabled("SelBtn", have);
  // Rename and Delete are for profiles this engine owns. One in the
  // installation is read and never written (`docs/legal.md` rule 1), so
  // both are refused for it, and the buttons say so rather than failing
  // when pressed. Selecting one, and playing as it, is unaffected.
  menu.set_enabled("RenBtn", mine);
  // The last profile cannot be deleted either: the screen must leave a
  // player to select. This engine's rule, labelled.
  menu.set_enabled("DelBtn", mine && rows.size() > 1);
}

/// `PROFILE.INI`: the profiles in `ProfileList` (`0x1015`), the selected
/// one's twelve lines in `InfoList` (`0x1017`) under its name, and four
/// buttons -- New `0x1001`, Rename `0x1002`, Delete `0x1003`, Select
/// `0x1004`, which is also `Enter` and `Esc`.
///
/// **What this engine does differently, and why.** The original writes into
/// `Profiles/` under the installation. `docs/legal.md` rule 1 keeps the
/// installation read-only, so the installed profiles are *read* and the
/// three buttons that change anything work on `Saves/Profiles/` instead:
/// New makes a directory there, Rename moves one, Delete removes one. A
/// profile that lives in the installation can be selected and played as and
/// cannot be renamed or deleted, and its two buttons are disabled rather
/// than failing when pressed. A renamed copy beside the saves was the other
/// way to read the rule and is not taken: the copy could not shadow the
/// original -- the directory is the name, so a renamed one has a different
/// key -- and the list would show the same player twice.
///
/// **The directory is the name.** The original fills its list (0x006ed520)
/// and its caption (0x006ec0e6) from the same string, which its scan took
/// from the directory entry; `[Player] name` is a separate field the setup
/// screen uses, and the shipped profile shows they differ -- `Nome` on disk,
/// `Angel` in the file.
///
/// The `RenameBack`/`RenameEdit` pair the file declares `HIDDEN` is the
/// original's in-place rename, floated over the list row. It is shown here
/// where the file puts it, at the top left of the screen, because where the
/// original moves it to is the list's row rectangle and that is presentation
/// this engine has no reading for. Labelled.
///
/// **Two frames, one `TextId`.** `ProfileBackFrame` and `InfoBackFrame` both
/// declare `TextId = 0x1011`, and the original writes the selected profile's
/// name to *the* widget of that id (0x006ed3c9) -- so which of the two
/// captions it lands on is a question about the order it builds its children
/// in, which the file does not answer. The name goes on the info frame here,
/// because that is the frame it describes. Labelled. `Players`, the other
/// frame's caption, has no row in the shipped translation table and shows in
/// English in every language; that is the data's, not this engine's.
void Application::open_profile_menu() {
  close_to_background();
  auto rows = std::make_shared<std::vector<ProfileRow>>(scan_profiles());
  // What the floating edit is for: nothing, a new profile's name, or a
  // rename of the row at `editing_row`. The original uses one edit for both
  // (0x006ed630 branches on a flag it was opened with) and so does this.
  enum class Editing { kNone, kNew, kRename };
  auto editing = std::make_shared<Editing>(Editing::kNone);
  core::ui::Dialog* dialog = open_menu(
      "menuini/profile.ini",
      [this, rows, editing](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        const auto refresh = [this, rows, &menu](const std::string& keep) {
          *rows = scan_profiles();
          std::vector<std::string> names;
          names.reserve(rows->size());
          for (const ProfileRow& row : *rows) names.push_back(row.directory);
          menu.set_items("ProfileList", std::move(names));
          std::int32_t index = rows->empty() ? -1 : 0;
          for (std::size_t row = 0; row < rows->size(); ++row) {
            if (fold_name((*rows)[row].directory) == fold_name(keep)) {
              index = static_cast<std::int32_t>(row);
            }
          }
          menu.select("ProfileList", index);
          show_profile(menu, *rows, index);
        };
        const auto stop_editing = [editing, &menu]() {
          *editing = Editing::kNone;
          menu.set_hidden("RenameBack", true);
          menu.set_hidden("RenameEdit", true);
        };
        const auto start_editing = [editing, &menu](Editing what, const std::string& text) {
          *editing = what;
          menu.set_text("RenameEdit", text);
          menu.set_hidden("RenameBack", false);
          menu.set_hidden("RenameEdit", false);
          menu.focus("RenameEdit");
        };
        if (event.kind == Kind::kSelect && event.widget == "ProfileList") {
          stop_editing();
          show_profile(menu, *rows, event.index);
          return;
        }
        if (event.kind != Kind::kCommand) return;
        const std::int32_t index = menu.selected("ProfileList");
        const bool have = index >= 0 && static_cast<std::size_t>(index) < rows->size();

        // The edit's Enter arrives as the screen's own `Enter = SelBtn`
        // line, so an edit in progress takes the key before Select does. A
        // name that is empty or carries a path separator or a dot is
        // refused rather than sanitised: it becomes a directory.
        if (*editing != Editing::kNone && (event.id == 0x1004 || event.id == 0x1101)) {
          const Editing what = *editing;
          const std::string name = menu.text("RenameEdit");
          stop_editing();
          const bool usable = !name.empty() && name.find_first_of("/\\.") == std::string::npos &&
                              std::none_of(rows->begin(), rows->end(), [&](const ProfileRow& row) {
                                return fold_name(row.directory) == fold_name(name);
                              });
          std::string keep = have ? (*rows)[static_cast<std::size_t>(index)].directory : "";
          if (usable && what == Editing::kNew) {
            std::error_code ignored;
            const std::filesystem::path made = profiles_directory() / name;
            std::filesystem::create_directories(made, ignored);
            // A fresh profile is a name and an empty journal. Everything
            // else on the screen is aggregated, so there is nothing else to
            // write; the setup screen fills the rest in when it is used.
            if (std::ofstream file(made / "player.ini", std::ios::binary | std::ios::trunc); file) {
              file << "[Player]\nname=" << name << "\ngames=0\n";
            }
            keep = name;
          } else if (usable && what == Editing::kRename && have) {
            const ProfileRow& row = (*rows)[static_cast<std::size_t>(index)];
            if (rename_profile(row, name)) {
              if (fold_name(profile_) == fold_name(row.directory)) {
                profile_ = name;
                (void)save_settings();
              }
              keep = name;
            }
          }
          refresh(keep);
          return;
        }

        switch (event.id) {
          case 0x1001:  // New: the same edit, on a default name
            start_editing(Editing::kNew, item_label("noname", {}));
            break;
          case 0x1002:  // Rename
            if (have && (*rows)[static_cast<std::size_t>(index)].writable) {
              start_editing(Editing::kRename, (*rows)[static_cast<std::size_t>(index)].directory);
            }
            break;
          case 0x1003: {  // Delete
            if (!have || rows->size() < 2) break;
            const ProfileRow& row = (*rows)[static_cast<std::size_t>(index)];
            if (!row.writable) break;  // the button is disabled; belt and braces
            const std::string directory = row.directory;
            // The confirmation is this engine's, labelled: nothing was read
            // that says the original asks. Deleting a profile throws away a
            // journal that cannot be rebuilt, so it asks.
            open_confirm(item_label("Delete", "/Menu/profile.ini:DelBtn:Text") + " " +
                             row.directory + "?",
                         [this, directory]() {
                           std::error_code ignored;
                           std::filesystem::remove_all(profiles_directory() / directory, ignored);
                           open_profile_menu();
                         });
            break;
          }
          case 0x1004:  // Select, and the screen's Esc and Enter
            if (have) {
              profile_ = (*rows)[static_cast<std::size_t>(index)].directory;
              (void)save_settings();
            }
            open_main_menu();
            break;
          default: break;
        }
      });
  if (dialog == nullptr) {
    open_main_menu();
    return;
  }
  std::vector<std::string> names;
  names.reserve(rows->size());
  for (const ProfileRow& row : *rows) names.push_back(row.directory);
  dialog->set_items("ProfileList", std::move(names));
  std::int32_t index = rows->empty() ? -1 : 0;
  for (std::size_t row = 0; row < rows->size(); ++row) {
    if (fold_name((*rows)[row].directory) == fold_name(profile_)) {
      index = static_cast<std::int32_t>(row);
    }
  }
  dialog->select("ProfileList", index);
  show_profile(*dialog, *rows, index);
}

/// The containers of a directory, by file name, with their `game.xml` name
/// and description. The list is the file order the directory gives sorted,
/// which puts `1_Great_Battles_Zama` first as the game does.
std::vector<Application::ContainerRow> Application::scan_containers(const std::filesystem::path& directory) {
  using Row = ContainerRow;
  std::vector<Row> rows;
  std::error_code ignored;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ignored)) {
    if (!entry.is_regular_file(ignored)) continue;
    std::string extension = entry.path().extension().string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension != ".bfhp") continue;
    Row row;
    row.path = entry.path();
    row.name = entry.path().stem().string();
    gamedata::MapContainer container;
    if (container.open(entry.path(), nullptr)) {
      if (const auto bytes = container.read("game.xml"); !bytes.empty()) {
        if (auto game = core::GameProperties::parse(bytes); game.ok()) {
          const core::game::TranslationTable table = container_table(container);
          if (!game->name.empty()) row.name = localised(table, game->name);
          row.description = localised(table, game->description);
        }
      }
    }
    std::filesystem::path picture = entry.path();
    picture.replace_extension(".bmp");
    if (std::filesystem::exists(picture, ignored)) {
      row.picture = gamedata::container_relative(vfs_.root(), picture);
    }
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.path.filename() < b.path.filename(); });
  return rows;
}

/// The Great Battles: `PREADVENTUREMENU.INI` (0x006e944a) as the main menu's
/// button opens it, two lists under two headings -- `ListBattles`, *The
/// Great Victories of Rome*, over `adventures/GreatBattles/`, and
/// `ListLoses`, *Rome's Enemies Fight for Freedom*, over
/// `adventures/GreatChallenges/` -- the chosen adventure's picture in
/// `DescriptionBmp` with its name and description beside, a difficulty
/// combo, Start (0x1005) and Cancel (0x1006). The screen serves the exe's
/// other fronts too with other widgets shown (`ListAll`, `Conquests`,
/// `Custom`, `StartConquest`); those stay hidden here. **Reading,
/// labelled:** the difficulty chosen is the game's difficulty
/// (`GetDifficulty`), kept with the settings.
void Application::open_great_battles_menu() {
  std::vector<ContainerRow> battles = scan_containers(vfs_.root() / "Adventures" / "GreatBattles");
  std::vector<ContainerRow> challenges = scan_containers(vfs_.root() / "Adventures" / "GreatChallenges");
  listed_containers_.clear();
  std::vector<std::string> battle_names;
  std::vector<std::string> challenge_names;
  std::vector<std::string> descriptions;
  std::vector<std::string> titles;
  std::vector<std::string> pictures;
  for (ContainerRow& row : battles) {
    listed_containers_.push_back(row.path);
    battle_names.push_back(row.name);
    titles.push_back(std::move(row.name));
    descriptions.push_back(std::move(row.description));
    pictures.push_back(std::move(row.picture));
  }
  const std::size_t split = listed_containers_.size();
  for (ContainerRow& row : challenges) {
    listed_containers_.push_back(row.path);
    challenge_names.push_back(row.name);
    titles.push_back(std::move(row.name));
    descriptions.push_back(std::move(row.description));
    pictures.push_back(std::move(row.picture));
  }
  close_to_background();
  // Which row of the two lists is chosen, as an index into `listed_containers_`.
  const auto chosen = [split](const core::ui::Dialog& menu) -> std::int32_t {
    const std::int32_t battle = menu.selected("ListBattles");
    if (battle >= 0) return battle;
    const std::int32_t challenge = menu.selected("ListLoses");
    return challenge >= 0 ? static_cast<std::int32_t>(split) + challenge : -1;
  };
  const auto show = [titles, descriptions, pictures, chosen](core::ui::Dialog& menu) {
    const std::int32_t index = chosen(menu);
    const bool have = index >= 0 && static_cast<std::size_t>(index) < titles.size();
    const auto at = static_cast<std::size_t>(std::max(0, index));
    menu.set_text("AdvDescriptionTitle", have ? titles[at] : "");
    menu.set_text("AdvDescriptionText", have ? descriptions[at] : "");
    menu.content().state("DescriptionBmp").image = have ? pictures[at] : "";
  };
  core::ui::Dialog* dialog = open_menu(
      "menuini/preadventuremenu.ini", [this, show, chosen](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        const auto start = [&]() {
          const std::int32_t index = chosen(menu);
          if (index < 0 || static_cast<std::size_t>(index) >= listed_containers_.size()) return;
          settings_.difficulty = std::clamp(menu.selected("AdvDifficultyType"), 0, 2);
          (void)save_settings();
          (void)play_from_front(gamedata::container_relative(vfs_.root(), listed_containers_[static_cast<std::size_t>(index)]), -1);
        };
        if (event.kind == Kind::kSelect && (event.widget == "ListBattles" || event.widget == "ListLoses")) {
          // One choice between the two lists.
          menu.select(event.widget == "ListBattles" ? "ListLoses" : "ListBattles", -1);
          show(menu);
          return;
        }
        if (event.kind == Kind::kActivate && (event.widget == "ListBattles" || event.widget == "ListLoses")) start();
        if (event.kind != Kind::kCommand) return;
        if (event.id == 0x1005) start();
        if (event.id == 0x1006) open_main_menu();
      });
  if (dialog == nullptr) {
    open_main_menu();
    return;
  }
  for (const char* name : {"StartConquest", "Custom", "GameAdv", "AllAdv", "ListAll", "ListAll.ScrollUp",
                           "ListAll.ScrollDown", "ListAll.VScrollBack", "ListAll.VScroll", "Conquests"}) {
    dialog->set_hidden(name, true);
  }
  for (const char* name : {"AdvBackFrame", "AdvThinFrame1", "AdvThinFrame2", "AdvRomeBattles", "AdvNoRomeBattles",
                           "ListBattles", "ListLoses"}) {
    dialog->set_hidden(name, false);
  }
  dialog->set_items("ListBattles", std::move(battle_names));
  dialog->set_items("ListLoses", std::move(challenge_names));
  dialog->set_items("AdvDifficultyType", {item_label("Easy", "Adventure difficulty level"),
                                           item_label("Normal", "Adventure difficulty level"),
                                           item_label("Hard", "Adventure difficulty level")});
  dialog->select("AdvDifficultyType", std::clamp(settings_.difficulty, 0, 2));
  if (!listed_containers_.empty()) dialog->select("ListBattles", 0);
  show(*dialog);
}

void Application::open_container_menu(std::string_view screen,
                                      const std::filesystem::path& directory,
                                      std::string_view list_widget) {
  std::vector<ContainerRow> rows = scan_containers(directory);
  listed_containers_.clear();
  std::vector<std::string> names;
  std::vector<std::string> descriptions;
  std::vector<std::string> pictures;
  for (ContainerRow& row : rows) {
    listed_containers_.push_back(row.path);
    names.push_back(std::move(row.name));
    descriptions.push_back(std::move(row.description));
    pictures.push_back(std::move(row.picture));
  }
  // A front screen replaces whatever stood over the background, and Cancel
  // brings the main menu back.
  close_to_background();
  const std::string list(list_widget);
  const auto show = [descriptions, pictures, list](core::ui::Dialog& menu) {
    const std::int32_t index = menu.selected(list);
    const std::size_t at = static_cast<std::size_t>(std::max(0, index));
    menu.set_text("AdvDescriptionText", index >= 0 && at < descriptions.size() ? descriptions[at] : "");
    menu.content().state("Caption").image = index >= 0 && at < pictures.size() ? pictures[at] : "";
  };
  core::ui::Dialog* dialog = open_menu(
      screen, [this, show, list](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        const auto start = [&]() {
          const std::int32_t index = menu.selected(list);
          if (index < 0 || static_cast<std::size_t>(index) >= listed_containers_.size()) return;
          const std::filesystem::path& chosen = listed_containers_[static_cast<std::size_t>(index)];
          // A conquest goes to its campaign map; an adventure straight in.
          gamedata::MapContainer container;
          if (container.open(chosen, nullptr) && !container.read("territories.xml").empty()) {
            open_campaign_screen(chosen);
            return;
          }
          (void)play_from_front(gamedata::container_relative(vfs_.root(), chosen), -1);
        };
        if (event.kind == Kind::kSelect && event.widget == list) show(menu);
        if (event.kind == Kind::kActivate && event.widget == list) start();
        if (event.kind != Kind::kCommand) return;
        if (event.id == 0x1005) start();
        if (event.id == 0x1006) open_main_menu();
      });
  if (dialog == nullptr) {
    open_main_menu();
    return;
  }
  dialog->set_items(list, std::move(names));
  dialog->set_items("AdvDifficultyType", {item_label("Easy", "Adventure difficulty level"),
                                        item_label("Normal", "Adventure difficulty level"),
                                        item_label("Hard", "Adventure difficulty level")});
  dialog->select("AdvDifficultyType", 1);
  if (!listed_containers_.empty()) dialog->select(list, 0);
  show(*dialog);
}

/// `SELECTMAP.INI` over `Scenarios/`: the map's name from its `game.xml`,
/// its description below, Select `0xA001` plays it as player 0.
void Application::open_scenario_menu(bool to_edit) {
  listed_containers_.clear();
  std::vector<std::string> names;
  std::vector<std::string> descriptions;
  std::error_code ignored;
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(vfs_.root() / "Scenarios", ignored)) {
    if (!entry.is_regular_file(ignored)) continue;
    std::string extension = entry.path().extension().string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (extension == ".bfhp") files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());
  for (const std::filesystem::path& file : files) {
    std::string name = file.stem().string();
    std::string description;
    gamedata::MapContainer container;
    if (container.open(file, nullptr)) {
      if (const auto bytes = container.read("game.xml"); !bytes.empty()) {
        if (auto game = core::GameProperties::parse(bytes); game.ok()) {
          const core::game::TranslationTable table = container_table(container);
          if (!game->name.empty()) name = localised(table, game->name);
          description = localised(table, game->description);
        }
      }
    }
    listed_containers_.push_back(file);
    names.push_back(std::move(name));
    descriptions.push_back(std::move(description));
  }
  close_to_background();
  const auto show = [descriptions](core::ui::Dialog& menu) {
    const std::int32_t index = menu.selected("List");
    const std::size_t at = static_cast<std::size_t>(std::max(0, index));
    menu.set_text("DescriptionText", index >= 0 && at < descriptions.size() ? descriptions[at] : "");
  };
  core::ui::Dialog* dialog = open_menu(
      "menuini/selectmap.ini", [this, show, to_edit](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        const auto start = [&]() {
          const std::int32_t index = menu.selected("List");
          if (index < 0 || static_cast<std::size_t>(index) >= listed_containers_.size()) return;
          if (to_edit) {
            args_.edit = true;
            (void)play_from_front(
                gamedata::container_relative(vfs_.root(), listed_containers_[static_cast<std::size_t>(index)]), -1);
            return;
          }
          if (scenario_for_host_) {
            open_setup_menu(listed_containers_[static_cast<std::size_t>(index)], SetupMode::host);
            return;
          }
          open_setup_menu(listed_containers_[static_cast<std::size_t>(index)]);
        };
        if (event.kind == Kind::kSelect && event.widget == "List") show(menu);
        if (event.kind == Kind::kActivate && event.widget == "List") start();
        if (event.kind != Kind::kCommand) return;
        if (event.id == 0xA001) start();
        if (event.id == 0xA002) {
#if IMPERIVM_HAVE_NET
          if (scenario_for_host_) {
            open_mp_menu();
            return;
          }
#endif
          open_main_menu();
        }
      });
  if (dialog == nullptr) {
    open_main_menu();
    return;
  }
  dialog->set_items("List", std::move(names));
  if (!listed_containers_.empty()) dialog->select("List", 0);
  show(*dialog);
}


/// The skirmish setup: `MPGAMEMENU.INI`, the players' screen, with
/// `SETTINGS.INI` at its `SettingsPos` (a right-top offset). One row per
/// slot the scenario's `player<i>.xml` declares as taking part, in slot
/// order: the colour square, the type button, the name (or the AI's
/// strength), the nation, the bonus and the team. The multiplayer widgets
/// -- ready marks, the host address, the clip -- are hidden. Start builds
/// the match from the rows; Cancel goes back to the scenario list.
///
/// Ids, per row `n` (1..8): `0xn000` colour, `0xn012` type, `0xn002` name,
/// `0xn022` nation, `0xn042` bonus, `0xn032` team; Start `0x9006`, Cancel
/// `0x9007`.
void Application::open_setup_menu(const std::filesystem::path& scenario, SetupMode mode) {
  setup_mode_ = mode;
  setup_players_.clear();
  gamedata::MapContainer container;
  std::string error;
  if (!container.open(scenario, &error)) {
    std::printf("setup: %s\n", error.c_str());
    return;
  }
  core::GameProperties game;
  if (const auto bytes = container.read("game.xml"); !bytes.empty()) {
    if (auto parsed = core::GameProperties::parse(bytes); parsed.ok()) game = parsed.value();
  }
  for (std::size_t i = 0; i < core::kPlayerSlots && setup_players_.size() < 8; ++i) {
    const auto document = container.read("player" + std::to_string(i) + ".xml");
    if (document.empty()) continue;
    auto slot = core::PlayerSlot::parse(document);
    if (!slot.ok()) continue;
    const core::sim::PlayerControl control = core::sim::parse_player_control(slot->control);
    if (control == core::sim::PlayerControl::disabled) continue;
    SetupPlayer row;
    row.slot = static_cast<core::PlayerId>(i);
    row.name = slot->name.empty() ? "Player " + std::to_string(i + 1) : slot->name;
    row.type = static_cast<std::int32_t>(i) == game.start_player ? 0 : 1;
    row.race = core::sim::race_from_name(slot->race);
    row.colour = slot->color;
    setup_players_.push_back(std::move(row));
  }
  if (setup_players_.empty()) return;
  if (std::none_of(setup_players_.begin(), setup_players_.end(),
                   [](const SetupPlayer& row) { return row.type == 0; })) {
    setup_players_.front().type = 0;
  }
  // A host's lobby: every seat but the host's own starts open, which is what
  // a host opening a networked game wants; it closes or computerises the
  // rest by hand.
  if (mode == SetupMode::host) {
    for (SetupPlayer& row : setup_players_) {
      if (row.type != 0) row.type = 3;
      if (row.type == 0 && !profile_.empty()) row.name = profile_;
    }
  }
  close_to_background();
  const std::string relative = gamedata::container_relative(vfs_.root(), scenario);
  core::ui::Dialog* dialog = open_menu(
      "menuini/mpgamemenu.ini",
      [this, relative](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        // A row a joiner may not edit: every row but its own, and in a
        // host's lobby every joiner's.
        const auto locked = [this](std::size_t i) {
          const SetupPlayer& row = setup_players_[i];
          if (setup_mode_ == SetupMode::join) return row.type != 0;
          if (setup_mode_ == SetupMode::host) return row.type == 4;
          return false;
        };
        if (event.kind == Kind::kChange) {
          // A nation or a strength chosen from a row's combobox.
          for (std::size_t i = 0; i < setup_players_.size(); ++i) {
            const std::string suffix = "_P" + std::to_string(i + 1);
            if (locked(i)) continue;
            if (event.widget == "PlayerAIRace" + suffix) {
              setup_players_[i].race = event.index <= 0 ? core::sim::kNoRace : event.index - 1;
            } else if (event.widget == "PlayerName" + suffix && setup_players_[i].type != 0) {
              setup_players_[i].difficulty = std::clamp(event.index, 0, 2);
            }
          }
#if IMPERIVM_HAVE_NET
          if (setup_mode_ == SetupMode::host) push_host_rows();
          if (setup_mode_ == SetupMode::join && net_front_.join != nullptr) {
            for (const SetupPlayer& row : setup_players_) {
              if (row.type != 0) continue;
              core::sim::Choice choice = net_front_.join->choice();
              choice.race = row.race;
              net_front_.join->set_choice(choice);
            }
          }
#endif
          return;
        }
        if (event.kind != Kind::kCommand) return;
#if IMPERIVM_HAVE_NET
        if (setup_mode_ != SetupMode::local) {
          if (event.id == 0x9007) {
            // Cancel: a host closes the game for everyone, a joiner leaves it.
            if (net_front_.host != nullptr) net_front_.host->close();
            if (net_front_.join != nullptr) net_front_.join->leave(core::sim::Refuse::Reason::left);
            end_net_front();
            open_mp_menu();
            return;
          }
          if (event.id == 0x11000) {  // Clip: the address, for the other player
            // Not a headless run's to touch: the clipboard is the user's.
            if (args_.headless) {
              std::printf("clip: %s\n", net_front_.address.c_str());
            } else {
              (void)SDL_SetClipboardText(net_front_.address.c_str());
            }
            return;
          }
          if (event.id == 0x9008 && net_front_.join != nullptr) {  // I'm ready
            core::sim::Choice choice = net_front_.join->choice();
            choice.ready = !choice.ready;
            net_front_.join->set_choice(choice);
            return;
          }
          if (event.id == 0x9006 && net_front_.host != nullptr) {
            if (!net_front_.startable) return;
            // The rules the settings screen holds, as they stand.
            push_host_rows();
            imperivm::net::Lobby lobby = net_front_.host->finish(imperivm::net::now_ms() | 1u);
            if (!lobby.ok) return;
            std::unique_ptr<imperivm::net::UdpSocket> socket = std::move(net_front_.socket);
            end_net_front();
            play_net_lobby(std::move(socket), std::move(lobby));
            return;
          }
        }
#endif
        if (event.id == 0x9007) {
          open_scenario_menu();
          return;
        }
        if (event.id == 0x9006) {
          setup_pending_ = true;
          // The rules outlive the game, as the profile keeps them.
          (void)save_settings();
          (void)play_from_front(relative, -1);
          return;
        }
        const int row = (event.id >> 12) - 1;
        const int kind = event.id & 0xFFF;
        if (row < 0 || static_cast<std::size_t>(row) >= setup_players_.size()) return;
        SetupPlayer& player = setup_players_[static_cast<std::size_t>(row)];
        if (setup_mode_ == SetupMode::join) {
          // A joiner edits its own row's team and bonus, and nothing else.
          if (player.type != 0) return;
          if (kind == 0x032) player.team = (player.team + 1) % 5;
          if (kind == 0x042) player.bonus = (player.bonus + 1) % 4;
#if IMPERIVM_HAVE_NET
          if (net_front_.join != nullptr) {
            core::sim::Choice choice = net_front_.join->choice();
            choice.race = player.race;
            choice.team = static_cast<std::uint8_t>(player.team);
            choice.bonus = static_cast<std::uint8_t>(player.bonus);
            net_front_.join->set_choice(choice);
          }
#endif
          refresh_setup_menu(menu);
          return;
        }
        if (setup_mode_ == SetupMode::host && kind == 0x012) {
          // The host's own row stays its; another row cycles computer, closed,
          // open; a joiner's row reopens, which the lobby tells the joiner.
          if (player.type == 1) player.type = 2;
          else if (player.type == 2) player.type = 3;
          else if (player.type == 3 || player.type == 4) player.type = player.type == 3 ? 1 : 3;
#if IMPERIVM_HAVE_NET
          push_host_rows();
#endif
          refresh_setup_menu(menu);
          return;
        }
        if (kind == 0x012) {
          // Human -> computer -> closed -> human; one human at a time.
          player.type = (player.type + 1) % 3;
          if (player.type == 0) {
            for (std::size_t i = 0; i < setup_players_.size(); ++i) {
              if (static_cast<int>(i) != row && setup_players_[i].type == 0) setup_players_[i].type = 1;
            }
          } else if (std::none_of(setup_players_.begin(), setup_players_.end(),
                                  [](const SetupPlayer& r) { return r.type == 0; })) {
            // Someone has to be the player.
            player.type = 0;
          }
        } else if (kind == 0x032) {
          player.team = (player.team + 1) % 5;
        } else if (kind == 0x042) {
          player.bonus = (player.bonus + 1) % 4;
        }
#if IMPERIVM_HAVE_NET
        if (setup_mode_ == SetupMode::host) push_host_rows();
#endif
        refresh_setup_menu(menu);
      });
  if (dialog == nullptr) {
    open_scenario_menu();
    return;
  }
  // The multiplayer widgets: the host's address and its clip in a host's
  // lobby, "I'm ready" in a joiner's, and every row's ready mark in both.
  const bool networked_setup = mode != SetupMode::local;
  for (const char* name : {"IamReadyText", "IamReady"}) dialog->set_hidden(name, true);
  for (const char* name : {"HostIPText", "HostIPName", "Clip"}) {
    dialog->set_hidden(name, mode != SetupMode::host);
  }
  dialog->set_hidden("ImReadyBig", mode != SetupMode::join);
  dialog->set_hidden("Start", mode == SetupMode::join);
  for (std::size_t i = 0; i < 8; ++i) {
    const std::string suffix = "_P" + std::to_string(i + 1);
    const bool used = i < setup_players_.size();
    for (const char* prefix : {"Color", "PlayerType", "PlayerName", "PlayerAIRace", "Bonus", "Team", "PlayerReady"}) {
      dialog->set_hidden(std::string(prefix) + suffix,
                         !used || (std::string_view(prefix) == "PlayerReady" && !networked_setup));
    }
    if (!used) continue;
    std::vector<std::string> nations{item_label("random", "race")};
    for (std::int32_t race = 0; race < 8; ++race) nations.push_back(item_label(race_display_name(race), "race"));
    dialog->set_items("PlayerAIRace" + suffix, std::move(nations));
  }
  refresh_setup_menu(*dialog);
  // The settings beside it, at the players' screen's `SettingsPos = 27, 27`,
  // a right-top offset. Shown with the one map type and game type this
  // engine plays; none of it is wired to a rule yet.
  const std::int32_t offset_x = 27;
  const std::int32_t offset_y = 27;
  (void)ensure_installation();
  core::ui::Dialog* settings = open_menu(
      "menuini/settings.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        // The collector 0x006f4d40 reads every widget on Start; reading them
        // as they change comes to the same record.
        if (event.kind == Kind::kChange) {
          const std::span<const std::string> scripts =
              install_ != nullptr ? install_->game_scripts() : std::span<const std::string>{};
          if (event.widget == "GameTypeCombo") {
            rules_.victory = event.index <= 0 || static_cast<std::size_t>(event.index) > scripts.size()
                                 ? std::string()
                                 : scripts[static_cast<std::size_t>(event.index) - 1];
            const std::vector<LimitRow> rows = limit_rows(rules_.victory);
            rules_.threshold = rows.empty() ? std::string() : rows.front().number;
            refresh_setup_rules(menu);
          } else if (event.widget == "LimitCombo") {
            const std::vector<LimitRow> rows = limit_rows(rules_.victory);
            if (event.index >= 0 && static_cast<std::size_t>(event.index) < rows.size()) {
              rules_.threshold = rows[static_cast<std::size_t>(event.index)].number;
            }
          } else if (event.widget == "WorldPopCombo") {
            const std::array<int, 3> levels = population_levels();
            rules_.world_population = levels[static_cast<std::size_t>(std::clamp(event.index, 0, 2))];
          } else if (event.widget == "StartingGoldCombo") {
            static constexpr int kGold[] = {2500, 5000, 10000, -1};
            rules_.starting_gold = kGold[std::clamp(event.index, 0, 3)];
          }
          return;
        }
        if (event.kind != Kind::kCommand) return;
        const auto checked = [&](const char* name) {
          const core::ui::WidgetState* state = menu.content().state_of(name);
          return state != nullptr && state->row == 1;
        };
        switch (event.id) {
          case 0x1007: rules_.no_fog = checked("NoFogCB"); break;
          case 0x1008: rules_.no_exploration = checked("NoExplorationCB"); break;
          case 0x1009: rules_.shared_support = checked("SharedSupportCB"); break;
          case 0x100a: rules_.shared_control = checked("SharedControlCB"); break;
          case 0x10a7: rules_.no_bonuses = checked("NoBonuses"); break;
          default: break;
        }
      });
  if (settings != nullptr) {
    const core::ui::Rect design = settings->screen().design;
    ui_.place_dialog(settings, 1024 - offset_x - design.width, offset_y);
    settings->set_items("MapTypeCombo", {item_label("Custom map", "gametype")});
    settings->select("MapTypeCombo", 0);
    settings->set_text("RM.MapSize", "");
    for (const char* name : {"RM.LabelMapSize", "RM.MapSize"}) settings->set_hidden(name, true);
    // The game types: `Map Default` -- the original's unselected state,
    // which hides the combo when restored from the profile (0x6f58a2) --
    // then `data/GameScripts/` by file, the text after the leading number.
    std::vector<std::string> types{item_label("Map Default", "")};
    const std::span<const std::string> scripts =
        install_ != nullptr ? install_->game_scripts() : std::span<const std::string>{};
    for (const std::string& script : scripts) {
      std::string_view text = script;
      while (!text.empty() && ((text.front() >= '0' && text.front() <= '9') || text.front() == ' ')) text.remove_prefix(1);
      types.push_back(item_label(text, ""));
    }
    settings->set_items("GameTypeCombo", std::move(types));
    settings->set_items("WorldPopCombo", {item_label("Low", "population"), item_label("Normal", "population"),
                                          item_label("High", "population")});
    settings->set_items("StartingGoldCombo", {"2500", "5000", "10000", item_label("Default", "gold")});
    refresh_setup_rules(*settings);
#if IMPERIVM_HAVE_NET
    // `MPCHAT.INI` below the players, where its own rectangle puts it: the
    // lobby's chat. Enter in its edit says the line (see `menus_take`).
    if (mode != SetupMode::local) {
      lobby_chat_shown_ = 0;
      core::ui::Dialog* chat = open_menu(
          "menuini/mpchat.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
            if (event.kind != core::ui::DialogEvent::Kind::kCommand || event.id != 0x9003) return;
            const std::string text = menu.text("ChatEdit");
            if (text.empty()) return;
            if (net_front_.host != nullptr) net_front_.host->say(text);
            if (net_front_.join != nullptr) net_front_.join->say(text);
            menu.set_text("ChatEdit", "");
          });
      if (chat != nullptr) {
        // `RectWH = 0, 424, ...`: the bottom-left, under the players.
        const core::ui::Rect design = chat->screen().design;
        ui_.place_dialog(chat, design.x, design.y);
        chat->focus("ChatEdit");
      }
    }
#endif
    // A joiner reads the host's rules; it does not set them.
    if (mode == SetupMode::join) {
      for (const char* name : {"GameTypeCombo", "LimitCombo", "WorldPopCombo", "StartingGoldCombo",
                               "MapTypeCombo", "NoFogCB", "NoExplorationCB", "SharedSupportCB",
                               "SharedControlCB", "NoBonuses"}) {
        settings->set_enabled(name, false);
      }
    }
  }
#if IMPERIVM_HAVE_NET
  if (mode == SetupMode::host) {
    // The lobby opens with the screen: the default port if it is free, any
    // port if not, and the address the other players type.
    std::string error;
    std::optional<imperivm::net::UdpSocket> socket =
        imperivm::net::UdpSocket::open(core::sim::kDefaultNetPort, false, &error);
    if (!socket.has_value()) socket = imperivm::net::UdpSocket::open(0, false, &error);
    if (!socket.has_value()) {
      open_multi_box("Cannot open a network port: " + error, [this] { open_mp_menu(); });
      return;
    }
    net_front_ = NetFront{};
    net_front_.role = NetFront::Role::host;
    net_front_.socket = std::make_unique<imperivm::net::UdpSocket>(std::move(*socket));
    core::sim::Start base;
    for (const SetupPlayer& row : setup_players_) {
      if (row.type == 0) base.you = row.slot;
    }
    base.map = relative;
    base.map_hash = imperivm::app::hash_file(scenario);
    base.map_index = core::sim::kFirstMap;
    base.difficulty = static_cast<std::uint32_t>(std::clamp(settings_.difficulty, 0, 2));
    net_front_.host = std::make_unique<imperivm::net::HostLobby>(
        *net_front_.socket, base, std::vector<core::PlayerId>{},
        imperivm::app::hash_file(vfs_.root() / "Packs" / "data.pak"),
        profile_.empty() ? std::string("host") : profile_);
    net_front_.address = imperivm::net::Endpoint{imperivm::net::local_address(),
                                                 net_front_.socket->port()}.str();
    // `HostIPName` is sized for an address alone -- its default text is
    // `127.0.0.1` -- so the port is shown only when it is not the default,
    // which a joiner's address field assumes. The clip copies it whole.
    const imperivm::net::Endpoint shown{imperivm::net::local_address(), net_front_.socket->port()};
    std::string display = shown.str();
    if (shown.port == core::sim::kDefaultNetPort) display = display.substr(0, display.rfind(':'));
    dialog->set_text("HostIPName", display);
    push_host_rows();
    std::printf("net:          hosting %s on %s\n", relative.c_str(), net_front_.address.c_str());
    std::fflush(stdout);
  }
#endif
}

std::vector<Application::LimitRow> Application::limit_rows(std::string_view victory) const {
  std::vector<LimitRow> rows;
  if (victory.empty() || install_ == nullptr) return rows;
  const auto constants = core::IniDocument::parse(install_->constants());
  if (!constants.ok()) return rows;
  const core::SectionIndex section = constants->section("GamePlay");
  if (section == core::kNoSection) return rows;
  std::string prefix(victory);
  for (char& c : prefix) c = c == ' ' ? '_' : c;
  for (int i = 0;; ++i) {
    const std::string_view value = constants->value(section, prefix + std::to_string(i));
    if (value.empty()) break;
    // The whole value goes through the table, then splits at its comma.
    const std::string translated = item_label(value, "");
    const std::size_t comma = translated.find(',');
    LimitRow row;
    row.number = translated.substr(0, comma);
    row.label = comma == std::string::npos ? translated : translated.substr(comma + 1);
    while (!row.number.empty() && row.number.back() == ' ') row.number.pop_back();
    while (!row.label.empty() && row.label.front() == ' ') row.label.erase(row.label.begin());
    rows.push_back(std::move(row));
  }
  return rows;
}

std::array<int, 3> Application::population_levels() const {
  std::array<int, 3> levels{50, 100, 150};
  if (install_ == nullptr) return levels;
  const auto constants = core::IniDocument::parse(install_->constants());
  if (!constants.ok()) return levels;
  const core::SectionIndex section = constants->section("GamePlay");
  if (section == core::kNoSection) return levels;
  levels[0] = constants->value_int(section, "LowPop", levels[0]);
  levels[1] = constants->value_int(section, "NormalPop", levels[1]);
  levels[2] = constants->value_int(section, "HighPop", levels[2]);
  return levels;
}

/// The rules onto the settings screen, as the restore 0x006f5630 puts a
/// profile back: the limit rows for the game type, gold -1 as `Default`, the
/// population as the first level at or above the kept percent.
void Application::refresh_setup_rules(core::ui::Dialog& settings) {
  const std::span<const std::string> scripts =
      install_ != nullptr ? install_->game_scripts() : std::span<const std::string>{};
  std::int32_t type = 0;
  for (std::size_t i = 0; i < scripts.size(); ++i) {
    if (scripts[i] == rules_.victory) type = static_cast<std::int32_t>(i) + 1;
  }
  if (type == 0) rules_.victory.clear();
  settings.select("GameTypeCombo", type);
  const std::vector<LimitRow> rows = limit_rows(rules_.victory);
  std::vector<std::string> labels;
  std::int32_t limit = 0;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    labels.push_back(rows[i].label);
    if (rows[i].number == rules_.threshold) limit = static_cast<std::int32_t>(i);
  }
  settings.set_items("LimitCombo", std::move(labels));
  if (!rows.empty()) {
    settings.select("LimitCombo", limit);
    rules_.threshold = rows[static_cast<std::size_t>(limit)].number;
  } else {
    rules_.threshold.clear();
  }
  const std::array<int, 3> levels = population_levels();
  std::int32_t population = 2;
  for (std::int32_t i = 0; i < 3; ++i) {
    if (levels[static_cast<std::size_t>(i)] >= rules_.world_population) {
      population = i;
      break;
    }
  }
  settings.select("WorldPopCombo", population);
  rules_.world_population = levels[static_cast<std::size_t>(population)];
  static constexpr int kGold[] = {2500, 5000, 10000, -1};
  std::int32_t gold = 3;
  for (std::int32_t i = 0; i < 4; ++i) {
    if (kGold[i] == rules_.starting_gold) gold = i;
  }
  settings.select("StartingGoldCombo", gold);
  rules_.starting_gold = kGold[gold];
  settings.set_row("NoFogCB", rules_.no_fog ? 1 : 0);
  settings.set_row("NoExplorationCB", rules_.no_exploration ? 1 : 0);
  settings.set_row("SharedSupportCB", rules_.shared_support ? 1 : 0);
  settings.set_row("SharedControlCB", rules_.shared_control ? 1 : 0);
  settings.set_row("NoBonuses", rules_.no_bonuses ? 1 : 0);
}

void Application::refresh_setup_menu(core::ui::Dialog& menu) {
  for (std::size_t i = 0; i < setup_players_.size(); ++i) {
    const SetupPlayer& player = setup_players_[i];
    const std::string suffix = "_P" + std::to_string(i + 1);
    core::ui::WidgetState& colour = menu.content().state("Color" + suffix);
    colour.has_color = true;
    colour.color = core::ui::Color{player.colour.red, player.colour.green, player.colour.blue, 255};
    // `MMENBUT.BMP`'s rows: the monitor, the face, the blank, the hand, the
    // cross -- this machine's player, another peer, an open seat, the AI, a
    // closed slot.
    static constexpr std::int32_t kTypeRow[] = {0, 3, 4, 2, 1};
    menu.set_row("PlayerType" + suffix, kTypeRow[std::clamp(player.type, 0, 4)]);
    menu.set_row("PlayerReady" + suffix, player.ready || player.type == 0 ? 1 : 0);
    menu.set_enabled("PlayerReady" + suffix, false);
    if (player.type == 0 || player.type == 3 || player.type == 4) {
      menu.set_items("PlayerName" + suffix, {});
      menu.set_text("PlayerName" + suffix, player.type == 3 ? std::string() : player.name);
    } else {
      core::ui::WidgetState& name = menu.content().state("PlayerName" + suffix);
      name.has_text = false;
      menu.set_items("PlayerName" + suffix,
                     {item_label("easy", "ai"), item_label("medium", "ai"), item_label("hard", "ai")});
      menu.select("PlayerName" + suffix, player.difficulty);
    }
    menu.select("PlayerAIRace" + suffix, player.race < 0 ? 0 : player.race + 1);
    menu.set_row("Team" + suffix, player.team);
    menu.set_row("Bonus" + suffix, player.bonus);
    // Closed, open or somebody else's: shown, not edited here.
    const bool mine = setup_mode_ == SetupMode::join ? player.type == 0
                      : setup_mode_ == SetupMode::host ? player.type != 4
                                                       : true;
    const bool live = player.type != 2 && player.type != 3 && mine;
    menu.set_enabled("PlayerName" + suffix, live);
    menu.set_enabled("PlayerAIRace" + suffix, live);
    menu.set_enabled("Team" + suffix, live);
    menu.set_enabled("Bonus" + suffix, live);
    menu.set_enabled("PlayerType" + suffix, setup_mode_ == SetupMode::local ||
                                                (setup_mode_ == SetupMode::host && player.type != 0));
  }
#if IMPERIVM_HAVE_NET
  menu.set_enabled("Start", setup_mode_ != SetupMode::host || net_front_.startable);
  if (net_front_.join != nullptr) menu.set_row("ImReadyBig", net_front_.join->choice().ready ? 1 : 0);
#endif
}

void Application::open_front_load_menu() {
  close_to_background();
  core::ui::Dialog* dialog = open_menu(
      "menuini/loadgamefrommainmenu.ini",
      [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        const auto chosen = [&]() -> std::string {
          const std::vector<std::string> names = save_names();
          const std::int32_t index = menu.selected("List");
          if (index < 0 || static_cast<std::size_t>(index) >= names.size()) return std::string();
          return names[static_cast<std::size_t>(index)];
        };
        const auto load = [&]() {
          const std::string name = chosen();
          if (!name.empty()) (void)load_from_front(saves_directory() / (name + ".bfhp"));
        };
        if (event.kind == Kind::kActivate && event.widget == "List") load();
        if (event.kind != Kind::kCommand) return;
        if (event.id == 0x1001) load();
        if (event.id == 0x1003) open_main_menu();
      });
  if (dialog == nullptr) {
    open_main_menu();
    return;
  }
  // The file declares both buttons `HIDDEN`; the game shows them.
  dialog->set_hidden("LoadBtn", false);
  dialog->set_hidden("CancelBtn", false);
  dialog->set_items("List", save_names());
  dialog->set_items("GameType", {"Saved games"});
  dialog->select("GameType", 0);
  if (!save_names().empty()) dialog->select("List", 0);
}


// --------------------------------------------------------------------------
// the campaign map
// --------------------------------------------------------------------------

void Application::open_campaign_screen(const std::filesystem::path& container_path) {
  auto screen = std::make_unique<CampaignScreen>();
  screen->container = container_path;
  screen->relative = gamedata::container_relative(vfs_.root(), container_path);
  gamedata::MapContainer container;
  std::string error;
  if (!container.open(container_path, &error)) {
    std::printf("campaign: %s\n", error.c_str());
    return;
  }
  const auto territories = container.read("territories.xml");
  auto parsed = core::sim::ConquestMap::parse(territories);
  if (!parsed.ok()) {
    std::printf("campaign: %s holds no territories.xml\n", screen->relative.c_str());
    return;
  }
  screen->map = std::move(parsed.value());
  screen->table = container_table(container);

  // The art directory, spelled `ConquestMaps/1 - GBR Europe` for a directory
  // that is `1 - GBR europe` on disk: matched case-blind.
  std::filesystem::path data = vfs_.root() / screen->map.data_path();
  if (!std::filesystem::exists(data)) {
    std::error_code ignored;
    const std::filesystem::path parent = data.parent_path();
    std::string wanted = data.filename().string();
    for (char& c : wanted) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& entry : std::filesystem::directory_iterator(parent, ignored)) {
      std::string name = entry.path().filename().string();
      for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (name == wanted) data = entry.path();
    }
  }
  screen->data_prefix = gamedata::container_relative(vfs_.root(), data) + "/";
  vfs_.mount_directory("ConquestMaps\\", vfs_.root() / "ConquestMaps");
  const auto read_file = [](const std::filesystem::path& path) {
    std::vector<std::byte> bytes;
    if (std::ifstream file(path, std::ios::binary); file) {
      std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      bytes.resize(text.size());
      std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
  };
  if (auto global = core::ui::decode_bmp(read_file(data / "global.bmp")); global.ok()) {
    screen->global = std::move(global.value());
  }
  if (auto mask = core::ui::decode_bmp_indices(read_file(data / "territories.bmp")); mask.ok()) {
    screen->mask = std::move(mask.value());
  }
  if (screen->global.empty()) {
    std::printf("campaign: no global.bmp under %s\n", data.string().c_str());
  }

  // The campaign in progress, or a fresh one: every territory enemy, and a
  // start to choose when the file says `choose="1"`.
  screen->file = gamedata::campaign_file_path(saves_directory(), screen->relative);
  core::sim::CampaignCarry carry;
  std::string why;
  if (gamedata::read_campaign_file(screen->file, carry, &why) && carry.container == screen->relative &&
      carry.progress.states.size() == screen->map.territories().size()) {
    screen->progress = carry.progress;
  } else {
    if (!why.empty()) std::printf("campaign: %s\n", why.c_str());
    screen->progress.states.assign(screen->map.territories().size(), core::sim::TerritoryState::enemy);
    screen->choosing = screen->map.choose();
  }
  if (!screen->choosing) {
    const std::vector<std::int32_t> open = gamedata::open_territories(screen->map, screen->progress);
    if (!open.empty()) screen->selected = open.front();
  }
  campaign_ = std::move(screen);

  close_to_background();
  core::ui::Dialog* dialog = open_menu(
      "menuini/conquestgame.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        CampaignScreen& screen = *campaign_;
        if (event.kind == Kind::kClick && event.widget == "GameMap") {
          // The point on the widget, back onto the art.
          const core::ui::Rect rect = menu.widget_rect("GameMap");
          if (rect.width <= 0 || rect.height <= 0 || screen.mask.empty()) return;
          const std::uint32_t art_x = static_cast<std::uint32_t>(
              static_cast<std::int64_t>(event.x) * screen.mask.width / rect.width);
          const std::uint32_t art_y = static_cast<std::uint32_t>(
              static_cast<std::int64_t>(event.y) * screen.mask.height / rect.height);
          const std::uint8_t index = screen.mask.at(art_x, art_y);
          for (std::size_t i = 0; i < screen.map.territories().size(); ++i) {
            if (screen.map.territories()[i].index == index) screen.selected = static_cast<std::int32_t>(i);
          }
          refresh_campaign_screen(menu);
          return;
        }
        if (event.kind != Kind::kCommand) return;
        switch (event.id) {
          case 0x1007: open_main_menu(); break;
          case 0x1006: (void)save_campaign_screen(); break;
          case 0x1008: {
            // The starting territory: owned, the rest enemy, and the file
            // written so that the mission's `ConquestBonus()` finds it.
            if (!screen.choosing || screen.selected < 0) break;
            screen.progress.states.assign(screen.map.territories().size(), core::sim::TerritoryState::enemy);
            screen.progress.states[static_cast<std::size_t>(screen.selected)] = core::sim::TerritoryState::owned;
            screen.progress.conquered.assign(1, screen.selected);
            screen.progress.active_bonus = screen.map.territories()[static_cast<std::size_t>(screen.selected)].bonus;
            screen.choosing = false;
            (void)save_campaign_screen();
            const std::vector<std::int32_t> open = gamedata::open_territories(screen.map, screen.progress);
            screen.selected = open.empty() ? -1 : open.front();
            refresh_campaign_screen(menu);
            break;
          }
          case 0x1005: {
            if (screen.choosing || screen.selected < 0) break;
            const std::vector<std::int32_t> open = gamedata::open_territories(screen.map, screen.progress);
            if (std::find(open.begin(), open.end(), screen.selected) == open.end()) break;
            gamedata::MapContainer container;
            if (!container.open(screen.container, nullptr)) break;
            const int number = gamedata::map_number_of_territory(container, screen.map, screen.selected);
            if (number < 0) break;
            (void)save_campaign_screen();
            settings_.difficulty = std::clamp(menu.selected("DifficultyType"), 0, 2);
            (void)save_settings();
            from_campaign_ = true;
            campaign_race_ = -1;
            if (!screen.progress.conquered.empty()) {
              const std::int32_t first = screen.progress.conquered.front();
              if (first >= 0 && static_cast<std::size_t>(first) < screen.map.territories().size()) {
                const core::sim::Territory& start =
                    screen.map.territories()[static_cast<std::size_t>(first)];
                campaign_race_ = start.interface_id;
                if (campaign_race_ < 0 && start.bonus.size() > 1 && start.bonus.front() == 'r') {
                  campaign_race_ = core::sim::race_from_name(std::string_view(start.bonus).substr(1));
                }
              }
            }
            (void)play_from_front(screen.relative, number);
            break;
          }
          default: break;
        }
      });
  if (dialog == nullptr) return;
  dialog->set_items("DifficultyType", {item_label("Easy", "Adventure difficulty level"),
                                        item_label("Normal", "Adventure difficulty level"),
                                        item_label("Hard", "Adventure difficulty level")});
  dialog->select("DifficultyType", std::clamp(settings_.difficulty, 0, 2));
  refresh_campaign_screen(*dialog);
}

bool Application::save_campaign_screen() {
  if (campaign_ == nullptr) return false;
  core::sim::CampaignCarry carry;
  carry.container = campaign_->relative;
  for (const core::sim::Territory& territory : campaign_->map.territories()) {
    carry.territories.push_back(territory.id);
  }
  carry.progress = campaign_->progress;
  std::string error;
  if (!gamedata::write_campaign_file(campaign_->file, carry, &error)) {
    std::printf("campaign: %s\n", error.c_str());
    return false;
  }
  std::printf("campaign: wrote %s\n", campaign_->file.string().c_str());
  return true;
}

void Application::refresh_campaign_screen(core::ui::Dialog& menu) {
  if (campaign_ == nullptr) return;
  CampaignScreen& screen = *campaign_;
  const auto& territories = screen.map.territories();
  const std::vector<std::int32_t> open = gamedata::open_territories(screen.map, screen.progress);

  // The art, its territories tinted by state, scaled into the widget.
  const core::ui::Rect rect = menu.widget_rect("GameMap");
  if (!screen.global.empty()) {
    core::ui::Image tinted = screen.global;
    std::int32_t owned_hue = 560, owned_sat = 1360, enemy_hue = 0, enemy_sat = 1000;
    bool owned_colorize = true, enemy_colorize = true;
    for (const auto& [name, value] : screen.map.display()) {
      if (name == "owned_hue") owned_hue = value;
      if (name == "owned_sat") owned_sat = value;
      if (name == "enemy_hue") enemy_hue = value;
      if (name == "enemy_sat") enemy_sat = value;
      if (name == "owned_colorize") owned_colorize = value != 0;
      if (name == "enemy_colorize") enemy_colorize = value != 0;
    }
    std::vector<core::ui::IndexTint> tints;
    for (std::size_t i = 0; i < territories.size(); ++i) {
      const bool owned = screen.progress.states[i] == core::sim::TerritoryState::owned;
      const bool reachable = screen.choosing ||
                             std::find(open.begin(), open.end(), static_cast<std::int32_t>(i)) != open.end();
      core::ui::IndexTint tint;
      tint.index = static_cast<std::uint8_t>(territories[i].index);
      if (owned && owned_colorize) {
        tint.hue = owned_hue;
        tint.saturation = owned_sat;
        tints.push_back(tint);
      } else if (!owned && reachable && enemy_colorize) {
        tint.hue = enemy_hue;
        tint.saturation = enemy_sat;
        tints.push_back(tint);
      }
    }
    core::ui::tint_by_index(tinted, screen.mask, tints);
    screen.view = core::ui::resample_box(tinted, static_cast<std::uint32_t>(std::max(0, rect.width)),
                                         static_cast<std::uint32_t>(std::max(0, rect.height)));
    menu.content().state("GameMap").bitmap = &screen.view;
  }

  // The description of the selected territory, or the start-area prompt.
  const bool has_selection = screen.selected >= 0 && static_cast<std::size_t>(screen.selected) < territories.size();
  const core::sim::Territory* chosen = has_selection ? &territories[static_cast<std::size_t>(screen.selected)] : nullptr;
  menu.set_text("DescriptionTitle", chosen != nullptr ? localised(screen.table, chosen->visual_name) : "");
  menu.set_text("DescriptionText", chosen != nullptr ? localised(screen.table, chosen->description) : "");
  menu.set_text("BonusText", chosen != nullptr ? localised(screen.table, chosen->bonus_description) : "");
  menu.set_hidden("SelectStartAreaText", !screen.choosing);
  menu.set_hidden("SelectStartAreaTextDesc", !screen.choosing);
  menu.set_hidden("BonusTitle", screen.choosing);
  menu.set_hidden("BonusText", screen.choosing);
  menu.set_hidden("Select", !screen.choosing);
  menu.set_hidden("Start", screen.choosing);
  const bool startable = chosen != nullptr && !screen.choosing &&
                         std::find(open.begin(), open.end(), screen.selected) != open.end();
  menu.set_enabled("Start", startable);
  menu.set_enabled("Select", screen.choosing && chosen != nullptr);
  menu.set_text("ShieldsFrame", localised(screen.table, screen.map.name()));
  // The legend shares the description's rectangles and shows in its place
  // while no territory is selected (its third entry is commented out of
  // the file's object list). **Reading, labelled.**
  for (const char* name : {"LegendTitle", "LegendIcon1", "LegendText1", "LegendIcon2", "LegendText2"}) {
    menu.set_hidden(name, chosen != nullptr);
  }
  menu.set_hidden("DescriptionTitle", chosen == nullptr);
  menu.set_hidden("DescriptionText", chosen == nullptr);

  // The road of conquest: the first territory's shield, the arrow, then
  // the rest in the order they fell.
  const auto shield_of = [&](std::int32_t territory) -> std::string {
    if (territory < 0 || static_cast<std::size_t>(territory) >= territories.size()) return std::string();
    // `%s/Shield%s.bmp` takes the territory's *id*: `ShieldItaly.bmp` for
    // the territory whose map is named `Rome`.
    return screen.data_prefix + "Shield" + territories[static_cast<std::size_t>(territory)].id + ".bmp";
  };
  const std::vector<std::int32_t>& road = screen.progress.conquered;
  menu.content().state("Shield1").image = road.empty() ? std::string() : shield_of(road[0]);
  menu.content().state("ShieldArrow").image = road.empty() ? std::string() : screen.data_prefix + "ShieldArrow.bmp";
  for (std::size_t i = 1; i < 7; ++i) {
    const std::string name = "Shield" + std::to_string(i + 1);
    menu.content().state(name).image = i < road.size() ? shield_of(road[i]) : std::string();
  }
  menu.content().state("GameMap").hidden = false;
  menu.set_hidden("GameMap", false);
}

void Application::play_sound(std::string_view path) {
  if (!sound_type_on(imperivm::sound::SoundType::ambient2)) {
    std::printf("sound     %.*s (nature sounds are off)\n", static_cast<int>(path.size()), path.data());
    std::fflush(stdout);
    return;
  }
  const bool played =
      play_sound_value(path, imperivm::sound::kPriorityUI, imperivm::sound::SoundType::ambient2, nullptr);
  std::printf("sound     %.*s%s\n", static_cast<int>(path.size()), path.data(), played ? "" : " -- not played");
  std::fflush(stdout);
}

bool Application::sound_type_on(imperivm::sound::SoundType type) const {
  using imperivm::sound::SoundType;
  switch (type) {
    case SoundType::music: return settings_.music && sound_config_.music;
    case SoundType::ambient:
    case SoundType::ambient2: return settings_.nature_sounds;
    case SoundType::unit_order:
    case SoundType::unit_idle: return settings_.speech;
    case SoundType::unit_fight:
    case SoundType::unit_walk:
    case SoundType::ui:
    case SoundType::select: return settings_.sound_fx;
    // Set by no option; on at 100.
    case SoundType::conv_speech: return true;
    case SoundType::none: break;
  }
  return false;
}

std::int32_t Application::sound_type_volume(imperivm::sound::SoundType type) const {
  using imperivm::sound::SoundType;
  switch (type) {
    case SoundType::music: return settings_.music_volume;
    case SoundType::unit_order:
    case SoundType::unit_idle: return settings_.speech_volume;
    case SoundType::conv_speech: return 100;
    default: return settings_.sound_volume;
  }
}

void Application::start_sound() {
  sound_log_ = std::getenv("IMPERIVM_SOUND_LOG") != nullptr && std::string_view(std::getenv("IMPERIVM_SOUND_LOG")) != "0";
  // `config.ini` is LZIS; unpacked, it is an INI like any other.
  std::vector<std::byte> packed;
  if (std::ifstream file(vfs_.root() / "config.ini", std::ios::binary); file) {
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    packed.resize(text.size());
    std::memcpy(packed.data(), text.data(), text.size());
  }
  std::vector<std::byte> plain;
  if (const auto header = core::parse_lzis_header(packed)) {
    plain.resize(header->uncompressed_size);
    if (!core::lzis_decompress(packed, plain)) plain.clear();
  } else {
    plain = packed;
  }
  if (!plain.empty()) {
    sound_config_ = imperivm::sound::parse_sound_config(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size()));
  }
  // One pool a type, at the type's number; music, both ambiences and
  // conversation speech steal their last channel when full, everything else
  // is dropped (0x006b0450; the steal flag is set for types 1, 2, 8 and 9 at
  // 0x006b09f4--0x006b0a43).
  std::vector<imperivm::sound::PoolSpec> pools;
  for (std::size_t t = 0; t < imperivm::sound::kSoundTypes; ++t) {
    const auto type = static_cast<imperivm::sound::SoundType>(t);
    pools.push_back(imperivm::sound::PoolSpec{
        static_cast<std::size_t>(std::max(0, sound_config_.channels[t])),
        type == imperivm::sound::SoundType::music || type == imperivm::sound::SoundType::ambient ||
            type == imperivm::sound::SoundType::ambient2 || type == imperivm::sound::SoundType::conv_speech});
  }
  audio_.configure(std::move(pools));
}

imperivm::sound::View Application::sound_view() const {
  imperivm::sound::View view;
  const std::int32_t w = camera_.width;
  const std::int32_t h = camera_.height;
  const core::sim::Point corners[] = {camera_.unproject(0, 0), camera_.unproject(w, 0), camera_.unproject(0, h),
                                      camera_.unproject(w, h)};
  view.left = view.right = corners[0].x;
  view.top = view.bottom = corners[0].y;
  for (const core::sim::Point& c : corners) {
    view.left = std::min(view.left, c.x);
    view.right = std::max(view.right, c.x);
    view.top = std::min(view.top, c.y);
    view.bottom = std::max(view.bottom, c.y);
  }
  // Screen x is world x less the camera's (the projection keeps x).
  view.screen_left = camera_.x;
  view.screen_right = camera_.x + w;
  // **Inferred**: the options' switch is the one applied last (0x006b08f0
  // from 0x006e7210), over `config.ini`'s.
  view.reverse_speakers = settings_.reverse_speakers;
  return view;
}

bool Application::play_sound_value(std::string_view value, std::uint16_t priority, imperivm::sound::SoundType type,
                                   const core::sim::Point* at, std::int32_t percent) {
  (void)priority;  // stored by the original on every sound, and read by nothing found
  if (!sound_config_.sound) return false;
  imperivm::sound::SoundEntity* entity = sound_bank_.entity(value);
  if (entity == nullptr) return false;
  const imperivm::sound::SoundType kind = type != imperivm::sound::SoundType::none ? type : entity->type;
  if (kind == imperivm::sound::SoundType::none || !sound_type_on(kind)) return false;
  const std::size_t pool = static_cast<std::size_t>(kind);
  // A pool that drops is asked before the draw, so a dropped sound does not
  // move the no-repeat rule on (0x006b09f4).
  if (audio_.full(pool)) return false;
  const std::optional<std::size_t> pick = imperivm::sound::choose_variant(*entity, variant_random_);
  if (!pick) return false;
  const std::string& file = entity->variants[*pick].file;
  std::int32_t attenuation = 0;
  std::int32_t pan = 0;
  if (at != nullptr) {
    const imperivm::sound::View view = sound_view();
    attenuation = imperivm::sound::attenuation(view, at->x, at->y);
    pan = imperivm::sound::pan(view, at->x);
  }
  imperivm::sound::PlayParams params;
  params.volume = imperivm::sound::volume(attenuation, percent, sound_type_volume(kind));
  params.pan = pan;
  params.pool = pool;
  imperivm::sound::Placed placed;
  if (at != nullptr) placed = imperivm::sound::Placed{true, at->x, at->y};
  std::string why;
  const bool played =
      audio_.play(file, audio_.knows(file) ? platform::ByteSpan() : vfs_.read(file), params, &why, placed);
  if (sound_log_) {
    // Quoted where a name has a space in it, so the line splits as a shell's.
    const auto word = [](std::string_view text) {
      return text.find(' ') == std::string_view::npos ? std::string(text) : "\"" + std::string(text) + "\"";
    };
    if (played) {
      std::printf("sound: %s %s vol %d pan %d\n", word(entity->name).c_str(), word(file).c_str(),
                  static_cast<int>(params.volume), static_cast<int>(params.pan));
    } else {
      std::printf("sound: %s %s not played: %s\n", word(entity->name).c_str(), word(file).c_str(), why.c_str());
    }
    std::fflush(stdout);
  }
  return played;
}

void Application::acknowledge_order() {
  if (session_ == nullptr) return;
  const core::sim::World& world = session_->world();
  const core::ClassGraph* graph = world.class_graph();
  const std::span<const core::ObjectId> selected = session_->selections().player(local_player_).ids();
  if (graph == nullptr || selected.empty()) return;
  // One of the selection, uniformly (0x005e61db, 0x005e71ae).
  const core::ObjectId chosen = selected[static_cast<std::size_t>(
      speaker_random_.between(0, static_cast<std::int32_t>(selected.size()) - 1))];
  const core::sim::WorldObject* group = world.find(chosen);
  if (group == nullptr) return;
  // `Talk` (0x005e36e0): a hero with an army lets one of it speak for him
  // six times in ten -- a roll of 0..100 above 40 -- at his own position.
  // **Inferred**: the original tests a flag of a squad-like object and its
  // member count; here, a hero and his army, which is what a hero's squad is.
  const core::sim::WorldObject* speaker = group;
  if (const core::sim::HeroSystem* heroes = core::sim::hero_system_of(session_->world()); heroes != nullptr) {
    if (const core::sim::HeroRecord* hero = heroes->hero(chosen); hero != nullptr && !hero->army.empty()) {
      if (speaker_random_.between(0, 100) > 40) {
        const core::ObjectId member = hero->army[static_cast<std::size_t>(
            speaker_random_.between(0, static_cast<std::int32_t>(hero->army.size()) - 1))];
        if (const core::sim::WorldObject* m = world.find(member)) speaker = m;
      }
    }
  }
  if (speaker->class_index == core::kNoClass) return;
  std::string_view command;
  for (const core::ClassProperty& sound : graph->resolved_sounds(speaker->class_index)) {
    if (sound.key == "command") command = sound.value;
  }
  if (command.empty()) return;
  const core::sim::Point at = group->state.position;
  (void)play_sound_value(command, imperivm::sound::kPriorityUnitOrder, imperivm::sound::SoundType::unit_order, &at);
}

void Application::play_click(std::int32_t control_id) {
  // The one control that does not click (0x006b0d2a).
  if ((control_id & 0xFFFF) == 0xBEDA) return;
  (void)play_sound_value("Sounds/UI/click.wav", imperivm::sound::kPriorityUI, imperivm::sound::SoundType::ui, nullptr);
}


/// A name for a `sound:`-style log line: quoted where it has a space in it,
/// so the line splits as a shell's.
std::string sound_word(std::string_view text) {
  return text.find(' ') == std::string_view::npos ? std::string(text) : "\"" + std::string(text) + "\"";
}

/// A path of the installation's own directories, `/` or `\` between its
/// parts, each part matched without regard to case as Windows matches it.
/// Empty when there is no such file.
std::filesystem::path find_loose_file(const std::filesystem::path& root, std::string_view relative) {
  std::filesystem::path at = root;
  std::size_t start = 0;
  while (start <= relative.size()) {
    std::size_t end = relative.find_first_of("/\\", start);
    if (end == std::string_view::npos) end = relative.size();
    const std::string part(relative.substr(start, end - start));
    start = end + 1;
    if (part.empty()) continue;
    std::error_code ec;
    if (std::filesystem::exists(at / part, ec)) {
      at /= part;
      continue;
    }
    const auto lower = [](std::string text) {
      for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return text;
    };
    std::filesystem::path found;
    for (const auto& entry : std::filesystem::directory_iterator(at, ec)) {
      if (lower(entry.path().filename().string()) == lower(part)) {
        found = entry.path();
        break;
      }
    }
    if (found.empty()) return {};
    at = found;
  }
  return at;
}


bool Application::play_music(std::string_view path) {
  using imperivm::sound::SoundType;
  // `Sound=0` never starts the manager; a type switched off plays nothing
  // (0x006b0910), and says nothing either, as every type does here.
  if (!sound_config_.sound || !sound_type_on(SoundType::music)) return false;
  imperivm::sound::PlayParams params;
  // Unplaced (x = -1), so the slider alone: `volume(0, 100, MusicVolume)`.
  params.volume = imperivm::sound::volume(0, 100, sound_type_volume(SoundType::music));
  params.pool = static_cast<std::size_t>(SoundType::music);
  std::string why = "no such file";
  bool played = false;
  const std::filesystem::path file = find_loose_file(vfs_.root(), path);
  if (!file.empty()) {
    // The compressed file whole, read where it lies; it is decoded as it
    // plays (`sound::OggStream`).
    std::vector<std::uint8_t> bytes;
    if (std::ifstream in(file, std::ios::binary); in) {
      bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    played = audio_.play_stream(path, std::move(bytes), params, &why);
  }
  if (sound_log_) {
    if (played) {
      std::printf("music: %s vol %d\n", sound_word(path).c_str(), static_cast<int>(params.volume));
    } else {
      std::printf("music: %s not played: %s\n", sound_word(path).c_str(), why.c_str());
    }
    std::fflush(stdout);
  }
  return played;
}

void Application::stop_sounds() {
  audio_.stop_all();
  sound_view_placed_.reset();
}

void Application::tick_music() {
  using imperivm::sound::SoundType;
  const std::size_t music = static_cast<std::size_t>(SoundType::music);
  if (front_mode_) {
    // The menus (0x00748600): a flag raised as they are entered asks for
    // `PregameUIMusic`. Once in `kMenuMusicPasses` passes, while it stands,
    // an idle `Music` channel gets the file; a play that succeeds lowers the
    // flag, and so does a `CONST.INI` without the key (0x00748808--
    // 0x0074885e). So it plays once each time the menus are entered, and is
    // not started again when it ends: **read**, against `sound.md`'s earlier
    // reading that it loops.
    if (menu_music_wanted_) {
      if (menu_music_countdown_ <= 0) {
        menu_music_countdown_ = imperivm::sound::kMenuMusicPasses;
        if (audio_.playing(music) == 0) {
          if (!menu_music_.has_value()) {
            menu_music_ = std::string();
            if (ensure_installation()) {
              if (const auto constants = core::IniDocument::parse(install_->constants()); constants.ok()) {
                const core::SectionIndex section = constants->section("GamePlay");
                if (section != core::kNoSection) menu_music_ = std::string(constants->value(section, "PregameUIMusic"));
              }
            }
          }
          if (menu_music_->empty() || play_music(*menu_music_)) menu_music_wanted_ = false;
        }
      }
      --menu_music_countdown_;
    }
    return;
  }
  if (!play_mode_ || session_ == nullptr) return;
  // The match's player (0x00551150, 0x00550f90): every two seconds of game
  // time, if the `Music` channel is idle and the clock is past 100 ms, a
  // track other than the last.
  const std::int64_t now = static_cast<std::int64_t>(session_->world().time());
  if (now < music_next_look_) return;
  music_next_look_ += imperivm::sound::kMusicCheckMs * (1 + (now - music_next_look_) / imperivm::sound::kMusicCheckMs);
  if (now <= imperivm::sound::kMusicStartMs || audio_.playing(music) != 0) return;
  const std::optional<std::size_t> pick = imperivm::sound::next_track(music_tracks_.size(), music_last_, music_random_);
  if (!pick) return;
  (void)play_music("music/" + music_tracks_[*pick]);
  // Kept whether or not it played (0x00550f82), except for a single track,
  // which is played with no draw and leaves it (0x00550ed9).
  if (music_tracks_.size() > 1) music_last_ = pick;
}

void Application::follow_view(bool force) {
  const imperivm::sound::View view = sound_view();
  const auto same = [](const imperivm::sound::View& a, const imperivm::sound::View& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom &&
           a.screen_left == b.screen_left && a.screen_right == b.screen_right &&
           a.reverse_speakers == b.reverse_speakers;
  };
  if (!force && sound_view_placed_.has_value() && same(*sound_view_placed_, view)) return;
  const bool first = !sound_view_placed_.has_value();
  sound_view_placed_ = view;
  if (first && !force) return;
  // Each of the first 64 live channels takes the level and pan its point
  // has from the view as it now is (0x006b0810).
  for (const platform::Audio::Live& live : audio_.live()) {
    if (static_cast<std::size_t>(live.voice) >= imperivm::sound::kFollowedChannels) break;
    const auto type = static_cast<imperivm::sound::SoundType>(live.pool);
    const imperivm::sound::Followed now = imperivm::sound::follow(view, live.placed, sound_type_volume(type));
    const std::int32_t pan = now.pans ? now.pan : live.pan;
    if (now.volume == live.volume && pan == live.pan) continue;
    if (!audio_.set_voice(live.voice, now.volume, pan)) continue;
    if (sound_log_) {
      std::printf("repan: %s vol %d pan %d\n", sound_word(live.name).c_str(), static_cast<int>(now.volume),
                  static_cast<int>(pan));
      std::fflush(stdout);
    }
  }
}

void Application::play_select_sounds(const std::vector<core::ObjectId>& before) {
  if (session_ == nullptr) return;
  const core::sim::World& world = session_->world();
  const core::ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return;
  // The original adds the objects one at a time (0x005e7d80), and each one
  // the local player controls -- the owner's relation word towards it has
  // bit 5, `share_control`, which a player always grants itself -- and that
  // is not a spawn template speaks its class's `select` value as it goes in
  // (`PlaySelectSound`, 0x005e2f80: priority 4500, type `Select`, at its
  // position). The one `Select` channel drops every voice after the first
  // while that one plays, so of a band the first such object is heard.
  // **Not reproduced:** the object's own visibility test (vtable +0x74,
  // with the players' table) before it; an object the player controls is
  // one the player sees. Only buildings and decor declare `select`.
  const std::span<const core::ObjectId> now = session_->selections().player(local_player_).ids();
  for (const core::ObjectId id : now) {
    if (std::find(before.begin(), before.end(), id) != before.end()) continue;
    const core::sim::WorldObject* object = world.find(id);
    if (object == nullptr || object->class_index == core::kNoClass || object->state.flags.unspawned) continue;
    if (!world.players().has(object->state.owner, local_player_, core::sim::Relation::share_control)) continue;
    std::string_view select;
    for (const core::ClassProperty& sound : graph->resolved_sounds(object->class_index)) {
      if (sound.key == "select") select = sound.value;
    }
    if (select.empty()) continue;
    const core::sim::Point at = object->state.position;
    (void)play_sound_value(select, imperivm::sound::kPrioritySelect, imperivm::sound::SoundType::select, &at);
  }
}

std::string Application::seat_name(core::PlayerId seat, const core::sim::PlayerSetup& setup) const {
  std::string name = setup.name.empty() ? "Player " + std::to_string(seat + 1) : setup.name;
#if IMPERIVM_HAVE_NET
  if (net_ != nullptr) name = net_->display_name(seat, name);
#endif
  return name;
}

#if IMPERIVM_HAVE_NET
// --------------------------------------------------------------------------
// the networked front: MPMENU.INI, the lobby, MULTI.INI
// --------------------------------------------------------------------------

/// `MULTI.INI`: one message and OK. The file's own comment calls its name
/// inappropriate; it is the multiplayer message box, and Quit is not shown
/// because leaving the game from a message is the menu's job.
void Application::open_multi_box(const std::string& message, std::function<void()> ok) {
  core::ui::Dialog* box = open_menu(
      "menuini/multi.ini", [ok](const core::ui::DialogEvent& event, core::ui::Dialog&) {
        if (event.kind == core::ui::DialogEvent::Kind::kCommand && event.id == 0x1001 && ok) ok();
      });
  if (box == nullptr) {
    std::printf("net: %s\n", message.c_str());
    if (ok) ok();
    return;
  }
  box->set_text("Message", message);
  box->set_hidden("QuitBtn", true);
}

void Application::end_net_front() {
  net_front_ = NetFront{};
  if (setup_mode_ != SetupMode::local) setup_mode_ = SetupMode::local;
}

/// `MPMENU.INI`: the LAN's open games, an address to type, Join and Host.
/// GameSpy is disabled, as the main menu's online buttons are; Lan and
/// Refresh ask the LAN again.
void Application::open_mp_menu() {
  end_net_front();
  scenario_for_host_ = false;
  close_to_background();
  std::string error;
  std::optional<imperivm::net::UdpSocket> socket = imperivm::net::UdpSocket::open(0, false, &error);
  if (socket.has_value()) {
    net_front_.role = NetFront::Role::browse;
    net_front_.socket = std::make_unique<imperivm::net::UdpSocket>(std::move(*socket));
    net_front_.browser = std::make_unique<imperivm::net::LanBrowser>(*net_front_.socket);
    net_front_.browser->refresh(imperivm::net::now_ms());
    net_front_.last_refresh = imperivm::net::now_ms();
  }
  core::ui::Dialog* menu = open_menu(
      "menuini/mpmenu.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        const auto describe_selected = [this, &menu]() {
          const std::int32_t index = menu.selected("List");
          if (net_front_.browser == nullptr || index < 0 ||
              static_cast<std::size_t>(index) >= net_front_.browser->games().size()) {
            menu.set_text("InfoText",
                          "Games on this network are listed below. To join one elsewhere, type "
                          "its host's address -- shown on the host's players' screen -- and "
                          "press Join.");
            return;
          }
          const auto& game = net_front_.browser->games()[static_cast<std::size_t>(index)];
          menu.set_text("InfoText", "Host: " + game.advert.host + "\nMap: " + game.advert.map +
                                        "\nAddress: " + game.at.str() + "\nFree seats: " +
                                        std::to_string(game.advert.open) + " of " +
                                        std::to_string(game.advert.seats));
        };
        if (event.kind == Kind::kSelect && event.widget == "List") describe_selected();
        if (event.kind != Kind::kCommand && event.kind != Kind::kActivate) return;
        const bool join = (event.kind == Kind::kActivate && event.widget == "List") ||
                          (event.kind == Kind::kCommand && event.id == 0x1002);
        if (event.kind == Kind::kCommand && (event.id == 0x1004 || event.id == 0x1008)) {
          if (net_front_.browser != nullptr) {
            net_front_.browser->refresh(imperivm::net::now_ms());
            net_front_.last_refresh = imperivm::net::now_ms();
          }
          return;
        }
        if (event.kind == Kind::kCommand && event.id == 0x1005) {
          end_net_front();
          open_main_menu();
          return;
        }
        if (event.kind == Kind::kCommand && event.id == 0x1003) {
          end_net_front();
          scenario_for_host_ = true;
          open_scenario_menu(false);
          return;
        }
        if (!join) return;
        // A typed address wins; otherwise the game chosen in the list.
        std::optional<imperivm::net::Endpoint> host;
        const std::string typed = menu.text("InetHostCombo");
        if (!typed.empty()) {
          host = imperivm::net::resolve(typed, core::sim::kDefaultNetPort);
        } else if (net_front_.browser != nullptr) {
          const std::int32_t index = menu.selected("List");
          if (index >= 0 && static_cast<std::size_t>(index) < net_front_.browser->games().size()) {
            host = net_front_.browser->games()[static_cast<std::size_t>(index)].at;
          }
        }
        if (!host.has_value() || net_front_.socket == nullptr) {
          menu.set_text("InfoText", typed.empty() ? "Choose a game, or type its host's address."
                                                  : "Cannot find " + typed + ".");
          return;
        }
        core::sim::Hello hello;
        hello.content = imperivm::app::hash_file(vfs_.root() / "Packs" / "data.pak");
        hello.name = profile_.empty() ? std::string("player") : profile_;
        net_front_.browser.reset();
        net_front_.role = NetFront::Role::join;
        net_front_.join =
            std::make_unique<imperivm::net::JoinLobby>(*net_front_.socket, *host, hello);
        net_front_.address = host->str();
        open_multi_box("Connecting to " + host->str() + "...", [this] {
          if (net_front_.join != nullptr) net_front_.join->leave(core::sim::Refuse::Reason::left);
          open_mp_menu();
        });
      });
  if (menu == nullptr) {
    open_main_menu();
    return;
  }
  menu->set_enabled("GameSpy", false);
  for (const char* name : {"ListThinFrame2", "List2", "List2.ScrollUp", "List2.ScrollDown",
                           "List2.VScrollBack", "List2.VScroll", "GameSpyBackground"}) {
    menu->set_hidden(name, true);
  }
  menu->set_items("List", {});
  menu->set_text("InfoText",
                 "Games on this network are listed below. To join one elsewhere, type its "
                 "host's address -- shown on the host's players' screen -- and press Join.");
}

void Application::push_host_rows() {
  if (net_front_.host == nullptr) return;
  std::vector<core::sim::SeatRow>& rows = net_front_.host->rows();
  rows.clear();
  for (const SetupPlayer& player : setup_players_) {
    core::sim::SeatRow row;
    row.slot = player.slot;
    row.type = player.type == 0 || player.type == 4 ? core::sim::SeatRow::Type::human
               : player.type == 1                   ? core::sim::SeatRow::Type::computer
               : player.type == 2                   ? core::sim::SeatRow::Type::closed
                                                    : core::sim::SeatRow::Type::open;
    row.name = player.name;
    row.race = player.race;
    row.difficulty = static_cast<std::uint8_t>(std::clamp(player.difficulty, 0, 2));
    row.team = static_cast<std::uint8_t>(std::clamp(player.team, 0, 4));
    row.bonus = static_cast<std::uint8_t>(std::clamp(player.bonus, 0, 3));
    row.ready = player.type == 0 || player.ready;
    rows.push_back(std::move(row));
  }
  core::sim::LobbyRules& r = net_front_.host->rules();
  r.victory = rules_.victory;
  r.threshold = rules_.threshold;
  r.world_population = rules_.world_population;
  r.starting_gold = rules_.starting_gold;
  r.fog_of_war = !rules_.no_fog;
  r.exploration = !rules_.no_exploration;
  r.bonuses = !rules_.no_bonuses;
  r.shared_support = rules_.shared_support;
  r.shared_control = rules_.shared_control;
}

void Application::pull_lobby_rows(const std::vector<core::sim::SeatRow>& rows, core::PlayerId you) {
  for (const core::sim::SeatRow& row : rows) {
    for (SetupPlayer& player : setup_players_) {
      if (player.slot != row.slot) continue;
      const bool human = row.type == core::sim::SeatRow::Type::human;
      player.type = human ? (row.slot == you ? 0 : 4)
                    : row.type == core::sim::SeatRow::Type::computer ? 1
                    : row.type == core::sim::SeatRow::Type::closed   ? 2
                                                                     : 3;
      player.name = row.name;
      // This peer's own row keeps what it chose; the rest is the host's word.
      if (row.slot != you || setup_mode_ == SetupMode::host) {
        player.race = row.race;
        player.team = row.team;
        player.bonus = row.bonus;
      }
      player.difficulty = row.difficulty;
      player.ready = row.ready;
    }
  }
}

/// Every frame the front is up: the list, the host's lobby, or the joiner's.
void Application::step_net_lobby() {
  if (net_front_.role == NetFront::Role::none) return;
  const std::uint32_t now = imperivm::net::now_ms();
  const auto dialog_named = [this](std::string_view file) -> core::ui::Dialog* {
    for (std::size_t i = ui_.dialog_count(); i-- > 0;) {
      core::ui::Dialog* dialog = ui_.dialog(i);
      if (dialog != nullptr && fold_name(dialog->screen().path).find(file) != std::string::npos) return dialog;
    }
    return nullptr;
  };

  if (net_front_.role == NetFront::Role::browse && net_front_.browser != nullptr) {
    if (now - net_front_.last_refresh > 3000) {
      net_front_.browser->refresh(now);
      net_front_.last_refresh = now;
    }
    net_front_.browser->step(now);
    std::vector<std::string> items;
    for (const auto& game : net_front_.browser->games()) {
      items.push_back(game.advert.host + " - " + game.advert.map + " (" +
                      std::to_string(game.advert.open) + "/" + std::to_string(game.advert.seats) + ")");
    }
    std::string listed;
    for (const std::string& item : items) listed += item + "\n";
    if (listed != net_front_.listed) {
      net_front_.listed = listed;
      if (core::ui::Dialog* menu = dialog_named("mpmenu.ini")) {
        const std::int32_t chosen = menu->selected("List");
        menu->set_items("List", std::move(items));
        if (chosen >= 0) menu->select("List", chosen);
      }
    }
    return;
  }

  if (net_front_.role == NetFront::Role::host && net_front_.host != nullptr) {
    const bool startable = net_front_.host->step(now);
    pull_lobby_rows(net_front_.host->rows(), net_front_.host->rows().empty()
                                                  ? core::kNoPlayer
                                                  : std::find_if(setup_players_.begin(), setup_players_.end(),
                                                                 [](const SetupPlayer& p) { return p.type == 0; })->slot);
    net_front_.startable = startable;
    if (core::ui::Dialog* menu = dialog_named("mpgamemenu.ini")) refresh_setup_menu(*menu);
    show_lobby_chat(net_front_.host->chat(), dialog_named("mpchat.ini"));
    return;
  }

  if (net_front_.role == NetFront::Role::join && net_front_.join != nullptr) {
    const imperivm::net::JoinLobby::State state = net_front_.join->step(now);
    if (state == imperivm::net::JoinLobby::State::refused) {
      const std::string why = net_front_.join->result().error;
      end_net_front();
      close_all_menus();
      open_menu("menuini/menuback.ini", [](const core::ui::DialogEvent&, core::ui::Dialog&) {});
      open_multi_box(why.empty() ? std::string("The host closed the game.") : why, [this] { open_mp_menu(); });
      return;
    }
    if (state == imperivm::net::JoinLobby::State::started) {
      imperivm::net::Lobby lobby = net_front_.join->result();
      std::unique_ptr<imperivm::net::UdpSocket> socket = std::move(net_front_.socket);
      end_net_front();
      play_net_lobby(std::move(socket), std::move(lobby));
      return;
    }
    if (state != imperivm::net::JoinLobby::State::seated) return;
    const core::sim::Start& roster = net_front_.join->roster();
    if (!net_front_.seated) {
      // The map the host named must be here, byte for byte, or this peer
      // leaves and says why -- before a desync can say it instead.
      const std::filesystem::path map = vfs_.root() / roster.map;
      if (roster.map.empty() || imperivm::app::hash_file(map) != roster.map_hash) {
        net_front_.join->leave(core::sim::Refuse::Reason::map);
        const std::string name = roster.map;
        end_net_front();
        close_all_menus();
        open_menu("menuini/menuback.ini", [](const core::ui::DialogEvent&, core::ui::Dialog&) {});
        open_multi_box("The host's map, " + name + ", is not installed here, or differs from yours.",
                       [this] { open_mp_menu(); });
        return;
      }
      net_front_.seated = true;
      open_setup_menu(map, SetupMode::join);
    }
    pull_lobby_rows(roster.rows, roster.you);
    const core::sim::LobbyRules& r = roster.rules;
    rules_.victory = r.victory;
    rules_.threshold = r.threshold;
    rules_.world_population = r.world_population;
    rules_.starting_gold = r.starting_gold;
    rules_.no_fog = !r.fog_of_war;
    rules_.no_exploration = !r.exploration;
    rules_.no_bonuses = !r.bonuses;
    rules_.shared_support = r.shared_support;
    rules_.shared_control = r.shared_control;
    if (core::ui::Dialog* menu = dialog_named("mpgamemenu.ini")) refresh_setup_menu(*menu);
    if (core::ui::Dialog* settings = dialog_named("settings.ini")) refresh_setup_rules(*settings);
    show_lobby_chat(net_front_.join->chat(), dialog_named("mpchat.ini"));
  }
}

void Application::show_lobby_chat(const std::vector<core::sim::ChatLine>& log, core::ui::Dialog* dialog) {
  // A roster carries the log's last lines, so what changed is told by the
  // last line, not the count: its speaker and number.
  std::size_t mark = log.size();
  if (!log.empty()) mark = (static_cast<std::size_t>(log.back().from) << 32) ^ log.back().seq ^ (log.size() << 48);
  if (mark == lobby_chat_shown_) return;
  lobby_chat_shown_ = mark;
  std::string text;
  const auto name = [this](core::PlayerId slot) {
    for (const SetupPlayer& row : setup_players_) {
      if (row.slot == slot && !row.name.empty()) return row.name;
    }
    return "Player " + std::to_string(static_cast<unsigned>(slot) + 1);
  };
  for (const core::sim::ChatLine& line : log) {
    text += name(line.from) + ": " + line.text + "\n";
  }
  if (!log.empty()) {
    std::printf("lobby     %s: %s\n", name(log.back().from).c_str(), log.back().text.c_str());
    std::fflush(stdout);
  }
  if (dialog != nullptr) dialog->set_text("ChatLog", text);
}

void Application::play_net_lobby(std::unique_ptr<imperivm::net::UdpSocket> socket,
                                 imperivm::net::Lobby lobby) {
  const core::sim::Start start = lobby.start;
  net_ = std::make_unique<imperivm::app::NetPlay>(args_.net, std::move(socket), std::move(lobby));
  net_departed_shown_ = false;
  seed_ = start.seed;
  args_.player = start.you;
  std::printf("net:          playing %s as player %u, seed %u\n", start.map.c_str(),
              static_cast<unsigned>(start.you), start.seed);
  std::fflush(stdout);
  std::string error;
  if (!receive_late_state(vfs_.root(), &error)) {
    std::printf("net:          %s\n", error.c_str());
    net_.reset();
    close_all_menus();
    open_menu("menuini/menuback.ini", [](const core::ui::DialogEvent&, core::ui::Dialog&) {});
    open_multi_box(error, [this] { open_mp_menu(); });
    return;
  }
  (void)play_from_front(start.map, static_cast<int>(start.map_index));
}

/// `INGAMECHAT.INI`: the line, whom it is for -- *Send to all* (0x20101),
/// *Send to allies* (0x20102), *Send to player* (0x20103) with
/// `PlayerCombo` (0x2009) naming whom -- and *Send* (0x1001), *Send
/// location* (0x1007, the line and the point at the middle of the view),
/// *Cancel* (0x1002). Chat is not an input (`sim/netchat.hpp`): nothing here
/// touches the session.
void Application::open_ingame_chat() {
  if (net_ == nullptr || ui_.dialog_count() > 0) return;
  struct State {
    core::sim::ChatLine::To to = core::sim::ChatLine::To::all;
    std::vector<core::PlayerId> players;
    std::int32_t chosen = 0;
  };
  auto state = std::make_shared<State>();
  for (const core::PlayerId seat : net_->start().config.peers) {
    if (seat != net_->seat()) state->players.push_back(seat);
  }
  core::ui::Dialog* dialog = open_menu(
      "menuini/ingamechat.ini", [this, state](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        if (event.kind == Kind::kChange && event.widget == "PlayerCombo") {
          state->chosen = event.index;
          return;
        }
        if (event.kind != Kind::kCommand) return;
        switch (event.id) {
          case 0x20101: state->to = core::sim::ChatLine::To::all; return;
          case 0x20102: state->to = core::sim::ChatLine::To::allies; return;
          case 0x20103: state->to = core::sim::ChatLine::To::player; return;
          case 0x1001:
          case 0x1007: {
            core::sim::ChatLine line;
            line.text = menu.text("ChatEdit");
            line.to = state->to;
            if (line.to == core::sim::ChatLine::To::player) {
              if (state->chosen < 0 || static_cast<std::size_t>(state->chosen) >= state->players.size()) return;
              line.target = state->players[static_cast<std::size_t>(state->chosen)];
            }
            if (event.id == 0x1007) {
              line.located = true;
              line.location = editor_view_centre();
            }
            close_menu();
            if (line.text.empty() && !line.located) return;
            const core::sim::ChatLine said = net_->say(std::move(line));
            std::printf("chat      said %u: %s\n", said.seq, said.text.c_str());
            std::fflush(stdout);
            show_chat(net_->name_of(said.from) + ": " + said.text);
            return;
          }
          case 0x1002: close_menu(); return;
          default: return;
        }
      });
  if (dialog == nullptr) return;
  dialog->set_row("AllRadio", 1);
  std::vector<std::string> names;
  for (const core::PlayerId seat : state->players) names.push_back(net_->display_name(seat));
  dialog->set_items("PlayerCombo", std::move(names));
  if (!state->players.empty()) dialog->select("PlayerCombo", 0);
  dialog->focus("ChatEdit");
}

void Application::show_chat(std::string text) {
  // Twelve seconds, the last six lines: this engine's numbers.
  chat_shown_.push_back(ShownChat{std::move(text), imperivm::net::now_ms() + 12000});
  if (chat_shown_.size() > 6) chat_shown_.erase(chat_shown_.begin());
  refresh_chat();
}

void Application::refresh_chat() {
  const std::uint32_t now = imperivm::net::now_ms();
  std::erase_if(chat_shown_, [now](const ShownChat& shown) {
    return static_cast<std::int32_t>(now - shown.until) >= 0;
  });
  std::vector<std::string> lines;
  for (const ShownChat& shown : chat_shown_) lines.push_back(shown.text);
  ui_.set_messages(std::move(lines));
}
#endif

bool Application::play_from_front(const std::string& map, int map_index) {
  args_.map = map;
  args_.map_index = map_index;
  args_.play = true;
  args_.from_front = true;
  args_.select.clear();
  args_.menu.clear();
  // `start_player` from the container's `game.xml`, unless `--player` said --
  // or unless this is a networked match, where the seat is the lobby's.
  if (!networked()) {
    gamedata::MapContainer container;
    if (container.open(vfs_.root() / map, nullptr)) {
      if (const auto bytes = container.read("game.xml"); !bytes.empty()) {
        if (auto game = core::GameProperties::parse(bytes); game.ok()) args_.player = game->start_player;
      }
    }
  }
  close_all_menus();
  front_mode_ = false;
  map_mode_ = true;
  if (!start_map()) {
    std::printf("front: %s would not open\n", map.c_str());
    return_to_front();
    return false;
  }
  play_mode_ = true;
  if (!start_play()) {
    return_to_front();
    return false;
  }
  return true;
}

bool Application::load_from_front(const std::filesystem::path& file) {
  core::sim::SaveFileContents contents;
  std::string error;
  if (!gamedata::read_save_file(file, contents, &error)) {
    std::printf("%s\n", error.c_str());
    return false;
  }
  seed_ = contents.manifest.seed;
  const std::string map = contents.manifest.container;
  const int index = contents.manifest.map_index.empty() ? -1 : SDL_atoi(contents.manifest.map_index.c_str());
  pending_load_ = std::move(contents);
  return play_from_front(map, index);
}

void Application::return_to_front() {
#if IMPERIVM_HAVE_NET
  // Going back to the front leaves a networked match, and says so.
  if (net_ != nullptr) {
    net_->leave();
    net_.reset();
  }
#endif
  infobar_.reset();
  cmdbar_.reset();
  session_.reset();
  (void)local_orders_.take();
  buttons_.clear();
  pending_command_.clear();
  reported_outcome_ = false;
  campaign_written_ = false;
  turns_run_ = 0;
  owed_ms_ = 0.0;
  infobar_turn_ = ~0ull;
  fog_view_.reset();
  pending_load_.reset();
  seed_ = 1;
  zoom_ground_ = core::ui::Image{};
  play_mode_ = false;
  map_mode_ = false;
  front_mode_ = true;
  // The match's sounds end with it (0x006aff00 from 0x0051d44e, read as the
  // match's teardown: **inferred**), and the menus, entered again, ask for
  // their music again (0x00748620).
  stop_sounds();
  menu_music_wanted_ = true;
  menu_music_countdown_ = 0;
  args_.edit = false;
  editor_ = Editor{};
  paused_ = false;
  ui_.show_bars(false);
  ui_.set_tooltip({}, 0, 0);
  if (from_campaign_ && campaign_ != nullptr) {
    from_campaign_ = false;
    const std::filesystem::path container = campaign_->container;
    close_all_menus();
    open_menu("menuini/menuback.ini", [](const core::ui::DialogEvent&, core::ui::Dialog&) {});
    // A conquest with every territory taken is over (0x0074ae84: game type
    // 2, the mission won, and 0x00501110 finding no territory in state
    // 0): `CONQUESTVICTORY.INI` says so, and OK goes to the main menu.
    core::sim::CampaignCarry carry;
    if (!campaign_file_.empty() && gamedata::read_campaign_file(campaign_file_, carry) &&
        !carry.progress.states.empty() &&
        std::none_of(carry.progress.states.begin(), carry.progress.states.end(),
                     [](core::sim::TerritoryState state) { return state == core::sim::TerritoryState::enemy; })) {
      core::ui::Dialog* box = open_menu(
          "menuini/conquestvictory.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog&) {
            if (event.kind == core::ui::DialogEvent::Kind::kCommand && event.id == 0x1000) open_main_menu();
          });
      if (box != nullptr) {
        // The sentence the exe puts in the box, translated through its
        // table: 0x0074aed9's literal.
        box->set_text("Text", item_label("Congratulations!!!\nYou have been victorious.", ""));
        return;
      }
    }
    open_campaign_screen(container);
    return;
  }
  open_main_menu();
}

/// The same map again from the start: the session and everything hanging
/// off it are rebuilt as `start_play` builds them.
bool Application::restart_play() {
  infobar_.reset();
  cmdbar_.reset();
  session_.reset();
  buttons_.clear();
  pending_command_.clear();
  reported_outcome_ = false;
  campaign_written_ = false;
  turns_run_ = 0;
  owed_ms_ = 0.0;
  infobar_turn_ = ~0ull;
  fog_view_.reset();
  zoom_ground_ = core::ui::Image{};
  return start_play();
}

/// Left selects, right commands. Middle drags the map.
///
/// The left button is a band select: press, drag, release. A press-and-release
/// without movement is the degenerate box, so a click and a drag are one path
/// rather than two that can disagree about what is under the cursor.
void Application::handle_play_mouse(const SDL_Event& event) {
  if (session_ == nullptr) return;
  core::sim::World& world = session_->world();
  const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
  const bool ctrl = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;

  if (editor_.active) {
    // The editor's mouse, by the chosen tool (the exe's tool windows take
    // the same seven messages, 0x004c1e90): the left button places, paints,
    // selects and drags; the right button turns, picks or steps back to
    // Edit objects; the middle button pans.
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
      const std::int32_t mx = static_cast<std::int32_t>(event.button.x);
      const std::int32_t my = static_cast<std::int32_t>(event.button.y);
      const core::sim::Point at = camera_.unproject(mx, my);
      if (event.button.button == SDL_BUTTON_MIDDLE) {
        dragging_ = true;
      } else if (event.button.button == SDL_BUTTON_LEFT) {
        switch (editor_.tool) {
          case EditorTool::kPlace:
            editor_place(mx, my);
            break;
          case EditorTool::kEdit:
            editor_select(mx, my);
            editor_.dragging = editor_.selected != core::kNoObject;
            break;
          case EditorTool::kTerrain:
          case EditorTool::kHeight:
          case EditorTool::kDecor:
          case EditorTool::kDecorDelete:
            editor_.painting = true;
            editor_paint(at, true);
            break;
          case EditorTool::kPlaceArea:
            if (editor_.chosen >= 0) editor_place_area(at, editor_.nodes[static_cast<std::size_t>(editor_.chosen)].circle);
            break;
          case EditorTool::kEditArea:
            editor_area_press(at);
            break;
        }
      } else if (event.button.button == SDL_BUTTON_RIGHT) {
        if (editor_.tool == EditorTool::kEdit) {
          // On the selected object itself, its properties (0x0048e970's
          // right button, which the notes read as the sheet on a selected
          // object); anywhere else, the selected object turns to the point.
          if (editor_.selected != core::kNoObject &&
              world_view_.pick(world, camera_, mx, my) == editor_.selected) {
            open_object_properties({editor_.selected});
          } else {
            editor_turn(at, shift);
          }
        } else if (editor_.tool == EditorTool::kPlaceArea || editor_.tool == EditorTool::kEditArea) {
          for (std::size_t i = 0; i < editor_.nodes.size(); ++i) {
            if (editor_.nodes[i].is_tool && editor_.nodes[i].tool == EditorTool::kEdit) {
              editor_choose(static_cast<std::int32_t>(i));
              break;
            }
          }
        } else {
          editor_pick(at);
        }
      }
    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
      editor_.pointer = camera_.unproject(static_cast<std::int32_t>(event.motion.x),
                                          static_cast<std::int32_t>(event.motion.y));
      editor_.pointer_on_map = true;
      if (dragging_) {
        map_.move_view(-static_cast<std::int32_t>(event.motion.xrel),
                       -static_cast<std::int32_t>(event.motion.yrel));
      } else if (editor_.painting) {
        editor_paint(editor_.pointer, false);
      } else if (editor_.tool == EditorTool::kEditArea && editor_.area_handle != 0) {
        editor_area_drag(editor_.pointer);
      } else if (editor_.dragging && editor_.selected != core::kNoObject) {
        (void)world.set_position(editor_.selected, editor_.pointer);
      }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
      if (event.button.button == SDL_BUTTON_MIDDLE) dragging_ = false;
      if (event.button.button == SDL_BUTTON_LEFT) {
        editor_drop();
        editor_.dragging = false;
        editor_.painting = false;
        editor_.area_handle = 0;
      }
    }
    (void)ctrl;
    return;
  }

  if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
    const std::int32_t mx = static_cast<std::int32_t>(event.button.x);
    const std::int32_t my = static_cast<std::int32_t>(event.button.y);
    // The bars take the click before the map does: a command button, or one
    // of the no-selection menu's nine (of which the two saves are wired).
    if (const std::int32_t index = ui_.button_at(mx, my); index >= 0) {
      if (event.button.button == SDL_BUTTON_LEFT) {
        pressed_button_ = buttons_[static_cast<std::size_t>(index)].name;
        bars_dirty_ = true;
      }
      return;
    }
    // A garrison portrait: the units behind it become the selection.
    if (event.button.button == SDL_BUTTON_LEFT) {
      const core::ui::StripCellHit cell = ui_.strip_cell_at(mx, my);
      if (cell.strip != nullptr && cell.strip->kind == core::ui::WidgetType::kUIHolder && cell.index >= 0) {
        holder_click(static_cast<std::size_t>(cell.index), shift, ctrl);
        return;
      }
      // A training queue entry: acted on at the release, over the same cell
      // (0x006ee27a compares the cell it went down on).
      if (cell.strip != nullptr && cell.strip->kind == core::ui::WidgetType::kBuildingQueue &&
          cell.index >= 0) {
        pressed_queue_cell_ = cell.index;
        return;
      }
    }
    if (const core::ui::Widget* widget = ui_.widget_at(mx, my); widget != nullptr) {
      if (event.button.button == SDL_BUTTON_LEFT && widget->has_id) {
        // The menu's IDs, from `EMPTY_*.INI`: 0x1003 is the quick save and
        // 0x1004 the quick load, whatever the section names say.
        if (widget->id == 0x1003) (void)quicksave();
        if (widget->id == 0x1004 && !quickload()) std::printf("the session is no longer usable\n");
        // 0x1001 is `MainMenu (F10)`: the game menu; 0x1002 `Notes (F8)`.
        if (widget->id == 0x1001) open_game_menu();
        if (widget->id == 0x1002) open_notes_menu();
        if (widget->id == 0x1005) open_diplomacy_menu();
#if IMPERIVM_HAVE_NET
        // 0x1006 is `Chat (Enter)`: the button did nothing while the key
        // worked. Only in a networked match, as the key.
        if (widget->id == 0x1006 && networked()) open_ingame_chat();
#endif
        if (widget->id == 0x1009) select_party();
        if (widget->id == 0x1007) open_zoom_map();
        if (widget->id == 0x1008) open_help();
      }
      // A `Switch` flips the info bar's tab to its `SwitchToTab`, and flips
      // it back after `SwitchBackTime` milliseconds when it declares one --
      // the army-and-items switch does, twenty seconds, the skills' does not.
      if (event.button.button == SDL_BUTTON_LEFT && widget->kind == core::ui::WidgetType::kSwitch) {
        std::int32_t tab = 0;
        if (widget->attribute_int("SwitchToTab", tab) && tab >= 0) {
          infobar_tab_ = static_cast<std::uint32_t>(tab);
          std::int32_t back = -1;
          (void)widget->attribute_int("SwitchBackTime", back);
          tab_back_at_ = back > 0 ? SDL_GetTicks() + static_cast<std::uint64_t>(back) : 0;
          bars_dirty_ = true;
        }
      }
      return;
    }
    // A row waiting for its target takes the next left click on the map.
    if (event.button.button == SDL_BUTTON_LEFT && !pending_command_.empty()) {
      core::sim::OrderTarget target;
      target.object = world_view_.pick(world, camera_, mx, my);
      target.point = camera_.unproject(mx, my);
#if IMPERIVM_HAVE_NET
      if (net_ != nullptr) {
        core::sim::NetOrder order;
        const std::span<const core::ObjectId> selected =
            session_->selections().player(local_player_).ids();
        order.actors.assign(selected.begin(), selected.end());
        order.kind = core::sim::NetOrderKind::command;
        order.command = pending_command_;
        order.aimed = true;
        order.target = target;
        // Shift is the append, Ctrl the `bModifier`: two flags, as the
        // original's bar writes them (0x005e39f0) and its wire packs them;
        // and Ctrl on a train row, its count, read here at the click.
        const core::sim::CommandBar::Flags flags =
            cmdbar_->flags(local_player_, pending_command_, core::sim::CommandBar::Keys{shift, ctrl});
        order.mode = flags.append ? core::sim::OrderMode::append : core::sim::OrderMode::replace;
        order.modifier = flags.modifier;
        order.repeat = flags.repeat;
        net_->queue(std::move(order));
        acknowledge_order();
        std::printf("command: %s queued at the target\n", pending_command_.c_str());
        pending_command_.clear();
        bars_dirty_ = true;
        return;
      }
#endif
      const bool aimed = cmdbar_->aim(local_player_, pending_command_, target,
                                      core::sim::CommandBar::Keys{shift, ctrl});
      std::printf("command: %s %s\n", pending_command_.c_str(),
                  aimed ? "issued at the target" : "no longer offered");
      if (aimed) acknowledge_order();
      if (aimed) {
        const std::span<const core::ObjectId> selected =
            session_->selections().player(local_player_).ids();
        if (!selected.empty()) print_queue(selected.front());
      }
      pending_command_.clear();
      bars_dirty_ = true;
      return;
    }
    if (event.button.button == SDL_BUTTON_RIGHT && !pending_command_.empty()) {
      // A right click cancels the pending row, as the original's does.
      pending_command_.clear();
      bars_dirty_ = true;
      return;
    }
    if (event.button.button == SDL_BUTTON_LEFT) {
      band_select_ = true;
      band_x0_ = band_x1_ = static_cast<std::int32_t>(event.button.x);
      band_y0_ = band_y1_ = static_cast<std::int32_t>(event.button.y);
    } else if (event.button.button == SDL_BUTTON_MIDDLE) {
      dragging_ = true;
    } else if (event.button.button == SDL_BUTTON_RIGHT) {
      // One call decides the verb from what is under the cursor, the class
      // graph's own `<defaultcmd>` lists and their `verify=` scripts. The app
      // does not know what "attack" means and must not.
      const platform::Camera& camera = camera_;
      core::sim::OrderTarget target;
      target.object = world_view_.pick(world, camera, static_cast<std::int32_t>(event.button.x),
                                       static_cast<std::int32_t>(event.button.y));
      target.point = camera.unproject(static_cast<std::int32_t>(event.button.x),
                                      static_cast<std::int32_t>(event.button.y));

#if IMPERIVM_HAVE_NET
      if (net_ != nullptr) {
        // An order, for the turn every peer agrees on, over what is selected
        // now: the selection is this screen's and travels with the order.
        core::sim::NetOrder order;
        const std::span<const core::ObjectId> selected =
            session_->selections().player(local_player_).ids();
        order.actors.assign(selected.begin(), selected.end());
        order.target = target;
        order.mode = shift ? core::sim::OrderMode::append : core::sim::OrderMode::replace;
        order.modifier = ctrl;
        if (!order.actors.empty()) {
          net_->queue(std::move(order));
          // Acknowledged as it is given, on this screen only: the original
          // speaks in its input layer, not when the turn runs.
          acknowledge_order();
        }
        return;
      }
#endif
      const core::sim::CommandTable* commands = core::sim::order_command_table(world);
      if (commands == nullptr) return;
      core::sim::ScriptOrderVerifier verifier(session_->scheduler(), session_->host_context());
      const core::sim::OrderReport report = core::sim::issue_default_order(
          world, *commands, session_->selections(), target,
          shift ? core::sim::OrderMode::append : core::sim::OrderMode::replace, ctrl,
          local_player_, &verifier);
      std::printf("order: %zu issued, %zu unresolved, %zu blocked\n", report.issued,
                  report.unresolved, report.blocked);
      if (report.issued > 0) acknowledge_order();
      if (report.issued > 0) {
        const std::span<const core::ObjectId> selected =
            session_->selections().player(local_player_).ids();
        if (!selected.empty()) print_queue(selected.front());
      }
      // A blocked click does nothing on screen, so say what blocked it: the
      // candidate whose `verify=` could not be answered.
      if (report.blocked > 0) {
        for (const core::ObjectId actor : session_->selections().player(local_player_).ids()) {
          const core::sim::DefaultOrder why =
              core::sim::resolve_default_order(world, *commands, actor, target, ctrl, &verifier);
          if (why.status != core::sim::DefaultOrderStatus::blocked) continue;
          std::printf("order: blocked by %.*s%s%s\n", static_cast<int>(why.blocked_by.size()),
                      why.blocked_by.data(), verifier.last_trap().empty() ? "" : ", whose verifier trapped at ",
                      verifier.last_trap().c_str());
          if (verifier.missing() > 0) std::printf("order: %zu verify script(s) not in the library\n", verifier.missing());
          break;
        }
      }
      std::fflush(stdout);
    }
  }

  if (event.type == SDL_EVENT_MOUSE_MOTION) {
    update_hover(static_cast<std::int32_t>(event.motion.x),
                 static_cast<std::int32_t>(event.motion.y));
    if (band_select_) {
      band_x1_ = static_cast<std::int32_t>(event.motion.x);
      band_y1_ = static_cast<std::int32_t>(event.motion.y);
    } else if (dragging_) {
      map_.move_view(-static_cast<std::int32_t>(event.motion.xrel),
                     -static_cast<std::int32_t>(event.motion.yrel));
    }
  }

  if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
    if (event.button.button == SDL_BUTTON_MIDDLE) dragging_ = false;
    if (event.button.button == SDL_BUTTON_LEFT && pressed_queue_cell_ >= 0) {
      const core::ui::StripCellHit cell = ui_.strip_cell_at(static_cast<std::int32_t>(event.button.x),
                                                            static_cast<std::int32_t>(event.button.y));
      const std::int32_t pressed = pressed_queue_cell_;
      pressed_queue_cell_ = -1;
      if (cell.strip != nullptr && cell.strip->kind == core::ui::WidgetType::kBuildingQueue &&
          cell.index == pressed) {
        queue_click(static_cast<std::size_t>(pressed), ctrl);
      }
      return;
    }
    if (event.button.button == SDL_BUTTON_LEFT && !pressed_button_.empty()) {
      // Released over the button it went down on: a press. Elsewhere: nothing.
      const std::int32_t index = ui_.button_at(static_cast<std::int32_t>(event.button.x),
                                               static_cast<std::int32_t>(event.button.y));
      const std::string name = pressed_button_;
      pressed_button_.clear();
      bars_dirty_ = true;
      if (index >= 0 && static_cast<std::size_t>(index) < buttons_.size() &&
          buttons_[static_cast<std::size_t>(index)].name == name) {
        press_button(index, core::sim::CommandBar::Keys{shift, ctrl});
      }
      return;
    }
    if (event.button.button != SDL_BUTTON_LEFT || !band_select_) return;
    band_select_ = false;

    platform::ScreenRect rect;
    rect.x = std::min(band_x0_, band_x1_);
    rect.y = std::min(band_y0_, band_y1_);
    rect.width = std::abs(band_x1_ - band_x0_);
    rect.height = std::abs(band_y1_ - band_y0_);

    picked_.clear();
    // A box of a few pixels is a click, and a click wants the topmost sprite
    // under it rather than whatever anchor happens to fall in the box.
    if (rect.width < 4 && rect.height < 4) {
      const core::ObjectId hit = world_view_.pick(world, camera_, band_x0_, band_y0_);
      if (hit != core::kNoObject) picked_.push_back(hit);
    } else {
      world_view_.pick_in_rect(world, camera_, rect, picked_);
    }

    core::sim::Selection& selection = session_->selections().player(local_player_);
    // A fresh selection is made from nothing, so all of it is new and an
    // object clicked again speaks again; Shift adds to what there is.
    std::vector<core::ObjectId> before;
    if (shift) before.assign(selection.ids().begin(), selection.ids().end());
    if (!shift) selection.clear();
    std::size_t taken = 0;
    for (const core::ObjectId id : picked_) {
      // `is_selectable` is the class graph's answer -- towers and walls carry
      // `non_selectable="1"` -- not a guess made here.
      if (!core::sim::is_selectable(world, id)) continue;
      if (shift) {
        selection.toggle(id);
      } else {
        selection.add(id);
      }
      ++taken;
    }
    // What the original's selection-changed handler does, and the reason it is
    // a call rather than something `add` does: `Obj::_LastSelectionTime` is
    // stamped for every member of the new selection, and this loop is the other
    // way a selection changes.
    session_->selections().note_selection_changed(local_player_, session_->scheduler().now());
    play_select_sounds(before);
    std::printf("selected %zu of %zu under the cursor (player %u holds %zu)\n", taken,
                picked_.size(), static_cast<unsigned>(local_player_), selection.size());
    std::fflush(stdout);
  }
}

void Application::tick_play(platform::Window::Frame& frame) {
  if (!target_.ensure(window_.device(), frame.width, frame.height)) return;

  if (view_state_.x != view_seen_.x || view_state_.y != view_seen_.y) {
    // A script's `View(pt, lock)` since the last frame: the camera goes where
    // the mission says -- Zama's landing is five of them in a row. It wins
    // over the opening look, which is only a default.
    map_.look_at(view_state_.x, view_state_.y, static_cast<std::int32_t>(frame.width),
                 static_cast<std::int32_t>(frame.height));
    look_pending_ = false;
  } else if (look_pending_) {
    map_.look_at(look_x_, look_y_, static_cast<std::int32_t>(frame.width),
                 static_cast<std::int32_t>(frame.height));
    look_pending_ = false;
  }
  map_.clamp_view(static_cast<std::int32_t>(frame.width),
                  static_cast<std::int32_t>(frame.height));

  // One view, two consumers. The map renderer owns the viewport -- panning and
  // dragging already go through it -- so the camera is synced from it rather
  // than kept in parallel, which is the arrangement that cannot drift.
  camera_.x = map_.view_x();
  camera_.y = map_.view_y();
  camera_.width = static_cast<std::int32_t>(frame.width);
  camera_.height = static_cast<std::int32_t>(frame.height);
  // The ground is a mesh the height displaces; what stands on it is lifted
  // the same way.
  camera_.elevation = world_.height().cell_size() != 0 ? &world_.height() : nullptr;
  // And what `ViewPos()` answers is where the camera is now, wherever the
  // player has scrolled it: the cutscene idiom saves it, `View`s away, and
  // `View`s back to it. Not world state -- see `sim/feedback.hpp`.
  view_seen_ = camera_.unproject(camera_.width / 2, camera_.height / 2);
  view_state_.x = view_seen_.x;
  view_state_.y = view_seen_.y;
  // The original's view setter (0x006265c0) moves every live sound with the
  // view (0x00626c56 calls 0x006b0810); here, the frame the camera moved.
  follow_view();
  refresh_fog();
  // The fog darkens the ground pixel by pixel at its own world point and
  // each sprite at the point it stands on, as the original's draws do --
  // not one picture stretched over the view, which a lifted ground would
  // slide out from under.
  map_.set_fog(fog_view_.empty() ? nullptr : &fog_view_.light(), fog_view_.generation());
  if (fog_view_.empty()) {
    world_view_.set_shade(nullptr);
  } else {
    world_view_.set_shade([this](core::sim::Point at) { return fog_view_.factor_at(at); });
  }

  map_.draw_terrain(frame.commands, target_.texture(), frame.width, frame.height);

  // Staging happens outside `begin`/`render`: a palette row or an atlas region
  // only reaches the GPU here.
  std::string error;
  world_view_.prepare(session_->world(), &error);

  renderer_.begin(frame.width, frame.height);
  // **The live world, not the map's static object list.** That list is where
  // the objects started; this is where they are.
  // The zoom map's 500 ms tick, and at once after a move made from it.
  if (zoom_open_) {
    const std::uint64_t ticks = SDL_GetTicks();
    if (zoom_refresh_ticks_ == 0 || ticks - zoom_refresh_ticks_ >= 500) {
      if (core::ui::Dialog* dialog = zoom_dialog(); dialog != nullptr) refresh_zoom_map(*dialog);
    }
  }
  world_view_.draw(session_->world(), camera_, renderer_);
  // Over the world and its bars, under the interface.
  draw_band();
  renderer_.render(frame.commands, target_.texture(), nullptr);

  // Over the world, under nothing. The bars are opaque strips at the top and
  // bottom; `bar_rect` is what the camera should be inset by once the view is
  // meant to sit between them.
  ui_.set_viewport(frame.width, frame.height);
  refresh_editor_overlay(frame.width, frame.height);
  refresh_infobar();
  if (args_.hover_x >= 0) update_hover(args_.hover_x, args_.hover_y);
  std::string ui_error;
  ui_.render(frame.commands, target_.texture(), &ui_error);

  target_.blit_to(frame.commands, frame.swapchain, frame.width, frame.height);

  const std::uint64_t now = SDL_GetTicks();
  const std::uint64_t elapsed = now - last_ticks_;
  last_ticks_ = now;

  // The zoom map alone does not stop the clock: it is a live window.
  const bool menus_hold = ui_.dialog_count() > (zoom_open_ ? 1u : 0u);
#if IMPERIVM_HAVE_NET
  if (net_ != nullptr && session_ != nullptr) {
    // The agreed turns, at their agreed length. Menus do not hold this clock:
    // a menu is one player's, and the match is everybody's.
    turns_run_ += net_->advance(*session_);
    if (net_->done(*session_)) quit_requested_ = true;
  } else
#endif
  if (!paused_ && session_ != nullptr && args_.turn_interval > 0 && !menus_hold) {
    // Owed time accumulates so that a slow frame catches up instead of
    // silently dropping game time -- which would make the simulation run at a
    // rate that depended on the frame rate, and two machines disagree.
    owed_ms_ += static_cast<double>(elapsed);
    const double interval = static_cast<double>(args_.turn_interval);
    // Capped: after a breakpoint or a drag of the window, do not try to
    // replay a minute of game time in one frame.
    int budget = 8;
    while (owed_ms_ >= interval && budget-- > 0) {
      // A script waiting on `turn:N` gets exactly that turn: the frame stops
      // there, so the step after it -- a `shot:` or a `hash` -- sees turn N
      // and not whatever the frame's budget ran on to.
      if (input_until_turn_ > 0 && session_->world().turns() >= input_until_turn_) {
        owed_ms_ = 0.0;
        break;
      }
      owed_ms_ -= interval;
      run_local_turn();
      // The turn that decides the match is the last one: the end-game screen
      // opens on it and holds the clock. Without this a frame ran out its
      // budget first, and Crossroads ended at turn 2,376, seven turns past
      // the 2,369 `imrun` stops at, on a world whose hash it could not match.
      if (!reported_outcome_ && session_->match_status().over) {
        owed_ms_ = 0.0;
        break;
      }
    }
    if (owed_ms_ > interval * 8) owed_ms_ = 0.0;
  }
  // An open Diplomacy screen shows the other side's policy as the turns
  // just applied left it: a networked peer's OK lands while it is open.
  for (std::size_t i = 0; i < ui_.dialog_count(); ++i) {
    core::ui::Dialog* dialog = ui_.dialog(i);
    if (dialog != nullptr && ends_with(fold_name(dialog->screen().path), "/diplomacy.ini")) {
      refresh_diplomacy(*dialog);
    }
  }

  // The outcome, the frame it is decided rather than on the status line's
  // clock: a surrender from the menu ends the match between two frames.
  if (args_.declare_won && !networked() && !reported_outcome_ && play_mode_ && turns_run_ > 0 &&
      core::sim::PlayerTable::is_valid(local_player_)) {
    args_.declare_won = false;
    session_->declare_match(local_player_, /*lost=*/false);
  }
  // In a networked match "did I win" is asked of this seat; see
  // `sim::match_status(world, viewer)`.
  const core::sim::MatchStatus match =
      networked() ? session_->match_status(local_player_) : session_->match_status();
  if (match.over && !reported_outcome_) {
    reported_outcome_ = true;
    std::printf("\n*** match over: %s ***\n\n",
                match.human_won ? "you win" : (match.human == core::kNoPlayer ? "decided"
                                                                             : "you lose"));
    // What `imrun`'s `match` block says of the same run, to hold beside it.
    std::printf("decided:      at turn %llu, hash %016llx\n",
                static_cast<unsigned long long>(session_->world().turns()),
                static_cast<unsigned long long>(session_->report().hash));
    std::fflush(stdout);
    // A won conquest mission leaves the campaign for the next one; a lost one
    // leaves the file as it was, which is the shipped rule.
    if (match.human_won) write_campaign_carry();
    if (match.human != core::kNoPlayer) open_endgame_menu(match.human_won);
  }
  draw_status();
}

/// The fog, once a frame: the light grid fed the camera's rect and the clock,
/// and the world view's hide predicate over it. Not in the editor, which
/// shows the whole map placed.
void Application::refresh_fog() {
  if (session_ == nullptr) return;
  // `SetFog(false)` lifts it for a cutscene; it is display, not the explored map.
  const bool active = !editor_.active && !args_.no_fog && view_state_.fog &&
                      core::sim::PlayerTable::is_valid(local_player_);
  if (!active) {
    fog_view_.update(session_->world(), nullptr, local_player_, false, false, 0, camera_, 0);
  } else {
    const core::sim::MatchSystem* match = core::sim::match_system_of(session_->world());
    const bool fog_of_war = match == nullptr || match->rules().fog_of_war;
    const bool exploration = match == nullptr || match->rules().exploration;
    const std::int32_t map_size = match != nullptr ? match->rules().map_size : 0;
    fog_view_.update(session_->world(), core::sim::fog_system_of(session_->world()),
                     local_player_, fog_of_war, exploration, map_size, camera_, SDL_GetTicks());
  }
  const bool hiding = !fog_view_.empty();
  if (hiding == fog_hiding_) return;
  fog_hiding_ = hiding;
  if (!hiding) {
    world_view_.set_hidden(nullptr);
    return;
  }
  world_view_.set_hidden([this](const core::sim::WorldObject& object) {
    return fog_view_.hides(object, local_player_);
  });
}

/// The info bar, from the selection: recomposited when a turn has run or the
/// selection has changed, and left alone otherwise.
void Application::refresh_infobar() {
  if (session_ == nullptr || infobar_ == nullptr) return;
  const std::uint64_t turn = session_->world().turns();
  const core::sim::Selection& selection = session_->selections().player(local_player_);
  // A cheap fingerprint of the selection: its size and its members' sum.
  std::uint32_t generation = 0;
  for (const core::ObjectId id : selection.ids()) generation = generation * 31u + id;
  if (tab_back_at_ != 0 && SDL_GetTicks() >= tab_back_at_) {
    tab_back_at_ = 0;
    infobar_tab_ = 0;
    bars_dirty_ = true;
  }
  // The skills switch blinks while the selection's head is a hero of the
  // local player's with skill points to spend (0x006c0f30); the phase flips
  // every `BlinkTime` milliseconds (0x006c1019), and a flip repaints.
  {
    bool active = false;
    if (!selection.empty()) {
      const core::ObjectId head = selection.ids().front();
      const core::sim::WorldObject* slot = session_->world().find(head);
      const core::sim::HeroSystem* heroes = core::sim::hero_system_of(session_->world());
      active = slot != nullptr && slot->state.owner == local_player_ && heroes != nullptr &&
               heroes->is_hero(head) && heroes->available_skill_points(head) > 0;
    }
    std::int32_t blink_time = 0;
    bool on = false;
    if (active) {
      for (const char* name : {"Switch2", "Switch1"}) {
        const core::ui::Widget* widget = ui_.bar_widget(name);
        if (widget != nullptr && widget->attribute_int("BlinkTime", blink_time) && blink_time > 0) break;
        blink_time = 0;
      }
      on = blink_time > 0 && ((SDL_GetTicks() / static_cast<std::uint64_t>(blink_time)) & 1u) != 0;
    }
    if (active != blink_active_ || on != blink_on_) {
      blink_active_ = active;
      blink_on_ = on;
      bars_dirty_ = true;
    }
  }
  if (turn == infobar_turn_ && selection.size() == infobar_selection_ &&
      generation == infobar_generation_ && !bars_dirty_) {
    return;
  }
  infobar_turn_ = turn;
  infobar_selection_ = selection.size();
  infobar_generation_ = generation;
  bars_dirty_ = false;
  refresh_cmdbar();
  // The tooltip under a resting pointer follows the buttons it describes.
  if (pointer_x_ >= 0 && ui_.dialog_count() == 0) update_hover(pointer_x_, pointer_y_);

  const core::sim::SelectionInfo info = infobar_->describe(local_player_);
  core::ui::BarContent content;
  content.tags = core::ui::SelectionTag::kNone;
  for (const std::string& tag : info.tags) content.tags |= core::ui::selection_tag_from_name(tag);
  content.tab = infobar_tab_;
  content.blink = blink_on_;
  content.name = info.name;
  content.thumbnail_path = info.icon;
  content.health = info.health;
  content.values.resize(info.values.size());
  for (std::size_t i = 0; i < info.values.size(); ++i) {
    content.values[i].text = info.values[i].text;
    content.values[i].icon = info.values[i].icon;
    content.values[i].present = info.values[i].present;
  }
  const auto cells = [](const std::vector<core::sim::InfoCell>& from) {
    std::vector<core::ui::StripCell> out;
    out.reserve(from.size());
    for (const core::sim::InfoCell& cell : from) {
      core::ui::StripCell to;
      to.icon = cell.icon;
      to.number = cell.number;
      to.health = cell.health;
      to.text = cell.text;
      to.glow = cell.glow;
      to.plus = cell.plus;
      switch (cell.frame) {
        case core::sim::InfoCell::Frame::selected: to.frame = core::ui::StripCell::Frame::kSelected; break;
        case core::sim::InfoCell::Frame::train: to.frame = core::ui::StripCell::Frame::kTrain; break;
        case core::sim::InfoCell::Frame::wait: to.frame = core::ui::StripCell::Frame::kWait; break;
        default: to.frame = core::ui::StripCell::Frame::kNormal; break;
      }
      out.push_back(std::move(to));
    }
    return out;
  };
  // A cancel posted and not yet run leaves the strip at once; an id the
  // queue no longer holds is forgotten.
  std::erase_if(queue_cancelled_, [&info](std::uint32_t id) {
    return std::none_of(info.queue.begin(), info.queue.end(),
                        [id](const core::sim::InfoCell& cell) { return cell.command == id; });
  });
  std::vector<core::sim::InfoCell> shown;
  queue_cells_.clear();
  for (const core::sim::InfoCell& cell : info.queue) {
    if (std::find(queue_cancelled_.begin(), queue_cancelled_.end(), cell.command) != queue_cancelled_.end()) {
      continue;
    }
    shown.push_back(cell);
    queue_cells_.push_back(cell.command);
  }
  if (!shown.empty() && shown.front().frame == core::sim::InfoCell::Frame::wait) {
    shown.front().frame = core::sim::InfoCell::Frame::train;
  }
  content.queue = cells(shown);
  content.holder = cells(info.holder);
  holder_cells_.clear();
  for (std::size_t i = 0; i < info.holder.size(); ++i) {
    const std::uint64_t key = info.holder[i].key;
    holder_cells_.push_back(HolderCell{key, info.holder[i].objects});
    // A marked portrait takes `SelFrameImage` at once (0x006d3ad6).
    if (std::any_of(holder_marks_.begin(), holder_marks_.end(),
                    [key](const auto& mark) { return mark.first == key; })) {
      content.holder[i].frame = core::ui::StripCell::Frame::kSelected;
    }
  }
  content.items = cells(info.items);
  content.skills = cells(info.skills);
  content.specials = cells(info.specials);
  const core::game::TranslationTable* table = session_->host_context().translations;
  content.translate = [table](std::string_view key) -> std::string_view {
    return table == nullptr ? key : table->translate(key);
  };
  if (std::getenv("IMPERIVM_INFOBAR") != nullptr) {
    std::printf("infobar: class %s name \"%s\" icon %s health %d tags", info.class_id.c_str(),
                info.name.c_str(), info.icon.c_str(), info.health);
    for (const std::string& tag : info.tags) std::printf(" %s", tag.c_str());
    for (std::size_t i = 0; i < info.values.size(); ++i) {
      if (info.values[i].present) std::printf(" v%zu=\"%s\"", i, info.values[i].text.c_str());
    }
    std::printf(" queue %zu holder %zu items %zu skills %zu specials %zu\n", info.queue.size(),
                info.holder.size(), info.items.size(), info.skills.size(), info.specials.size());
    // The `UIHolder` portraits, each with its number and what a click on it
    // selects; and, for a hero, its army as the hero system keeps it, so
    // that the two can be compared.
    for (std::size_t i = 0; i < info.holder.size(); ++i) {
      std::printf("infobar: holder %zu icon %s number \"%s\" selects", i, info.holder[i].icon.c_str(),
                  info.holder[i].number.c_str());
      for (const core::ObjectId id : info.holder[i].objects) std::printf(" %u", static_cast<unsigned>(id));
      std::printf("\n");
    }
    const core::sim::Selection& selected = session_->selections().player(local_player_);
    const core::sim::HeroSystem* heroes = core::sim::hero_system_of(session_->world());
    if (selected.size() == 1 && heroes != nullptr) {
      if (const core::sim::HeroRecord* hero = heroes->hero(selected.ids().front())) {
        std::printf("infobar: hero %u army", static_cast<unsigned>(hero->id));
        for (const core::ObjectId id : hero->army) std::printf(" %u", static_cast<unsigned>(id));
        std::printf("\n");
      }
    }
  }
  for (const std::string& complaint : info.complaints) {
    if (reported_.emplace("infobar: " + complaint).second) {
      std::printf("infobar: %s\n", complaint.c_str());
    }
  }
  ui_.set_content(platform::Bar::kUpper, content);
}

/// The command bar: the selection's rows as buttons, and the no-selection
/// menu when there is nothing selected.
void Application::refresh_cmdbar() {
  if (session_ == nullptr || cmdbar_ == nullptr) return;
  buttons_ = cmdbar_->describe(local_player_);
  // While a row waits for its target, the bar's own `CmdCancel` -- declared
  // `HIDDEN` with no rectangle, for the code to place -- stands at the end
  // of the row. **Reading, labelled:** where the original puts it is not
  // read; the end of the row is where a button the row does not otherwise
  // have goes without moving the others.
  if (!pending_command_.empty()) {
    if (const core::ui::Widget* cancel = ui_.bar_widget("CmdCancel")) {
      core::sim::CommandButton button;
      button.name = "CmdCancel";
      button.icon = cancel->image.path;
      button.rollover = cancel->help_text;
      button.enabled = true;
      buttons_.push_back(std::move(button));
    }
  }
  core::ui::BarContent content;
  content.tags = session_->selections().player(local_player_).empty()
                     ? core::ui::SelectionTag::kEmpty
                     : core::ui::SelectionTag::kNone;
  content.hovered = hovered_button_;
  content.pressed = pressed_button_.empty() ? pending_command_ : pressed_button_;
  for (const core::sim::CommandButton& button : buttons_) {
    core::ui::CommandButtonView view;
    view.name = button.name;
    view.icon = button.icon;
    view.enabled = button.enabled;
    content.buttons.push_back(std::move(view));
  }
  const core::game::TranslationTable* table = session_->host_context().translations;
  content.translate = [table](std::string_view key) -> std::string_view {
    return table == nullptr ? key : table->translate(key);
  };
  ui_.set_content(platform::Bar::kLower, content);
}

/// The command tooltip as `gbr.exe` composes it, tags and all.
///
/// 0x004f3590 formats the title through the table's own template --
/// `<color 255 255 0>%s1<color 255 255 255>  <imagetransp gameres/infobar/
/// common/hotkey.bmp> <color 255 255 255>%s2`, the key in upper case -- when
/// the row has a key, and wraps the bare name in yellow when it has not;
/// then the description on its own line; then 0x004ea790's cost line, one
/// `<imagetransp .../gold_cost.bmp> 250 ` per non-zero cost, white while the
/// settlement can pay and `<color 255 0 0>` when it cannot, the stamina icon
/// swapping for `Loyality.bmp` at ten and above; then the verifier's reason.
/// The costs are tested against the selection's settlement, as the five
/// `*_VERIFY.VS` scripts test them; with none selected every cost is white.
std::vector<std::string> Application::command_tooltip(const core::sim::CommandButton& button) const {
  const core::game::TranslationTable* table = session_->host_context().translations;
  // A row's texts are keyed `<text>@<row name>` -- `Move@move`, "Rollover for
  // command move".
  const auto text = [&](const std::string& key) -> std::string {
    return std::string(table == nullptr ? std::string_view(key)
                                        : table->translate_in_context(key, button.name));
  };
  std::vector<std::string> lines;
  const std::string name = button.rollover.empty() ? button.name : text(button.rollover);
  if (button.key.empty()) {
    lines.push_back("<color 255 255 0>" + name + "<color 255 255 255>");
  } else {
    std::string key = button.key;
    for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    // legal-ok: this is the language table's lookup key for the tooltip's
    // title (0x004f3590 translates the template itself), so it has to be
    // spelled as the table spells it; a key names content and is not content.
    static constexpr std::string_view kTitle =
        "<color 255 255 0>%s1<color 255 255 255>  <imagetransp gameres/infobar/common/hotkey.bmp> "
        "<color 255 255 255>%s2";
    std::string title(table == nullptr ? kTitle : table->translate(kTitle));
    if (const std::size_t at = title.find("%s1"); at != std::string::npos) title.replace(at, 3, name);
    if (const std::size_t at = title.find("%s2"); at != std::string::npos) title.replace(at, 3, key);
    lines.push_back(std::move(title));
  }
  if (!button.description.empty()) lines.push_back(text(button.description));

  // What the selection's settlement holds, for the colour of each cost.
  std::int32_t gold = -1;
  std::int32_t food = -1;
  std::int32_t population = -1;
  std::int32_t min_population = 0;
  if (const core::sim::EconomySystem* economy = core::sim::economy_of(session_->world());
      economy != nullptr) {
    const core::sim::Selection& selection = session_->selections().player(local_player_);
    for (const core::ObjectId id : selection.ids()) {
      const core::sim::WorldObject* slot = session_->world().find(id);
      if (slot == nullptr || slot->settlement == core::kNoObject) continue;
      const core::sim::Settlement* town = economy->settlements().for_object(slot->settlement);
      if (town == nullptr) continue;
      gold = town->gold();
      food = town->food();
      population = town->population;
      min_population = economy->rules().min_population;
      break;
    }
  }
  std::string costs;
  const auto cost = [&](std::int32_t amount, bool affordable, std::string_view icon) {
    if (amount <= 0) return;
    costs += affordable ? "<color 255 255 255>" : "<color 255 0 0>";
    costs += "<imagetransp gameres/infobar/common/";
    costs += icon;
    costs += "> ";
    costs += std::to_string(amount);
    costs += ' ';
  };
  cost(button.cost_gold, gold < 0 || button.cost_gold <= gold, "gold_cost.bmp");
  cost(button.cost_food, food < 0 || button.cost_food <= food, "food_cost.bmp");
  // Stamina is the hero's, which the bar does not read here; it stays white.
  cost(button.cost_stamina, true, button.cost_stamina >= 10 ? "Loyality.bmp" : "stamina_cost.bmp");
  cost(button.cost_pop, population < 0 || button.cost_pop + min_population <= population,
       "pop_cost.bmp");
  if (!costs.empty()) lines.push_back(costs + "<color 255 255 255>");
  if (!button.enabled && !button.reason.empty()) lines.push_back(button.reason);
  return lines;
}

std::vector<std::string> Application::widget_tooltip(const core::ui::Widget* widget) const {
  std::vector<std::string> lines;
  if (widget == nullptr || (widget->help_text.empty() && widget->rollover.empty())) return lines;
  const core::game::TranslationTable* table =
      session_ != nullptr ? session_->host_context().translations : translations_.get();
  const bool from_help = !widget->help_text.empty();
  const std::string& key = from_help ? widget->help_text : widget->rollover;
  const core::ui::Screen* screen = ui_.screen_of(widget);
  if (table == nullptr) {
    lines.emplace_back(key);
  } else {
    lines.emplace_back(table->translate_in_context(
        key, screen == nullptr ? std::string()
                               : core::ui::translation_context(screen->path, widget->name,
                                                               from_help ? "HelpText" : "Rollover")));
  }
  return lines;
}

/// What the pointer at `(x, y)` is over: a highlighted button and a tooltip.
void Application::update_hover(std::int32_t x, std::int32_t y) {
  pointer_x_ = x;
  pointer_y_ = y;
  if (session_ == nullptr || cmdbar_ == nullptr) return;
  // Hover over the command buttons, for the highlighted frame.
  const std::int32_t index = ui_.button_at(x,
                                           y);
  std::string over;
  if (index >= 0 && static_cast<std::size_t>(index) < buttons_.size()) {
    over = buttons_[static_cast<std::size_t>(index)].name;
  }
  if (over != hovered_button_) {
    hovered_button_ = over;
    bars_dirty_ = true;
  }
  // The tooltip: a command button's row, or a widget's `HelpText`. Each
  // paragraph carries the inline markup the original composes with, which
  // the renderer draws (`core/ui/markup.hpp`).
  std::vector<std::string> lines;
  if (index >= 0 && static_cast<std::size_t>(index) < buttons_.size()) {
    lines = command_tooltip(buttons_[static_cast<std::size_t>(index)]);
  } else {
    lines = widget_tooltip(ui_.widget_at(x, y));
  }
  // A paragraph that is blank once its markup is gone -- a verifier's reason
  // that ends in `\n<color 255 0 0>` and nothing after -- is not a line.
  std::vector<std::string> split;
  for (std::string& raw : lines) {
    const std::string plain = core::ui::strip_markup(raw);
    if (plain.find_first_not_of(" \r\n\t") == std::string::npos) continue;
    split.push_back(std::move(raw));
  }
  ui_.set_tooltip(std::move(split), x + 12,
                  y - 8);
}

/// A press on button `index`: issued at once, or armed to wait for a click
/// on the map, or refused with the verifier's reason.
void Application::press_button(std::int32_t index, core::sim::CommandBar::Keys keys) {
  if (index < 0 || static_cast<std::size_t>(index) >= buttons_.size()) return;
  const core::sim::CommandButton& button = buttons_[static_cast<std::size_t>(index)];
  const std::string name = button.name;
  // A bar button is a control like any other, and its activation clicks.
  // **Inferred**: the bar's buttons post the same activation message.
  play_click(0);
  if (name == "CmdCancel") {
    std::printf("command: %s cancelled\n", pending_command_.c_str());
    pending_command_.clear();
    bars_dirty_ = true;
    return;
  }
#if IMPERIVM_HAVE_NET
  if (net_ != nullptr) {
    // The verdict now, the effect on the agreed turn.
    const core::sim::CommandBar::Press verdict = cmdbar_->check(local_player_, name);
    if (verdict == core::sim::CommandBar::Press::issued) {
      core::sim::NetOrder order;
      const std::span<const core::ObjectId> selected =
          session_->selections().player(local_player_).ids();
      order.actors.assign(selected.begin(), selected.end());
      order.kind = core::sim::NetOrderKind::command;
      order.command = name;
      const core::sim::CommandBar::Flags flags = cmdbar_->flags(local_player_, name, keys);
      order.mode = flags.append ? core::sim::OrderMode::append : core::sim::OrderMode::replace;
      order.modifier = flags.modifier;
      order.repeat = flags.repeat;
      net_->queue(std::move(order));
      std::printf("command: %s queued\n", name.c_str());
      pending_command_.clear();
      bars_dirty_ = true;
      return;
    }
    if (verdict != core::sim::CommandBar::Press::waiting) {
      std::printf("command: %s %s\n", name.c_str(),
                  verdict == core::sim::CommandBar::Press::disabled ? "refused" : "is not offered");
      bars_dirty_ = true;
      return;
    }
    // A row that wants a target waits for the click, as it does alone.
  }
#endif
  switch (networked() ? core::sim::CommandBar::Press::waiting
                      : cmdbar_->press(local_player_, name, keys)) {
    case core::sim::CommandBar::Press::issued: {
      std::printf("command: %s issued\n", name.c_str());
      pending_command_.clear();
      const std::span<const core::ObjectId> selected =
          session_->selections().player(local_player_).ids();
      if (!selected.empty()) print_queue(selected.front());
      break;
    }
    case core::sim::CommandBar::Press::waiting:
      std::printf("command: %s -- click the target\n", name.c_str());
      pending_command_ = name;
      break;
    case core::sim::CommandBar::Press::disabled:
      std::printf("command: %s refused%s%s\n", name.c_str(), button.reason.empty() ? "" : ": ",
                  button.reason.c_str());
      break;
    case core::sim::CommandBar::Press::unknown:
      std::printf("command: %s is not offered\n", name.c_str());
      break;
  }
  bars_dirty_ = true;
}

void Application::print_queue(core::ObjectId id) const {
  if (session_ == nullptr) return;
  const core::sim::CommandSystem* commands = core::sim::command_system(session_->world());
  const core::sim::CommandQueue* queue = commands == nullptr ? nullptr : commands->find(id);
  std::printf("queue:        object %u holds %zu", static_cast<unsigned>(id),
              queue == nullptr ? std::size_t{0} : queue->size());
  if (queue != nullptr) {
    for (const core::sim::Command& command : queue->entries) {
      std::printf(" | %s", command.name.empty() ? command.verb.c_str() : command.name.c_str());
    }
  }
  std::printf("\n");
  std::fflush(stdout);
}

/// The scaffolding HUD.
///
/// Printed to the terminal rather than drawn, on purpose: the real interface is
/// 24 declarative INI files and an interpreter for them is being built in
/// parallel, so anything drawn here would be thrown away twice. What this is
/// for is putting a human in front of a running simulation today, which finds
/// things no test phrases.
void Application::draw_status() {
  if (session_ == nullptr) return;
  // Once a second of wall time, not once a frame.
  static std::uint64_t last_print = 0;
  const std::uint64_t now = SDL_GetTicks();
  ++frames_since_status_;
  if (now - last_print < 1000) return;
  const std::uint64_t last_print_at = last_print;
  last_print = now;

  const double fps = static_cast<double>(frames_since_status_) * 1000.0 /
                     static_cast<double>(now - last_print_at);
  frames_since_status_ = 0;

  const core::sim::SessionReport report = session_->report();
  // **What the world could not do, as it happens.** Traps and launch failures
  // were both invisible here: the status line said the simulation was healthy
  // while every order silently retired. Printed once each, the first time they
  // appear, so a long session does not scroll.
  for (const core::sim::SessionReport::Trap& trap : report.traps) {
    if (reported_.emplace(trap.message).second) {
      std::printf("trap: %s (%zu script(s))\n", trap.message.c_str(), trap.scripts);
    }
  }
  for (const core::sim::SessionReport::LaunchFailure& miss : report.launch_failures) {
    if (reported_.emplace("verb:" + miss.verb).second) {
      std::printf("command '%s' has no script this build can compile\n", miss.verb.c_str());
    }
  }
  std::printf("turn %-6llu  scripts %4zu running  %5.1f fps  hash %016llx%s\n",
              static_cast<unsigned long long>(report.turns), report.scripts_running, fps,
              static_cast<unsigned long long>(report.hash), paused_ ? "  [paused]" : "");
  std::fflush(stdout);
}

// --------------------------------------------------------------------------
// map mode
// --------------------------------------------------------------------------

bool Application::start_map() {
  std::string error;
  const std::filesystem::path path = vfs_.root() / args_.map;
  if (!container_.open(path, &error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return false;
  }

  // The season selects the `%season%` half of every terrain texture path and
  // the class graph's seasonal entity override, so it is read before anything
  // is resolved.
  core::GameProperties game;
  if (const auto bytes = container_.read("game.xml"); !bytes.empty()) {
    if (auto parsed = core::GameProperties::parse(bytes); parsed) game = parsed.value();
  }
  season_ = core::season_from_name(game.season);
  game_ = game;

  const std::vector<std::string> maps = container_.map_directories();
  if (maps.empty()) {
    std::fprintf(stderr, "%s: holds no Maps/<n>/map.xml\n", args_.map.c_str());
    return false;
  }
  // Map numbers are not contiguous -- the conquest uses 3, 4 and 6 to 10 -- so
  // the requested one is matched by name, never by position.
  std::string directory = maps.front();
  const int wanted = args_.map_index >= 0 ? args_.map_index : game.start_map;
  const std::string requested = "Maps/" + std::to_string(wanted);
  for (const std::string& candidate : maps) {
    if (candidate == requested) directory = candidate;
  }
  map_directory_ = directory;

  const auto pass = container_.read(directory + "/Terrain.pass.grid");
  const auto height = container_.read(directory + "/Terrain.height.grid");
  const auto light = container_.read(directory + "/Terrain.light.grid");
  const auto terrain = container_.read(directory + "/Terrain.terrain.grid");
  const auto decor = container_.read(directory + "/Terrain.decor.grid");
  const auto transitions = container_.read(directory + "/Terrain.trans.grid");
  const auto map_xml = container_.read(directory + "/map.xml");
  const auto object_xml = container_.read(directory + "/map.obj.xml");

  core::MapLayerBytes bytes;
  bytes.map_xml = map_xml;
  bytes.object_xml = object_xml;
  bytes.pass = pass;
  bytes.height = height;
  bytes.light = light;
  bytes.terrain = terrain;
  bytes.decor = decor;
  bytes.trans = transitions;

  auto loaded = core::WorldMap::load(bytes);
  if (!loaded) {
    std::fprintf(stderr, "%s/%s: map did not load (error %d)\n", args_.map.c_str(),
                 directory.c_str(), static_cast<int>(loaded.error()));
    return false;
  }
  world_ = std::move(loaded.value());

  std::printf("map:      %s %s (\"%s\"), %d x %d world units, season %s\n",
              args_.map.c_str(), directory.c_str(), world_.geometry().name.c_str(),
              world_.geometry().size_x, world_.geometry().size_y, game.season.c_str());
  std::printf("objects:  %zu, %zu settlements, %zu groups\n",
              world_.objects().objects().size(), world_.objects().settlements().size(),
              world_.objects().groups().size());
  std::printf("terrain:  %u x %u cells\n", world_.terrain_cells(), world_.terrain_cells());

  if (!renderers_ready_) {
    if (!map_.create(vfs_, renderer_, window_.device(), &error)) {
      std::fprintf(stderr, "map renderer: %s\n", error.c_str());
      return false;
    }
    renderers_ready_ = true;
  }
  // Player colours, in slot order. `player<i>.xml` is absent from the four
  // blank templates, so a missing slot is left black and falls back to the
  // sprite's own neutral palette.
  std::vector<core::Rgb888> players(core::kPlayerSlots);
  for (std::size_t i = 0; i < players.size(); ++i) {
    const auto document = container_.read("player" + std::to_string(i) + ".xml");
    if (document.empty()) continue;
    if (auto slot = core::PlayerSlot::parse(document); slot) players[i] = slot->color;
  }

  if (!map_.load(world_, game.season, players, &error)) {
    std::fprintf(stderr, "map renderer: %s\n", error.c_str());
    return false;
  }
  // The live view draws the map's decorations from the layer, which the
  // simulation never loads: a tree is scenery, not an object.
  world_view_.set_decorations(&world_.decor(), &map_.decors());

  const platform::MapRenderer::Stats& stats = map_.stats();
  std::printf("classes:  %zu, %zu entities, %zu sprite sheets\n", stats.classes,
              stats.entities, stats.sheets);
  std::printf("placed:   %zu of %zu objects, %zu decorations, %zu terrain layers\n",
              stats.placed, stats.objects, stats.decorations, stats.terrain_layers);
  std::printf("atlas:    %zu page(s), %.1f MiB of R8 indices\n", renderer_.atlas_pages(),
              static_cast<double>(renderer_.atlas_bytes()) / (1024.0 * 1024.0));

  look_pending_ = true;
  look_x_ = args_.at_x >= 0 ? args_.at_x
                            : (world_.geometry().has_start ? world_.geometry().start_x
                                                           : world_.geometry().size_x / 2);
  look_y_ = args_.at_y >= 0 ? args_.at_y
                            : (world_.geometry().has_start ? world_.geometry().start_y
                                                           : world_.geometry().size_y / 2);
  // Play mode points the camera again once the match is set up; see
  // `start_play`. No path reaches here with a session built yet.
  return true;
}

// --------------------------------------------------------------------------
// the editor
// --------------------------------------------------------------------------

bool Application::is_editor_pane(const core::ui::Dialog& dialog) noexcept {
  // Every editor screen but the save dialog is a tool window over the map.
  const std::string& path = dialog.screen().path;
  std::string folded;
  for (const char c : path) folded.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  // `editorini/` is an alias for `data/interface/editor/`, and a screen's
  // path is kept resolved.
  return (folded.find("editorini/") != std::string::npos || folded.find("interface/editor/") != std::string::npos) &&
         folded.find("savedlg") == std::string::npos && folded.find("opendlg") == std::string::npos;
}

/// `--select`'s grammar, and the `select:`/`look:` input steps': a comma
/// list of `class:NAME[@owner]` (that owner's first live object of the
/// class), `mine:N` (the local player's first N live objects not held) and
/// object ids, resolved in order against the world as it is now.
std::vector<core::ObjectId> Application::resolve_objects(const std::string& spec) const {
  std::vector<core::ObjectId> out;
  if (session_ == nullptr) return out;
  const core::sim::World& world = session_->world();
  const auto add = [&out](core::ObjectId id) {
    if (std::find(out.begin(), out.end(), id) != out.end()) return false;
    out.push_back(id);
    return true;
  };
  std::size_t start = 0;
  while (start < spec.size()) {
    std::size_t end = spec.find(',', start);
    if (end == std::string::npos) end = spec.size();
    const std::string token = spec.substr(start, end - start);
    start = end + 1;
    // `class:RTownhall`: the local player's first live object of that class,
    // which is how a runtime-spawned town is reached without knowing its id.
    if (token.rfind("class:", 0) == 0) {
      // `class:BaseBarracks@2` names another owner's, for looking at what
      // an AI player is doing.
      std::string class_id = token.substr(6);
      core::PlayerId owner = local_player_;
      if (const std::size_t at = class_id.find('@'); at != std::string::npos) {
        owner = static_cast<core::PlayerId>(SDL_atoi(class_id.substr(at + 1).c_str()));
        class_id.erase(at);
      }
      const core::ClassGraph* graph = world.class_graph();
      const core::ClassIndex wanted = graph == nullptr ? core::kNoClass : graph->find(class_id);
      if (wanted == core::kNoClass) continue;
      for (const core::sim::WorldObject& slot : world.objects()) {
        if (slot.class_index == core::kNoClass || slot.state.owner != owner) continue;
        if (!world.class_is_a(slot.id, wanted)) continue;
        if (slot.state.flags.unspawned) continue;
        add(slot.id);
        break;
      }
      continue;
    }
    // `mine:8`: the local player's first eight live objects, ascending id --
    // what a scripted run of a networked match selects, since each window
    // plays a different seat and no one id list fits both.
    if (token.rfind("mine:", 0) == 0) {
      std::size_t wanted = static_cast<std::size_t>(std::max(0, SDL_atoi(token.c_str() + 5)));
      for (const core::sim::WorldObject& slot : world.objects()) {
        if (wanted == 0) break;
        if (slot.state.owner != local_player_ || slot.state.health <= 0) continue;
        if (slot.state.holder != core::kNoObject) continue;
        if (add(slot.id)) --wanted;
      }
      continue;
    }
    const int id = SDL_atoi(token.c_str());
    if (id > 0 && world.find(static_cast<core::ObjectId>(id)) != nullptr) add(static_cast<core::ObjectId>(id));
  }
  return out;
}

/// `--select`: the local player's selection before the first frame.
void Application::apply_select_argument() {
  if (args_.select.empty()) return;
  core::sim::Selection& selection = session_->selections().player(local_player_);
  for (const core::ObjectId id : resolve_objects(args_.select)) (void)selection.add(id);
  std::printf("selected:     %zu object(s) from --select\n", selection.size());
}

void Application::start_editor() {
  editor_ = Editor{};
  editor_.active = true;
  editor_.object_ids = session_->populated().object_ids;
  paused_ = true;
  ui_.show_bars(false);
  // The five blank templates store the terrain at four bits a cell, which
  // holds no layer past 15; the original unpacks to bytes at load and
  // writes bytes, and so does this.
  if (!world_.widen_terrain()) std::printf("editor: the terrain layer could not be widened to eight bits\n");
  build_editor_tree();
  editor_load_keys();
  // The decoration kinds' masks, for the passability rebuild.
  editor_.decor_masks.assign(256, nullptr);
  for (const core::DecorKind& kind : map_.decors().kinds()) {
    if (kind.type <= 0 || kind.type >= 256 || entities_ == nullptr) continue;
    editor_.decor_masks[static_cast<std::size_t>(kind.type)] =
        entities_->mask_of(entities_->resolve(kind.entity));
  }
  // The whole layer rebuilt over the map as loaded, as the original's editor
  // does when it opens one (0x00494cca): what a save writes is then a
  // rebuild plus this session's edits, never an older editor's leftovers.
  editor_rebuild_passability(core::edit::WorldRect::of_map(world_.terrain()));
  // The areas the map authored: by `num`, through the authored index.
  {
    const core::MapObjectList& list = world_.objects();
    std::map<std::int32_t, std::size_t> index_of_num;
    for (std::size_t i = 0; i < list.objects().size(); ++i) index_of_num[list.objects()[i].num] = i;
    for (const core::MapArea& area : list.areas()) {
      const auto found = index_of_num.find(area.num);
      if (found == index_of_num.end() || found->second >= editor_.object_ids.size()) continue;
      EditorArea entry;
      entry.id = editor_.object_ids[found->second];
      entry.shape = area;
      for (const core::MapGroup& group : list.groups()) {
        if (group.type == core::kGroupAlias && group.members.size() == 1 && group.members[0] == area.num) {
          entry.name = group.name;
          break;
        }
      }
      editor_.areas.push_back(std::move(entry));
    }
  }
  // The palette, at the right as the original's floating dialog opens; the
  // tool's pane docks where `SettingsPosCtl` stands (8, 253).
  core::ui::Dialog* palette = open_menu(
      "editorini/MapToolsDlg.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        if ((event.kind == Kind::kSelect || event.kind == Kind::kActivate) && event.widget == "Browser") {
          if (event.index < 0 || static_cast<std::size_t>(event.index) >= editor_.rows.size()) return;
          editor_choose(editor_.rows[static_cast<std::size_t>(event.index)]);
          (void)menu;
          return;
        }
        if (event.kind == Kind::kCommand && event.id == 0x5000) editor_toggle_collapse(&menu);
        if (event.kind == Kind::kCommand && event.id == 0x100BB) return;
      });
  if (palette == nullptr) return;
  editor_.palette = palette;
  ui_.place_dialog(palette, 1024 - 8 - palette->screen().design.width, 8);
  palette->set_text("Caption2", "");
  // The Adventure Palette beside it, at the left.
  open_explorer();
  // The builder ends on Edit objects (0x004a36e0).
  for (std::size_t i = 0; i < editor_.nodes.size(); ++i) {
    if (editor_.nodes[i].is_tool && editor_.nodes[i].tool == EditorTool::kEdit) {
      editor_choose(static_cast<std::int32_t>(i));
      break;
    }
  }
  refresh_editor_palette();
  // `--select` names the default tool's object too, and the view goes to
  // it: a scripted run reaches an object's sheet without a click.
  {
    apply_select_argument();
    const core::sim::Selection& selection = session_->selections().player(local_player_);
    if (selection.size() > 0) {
      editor_.selected = selection.ids().front();
      if (const core::sim::WorldObject* object = session_->world().find(editor_.selected)) {
        editor_.drag_from = object->state.position;
        look_x_ = object->state.position.x;
        look_y_ = object->state.position.y;
        look_pending_ = true;
      }
    }
  }
  std::printf("editor:       %zu nodes in the palette, %zu objects on the map, %zu areas; %zu shortcuts\n",
              editor_.nodes.size(), session_->world().objects().size(), editor_.areas.size(),
              editor_.keys.size());
}

/// The palette's tree, in the order the exe builds it (0x004a29a0):
/// Decorations, Delete decorations, Height, Terrains, the classes'
/// `edittree_pos` tree, Areas, Edit objects. Every label goes through
/// `<leaf>@editortree/<parent path>` as 0x004a277b pushes the context.
void Application::build_editor_tree() {
  editor_.nodes.clear();
  const core::ClassGraph* graph = session_->world().class_graph();
  if (graph == nullptr) return;
  std::map<std::string, std::int32_t> by_path;
  // The table keys a class leaf by its parent's path (`Barracks@editortree/
  // Structures/Stronghold (Gaul)`) and every native node by the bare
  // context (`Grass@editortree`), so the parent's context is tried first
  // and the root's is the fallback.
  const auto label_for = [&](std::string_view leaf, std::int32_t parent) {
    if (parent >= 0) {
      const std::string scoped =
          item_label(leaf, "editortree/" + editor_.nodes[static_cast<std::size_t>(parent)].path);
      if (scoped != leaf) return scoped;
    }
    return item_label(leaf, "editortree");
  };
  const auto add = [&](std::string_view leaf, std::int32_t parent) -> std::int32_t {
    EditorNode node;
    node.path = parent < 0 ? std::string(leaf)
                           : editor_.nodes[static_cast<std::size_t>(parent)].path + "/" + std::string(leaf);
    node.label = label_for(leaf, parent);
    node.parent = parent;
    editor_.nodes.push_back(std::move(node));
    const auto index = static_cast<std::int32_t>(editor_.nodes.size()) - 1;
    if (parent >= 0) editor_.nodes[static_cast<std::size_t>(parent)].children.push_back(index);
    by_path.emplace(editor_.nodes[static_cast<std::size_t>(index)].path, index);
    return index;
  };
  const std::function<std::int32_t(std::string_view)> node_for = [&](std::string_view path) -> std::int32_t {
    if (path.empty()) return -1;
    if (const auto it = by_path.find(std::string(path)); it != by_path.end()) return it->second;
    const std::size_t slash = path.rfind('/');
    const std::int32_t parent = node_for(slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash));
    return add(slash == std::string_view::npos ? path : path.substr(slash + 1), parent);
  };

  // Decorations: `group` / `subgroup` / `name` from DECORS.INI, in section
  // order (0x0048c8c0); a kind with no group stands in no tree, and every
  // node holds the kinds beneath it. The root itself holds none.
  {
    const std::int32_t root = add("Decorations", -1);
    editor_.nodes[static_cast<std::size_t>(root)].pane = "editorini/DecorSettings.ini";
    for (const core::DecorKind& kind : map_.decors().kinds()) {
      if (kind.group.empty()) continue;
      const std::int32_t group = node_for("Decorations/" + kind.group);
      std::int32_t parent = group;
      if (!kind.subgroup.empty()) parent = node_for("Decorations/" + kind.group + "/" + kind.subgroup);
      const std::int32_t leaf = add(kind.name.empty() ? kind.section : kind.name, parent);
      for (const std::int32_t at : {group, parent, leaf}) {
        EditorNode& node = editor_.nodes[static_cast<std::size_t>(at)];
        if (!node.values.empty() && node.values.back() == kind.type) continue;
        node.values.push_back(kind.type);
        node.is_tool = true;
        node.tool = EditorTool::kDecor;
        node.pane = "editorini/DecorSettings.ini";
      }
    }
  }
  {
    const std::int32_t leaf = add("Delete decorations", -1);
    EditorNode& node = editor_.nodes[static_cast<std::size_t>(leaf)];
    node.is_tool = true;
    node.tool = EditorTool::kDecorDelete;
    node.pane = "editorini/DecorDelete.ini";
  }
  {
    const std::int32_t root = add("Height", -1);
    const struct {
      const char* label;
      core::edit::HeightTool tool;
      const char* pane;
    } tools[] = {{"Raise/lower", core::edit::HeightTool::kRaiseLower, "editorini/HeightPaintSettings.ini"},
                 {"Smooth", core::edit::HeightTool::kSmooth, "editorini/HeightBlurSettings.ini"},
                 {"Set height", core::edit::HeightTool::kSet, "editorini/HeightSetSettings.ini"}};
    for (const auto& tool : tools) {
      const std::int32_t leaf = add(tool.label, root);
      EditorNode& node = editor_.nodes[static_cast<std::size_t>(leaf)];
      node.is_tool = true;
      node.tool = EditorTool::kHeight;
      node.height_tool = tool.tool;
      node.pane = tool.pane;
    }
  }
  // Terrains: one group per TERRAINS.XML `type` -- the `tt*` names without
  // their prefix (0x004a370c) -- with a leaf per layer of that type, by the
  // layer's own name. Waves (7) are skipped; Invalid (0) only with
  // Config.ini's `InvalidTerrains`, which this engine does not read, so it
  // is skipped too. A group paints one of its layers per cell; Water paints
  // deep water (0x004b316b).
  {
    const std::int32_t root = add("Terrains", -1);
    editor_.nodes[static_cast<std::size_t>(root)].pane = "editorini/TerrainSettings.ini";
    const char* groups[] = {"Invalid", "Grass", "Ground", "Sand", "Water", "Rocks", "Roads"};
    for (std::int32_t type = 1; type <= 6; ++type) {
      const std::int32_t group = add(groups[type], root);
      EditorNode& group_node = editor_.nodes[static_cast<std::size_t>(group)];
      group_node.is_tool = true;
      group_node.tool = EditorTool::kTerrain;
      group_node.pane = "editorini/TerrainSettings.ini";
      group_node.water_group = type == 4;
      for (const core::TerrainLayerDef& layer : map_.terrain_table().layers()) {
        if (layer.type != type) continue;
        editor_.nodes[static_cast<std::size_t>(group)].values.push_back(layer.z);
        const std::int32_t leaf = add(layer.display.empty() ? std::to_string(layer.z) : layer.display, group);
        EditorNode& node = editor_.nodes[static_cast<std::size_t>(leaf)];
        node.is_tool = true;
        node.tool = EditorTool::kTerrain;
        node.pane = "editorini/TerrainSettings.ini";
        node.values.push_back(layer.z);
      }
    }
  }
  // The classes, by their `edittree_pos`.
  for (std::size_t i = 0; i < graph->size(); ++i) {
    const core::ClassDefinition& definition = graph->at(static_cast<core::ClassIndex>(i));
    std::string_view position;
    for (const core::ClassProperty& property : definition.properties) {
      if (property.key == "edittree_pos") position = property.value;
    }
    if (position.empty()) continue;
    const std::size_t slash = position.rfind('/');
    const std::int32_t parent = node_for(slash == std::string_view::npos ? std::string_view{} : position.substr(0, slash));
    const std::int32_t leaf = add(slash == std::string_view::npos ? position : position.substr(slash + 1), parent);
    EditorNode& node = editor_.nodes[static_cast<std::size_t>(leaf)];
    node.class_id = std::string(definition.id);
    node.is_tool = true;
    node.tool = EditorTool::kPlace;
    node.pane = "editorini/PlaceObj.ini";
  }
  {
    const std::int32_t root = add("Area", -1);
    const struct {
      const char* label;
      EditorTool tool;
      bool circle;
    } tools[] = {{"Place circular area", EditorTool::kPlaceArea, true},
                 {"Place rectangular area", EditorTool::kPlaceArea, false},
                 {"Edit areas", EditorTool::kEditArea, false}};
    for (const auto& tool : tools) {
      const std::int32_t leaf = add(tool.label, root);
      EditorNode& node = editor_.nodes[static_cast<std::size_t>(leaf)];
      node.is_tool = true;
      node.tool = tool.tool;
      node.circle = tool.circle;
      node.pane = tool.tool == EditorTool::kEditArea ? "editorini/AdvArea.ini" : "";
    }
  }
  {
    const std::int32_t leaf = add("Edit objects", -1);
    EditorNode& node = editor_.nodes[static_cast<std::size_t>(leaf)];
    node.is_tool = true;
    node.tool = EditorTool::kEdit;
    node.pane = "editorini/DefaultTool.ini";
  }
  // Within the class tree, branches before leaves in name order, as a tree
  // control shows them; the native roots and their children stay in the
  // order the exe inserted them.
  const auto sort_children = [&](std::vector<std::int32_t>& children) {
    std::stable_sort(children.begin(), children.end(), [&](std::int32_t a, std::int32_t b) {
      const EditorNode& na = editor_.nodes[static_cast<std::size_t>(a)];
      const EditorNode& nb = editor_.nodes[static_cast<std::size_t>(b)];
      if (na.class_id.empty() != nb.class_id.empty()) return na.class_id.empty();
      return na.label < nb.label;
    });
  };
  const std::function<bool(std::int32_t)> in_class_tree = [&](std::int32_t index) {
    const EditorNode& node = editor_.nodes[static_cast<std::size_t>(index)];
    if (!node.class_id.empty()) return true;
    return std::any_of(node.children.begin(), node.children.end(), in_class_tree);
  };
  for (std::size_t i = 0; i < editor_.nodes.size(); ++i) {
    EditorNode& node = editor_.nodes[i];
    if (!node.children.empty() && node.tool != EditorTool::kDecor && node.tool != EditorTool::kTerrain &&
        node.path.rfind("Decorations", 0) != 0 && node.path.rfind("Height", 0) != 0 &&
        node.path.rfind("Areas", 0) != 0 && in_class_tree(static_cast<std::int32_t>(i))) {
      sort_children(node.children);
    }
  }
}

void Application::refresh_editor_palette() {
  core::ui::Dialog* palette = nullptr;
  for (std::size_t i = 0; i < ui_.dialog_count(); ++i) {
    if (ui_.dialog(i) != nullptr && ui_.dialog(i)->screen().path.find("MapToolsDlg") != std::string::npos) {
      palette = ui_.dialog(i);
    }
  }
  if (palette == nullptr) return;
  editor_.rows.clear();
  std::vector<std::string> items;
  std::int32_t selected = -1;
  const std::function<void(std::int32_t, int)> walk = [&](std::int32_t index, int depth) {
    const EditorNode& node = editor_.nodes[static_cast<std::size_t>(index)];
    std::string text(static_cast<std::size_t>(depth) * 2, ' ');
    text += node.children.empty() ? "  " : (node.expanded ? "- " : "+ ");
    text += node.label;
    if (index == editor_.chosen) selected = static_cast<std::int32_t>(items.size());
    items.push_back(std::move(text));
    editor_.rows.push_back(index);
    if (node.expanded) {
      for (const std::int32_t child : node.children) walk(child, depth + 1);
    }
  };
  for (std::size_t i = 0; i < editor_.nodes.size(); ++i) {
    if (editor_.nodes[i].parent < 0) walk(static_cast<std::int32_t>(i), 0);
  }
  // The list keeps its place: `select` scrolls the chosen row into view,
  // which would drag the list away from a branch just unfolded above it.
  const std::int32_t scroll = palette->content().state("Browser").scroll;
  palette->set_items("Browser", std::move(items));
  palette->select("Browser", selected);
  palette->content().state("Browser").scroll =
      std::min(scroll, std::max(0, static_cast<std::int32_t>(editor_.rows.size()) - 1));
  palette->set_text("Caption2", editor_.chosen >= 0 ? editor_.nodes[static_cast<std::size_t>(editor_.chosen)].label
                                                    : std::string());
}

/// A node chosen in the palette: a branch folds, a tool becomes the map's
/// and docks its pane. The exe's selection handler (0x00453840) activates
/// the node and falls back to Edit objects when nothing took.
void Application::editor_choose(std::int32_t index) {
  if (index < 0 || static_cast<std::size_t>(index) >= editor_.nodes.size()) return;
  EditorNode& node = editor_.nodes[static_cast<std::size_t>(index)];
  if (!node.children.empty()) node.expanded = !node.expanded;
  if (node.is_tool) {
    editor_.chosen = index;
    editor_.tool = node.tool;
    editor_.placing = node.class_id;
    editor_.painting = false;
    editor_.area_handle = 0;
    if (node.tool != EditorTool::kEditArea) editor_.area_selected = -1;
    open_editor_pane(node);
  }
  refresh_editor_palette();
}

void Application::close_editor_pane() {
  // The tool's own pane, wherever it stands: the explorer and the property
  // sheet are windows of their own and stay.
  close_menu(editor_.tool_pane);
  editor_.tool_pane = nullptr;
}

/// The brush slot, 0..4, a picker's frame stands for: its position in the
/// widget's `Frames` (0x006b4580 stores the position, not the digit).
std::int32_t Application::editor_brush_slot(const core::ui::Dialog& pane, std::int32_t frame) const {
  const core::ui::Widget* brushes = pane.screen().find("Brushes");
  if (brushes == nullptr) return std::clamp(frame - 1, 0, 4);
  const std::string_view frames = brushes->attribute("Frames");
  for (std::size_t i = 0; i < frames.size(); ++i) {
    if (frames[i] - '0' == frame) return static_cast<std::int32_t>(std::min<std::size_t>(i, 4));
  }
  return std::clamp(frame - 1, 0, 4);
}

void Application::open_editor_pane(const EditorNode& node) {
  close_editor_pane();
  if (node.pane.empty()) return;
  core::ui::Dialog* palette = palette_dialog();
  if (palette == nullptr) return;
  const std::int32_t family = node.tool == EditorTool::kHeight ? 2 : node.tool == EditorTool::kTerrain ? 0 : 1;
  const EditorTool tool = node.tool;
  core::ui::Dialog* pane = open_menu(node.pane, [this, family, tool](const core::ui::DialogEvent& event,
                                                                   core::ui::Dialog& menu) {
    using Kind = core::ui::DialogEvent::Kind;
    if (event.kind == Kind::kChange && event.widget == "PlayerCombo") {
      editor_.player = static_cast<core::PlayerId>(std::clamp(event.index, 0, static_cast<int>(core::sim::kPlayerCount) - 1));
      return;
    }
    if (event.kind == Kind::kChange && event.widget == "Brushes") {
      editor_.brush[family] = editor_brush_slot(menu, event.index);
      return;
    }
    if (event.kind == Kind::kChange && event.widget == "StrengthEdit") {
      std::int32_t value = 0;
      (void)core::parse_int(menu.text("StrengthEdit"), value);
      if (tool == EditorTool::kHeight && editor_.chosen >= 0 &&
          editor_.nodes[static_cast<std::size_t>(editor_.chosen)].height_tool == core::edit::HeightTool::kSet) {
        editor_.level = std::clamp(value, 0, 100);
      } else {
        editor_.amount = std::clamp(value, -100, 100);
      }
      return;
    }
    if (event.kind == Kind::kChange && event.widget == "DensityEdit") {
      std::int32_t value = 0;
      (void)core::parse_int(menu.text("DensityEdit"), value);
      editor_.density = std::clamp(value, 0, 100);
      return;
    }
    // The area dialog: the name, the shape, view and delete.
    if (tool == EditorTool::kEditArea && editor_.area_selected >= 0 &&
        static_cast<std::size_t>(editor_.area_selected) < editor_.areas.size()) {
      EditorArea& area = editor_.areas[static_cast<std::size_t>(editor_.area_selected)];
      if (event.kind == Kind::kChange && event.widget == "Name.Edit") {
        area.name = menu.text("Name.Edit");
        if (area.name.empty()) area.name = item_label("Unnamed", "advarea");
        return;
      }
      if (event.kind == Kind::kCommand && (event.id == 0x021001 || event.id == 0x021002)) {
        // A circle from a rectangle keeps the centre and takes the half
        // width; a rectangle from a circle is the circle's box. **This
        // engine's**: the exe's conversion (dialog slot +0x8c) was not read.
        const bool circle = event.id == 0x021001;
        const core::sim::AreaShape shape = area.shape.is_circle()
            ? core::sim::AreaShape::of_circle({area.shape.ptx, area.shape.pty}, area.shape.radius)
            : core::sim::AreaShape::of_rectangle(area.shape.left, area.shape.top, area.shape.right, area.shape.bottom);
        const core::sim::Point centre = shape.centre();
        if (circle && !area.shape.is_circle()) {
          area.shape.type = core::kAreaCircle;
          area.shape.ptx = centre.x;
          area.shape.pty = centre.y;
          area.shape.radius = std::max(1, (area.shape.right - area.shape.left) / 2);
        } else if (!circle && area.shape.is_circle()) {
          area.shape.type = core::kAreaRectangle;
          area.shape.left = centre.x - area.shape.radius;
          area.shape.right = centre.x + area.shape.radius;
          area.shape.top = centre.y - area.shape.radius;
          area.shape.bottom = centre.y + area.shape.radius;
        }
        menu.set_row("Circular.Radio", area.shape.is_circle() ? 1 : 0);
        menu.set_row("Rectangular.Radio", area.shape.is_circle() ? 0 : 1);
        return;
      }
      if (event.kind == Kind::kCommand && event.id == 0x0004) {
        const core::sim::AreaShape shape = area.shape.is_circle()
            ? core::sim::AreaShape::of_circle({area.shape.ptx, area.shape.pty}, area.shape.radius)
            : core::sim::AreaShape::of_rectangle(area.shape.left, area.shape.top, area.shape.right, area.shape.bottom);
        look_x_ = shape.centre().x;
        look_y_ = shape.centre().y;
        look_pending_ = true;
        return;
      }
      if (event.kind == Kind::kCommand && event.id == 0x0005) {
        editor_.selected = area.id;
        editor_delete();
        return;
      }
    }
  });
  if (pane == nullptr) return;
  editor_.tool_pane = pane;
  redock_windows();
  // Its numbers: the brush picker shows the family's slot, the edits the
  // tool's number.
  if (const core::ui::Widget* brushes = pane->screen().find("Brushes")) {
    const std::string_view frames = brushes->attribute("Frames");
    const auto slot = static_cast<std::size_t>(editor_.brush[family]);
    if (slot < frames.size()) pane->set_value("Brushes", frames[slot] - '0');
  }
  if (tool == EditorTool::kHeight) {
    const bool set = node.height_tool == core::edit::HeightTool::kSet;
    pane->set_text("StrengthEdit", std::to_string(set ? editor_.level : editor_.amount));
  }
  if (tool == EditorTool::kDecor) pane->set_text("DensityEdit", std::to_string(editor_.density));
  if (tool == EditorTool::kPlace) {
    std::vector<std::string> players;
    for (std::size_t i = 0; i < core::sim::kPlayerCount; ++i) {
      players.push_back(item_label("Player", "/Editor/PlaceDefence.ini:PlayerHint:Text") + " " + std::to_string(i + 1));
    }
    pane->set_items("PlayerCombo", std::move(players));
    pane->select("PlayerCombo", static_cast<std::int32_t>(editor_.player));
    pane->set_text("XToAddEdit", "0");
    pane->set_text("YToAddEdit", "0");
  }
  if (tool == EditorTool::kEditArea) open_area_dialog();
}

/// The area pane's contents for the selected area, or blank.
void Application::open_area_dialog() {
  core::ui::Dialog* pane = editor_.tool_pane;
  if (pane == nullptr || pane->screen().path.find("AdvArea") == std::string::npos) return;
  if (editor_.area_selected < 0 || static_cast<std::size_t>(editor_.area_selected) >= editor_.areas.size()) {
    pane->set_text("Name.Edit", "");
    pane->set_text("Point.Text", "0,0");
    return;
  }
  const EditorArea& area = editor_.areas[static_cast<std::size_t>(editor_.area_selected)];
  const core::sim::AreaShape shape = area.shape.is_circle()
      ? core::sim::AreaShape::of_circle({area.shape.ptx, area.shape.pty}, area.shape.radius)
      : core::sim::AreaShape::of_rectangle(area.shape.left, area.shape.top, area.shape.right, area.shape.bottom);
  pane->set_text("Name.Edit", area.name);
  pane->set_text("Point.Text", std::to_string(shape.centre().x) + "," + std::to_string(shape.centre().y));
  pane->set_row("Circular.Radio", area.shape.is_circle() ? 1 : 0);
  pane->set_row("Rectangular.Radio", area.shape.is_circle() ? 0 : 1);
}

// -- the property sheet ---------------------------------------------------------

namespace {
/// A class property as a number, or `fallback` when it is not one.
std::int32_t property_int(const core::ClassGraph* graph, core::ClassIndex index, std::string_view name,
                          std::int32_t fallback) {
  std::int32_t value = 0;
  if (graph == nullptr || index == core::kNoClass || !core::parse_int(graph->property(index, name), value)) return fallback;
  return value;
}
}  // namespace

// -- what marks an object in the world ------------------------------------------

const Application::ClassMarks& Application::class_marks(core::ClassIndex index) {
  static const ClassMarks kNone{};
  if (index == core::kNoClass || session_ == nullptr) return kNone;
  if (class_marks_.size() <= index) class_marks_.resize(static_cast<std::size_t>(index) + 1);
  ClassMarks& marks = class_marks_[index];
  if (!marks.read) {
    const core::ClassGraph* graph = session_->world().class_graph();
    marks.read = true;
    marks.selection_radius = property_int(graph, index, "selection_radius", 0);
    marks.radius = property_int(graph, index, "radius", 0);
    marks.healthbar_type = property_int(graph, index, "healthbar_type", 0);
    marks.healthbar_offset = property_int(graph, index, "healthbaroffset", 0);
    marks.max_health = property_int(graph, index, "maxhealth", 0);
  }
  return marks;
}

/// The ring and the bar one object shows this frame, as `gbr.exe` decides
/// them.
///
/// **The ring's colour** is the object's own virtual (`vtbl+0xd0`, asked at
/// 0x005a835b and written to `[visual+0x5e8]` by 0x0062ab60; -1 draws none):
///   * a unit's (0x005d3af0): selected, **yellow** (0x7fe0) when it serves a
///     hero (`Unit::hero`, `[unit+0x170]`) and **white** (0x7fff) when it
///     does not (0x0051d2a0); not selected, yellow while its hero is
///     selected, so selecting a hero rings its whole army; else none;
///   * a building's (0x005a7a60, `[obj+0x2c]` bit 23, the bit
///     `Obj::AsBuilding` tests): none in a match -- only the editor rings
///     one; and every other object's, white when selected;
///   * a class whose `selection_radius` is -15,000,000 has none at all.
/// Not reproduced: the green (0x03e0) of the one object held at
/// `[0x00996acc]` -- by its writers the object under a list's pointer, a
/// portrait hovered, which is a reading -- the red and green blink of
/// 0x005a8567 (a flash a registry at `[0x009c0d10]` asks for, as an order's
/// target), and 0x0051d2a0's pale green (0x2fef) when the globals' word at
/// `+0x174` is 3, a value no skirmish has been seen to hold.
///
/// **The bar** (0x0062ac80) shows for a class with a `healthbar_type`, when
/// the mode is on and the owner passes it: mode 2 wants the local player's
/// relation word towards the owner to have bit 0 -- ceasefire, which the
/// player holds with itself -- and mode 3 wants it clear
/// (`[[0x996ff4]+0x12c8]+0x24+4*owner`, 0x0062ad1d).
platform::WorldView::Marks Application::marks_for(const core::sim::WorldObject& object) {
  platform::WorldView::Marks marks;
  if (session_ == nullptr) return marks;
  core::sim::World& world = session_->world();
  const ClassMarks& klass = class_marks(object.class_index);
  const core::sim::Selection& selection = session_->selections().player(local_player_);
  constexpr std::int32_t kNoRing = -15000000;
  constexpr std::int32_t kWhite = 0x7FFF;
  constexpr std::int32_t kYellow = 0x7FE0;

  if (klass.selection_radius != kNoRing && !object.state.flags.is_building) {
    const bool selected = selection.contains(object.id);
    if (object.state.flags.is_unit) {
      const core::sim::HeroSystem* heroes = core::sim::hero_system_of(world);
      const core::ObjectId hero = heroes != nullptr ? heroes->hero_of(object.id) : core::kNoObject;
      const bool serves = hero != core::kNoObject && world.find(hero) != nullptr;
      if (selected) {
        marks.ring = serves ? kYellow : kWhite;
      } else if (serves && selection.contains(hero)) {
        marks.ring = kYellow;
      }
    } else if (selected) {
      marks.ring = kWhite;
    }
    marks.selection_radius = klass.selection_radius;
  }

  if (bar_mode_ != 0 && klass.healthbar_type > 0) {
    const core::sim::PlayerTable& players = world.players();
    const bool friendly = core::sim::PlayerTable::is_valid(local_player_) &&
                          core::sim::PlayerTable::is_valid(object.state.owner) &&
                          (players.relation_word(local_player_, object.state.owner) & 1u) != 0;
    const bool shown = bar_mode_ == 1 || (bar_mode_ == 2 && friendly) || (bar_mode_ == 3 && !friendly);
    if (shown) {
      marks.bar.type = klass.healthbar_type;
      marks.bar.radius = klass.radius;
      marks.bar.offset = klass.healthbar_offset;
      marks.bar.health = object.state.health;
      const core::sim::CombatSystem* combat = core::sim::combat_system_of(world);
      const std::int32_t max_health = combat != nullptr ? combat->max_health(object.id) : 0;
      marks.bar.max_health = max_health > 0 ? max_health : klass.max_health;
      marks.bar.stamina = object.state.stamina;
    }
  }
  return marks;
}

/// The band, while the left button is held over the world: a one-pixel
/// rectangle between the press and the pointer.
///
/// **Not read from `gbr.exe`, and labelled so.** A search of every caller of
/// the back buffer's line primitive (the `+0x38` slot the health bar draws
/// with) and of its bevelled rectangle (0x0063d990) found the health bar, the
/// interface's frames and a debug cross, and no band. What is drawn here is
/// the plainest reading: the corners the drag already keeps, in the white
/// the original rings a selected unit with.
void Application::draw_band() {
  band_drawn_ = false;
  if (!band_select_) return;
  const std::int32_t left = std::min(band_x0_, band_x1_);
  const std::int32_t top = std::min(band_y0_, band_y1_);
  const std::int32_t right = std::max(band_x0_, band_x1_);
  const std::int32_t bottom = std::max(band_y0_, band_y1_);
  // A press that has not moved is a click, not a band (the release treats
  // anything under four pixels so).
  if (right - left < 4 && bottom - top < 4) return;
  const platform::Rgba white = platform::rgb555(0x7FFF);
  const auto width = static_cast<float>(right - left + 1);
  const auto height = static_cast<float>(bottom - top + 1);
  renderer_.fill_rect(static_cast<float>(left), static_cast<float>(top), width, 1.0F, white);
  renderer_.fill_rect(static_cast<float>(left), static_cast<float>(bottom), width, 1.0F, white);
  renderer_.fill_rect(static_cast<float>(left), static_cast<float>(top), 1.0F, height, white);
  renderer_.fill_rect(static_cast<float>(right), static_cast<float>(top), 1.0F, height, white);
  band_drawn_ = true;
}

/// The backtick, as 0x00627ef0 reads `VK_OEM_3` (0xc0). A repeat is ignored,
/// as the original's own down flag (`[0x009edd30]`) ignores one.
void Application::bar_key(const SDL_Event& event) {
  const std::uint64_t now = SDL_GetTicks();
  if (event.type == SDL_EVENT_KEY_UP) {
    // Held past half a second: the bars were a peek, and go.
    if (bar_key_ticks_ != 0 && now - bar_key_ticks_ > 500) bar_mode_ = 0;
    bar_key_ticks_ = 0;
    return;
  }
  if (event.key.repeat) return;
  if ((event.key.mod & SDL_KMOD_CTRL) != 0) {
    // 1 -> 3 -> 2 -> 1, and from 0 to 2: (mode + 1) % 3 + 1.
    bar_mode_ = (bar_mode_ + 1) % 3 + 1;
  } else if (bar_mode_ == 0) {
    bar_mode_ = bar_last_mode_;
  } else {
    bar_last_mode_ = bar_mode_;
    bar_mode_ = 0;
  }
  bar_key_ticks_ = now;
  std::printf("health bars: mode %d\n", bar_mode_);
  std::fflush(stdout);
}

/// The sheet's dialog, the one docked in the parent, or null when none is open.
core::ui::Dialog* Application::object_properties_sheet() noexcept {
  return editor_.props_sheet;
}

/// `%s1 at (%d2, %d3)` (0x004ab290, string 0x007b5404): the object's own
/// `display_name` when the map gave it one, else its class's, and where it
/// stands.
std::string Application::object_display_line(core::ObjectId id) const {
  const core::sim::World& world = session_->world();
  const core::sim::WorldObject* object = world.find(id);
  if (object == nullptr) return std::string();
  std::string name;
  if (!object->display_name.empty()) {
    name = item_label(object->display_name, "");
  } else if (const core::ClassGraph* graph = world.class_graph(); graph != nullptr && object->class_index != core::kNoClass) {
    name = item_label(graph->property(object->class_index, "display_name"),
                      std::string(graph->classes()[static_cast<std::size_t>(object->class_index)].id) + " class name");
  }
  return name + " at (" + std::to_string(object->state.position.x) + ", " + std::to_string(object->state.position.y) + ")";
}

/// Every type-1 group the world knows, in its interning order -- the
/// `GRP_Combo`'s rows (0x004aba80 walks the group manager the same way).
std::vector<std::string> Application::group_names() const {
  std::vector<std::string> names;
  const core::sim::GroupTable& groups = session_->world().groups();
  for (std::size_t i = 0; i < groups.size(); ++i) names.emplace_back(groups.name(static_cast<std::int32_t>(i)));
  return names;
}

const core::MapObject* Application::editor_record(core::ObjectId id) const {
  for (std::size_t i = 0; i < editor_.object_ids.size(); ++i) {
    if (editor_.object_ids[i] != id) continue;
    const std::size_t authored = session_->populated().object_ids.size();
    if (i < authored) return i < world_.objects().objects().size() ? &world_.objects().objects()[i] : nullptr;
    return i - authored < editor_.added.size() ? &editor_.added[i - authored] : nullptr;
  }
  return nullptr;
}

std::vector<std::pair<core::sim::HeroSkill, std::int32_t>> Application::hero_skill_rows(core::ObjectId id) const {
  using core::sim::HeroSkill;
  std::vector<std::pair<HeroSkill, std::int32_t>> rows;
  if (const auto found = editor_.props.find(id); found != editor_.props.end() && found->second.skills) {
    rows = *found->second.skills;
  } else {
    if (const core::MapObject* record = editor_record(id)) {
      for (const auto& [name, value] : record->attributes) {
        if (name.size() <= 2 || name[0] != 'h' || name[1] != 's') continue;
        const std::int32_t which = core::sim::hero_skill_id(name);
        std::int32_t points = 0;
        if (which < 0 || !core::parse_int(value, points)) continue;
        rows.emplace_back(static_cast<HeroSkill>(which), points);
      }
    }
    const core::sim::HeroSystem* heroes = core::sim::hero_system_of(session_->world());
    for (std::size_t i = 0; rows.empty() && heroes != nullptr && i < core::sim::kHeroSkillCount; ++i) {
      const auto which = static_cast<HeroSkill>(i);
      if (heroes->offers_skill(id, which)) rows.emplace_back(which, std::max(0, heroes->skill(id, which)));
    }
  }
  rows.resize(5, {HeroSkill::count, 0});
  return rows;
}

/// Open `AdvObjProps.ini` on `ids`: the `AdvObjPropsParent` window with the
/// section 0x004ad6f0 picks for the objects' kind docked where its
/// `SettingsPosCtl` stands -- `AdvMultipleUnitsProps` for more than one,
/// else `AdvBuildingProps` (bit 23), `AdvWagonProps` (a `CVXWagon`),
/// `AdvHeroProps` (bit 24), `AdvUnitProps` (bit 22), and nothing at all
/// for anything else (a decoration, an area). `tab` is the one to start
/// on; the toolkit opens on the first (0x0066d5f0) and the opener switches
/// when asked for another (0x004adaee).
///
/// **This engine's**: the window stands beside the palette, as the area
/// dialog does, since the original's placement of a `StdDlg` was not read;
/// choosing a tool closes it with the pane, where the original keeps a
/// list of open sheets (0x008c8e68) and re-focuses one for the same set.
void Application::open_object_properties(std::vector<core::ObjectId> ids, std::int32_t tab) {
  close_object_properties();
  if (session_ == nullptr || ids.empty()) return;
  core::sim::World& world = session_->world();
  std::string section;
  if (ids.size() > 1) {
    section = "AdvMultipleUnitsProps";
  } else {
    const core::sim::WorldObject* object = world.find(ids.front());
    if (object == nullptr) return;
    if (object->state.flags.is_building) section = "AdvBuildingProps";
    else if (object->object != nullptr && object->object->is_a(core::NativeClass::wagon)) section = "AdvWagonProps";
    else if (object->state.flags.is_hero) section = "AdvHeroProps";
    else if (object->state.flags.is_unit) section = "AdvUnitProps";
    else {
      return;  // a decoration, an area: the original opens nothing (0x004ad90b)
    }
  }
  core::ui::Dialog* palette = palette_dialog();
  if (palette == nullptr) return;
  std::printf("editor: properties of %zu object(s), %s\n", ids.size(), section.c_str());
  editor_.props_ids = std::move(ids);
  core::ui::Dialog* parent = open_menu(
      "editorini/AdvObjProps.ini",
      [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        // The frame's `CloseButton` (`%ID_CLOSE%`, 0x10082), and its Collapse.
        if (event.kind == core::ui::DialogEvent::Kind::kCommand && event.id == 0x10082) close_object_properties();
        if (event.kind == core::ui::DialogEvent::Kind::kCommand && event.id == 0x5000) editor_toggle_collapse(&menu);
      },
      "AdvObjPropsParent");
  if (parent == nullptr) {
    editor_.props_ids.clear();
    return;
  }
  editor_.props_parent = parent;
  core::ui::Dialog* sheet = open_menu(
      "editorini/AdvObjProps.ini",
      [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) { object_properties_event(event, menu); },
      section);
  if (sheet == nullptr) {
    close_object_properties();
    return;
  }
  editor_.props_sheet = sheet;
  const core::ui::Rect canvas = ui_.canvas_rect();
  ui_.place_dialog(parent, palette->rect().x - canvas.x - 8 - parent->screen().design.width, palette->rect().y - canvas.y);
  redock_windows();
  if (tab != 1) sheet->set_tab(tab);
  fill_object_properties();
}

void Application::close_object_properties() {
  editor_.collapsed.erase(editor_.props_parent);
  close_menu(editor_.props_sheet);
  close_menu(editor_.props_parent);
  editor_.props_sheet = nullptr;
  editor_.props_parent = nullptr;
  editor_.props_ids.clear();
}

/// The sheet's widgets from the objects (0x004ab290 the General tab,
/// 0x004aa150 Stats, 0x004aba80 Groups, and the settlement and inventory
/// fills beside them). A field the objects disagree on shows blank, a check
/// box they disagree on its third row.
void Application::fill_object_properties() {
  core::ui::Dialog* sheet = object_properties_sheet();
  if (sheet == nullptr || session_ == nullptr) return;
  core::sim::World& world = session_->world();
  const core::ClassGraph* graph = world.class_graph();
  const std::vector<core::ObjectId>& ids = editor_.props_ids;
  const auto edits_of = [&](core::ObjectId id) -> const PropertyEdits* {
    const auto found = editor_.props.find(id);
    return found == editor_.props.end() ? nullptr : &found->second;
  };

  // -- General
  if (ids.size() == 1) {
    sheet->set_text("GEN_DispNameEdit", object_display_line(ids.front()));
  } else {
    std::vector<std::string> lines;
    for (const core::ObjectId id : ids) lines.push_back(object_display_line(id));
    sheet->set_items("GEN_DispName", std::move(lines));
  }
  {
    std::vector<std::string> players;
    for (std::size_t i = 0; i < core::sim::kPlayerCount; ++i) {
      players.push_back(item_label("Player", "/Editor/PlaceDefence.ini:PlayerHint:Text") + " " + std::to_string(i + 1));
    }
    std::int32_t owner = -1;
    bool mixed = false;
    for (const core::ObjectId id : ids) {
      const core::sim::WorldObject* object = world.find(id);
      if (object == nullptr) continue;
      const std::int32_t at = object->state.owner == core::kNoPlayer ? -1 : static_cast<std::int32_t>(object->state.owner);
      if (owner == -1 && !mixed) owner = at;
      else if (at != owner) mixed = true;
    }
    sheet->set_items("GEN_PlayerCombo", players);
    sheet->select("GEN_PlayerCombo", mixed ? -1 : owner);
    sheet->set_items("SET_PlayerCombo", std::move(players));
  }
  if (ids.size() == 1) {
    const core::ObjectId id = ids.front();
    const core::sim::WorldObject* object = world.find(id);
    const PropertyEdits* edits = edits_of(id);
    std::string script_name(world.named_objects().name_of(id));
    if (edits != nullptr && edits->script_name) script_name = *edits->script_name;
    sheet->set_text("GEN_Name", script_name);
    sheet->set_text("GEN_UserNameEdit", object != nullptr ? object->display_name : std::string());
    // `GEN_SetInnPosition` is for a class with the `Inn` property; the inn
    // is not built here, so the button stays but does nothing.
    sheet->set_enabled("GEN_SetInnPosition", false);
  }

  // -- Stats: health as a percentage of the class maximum, stamina raw,
  // the level, and the five check boxes.
  {
    std::int32_t health = -1;
    std::int32_t stamina = -1;
    std::int32_t level = -1;
    std::int32_t tmpl = -1;
    std::int32_t messenger = -1;
    std::int32_t party = -1;
    std::int32_t no_ai = -1;
    std::int32_t no_feeding = -1;
    const auto fold = [](std::int32_t& into, std::int32_t value) {
      if (into == -1) into = value;
      else if (into != value) into = -2;
    };
    const core::sim::HeroSystem* heroes = core::sim::hero_system_of(world);
    for (const core::ObjectId id : ids) {
      const core::sim::WorldObject* object = world.find(id);
      if (object == nullptr) continue;
      const std::int32_t max_health = property_int(graph, object->class_index, "maxhealth", 0);
      fold(health, max_health > 0 ? object->state.health * 100 / max_health : 100);
      fold(stamina, object->state.stamina);
      if (object->state.flags.is_unit) {
        fold(level, heroes != nullptr ? heroes->inherent_level(id) : 1);
        fold(tmpl, object->state.flags.unspawned ? 1 : 0);
        fold(messenger, object->state.flags.messenger ? 1 : 0);
        fold(party, object->state.flags.in_party ? 1 : 0);
        fold(no_ai, object->state.flags.no_ai ? 1 : 0);
        // "Unit doesn't eat" is the *inverse* of `UnitFlags` bit 17
        // (0x004aa2f4 reads the bit and negates it; 0x004ab130 clears it
        // when the box is ticked). The world does not carry the bit, so
        // it is read off the authored record and this run's edits.
        std::uint32_t unit_flags = 0;
        for (std::size_t i = 0; i < editor_.object_ids.size(); ++i) {
          if (editor_.object_ids[i] != id) continue;
          const std::size_t authored = session_->populated().object_ids.size();
          if (i < authored && i < world_.objects().objects().size()) unit_flags = world_.objects().objects()[i].unit_flags;
          else if (i >= authored && i - authored < editor_.added.size()) unit_flags = editor_.added[i - authored].unit_flags;
        }
        if (const PropertyEdits* edits = edits_of(id)) {
          unit_flags = (unit_flags & ~edits->unit_flags_clear) | edits->unit_flags_set;
        }
        fold(no_feeding, (unit_flags & 0x20000u) != 0 ? 0 : 1);
      }
    }
    const auto number = [](std::int32_t value) { return value < 0 ? std::string() : std::to_string(value); };
    sheet->set_text("GEN_Health", number(health));
    sheet->set_text("STA_Health", number(health));
    sheet->set_text("STA_Stamina", number(stamina));
    sheet->set_text("STA_Level", number(level));
    const auto box = [&](const char* name, std::int32_t value) {
      sheet->set_row(name, value == -2 ? 2 : std::max(0, value));
    };
    box("STA_Template", tmpl);
    box("STA_Messenger", messenger);
    box("STA_Party", party);
    box("STA_NoAI", no_ai);
    box("STA_NoFeeding", no_feeding);
    // A wagon's cargo: the amount and which resource.
    if (ids.size() == 1) {
      if (const core::sim::WorldObject* object = world.find(ids.front());
          object != nullptr && object->object != nullptr && object->object->is_a(core::NativeClass::wagon)) {
        sheet->set_text("STA_ResourceAmount", std::to_string(object->state.cargo));
        sheet->set_row("STA_ResourceGold", object->state.cargo_resource == 0 ? 1 : 0);
        sheet->set_row("STA_ResourceFood", object->state.cargo_resource == 0 ? 0 : 1);
      }
    }
  }

  // -- Groups: the combo lists every group, the list the objects' own.
  {
    std::vector<std::string> all = group_names();
    std::vector<std::string> member_of;
    const core::sim::GroupTable& groups = world.groups();
    for (std::size_t g = 0; g < groups.size(); ++g) {
      bool every = true;
      for (const core::ObjectId id : ids) {
        if (!groups.contains(static_cast<std::int32_t>(g), id)) every = false;
      }
      if (every) member_of.emplace_back(groups.name(static_cast<std::int32_t>(g)));
    }
    sheet->set_items("GRP_Combo", std::move(all));
    sheet->set_items("GRP_List", std::move(member_of));
  }

  // -- Settlement: a building's, from the world's economy.
  if (ids.size() == 1) {
    const core::sim::WorldObject* object = world.find(ids.front());
    const core::sim::EconomySystem* economy = core::sim::economy_of(world);
    const core::sim::Settlement* town =
        object != nullptr && economy != nullptr && object->settlement != core::kNoObject
            ? economy->settlements().for_object(object->settlement)
            : nullptr;
    if (town != nullptr) {
      sheet->set_text("SET_Name", town->name);
      sheet->select("SET_PlayerCombo", town->owner == core::kNoPlayer ? -1 : static_cast<std::int32_t>(town->owner));
      sheet->set_text("SET_Gold", std::to_string(town->gold()));
      sheet->set_text("SET_Food", std::to_string(town->food()));
      sheet->set_text("SET_Population", std::to_string(town->population));
      sheet->set_text("SET_MaxPopulation", std::to_string(town->max_population));
      std::int32_t sentries = 0;
      const PropertyEdits* edits = edits_of(ids.front());
      if (edits != nullptr && edits->settlement_sentries) sentries = *edits->settlement_sentries;
      sheet->set_text("SET_AdditionalInitialSentries", std::to_string(sentries));
    } else {
      for (const char* name : {"SET_Name", "SET_Gold", "SET_Food", "SET_Population", "SET_MaxPopulation",
                               "SET_AdditionalInitialSentries"}) {
        sheet->set_text(name, "");
      }
    }
  }

  // -- Skills: five rows of a combo over the twenty-five skills and a
  // 0..10 edit, from the hero's `hs*` attributes.
  if (ids.size() == 1) {
    std::vector<std::string> names;
    names.push_back(item_label("<None>", ""));
    for (std::size_t i = 0; i < core::sim::kHeroSkillCount; ++i) {
      names.emplace_back(core::sim::hero_skill_name(static_cast<core::sim::HeroSkill>(i)));
    }
    const auto rows = hero_skill_rows(ids.front());
    for (std::size_t row = 0; row < 5; ++row) {
      const std::string combo = "SPEC_" + std::to_string(row + 1);
      sheet->set_items(combo, names);
      const bool set = rows[row].first != core::sim::HeroSkill::count;
      sheet->select(combo, set ? static_cast<std::int32_t>(rows[row].first) + 1 : 0);
      sheet->set_text(combo + "_Level", set ? std::to_string(rows[row].second) : "0");
    }
  }

  // -- Visuals: the icon, from `editorini/combo/uniticons.ini`'s
  // `[FillCombo]` (0x007b63c4): `name = path`, the path being the
  // object's `Icon`. The preview shows the chosen bitmap.
  if (ids.size() == 1) {
    std::vector<std::string> names;
    std::vector<std::string> paths;
    if (const platform::ByteSpan bytes = vfs_.read("DATA\\INTERFACE\\EDITOR\\COMBO\\UNITICONS.INI"); !bytes.empty()) {
      if (const core::Result<core::IniDocument> doc = core::IniDocument::parse(platform::as_core_bytes(bytes)); doc.ok()) {
        const core::SectionIndex section = doc->section("FillCombo");
        for (const core::IniEntry& entry : doc->entries_of(section)) {
          if (!entry.has_key) continue;
          names.emplace_back(entry.key);
          paths.emplace_back(entry.value);
        }
      }
    }
    std::string icon;
    if (const PropertyEdits* edits = edits_of(ids.front()); edits != nullptr && edits->icon) icon = *edits->icon;
    else if (const core::MapObject* record = editor_record(ids.front())) icon = std::string(record->attribute("Icon"));
    std::int32_t chosen = -1;
    for (std::size_t i = 0; i < paths.size(); ++i) {
      if (fold_name(paths[i]) == fold_name(icon)) chosen = static_cast<std::int32_t>(i);
    }
    sheet->set_items("VIS_Combo", std::move(names));
    sheet->select("VIS_Combo", chosen);
    sheet->content().state("VIS_IconPreview").image = icon;
    sheet->content().state("VIS_Combo").items_data = std::move(paths);
  }

  // -- Inventory: four combos of the catalogue, `<None>` first (0x007b63ac).
  if (ids.size() == 1) {
    const core::sim::HeroSystem* heroes = core::sim::hero_system_of(world);
    std::vector<std::string> catalogue;
    catalogue.push_back(item_label("<None>", ""));
    if (heroes != nullptr && heroes->items().catalog() != nullptr) {
      for (const core::sim::ItemDefinition& item : heroes->items().catalog()->definitions()) catalogue.push_back(item.id);
    }
    std::vector<core::ObjectId> held;
    if (heroes != nullptr) heroes->items().contents_for(ids.front(), held);
    for (std::size_t slot = 0; slot < 4; ++slot) {
      const std::string name = "INV_" + std::to_string(slot + 1);
      sheet->set_items(name, catalogue);
      std::int32_t chosen = 0;
      if (heroes != nullptr && slot < held.size()) {
        const core::sim::ItemInstance* instance = heroes->items().find(held[slot]);
        const core::sim::ItemDefinition* definition =
            instance != nullptr ? heroes->items().catalog()->at(instance->type) : nullptr;
        for (std::size_t i = 1; definition != nullptr && i < catalogue.size(); ++i) {
          if (catalogue[i] == definition->id) chosen = static_cast<std::int32_t>(i);
        }
      }
      sheet->select(name, chosen);
    }
  }
}

/// The sheet's events: 0x004add20, by widget id. The ids are tab-based --
/// `IDT_GEN 0x01000000 + n`, `GRP 0x02..`, `SET 0x03..`, `STA 0x04..`,
/// `VIS 0x05..`, `INV 0x06..`, `MIV 0x07..`, `SPC 0x08..` -- and every
/// edit applies to every object of the sheet as it is typed, as the
/// original's setters walk the sheet's deque (0x004aa710 and its
/// neighbours). What each one does to the object is written beside it.
void Application::object_properties_event(const core::ui::DialogEvent& event, core::ui::Dialog& sheet) {
  using Kind = core::ui::DialogEvent::Kind;
  if (session_ == nullptr || editor_.props_ids.empty()) return;
  core::sim::World& world = session_->world();
  const core::ClassGraph* graph = world.class_graph();
  const std::vector<core::ObjectId> ids = editor_.props_ids;
  const core::ui::Widget* widget = event.widget.empty() ? nullptr : sheet.screen().find(event.widget);
  const std::uint32_t id = widget != nullptr && widget->has_id ? static_cast<std::uint32_t>(widget->id) : 0u;
  const auto number = [&](std::string_view name) {
    std::int32_t value = 0;
    return core::parse_int(sheet.text(name), value) ? value : std::numeric_limits<std::int32_t>::min();
  };
  const auto checked = [&](std::string_view name) {
    const core::ui::WidgetState* state = sheet.content().state_of(name);
    return state != nullptr && state->row == 1;
  };
  const auto each = [&](const auto& apply) {
    for (const core::ObjectId object : ids) apply(object, editor_.props[object]);
  };

  if (event.kind == Kind::kChange) {
    switch (id) {
      case 0x1000004: {  // GEN_PlayerCombo -> vslot 0xa0, the owner (0x004aa6a0)
        const std::int32_t row = sheet.selected("GEN_PlayerCombo");
        if (row < 0) return;
        const auto owner = static_cast<core::PlayerId>(std::clamp(row, 0, static_cast<int>(core::sim::kPlayerCount) - 1));
        each([&](core::ObjectId object, PropertyEdits& edits) {
          (void)world.set_owner(object, owner);
          edits.player = static_cast<std::int32_t>(owner) + 1;
        });
        return;
      }
      case 0x1000007: {  // GEN_Name -> Obj::SetName (0x004ac7e0), single object
        std::string name = sheet.text("GEN_Name");
        each([&](core::ObjectId object, PropertyEdits& edits) {
          if (!name.empty()) (void)world.named_objects().rebind(name, object);
          edits.script_name = name;
        });
        return;
      }
      case 0x100000a:    // GEN_Health and
      case 0x4000003: {  // STA_Health -> health = maxhealth * pct / 100 (0x004aa710)
        const std::int32_t pct = number(id == 0x100000a ? "GEN_Health" : "STA_Health");
        if (pct == std::numeric_limits<std::int32_t>::min()) return;
        each([&](core::ObjectId object, PropertyEdits& edits) {
          const core::sim::WorldObject* slot = world.find(object);
          if (slot == nullptr || graph == nullptr) return;
          const std::int32_t max_health = property_int(graph, slot->class_index, "maxhealth", 0);
          (void)world.set_health(object, static_cast<std::int32_t>(static_cast<std::int64_t>(max_health) * pct / 100));
          edits.health_percent = pct;
        });
        sheet.set_text(id == 0x100000a ? "STA_Health" : "GEN_Health", std::to_string(pct));
        return;
      }
      case 0x1000033: {  // GEN_UserNameEdit -> the display name (0x004acb10)
        const std::string name = sheet.text("GEN_UserNameEdit");
        each([&](core::ObjectId object, PropertyEdits& edits) {
          if (core::sim::WorldObject* slot = world.find(object)) slot->display_name = name;
          edits.display_name = name;
        });
        return;
      }
      case 0x400002b: {  // STA_Stamina, clamped to 0..maxstamina (0x004aa7a0)
        std::int32_t value = number("STA_Stamina");
        if (value == std::numeric_limits<std::int32_t>::min()) return;
        each([&](core::ObjectId object, PropertyEdits& edits) {
          const core::sim::WorldObject* slot = world.find(object);
          if (slot == nullptr) return;
          const std::int32_t max_stamina = property_int(graph, slot->class_index, "maxstamina", value);
          value = std::clamp(value, 0, std::max(0, max_stamina));
          (void)world.set_stamina(object, value);
          edits.stamina = value;
        });
        return;
      }
      case 0x400000b: {  // STA_Level, 1..1000, units only (0x004aa840)
        std::int32_t value = number("STA_Level");
        if (value == std::numeric_limits<std::int32_t>::min()) return;
        value = std::clamp(value, 1, 1000);
        core::sim::HeroSystem* heroes = core::sim::hero_system_of(world);
        each([&](core::ObjectId object, PropertyEdits& edits) {
          const core::sim::WorldObject* slot = world.find(object);
          if (slot == nullptr || !slot->state.flags.is_unit) return;
          // The original writes the experience threshold of the level
          // (`SetExperience(exp_for_level)`); this engine's unit keeps a
          // level, and the map's `Level` is the level less one.
          if (heroes != nullptr) (void)heroes->set_level(object, value);
          edits.level = value - 1;
        });
        return;
      }
      case 0x400001b: {  // STA_ResourceAmount, 0..10000, a wagon's cargo (0x004aa900)
        std::int32_t value = number("STA_ResourceAmount");
        if (value == std::numeric_limits<std::int32_t>::min()) return;
        value = std::clamp(value, 0, 10000);
        each([&](core::ObjectId object, PropertyEdits&) {
          if (core::sim::WorldObject* slot = world.find(object);
              slot != nullptr && slot->object != nullptr && slot->object->is_a(core::NativeClass::wagon)) {
            slot->state.cargo = value;
          }
        });
        return;
      }
      case 0x3000003:    // SET_Name
      case 0x3000005:    // SET_PlayerCombo
      case 0x3000008:    // SET_Gold
      case 0x300000b:    // SET_Food
      case 0x3000011:    // SET_Population
      case 0x3000014:    // SET_MaxPopulation
      case 0x3000017: {  // SET_AdditionalInitialSentries
        // The building's settlement (0x004aab70 and its neighbours): the
        // world's record where it has the field, and the map's element.
        core::sim::EconomySystem* economy = core::sim::economy_of(world);
        each([&](core::ObjectId object, PropertyEdits& edits) {
          const core::sim::WorldObject* slot = world.find(object);
          core::sim::Settlement* town = slot != nullptr && economy != nullptr && slot->settlement != core::kNoObject
                                            ? economy->settlements().for_object(slot->settlement)
                                            : nullptr;
          if (town == nullptr) return;
          if (id == 0x3000003) {
            edits.settlement_name = sheet.text("SET_Name");
            town->name = *edits.settlement_name;
          } else if (id == 0x3000005) {
            const std::int32_t row = sheet.selected("SET_PlayerCombo");
            if (row < 0) return;
            edits.settlement_player = row + 1;
            (void)economy->set_owner(town->id, static_cast<core::PlayerId>(row));
          } else {
            const char* name = id == 0x3000008 ? "SET_Gold" : id == 0x300000b ? "SET_Food"
                             : id == 0x3000011 ? "SET_Population" : id == 0x3000014 ? "SET_MaxPopulation"
                                                                                     : "SET_AdditionalInitialSentries";
            const std::int32_t value = number(name);
            if (value == std::numeric_limits<std::int32_t>::min()) return;
            switch (id) {
              case 0x3000008: edits.settlement_gold = value; (void)economy->set_resource(town->id, core::sim::Resource::gold, value); break;
              case 0x300000b: edits.settlement_food = value; (void)economy->set_resource(town->id, core::sim::Resource::food, value); break;
              case 0x3000011: edits.settlement_population = value; (void)economy->set_population(town->id, value); break;
              case 0x3000014: edits.settlement_max_population = value; town->max_population = value; break;
              default: edits.settlement_sentries = value; break;
            }
          }
        });
        return;
      }
      case 0x5000003: {  // VIS_Combo -> Unit::SetIcon (0x004aaf70): the `Icon` attribute, and the preview
        const std::int32_t row = sheet.selected("VIS_Combo");
        const core::ui::WidgetState* state = sheet.content().state_of("VIS_Combo");
        if (row < 0 || state == nullptr || static_cast<std::size_t>(row) >= state->items_data.size()) return;
        const std::string path = state->items_data[static_cast<std::size_t>(row)];
        each([&](core::ObjectId, PropertyEdits& edits) { edits.icon = path; });
        sheet.content().state("VIS_IconPreview").image = path;
        return;
      }
      case 0x8000001:
      case 0x8000002:
      case 0x8000003:
      case 0x8000004:
      case 0x8000005:
      case 0x8000015:
      case 0x8000016:
      case 0x8000017:
      case 0x8000018:
      case 0x8000019: {  // SPEC_n / SPEC_n_Level: the five skill rows (0x004ad090 at close; a level is clamped 0..10 as typed, 0x004add83)
        using core::sim::HeroSkill;
        std::vector<std::pair<HeroSkill, std::int32_t>> rows;
        for (std::size_t row = 0; row < 5; ++row) {
          const std::string combo = "SPEC_" + std::to_string(row + 1);
          const std::int32_t choice = sheet.selected(combo);
          std::int32_t points = number(combo + "_Level");
          if (points == std::numeric_limits<std::int32_t>::min()) points = 0;
          if (points < 0 || points > 10) {
            points = std::clamp(points, 0, 10);
            sheet.set_text(combo + "_Level", std::to_string(points));
          }
          rows.emplace_back(choice > 0 && static_cast<std::size_t>(choice) <= core::sim::kHeroSkillCount
                                ? static_cast<HeroSkill>(choice - 1)
                                : HeroSkill::count,
                            points);
        }
        core::sim::HeroSystem* heroes = core::sim::hero_system_of(world);
        each([&](core::ObjectId object, PropertyEdits& edits) {
          const core::sim::WorldObject* slot = world.find(object);
          if (slot == nullptr || !slot->state.flags.is_hero) return;
          if (heroes != nullptr) {
            for (std::size_t i = 0; i < core::sim::kHeroSkillCount; ++i) {
              const auto which = static_cast<HeroSkill>(i);
              std::int32_t points = 0;
              for (const auto& [skill, value] : rows) {
                if (skill == which) points = value;
              }
              (void)heroes->load_skill(object, which, points);
            }
          }
          edits.skills = rows;
        });
        return;
      }
      case 0x6000001:
      case 0x6000002:
      case 0x6000003:
      case 0x6000004: {  // INV_1..4: the holder's items become the four combos' choices
        core::sim::HeroSystem* heroes = core::sim::hero_system_of(world);
        if (heroes == nullptr) return;
        std::vector<std::string> wanted;
        for (std::size_t slot = 0; slot < 4; ++slot) {
          const std::string name = "INV_" + std::to_string(slot + 1);
          const std::int32_t row = sheet.selected(name);
          const core::ui::WidgetState* state = sheet.content().state_of(name);
          if (row > 0 && state != nullptr && static_cast<std::size_t>(row) < state->items.size()) {
            wanted.push_back(state->items[static_cast<std::size_t>(row)]);
          }
        }
        each([&](core::ObjectId object, PropertyEdits& edits) {
          std::vector<core::ObjectId> held;
          heroes->items().contents_for(object, held);
          for (const core::ObjectId item : held) (void)heroes->items().remove(world, item);
          for (const std::string& item : wanted) (void)heroes->items().add(world, object, item);
          edits.items = wanted;
        });
        return;
      }
      default:
        return;
    }
  }

  if (event.kind == Kind::kCommand) {
    // The five check boxes: `vtbl+0x44`/`+0x48` on the sync word for the
    // first three, the second word's bits for the last two.
    const auto sync_bit = [&](const char* name, std::uint32_t bit) {
      const bool on = checked(name);
      each([&](core::ObjectId object, PropertyEdits& edits) {
        core::sim::WorldObject* slot = world.find(object);
        if (slot == nullptr) return;
        if (bit == core::sim::kSyncUnspawned) slot->state.flags.unspawned = on;
        else if (bit == 0x4000000u) slot->state.flags.messenger = on;
        else slot->state.flags.in_party = on;
        (on ? edits.flags_set : edits.flags_clear) |= bit;
        (on ? edits.flags_clear : edits.flags_set) &= ~bit;
      });
    };
    const auto unit_bit = [&](const char* name, std::uint32_t bit, bool inverted) {
      const bool on = checked(name) != inverted;
      each([&](core::ObjectId object, PropertyEdits& edits) {
        core::sim::WorldObject* slot = world.find(object);
        if (slot == nullptr || !slot->state.flags.is_unit) return;
        if (bit == core::sim::kUnitFlagNoAI) slot->state.flags.no_ai = on;
        (on ? edits.unit_flags_set : edits.unit_flags_clear) |= bit;
        (on ? edits.unit_flags_clear : edits.unit_flags_set) &= ~bit;
      });
    };
    switch (id) {
      case 0x4000006: sync_bit("STA_Template", core::sim::kSyncUnspawned); return;   // bit 27 (0x004aadf0)
      case 0x4000008: sync_bit("STA_Messenger", 0x4000000u); return;                 // bit 26 (0x004aae70)
      case 0x400000a: sync_bit("STA_Party", core::sim::kSyncParty); return;         // bit 19 (0x004aaef0)
      case 0x400001f: unit_bit("STA_NoAI", core::sim::kUnitFlagNoAI, false); return;  // 0x40000 (0x004ab090)
      case 0x4000021: unit_bit("STA_NoFeeding", 0x20000u, true); return;             // !0x20000 (0x004ab130)
      case 0x4020021:    // STA_ResourceGold
      case 0x4020022: {  // STA_ResourceFood -> Wagon::restype (0x004aa980)
        each([&](core::ObjectId object, PropertyEdits&) {
          if (core::sim::WorldObject* slot = world.find(object);
              slot != nullptr && slot->object != nullptr && slot->object->is_a(core::NativeClass::wagon)) {
            slot->state.cargo_resource = id == 0x4020021 ? 0 : 1;
          }
        });
        return;
      }
      case 0x2000002:    // GRP_Add: the combo's group, made if it is new (0x004cdb90 / 0x0049c390)
      case 0x2000005: {  // GRP_Remove
        std::string name;
        if (const std::int32_t row = sheet.selected("GRP_Combo"); row >= 0) {
          const core::ui::WidgetState* state = sheet.content().state_of("GRP_Combo");
          if (state != nullptr && static_cast<std::size_t>(row) < state->items.size()) name = state->items[static_cast<std::size_t>(row)];
        } else {
          name = sheet.text("GRP_Combo");
        }
        if (name.empty()) return;
        const bool add = id == 0x2000002;
        const std::int32_t group = add ? world.group_index(name) : world.groups().find(name);
        if (group == core::sim::GroupTable::kNoGroup) return;
        each([&](core::ObjectId object, PropertyEdits& edits) {
          if (add) {
            (void)world.groups().add(group, object);
            std::erase(edits.groups_removed, name);
            if (std::find(edits.groups_added.begin(), edits.groups_added.end(), name) == edits.groups_added.end()) edits.groups_added.push_back(name);
          } else {
            (void)world.groups().remove(group, object);
            std::erase(edits.groups_added, name);
            if (std::find(edits.groups_removed.begin(), edits.groups_removed.end(), name) == edits.groups_removed.end()) edits.groups_removed.push_back(name);
          }
        });
        fill_object_properties();
        return;
      }
      case 0x100000d:    // GEN_ViewSingle / GEN_ViewMultiple: the view to the objects
      case 0x1000037: {  // GEN_SelectAll: and every one selected (0x004ae022)
        core::sim::Selection& selection = session_->selections().player(local_player_);
        selection.clear();
        std::int64_t sx = 0;
        std::int64_t sy = 0;
        std::int64_t n = 0;
        for (const core::ObjectId object : ids) {
          const core::sim::WorldObject* slot = world.find(object);
          if (slot == nullptr) continue;
          selection.add(object);
          sx += slot->state.position.x;
          sy += slot->state.position.y;
          ++n;
        }
        if (n > 0) {
          editor_.selected = ids.front();
          look_x_ = static_cast<std::int32_t>(sx / n);
          look_y_ = static_cast<std::int32_t>(sy / n);
          look_pending_ = true;
        }
        return;
      }
      case 0x100000c: {  // GEN_Properties: the list's row gets its own sheet (0x004ae561)
        const std::int32_t row = sheet.selected("GEN_DispName");
        if (row >= 0 && static_cast<std::size_t>(row) < ids.size()) open_object_properties({ids[static_cast<std::size_t>(row)]});
        return;
      }
      default:
        return;
    }
  }

  if (event.kind == Kind::kSelect && id == 0x2000004) {
    // A row of `Member of` picks the same group in the combo (0x004ae65a),
    // which is what `Remove` then reads.
    const core::ui::WidgetState* list = sheet.content().state_of("GRP_List");
    if (list == nullptr || event.index < 0 || static_cast<std::size_t>(event.index) >= list->items.size()) return;
    const std::string& name = list->items[static_cast<std::size_t>(event.index)];
    const core::ui::WidgetState* combo = sheet.content().state_of("GRP_Combo");
    if (combo == nullptr) return;
    for (std::size_t i = 0; i < combo->items.size(); ++i) {
      if (combo->items[i] == name) sheet.select("GRP_Combo", static_cast<std::int32_t>(i));
    }
    return;
  }
  if (event.kind == Kind::kActivate && id == 0x1000002 && ids.size() > 1) {
    // A row of the objects' list, clicked again: its own sheet (0x004ade63).
    if (event.index >= 0 && static_cast<std::size_t>(event.index) < ids.size()) open_object_properties({ids[static_cast<std::size_t>(event.index)]});
  }
}

// -- the explorer ---------------------------------------------------------------

/// The Adventure Palette (`AdvExplorerDlg.ini`, "Adventure Palette"; the
/// exe builds one per container flavour, 0x0047b000 / 0x0047b420 /
/// 0x0047b81b, on the same file with a different root). Its `Browser` is a
/// tree the managers fill as they load, each node registered with the
/// dialog it opens (0x00463250 takes the node path, the `.ini` and the
/// label): the root (`AdvAdventure.ini`, `AdvScenario.ini`), `players`
/// with a leaf per slot (`AdvPlayer.ini`), `playersdiplomacy`
/// (`AdvDiplomacy.ini`), `Conversations/<name>` (`AdvConverse.ini`),
/// `Notes/<name>` (`AdvNotes.ini`), `Sequences/<name>` (`AdvSequence.ini`),
/// `items` (`AdvItems.ini`), `resources` labelled *Settlements*
/// (`AdvResources.ini`), and `CurMap` labelled *Map* (`AdvCurMap.ini`) with
/// `Areas/<name>`, `NamedUnits/<name>`, `groups/<name>` (`AdvGrpProps.ini`),
/// `IHolder` and `Outposts` under it. A node chosen docks its dialog where
/// `SettingsPosCtl` stands (265, 27).
///
/// **What is wired:** the tree, and every leaf -- a group's properties
/// (rename, delete, the members, select, remove, add the selection, a
/// member's sheet), an area (chosen in the Edit-areas tool, whose dialog it
/// already has), a named unit (selected and looked at), and the document
/// dialogs (`fill_explorer_pane`, `explorer_pane_event`), which read the
/// container's own text and patch it in place (`core/xml_patch.hpp`).
/// **This engine's:** the window stands at the top left and is dragged by
/// its caption; the original's placement was not read. Its Collapse rolls
/// it up (`editor_toggle_collapse`).
void Application::open_explorer() {
  if (session_ == nullptr) return;
  build_explorer_tree();
  core::ui::Dialog* explorer = open_menu(
      "editorini/AdvExplorerDlg.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        if ((event.kind == Kind::kSelect || event.kind == Kind::kActivate) && event.widget == "Browser") {
          if (event.index < 0 || static_cast<std::size_t>(event.index) >= editor_.explorer_rows.size()) return;
          explorer_choose(editor_.explorer_rows[static_cast<std::size_t>(event.index)]);
          return;
        }
        // The frame's Close (`%ID_CLOSE%`): the window goes, and `togglewnd`
        // does not bring it back -- as the original's Close would not.
        if (event.kind == Kind::kCommand && event.id == 0x10082) close_explorer();
        if (event.kind == Kind::kCommand && event.id == 0x5000) editor_toggle_collapse(&menu);
      });
  if (explorer == nullptr) return;
  editor_.explorer = explorer;
  // Narrowed to what stands left of the palette (the window is sized by
  // its grips in the original, between `MinSize` 360 and the display);
  // the docked dialogs are 550 wide and the frame stretches to hold them.
  core::ui::Dialog* palette = palette_dialog();
  const std::int32_t width = std::max(360, 1024 - 8 - (palette != nullptr ? palette->screen().design.width : 265) - 8 - 8);
  explorer->resize(width, explorer->screen().design.height);
  ui_.place_dialog(explorer, 8, 8);
  // The caption names the container the way the exe does: `<flavour> - <name>`.
  std::string flavour = item_label("Scenario", "");
  if (game_.game_type == 1) flavour = item_label("Adventure", "");
  if (game_.game_type == 2) flavour = item_label("Conquest", "");
  explorer->set_text("Caption", flavour + " - " + container_.path().stem().string());
  refresh_explorer();
}

void Application::close_explorer() {
  close_explorer_pane();
  editor_.collapsed.erase(editor_.explorer);
  close_menu(editor_.explorer);
  editor_.explorer = nullptr;
  editor_.explorer_chosen = -1;
}

core::ui::Dialog* Application::explorer_dialog() noexcept {
  return editor_.explorer;
}

/// The tools palette, wherever it stands in the stack.
core::ui::Dialog* Application::palette_dialog() noexcept {
  return editor_.palette != nullptr && ui_.index_of(editor_.palette) < ui_.dialog_count()
             ? editor_.palette
             : nullptr;
}

void Application::build_explorer_tree() {
  editor_.explorer_nodes.clear();
  editor_.explorer_chosen = -1;
  const auto add = [&](std::int32_t parent, std::string label, ExplorerKind kind, std::string pane,
                       std::string key = {}) {
    ExplorerNode node;
    node.label = std::move(label);
    node.kind = kind;
    node.pane = std::move(pane);
    node.key = std::move(key);
    node.parent = parent;
    editor_.explorer_nodes.push_back(std::move(node));
    const auto index = static_cast<std::int32_t>(editor_.explorer_nodes.size() - 1);
    if (parent >= 0) editor_.explorer_nodes[static_cast<std::size_t>(parent)].children.push_back(index);
    return index;
  };
  const core::sim::World& world = session_->world();

  // The root: the container's own properties.
  const bool adventure = game_.game_type != 0;
  add(-1, item_label(adventure ? "Adventure" : "Scenario", ""), ExplorerKind::kPane,
      adventure ? "editorini/AdvAdventure.ini" : "editorini/AdvScenario.ini");
  // Players, a leaf per slot the container declares.
  const std::int32_t players = add(-1, item_label("Players", ""), ExplorerKind::kBranch, "");
  for (std::size_t i = 0; i < core::sim::kPlayerCount; ++i) {
    const core::sim::PlayerSetup& setup = world.players().setup(static_cast<core::PlayerId>(i));
    if (setup.control == core::sim::PlayerControl::disabled && setup.name.empty()) continue;
    add(players, std::to_string(i + 1) + ". " + (setup.name.empty() ? setup.race : setup.name), ExplorerKind::kPane,
        "editorini/AdvPlayer.ini", std::to_string(i));
  }
  add(-1, item_label("Players diplomacy", ""), ExplorerKind::kPane, "editorini/AdvDiplomacy.ini");
  const std::int32_t conversations = add(-1, item_label("Conversations", ""), ExplorerKind::kBranch, "");
  {
    // From the documents rather than the catalogue, so that a conversation
    // renamed or emptied this run is listed as it stands.
    const std::string map_prefix = fold_name(editor_map_prefix() + "conversations/");
    for (const std::string& stored : container_.list()) {
      std::string folded = fold_name(stored);
      for (char& c : folded) if (c == '\\') c = '/';
      if (!folded.ends_with(".conv.xml") || folded.rfind("local/", 0) == 0) continue;
      if (folded.rfind(map_prefix, 0) != 0 && folded.rfind("conversations/", 0) != 0) continue;
      const std::string name = core::xml_get_attribute(editor_document_text(stored), "conversation", "name");
      if (!name.empty()) add(conversations, name, ExplorerKind::kPane, "editorini/AdvConverse.ini", name);
    }
    // The documents this run made are not in the container's list yet.
    for (const auto& [path, text] : editor_.documents) {
      if (editor_.created.count(path) == 0 || !fold_name(path).ends_with(".conv.xml")) continue;
      const std::string name = core::xml_get_attribute(text, "conversation", "name");
      if (!name.empty()) add(conversations, name, ExplorerKind::kPane, "editorini/AdvConverse.ini", name);
    }
    add(conversations, item_label("New conversation", ""), ExplorerKind::kNew, "editorini/AdvConverse.ini", "conversation");
  }
  // Each manager's *New ...* leaf stands last under its branch: the exe
  // registers one browseable per manager with the manager's dialog
  // (0x00472385 makes the notes', 0x004723db its label) and choosing it
  // makes the thing. **Reading, labelled:** that the leaf stands in the tree
  // rather than on a menu -- the registration is the same routine every
  // other node goes through, and the tree is the only place they show.
  const std::int32_t notes = add(-1, item_label("Notes", ""), ExplorerKind::kBranch, "");
  {
    // From the documents, as the conversations: the map's `Notes.xml` and
    // the root's, in that order, so a note made this run is listed.
    std::set<std::string> listed;
    for (const std::string document : {editor_map_prefix() + "Notes.xml", std::string("Notes.xml")}) {
      const std::string& text = editor_document_text(document);
      const std::size_t count = core::xml_count_elements(text, "notes/note");
      for (std::size_t i = 0; i < count; ++i) {
        const std::string id = core::xml_get_attribute(text, "notes/note[" + std::to_string(i) + "]", "id");
        if (id.empty() || !listed.insert(id).second) continue;
        add(notes, id, ExplorerKind::kPane, "editorini/AdvNotes.ini", id);
      }
    }
    add(notes, item_label("New note", ""), ExplorerKind::kNew, "editorini/AdvNotes.ini", "note");
  }
  const std::int32_t sequences = add(-1, item_label("Sequences", ""), ExplorerKind::kBranch, "");
  {
    std::set<std::string> listed;
    for (const std::string document : {std::string("Sequences/sequences.xml"), editor_map_prefix() + "Sequences/sequences.xml"}) {
      const std::string& text = editor_document_text(document);
      const std::size_t count = core::xml_count_elements(text, "sequences/sequence");
      for (std::size_t i = 0; i < count; ++i) {
        const std::string name = core::xml_get_attribute(text, "sequences/sequence[" + std::to_string(i) + "]", "name");
        if (name.empty() || !listed.insert(name).second) continue;
        add(sequences, name, ExplorerKind::kPane, "editorini/AdvSequence.ini", name);
      }
    }
    add(sequences, item_label("New sequence", ""), ExplorerKind::kNew, "editorini/AdvSequence.ini", "sequence");
  }
  const std::int32_t items = add(-1, item_label("Item types", ""), ExplorerKind::kBranch, "");
  {
    // `itemsCustom.xml`'s own items, a leaf each (the exe's `%citemtype%u`
    // nodes, 0x00467f94).
    const std::string& text = editor_document_text("itemsCustom.xml");
    std::size_t from = 0;
    while ((from = text.find("<item", from)) != std::string::npos) {
      const std::size_t close = text.find('>', from);
      if (close == std::string::npos) break;
      const std::string_view tag(text.data() + from, close - from);
      if (tag.size() > 5 && tag[5] != ' ' && tag[5] != '\r' && tag[5] != '\n' && tag[5] != '\t') { from = close; continue; }
      const std::size_t key = tag.find("id=\"");
      if (key != std::string_view::npos) {
        const std::size_t end = tag.find('"', key + 4);
        if (end != std::string_view::npos) {
          const std::string id(tag.substr(key + 4, end - key - 4));
          add(items, id, ExplorerKind::kPane, "editorini/AdvItems.ini", id);
        }
      }
      from = close;
    }
    add(items, item_label("New item type", ""), ExplorerKind::kNew, "editorini/AdvItems.ini", "item");
  }
  add(-1, item_label("Settlements", ""), ExplorerKind::kPane, "editorini/AdvResources.ini");
  // The current map and what it holds.
  const std::int32_t map = add(-1, item_label("Map", ""), ExplorerKind::kPane, "editorini/AdvCurMap.ini");
  const std::int32_t areas = add(map, item_label("Areas", ""), ExplorerKind::kBranch, "");
  for (std::size_t i = 0; i < editor_.areas.size(); ++i) {
    add(areas, editor_.areas[i].name, ExplorerKind::kArea, "", std::to_string(i));
  }
  const std::int32_t named = add(map, item_label("Named units", ""), ExplorerKind::kBranch, "");
  const core::sim::NamedObjectTable& names = world.named_objects();
  for (std::size_t i = 0; i < names.size(); ++i) {
    const core::ObjectId id = names.object(static_cast<std::int32_t>(i));
    const core::sim::WorldObject* object = world.find(id);
    if (object == nullptr || !object->state.flags.is_unit) continue;
    add(named, std::string(names.name(static_cast<std::int32_t>(i))), ExplorerKind::kNamedUnit, "", std::to_string(id));
  }
  const std::int32_t groups = add(map, item_label("Groups", ""), ExplorerKind::kBranch, "");
  for (const std::string& name : group_names()) {
    if (editor_.groups_deleted.count(name) != 0) continue;
    add(groups, name, ExplorerKind::kGroup, "editorini/AdvGrpProps.ini", name);
  }
  const std::int32_t holders = add(map, item_label("Item holders", ""), ExplorerKind::kBranch, "");
  for (const core::sim::WorldObject& object : world.objects()) {
    if (object.object == nullptr || !object.object->is_a(core::NativeClass::item_holder)) continue;
    std::string label(names.name_of(object.id));
    if (label.empty()) label = object_display_line(object.id);
    add(holders, label, ExplorerKind::kPane, "editorini/AdvItemHolder.ini", std::to_string(object.id));
  }
  const std::int32_t outposts = add(map, item_label("Outposts", ""), ExplorerKind::kBranch, "");
  if (const core::sim::EconomySystem* economy = core::sim::economy_of(session_->world()); economy != nullptr) {
    for (const core::sim::Settlement& town : economy->settlements().all()) {
      if (town.kind != core::sim::SettlementKind::outpost) continue;
      add(outposts, town.name, ExplorerKind::kPane, "editorini/AdvOutpost.ini", std::to_string(town.id));
    }
  }
  editor_.explorer_nodes[static_cast<std::size_t>(map)].expanded = true;
}

void Application::refresh_explorer() {
  core::ui::Dialog* explorer = explorer_dialog();
  if (explorer == nullptr) return;
  editor_.explorer_rows.clear();
  std::vector<std::string> items;
  std::int32_t selected = -1;
  const std::function<void(std::int32_t, int)> walk = [&](std::int32_t index, int depth) {
    const ExplorerNode& node = editor_.explorer_nodes[static_cast<std::size_t>(index)];
    std::string text(static_cast<std::size_t>(depth) * 2, ' ');
    text += node.children.empty() ? "  " : (node.expanded ? "- " : "+ ");
    text += node.label;
    if (index == editor_.explorer_chosen) selected = static_cast<std::int32_t>(items.size());
    items.push_back(std::move(text));
    editor_.explorer_rows.push_back(index);
    if (node.expanded) {
      for (const std::int32_t child : node.children) walk(child, depth + 1);
    }
  };
  for (std::size_t i = 0; i < editor_.explorer_nodes.size(); ++i) {
    if (editor_.explorer_nodes[i].parent < 0) walk(static_cast<std::int32_t>(i), 0);
  }
  const std::int32_t scroll = explorer->content().state("Browser").scroll;
  explorer->set_items("Browser", std::move(items));
  explorer->select("Browser", selected);
  explorer->content().state("Browser").scroll =
      std::min(scroll, std::max(0, static_cast<std::int32_t>(editor_.explorer_rows.size()) - 1));
}

/// The dialog the explorer docked, or null.
core::ui::Dialog* Application::explorer_pane() noexcept {
  return editor_.explorer_pane;
}

void Application::close_explorer_pane() {
  close_menu(editor_.explorer_pane);
  editor_.explorer_pane = nullptr;
}

void Application::explorer_choose(std::int32_t index) {
  if (index < 0 || static_cast<std::size_t>(index) >= editor_.explorer_nodes.size()) return;
  ExplorerNode& node = editor_.explorer_nodes[static_cast<std::size_t>(index)];
  if (editor_.explorer_chosen == index && !node.children.empty()) {
    node.expanded = !node.expanded;
  } else if (!node.children.empty()) {
    node.expanded = true;
  }
  editor_.explorer_chosen = index;
  refresh_explorer();
  close_explorer_pane();
  switch (node.kind) {
    case ExplorerKind::kBranch:
      return;
    case ExplorerKind::kArea: {
      // The area in the Edit-areas tool, whose dialog is the area's.
      const auto which = static_cast<std::size_t>(std::atoi(node.key.c_str()));
      for (std::size_t i = 0; i < editor_.nodes.size(); ++i) {
        if (editor_.nodes[i].is_tool && editor_.nodes[i].tool == EditorTool::kEditArea) {
          editor_choose(static_cast<std::int32_t>(i));
          break;
        }
      }
      if (which < editor_.areas.size()) {
        editor_.area_selected = static_cast<std::int32_t>(which);
        editor_.selected = editor_.areas[which].id;
        open_area_dialog();
        const core::MapArea& shape = editor_.areas[which].shape;
        const core::sim::AreaShape centre = shape.is_circle()
            ? core::sim::AreaShape::of_circle({shape.ptx, shape.pty}, shape.radius)
            : core::sim::AreaShape::of_rectangle(shape.left, shape.top, shape.right, shape.bottom);
        look_x_ = centre.centre().x;
        look_y_ = centre.centre().y;
        look_pending_ = true;
      }
      return;
    }
    case ExplorerKind::kNamedUnit: {
      const auto id = static_cast<core::ObjectId>(std::atoi(node.key.c_str()));
      if (const core::sim::WorldObject* object = session_->world().find(id)) {
        editor_.selected = id;
        editor_.drag_from = object->state.position;
        core::sim::Selection& selection = session_->selections().player(local_player_);
        selection.clear();
        selection.add(id);
        look_x_ = object->state.position.x;
        look_y_ = object->state.position.y;
        look_pending_ = true;
      }
      return;
    }
    case ExplorerKind::kGroup:
      open_group_properties(node.key);
      return;
    case ExplorerKind::kNew:
      explorer_create(node.key);
      return;
    case ExplorerKind::kPane: {
      core::ui::Dialog* explorer = explorer_dialog();
      if (explorer == nullptr || node.pane.empty()) return;
      ExplorerNode chosen = node;
      // The handler's copy is its own: a rename writes the new key into it,
      // so the next edit patches the renamed element.
      core::ui::Dialog* pane = open_menu(node.pane, [this, chosen](const core::ui::DialogEvent& event, core::ui::Dialog& menu) mutable {
        explorer_pane_event(chosen, event, menu);
      });
      if (pane == nullptr) return;
      editor_.explorer_pane = pane;
      dock_in_explorer(pane);
      fill_explorer_pane(chosen, *pane);
      return;
    }
  }
}

/// `AdvGrpProps.ini` on a type-1 group, docked in the explorer: the name
/// (id 1, renamed on change), Delete (2), the members (3, `class at (x,
/// y)`), Select (4), Properties (5), Remove (6), Add selection (7),
/// Select All (8). The handler (0x0049b52e's registration; the dialog's
/// own is beside it) was not read case by case: what each button does is
/// what its label says over the live group table, and the document
/// follows at the save.
void Application::open_group_properties(const std::string& name) {
  core::ui::Dialog* explorer = explorer_dialog();
  if (explorer == nullptr) return;
  editor_.explorer_group = name;
  core::ui::Dialog* pane = open_menu(
      "editorini/AdvGrpProps.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        core::sim::World& world = session_->world();
        const std::int32_t group = world.groups().find(editor_.explorer_group);
        if (group == core::sim::GroupTable::kNoGroup) return;
        const auto member_at = [&](std::int32_t row) -> core::ObjectId {
          const std::span<const core::ObjectId> members = world.groups().members(group);
          return row >= 0 && static_cast<std::size_t>(row) < members.size() ? members[static_cast<std::size_t>(row)] : core::kNoObject;
        };
        if (event.kind == Kind::kChange && event.widget == "Name") {
          const std::string renamed = menu.text("Name");
          if (renamed.empty() || renamed == editor_.explorer_group) return;
          // The world's table has no rename: the new name is interned and
          // the members move; the document renames the element.
          const std::int32_t target = world.group_index(renamed);
          const std::vector<core::ObjectId> members(world.groups().members(group).begin(), world.groups().members(group).end());
          for (const core::ObjectId id : members) {
            (void)world.groups().remove(group, id);
            (void)world.groups().add(target, id);
          }
          editor_.groups_deleted.insert(editor_.explorer_group);
          editor_.group_renames[editor_.explorer_group] = renamed;
          editor_.explorer_group = renamed;
          build_explorer_tree();
          for (std::size_t i = 0; i < editor_.explorer_nodes.size(); ++i) {
            if (editor_.explorer_nodes[i].kind == ExplorerKind::kGroup && editor_.explorer_nodes[i].key == renamed) {
              editor_.explorer_chosen = static_cast<std::int32_t>(i);
              editor_.explorer_nodes[static_cast<std::size_t>(editor_.explorer_nodes[i].parent)].expanded = true;
              editor_.explorer_nodes[static_cast<std::size_t>(editor_.explorer_nodes[static_cast<std::size_t>(editor_.explorer_nodes[i].parent)].parent)].expanded = true;
            }
          }
          refresh_explorer();
          return;
        }
        if (event.kind != Kind::kCommand) return;
        switch (event.id) {
          case 2: {  // Delete
            const std::vector<core::ObjectId> members(world.groups().members(group).begin(), world.groups().members(group).end());
            for (const core::ObjectId id : members) (void)world.groups().remove(group, id);
            editor_.groups_deleted.insert(editor_.explorer_group);
            close_explorer_pane();
            build_explorer_tree();
            for (std::size_t i = 0; i < editor_.explorer_nodes.size(); ++i) {
              if (editor_.explorer_nodes[i].kind == ExplorerKind::kBranch && editor_.explorer_nodes[i].label == item_label("Groups", "")) {
                editor_.explorer_chosen = static_cast<std::int32_t>(i);
                editor_.explorer_nodes[i].expanded = true;
                editor_.explorer_nodes[static_cast<std::size_t>(editor_.explorer_nodes[i].parent)].expanded = true;
              }
            }
            refresh_explorer();
            return;
          }
          case 4:    // Select
          case 5: {  // Properties
            const core::ObjectId id = member_at(menu.selected("List"));
            const core::sim::WorldObject* object = world.find(id);
            if (object == nullptr) return;
            editor_.selected = id;
            editor_.drag_from = object->state.position;
            core::sim::Selection& selection = session_->selections().player(local_player_);
            selection.clear();
            selection.add(id);
            look_x_ = object->state.position.x;
            look_y_ = object->state.position.y;
            look_pending_ = true;
            if (event.id == 5) open_object_properties({id});
            return;
          }
          case 6: {  // Remove
            const core::ObjectId id = member_at(menu.selected("List"));
            if (id == core::kNoObject) return;
            (void)world.groups().remove(group, id);
            PropertyEdits& edits = editor_.props[id];
            std::erase(edits.groups_added, editor_.explorer_group);
            if (std::find(edits.groups_removed.begin(), edits.groups_removed.end(), editor_.explorer_group) == edits.groups_removed.end()) {
              edits.groups_removed.push_back(editor_.explorer_group);
            }
            fill_group_properties();
            return;
          }
          case 7: {  // Add selection
            if (editor_.selected == core::kNoObject || world.find(editor_.selected) == nullptr) return;
            (void)world.groups().add(group, editor_.selected);
            PropertyEdits& edits = editor_.props[editor_.selected];
            std::erase(edits.groups_removed, editor_.explorer_group);
            if (std::find(edits.groups_added.begin(), edits.groups_added.end(), editor_.explorer_group) == edits.groups_added.end()) {
              edits.groups_added.push_back(editor_.explorer_group);
            }
            fill_group_properties();
            return;
          }
          case 8: {  // Select All
            core::sim::Selection& selection = session_->selections().player(local_player_);
            selection.clear();
            std::int64_t sx = 0;
            std::int64_t sy = 0;
            std::int64_t n = 0;
            for (const core::ObjectId id : world.groups().members(group)) {
              const core::sim::WorldObject* object = world.find(id);
              if (object == nullptr) continue;
              selection.add(id);
              sx += object->state.position.x;
              sy += object->state.position.y;
              ++n;
              editor_.selected = id;
            }
            if (n > 0) {
              look_x_ = static_cast<std::int32_t>(sx / n);
              look_y_ = static_cast<std::int32_t>(sy / n);
              look_pending_ = true;
            }
            return;
          }
          default:
            return;
        }
      });
  if (pane == nullptr) return;
  editor_.explorer_pane = pane;
  dock_in_explorer(pane);
  fill_group_properties();
}

/// The editor's windows, in the order `nextwnd` visits them.
///
/// The original keeps **two** lists and walks the first and then the
/// second (0x00493a00): it finds the active window in list one and takes
/// the node after it, falling through to the head of list two at the end
/// and back to the head of list one at the end of that; a window in
/// neither list starts the walk at the head of list one. The two lists are
/// filled by the constructors of two different window classes, and the
/// executable carries no RTTI, so **which of this editor's windows would
/// sit in which list was not read.** The order below is this engine's,
/// labelled: the tools palette, which is the editor's own window and is
/// always open, then the others as they were opened.
///
/// `prevwnd` is the other half of the pair. `DATA\vxAction.xml` lists it
/// among the actions and gives it no `<shortcut>`, so nothing can reach
/// it; read, recorded, left unbound.
std::vector<core::ui::Dialog*> Application::editor_windows() {
  std::vector<core::ui::Dialog*> windows;
  const auto add = [&](core::ui::Dialog* dialog) {
    if (dialog == nullptr || ui_.index_of(dialog) >= ui_.dialog_count()) return;
    if (std::find(windows.begin(), windows.end(), dialog) != windows.end()) return;
    windows.push_back(dialog);
  };
  add(palette_dialog());
  add(editor_.explorer);
  add(editor_.props_parent);
  add(editor_.place_label);
  // Anything else the editor has opened that is not one of the panes those
  // four dock. A window built later is in the cycle without being named
  // here, which is the point of asking the stack rather than a list.
  for (std::size_t i = 0; i < ui_.dialog_count(); ++i) {
    core::ui::Dialog* dialog = ui_.dialog(i);
    if (dialog == nullptr || !is_editor_pane(*dialog)) continue;
    if (dialog == editor_.tool_pane || dialog == editor_.explorer_pane ||
        dialog == editor_.props_sheet) {
      continue;
    }
    add(dialog);
  }
  return windows;
}

void Application::editor_activate_window(core::ui::Dialog* window) {
  if (window == nullptr) return;
  // A stack index held across a raise means something else afterwards, and
  // the press that a release is looking for is one. Resolve it to the
  // dialog it names, reorder, then find it again.
  core::ui::Dialog* pressed =
      mouse_menu_ < ui_.dialog_count() ? ui_.dialog(mouse_menu_) : nullptr;
  raise_menu(window);
  // The pane docked in it goes with it, and above it, which is the order
  // they were opened in.
  core::ui::Dialog* pane = window == palette_dialog()     ? editor_.tool_pane
                           : window == editor_.explorer   ? editor_.explorer_pane
                           : window == editor_.props_parent ? editor_.props_sheet
                                                            : nullptr;
  if (pane != nullptr) raise_menu(pane);
  mouse_menu_ = pressed == nullptr ? ~std::size_t{0} : ui_.index_of(pressed);
  if (mouse_menu_ >= ui_.dialog_count()) mouse_menu_ = ~std::size_t{0};
  // A window that moved has to be laid out where it stands, and a pane
  // that follows it with it.
  redock_windows();
}

/// The docked dialogs follow their frames: the tool's pane sits at the
/// palette's `SettingsPosCtl` (8, 253) -- `AdvArea.ini`, a 550-wide
/// `AdvDlg` rather than a settings pane, beside the palette instead --
/// the sheet at its parent's (8, 26), the explorer's at its (265, 27).
void Application::redock_windows() {
  const core::ui::Rect canvas = ui_.canvas_rect();
  // A pane whose window is rolled up waits off the display.
  const auto parked = [&](core::ui::Dialog* parent, core::ui::Dialog* pane) {
    if (parent == nullptr || pane == nullptr || editor_.collapsed.count(parent) == 0) return false;
    ui_.place_dialog(pane, 100000, 0);
    return true;
  };
  if (core::ui::Dialog* palette = palette_dialog(); palette != nullptr && editor_.tool_pane != nullptr && !parked(palette, editor_.tool_pane)) {
    const core::ui::Rect at = palette->rect();
    if (editor_.tool_pane->screen().path.find("AdvArea") != std::string::npos) {
      ui_.place_dialog(editor_.tool_pane, at.x - canvas.x - 8 - editor_.tool_pane->screen().design.width, at.y - canvas.y);
    } else {
      ui_.place_dialog(editor_.tool_pane, at.x - canvas.x + 8, at.y - canvas.y + 253);
    }
  }
  if (editor_.props_parent != nullptr && editor_.props_sheet != nullptr && !parked(editor_.props_parent, editor_.props_sheet)) {
    const core::ui::Rect at = editor_.props_parent->rect();
    ui_.place_dialog(editor_.props_sheet, at.x - canvas.x + 8, at.y - canvas.y + 26);
  }
  if (editor_.explorer != nullptr && editor_.explorer_pane != nullptr && !parked(editor_.explorer, editor_.explorer_pane)) {
    dock_in_explorer(editor_.explorer_pane);
  }
}

/// The palette's Collapse (0x004a25b0): the window's height is remembered
/// and set to 0xac, and the widgets below the strip are hidden; Expand
/// (0x004a23e0) puts the height back and shows them. Here the window is
/// laid out again at the shorter height -- its springs pull the frame and
/// the tree to it -- and a pane docked in it is parked out of sight until
/// it unrolls. **Reading, labelled:** the same 172 pixels for every editor
/// window; the palette's constant is the one read.
void Application::editor_toggle_collapse(core::ui::Dialog* dialog) {
  if (dialog == nullptr) return;
  const core::ui::Rect rect = dialog->rect();
  const auto found = editor_.collapsed.find(dialog);
  if (found == editor_.collapsed.end()) {
    editor_.collapsed.emplace(dialog, rect.height);
    dialog->resize(rect.width, 172);
  } else {
    dialog->resize(rect.width, found->second);
    editor_.collapsed.erase(found);
  }
  redock_windows();
}

/// A dialog docked where the explorer's `SettingsPosCtl` stands (265, 27,
/// 550 x 610 in the design), sized to the room the narrowed frame has for
/// it: the editor's dialogs are `AdvDlg`s with springs and a `MinSize`.
void Application::dock_in_explorer(core::ui::Dialog* pane) {
  core::ui::Dialog* explorer = explorer_dialog();
  if (explorer == nullptr || pane == nullptr) return;
  const core::ui::Rect canvas = ui_.canvas_rect();
  const core::ui::Rect frame = explorer->rect();
  const std::int32_t width = std::min(pane->screen().design.width, std::max(345, frame.width - 265 - 8));
  const std::int32_t height = std::min(pane->screen().design.height, std::max(200, frame.height - 27 - 16));
  pane->resize(width, height);
  ui_.place_dialog(pane, frame.x - canvas.x + 265, frame.y - canvas.y + 27);
}

void Application::fill_group_properties() {
  core::ui::Dialog* pane = explorer_pane();
  if (pane == nullptr || pane->screen().path.find("AdvGrpProps") == std::string::npos) return;
  const core::sim::World& world = session_->world();
  pane->set_text("Name", editor_.explorer_group);
  std::vector<std::string> rows;
  const std::int32_t group = world.groups().find(editor_.explorer_group);
  if (group != core::sim::GroupTable::kNoGroup) {
    for (const core::ObjectId id : world.groups().members(group)) rows.push_back(object_display_line(id));
  }
  const std::int32_t selected = pane->selected("List");
  pane->set_items("List", std::move(rows));
  pane->select("List", selected);
}

// -- the explorer's documents ----------------------------------------------------

/// The text of one of the container's documents as this run of the editor
/// holds it: the source's bytes the first time, and patched in place from
/// then on (`core/xml_patch.hpp`). Empty when the container has no such
/// document.
std::string& Application::editor_document_text(const std::string& path) {
  auto found = editor_.documents.find(path);
  if (found == editor_.documents.end()) {
    std::string text;
    const std::vector<std::byte> bytes = container_.read(path);
    text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    found = editor_.documents.emplace(path, std::move(text)).first;
  }
  return found->second;
}

/// `Maps/<n>/` of the map being edited.
std::string Application::editor_map_prefix() const {
  return map_directory_.empty() ? std::string("Maps/1/") : map_directory_ + "/";
}

/// The centre of the view, for the *set starting point* buttons.
/// **This engine's:** what point the original's button takes was not read;
/// the view's centre is the one thing the button can mean without a click.
core::sim::Point Application::editor_view_centre() const {
  return camera_.unproject(window_.pixel_width() / 2, window_.pixel_height() / 2);
}

/// The document dialogs the explorer docks, filled from the container's
/// text and patched as they are worked. Each is the field list its screen
/// declares; what the original does with a field beyond writing it back
/// was not read, and the writers here are the patches themselves.
void Application::fill_explorer_pane(const ExplorerNode& node, core::ui::Dialog& pane) {
  const std::string& path = pane.screen().path;
  const auto has = [&](const char* name) { return path.find(name) != std::string::npos; };
  if (has("AdvPlayer")) {
    // `player<i>.xml`: `name`, `race`, `control`, `AI`, `startx`/`starty`.
    const std::string document = "player" + node.key + ".xml";
    const std::string& text = editor_document_text(document);
    pane.set_text("Name.Edit", core::xml_get_attribute(text, "playerdata", "name"));
    std::vector<std::string> races;
    std::int32_t race_row = -1;
    const std::string race = core::xml_get_attribute(text, "playerdata", "race");
    for (std::int32_t i = 0; i < core::sim::kRaceCount; ++i) {
      races.emplace_back(core::sim::race_to_name(i));
      if (fold_name(races.back()) == fold_name(race)) race_row = i;
    }
    pane.set_items("RaceCombo", std::move(races));
    pane.select("RaceCombo", race_row);
    static const char* const kControls[] = {"Disabled", "Human", "Computer", "Both"};
    std::vector<std::string> controls(std::begin(kControls), std::end(kControls));
    std::int32_t control_row = -1;
    const std::string control = core::xml_get_attribute(text, "playerdata", "control");
    for (std::size_t i = 0; i < controls.size(); ++i) {
      if (fold_name(controls[i]) == fold_name(control)) control_row = static_cast<std::int32_t>(i);
    }
    pane.set_items("ControlCombo", std::move(controls));
    pane.select("ControlCombo", control_row);
    // The AI combo as the exe fills it (0x004743b0): *None* first, then
    // three rows per AI profile -- `%s1(%s2)` of the profile's name, its
    // first letter kept and the rest lowered, and `easy`, `medium`, `hard`
    // translated in the `AIProfile` context. The profiles are the
    // subdirectories of `data/ai/` (0x004433b0 lists them and sorts them),
    // less `default` and any name beginning with `_`. Each row carries the
    // profile and a difficulty of 0, 1 or 2, and the slot's own `AI` and
    // `difficulty` pick the row shown.
    std::vector<std::string> ai_rows{item_label("None", "")};
    std::vector<std::string> ai_data{""};
    std::int32_t ai_row = 0;
    const std::string ai = core::xml_get_attribute(text, "playerdata", "AI");
    std::int32_t difficulty = 0;
    (void)core::parse_int(core::xml_get_attribute(text, "playerdata", "difficulty"), difficulty);
    std::vector<std::string> profiles;
    for (const std::string& stored : vfs_.list()) {
      // The packs list `DATA\AI\DEFENSIVE\MAIN.VS`, upper case, backslashes.
      std::string folded = fold_name(stored);
      for (char& c : folded) if (c == '\\') c = '/';
      if (folded.rfind("data/ai/", 0) != 0) continue;
      const std::size_t slash = folded.find('/', 8);
      if (slash == std::string::npos) continue;
      std::string profile = stored.substr(8, slash - 8);
      if (profile.empty() || profile[0] == '_' || fold_name(profile) == "default") continue;
      if (std::find_if(profiles.begin(), profiles.end(), [&](const std::string& p) { return fold_name(p) == fold_name(profile); }) == profiles.end()) {
        profiles.push_back(std::move(profile));
      }
    }
    std::sort(profiles.begin(), profiles.end(), [&](const std::string& a, const std::string& b) { return fold_name(a) < fold_name(b); });
    static const char* const kDifficulties[] = {"easy", "medium", "hard"};
    for (const std::string& profile : profiles) {
      std::string shown = profile;
      for (std::size_t i = 1; i < shown.size(); ++i) shown[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(shown[i])));
      for (std::int32_t level = 0; level < 3; ++level) {
        if (fold_name(profile) == fold_name(ai) && level == difficulty) ai_row = static_cast<std::int32_t>(ai_rows.size());
        ai_rows.push_back(shown + "(" + item_label(kDifficulties[level], "AIProfile") + ")");
        ai_data.push_back(profile + "\n" + std::to_string(level));
      }
    }
    pane.set_items("AICombo", std::move(ai_rows));
    pane.select("AICombo", ai_row);
    pane.content().state("AICombo").items_data = std::move(ai_data);
    pane.set_text("Start.Text", "Start: " + core::xml_get_attribute(text, "playerdata", "startx") + "," +
                                    core::xml_get_attribute(text, "playerdata", "starty"));
    std::int32_t colour = 0;
    if (core::parse_int(core::xml_get_attribute(text, "playerdata", "color"), colour)) {
      // `color` is X1R5G5B5 (`31810` = 0x7C42), shown on the square.
      const auto five = [](std::int32_t v) { return static_cast<std::uint8_t>((v << 3) | (v >> 2)); };
      core::ui::WidgetState& square = pane.content().state("color");
      square.has_color = true;
      square.color = core::ui::Color{five((colour >> 10) & 31), five((colour >> 5) & 31), five(colour & 31), 255};
    }
    return;
  }
  if (has("AdvScenario") || has("AdvAdventure")) {
    // `game.xml`'s `<properties>`: name, author, description, the victory
    // condition and its threshold, `single_only`; `map.xml`'s `<expl>` for
    // the two fog flags on the scenario sheet.
    const std::string& text = editor_document_text("game.xml");
    pane.set_text("NameEdit", core::xml_get_attribute(text, "game/properties", "name"));
    pane.set_text("AuthorEdit", core::xml_get_attribute(text, "game/properties", "author"));
    pane.set_text("DescrEdit", core::xml_get_attribute(text, "game/properties", "description"));
    if (has("AdvScenario")) {
      static const char* const kConditions[] = {"None", "Elimination", "Score limit", "Time limit (military rating)",
                                                "Time limit (score)"};
      std::vector<std::string> conditions(std::begin(kConditions), std::end(kConditions));
      pane.set_items("VictoryCondCombo", std::move(conditions));
      pane.select("VictoryCondCombo", static_cast<std::int32_t>(core::sim::parse_victory_condition(
                                          core::xml_get_attribute(text, "game/properties", "victory_condition"))));
      pane.set_items("VictoryThresCB", {core::xml_get_attribute(text, "game/properties", "victory_threshold")});
      pane.select("VictoryThresCB", 0);
      pane.set_row("SinglePlayerOnlyBtn", core::xml_get_attribute(text, "game/properties", "single_only") == "1" ? 1 : 0);
      const std::string& map = editor_document_text(editor_map_prefix() + "map.xml");
      pane.set_row("NoFogBtn", core::xml_get_attribute(map, "map/expl", "NoFog") == "1" ? 1 : 0);
      pane.set_row("ExploreBtn", core::xml_get_attribute(map, "map/expl", "NoExplore") == "1" ? 1 : 0);
      pane.set_text("MiniMapEdit", core::xml_get_attribute(map, "map/user_art", "explored"));
      fill_labels(pane);
    } else {
      fill_maps(pane);
      std::vector<std::string> players;
      for (std::size_t i = 0; i < core::sim::kPlayerCount; ++i) players.push_back(std::to_string(i + 1));
      pane.set_items("PlayerCombo", std::move(players));
      std::int32_t start_player = 0;
      (void)core::parse_int(core::xml_get_attribute(text, "game/properties", "start_player"), start_player);
      pane.select("PlayerCombo", start_player);
      std::vector<std::string> races;
      for (std::int32_t i = 0; i < core::sim::kRaceCount; ++i) races.emplace_back(core::sim::race_to_name(i));
      pane.set_items("RaceCombo", std::move(races));
      std::int32_t race = 0;
      (void)core::parse_int(core::xml_get_attribute(text, "game/properties", "user_interface"), race);
      pane.select("RaceCombo", race);
    }
    return;
  }
  if (has("AdvCurMap")) {
    const std::string& map = editor_document_text(editor_map_prefix() + "map.xml");
    pane.set_text("NameEdit", core::xml_get_attribute(map, "map", "name"));
    pane.set_text("DispNameEdit", core::xml_get_attribute(map, "map", "displayname"));
    pane.set_text("DescrEdit", core::xml_get_attribute(map, "map", "descr"));
    pane.set_row("NoFogBtn", core::xml_get_attribute(map, "map/expl", "NoFog") == "1" ? 1 : 0);
    pane.set_row("ExploreBtn", core::xml_get_attribute(map, "map/expl", "NoExplore") == "1" ? 1 : 0);
    pane.set_row("PersistStateBtn", core::xml_get_attribute(map, "map", "persist_state") == "1" ? 1 : 0);
    pane.set_text("Start.Text", "Start: " + core::xml_get_attribute(map, "map/start_pt", "x") + "," +
                                    core::xml_get_attribute(map, "map/start_pt", "y"));
    fill_maps(pane);
    fill_labels(pane);
    return;
  }
  if (has("AdvNotes")) {
    const std::string document = note_document_of(node.key);
    if (document.empty()) return;
    const std::string& text = editor_document_text(document);
    const std::string at = "notes/note[id=" + node.key + "]";
    pane.set_text("NameEdit", node.key);
    pane.set_text("TitleEdit", core::xml_get_attribute(text, at, "title"));
    pane.set_text("DescrEdit", core::xml_get_attribute(text, at, "text"));
    std::vector<std::string> maps;
    maps.emplace_back("");
    std::int32_t map_row = 0;
    const std::string map_name = core::xml_get_attribute(text, at, "map");
    for (const std::string& directory : container_.map_directories()) {
      const std::vector<std::byte> bytes = container_.read(directory + "/map.xml");
      std::string name;
      if (!bytes.empty()) {
        name = core::xml_get_attribute(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), "map", "name");
      }
      if (name == map_name) map_row = static_cast<std::int32_t>(maps.size());
      maps.push_back(std::move(name));
    }
    pane.set_items("LocMapCombo", std::move(maps));
    pane.select("LocMapCombo", map_row);
    pane.set_text("PosEdit", core::xml_get_attribute(text, at, "locationx") + "," + core::xml_get_attribute(text, at, "locationy"));
    // The icons: `editorini/combo/note_icons.ini`'s `[FillCombo]`, as the
    // sheet's unit icons; the note's own first when it names another.
    std::vector<std::string> names;
    std::vector<std::string> paths;
    const std::string icon = core::xml_get_attribute(text, at, "icon");
    std::int32_t icon_row = -1;
    if (const platform::ByteSpan bytes = vfs_.read("DATA\\INTERFACE\\EDITOR\\COMBO\\NOTE_ICONS.INI"); !bytes.empty()) {
      if (const core::Result<core::IniDocument> doc = core::IniDocument::parse(platform::as_core_bytes(bytes)); doc.ok()) {
        for (const core::IniEntry& entry : doc->entries_of(doc->section("FillCombo"))) {
          if (!entry.has_key) continue;
          if (fold_name(std::string(entry.value)) == fold_name(icon)) icon_row = static_cast<std::int32_t>(names.size());
          names.emplace_back(entry.key);
          paths.emplace_back(entry.value);
        }
      }
    }
    pane.set_items("IconCombo", std::move(names));
    pane.select("IconCombo", icon_row);
    pane.content().state("IconCombo").items_data = std::move(paths);
    pane.content().state("IconPreview").image = icon;
    pane.set_row("ShowIconCheck", core::xml_get_attribute(text, at, "show_on_minimap") == "1" ? 1 : 0);
    return;
  }
  if (has("AdvDiplomacy")) {
    fill_diplomacy(pane);
    return;
  }
  if (has("AdvItems")) {
    fill_item_type(node.key, pane);
    return;
  }
  if (has("AdvConverse")) {
    fill_conversation(node.key, pane);
    return;
  }
  if (has("AdvItemHolder")) {
    // `CurMap/IHolder/<name>`: a chest's script name and its four slots,
    // the sheet's inventory tab over a holder object.
    const auto id = static_cast<core::ObjectId>(std::atoi(node.key.c_str()));
    core::sim::World& world = session_->world();
    pane.set_text("NameEdit", std::string(world.named_objects().name_of(id)));
    if (const core::sim::WorldObject* object = world.find(id)) {
      const core::ClassGraph* graph = world.class_graph();
      pane.set_text("ClassName", graph != nullptr && object->class_index != core::kNoClass
                                     ? std::string(graph->classes()[static_cast<std::size_t>(object->class_index)].id)
                                     : std::string());
      pane.set_text("LocPos", std::to_string(object->state.position.x) + "," + std::to_string(object->state.position.y));
    }
    const core::sim::HeroSystem* heroes = core::sim::hero_system_of(world);
    std::vector<std::string> catalogue;
    catalogue.push_back(item_label("<None>", ""));
    if (heroes != nullptr && heroes->items().catalog() != nullptr) {
      for (const core::sim::ItemDefinition& item : heroes->items().catalog()->definitions()) catalogue.push_back(item.id);
    }
    std::vector<core::ObjectId> held;
    if (heroes != nullptr) heroes->items().contents_for(id, held);
    for (std::size_t slot = 0; slot < 4; ++slot) {
      const std::string name = "Slot" + std::to_string(slot + 1) + "Combo";
      pane.set_items(name, catalogue);
      std::int32_t chosen = 0;
      const core::sim::ItemInstance* instance = heroes != nullptr && slot < held.size() ? heroes->items().find(held[slot]) : nullptr;
      const core::sim::ItemDefinition* definition = instance != nullptr ? heroes->items().catalog()->at(instance->type) : nullptr;
      for (std::size_t i = 1; definition != nullptr && i < catalogue.size(); ++i) {
        if (catalogue[i] == definition->id) chosen = static_cast<std::int32_t>(i);
      }
      pane.select(name, chosen);
    }
    return;
  }
  if (has("AdvResources")) {
    pane.set_row("All.Radio", 1);
    fill_resources(pane, 1);
    return;
  }
  if (has("AdvOutpost")) {
    const core::sim::EconomySystem* economy = core::sim::economy_of(session_->world());
    const core::sim::Settlement* town = economy != nullptr ? economy->settlements().find(static_cast<core::sim::SettlementId>(std::atoi(node.key.c_str()))) : nullptr;
    if (town == nullptr) return;
    pane.set_text("NameEdit", town->name);
    pane.set_text("GoldEdit", std::to_string(town->gold()));
    pane.set_text("FoodEdit", std::to_string(town->food()));
    std::vector<std::string> players;
    for (std::size_t i = 0; i < core::sim::kPlayerCount; ++i) {
      players.push_back(item_label("Player", "/Editor/PlaceDefence.ini:PlayerHint:Text") + " " + std::to_string(i + 1));
    }
    pane.set_items("PlayerCombo", std::move(players));
    pane.select("PlayerCombo", town->owner == core::kNoPlayer ? -1 : static_cast<std::int32_t>(town->owner));
    return;
  }
  if (has("AdvSequence")) {
    // `sequences.xml`'s element and the script it names, shown; the source
    // is read-only here (a multi-line editor and the compiler's messages
    // are the next slice).
    std::string document = editor_map_prefix() + "Sequences/sequences.xml";
    std::string at = "sequences/sequence[name=" + node.key + "]";
    if (core::xml_get_attribute(editor_document_text(document), at, "script").empty()) document = "Sequences/sequences.xml";
    const std::string& text = editor_document_text(document);
    pane.set_text("NameEdit", node.key);
    pane.set_text("PreEdit", core::xml_get_attribute(text, at, "prerequisites"));
    const std::string autorun = core::xml_get_attribute(text, at, "autorunallowed");
    pane.set_row("AutoRunCBox", autorun.empty() || autorun == "yes" || autorun == "1" ? 1 : 0);
    pane.set_text("ScriptEdit", editor_document_text(sequence_script_path(node.key)));
    pane.set_text("ErrorText", "");
    return;
  }
}

/// The `.vs` a sequence's element names, as a container path:
/// `CurrentMap/` is the map's directory, `CurrentGame/` the root.
std::string Application::sequence_script_path(const std::string& name) {
  std::string document = editor_map_prefix() + "Sequences/sequences.xml";
  const std::string at = "sequences/sequence[name=" + name + "]";
  if (core::xml_get_attribute(editor_document_text(document), at, "script").empty()) document = "Sequences/sequences.xml";
  std::string script = core::xml_get_attribute(editor_document_text(document), at, "script");
  if (script.rfind("CurrentMap/", 0) == 0) return editor_map_prefix() + script.substr(11);
  if (script.rfind("CurrentGame/", 0) == 0) return script.substr(12);
  return script;
}

// -- the explorer's New leaves ------------------------------------------------------

/// `base`, or `base 2`, `base 3`, ... -- the first `taken` does not refuse.
std::string Application::unique_name(std::string base, const std::function<bool(const std::string&)>& taken) const {
  if (!taken(base)) return base;
  for (int n = 2;; ++n) {
    const std::string candidate = base + " " + std::to_string(n);
    if (!taken(candidate)) return candidate;
  }
}

/// Choose the node of `pane` whose key is `key` after the tree is rebuilt,
/// its branch unfolded, and open it.
void Application::explorer_reselect(std::string_view pane, const std::string& key) {
  build_explorer_tree();
  for (std::size_t i = 0; i < editor_.explorer_nodes.size(); ++i) {
    ExplorerNode& node = editor_.explorer_nodes[i];
    if (node.kind != ExplorerKind::kPane || node.key != key || fold_name(node.pane) != fold_name(std::string(pane))) continue;
    for (std::int32_t parent = node.parent; parent >= 0; parent = editor_.explorer_nodes[static_cast<std::size_t>(parent)].parent) {
      editor_.explorer_nodes[static_cast<std::size_t>(parent)].expanded = true;
    }
    editor_.explorer_chosen = -1;
    explorer_choose(static_cast<std::int32_t>(i));
    return;
  }
  refresh_explorer();
}

/// What a *New ...* leaf makes, in the map's own documents, named after the
/// leaf and numbered past a name already taken; the tree is rebuilt and
/// the new node opened, its dialog ready for the name to be typed over.
/// **This engine's, labelled:** which document the exe adds to -- it keeps
/// a manager per document and a *New* leaf under each, where this tree
/// lists the map's and the root's together -- so a note, a sequence and a
/// conversation go to the map's (`Maps/<n>/`), where 150 of the 172
/// shipped notes, every shipped sequence and 109 of the 110 shipped
/// conversations live; an item type has one document, the root's
/// `itemsCustom.xml`. The elements carry the attributes the shipped ones
/// do, empty; a sequence's `.vs` is made empty, a conversation's document
/// with its root alone. A new item type joins the session's catalogue on
/// the next load, not this one.
void Application::explorer_create(const std::string& what) {
  const std::string prefix = editor_map_prefix();
  if (what == "note") {
    const std::string document = prefix + "Notes.xml";
    std::string& text = editor_document_text(document);
    if (text.find("<notes") == std::string::npos) {
      text = "\t<notes>\r\n\t</notes>\r\n";
      editor_.created.insert(document);
    }
    const std::string id = unique_name(item_label("New note", ""), [&](const std::string& candidate) {
      return !note_document_of(candidate).empty();
    });
    const std::string map_name = core::xml_get_attribute(editor_document_text(prefix + "map.xml"), "map", "name");
    const core::XmlEdit attributes[] = {{"id", id},   {"title", id},          {"text", ""},        {"icon", ""},
                                        {"map", map_name}, {"show_on_minimap", "0"}, {"locationx", "-1"}, {"locationy", "-1"}};
    bool found = false;
    text = core::xml_append_element(text, "notes", "note", attributes, &found);
    if (found) explorer_reselect("editorini/AdvNotes.ini", id);
    return;
  }
  if (what == "sequence") {
    const std::string document = prefix + "Sequences/sequences.xml";
    std::string& text = editor_document_text(document);
    if (text.find("<sequences") == std::string::npos) {
      text = "\t<sequences>\r\n\t</sequences>\r\n";
      editor_.created.insert(document);
    }
    const std::string root = editor_document_text("Sequences/sequences.xml");
    const std::string name = unique_name(item_label("New sequence", ""), [&](const std::string& candidate) {
      const std::string at = "sequences/sequence[name=" + candidate + "]";
      return !core::xml_get_attribute(text, at, "script").empty() || !core::xml_get_attribute(root, at, "script").empty();
    });
    // `seq<k>.vs`, the first k no file of the map's `Sequences/` takes.
    int k = 0;
    const auto script_taken = [&](int n) {
      const std::string path = prefix + "Sequences/seq" + std::to_string(n) + ".vs";
      return container_.contains(path) || editor_.documents.count(path) != 0;
    };
    while (script_taken(k)) ++k;
    const std::string script = "CurrentMap/sequences/seq" + std::to_string(k) + ".vs";
    const core::XmlEdit attributes[] = {{"name", name}, {"wizard", ""}, {"script", script}};
    bool found = false;
    text = core::xml_append_element(text, "sequences", "sequence", attributes, &found);
    if (!found) return;
    // One blank line rather than no bytes: an empty span is what the
    // resolver answers for a file that is not there, and the sequence
    // would start as *no source*; a line of nothing compiles to nothing.
    const std::string path = prefix + "Sequences/seq" + std::to_string(k) + ".vs";
    editor_document_text(path) = "\r\n";
    editor_.created.insert(path);
    explorer_reselect("editorini/AdvSequence.ini", name);
    return;
  }
  if (what == "conversation") {
    // `cnv<k>.conv.xml` in the map's `Conversations/`, the first k free.
    int k = 0;
    const auto taken = [&](int n) {
      const std::string path = prefix + "Conversations/cnv" + std::to_string(n) + ".conv.xml";
      return container_.contains(path) || editor_.documents.count(path) != 0;
    };
    while (taken(k)) ++k;
    const std::string name = unique_name(item_label("New conversation", ""), [&](const std::string& candidate) {
      return !conversation_document_of(candidate).empty();
    });
    const std::string path = prefix + "Conversations/cnv" + std::to_string(k) + ".conv.xml";
    std::string escaped;
    for (const char c : name) {
      if (c == '&') escaped += "&amp;";
      else if (c == '<') escaped += "&lt;";
      else if (c == '"') escaped += "&quot;";
      else escaped.push_back(c);
    }
    editor_document_text(path) =
        "\t<conversation\r\n\t\tname=\"" + escaped + "\"\r\n\t\tstartup=\"first\"\r\n\t\trestore_view=\"0\">\r\n\t</conversation>\r\n";
    editor_.created.insert(path);
    explorer_reselect("editorini/AdvConverse.ini", name);
    return;
  }
  if (what == "item") {
    std::string& text = editor_document_text("itemsCustom.xml");
    if (text.find("<items") == std::string::npos) {
      text = "\t<items>\r\n\t</items>\r\n";
      editor_.created.insert("itemsCustom.xml");
    }
    const std::string id = unique_name(item_label("New item type", ""), [&](const std::string& candidate) {
      return !core::xml_get_attribute(text, "items/item[id=" + candidate + "]", "id").empty();
    });
    const core::XmlEdit attributes[] = {
        {"id", id},           {"name", id},             {"image", ""},          {"sound", ""},
        {"level", "0"},       {"description", ""},      {"usecount", "0"},      {"customdata", "0"},
        {"use_script", ""},   {"location_script", ""},  {"object_script", ""},  {"kill_script", ""},
        {"die_script", ""},   {"attachedkill_script", ""}, {"attacheddie_script", ""}, {"equip_script", ""},
        {"remove_script", ""}, {"important", "no"},     {"cursed", "0"}};
    const core::XmlEdit bonus[] = {{"health", "0"},         {"damage", "0"},         {"armor_slash", "0"},
                                   {"armor_pierce", "0"},   {"health_percent", "0"}, {"damage_percent", "0"},
                                   {"armor_slash_percent", "0"}, {"armor_pierce_percent", "0"}, {"level", "0"},
                                   {"experience", "0"}};
    bool found = false;
    text = core::xml_append_element(text, "items", "item", attributes, "bonus", bonus, &found);
    if (found) explorer_reselect("editorini/AdvItems.ini", id);
    return;
  }
}

// -- the Maps tab ------------------------------------------------------------------

/// `Maps/<n>` of every map the container will hold when written: the
/// source's, less those deleted this run, plus those made this run,
/// ascending by number.
std::vector<std::string> Application::editor_map_list() {
  std::set<int> numbers;
  for (const std::string& directory : container_.map_directories()) {
    if (editor_.removed_maps.count(directory) != 0) continue;
    numbers.insert(SDL_atoi(gamedata::map_number(directory).c_str()));
  }
  for (const std::string& path : editor_.created) {
    std::string folded = fold_name(path);
    for (char& c : folded) if (c == '\\') c = '/';
    if (folded.rfind("maps/", 0) != 0 || !folded.ends_with("/map.xml")) continue;
    const std::string directory = path.substr(0, path.size() - 8);
    if (editor_.removed_maps.count(directory) != 0) continue;
    numbers.insert(SDL_atoi(gamedata::map_number(directory).c_str()));
  }
  std::vector<std::string> out;
  for (const int n : numbers) out.push_back("Maps/" + std::to_string(n));
  return out;
}

std::string Application::editor_map_name(const std::string& directory) {
  return core::xml_get_attribute(editor_document_text(directory + "/map.xml"), "map", "name");
}

/// The Maps tab as 0x0046fac0 fills it: a row per map, its `map.xml` name,
/// the map being edited marked with `EditorRes/curmap.bmp`; the row chosen
/// before stays chosen. `AdvAdventure`'s `StartMapCombo` offers the same
/// maps and shows `start_map`'s.
void Application::fill_maps(core::ui::Dialog& pane) {
  if (pane.content().state_of("MapsList") == nullptr) return;
  const std::vector<std::string> maps = editor_map_list();
  std::vector<std::string> rows;
  std::vector<std::string> icons;
  bool marked = false;
  for (const std::string& directory : maps) {
    rows.push_back(editor_map_name(directory));
    const bool current = directory == map_directory_;
    icons.emplace_back(current ? "EditorRes/curmap.bmp" : "");
    marked = marked || current;
  }
  if (editor_.map_selected >= static_cast<std::int32_t>(rows.size())) editor_.map_selected = -1;
  pane.set_items("MapsList", std::move(rows));
  pane.content().state("MapsList").icons = marked ? std::move(icons) : std::vector<std::string>{};
  pane.select("MapsList", editor_.map_selected);
  if (pane.content().state_of("StartMapCombo") != nullptr) {
    std::vector<std::string> names;
    std::vector<std::string> numbers;
    std::int32_t row = -1;
    const std::string start = core::xml_get_attribute(editor_document_text("game.xml"), "game/properties", "start_map");
    for (const std::string& directory : maps) {
      const std::string number = gamedata::map_number(directory);
      if (number == start) row = static_cast<std::int32_t>(names.size());
      names.push_back(editor_map_name(directory) + " (" + number + ")");
      numbers.push_back(number);
    }
    pane.set_items("StartMapCombo", std::move(names));
    pane.content().state("StartMapCombo").items_data = std::move(numbers);
    pane.select("StartMapCombo", row);
  }
}

/// The tab's buttons. *New* (`..7001`, 0x0046f380) takes the first number
/// from 1 no map has and copies the blank template's `Maps/1` in as
/// `Maps/<n>` (the exe copies `Packs/newmap/Maps/1`; here it is read out
/// of `Packs/newmap.BFHP`). *Edit* (`..7003`) makes the chosen map the one
/// being edited; *Delete* (`..7004`) leaves the chosen map out of the
/// container written; *Import adventure maps* (`..7005`) and *Import
/// scenario map* (`..7006`) open the file dialog over `Adventures/` or
/// `Scenarios/` (0x004aedd0 with `bfhp`) and copy the chosen container's
/// maps in under fresh numbers. **Readings, labelled:** whether the exe
/// refuses to delete or re-edit the current map is not read; both are
/// refused here, since the map on the screen cannot be dropped from under
/// it. A `StartMapCombo` choice writes `start_map`.
void Application::maps_event(const core::ui::DialogEvent& event, core::ui::Dialog& pane) {
  using Kind = core::ui::DialogEvent::Kind;
  if (pane.content().state_of("MapsList") == nullptr) return;
  if (event.kind == Kind::kSelect && event.widget == "MapsList") {
    editor_.map_selected = event.index;
    return;
  }
  if (event.kind == Kind::kChange && event.widget == "StartMapCombo") {
    const std::int32_t row = pane.selected("StartMapCombo");
    const core::ui::WidgetState* state = pane.content().state_of("StartMapCombo");
    if (row >= 0 && state != nullptr && static_cast<std::size_t>(row) < state->items_data.size()) {
      std::string& text = editor_document_text("game.xml");
      const core::XmlEdit edit[] = {{"start_map", state->items_data[static_cast<std::size_t>(row)]}};
      text = core::xml_set_attributes(text, "game/properties", edit);
    }
    return;
  }
  if (event.kind != Kind::kCommand) return;
  const std::vector<std::string> maps = editor_map_list();
  const bool chosen = editor_.map_selected >= 0 && static_cast<std::size_t>(editor_.map_selected) < maps.size();
  const std::string directory = chosen ? maps[static_cast<std::size_t>(editor_.map_selected)] : std::string();
  if (event.widget == "NewMap") {
    editor_new_map();
    fill_maps(pane);
    return;
  }
  if (event.widget == "DeleteMap") {
    if (!chosen) return;
    if (directory == map_directory_) {
      std::printf("editor: %s is the map being edited and is not deleted\n", directory.c_str());
      return;
    }
    editor_.removed_maps.insert(directory);
    for (auto it = editor_.created.begin(); it != editor_.created.end();) {
      if (it->rfind(directory + "/", 0) == 0) {
        editor_.documents.erase(*it);
        it = editor_.created.erase(it);
      } else {
        ++it;
      }
    }
    editor_.map_selected = -1;
    std::printf("editor: %s deleted; the container written will not hold it\n", directory.c_str());
    fill_maps(pane);
    return;
  }
  if (event.widget == "EditMap") {
    if (!chosen || directory == map_directory_) return;
    editor_switch_map(SDL_atoi(gamedata::map_number(directory).c_str()));
    return;
  }
  if (event.widget == "ImportAdvMap" || event.widget == "ImportScenMap") {
    open_import_maps_dialog(event.widget == "ImportAdvMap");
    return;
  }
}

/// The first number from 1 that no map of the container -- shipped, made
/// or deleted this run -- takes (0x0046f380 counts from 1 to 9999).
int Application::editor_free_map_number() const {
  std::set<std::string> taken;
  for (const std::string& directory : container_.map_directories()) taken.insert(fold_name(directory));
  for (const std::string& path : editor_.created) {
    if (path.rfind("Maps/", 0) != 0) continue;
    const std::size_t slash = path.find('/', 5);
    if (slash != std::string::npos) taken.insert(fold_name(path.substr(0, slash)));
  }
  for (const std::string& directory : editor_.removed_maps) taken.insert(fold_name(directory));
  for (int n = 1; n < 10000; ++n) {
    if (taken.count("maps/" + std::to_string(n)) == 0) return n;
  }
  return -1;
}

/// Every file under `directory` of `from`, copied in as `Maps/<n>/...`
/// under a fresh number; the number, or -1 when there was nothing to copy.
int Application::editor_copy_map(const gamedata::MapContainer& from, const std::string& directory) {
  const int number = editor_free_map_number();
  if (number < 0) return -1;
  const std::string target = "Maps/" + std::to_string(number) + "/";
  std::size_t copied = 0;
  for (const std::string& stored : from.list()) {
    std::string path = stored;
    for (char& c : path) if (c == '\\') c = '/';
    if (fold_name(path).rfind(fold_name(directory) + "/", 0) != 0) continue;
    const std::vector<std::byte> bytes = from.read(stored);
    const std::string out = target + path.substr(directory.size() + 1);
    editor_document_text(out).assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    editor_.created.insert(out);
    ++copied;
  }
  if (copied == 0) return -1;
  std::printf("editor: %s of %s copied in as Maps/%d (%zu files)\n", directory.c_str(),
              from.path().filename().string().c_str(), number, copied);
  return number;
}

void Application::editor_new_map() {
  gamedata::MapContainer blank;
  std::string error;
  if (!blank.open(vfs_.root() / "Packs" / "newmap.BFHP", &error)) {
    std::printf("editor: the blank map template would not open: %s\n", error.c_str());
    return;
  }
  const std::vector<std::string> maps = blank.map_directories();
  if (maps.empty()) return;
  (void)editor_copy_map(blank, maps.front());
}

/// *Edit* on another map of the container. The exe works on a mounted copy
/// of the adventure (`currentadv.bfhp` in the installation's root) and
/// switching is opening another of its maps (0x0046f050); this engine
/// writes nothing before Save, so the switch is a Save first -- to a
/// working copy of its own in the temporary directory, every edit of this
/// run in it -- and the editor opened again on that copy's `Maps/<number>`.
/// Save then writes from the copy, under the original's name.
void Application::editor_switch_map(int number) {
  std::error_code ignored;
  const std::filesystem::path working =
      std::filesystem::temp_directory_path(ignored) / ("imperivm-editor-" + std::to_string(SDL_GetCurrentThreadID()) + ".bfhp");
  const std::string origin = editor_.origin_stem.empty() ? container_.path().stem().string() : editor_.origin_stem;
  if (!editor_save(working)) return;
  close_all_menus();
  infobar_.reset();
  cmdbar_.reset();
  session_.reset();
  buttons_.clear();
  pending_command_.clear();
  fog_view_.reset();
  zoom_ground_ = core::ui::Image{};
  args_.map = working.string();
  args_.map_index = number;
  args_.edit = true;
  if (!start_map() || !start_play()) {
    std::printf("editor: Maps/%d would not open\n", number);
    quit_requested_ = true;
    return;
  }
  editor_.origin_stem = origin;
  std::printf("editor: editing Maps/%d of %s\n", number, origin.c_str());
}

/// `OpenDlg.ini` over `Adventures/` or `Scenarios/`: the `.bfhp` files
/// listed, the one chosen imported -- every map of an adventure, the one
/// map of a scenario -- under fresh numbers.
void Application::open_import_maps_dialog(bool adventure) {
  editor_.import_files.clear();
  std::vector<std::string> names;
  std::error_code ignored;
  const std::filesystem::path folder = vfs_.root() / (adventure ? "Adventures" : "Scenarios");
  for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ignored)) {
    if (!entry.is_regular_file(ignored)) continue;
    if (fold_name(entry.path().extension().string()) != ".bfhp") continue;
    editor_.import_files.push_back(entry.path());
  }
  std::sort(editor_.import_files.begin(), editor_.import_files.end());
  for (const std::filesystem::path& file : editor_.import_files) names.push_back(file.stem().string());
  core::ui::Dialog* dialog = open_menu(
      "editorini/OpenDlg.ini", [this, adventure](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        if (event.kind == Kind::kSelect && event.widget == "List") {
          if (event.index >= 0 && static_cast<std::size_t>(event.index) < editor_.import_files.size()) {
            menu.set_text("NameEdit", editor_.import_files[static_cast<std::size_t>(event.index)].filename().string());
          }
          return;
        }
        const bool open = (event.kind == Kind::kCommand && event.id == 0x1001) || event.kind == Kind::kActivate;
        if (event.kind == Kind::kCommand && event.id == 0x1003) {
          close_menu();
          return;
        }
        if (!open) return;
        const std::int32_t row = menu.selected("List");
        if (row < 0 || static_cast<std::size_t>(row) >= editor_.import_files.size()) return;
        const std::filesystem::path file = editor_.import_files[static_cast<std::size_t>(row)];
        gamedata::MapContainer from;
        std::string error;
        if (!from.open(file, &error)) {
          std::printf("editor: %s\n", error.c_str());
          return;
        }
        std::vector<std::string> maps = from.map_directories();
        if (!adventure && maps.size() > 1) maps.resize(1);
        for (const std::string& directory : maps) (void)editor_copy_map(from, directory);
        close_menu();
        if (core::ui::Dialog* pane = explorer_pane(); pane != nullptr) fill_maps(*pane);
      });
  if (dialog == nullptr) return;
  dialog->set_items("List", std::move(names));
  dialog->set_text("NameEdit", "");
}

// -- the overlay-text labels -----------------------------------------------------

/// `Maps/<n>/labels.xml` -- the exe's `CurrentMap/labels.xml`, a `<root>`
/// of `<label text x y/>` (`docs/formats/map.md`, "labels.xml"). The
/// labels are the editor's: they are drawn on its floating minimap window
/// and on the *Place Label* screen (0x0049f7a0, the only drawer, in
/// `Fonts/tahoma16b.apf`), and nothing in the game's own screens reads the
/// document -- the zoom map does not, and the two callers of the loader
/// (0x0049f590) are both editor code.
std::string Application::labels_document() const {
  return editor_map_prefix() + "labels.xml";
}

/// The document's text, seeded with an empty root where the map ships
/// none (6 of the 29 maps), so that the first label has a parent.
std::string& Application::labels_text() {
  std::string& text = editor_document_text(labels_document());
  if (text.find("<root") == std::string::npos) text = "\t<root>\r\n\t</root>\r\n";
  return text;
}

/// The Labels tab (`Overlay text`) as 0x0049f630 fills it and 0x0049f2f0
/// refreshes it: one row per label, its text; the chosen label's text in
/// `LabelText`, its position in `LabelPos` as `<x>,<y>`; with no label
/// chosen the two are blank and Delete and Place are disabled.
void Application::fill_labels(core::ui::Dialog& pane) {
  if (pane.content().state_of("LabelsList") == nullptr) return;
  const std::string& text = labels_text();
  const std::size_t count = core::xml_count_elements(text, "root/label");
  std::vector<std::string> rows;
  for (std::size_t i = 0; i < count; ++i) {
    rows.push_back(core::xml_decode_entities(
        core::xml_get_attribute(text, "root/label[" + std::to_string(i) + "]", "text")));
  }
  if (editor_.label_selected >= static_cast<std::int32_t>(count)) editor_.label_selected = -1;
  const std::int32_t scroll = pane.content().state("LabelsList").scroll;
  pane.set_items("LabelsList", std::move(rows));
  pane.select("LabelsList", editor_.label_selected);
  pane.content().state("LabelsList").scroll = std::min(scroll, std::max(0, static_cast<std::int32_t>(count) - 1));
  const bool have = editor_.label_selected >= 0;
  const std::string at = "root/label[" + std::to_string(std::max(0, editor_.label_selected)) + "]";
  pane.set_text("LabelText", have ? core::xml_decode_entities(core::xml_get_attribute(text, at, "text")) : "");
  pane.set_text("LabelPos", have ? "<" + core::xml_get_attribute(text, at, "x") + ">,<" +
                                       core::xml_get_attribute(text, at, "y") + ">"
                                 : "");
  pane.set_enabled("LabelDelete", have);
  pane.set_enabled("LabelPosSet", have);
}

/// The tab's handler (0x004a1640). A row chosen shows its label. *New*
/// (id `..7028`) lays a label named `<New label>` -- translated, as the exe
/// does through its table -- at the centre of the view, and chooses it.
/// *Delete* (`..7029`) cuts the chosen one out; the row at the same index
/// is chosen next, or none when it was the last. *Set* (`..7026`) writes
/// `LabelText` into the label -- here the edit writes as it is typed, as
/// every other field of these dialogs does, and Set is the same write.
/// *Place* (`..7027`) opens the *Place Label* screen over the minimap.
void Application::labels_event(const core::ui::DialogEvent& event, core::ui::Dialog& pane) {
  using Kind = core::ui::DialogEvent::Kind;
  if (pane.content().state_of("LabelsList") == nullptr) return;
  std::string& text = labels_text();
  const std::size_t count = core::xml_count_elements(text, "root/label");
  const std::string at = "root/label[" + std::to_string(std::max(0, editor_.label_selected)) + "]";
  const bool have = editor_.label_selected >= 0 && static_cast<std::size_t>(editor_.label_selected) < count;
  if (event.kind == Kind::kSelect && event.widget == "LabelsList") {
    // The row chosen shows its label, and the text edit takes the focus
    // (0x004a18b4), so the typing is the label's.
    editor_.label_selected = event.index;
    fill_labels(pane);
    pane.focus("LabelText");
    return;
  }
  if ((event.kind == Kind::kChange && event.widget == "LabelText") ||
      (event.kind == Kind::kCommand && event.widget == "LabelSet")) {
    if (!have) return;
    const std::string value = pane.text("LabelText");
    const core::XmlEdit edit[] = {{"text", value}};
    text = core::xml_set_attributes(text, at, edit);
    core::ui::WidgetState& list = pane.content().state("LabelsList");
    if (static_cast<std::size_t>(editor_.label_selected) < list.items.size()) {
      list.items[static_cast<std::size_t>(editor_.label_selected)] = value;
    }
    return;
  }
  if (event.kind == Kind::kCommand && event.widget == "LabelNew") {
    const core::sim::Point centre = editor_view_centre();
    const std::string name = item_label("<New label>", "");
    const std::string x = std::to_string(centre.x);
    const std::string y = std::to_string(centre.y);
    const core::XmlEdit attributes[] = {{"text", name}, {"x", x}, {"y", y}};
    bool found = false;
    text = core::xml_append_element(text, "root", "label", attributes, &found);
    if (!found) {
      std::printf("editor: %s has no root to lay a label in\n", labels_document().c_str());
      return;
    }
    editor_.label_selected = static_cast<std::int32_t>(count);
    fill_labels(pane);
    pane.focus("LabelText");
    return;
  }
  if (event.kind == Kind::kCommand && event.widget == "LabelDelete") {
    if (!have) return;
    text = core::xml_erase_element(text, at);
    if (static_cast<std::size_t>(editor_.label_selected) + 1 >= count) editor_.label_selected = -1;
    fill_labels(pane);
    return;
  }
  if (event.kind == Kind::kCommand && event.widget == "LabelPosSet") {
    if (have) open_place_label();
    return;
  }
}

/// *Place Label* (`MapPlace.ini`, opened at 0x004a0af0): the minimap in
/// the `BMP` button, the label's text over it, and a click puts the label
/// there. The picture is the zoom map's ground, fitted to the button; the
/// labels are drawn on it as the minimap draws them (0x0049f7a0: each at
/// its position scaled into the picture, centred on the point and kept
/// inside the edges, in `Fonts/tahoma16b.apf`), the one being placed in
/// the screen's own `Text` colour (yellow). **Readings, labelled:** the
/// other labels' colour is not read and is white here; whether the
/// original closes the screen on the click is not read, and it does here;
/// the `Building Minimap. Please wait.` text is not shown, the picture
/// being composed before the screen opens.
void Application::open_place_label() {
  if (session_ == nullptr || editor_.place_label != nullptr) return;
  std::uint32_t divisor = 16;
  if (const core::sim::MatchSystem* match = core::sim::match_system_of(session_->world()); match != nullptr) {
    const std::int32_t size = match->rules().map_size;
    if (size == 8192 || size == 16384 || size == 32768) divisor = static_cast<std::uint32_t>(size / 1024);
  }
  if (zoom_ground_.empty() || divisor != zoom_divisor_) {
    zoom_divisor_ = divisor;
    map_.compose_zoom(zoom_divisor_, zoom_ground_);
  }
  if (zoom_ground_.empty()) return;
  core::ui::Dialog* dialog = open_menu(
      "editorini/MapPlace.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        if (event.kind == Kind::kClick && event.widget == "BMP") {
          const core::ui::Rect& fitted = editor_.place_fitted;
          if (fitted.width <= 0 || fitted.height <= 0) return;
          if (event.x < fitted.x || event.y < fitted.y || event.x >= fitted.right() || event.y >= fitted.bottom()) return;
          const auto kDivisor = static_cast<std::int64_t>(zoom_divisor_);
          const std::int64_t gx = static_cast<std::int64_t>(event.x - fitted.x) * zoom_ground_.width / fitted.width;
          const std::int64_t gy = static_cast<std::int64_t>(event.y - fitted.y) * zoom_ground_.height / fitted.height;
          const auto x = static_cast<std::int32_t>(gx * kDivisor);
          const auto y = core::screen_to_world_y(static_cast<std::int32_t>(gy * kDivisor));
          std::string& text = labels_text();
          const std::string at = "root/label[" + std::to_string(std::max(0, editor_.label_selected)) + "]";
          const std::string sx = std::to_string(x);
          const std::string sy = std::to_string(y);
          const core::XmlEdit edits[] = {{"x", sx}, {"y", sy}};
          text = core::xml_set_attributes(text, at, edits);
          if (core::ui::Dialog* pane = explorer_pane(); pane != nullptr) fill_labels(*pane);
          close_place_label();
          return;
        }
        if (event.kind == Kind::kEscape || (event.kind == Kind::kCommand && event.id == 0x10082)) close_place_label();
        (void)menu;
      });
  if (dialog == nullptr) return;
  editor_.place_label = dialog;
  refresh_place_label(*dialog);
}

void Application::refresh_place_label(core::ui::Dialog& menu) {
  const core::ui::Rect rect = menu.widget_rect("BMP");
  if (rect.width <= 0 || rect.height <= 0) return;
  // The ground, fitted to the button and centred in it.
  std::int32_t width = rect.width;
  std::int32_t height = static_cast<std::int32_t>(static_cast<std::int64_t>(zoom_ground_.height) * rect.width / zoom_ground_.width);
  if (height > rect.height) {
    height = rect.height;
    width = static_cast<std::int32_t>(static_cast<std::int64_t>(zoom_ground_.width) * rect.height / zoom_ground_.height);
  }
  editor_.place_fitted = core::ui::Rect{(rect.width - width) / 2, (rect.height - height) / 2, width, height};
  core::ui::Canvas canvas(static_cast<std::uint32_t>(rect.width), static_cast<std::uint32_t>(rect.height));
  canvas.clear(core::ui::Color{0, 0, 0, 255});
  const core::ui::Rect all{0, 0, rect.width, rect.height};
  canvas.blit_scaled(zoom_ground_, editor_.place_fitted, all);
  // The labels over it, each centred on its point and kept inside.
  core::ui::ResourceCache fonts([this](std::string_view path) { return platform::as_core_bytes(vfs_.read(path)); });
  const core::ui::Font* font = fonts.font("Fonts/tahoma16b.apf");
  const std::string& text = labels_text();
  const std::size_t count = core::xml_count_elements(text, "root/label");
  const std::int32_t map_width = static_cast<std::int32_t>(zoom_ground_.width) * static_cast<std::int32_t>(zoom_divisor_);
  const std::int32_t map_height = static_cast<std::int32_t>(zoom_ground_.height) * static_cast<std::int32_t>(zoom_divisor_);
  for (std::size_t i = 0; font != nullptr && i < count; ++i) {
    const std::string at = "root/label[" + std::to_string(i) + "]";
    std::int32_t x = 0;
    std::int32_t y = 0;
    if (!core::parse_int(core::xml_get_attribute(text, at, "x"), x) ||
        !core::parse_int(core::xml_get_attribute(text, at, "y"), y)) {
      continue;
    }
    const std::string label = core::ui::cp1252_from_utf8(core::xml_decode_entities(core::xml_get_attribute(text, at, "text")));
    const std::int32_t px = editor_.place_fitted.x + static_cast<std::int32_t>(static_cast<std::int64_t>(x) * width / std::max(1, map_width));
    const std::int32_t py = editor_.place_fitted.y + static_cast<std::int32_t>(static_cast<std::int64_t>(core::world_to_screen_y(y)) * height / std::max(1, map_height));
    const std::int32_t text_width = font->measure(label);
    std::int32_t left = px - text_width / 2;
    std::int32_t top = py - font->height() / 2;
    left = std::clamp(left, 0, std::max(0, rect.width - text_width));
    top = std::clamp(top, 0, std::max(0, rect.height - font->height()));
    const bool placing = static_cast<std::int32_t>(i) == editor_.label_selected;
    const core::ui::Color ink = placing ? core::ui::Color{255, 255, 0, 255} : core::ui::Color{255, 255, 255, 255};
    canvas.draw_text(*font, label, left, top, ink, all);
  }
  editor_.place_picture.width = static_cast<std::uint32_t>(rect.width);
  editor_.place_picture.height = static_cast<std::uint32_t>(rect.height);
  editor_.place_picture.rgba.assign(canvas.pixels().begin(), canvas.pixels().end());
  menu.content().state("BMP").bitmap = &editor_.place_picture;
  menu.set_hidden("Text", true);
  menu.set_hidden("ErrText", true);
}

void Application::close_place_label() {
  if (editor_.place_label == nullptr) return;
  close_menu(editor_.place_label);
  editor_.place_label = nullptr;
}

/// The document a note lives in: the map's `Notes.xml` or the root's.
std::string Application::note_document_of(const std::string& id) const {
  for (const std::string document : {editor_map_prefix() + "Notes.xml", std::string("Notes.xml")}) {
    const auto found = editor_.documents.find(document);
    std::string text;
    if (found != editor_.documents.end()) {
      text = found->second;
    } else {
      const std::vector<std::byte> bytes = container_.read(document);
      text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    if (!core::xml_get_attribute(text, "notes/note[id=" + id + "]", "id").empty()) return document;
  }
  return std::string();
}

/// `AdvResources.ini`'s filter: which radio is pressed, 1..4.
std::int32_t Application::resources_filter(const core::ui::Dialog& pane) const {
  const char* names[] = {"All.Radio", "Strongholds.Radio", "Villages.Radio", "Outposts.Radio"};
  for (std::int32_t id = 1; id <= 4; ++id) {
    const core::ui::WidgetState* state = pane.content().state_of(names[id - 1]);
    if (state != nullptr && state->row == 1) return id;
  }
  return 1;
}

/// `AdvResources.ini` (the *Settlements* node): the settlements the filter
/// keeps, and the chosen one's gold, food and population. The `Level` edit
/// is shown and not wired: the settlement level is not modelled here.
void Application::fill_resources(core::ui::Dialog& pane, std::int32_t filter) {
  const core::sim::EconomySystem* economy = core::sim::economy_of(session_->world());
  if (economy == nullptr) return;
  std::vector<std::string> rows;
  std::vector<std::string> ids;
  for (const core::sim::Settlement& town : economy->settlements().all()) {
    const bool kept = filter == 1 || (filter == 2 && town.kind == core::sim::SettlementKind::stronghold) ||
                      (filter == 3 && town.kind == core::sim::SettlementKind::village) ||
                      (filter == 4 && town.kind == core::sim::SettlementKind::outpost);
    if (!kept) continue;
    rows.push_back(town.name);
    ids.push_back(std::to_string(town.id));
  }
  std::int32_t selected = pane.selected("List");
  pane.set_items("List", std::move(rows));
  pane.content().state("List").items_data = std::move(ids);
  const core::ui::WidgetState& list = pane.content().state("List");
  if (selected < 0 || static_cast<std::size_t>(selected) >= list.items.size()) selected = list.items.empty() ? -1 : 0;
  pane.select("List", selected);
  const core::sim::Settlement* town =
      selected >= 0 ? economy->settlements().find(static_cast<core::sim::SettlementId>(std::atoi(list.items_data[static_cast<std::size_t>(selected)].c_str())))
                    : nullptr;
  pane.set_text("Gold", town != nullptr ? std::to_string(town->gold()) : "");
  pane.set_text("Food", town != nullptr ? std::to_string(town->food()) : "");
  pane.set_text("Population", town != nullptr ? std::to_string(town->population) : "");
  pane.set_text("Level", "");
}

/// One of a settlement's numbers, in the world and in the record the
/// save writes (the anchor object's settlement element).
void Application::set_settlement_number(core::sim::SettlementId id, std::string_view field, std::int32_t value) {
  core::sim::EconomySystem* economy = core::sim::economy_of(session_->world());
  core::sim::Settlement* town = economy != nullptr ? economy->settlements().find(id) : nullptr;
  if (town == nullptr) return;
  PropertyEdits& edits = editor_.props[town->anchor];
  if (field == "Gold") {
    (void)economy->set_resource(id, core::sim::Resource::gold, value);
    edits.settlement_gold = value;
  } else if (field == "Food") {
    (void)economy->set_resource(id, core::sim::Resource::food, value);
    edits.settlement_food = value;
  } else {
    (void)economy->set_population(id, value);
    edits.settlement_population = value;
  }
}

/// `AdvItems.ini` on one of the container's own item types
/// (`itemsCustom.xml`'s `<item>`, with its `<bonus>` child): the General
/// tab's id, name, description, image (`itemicons.ini`), sound
/// (`itemsounds.ini`) and *Important*; the Wear-bonus tab's numbers, armor
/// standing for both `armor_slash` and `armor_pierce` as every shipped
/// element keeps them equal; the Use-script tab's count and script, with
/// Compile. Type, Amount, Healing, Invisibility and the target type name
/// no attribute of the document and are shown unwired.
void Application::fill_item_type(const std::string& id, core::ui::Dialog& pane) {
  const std::string& text = editor_document_text("itemsCustom.xml");
  const std::string at = "items/item[id=" + id + "]";
  pane.set_text("ItemIdEdit", id);
  pane.set_text("ItemNameEdit", core::xml_get_attribute(text, at, "name"));
  pane.set_text("DescrEdit", core::xml_get_attribute(text, at, "description"));
  const auto combo_from = [&](const char* widget, const char* file, const std::string& current) {
    std::vector<std::string> names;
    std::vector<std::string> paths;
    std::int32_t row = -1;
    if (const platform::ByteSpan bytes = vfs_.read(file); !bytes.empty()) {
      if (const core::Result<core::IniDocument> doc = core::IniDocument::parse(platform::as_core_bytes(bytes)); doc.ok()) {
        for (const core::IniEntry& entry : doc->entries_of(doc->section("FillCombo"))) {
          if (!entry.has_key) continue;
          if (fold_name(std::string(entry.value)) == fold_name(current)) row = static_cast<std::int32_t>(names.size());
          names.emplace_back(entry.key);
          paths.emplace_back(entry.value);
        }
      }
    }
    pane.set_items(widget, std::move(names));
    pane.select(widget, row);
    pane.content().state(widget).items_data = std::move(paths);
  };
  const std::string image = core::xml_get_attribute(text, at, "image");
  combo_from("ImageCombo", "DATA\\INTERFACE\\EDITOR\\COMBO\\ITEMICONS.INI", image);
  pane.content().state("IconPreview").image = image;
  combo_from("SoundCombo", "DATA\\INTERFACE\\EDITOR\\COMBO\\ITEMSOUNDS.INI", core::xml_get_attribute(text, at, "sound"));
  pane.set_row("ImportantBtn", core::xml_get_attribute(text, at, "important") == "yes" ? 1 : 0);
  const std::string bonus = at + "/bonus";
  pane.set_text("DamageEdit", core::xml_get_attribute(text, bonus, "damage"));
  pane.set_text("DamagePercentEdit", core::xml_get_attribute(text, bonus, "damage_percent"));
  pane.set_text("ArmorEdit", core::xml_get_attribute(text, bonus, "armor_slash"));
  pane.set_text("ArmorPercentEdit", core::xml_get_attribute(text, bonus, "armor_slash_percent"));
  pane.set_text("LevelEdit", core::xml_get_attribute(text, bonus, "level"));
  pane.set_text("ExpEdit", core::xml_get_attribute(text, bonus, "experience"));
  pane.set_text("HealthEdit", core::xml_get_attribute(text, bonus, "health"));
  pane.set_text("HealthPercentEdit", core::xml_get_attribute(text, bonus, "health_percent"));
  pane.set_text("UseCountEdit", core::xml_get_attribute(text, at, "usecount"));
  // The script is stored with `\n` as two characters and `\'`, `\l`, `\g`,
  // `\a` for `'`, `<`, `>`, `&` -- the shipped document's own escaping.
  pane.set_text("ScriptEdit", unescape_item_script(core::xml_get_attribute(text, at, "use_script")));
}

/// `use_script`'s escaping, as the shipped `itemsCustom.xml` carries it:
/// `\n` a line break, `\'` a quote, `\l` `<`, `\g` `>`, `\a` `&`.
/// **Reading, labelled:** inferred from the two campaign documents that
/// hold a script; the writer at the other end was not read.
std::string Application::unescape_item_script(std::string_view stored) {
  std::string out;
  for (std::size_t i = 0; i < stored.size(); ++i) {
    if (stored[i] == '\\' && i + 1 < stored.size()) {
      switch (stored[i + 1]) {
        case 'n': out.push_back('\n'); ++i; continue;
        case '\'': out.push_back('\''); ++i; continue;
        case 'l': out.push_back('<'); ++i; continue;
        case 'g': out.push_back('>'); ++i; continue;
        case 'a': out.push_back('&'); ++i; continue;
        default: break;
      }
    }
    out.push_back(stored[i]);
  }
  return out;
}

std::string Application::escape_item_script(std::string_view source) {
  std::string out;
  for (const char c : source) {
    switch (c) {
      case '\n': out += "\\n"; break;
      case '\r': break;
      case '\'': out += "\\'"; break;
      case '<': out += "\\l"; break;
      case '>': out += "\\g"; break;
      case '&': out += "\\a"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

/// The `.conv.xml` that declares `name`: the map's `Conversations/` first,
/// then the root's -- by path, never by suffix (the `Local/` copies share
/// it and are translation tables). Empty when none does.
std::string Application::conversation_document_of(const std::string& name) {
  const std::string map_prefix = fold_name(editor_map_prefix() + "conversations/");
  std::vector<std::string> candidates;
  for (const std::string& stored : container_.list()) {
    std::string folded = fold_name(stored);
    for (char& c : folded) if (c == '\\') c = '/';
    if (!folded.ends_with(".conv.xml") || folded.rfind("local/", 0) == 0) continue;
    if (folded.rfind(map_prefix, 0) == 0 || folded.rfind("conversations/", 0) == 0) candidates.push_back(stored);
  }
  for (const std::string& path : candidates) {
    if (core::xml_get_attribute(editor_document_text(path), "conversation", "name") == name) return path;
  }
  return std::string();
}

/// The phrases' rows and the current phrase's fields, from the document.
void Application::fill_conversation(const std::string& name, core::ui::Dialog& pane) {
  const std::string document = conversation_document_of(name);
  if (document.empty()) return;
  const std::string& text = editor_document_text(document);
  std::vector<std::string> rows;
  std::vector<std::string> actors;
  for (std::size_t i = 0;; ++i) {
    const std::string at = "conversation/actor[" + std::to_string(i) + "]";
    const std::string actor = core::xml_get_attribute(text, at, "name");
    if (actor.empty() && text.find("<actor") == std::string::npos) break;
    if (actor.empty()) break;
    actors.push_back(actor);
  }
  for (std::size_t i = 0;; ++i) {
    const std::string at = "conversation/phrase[" + std::to_string(i) + "]";
    const std::string followup = core::xml_get_attribute(text, at, "followup");
    const std::string line = core::xml_get_attribute(text, at, "text");
    if (followup.empty() && line.empty() && core::xml_get_attribute(text, at, "actor").empty()) break;
    std::string row = core::xml_get_attribute(text, at, "actor");
    if (!row.empty()) row += ": ";
    row += line.size() > 60 ? line.substr(0, 57) + "..." : line;
    rows.push_back(std::move(row));
  }
  std::int32_t selected = pane.selected("PhraseList");
  pane.set_items("PhraseList", std::move(rows));
  const core::ui::WidgetState& list = pane.content().state("PhraseList");
  if (selected < 0 || static_cast<std::size_t>(selected) >= list.items.size()) selected = list.items.empty() ? -1 : 0;
  pane.select("PhraseList", selected);
  // The phrase's own tab.
  const std::string at = "conversation/phrase[" + std::to_string(std::max(0, selected)) + "]";
  const bool have = selected >= 0;
  pane.set_text("PhraseLabelEdit", have ? core::xml_get_attribute(text, at, "label") : "");
  pane.set_text("PhraseBriefEdit", have ? core::xml_get_attribute(text, at, "choice_text") : "");
  pane.set_text("PhraseTextEdit", have ? core::xml_get_attribute(text, at, "text") : "");
  pane.set_text("PhraseFollowUpsEdit", have ? core::xml_get_attribute(text, at, "followup_phrases") : "");
  pane.set_text("CondScriptEdit", have ? core::xml_get_attribute(text, at, "condition") : "");
  pane.set_text("ActionScriptEdit", have ? core::xml_get_attribute(text, at, "action") : "");
  pane.set_text("ReturnScriptEdit", have ? core::xml_get_attribute(text, at, "return") : "");
  pane.set_text("CommentsEdit", have ? core::xml_get_attribute(text, at, "comments") : "");
  const std::string actor = have ? core::xml_get_attribute(text, at, "actor") : "";
  std::int32_t actor_row = -1;
  for (std::size_t i = 0; i < actors.size(); ++i) {
    if (actors[i] == actor) actor_row = static_cast<std::int32_t>(i);
  }
  pane.set_items("PhraseActorCombo", actors);
  if (actor_row >= 0) pane.select("PhraseActorCombo", actor_row);
  else pane.set_text("PhraseActorCombo", actor);
  static const char* const kModes[] = {"choice", "random", "first", "end", "cycle", "cycle then first", "cycle then random"};
  std::vector<std::string> modes(std::begin(kModes), std::end(kModes));
  const auto mode_row = [&](const std::string& mode) {
    for (std::size_t i = 0; i < modes.size(); ++i) {
      if (modes[i] == (mode.empty() ? "first" : mode)) return static_cast<std::int32_t>(i);
    }
    return 2;
  };
  pane.set_items("PhraseFollowUpCombo", modes);
  pane.select("PhraseFollowUpCombo", mode_row(have ? core::xml_get_attribute(text, at, "followup") : "first"));
  // The conversation's own tab.
  pane.set_text("ConvLabelEdit", name);
  pane.set_items("StartingPhraseCombo", modes);
  pane.select("StartingPhraseCombo", mode_row(core::xml_get_attribute(text, "conversation", "startup")));
  pane.set_text("StartingPhraseEdit", core::xml_get_attribute(text, "conversation", "startup_phrases"));
  pane.set_text("CCommentsEdit", core::xml_get_attribute(text, "conversation", "comments"));
  pane.set_row("ReturnCheckbox", core::xml_get_attribute(text, "conversation", "restore_view") == "1" ? 1 : 0);
}

/// `AdvConverse.ini`'s three tabs over one `.conv.xml`: the phrases
/// (new, delete, up, down), the chosen phrase's fields, and the
/// conversation's own. Every edit patches the document; the actor combo
/// takes a typed role as well as a declared one; Compile parses each of
/// the phrase's three scripts with this engine's compiler and reports the
/// first that fails.
void Application::conversation_event(const std::string& name, const core::ui::DialogEvent& event,
                                     core::ui::Dialog& pane) {
  using Kind = core::ui::DialogEvent::Kind;
  const std::string document = conversation_document_of(name);
  if (document.empty()) return;
  const auto patch = [&](std::string_view at, std::string_view attribute, std::string value) {
    std::string& text = editor_document_text(document);
    const core::XmlEdit edit[] = {{attribute, value}};
    text = core::xml_set_attributes(text, at, edit);
  };
  const std::int32_t row = pane.selected("PhraseList");
  const std::string at = "conversation/phrase[" + std::to_string(std::max(0, row)) + "]";
  const auto edited = [&](const char* widget) { return event.kind == Kind::kChange && event.widget == widget; };
  const auto chosen = [&](const char* widget) -> std::string {
    const std::int32_t index = pane.selected(widget);
    const core::ui::WidgetState* state = pane.content().state_of(widget);
    if (index < 0 || state == nullptr || static_cast<std::size_t>(index) >= state->items.size()) return pane.text(widget);
    return state->items[static_cast<std::size_t>(index)];
  };
  if (event.kind == Kind::kSelect && event.widget == "PhraseList") {
    fill_conversation(name, pane);
    return;
  }
  if (event.kind == Kind::kCommand) {
    std::string& text = editor_document_text(document);
    switch (event.id) {
      case 0x01007001:    // New phrase
      case 0x2008002: {   // and the phrase tab's own
        const core::XmlEdit fresh[] = {{"actor", ""}, {"text", ""}, {"followup", "first"}};
        text = core::xml_append_element(text, "conversation", "phrase", fresh);
        fill_conversation(name, pane);
        const core::ui::WidgetState& list = pane.content().state("PhraseList");
        pane.select("PhraseList", static_cast<std::int32_t>(list.items.size()) - 1);
        fill_conversation(name, pane);
        return;
      }
      case 0x1007002:  // Delete phrase
        if (row < 0) return;
        text = core::xml_erase_element(text, at);
        pane.select("PhraseList", -1);
        fill_conversation(name, pane);
        return;
      case 0x01005003:    // up
      case 0x01005004:    // down
      case 0x2008000:     // previous
      case 0x2008001: {   // next
        if (row < 0) return;
        const std::int32_t other = row + (event.id == 0x01005003 || event.id == 0x2008000 ? -1 : 1);
        const core::ui::WidgetState& list = pane.content().state("PhraseList");
        if (other < 0 || static_cast<std::size_t>(other) >= list.items.size()) return;
        if (event.id == 0x01005003 || event.id == 0x01005004) {
          text = core::xml_swap_elements(text, at, "conversation/phrase[" + std::to_string(other) + "]");
        }
        pane.select("PhraseList", other);
        fill_conversation(name, pane);
        return;
      }
      case 0x200701c: {  // Compile: the three scripts of the phrase
        for (const char* widget : {"CondScriptEdit", "ActionScriptEdit", "ReturnScriptEdit"}) {
          const std::string source = pane.text(widget);
          if (source.empty()) continue;
          const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(source.data()), source.size()};
          core::script::Diagnostic diagnostic;
          const core::Result<core::script::Script> parsed = core::script::parse(bytes, widget, &diagnostic);
          if (!parsed.ok()) {
            std::printf("editor: %s %s: line %u: %.*s\n", name.c_str(), widget, diagnostic.line,
                        static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
            return;
          }
          core::script::CompileError error;
          if (!core::script::compile(parsed.value(), &registry_, &error).ok()) {
            std::printf("editor: %s %s: line %u: %s\n", name.c_str(), widget, error.line, error.message.c_str());
            return;
          }
        }
        std::printf("editor: %s compiles\n", name.c_str());
        return;
      }
      case 0x3000901:  // restore the view afterwards
        patch("conversation", "restore_view", pane.content().state("ReturnCheckbox").row == 1 ? "1" : "0");
        return;
      case 0x3007003: {  // Delete conversation: the document goes empty and the node with it
        text = "\t<conversation\r\n\t\tname=\"\"\r\n\t\tstartup=\"first\"\r\n\t\trestore_view=\"0\">\r\n\t</conversation>\r\n";
        close_explorer_pane();
        build_explorer_tree();
        refresh_explorer();
        return;
      }
      default:
        return;
    }
  }
  if (event.kind != Kind::kChange) return;
  if (edited("ConvLabelEdit")) {
    patch("conversation", "name", pane.text("ConvLabelEdit"));
    return;
  }
  if (edited("StartingPhraseCombo")) patch("conversation", "startup", chosen("StartingPhraseCombo"));
  if (edited("StartingPhraseEdit")) patch("conversation", "startup_phrases", pane.text("StartingPhraseEdit"));
  if (edited("CCommentsEdit")) patch("conversation", "comments", pane.text("CCommentsEdit"));
  if (row < 0) return;
  if (edited("PhraseLabelEdit")) patch(at, "label", pane.text("PhraseLabelEdit"));
  if (edited("PhraseActorCombo")) patch(at, "actor", chosen("PhraseActorCombo"));
  if (edited("PhraseBriefEdit")) patch(at, "choice_text", pane.text("PhraseBriefEdit"));
  if (edited("PhraseTextEdit")) {
    patch(at, "text", pane.text("PhraseTextEdit"));
    // The list's row follows the line.
    const std::int32_t keep = row;
    fill_conversation(name, pane);
    pane.select("PhraseList", keep);
  }
  if (edited("PhraseFollowUpCombo")) patch(at, "followup", chosen("PhraseFollowUpCombo"));
  if (edited("PhraseFollowUpsEdit")) patch(at, "followup_phrases", pane.text("PhraseFollowUpsEdit"));
  if (edited("CondScriptEdit")) patch(at, "condition", pane.text("CondScriptEdit"));
  if (edited("ActionScriptEdit")) patch(at, "action", pane.text("ActionScriptEdit"));
  if (edited("ReturnScriptEdit")) patch(at, "return", pane.text("ReturnScriptEdit"));
  if (edited("CommentsEdit")) patch(at, "comments", pane.text("CommentsEdit"));
}

/// `AdvDiplomacy.ini`: sixteen rows of a colour, a name button and six
/// toggles, showing the current player's word toward each (the fill at
/// 0x00465c60): CF bit 0, SV bit 4, SS bit 2, SC bit 5, MR whether the two
/// directions agree, SVic bit 1 (shown; what sets it was not read). The
/// current player's own row is disabled.
void Application::fill_diplomacy(core::ui::Dialog& pane) {
  const core::sim::PlayerTable& players = session_->world().players();
  const core::PlayerId from = editor_.diplomacy_player;
  for (std::size_t q = 0; q < core::sim::kPlayerCount; ++q) {
    const auto to = static_cast<core::PlayerId>(q);
    const std::string prefix = "Player" + std::to_string(q + 1) + ".";
    const core::sim::PlayerSetup& setup = players.setup(to);
    pane.set_text(prefix + "Name", setup.name.empty() ? "Player " + std::to_string(q + 1) : setup.name);
    const auto five = [](std::uint32_t v) { return static_cast<std::uint8_t>((v << 3) | (v >> 2)); };
    core::ui::WidgetState& square = pane.content().state(prefix + "Color");
    square.has_color = true;
    square.color = core::ui::Color{five((setup.colour >> 10) & 31), five((setup.colour >> 5) & 31), five(setup.colour & 31), 255};
    const std::uint32_t word = players.relation_word(from, to);
    pane.set_row(prefix + "CF", (word & 0x01u) != 0 ? 1 : 0);
    pane.set_row(prefix + "SV", (word & 0x10u) != 0 ? 1 : 0);
    pane.set_row(prefix + "SS", (word & 0x04u) != 0 ? 1 : 0);
    pane.set_row(prefix + "SC", (word & 0x20u) != 0 ? 1 : 0);
    pane.set_row(prefix + "MR", word == players.relation_word(to, from) ? 1 : 0);
    pane.set_row(prefix + "SVic", (word & 0x02u) != 0 ? 1 : 0);
    for (const char* control : {"CF", "SV", "SS", "SC", "MR", "SVic"}) pane.set_enabled(prefix + control, to != from);
    pane.set_row(prefix + "Name", to == from ? 1 : 0);
  }
}

void Application::explorer_pane_event(ExplorerNode& node, const core::ui::DialogEvent& event,
                                      core::ui::Dialog& pane) {
  using Kind = core::ui::DialogEvent::Kind;
  const std::string& path = pane.screen().path;
  const auto has = [&](const char* name) { return path.find(name) != std::string::npos; };
  const auto patch = [&](const std::string& document, std::string_view at, std::string_view attribute, std::string value) {
    std::string& text = editor_document_text(document);
    const core::XmlEdit edit[] = {{attribute, value}};
    bool found = false;
    text = core::xml_set_attributes(text, at, edit, &found);
    if (!found) std::printf("editor: %s has no %.*s to write %.*s into\n", document.c_str(),
                            static_cast<int>(at.size()), at.data(), static_cast<int>(attribute.size()), attribute.data());
  };
  const auto checked = [&](const char* name) {
    const core::ui::WidgetState* state = pane.content().state_of(name);
    return state != nullptr && state->row == 1;
  };
  const auto chosen = [&](const char* name) -> std::string {
    const std::int32_t row = pane.selected(name);
    const core::ui::WidgetState* state = pane.content().state_of(name);
    if (row < 0 || state == nullptr || static_cast<std::size_t>(row) >= state->items.size()) return std::string();
    return state->items[static_cast<std::size_t>(row)];
  };
  if (has("AdvPlayer")) {
    const std::string document = "player" + node.key + ".xml";
    if (event.kind == Kind::kChange && event.widget == "Name.Edit") patch(document, "playerdata", "name", pane.text("Name.Edit"));
    if (event.kind == Kind::kChange && event.widget == "RaceCombo") patch(document, "playerdata", "race", chosen("RaceCombo"));
    if (event.kind == Kind::kChange && event.widget == "ControlCombo") patch(document, "playerdata", "control", chosen("ControlCombo"));
    if (event.kind == Kind::kChange && event.widget == "AICombo") {
      // The row's profile and difficulty, as 0x00475884 reads them off the
      // row's data; *None* is an empty profile at difficulty 0.
      const std::int32_t row = pane.selected("AICombo");
      const core::ui::WidgetState* state = pane.content().state_of("AICombo");
      std::string profile;
      std::string difficulty = "0";
      if (row > 0 && state != nullptr && static_cast<std::size_t>(row) < state->items_data.size()) {
        const std::string& data = state->items_data[static_cast<std::size_t>(row)];
        const std::size_t cut = data.find('\n');
        profile = data.substr(0, cut);
        if (cut != std::string::npos) difficulty = data.substr(cut + 1);
      }
      patch(document, "playerdata", "AI", profile);
      patch(document, "playerdata", "difficulty", difficulty);
    }
    if (event.kind == Kind::kCommand && event.id == 0x1003) {
      const core::sim::Point at = editor_view_centre();
      patch(document, "playerdata", "startx", std::to_string(at.x));
      patch(document, "playerdata", "starty", std::to_string(at.y));
      pane.set_text("Start.Text", "Start: " + std::to_string(at.x) + "," + std::to_string(at.y));
    }
    if (event.kind == Kind::kCommand && event.id == 0x1004) {
      const std::string& text = editor_document_text(document);
      std::int32_t x = 0;
      std::int32_t y = 0;
      if (core::parse_int(core::xml_get_attribute(text, "playerdata", "startx"), x) &&
          core::parse_int(core::xml_get_attribute(text, "playerdata", "starty"), y)) {
        look_x_ = x;
        look_y_ = y;
        look_pending_ = true;
      }
    }
    return;
  }
  if (has("AdvScenario") || has("AdvAdventure")) {
    const std::string map = editor_map_prefix() + "map.xml";
    if (has("AdvScenario")) labels_event(event, pane);
    if (has("AdvAdventure")) maps_event(event, pane);
    if (event.kind == Kind::kChange && event.widget == "NameEdit") patch("game.xml", "game/properties", "name", pane.text("NameEdit"));
    if (event.kind == Kind::kChange && event.widget == "AuthorEdit") patch("game.xml", "game/properties", "author", pane.text("AuthorEdit"));
    if (event.kind == Kind::kChange && event.widget == "DescrEdit") patch("game.xml", "game/properties", "description", pane.text("DescrEdit"));
    if (event.kind == Kind::kChange && event.widget == "VictoryCondCombo") {
      // Written as the setup reads it: the index, then the display name
      // (`"1 Elimination"`), or a bare `0` for none.
      const std::int32_t row = pane.selected("VictoryCondCombo");
      patch("game.xml", "game/properties", "victory_condition", row <= 0 ? std::string("0") : std::to_string(row) + " " + chosen("VictoryCondCombo"));
    }
    if (event.kind == Kind::kChange && event.widget == "VictoryThresCB") patch("game.xml", "game/properties", "victory_threshold", pane.text("VictoryThresCB"));
    if (event.kind == Kind::kChange && event.widget == "PlayerCombo") patch("game.xml", "game/properties", "start_player", std::to_string(std::max(0, pane.selected("PlayerCombo"))));
    if (event.kind == Kind::kChange && event.widget == "RaceCombo") patch("game.xml", "game/properties", "user_interface", std::to_string(std::max(0, pane.selected("RaceCombo"))));
    if (event.kind == Kind::kChange && event.widget == "MiniMapEdit") patch(map, "map/user_art", "explored", pane.text("MiniMapEdit"));
    if (event.kind == Kind::kCommand && event.id == 0x100700F) patch("game.xml", "game/properties", "single_only", checked("SinglePlayerOnlyBtn") ? "1" : "0");
    if (event.kind == Kind::kCommand && event.id == 0x1007011) patch(map, "map/expl", "NoFog", checked("NoFogBtn") ? "1" : "0");
    if (event.kind == Kind::kCommand && event.id == 0x1007013) patch(map, "map/expl", "NoExplore", checked("ExploreBtn") ? "1" : "0");
    return;
  }
  if (has("AdvCurMap")) {
    const std::string map = editor_map_prefix() + "map.xml";
    labels_event(event, pane);
    maps_event(event, pane);
    if (event.kind == Kind::kChange && event.widget == "NameEdit") patch(map, "map", "name", pane.text("NameEdit"));
    if (event.kind == Kind::kChange && event.widget == "DispNameEdit") patch(map, "map", "displayname", pane.text("DispNameEdit"));
    if (event.kind == Kind::kChange && event.widget == "DescrEdit") patch(map, "map", "descr", pane.text("DescrEdit"));
    if (event.kind == Kind::kCommand && event.id == 0x100700A) patch(map, "map/expl", "NoFog", checked("NoFogBtn") ? "1" : "0");
    if (event.kind == Kind::kCommand && event.id == 0x100700C) patch(map, "map/expl", "NoExplore", checked("ExploreBtn") ? "1" : "0");
    if (event.kind == Kind::kCommand && event.id == 0x1007012) patch(map, "map", "persist_state", checked("PersistStateBtn") ? "1" : "0");
    if (event.kind == Kind::kCommand && event.id == 0x1007010) {
      const core::sim::Point at = editor_view_centre();
      patch(map, "map/start_pt", "x", std::to_string(at.x));
      patch(map, "map/start_pt", "y", std::to_string(at.y));
      pane.set_text("Start.Text", "Start: " + std::to_string(at.x) + "," + std::to_string(at.y));
    }
    if (event.kind == Kind::kCommand && event.id == 0x1007011) {
      const std::string& text = editor_document_text(map);
      std::int32_t x = 0;
      std::int32_t y = 0;
      if (core::parse_int(core::xml_get_attribute(text, "map/start_pt", "x"), x) &&
          core::parse_int(core::xml_get_attribute(text, "map/start_pt", "y"), y)) {
        look_x_ = x;
        look_y_ = y;
        look_pending_ = true;
      }
    }
    return;
  }
  if (has("AdvNotes")) {
    const std::string document = note_document_of(node.key);
    if (document.empty()) return;
    const std::string at = "notes/note[id=" + node.key + "]";
    if (event.kind == Kind::kChange && event.widget == "NameEdit") {
      // The id, renamed: the element's, the node's, and the tree's row.
      const std::string renamed = pane.text("NameEdit");
      if (renamed.empty() || renamed == node.key || !note_document_of(renamed).empty()) return;
      patch(document, at, "id", renamed);
      node.key = renamed;
      build_explorer_tree();
      for (std::size_t i = 0; i < editor_.explorer_nodes.size(); ++i) {
        if (editor_.explorer_nodes[i].kind == ExplorerKind::kPane && editor_.explorer_nodes[i].key == renamed &&
            fold_name(editor_.explorer_nodes[i].pane).find("advnotes") != std::string::npos) {
          editor_.explorer_chosen = static_cast<std::int32_t>(i);
          editor_.explorer_nodes[static_cast<std::size_t>(editor_.explorer_nodes[i].parent)].expanded = true;
        }
      }
      refresh_explorer();
      return;
    }
    if (event.kind == Kind::kChange && event.widget == "TitleEdit") patch(document, at, "title", pane.text("TitleEdit"));
    if (event.kind == Kind::kChange && event.widget == "DescrEdit") patch(document, at, "text", pane.text("DescrEdit"));
    if (event.kind == Kind::kChange && event.widget == "LocMapCombo") patch(document, at, "map", chosen("LocMapCombo"));
    if (event.kind == Kind::kChange && event.widget == "IconCombo") {
      const std::int32_t row = pane.selected("IconCombo");
      const core::ui::WidgetState* state = pane.content().state_of("IconCombo");
      if (row >= 0 && state != nullptr && static_cast<std::size_t>(row) < state->items_data.size()) {
        patch(document, at, "icon", state->items_data[static_cast<std::size_t>(row)]);
        pane.content().state("IconPreview").image = state->items_data[static_cast<std::size_t>(row)];
      }
    }
    if (event.kind == Kind::kCommand && event.id == 0x600F) patch(document, at, "show_on_minimap", checked("ShowIconCheck") ? "1" : "0");
    if (event.kind == Kind::kCommand && (event.id == 0x6006 || event.id == 0x6008)) {
      const core::sim::Point pt = event.id == 0x6006 ? editor_view_centre() : core::sim::Point{-1, -1};
      patch(document, at, "locationx", std::to_string(pt.x));
      patch(document, at, "locationy", std::to_string(pt.y));
      pane.set_text("PosEdit", std::to_string(pt.x) + "," + std::to_string(pt.y));
    }
    if (event.kind == Kind::kCommand && event.id == 0x6007) {
      const std::string& text = editor_document_text(document);
      std::int32_t x = 0;
      std::int32_t y = 0;
      if (core::parse_int(core::xml_get_attribute(text, at, "locationx"), x) &&
          core::parse_int(core::xml_get_attribute(text, at, "locationy"), y) && x >= 0) {
        look_x_ = x;
        look_y_ = y;
        look_pending_ = true;
      }
    }
    if (event.kind == Kind::kCommand && event.id == 0x6009) {
      std::string& text = editor_document_text(document);
      text = core::xml_erase_element(text, at);
      close_explorer_pane();
      build_explorer_tree();
      refresh_explorer();
    }
    return;
  }
  if (has("AdvConverse")) {
    conversation_event(node.key, event, pane);
    return;
  }
  if (has("AdvItems")) {
    const std::string at = "items/item[id=" + node.key + "]";
    const std::string bonus = at + "/bonus";
    const auto edited = [&](const char* widget) { return event.kind == Kind::kChange && event.widget == widget; };
    if (edited("ItemIdEdit")) {
      // The id, renamed -- the screen keeps an `ItemIdBackDisabled` for
      // the id of an item already made, which this engine does not draw:
      // **reading, labelled**, the id is editable here as the name is.
      const std::string renamed = pane.text("ItemIdEdit");
      if (renamed.empty() || renamed == node.key ||
          !core::xml_get_attribute(editor_document_text("itemsCustom.xml"), "items/item[id=" + renamed + "]", "id").empty()) {
        return;
      }
      patch("itemsCustom.xml", at, "id", renamed);
      node.key = renamed;
      build_explorer_tree();
      for (std::size_t i = 0; i < editor_.explorer_nodes.size(); ++i) {
        if (editor_.explorer_nodes[i].kind == ExplorerKind::kPane && editor_.explorer_nodes[i].key == renamed &&
            fold_name(editor_.explorer_nodes[i].pane).find("advitems") != std::string::npos) {
          editor_.explorer_chosen = static_cast<std::int32_t>(i);
          editor_.explorer_nodes[static_cast<std::size_t>(editor_.explorer_nodes[i].parent)].expanded = true;
        }
      }
      refresh_explorer();
      return;
    }
    if (edited("ItemNameEdit")) patch("itemsCustom.xml", at, "name", pane.text("ItemNameEdit"));
    if (edited("DescrEdit")) patch("itemsCustom.xml", at, "description", pane.text("DescrEdit"));
    if (edited("ImageCombo") || edited("SoundCombo")) {
      const std::int32_t row = pane.selected(event.widget);
      const core::ui::WidgetState* state = pane.content().state_of(event.widget);
      if (row >= 0 && state != nullptr && static_cast<std::size_t>(row) < state->items_data.size()) {
        const std::string& path = state->items_data[static_cast<std::size_t>(row)];
        patch("itemsCustom.xml", at, event.widget == "ImageCombo" ? "image" : "sound", path);
        if (event.widget == "ImageCombo") pane.content().state("IconPreview").image = path;
      }
    }
    if (event.kind == Kind::kCommand && event.id == 0x1000605) patch("itemsCustom.xml", at, "important", checked("ImportantBtn") ? "yes" : "no");
    const std::pair<const char*, const char*> numbers[] = {
        {"DamageEdit", "damage"},   {"DamagePercentEdit", "damage_percent"}, {"LevelEdit", "level"},
        {"ExpEdit", "experience"},  {"HealthEdit", "health"},                {"HealthPercentEdit", "health_percent"}};
    for (const auto& [widget, attribute] : numbers) {
      if (edited(widget)) patch("itemsCustom.xml", bonus, attribute, pane.text(widget));
    }
    if (edited("ArmorEdit")) {
      patch("itemsCustom.xml", bonus, "armor_slash", pane.text("ArmorEdit"));
      patch("itemsCustom.xml", bonus, "armor_pierce", pane.text("ArmorEdit"));
    }
    if (edited("ArmorPercentEdit")) {
      patch("itemsCustom.xml", bonus, "armor_slash_percent", pane.text("ArmorPercentEdit"));
      patch("itemsCustom.xml", bonus, "armor_pierce_percent", pane.text("ArmorPercentEdit"));
    }
    if (edited("UseCountEdit")) patch("itemsCustom.xml", at, "usecount", pane.text("UseCountEdit"));
    if (edited("ScriptEdit")) patch("itemsCustom.xml", at, "use_script", escape_item_script(pane.text("ScriptEdit")));
    if (event.kind == Kind::kCommand && event.widget == "CompileBtn") {
      const std::string source = pane.text("ScriptEdit");
      const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(source.data()), source.size()};
      core::script::Diagnostic diagnostic;
      const core::Result<core::script::Script> parsed = core::script::parse(bytes, node.key, &diagnostic);
      if (!parsed.ok()) {
        std::printf("editor: %s: line %u: %.*s\n", node.key.c_str(), diagnostic.line, static_cast<int>(diagnostic.message.size()), diagnostic.message.data());
        return;
      }
      core::script::CompileError error;
      if (!core::script::compile(parsed.value(), &registry_, &error).ok()) {
        std::printf("editor: %s: line %u: %s\n", node.key.c_str(), error.line, error.message.c_str());
        return;
      }
      std::printf("editor: %s compiles\n", node.key.c_str());
    }
    if (event.kind == Kind::kCommand && event.id == 0x1000607) {
      std::string& text = editor_document_text("itemsCustom.xml");
      text = core::xml_erase_element(text, at);
      close_explorer_pane();
      build_explorer_tree();
      for (ExplorerNode& branch : editor_.explorer_nodes) {
        if (branch.parent < 0 && branch.label == item_label("Item types", "")) branch.expanded = true;
      }
      refresh_explorer();
    }
    return;
  }
  if (has("AdvItemHolder")) {
    const auto id = static_cast<core::ObjectId>(std::atoi(node.key.c_str()));
    core::sim::World& world = session_->world();
    if (world.find(id) == nullptr) return;
    if (event.kind == Kind::kChange && event.widget == "NameEdit") {
      const std::string name = pane.text("NameEdit");
      if (!name.empty()) (void)world.named_objects().rebind(name, id);
      editor_.props[id].script_name = name;
      return;
    }
    if (event.kind == Kind::kChange && event.widget.rfind("Slot", 0) == 0) {
      core::sim::HeroSystem* heroes = core::sim::hero_system_of(world);
      if (heroes == nullptr) return;
      std::vector<std::string> wanted;
      for (std::size_t slot = 0; slot < 4; ++slot) {
        const std::string name = "Slot" + std::to_string(slot + 1) + "Combo";
        const std::int32_t row = pane.selected(name);
        const core::ui::WidgetState* state = pane.content().state_of(name);
        if (row > 0 && state != nullptr && static_cast<std::size_t>(row) < state->items.size()) wanted.push_back(state->items[static_cast<std::size_t>(row)]);
      }
      std::vector<core::ObjectId> held;
      heroes->items().contents_for(id, held);
      for (const core::ObjectId item : held) (void)heroes->items().remove(world, item);
      for (const std::string& item : wanted) (void)heroes->items().add(world, id, item);
      editor_.props[id].items = wanted;
      return;
    }
    if (event.kind == Kind::kCommand && event.id == 0x5008) {
      if (const core::sim::WorldObject* object = world.find(id)) {
        look_x_ = object->state.position.x;
        look_y_ = object->state.position.y;
        look_pending_ = true;
      }
      return;
    }
    if (event.kind == Kind::kCommand && event.id == 0x5009) {
      editor_.selected = id;
      editor_delete();
      close_explorer_pane();
      build_explorer_tree();
      refresh_explorer();
      return;
    }
    return;
  }
  if (has("AdvResources")) {
    // The filter radios (ids 1..4, a group the file does not mark), the
    // settlements' list, and the four numbers of the one chosen, with
    // *Equalize* writing a number to every settlement listed.
    if (event.kind == Kind::kCommand && event.id >= 1 && event.id <= 4) {
      for (std::int32_t id = 1; id <= 4; ++id) {
        const char* names[] = {"All.Radio", "Strongholds.Radio", "Villages.Radio", "Outposts.Radio"};
        pane.set_row(names[id - 1], id == event.id ? 1 : 0);
      }
      fill_resources(pane, event.id);
      return;
    }
    if (event.kind == Kind::kSelect && event.widget == "List") {
      fill_resources(pane, resources_filter(pane));
      return;
    }
    const auto listed = [&](std::vector<core::sim::SettlementId>& out) {
      const core::ui::WidgetState* state = pane.content().state_of("List");
      if (state == nullptr) return;
      for (const std::string& datum : state->items_data) out.push_back(static_cast<core::sim::SettlementId>(std::atoi(datum.c_str())));
    };
    const auto current = [&]() -> core::sim::SettlementId {
      const core::ui::WidgetState* state = pane.content().state_of("List");
      const std::int32_t row = pane.selected("List");
      if (state == nullptr || row < 0 || static_cast<std::size_t>(row) >= state->items_data.size()) return core::sim::kNoSettlement;
      return static_cast<core::sim::SettlementId>(std::atoi(state->items_data[static_cast<std::size_t>(row)].c_str()));
    };
    std::vector<core::sim::SettlementId> targets;
    const char* field = nullptr;
    if (event.kind == Kind::kChange && (event.widget == "Gold" || event.widget == "Food" || event.widget == "Population")) {
      field = event.widget == "Gold" ? "Gold" : event.widget == "Food" ? "Food" : "Population";
      if (const core::sim::SettlementId id = current(); id != core::sim::kNoSettlement) targets.push_back(id);
    } else if (event.kind == Kind::kCommand && (event.id == 7 || event.id == 9 || event.id == 13)) {
      field = event.id == 7 ? "Gold" : event.id == 9 ? "Food" : "Population";
      listed(targets);
    } else {
      return;  // `Level` and its Equalize: the settlement level is not modelled here
    }
    std::int32_t value = 0;
    if (!core::parse_int(pane.text(field), value)) return;
    for (const core::sim::SettlementId id : targets) set_settlement_number(id, field, value);
    if (event.kind == Kind::kCommand) fill_resources(pane, resources_filter(pane));
    return;
  }
  if (has("AdvOutpost")) {
    const auto id = static_cast<core::sim::SettlementId>(std::atoi(node.key.c_str()));
    if (event.kind == Kind::kChange && event.widget == "GoldEdit") {
      std::int32_t value = 0;
      if (core::parse_int(pane.text("GoldEdit"), value)) set_settlement_number(id, "Gold", value);
    } else if (event.kind == Kind::kChange && event.widget == "FoodEdit") {
      std::int32_t value = 0;
      if (core::parse_int(pane.text("FoodEdit"), value)) set_settlement_number(id, "Food", value);
    } else if (event.kind == Kind::kChange && event.widget == "NameEdit") {
      core::sim::EconomySystem* economy = core::sim::economy_of(session_->world());
      core::sim::Settlement* town = economy != nullptr ? economy->settlements().find(id) : nullptr;
      if (town == nullptr) return;
      town->name = pane.text("NameEdit");
      editor_.props[town->anchor].settlement_name = town->name;
    } else if (event.kind == Kind::kChange && event.widget == "PlayerCombo") {
      const std::int32_t row = pane.selected("PlayerCombo");
      core::sim::EconomySystem* economy = core::sim::economy_of(session_->world());
      core::sim::Settlement* town = economy != nullptr ? economy->settlements().find(id) : nullptr;
      if (town == nullptr || row < 0) return;
      (void)economy->set_owner(id, static_cast<core::PlayerId>(row));
      editor_.props[town->anchor].settlement_player = row + 1;
    } else if (event.kind == Kind::kCommand && event.id == 0x156) {
      const core::sim::EconomySystem* economy = core::sim::economy_of(session_->world());
      const core::sim::Settlement* town = economy != nullptr ? economy->settlements().find(id) : nullptr;
      if (const core::sim::WorldObject* object = town != nullptr ? session_->world().find(town->anchor) : nullptr) {
        look_x_ = object->state.position.x;
        look_y_ = object->state.position.y;
        look_pending_ = true;
      }
    }
    return;
  }
  if (has("AdvDiplomacy")) {
    // `0x1pn`: player p's row, control n (the file says so itself). The
    // name button makes p the player whose relations the rows show
    // (0x00465c60); the toggles edit p's word toward the row's player
    // (0x0046636b): CF sets bit 0 or clears the four (`& ~0x35`) and
    // unpresses SV, SS and SC; SV, SS and SC set their bit with bit 0
    // (`0x11`, `0x05`, `0x21`) or clear it. MR shows whether the two
    // directions agree and takes no press; SVic's press reaches nothing
    // in the handler.
    if (event.kind != Kind::kCommand || (event.id & 0xf00) != 0x100) return;
    const auto row = static_cast<core::PlayerId>((event.id >> 4) & 0xf);
    const std::int32_t control = event.id & 0xf;
    core::sim::PlayerTable& players = session_->world().players();
    if (control == 1) {
      editor_.diplomacy_player = row;
      fill_diplomacy(pane);
      return;
    }
    const core::PlayerId from = editor_.diplomacy_player;
    if (row == from || control < 2 || control > 5) {
      fill_diplomacy(pane);
      return;
    }
    std::uint32_t word = players.relation_word(from, row);
    const std::uint32_t bit = control == 2 ? 0x01u : control == 3 ? 0x10u : control == 4 ? 0x04u : 0x20u;
    if ((word & bit) == 0) {
      word |= bit | 0x01u;
    } else if (control == 2) {
      word &= ~0x35u;
    } else {
      word &= ~bit;
    }
    players.set_relation_word(from, row, word);
    // `player<p>.xml`'s `relations`: sixteen words of eight hex digits.
    std::string hex;
    for (std::size_t to = 0; to < core::sim::kPlayerCount; ++to) {
      char digits[9];
      std::snprintf(digits, sizeof digits, "%08X", players.relation_word(from, static_cast<core::PlayerId>(to)));
      hex += digits;
    }
    patch("player" + std::to_string(from) + ".xml", "playerdata", "relations", hex);
    fill_diplomacy(pane);
    return;
  }
  if (has("AdvSequence")) {
    std::string document = editor_map_prefix() + "Sequences/sequences.xml";
    const std::string at = "sequences/sequence[name=" + node.key + "]";
    if (core::xml_get_attribute(editor_document_text(document), at, "script").empty()) document = "Sequences/sequences.xml";
    if (event.kind == Kind::kChange && event.widget == "NameEdit") {
      // Renamed: the element keeps its script, the node and the row follow.
      const std::string renamed = pane.text("NameEdit");
      const auto taken = [&](const std::string& doc) {
        return !core::xml_get_attribute(editor_document_text(doc), "sequences/sequence[name=" + renamed + "]", "script").empty();
      };
      if (renamed.empty() || renamed == node.key || taken("Sequences/sequences.xml") || taken(editor_map_prefix() + "Sequences/sequences.xml")) return;
      patch(document, at, "name", renamed);
      node.key = renamed;
      build_explorer_tree();
      for (std::size_t i = 0; i < editor_.explorer_nodes.size(); ++i) {
        if (editor_.explorer_nodes[i].kind == ExplorerKind::kPane && editor_.explorer_nodes[i].key == renamed &&
            fold_name(editor_.explorer_nodes[i].pane).find("advsequence") != std::string::npos) {
          editor_.explorer_chosen = static_cast<std::int32_t>(i);
          editor_.explorer_nodes[static_cast<std::size_t>(editor_.explorer_nodes[i].parent)].expanded = true;
        }
      }
      refresh_explorer();
      return;
    }
    if (event.kind == Kind::kCommand && event.id == 0x1000607) {
      // Delete: the element goes; the `.vs` it named stays in the container.
      std::string& text = editor_document_text(document);
      text = core::xml_erase_element(text, at);
      close_explorer_pane();
      build_explorer_tree();
      for (ExplorerNode& branch : editor_.explorer_nodes) {
        if (branch.parent < 0 && branch.label == item_label("Sequences", "")) branch.expanded = true;
      }
      refresh_explorer();
      return;
    }
    if (event.kind == Kind::kChange && event.widget == "PreEdit") patch(document, at, "prerequisites", pane.text("PreEdit"));
    if (event.kind == Kind::kChange && event.widget == "ScriptEdit") {
      // The source, as typed, into the `.vs` the element names.
      editor_document_text(sequence_script_path(node.key)) = pane.text("ScriptEdit");
    }
    if (event.kind == Kind::kCommand && event.widget == "CompileBtn") {
      // This engine's own compiler over the same host surface the session
      // runs on; the first failure's line and message, or nothing.
      const std::string source = pane.text("ScriptEdit");
      const std::span<const std::byte> bytes{reinterpret_cast<const std::byte*>(source.data()), source.size()};
      core::script::Diagnostic diagnostic;
      const core::Result<core::script::Script> parsed = core::script::parse(bytes, node.key, &diagnostic);
      if (!parsed.ok()) {
        pane.set_text("ErrorText", "line " + std::to_string(diagnostic.line) + ": " + std::string(diagnostic.message));
        return;
      }
      core::script::CompileError error;
      if (!core::script::compile(parsed.value(), &registry_, &error).ok()) {
        pane.set_text("ErrorText", "line " + std::to_string(error.line) + ": " + error.message);
        return;
      }
      pane.set_text("ErrorText", item_label("OK", ""));
    }
    if (event.kind == Kind::kCommand && pane.screen().find("AutoRunCBox") != nullptr &&
        event.widget == "AutoRunCBox") {
      patch(document, at, "autorunallowed", checked("AutoRunCBox") ? "yes" : "no");
    }
    return;
  }
}

/// `DATA\vxAction.xml`: the editor's shortcuts, `<shortcut action= key=
/// ctrl= alt= shift=>`, keys named by their character or `up`, `down`,
/// `left`, `right`, `esc`, `del`, `enter`, `tab`.
void Application::editor_load_keys() {
  editor_.keys.clear();
  const platform::ByteSpan bytes = vfs_.read("DATA\\VXACTION.XML");
  if (bytes.empty()) return;
  auto parsed = core::XmlDocument::parse(platform::as_core_bytes(bytes));
  if (!parsed) return;
  const core::XmlDocument& doc = parsed.value();
  for (core::NodeIndex node = doc.child(doc.root(), "shortcut"); node != core::kNoNode;
       node = doc.next(node, "shortcut")) {
    EditorKey key;
    key.action = std::string(doc.attribute(node, "action"));
    std::string name(doc.attribute(node, "key"));
    for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (name == "up") key.key = SDLK_UP;
    else if (name == "down") key.key = SDLK_DOWN;
    else if (name == "left") key.key = SDLK_LEFT;
    else if (name == "right") key.key = SDLK_RIGHT;
    else if (name == "esc") key.key = SDLK_ESCAPE;
    else if (name == "del") key.key = SDLK_DELETE;
    else if (name == "enter") key.key = SDLK_RETURN;
    else if (name == "tab") key.key = SDLK_TAB;
    else if (name.size() == 1) key.key = static_cast<SDL_Keycode>(static_cast<unsigned char>(name[0]));
    else continue;
    // The modifiers are spelled `ctrl="on"`, which the generic yes/no
    // reader does not know -- so for as long as it was asked, every
    // shortcut loaded unmodified and Ctrl+Z was a plain Z.
    const auto on = [&](const char* attribute) {
      const std::string_view value = doc.attribute(node, attribute);
      return value == "on" || value == "1" || value == "yes" || value == "true";
    };
    key.ctrl = on("ctrl");
    key.alt = on("alt");
    key.shift = on("shift");
    editor_.keys.push_back(std::move(key));
  }
}

/// The editor's keys, through the shipped table (0x004cb7a0 looks the key
/// and its modifiers up): true when one was taken. The plus and minus of a
/// keypad or a shifted `=` are the same brush keys.
bool Application::editor_key(const SDL_KeyboardEvent& key) {
  const bool ctrl = (key.mod & SDL_KMOD_CTRL) != 0;
  const bool alt = (key.mod & SDL_KMOD_ALT) != 0;
  const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
  SDL_Keycode code = key.key;
  if (code == SDLK_KP_PLUS || (code == SDLK_EQUALS && shift)) code = SDLK_PLUS;
  if (code == SDLK_KP_MINUS) code = SDLK_MINUS;
  if (code == SDLK_KP_ENTER) code = SDLK_RETURN;
  std::string action;
  for (const EditorKey& entry : editor_.keys) {
    if (entry.key != code) continue;
    if (entry.ctrl != ctrl || entry.alt != alt) continue;
    if (entry.shift && !shift) continue;
    action = entry.action;
    break;
  }
  if (action.empty()) {
    // This engine's own: F2 saves.
    if (code == SDLK_F2) {
      open_editor_save_dialog();
      return true;
    }
    return false;
  }
  const std::int32_t family = editor_.tool == EditorTool::kHeight ? 2 : editor_.tool == EditorTool::kTerrain ? 0 : 1;
  const auto reopen = [&]() {
    if (editor_.chosen >= 0) open_editor_pane(editor_.nodes[static_cast<std::size_t>(editor_.chosen)]);
  };
  if (action == "brushplus" || action == "brushminus") {
    editor_.brush[family] = std::clamp(editor_.brush[family] + (action == "brushplus" ? 1 : -1), 0, 4);
    reopen();
    return true;
  }
  if (action == "toolprev" || action == "toolnext") {
    // Up and down walk the visible rows of the palette.
    std::int32_t row = -1;
    for (std::size_t i = 0; i < editor_.rows.size(); ++i) {
      if (editor_.rows[i] == editor_.chosen) row = static_cast<std::int32_t>(i);
    }
    row = std::clamp(row + (action == "toolnext" ? 1 : -1), 0, static_cast<std::int32_t>(editor_.rows.size()) - 1);
    if (!editor_.rows.empty()) {
      EditorNode& node = editor_.nodes[static_cast<std::size_t>(editor_.rows[static_cast<std::size_t>(row)])];
      const bool folded = node.expanded;
      editor_choose(editor_.rows[static_cast<std::size_t>(row)]);
      node.expanded = folded;  // walking past a branch does not fold it
      refresh_editor_palette();
    }
    return true;
  }
  if (action == "toolup" || action == "tooldown") {
    // Left folds the chosen branch, right unfolds it. **Reading, labelled:**
    // what the exe's two commands do to its tree was not read.
    if (editor_.chosen >= 0) {
      EditorNode& node = editor_.nodes[static_cast<std::size_t>(editor_.chosen)];
      if (!node.children.empty()) node.expanded = action == "tooldown";
      refresh_editor_palette();
    }
    return true;
  }
  if (action.rfind("tool", 0) == 0 && action.size() > 4 && std::isdigit(static_cast<unsigned char>(action[4]))) {
    // `tool1`..`tool10`: the n-th root of the palette.
    const int wanted = std::atoi(action.c_str() + 4);
    int seen = 0;
    for (std::size_t i = 0; i < editor_.nodes.size(); ++i) {
      if (editor_.nodes[i].parent >= 0) continue;
      if (++seen == wanted) {
        editor_choose(static_cast<std::int32_t>(i));
        break;
      }
    }
    return true;
  }
  if (action == "tooldefault") {
    if (editor_.tool != EditorTool::kEdit || editor_.selected != core::kNoObject) {
      for (std::size_t i = 0; i < editor_.nodes.size(); ++i) {
        if (editor_.nodes[i].is_tool && editor_.nodes[i].tool == EditorTool::kEdit) {
          editor_choose(static_cast<std::int32_t>(i));
          break;
        }
      }
      editor_.selected = core::kNoObject;
      session_->selections().player(local_player_).clear();
      return true;
    }
    // Out of the editor: to the front it came from, or out of the app.
    // **This engine's**: no shipped key leaves the editor.
    if (args_.from_front) {
      return_to_front();
      return true;
    }
    quit_requested_ = true;
    return true;
  }
  if (action == "delobjects") {
    editor_delete();
    return true;
  }
  if (action == "togglewnd") {
    // The palettes hidden and shown: moved off the display and back, since
    // a dialog has no hidden state of its own.
    editor_.windows_hidden = !editor_.windows_hidden;
    for (std::size_t i = 0; i < ui_.dialog_count(); ++i) {
      core::ui::Dialog* dialog = ui_.dialog(i);
      if (dialog == nullptr || !is_editor_pane(*dialog)) continue;
      const core::ui::Rect rect = dialog->rect();
      ui_.place_dialog(dialog, editor_.windows_hidden ? rect.x + 100000 : rect.x - 100000, rect.y);
    }
    return true;
  }
  if (action == "undo" || action == "redo") {
    // Only the brushes' work: the original's default tool has no undo
    // (0x004c2626 asks the tool first, and object edits never record).
    if (editor_.tool == EditorTool::kTerrain || editor_.tool == EditorTool::kHeight ||
        editor_.tool == EditorTool::kDecor || editor_.tool == EditorTool::kDecorDelete) {
      editor_undo(action == "redo");
    }
    return true;
  }
  if (action == "objprops") {
    if (editor_.selected != core::kNoObject) open_object_properties({editor_.selected});
    return true;
  }
  if (action == "nextwnd") {
    // The window after the active one, wrapping. The active one is the
    // topmost, which is what takes a click and what the stack draws last.
    // Not while something that is not one of them stands over them: the
    // save dialog is a screen, not a tool window, and raising a window
    // through it would put a palette over the dialog the player is in.
    core::ui::Dialog* top = ui_.dialog(ui_.dialog_count() - 1);
    if (top == nullptr || !is_editor_pane(*top)) return true;
    const std::vector<core::ui::Dialog*> windows = editor_windows();
    if (windows.size() < 2) return true;
    std::size_t active = 0;
    std::size_t highest = 0;
    for (std::size_t i = 0; i < windows.size(); ++i) {
      const std::size_t where = ui_.index_of(windows[i]);
      if (where >= highest) {
        highest = where;
        active = i;
      }
    }
    editor_activate_window(windows[(active + 1) % windows.size()]);
    return true;
  }
  if (action == "compilescript") {
    // Ctrl+Enter presses *Compile* on whatever pane is showing one. Two
    // of the editor's dialogs have the button -- a sequence's Source tab
    // and an item type's use script -- and each already answers it, so
    // this only has to find the button and say it was pressed. The event
    // goes through the same path a click's does, which is what keeps the
    // key and the button from drifting apart.
    for (std::size_t i = ui_.dialog_count(); i-- > 0;) {
      core::ui::Dialog* dialog = ui_.dialog(i);
      if (dialog == nullptr || !is_editor_pane(*dialog)) continue;
      const core::ui::Widget* button = dialog->screen().find("CompileBtn");
      if (button == nullptr) continue;
      core::ui::DialogEvent press;
      press.kind = core::ui::DialogEvent::Kind::kCommand;
      press.widget = "CompileBtn";
      press.id = button->id;
      if (i < menu_handlers_.size()) {
        const MenuHandler handler = menu_handlers_[i];
        handler(press, *dialog);
      }
      return true;
    }
    return true;
  }
  std::printf("editor: %s is not built\n", action.c_str());
  return true;
}

/// A click on the map with a class chosen: the object, minted the way a
/// script places one (`spawn_of_class`, then owner and position), and its
/// authored record for the save.
void Application::editor_place(std::int32_t mx, std::int32_t my) {
  core::sim::World& world = session_->world();
  const core::ClassGraph* graph = world.class_graph();
  if (graph == nullptr || editor_.placing.empty()) return;
  const core::ClassIndex index = graph->find(editor_.placing);
  if (index == core::kNoClass) return;
  const auto at = camera_.unproject(mx, my);
  const core::ObjectId id = world.spawn_of_class(index);
  if (id == core::kNoObject) {
    std::printf("editor: %s has no native class to place\n", editor_.placing.c_str());
    return;
  }
  (void)world.set_owner(id, editor_.player);
  (void)world.set_position(id, at);
  editor_stamp(editor_mask_of(index), at);
  core::MapObject record;
  record.class_name = editor_.placing;
  record.num = -1;  // numbered at the save
  record.x = at.x;
  record.y = at.y;
  record.player = static_cast<std::int32_t>(editor_.player) + 1;
  if (const core::sim::WorldObject* slot = world.find(id)) record.flags = core::sim::pack_sync_flags(slot->state);
  editor_.added.push_back(std::move(record));
  editor_.object_ids.push_back(id);
  editor_.selected = id;
  std::printf("editor: placed %s at %d, %d for player %u\n", editor_.placing.c_str(), at.x, at.y,
              static_cast<unsigned>(editor_.player) + 1);
}

void Application::editor_select(std::int32_t mx, std::int32_t my) {
  core::sim::World& world = session_->world();
  editor_.selected = world_view_.pick(world, camera_, mx, my);
  core::sim::Selection& selection = session_->selections().player(local_player_);
  selection.clear();
  if (editor_.selected != core::kNoObject) {
    selection.add(editor_.selected);
    if (const core::sim::WorldObject* object = world.find(editor_.selected)) {
      editor_.drag_from = object->state.position;
    }
  }
}

/// The end of a drag: the object's footprint taken out where it stood and
/// stamped where it is (0x004903b0 drops through 0x00547a30 and the stamp).
void Application::editor_drop() {
  if (!editor_.dragging || editor_.selected == core::kNoObject) return;
  const core::sim::WorldObject* object = session_->world().find(editor_.selected);
  if (object == nullptr || object->state.position == editor_.drag_from) return;
  const core::edit::PassMask* mask = editor_mask_of(object->class_index);
  editor_unstamp(mask, editor_.drag_from);
  editor_stamp(mask, object->state.position);
  editor_.drag_from = object->state.position;
}

void Application::editor_delete() {
  if (editor_.selected == core::kNoObject) return;
  core::sim::World& world = session_->world();
  for (std::size_t i = 0; i < editor_.object_ids.size(); ++i) {
    if (editor_.object_ids[i] != editor_.selected) continue;
    editor_.object_ids[i] = core::kNoObject;
    const std::size_t authored = session_->populated().object_ids.size();
    if (i < authored) {
      editor_.removed.insert(static_cast<std::int32_t>(i));
    } else {
      editor_.added.erase(editor_.added.begin() + static_cast<std::ptrdiff_t>(i - authored));
      editor_.object_ids.erase(editor_.object_ids.begin() + static_cast<std::ptrdiff_t>(i));
    }
    break;
  }
  for (std::size_t i = 0; i < editor_.areas.size(); ++i) {
    if (editor_.areas[i].id != editor_.selected) continue;
    editor_.areas.erase(editor_.areas.begin() + static_cast<std::ptrdiff_t>(i));
    if (editor_.area_selected == static_cast<std::int32_t>(i)) editor_.area_selected = -1;
    else if (editor_.area_selected > static_cast<std::int32_t>(i)) --editor_.area_selected;
    open_area_dialog();
    break;
  }
  editor_.directions.erase(editor_.selected);
  const core::edit::PassMask* mask = nullptr;
  core::sim::Point stood;
  if (const core::sim::WorldObject* object = world.find(editor_.selected)) {
    mask = editor_mask_of(object->class_index);
    stood = object->state.position;
  }
  (void)world.despawn(editor_.selected);
  editor_unstamp(mask, stood);
  session_->selections().player(local_player_).clear();
  editor_.selected = core::kNoObject;
}

/// The right button of the default tool: the selected object looks at the
/// point (0x0048e970 -> `LookAt` -> `SetDirection(world - pos)`), or away
/// from it under Shift. The vector is kept raw, as the exe keeps it, and
/// the sprite's facing follows through the movement system.
void Application::editor_turn(core::sim::Point towards, bool opposite) {
  if (editor_.selected == core::kNoObject) return;
  core::sim::World& world = session_->world();
  const core::sim::WorldObject* object = world.find(editor_.selected);
  if (object == nullptr) return;
  core::sim::Point dir{towards.x - object->state.position.x, towards.y - object->state.position.y};
  if (opposite) dir = core::sim::Point{-dir.x, -dir.y};
  if (dir.x == 0 && dir.y == 0) dir = core::sim::Point{0, 1};
  editor_.directions[editor_.selected] = dir;
  if (core::sim::MovementSystem* movement = core::sim::movement_system(world)) {
    movement->set_facing(world, editor_.selected, movement->state(editor_.selected), dir);
  }
}

/// A painting tool at `world`, on the press (`first`) and along the drag.
void Application::editor_paint(core::sim::Point world, bool first) {
  using namespace core::edit;
  if (editor_.chosen < 0) return;
  const EditorNode& node = editor_.nodes[static_cast<std::size_t>(editor_.chosen)];
  const bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
  switch (editor_.tool) {
    case EditorTool::kTerrain: {
      if (!first && !terrain_stroke_moved(editor_.last_paint, world)) return;
      editor_.last_paint = world;
      const core::sim::Point cell = terrain_brush_cell(world);
      const std::int32_t radius = brush_radius(editor_.brush[0], false);
      // All four layers, `radius + 2` cells around the corner (0x004b4108).
      editor_snapshot(undo_rect(core::sim::Point{cell.x * 64, cell.y * 64}, (radius + 2) * 64),
                      UndoStack::kTerrain | UndoStack::kHeight | UndoStack::kDecor | UndoStack::kPass, first);
      CellRect changed = paint_terrain(world_.terrain_mut(), cell.x, cell.y, radius, node.values,
                                       node.water_group, shift, editor_.rng);
      // A water *leaf* -- shallow or deep, chosen as a leaf rather than
      // through the Water group -- levels the ground under the brush's
      // square to sea level, removes the decorations standing in it and
      // rebuilds the passability there (0x004b3dcd), stroke after stroke.
      if (node.values.size() == 1 && !node.water_group) {
        const core::TerrainLayerDef* layer = map_.terrain_table().layer(node.values[0]);
        if (layer != nullptr && layer->type == 4) {
          const WorldRect square = water_leaf_rect(cell.x, cell.y, radius);
          const CellRect levelled = level_height(world_.height_mut(), square, 0);
          if (!levelled.empty()) {
            rebake_light(world_.light_mut(), world_.height(), levelled);
            changed.merge(CellRect{levelled.x0 / 2, levelled.y0 / 2, levelled.x1 / 2, levelled.y1 / 2});
          }
          changed.merge(clear_decor_in(world_.decor_mut(), square));
          editor_rebuild_passability(square);
        }
      }
      // Then the stroke's own rebuild (0x00547700 over the disc's rectangle).
      // 0x00547c00's widening for a shipyard near the stroke is not
      // reproduced; see the header.
      editor_rebuild_passability(terrain_stroke_rect(cell.x, cell.y, radius));
      if (changed.empty()) return;
      editor_repaint(changed);
      return;
    }
    case EditorTool::kHeight: {
      const std::int32_t cx = world.x >= 0 ? world.x / 32 : -1;
      const std::int32_t cy = world.y >= 0 ? world.y / 32 : -1;
      if (first) {
        editor_.stroke.begin(world_.height());
        editor_.last_cell = core::sim::Point{cx, cy};
      } else if (editor_.last_cell == core::sim::Point{cx, cy}) {
        return;
      }
      // Every cell along the line from the last one, as the drag walks it.
      const core::sim::Point from = editor_.last_cell;
      const std::int32_t steps = std::max(std::abs(cx - from.x), std::abs(cy - from.y));
      const std::int32_t radius = brush_radius(editor_.brush[2], true);
      CellRect changed;
      for (std::int32_t k = first ? 0 : 1; k <= std::max(steps, 0); ++k) {
        const std::int32_t x = steps == 0 ? cx : from.x + (cx - from.x) * k / steps;
        const std::int32_t y = steps == 0 ? cy : from.y + (cy - from.y) * k / steps;
        const std::int32_t value = node.height_tool == HeightTool::kSet ? editor_.level : editor_.amount;
        // Terrain, height and passability over the disc (0x0049da20).
        editor_snapshot(WorldRect{(x - radius) * 32, (y - radius) * 32, (x + radius + 1) * 32 - 1,
                                  (y + radius + 1) * 32 - 1},
                        UndoStack::kTerrain | UndoStack::kHeight | UndoStack::kPass, first && k == 0);
        changed.merge(apply_height(world_.height_mut(), editor_.stroke, node.height_tool, x, y, radius, value));
      }
      editor_.last_cell = core::sim::Point{cx, cy};
      if (changed.empty()) return;
      rebake_light(world_.light_mut(), world_.height(), changed);
      // Height cells are 32 units; the ground is composed per 64-unit cell.
      editor_repaint(CellRect{changed.x0 / 2, changed.y0 / 2, changed.x1 / 2, changed.y1 / 2});
      return;
    }
    case EditorTool::kDecor:
    case EditorTool::kDecorDelete: {
      const std::int32_t radius = brush_radius(editor_.brush[1], false);
      const std::int32_t cx = world.x >= 0 ? world.x / 64 : -1;
      const std::int32_t cy = world.y >= 0 ? world.y / 64 : -1;
      if (!first && editor_.last_cell == core::sim::Point{cx, cy}) return;
      editor_.last_cell = core::sim::Point{cx, cy};
      // Decorations and passability, five cells around the corner whatever
      // the brush (0x0048d9c1).
      editor_snapshot(undo_rect(core::sim::Point{cx * 64, cy * 64}, 320), UndoStack::kDecor | UndoStack::kPass, first);
      // What the disc held before, so that a decoration taken out or
      // written over gives its footprint back (0x00547a30) and one put down
      // stamps its own (0x00546d90).
      const core::Grid& decor = world_.decor();
      std::vector<std::pair<core::sim::Point, std::uint32_t>> before;
      for (const core::sim::Point d : brush_disc(radius)) {
        const std::int32_t x = cx + d.x;
        const std::int32_t y = cy + d.y;
        if (x < 0 || y < 0 || decor.cell_size() == 0) continue;
        if (static_cast<std::uint32_t>(x) >= decor.width() || static_cast<std::uint32_t>(y) >= decor.height()) continue;
        before.emplace_back(core::sim::Point{x, y}, decor.cell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)));
      }
      CellRect changed;
      if (editor_.tool == EditorTool::kDecorDelete) {
        changed = clear_decor(world_.decor_mut(), cx, cy, radius);
      } else if (radius == 0) {
        if (node.values.empty()) return;
        // The exact stamp places the chosen kind and draws the next.
        const std::int32_t kind = node.values[static_cast<std::size_t>(editor_.rng.between(0, static_cast<std::int32_t>(node.values.size()) - 1))];
        changed = stamp_decor(world_.decor_mut(), world, kind);
      } else {
        changed = scatter_decor(world_.decor_mut(), cx, cy, radius, node.values, editor_.density, editor_.rng);
      }
      (void)changed;  // the live view reads the layer every frame
      for (const auto& [at, was] : before) {
        const std::uint32_t now = decor.cell(static_cast<std::uint32_t>(at.x), static_cast<std::uint32_t>(at.y));
        if (now == was) continue;
        if (was != 0) editor_unstamp(editor_mask_of_kind(was), decor_position(at.x, at.y, was));
        if (now != 0) {
          const core::edit::PassMask* mask = editor_mask_of_kind(now);
          if (mask != nullptr) {
            core::edit::stamp_footprint(world_.passability_mut(), *mask, decor_position(at.x, at.y, now),
                                        world_.height(), WorldRect::of_map(world_.terrain()),
                                        WorldRect::of_map(world_.terrain()));
          }
        }
      }
      return;
    }
    default:
      return;
  }
}

/// The mask a class stamps: its entity in the map's season, and the
/// entity's `.pass`, read out of the packs once and kept.
const core::edit::PassMask* Application::editor_mask_of(core::ClassIndex index) {
  if (index == core::kNoClass || entities_ == nullptr) return nullptr;
  if (const auto found = editor_.class_masks.find(index); found != editor_.class_masks.end()) {
    return found->second;
  }
  const core::ClassGraph* graph = session_->world().class_graph();
  const core::Entity* entity = nullptr;
  if (graph != nullptr) {
    const std::string_view path = graph->entity_path(index, season_);
    if (!path.empty()) entity = entities_->resolve(path);
  }
  const core::edit::PassMask* mask = entities_->mask_of(entity);
  editor_.class_masks[index] = mask;
  return mask;
}

const core::edit::PassMask* Application::editor_mask_of_kind(std::uint32_t cell) const {
  const std::size_t kind = cell & 0xff;
  return kind < editor_.decor_masks.size() ? editor_.decor_masks[kind] : nullptr;
}

/// Every standing object with a mask, at its position -- what the rebuild
/// picks the ones in range from.
std::vector<core::edit::Footprint> Application::editor_footprints() {
  std::vector<core::edit::Footprint> out;
  for (const core::sim::WorldObject& object : session_->world().objects()) {
    if (object.object == nullptr || object.class_index == core::kNoClass) continue;
    if (object.state.flags.unspawned || object.state.is_held()) continue;
    if (const core::edit::PassMask* mask = editor_mask_of(object.class_index)) {
      out.push_back({mask, object.state.position});
    }
  }
  return out;
}

void Application::editor_rebuild_passability(const core::edit::WorldRect& rect) {
  if (rect.empty() || world_.passability().cell_size() == 0 || world_.terrain().cell_size() == 0) return;
  core::edit::rebuild_passability(world_.passability_mut(), rect, world_.terrain(), map_.terrain_table(),
                                  world_.height(), editor_footprints(), world_.decor(), editor_.decor_masks);
}

/// A placed object's footprint, stamped within its own flat extent
/// (0x004a3fc0 hands the stamp the extent 0x005fe080 answers).
void Application::editor_stamp(const core::edit::PassMask* mask, core::sim::Point at) {
  if (mask == nullptr || world_.passability().cell_size() == 0 || world_.terrain().cell_size() == 0) return;
  const core::edit::WorldRect map = core::edit::WorldRect::of_map(world_.terrain());
  core::edit::stamp_footprint(world_.passability_mut(), *mask, at, world_.height(), map,
                              core::edit::footprint_rect(*mask, at));
}

/// A removed object's footprint given back: the rebuild over its flat
/// extent (0x00547a30), which puts back whatever else stands there.
void Application::editor_unstamp(const core::edit::PassMask* mask, core::sim::Point at) {
  if (mask == nullptr) return;
  editor_rebuild_passability(core::edit::footprint_rect(*mask, at));
}

core::edit::UndoStack::Layers Application::editor_layers() {
  core::edit::UndoStack::Layers layers;
  layers.terrain = &world_.terrain_mut();
  layers.height = &world_.height_mut();
  layers.decor = &world_.decor_mut();
  layers.pass = &world_.passability_mut();
  return layers;
}

void Application::editor_snapshot(const core::edit::WorldRect& rect, std::uint32_t flags, bool first) {
  editor_.undo.snapshot(editor_layers(), rect, flags, first);
}

/// Ctrl+Z and Ctrl+Y: the newest stroke taken back or put again, the light
/// re-baked over it (the record carries the height, not the light, which
/// is the height's function), and the ground redrawn.
void Application::editor_undo(bool redo) {
  using namespace core::edit;
  const UndoStack::Layers layers = editor_layers();
  const WorldRect rect = redo ? editor_.undo.redo(layers) : editor_.undo.undo(layers);
  if (rect.empty()) return;
  const CellRect heights{std::max(0, rect.x0 / 32), std::max(0, rect.y0 / 32), std::max(0, rect.x1 / 32),
                         std::max(0, rect.y1 / 32)};
  rebake_light(world_.light_mut(), world_.height(), heights);
  editor_repaint(CellRect{std::max(0, rect.x0 / 64), std::max(0, rect.y0 / 64), std::max(0, rect.x1 / 64),
                          std::max(0, rect.y1 / 64)});
  // A stroke in progress does not straddle an undo.
  editor_.painting = false;
}

/// The right button of a brush: the terrain under the pointer becomes the
/// chosen leaf (0x004b3470), Set height reads the level under it
/// (0x0049e120); the decor and delete tools go back to Edit objects.
void Application::editor_pick(core::sim::Point world) {
  using namespace core::edit;
  if (editor_.chosen < 0) return;
  const EditorNode& node = editor_.nodes[static_cast<std::size_t>(editor_.chosen)];
  if (editor_.tool == EditorTool::kTerrain) {
    const core::Grid& terrain = world_.terrain();
    if (terrain.cell_size() == 0 || world.x < 0 || world.y < 0) return;
    const auto z = static_cast<std::int32_t>(terrain.cell(static_cast<std::uint32_t>(world.x / 64),
                                                          static_cast<std::uint32_t>(world.y / 64)));
    for (std::size_t i = 0; i < editor_.nodes.size(); ++i) {
      const EditorNode& candidate = editor_.nodes[i];
      if (candidate.tool != EditorTool::kTerrain || !candidate.children.empty()) continue;
      if (candidate.values.size() == 1 && candidate.values[0] == z) {
        // The leaf, or -- when a group was chosen -- the layer's group.
        std::int32_t target = static_cast<std::int32_t>(i);
        if (node.values.size() > 1 && candidate.parent >= 0) target = candidate.parent;
        if (candidate.parent >= 0) editor_.nodes[static_cast<std::size_t>(candidate.parent)].expanded = true;
        const bool was = editor_.nodes[static_cast<std::size_t>(target)].expanded;
        editor_choose(target);
        editor_.nodes[static_cast<std::size_t>(target)].expanded = was || target != static_cast<std::int32_t>(i);
        refresh_editor_palette();
        return;
      }
    }
    return;
  }
  if (editor_.tool == EditorTool::kHeight && node.height_tool == HeightTool::kSet) {
    const core::Grid& heights = world_.height();
    if (heights.cell_size() == 0 || world.x < 0 || world.y < 0) return;
    editor_.level = level_of_height(static_cast<std::int32_t>(heights.cell(static_cast<std::uint32_t>(world.x / 32),
                                                                             static_cast<std::uint32_t>(world.y / 32))));
    if (editor_.tool_pane != nullptr) editor_.tool_pane->set_text("StrengthEdit", std::to_string(editor_.level));
    return;
  }
  if (editor_.tool == EditorTool::kDecorDelete || editor_.tool == EditorTool::kPlace ||
      (editor_.tool == EditorTool::kDecor && brush_radius(editor_.brush[1], false) > 0)) {
    for (std::size_t i = 0; i < editor_.nodes.size(); ++i) {
      if (editor_.nodes[i].is_tool && editor_.nodes[i].tool == EditorTool::kEdit) {
        editor_choose(static_cast<std::int32_t>(i));
        break;
      }
    }
  }
  // The decor tool's exact stamp draws its next kind at random already, so
  // the "cycle" the right button does with one cell is a redraw.
}

/// A click with an area tool: an `AdvArea` at the point -- a circle of
/// radius 300, or the 400-unit square around it (0x0047c2b0) -- owned by
/// the place pane's player, named `Unnamed` (0x00458bc0).
void Application::editor_place_area(core::sim::Point world, bool circle) {
  core::sim::World& sim = session_->world();
  const core::ClassGraph* graph = sim.class_graph();
  if (graph == nullptr) return;
  const core::ClassIndex index = graph->find("AdvArea");
  if (index == core::kNoClass) return;
  const core::ObjectId id = sim.spawn_of_class(index);
  if (id == core::kNoObject) return;
  (void)sim.set_owner(id, editor_.player);
  (void)sim.set_position(id, world);
  EditorArea area;
  area.id = id;
  area.name = item_label("Unnamed", "advarea");
  if (circle) {
    area.shape.type = core::kAreaCircle;
    area.shape.ptx = world.x;
    area.shape.pty = world.y;
    area.shape.radius = 300;
  } else {
    area.shape.type = core::kAreaRectangle;
    area.shape.left = std::max(0, world.x - 199);
    area.shape.top = std::max(0, world.y - 199);
    area.shape.right = std::min(world_.geometry().size_x, world.x + 200);
    area.shape.bottom = std::min(world_.geometry().size_y, world.y + 200);
  }
  core::MapObject record;
  record.class_name = "AdvArea";
  record.num = -1;
  record.x = world.x;
  record.y = world.y;
  record.player = static_cast<std::int32_t>(editor_.player) + 1;
  if (const core::sim::WorldObject* slot = sim.find(id)) record.flags = core::sim::pack_sync_flags(slot->state);
  editor_.added.push_back(std::move(record));
  editor_.object_ids.push_back(id);
  editor_.areas.push_back(std::move(area));
  editor_.selected = id;
}

namespace {

/// The handles of an area, in world units: 1 the centre, then for a circle
/// 2 on the rim to the right, for a rectangle 2..5 the edge midpoints
/// (left, right, top, bottom) and 6..9 the corners.
std::vector<std::pair<std::int32_t, core::sim::Point>> area_handles(const core::MapArea& shape) {
  std::vector<std::pair<std::int32_t, core::sim::Point>> out;
  if (shape.is_circle()) {
    out.emplace_back(1, core::sim::Point{shape.ptx, shape.pty});
    out.emplace_back(2, core::sim::Point{shape.ptx + shape.radius, shape.pty});
    return out;
  }
  const std::int32_t cx = (shape.left + shape.right) / 2;
  const std::int32_t cy = (shape.top + shape.bottom) / 2;
  out.emplace_back(1, core::sim::Point{cx, cy});
  out.emplace_back(2, core::sim::Point{shape.left, cy});
  out.emplace_back(3, core::sim::Point{shape.right, cy});
  out.emplace_back(4, core::sim::Point{cx, shape.top});
  out.emplace_back(5, core::sim::Point{cx, shape.bottom});
  out.emplace_back(6, core::sim::Point{shape.left, shape.top});
  out.emplace_back(7, core::sim::Point{shape.right, shape.top});
  out.emplace_back(8, core::sim::Point{shape.left, shape.bottom});
  out.emplace_back(9, core::sim::Point{shape.right, shape.bottom});
  return out;
}

}  // namespace

/// The edit-areas tool's press: a handle within its 15-pixel square starts
/// a move or a resize; a press inside an area selects it; elsewhere clears.
void Application::editor_area_press(core::sim::Point world) {
  editor_.area_handle = 0;
  const platform::ScreenPoint at = camera_.project(world);
  // The selected area's handles first, then any area's body.
  if (editor_.area_selected >= 0 && static_cast<std::size_t>(editor_.area_selected) < editor_.areas.size()) {
    for (const auto& [kind, point] : area_handles(editor_.areas[static_cast<std::size_t>(editor_.area_selected)].shape)) {
      const platform::ScreenPoint h = camera_.project(point);
      if (std::abs(h.x - at.x) <= 7 && std::abs(h.y - at.y) <= 7) {
        editor_.area_handle = kind;
        return;
      }
    }
  }
  for (std::size_t i = 0; i < editor_.areas.size(); ++i) {
    const core::MapArea& shape = editor_.areas[i].shape;
    const core::sim::AreaShape probe = shape.is_circle()
        ? core::sim::AreaShape::of_circle({shape.ptx, shape.pty}, shape.radius)
        : core::sim::AreaShape::of_rectangle(shape.left, shape.top, shape.right, shape.bottom);
    if (!probe.contains(world)) continue;
    editor_.area_selected = static_cast<std::int32_t>(i);
    editor_.selected = editor_.areas[i].id;
    editor_.area_handle = 1;
    open_area_dialog();
    return;
  }
  editor_.area_selected = -1;
  open_area_dialog();
}

/// The drag of a handle (0x0047f950): the centre moves the whole area and
/// its object; a circle's rim sets the radius to the distance; a
/// rectangle's grip takes its edge along, edges swapping when they cross.
void Application::editor_area_drag(core::sim::Point world) {
  if (editor_.area_handle == 0 || editor_.area_selected < 0 ||
      static_cast<std::size_t>(editor_.area_selected) >= editor_.areas.size()) {
    return;
  }
  EditorArea& area = editor_.areas[static_cast<std::size_t>(editor_.area_selected)];
  core::MapArea& shape = area.shape;
  world.x = std::clamp(world.x, 0, world_.geometry().size_x);
  world.y = std::clamp(world.y, 0, world_.geometry().size_y);
  if (editor_.area_handle == 1) {
    if (shape.is_circle()) {
      shape.ptx = world.x;
      shape.pty = world.y;
    } else {
      const std::int32_t w = shape.right - shape.left;
      const std::int32_t h = shape.bottom - shape.top;
      shape.left = world.x - w / 2;
      shape.top = world.y - h / 2;
      shape.right = shape.left + w;
      shape.bottom = shape.top + h;
    }
    (void)session_->world().set_position(area.id, world);
  } else if (shape.is_circle()) {
    const std::int64_t dx = world.x - shape.ptx;
    const std::int64_t dy = world.y - shape.pty;
    std::int64_t r = 0;
    while ((r + 1) * (r + 1) <= dx * dx + dy * dy) ++r;
    // Rounded: up when the remainder passes the half.
    if ((r + 1) * (r + 1) - (dx * dx + dy * dy) < (dx * dx + dy * dy) - r * r) ++r;
    shape.radius = static_cast<std::int32_t>(std::max<std::int64_t>(1, r));
  } else {
    const std::int32_t handle = editor_.area_handle;
    const bool left = handle == 2 || handle == 6 || handle == 8;
    const bool right = handle == 3 || handle == 7 || handle == 9;
    const bool top = handle == 4 || handle == 6 || handle == 7;
    const bool bottom = handle == 5 || handle == 8 || handle == 9;
    if (left) shape.left = world.x;
    if (right) shape.right = world.x;
    if (top) shape.top = world.y;
    if (bottom) shape.bottom = world.y;
    if (shape.left > shape.right) {
      std::swap(shape.left, shape.right);
      editor_.area_handle = left ? (handle == 2 ? 3 : handle == 6 ? 7 : 9) : (handle == 3 ? 2 : handle == 7 ? 6 : 8);
    }
    if (shape.top > shape.bottom) {
      std::swap(shape.top, shape.bottom);
      const std::int32_t h = editor_.area_handle;
      editor_.area_handle = top ? (h == 4 ? 5 : h == 6 ? 8 : 9) : (h == 5 ? 4 : h == 8 ? 6 : 7);
    }
    (void)session_->world().set_position(area.id, core::sim::Point{(shape.left + shape.right) / 2,
                                                                     (shape.top + shape.bottom) / 2});
  }
  open_area_dialog();
}

/// The ground under painted terrain cells is composed again.
void Application::editor_repaint(const core::edit::CellRect& cells) {
  if (cells.empty()) return;
  map_.invalidate_ground(cells.x0, cells.y0, cells.x1, cells.y1);
}

namespace {

void overlay_pixel(core::ui::Image& image, std::int32_t x, std::int32_t y, std::uint8_t r, std::uint8_t g,
                   std::uint8_t b) {
  if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(image.width) || y >= static_cast<std::int32_t>(image.height)) return;
  std::uint8_t* p = image.pixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
  p[0] = r;
  p[1] = g;
  p[2] = b;
  p[3] = 255;
}

void overlay_line(core::ui::Image& image, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                  std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  const std::int32_t dx = std::abs(x1 - x0);
  const std::int32_t dy = -std::abs(y1 - y0);
  const std::int32_t sx = x0 < x1 ? 1 : -1;
  const std::int32_t sy = y0 < y1 ? 1 : -1;
  std::int32_t err = dx + dy;
  for (int guard = 0; guard < 100000; ++guard) {
    overlay_pixel(image, x0, y0, r, g, b);
    if (x0 == x1 && y0 == y1) break;
    const std::int32_t e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void overlay_square(core::ui::Image& image, std::int32_t cx, std::int32_t cy, std::int32_t half,
                    std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  overlay_line(image, cx - half, cy - half, cx + half, cy - half, r, g, b);
  overlay_line(image, cx + half, cy - half, cx + half, cy + half, r, g, b);
  overlay_line(image, cx + half, cy + half, cx - half, cy + half, r, g, b);
  overlay_line(image, cx - half, cy + half, cx - half, cy - half, r, g, b);
}

}  // namespace

/// The editor's marks over the world: every area's outline in the tool's
/// green (0x7fe0 in the exe's 16-bit draw, 0x00480070), the selected one's
/// handles as 15-pixel squares (0x00458d20), and the brush's footprint
/// under the pointer -- **this engine's**, the exe's brush cursor was not
/// read. Areas are drawn through the same projection as the objects, so a
/// circle is an ellipse 46/64 as tall as it is wide.
void Application::refresh_editor_overlay(std::uint32_t width, std::uint32_t height) {
  static core::ui::Image image;
  if (!editor_.active) {
    if (!image.empty()) {
      image = core::ui::Image{};
      ui_.set_overlay(image, 0, 0);
    }
    return;
  }
  if (image.width != width || image.height != height) {
    image.width = width;
    image.height = height;
  }
  image.rgba.assign(static_cast<std::size_t>(width) * height * 4, 0);
  const auto project = [&](core::sim::Point p) { return camera_.project(p); };
  for (std::size_t i = 0; i < editor_.areas.size(); ++i) {
    const core::MapArea& shape = editor_.areas[i].shape;
    const bool selected = static_cast<std::int32_t>(i) == editor_.area_selected;
    const std::uint8_t g = selected ? 255 : 200;
    if (shape.is_circle()) {
      platform::ScreenPoint last = project({shape.ptx + shape.radius, shape.pty});
      for (int step = 1; step <= 64; ++step) {
        // 64 chords; the trigonometry is a table of the unit circle at
        // integer scale, so the core's no-float rule is kept in spirit.
        static const std::int32_t kCos[65] = {
            1000, 995, 981, 957, 924, 882, 831, 773, 707, 634, 556, 471, 383, 290, 195, 98, 0, -98, -195, -290, -383, -471,
            -556, -634, -707, -773, -831, -882, -924, -957, -981, -995, -1000, -995, -981, -957, -924, -882, -831, -773,
            -707, -634, -556, -471, -383, -290, -195, -98, 0, 98, 195, 290, 383, 471, 556, 634, 707, 773, 831, 882, 924,
            957, 981, 995, 1000};
        const std::int32_t c = kCos[step];
        const std::int32_t s = kCos[(step + 48) % 64];
        const platform::ScreenPoint next = project({shape.ptx + shape.radius * c / 1000, shape.pty + shape.radius * s / 1000});
        overlay_line(image, last.x, last.y, next.x, next.y, 0, g, 0);
        last = next;
      }
    } else {
      const platform::ScreenPoint a = project({shape.left, shape.top});
      const platform::ScreenPoint b = project({shape.right, shape.top});
      const platform::ScreenPoint c = project({shape.right, shape.bottom});
      const platform::ScreenPoint d = project({shape.left, shape.bottom});
      overlay_line(image, a.x, a.y, b.x, b.y, 0, g, 0);
      overlay_line(image, b.x, b.y, c.x, c.y, 0, g, 0);
      overlay_line(image, c.x, c.y, d.x, d.y, 0, g, 0);
      overlay_line(image, d.x, d.y, a.x, a.y, 0, g, 0);
    }
    if (selected && editor_.tool == EditorTool::kEditArea) {
      for (const auto& [kind, point] : area_handles(shape)) {
        const platform::ScreenPoint h = project(point);
        overlay_square(image, h.x, h.y, 7, 255, 255, 255);
      }
    }
  }
  // The brush under the pointer, as the cells it would touch.
  if (editor_.pointer_on_map &&
      (editor_.tool == EditorTool::kTerrain || editor_.tool == EditorTool::kHeight ||
       editor_.tool == EditorTool::kDecor || editor_.tool == EditorTool::kDecorDelete)) {
    const bool height = editor_.tool == EditorTool::kHeight;
    const std::int32_t size = height ? 32 : 64;
    const std::int32_t family = height ? 2 : editor_.tool == EditorTool::kTerrain ? 0 : 1;
    const std::int32_t radius = core::edit::brush_radius(editor_.brush[family], height);
    core::sim::Point centre = editor_.tool == EditorTool::kTerrain
        ? core::edit::terrain_brush_cell(editor_.pointer)
        : core::sim::Point{editor_.pointer.x / size, editor_.pointer.y / size};
    for (const core::sim::Point d : core::edit::brush_disc(radius)) {
      const std::int32_t cx = centre.x + d.x;
      const std::int32_t cy = centre.y + d.y;
      const platform::ScreenPoint a = project({cx * size, cy * size});
      const platform::ScreenPoint b = project({(cx + 1) * size, (cy + 1) * size});
      // Only the outer edges: a neighbour inside the disc shares the line.
      const auto inside = [&](std::int32_t i, std::int32_t j) { return i * i + j * j <= radius * radius; };
      if (!inside(d.x, d.y - 1)) overlay_line(image, a.x, a.y, b.x - 1, a.y, 255, 255, 0);
      if (!inside(d.x, d.y + 1)) overlay_line(image, a.x, b.y - 1, b.x - 1, b.y - 1, 255, 255, 0);
      if (!inside(d.x - 1, d.y)) overlay_line(image, a.x, a.y, a.x, b.y - 1, 255, 255, 0);
      if (!inside(d.x + 1, d.y)) overlay_line(image, b.x - 1, a.y, b.x - 1, b.y - 1, 255, 255, 0);
    }
  }
  ui_.set_overlay(image, 0, 0);
}

/// The authored list as edited: the map's objects less the removed, at
/// their moved positions and turned directions, plus the placed ones
/// numbered after the last, the areas' shapes and names with them.
core::MapObjectList Application::editor_document() const {
  core::MapObjectList list;
  if (auto parsed = core::MapObjectList::parse(payloads_.objects); parsed.ok()) list = std::move(parsed.value());
  const core::sim::World& world = session_->world();
  const std::size_t authored = std::min(list.objects().size(), session_->populated().object_ids.size());
  const auto turned = [&](core::ObjectId id, core::MapObject& object) {
    const auto found = editor_.directions.find(id);
    if (found == editor_.directions.end()) return;
    object.dir_x = found->second.x;
    object.dir_y = found->second.y;
  };
  // The property sheet's edits on the record's own fields; the names and
  // the groups need the object's `num`, which is settled below.
  const auto edited = [&](core::ObjectId id, core::MapObject& object) {
    const auto found = editor_.props.find(id);
    if (found == editor_.props.end()) return;
    const PropertyEdits& edits = found->second;
    if (edits.player) {
      // `player`, and the owner's bit in the low half of `flags`, which the
      // original's save writes from the live object (0x00540a20).
      object.player = *edits.player;
      object.flags = (object.flags & 0xffff0000u) |
                     (*edits.player > 0 && *edits.player <= 16 ? 1u << (*edits.player - 1) : 0u);
    }
    if (edits.health_percent) {
      object.health_percent = *edits.health_percent;
      object.health_absolute = -1;
    }
    if (edits.stamina) object.set_attribute("stamina", std::to_string(*edits.stamina));
    if (edits.level) object.set_attribute("Level", std::to_string(*edits.level));
    object.flags = (object.flags & ~edits.flags_clear) | edits.flags_set;
    object.unit_flags = (object.unit_flags & ~edits.unit_flags_clear) | edits.unit_flags_set;
    if (edits.display_name) {
      if (edits.display_name->empty()) object.drop_attribute("display_name");
      else object.set_attribute("display_name", *edits.display_name);
    }
    if (edits.skills) {
      // A skill the record already names keeps its place; one it does not
      // goes before the tail; one no row names any more is dropped.
      std::vector<std::string> kept;
      for (const auto& [skill, points] : *edits.skills) {
        if (skill == core::sim::HeroSkill::count) continue;
        kept.emplace_back(core::sim::hero_skill_constant(skill));
        object.set_attribute(kept.back(), std::to_string(points));
      }
      std::vector<std::string> gone;
      for (const auto& [name, value] : object.attributes) {
        if (name.size() <= 2 || name[0] != 'h' || name[1] != 's' || core::sim::hero_skill_id(name) < 0) continue;
        if (std::find(kept.begin(), kept.end(), name) == kept.end()) gone.push_back(name);
      }
      for (const std::string& name : gone) object.drop_attribute(name);
    }
    if (edits.icon) {
      if (edits.icon->empty()) object.drop_attribute("Icon");
      else object.set_attribute("Icon", *edits.icon);
    }
    if (edits.items) {
      for (std::size_t slot = 0; slot < 8; ++slot) object.drop_attribute("slot" + std::to_string(slot));
      for (std::size_t slot = 0; slot < edits.items->size(); ++slot) {
        object.set_attribute("slot" + std::to_string(slot), (*edits.items)[slot]);
      }
    }
    if (object.settlement >= 0 && static_cast<std::size_t>(object.settlement) < list.settlements().size()) {
      core::MapSettlement& town = list.settlements_mut()[static_cast<std::size_t>(object.settlement)];
      if (edits.settlement_name) town.name = *edits.settlement_name;
      if (edits.settlement_player) town.player = *edits.settlement_player;
      if (edits.settlement_gold) town.gold = *edits.settlement_gold;
      if (edits.settlement_food) town.food = *edits.settlement_food;
      if (edits.settlement_population) town.population = *edits.settlement_population;
      if (edits.settlement_max_population) town.max_population = *edits.settlement_max_population;
      if (edits.settlement_sentries) town.extra_sentries = *edits.settlement_sentries;
    }
  };
  // Moved: the world's position back onto the authored record, which keeps
  // every other attribute it carried (`sync_attributes` folds the fields in).
  for (std::size_t i = 0; i < authored; ++i) {
    if (editor_.removed.count(static_cast<std::int32_t>(i)) != 0) continue;
    if (const core::sim::WorldObject* slot = world.find(editor_.object_ids[i])) {
      core::MapObject& object = list.objects_mut()[i];
      object.x = slot->state.position.x;
      object.y = slot->state.position.y;
      turned(editor_.object_ids[i], object);
      edited(editor_.object_ids[i], object);
    }
  }
  // Removed: by `num`, which takes the group and area references with it.
  std::vector<std::int32_t> gone;
  for (const std::int32_t index : editor_.removed) {
    if (static_cast<std::size_t>(index) < list.objects().size()) gone.push_back(list.objects()[static_cast<std::size_t>(index)].num);
  }
  for (const std::int32_t num : gone) (void)list.erase(num);
  // Placed: numbered after the last, where they stand now -- a placed
  // object can have been dragged since.
  // Each surviving authored object's `num`, from the loaded list: the
  // erasures above have shifted the edited list's indices.
  std::map<core::ObjectId, std::int32_t> num_of;
  {
    const core::MapObjectList& source = world_.objects();
    for (std::size_t i = 0; i < authored && i < source.objects().size(); ++i) {
      if (editor_.removed.count(static_cast<std::int32_t>(i)) != 0) continue;
      num_of[editor_.object_ids[i]] = source.objects()[i].num;
    }
  }
  for (std::size_t k = 0; k < editor_.added.size(); ++k) {
    core::MapObject object = editor_.added[k];
    const std::size_t at = session_->populated().object_ids.size() + k;
    core::ObjectId id = core::kNoObject;
    if (at < editor_.object_ids.size()) {
      id = editor_.object_ids[at];
      if (const core::sim::WorldObject* slot = world.find(id)) {
        object.x = slot->state.position.x;
        object.y = slot->state.position.y;
      }
      turned(id, object);
      edited(id, object);
    }
    object.num = list.next_num();
    num_of[id] = object.num;
    list.objects_mut().push_back(std::move(object));
  }
  // The explorer's group edits: a type-1 group renamed or deleted by name.
  for (const auto& [from, to] : editor_.group_renames) {
    for (core::MapGroup& group : list.groups_mut()) {
      if (group.type != core::kGroupAlias && group.name == from) group.name = to;
    }
  }
  std::erase_if(list.groups_mut(), [&](const core::MapGroup& group) {
    return group.type != core::kGroupAlias && editor_.groups_deleted.count(group.name) != 0 &&
           editor_.group_renames.count(group.name) == 0;
  });
  // The sheet's names and groups, by `num`: a script name is the type-0
  // group naming the object (renamed, made, or dropped when emptied); a
  // type-1 group is found by name or made, and the member added or taken
  // out -- what `CVXGroup::Add`/`Remove` do to the live table.
  for (const auto& [id, edits] : editor_.props) {
    const auto found = num_of.find(id);
    if (found == num_of.end()) continue;
    const std::int32_t num = found->second;
    if (edits.script_name) {
      bool named = false;
      std::vector<core::MapGroup>& groups = list.groups_mut();
      for (std::size_t g = 0; g < groups.size(); ++g) {
        core::MapGroup& group = groups[g];
        if (group.type != core::kGroupAlias || group.members.size() != 1 || group.members[0] != num) continue;
        if (edits.script_name->empty()) {
          groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(g));
        } else {
          group.name = *edits.script_name;
        }
        named = true;
        break;
      }
      if (!named && !edits.script_name->empty()) {
        core::MapGroup group;
        group.name = *edits.script_name;
        group.type = core::kGroupAlias;
        group.members.push_back(num);
        groups.push_back(std::move(group));
      }
    }
    for (const std::string& name : edits.groups_removed) {
      for (core::MapGroup& group : list.groups_mut()) {
        if (group.type == core::kGroupAlias || group.name != name) continue;
        std::erase(group.members, num);
      }
    }
    for (const std::string& name : edits.groups_added) {
      core::MapGroup* target = nullptr;
      for (core::MapGroup& group : list.groups_mut()) {
        if (group.type != core::kGroupAlias && group.name == name) target = &group;
      }
      if (target == nullptr) {
        core::MapGroup group;
        group.name = name;
        group.type = core::kGroupArmy;
        list.groups_mut().push_back(std::move(group));
        target = &list.groups_mut().back();
      }
      if (std::find(target->members.begin(), target->members.end(), num) == target->members.end()) {
        target->members.push_back(num);
      }
    }
  }
  // The areas: an authored one's shape brought up to date, a placed one's
  // shape added, and each one's name as the type-0 group that names it.
  for (const EditorArea& area : editor_.areas) {
    const auto found = num_of.find(area.id);
    if (found == num_of.end()) continue;
    const std::int32_t num = found->second;
    bool had = false;
    for (core::MapArea& shape : list.areas_mut()) {
      if (shape.num != num) continue;
      shape = area.shape;
      shape.num = num;
      had = true;
    }
    if (!had) {
      core::MapArea shape = area.shape;
      shape.num = num;
      list.areas_mut().push_back(shape);
    }
    bool named = false;
    for (core::MapGroup& group : list.groups_mut()) {
      if (group.type != core::kGroupAlias || group.members.size() != 1 || group.members[0] != num) continue;
      group.name = area.name;
      named = true;
    }
    if (!named && !area.name.empty()) {
      core::MapGroup group;
      group.name = area.name;
      group.type = core::kGroupAlias;
      group.members.push_back(num);
      list.groups_mut().push_back(std::move(group));
    }
  }
  return list;
}

bool Application::editor_save(const std::filesystem::path& out) {
  std::string error;
  const core::MapObjectList document = editor_document();
  // The object list from the editor's tables, and the six layers as the
  // loaded map holds them -- painted or not. A layer the container did
  // not carry is an empty grid and is left to the source rather than
  // invented. The terrain goes back at eight bits whatever it came in at,
  // as the original writes it (0x0054a260).
  gamedata::MapEdits edits;
  edits.objects = &document;
  const auto present = [](const core::Grid& grid) -> const core::Grid* {
    return grid.cell_size() != 0 ? &grid : nullptr;
  };
  edits.pass = present(world_.passability());
  edits.height = present(world_.height());
  edits.light = present(world_.light());
  edits.terrain = present(world_.terrain());
  edits.decor = present(world_.decor());
  edits.trans = present(world_.transitions());
  for (const auto& [path, text] : editor_.documents) {
    // A document read and found missing -- a map with no `labels.xml` --
    // is not written back as an empty file; one this run made is.
    if (text.empty() && !container_.contains(path) && editor_.created.count(path) == 0) continue;
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    edits.documents.emplace_back(path, std::move(bytes));
  }
  edits.removed_directories.assign(editor_.removed_maps.begin(), editor_.removed_maps.end());
  if (!gamedata::write_map_container(out, container_.path(),
                                     SDL_atoi(gamedata::map_number(map_directory_).c_str()), edits,
                                     &error)) {
    std::printf("editor: %s not written: %s\n", out.string().c_str(), error.c_str());
    return false;
  }
  std::printf("editor: wrote %s (%zu objects, %zu areas)\n", out.string().c_str(), document.objects().size(),
              document.areas().size());
  return true;
}

void Application::open_editor_save_dialog() {
  core::ui::Dialog* dialog = open_menu(
      "editorini/SaveDlg.ini", [this](const core::ui::DialogEvent& event, core::ui::Dialog& menu) {
        using Kind = core::ui::DialogEvent::Kind;
        if (event.kind != Kind::kCommand) return;
        if (event.id == 0x1003) {
          close_menu();
          return;
        }
        if (event.id == 0x1001) {
          std::string name = menu.text("NameEdit");
          while (!name.empty() && name.back() == ' ') name.pop_back();
          if (name.empty()) return;
          if (name.size() < 5 || name.substr(name.size() - 5) != ".bfhp") name += ".bfhp";
          const std::filesystem::path out = editor_maps_directory() / name;
          // Never over a shipped container: the installation is read, not
          // written, and the default filesystem folds case, so `Crossroads`
          // would replace `Crossroads.BFHP`. A name this engine wrote before
          // may be written again.
          std::error_code ignored;
          if (std::filesystem::exists(out, ignored)) {
            const std::string folded_out = fold_name(out.filename().string());
            const bool shipped = folded_out == fold_name(container_.path().filename().string()) ||
                                 (!editor_.origin_stem.empty() && folded_out == fold_name(editor_.origin_stem + ".bfhp")) ||
                                 std::any_of(listed_containers_.begin(), listed_containers_.end(),
                                             [&](const std::filesystem::path& listed) {
                                               return fold_name(listed.filename().string()) == folded_out;
                                             });
            if (shipped && !written_by_editor_.count(folded_out)) {
              std::printf("editor: %s is a shipped map and is not overwritten; choose another name\n",
                          out.string().c_str());
              return;
            }
          }
          std::filesystem::create_directories(out.parent_path(), ignored);
          if (editor_save(out)) {
            written_by_editor_.insert(fold_name(out.filename().string()));
            close_menu();
          }
        }
      });
  if (dialog == nullptr) return;
  std::vector<std::string> names;
  std::error_code ignored;
  for (const auto& entry : std::filesystem::directory_iterator(editor_maps_directory(), ignored)) {
    if (entry.is_regular_file()) names.push_back(entry.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  dialog->set_items("List", std::move(names));
  dialog->set_text("NameEdit", (editor_.origin_stem.empty() ? container_.path().stem().string() : editor_.origin_stem) + " edited");
  dialog->focus("NameEdit");
}

/// Point the opening camera at the local player's first holding.
///
/// The lowest-id object they own, which on every shipped map is authored before
/// their units and is in practice their town hall. Lowest id rather than a
/// centroid because a centroid of split holdings lands between them, and
/// because ascending object id is the one order that does not depend on how the
/// map happened to be walked. Leaves the default alone, and says so, when the
/// player owns nothing, which is the honest answer for an observer slot.
bool Application::look_at_local_player() {
  const core::sim::World& world = session_->world();
  for (const core::sim::WorldObject& object : world.objects()) {
    // An internal object -- a settlement record, a holder, a warehouse -- is
    // owned too, and sits at 0, 0: it is bookkeeping, not something standing
    // on the map.
    if (object.internal != core::sim::InternalKind::none) continue;
    // Nor a spawn template, which is placed but not in play, nor a script's
    // `AdvArea`, which is owned and invisible: Zama gives the Romans 55.
    if (object.state.flags.unspawned) continue;
    if (!object.state.flags.is_unit && !object.state.flags.is_building) continue;
    if (object.state.owner != local_player_ || object.state.is_held()) continue;
    look_x_ = object.state.position.x;
    look_y_ = object.state.position.y;
    return true;
  }
  return false;
}

void Application::tick_map(platform::Window::Frame& frame) {
  if (!target_.ensure(window_.device(), frame.width, frame.height)) return;

  if (look_pending_) {
    map_.look_at(look_x_, look_y_, static_cast<std::int32_t>(frame.width),
                 static_cast<std::int32_t>(frame.height));
    look_pending_ = false;
  }
  map_.clamp_view(static_cast<std::int32_t>(frame.width),
                  static_cast<std::int32_t>(frame.height));
  map_.draw_terrain(frame.commands, target_.texture(), frame.width, frame.height);

  renderer_.begin(frame.width, frame.height);
  map_.queue_objects(frame.width, frame.height);
  // No clear: the ground is already in the target and the sprites load over it.
  renderer_.render(frame.commands, target_.texture(), nullptr);
  target_.blit_to(frame.commands, frame.swapchain, frame.width, frame.height);
}

// --------------------------------------------------------------------------
// sheet mode
// --------------------------------------------------------------------------

bool Application::start_sheet() {
  std::string error;
  const auto load = [&](const std::string& path, core::RleImage& image,
                        platform::Sprite& sprite) -> bool {
    const platform::ByteSpan table = vfs_.read(path);
    if (table.empty()) {
      std::fprintf(stderr, "%s: not in any mounted pack\n", path.c_str());
      return false;
    }
    auto parsed = core::RleImage::parse(platform::as_core_bytes(table));
    if (!parsed) {
      std::fprintf(stderr, "%s: not a sprite frame table (error %d)\n", path.c_str(),
                   static_cast<int>(parsed.error()));
      return false;
    }
    image = std::move(parsed.value());
    if (!platform::upload_sprite(renderer_, image, vfs_.pixel_store(), sprite, &error)) {
      std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
      return false;
    }
    std::printf("%s: %ux%u frames, class %u, palette %zu entries, transparent index %u\n",
                path.c_str(), sprite.columns, sprite.rows,
                static_cast<unsigned>(image.raw_image_class()), image.palette_size(),
                sprite.transparent_index);
    return true;
  };

  if (!load(args_.sprite, body_image_, body_)) return false;
  const bool have_shadow = !args_.shadow.empty() && vfs_.contains(args_.shadow) &&
                           load(args_.shadow, shadow_image_, shadow_);

  // Team colour: one extra palette row per player, over the same index texels.
  // Four players cost four kilobytes and no extra atlas at all -- which is the
  // entire argument for the index-plus-lookup design.
  for (const core::Rgb888& team : args_.teams) {
    team_palettes_.push_back(platform::upload_team_palette(renderer_, body_image_, team,
                                                           body_.transparent_index));
  }

  if (!renderer_.commit_uploads(&error)) {
    std::fprintf(stderr, "upload: %s\n", error.c_str());
    return false;
  }
  std::printf("atlas: %zu page(s), %.1f MiB of R8 indices\n", renderer_.atlas_pages(),
              static_cast<double>(renderer_.atlas_bytes()) / (1024.0 * 1024.0));

  // The body and its shadow share one canvas, which is exactly why they
  // composite by bounding box with no alignment arithmetic. Size the sheet's
  // cell to the union of the two.
  canvas_left_ = body_.canvas_left;
  canvas_top_ = body_.canvas_top;
  std::int32_t right = canvas_left_ + static_cast<std::int32_t>(body_.canvas_width);
  std::int32_t bottom = canvas_top_ + static_cast<std::int32_t>(body_.canvas_height);
  if (have_shadow && shadow_.canvas_width != 0) {
    canvas_left_ = std::min(canvas_left_, shadow_.canvas_left);
    canvas_top_ = std::min(canvas_top_, shadow_.canvas_top);
    right = std::max(right,
                     shadow_.canvas_left + static_cast<std::int32_t>(shadow_.canvas_width));
    bottom = std::max(bottom,
                      shadow_.canvas_top + static_cast<std::int32_t>(shadow_.canvas_height));
  }
  cell_width_ = static_cast<std::uint32_t>(right - canvas_left_);
  cell_height_ = static_cast<std::uint32_t>(bottom - canvas_top_);

  sheet_width_ = cell_width_ * std::max(body_.columns, 1u);
  const std::uint32_t sheet_rows =
      args_.players ? static_cast<std::uint32_t>(team_palettes_.size())
                    : (args_.animate ? 1u : std::max(body_.rows, 1u));
  sheet_height_ = cell_height_ * sheet_rows;

  std::printf("sheet: %ux%u px, cell %ux%u\n", sheet_width_, sheet_height_, cell_width_,
              cell_height_);
  return true;
}

void Application::draw_sheet() {
  // Cells the size of the shared canvas, frames placed at their own canvas
  // coordinates inside them, so the animation reads down a column exactly as
  // the engine moves it.
  const std::uint32_t rows = args_.players
                                 ? static_cast<std::uint32_t>(team_palettes_.size())
                                 : (args_.animate ? 1u : body_.rows);
  const std::uint32_t step =
      args_.animate && body_.rows != 0
          ? static_cast<std::uint32_t>(frame_number_ / 4 % body_.rows)
          : 0;

  for (std::uint32_t row = 0; row < rows; ++row) {
    // In the players demo every row is the same animation step drawn through a
    // different palette row. Same atlas texels, four different armies.
    const platform::PaletteRow palette =
        team_palettes_[args_.players ? row : 0];
    for (std::uint32_t column = 0; column < body_.columns; ++column) {
      const std::uint32_t source_row = args_.animate ? step : row;
      const float cell_x = static_cast<float>(column * cell_width_);
      const float cell_y = static_cast<float>(row * cell_height_);

      // Shadow first: a darkening pass under the body, drawn through the
      // two-entry mask palette with the alpha carried by the modulate.
      if (const platform::SpriteFrame* frame = shadow_.at(source_row, column);
          frame != nullptr && frame->region.valid()) {
        renderer_.draw(frame->region,
                       cell_x + static_cast<float>(frame->left - canvas_left_),
                       cell_y + static_cast<float>(frame->top - canvas_top_),
                       shadow_.neutral, platform::Rgba{1.0F, 1.0F, 1.0F, 0.5F});
      }
      if (const platform::SpriteFrame* frame = body_.at(source_row, column);
          frame != nullptr && frame->region.valid()) {
        renderer_.draw(frame->region,
                       cell_x + static_cast<float>(frame->left - canvas_left_),
                       cell_y + static_cast<float>(frame->top - canvas_top_), palette);
      }
    }
  }
}

void Application::tick_sheet(platform::Window::Frame& frame) {
  // The sheet is rendered at its own native size and scaled onto the window,
  // so a screenshot is pixel-exact and comparable with the reference export.
  if (target_.ensure(window_.device(), sheet_width_, sheet_height_)) {
    renderer_.begin(sheet_width_, sheet_height_);
    draw_sheet();
    const platform::Rgba clear{0.16F, 0.14F, 0.20F, 1.0F};
    renderer_.render(frame.commands, target_.texture(), &clear);
    target_.blit_to(frame.commands, frame.swapchain, frame.width, frame.height);
  }
}

// --------------------------------------------------------------------------

void Application::take_screenshot(const std::string& path) {
  std::vector<std::uint8_t> rgba;
  std::string error = "no render target";
  if (target_.download(rgba, &error) &&
      platform::save_png(path.c_str(), rgba.data(), target_.width(), target_.height(), &error)) {
    std::printf("screenshot: %s (%ux%u) at turn %llu\n", path.c_str(), target_.width(),
                target_.height(),
                session_ != nullptr ? static_cast<unsigned long long>(session_->report().turns) : 0ULL);
    if (session_ != nullptr && world_view_.ready()) {
      // What the shot holds of the live world, and what it could not: an
      // object whose art did not resolve is left out of the draw and the pick
      // alike, and a refused palette row is art that did not resolve.
      const platform::WorldView::Stats& drawn = world_view_.stats();
      std::printf("drawn:      %zu object(s) in view in %zu layer(s), %zu without art; "
                  "palette %u of %u rows, %zu shared, %zu refused\n",
                  drawn.drawable - drawn.culled, drawn.layers, drawn.no_art,
                  renderer_.palette_rows(), platform::SpriteRenderer::palette_capacity(),
                  renderer_.palette_rows_shared(), renderer_.palette_rows_refused());
      // And what the shot holds over the world: the band, the rings under
      // what is selected, and the health bars with the mode that shows them.
      std::printf("overlays:   band %d, rings %zu, bars %zu (mode %d)\n", band_drawn_ ? 1 : 0,
                  drawn.rings, drawn.bars, bar_mode_);
    }
  } else {
    std::fprintf(stderr, "screenshot failed: %s\n", error.c_str());
  }
}

/// One step of `--input` per frame, posted as the events a mouse and a
/// keyboard would produce. `click:X,Y` is a left press and release at the
/// point (with a move first, so hover is right), `key:NAME` an SDL key by
/// name, `text:S` typed characters, `move:X,Y` the pointer alone (for a
/// tooltip), `rclick:X,Y` the right button, `mod:shift` (or `ctrl`,
/// `ctrl+shift`, empty for none) the modifier keys held from then on,
/// `drag:X1,Y1,X2,Y2` the left
/// button held from one point to another, `wait:N` a pause of N frames,
/// `shot:PATH` a screenshot of this frame, so that one run can show a world
/// before and after an order rather than only where it ended. `turn:N`
/// waits for the world's N-th turn and `over` for the match to be decided
/// (a decided match releases `turn:N` too: its end-game screen stops the
/// clock), which is how a scripted run shoots a moment of the game rather
/// than a moment of the machine. The turn loop stops on turn N, so what
/// follows sees exactly it; `hash` prints the world's hash there.
void Application::play_input() {
  if (args_.input.empty() || input_at_ >= args_.input.size() || frame_number_ < 2) return;
  if (frame_number_ < input_wait_until_) return;
  if (input_until_turn_ > 0) {
    if (session_ != nullptr && !reported_outcome_ && session_->world().turns() < input_until_turn_) return;
    input_until_turn_ = 0;
  }
  if (input_until_over_) {
    if (session_ != nullptr && !reported_outcome_) return;
    input_until_over_ = false;
  }
  std::size_t end = args_.input.find(';', input_at_);
  if (end == std::string::npos) end = args_.input.size();
  const std::string step = args_.input.substr(input_at_, end - input_at_);
  input_at_ = end + 1;
  const std::size_t colon = step.find(':');
  const std::string verb = step.substr(0, colon);
  const std::string arg = colon == std::string::npos ? std::string() : step.substr(colon + 1);
  SDL_Event event{};
  if (verb == "move") {
    int x = 0;
    int y = 0;
    if (SDL_sscanf(arg.c_str(), "%d,%d", &x, &y) != 2) return;
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = static_cast<float>(x);
    event.motion.y = static_cast<float>(y);
    SDL_PushEvent(&event);
  } else if (verb == "click") {
    int x = 0;
    int y = 0;
    if (SDL_sscanf(arg.c_str(), "%d,%d", &x, &y) != 2) return;
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = static_cast<float>(x);
    event.motion.y = static_cast<float>(y);
    SDL_PushEvent(&event);
    event = SDL_Event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = static_cast<float>(x);
    event.button.y = static_cast<float>(y);
    SDL_PushEvent(&event);
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    SDL_PushEvent(&event);
  } else if (verb == "rclick") {
    // `rclick:X,Y`: a right click there -- the default order, which is how a
    // scripted run gives one without knowing what is under the pointer.
    int x = 0;
    int y = 0;
    if (SDL_sscanf(arg.c_str(), "%d,%d", &x, &y) != 2) return;
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = static_cast<float>(x);
    event.motion.y = static_cast<float>(y);
    SDL_PushEvent(&event);
    event = SDL_Event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_RIGHT;
    event.button.x = static_cast<float>(x);
    event.button.y = static_cast<float>(y);
    SDL_PushEvent(&event);
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    SDL_PushEvent(&event);
  } else if (verb == "drag") {
    // `drag:X1,Y1,X2,Y2`: the left button down at the first point, the
    // pointer to the second, and up there.
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    if (SDL_sscanf(arg.c_str(), "%d,%d,%d,%d", &x1, &y1, &x2, &y2) != 4) return;
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = static_cast<float>(x1);
    event.motion.y = static_cast<float>(y1);
    SDL_PushEvent(&event);
    event = SDL_Event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = static_cast<float>(x1);
    event.button.y = static_cast<float>(y1);
    SDL_PushEvent(&event);
    event = SDL_Event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = static_cast<float>(x2);
    event.motion.y = static_cast<float>(y2);
    event.motion.xrel = static_cast<float>(x2 - x1);
    event.motion.yrel = static_cast<float>(y2 - y1);
    SDL_PushEvent(&event);
    event = SDL_Event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = static_cast<float>(x2);
    event.button.y = static_cast<float>(y2);
    SDL_PushEvent(&event);
  } else if (verb == "hold" || verb == "release") {
    // `hold:X1,Y1,X2,Y2` is `drag` with the button still down at the end, so
    // that a `shot:` can see the band; `release:X,Y` lets it go there.
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
    if (verb == "release") {
      if (SDL_sscanf(arg.c_str(), "%d,%d", &x2, &y2) != 2) return;
      event.type = SDL_EVENT_MOUSE_BUTTON_UP;
      event.button.button = SDL_BUTTON_LEFT;
      event.button.x = static_cast<float>(x2);
      event.button.y = static_cast<float>(y2);
      SDL_PushEvent(&event);
      return;
    }
    if (SDL_sscanf(arg.c_str(), "%d,%d,%d,%d", &x1, &y1, &x2, &y2) != 4) return;
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = static_cast<float>(x1);
    event.motion.y = static_cast<float>(y1);
    SDL_PushEvent(&event);
    event = SDL_Event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.button = SDL_BUTTON_LEFT;
    event.button.x = static_cast<float>(x1);
    event.button.y = static_cast<float>(y1);
    SDL_PushEvent(&event);
    event = SDL_Event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = static_cast<float>(x2);
    event.motion.y = static_cast<float>(y2);
    event.motion.xrel = static_cast<float>(x2 - x1);
    event.motion.yrel = static_cast<float>(y2 - y1);
    SDL_PushEvent(&event);
  } else if (verb == "key") {
    // `key:ctrl+Z`, `key:shift+alt+F2`: the modifiers ride in the event's
    // own field, the way a held key's would.
    event.type = SDL_EVENT_KEY_DOWN;
    std::string name = arg;
    for (std::size_t plus = name.find('+'); plus != std::string::npos && plus + 1 < name.size();
         plus = name.find('+')) {
      const std::string modifier = name.substr(0, plus);
      if (modifier == "ctrl") event.key.mod |= SDL_KMOD_CTRL;
      else if (modifier == "alt") event.key.mod |= SDL_KMOD_ALT;
      else if (modifier == "shift") event.key.mod |= SDL_KMOD_SHIFT;
      else break;
      name = name.substr(plus + 1);
    }
    event.key.key = SDL_GetKeyFromName(name.c_str());
    if (event.key.key == SDLK_UNKNOWN) return;
    SDL_PushEvent(&event);
  } else if (verb == "mod") {
    // `mod:shift`, `mod:ctrl+shift`, `mod:` for none: the modifier keys held
    // from this step on, as the mouse reads them -- so that `mod:shift;
    // rclick:X,Y` is a Shift right click, which appends.
    SDL_Keymod held = SDL_KMOD_NONE;
    std::size_t from = 0;
    while (from < arg.size()) {
      const std::size_t plus = arg.find('+', from);
      const std::string key = arg.substr(from, plus == std::string::npos ? std::string::npos : plus - from);
      if (key == "ctrl") held |= SDL_KMOD_LCTRL;
      else if (key == "shift") held |= SDL_KMOD_LSHIFT;
      else if (key == "alt") held |= SDL_KMOD_LALT;
      if (plus == std::string::npos) break;
      from = plus + 1;
    }
    SDL_SetModState(held);
    std::printf("input:        holding%s%s%s\n", (held & SDL_KMOD_CTRL) != 0 ? " ctrl" : "",
                (held & SDL_KMOD_SHIFT) != 0 ? " shift" : "", (held & SDL_KMOD_ALT) != 0 ? " alt" : "");
  } else if (verb == "text") {
    event.type = SDL_EVENT_TEXT_INPUT;
    // The event's text is the caller's to keep alive; a static copy is.
    static std::string typed;
    typed = arg;
    event.text.text = typed.c_str();
    SDL_PushEvent(&event);
  } else if (verb == "press") {
    // `press:LabelNew`, `press:Browser@3`, `press:Browser@=Map`: a click
    // on the named widget of the topmost dialog that has it -- its centre,
    // or the centre of a list's n-th row, or of the row whose text (less a
    // tree's fold marks) is the one given. A script that names widgets
    // outlives a layout change; one that names pixels does not.
    // `press:Queue@1>500,380` goes down on the bar's cell and comes up at
    // the point given, off it: a press that is not a click.
    std::string target_spec = arg;
    int up_x = -1;
    int up_y = -1;
    if (const std::size_t off = target_spec.find('>'); off != std::string::npos) {
      if (SDL_sscanf(target_spec.c_str() + off + 1, "%d,%d", &up_x, &up_y) != 2) up_x = up_y = -1;
      target_spec.resize(off);
    }
    std::string widget = target_spec;
    std::string row;
    if (const std::size_t at = target_spec.find('@'); at != std::string::npos) {
      widget = target_spec.substr(0, at);
      row = target_spec.substr(at + 1);
    }
    for (std::size_t i = ui_.dialog_count(); i-- > 0;) {
      core::ui::Dialog* dialog = ui_.dialog(i);
      if (dialog == nullptr || dialog->screen().find(widget) == nullptr) continue;
      core::ui::Rect target = dialog->widget_rect(widget);
      if (!row.empty()) {
        std::int32_t index = -1;
        if (row[0] == '=') {
          const core::ui::WidgetState* state = dialog->content().state_of(widget);
          for (std::size_t r = 0; state != nullptr && r < state->items.size(); ++r) {
            std::string_view text = state->items[r];
            while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
            if (text.size() >= 2 && (text[0] == '+' || text[0] == '-') && text[1] == ' ') text.remove_prefix(2);
            if (text == std::string_view(row).substr(1)) {
              index = static_cast<std::int32_t>(r);
              break;
            }
          }
        } else {
          index = SDL_atoi(row.c_str());
        }
        target = dialog->item_rect(widget, index);
      }
      if (target.width <= 0 || target.height <= 0) {
        std::printf("input: %s has no %s to press\n", dialog->screen().path.c_str(), arg.c_str());
        return;
      }
      const core::ui::Rect where = dialog->rect();
      const int x = where.x + target.x + target.width / 2;
      int y = where.y + target.y + target.height / 2;
      // A combobox's rectangle is its dropped list's; the closed box is
      // the strip at its top, and that is what a press on it means.
      if (const core::ui::Widget* w = dialog->screen().find(widget);
          w != nullptr && row.empty() &&
          (w->kind == core::ui::WidgetType::kCombobox || w->kind == core::ui::WidgetType::kPlayerCombobox)) {
        y = where.y + target.y + 8;
      }
      event.type = SDL_EVENT_MOUSE_MOTION;
      event.motion.x = static_cast<float>(x);
      event.motion.y = static_cast<float>(y);
      SDL_PushEvent(&event);
      event = SDL_Event{};
      event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
      event.button.button = SDL_BUTTON_LEFT;
      event.button.x = static_cast<float>(x);
      event.button.y = static_cast<float>(y);
      SDL_PushEvent(&event);
      event.type = SDL_EVENT_MOUSE_BUTTON_UP;
      SDL_PushEvent(&event);
      return;
    }
    // Not a dialog's: a bar's -- `press:Diplomacy`, or `press:Holder@0` for a
    // strip's cell (a garrison portrait), aimed where it is drawn.
    core::ui::Rect bar_target;
    if (!row.empty()) {
      bar_target = ui_.strip_cell_rect(widget, static_cast<std::size_t>(std::max(0, SDL_atoi(row.c_str()))));
    } else {
      bar_target = ui_.bar_widget_rect(widget);
    }
    if (bar_target.width > 0 && bar_target.height > 0) {
      const int x = bar_target.x + bar_target.width / 2;
      const int y = bar_target.y + bar_target.height / 2;
      event.type = SDL_EVENT_MOUSE_MOTION;
      event.motion.x = static_cast<float>(x);
      event.motion.y = static_cast<float>(y);
      SDL_PushEvent(&event);
      event = SDL_Event{};
      event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
      event.button.button = SDL_BUTTON_LEFT;
      event.button.x = static_cast<float>(x);
      event.button.y = static_cast<float>(y);
      SDL_PushEvent(&event);
      if (up_x >= 0 && up_y >= 0) {
        event = SDL_Event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.x = static_cast<float>(up_x);
        event.motion.y = static_cast<float>(up_y);
        SDL_PushEvent(&event);
        event = SDL_Event{};
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = static_cast<float>(up_x);
        event.button.y = static_cast<float>(up_y);
      }
      event.type = SDL_EVENT_MOUSE_BUTTON_UP;
      SDL_PushEvent(&event);
      return;
    }
    std::printf("input: no open dialog or bar has %s\n", arg.c_str());
  } else if (verb == "select" && session_ != nullptr) {
    // `select:class:RVillage`: the selection becomes what `--select` would
    // make of the same words, now -- a building the units just entered.
    core::sim::Selection& selection = session_->selections().player(local_player_);
    selection.clear();
    for (const core::ObjectId id : resolve_objects(arg)) (void)selection.add(id);
    session_->selections().note_selection_changed(local_player_, session_->scheduler().now());
    play_select_sounds({});
    bars_dirty_ = true;
    std::printf("selected:     %zu object(s) from select:%s\n", selection.size(), arg.c_str());
  } else if (verb == "look" && session_ != nullptr) {
    // `look:class:RVillage`: the camera centred on the first object those
    // words name, as the opening look centres it, so that a click at the
    // middle of the view lands on it.
    const std::vector<core::ObjectId> found = resolve_objects(arg);
    const core::sim::WorldObject* slot = found.empty() ? nullptr : session_->world().find(found.front());
    if (slot == nullptr || slot->state.is_held()) {
      std::printf("input: look:%s names nothing on the map\n", arg.c_str());
    } else {
      look_x_ = slot->state.position.x;
      look_y_ = slot->state.position.y;
      look_pending_ = true;
      std::printf("view:         object %u at %d, %d\n", static_cast<unsigned>(slot->id), look_x_, look_y_);
    }
  } else if (verb == "shot") {
    pending_shot_ = arg;
  } else if (verb == "wait") {
    input_wait_until_ = frame_number_ + static_cast<std::uint64_t>(std::max(0, SDL_atoi(arg.c_str())));
  } else if (verb == "turn") {
    input_until_turn_ = static_cast<std::uint64_t>(std::max(0, SDL_atoi(arg.c_str())));
  } else if (verb == "over") {
    input_until_over_ = true;
  } else if (verb == "hash") {
    // The world's hash on this turn, as `imrun`'s `match` block prints it for
    // the turn it stopped at: `turn:N;hash` holds the two side by side.
    if (session_ != nullptr) {
      std::printf("hash:         at turn %llu, hash %016llx\n",
                  static_cast<unsigned long long>(session_->world().turns()),
                  static_cast<unsigned long long>(session_->report().hash));
      std::fflush(stdout);
    }
  }
}

bool Application::tick() {
#if IMPERIVM_HAVE_NET
  if (front_mode_) step_net_lobby();
  if (net_ != nullptr && play_mode_ && session_ != nullptr) {
    // Chat heard: shown when it is for this seat, as this seat's world says,
    // and said on the console either way for a script to read.
    for (const core::sim::ChatLine& line : net_->take_chat()) {
      const char* to = line.to == core::sim::ChatLine::To::all      ? "all"
                       : line.to == core::sim::ChatLine::To::allies ? "allies"
                                                                    : "player";
      std::printf("chat      player %u to %s: %s\n", static_cast<unsigned>(line.from), to,
                  line.text.c_str());
      std::fflush(stdout);
      if (!core::sim::shown_to(line, net_->seat(), session_->world().players())) continue;
      std::string text = net_->name_of(line.from) + ": " + line.text;
      if (line.to == core::sim::ChatLine::To::allies) text += " (allies)";
      if (line.located) {
        text += " (at " + std::to_string(line.location.x) + ", " + std::to_string(line.location.y) + ")";
      }
      show_chat(std::move(text));
    }
    refresh_chat();
  }
  if (net_ != nullptr && play_mode_) {
    // A player who left is dropped from an agreed turn and the match goes
    // on, the computer in their seat (`sim/netdepart.hpp`). Said, and
    // dismissed.
    // A late joiner taking a seat: said in the corner, as a line is -- the
    // match does not stop for it, and waits only if the save is slow.
    for (std::string& news : net_->take_join_news()) show_chat(std::move(news));
    for (const imperivm::net::NetDeparture& gone : net_->take_departures()) {
      const std::string who = net_->name_of(gone.player);
      open_multi_box(who + " left the game (" + core::sim::describe(gone.reason) +
                         "). The match goes on; the computer takes their seat.",
                     [this] { close_menu(); });
    }
    // The turn the computer took a seat on: the original's line, then its
    // sound, once for the turn however many seats went (0x00406840). The
    // name the line gives is the one before the marker, as there: it prints,
    // then appends. A seat handed back to a late joiner says nothing more
    // than the join already did, and plays nothing.
    for (const core::sim::ComputerSeats::Change& change : net_->take_seat_news()) {
      if (change.taken.empty()) continue;
      const std::string_view dropped =
          translations_ != nullptr ? translations_->translate("Player %s1 dropped") : "Player %s1 dropped";
      for (const core::PlayerId seat : change.taken) {
        const std::string name[] = {net_->name_of(seat)};
        show_chat(core::game::substitute(dropped, name));
      }
      play_sound("Sounds/UI/PlayerDropped.wav");
    }
  }
  if (net_ != nullptr && play_mode_ && !net_departed_shown_) {
    if (const auto ended = net_->ended(); ended.has_value()) {
      net_departed_shown_ = true;
      open_multi_box(std::string("The match cannot go on: ") + core::sim::describe(*ended) + ".",
                     [this] {
                       close_all_menus();
                       if (args_.from_front) {
                         return_to_front();
                       } else {
                         quit_requested_ = true;
                       }
                     });
    }
  }
#endif
  play_input();
  tick_credits();
  // Panning: the held key state rather than key repeats, so the camera glides
  // instead of stuttering at the repeat rate.
  // Not in the editor, where the shipped table binds the arrows to the tools.
  if (const bool* keys = SDL_GetKeyboardState(nullptr); map_mode_ && !editor_.active && keys != nullptr) {
    // The options' scroll speed: 4 to 32 pixels a frame, shift trebles it.
    const int base = 4 + std::clamp(settings_.scroll_speed, 0, 100) * 28 / 100;
    const int speed = (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) ? base * 3 : base;
    int dx = 0;
    int dy = 0;
    if (keys[SDL_SCANCODE_LEFT]) dx -= speed;
    if (keys[SDL_SCANCODE_RIGHT]) dx += speed;
    if (keys[SDL_SCANCODE_UP]) dy -= speed;
    if (keys[SDL_SCANCODE_DOWN]) dy += speed;
    if (dx != 0 || dy != 0) map_.move_view(dx, dy);
  }

  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_EVENT_QUIT) return false;
    // Space is the zoom map's, down and up, ahead of the map's own dialog;
    // any other menu keeps it.
    if (play_mode_ && (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
        event.key.key == SDLK_SPACE && !editor_.active &&
        (zoom_open_ || paused_menu_ || ui_.dialog_count() == 0)) {
      zoom_key(event);
      continue;
    }
    // Shift released: the `UIHolder` portraits it marked are selected (0x006d34f0).
    if (play_mode_ && event.type == SDL_EVENT_KEY_UP && !holder_marks_.empty() &&
        (event.key.key == SDLK_LSHIFT || event.key.key == SDLK_RSHIFT)) {
      apply_holder_marks();
    }
    // An open menu takes the input first -- except the pause word, which
    // takes nothing, so that Space lifts it and the game's keys still work.
    if ((play_mode_ || front_mode_) && ui_.dialog_count() > 0 && !paused_menu_ && menus_take(event)) {
      if (quit_requested_) return false;
      continue;
    }
    if (front_mode_) {
      if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F11) {
        args_.fullscreen = !args_.fullscreen;
        window_.set_fullscreen(args_.fullscreen);
      }
      continue;
    }
    // The backtick: the health bars' mode. The physical key left of 1, which
    // is what `VK_OEM_3` names on the layout the game shipped for.
    if (play_mode_ && !editor_.active &&
        (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
        (event.key.key == SDLK_GRAVE || event.key.scancode == SDL_SCANCODE_GRAVE)) {
      bar_key(event);
      continue;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && editor_.active) {
      // The editor's keys are the shipped `vxAction.xml`'s (and F2 saves,
      // which is this engine's); a key the table does not name falls
      // through to the map's.
      if (editor_key(event.key)) {
        if (quit_requested_) return false;
        continue;
      }
    }
    if (event.type == SDL_EVENT_KEY_DOWN) {
      if (event.key.key == SDLK_ESCAPE) {
        if (!pending_command_.empty()) {
          pending_command_.clear();
          bars_dirty_ = true;
          continue;
        }
        // In play, Escape opens the game menu; quitting is the menu's Quit.
        if (play_mode_ && session_ != nullptr) {
          open_game_menu();
          continue;
        }
        return false;
      }
      if (event.key.key == SDLK_F10 && play_mode_) {
        open_game_menu();
        continue;
      }
#if IMPERIVM_HAVE_NET
      // The command bar's `Chat (Enter)`: in a networked match, the key.
      if ((event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) && play_mode_ &&
          networked()) {
        open_ingame_chat();
        continue;
      }
#endif
      if (event.key.key == SDLK_F8 && play_mode_) {
        open_notes_menu();
        continue;
      }
      if (event.key.key == SDLK_F7 && play_mode_) {
        select_party();
        continue;
      }
      if (event.key.key == SDLK_F1 && play_mode_) {
        open_help();
        continue;
      }
      // F5 is the bar's `Diplomacy (F5)` and, before the bars, the quick save;
      // the save keeps the key and the menu has its button.
      if (event.key.key == SDLK_F11) {
        args_.fullscreen = !args_.fullscreen;
        window_.set_fullscreen(args_.fullscreen);
      }
      if (play_mode_ && networked() &&
          (event.key.key == SDLK_P || event.key.key == SDLK_PAUSE || event.key.key == SDLK_PERIOD ||
           event.key.key == SDLK_F5 || event.key.key == SDLK_F9)) {
        // One player cannot stop, step, save or rewind everybody's game.
        std::printf("net: not in a networked match\n");
        continue;
      }
      if (play_mode_) {
        if (event.key.key == SDLK_P || event.key.key == SDLK_PAUSE) {
          paused_ = !paused_;
          // Owed time is dropped on pause rather than banked, so unpausing
          // does not fast-forward through however long you were reading.
          owed_ms_ = 0.0;
          // `PAUSED.INI`, the word over the world while the clock stands.
          if (paused_ && ui_.dialog_count() == 0) {
            open_menu("menuini/paused.ini", [](const core::ui::DialogEvent&, core::ui::Dialog&) {});
            paused_menu_ = true;
          } else if (!paused_ && paused_menu_) {
            close_all_menus();
            paused_menu_ = false;
          }
        }
        // Stepping one turn at a time is how a divergence gets looked at.
        if (event.key.key == SDLK_PERIOD && paused_ && session_ != nullptr) run_local_turn();
        if (event.key.key == SDLK_F5 && session_ != nullptr) (void)quicksave();
        // A row's `key=` letter presses its button; Escape cancels a row
        // waiting for its target rather than quitting.
        if (event.key.key >= SDLK_A && event.key.key <= SDLK_Z && cmdbar_ != nullptr) {
          const char letter = static_cast<char>('a' + (event.key.key - SDLK_A));
          for (std::size_t i = 0; i < buttons_.size(); ++i) {
            const std::string& key = buttons_[i].key;
            if (key.size() == 1 && (key[0] == letter || key[0] == static_cast<char>(letter - 32))) {
              // The keys held with this one: its own event's, so that a
              // scripted `key:ctrl+s` holds Ctrl as a player's would.
              press_button(static_cast<std::int32_t>(i),
                           core::sim::CommandBar::Keys{(event.key.mod & SDL_KMOD_SHIFT) != 0,
                                                       (event.key.mod & SDL_KMOD_CTRL) != 0});
              break;
            }
          }
        }
        // A load that fails leaves no usable session behind, so the app ends
        // rather than drawing a game that never existed.
        if (event.key.key == SDLK_F9 && session_ != nullptr && !quickload()) return false;
      }
    }
    if (play_mode_) {
      handle_play_mouse(event);
    } else if (map_mode_) {
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) dragging_ = true;
      if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) dragging_ = false;
      if (event.type == SDL_EVENT_MOUSE_MOTION && dragging_) {
        // Drag the world, not the camera: the map follows the cursor.
        map_.move_view(-static_cast<std::int32_t>(event.motion.xrel),
                       -static_cast<std::int32_t>(event.motion.yrel));
      }
    }
  }

  platform::Window::Frame frame;
  if (!window_.begin_frame(frame)) return false;

  if (frame.drawable()) {
    if (front_mode_) {
      tick_front(frame);
    } else if (play_mode_) {
      tick_play(frame);
    } else if (map_mode_) {
      tick_map(frame);
    } else {
      tick_sheet(frame);
    }
  }
  window_.end_frame(frame);
  // The music is looked at every frame, drawn or not: a pass of the menus'
  // loop, or the match's clock.
  tick_music();

  ++frame_number_;

  // The screenshot is taken a frame late on purpose: the download must not
  // race the render that produced the pixels.
  //
  // With `--frames` it is taken on the *last* frame instead. A still map looks
  // the same on frame 2 as on frame 400, but a world that is running does not:
  // the shot that shows units walking, turning and animating is the one at the
  // end of the run, and capturing it at the start made `--play --screenshot`
  // able to photograph nothing but the opening pose.
  const bool last_frame =
      args_.frames <= 0 || frame_number_ >= static_cast<std::uint64_t>(args_.frames);
  if (!pending_shot_.empty()) {
    take_screenshot(pending_shot_);
    pending_shot_.clear();
  }
  if (!args_.screenshot.empty() && !screenshot_taken_ && frame_number_ >= 2 && last_frame) {
    take_screenshot(args_.screenshot);
    screenshot_taken_ = true;
  }

  if (args_.frames > 0 && frame_number_ >= static_cast<std::uint64_t>(args_.frames)) {
    return false;
  }
  // A networked run with `--net-turns` ends here once it has played them.
  if (quit_requested_ && networked()) return false;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      std::fputs(kUsage, stdout);
      return 0;
    }
  }
  // Settled before `SDL_Init`, which is when SDL would otherwise bring the
  // process forward and pick a sound driver.
  const bool headless = platform::headless_requested(argc, argv);
  if (headless) platform::prepare_headless();
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  std::printf("Imperivm Reforged %s\n", imperivm::core::version_string());
  std::printf("SDL %d.%d.%d\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);

  auto application = std::make_unique<Application>();
  Arguments arguments = parse_arguments(argc, argv);
  arguments.headless = headless;
  if (!application->start(arguments)) {
    SDL_Quit();
    return 1;
  }

  platform::run_frame_loop([app = application.get()] { return app->tick(); });

  if (platform::frame_loop_returns_early()) {
    // The browser drives the loop after this returns, so the application must
    // outlive main. On the web the page's lifetime is the process lifetime,
    // and this deliberate leak is how you say that out loud.
    (void)application.release();
    return 0;
  }

  application.reset();
  SDL_Quit();
  return 0;
}
