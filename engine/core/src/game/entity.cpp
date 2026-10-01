// Entity definitions. See include/imperivm/core/game/entity.hpp.
//
// The reader mirrors the reference loader in src/imperivm/formats/gamedata.py
// element for element, including its tolerances, because that loader is
// validated across the whole retail corpus and the two are diffed against each
// other. Where it accepts something odd, so does this.

#include "imperivm/core/game/entity.hpp"

#include <algorithm>
#include <utility>

namespace imperivm::core {
namespace {

/// Every descendant of `start` named `name`, in document order.
///
/// This mirrors ElementTree's `iter()`, which the reference loader uses, and
/// so it does not require the `<images>`/`<layers>`/… wrappers. Three shipped
/// files omit `<anims>` and one omits `<points>`; nothing in the format nests
/// an `<image>` anywhere else, so the recursive walk cannot pick up a stray.
template <class F>
void for_each_descendant(const XmlDocument& doc, NodeIndex start, std::string_view name,
                         F&& fn) {
  if (start == kNoNode) return;
  if (doc.node(start).name == name) fn(start);
  for (NodeIndex child = doc.node(start).first_child; child != kNoNode;
       child = doc.node(child).next_sibling) {
    for_each_descendant(doc, child, name, fn);
  }
}

std::string text_of(const XmlDocument& doc, NodeIndex node, std::string_view key) {
  const std::string_view value = doc.attribute(node, key);
  return std::string(value);
}

/// An integer attribute that is only "present" when it is there and non-empty.
/// The data writes `pass_file=""` and `radius=""`, and an empty attribute must
/// read as absent rather than as zero: `0` is a real value here.
OptionalInt optional_int(const XmlDocument& doc, NodeIndex node, std::string_view key) {
  const std::string_view raw = doc.attribute(node, key);
  if (raw.empty()) return {};
  return OptionalInt{doc.attribute_int(node, key, 0), true};
}

std::string join_path(std::string_view directory, std::string_view file) {
  std::string joined;
  joined.reserve(directory.size() + file.size() + 1);
  if (!directory.empty()) {
    joined.append(directory);
    joined.push_back('\\');
  }
  joined.append(file);
  return joined;
}

}  // namespace

// --------------------------------------------------------------------------
// enumerations
// --------------------------------------------------------------------------

DrawMode draw_mode_from_name(std::string_view name) noexcept {
  if (name == "normal") return DrawMode::normal;
  if (name == "player_color") return DrawMode::player_color;
  // One shipped image spells it `playercol`. It is the same mode.
  if (name == "playercol") return DrawMode::player_color;
  if (name == "shadow") return DrawMode::shadow;
  if (name == "index") return DrawMode::indexed;
  if (name == "clouds") return DrawMode::clouds;
  return DrawMode::unknown;
}

std::string_view draw_mode_name(DrawMode mode) noexcept {
  switch (mode) {
    case DrawMode::normal:
      return "normal";
    case DrawMode::player_color:
      return "player_color";
    case DrawMode::shadow:
      return "shadow";
    case DrawMode::indexed:
      return "index";
    case DrawMode::clouds:
      return "clouds";
    case DrawMode::unknown:
      break;
  }
  return "unknown";
}

DrawMode draw_mode_from_image_class(RleImageClass image_class) noexcept {
  switch (image_class) {
    case RleImageClass::truecolor:
      return DrawMode::normal;
    case RleImageClass::indexed:
      return DrawMode::indexed;
    case RleImageClass::player_color:
      return DrawMode::player_color;
    case RleImageClass::shadow:
      return DrawMode::shadow;
    case RleImageClass::clouds:
      return DrawMode::clouds;
  }
  return DrawMode::unknown;
}

AnimOrder anim_order_from_name(std::string_view name) noexcept {
  if (name == "reverse") return AnimOrder::reverse;
  if (name == "pingpong") return AnimOrder::pingpong;
  return AnimOrder::forward;
}

std::string_view anim_order_name(AnimOrder order) noexcept {
  switch (order) {
    case AnimOrder::reverse:
      return "reverse";
    case AnimOrder::pingpong:
      return "pingpong";
    case AnimOrder::forward:
      break;
  }
  return "none";
}

std::uint32_t sequenced_row(AnimOrder order, std::uint32_t rows, std::uint32_t step) noexcept {
  if (rows == 0) return 0;
  switch (order) {
    case AnimOrder::forward:
      return step % rows;
    case AnimOrder::reverse:
      return rows - 1 - (step % rows);
    case AnimOrder::pingpong: {
      if (rows == 1) return 0;
      const std::uint32_t period = 2 * rows - 2;
      const std::uint32_t position = step % period;
      return position < rows ? position : period - position;
    }
  }
  return 0;
}

std::int32_t EntityAnim::measured_duration() const noexcept {
  std::int32_t total = 0;
  for (const std::int32_t duration : frame_durations) total += duration;
  return total;
}

// --------------------------------------------------------------------------
// animation playback
// --------------------------------------------------------------------------

std::vector<std::int32_t> anim_frame_holds(const EntityAnim& anim) {
  std::vector<std::int32_t> holds(anim.frame_durations.begin(), anim.frame_durations.end());
  // Drop the entry marker, then the exit marker. Both are present in 1,114 of
  // the 1,168 retail animations, the entry alone in 8, and neither in 46; no
  // shipped animation carries an exit marker without an entry one.
  if (!holds.empty() && holds.front() == 0) holds.erase(holds.begin());
  if (!holds.empty() && holds.back() == 0) holds.pop_back();
  // A negative hold is not in the retail data and would run the clock
  // backwards, so it reads as an instant frame rather than as a subtraction.
  for (std::int32_t& hold : holds) {
    if (hold < 0) hold = 0;
  }
  return holds;
}

AnimTimeline::AnimTimeline(const EntityAnim& anim, std::uint32_t rows, AnimOrder order)
    : rows_(rows), order_(order) {
  if (rows_ == 0) return;

  std::vector<std::int32_t> strip = anim_frame_holds(anim);
  holds_.resize(rows_);
  for (std::uint32_t row = 0; row < rows_; ++row) {
    if (row < strip.size()) {
      holds_[row] = strip[row];
      continue;
    }
    // The strip is shorter than the sheet in 46 retail animations. The sheet
    // is authoritative about how many rows exist, so the missing rows are
    // played rather than skipped, and they take the last stated hold — or
    // `default_duration`, which is the only other per-frame figure the format
    // offers, when there is no last hold to take. **Both are guesses**: the
    // data does not say what the engine does with a short strip.
    holds_[row] = strip.empty() ? anim.default_duration : strip.back();
    if (holds_[row] < 0) holds_[row] = 0;
  }

  switch (order_) {
    case AnimOrder::forward:
    case AnimOrder::reverse:
      steps_ = rows_;
      break;
    case AnimOrder::pingpong:
      // Up and back with neither endpoint held twice, matching sequenced_row.
      steps_ = rows_ > 1 ? 2 * rows_ - 2 : 1;
      break;
  }

  std::int64_t cycle = 0;
  for (std::uint32_t step = 0; step < steps_; ++step) {
    cycle += holds_[sequenced_row(order_, rows_, step)];
  }
  // Nothing in the corpus comes close: the longest cycle is 16,424 units.
  cycle_ = cycle > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<std::int32_t>(cycle);
}

std::int32_t AnimTimeline::hold_of_step(std::uint32_t step) const noexcept {
  if (steps_ == 0) return 0;
  return holds_[sequenced_row(order_, rows_, step % steps_)];
}

std::uint32_t AnimTimeline::row_of_step(std::uint32_t step) const noexcept {
  if (steps_ == 0) return 0;
  return sequenced_row(order_, rows_, step % steps_);
}

std::int32_t AnimTimeline::normalise(std::int32_t elapsed, AnimRepeat repeat) const noexcept {
  if (elapsed < 0) return 0;
  if (!valid()) return 0;
  if (repeat == AnimRepeat::hold) return elapsed < cycle_ ? elapsed : cycle_;
  return elapsed % cycle_;
}

AnimSample AnimTimeline::sample(std::int32_t elapsed, AnimRepeat repeat) const noexcept {
  AnimSample out;
  if (!valid()) {
    out.finished = true;
    return out;
  }

  const bool past_end = repeat == AnimRepeat::hold && elapsed >= cycle_;
  const std::int32_t at = normalise(elapsed, repeat);

  if (past_end) {
    // Stop on the last step that is actually shown. A zero-length step at the
    // end of the strip is never visible, so holding on it would freeze the
    // sprite on a frame the animation skipped over while running.
    std::int32_t start = 0;
    for (std::uint32_t step = 0; step < steps_; ++step) {
      const std::int32_t hold = holds_[sequenced_row(order_, rows_, step)];
      if (hold > 0) {
        out.step = step;
        out.step_start = start;
        out.step_hold = hold;
      }
      start += hold;
    }
    out.row = sequenced_row(order_, rows_, out.step);
    out.finished = true;
    return out;
  }

  std::int32_t start = 0;
  for (std::uint32_t step = 0; step < steps_; ++step) {
    const std::int32_t hold = holds_[sequenced_row(order_, rows_, step)];
    if (hold > 0 && at < start + hold) {
      out.step = step;
      out.row = sequenced_row(order_, rows_, step);
      out.step_start = start;
      out.step_hold = hold;
      return out;
    }
    start += hold;
  }

  // Unreachable while `at < cycle_` and the holds sum to `cycle_`; kept so
  // that a caller who reaches it gets the last frame rather than garbage.
  out.step = steps_ - 1;
  out.row = sequenced_row(order_, rows_, out.step);
  out.step_start = cycle_;
  return out;
}

const EntityImage* Entity::anim_image(const EntityAnim& anim) const noexcept {
  for (const AnimReplace& replace : anim.replaces) {
    if (const EntityImage* found = image(replace.image)) return found;
  }
  return nullptr;
}

std::uint32_t Entity::anim_rows(const EntityAnim& anim) const noexcept {
  if (const EntityImage* sheet = anim_image(anim)) return sheet->geometry.rows;
  const std::int32_t fallback = anim.sprite_rows();
  return fallback > 0 ? static_cast<std::uint32_t>(fallback) : 0;
}

AnimOrder Entity::anim_order(const EntityAnim& anim) const noexcept {
  if (const EntityImage* sheet = anim_image(anim)) return sheet->order;
  return AnimOrder::forward;
}

AnimTimeline Entity::timeline(const EntityAnim& anim) const {
  return AnimTimeline(anim, anim_rows(anim), anim_order(anim));
}

AnimTimeline Entity::timeline_for_slot(std::int32_t slot) const {
  const EntityAnim* found = anim(slot);
  return found != nullptr ? timeline(*found) : AnimTimeline();
}

// --------------------------------------------------------------------------
// Entity
// --------------------------------------------------------------------------

Result<Entity> Entity::parse(std::span<const std::byte> xml, std::string_view path) {
  auto document = XmlDocument::parse(xml);
  if (!document) return document.error();
  return from_document(*document, path);
}

Result<Entity> Entity::from_document(const XmlDocument& doc, std::string_view path) {
  const NodeIndex root = doc.root();
  if (root == kNoNode) return FormatError::malformed;
  if (doc.node(root).name != "entity") return FormatError::malformed;

  Entity entity;
  entity.path_ = normalise_resource_path(path);
  entity.name_ = text_of(doc, root, "name");
  entity.type_ = text_of(doc, root, "type");
  entity.pass_file_ = text_of(doc, root, "pass_file");
  entity.variations_ = doc.attribute_int(root, "variations", 1);
  entity.radius_ = optional_int(doc, root, "radius");
  entity.selection_radius_ = optional_int(doc, root, "selection_radius");
  entity.floating_turnspeed_ = optional_int(doc, root, "floating_turnspeed");

  for_each_descendant(doc, root, "image", [&](NodeIndex node) {
    EntityImage image;
    image.idx = doc.attribute_int(node, "idx", 0);
    image.file = text_of(doc, node, "file");
    image.order = anim_order_from_name(doc.attribute(node, "remaping"));
    image.declared_rows = static_cast<std::uint32_t>(std::max(0, doc.attribute_int(node, "rows", 1)));
    image.declared_columns =
        static_cast<std::uint32_t>(std::max(0, doc.attribute_int(node, "columns", 1)));
    image.declared_draw_mode = draw_mode_from_name(doc.attribute(node, "drawmode"));
    // Until a frame table says otherwise the declaration is all we have; the
    // flag is what tells a caller the difference.
    image.geometry = ImageGeometry{image.declared_rows, image.declared_columns,
                                   image.declared_draw_mode, false};
    entity.images_.push_back(std::move(image));
  });

  for_each_descendant(doc, root, "point", [&](NodeIndex node) {
    entity.points_.push_back(EntityPoint{
        doc.attribute_int(node, "idx", 0),
        doc.attribute_int(node, "type", 0),
        doc.attribute_int(node, "x", 0),
        doc.attribute_int(node, "y", 0),
    });
  });

  for_each_descendant(doc, root, "layer", [&](NodeIndex node) {
    EntityLayer layer;
    layer.idx = doc.attribute_int(node, "idx", 0);
    layer.name = text_of(doc, node, "name");
    layer.image = doc.attribute_int(node, "image", 0);
    layer.z = doc.attribute_int(node, "z", 0);
    layer.offsetx = doc.attribute_int(node, "offsetx", 0);
    layer.offsety = doc.attribute_int(node, "offsety", 0);
    layer.sortoffsetx = doc.attribute_int(node, "sortoffsetx", 0);
    layer.sortoffsety = doc.attribute_int(node, "sortoffsety", 0);
    layer.xray = doc.attribute_int(node, "xray", 0);
    layer.nohighlight = doc.attribute_int(node, "nohighlight", 0);
    layer.percent = optional_int(doc, node, "percent");
    entity.layers_.push_back(std::move(layer));
  });

  for_each_descendant(doc, root, "state", [&](NodeIndex node) {
    EntityState state;
    state.idx = doc.attribute_int(node, "idx", 0);
    state.name = text_of(doc, node, "name");
    state.image_idx = doc.attribute_int(node, "image_idx", 0);
    state.image_row = doc.attribute_int(node, "image_row", 0);
    state.offsetx = doc.attribute_int(node, "offsetx", 0);
    state.offsety = doc.attribute_int(node, "offsety", 0);
    state.anim_idx = doc.attribute_int(node, "anim_idx", kNoAnim);
    state.anim_frame = doc.attribute_int(node, "anim_frame", kNoAnim);
    state.anim_row = optional_int(doc, node, "anim_row");
    entity.states_.push_back(std::move(state));
  });

  for_each_descendant(doc, root, "anim", [&](NodeIndex node) {
    EntityAnim anim;
    anim.idx = doc.attribute_int(node, "idx", 0);
    anim.name = text_of(doc, node, "name");
    anim.startstate = doc.attribute_int(node, "startstate", 0);
    anim.endstate = doc.attribute_int(node, "endstate", 0);
    anim.frames = doc.attribute_int(node, "frames", 0);
    anim.duration = doc.attribute_int(node, "duration", 0);
    anim.default_duration = doc.attribute_int(node, "default_duration", 0);
    anim.action_time = doc.attribute_int(node, "action_time", 0);
    anim.step = doc.attribute_int(node, "step", 0);
    anim.floating_heading = doc.attribute(node, "floating") == "1";
    for_each_descendant(doc, node, "replace", [&](NodeIndex child) {
      anim.replaces.push_back(AnimReplace{
          doc.attribute_int(child, "layer", 0),
          doc.attribute_int(child, "image", 0),
          doc.attribute_int(child, "offsetx", 0),
          doc.attribute_int(child, "offsety", 0),
      });
    });
    for_each_descendant(doc, node, "frame", [&](NodeIndex child) {
      anim.frame_durations.push_back(doc.attribute_int(child, "duration", 0));
    });
    entity.anims_.push_back(std::move(anim));
  });

  return entity;
}

const EntityImage* Entity::image(std::int32_t idx) const noexcept {
  for (auto it = images_.rbegin(); it != images_.rend(); ++it) {
    if (it->idx == idx) return &*it;
  }
  return nullptr;
}

const EntityLayer* Entity::layer(std::int32_t idx) const noexcept {
  for (auto it = layers_.rbegin(); it != layers_.rend(); ++it) {
    if (it->idx == idx) return &*it;
  }
  return nullptr;
}

const EntityState* Entity::state(std::int32_t idx) const noexcept {
  for (auto it = states_.rbegin(); it != states_.rend(); ++it) {
    if (it->idx == idx) return &*it;
  }
  return nullptr;
}

const EntityAnim* Entity::anim(std::int32_t slot) const noexcept {
  for (auto it = anims_.rbegin(); it != anims_.rend(); ++it) {
    if (it->idx == slot) return &*it;
  }
  return nullptr;
}

const EntityAnim* Entity::anim_for(const EntityState& state) const noexcept {
  if (!state.has_anim()) return nullptr;
  return anim(state.anim_idx);
}

std::vector<std::uint32_t> Entity::draw_order() const {
  std::vector<std::uint32_t> order(layers_.size());
  for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  // Stable, so equal z keeps declaration order. Iteration order is state in
  // this engine (docs/engine/architecture.md), and a comparison-sort tie broken
  // arbitrarily would be a real desync hazard, not a cosmetic one.
  std::stable_sort(order.begin(), order.end(), [this](std::uint32_t a, std::uint32_t b) {
    return layers_[a].z < layers_[b].z;
  });
  return order;
}

Result<GeometryConflict> Entity::adopt_frame_table(std::int32_t idx, const RleImage& sheet) {
  EntityImage* target = nullptr;
  for (auto it = images_.rbegin(); it != images_.rend(); ++it) {
    if (it->idx == idx) {
      target = &*it;
      break;
    }
  }
  if (target == nullptr) return FormatError::not_found;

  const DrawMode sheet_mode = draw_mode_from_image_class(sheet.image_class());
  GeometryConflict conflict;
  conflict.rows = sheet.rows() != target->declared_rows;
  conflict.columns = sheet.columns() != target->declared_columns;
  conflict.draw_mode = sheet_mode != target->declared_draw_mode;

  target->geometry = ImageGeometry{sheet.rows(), sheet.columns(), sheet_mode, true};
  return conflict;
}

std::size_t Entity::unresolved_geometry() const noexcept {
  std::size_t count = 0;
  for (const EntityImage& image : images_) {
    if (!image.geometry.from_frame_table) ++count;
  }
  return count;
}

// --------------------------------------------------------------------------
// reference resolution
// --------------------------------------------------------------------------

std::string normalise_resource_path(std::string_view path) {
  std::size_t begin = 0;
  std::size_t end = path.size();
  while (begin < end && (path[begin] == ' ' || path[begin] == '\t')) ++begin;
  while (end > begin && (path[end - 1] == ' ' || path[end - 1] == '\t')) --end;

  std::string out;
  out.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    char c = path[i];
    if (c == '/') c = '\\';
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    out.push_back(c);
  }
  // `gameres/icons/x.bmp` is the pack entry `UI\ICONS\X.BMP`. The virtual root
  // appears on icon references rather than on entities, but the folding rule is
  // one rule and lives in one place.
  constexpr std::string_view kVirtualRoot = "GAMERES\\";
  if (out.size() >= kVirtualRoot.size() &&
      std::string_view(out).substr(0, kVirtualRoot.size()) == kVirtualRoot) {
    out.replace(0, kVirtualRoot.size(), "UI\\");
  }
  return out;
}

std::string_view entity_directory(std::string_view entity_path) noexcept {
  const std::size_t slash = entity_path.find_last_of("\\/");
  if (slash == std::string_view::npos) return {};
  return entity_path.substr(0, slash);
}

std::array<std::string, 3> image_path_candidates(std::string_view entity_path,
                                                 std::string_view file) {
  const std::string stem = join_path(entity_directory(entity_path), file);
  return {normalise_resource_path(stem), normalise_resource_path(stem + ".MMP"),
          normalise_resource_path(stem + ".RLE.MMP")};
}

std::array<std::string, 2> pass_path_candidates(std::string_view entity_path,
                                                std::string_view file) {
  const std::string stem = join_path(entity_directory(entity_path), file);
  return {normalise_resource_path(stem), normalise_resource_path(stem + ".PASS")};
}

// --------------------------------------------------------------------------
// the loaded set
// --------------------------------------------------------------------------

Result<const Entity*> EntityLibrary::load(std::string_view path,
                                          std::span<const std::byte> xml) {
  const std::string key = normalise_resource_path(path);
  if (const Entity* existing = find(key)) return existing;

  auto parsed = Entity::parse(xml, key);
  if (!parsed) return parsed.error();

  entities_.push_back(std::make_unique<Entity>(std::move(*parsed)));
  const auto at = std::lower_bound(
      index_.begin(), index_.end(), key,
      [](const std::pair<std::string, std::uint32_t>& entry, const std::string& value) {
        return entry.first < value;
      });
  index_.insert(at, {key, static_cast<std::uint32_t>(entities_.size() - 1)});
  return entities_.back().get();
}

Entity* EntityLibrary::find_mutable(std::string_view path) {
  // Deliberately implemented in terms of the const one rather than duplicating
  // the lookup: two copies of a normalise-and-binary-search would drift.
  return const_cast<Entity*>(std::as_const(*this).find(path));
}

const Entity* EntityLibrary::find(std::string_view path) const {
  const std::string key = normalise_resource_path(path);
  const auto at = std::lower_bound(
      index_.begin(), index_.end(), key,
      [](const std::pair<std::string, std::uint32_t>& entry, const std::string& value) {
        return entry.first < value;
      });
  if (at == index_.end() || at->first != key) return nullptr;
  return entities_[at->second].get();
}

// --------------------------------------------------------------------------
// depth sort bins
// --------------------------------------------------------------------------

Result<ZBins> ZBins::parse(std::span<const std::byte> xml) {
  auto document = XmlDocument::parse(xml);
  if (!document) return document.error();
  return from_document(*document);
}

Result<ZBins> ZBins::from_document(const XmlDocument& doc) {
  const NodeIndex root = doc.root();
  if (root == kNoNode) return FormatError::malformed;
  if (doc.node(root).name != "zbins") return FormatError::malformed;

  ZBins bins;
  for (NodeIndex node = doc.child(root, "zbin"); node != kNoNode;
       node = doc.next(node, "zbin")) {
    bins.bins_.push_back(Bin{doc.attribute_int(node, "startz", 0),
                             doc.attribute_int(node, "sort", 0) != 0});
  }
  std::stable_sort(bins.bins_.begin(), bins.bins_.end(),
                   [](const Bin& a, const Bin& b) { return a.start_z < b.start_z; });
  return bins;
}

std::size_t ZBins::bin_for(std::int32_t z) const noexcept {
  std::size_t found = bins_.size();
  for (std::size_t i = 0; i < bins_.size(); ++i) {
    if (bins_[i].start_z <= z) found = i;
  }
  return found;
}

bool ZBins::sorted_at(std::int32_t z) const noexcept {
  const std::size_t index = bin_for(z);
  return index < bins_.size() && bins_[index].sorted;
}

}  // namespace imperivm::core
