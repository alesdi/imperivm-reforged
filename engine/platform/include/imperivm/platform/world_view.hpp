#pragma once

// Draws a live `core::sim::World`: the objects the simulation is actually
// moving, in the poses it has actually put them in.
//
// Reads: docs/engine/projection.md (the mapping and its inverse),
//        docs/engine/rendering.md (the atlas, team colour, the depth sort),
//        docs/formats/ent-xml.md  (layers, states, animations, `<replace>`).
//
// ## How this differs from `MapRenderer`
//
// `MapRenderer` draws `core::MapObjectList` -- the *static* map file. Every
// object is at the position the editor saved, owned by the player the file
// names, holding frame (0, 0) of its first sheet forever. That is the right
// shape for looking at a shipped map and the wrong shape for a game: nothing in
// it can move, turn, animate, die or change hands.
//
// This draws `core::sim::World` instead, which answers all of those from state
// the tick loop owns:
//
//   * `WorldObject::state.position` -- where the object is *now*, which for a
//     garrisoned object is nowhere at all (`kHeldPosition`), so it is not drawn;
//   * `WorldObject::state.owner` -- a 0-based player slot, which selects the
//     palette row team colour swaps into indices 0..63;
//   * `WorldObject::timeline` / `animating` / `NativeObject::anim` -- the
//     animation cursor, which selects the sprite *row*, and `anim.variation`
//     the *column*;
//   * `WorldObject::internal` -- settlements, holders, warehouses and queries
//     are state, not scenery, and have no art at all.
//
// The ground underneath is still `MapRenderer::draw_terrain`'s job. The two
// share a camera and compose in one pass: terrain, then this.
//
// ## What it deliberately does not own
//
// No window, no frame loop, no input, and no clock. It is handed a world, a
// camera and a sprite renderer and it queues draw calls. The application owns
// the loop, decides when the world advances, and moves the camera; this reads
// the world and never writes to it, which is why every argument is const.
//
// ## The atlas cannot hold whole sheets
//
// A unit's walk cycle is eight facings by fourteen steps: 112 frames, and a map
// draws several hundred distinct sheets. Uploading all of it is hundreds of
// megabytes of atlas for pixels almost none of which is ever sampled, and
// uploading only frame (0, 0) -- what `MapRenderer` does -- is a scene where
// nothing animates and everything faces the same way. So sheets are *prepared*
// and their cells staged **on demand**: `prepare` walks the world, works out
// the (sheet, row, column) each object is currently showing, stages anything
// new, and commits. `draw` then only ever draws what is already resident, which
// is the invariant that keeps a sprite from being sampled a frame before its
// pixels exist. Call them in that order, every frame.

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "imperivm/core/formats/color.hpp"
#include "imperivm/core/formats/rle.hpp"
#include "imperivm/core/game/entity.hpp"
#include "imperivm/core/sim/glide.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/world/map.hpp"
#include "imperivm/platform/selection_marks.hpp"
#include "imperivm/platform/sprite_atlas.hpp"
#include "imperivm/platform/sprite_renderer.hpp"
#include "imperivm/platform/vfs.hpp"

namespace imperivm::platform {

/// A point in the window, in pixels, origin top-left.
struct ScreenPoint {
  std::int32_t x = 0;
  std::int32_t y = 0;
};

/// An axis-aligned rectangle of the window, in pixels. `width`/`height` may be
/// negative: a drag box is built from two corners and normalising it is this
/// type's job, not every caller's.
struct ScreenRect {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;

  [[nodiscard]] ScreenRect normalised() const noexcept;
  [[nodiscard]] bool contains(std::int32_t px, std::int32_t py) const noexcept;
};

/// Where the view is looking, and how big the view is.
///
/// Held in **screen pixels**, matching `MapRenderer`'s `view_x`/`view_y`
/// exactly, so one camera drives the ground and the objects with no conversion
/// between them. World coordinates enter only through the projection, which is
/// `core::world_to_screen_x/y`: horizontal scale 1, vertical 46/64, integer
/// arithmetic, no rotation and no shear (docs/engine/projection.md). Do not
/// re-derive it and do not float it: the truncation is part of the mapping.
struct Camera {
  std::int32_t x = 0;
  std::int32_t y = 0;
  std::int32_t width = 0;
  std::int32_t height = 0;
  /// The map's height layer, or null for a flat map. With it, `project`
  /// lifts a point by the height under it and `unproject` scans for the
  /// ground a screen point is on -- the sim's own inverse (`sim/projection.
  /// hpp`) at the renderer's 46/64 -- so what the ground mesh draws and
  /// what a click picks agree.
  const core::Grid* elevation = nullptr;

  /// The height under a world point: bilinear over the layer, zero off it or
  /// without one.
  [[nodiscard]] std::int32_t height_at(core::sim::Point world) const noexcept;

  /// Centre the view on a world position.
  void look_at(core::sim::Point world) noexcept;
  void move(std::int32_t dx, std::int32_t dy) noexcept;
  /// Keep the viewport inside a map `size_x` by `size_y` world units, which in
  /// screen space is `size_x` by `size_y * 46/64`.
  void clamp_to_map(std::int32_t size_x, std::int32_t size_y) noexcept;

  /// World to window.
  [[nodiscard]] ScreenPoint project(core::sim::Point world) const noexcept;
  /// Window to world -- the projection inverted, which is what a click is.
  ///
  /// The inverse is exact in x and rounds in y, because 46/64 is not
  /// invertible over the integers: 64 world units map onto 46 pixels, so one
  /// pixel of screen y is 1.39 world units and the answer is the nearest
  /// world row rather than the only one. That is a property of the projection,
  /// not of this code. With an elevation layer the answer is the nearest
  /// ground the screen row shows, found by the scan 0x006243b0 runs: march
  /// down the world in 32-unit steps until the projected ground passes the
  /// row, then interpolate once.
  [[nodiscard]] core::sim::Point unproject(std::int32_t screen_x,
                                           std::int32_t screen_y) const noexcept;
  /// The world position at the centre of the view.
  [[nodiscard]] core::sim::Point centre() const noexcept;
};

/// Turns a simulated world into draw calls.
class WorldView {
 public:
  WorldView() = default;

  WorldView(const WorldView&) = delete;
  WorldView& operator=(const WorldView&) = delete;

  /// Binds the resources. `vfs` supplies entity art and the pixel store;
  /// `renderer` receives the atlas and palette uploads. Both must outlive this.
  /// Loads `DATA\ZBINS.XML`, which is the depth-bin table.
  bool create(Vfs& vfs, SpriteRenderer& renderer, std::string* error = nullptr);

  [[nodiscard]] bool ready() const noexcept { return vfs_ != nullptr; }

  /// The player colours, indexed by `ObjectState::owner`, which is a **0-based**
  /// slot: `player0.xml` is slot 0. (`MapObject::player` in the map file is
  /// 1-based and `World::populate_from_map` subtracts one, so the two agree
  /// once and only once.) A slot that is absent, black, or beyond the end of
  /// the span draws through the sprite's own neutral palette, which is what an
  /// unowned object should look like.
  void set_player_colors(std::span<const core::Rgb888> players);

  /// Give `entity`'s images the geometry their sprite sheets actually have, and
  /// report how many declarations the sheets contradicted.
  ///
  /// **The entity XML is not authoritative about the sprite grid**: 430 of the
  /// 4,033 resolvable `<image>` declarations state a `rows`/`columns` the frame
  /// table disagrees with, and the frame table wins (docs/formats/ent-xml.md).
  /// Since `AnimTimeline` takes its step count from the image geometry, an
  /// entity that never had this called animates on the wrong row count for
  /// about one image in ten. The view clamps regardless -- it can only draw
  /// rows that exist -- but the simulation's cursor would still be wrong, so
  /// call this from the entity resolver, once, as each entity is loaded.
  std::size_t adopt_frame_tables(core::Entity& entity);

  /// Stages the art the world is currently showing and commits the uploads.
  ///
  /// Must run before `draw`, outside the sprite renderer's `begin`/`render`
  /// pair, because a palette row or an atlas region only reaches the GPU here.
  /// Cheap once the scene is warm: a map lookup per object layer and no upload
  /// at all when nothing new has appeared. Returns the number of frames staged,
  /// so a caller can see a load spike for what it is.
  std::size_t prepare(const core::sim::World& world, std::string* error = nullptr);

  /// Places, sorts and queues every visible object. Returns the layers queued.
  ///
  /// Call between `SpriteRenderer::begin` and `SpriteRenderer::render`, after
  /// the ground has been blitted into the same target.
  std::size_t draw(const core::sim::World& world, const Camera& camera,
                   SpriteRenderer& renderer);

  /// How far the real clock is through the turn the world is waiting for: 0
  /// the moment a turn has run, 1 when the next one is due -- and 1, the
  /// default, whenever the clock is not running, so a paused world or one a
  /// script is holding at a turn is drawn exactly as it stands.
  ///
  /// The world is drawn at the game time the original's clock would show
  /// then, `fraction` of the way from the turn end before to the world's
  /// (`sim/glide.hpp`): a walking unit runs between the two ends, its
  /// animation steps on its own clock, a bird flies its leg, a portcullis
  /// rises -- frame by frame, where they used to move once a turn. The ring,
  /// the bar, the shadow and both picks are placed from the same anchor, so
  /// what is clicked is what is drawn. Set it once a frame, before anything
  /// picks; the world is read, never written.
  void set_turn_fraction(float fraction) noexcept;
  /// Forget the turn ends seen: a new match, a load. Harmless to skip -- a
  /// world whose clock went backwards or jumped is forgotten anyway -- and
  /// cheap to call.
  void reset_glide() noexcept { glide_.reset(); }
  /// The game time the last `prepare`, `draw` or pick placed the world at.
  [[nodiscard]] core::sim::GameTime drawn_time() const noexcept { return draw_time_; }

  /// The fog's darkening of what is drawn: a factor over 32 at a world
  /// point, applied to every sprite at the point it stands on (the
  /// original darkens each sprite by the fog at its own point, 0x00604a40,
  /// and the ground the same way pixel by pixel). Null darkens nothing.
  void set_shade(std::function<std::int32_t(core::sim::Point)> shade) { shade_ = std::move(shade); }

  /// Objects the fog hides are neither drawn nor picked: the predicate the
  /// application installs from its `FogView` (0x00605e10 asks the fog
  /// manager per view entity). Null draws everything.
  void set_hidden(std::function<bool(const core::sim::WorldObject&)> hidden) {
    hidden_ = std::move(hidden);
  }

  /// Whether spawn templates are drawn and picked. A template
  /// (`ObjectState::flags.unspawned`, `<scriptobj flags>` bit 27) is placed
  /// by the map but not in play -- `SpawnGroup` mints copies of it and every
  /// collection path skips it -- so a running game shows none of them. The
  /// editor shows the map as placed, templates and all. Off by default.
  void set_show_templates(bool show) noexcept { show_templates_ = show; }

  /// The map's scattered decoration: `Terrain.decor.grid` and the table that
  /// names each kind's entity (`MAPOBJECTS\DECORS\DECORS.INI`). A tree is
  /// not a simulated object -- the world has no handle for it, nothing
  /// scripts it, and the shipped passability layer already carries its
  /// footprint -- so it is drawn from the layer, one entity per non-zero
  /// cell at the cell's corner plus its two sub-cell nibbles, and sorted
  /// with the objects so a unit walks behind a trunk. Both pointers must
  /// outlive the view or be reset; null draws none. The grid is read every
  /// frame, so a cell the editor paints shows at once. Decorations are
  /// never picked: a click through a crown lands on what stands under it.
  void set_decorations(const core::Grid* decor, const core::DecorTable* kinds) {
    decor_ = decor;
    decor_kinds_ = kinds;
  }

  /// What marks one object this frame: the ring under it and the health bar
  /// over it. Deciding them is the caller's -- the selection, the bar mode
  /// and the class properties are not the view's -- drawing them is here,
  /// because the ring sorts with the sprites.
  struct Marks {
    /// The ring's RGB555 colour, or -1 for no ring.
    std::int32_t ring = -1;
    /// The class's `selection_radius`, which picks the ring.
    std::int32_t selection_radius = 0;
    /// Type 0 draws no bar.
    HealthBar bar;
  };

  /// Asked once a frame for every object `draw` places. The ring goes down
  /// after the ground decals and the shadows and before the first sorted
  /// bin, so a unit stands on it (**a reading**: the original's ring is an
  /// entry of its own in the draw list, `[visual+0x5c8]` at 0x0062b5d0, and
  /// what depth that entry is given has not been read). The bars go last,
  /// over every sprite: the original draws them from a list of their own
  /// after the scene (0x0062ba80). Null rings or no callback draws neither.
  void set_marks(const SelectionRings* rings,
                 std::function<Marks(const core::sim::WorldObject&)> marks) {
    rings_ = rings;
    marks_ = std::move(marks);
  }

  // -- picking -----------------------------------------------------------
  //
  // The projection inverted. It lives here rather than in the application
  // because it is the same arithmetic, the same culling and the same depth
  // order the draw uses, and two copies of that would disagree the first time
  // one of them changed.

  /// The object under a window point: the **topmost** one whose sprite covers
  /// it, in the same depth order the frame was drawn in.
  ///
  /// The test is against each layer's placed rectangle, not its silhouette --
  /// the atlas holds palette indices and the shader resolves transparency, so
  /// there is no coverage mask on this side to test against. Shadow layers are
  /// excluded, which matters: a building's ground shadow is wider than the
  /// building and would otherwise swallow clicks meant for whatever stands
  /// beside it. `kNoObject` when nothing is there.
  core::ObjectId pick(const core::sim::World& world, const Camera& camera,
                           std::int32_t screen_x, std::int32_t screen_y);

  /// Every object whose **ground position** falls inside a window rectangle,
  /// in ascending id order.
  ///
  /// Band selection tests the anchor point rather than the sprite box on
  /// purpose: a box test would drag in every tree whose crown overhangs the
  /// rectangle, and the anchor is where the player sees the unit standing.
  /// Objects with no art are not returned -- an invisible script area is not
  /// something a drag box should select.
  std::size_t pick_in_rect(const core::sim::World& world, const Camera& camera,
                           const ScreenRect& rect, std::vector<core::ObjectId>& out);

  // -- diagnostics -------------------------------------------------------

  struct Stats {
    std::size_t objects = 0;      ///< objects the world holds
    std::size_t drawable = 0;     ///< of those, ones that resolve to art
    std::size_t no_art = 0;       ///< and ones that do not (AdvArea and friends)
    std::size_t held = 0;         ///< garrisoned, so not on the map at all
    std::size_t internal = 0;     ///< settlements, holders, warehouses, queries
    std::size_t templates = 0;    ///< spawn templates, not in play (`set_show_templates`)
    std::size_t culled = 0;       ///< drawable, but off screen
    std::size_t layers = 0;       ///< layers queued by the last draw
    std::size_t sheets = 0;       ///< distinct `.rle.mmp` prepared
    std::size_t frames = 0;       ///< distinct cells staged in the atlas
    std::size_t entities = 0;     ///< distinct entities resolved
    std::size_t rings = 0;        ///< selection rings queued by the last draw
    std::size_t bars = 0;         ///< health bars queued by the last draw
  };
  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

 private:
  /// One `.rle.mmp`, prepared but only partly resident. `image` views the
  /// mounted pack's bytes, which outlive the view.
  struct Sheet {
    core::RleImage image;
    Sprite sprite;
    bool shadow = false;
  };

  /// One `<layer>` of an entity, resolved to a sheet and a screen offset.
  ///
  /// The frame's own `left`/`top` are *not* folded in here the way
  /// `MapRenderer` folds them: they are per frame, and this layer will be drawn
  /// through a different frame next turn.
  struct LayerArt {
    std::int32_t sheet = -1;
    std::int32_t layer_idx = 0;  ///< `<layer idx>`, what `<replace>` names
    /// Where the layer stands in the entity's declaration order, 0-based: how
    /// `gbr.exe`'s per-layer offsets name a layer (see `sim/gate.hpp`).
    std::uint32_t declared = 0;
    std::int32_t z = 0;
    std::int32_t sort_offset_x = 0;
    std::int32_t sort_offset_y = 0;
    std::int32_t offset_x = 0;
    std::int32_t offset_y = 0;
  };

  /// A resolved draw stack: the entity's layers, or those layers with one
  /// animation's `<replace>` swaps already applied.
  using Pose = std::vector<LayerArt>;

  struct EntityArt {
    Pose base;
    /// By animation slot, ascending. Slots are a sparse vocabulary of at most a
    /// handful per entity, so a sorted vector beats a map at every size.
    std::vector<std::pair<std::int32_t, Pose>> anims;
    [[nodiscard]] const Pose* for_slot(std::int32_t slot) const noexcept;
  };

  /// One placed layer, before the depth sort.
  struct DrawItem {
    core::ObjectId id = core::kNoObject;
    std::uint32_t bin = 0;
    std::int32_t sort_y = 0;
    std::int32_t sort_x = 0;
    std::uint32_t order = 0;
    std::int32_t origin_x = 0;  ///< the object's ground point, for band select
    std::int32_t origin_y = 0;
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    AtlasRegion region;
    PaletteRow palette;
    Rgba modulate;
    bool shadow = false;
  };

  /// What an object is showing this instant: a draw stack, a sprite row and a
  /// sheet column.
  struct Cursor {
    const Pose* pose = nullptr;
    std::uint32_t row = 0;
    std::uint32_t column = 0;
  };

  [[nodiscard]] std::int32_t sheet_for(const std::string& path);
  /// The sheet behind an `<image idx>`, resolved through the three spellings
  /// the original tries. -1 when none of them is in a mounted pack.
  [[nodiscard]] std::int32_t sheet_for_image(const core::Entity& entity,
                                             std::int32_t image_idx);
  [[nodiscard]] const EntityArt* art_for(const core::Entity* entity);
  [[nodiscard]] PaletteRow palette_for(std::int32_t sheet, core::PlayerId owner,
                                       bool create);
  /// The pose, row and column an object is holding at the drawn time.
  [[nodiscard]] Cursor cursor_for(const core::sim::WorldObject& object,
                                  const EntityArt& art) const;
  /// Note the world and work out the game time it is drawn at. `prepare`,
  /// `build` and so `draw` and both picks start here, so all of them stage,
  /// place and test the same instant.
  void begin_view(const core::sim::World& world);
  /// The object's art and cursor, or a null pose when it must not be drawn.
  [[nodiscard]] Cursor visible_cursor(const core::sim::WorldObject& object);
  /// A decoration kind's resting art, its entity loaded on first use; null
  /// when the kind names no entity or none of its sheets is in a pack.
  [[nodiscard]] const EntityArt* decor_art(std::int32_t kind);

  /// Fills `items_` and `order_` for one view of one world. Both `draw` and the
  /// two picks go through this, so all three agree by construction.
  void build(const core::sim::World& world, const Camera& camera);

  Vfs* vfs_ = nullptr;
  /// The two turn ends last seen and the instant between them the world is
  /// drawn at. Presentation: derived from the world, written to nothing.
  core::sim::TurnGlide glide_;
  float turn_fraction_ = 1.0F;
  const core::sim::World* view_world_ = nullptr;
  core::sim::GameTime draw_time_ = 0;
  std::function<bool(const core::sim::WorldObject&)> hidden_;
  bool show_templates_ = false;
  SpriteRenderer* renderer_ = nullptr;

  core::ZBins zbins_;
  std::vector<core::Rgb888> players_;

  std::deque<Sheet> sheets_;  ///< stable addresses: art layers hold indices
  std::map<std::string, std::int32_t> sheet_by_path_;
  /// Keyed on the entity itself: `EntityLibrary` hands out pointers that stay
  /// valid for its lifetime, and two classes sharing one definition should
  /// share one resolution.
  std::map<const core::Entity*, EntityArt> art_;
  std::map<std::uint64_t, PaletteRow> palettes_;

  const core::Grid* decor_ = nullptr;
  const core::DecorTable* decor_kinds_ = nullptr;
  /// The decorations' entities, loaded here because the simulation never
  /// loads them: nothing in it refers to a decor entity.
  core::EntityLibrary decor_entities_;
  std::function<std::int32_t(core::sim::Point)> shade_;
  /// By kind, 0..255: resolved once, null kept for a kind with no art.
  std::vector<const EntityArt*> decor_art_;
  std::vector<bool> decor_resolved_;

  /// Every object `build` placed, once, with its anchor: what `draw` asks
  /// `marks_` about.
  struct Placed {
    const core::sim::WorldObject* object = nullptr;
    ScreenPoint at;
    /// How far a bird's body is lifted off the anchor, which its ring
    /// shares (`sim::flying_lift`).
    std::int32_t lift = 0;
  };
  std::vector<Placed> placed_;
  const SelectionRings* rings_ = nullptr;
  std::function<Marks(const core::sim::WorldObject&)> marks_;

  std::vector<DrawItem> items_;
  std::vector<std::uint32_t> order_;  ///< indices into `items_`, in draw order
  Stats stats_;
};

}  // namespace imperivm::platform
