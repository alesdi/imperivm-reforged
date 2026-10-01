#include "imperivm/core/sim/item.hpp"

#include <algorithm>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/xml.hpp"

namespace imperivm::core::sim {
namespace {

constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

void fold(std::uint64_t& h, std::uint64_t value) noexcept {
  for (int byte = 0; byte < 8; ++byte) {
    h ^= static_cast<std::uint64_t>((value >> (byte * 8)) & 0xFF);
    h *= kFnvPrime;
  }
}

char lower(char c) noexcept { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool iequal(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (lower(a[i]) != lower(b[i])) return false;
  }
  return true;
}

/// The `file://` prefix every script reference in `ITEMS.XML` carries.
std::string_view strip_scheme(std::string_view path) noexcept {
  constexpr std::string_view kScheme = "file://";
  if (path.size() > kScheme.size() && path.substr(0, kScheme.size()) == kScheme) {
    return path.substr(kScheme.size());
  }
  return path;
}

}  // namespace

// -- ItemBonus --------------------------------------------------------------

ItemBonus& ItemBonus::operator+=(const ItemBonus& other) noexcept {
  health += other.health;
  health_percent += other.health_percent;
  damage += other.damage;
  damage_percent += other.damage_percent;
  armor_slash += other.armor_slash;
  armor_slash_percent += other.armor_slash_percent;
  armor_pierce += other.armor_pierce;
  armor_pierce_percent += other.armor_pierce_percent;
  level += other.level;
  experience += other.experience;
  return *this;
}

bool ItemBonus::any() const noexcept { return !(*this == ItemBonus{}); }

// -- ItemCatalog ------------------------------------------------------------

Status ItemCatalog::load(std::span<const std::byte> document) {
  auto parsed = XmlDocument::parse(document);
  if (!parsed) return parsed.error();
  const XmlDocument& doc = *parsed;
  const NodeIndex root = doc.root();
  if (root == kNoNode || doc.node(root).name != "items") return FormatError::malformed;

  for (NodeIndex node = doc.child(root, "item"); node != kNoNode;
       node = doc.next(node, "item")) {
    ItemDefinition def;
    def.id = std::string(doc.attribute(node, "id"));
    if (def.id.empty()) continue;  // an item without identity is not an item
    def.name = std::string(doc.attribute(node, "name"));
    if (def.name.empty()) def.name = def.id;
    def.image = std::string(doc.attribute(node, "image"));
    def.description = std::string(doc.attribute(node, "description"));
    def.usecount = doc.attribute_int(node, "usecount", 0);
    def.level = doc.attribute_int(node, "level", 0);
    def.important = doc.attribute_bool(node, "important", false);
    def.cursed = doc.attribute_bool(node, "cursed", false);

    static constexpr std::string_view kHookAttr[] = {"use_script", "equip_script",
                                                     "object_script", "kill_script",
                                                     "attacheddie_script"};
    for (std::size_t i = 0; i < std::size(kHookAttr); ++i) {
      def.scripts[i] = std::string(strip_scheme(doc.attribute(node, kHookAttr[i])));
    }

    const NodeIndex bonus = doc.child(node, "bonus");
    if (bonus != kNoNode) {
      ItemBonus& b = def.bonus;
      b.health = doc.attribute_int(bonus, "health", 0);
      b.health_percent = doc.attribute_int(bonus, "health_percent", 0);
      b.damage = doc.attribute_int(bonus, "damage", 0);
      b.damage_percent = doc.attribute_int(bonus, "damage_percent", 0);
      b.armor_slash = doc.attribute_int(bonus, "armor_slash", 0);
      b.armor_slash_percent = doc.attribute_int(bonus, "armor_slash_percent", 0);
      b.armor_pierce = doc.attribute_int(bonus, "armor_pierce", 0);
      b.armor_pierce_percent = doc.attribute_int(bonus, "armor_pierce_percent", 0);
      b.level = doc.attribute_int(bonus, "level", 0);
      b.experience = doc.attribute_int(bonus, "experience", 0);
    }

    add(std::move(def));
  }
  return Status{};
}

ItemTypeIndex ItemCatalog::add(ItemDefinition definition) {
  defs_.push_back(std::move(definition));
  return static_cast<ItemTypeIndex>(defs_.size() - 1);
}

ItemTypeIndex ItemCatalog::find(std::string_view id) const {
  for (std::size_t i = 0; i < defs_.size(); ++i) {
    if (iequal(defs_[i].id, id)) return static_cast<ItemTypeIndex>(i);
  }
  return kNoItemType;
}

const ItemDefinition* ItemCatalog::at(ItemTypeIndex index) const {
  if (index >= defs_.size()) return nullptr;
  return &defs_[index];
}

// -- ItemStore --------------------------------------------------------------

std::size_t ItemStore::lower_bound(ObjectId item) const noexcept {
  const auto it = std::lower_bound(items_.begin(), items_.end(), item,
                                   [](const ItemInstance& a, ObjectId b) { return a.id < b; });
  return static_cast<std::size_t>(it - items_.begin());
}

const ItemInstance* ItemStore::find(ObjectId item) const {
  const std::size_t at = lower_bound(item);
  if (at >= items_.size() || items_[at].id != item) return nullptr;
  return &items_[at];
}

ItemInstance* ItemStore::find(ObjectId item) {
  return const_cast<ItemInstance*>(static_cast<const ItemStore*>(this)->find(item));
}

std::int32_t ItemStore::capacity_of(const World& world, ObjectId owner,
                                    std::int32_t fallback) const {
  const WorldObject* slot = world.find(owner);
  if (slot == nullptr) return 0;
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot->class_index == kNoClass) return fallback;
  const std::string_view value = graph->property(slot->class_index, "inventory_size");
  if (value.empty()) return fallback;
  std::int32_t out = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return fallback;
    out = out * 10 + (c - '0');
  }
  return out;
}

ObjectId ItemStore::insert(World& world, ItemTypeIndex type, ObjectId owner) {
  const ObjectId id = world.spawn_internal(InternalKind::item);
  if (id == kNoObject) return kNoObject;

  ItemInstance instance;
  instance.id = id;
  instance.type = type;
  instance.owner = owner;
  if (const ItemDefinition* def = catalog_ != nullptr ? catalog_->at(type) : nullptr) {
    instance.usecount = def->usecount;
  }
  // Ids ascend, so a new item always belongs at the end.
  items_.push_back(instance);
  if (owner != kNoObject) world.put_in_holder(id, owner);
  return id;
}

ObjectId ItemStore::add(World& world, ObjectId owner, std::string_view id) {
  if (catalog_ == nullptr) return kNoObject;
  const ItemTypeIndex type = catalog_->find(id);
  if (type == kNoItemType) return kNoObject;
  return add(world, owner, type);
}

ObjectId ItemStore::add(World& world, ObjectId owner, ItemTypeIndex type) {
  if (catalog_ == nullptr || catalog_->at(type) == nullptr) return kNoObject;
  if (owner != kNoObject) {
    if (world.find(owner) == nullptr) return kNoObject;
    const std::int32_t capacity = capacity_of(world, owner);
    if (count_for(owner) >= capacity) return kNoObject;
  }
  return insert(world, type, owner);
}

bool ItemStore::give(World& world, ObjectId item, ObjectId to) {
  ItemInstance* instance = find(item);
  if (instance == nullptr || to == kNoObject) return false;
  if (instance->owner == to) return true;
  if (world.find(to) == nullptr) return false;
  if (count_for(to) >= capacity_of(world, to)) return false;
  instance->owner = to;
  world.put_in_holder(item, to);
  return true;
}

bool ItemStore::drop(World& world, ObjectId item, Point at) {
  ItemInstance* instance = find(item);
  if (instance == nullptr) return false;
  instance->owner = kNoObject;
  world.remove_from_holder(item, at);
  return true;
}

bool ItemStore::remove(World& world, ObjectId item) {
  const std::size_t at = lower_bound(item);
  if (at >= items_.size() || items_[at].id != item) return false;
  items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(at));
  world.despawn(item);
  return true;
}

bool ItemStore::remove_at(World& world, ObjectId owner, std::size_t index) {
  std::vector<ObjectId> held;
  contents_for(owner, held);
  if (index == 0 || index > held.size()) return false;  // 1-based, see index_of
  return remove(world, held[index - 1]);
}

std::int32_t ItemStore::remove_all_of_type(World& world, ObjectId owner, std::string_view id) {
  if (catalog_ == nullptr) return 0;
  const ItemTypeIndex type = catalog_->find(id);
  if (type == kNoItemType) return 0;

  std::vector<ObjectId> doomed;
  for (const ItemInstance& instance : items_) {
    if (instance.owner == owner && instance.type == type) doomed.push_back(instance.id);
  }
  for (const ObjectId victim : doomed) remove(world, victim);
  return static_cast<std::int32_t>(doomed.size());
}

bool ItemStore::exchange(ObjectId item, std::string_view new_id) {
  if (catalog_ == nullptr) return false;
  ItemInstance* instance = find(item);
  if (instance == nullptr) return false;
  const ItemTypeIndex type = catalog_->find(new_id);
  if (type == kNoItemType) return false;
  instance->type = type;
  instance->usecount = catalog_->at(type)->usecount;
  return true;
}

bool ItemStore::use(World& world, ObjectId item, std::int32_t charges) {
  ItemInstance* instance = find(item);
  if (instance == nullptr) return false;
  const ItemDefinition* def = catalog_ != nullptr ? catalog_->at(instance->type) : nullptr;
  const bool consumable = def != nullptr && def->usecount > 0;

  instance->usecount -= charges;
  if (instance->usecount < 0) instance->usecount = 0;
  if (consumable && instance->usecount == 0) return remove(world, item);
  return true;
}

bool ItemStore::spend(World& world, ObjectId item, std::int32_t charges) {
  ItemInstance* instance = find(item);
  if (instance == nullptr) return false;
  // Written before the test, and not clamped: `0x00534a90` stores the
  // difference and only then compares it. A `jg` skips the destruction, so
  // "at or below zero" is the destroying case and zero is inside it.
  instance->usecount -= charges;
  if (instance->usecount > 0) return true;
  return remove(world, item);
}

std::int32_t ItemStore::use_count(ObjectId item) const {
  const ItemInstance* instance = find(item);
  return instance != nullptr ? instance->usecount : 0;
}

void ItemStore::set_use_count(ObjectId item, std::int32_t value) {
  if (ItemInstance* instance = find(item)) instance->usecount = value < 0 ? 0 : value;
}

ObjectId ItemStore::find_of_type(ObjectId owner, std::string_view id) const {
  if (catalog_ == nullptr) return kNoObject;
  const ItemTypeIndex type = catalog_->find(id);
  if (type == kNoItemType) return kNoObject;
  for (const ItemInstance& instance : items_) {
    if (instance.owner == owner && instance.type == type) return instance.id;
  }
  return kNoObject;
}

bool ItemStore::has_type(ObjectId owner, std::string_view id) const {
  return find_of_type(owner, id) != kNoObject;
}

std::int32_t ItemStore::count_for(ObjectId owner) const {
  std::int32_t n = 0;
  for (const ItemInstance& instance : items_) {
    if (instance.owner == owner) ++n;
  }
  return n;
}

std::size_t ItemStore::index_of(ObjectId owner, ObjectId item) const {
  std::size_t slot = 0;
  for (const ItemInstance& instance : items_) {
    if (instance.owner != owner) continue;
    ++slot;
    if (instance.id == item) return slot;
  }
  return 0;
}

std::size_t ItemStore::contents_for(ObjectId owner, std::vector<ObjectId>& out) const {
  out.clear();
  for (const ItemInstance& instance : items_) {
    if (instance.owner == owner) out.push_back(instance.id);
  }
  return out.size();
}

ItemBonus ItemStore::total_bonus(ObjectId owner) const {
  ItemBonus total;
  if (catalog_ == nullptr) return total;
  for (const ItemInstance& instance : items_) {
    if (instance.owner != owner) continue;
    if (const ItemDefinition* def = catalog_->at(instance.type)) total += def->bonus;
  }
  return total;
}

void ItemStore::drop_owner(World& world, ObjectId owner) {
  std::vector<ObjectId> held;
  contents_for(owner, held);
  for (const ObjectId item : held) remove(world, item);
}

void ItemStore::hash(std::uint64_t& accumulator) const noexcept {
  for (const ItemInstance& instance : items_) {
    fold(accumulator, instance.id);
    fold(accumulator, instance.type);
    fold(accumulator, instance.owner);
    fold(accumulator, static_cast<std::uint64_t>(static_cast<std::uint32_t>(instance.usecount)));
    fold(accumulator,
         static_cast<std::uint64_t>(static_cast<std::uint32_t>(instance.customdata)));
  }
}

}  // namespace imperivm::core::sim
