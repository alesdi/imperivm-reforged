#include "imperivm/platform/world_view.hpp"

#include "imperivm/core/sim/anim.hpp"
#include "imperivm/core/sim/flying.hpp"
#include "imperivm/core/sim/gate.hpp"
#include "imperivm/core/sim/glide.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace imperivm::platform {
namespace {

namespace core = imperivm::core;
namespace sim = imperivm::core::sim;

/// How far outside the viewport an object's *origin* may sit and still be
/// drawn. A sprite hangs a long way above and to the left of its origin -- a
/// tower is 230 px tall and a layer offset of -114 is ordinary -- so a tight
/// cull pops the tops of buildings in at the top edge. The same 512 px
/// `MapRenderer` uses, for the same reason and so the two agree at the seam.
constexpr std::int32_t kSpriteMargin = 512;

/// The shadow pass's opacity. `DATA\CONST.INI` records `XRayTranslucency` for a
/// different effect and nothing at all for this one, so it is chosen to match
/// the reference render rather than recovered. Kept identical to
/// `MapRenderer`'s: a unit's shadow and a tree's must land the same weight.
constexpr float kShadowAlpha = 110.0F / 255.0F;

/// Reduce a sprite row or column to one the sheet actually has.
///
/// Needed in both directions. The entity XML's declared grid is wrong for 430
/// of 4,033 images, so a cursor built from it can name a row past the end; and
/// a facing is a number the simulation will eventually produce for an eight-way
/// sheet that some layer of the same entity only draws one of. Wrapping rather
/// than clamping is what a facing wants -- direction 9 of an 8-column sheet is
/// direction 1, not direction 7.
std::uint32_t wrap(std::uint32_t value, std::uint32_t count) noexcept {
  return count == 0 ? 0 : value % count;
}

}  // namespace

// --------------------------------------------------------------------------
// screen geometry
// --------------------------------------------------------------------------

ScreenRect ScreenRect::normalised() const noexcept {
  ScreenRect out = *this;
  if (out.width < 0) {
    out.x += out.width;
    out.width = -out.width;
  }
  if (out.height < 0) {
    out.y += out.height;
    out.height = -out.height;
  }
  return out;
}

bool ScreenRect::contains(std::int32_t px, std::int32_t py) const noexcept {
  const ScreenRect box = normalised();
  return px >= box.x && px < box.x + box.width && py >= box.y && py < box.y + box.height;
}

void Camera::look_at(sim::Point world) noexcept {
  x = core::world_to_screen_x(world.x) - width / 2;
  y = core::world_to_screen_y(world.y, height_at(world)) - height / 2;
}

void Camera::move(std::int32_t dx, std::int32_t dy) noexcept {
  x += dx;
  y += dy;
}

void Camera::clamp_to_map(std::int32_t size_x, std::int32_t size_y) noexcept {
  // The map in screen space is `size_x` by `size_y * 46/64` -- the projection
  // applied to the far corner, which is the only place the ratio belongs.
  const std::int32_t map_width = core::world_to_screen_x(size_x);
  const std::int32_t map_height = core::world_to_screen_y(size_y);
  x = std::clamp(x, 0, std::max(0, map_width - width));
  y = std::clamp(y, 0, std::max(0, map_height - height));
}

std::int32_t Camera::height_at(sim::Point world) const noexcept {
  if (elevation == nullptr || elevation->cell_size() == 0) return 0;
  if (world.x < 0 || world.y < 0) return 0;
  if (world.x >= static_cast<std::int32_t>(elevation->extent_x()) ||
      world.y >= static_cast<std::int32_t>(elevation->extent_y())) {
    return 0;
  }
  return sim::sample_height(*elevation, world);
}

ScreenPoint Camera::project(sim::Point world) const noexcept {
  return ScreenPoint{core::world_to_screen_x(world.x) - x,
                     core::world_to_screen_y(world.y, height_at(world)) - y};
}

sim::Point Camera::unproject(std::int32_t screen_x, std::int32_t screen_y) const noexcept {
  const std::int32_t world_x = screen_x + x;
  const std::int32_t row = screen_y + y;
  if (elevation == nullptr || elevation->cell_size() == 0) {
    return sim::Point{world_x, core::screen_to_world_y(row)};
  }
  // The scan: from the flat answer floored to a height cell, march down
  // the world while the ground projects at or above the row, then one
  // linear step inside the last cell. Terrain height only ever lifts, so
  // the flat answer is never past the true one.
  const auto forward = [&](std::int32_t world_y) {
    return core::world_to_screen_y(world_y, height_at(sim::Point{world_x, world_y}));
  };
  constexpr std::int32_t kStep = 32;
  std::int32_t candidate = core::screen_to_world_y(row) - kStep;
  candidate = (candidate >= 0 ? candidate / kStep : -((-candidate + kStep - 1) / kStep)) * kStep;
  std::int32_t low = candidate;
  std::int32_t low_row = forward(candidate);
  std::int32_t high_row = low_row;
  // A bound on the march: the tallest ground lifts 255 rows, eight cells.
  for (int steps = 0; steps < 64; ++steps) {
    const std::int32_t next = candidate + kStep;
    high_row = forward(next);
    if (high_row > row) break;
    candidate = next;
    low = next;
    low_row = high_row;
  }
  const std::int32_t span = high_row - low_row;
  if (span <= 0) return sim::Point{world_x, low};
  return sim::Point{world_x, low + (row - low_row) * kStep / span};
}

sim::Point Camera::centre() const noexcept { return unproject(width / 2, height / 2); }

// --------------------------------------------------------------------------
// setup
// --------------------------------------------------------------------------

const WorldView::Pose* WorldView::EntityArt::for_slot(std::int32_t slot) const noexcept {
  for (const auto& entry : anims) {
    if (entry.first == slot) return &entry.second;
  }
  return nullptr;
}

bool WorldView::create(Vfs& vfs, SpriteRenderer& renderer, std::string* error) {
  vfs_ = &vfs;
  renderer_ = &renderer;

  const ByteSpan zbins = vfs.read("DATA\\ZBINS.XML");
  auto bins = core::ZBins::parse(as_core_bytes(zbins));
  if (!bins) {
    // Without the bin table a shadow would sort against the bodies it lies
    // under, so this is a real failure rather than a degradation.
    if (error != nullptr) *error = "DATA\\ZBINS.XML did not parse";
    return false;
  }
  zbins_ = std::move(bins.value());
  return true;
}

void WorldView::set_player_colors(std::span<const core::Rgb888> players) {
  players_.assign(players.begin(), players.end());
  // Team palettes are keyed on (sheet, owner) and were built from the previous
  // colours, so they are no longer answers to the question being asked.
  palettes_.clear();
}

// --------------------------------------------------------------------------
// art resolution
// --------------------------------------------------------------------------

std::int32_t WorldView::sheet_for(const std::string& path) {
  const auto found = sheet_by_path_.find(path);
  if (found != sheet_by_path_.end()) return found->second;

  std::int32_t result = -1;
  const ByteSpan table = vfs_->read(path);
  if (!table.empty()) {
    auto parsed = core::RleImage::parse(as_core_bytes(table));
    if (parsed) {
      Sheet sheet;
      sheet.image = std::move(parsed.value());
      // Prepared, not uploaded: the cells this scene will sample are not known
      // until objects are looked at, and there are far too many to take them
      // all. See `prepare_sprite`.
      if (prepare_sprite(*renderer_, sheet.image, vfs_->pixel_store(), sheet.sprite)) {
        sheet.shadow = sheet.sprite.mask ||
                       sheet.image.image_class() == core::RleImageClass::shadow;
        sheets_.push_back(std::move(sheet));
        result = static_cast<std::int32_t>(sheets_.size()) - 1;
        stats_.sheets = sheets_.size();
      }
    }
  }
  sheet_by_path_.emplace(path, result);
  return result;
}

std::int32_t WorldView::sheet_for_image(const core::Entity& entity, std::int32_t image_idx) {
  const core::EntityImage* image = entity.image(image_idx);
  if (image == nullptr) return -1;
  for (const std::string& candidate :
       core::image_path_candidates(entity.path(), image->file)) {
    if (!vfs_->contains(candidate)) continue;
    const std::int32_t sheet = sheet_for(candidate);
    if (sheet >= 0) return sheet;
  }
  return -1;
}

const WorldView::EntityArt* WorldView::art_for(const core::Entity* entity) {
  if (entity == nullptr) return nullptr;
  const auto found = art_.find(entity);
  if (found != art_.end()) return &found->second;

  EntityArt art;

  // The resting stack: every layer, in `draw_order()` -- ascending z, ties by
  // declaration order. The frame's own left/top are *not* folded in the way
  // `MapRenderer` folds them, because they belong to a frame and this layer
  // will be drawn through a different one next turn.
  for (const std::uint32_t index : entity->draw_order()) {
    const core::EntityLayer& layer = entity->layers()[index];
    const std::int32_t sheet = sheet_for_image(*entity, layer.image);
    if (sheet < 0) continue;

    LayerArt out;
    out.sheet = sheet;
    out.layer_idx = layer.idx;
    out.declared = index;
    out.z = layer.z;
    out.sort_offset_x = layer.sortoffsetx;
    out.sort_offset_y = layer.sortoffsety;
    out.offset_x = layer.offsetx;
    out.offset_y = layer.offsety;
    art.base.push_back(out);
  }

  // An animation owns no images: it *re-points* existing layers at other
  // sheets for its run (`<replace>`), so each slot is the resting stack with
  // the swaps applied. Resolving them once here means the per-object path is a
  // lookup rather than a rebuild.
  for (const core::EntityAnim& anim : entity->anims()) {
    if (anim.replaces.empty()) continue;
    Pose pose = art.base;
    for (const core::AnimReplace& replace : anim.replaces) {
      const std::int32_t sheet = sheet_for_image(*entity, replace.image);
      if (sheet < 0) continue;
      for (LayerArt& layer : pose) {
        if (layer.layer_idx != replace.layer) continue;
        layer.sheet = sheet;
        layer.offset_x = replace.offsetx;
        layer.offset_y = replace.offsety;
      }
    }
    art.anims.emplace_back(anim.idx, std::move(pose));
  }
  std::sort(art.anims.begin(), art.anims.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });

  const auto inserted = art_.emplace(entity, std::move(art)).first;
  stats_.entities = art_.size();
  return &inserted->second;
}

std::size_t WorldView::adopt_frame_tables(core::Entity& entity) {
  if (vfs_ == nullptr || renderer_ == nullptr) return 0;

  // The indices are collected first: `adopt_frame_table` writes into the image
  // list it is iterating over, and a loop that holds a reference across the
  // call is one refactor away from being wrong.
  std::vector<std::int32_t> indices;
  indices.reserve(entity.images().size());
  for (const core::EntityImage& image : entity.images()) indices.push_back(image.idx);

  std::size_t conflicts = 0;
  for (const std::int32_t idx : indices) {
    const std::int32_t sheet = sheet_for_image(entity, idx);
    if (sheet < 0) continue;
    const auto conflict =
        entity.adopt_frame_table(idx, sheets_[static_cast<std::size_t>(sheet)].image);
    if (conflict && conflict->any()) ++conflicts;
  }
  return conflicts;
}

PaletteRow WorldView::palette_for(std::int32_t sheet, core::PlayerId owner, bool create) {
  Sheet& stored = sheets_[static_cast<std::size_t>(sheet)];
  // Team colour is a swap of palette slots 0..63, and only a class 2 sheet has
  // that block. Everything else -- shadows, neutral scenery, effects -- draws
  // through the palette the artist shipped.
  if (!stored.sprite.player_color) return stored.sprite.neutral;
  if (owner == core::kNoPlayer || owner >= players_.size()) return stored.sprite.neutral;

  const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(sheet))
                             << 8) |
                            owner;
  const auto found = palettes_.find(key);
  if (found != palettes_.end()) return found->second;
  if (!create) return stored.sprite.neutral;

  const core::Rgb888 team = players_[owner];
  if (team.red == 0 && team.green == 0 && team.blue == 0) return stored.sprite.neutral;

  PaletteRow row = upload_team_palette(*renderer_, stored.image, team,
                                       stored.sprite.transparent_index);
  if (!row.valid) row = stored.sprite.neutral;
  palettes_.emplace(key, row);
  return row;
}

// --------------------------------------------------------------------------
// what an object is showing
// --------------------------------------------------------------------------

WorldView::Cursor WorldView::cursor_for(const sim::WorldObject& object,
                                        const EntityArt& art) const {
  Cursor out;
  out.pose = &art.base;

  const core::AnimCursor& anim = object.object->anim;
  // The sheet column: which way the object is facing. The simulation owns it --
  // it is hashed world state, written by `MovementSystem` through
  // `core::sim::facing_column` -- so this reads what the world says and
  // re-derives nothing. A renderer that worked the facing out for itself from
  // the object's heading would be a second opinion about a fact the world
  // already holds, and the two would part company the first time one of them
  // rounded differently.
  out.column = anim.variation;

  // An animation is showing whenever the object has a resolved timeline and a
  // slot, which is **not** the same test as `animating`. `animating` goes false
  // the turn a `hold` animation reaches its end, and the object then holds that
  // final step -- `stop_anim` says so in as many words. Keying off `animating`
  // would snap a corpse back to its idle pose on the frame its death animation
  // finished, which is the most visible way to get this wrong. `enter_state`
  // clears both the timeline and the slot when it moves to a still pose, so the
  // still case is still reached exactly when the simulation means it to be.
  if (object.timeline.valid() && anim.anim_slot != core::kNoAnim) {
    if (const Pose* pose = art.for_slot(anim.anim_slot); pose != nullptr) out.pose = pose;
    // `step` is the world's own cursor, advanced by `run_turn` from the elapsed
    // game time under the object's `repeat` mode; `row_of_step` applies the
    // sheet's `remaping` sequencing. Neither is recomputed here: re-deriving a
    // frame index from a clock the view does not own is how a renderer and a
    // simulation end up disagreeing about what happened.
    //
    // Between two turns the cursor is the world's carried back to the drawn
    // time (`TurnGlide::anim_elapsed`): the same clock, sampled the way
    // `run_turn` samples it, at an instant `run_turn` steps over.
    std::uint32_t step = anim.step;
    if (view_world_ != nullptr) {
      const std::int32_t elapsed = glide_.anim_elapsed(*view_world_, object, draw_time_);
      if (elapsed != anim.elapsed_ms) step = object.timeline.sample(elapsed, object.repeat).step;
    }
    out.row = object.timeline.row_of_step(step);
    // A siege engine under construction is frozen on the stage
    // `Catapult::SetBuildFrame` set, whatever its build animation's clock
    // says (0x004e2ed0 stops the sprite's clock) -- until `SetBuilt` clears it.
    if (object.build_frame >= 0) {
      if (const core::EntityAnim* playing = object.object->entity->anim(anim.anim_slot)) {
        out.row = sim::build_frame_row(*playing, object.timeline, object.build_frame);
      }
    }
    return out;
  }

  // A resting pose. `image_row` selects the still frame -- for a building that
  // is its construction or damage stage. The state's `offsetx`/`offsety` are
  // deliberately not added: they repeat the layer's own offsets in the shipped
  // data, so adding them would double every entity's displacement.
  if (const core::EntityState* state = object.object->entity->state(anim.state_idx);
      state != nullptr && state->image_row > 0) {
    out.row = static_cast<std::uint32_t>(state->image_row);
  }
  return out;
}

WorldView::Cursor WorldView::visible_cursor(const sim::WorldObject& object) {
  const Cursor none;

  // Settlements, holders, warehouses, items and queries occupy handles and sit
  // in the hashed slot table, but they are economy and bookkeeping: there is no
  // native object under them at all, let alone art.
  if (object.internal != sim::InternalKind::none || object.object == nullptr) {
    ++stats_.internal;
    return none;
  }
  // A garrisoned object has no position -- the original writes (-1, -1) and its
  // location *is* its holder's. It is inside a building, so it is not drawn.
  if (object.state.is_held() || object.state.position == sim::kHeldPosition) {
    ++stats_.held;
    return none;
  }
  // A spawn template is not in play. Zama places 1,119 objects and 607 of
  // them are templates -- both armies, before its sequences spawn them --
  // and all of them used to be drawn where the map put them, and picked.
  if (object.state.flags.unspawned && !show_templates_) {
    ++stats_.templates;
    return none;
  }

  const core::Entity* entity = object.object->entity;
  const EntityArt* art = art_for(entity);
  if (art == nullptr || art->base.empty()) {
    // Ordinary, not a gap: `AdvArea`, the invisible script area class, is the
    // whole of this population on the shipped maps.
    ++stats_.no_art;
    return none;
  }

  ++stats_.drawable;
  return cursor_for(object, *art);
}

const WorldView::EntityArt* WorldView::decor_art(std::int32_t kind) {
  if (kind <= 0 || kind > 255 || decor_kinds_ == nullptr) return nullptr;
  if (decor_resolved_.size() != 256) {
    decor_resolved_.assign(256, false);
    decor_art_.assign(256, nullptr);
  }
  const auto slot = static_cast<std::size_t>(kind);
  if (decor_resolved_[slot]) return decor_art_[slot];
  decor_resolved_[slot] = true;
  const core::DecorKind* entry = decor_kinds_->find(kind);
  if (entry == nullptr || entry->entity.empty()) return nullptr;
  const std::string key = core::normalise_resource_path(entry->entity);
  const core::Entity* entity = decor_entities_.find(key);
  if (entity == nullptr) {
    const ByteSpan document = vfs_->read(key);
    if (document.empty()) return nullptr;
    auto loaded = decor_entities_.load(key, as_core_bytes(document));
    if (!loaded.ok()) return nullptr;
    entity = loaded.value();
    (void)adopt_frame_tables(*decor_entities_.find_mutable(key));
  }
  const EntityArt* art = art_for(entity);
  if (art == nullptr || art->base.empty()) return nullptr;
  decor_art_[slot] = art;
  return art;
}

// --------------------------------------------------------------------------
// staging
// --------------------------------------------------------------------------

void WorldView::set_turn_fraction(float fraction) noexcept {
  turn_fraction_ = std::isfinite(fraction) ? std::clamp(fraction, 0.0F, 1.0F) : 1.0F;
}

void WorldView::begin_view(const sim::World& world) {
  glide_.observe(world);
  view_world_ = &world;
  // The original's clock (0x00528e40) puts game time the real clock's share
  // of the way across the turn's window; the window here is the turn that
  // last ran, from the turn end before to the world's.
  const sim::GameTime now = world.time();
  const std::int32_t length = std::max(0, world.clock().turn().length);
  const auto lag = static_cast<sim::GameTime>(
      std::lround(static_cast<double>(1.0F - turn_fraction_) * static_cast<double>(length)));
  draw_time_ = std::max(glide_.since(), now - lag);
}

std::size_t WorldView::prepare(const sim::World& world, std::string* error) {
  if (vfs_ == nullptr || renderer_ == nullptr) return 0;
  begin_view(world);

  stats_.objects = world.size();
  stats_.drawable = 0;
  stats_.no_art = 0;
  stats_.held = 0;
  stats_.internal = 0;
  stats_.templates = 0;

  std::size_t staged = 0;
  for (const sim::WorldObject& object : world.objects()) {
    if (hidden_ && hidden_(object)) continue;
    const Cursor cursor = visible_cursor(object);
    if (cursor.pose == nullptr) continue;

    for (const LayerArt& layer : *cursor.pose) {
      Sheet& sheet = sheets_[static_cast<std::size_t>(layer.sheet)];
      const std::uint32_t row = wrap(cursor.row, sheet.sprite.rows);
      const std::uint32_t column = wrap(cursor.column, sheet.sprite.columns);

      const SpriteFrame* before = sheet.sprite.at(row, column);
      const bool resident = before != nullptr && before->region.valid();
      if (!resident && upload_sprite_frame(*renderer_, sheet.image, vfs_->pixel_store(),
                                           sheet.sprite, row, column) != nullptr) {
        ++staged;
        ++stats_.frames;
      }
      // Palette rows are staged too, and a row created while a draw list was
      // being built would be sampled a frame before it existed -- the sprite
      // comes out of the shader as whatever the lookup texture happened to
      // hold, which is memorably white. So every (sheet, owner) pairing the
      // scene can ask for is claimed here, and `draw` only ever reads back.
      (void)palette_for(layer.sheet, object.state.owner, true);
    }
  }

  // The decorations: frame (0, 0) of every kind the layer holds. The whole
  // grid is walked -- 65,536 cells at 16,384 units -- because the set of
  // kinds is not fixed: the editor paints new ones.
  if (decor_ != nullptr && decor_->cell_size() != 0) {
    std::vector<bool> seen(256, false);
    for (std::uint32_t cy = 0; cy < decor_->height(); ++cy) {
      for (std::uint32_t cx = 0; cx < decor_->width(); ++cx) {
        core::DecorCell cell;
        if (!core::decor_unpack(decor_->cell(cx, cy), cell)) continue;
        if (seen[static_cast<std::size_t>(cell.kind)]) continue;
        seen[static_cast<std::size_t>(cell.kind)] = true;
        const EntityArt* art = decor_art(cell.kind);
        if (art == nullptr) continue;
        for (const LayerArt& layer : art->base) {
          Sheet& sheet = sheets_[static_cast<std::size_t>(layer.sheet)];
          const SpriteFrame* before = sheet.sprite.at(0, 0);
          const bool resident = before != nullptr && before->region.valid();
          if (!resident && upload_sprite_frame(*renderer_, sheet.image, vfs_->pixel_store(),
                                               sheet.sprite, 0, 0) != nullptr) {
            ++staged;
            ++stats_.frames;
          }
        }
      }
    }
  }

  if (!renderer_->commit_uploads(error)) return 0;
  return staged;
}

// --------------------------------------------------------------------------
// placement and depth
// --------------------------------------------------------------------------

void WorldView::build(const sim::World& world, const Camera& camera) {
  begin_view(world);
  {
    // One line a placement -- a frame's, or a pick's -- ahead of its layers:
    // the turn the world stands at and the game time it is drawn at, so a
    // trace can tell a frame that moved a unit between two turns from one
    // that moved it with a turn.
    static const bool trace = std::getenv("IMPERIVM_DEBUG_VIEW") != nullptr;
    if (trace) {
      std::printf("frame turn %llu time %lld drawn %lld\n",
                  static_cast<unsigned long long>(world.turns()),
                  static_cast<long long>(world.time()), static_cast<long long>(draw_time_));
    }
  }
  items_.clear();
  order_.clear();
  placed_.clear();

  stats_.objects = world.size();
  stats_.drawable = 0;
  stats_.no_art = 0;
  stats_.held = 0;
  stats_.internal = 0;
  stats_.templates = 0;
  stats_.culled = 0;

  for (const sim::WorldObject& object : world.objects()) {
    // What the fog hides is neither drawn nor picked. `prepare` skipped it
    // from the day the fog was drawn and this did not, so an enemy standing in
    // explored-but-unlit ground was left out of staging but still drawn --
    // with whatever frames another object had staged -- and still answered a
    // right click.
    if (hidden_ && hidden_(object)) continue;
    const Cursor cursor = visible_cursor(object);
    if (cursor.pose == nullptr) continue;

    // Where the object is at the drawn time, between the turn end before and
    // the world's: a walking unit part of the way along its last turn's walk
    // (`sim/glide.hpp`), anything that did not walk where it stands.
    const std::int32_t clock = glide_.anim_elapsed(world, object, draw_time_);
    const sim::Point ground = glide_.position(world, object, draw_time_);
    ScreenPoint at = camera.project(ground);
    // A bird flying a leg is drawn along it: each end projected, ground and
    // all, and the anchor run between the two by the animation's clock, as
    // the original's visual runs it (`sim::flight_progress`) -- the clock at
    // the drawn time. Everything placed from the anchor follows -- the
    // shadow, the ring, the bar, the pick -- as it follows the original's.
    if (const sim::FlightProgress leg = sim::flight_progress(world, object, clock); leg.moving()) {
      const ScreenPoint from = camera.project(leg.from);
      const ScreenPoint to = camera.project(leg.to);
      at = ScreenPoint{leg.along(from.x, to.x), leg.along(from.y, to.y)};
    }
    if (at.x < -kSpriteMargin || at.x > camera.width + kSpriteMargin ||
        at.y < -kSpriteMargin || at.y > camera.height + kSpriteMargin) {
      ++stats_.culled;
      continue;
    }
    const float shade = shade_ ? static_cast<float>(shade_(ground)) / 32.0F : 1.0F;
    // Two per-layer offsets the original's visuals carry and the entity data
    // does not: a gate's portcullis raised by its position (`sim/gate.hpp`),
    // and a bird's body lifted by its altitude above the ground while its
    // shadow stays below it (`sim::flying_lift`). Both are read from the world
    // and neither is worked out here. They move the picture and not the sort
    // key: the original writes them beside the layer's own offset, and what
    // its depth sort does with them is not read.
    const std::int32_t raise = sim::gate_raise(object, draw_time_);
    const std::int32_t lift = sim::flying_lift(world, object, clock);
    placed_.push_back(Placed{&object, at, lift});

    for (const LayerArt& layer : *cursor.pose) {
      Sheet& sheet = sheets_[static_cast<std::size_t>(layer.sheet)];
      const SpriteFrame* frame =
          sheet.sprite.at(wrap(cursor.row, sheet.sprite.rows),
                          wrap(cursor.column, sheet.sprite.columns));
      // Not staged, or an RGB555 sheet an index atlas cannot hold. Either way
      // there is nothing to sample, and drawing an invalid region would sample
      // whatever else is in the page.
      if (frame == nullptr || !frame->region.valid()) continue;

      DrawItem item;
      item.id = object.id;
      item.bin = static_cast<std::uint32_t>(zbins_.bin_for(layer.z));
      item.order = static_cast<std::uint32_t>(items_.size());
      item.sort_y = at.y + layer.sort_offset_y;
      item.sort_x = at.x + layer.sort_offset_x;
      item.origin_x = at.x;
      item.origin_y = at.y;
      // The canvas origin goes at the object's screen position plus the
      // layer's offset; the frame then blits at its own place on that shared
      // canvas. That is why a body and its shadow line up with no alignment
      // arithmetic at all.
      item.x = static_cast<float>(at.x + layer.offset_x + frame->left);
      std::int32_t rise = 0;
      if (layer.declared == sim::kGateLayer) rise += raise;
      if (sim::flying_lifts_layer(layer.z)) rise += lift;
      item.y = static_cast<float>(at.y + layer.offset_y + frame->top - rise);
      item.width = static_cast<float>(frame->region.width);
      item.height = static_cast<float>(frame->region.height);
      item.region = frame->region;
      // Read once, not once per layer per frame: this is the innermost loop
      // the renderer has, and `getenv` walks the environment on every call.
      static const bool trace = std::getenv("IMPERIVM_DEBUG_VIEW") != nullptr;
      if (trace) {
        std::printf(
            "id %u sheet %d grid %ux%u row %u col %u region p%u %u,%u %ux%u lt %d,%d "
            "off %d,%d layer %u z %d at %.0f,%.0f\n",
            object.id, layer.sheet, sheet.sprite.rows, sheet.sprite.columns,
            wrap(cursor.row, sheet.sprite.rows), wrap(cursor.column, sheet.sprite.columns),
            frame->region.page, frame->region.x, frame->region.y, frame->region.width,
            frame->region.height, frame->left, frame->top, layer.offset_x, layer.offset_y,
            layer.declared, layer.z, item.x, item.y);
      }
      item.shadow = sheet.shadow;

      if (sheet.shadow) {
        // A shadow decodes to a coverage mask with no colour of its own: it is
        // drawn as translucent black through the two-entry mask palette.
        item.palette = sheet.sprite.neutral;
        item.modulate = Rgba{1.0F, 1.0F, 1.0F, kShadowAlpha};
      } else {
        item.palette = palette_for(layer.sheet, object.state.owner, false);
        item.modulate = Rgba{shade, shade, shade, 1.0F};
      }
      items_.push_back(item);
    }
  }

  // The decorations in view, from the layer: the cell's corner plus its
  // nibbles, four world units a step, through the same projection and into
  // the same bins, so a tree sorts against the units around it. No id, no
  // owner, no animation: the resting pose in the neutral palette.
  if (decor_ != nullptr && decor_->cell_size() != 0) {
    const auto cell_size = static_cast<std::int32_t>(decor_->cell_size());
    const core::sim::Point top_left = camera.unproject(-kSpriteMargin, -kSpriteMargin);
    // The tallest ground lifts 255 rows, so cells that far below the view
    // can stand in it.
    const core::sim::Point bottom_right =
        camera.unproject(camera.width + kSpriteMargin, camera.height + kSpriteMargin + 255);
    const std::int32_t first_x = std::max(0, top_left.x / cell_size - 1);
    const std::int32_t first_y = std::max(0, top_left.y / cell_size - 1);
    const std::int32_t last_x = std::min(static_cast<std::int32_t>(decor_->width()), bottom_right.x / cell_size + 2);
    const std::int32_t last_y = std::min(static_cast<std::int32_t>(decor_->height()), bottom_right.y / cell_size + 2);
    for (std::int32_t cy = first_y; cy < last_y; ++cy) {
      for (std::int32_t cx = first_x; cx < last_x; ++cx) {
        core::DecorCell cell;
        if (!core::decor_unpack(decor_->cell(static_cast<std::uint32_t>(cx), static_cast<std::uint32_t>(cy)),
                                cell)) {
          continue;
        }
        const EntityArt* art = decor_art(cell.kind);
        if (art == nullptr) continue;
        const core::sim::Point stands{cx * cell_size + cell.offset_x, cy * cell_size + cell.offset_y};
        const ScreenPoint at = camera.project(stands);
        const float shade = shade_ ? static_cast<float>(shade_(stands)) / 32.0F : 1.0F;
        for (const LayerArt& layer : art->base) {
          Sheet& sheet = sheets_[static_cast<std::size_t>(layer.sheet)];
          const SpriteFrame* frame = sheet.sprite.at(0, 0);
          if (frame == nullptr || !frame->region.valid()) continue;
          DrawItem item;
          item.id = core::kNoObject;
          item.bin = static_cast<std::uint32_t>(zbins_.bin_for(layer.z));
          item.order = static_cast<std::uint32_t>(items_.size());
          item.sort_y = at.y + layer.sort_offset_y;
          item.sort_x = at.x + layer.sort_offset_x;
          item.origin_x = at.x;
          item.origin_y = at.y;
          item.x = static_cast<float>(at.x + layer.offset_x + frame->left);
          item.y = static_cast<float>(at.y + layer.offset_y + frame->top);
          item.width = static_cast<float>(frame->region.width);
          item.height = static_cast<float>(frame->region.height);
          item.region = frame->region;
          item.shadow = sheet.shadow;
          item.palette = sheet.sprite.neutral;
          item.modulate = sheet.shadow ? Rgba{1.0F, 1.0F, 1.0F, kShadowAlpha} : Rgba{shade, shade, shade, 1.0F};
          items_.push_back(item);
        }
      }
    }
  }

  // Bin order first, from `DATA\ZBINS.XML`: ground decals, then all shadows
  // unsorted, then everything solid depth-sorted, then effects. Inside a
  // sorted bin the key is the layer's own ground contact point in screen
  // space -- **projected y is depth** in this projection, because the camera
  // looks straight down the world's Y axis -- with x, then the object id, then
  // emission order as tie-breaks. The id is in there so that two objects
  // standing on the same screen row do not swap places between frames, which
  // reads as a flicker; ids are stable and ascending, so the order is too.
  order_.resize(items_.size());
  for (std::uint32_t i = 0; i < order_.size(); ++i) order_[i] = i;
  std::sort(order_.begin(), order_.end(), [this](std::uint32_t a, std::uint32_t b) {
    const DrawItem& left = items_[a];
    const DrawItem& right = items_[b];
    if (left.bin != right.bin) return left.bin < right.bin;
    const bool sorted = left.bin < zbins_.bins().size() && zbins_.bins()[left.bin].sorted;
    if (!sorted) return left.order < right.order;
    if (left.sort_y != right.sort_y) return left.sort_y < right.sort_y;
    if (left.sort_x != right.sort_x) return left.sort_x < right.sort_x;
    if (left.id != right.id) return left.id < right.id;
    return left.order < right.order;
  });

  stats_.layers = items_.size();
}

std::size_t WorldView::draw(const sim::World& world, const Camera& camera,
                            SpriteRenderer& renderer) {
  if (vfs_ == nullptr) return 0;
  build(world, camera);
  stats_.rings = 0;
  stats_.bars = 0;

  // The marks of everything placed, asked once.
  struct Marked {
    core::ObjectId id = core::kNoObject;
    ScreenPoint at;
    std::int32_t lift = 0;
    Marks marks;
  };
  std::vector<Marked> marked;
  if (rings_ != nullptr && marks_) {
    for (const Placed& placed : placed_) {
      Marks marks = marks_(*placed.object);
      if (marks.ring < 0 && marks.bar.type <= 0) continue;
      marked.push_back(Marked{placed.object->id, placed.at, placed.lift, marks});
    }
  }
  const auto queue_rings = [&] {
    for (const Marked& mark : marked) {
      if (mark.marks.ring < 0) continue;
      const SelectionRings::Ring* ring = rings_->for_radius(mark.marks.selection_radius);
      if (ring == nullptr) continue;
      // Centred on the anchor. A reading: the draw places the image by a
      // point the image reports (0x0062b63a), which has not been read; every
      // ring is drawn centred in its frame. A bird's rises with its body: the
      // draw adds the offset its visual update stored beside the body's
      // (`sim::flying_lift`), which the bar below does not.
      const std::int32_t centre_y = mark.at.y - mark.lift;
      renderer.draw_mask(ring->region, static_cast<float>(mark.at.x - ring->width / 2),
                         static_cast<float>(centre_y - ring->height / 2),
                         rgb555(static_cast<std::uint16_t>(mark.marks.ring)));
      static const bool trace = std::getenv("IMPERIVM_DEBUG_VIEW") != nullptr;
      if (trace) std::printf("ring id %u at %d,%d\n", mark.id, mark.at.x, centre_y);
      ++stats_.rings;
    }
  };

  bool rings_down = false;
  for (const std::uint32_t index : order_) {
    const DrawItem& item = items_[index];
    if (!rings_down && item.bin < zbins_.bins().size() && zbins_.bins()[item.bin].sorted) {
      queue_rings();
      rings_down = true;
    }
    renderer.draw(item.region, item.x, item.y, item.palette, item.modulate);
  }
  if (!rings_down) queue_rings();
  for (const Marked& mark : marked) {
    if (queue_health_bar(renderer, mark.at.x, mark.at.y, mark.marks.bar)) ++stats_.bars;
  }
  return items_.size();
}

// --------------------------------------------------------------------------
// picking
// --------------------------------------------------------------------------

core::ObjectId WorldView::pick(const sim::World& world, const Camera& camera,
                               std::int32_t screen_x, std::int32_t screen_y) {
  if (vfs_ == nullptr) return core::kNoObject;
  build(world, camera);

  const auto px = static_cast<float>(screen_x);
  const auto py = static_cast<float>(screen_y);
  // Backwards through the draw order: the last thing drawn is the thing on top,
  // and the thing on top is what the player thinks they clicked.
  for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
    const DrawItem& item = items_[*it];
    if (item.shadow || item.id == core::kNoObject) continue;
    if (px < item.x || px >= item.x + item.width) continue;
    if (py < item.y || py >= item.y + item.height) continue;
    return item.id;
  }
  return core::kNoObject;
}

std::size_t WorldView::pick_in_rect(const sim::World& world, const Camera& camera,
                                    const ScreenRect& rect,
                                    std::vector<core::ObjectId>& out) {
  out.clear();
  if (vfs_ == nullptr) return 0;
  build(world, camera);

  const ScreenRect box = rect.normalised();
  // `items_` is in emission order, which is world order, which is ascending id:
  // objects live in a vector in spawn order and every query in the simulation
  // yields them that way. Walking it forwards and skipping repeats therefore
  // produces ascending ids without a sort.
  core::ObjectId last = core::kNoObject;
  for (const DrawItem& item : items_) {
    if (item.id == last || item.id == core::kNoObject) continue;
    if (!box.contains(item.origin_x, item.origin_y)) continue;
    out.push_back(item.id);
    last = item.id;
  }
  return out.size();
}

}  // namespace imperivm::platform
