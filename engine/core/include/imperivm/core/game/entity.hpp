#pragma once

// Reader and runtime shape for entity definitions (`*.ENT.XML`).
//
// Specification: docs/formats/ent-xml.md
//
// An entity is the *art and animation contract* for a class. A `.SC.XML` says
// what a Briton bowman is; his `.ENT.XML` says which sprite sheets he is drawn
// from, in what layer order, which poses he can hold, and how he animates
// between them. There are 889 of them in the retail packs.
//
// Three things about this format shape the API below, and each of them is a
// correctness trap rather than a style choice:
//
//   * **The XML is a manifest, not the authority, for sprite geometry.** 430 of
//     the 4,033 resolvable `<image>` declarations state a `rows`/`columns` the
//     matching `.rle.mmp` frame table contradicts, and hundreds more disagree
//     on `drawmode`. A loader that slices animations from the XML grid
//     mis-slices roughly one image in ten. So an `EntityImage` keeps what the
//     XML *declared* separate from the geometry it will actually be drawn
//     with, and the second is only trustworthy once `adopt_frame_table` has
//     handed it the sheet. See `RleImage` in formats/rle.hpp.
//   * **Animations are addressed by fixed numeric slot, not by name.** `.vs`
//     scripts call `PlayAnim(19, …)` with a literal, so the runtime has to
//     expose slots. `anim(slot)` is the accessor; it returns null for a slot
//     the entity does not declare, which scripts do ask for (slots 0 and 16
//     appear in shipped script code and in no entity at all).
//   * **`remaping` is animation sequencing, not palette remapping.** The name
//     misleads; it is the row playback order. `AnimOrder` is the honest name.
//
// The reader is tolerant on purpose. The retail data ships with uninitialised
// memory in seven `<state>` offsets, a `drawmode` typo, `xray="226"` where the
// domain is 0/1, and internal references that dangle in three files. All of it
// runs in the original engine, so none of it may abort a load.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/formats/result.hpp"
#include "imperivm/core/formats/rle.hpp"
#include "imperivm/core/xml.hpp"

namespace imperivm::core {

/// `<state anim_idx>` / `<state anim_frame>` sentinel meaning "no animation".
/// 0x10000, i.e. zero in the low 16 bits of a value the engine treats as a
/// packed index. 987 states carry it.
inline constexpr std::int32_t kNoAnim = 65536;

/// 0xCDCDCDCD read as a signed 32-bit integer: the MSVC debug-heap fill for
/// uninitialised memory, which leaked into seven shipped `<state>` offsets.
/// The affected entities render correctly in the retail game, so the engine
/// evidently ignores the field there; `EntityState::offsets_are_garbage` flags
/// it rather than the loader silently trusting or rejecting it.
inline constexpr std::int32_t kUninitialisedOffset = -842150451;

/// The blit path a sheet takes. Correlates with `<image drawmode>`, but the
/// XML disagrees with the sheet in hundreds of cases and the sheet wins; see
/// `EntityImage::geometry`.
enum class DrawMode : std::uint8_t {
  normal = 0,    ///< the sprite's own palette
  player_color,  ///< participates in team colouring (a palette swap inside the sheet)
  shadow,        ///< flat shadow, not coloured
  indexed,       ///< `drawmode="index"`, 162 images. Meaning unknown.
  clouds,        ///< effects and fog; presumably additive or translucent
  unknown,       ///< the attribute was missing or unrecognised
};

/// Accepts the one shipped typo (`playercol`, used once) as `player_color`.
[[nodiscard]] DrawMode draw_mode_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view draw_mode_name(DrawMode mode) noexcept;
/// The draw mode implied by the frame table's storage class, which is the
/// authoritative one.
[[nodiscard]] DrawMode draw_mode_from_image_class(RleImageClass image_class) noexcept;

/// Row playback order — the real meaning of the `remaping` attribute.
enum class AnimOrder : std::uint8_t {
  forward = 0,  ///< `none`: 1 -> N. 3,816 images.
  reverse,      ///< N -> 1. 135 images, every one a rotation sheet.
  pingpong,     ///< 1 -> N -> 1. 83 images: fires, swaying trees, crops.
};

[[nodiscard]] AnimOrder anim_order_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view anim_order_name(AnimOrder order) noexcept;

/// The sprite row shown at animation `step` of a `rows`-row sheet.
///
/// `forward` and `reverse` wrap with period `rows`; `pingpong` walks up and
/// back with period `2 * rows - 2`, so neither endpoint is held for two steps.
/// **The pingpong period is an inference** — the data shows only that the
/// sequence must not snap back — and is the one place here where a different
/// reading would be invisible in a static frame comparison.
[[nodiscard]] std::uint32_t sequenced_row(AnimOrder order, std::uint32_t rows,
                                          std::uint32_t step) noexcept;

/// The animation slots the corpus exercises. `.vs` code also calls slots 0 and
/// 16, which no entity declares, so this is a partial vocabulary and not an
/// enumeration to validate against. Names are the dominant `<anim name>` for
/// the slot, not an authority: the slot number is what the engine addresses.
enum : std::int32_t {
  kAnimWalk = 1,       ///< locomotion; on non-units simply "the animation"
  kAnimAnim2 = 2,      ///< generic extra loop
  kAnimAnim3 = 3,      ///< generic extra loop
  kAnimAttack = 5,     ///< attack; also `defence`/`defend`
  kAnimDie = 9,        ///< death; `destroy` on buildings
  kAnimIdle = 13,      ///< idle loop
  kAnimIdle2 = 14,     ///< a second idle (one entity)
  kAnimSecondary = 17, ///< `heal` / `carry`
  kAnimToIdle = 18,    ///< transition attack -> idle
  kAnimToAttack = 19,  ///< transition idle -> attack
  kAnimTaunt = 20,     ///< taunt / work
};

/// An integer attribute that may legitimately be absent. `0` is a real value
/// throughout this format (`radius="0"` on 120 entities), so absence cannot be
/// encoded as zero.
struct OptionalInt {
  std::int32_t value = 0;
  bool present = false;

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return present; }
  [[nodiscard]] constexpr std::int32_t value_or(std::int32_t fallback) const noexcept {
    return present ? value : fallback;
  }
};

/// The grid an image is actually drawn with.
///
/// Starts as a copy of what the XML declared, with `from_frame_table` false.
/// `Entity::adopt_frame_table` replaces it with the sheet's own numbers and
/// sets the flag. Anything that slices frames should assert the flag or accept
/// that it will be wrong for about one image in ten.
struct ImageGeometry {
  std::uint32_t rows = 1;     ///< animation steps
  std::uint32_t columns = 1;  ///< variations: facing directions, or random looks
  DrawMode draw_mode = DrawMode::unknown;
  bool from_frame_table = false;
};

/// Which fields of an `<image>` declaration the frame table contradicted.
struct GeometryConflict {
  bool rows = false;
  bool columns = false;
  bool draw_mode = false;

  [[nodiscard]] constexpr bool any() const noexcept { return rows || columns || draw_mode; }
};

/// One `<image>`: a sprite sheet handle.
struct EntityImage {
  std::int32_t idx = 0;   ///< 1..25, the handle `<layer>` and `<replace>` use
  std::string file;       ///< a `.rle` name, relative to the entity's directory
  AnimOrder order = AnimOrder::forward;

  /// What the XML said. Kept only as a cross-check: see the file header.
  std::uint32_t declared_rows = 1;
  std::uint32_t declared_columns = 1;
  DrawMode declared_draw_mode = DrawMode::unknown;

  /// What to draw with.
  ImageGeometry geometry;
};

/// One `<point>`: a typed attachment marker.
///
/// **The type semantics are unknown.** 16 values over 3,607 instances with no
/// legend anywhere in the data; they gate sentry slots, projectile origins and
/// unit exits, and the mapping needs the `.VS` host API or runtime observation
/// to settle. The loader carries `(idx, type, x, y)` through verbatim and
/// invents nothing.
struct EntityPoint {
  std::int32_t idx = 0;
  std::int32_t type = 0;
  std::int32_t x = 0;
  std::int32_t y = 0;
};

/// One `<layer>`: an image drawn at a depth.
struct EntityLayer {
  std::int32_t idx = 0;
  std::string name;          ///< decorative; several are Bulgarian
  std::int32_t image = 0;    ///< an `<image idx>`; resolves for all 2,430 in the corpus
  std::int32_t z = 0;        ///< feeds the `DATA\ZBINS.XML` sort bins, see ZBins
  std::int32_t offsetx = 0;
  std::int32_t offsety = 0;
  std::int32_t sortoffsetx = 0;  ///< shifts the y-sort point without moving the sprite
  std::int32_t sortoffsety = 0;
  std::int32_t xray = 0;         ///< 0/1 — plus four shipped layers holding 226
  std::int32_t nohighlight = 0;  ///< excluded from the selection outline pass
  OptionalInt percent;           ///< 13 layers; read as blend percentage, `-90` unexplained
};

/// One `<state>`: a resting pose, optionally looping an animation slot.
struct EntityState {
  std::int32_t idx = 0;
  std::string name;  ///< `idle`, `attack`, or a bare number for building stages
  std::int32_t image_idx = 0;
  std::int32_t image_row = 0;
  std::int32_t offsetx = 0;
  std::int32_t offsety = 0;
  std::int32_t anim_idx = kNoAnim;    ///< animation *slot* to loop, or kNoAnim
  std::int32_t anim_frame = kNoAnim;
  OptionalInt anim_row;  ///< 17 uses, values 0/1. No hypothesis.

  [[nodiscard]] bool has_anim() const noexcept { return anim_idx != kNoAnim; }
  /// True for the seven states holding 0xCDCDCDCD instead of an offset.
  [[nodiscard]] bool offsets_are_garbage() const noexcept {
    return offsetx == kUninitialisedOffset || offsety == kUninitialisedOffset;
  }
};

/// One `<replace>`: re-point a layer at another sheet for an animation's run.
struct AnimReplace {
  std::int32_t layer = 0;
  std::int32_t image = 0;
  std::int32_t offsetx = 0;
  std::int32_t offsety = 0;
};

/// One `<anim>`: an edge of the entity's state machine, addressed by slot.
struct EntityAnim {
  std::int32_t idx = 0;  ///< the **slot**, not a serial: 1, 5, 9, 13, 19, …
  std::string name;      ///< documentation only
  std::int32_t startstate = 0;
  std::int32_t endstate = 0;
  std::int32_t frames = 0;
  std::int32_t duration = 0;          ///< ms; disagrees with the frame sum in 127 anims
  std::int32_t default_duration = 0;  ///< ms; the distinction from `duration` is unconfirmed
  std::int32_t action_time = 0;       ///< ms into the animation at which the effect fires
  std::int32_t step = 0;              ///< world units advanced per cycle; 0 off locomotion
  bool floating_heading = false;      ///< `floating="1"`: ships and birds
  std::vector<AnimReplace> replaces;
  std::vector<std::int32_t> frame_durations;  ///< ms per `<frame>`, in order

  /// Sum of the frame durations. The safe playback length: `duration`
  /// disagrees with this in 127 of 1,168 retail animations and which one the
  /// original honours is unknown, so treat `duration` as advisory.
  [[nodiscard]] std::int32_t measured_duration() const noexcept;

  /// Real sprite rows: the frame strip is bookended by two zero-length entry
  /// and exit markers, so `frames == rows + 2` in 1,116 of 1,168 anims.
  [[nodiscard]] std::int32_t sprite_rows() const noexcept {
    return frames > 2 ? frames - 2 : 0;
  }
};

// --------------------------------------------------------------------------
// animation playback
// --------------------------------------------------------------------------
//
// Timing here is in *game-time units*. `DATA\CONST.INI` declares
// `[VXTIME] GameSpeed = 1000` and lists the speed presets in the same scale
// (`SlowSpeed 700`, `NormalSpeed 1000`, `FastSpeed 1400`), so a game-time unit
// is a millisecond at 100% speed and the animation `duration` fields — which
// the format documents as milliseconds — are already in the simulation's own
// clock. Nothing here converts anything. See docs/engine/tick.md.
//
// **Playback is a pure function of elapsed game time.** Nothing accumulates a
// frame index, because an accumulator makes the result depend on how the
// caller sliced the interval — and the interval is sliced differently on
// different machines: the lockstep turn length is renegotiated as latency
// moves, and the retail dumps show it taking the values 200, 400, 799 and 800.
// So a sequence of turns has to leave a cursor exactly where the single turn
// of their sum would, and here that is true by construction rather than by
// care.

/// What a cursor does when it reaches the end of the strip.
///
/// **The data does not encode this.** `startstate == endstate` looks like the
/// discriminator and is not: it holds for all 154 `die` animations, which
/// plainly do not loop. What the data does support is the split by *how the
/// animation was reached* — an animation named by `<state anim_idx>` is the
/// pose's own loop (slots 13, 5 and 17, and all of those are same-state),
/// while one started by a script's `PlayAnim` runs once and hands control
/// back. So the caller says which, and this type is the parameter it says it
/// with.
enum class AnimRepeat : std::uint8_t {
  loop = 0,  ///< restart at the top of the cycle, forever
  hold,      ///< stop on the final step and report `finished`
};

/// Where a timeline is at some elapsed game time.
struct AnimSample {
  std::uint32_t step = 0;      ///< index into the step cycle, before sequencing
  std::uint32_t row = 0;       ///< the sprite row to draw, after sequencing
  std::int32_t step_start = 0; ///< elapsed time at which this step began
  std::int32_t step_hold = 0;  ///< how long this step is held
  bool finished = false;       ///< a `hold` animation has run past its end
};

/// The playable schedule of one `<anim>`: which sprite row is shown when.
///
/// Three separate pieces of the data meet here, and each contributes exactly
/// one thing:
///
///   * the **frame strip** (`EntityAnim::frame_durations`) gives the per-row
///     hold. It is the only per-frame timing in the format;
///   * the **sheet** gives the row count, via `EntityImage::geometry` once
///     `Entity::adopt_frame_table` has made it authoritative. The XML's own
///     `rows` is wrong for 430 of 4,033 images, so the strip's length is not a
///     safe row count;
///   * the **`remaping` order** (`AnimOrder`) turns a row count into a step
///     cycle: `forward` and `reverse` have `rows` steps, `pingpong` has
///     `2 * rows - 2`.
///
/// A hold belongs to the *row*, not to the step, so a pingpong sheet holds row
/// 3 for the same time going up as coming back down. That is an inference —
/// the data gives one duration per row and never says what happens on the
/// return leg — but the alternative (indexing the strip by step) needs strip
/// entries that do not exist for steps `rows`..`2*rows-3`.
///
/// ### Which duration drives playback
///
/// `EntityAnim::duration` disagrees with the frame sum in 127 of the 1,168
/// retail animations. **This class honours the frame strip and ignores
/// `duration`.** The evidence is set out in docs/engine/tick.md; the short
/// version is that `action_time` — the only other absolute offset in the
/// format — lands on an un-rescaled strip boundary in every disagreeing unit
/// animation that carries one, and on a boundary of the strip rescaled to
/// `duration` in none of them.
class AnimTimeline {
 public:
  AnimTimeline() = default;

  /// `rows` is the sheet's real row count and `order` its `remaping`.
  /// Both come from the image the animation replaces; `Entity::timeline`
  /// resolves them for you.
  AnimTimeline(const EntityAnim& anim, std::uint32_t rows, AnimOrder order);

  /// False for an animation that can never advance: no rows, or every hold
  /// zero. 0 of the 1,168 retail animations are invalid; a modified file could
  /// be, and sampling one must not divide by zero.
  [[nodiscard]] bool valid() const noexcept { return steps_ != 0 && cycle_ > 0; }

  [[nodiscard]] std::uint32_t rows() const noexcept { return rows_; }
  [[nodiscard]] AnimOrder order() const noexcept { return order_; }
  /// Steps in one full cycle: `rows` forward or reverse, `2 * rows - 2` for
  /// pingpong (`1` when `rows` is 1, which cannot ping anywhere).
  [[nodiscard]] std::uint32_t steps() const noexcept { return steps_; }
  /// Total game time of one full cycle. For pingpong this is *not* twice the
  /// strip sum: the two endpoints are visited once each, not twice.
  [[nodiscard]] std::int32_t cycle() const noexcept { return cycle_; }
  /// The per-row holds actually used, in row order, after the entry/exit
  /// bookends were dropped and the strip was fitted to the sheet.
  [[nodiscard]] std::span<const std::int32_t> holds() const noexcept { return holds_; }

  /// How long `step` is held. Zero-length steps exist (14 retail animations
  /// carry one inside the strip) and are simply never observed.
  [[nodiscard]] std::int32_t hold_of_step(std::uint32_t step) const noexcept;
  /// The sprite row shown at `step`.
  [[nodiscard]] std::uint32_t row_of_step(std::uint32_t step) const noexcept;

  /// Reduce `elapsed` to the canonical value for its position in the cycle:
  /// modulo the cycle when looping, clamped to the cycle when holding.
  ///
  /// This is what keeps a cursor's stored time bounded, and it is why
  /// advancing in one batch equals advancing in pieces: `(a + b) mod c` does
  /// not depend on where the sum was split.
  [[nodiscard]] std::int32_t normalise(std::int32_t elapsed,
                                       AnimRepeat repeat) const noexcept;

  /// The step and row shown `elapsed` game-time units into the animation.
  /// Negative `elapsed` reads as zero. An invalid timeline samples as step 0,
  /// row 0, finished.
  [[nodiscard]] AnimSample sample(std::int32_t elapsed, AnimRepeat repeat) const noexcept;

 private:
  std::vector<std::int32_t> holds_;  ///< per sprite row, `rows_` entries
  std::uint32_t rows_ = 0;
  std::uint32_t steps_ = 0;
  std::int32_t cycle_ = 0;
  AnimOrder order_ = AnimOrder::forward;
};

/// The frame strip with its entry and exit markers removed, in row order.
///
/// The strip is bookended by zero-length entry/exit frames in 1,114 of the
/// 1,168 retail animations, by a leading marker alone in 8, and by neither in
/// 46; no shipped animation has a trailing marker without a leading one. So
/// the rule is simply "drop a leading zero, drop a trailing zero", which is
/// correct for all three shapes.
[[nodiscard]] std::vector<std::int32_t> anim_frame_holds(const EntityAnim& anim);


/// One `<entity>` document.
///
/// Owns its strings, so it outlives the buffer it was parsed from — entities
/// are loaded once from a pack blob that the caller is free to drop.
///
/// Elements are kept in document order. Lookup by `idx` searches backwards, so
/// a duplicated index resolves to the last declaration, matching the reference
/// loader's dictionary semantics. (The retail corpus contains no duplicates;
/// the rule exists so that a modified file cannot change meaning between the
/// two implementations.)
class Entity {
 public:
  /// Parse `xml`, tagging the entity with its pack path. The path is what a
  /// class's `entity=` attribute carries and the base for relative image and
  /// mask references; `name` is *not* an identity (52 names are shared and 12
  /// entities have none).
  static Result<Entity> parse(std::span<const std::byte> xml, std::string_view path = {});
  /// Same, from an already parsed document.
  static Result<Entity> from_document(const XmlDocument& doc, std::string_view path = {});

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  /// `vx/building`, `tree`, `vx/unit`, `tobj`, or empty. A loose editor tag: it
  /// does not discriminate the schema and does not agree with `cpp_class`.
  [[nodiscard]] const std::string& type() const noexcept { return type_; }
  /// Number of distinct renderings: facing directions for a unit, random looks
  /// for a prop. The entity-wide default for an image's column count, not a
  /// constraint — 22 entities disagree with their own images.
  [[nodiscard]] std::int32_t variations() const noexcept { return variations_; }
  /// The passability mask link, relative to the entity's directory, and the
  /// entity's *only* footprint information. Empty for entities that obstruct
  /// nothing. See `pass_path_candidates`.
  [[nodiscard]] const std::string& pass_file() const noexcept { return pass_file_; }
  /// Per-entity overrides of the class properties of the same name. Which side
  /// wins when both are set is unknown; the class value is the one balance
  /// data varies, so this is most likely the default.
  [[nodiscard]] OptionalInt radius() const noexcept { return radius_; }
  [[nodiscard]] OptionalInt selection_radius() const noexcept { return selection_radius_; }
  [[nodiscard]] OptionalInt floating_turnspeed() const noexcept { return floating_turnspeed_; }

  [[nodiscard]] const std::vector<EntityImage>& images() const noexcept { return images_; }
  [[nodiscard]] const std::vector<EntityPoint>& points() const noexcept { return points_; }
  [[nodiscard]] const std::vector<EntityLayer>& layers() const noexcept { return layers_; }
  [[nodiscard]] const std::vector<EntityState>& states() const noexcept { return states_; }
  [[nodiscard]] const std::vector<EntityAnim>& anims() const noexcept { return anims_; }

  [[nodiscard]] const EntityImage* image(std::int32_t idx) const noexcept;
  [[nodiscard]] const EntityLayer* layer(std::int32_t idx) const noexcept;
  [[nodiscard]] const EntityState* state(std::int32_t idx) const noexcept;

  /// The animation in `slot`, or null if this entity declares none.
  ///
  /// Slots are a sparse, fixed vocabulary the scripts address with literals,
  /// and the authoritative slot table is not in the data. A missing slot is
  /// therefore ordinary — `PlayAnim(16, …)` occurs in shipped script code and
  /// no entity declares slot 16 — and must not be treated as an error.
  [[nodiscard]] const EntityAnim* anim(std::int32_t slot) const noexcept;
  [[nodiscard]] bool has_anim(std::int32_t slot) const noexcept { return anim(slot) != nullptr; }

  /// The animation a state loops while it is held, or null.
  [[nodiscard]] const EntityAnim* anim_for(const EntityState& state) const noexcept;

  /// The sprite sheet an animation plays over, or null.
  ///
  /// An animation owns no images; it re-points layers at other sheets for its
  /// run, so its geometry is the geometry of the first `<replace>` whose image
  /// resolves. All 1,168 retail animations have at least one. The order the
  /// rows are played in is that image's `remaping`.
  [[nodiscard]] const EntityImage* anim_image(const EntityAnim& anim) const noexcept;

  /// The number of sprite rows an animation steps through.
  ///
  /// Taken from the sheet, which is authoritative, once `adopt_frame_table`
  /// has supplied it. Before that it is the XML's declared row count, which is
  /// wrong for 430 of 4,033 images — so a caller that cares should check
  /// `unresolved_geometry()`. Falls back to `anim.sprite_rows()` when the
  /// animation replaces nothing resolvable.
  [[nodiscard]] std::uint32_t anim_rows(const EntityAnim& anim) const noexcept;
  [[nodiscard]] AnimOrder anim_order(const EntityAnim& anim) const noexcept;

  /// The playable schedule for an animation.
  [[nodiscard]] AnimTimeline timeline(const EntityAnim& anim) const;
  /// The playable schedule for a *slot*, which is how scripts address
  /// animations. An undeclared slot yields an invalid timeline rather than an
  /// error: shipped `.vs` code calls `PlayAnim(0, …)` and `PlayAnim(16, …)`
  /// and no entity declares either.
  [[nodiscard]] AnimTimeline timeline_for_slot(std::int32_t slot) const;

  /// Layer indices (into `layers()`) in draw order: ascending `z`, ties broken
  /// by declaration order. Within a `sort="1"` z bin the scene still sorts
  /// these against other objects by y; see `ZBins`.
  [[nodiscard]] std::vector<std::uint32_t> draw_order() const;

  /// Take the geometry of image `idx` from its frame table, which is
  /// authoritative, and report what the XML got wrong.
  ///
  /// Fails with `not_found` if the entity declares no such image.
  Result<GeometryConflict> adopt_frame_table(std::int32_t idx, const RleImage& sheet);

  /// Images whose geometry still comes from the XML. Non-zero here means a
  /// caller is about to slice frames from numbers that may be wrong.
  [[nodiscard]] std::size_t unresolved_geometry() const noexcept;

 private:
  std::string path_;
  std::string name_;
  std::string type_;
  std::string pass_file_;
  std::int32_t variations_ = 1;
  OptionalInt radius_;
  OptionalInt selection_radius_;
  OptionalInt floating_turnspeed_;
  std::vector<EntityImage> images_;
  std::vector<EntityPoint> points_;
  std::vector<EntityLayer> layers_;
  std::vector<EntityState> states_;
  std::vector<EntityAnim> anims_;
};

// --------------------------------------------------------------------------
// reference resolution
// --------------------------------------------------------------------------
//
// The core knows how a reference is spelled; it never looks anything up. These
// build the candidate pack paths, in the order the original engine's misses
// prove it tries them, and the caller asks its own index whether each exists.

/// The pack directory holding `entity_path`, without a trailing separator.
/// Image and mask references are relative to it.
[[nodiscard]] std::string_view entity_directory(std::string_view entity_path) noexcept;

/// Candidate paths for an `<image file>`, in resolution order. The extension is
/// frequently omitted and the pixel data ships as `.RLE.MMP`; with these three
/// candidates 4,033 of the corpus's 4,034 references resolve.
[[nodiscard]] std::array<std::string, 3> image_path_candidates(std::string_view entity_path,
                                                               std::string_view file);

/// Candidate paths for a `pass_file`. 51 of the 597 shipped masks are stored
/// under the bare name `PASS` with no extension at all, which is what
/// `pass_file="pass"` refers to.
///
/// **Name resolution is not the whole story**: the engine matches masks by
/// content, so 16 references that miss by name may still be satisfied at
/// runtime, and a missing mask must never be fatal.
[[nodiscard]] std::array<std::string, 2> pass_path_candidates(std::string_view entity_path,
                                                              std::string_view file);

/// Fold a reference to the form used as a pack index key: upper case,
/// backslash separators, and the `gameres\` virtual root rewritten to `UI\`.
[[nodiscard]] std::string normalise_resource_path(std::string_view path);

// --------------------------------------------------------------------------
// the loaded set
// --------------------------------------------------------------------------

/// Every entity loaded so far, keyed by normalised pack path.
///
/// This is the join between the class graph and the art. A class names an
/// entity *path* per season (`ClassGraph::entity_path`, which owns the seasonal
/// fallback rule); the library turns a path into a loaded `Entity`, once,
/// shared by every object of every class that names it. 889 entities back
/// 845 classes, and the sharing is the point: the retail data has eight faction
/// gates pointing at one definition.
///
/// Pointers stay valid for the lifetime of the library, so an object can hold
/// one. Iteration is in load order, never by hash: iteration order is world
/// state (docs/engine/architecture.md).
class EntityLibrary {
 public:
  /// Parse `xml` and keep it under `path`. If `path` is already loaded the
  /// stored entity is returned and the bytes are ignored — a definition is
  /// immutable, and reloading one behind live objects would be a dangling
  /// pointer with extra steps.
  Result<const Entity*> load(std::string_view path, std::span<const std::byte> xml);

  /// The entity at `path`, or null. `path` is normalised on the way in, so a
  /// reference spelled `Units/BBowman/bbowman.ent.xml` finds it.
  [[nodiscard]] const Entity* find(std::string_view path) const;

  /// The same, mutable, for a **load-time correction** and nothing else.
  ///
  /// A definition is immutable once objects hold pointers into it, which is why
  /// everything else here hands back `const`. The one legitimate write is
  /// reconciling an `<image>`'s declared `rows`/`columns` with the frame table,
  /// which disagrees for 430 of the 4,033 resolvable declarations and wins --
  /// see `docs/formats/ent-xml.md` and
  /// `platform::WorldView::adopt_frame_tables`. Do that immediately after
  /// `load`, before anything can be looking.
  [[nodiscard]] Entity* find_mutable(std::string_view path);

  [[nodiscard]] std::size_t size() const noexcept { return entities_.size(); }
  [[nodiscard]] bool empty() const noexcept { return entities_.empty(); }
  /// In load order.
  [[nodiscard]] const Entity& at(std::size_t index) const { return *entities_[index]; }

 private:
  /// Owning, because a vector of entities would move its elements on growth
  /// and every pointer handed out would dangle.
  std::vector<std::unique_ptr<Entity>> entities_;
  /// (path, index into entities_), sorted by path for lookup.
  std::vector<std::pair<std::string, std::uint32_t>> index_;
};

// --------------------------------------------------------------------------
// depth sort bins
// --------------------------------------------------------------------------

/// `DATA\ZBINS.XML`: a partition of the layer-`z` range into bins, each flagged
/// sorted or not.
///
/// The bin boundaries line up exactly with the observed `z` clusters —
/// shadows at 800 and full-screen effects at 1500 land in unsorted bins, the
/// sprite body at 1000 in a sorted one — which is the evidence for the reading
/// that a sorted bin is y-sorted against the rest of the scene while an
/// unsorted one draws in fixed order. **That reading is an inference**; the
/// data proves only where the boundaries are.
class ZBins {
 public:
  struct Bin {
    std::int32_t start_z = 0;
    bool sorted = false;
  };

  static Result<ZBins> parse(std::span<const std::byte> xml);
  static Result<ZBins> from_document(const XmlDocument& doc);

  [[nodiscard]] const std::vector<Bin>& bins() const noexcept { return bins_; }
  /// Index of the bin containing `z`, or `bins().size()` if `z` falls below
  /// the first bin's start.
  [[nodiscard]] std::size_t bin_for(std::int32_t z) const noexcept;
  /// Whether the bin containing `z` is y-sorted. False when `z` falls outside.
  [[nodiscard]] bool sorted_at(std::int32_t z) const noexcept;

 private:
  std::vector<Bin> bins_;  ///< ascending by `start_z`
};

}  // namespace imperivm::core
