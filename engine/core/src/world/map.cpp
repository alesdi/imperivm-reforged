#include "imperivm/core/world/map.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace imperivm::core {
namespace {

/// 0xCDCDCDCD read as a signed 32-bit integer, which `randommap.BFHP` ships in
/// both halves of its `start_pt`: uninitialised memory serialised into the file.
constexpr std::int32_t kUninitialised = -842150451;

std::string to_string(std::string_view view) { return std::string(view); }

/// Fold a reference to the pack index's spelling: upper case, backslashes.
std::string fold(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (const char c : path) {
    if (c == '/') {
      out.push_back('\\');
    } else if (c >= 'a' && c <= 'z') {
      out.push_back(static_cast<char>(c - 'a' + 'A'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

}  // namespace

// --------------------------------------------------------------------------
// map.xml, game.xml
// --------------------------------------------------------------------------

Result<MapGeometry> MapGeometry::parse(std::span<const std::byte> xml) {
  auto doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  const NodeIndex root = doc->root();
  if (root == kNoNode || doc->node(root).name != "map") return FormatError::malformed;

  MapGeometry out;
  out.name = to_string(doc->attribute(root, "name"));
  out.display_name = to_string(doc->attribute(root, "displayname"));

  const NodeIndex size = doc->child(root, "size");
  if (size == kNoNode) return FormatError::malformed;
  out.size_x = doc->attribute_int(size, "x");
  out.size_y = doc->attribute_int(size, "y");
  if (out.size_x <= 0 || out.size_y <= 0) return FormatError::malformed;

  const NodeIndex start = doc->child(root, "start_pt");
  if (start != kNoNode) {
    const std::int32_t x = doc->attribute_int(start, "x", kUninitialised);
    const std::int32_t y = doc->attribute_int(start, "y", kUninitialised);
    if (x != kUninitialised && y != kUninitialised && x >= 0 && y >= 0) {
      out.start_x = x;
      out.start_y = y;
      out.has_start = true;
    }
  }
  return out;
}

Result<GameProperties> GameProperties::parse(std::span<const std::byte> xml) {
  auto doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  const NodeIndex root = doc->root();
  if (root == kNoNode || doc->node(root).name != "game") return FormatError::malformed;
  const NodeIndex properties = doc->child(root, "properties");
  if (properties == kNoNode) return FormatError::malformed;

  GameProperties out;
  out.name = to_string(doc->attribute(properties, "name"));
  out.description = to_string(doc->attribute(properties, "description"));
  out.start_player = doc->attribute_int(properties, "start_player", 0);
  const std::string_view season = doc->attribute(properties, "season");
  if (!season.empty()) out.season = to_string(season);
  out.start_map = doc->attribute_int(properties, "start_map", 1);
  out.game_type = doc->attribute_int(properties, "game_type", 0);
  return out;
}

Result<PlayerSlot> PlayerSlot::parse(std::span<const std::byte> xml) {
  auto doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  const NodeIndex root = doc->root();
  if (root == kNoNode || doc->node(root).name != "playerdata") return FormatError::malformed;

  PlayerSlot out;
  out.id = doc->attribute_int(root, "id");
  out.name = to_string(doc->attribute(root, "name"));
  out.race = to_string(doc->attribute(root, "race"));
  out.control = to_string(doc->attribute(root, "control"));
  out.start_x = doc->attribute_int(root, "startx");
  out.start_y = doc->attribute_int(root, "starty");

  const std::int32_t packed = doc->attribute_int(root, "color");
  const auto expand = [](std::int32_t value) -> std::uint8_t {
    return static_cast<std::uint8_t>((value * 255 + 15) / 31);
  };
  out.color.red = expand((packed >> 10) & 31);
  out.color.green = expand((packed >> 5) & 31);
  out.color.blue = expand(packed & 31);
  return out;
}

// --------------------------------------------------------------------------
// player<i>.xml, for the simulation
// --------------------------------------------------------------------------

namespace {

/// The shared body of both entry points. Reports which slot it wrote, so that
/// `load_player_table` can detect a duplicated `id` without parsing twice.
Status read_playerdata(std::span<const std::byte> xml, sim::PlayerTable& table,
                       sim::PlayerId& slot_out) {
  auto doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  const NodeIndex root = doc->root();
  if (root == kNoNode || doc->node(root).name != "playerdata") return FormatError::malformed;

  // The slot is the document's own, not the caller's. All 304 shipped documents
  // carry `id`, and one that does not is a document whose row we would have to
  // guess -- so the fallback is deliberately out of range rather than zero.
  constexpr std::int32_t kNoId = -1;
  const std::int32_t id = doc->attribute_int(root, "id", kNoId);
  if (id < 0 || id >= static_cast<std::int32_t>(sim::kPlayerCount)) return FormatError::malformed;
  const sim::PlayerId slot = static_cast<sim::PlayerId>(id);

  // Decoded into a local first, and the relations row applied before any of it
  // is committed: a document that is half-good must leave the table alone.
  sim::PlayerSetup setup;
  setup.name = to_string(doc->attribute(root, "name"));
  setup.race = to_string(doc->attribute(root, "race"));
  // `AllowedRaces` is absent in `randommap.BFHP` and `RandomMapSettlements.bfhp`
  // and is `All` in the other 272 documents.
  const std::string_view allowed = doc->attribute(root, "AllowedRaces");
  if (!allowed.empty()) setup.allowed_races = to_string(allowed);
  setup.control = sim::parse_player_control(doc->attribute(root, "control"));
  // `difficulty` and `bonus` are absent in `randommap.BFHP` alone.
  setup.difficulty = doc->attribute_int(root, "difficulty", 0);
  // A decimal integer holding a 16-bit RGB555 word; `docs/formats/rle.md`
  // unpacks it. Kept verbatim -- 0 is a real value (51 documents ship it) and
  // means black, not "absent". The observed range is 0..32736.
  setup.colour = static_cast<std::uint16_t>(doc->attribute_int(root, "color", 0) & 0xFFFF);
  setup.start.x = doc->attribute_int(root, "startx", 0);
  setup.start.y = doc->attribute_int(root, "starty", 0);
  setup.bonus = doc->attribute_int(root, "bonus", -1);
  setup.allied_flag = doc->attribute_int(root, "allied", 0) != 0;
  setup.ai_script = to_string(doc->attribute(root, "AI"));

  const std::string_view relations = doc->attribute(root, "relations");
  if (relations.empty()) return FormatError::malformed;
  if (!table.set_row_from_hex(slot, relations)) return FormatError::malformed;

  table.setup(slot) = std::move(setup);
  slot_out = slot;
  return Status();
}

}  // namespace

Status parse_player_setup(std::span<const std::byte> xml, sim::PlayerTable& table) {
  sim::PlayerId slot = sim::kNoPlayer;
  return read_playerdata(xml, table, slot);
}

Status load_player_table(std::span<const std::span<const std::byte>> documents,
                         sim::PlayerTable& table) {
  if (documents.size() != sim::kPlayerCount) return FormatError::malformed;

  // Parsed into a scratch table so that a bad sixteenth document cannot leave
  // fifteen good rows behind on the caller's.
  sim::PlayerTable parsed;
  std::array<bool, sim::kPlayerCount> seen{};
  for (const std::span<const std::byte> document : documents) {
    sim::PlayerId slot = sim::kNoPlayer;
    const Status status = read_playerdata(document, parsed, slot);
    if (!status) return status.error();
    // Two documents claiming the same `id` would silently leave one slot
    // unwritten -- and `id` is a permutation of 0..15 in all nineteen shipped
    // sets, so this is a container we do not understand rather than a table to
    // fill in partially.
    if (seen[slot]) return FormatError::malformed;
    seen[slot] = true;
  }
  for (const bool present : seen) {
    if (!present) return FormatError::malformed;
  }

  table = std::move(parsed);
  return Status();
}

// --------------------------------------------------------------------------
// map.obj.xml
// --------------------------------------------------------------------------

namespace {

MapObject read_object(const XmlDocument& doc, NodeIndex node, std::int32_t settlement) {
  MapObject out;
  out.class_name = to_string(doc.attribute(node, "class"));
  out.num = doc.attribute_int(node, "num");
  out.x = doc.attribute_int(node, "x");
  out.y = doc.attribute_int(node, "y");
  out.dir_x = doc.attribute_int(node, "dir.x", 0);
  out.dir_y = doc.attribute_int(node, "dir.y", 1);
  out.player = doc.attribute_int(node, "player", 0);
  out.settlement = settlement;
  // Health as authored. `healthperc` is a percentage of the class `maxhealth`
  // and defaults to 100 -- both for the objects that carry it at 100 (21,547 of
  // 21,549) and for the ones that carry no health attribute at all. `health` is
  // absolute and overrides it; it occurs exactly once in the retail install, so
  // the sentinel for "absent" has to be a value the attribute cannot take, and
  // -1 is that. See `MapObject`.
  out.health_percent = doc.attribute_int(node, "healthperc", 100);
  out.health_absolute = doc.attribute_int(node, "health", -1);
  // Decimal, unlike `flags` below: `UnitFlags="262144"`. Absent on everything
  // that is not a unit, and zero is a value 708 units genuinely carry.
  out.unit_flags = static_cast<std::uint32_t>(doc.attribute_int(node, "UnitFlags", 0));
  out.destination_set = doc.attribute_int(node, "destination_set", -1);

  // `flags` is hexadecimal with an `0x` prefix, which attribute_int does not
  // read; the low sixteen bits are a one-hot owner mask and agree with
  // `player` everywhere, so it is worth carrying rather than recomputing.
  const std::string_view flags = doc.attribute(node, "flags");
  std::uint32_t value = 0;
  std::size_t at = 0;
  if (flags.size() > 2 && flags[0] == '0' && (flags[1] == 'x' || flags[1] == 'X')) at = 2;
  for (; at < flags.size(); ++at) {
    const char c = flags[at];
    std::uint32_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint32_t>(c - 'A' + 10);
    } else {
      break;
    }
    value = value * 16 + digit;
  }
  out.flags = value;
  // Everything, verbatim: what the writer puts back.
  const XmlNode& element = doc.node(node);
  out.attributes.reserve(element.attribute_count);
  for (std::uint32_t i = 0; i < element.attribute_count; ++i) {
    const XmlAttribute& attribute = doc.attributes()[element.attribute_begin + i];
    out.attributes.emplace_back(std::string(attribute.name), std::string(attribute.value));
  }
  return out;
}

/// The trigger region on a `<scriptobj>`, or false when it carries none.
///
/// **Keyed on the shape attributes, not on `class` and not on `type`.** `type`
/// is also a `CVXWagon`'s cargo kind (26 objects in the retail data), so a
/// reader that branched on `type` alone would manufacture a degenerate
/// rectangle at the origin for every wagon. Requiring the whole attribute set
/// its `type` implies makes the two disjoint by construction: the split is
/// perfectly clean in the shipped data -- 661 objects carry `ptx`/`pty`/`r`
/// and 243 carry `left`/`top`/`right`/`bottom`, and none carries either set
/// without the matching `type`.
[[nodiscard]] bool read_area(const XmlDocument& doc, NodeIndex node, std::int32_t num,
                             MapArea& out) {
  const std::string_view type = doc.attribute(node, "type");
  if (type.empty()) return false;

  out = MapArea{};
  out.num = num;
  out.type = doc.attribute_int(node, "type");
  if (out.type == kAreaCircle) {
    if (doc.attribute(node, "ptx").empty() || doc.attribute(node, "pty").empty() ||
        doc.attribute(node, "r").empty()) {
      return false;
    }
    out.ptx = doc.attribute_int(node, "ptx");
    out.pty = doc.attribute_int(node, "pty");
    out.radius = doc.attribute_int(node, "r");
    return true;
  }
  if (out.type == kAreaRectangle) {
    if (doc.attribute(node, "left").empty() || doc.attribute(node, "top").empty() ||
        doc.attribute(node, "right").empty() || doc.attribute(node, "bottom").empty()) {
      return false;
    }
    out.left = doc.attribute_int(node, "left");
    out.top = doc.attribute_int(node, "top");
    out.right = doc.attribute_int(node, "right");
    out.bottom = doc.attribute_int(node, "bottom");
    // Authored corners are not guaranteed to be ordered by the editor, and a
    // reversed pair would make the region empty rather than wrong-looking.
    // Every shipped rectangle is already ordered; normalising costs nothing
    // and removes the question.
    if (out.left > out.right) std::swap(out.left, out.right);
    if (out.top > out.bottom) std::swap(out.top, out.bottom);
    return true;
  }
  return false;
}

}  // namespace

Result<MapObjectList> MapObjectList::parse(std::span<const std::byte> xml) {
  auto doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  return from_document(*doc);
}

Result<MapObjectList> MapObjectList::from_document(const XmlDocument& doc) {
  const NodeIndex root = doc.root();
  if (root == kNoNode || doc.node(root).name != "mapobject") return FormatError::malformed;

  MapObjectList out;
  for (NodeIndex child = doc.node(root).first_child; child != kNoNode;
       child = doc.node(child).next_sibling) {
    const std::string_view name = doc.node(child).name;
    if (name == "scriptobj") {
      out.objects_.push_back(read_object(doc, child, -1));
      MapArea area;
      if (read_area(doc, child, out.objects_.back().num, area)) out.areas_.push_back(area);
    } else if (name == "settlement") {
      MapSettlement settlement;
      settlement.id = doc.attribute_int(child, "id");
      settlement.player = doc.attribute_int(child, "player");
      settlement.name = to_string(doc.attribute(child, "name"));
      settlement.class_of_first_building =
          to_string(doc.attribute(child, "classoffirstbuilding"));
      {
        const XmlNode& element = doc.node(child);
        for (std::uint32_t i = 0; i < element.attribute_count; ++i) {
          const XmlAttribute& attribute = doc.attributes()[element.attribute_begin + i];
          settlement.attributes.emplace_back(std::string(attribute.name),
                                             std::string(attribute.value));
        }
      }
      settlement.population = doc.attribute_int(child, "population", -1);
      settlement.max_population = doc.attribute_int(child, "maxpopulation", -1);
      settlement.gold = doc.attribute_int(child, "gold", -1);
      settlement.food = doc.attribute_int(child, "food", -1);
      settlement.max_gold = doc.attribute_int(child, "maxgold", -1);
      settlement.max_food = doc.attribute_int(child, "maxfood", -1);
      settlement.extra_sentries = doc.attribute_int(child, "extrasentries", -1);
      const auto index = static_cast<std::int32_t>(out.settlements_.size());
      out.settlements_.push_back(std::move(settlement));
      for (NodeIndex member = doc.child(child, "scriptobj"); member != kNoNode;
           member = doc.next(member, "scriptobj")) {
        out.objects_.push_back(read_object(doc, member, index));
        // No shipped area is inside a `<settlement>`, but the element is the
        // same element and reading it here costs one call: an area that were
        // skipped for being nested would be a silently nameless region.
        MapArea area;
        if (read_area(doc, member, out.objects_.back().num, area)) out.areas_.push_back(area);
      }
    } else if (name == "group") {
      // Kept one entry per element, not merged by name: four shipped maps
      // repeat a name across several `<group>`s and the document order of the
      // elements is what decides the merge. Merging is `sim::GroupTable`'s job,
      // where the live table is; this reader stays a transcription of the file.
      MapGroup group;
      group.name = to_string(doc.attribute(child, "name"));
      group.type = doc.attribute_int(child, "type");
      for (NodeIndex member = doc.child(child, "obj"); member != kNoNode;
           member = doc.next(member, "obj")) {
        group.members.push_back(doc.attribute_int(member, "num"));
      }
      out.groups_.push_back(std::move(group));
    }
  }
  return out;
}

const MapObject* MapObjectList::find(std::int32_t num) const noexcept {
  // `num` is contiguous from zero in document order in every shipped map, so
  // the direct index is almost always right; the scan is the fallback for data
  // that does not honour it.
  if (num >= 0 && static_cast<std::size_t>(num) < objects_.size() &&
      objects_[static_cast<std::size_t>(num)].num == num) {
    return &objects_[static_cast<std::size_t>(num)];
  }
  for (const MapObject& object : objects_) {
    if (object.num == num) return &object;
  }
  return nullptr;
}

namespace {

using Attributes = std::vector<std::pair<std::string, std::string>>;

std::string hex_flags(std::uint32_t flags) {
  static constexpr char kDigits[] = "0123456789ABCDEF";  // `0x8080000A`, upper case as authored
  std::string out = "0x00000000";
  for (int i = 0; i < 8; ++i) out[9 - static_cast<std::size_t>(i)] = kDigits[(flags >> (4 * i)) & 0xF];
  return out;
}

/// Overwrite `name` in place, or insert it before the first of `before`
/// that is present (at the end when none is).
void put_attribute(Attributes& list, std::string_view name, std::string value,
                   std::initializer_list<std::string_view> before) {
  for (auto& entry : list) {
    if (entry.first == name) {
      entry.second = std::move(value);
      return;
    }
  }
  auto at = list.end();
  for (const std::string_view anchor : before) {
    const auto found = std::find_if(list.begin(), list.end(),
                                    [&](const auto& entry) { return entry.first == anchor; });
    if (found != list.end()) {
      at = found;
      break;
    }
  }
  list.emplace(at, std::string(name), std::move(value));
}

void erase_attribute(Attributes& list, std::string_view name) {
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const auto& entry) { return entry.first == name; }),
             list.end());
}

}  // namespace

void MapObject::set_attribute(std::string_view name, std::string value) {
  put_attribute(attributes, name, std::move(value), {"x", "y", "flags", "dir.x", "dir.y"});
}

void MapObject::drop_attribute(std::string_view name) {
  erase_attribute(attributes, name);
}

void MapObject::sync_attributes() {
  // The shipped order is `class, num, <the class's own>, player, healthperc,
  // [stamina, inventorysize, slots], x, y, flags, dir.x, dir.y`; each field
  // goes before the first tail attribute that follows it. An element read
  // from a file keeps its own set -- the one object in the install with no
  // `dir.x` stays without one -- and only a fresh object gets the whole tail.
  const bool fresh = attributes.empty();
  const auto has = [&](std::string_view name) { return !attribute(name).empty(); };
  // `class` first and `num` second, wherever the list had them.
  erase_attribute(attributes, "class");
  erase_attribute(attributes, "num");
  attributes.emplace(attributes.begin(), "num", std::to_string(num));
  attributes.emplace(attributes.begin(), "class", class_name);
  if (destination_set >= 0) {
    put_attribute(attributes, "destination_set", std::to_string(destination_set),
                  {"player", "healthperc", "x"});
  } else {
    erase_attribute(attributes, "destination_set");
  }
  if (unit_flags != 0 || has("UnitFlags")) {
    put_attribute(attributes, "UnitFlags", std::to_string(unit_flags), {"player", "healthperc", "x"});
  }
  if (fresh || has("player") || player != 0) {
    put_attribute(attributes, "player", std::to_string(player), {"healthperc", "health", "x"});
  }
  if (health_absolute >= 0) {
    put_attribute(attributes, "health", std::to_string(health_absolute), {"x"});
    erase_attribute(attributes, "healthperc");
  } else {
    if (fresh || has("healthperc") || health_percent != 100) {
      put_attribute(attributes, "healthperc", std::to_string(health_percent),
                    {"stamina", "inventorysize", "x"});
    }
    erase_attribute(attributes, "health");
  }
  put_attribute(attributes, "x", std::to_string(x), {"y", "flags"});
  put_attribute(attributes, "y", std::to_string(y), {"flags"});
  put_attribute(attributes, "flags", hex_flags(flags), {"dir.x"});
  if (fresh || has("dir.x") || has("dir.y") || dir_x != 0 || dir_y != 1) {
    put_attribute(attributes, "dir.x", std::to_string(dir_x), {"dir.y"});
    put_attribute(attributes, "dir.y", std::to_string(dir_y), {});
  }
}

std::string_view MapObject::attribute(std::string_view name) const noexcept {
  for (const auto& entry : attributes) {
    if (entry.first == name) return entry.second;
  }
  return {};
}

void MapSettlement::sync_attributes() {
  // `id, player, classoffirstbuilding, maxpopulation, extrasentries, maxgold,
  // maxfood, population, gold, food, icon, name` on 683 of 684.
  const bool fresh = attributes.empty();
  put_attribute(attributes, "id", std::to_string(id), {"player"});
  put_attribute(attributes, "player", std::to_string(player), {"classoffirstbuilding", "type", "maxpopulation"});
  // 683 of 684 carry it; the one that does not (`randommap.BFHP`'s Townhall,
  // with `type`, `efficiency` and `maxunits` instead) keeps its own set.
  const bool has_class = std::any_of(attributes.begin(), attributes.end(), [](const auto& entry) {
    return entry.first == "classoffirstbuilding";
  });
  if (fresh || has_class || !class_of_first_building.empty()) {
    put_attribute(attributes, "classoffirstbuilding", class_of_first_building, {"maxpopulation"});
  }
  const auto economy = [&](std::string_view key, std::int32_t value,
                           std::initializer_list<std::string_view> before) {
    if (value < 0) return;
    put_attribute(attributes, key, std::to_string(value), before);
  };
  economy("maxpopulation", max_population, {"extrasentries", "maxgold"});
  economy("extrasentries", extra_sentries, {"maxgold"});
  economy("maxgold", max_gold, {"maxfood"});
  economy("maxfood", max_food, {"population"});
  economy("population", population, {"gold"});
  economy("gold", gold, {"food"});
  economy("food", food, {"icon", "name"});
  put_attribute(attributes, "name", name, {});
}

std::int32_t MapObjectList::next_num() const noexcept {
  std::int32_t next = 0;
  for (const MapObject& object : objects_) next = std::max(next, object.num + 1);
  return next;
}

bool MapObjectList::erase(std::int32_t num) {
  const auto at = std::find_if(objects_.begin(), objects_.end(),
                               [num](const MapObject& object) { return object.num == num; });
  if (at == objects_.end()) return false;
  objects_.erase(at);
  areas_.erase(std::remove_if(areas_.begin(), areas_.end(),
                              [num](const MapArea& area) { return area.num == num; }),
               areas_.end());
  for (MapGroup& group : groups_) {
    group.members.erase(std::remove(group.members.begin(), group.members.end(), num),
                        group.members.end());
  }
  groups_.erase(std::remove_if(groups_.begin(), groups_.end(),
                               [](const MapGroup& group) { return group.members.empty(); }),
                groups_.end());
  return true;
}

const MapArea* MapObjectList::find_area(std::int32_t num) const noexcept {
  // Linear over the 904-at-most areas of one map rather than an index: this is
  // called once per named alias at load and never again, and a side index is a
  // second thing to keep in step with the first.
  for (const MapArea& area : areas_) {
    if (area.num == num) return &area;
  }
  return nullptr;
}

// --------------------------------------------------------------------------
// DATA\TERRAINS.XML
// --------------------------------------------------------------------------

Result<TerrainTable> TerrainTable::parse(std::span<const std::byte> xml) {
  auto doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  const NodeIndex root = doc->root();
  if (root == kNoNode || doc->node(root).name != "terrain") return FormatError::malformed;

  TerrainTable out;
  for (NodeIndex node = doc->child(root, "layer"); node != kNoNode;
       node = doc->next(node, "layer")) {
    TerrainLayerDef layer;
    layer.z = doc->attribute_int(node, "z");
    layer.type = doc->attribute_int(node, "type");
    layer.frames = doc->attribute_int(node, "frames", 0);
    layer.transition = doc->attribute_int(node, "transition", kDefaultTransition);
    layer.passable = doc->attribute_bool(node, "passable", true);
    layer.passable_water = doc->attribute_bool(node, "passable_water", false);
    layer.dark = doc->attribute_bool(node, "dark", false);
    layer.display = to_string(doc->attribute(node, "display"));
    layer.image = to_string(doc->attribute(node, "image"));
    layer.minimap = to_string(doc->attribute(node, "minimap"));
    out.layers_.push_back(std::move(layer));
  }
  if (out.layers_.empty()) return FormatError::malformed;
  return out;
}

const TerrainLayerDef* TerrainTable::layer(std::int32_t z) const noexcept {
  for (const TerrainLayerDef& def : layers_) {
    if (def.z == z) return &def;
  }
  return nullptr;
}

std::string TerrainTable::texture_path(std::string_view image, std::string_view season) {
  static constexpr std::string_view kToken = "%season%";
  std::string out;
  out.reserve(image.size() + season.size());
  std::size_t at = 0;
  while (at < image.size()) {
    if (image.compare(at, kToken.size(), kToken) == 0) {
      out.append(season);
      at += kToken.size();
    } else {
      out.push_back(image[at]);
      ++at;
    }
  }
  return fold(out);
}

// --------------------------------------------------------------------------
// transitions
// --------------------------------------------------------------------------

std::size_t terrain_overlays(const Grid& terrain, std::int32_t cx, std::int32_t cy,
                             std::span<TerrainOverlay> out) {
  if (terrain.cell_size() == 0 || out.empty()) return 0;
  const auto width = static_cast<std::int32_t>(terrain.width());
  const auto height = static_cast<std::int32_t>(terrain.height());
  if (width <= 0 || height <= 0) return 0;

  const auto sample = [&](std::int32_t x, std::int32_t y) -> std::int32_t {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return static_cast<std::int32_t>(
        terrain.cell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)));
  };

  const std::int32_t self = sample(cx, cy);

  // Each corner touches three cells. A neighbouring type claims the corner if
  // it is any of them.
  struct CornerCells {
    std::uint8_t bit;
    std::int32_t dx[3];
    std::int32_t dy[3];
  };
  static constexpr std::array<CornerCells, 4> kCorners{{
      {kCornerTopLeft, {-1, -1, 0}, {0, -1, -1}},
      {kCornerTopRight, {0, 1, 1}, {-1, -1, 0}},
      {kCornerBottomRight, {1, 1, 0}, {0, 1, 1}},
      {kCornerBottomLeft, {0, -1, -1}, {1, 1, 0}},
  }};

  std::array<std::int32_t, kMaxTerrainOverlays> types{};
  std::array<std::uint8_t, kMaxTerrainOverlays> corners{};
  std::size_t found = 0;

  for (const CornerCells& corner : kCorners) {
    for (int i = 0; i < 3; ++i) {
      const std::int32_t type = sample(cx + corner.dx[i], cy + corner.dy[i]);
      if (type == self) continue;
      // Priority is the layer `z` itself: only a higher type is drawn over.
      if (type <= self) continue;
      std::size_t slot = 0;
      for (; slot < found; ++slot) {
        if (types[slot] == type) break;
      }
      if (slot == found) {
        if (found == types.size()) continue;
        types[found] = type;
        corners[found] = 0;
        ++found;
      }
      corners[slot] |= corner.bit;
    }
  }

  // Ascending by type, so that a cell bordering two higher terrains composites
  // them in a fixed order rather than in neighbour-scan order.
  std::array<std::size_t, kMaxTerrainOverlays> order{};
  for (std::size_t i = 0; i < found; ++i) order[i] = i;
  std::sort(order.begin(), order.begin() + static_cast<long>(found),
            [&](std::size_t a, std::size_t b) { return types[a] < types[b]; });

  const std::size_t count = std::min(found, out.size());
  for (std::size_t i = 0; i < count; ++i) {
    out[i].type = types[order[i]];
    out[i].corners = corners[order[i]];
  }
  return count;
}

std::string transition_mask_name(char style, std::uint8_t corners) {
  std::string name(5, '0');
  name[0] = style;
  name[1] = (corners & kCornerTopLeft) != 0 ? '1' : '0';
  name[2] = (corners & kCornerTopRight) != 0 ? '1' : '0';
  name[3] = (corners & kCornerBottomRight) != 0 ? '1' : '0';
  name[4] = (corners & kCornerBottomLeft) != 0 ? '1' : '0';
  return name;
}

TerrainTile terrain_tile(const Grid& terrain, std::int32_t cx, std::int32_t cy) {
  TerrainTile tile;
  if (terrain.cell_size() == 0) return tile;
  const auto width = static_cast<std::int32_t>(terrain.width());
  const auto height = static_cast<std::int32_t>(terrain.height());
  if (width <= 0 || height <= 0) return tile;

  const auto sample = [&](std::int32_t x, std::int32_t y) -> std::int32_t {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return static_cast<std::int32_t>(
        terrain.cell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)));
  };
  tile.corners = {sample(cx, cy), sample(cx + 1, cy), sample(cx + 1, cy + 1), sample(cx, cy + 1)};
  static constexpr std::array<std::uint8_t, 4> kBits{kCornerTopLeft, kCornerTopRight,
                                                     kCornerBottomRight, kCornerBottomLeft};
  const auto occupied = [&](std::int32_t type) {
    std::uint8_t bits = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      if (tile.corners[i] == type) bits = static_cast<std::uint8_t>(bits | kBits[i]);
    }
    return bits;
  };
  const auto is_water = [](std::int32_t type) {
    return type == kShallowWaterLayer || type == kDeepWaterLayer;
  };

  // The four corner types, ascending and without repeats: the only layers
  // the original's scan over 0..255 can find.
  std::array<std::int32_t, 4> present = tile.corners;
  std::sort(present.begin(), present.end());
  const auto end = std::unique(present.begin(), present.end());
  const bool all_water = std::all_of(tile.corners.begin(), tile.corners.end(), is_water);

  // The base: the lowest present, shallow water skipped unless all is water.
  tile.base = present[0];
  for (auto it = present.begin(); it != end; ++it) {
    if (*it != kShallowWaterLayer || all_water) {
      tile.base = *it;
      break;
    }
  }

  // Three passes: land, then shallow water, then deep water.
  for (int pass = 0; pass < 3; ++pass) {
    for (auto it = present.begin(); it != end; ++it) {
      const std::int32_t type = *it;
      if (type == tile.base) continue;
      if (pass == 0 && is_water(type)) continue;
      if (pass == 1 && type != kShallowWaterLayer) continue;
      if (pass == 2 && type != kDeepWaterLayer) continue;
      tile.overlays[tile.overlay_count++] = TerrainTileLayer{type, occupied(type)};
    }
  }
  return tile;
}

char transition_style(std::int32_t transition, std::int32_t cy) {
  return static_cast<char>('A' + transition + (cy & 1));
}

// --------------------------------------------------------------------------
// DECORS.INI
// --------------------------------------------------------------------------

namespace {

std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

}  // namespace

Result<DecorTable> DecorTable::parse(std::span<const std::byte> ini) {
  const std::string_view text(reinterpret_cast<const char*>(ini.data()), ini.size());
  DecorTable out;

  DecorKind current;
  bool in_section = false;
  const auto flush = [&]() {
    if (in_section && current.type != 0 && !current.entity.empty()) {
      out.kinds_.push_back(current);
    }
    current = DecorKind{};
    in_section = false;
  };

  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    const std::string_view line = trim(text.substr(at, end - at));
    at = end + 1;
    if (line.empty() || line.front() == ';') continue;
    if (line.front() == '[') {
      flush();
      const std::size_t close = line.find(']');
      current.section = std::string(line.substr(1, close == std::string_view::npos
                                                       ? line.size() - 1
                                                       : close - 1));
      in_section = true;
      continue;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) continue;
    std::string key(trim(line.substr(0, equals)));
    for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string_view value = trim(line.substr(equals + 1));
    if (key == "type") {
      std::int32_t number = 0;
      for (const char c : value) {
        if (c < '0' || c > '9') break;
        number = number * 10 + (c - '0');
      }
      current.type = number;
    } else if (key == "entity") {
      current.entity = fold(value);
    } else if (key == "group") {
      current.group = std::string(value);
    } else if (key == "subgroup") {
      current.subgroup = std::string(value);
    } else if (key == "name") {
      current.name = std::string(value);
    } else if (key == "season") {
      current.season = std::string(value);
    }
    if (at > text.size()) break;
  }
  flush();

  std::sort(out.kinds_.begin(), out.kinds_.end(),
            [](const DecorKind& a, const DecorKind& b) { return a.type < b.type; });
  if (out.kinds_.empty()) return FormatError::malformed;
  return out;
}

const DecorKind* DecorTable::find(std::int32_t type) const noexcept {
  const auto it = std::lower_bound(
      kinds_.begin(), kinds_.end(), type,
      [](const DecorKind& kind, std::int32_t value) { return kind.type < value; });
  if (it == kinds_.end() || it->type != type) return nullptr;
  return &*it;
}

// --------------------------------------------------------------------------
// WorldMap
// --------------------------------------------------------------------------

Status WorldMap::adopt(std::vector<std::byte>& owned, std::span<const std::byte> source,
                       Grid& grid) {
  if (source.empty()) return {};  // an absent layer is not fatal
  owned.assign(source.begin(), source.end());
  // Over the copy, writable: the editor paints into these bytes through the
  // `*_mut()` accessors and the same `Grid` answers the const reads.
  auto parsed = Grid::parse_mutable(owned);
  if (!parsed) return parsed.error();
  grid = parsed.value();
  return {};
}

bool WorldMap::widen_terrain() {
  if (terrain_.cell_size() == 0 || terrain_.bits_per_cell() == 8) return true;
  auto wide = OwnedGrid::create(terrain_.cell_size(), 8, terrain_.extent_x(), terrain_.extent_y());
  if (!wide) return false;
  for (std::uint32_t y = 0; y < terrain_.height(); ++y) {
    for (std::uint32_t x = 0; x < terrain_.width(); ++x) {
      if (!wide->grid().set_cell(x, y, terrain_.cell(x, y))) return false;
    }
  }
  return adopt(terrain_bytes_, wide->bytes(), terrain_).ok();
}

Result<WorldMap> WorldMap::load(const MapLayerBytes& bytes) {
  WorldMap out;

  auto geometry = MapGeometry::parse(bytes.map_xml);
  if (!geometry) return geometry.error();
  out.geometry_ = std::move(geometry.value());

  if (!bytes.object_xml.empty()) {
    auto objects = MapObjectList::parse(bytes.object_xml);
    if (!objects) return objects.error();
    out.objects_ = std::move(objects.value());
  }

  if (const auto status = out.adopt(out.pass_bytes_, bytes.pass, out.pass_); !status) {
    return status.error();
  }
  if (const auto status = out.adopt(out.height_bytes_, bytes.height, out.height_); !status) {
    return status.error();
  }
  if (const auto status = out.adopt(out.light_bytes_, bytes.light, out.light_); !status) {
    return status.error();
  }
  if (const auto status = out.adopt(out.terrain_bytes_, bytes.terrain, out.terrain_); !status) {
    return status.error();
  }
  if (const auto status = out.adopt(out.decor_bytes_, bytes.decor, out.decor_); !status) {
    return status.error();
  }
  if (const auto status = out.adopt(out.trans_bytes_, bytes.trans, out.trans_); !status) {
    return status.error();
  }

  if (out.terrain_.cell_size() == 0) return FormatError::malformed;
  return out;
}

std::uint32_t WorldMap::light_at(std::int32_t world_x, std::int32_t world_y) const noexcept {
  if (light_.cell_size() == 0) return 16;
  if (world_x < 0 || world_y < 0) return 16;
  return light_.cell(static_cast<std::uint32_t>(world_x) / light_.cell_size(),
                     static_cast<std::uint32_t>(world_y) / light_.cell_size());
}

std::uint32_t WorldMap::height_at(std::int32_t world_x, std::int32_t world_y) const noexcept {
  if (height_.cell_size() == 0) return 0;
  if (world_x < 0 || world_y < 0) return 0;
  return height_.cell(static_cast<std::uint32_t>(world_x) / height_.cell_size(),
                      static_cast<std::uint32_t>(world_y) / height_.cell_size());
}

}  // namespace imperivm::core
