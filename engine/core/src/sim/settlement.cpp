#include "imperivm/core/sim/settlement.hpp"

#include <algorithm>
#include <string_view>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/game/class_graph.hpp"

namespace imperivm::core::sim {

namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void mix(std::uint64_t& h, std::uint64_t value) noexcept {
  for (int i = 0; i < 8; ++i) {
    h ^= static_cast<std::uint8_t>(value >> (i * 8));
    h *= kFnvPrime;
  }
}

}  // namespace

// --------------------------------------------------------------------------
// Warehouse
// --------------------------------------------------------------------------

std::int32_t Warehouse::store(Resource r, std::int32_t amount) noexcept {
  if (amount <= 0) return 0;
  std::int32_t& slot = (r == Resource::food) ? food : gold;
  const std::int32_t cap = capacity(r);
  const std::int32_t room = cap - slot;
  if (room <= 0) return 0;
  const std::int32_t accepted = std::min(amount, room);
  slot += accepted;
  return accepted;
}

std::int32_t Warehouse::take(Resource r, std::int32_t amount) noexcept {
  if (amount <= 0) return 0;
  std::int32_t& slot = (r == Resource::food) ? food : gold;
  const std::int32_t taken = std::min(amount, slot > 0 ? slot : 0);
  slot -= taken;
  return taken;
}

void Warehouse::set(Resource r, std::int32_t value) noexcept {
  std::int32_t& slot = (r == Resource::food) ? food : gold;
  slot = std::clamp(value, 0, capacity(r));
}

// --------------------------------------------------------------------------
// Holder
// --------------------------------------------------------------------------

bool Holder::contains(ObjectId id) const noexcept {
  return std::find(units.begin(), units.end(), id) != units.end();
}

bool Holder::add(ObjectId id) {
  if (id == kNoObject || full() || contains(id)) return false;
  units.push_back(id);
  return true;
}

bool Holder::force_add(ObjectId id) {
  if (id == kNoObject || contains(id)) return false;
  units.push_back(id);
  return true;
}

bool Holder::remove(ObjectId id) {
  const auto it = std::find(units.begin(), units.end(), id);
  if (it == units.end()) return false;
  units.erase(it);
  return true;
}

// --------------------------------------------------------------------------
// SettlementStore
// --------------------------------------------------------------------------

namespace {

/// A class property as a boolean, defaulting to false.
[[nodiscard]] bool class_flag(const ClassGraph& graph, ClassIndex index, std::string_view key) {
  const std::string_view value = graph.property(index, key);
  return !value.empty() && value != "0";
}

/// A class property as an integer, defaulting to `fallback`.
[[nodiscard]] std::int32_t class_int(const ClassGraph& graph, ClassIndex index,
                                     std::string_view key, std::int32_t fallback) {
  std::int32_t out = fallback;
  return parse_int(graph.property(index, key), out) ? out : fallback;
}

[[nodiscard]] SettlementKind kind_of(const ClassGraph& graph, ClassIndex index) {
  struct Rule {
    std::string_view base;
    SettlementKind kind;
  };
  // Order matters: a class can descend from more than one of these bases, and
  // the first match wins.
  static constexpr Rule kRules[] = {
      {"Outpost", SettlementKind::outpost},
      {"TTent", SettlementKind::teuton_tent},
      {"BaseShipyard", SettlementKind::shipyard},
      {"BaseTownhall", SettlementKind::stronghold},
      {"BaseVillage", SettlementKind::village},
  };
  for (const ClassIndex ancestor : graph.ancestry(index)) {
    for (const Rule& rule : kRules) {
      if (graph.at(ancestor).id == rule.base) return rule.kind;
    }
  }
  return SettlementKind::other;
}

}  // namespace

void fill_settlement_class_defaults(const ClassGraph& graph, ClassIndex index,
                                    SettlementInit& init) {
  if (index == kNoClass) return;
  init.kind = kind_of(graph, index);
  init.can_be_captured = class_flag(graph, index, "can_be_captured");
  init.can_be_attacked = class_flag(graph, index, "can_be_attacked");
  init.produces_gold = class_flag(graph, index, "produces_gold");
  init.produces_food = class_flag(graph, index, "produces_food");
  init.efficiency = class_int(graph, index, "efficiency", 0);
  init.food_per_pop = class_int(graph, index, "foodperpop", 0);
  init.capture_health_percent = class_int(graph, index, "capture_health_percent", 100);
  init.anchor_max_health = class_int(graph, index, "maxhealth", 0);
  // The two ceilings `BASETOWNHALL.SC.XML` declares as 10,000 and 100,
  // `BASEVILLAGE.SC.XML` as 20 and 0, `OUTPOST.SC.XML` as 0 and 10,000.
  init.max_units = class_int(graph, index, "max_units", 0);
  init.max_population = class_int(graph, index, "max_population", 0);
  init.population = class_int(graph, index, "population", 0);
  init.gold = class_int(graph, index, "settlement_gold", 0);
  init.food = class_int(graph, index, "settlement_food", 0);
  init.max_gold = class_int(graph, index, "settlement_maxgold", 0);
  init.max_food = class_int(graph, index, "settlement_maxfood", 0);
}

SettlementId SettlementStore::create(const SettlementInit& init) {
  Settlement s;
  s.id = static_cast<SettlementId>(settlements_.size());
  s.object = init.settlement_object;
  s.anchor = init.anchor;
  s.owner = init.owner;
  s.kind = init.kind;
  s.name = init.name;

  s.can_be_captured = init.can_be_captured;
  s.can_be_attacked = init.can_be_attacked;
  s.efficiency = init.efficiency;
  s.food_per_pop = init.food_per_pop;
  s.capture_health_percent = init.capture_health_percent;
  s.anchor_max_health = init.anchor_max_health;
  s.exit_interval = init.exit_interval;

  s.population = std::max(0, init.population);
  s.max_population = std::max(0, init.max_population);

  s.warehouse.object = init.warehouse_object;
  s.warehouse.max_gold = std::max(0, init.max_gold);
  s.warehouse.max_food = std::max(0, init.max_food);
  s.warehouse.set(Resource::gold, init.gold);
  s.warehouse.set(Resource::food, init.food);

  s.holder.object = init.holder_object;
  s.holder.max_units = std::max(0, init.max_units);

  if (init.anchor != kNoObject) {
    s.buildings.push_back(SettlementBuilding{init.anchor, init.anchor_max_health});
  }

  settlements_.push_back(std::move(s));
  return settlements_.back().id;
}

bool SettlementStore::replace(SettlementId id, const SettlementInit& init) {
  if (id >= settlements_.size()) return false;
  // Built by the one constructor there is, then moved into the slot, so that a
  // field added to `create` cannot be forgotten here.
  const SettlementId made = create(init);
  Settlement fresh = std::move(settlements_.back());
  settlements_.pop_back();
  (void)made;
  fresh.id = id;
  settlements_[id] = std::move(fresh);
  return true;
}

const Settlement* SettlementStore::find(SettlementId id) const noexcept {
  if (id >= settlements_.size()) return nullptr;
  return &settlements_[id];
}

Settlement* SettlementStore::find(SettlementId id) noexcept {
  if (id >= settlements_.size()) return nullptr;
  return &settlements_[id];
}

const Settlement* SettlementStore::for_object(ObjectId object) const noexcept {
  if (object == kNoObject) return nullptr;
  for (const Settlement& s : settlements_) {
    if (s.object == object || s.holder.object == object || s.warehouse.object == object) return &s;
    for (const SettlementBuilding& b : s.buildings) {
      if (b.object == object) return &s;
    }
  }
  return nullptr;
}

const Settlement* SettlementStore::find_by_name(std::string_view name) const noexcept {
  // An empty name is not a wildcard and not a match: 496 of the installation's
  // 684 settlements have none, and answering the first of them would be the
  // widening this project has been caught by before.
  if (name.empty()) return nullptr;
  for (const Settlement& s : settlements_) {
    if (s.name == name) return &s;
  }
  return nullptr;
}

Settlement* SettlementStore::for_object(ObjectId object) noexcept {
  const auto* found = const_cast<const SettlementStore*>(this)->for_object(object);
  if (found == nullptr) return nullptr;
  return &settlements_[found->id];
}

bool SettlementStore::add_building(SettlementId id, ObjectId building,
                                   std::int32_t max_health) {
  Settlement* s = find(id);
  if (s == nullptr || building == kNoObject) return false;
  for (const SettlementBuilding& b : s->buildings) {
    if (b.object == building) return false;
  }
  s->buildings.push_back(SettlementBuilding{building, max_health});
  return true;
}

std::uint64_t SettlementStore::hash() const noexcept {
  std::uint64_t h = kFnvOffset;
  for (const Settlement& s : settlements_) {
    mix(h, s.id);
    mix(h, s.object);
    mix(h, s.anchor);
    mix(h, static_cast<std::uint64_t>(s.owner));
    mix(h, static_cast<std::uint64_t>(s.kind));
    mix(h, static_cast<std::uint64_t>(s.can_be_captured ? 1 : 0));
    mix(h, static_cast<std::uint64_t>(s.can_be_attacked ? 1 : 0));
    mix(h, static_cast<std::uint32_t>(s.gold_rate));
    mix(h, static_cast<std::uint32_t>(s.food_rate));
    mix(h, static_cast<std::uint32_t>(s.population));
    mix(h, static_cast<std::uint32_t>(s.max_population));
    mix(h, static_cast<std::uint32_t>(s.warehouse.gold));
    mix(h, static_cast<std::uint32_t>(s.warehouse.food));
    mix(h, static_cast<std::uint32_t>(s.warehouse.max_gold));
    mix(h, static_cast<std::uint32_t>(s.warehouse.max_food));
    mix(h, static_cast<std::uint32_t>(s.holder.count()));
    for (ObjectId u : s.holder.units) mix(h, u);
    mix(h, static_cast<std::uint32_t>(s.buildings.size()));
    for (const SettlementBuilding& b : s.buildings) mix(h, b.object);
    mix(h, static_cast<std::uint32_t>(s.first_to_repair));
    mix(h, static_cast<std::uint64_t>(s.last_tower_fire_time));
    mix(h, static_cast<std::uint64_t>(s.last_unit_exit_time));
    mix(h, static_cast<std::uint64_t>(s.last_capture_query_time));
    mix(h, static_cast<std::uint32_t>(s.exit_interval));
    mix(h, static_cast<std::uint32_t>(s.loan));
    mix(h, static_cast<std::uint32_t>(s.loyalty));
    mix(h, static_cast<std::uint32_t>(s.loyalty_decreased));
    mix(h, static_cast<std::uint32_t>(s.efficiency));
    mix(h, static_cast<std::uint32_t>(s.sentries));
    mix(h, static_cast<std::uint32_t>(s.max_sentries));
    mix(h, static_cast<std::uint32_t>(s.sentries_ready));
    mix(h, s.supplied);
    mix(h, static_cast<std::uint64_t>(s.outpost_trade));
    mix(h, static_cast<std::uint64_t>(s.burning ? 1 : 0));
    for (std::int32_t t : s.timers) mix(h, static_cast<std::uint32_t>(t));
  }
  return h;
}

}  // namespace imperivm::core::sim
