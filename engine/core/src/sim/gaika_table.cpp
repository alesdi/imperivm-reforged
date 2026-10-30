#include "imperivm/core/sim/gaika_table.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/entrance.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/path.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

/// The claim grid's "staked by no seed" mark.
constexpr std::uint16_t kOpen = 0xffff;

/// One seed (`docs/formats/gaika.md`, "How the table is built").
struct Seed {
  std::int16_t x = 0;
  std::int16_t y = 0;
  GaikaRect rect{};
  bool central = false;
  ObjectId settlement = kNoObject;
  /// The settlement's central building, for a settlement's seeds.
  ObjectId anchor = kNoObject;
  std::int32_t weight = 0;
  LsaId label = kNoLsa;
};

/// A slot coordinate pair as the original carries it, two 16-bit numbers.
struct Slot {
  std::int16_t x = 0;
  std::int16_t y = 0;
};

constexpr Slot kNoSlot{-1, -1};

/// A world coordinate to its slot: shifted down seven bits as an unsigned
/// 32-bit number and cut to 16, which is the conversion every step of the
/// build makes. -1 comes out as -1.
[[nodiscard]] std::int16_t slot_of(std::int32_t world) noexcept {
  return static_cast<std::int16_t>(static_cast<std::uint16_t>(static_cast<std::uint32_t>(world) >> 7));
}

[[nodiscard]] bool descends(const ClassGraph* graph, ClassIndex index, std::string_view name) {
  if (graph == nullptr || index == kNoClass) return false;
  for (const ClassIndex ancestor : graph->ancestry(index)) {
    if (graph->at(ancestor).id == name) return true;
  }
  return false;
}

/// Grow `into` by `by` (0x00449c80): an empty `by` changes nothing, an empty
/// `into` becomes `by`, and otherwise each edge moves out to cover it.
void widen(GaikaRect& into, const GaikaRect& by) noexcept {
  if (by.left > by.right || by.top > by.bottom) return;
  if (into.left > into.right || into.top > into.bottom) {
    into = by;
    return;
  }
  into.left = std::min(into.left, by.left);
  into.top = std::min(into.top, by.top);
  into.right = std::max(into.right, by.right);
  into.bottom = std::max(into.bottom, by.bottom);
}

class Partition {
 public:
  Partition(const World& world, const LsaPartition& areas, const ObstructionGrid* passability)
      : world_(world), areas_(areas), passability_(passability),
        w_(areas.slot_columns()), h_(areas.slot_rows()),
        claim_(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), 0),
        node_(claim_.size(), 0) {}

  void run(const SettlementStore& settlements);

  std::vector<GaikaNode> records;  ///< record 0 included
  [[nodiscard]] std::vector<std::uint16_t>& grid() noexcept { return node_; }

 private:
  [[nodiscard]] bool inside(std::int32_t x, std::int32_t y) const noexcept {
    return x >= 0 && y >= 0 && x < w_ && y < h_;
  }
  [[nodiscard]] std::size_t index(std::int32_t x, std::int32_t y) const noexcept {
    return static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) + static_cast<std::size_t>(x);
  }
  /// The index the original forms in 16 bits; in range of every grid that
  /// has at most 65,536 slots, and wrapped on a larger one as it wraps there.
  [[nodiscard]] std::size_t index16(std::int32_t x, std::int32_t y) const noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(w_) * static_cast<std::uint32_t>(y) +
                                      static_cast<std::uint32_t>(x));
  }
  [[nodiscard]] LsaId label(std::int32_t x, std::int32_t y) const noexcept {
    return areas_.slot_label(x, y);
  }
  [[nodiscard]] GaikaRect around(Slot at, std::int32_t radius) const noexcept {
    GaikaRect r;
    r.left = static_cast<std::int16_t>(std::max<std::int32_t>(at.x - radius, 0));
    r.top = static_cast<std::int16_t>(std::max<std::int32_t>(at.y - radius, 0));
    r.right = static_cast<std::int16_t>(std::min<std::int32_t>(at.x + radius, w_ - 1));
    r.bottom = static_cast<std::int16_t>(std::min<std::int32_t>(at.y + radius, h_ - 1));
    return r;
  }

  /// A breadth-first flood from `start`: `pass` is asked of each slot taken
  /// off the queue, and a slot that passes queues its four neighbours left,
  /// right, up, down. The queue is unbounded; `pass` is what stops it.
  template <typename Pass>
  void flood(Slot start, Pass&& pass) {
    std::deque<Slot> queue;
    queue.push_back(start);
    while (!queue.empty()) {
      const Slot at = queue.front();
      queue.pop_front();
      if (!pass(at)) continue;
      queue.push_back(Slot{static_cast<std::int16_t>(at.x - 1), at.y});
      queue.push_back(Slot{static_cast<std::int16_t>(at.x + 1), at.y});
      queue.push_back(Slot{at.x, static_cast<std::int16_t>(at.y - 1)});
      queue.push_back(Slot{at.x, static_cast<std::int16_t>(at.y + 1)});
    }
  }
  [[nodiscard]] static bool in_rect(const GaikaRect& r, Slot at) noexcept {
    return at.x >= r.left && at.x <= r.right && at.y >= r.top && at.y <= r.bottom;
  }

  [[nodiscard]] Slot snap(Slot from) const;
  [[nodiscard]] Slot entrance_slot(ObjectId building, bool shipyard) const;
  [[nodiscard]] Slot second_entrance(ObjectId building, Slot first) const;
  void stake(std::uint16_t number);
  void place_settlement_seeds(const SettlementStore& settlements);
  void place_region_seeds();
  void claim();
  [[nodiscard]] std::int32_t score(const Seed& seed, Slot at) const noexcept;
  void settlement_nodes();
  void region_nodes();
  void smooth();
  bool four_way(Slot at);
  bool eight_way(Slot at);
  void merge_small();
  void centres();
  [[nodiscard]] bool wholly_free(std::int32_t x, std::int32_t y) const noexcept;

  const World& world_;
  const LsaPartition& areas_;
  const ObstructionGrid* passability_;
  std::int32_t w_;
  std::int32_t h_;
  std::vector<std::uint16_t> claim_;
  std::vector<std::uint16_t> node_;
  std::vector<Seed> seeds_;

  // The neighbour tallies, kept between calls as the original keeps them on
  // its stack: only the counts are cleared, and an unused entry's id and area
  // are whatever the last tally left there.
  std::array<std::uint16_t, 8> tally_ids_{};
  std::array<std::uint16_t, 8> tally_votes_{};
  std::array<LsaId, 8> tally_areas_{};
};

/// 0x0044ab60: the best ground slot within two of `from`.
Slot Partition::snap(Slot from) const {
  static constexpr std::int16_t kOffsets[21][2] = {
      {0, 0},  {-1, 0}, {1, 0},  {0, -1},  {0, 1},  {-1, 1},  {1, -1},
      {-1, -1}, {1, 1}, {-2, 0}, {2, 0},   {0, -2}, {0, 2},   {1, 2},
      {-1, 2}, {1, -2}, {-1, -2}, {2, 1},  {2, -1}, {-2, 1},  {-2, -1}};
  std::uint32_t best = 0;
  Slot found = kNoSlot;
  for (const auto& step : kOffsets) {
    const auto x = static_cast<std::int16_t>(from.x + step[0]);
    const auto y = static_cast<std::int16_t>(from.y + step[1]);
    if (!inside(x, y)) continue;
    const LsaId id = label(x, y);
    const LsaArea* area = areas_.find(id);
    if (area == nullptr || area->water) continue;
    const auto coverage = static_cast<std::uint32_t>(area->coverage);
    if (coverage <= best) continue;
    best = coverage;
    found = Slot{x, y};
  }
  return found;
}

/// 0x0044af00 before its snap: the slot of the building's land entrance
/// nearest `(-1, -1)`, or of the building itself when it has none -- except
/// that a shipyard takes the entrance's slot even then, which is `(-1, -1)`.
Slot Partition::entrance_slot(ObjectId building, bool shipyard) const {
  const Point position = world_.resolve_position(building);
  const Slot own{slot_of(position.x), slot_of(position.y)};
  std::vector<Point> doors;
  building_doors(world_, passability_, building, kEnterPointType, /*water=*/false, doors);
  // 0x0056fba0: the floored square root of the squared distance, strictly
  // less, from an unsigned "none yet".
  Point door{-1, -1};
  std::uint64_t nearest = 0xffffffffu;
  for (const Point& d : doors) {
    const std::int64_t dx = static_cast<std::int64_t>(-1) - d.x;
    const std::int64_t dy = static_cast<std::int64_t>(-1) - d.y;
    const auto distance = static_cast<std::uint64_t>(isqrt(dx * dx + dy * dy));
    if (distance < nearest) {
      nearest = distance;
      door = d;
    }
  }
  if (!shipyard && door.x == -1) return own;
  return Slot{slot_of(static_cast<std::int16_t>(door.x)), slot_of(static_cast<std::int16_t>(door.y))};
}

/// 0x0044afc0 before its snap: the first land entrance, in list order, whose
/// slot is not `first`.
Slot Partition::second_entrance(ObjectId building, Slot first) const {
  std::vector<Point> doors;
  building_doors(world_, passability_, building, kEnterPointType, /*water=*/false, doors);
  for (const Point& d : doors) {
    const auto x = static_cast<std::int32_t>(static_cast<std::uint32_t>(d.x) >> 7);
    const auto y = static_cast<std::int32_t>(static_cast<std::uint32_t>(d.y) >> 7);
    if (x == first.x && y == first.y) continue;
    return Slot{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
  }
  // Every entrance in the first slot. An outpost with no land entrance at all
  // compares the original's uninitialised locals here; that case is answered
  // the same way, with no second seed.
  return kNoSlot;
}

/// 0x0044cf30: the newest seed writes its number over every slot of its own
/// area inside its rectangle that it can reach from its own slot.
void Partition::stake(std::uint16_t number) {
  const Seed& seed = seeds_[number];
  flood(Slot{seed.x, seed.y}, [&](Slot at) {
    if (!in_rect(seed.rect, at)) return false;
    if (label(at.x, at.y) != seed.label) return false;
    std::uint16_t& cell = claim_[index(at.x, at.y)];
    if (cell == number) return false;
    cell = number;
    return true;
  });
}

/// Step 2, 0x0044d080.
void Partition::place_settlement_seeds(const SettlementStore& settlements) {
  const ClassGraph* graph = world_.class_graph();
  const auto class_of = [&](ObjectId id) {
    const WorldObject* slot = world_.find(id);
    return slot == nullptr ? kNoClass : slot->class_index;
  };
  for (const Settlement& s : settlements.all()) {
    const ClassIndex central = class_of(s.anchor);
    if (!descends(graph, central, "BaseTownhall") && !descends(graph, central, "BaseVillage") &&
        !descends(graph, central, "Outpost") && !descends(graph, central, "BaseShipyard") &&
        !descends(graph, central, "TTent")) {
      continue;
    }
    for (const SettlementBuilding& building : s.buildings) {
      const ClassIndex cls = class_of(building.object);
      const Slot first =
          snap(entrance_slot(building.object, descends(graph, cls, "BaseShipyard")));
      if (first.x == -1) continue;
      Seed seed;
      seed.x = first.x;
      seed.y = first.y;
      seed.rect = around(first, 10);
      seed.central = building.object == s.anchor;
      seed.settlement = s.object;
      seed.anchor = s.anchor;
      seed.weight = seed.central || descends(graph, cls, "Tower") ? 3 : 2;
      seed.label = label(first.x, first.y);
      seeds_.push_back(seed);
      stake(static_cast<std::uint16_t>(seeds_.size() - 1));

      if (!seed.central || !descends(graph, cls, "Outpost")) continue;
      const Slot raw = second_entrance(building.object, first);
      if (raw.x == -1 && raw.y == -1) continue;
      const Slot second = snap(raw);
      if (second.x == -1) continue;
      Seed gate = seed;
      gate.x = second.x;
      gate.y = second.y;
      gate.rect = around(second, 10);
      gate.central = false;
      seeds_.push_back(gate);
      stake(static_cast<std::uint16_t>(seeds_.size() - 1));
    }
  }
}

/// Step 3, 0x0044d4c0.
void Partition::place_region_seeds() {
  for (std::int32_t y = 0; y < h_; ++y) {
    for (std::int32_t x = 0; x < w_; ++x) {
      if (claim_[index16(x, y)] != kOpen) continue;
      Seed seed;
      seed.x = static_cast<std::int16_t>(x);
      seed.y = static_cast<std::int16_t>(y);
      seed.rect = around(Slot{seed.x, seed.y}, 15);
      seed.weight = 1;
      seed.label = label(x, y);
      seeds_.push_back(seed);
      stake(static_cast<std::uint16_t>(seeds_.size() - 1));
    }
  }
}

/// 0x004497d0.
std::int32_t Partition::score(const Seed& seed, Slot at) const noexcept {
  const auto near_x = static_cast<std::int16_t>(15 - std::abs(at.x - seed.x));
  const auto near_y = static_cast<std::int16_t>(15 - std::abs(at.y - seed.y));
  return (near_x * near_x + near_y * near_y) * seed.weight;
}

/// Step 4, 0x0044d670.
void Partition::claim() {
  const std::size_t total = claim_.size();
  for (std::size_t i = 0; i < total; ++i) {
    std::uint16_t& cell = claim_[static_cast<std::uint16_t>(i)];
    if (cell != 0) cell = kOpen;
  }
  std::vector<std::uint8_t> seen;
  for (std::size_t n = 1; n < seeds_.size(); ++n) {
    const Seed& seed = seeds_[n];
    const std::int32_t width = seed.rect.right - seed.rect.left + 1;
    const std::int32_t height = seed.rect.bottom - seed.rect.top + 1;
    seen.assign(static_cast<std::size_t>(std::max(width, 0)) * static_cast<std::size_t>(std::max(height, 0)), 0);
    const auto number = static_cast<std::uint16_t>(n);
    flood(Slot{seed.x, seed.y}, [&](Slot at) {
      if (!in_rect(seed.rect, at)) return false;
      if (label(at.x, at.y) != seed.label) return false;
      std::uint8_t& mark = seen[static_cast<std::size_t>(at.y - seed.rect.top) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(at.x - seed.rect.left)];
      if (mark != 0) return false;
      mark = 1;
      std::uint16_t& cell = claim_[index(at.x, at.y)];
      if (cell == kOpen) {
        cell = number;
      } else if (static_cast<std::uint32_t>(score(seeds_[cell], at)) <=
                 static_cast<std::uint32_t>(score(seed, at))) {
        cell = number;
      }
      return true;
    });
  }
}

/// Step 5, 0x0044d760.
void Partition::settlement_nodes() {
  for (std::size_t n = 1; n < seeds_.size(); ++n) {
    const Seed seed = seeds_[n];
    if (!seed.central) continue;
    const auto id = static_cast<std::uint16_t>(records.size());
    GaikaRect box{seed.x, seed.y, seed.x, seed.y};
    std::int32_t count = 0;
    const GaikaRect whole{0, 0, static_cast<std::int16_t>(w_ - 1), static_cast<std::int16_t>(h_ - 1)};
    flood(Slot{seed.x, seed.y}, [&](Slot at) {
      if (!in_rect(whole, at)) return false;
      std::uint16_t& cell = claim_[index(at.x, at.y)];
      if (cell == 0 || cell == kOpen) return false;
      if (seeds_[cell].settlement != seed.settlement) return false;
      if (label(at.x, at.y) != seed.label) return false;
      widen(box, GaikaRect{at.x, at.y, at.x, at.y});
      node_[index(at.x, at.y)] = id;
      cell = kOpen;
      ++count;
      return true;
    });
    GaikaNode record;
    record.rect = box;
    record.center = Point{seed.x * kLsaSlotSize + kLsaSlotSize / 2, seed.y * kLsaSlotSize + kLsaSlotSize / 2};
    record.lsa = seed.label;
    record.settlement = seed.settlement;
    record.coverage = count;
    // The slot under the central building itself is the node's too, whoever
    // won it -- before the next settlement's flood, which may write it again.
    // The original does not test the index; a building stands on the map, so
    // it is always in range there, and is skipped here when not.
    const Point at = world_.resolve_position(seed.anchor);
    const auto bx = static_cast<std::uint16_t>(static_cast<std::uint32_t>(at.x) >> 7);
    const auto by = static_cast<std::uint16_t>(static_cast<std::uint32_t>(at.y) >> 7);
    const std::size_t under = static_cast<std::size_t>(by) * static_cast<std::size_t>(w_) + bx;
    if (under < node_.size()) node_[under] = id;
    records.push_back(record);
  }
}

/// Step 6, 0x0044d940.
void Partition::region_nodes() {
  const GaikaRect whole{0, 0, static_cast<std::int16_t>(w_ - 1), static_cast<std::int16_t>(h_ - 1)};
  for (std::size_t n = 1; n < seeds_.size(); ++n) {
    const auto number = static_cast<std::uint16_t>(n);
    const Seed& seed = seeds_[n];
    for (std::int32_t y = seed.rect.top; y <= seed.rect.bottom; ++y) {
      for (std::int32_t x = seed.rect.left; x <= seed.rect.right; ++x) {
        if (claim_[index16(x, y)] != number) continue;
        const auto id = static_cast<std::uint16_t>(records.size());
        GaikaRect box{-1, -1, -1, -1};
        flood(Slot{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)}, [&](Slot at) {
          if (!in_rect(whole, at)) return false;
          std::uint16_t& cell = claim_[index(at.x, at.y)];
          if (cell != number) return false;
          cell = kOpen;
          node_[index(at.x, at.y)] = id;
          if (box.left == -1) {
            box = GaikaRect{at.x, at.y, at.x, at.y};
          } else {
            widen(box, GaikaRect{at.x, at.y, at.x, at.y});
          }
          return true;
        });
        GaikaNode record;
        record.rect = box;
        record.center = Point{x * kLsaSlotSize + kLsaSlotSize / 2, y * kLsaSlotSize + kLsaSlotSize / 2};
        record.lsa = seed.label;
        records.push_back(record);
      }
    }
  }
}

/// 0x004499d0 then 0x0044a010: the four-neighbour vote at `at`.
bool Partition::four_way(Slot at) {
  static constexpr std::int16_t kFour[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
  tally_votes_[0] = tally_votes_[1] = tally_votes_[2] = tally_votes_[3] = 0;
  for (const auto& step : kFour) {
    const auto x = static_cast<std::int16_t>(at.x + step[0]);
    const auto y = static_cast<std::int16_t>(at.y + step[1]);
    if (!inside(x, y)) continue;
    const std::uint16_t value = node_[index(x, y)];
    for (std::size_t i = 0; i < 4; ++i) {
      if (tally_votes_[i] == 0) {
        tally_votes_[i] = 1;
        tally_ids_[i] = value;
        tally_areas_[i] = label(x, y);
        break;
      }
      if (tally_ids_[i] == value) {
        ++tally_votes_[i];
        break;
      }
    }
  }
  const auto exchange = [&](std::size_t a, std::size_t b) {
    std::swap(tally_ids_[a], tally_ids_[b]);
    std::swap(tally_votes_[a], tally_votes_[b]);
    std::swap(tally_areas_[a], tally_areas_[b]);
  };
  if (tally_votes_[0] < tally_votes_[1]) exchange(0, 1);
  if (tally_votes_[0] < tally_votes_[2]) exchange(0, 2);
  const std::uint16_t leader = tally_ids_[0];
  if (leader == 0) return false;
  if (label(at.x, at.y) != tally_areas_[0]) return false;
  std::uint16_t& cell = node_[index(at.x, at.y)];
  if (tally_votes_[0] == 3) {
    if (cell == leader) return false;
    cell = leader;
    widen(records[leader].rect, GaikaRect{at.x, at.y, at.x, at.y});
    return true;
  }
  if (tally_votes_[0] == 2) {
    if (cell == leader) return false;
    if (tally_votes_[1] == 2 && cell == tally_ids_[1]) return false;
    cell = leader;
    // The rectangle the original builds here is (column, row, row, row).
    widen(records[leader].rect, GaikaRect{at.x, at.y, at.y, at.y});
    return true;
  }
  return false;
}

/// 0x00449af0 then 0x0044a190: the eight-neighbour vote at `at`.
bool Partition::eight_way(Slot at) {
  static constexpr std::int16_t kEight[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 1},
                                                {0, 1},   {1, 1},  {-1, 0}, {1, 0}};
  tally_votes_.fill(0);
  for (const auto& step : kEight) {
    const auto x = static_cast<std::int16_t>(at.x + step[0]);
    const auto y = static_cast<std::int16_t>(at.y + step[1]);
    if (!inside(x, y)) continue;
    const std::uint16_t value = node_[index(x, y)];
    for (std::size_t i = 0; i < 8; ++i) {
      if (tally_votes_[i] == 0) {
        tally_votes_[i] = 1;
        tally_ids_[i] = value;
        tally_areas_[i] = label(x, y);
        break;
      }
      if (tally_ids_[i] == value) {
        ++tally_votes_[i];
        break;
      }
    }
  }
  for (std::size_t i = 0; i + 1 < 8; ++i) {
    for (std::size_t j = i + 1; j < 8; ++j) {
      if (tally_votes_[j] > tally_votes_[i]) {
        std::swap(tally_ids_[i], tally_ids_[j]);
        std::swap(tally_votes_[i], tally_votes_[j]);
        std::swap(tally_areas_[i], tally_areas_[j]);
      }
    }
  }
  const std::uint16_t leader = tally_ids_[0];
  if (leader == 0) return false;
  if (label(at.x, at.y) != tally_areas_[0]) return false;
  if (tally_votes_[0] <= tally_votes_[1]) return false;
  std::uint16_t& cell = node_[index(at.x, at.y)];
  if (cell == leader) return false;
  cell = leader;
  widen(records[leader].rect, GaikaRect{at.x, at.y, at.x, at.y});
  return true;
}

/// Step 7, 0x0044c5d0.
void Partition::smooth() {
  std::deque<Slot> queue;
  const auto changes = [&](Slot at) {
    if (four_way(at)) return true;
    return tally_votes_[3] == 1 && eight_way(at);
  };
  for (std::int32_t y = 1; y < h_ - 1; ++y) {
    for (std::int32_t x = 1; x < w_ - 1; ++x) {
      if (node_[index(x, y)] == 0) continue;
      const Slot at{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y)};
      if (changes(at)) queue.push_back(at);
    }
  }
  static constexpr std::int16_t kFour[4][2] = {{0, 1}, {0, -1}, {1, 0}, {-1, 0}};
  while (!queue.empty()) {
    const Slot at = queue.front();
    queue.pop_front();
    for (const auto& step : kFour) {
      const Slot next{static_cast<std::int16_t>(at.x + step[0]), static_cast<std::int16_t>(at.y + step[1])};
      if (next.x <= 0 || at.y <= 0 || at.x >= w_ - 1 || at.y >= h_ - 1) continue;
      if (node_[index(next.x, next.y)] == 0) continue;
      if (changes(next)) {
        queue.push_back(next);
        break;
      }
    }
  }
}

/// Step 8, 0x0044ddd0 with 0x0044db20.
void Partition::merge_small() {
  static constexpr std::int16_t kAround[8][2] = {{-1, -1}, {1, 1},  {-1, 0}, {1, 0},
                                                 {-1, 1},  {1, -1}, {0, -1}, {0, 1}};
  std::vector<std::pair<std::uint16_t, std::int32_t>> votes;
  for (std::size_t n = 1; n < records.size(); ++n) {
    const GaikaNode& small = records[n];
    const auto rows = static_cast<std::int16_t>(small.rect.bottom - small.rect.top + 1);
    const auto columns = static_cast<std::int16_t>(small.rect.right - small.rect.left + 1);
    if (static_cast<std::int32_t>(rows) * static_cast<std::int32_t>(columns) >= 30) continue;
    if (small.settlement != kNoObject) continue;
    const auto id = static_cast<std::uint16_t>(n);

    votes.clear();
    for (std::int32_t y = small.rect.top; y <= small.rect.bottom; ++y) {
      for (std::int32_t x = small.rect.left; x <= small.rect.right; ++x) {
        if (node_[index16(x, y)] != id) continue;
        for (const auto& step : kAround) {
          const auto nx = static_cast<std::int16_t>(x + step[0]);
          const auto ny = static_cast<std::int16_t>(y + step[1]);
          if (!inside(nx, ny)) continue;
          const std::uint16_t other = node_[index(nx, ny)];
          if (other == 0 || other == id) continue;
          auto found = std::find_if(votes.begin(), votes.end(),
                                    [&](const auto& v) { return v.first == other; });
          if (found != votes.end()) {
            ++found->second;
          } else if (records[other].lsa == small.lsa) {
            votes.emplace_back(other, 1);
          }
        }
      }
    }
    if (votes.empty()) continue;
    std::uint16_t host = 0xffff;
    std::int32_t most = 0;
    for (const auto& [candidate, count] : votes) {
      if (count > most) {
        most = count;
        host = candidate;
      }
    }

    const auto rename = [&](std::uint16_t from, std::uint16_t to, const GaikaRect& within) {
      for (std::int32_t y = within.top; y <= within.bottom; ++y) {
        for (std::int32_t x = within.left; x <= within.right; ++x) {
          std::uint16_t& cell = node_[index(x, y)];
          if (cell == from) cell = to;
        }
      }
    };
    rename(id, host, small.rect);
    widen(records[host].rect, small.rect);
    const std::size_t last = records.size() - 1;
    if (n < last) {
      rename(static_cast<std::uint16_t>(last), id, records[last].rect);
      records[n] = records[last];
    }
    records.pop_back();
    n = 1;  // the walk starts again at node 2
  }
}

/// 0x00449d30: every passability cell of the slot is free.
bool Partition::wholly_free(std::int32_t x, std::int32_t y) const noexcept {
  if (!inside(x, y)) return false;
  if (passability_ == nullptr) return true;
  const std::int32_t per = kLsaSlotSize / kCollisionCellSize;
  for (std::int32_t cy = 0; cy < per; ++cy) {
    for (std::int32_t cx = 0; cx < per; ++cx) {
      if (passability_->blocked_cell(x * per + cx, y * per + cy)) return false;
    }
  }
  return true;
}

/// Step 10, 0x0044aaf0 -> 0x0044a8a0.
void Partition::centres() {
  for (std::size_t n = 1; n < records.size(); ++n) {
    GaikaNode& record = records[n];
    if (record.settlement != kNoObject) continue;
    const auto id = static_cast<std::uint16_t>(n);
    const std::int32_t cx = (record.rect.left + record.rect.right) / 2;
    const std::int32_t cy = (record.rect.top + record.rect.bottom) / 2;
    if (inside(cx, cy) && node_[index(cx, cy)] == id && wholly_free(cx, cy)) {
      record.center = Point{cx * kLsaSlotSize, cy * kLsaSlotSize};
      continue;
    }
    std::int32_t best = 9999999;
    for (std::int32_t dx = -5; dx <= 5; ++dx) {
      for (std::int32_t dy = -5; dy <= 5; ++dy) {
        const std::int32_t d2 = dx * dx + dy * dy;
        if (d2 >= best) continue;
        const std::int32_t x = cx + dx;
        const std::int32_t y = cy + dy;
        if (!inside(x, y) || node_[index(x, y)] != id || !wholly_free(x, y)) continue;
        best = d2;
        // The column of the slot found, and the row of the rectangle's
        // middle: the search does not keep the row it found.
        record.center = Point{x * kLsaSlotSize + kLsaSlotSize / 2, cy * kLsaSlotSize + kLsaSlotSize / 2};
      }
    }
    if (best < 9999999) continue;
    bool placed = false;
    for (std::int32_t y = record.rect.top; y <= record.rect.bottom && !placed; ++y) {
      for (std::int32_t x = record.rect.left; x <= record.rect.right; ++x) {
        if (node_[index16(x, y)] != id) continue;
        bool block = true;
        for (std::int32_t by = -1; by <= 1 && block; ++by) {
          for (std::int32_t bx = -1; bx <= 1; ++bx) {
            const std::int32_t sx = x + bx;
            const std::int32_t sy = y + by;
            if (!inside(sx, sy)) continue;
            if (node_[index(sx, sy)] != id || !wholly_free(sx, sy)) {
              block = false;
              break;
            }
          }
        }
        if (block) {
          record.center = Point{x * kLsaSlotSize, y * kLsaSlotSize};
          placed = true;
          break;
        }
      }
    }
  }
}

void Partition::run(const SettlementStore& settlements) {
  // Step 1: the claim grid open wherever an area claimed the slot.
  for (std::int32_t y = 0; y < h_; ++y) {
    for (std::int32_t x = 0; x < w_; ++x) {
      claim_[index(x, y)] = label(x, y) == kNoLsa ? 0 : kOpen;
    }
  }
  // Record 0 and seed 0, the placeholders (0x0044cff0).
  GaikaNode none;
  none.center = Point{-1, -1};
  none.rect = GaikaRect{0, 0, -1, -1};
  records.push_back(none);
  seeds_.push_back(Seed{});

  place_settlement_seeds(settlements);
  place_region_seeds();
  claim();

  settlement_nodes();
  region_nodes();
  smooth();
  merge_small();
  // Step 9, coverage and neighbours, is the table's own `link`; step 10 reads
  // nothing it writes.
  centres();
}

}  // namespace

void GaikaTable::build(const World& world, const LsaPartition& areas,
                       const SettlementStore& settlements) {
  clear();
  built_ = true;
  const std::int32_t columns = areas.slot_columns();
  const std::int32_t rows = areas.slot_rows();
  if (columns <= 0 || rows <= 0) return;
  MovementSystem* movement = movement_system(const_cast<World&>(world));
  Partition cut(world, areas, movement == nullptr ? nullptr : &movement->grid());
  cut.run(settlements);
  columns_ = columns;
  rows_ = rows;
  slots_ = std::move(cut.grid());
  nodes_.assign(cut.records.begin() + 1, cut.records.end());
  link();
}

bool GaikaTable::restore(std::vector<GaikaNode> nodes, std::int32_t columns, std::int32_t rows,
                         std::vector<std::uint16_t> slots) {
  clear();
  if (columns < 0 || rows < 0 ||
      slots.size() != static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows)) {
    return false;
  }
  for (const std::uint16_t id : slots) {
    if (id > nodes.size()) return false;
  }
  nodes_ = std::move(nodes);
  slots_ = std::move(slots);
  columns_ = columns;
  rows_ = rows;
  link();
  built_ = true;
  return true;
}

/// Step 9, 0x0044a3c0.
void GaikaTable::link() {
  const std::size_t count = nodes_.size() + 1;  // with record 0
  neighbour_starts_.assign(nodes_.size(), 0);
  neighbours_.clear();
  for (GaikaNode& node : nodes_) node.coverage = 0;
  if (nodes_.empty()) return;
  static constexpr std::int32_t kAround[8][2] = {{-1, 1}, {-1, 0}, {-1, -1}, {0, -1},
                                                 {1, -1}, {1, 0},  {1, 1},   {0, 1}};
  std::vector<std::uint8_t> paired(count * count, 0);
  for (std::int32_t y = 0; y < rows_; ++y) {
    for (std::int32_t x = 0; x < columns_; ++x) {
      const std::uint16_t here = slots_[static_cast<std::size_t>(y) * static_cast<std::size_t>(columns_) +
                                        static_cast<std::size_t>(x)];
      if (here == 0) continue;
      ++nodes_[here - 1u].coverage;
      for (const auto& step : kAround) {
        const std::int32_t nx = x + step[0];
        const std::int32_t ny = y + step[1];
        if (nx < 0 || ny < 0 || nx >= columns_ || ny >= rows_) continue;
        const std::uint16_t there = slots_[static_cast<std::size_t>(ny) * static_cast<std::size_t>(columns_) +
                                           static_cast<std::size_t>(nx)];
        if (there == 0 || there == here) continue;
        paired[static_cast<std::size_t>(here) * count + there] = 1;
        paired[static_cast<std::size_t>(there) * count + here] = 1;
      }
    }
  }
  for (std::size_t a = 1; a < count; ++a) {
    for (std::size_t b = 1; b < count; ++b) {
      if (paired[a * count + b] != 0) neighbours_.push_back(static_cast<GaikaId>(b));
    }
    neighbour_starts_[a - 1] = static_cast<std::uint32_t>(neighbours_.size());
  }
}

std::span<const GaikaId> GaikaTable::neighbours(GaikaId id) const noexcept {
  if (id < 1 || static_cast<std::size_t>(id) > nodes_.size()) return {};
  const std::size_t at_index = static_cast<std::size_t>(id) - 1;
  const std::uint32_t from = at_index == 0 ? 0u : neighbour_starts_[at_index - 1];
  const std::uint32_t to = neighbour_starts_[at_index];
  return std::span<const GaikaId>(neighbours_).subspan(from, to - from);
}

GaikaId GaikaTable::for_settlement(ObjectId settlement) const noexcept {
  if (settlement == kNoObject) return kNoGaika;
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    if (nodes_[i].settlement == settlement) return static_cast<GaikaId>(i + 1);
  }
  return kNoGaika;
}

GaikaId GaikaTable::at(const LsaPartition& /*areas*/, Point where) const noexcept {
  return at(where);
}

GaikaId GaikaTable::at(Point where) const noexcept {
  if (where.x == -1 || where.x < 0 || where.y < 0) return kNoGaika;
  const auto x = static_cast<std::uint16_t>(static_cast<std::uint32_t>(where.x) >> 7);
  const auto y = static_cast<std::uint16_t>(static_cast<std::uint32_t>(where.y) >> 7);
  const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(columns_) + x;
  return i < slots_.size() ? static_cast<GaikaId>(slots_[i]) : kNoGaika;
}

}  // namespace imperivm::core::sim
