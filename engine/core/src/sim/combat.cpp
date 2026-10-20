// Combat. The model, its evidence, and what is invented: sim/combat.hpp.
//
// No floating point, no clock, no allocation in the inner loop that is not a
// growing vector, and no unordered container anywhere: iteration order is world
// state (docs/engine/architecture.md).

#include "imperivm/core/sim/combat.hpp"

#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/hooks.hpp"

#include <algorithm>
#include <utility>

#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/siege_fire.hpp"
#include "imperivm/core/sim/stats.hpp"
#include "imperivm/core/sim/world.hpp"
// `unpack_point`: `BestTargetInRange/2` takes a `point`, and the object model
// owns how one is packed into a `Value`.
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/xml.hpp"

namespace imperivm::core::sim {
namespace {

// -- small integral helpers -------------------------------------------------

/// Percent scaling, truncating. Both operands are non-negative in every call
/// site, so this is floor division and identical on every target.
[[nodiscard]] std::int32_t scale_percent(std::int32_t value, std::int32_t percent) noexcept {
  if (percent == 100) return value;
  const std::int64_t wide = static_cast<std::int64_t>(value) * percent / 100;
  if (wide > 0x7FFFFFFF) return 0x7FFFFFFF;
  if (wide < -0x7FFFFFFF) return -0x7FFFFFFF;
  return static_cast<std::int32_t>(wide);
}

[[nodiscard]] std::int64_t distance_squared(Point a, Point b) noexcept {
  const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
  const std::int64_t dy = static_cast<std::int64_t>(a.y) - b.y;
  return dx * dx + dy * dy;
}

[[nodiscard]] std::int64_t squared(std::int64_t v) noexcept { return v * v; }

// The integer square root this file used to carry privately is gone: it is
// `sim/world_host.hpp`'s `isqrt`, which says in as many words that "there is
// exactly one right answer only if everybody uses the same one". Two
// implementations of a determinism-critical primitive in one binary is the
// thing that warning is about, even when -- as here -- both were exact floors
// and agreed on every input.

// -- text scanning ----------------------------------------------------------
//
// The core cannot open a file, so both readers take the bytes. Neither uses
// <cctype>: locale-dependent classification in a deterministic simulation is a
// trap, and the shipped files are plain ASCII.

[[nodiscard]] bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && is_space(text[begin])) ++begin;
  while (end > begin && is_space(text[end - 1])) --end;
  return text.substr(begin, end - begin);
}

[[nodiscard]] bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

/// Signed decimal, stopping at the first character that is not one. Returns
/// `fallback` when nothing was consumed, which is how a malformed value keeps
/// its compiled-in default rather than becoming zero.
[[nodiscard]] std::int32_t to_int(std::string_view text, std::int32_t fallback = 0) noexcept {
  text = trim(text);
  std::size_t i = 0;
  bool negative = false;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
    negative = text[i] == '-';
    ++i;
  }
  std::int64_t value = 0;
  std::size_t digits = 0;
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i, ++digits) {
    value = value * 10 + (text[i] - '0');
    if (value > 0x7FFFFFFF) value = 0x7FFFFFFF;
  }
  if (digits == 0) return fallback;
  return static_cast<std::int32_t>(negative ? -value : value);
}

[[nodiscard]] std::string_view as_text(std::span<const std::byte> bytes) noexcept {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

/// One line of an INI file, comment stripped. `;` starts a comment in
/// `CONST.INI` and appears inline (`LevelExpDivider = 8 ; exp_gain = ...`).
[[nodiscard]] std::string_view strip_comment(std::string_view line) noexcept {
  const std::size_t semi = line.find(';');
  return semi == std::string_view::npos ? line : line.substr(0, semi);
}

}  // namespace

// ---------------------------------------------------------------------------
// damage types
// ---------------------------------------------------------------------------

DamageType damage_type_from_name(std::string_view name) noexcept {
  if (equal_ignoring_case(name, "slash")) return DamageType::slash;
  if (equal_ignoring_case(name, "pierce")) return DamageType::pierce;
  if (equal_ignoring_case(name, "siege")) return DamageType::siege;
  if (equal_ignoring_case(name, "none")) return DamageType::none;
  // `Unit`'s own declaration is `slash`, so an unknown spelling falls back to
  // the class-graph default rather than to "cannot hurt anything".
  return DamageType::slash;
}

std::string_view damage_type_name(DamageType type) noexcept {
  switch (type) {
    case DamageType::none: return "none";
    case DamageType::slash: return "slash";
    case DamageType::pierce: return "pierce";
    case DamageType::siege: return "siege";
  }
  return "slash";
}

// ---------------------------------------------------------------------------
// LevelDifferenceTable
// ---------------------------------------------------------------------------

LevelDifferenceTable LevelDifferenceTable::shipped() noexcept {
  // DATA\CONST.INI [GamePlay]:
  //   LevelDifferenceDamageTable = 1,5 5,20 10,40 20,60 40,80 100,90
  LevelDifferenceTable table;
  table.points[0] = {1, 5};
  table.points[1] = {5, 20};
  table.points[2] = {10, 40};
  table.points[3] = {20, 60};
  table.points[4] = {40, 80};
  table.points[5] = {100, 90};
  table.count = 6;
  return table;
}

LevelDifferenceTable LevelDifferenceTable::parse(std::string_view text) noexcept {
  LevelDifferenceTable table;
  std::size_t i = 0;
  while (i < text.size() && table.count < kMaxBreakpoints) {
    while (i < text.size() && (is_space(text[i]) || text[i] == ',')) ++i;
    const std::size_t begin = i;
    while (i < text.size() && !is_space(text[i])) ++i;
    const std::string_view pair = text.substr(begin, i - begin);
    const std::size_t comma = pair.find(',');
    if (comma == std::string_view::npos) continue;
    const std::int32_t difference = to_int(pair.substr(0, comma), -1);
    const std::int32_t percent = to_int(pair.substr(comma + 1), -1);
    if (difference < 0 || percent < 0) continue;
    table.points[table.count++] = {difference, percent};
  }
  if (table.count == 0) return shipped();
  return table;
}

std::int32_t LevelDifferenceTable::percent(std::int32_t difference) const noexcept {
  if (difference < 0) difference = -difference;
  if (count == 0) return 0;
  if (difference < points[0].difference) return 0;
  for (std::uint8_t i = 1; i < count; ++i) {
    if (difference >= points[i].difference) continue;
    // Linear interpolation between the bracketing breakpoints. INFERRED: the
    // data gives six points and no rule; a step function fits them too.
    const Breakpoint& lo = points[i - 1];
    const Breakpoint& hi = points[i];
    const std::int32_t span = hi.difference - lo.difference;
    if (span <= 0) return lo.percent;
    const std::int64_t rise = static_cast<std::int64_t>(hi.percent - lo.percent);
    return lo.percent + static_cast<std::int32_t>(rise * (difference - lo.difference) / span);
  }
  return points[count - 1].percent;
}

// ---------------------------------------------------------------------------
// CombatConstants
// ---------------------------------------------------------------------------

CombatConstants CombatConstants::from_const_ini(std::span<const std::byte> bytes) noexcept {
  CombatConstants out;
  const std::string_view text = as_text(bytes);
  bool in_gameplay = false;

  std::size_t pos = 0;
  while (pos <= text.size()) {
    std::size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view raw = trim(text.substr(pos, end - pos));
    pos = end + 1;
    if (raw.empty()) {
      if (end == text.size()) break;
      continue;
    }

    if (raw.front() == '[') {
      const std::size_t close = raw.find(']');
      in_gameplay = close != std::string_view::npos &&
                    equal_ignoring_case(raw.substr(1, close - 1), "GamePlay");
      if (end == text.size()) break;
      continue;
    }
    if (!in_gameplay) {
      if (end == text.size()) break;
      continue;
    }

    const std::string_view line = strip_comment(raw);
    const std::size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      if (end == text.size()) break;
      continue;
    }
    const std::string_view key = trim(line.substr(0, eq));
    const std::string_view value = trim(line.substr(eq + 1));

    if (equal_ignoring_case(key, "MinDamage")) {
      out.min_damage = to_int(value, out.min_damage);
    } else if (equal_ignoring_case(key, "MinPercentOfAttackersDamage")) {
      out.min_percent_of_attackers_damage = to_int(value, out.min_percent_of_attackers_damage);
    } else if (equal_ignoring_case(key, "DamageAmplify")) {
      out.damage_amplify = to_int(value, out.damage_amplify);
    } else if (equal_ignoring_case(key, "ChargeDamageFactor")) {
      out.charge_damage_factor = to_int(value, out.charge_damage_factor);
    } else if (equal_ignoring_case(key, "LevelDifferenceDamageTable")) {
      out.level_difference = LevelDifferenceTable::parse(value);
    } else if (equal_ignoring_case(key, "EasyDifficultyLevelAddend")) {
      out.easy_level_addend = to_int(value, out.easy_level_addend);
    } else if (equal_ignoring_case(key, "NormalDifficultyLevelAddend")) {
      out.normal_level_addend = to_int(value, out.normal_level_addend);
    } else if (equal_ignoring_case(key, "HardDifficultyLevelAddend")) {
      out.hard_level_addend = to_int(value, out.hard_level_addend);
    } else if (equal_ignoring_case(key, "LevelExpDivider")) {
      out.level_exp_divider = to_int(value, out.level_exp_divider);
    } else if (equal_ignoring_case(key, "ExpA")) {
      out.exp_a = to_int(value, out.exp_a);
    } else if (equal_ignoring_case(key, "ExpB")) {
      out.exp_b = to_int(value, out.exp_b);
    } else if (equal_ignoring_case(key, "ExpT")) {
      out.exp_t = to_int(value, out.exp_t);
    } else if (equal_ignoring_case(key, "BattleTacticsExpModifier")) {
      out.experience_modifier = to_int(value, out.experience_modifier);
    } else if (equal_ignoring_case(key, "CatapultBaseFireRate")) {
      out.catapult_base_fire_rate = to_int(value, out.catapult_base_fire_rate);
    } else if (equal_ignoring_case(key, "CatapultAddFireRate")) {
      out.catapult_add_fire_rate = to_int(value, out.catapult_add_fire_rate);
    } else if (equal_ignoring_case(key, "BaseFireRate")) {
      out.base_fire_rate = to_int(value, out.base_fire_rate);
    } else if (equal_ignoring_case(key, "CatapultMaxUnits")) {
      out.catapult_max_units = to_int(value, out.catapult_max_units);
    } else if (equal_ignoring_case(key, "SquadLookIvl")) {
      out.acquire_interval = to_int(value, out.acquire_interval);
    }

    if (end == text.size()) break;
  }

  if (out.level_exp_divider <= 0) out.level_exp_divider = 1;
  if (out.acquire_interval <= 0) out.acquire_interval = 1;
  return out;
}

std::int32_t CombatConstants::difficulty_addend(Difficulty level) const noexcept {
  switch (level) {
    case Difficulty::easy: return easy_level_addend;
    case Difficulty::normal: return normal_level_addend;
    case Difficulty::hard: return hard_level_addend;
  }
  return 0;
}

std::int32_t CombatConstants::experience_for_level(std::int32_t level) const noexcept {
  // Exp[level] = Exp[level-1] + Pos(level - T) * ExpA + ExpB, Exp[1] = 0.
  if (level <= 1) return 0;
  std::int64_t total = 0;
  for (std::int32_t l = 2; l <= level; ++l) {
    const std::int64_t positive = l - exp_t > 0 ? l - exp_t : 0;
    total += positive * exp_a + exp_b;
    if (total > 0x7FFFFFFF) return 0x7FFFFFFF;
  }
  return static_cast<std::int32_t>(total);
}

std::int32_t CombatConstants::level_for_experience(std::int32_t experience) const noexcept {
  if (experience <= 0) return 1;
  std::int32_t level = 1;
  // The dumps cap level at 24; 64 is slack and bounds the loop on hostile input.
  while (level < 64 && experience >= experience_for_level(level + 1)) ++level;
  return level;
}

std::int32_t CombatConstants::experience_gain(std::int32_t target_level) const noexcept {
  return experience_gain(target_level, experience_modifier);
}

std::int32_t CombatConstants::experience_gain(std::int32_t target_level,
                                              std::int32_t percent) const noexcept {
  if (target_level < 0) target_level = 0;
  const std::int32_t base = target_level / level_exp_divider + 1;
  return scale_percent(base, percent);
}

// ---------------------------------------------------------------------------
// CounterTable
// ---------------------------------------------------------------------------

void CounterTable::clear() noexcept {
  entries_.clear();
  unresolved_ = 0;
}

void CounterTable::add(ClassIndex counter, ClassIndex countered, std::int32_t coeff) {
  if (counter == kNoClass || countered == kNoClass) {
    ++unresolved_;
    return;
  }
  // Inserted in place rather than appended and sorted afterwards, so a table
  // built entry by entry answers immediately and there is no state in which a
  // lookup silently returns nothing.
  const Entry entry{counter, countered, coeff};
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), entry,
                                   [](const Entry& a, const Entry& b) {
                                     if (a.counter != b.counter) return a.counter < b.counter;
                                     return a.countered < b.countered;
                                   });
  if (it != entries_.end() && it->counter == counter && it->countered == countered) {
    it->coeff = coeff;
    return;
  }
  entries_.insert(it, entry);
}

Result<void> CounterTable::load(std::span<const std::byte> document, const ClassGraph& graph) {
  auto parsed = XmlDocument::parse(document);
  if (!parsed) return parsed.error();
  const XmlDocument& doc = parsed.value();
  if (doc.empty()) return FormatError::malformed;

  const NodeIndex root = doc.root();
  if (doc.node(root).name != "counterunits") return FormatError::bad_magic;

  clear();
  for (NodeIndex unit = doc.child(root, "unit"); unit != kNoNode;
       unit = doc.next(unit, "unit")) {
    // `<unit class="A">` is the class being countered; each `<counter
    // class="B" coeff="c"/>` names a class that counters it. See the
    // orientation argument on CounterMode in the header.
    const ClassIndex countered = graph.lookup(doc.attribute(unit, "class"));
    for (NodeIndex counter = doc.child(unit, "counter"); counter != kNoNode;
         counter = doc.next(counter, "counter")) {
      const ClassIndex who = graph.lookup(doc.attribute(counter, "class"));
      add(who, countered, doc.attribute_int(counter, "coeff", 0));
    }
  }
  return Result<void>{};
}

std::vector<ClassIndex> CounterTable::countered_classes() const {
  std::vector<ClassIndex> out;
  for (const Entry& entry : entries_) {
    if (entry.countered == kNoClass) continue;
    if (std::find(out.begin(), out.end(), entry.countered) == out.end()) {
      out.push_back(entry.countered);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::int32_t CounterTable::strength(ClassIndex counter, ClassIndex countered) const noexcept {
  if (entries_.empty() || counter == kNoClass || countered == kNoClass) return 0;
  const Entry needle{counter, countered, 0};
  const auto it = std::lower_bound(entries_.begin(), entries_.end(), needle,
                                   [](const Entry& a, const Entry& b) {
                                     if (a.counter != b.counter) return a.counter < b.counter;
                                     return a.countered < b.countered;
                                   });
  if (it == entries_.end() || it->counter != counter || it->countered != countered) return 0;
  return it->coeff;
}

std::int32_t CounterTable::damage_percent(ClassIndex attacker, ClassIndex defender,
                                          CounterMode mode) const noexcept {
  switch (mode) {
    case CounterMode::off:
      return 100;
    case CounterMode::counter_bonus: {
      // The attacker is listed as a counter of the defender: it hits harder.
      const std::int32_t s = strength(attacker, defender);
      return 100 + s;
    }
    case CounterMode::countered_penalty: {
      // The defender is listed as a counter of the attacker: the attacker's
      // damage is scaled down to `coeff` percent of normal.
      const std::int32_t s = strength(defender, attacker);
      return s == 0 ? 100 : s;
    }
  }
  return 100;
}

// ---------------------------------------------------------------------------
// resolve_damage
// ---------------------------------------------------------------------------

DamageBreakdown resolve_damage(const DamageInputs& in,
                               const CombatConstants& constants) noexcept {
  DamageBreakdown out;
  out.attack = in.attack < 0 ? 0 : in.attack;

  // A class whose damage_type is `none` -- peasants, wagons, wildlife -- never
  // damages anything, whatever its `damage` says.
  if (in.type == DamageType::none || out.attack == 0) {
    out.armour = 0;
    return out;
  }

  // Step 2: armour. `siege` meets none: no armor_siege property exists.
  const bool armoured = !in.ignores_armour && in.type != DamageType::siege;
  out.armour = armoured ? (in.armour < 0 ? 0 : in.armour) : 0;
  std::int32_t value = out.attack - out.armour;

  // Step 3: MinPercentOfAttackersDamage. Armour subtraction is the only step
  // that can drive damage to zero, and this is the constant named for it.
  const std::int32_t floor_by_percent =
      scale_percent(out.attack, constants.min_percent_of_attackers_damage);
  if (value < floor_by_percent) {
    value = floor_by_percent;
    out.floored_by_min_percent = true;
  }
  out.after_armour = value;

  // Step 4: the counter-unit table. INFERRED -- see CounterMode.
  value = scale_percent(value, in.counter_percent);
  out.after_counter = value;

  // Step 5: the level modifier, in the order Expertise's description gives.
  out.level_difference = in.attacker_level - in.defender_level;
  out.level_percent = constants.level_difference.percent(out.level_difference);
  if (out.level_difference > 0) {
    value = scale_percent(value, 100 + out.level_percent);
  } else if (out.level_difference < 0) {
    value = scale_percent(value, 100 - out.level_percent);
  }
  out.after_level = value;

  // Step 6: flat multipliers, downstream of armour as Charge's text requires.
  value = scale_percent(value, in.multiplier_percent);

  // Step 7: DamageAmplify, "Damage in settlements".
  if (in.in_settlement) value = scale_percent(value, constants.damage_amplify);

  // Step 7b: the sheltered victim. `[victim+0x194]` bit 28 -- raised by a
  // cover of mercy on everyone under it, and by nothing else -- has one
  // reader in `.text`, 0x005110c6: after every special-ability branch of
  // the formula and before the cap that is the only step left, the running
  // damage is halved (`cdq; sub eax, edx; sar eax, 1`, toward zero). Its
  // place here is the same one: the last modifier, ahead of the floor.
  if (in.halved) {
    value /= 2;
    out.halved = true;
  }

  // Step 8: MinDamage.
  if (value < constants.min_damage) {
    value = constants.min_damage;
    out.floored_by_min_damage = true;
  }
  out.final_damage = value;
  return out;
}

// ---------------------------------------------------------------------------
// CombatProfile
// ---------------------------------------------------------------------------

std::int32_t CombatProfile::armour_against(DamageType type) const noexcept {
  switch (type) {
    case DamageType::slash: return armor_slash;
    case DamageType::pierce: return armor_pierce;
    // No armor_siege exists in the 845-class graph, and `none` never lands.
    case DamageType::siege:
    case DamageType::none: return 0;
  }
  return 0;
}

CombatProfile CombatProfile::from_class(const ClassGraph& graph, ClassIndex index,
                                        const Entity* entity) {
  CombatProfile out;
  if (index == kNoClass || index >= graph.size()) return out;
  out.class_index = index;

  const auto number = [&](std::string_view key, std::int32_t fallback) {
    const std::string_view value = graph.property(index, key);
    return value.empty() ? fallback : to_int(value, fallback);
  };

  out.damage = number("damage", 0);
  out.damage_type = damage_type_from_name(graph.property(index, "damage_type"));
  out.armor_slash = number("armor_slash", 0);
  out.armor_pierce = number("armor_pierce", 0);
  out.max_health = number("maxhealth", 0);
  out.max_stamina = number("maxstamina", 0);
  out.range = number("range", 0);
  out.min_range = number("min_range", 0);
  out.radius = number("radius", 0);
  out.selection_radius = number("selection_radius", 0);
  out.sight = number("sight", 0);
  out.target_factor = number("target_factor", 100);
  out.target_priority = number("target_priority", 100);
  out.splash_radius = number("splash_radius", 0);
  out.shot_height = number("shot_height", 0);
  out.can_be_attacked = number("can_be_attacked", 1) != 0;

  const std::string_view projectile = graph.property(index, "projectile_class");
  if (!projectile.empty()) out.projectile = graph.lookup(projectile);

  // Cadence: the entity's attack animation if there is one, then the class's
  // own `attack_delay` (only Catapult and FakeTower declare it), then the
  // invented default.
  std::int32_t interval = 0;
  if (entity != nullptr) {
    if (const EntityAnim* attack = entity->anim(kAnimAttack); attack != nullptr) {
      interval = attack->measured_duration();
    }
  }
  if (interval <= 0) interval = number("attack_delay", 0);
  if (interval <= 0) interval = kDefaultAttackInterval;
  out.attack_interval = interval;

  out.shoots = !projectile.empty() && projectile != "**Invalid**";
  // One walk up the ancestry for every name `Unit::CanAttack` tests, so the
  // predicate never parses a class name per candidate.
  for (const ClassIndex step : graph.ancestry(index)) {
    const std::string_view id = graph.at(step).id;
    if (id == "RamUnit") out.lineage |= kLineRamUnit;
    else if (id == "Sentry") out.lineage |= kLineSentry;
    else if (id == "FakeTower") out.lineage |= kLineFakeTower;
    else if (id == "Gate") out.lineage |= kLineGate;
    else if (id == "Catapult") out.lineage |= kLineCatapult;
    else if (id == "Outpost") out.lineage |= kLineOutpost;
    else if (id == "BaseShipyard") out.lineage |= kLineBaseShipyard;
    else if (id == "Wolf" || id == "Bear" || id == "Boar" || id == "LionM" || id == "LionF") {
      out.lineage |= kLineBeast;
    }
  }
  if (graph.at(index).id == "Fish") out.lineage |= kLineFish;

  return out;
}

// ---------------------------------------------------------------------------
// CombatSystem: configuration and population
// ---------------------------------------------------------------------------

void CombatSystem::set_player_level_addend(PlayerId player, std::int32_t addend) noexcept {
  const auto it = std::lower_bound(
      level_addends_.begin(), level_addends_.end(), player,
      [](const std::pair<PlayerId, std::int32_t>& e, PlayerId p) { return e.first < p; });
  if (it != level_addends_.end() && it->first == player) {
    it->second = addend;
    return;
  }
  level_addends_.insert(it, {player, addend});
}

std::int32_t CombatSystem::player_level_addend(PlayerId player) const noexcept {
  const auto it = std::lower_bound(
      level_addends_.begin(), level_addends_.end(), player,
      [](const std::pair<PlayerId, std::int32_t>& e, PlayerId p) { return e.first < p; });
  if (it != level_addends_.end() && it->first == player) return it->second;
  return 0;
}

void CombatSystem::set_player_experience_modifier(PlayerId player,
                                                  std::int32_t percent) noexcept {
  const auto it = std::lower_bound(
      experience_modifiers_.begin(), experience_modifiers_.end(), player,
      [](const std::pair<PlayerId, std::int32_t>& e, PlayerId p) { return e.first < p; });
  if (it != experience_modifiers_.end() && it->first == player) {
    it->second = percent;
    return;
  }
  experience_modifiers_.insert(it, {player, percent});
}

std::int32_t CombatSystem::player_experience_modifier(PlayerId player) const noexcept {
  const auto it = std::lower_bound(
      experience_modifiers_.begin(), experience_modifiers_.end(), player,
      [](const std::pair<PlayerId, std::int32_t>& e, PlayerId p) { return e.first < p; });
  if (it != experience_modifiers_.end() && it->first == player) return it->second;
  // The player record's constructor (0x00565a00) zeroes `+0x84`.
  return 0;
}

void CombatSystem::set_profile(ClassIndex index, const CombatProfile& profile) {
  const auto it = std::lower_bound(
      profiles_.begin(), profiles_.end(), index,
      [](const CombatProfile& p, ClassIndex i) { return p.class_index < i; });
  CombatProfile copy = profile;
  copy.class_index = index;
  if (it != profiles_.end() && it->class_index == index) {
    *it = copy;
    return;
  }
  profiles_.insert(it, copy);
}

CombatProfile CombatSystem::profile(ClassIndex index) const {
  static const CombatProfile empty{};
  const auto it = std::lower_bound(
      profiles_.begin(), profiles_.end(), index,
      [](const CombatProfile& p, ClassIndex i) { return p.class_index < i; });
  if (it != profiles_.end() && it->class_index == index) return *it;
  if (graph_ == nullptr || index == kNoClass) return empty;
  const auto inserted = profiles_.insert(it, CombatProfile::from_class(*graph_, index));
  return *inserted;
}

Combatant& CombatSystem::add(const Combatant& combatant) {
  // Built from its class, as an object is: the stored stats start as the
  // class's numbers (and the record's, should the caller have given one).
  Combatant built = combatant;
  built.stats = class_stats(built);
  const auto it = std::lower_bound(
      units_.begin(), units_.end(), built.id,
      [](const Combatant& c, ObjectId id) { return c.id < id; });
  if (it != units_.end() && it->id == built.id) {
    *it = built;
    return *it;
  }
  return *units_.insert(it, built);
}

Combatant* CombatSystem::mutable_find(ObjectId id) noexcept {
  const auto it = std::lower_bound(units_.begin(), units_.end(), id,
                                   [](const Combatant& c, ObjectId i) { return c.id < i; });
  if (it != units_.end() && it->id == id) return &*it;
  return nullptr;
}

Combatant* CombatSystem::find(ObjectId id) noexcept { return mutable_find(id); }

const Combatant* CombatSystem::find(ObjectId id) const noexcept {
  const auto it = std::lower_bound(units_.begin(), units_.end(), id,
                                   [](const Combatant& c, ObjectId i) { return c.id < i; });
  if (it != units_.end() && it->id == id) return &*it;
  return nullptr;
}

bool CombatSystem::remove(ObjectId id) {
  const auto it = std::lower_bound(units_.begin(), units_.end(), id,
                                   [](const Combatant& c, ObjectId i) { return c.id < i; });
  if (it == units_.end() || it->id != id) return false;
  units_.erase(it);
  // Anything aiming at it forgets it: a stale handle reads as dead rather than
  // as somebody else, which is the whole point of never reusing ids.
  for (Combatant& unit : units_) {
    if (unit.target == id) {
      unit.target = kNoObject;
      unit.attacks = 0;
    }
  }
  return true;
}

void CombatSystem::set_position(ObjectId id, Point position) noexcept {
  if (Combatant* unit = mutable_find(id); unit != nullptr) unit->position = position;
}

void CombatSystem::set_allied(PlayerId a, PlayerId b, bool allied) {
  if (a == b) return;
  const std::pair<PlayerId, PlayerId> key = a < b ? std::pair{a, b} : std::pair{b, a};
  const auto it = std::lower_bound(allies_.begin(), allies_.end(), key);
  const bool present = it != allies_.end() && *it == key;
  if (allied && !present) allies_.insert(it, key);
  if (!allied && present) allies_.erase(it);
}

bool is_combat_object(const WorldObject& slot) noexcept {
  if (slot.internal != InternalKind::none) return false;
  if (slot.class_index == kNoClass) return false;
  // A spawn template is not in play, so it does not fight and cannot be
  // fought. Reconcile walks `World::objects()` directly rather than through
  // `World::collect`, so it has to make this test itself -- which is also how
  // `gbr.exe` does it, once per sweep rather than once centrally.
  if (slot.state.flags.unspawned) return false;
  return slot.state.flags.is_unit || slot.state.flags.is_building;
}

bool CombatSystem::reclass(ObjectId id, ClassIndex to) {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr) return false;
  unit->class_index = to;
  unit->target = kNoObject;
  unit->action = Action::idle;
  unit->anim = kAnimIdle;
  unit->attacks = 0;
  unit->last_attack_time = 0;
  unit->next_action_time = 0;
  // The constructor's zeros (0x005d3170); what the mutation copies across
  // was not read to include them, so the fresh object starts unpicked.
  unit->danger = 0;
  unit->danger_time = 0;
  unit->damage_multiplier_percent = 100;
  unit->ignores_armour = false;
  unit->bonus = StatBonus{};
  // The fresh object is built from its class and rebuilt (0x005deef0): its
  // stats start as the new class's, with no rescale, and the rebuild then
  // adds what the unit's hero, items and formation give it.
  unit->stats = class_stats(*unit);
  recalc(id);
  return true;
}

std::size_t CombatSystem::reconcile(World& world) {
  // Without world ids there is nothing to reconcile against, and reconciling
  // anyway would delete every combatant a synthetic test registered.
  if (!world_bound_) return units_.size();
  // The rebuild of what enrols below reads the rest of the world through it.
  world_ = &world;

  const std::span<const WorldObject> objects = world.objects();

  // A linear merge of two sequences that are both sorted ascending by id.
  // Rebuilt into a fresh vector rather than patched in place, because an insert
  // in the middle of `units_` is O(n) and doing that per new object on a map
  // that spawns a hundred at once is O(n^2) for no reason. The result is in
  // ascending id order either way, which is what `add`'s contract promises and
  // what makes iteration order reproducible.
  std::vector<Combatant> merged;
  merged.reserve(units_.size());
  std::vector<ObjectId> enrolled;

  std::size_t mine = 0;
  for (const WorldObject& slot : objects) {
    if (!is_combat_object(slot)) continue;
    // Anything of ours below this object's id no longer has an object.
    while (mine < units_.size() && units_[mine].id < slot.id) ++mine;
    if (mine < units_.size() && units_[mine].id == slot.id) {
      merged.push_back(units_[mine]);
      ++mine;
      continue;
    }

    // New. Seeded from the world, which is where the map's own numbers landed.
    const CombatProfile spec = profile(slot.class_index);
    Combatant fresh;
    fresh.id = slot.id;
    fresh.class_index = slot.class_index;
    fresh.owner = slot.state.owner;
    fresh.position = slot.state.position;
    // `World::populate_from_map` resolves `healthperc` (and the one absolute
    // `health`) against the class maximum, so this is the map's figure. A class
    // with no `maxhealth` at all still gets whatever the world holds.
    fresh.health = slot.state.health;
    if (spec.max_health > 0 && fresh.health > spec.max_health) fresh.health = spec.max_health;
    fresh.stamina = spec.max_stamina;
    // **`experience` and `level` are not read from the map, and that is a
    // refusal rather than an omission.** The map's `Level` attribute takes 32
    // distinct values spanning **0..59** over the 28 shipped `map.obj.xml`
    // documents (measured; `docs/formats/map.md` records the domain as 0..31,
    // which is the count of distinct values and not the range). The `level`
    // the nine desync dumps record is 1..24. And the host API has two of them:
    // `hero.hpp` documents that `SetLevel` writes an *inherent* level while
    // `.level` reads an effective one that Battle Cry and items move, so even
    // if the ranges agreed there would be no evidence for which one the
    // attribute is. `effective_level` feeds the damage formula on every strike
    // on every map, so a plausible number here would be worse than none.
    fresh.level = 1;
    // Built from the class, then rebuilt once it is in place (0x005d3090: the
    // object start, then `vtbl+0xc4`), so a unit that already has a hero, an
    // item or a skill when it joins the population starts with them.
    fresh.stats = class_stats(fresh);
    enrolled.push_back(fresh.id);
    merged.push_back(fresh);
  }

  units_ = std::move(merged);
  for (const ObjectId id : enrolled) recalc(id);
  return units_.size();
}

void CombatSystem::start(World& world) {
  // The world's diplomacy replaces this system's own ally table from here on.
  players_ = &world.players();
  world_ = &world;
  // And the world's objects are the population. Registering a system is not the
  // same as feeding it: `CombatSystem` was complete, tested and registered in
  // `kSystemOrder`, and `add` had no caller outside the unit tests, so every
  // real session ran with `units_.empty()` and `advance` returned on its first
  // line. Two hostile armies could stand on top of each other for a thousand
  // turns and nothing happened.
  reconcile(world);
}

bool CombatSystem::is_enemy(const Combatant& a, const Combatant& b) const noexcept {
  if (a.id == b.id) return false;
  if (a.owner == kNoPlayer || b.owner == kNoPlayer) return false;
  if (a.owner == b.owner) return false;

  // The real relation, when there is a world to ask. It is **one-directional**:
  // `Obj::IsEnemy` in `gbr.exe` tests bit 0 of the receiver's owner row and
  // never the transpose, so `a` may consider `b` an enemy while `b` does not
  // return the compliment. `sim/player.hpp` has the addresses.
  if (players_ != nullptr) return players_->is_enemy(a.owner, b.owner);

  // No world: fall back to this system's symmetric table. Reachable only from
  // a `CombatSystem` that was never started against a world.
  const std::pair<PlayerId, PlayerId> key =
      a.owner < b.owner ? std::pair{a.owner, b.owner} : std::pair{b.owner, a.owner};
  return !std::binary_search(allies_.begin(), allies_.end(), key);
}

// ---------------------------------------------------------------------------
// CombatSystem: targeting
// ---------------------------------------------------------------------------

bool CombatSystem::in_attack_range(const Combatant& attacker, const Combatant& defender) const {
  const CombatProfile mine = profile(attacker.class_index);
  const CombatProfile theirs = profile(defender.class_index);
  const std::int64_t d2 = distance_squared(attacker.position, defender.position);
  // Reach is measured surface to surface: the stored range plus both radii.
  // The far edge is the attacker's `[obj+0xd4]` (0x005a7800), so a formation's
  // `BonusRange` lengthens it; the minimum below is the class's `[obj+0xd8]`,
  // which no rebuild touches.
  const std::int64_t reach =
      static_cast<std::int64_t>(range_of(attacker)) + mine.radius + theirs.radius;
  if (d2 > squared(reach)) return false;
  if (mine.min_range > 0) {
    // A catapult's `min_range` of 301 really is a dead zone. Measured centre to
    // centre, because the far edge is what the class number describes.
    if (d2 < squared(static_cast<std::int64_t>(mine.min_range))) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// the engine's own acquisition
// ---------------------------------------------------------------------------

// What `act` gives a unit with no target, and what `BestTarget` answers on a
// unit receiver. **Not the original's** -- `sim/combat.hpp` says why it is
// kept -- and so it keeps the rule it was written with and changes nothing.
ObjectId CombatSystem::auto_target(ObjectId id) const {
  const Combatant* self = find(id);
  if (self == nullptr || !self->alive || self->action == Action::dying) return kNoObject;
  if (self->position == kHeldPosition) return kNoObject;
  const CombatProfile mine = profile(self->class_index);
  if (!mine.can_attack()) return kNoObject;
  const std::int64_t sight = mine.sight > 0 ? mine.sight : mine.range;
  const std::int64_t reach2 = squared(sight);
  ObjectId best = kNoObject;
  std::int64_t best_score = 0;
  for (const Combatant& other : units_) {
    if (distance_squared(self->position, other.position) > reach2) continue;
    if (!targetable(*self, other)) continue;
    const std::int64_t score = target_score(*self, other);
    // Strict, so a tie keeps the lower id the ascending roll meets first.
    if (best == kNoObject || score < best_score) {
      best = other.id;
      best_score = score;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// the danger counter
// ---------------------------------------------------------------------------

namespace {

/// 0x005d2800's decay, on a copy: a point per whole 1024 ms since the stamp,
/// floored at 0, and the stamp moved to `now` -- the whole difference, so
/// what the shift dropped is lost. The difference is unsigned 32-bit, as the
/// original's is, and 1024 ms or less changes nothing.
void decay_danger(std::int32_t& danger, GameTime& stamp, GameTime now) noexcept {
  const std::uint32_t elapsed =
      static_cast<std::uint32_t>(now) - static_cast<std::uint32_t>(stamp);
  if (elapsed <= 0x400u) return;
  const std::int64_t left = static_cast<std::int64_t>(danger) - (elapsed >> 10);
  danger = left < 0 ? 0 : static_cast<std::int32_t>(left);
  stamp += elapsed;
}

/// The cap 0x005d2850 keeps the counter under.
constexpr std::int32_t kMaxDanger = 6;

}  // namespace

std::int32_t CombatSystem::danger(ObjectId id, GameTime now) {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr) return 0;
  decay_danger(unit->danger, unit->danger_time, now);
  return unit->danger;
}

std::int32_t CombatSystem::danger_at(ObjectId id, GameTime now) const {
  const Combatant* unit = find(id);
  if (unit == nullptr) return 0;
  std::int32_t value = unit->danger;
  GameTime stamp = unit->danger_time;
  decay_danger(value, stamp, now);
  return value;
}

void CombatSystem::raise_danger(ObjectId id, std::int32_t amount, GameTime now) {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr) return;
  decay_danger(unit->danger, unit->danger_time, now);
  const std::int64_t sum = static_cast<std::int64_t>(unit->danger) + amount;
  unit->danger = sum < 0 ? 0 : sum > kMaxDanger ? kMaxDanger : static_cast<std::int32_t>(sum);
}

GameTime CombatSystem::clock() const noexcept {
  return world_ != nullptr ? world_->time() : now_;
}

// ---------------------------------------------------------------------------
// the unit target family (`docs/formats/target-score.md`)
// ---------------------------------------------------------------------------

void target_sweep(const World& world, Point centre, std::int32_t radius, PlayerId viewer,
                  std::vector<ObjectId>& out) {
  out.clear();
  if (radius < 0) return;
  std::vector<ObjectId> found;
  world.objects_located_in_rect(centre.x - radius, centre.y - radius, centre.x + radius,
                                centre.y + radius, found, SweepOrder::grid);
  const std::int64_t reach2 = squared(radius);
  const PlayerTable& players = world.players();
  for (const ObjectId id : found) {
    const WorldObject* slot = world.find(id);
    if (slot == nullptr || slot->state.flags.unspawned) continue;
    if (distance_squared(centre, slot->state.position) > reach2) continue;
    if (slot->state.flags.hidden) {
      // Past only with its owner's bit in the viewer's `seeflags`: the owner
      // shares its view with the viewer. A player's own bit is taken as set
      // (`docs/formats/target-score.md` names it unknown).
      const PlayerId owner = slot->state.owner;
      const bool seen = owner == viewer ||
                        (PlayerTable::is_valid(owner) && PlayerTable::is_valid(viewer) &&
                         players.has(owner, viewer, Relation::share_view));
      if (!seen) continue;
    }
    out.push_back(id);
  }
}

/// One run of the search object 0x005d45a0 builds: the attacker, the five
/// slots, and what the predicate holds so far.
struct CombatSystem::Search {
  CombatSystem& sys;
  const Combatant& self;
  const CombatProfile mine;
  const TargetFilter& filter;
  GameTime now;
  bool commit;
  /// The attacker's hero (`[unit+0x170]`), when it resolves: the score
  /// measures from both.
  const Combatant* hero = nullptr;

  ObjectId best = kNoObject;             ///< `[search+0x14]`
  std::int64_t best_score = 0x7fffffff;  ///< `[search+0x20]`
  bool best_unit = false;                ///< `[search+0x24]`

  [[nodiscard]] bool is_unit(const Combatant& c) const {
    if (sys.world_ == nullptr) return true;
    const WorldObject* slot = sys.world_->find(c.id);
    return slot == nullptr || slot->state.flags.is_unit;
  }

  /// 0x005a77b0: centre to centre, less both class radii.
  [[nodiscard]] std::int64_t surface(const Combatant& from, const Combatant& to) const {
    return isqrt(distance_squared(from.position, to.position)) -
           sys.profile(from.class_index).radius - sys.profile(to.class_index).radius;
  }

  /// 0x005d3630. Answers the score; `in_range` is its third output.
  std::int64_t score(const Combatant& cand, bool& in_range) {
    std::int64_t d = surface(self, cand);
    // A unit that follows a hero measures from both, the sum halved toward
    // zero.
    if (hero != nullptr) d = (surface(*hero, cand) + d) / 2;
    const std::int32_t danger = commit ? sys.danger(cand.id, now) : sys.danger_at(cand.id, now);
    std::int64_t s = sys.profile(cand.class_index).target_factor +
                     static_cast<std::int64_t>(danger) * 40 + d;
    if (d < mine.min_range) s += 40;
    if (sys.world_ != nullptr) {
      // Moving: the active-path bit, 0x20000 of the sync word.
      if (const WorldObject* slot = sys.world_->find(cand.id);
          slot != nullptr && slot->state.flags.has_active_path) {
        s += 80;
      }
    }
    in_range = d <= sys.range_of(self);
    return s;
  }

  void take(ObjectId id, std::int64_t s) {
    best = id;
    best_score = s;
    best_unit = true;
  }

  /// 0x005db650, for a unit. False ends a sweep.
  bool predicate(const Combatant& cand) {
    // The `CanAttack` virtual, held, dead, a negative `target_factor`, the
    // enemy mask: `targetable`.
    if (!sys.targetable(self, cand)) return true;
    if (filter.exclude_independents && cand.owner == kNeutralWildlife) return true;
    if (filter.require_armed && sys.attack_of(cand) <= 0) return true;
    bool in_range = false;
    std::int64_t s = score(cand, in_range);
    if (filter.prefer_undiseased && sys.world_ != nullptr) {
      if (const WorldObject* slot = sys.world_->find(cand.id);
          slot != nullptr && slot->state.flags.diseased) {
        s += 200;
      }
    }
    const bool mismatch = filter.named && (filter.named_class == kNoClass ||
                                           cand.class_index != filter.named_class);
    if (mismatch) {
      if (filter.exclusive) return true;
      s += 200;
    } else if (in_range) {
      if (s < 50) {
        take(cand.id, s);
        return false;
      }
      s -= 100;
    }
    // A unit held keeps its place only against a strictly worse score.
    if (best != kNoObject && best_unit && best_score < s) return true;
    take(cand.id, s);
    return true;
  }

  /// 0x005db7f0. False ends a sweep.
  bool offer(const Combatant& cand) {
    if (filter.require_in_attack_range) {
      const std::int64_t d = surface(self, cand);
      if (d > sys.range_of(self) || d < mine.min_range) return true;
    }
    if (is_unit(cand)) return predicate(cand);
    // The catapult fallback (0x005db856): only while no unit is held, only a
    // siege engine the attacker may strike, by plain surface distance.
    if (best_unit) return true;
    if (!sys.profile(cand.class_index).is(CombatProfile::kLineCatapult)) return true;
    if (!sys.targetable(self, cand)) return true;
    const std::int64_t d = surface(self, cand);
    if (best != kNoObject && d > best_score) return true;
    best = cand.id;
    best_score = d;
    return true;
  }
};

ObjectId CombatSystem::search(ObjectId id, const TargetFilter& filter,
                              std::span<const ObjectId> list, bool use_list, bool commit,
                              bool raise) {
  const Combatant* self = find(id);
  if (self == nullptr || !self->alive || self->action == Action::dying) return kNoObject;
  const GameTime now = clock();
  Search run{*this, *self, profile(self->class_index), filter, now, commit};
  if (world_ != nullptr) {
    if (const HeroSystem* heroes = hero_system_of(*world_); heroes != nullptr) {
      const ObjectId leader = heroes->hero_of(id);
      if (leader != kNoObject && leader != id) run.hero = find(leader);
    }
  }

  if (use_list) {
    // The squad's `Enemies` or a GAIKA node's squads: every entry offered,
    // whatever the predicate answers.
    for (const ObjectId candidate : list) {
      if (const Combatant* other = find(candidate); other != nullptr) (void)run.offer(*other);
    }
  } else {
    // Inside a holder, nothing: 0x005dca02 and 0x005dd0f0 both refuse it.
    if (self->position == kHeldPosition) return kNoObject;
    const auto sight_of = [this](const Combatant& c) -> std::int32_t {
      if (world_ != nullptr) {
        if (const WorldObject* slot = world_->find(c.id); slot != nullptr) return slot->sight;
      }
      const CombatProfile p = profile(c.class_index);
      return p.sight > 0 ? p.sight : p.range;
    };
    Point centre = self->position;
    std::int32_t radius = sight_of(*self);
    if (filter.centre_on_point) {
      // 0x005dd0f8: an x of -1 is the receiver's own position. 0x005dd11d:
      // the radius given, unless it is under 1 or the receiver shoots (a
      // declared `projectile_class`, 0x0053f5b0) -- then its sight.
      if (filter.centre.x != -1) centre = filter.centre;
      if (filter.radius >= 1 && !run.mine.shoots) radius = filter.radius;
    } else if (filter.around_hero && run.hero != nullptr) {
      centre = run.hero->position;
      radius = sight_of(*run.hero);
    }
    // The enemy mask: the attacker's player's war word, less wildlife's bit
    // for the no-independents flag.
    const auto in_mask = [&](const Combatant& c) {
      if (!is_enemy(*self, c)) return false;
      return !(filter.exclude_independents && c.owner == kNeutralWildlife);
    };
    if (world_ != nullptr) {
      std::vector<ObjectId> ids;
      target_sweep(*world_, centre, radius, self->owner, ids);
      for (const ObjectId candidate : ids) {
        const Combatant* other = find(candidate);
        if (other == nullptr || !in_mask(*other)) continue;
        if (!run.offer(*other)) break;
      }
    } else {
      const std::int64_t reach2 = squared(radius);
      for (const Combatant& other : units_) {
        if (distance_squared(centre, other.position) > reach2 || !in_mask(other)) continue;
        if (!run.offer(other)) break;
      }
    }
  }

  if (raise && run.best_unit && run.best != kNoObject) raise_danger(run.best, 1, now);
  return run.best;
}

ObjectId CombatSystem::best_target(ObjectId id) const { return best_target(id, TargetFilter{}); }

ObjectId CombatSystem::best_target(ObjectId id, const TargetFilter& filter) const {
  // With `commit` and `raise` clear the search writes nothing.
  return const_cast<CombatSystem*>(this)->search(id, filter, {}, false, false, false);
}

ObjectId CombatSystem::acquire(ObjectId id, const TargetFilter& filter) {
  return search(id, filter, {}, false, true, true);
}

ObjectId CombatSystem::best_target_among(ObjectId id, std::span<const ObjectId> candidates) {
  return search(id, TargetFilter{}, candidates, true, true, false);
}

ObjectId CombatSystem::best_target_in_list(ObjectId id, std::span<const ObjectId> candidates,
                                           const TargetFilter& filter) {
  return search(id, filter, candidates, true, true, true);
}

bool CombatSystem::targetable(const Combatant& self, const Combatant& other) const {
  if (!other.alive || other.action == Action::dying) return false;
  // Nothing at no health is a target. For a unit that is death itself (its
  // `vtbl + 0x50` is `health == 0`, 0x004e1f50); for a building, which never
  // dies (`dies_at_no_health`), it is the separate test `0x005b1f69` makes on
  // the candidate's health once it has found it a building -- so a town hall
  // brought to nothing stands broken and nobody goes on hitting it. That test
  // spares a building whose settlement's centre is an `Outpost` or a
  // `BaseShipyard` (0x00440060, 0x004400b0) with something in its holder
  // (inferred from the `+0x40` it reads); that exception needs the settlement
  // and is not reproduced.
  if (other.health <= 0) return false;
  // **Nothing inside a holder is a target.** A garrisoned unit, or a ship's
  // passenger, is off the original's grid altogether (0x005d3e10 drops it to
  // (-1, -1) and out of its cell), so no sweep can find it; here every held
  // object stands at `kHeldPosition` and nothing else ever does, which is
  // the test. Without it every garrison on a map stood on one spot and
  // fought every other.
  if (other.position == kHeldPosition) return false;
  if (!is_enemy(self, other)) return false;
  const CombatProfile theirs = profile(other.class_index);
  if (!theirs.can_be_attacked || theirs.never_targeted()) return false;
  // 0x005db650 asks the attacker's `CanAttack` virtual of every candidate
  // before anything else, so a swordsman's sweep never finds a building.
  return unit_can_attack(self, other);
}

bool CombatSystem::unit_can_attack(const Combatant& self, const Combatant& target) const {
  if (world_ == nullptr) return true;
  const WorldObject* me = world_->find(self.id);
  const WorldObject* them = world_->find(target.id);
  if (me == nullptr || them == nullptr) return true;
  // A tower's or an engine's own virtual is not this one.
  if (!me->state.flags.is_unit) return true;

  const CombatProfile mine = profile(self.class_index);
  const CombatProfile theirs = profile(target.class_index);
  const bool target_is_unit = them->state.flags.is_unit;
  const bool target_is_building = them->state.flags.is_building;

  if (target_is_unit) {
    if (theirs.is(CombatProfile::kLineSentry) && !mine.shoots) return false;
    if (mine.is(CombatProfile::kLineRamUnit)) return false;
  }
  if (theirs.is(CombatProfile::kLineFish)) return false;

  // The settlement is looked up only for the two clauses that read it: it is
  // a scan of the store, and every unit's sweep asks this of every building.
  if (target_is_building && them->settlement != kNoObject &&
      (theirs.is(CombatProfile::kLineFakeTower) || mine.is(CombatProfile::kLineRamUnit))) {
    if (EconomySystem* economy = economy_of(*world_); economy != nullptr) {
      if (const Settlement* set = economy->settlements().for_object(them->settlement);
          set != nullptr) {
        if (theirs.is(CombatProfile::kLineFakeTower)) return false;
        if (mine.is(CombatProfile::kLineRamUnit)) {
          const WorldObject* anchor = world_->find(set->anchor);
          const CombatProfile hub = profile(anchor != nullptr ? anchor->class_index : kNoClass);
          const bool soft =
              hub.is(CombatProfile::kLineOutpost) || hub.is(CombatProfile::kLineBaseShipyard);
          return soft || theirs.is(CombatProfile::kLineGate);
        }
      }
    }
  }

  if (target_is_building && theirs.is(CombatProfile::kLineCatapult)) {
    return !mine.is(CombatProfile::kLineBeast);
  }

  switch (mine.damage_type) {
    case DamageType::slash:
    case DamageType::pierce:
      return target_is_unit || mine.shoots;
    case DamageType::siege:
      return true;
    case DamageType::none:
    default:
      return false;
  }
}

std::int64_t CombatSystem::target_score(const Combatant& self,
                                        const Combatant& other) const noexcept {
  // `auto_target`'s: distance^2 * target_factor / target_priority, smallest
  // wins, from the attacker's own position. This engine's own rule; the
  // original's family scores with 0x005d3630 (`Search::score`).
  const CombatProfile theirs = profile(other.class_index);
  const std::int64_t factor = theirs.target_factor > 0 ? theirs.target_factor : 100;
  const std::int64_t priority = theirs.target_priority > 0 ? theirs.target_priority : 100;
  // +1 so a target standing exactly on top of us still ranks by its factor
  // rather than collapsing every candidate to zero.
  return (distance_squared(self.position, other.position) + 1) * factor / priority;
}

bool CombatSystem::order_attack(ObjectId attacker, ObjectId target) {
  Combatant* a = mutable_find(attacker);
  Combatant* d = mutable_find(target);
  if (a == nullptr || d == nullptr || a == d) return false;
  if (!a->alive || !d->alive) return false;
  if (a->action == Action::dying || d->action == Action::dying) return false;
  if (d->health <= 0) return false;  // see `targetable`
  if (d->position == kHeldPosition || a->position == kHeldPosition) return false;  // and there
  if (!is_enemy(*a, *d)) return false;
  if (!profile(d->class_index).can_be_attacked) return false;
  // `Unit::Attack`'s refused path (0x005ddbd2): playtest #24's swordsman.
  if (!unit_can_attack(*a, *d)) return false;
  if (a->target != target) a->attacks = 0;
  a->target = target;
  a->action = Action::engaging;
  a->anim = kAnimEngage;
  return true;
}

bool CombatSystem::stop(ObjectId id) {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr) return false;
  unit->target = kNoObject;
  unit->attacks = 0;
  if (unit->action == Action::engaging) {
    unit->action = Action::idle;
    unit->anim = kAnimIdle;
  }
  return true;
}

bool CombatSystem::drop_target(ObjectId id) {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr) return false;
  if (unit->target != kNoObject) {
    unit->target = kNoObject;
    unit->attacks = 0;
  }
  return true;
}

bool CombatSystem::hand_target(ObjectId id, ObjectId target) {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr) return false;
  if (unit->target != target) {
    unit->target = target;
    unit->attacks = 0;
  }
  return true;
}

// ---------------------------------------------------------------------------
// CombatSystem: damage
// ---------------------------------------------------------------------------

std::int32_t CombatSystem::effective_level(const Combatant& unit) const noexcept {
  return unit.base_effective_level() + player_level_addend(unit.owner);
}

DamageInputs CombatSystem::inputs_for(const Combatant& attacker,
                                      const Combatant& defender) const {
  const CombatProfile mine = profile(attacker.class_index);

  DamageInputs in;
  in.attack = attack_of(attacker);
  in.type = mine.damage_type;
  in.armour = armour_of(defender, in.type);
  in.attacker_level = effective_level(attacker);
  in.defender_level = effective_level(defender);
  in.counter_percent =
      counters_.damage_percent(attacker.class_index, defender.class_index, counter_mode_);
  in.ignores_armour = attacker.ignores_armour;
  in.in_settlement = attacker.in_settlement;
  in.multiplier_percent = attacker.damage_multiplier_percent;
  return in;
}

DamageBreakdown CombatSystem::preview(ObjectId attacker, ObjectId defender) const {
  const Combatant* a = find(attacker);
  const Combatant* d = find(defender);
  if (a == nullptr || d == nullptr) return DamageBreakdown{};
  return resolve_damage(inputs_for(*a, *d), constants_);
}

namespace {

/// Start `slot` on `id` **as of `began`**, not as of the end of the turn.
///
/// Combat drains at exact game times inside a turn, so a blow or a death
/// happens at some instant `began` before the turn ends. `World::play_anim`
/// starts the cursor at zero, and the next turn's animation pass then adds
/// the whole next turn -- which would leave the cursor `turn.end - began`
/// behind where it should be, by an amount that depends on where the turn
/// boundaries fell. Combat's own schedule does not depend on that
/// (`advance`'s drain is in absolute time), and the cursor is in the `slots`
/// hash, so it must not either. This brings it forward by exactly the part
/// of the turn it has already lived through, the way `World::run_turn`
/// advances every other cursor.
///
/// Measured to `turn.end`, the drain's own upper bound, and **not** to
/// `World::time()`: a turn's window is `(time_before, time_before + 1 +
/// length]`, one unit past the clock, so a blow can land at `time() + 1`.
/// The first version measured to `time()`, and a partition test caught it a
/// millisecond out on exactly that blow.
void start_anim_at(World& world, ObjectId id, std::int32_t slot, GameTime began) {
  if (!world.play_anim(id, slot, AnimRepeat::hold)) return;
  const GameTime late = world.turn().end - began;
  if (late <= 0) return;
  WorldObject* object = world.find(id);
  if (object == nullptr || object->object == nullptr) return;
  AnimCursor& cursor = object->object->anim;
  cursor.elapsed_ms = advance_elapsed(object->timeline, cursor.elapsed_ms, late, object->repeat);
  const AnimSample sample = object->timeline.sample(cursor.elapsed_ms, object->repeat);
  cursor.step = sample.step;
  if (sample.finished) object->animating = false;
}

/// A siege engine, the one heir of `CVXBuilding` that dies: see
/// `dies_at_no_health`.
[[nodiscard]] bool is_siege_engine(const World& world, ObjectId id) noexcept {
  const WorldObject* slot = world.find(id);
  return slot != nullptr && slot->object != nullptr && slot->state.flags.is_building &&
         slot->object->is_a(NativeClass::catapult);
}

}  // namespace

bool CombatSystem::is_dying(ObjectId id) const noexcept {
  const Combatant* unit = find(id);
  return unit != nullptr && unit->action == Action::dying;
}

GameTime CombatSystem::death_duration_of(const World& world, const Combatant& unit) const {
  if (world_bound_) {
    const WorldObject* slot = world.find(unit.id);
    if (slot != nullptr && slot->object != nullptr && slot->object->entity != nullptr) {
      const AnimTimeline death = slot->object->entity->timeline_for_slot(core::kAnimDie);
      if (death.valid() && death.cycle() > 0) return death.cycle();
    }
  }
  return death_duration_;
}

/// **Facing.** The dumps print a vector after every object's `Anim` index,
/// and on an engaged unit it is not a heading of the unit's own choosing: of
/// the 92 blocks with `Anim=4` whose `target handle` resolves, **88 carry
/// exactly `target position - own position`** and the other four are within
/// 90 degrees of it (their target has moved since). So a swinging unit faces
/// its target, and the facing is in the object table the `slots` channel
/// hashes. This engine keeps it where movement keeps it: `MoveState::facing`
/// and the sheet column `AnimCursor::variation`, both already hashed, both
/// already saved. A unit movement has never tracked gets the column only,
/// rather than a movement record it did not have -- a tower that turns to
/// shoot is not a mover.
///
/// **The animation.** All 92 dump blocks in `action=2` (90 `engage`, 2
/// `attack`) print `Anim=4`, and where the printed `Anim` can be checked
/// against what the object is doing it is the entity slot less one: 0 on
/// movers (slot 1, walk), 4 on attackers (slot 5), 8 on all 15 dying (slot
/// 9, die), 16 on 27 villagers carrying goods home (slot 17, carry). So an
/// engaged unit plays slot 5, `kAnimAttack`, and the cursor that says so is
/// in the hash. (17, 18 and 19 fit the same rule less cleanly, and nothing
/// here leans on them.)
///
/// One cycle per blow, **held** rather than looped: `attack_interval` is that
/// same animation's measured length, so back-to-back blows play back-to-back
/// swings, and a unit whose next blow does not come -- target dead, out of
/// reach, a ceasefire -- is not left swinging at the air.
///
/// **What is not reproduced, and is recorded rather than changed.** The
/// original lands the blow `action_time` into the swing (`GHORSEMAN`'s attack
/// is 1,065 ms with `action_time` 568; `BBOWMAN`'s arrow leaves at 726 of
/// 990). Here the blow lands when the swing *starts*, because that is when
/// `CombatSystem::act` has always struck and moving it is a combat-timing
/// change. And the `toattack` transition (slot 19, the 1-to-2 edge) is not
/// played before the first swing: it would delay either the blow or the
/// swing it belongs to.
void CombatSystem::show_swing(World& world, const Combatant& attacker,
                              const Combatant& defender, GameTime now) const {
  if (!world_bound_) return;
  WorldObject* slot = world.find(attacker.id);
  if (slot == nullptr || slot->object == nullptr) return;
  const Point towards{defender.position.x, defender.position.y};
  const Point heading{towards.x - attacker.position.x, towards.y - attacker.position.y};
  if (heading.x != 0 || heading.y != 0) {
    MovementSystem* movement = movement_system(world);
    if (movement != nullptr && movement->find(attacker.id) != nullptr) {
      movement->face(world, attacker.id, towards);
    } else if (slot->object->entity != nullptr) {
      slot->object->anim.variation = facing_column(heading, slot->object->entity->variations());
    }
  }
  start_anim_at(world, attacker.id, core::kAnimAttack, now);
}

/// Slot 18, `toidle`, is the edge back out of the fighting pose in the
/// entity's own state machine: `attack` runs state 2 to 2 and `toidle` runs
/// 2 to 1, in every unit that declares both (122 of the 148 unit entities
/// declare slot 18). **INFERRED** from that graph rather than seen: nothing
/// in the dumps can say which frame a settling unit was on. Played only over
/// a swing that has *finished*, so a blow in progress is never cut, and only
/// over a swing, so a unit that has started walking keeps its walk.
void CombatSystem::settle_swing(World& world, const Combatant& unit, GameTime now) const {
  if (!world_bound_) return;
  const WorldObject* slot = world.find(unit.id);
  if (slot == nullptr || slot->object == nullptr) return;
  if (slot->animating || slot->object->anim.anim_slot != core::kAnimAttack) return;
  start_anim_at(world, unit.id, core::kAnimToIdle, now);
}

void CombatSystem::enter_dying(World& world, Combatant& unit, GameTime now) {
  if (unit.action == Action::dying) return;
  // `gbr.exe`'s unit death virtual (0x005db270), in its order, as far as this
  // engine has the parts. See `HeroSystem`'s "death" block for the whole list.
  const ObjectId id = unit.id;
  HeroSystem* heroes = hero_system_of(world);
  // One: the Wisdom roll, and its draw on the world RNG, ahead of everything.
  if (heroes != nullptr) heroes->roll_wisdom(world, id);

  // The dumps' dying state: action=8, Anim=8, an empty method, health 0-3.
  //
  // **Marked here, before the hook, where the original sets it last** (its
  // base death, 0x005b16f0, is the final step). Nothing a shipped hook calls
  // reads the action, and the mark is this engine's re-entry guard: a hook that
  // damages the unit it is running for must find it already dying.
  unit.action = Action::dying;
  unit.anim = kAnimDying;
  fresh_deaths_ = true;
  unit.target = kNoObject;
  unit.attacks = 0;
  unit.death_time = now + death_duration_of(world, unit);
  if (world_bound_) {
    // A corpse does not walk: all 15 dying objects across the nine dumps carry
    // `SyncFlags` without bit 17, the active-path bit, and none is `action=3`.
    // Stopped before the animation starts so that movement's own arrival
    // handling has nothing left to stop; see `MovementSystem::play_locomotion`.
    if (MovementSystem* movement = movement_system(world); movement != nullptr) {
      movement->stop(world, unit.id);
    }
    // `Anim=8` is slot 9. Held, so the last row -- what is left of the body --
    // stays on screen until the object goes. Facing is left where it was: every
    // dying block keeps a non-zero vector.
    start_anim_at(world, unit.id, core::kAnimDie, now);
  }
  unit.next_action_time = unit.death_time;
  if (unit.health < 0) unit.health = 0;
  events_.push_back(CombatEvent{CombatEvent::Kind::death, now, kNoObject, unit.id, 0,
                                unit.health});
  // Three: `SetHealth(0)`, on the world, now -- not at the end of the turn with
  // everybody else's -- because the hook is about to run and `IsDead`,
  // `IsAlive` and `health` read the world. A unit only: the death virtual is
  // `CVXUnit`'s, and a building's tier is left to the end-of-turn write. A
  // siege engine too, the one building that dies: its damage routine stores
  // the health before it asks whether the engine is dead, so the engine's own
  // `CATAPULT_IDLE.VS` loop, `while (.IsAlive)`, ends on the turn it dies.
  const bool engine = is_siege_engine(world, id);
  if (world_bound_) {
    if (const ObjectState* state = world.state(id);
        state != nullptr && (state->flags.is_unit || engine)) {
      (void)world.set_health(id, unit.health);
    }
  }
  // Four: the class's own death script, on the object that just died, **run to
  // completion right here** -- see `sim/hooks.hpp`. `unit` is a reference into
  // `units_` and is not touched again: a hook runs arbitrary host code, and
  // nothing below may assume the vector it points into is where it was.
  //
  // `fire_class_hook` takes the object's one-shot latch, so a script that
  // `Erase`s the corpse before its death animation ends does not run the hook a
  // second time.
  (void)fire_class_hook(world, ClassHook::on_die, id);
  // An engine's own death virtual goes on from here; see `die_as_engine`.
  if (engine) die_as_engine(world, id, now);
  // Six to eight: out of the hero's army and out of the squad, after the hook
  // and not before it -- `CMERCENARY_ONDIE.VS` pays its gold to `.hero`.
  // Five, the held unit's erase, is `on_death`'s exception. `SetPath(null)` is
  // the `movement->stop` above; the census counts and the GAIKA-node removal
  // of the AI unregister are not reproduced (this engine counts on demand).
  if (heroes != nullptr) heroes->on_death(world, id);
}

/// What `CVXCatapult`'s death virtual (0x004e3fd0) does between the `ondie`
/// launch it shares with every death (0x005141b0, `enter_dying`'s hook) and
/// the base death that makes the corpse (0x005b16f0, `enter_dying`'s dying
/// state), in its order:
///
///   1. **`SetBuilt`'s body, inlined** (compare 0x004e2e20): the built flag at
///      `[cat+0x208]` set, the sprite clock let run, bit 25 of the `+0x2c`
///      word cleared, the build frame at `[cat+0x20c]` written back to -1, and
///      the two animation calls. What lands here is what `Catapult::SetBuilt`
///      lands -- the flag and the frame -- so that the `destroy` animation is
///      drawn rather than the construction stage the engine had frozen on.
///      Bit 25 is left out for the reason `SetBuilt` gives.
///   2. A call on the global at `[0x009c0938]` (0x005e7710), a walk over
///      lists of its own that this engine has no counterpart for. Not
///      reproduced.
///   3. **Every crewman dies.** The roster is copied (0x00438330) from the
///      engine's settlement (`[cat+0x148]`), its holder (`+0x5e`) and the
///      holder's member deque (`+0x28`) -- the list `Settlement::Units`
///      returns -- and each member's own death virtual, `vtbl+0xb0`, is
///      called on it: for a unit that is 0x005db270, the whole unit death,
///      `ondie` and all. They are not ejected and not released; nothing in
///      the handler reads their health or deals them damage first.
///
/// A crewman is held, and the unit death erases a held unit on the spot
/// (0x005db35a tests the holder handle at `[unit+0x154]` and takes
/// `vtbl+0x94`). This engine defers that erase to the end of the death
/// animation for every held unit, as `HeroSystem::on_death` records; the
/// corpse is off the roster when the turn's reap runs (`reap_departed`).
///
/// **And then the engine leaves the world.** The corpse lives as a unit's
/// does: `CVXCatapult`'s animation-finished virtual (0x004e2c00, `vtbl+0x54`)
/// ends in `Obj`'s (0x005b0f70), which, for an object its `vtbl+0x50` calls
/// dead, plays the die animation and erases the object (`vtbl+0x94`) once
/// that has finished. All eight shipped engine entities declare slot 9 --
/// `destroy`, 385 ms, on the base, Gallic, Carthaginian and Iberian
/// catapults; 660 ms on the Roman one and 733 ms on the Egyptian ballista;
/// about 16 s on the British and German rams -- so `death_duration_of` reads
/// the cycle a unit's `die` gives it, and `advance` despawns the engine at its
/// end. From then on `bld.IsValid` fails, and `UNIT_BUILD_CATAPULT.VS` sends
/// a builder still on its way off with a `move` instead of an `AddUnit`.
///
/// What the handler does not do is refuse a builder that arrives while the
/// corpse is still playing: `Settlement::AddUnit` (0x005c1b20 -> 0x005d3e10
/// -> 0x00532160) tests only that the unit is not already in and the holder
/// is not full. Nor does it touch the engine's fake towers; what becomes of
/// them when the corpse is erased is not read here.
void CombatSystem::die_as_engine(World& world, ObjectId id, GameTime now) {
  WorldObject* slot = world.find(id);
  if (slot == nullptr) return;
  slot->state.flags.built = true;
  slot->build_frame = -1;
  EconomySystem* economy = economy_of(world);
  if (economy == nullptr) return;
  const Settlement* settlement = economy->settlements().for_object(slot->settlement);
  if (settlement == nullptr) return;
  // Copied, as the original copies it: each death runs its own hook, and a
  // hook can move anything this would otherwise be pointing into.
  const std::vector<ObjectId> crew = settlement->holder.units;
  for (const ObjectId member : crew) {
    Combatant* crewman = mutable_find(member);
    if (crewman == nullptr || !crewman->alive) continue;
    // The unit death virtual itself, not a blow: no damage is dealt, nothing
    // is scored, and nobody is credited with the kill.
    crewman->health = 0;
    enter_dying(world, *crewman, now);
  }
}

std::int32_t CombatSystem::kill_gain(World& world, ObjectId attacker, ObjectId victim,
                                     std::int32_t victim_experience) {
  // 0x00511976..0x005119e0, the kill block of the blow. Both ends must be
  // units -- the blow casts each to `CVXUnit` and drops one that is not --
  // and the victim no `Sentry` (0x005400c0).
  if (!world_bound_) return 0;
  HeroSystem* heroes = hero_system_of(world);
  if (heroes == nullptr) return 0;
  const ObjectState* killer = world.state(attacker);
  const ObjectState* dead = world.state(victim);
  if (killer == nullptr || dead == nullptr || !killer->flags.is_unit || !dead->flags.is_unit) {
    return 0;
  }
  if (const ClassGraph* graph = world.class_graph(); graph != nullptr) {
    const ClassIndex sentry = graph->find("Sentry");
    if (sentry != kNoClass && world.class_is_a(victim, sentry)) return 0;
  }
  // A sixth of how many levels the victim stood above the killer, truncated,
  // kept to [0, 8], plus one: 1 for an equal or a lesser victim, 9 at most.
  // Both levels are of the running totals (0x005d28c0), not the stored levels.
  const std::int32_t mine = heroes->level_for_experience(heroes->experience(attacker));
  const std::int32_t theirs = heroes->level_for_experience(victim_experience);
  const std::int32_t steps = std::clamp((theirs - mine) / 6, 0, 8);
  const std::int32_t base = steps + 1;
  // The owner's modifier `[player+0x84]` (0x0050fec0): none keeps the base; a
  // positive one multiplies it by `m / 100 + 1`, and by one more with a
  // chance of `m % 100` in a hundred; a negative one loses the whole gain with
  // a chance of `-m` in a hundred. A draw on the world's generator only where
  // a chance is taken.
  const std::int32_t modifier = player_experience_modifier(killer->owner);
  std::int32_t gain = base;
  if (modifier > 0) {
    std::int32_t times = modifier / 100 + 1;
    if (modifier % 100 > 0 && world.rng().between(0, 99) < modifier % 100) ++times;
    gain = times * base;
  } else if (modifier < 0) {
    if (world.rng().between(0, 99) < -modifier) gain = 0;
  }
  // And the same gain goes into the killer's hero's pool (0x005119d7).
  if (const ObjectId hero = heroes->hero_of(attacker); hero != kNoObject) {
    heroes->pool_experience(hero, gain);
  }
  return gain;
}

void CombatSystem::pay_experience(World& world, ObjectId attacker, std::int32_t amount) {
  // 0x005119f3..0x00511ad5. A hero or a unit in training takes it all;
  // anyone else stops at the start of its owner's `max_battle_level` -- the
  // Standardization researches' "cannot gain more experience" -- when that
  // ledger entry is set: a total that would reach it is set to it, and one
  // already there is left alone.
  if (amount == 0 || !world_bound_) return;
  HeroSystem* heroes = hero_system_of(world);
  const ObjectState* killer = world.state(attacker);
  if (heroes == nullptr || killer == nullptr) return;
  const std::int32_t had = heroes->experience(attacker);
  const std::int32_t total = had + amount;
  if (!killer->flags.is_hero && !killer->flags.training) {
    const EnvSystem* env = env_of(world);
    const std::int32_t level =
        env == nullptr
            ? 0
            : env->env().read_int(EnvScope::for_player(player_to_script(killer->owner)),
                                  "max_battle_level");
    if (level != 0) {
      const std::int32_t cap = heroes->experience_for_level(level);
      if (total >= cap) {
        if (had < cap) (void)heroes->set_experience(world, attacker, cap);
        return;
      }
    }
  }
  (void)heroes->set_experience(world, attacker, total);
}

/// A building keeps its own running total of what it has taken --
/// `ObjectState::damage_taken`, which `MostDamagedBuilding` ranks by and
/// `ClearDamageTaken` zeroes. Called from both places health is removed, the
/// blow and `Obj::Damage`, because the original's counter sits in the
/// building's own damage handler and sees both; unlike `record_score` it is
/// not `hit`-only. Only the health actually removed counts, never overkill.
/// Whether a combatant brought to no health dies of it. **A building does
/// not**: it stands at zero, broken, and stays its owner's.
///
/// `gbr.exe`'s damage routine (0x005a7720, reached from the building override
/// 0x004db950) clamps the blow to the health left, stores the new health, and
/// only then asks the virtual at `vtbl + 0x50` whether the object is dead,
/// calling the death virtual at `vtbl + 0xb0` if it is. In every building-family
/// vtable -- `CVXBuilding` 0x007ba488, `CVXTownHall` 0x007d0500, `CVXBarrack`
/// 0x007b97f8, the gate's 0x007bfbf8 and the other three -- `+0x50` is
/// 0x00746ea0, `return 0`, and `+0xb0` is 0x00686eb0, a bare `ret`. The unit
/// vtables carry 0x004e1f50 and 0x005db270 there. `Obj::IsAlive` (0x005aae00)
/// is the same `+0x50` negated, so a building at no health still answers
/// alive. What does happen at the bottom is the damage tier: health 0 is tier
/// 3, `IsBroken`, and entering tier 3 runs the class's `broken` method.
///
/// That is also what the shipped data expects. `1 ELIMINATION.VS` counts a
/// seat's `BaseTownhall`s and says "managed to recapture" when the count comes
/// back; the repopulate and supply scripts ask whether a village's centre
/// `IsBroken` and hold back gold to repair it; the catapult scripts skip a
/// very broken building unless it is a centre with a garrison. None of that
/// reads right if the building were gone.
///
/// **A siege engine is the exception** (playtest #19). `CVXCatapult` is a
/// building heir, but its vtable (0x007baee0) carries the unit's `+0x50`,
/// 0x004e1f50 -- dead is `health == 0` -- and a death virtual of its own at
/// `+0xb0`, 0x004e3fd0. So an engine brought to nothing dies like a unit and
/// leaves the world after its `destroy` animation; see `die_as_engine`. Held
/// as a building, an uncrewed engine that decayed stood at zero with its door
/// open, and the builders still on their way walked in.
///
/// Unbound, every combatant stays mortal: a synthetic roll has no world
/// objects to say which of its ids is a building.
bool dies_at_no_health(const World& world, bool world_bound, ObjectId id) {
  if (!world_bound) return true;
  const WorldObject* slot = world.find(id);
  if (slot == nullptr || !slot->state.flags.is_building) return true;
  return slot->object != nullptr && slot->object->is_a(NativeClass::catapult);
}

void note_building_damage(World& world, ObjectId victim, std::int32_t removed) {
  ObjectState* state = world.mutable_state(victim);
  if (state != nullptr && state->flags.is_building) state->damage_taken += removed;
}

void CombatSystem::record_score(World& world, ObjectId source, const Combatant& defender,
                                std::int32_t removed, bool killed) const {
  MatchSystem* match = match_system_of(world);
  if (match == nullptr) return;
  const Combatant* attacker = find(source);
  const PlayerId by = attacker == nullptr ? kNoPlayer : attacker->owner;
  match->record_damage(world, by, defender.owner, removed);
  // The victim's *class* maximum, not the health it had left: a finishing blow
  // on a sliver is worth the same as a kill from full. `profile` answers 0 for
  // a class this system has no row for, and `record_kill` drops a zero.
  if (killed) match->record_kill(world, by, defender.owner, profile(defender.class_index).max_health);
}

void CombatSystem::hit(World& world, Combatant& defender, const DamageInputs& inputs,
                       ObjectId source, GameTime now) {
  // The victim's shelter is world state, not a combatant field, so it is read
  // here where the world is in hand -- the shape `note_building_damage` uses.
  DamageInputs in = inputs;
  if (const WorldObject* slot = world.find(defender.id); slot != nullptr) {
    in.halved = slot->state.flags.half_damage;
  }
  const DamageBreakdown result = resolve_damage(in, constants_);
  const std::int32_t before = defender.health;
  defender.health -= result.final_damage;
  // A building stops at zero and stays: see `dies_at_no_health`.
  const bool mortal = dies_at_no_health(world, world_bound_, defender.id);
  if (!mortal && defender.health < 0) defender.health = 0;
  events_.push_back(CombatEvent{CombatEvent::Kind::strike, now, source, defender.id,
                                result.final_damage, defender.health});
  // Overkill is not counted: the number the original books is the victim's
  // health *delta*, and its new health is floored before the delta is taken.
  const std::int32_t removed = before - (defender.health < 0 ? 0 : defender.health);
  const bool killed = mortal && defender.health <= 0;
  // The id, not the reference, because `enter_dying` fires a hook and a hook
  // can run arbitrary session code before this line is reached again.
  const ObjectId victim = defender.id;
  note_building_damage(world, victim, removed);
  // The victim as it stood when the blow landed, for the score: the kill's
  // hook runs inside `enter_dying`, and `defender` is not safe to read after it.
  const Combatant scored = defender;
  // The victim's running total, for the kill's gain. Its death changes
  // nothing of it in the original; here the death may let its record go.
  const HeroSystem* heroes = killed ? hero_system_of(world) : nullptr;
  const std::int32_t victim_experience = heroes != nullptr ? heroes->experience(victim) : 0;
  if (killed) enter_dying(world, defender, now);
  record_score(world, source, scored, removed, killed);
  // **Experience is paid for a kill, not for a blow** (0x00511550): the gain
  // is drawn after the score and before the killer's hook, and paid after it.
  // `docs/formats/unit-stats.md`, "What a blow pays".
  const std::int32_t gain = killed ? kill_gain(world, source, victim, victim_experience) : 0;
  // The killer's own hook, after the victim's. `0x005119ee` fires it from the
  // block that pays kill experience, on the attacker, with the victim as the
  // second argument -- `UNIT_ON_KILL.VS`'s `Victim`, which its Gambler's Day
  // branch reads the level off.
  //
  // **`hit` only.** The receiver *is* the attacker, so a script's
  // `Obj::Damage(n)` has nobody to run it on: `apply_damage` defaults `source`
  // to `kNoObject` and its one host call site passes nothing. There is
  // deliberately no `source != kNoObject` test here -- `fire_class_hook`'s
  // first guard is a receiver that does not resolve, and `kNoObject` never
  // does, so a second one would be a branch nothing can reach and therefore a
  // claim this engine is not making.
  if (killed) (void)fire_class_hook(world, ClassHook::on_kill, source, victim);
  pay_experience(world, source, gain);
}

std::int32_t CombatSystem::apply_damage(World& world, ObjectId victim, std::int32_t amount,
                                       ObjectId source) {
  Combatant* unit = mutable_find(victim);
  if (unit == nullptr || !unit->alive || unit->action == Action::dying) return 0;
  if (amount < 0) amount = 0;
  const std::int32_t before = unit->health;
  unit->health -= amount;
  const bool mortal = dies_at_no_health(world, world_bound_, victim);
  if (!mortal && unit->health < 0) unit->health = 0;
  const std::int32_t removed = before - (unit->health < 0 ? 0 : unit->health);
  note_building_damage(world, victim, removed);
  events_.push_back(
      CombatEvent{CombatEvent::Kind::strike, now_, source, victim, amount, unit->health});
  if (mortal && unit->health <= 0) enter_dying(world, *unit, now_);
  return removed;
}

bool CombatSystem::kill(World& world, ObjectId id) {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr || !unit->alive || unit->action == Action::dying) return false;
  unit->health = 0;
  enter_dying(world, *unit, now_);
  return true;
}

std::int32_t CombatSystem::heal(ObjectId id, std::int32_t amount) {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr || !unit->alive || unit->action == Action::dying) return 0;
  const std::int32_t cap = max_health_of(*unit);
  const std::int32_t before = unit->health;
  unit->health += amount < 0 ? 0 : amount;
  if (cap > 0 && unit->health > cap) unit->health = cap;
  return unit->health - before;
}

std::int32_t CombatSystem::health(ObjectId id) const noexcept {
  const Combatant* unit = find(id);
  return unit == nullptr ? 0 : unit->health;
}

std::int32_t CombatSystem::max_health(ObjectId id) const noexcept {
  const Combatant* unit = find(id);
  return unit == nullptr ? 0 : max_health_of(*unit);
}

std::int32_t CombatSystem::attack_of(const Combatant& unit) const { return unit.stats.attack; }

std::int32_t CombatSystem::armour_of(const Combatant& unit, DamageType type) const {
  switch (type) {
    case DamageType::slash: return unit.stats.armour_slash;
    case DamageType::pierce: return unit.stats.armour_pierce;
    case DamageType::siege:
    case DamageType::none: return 0;
  }
  return 0;
}

std::int32_t CombatSystem::max_health_of(const Combatant& unit) const {
  return unit.stats.max_health;
}

std::int32_t CombatSystem::max_stamina_of(const Combatant& unit) const {
  return unit.stats.max_stamina;
}

std::int32_t CombatSystem::range_of(const Combatant& unit) const { return unit.stats.range; }

StatBlock CombatSystem::class_stats(const Combatant& unit) const {
  const CombatProfile cls = profile(unit.class_index);
  StatBlock out;
  out.max_health = cls.max_health + unit.bonus.max_health;
  out.max_stamina = cls.max_stamina + unit.bonus.max_stamina;
  out.range = cls.range;
  out.attack = cls.damage + unit.bonus.attack;
  out.armour_slash = cls.armor_slash + unit.bonus.armour_slash;
  out.armour_pierce = cls.armor_pierce + unit.bonus.armour_pierce;
  return out;
}

void CombatSystem::store_stats(Combatant& unit, const StatBlock& next) {
  const StatBlock old = unit.stats;
  const std::int32_t health_before = unit.health;
  const std::int32_t stamina_before = unit.stamina;
  unit.stats = next;
  if (old.max_health > 0) {
    unit.health = static_cast<std::int32_t>(static_cast<std::int64_t>(unit.health) *
                                            next.max_health / old.max_health);
  }
  if (old.max_stamina > 0) {
    unit.stamina = static_cast<std::int32_t>(static_cast<std::int64_t>(unit.stamina) *
                                             next.max_stamina / old.max_stamina);
  }
  // Written through only when the rescale moved them: the end of the turn
  // publishes the rest, as it always has.
  if (world_bound_ && world_ != nullptr &&
      (unit.health != health_before || unit.stamina != stamina_before)) {
    (void)world_->set_health(unit.id, unit.health);
    if (ObjectState* state = world_->mutable_state(unit.id); state != nullptr) {
      state->stamina = unit.stamina;
    }
  }
}

void CombatSystem::recalc(ObjectId id) {
  if (world_bound_ && world_ != nullptr) {
    recalc_bonuses(*world_, id);
    return;
  }
  Combatant* unit = mutable_find(id);
  if (unit == nullptr || !unit->alive || unit->action == Action::dying) return;
  store_stats(*unit, class_stats(*unit));
}

bool CombatSystem::add_bonus(ObjectId id, const StatBonus& delta) {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr) return false;
  unit->bonus.attack += delta.attack;
  unit->bonus.armour_slash += delta.armour_slash;
  unit->bonus.armour_pierce += delta.armour_pierce;
  unit->bonus.max_health += delta.max_health;
  unit->bonus.max_stamina += delta.max_stamina;
  // 0x005e0ce0 ends in the rebuild (`vtbl+0xc4`).
  recalc(id);
  return true;
}

bool CombatSystem::remove_bonus(ObjectId id, const StatBonus& delta) {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr) return false;
  const auto down = [](std::int32_t& held, std::int32_t by) {
    held -= by;
    if (held < 0) held = 0;
  };
  down(unit->bonus.attack, delta.attack);
  down(unit->bonus.armour_slash, delta.armour_slash);
  down(unit->bonus.armour_pierce, delta.armour_pierce);
  down(unit->bonus.max_health, delta.max_health);
  down(unit->bonus.max_stamina, delta.max_stamina);
  // 0x005e07f0 ends in the rebuild too.
  recalc(id);
  return true;
}

bool CombatSystem::is_alive(ObjectId id) const noexcept {
  const Combatant* unit = find(id);
  return unit != nullptr && unit->alive && unit->action != Action::dying;
}

// ---------------------------------------------------------------------------
// CombatSystem: the turn
// ---------------------------------------------------------------------------

void CombatSystem::strike(World& world, Combatant& attacker, Combatant& defender,
                          GameTime now) {
  attacker.action = Action::engaging;
  attacker.anim = kAnimEngage;
  attacker.last_attack_time = now;
  ++attacker.attacks;
  show_swing(world, attacker, defender, now);

  // The defender's *hero* is stamped here, on the swing rather than on the
  // landing -- which is where the original does it: 0x005d5faa calls the
  // target's `vtbl + 0xa4` immediately after the attack animation starts, and
  // for a ranged attacker that is before the arrow exists. See
  // `record_army_attacked`, and `HeroRecord::army_attacked_at` for what the
  // three shipped readers do with it.
  //
  // **Not in `apply_damage`.** That is also `Obj::Damage(n)`'s path, and a
  // script dealing damage out of nowhere calls no virtual on its victim in the
  // original, so it must not move a hero's clock.
  record_army_attacked(world, defender.id, now);
  // And the victim's squad, which the original stamps in the same call and one
  // line earlier (0x005dc214). Same hook for the same reason.
  record_squad_attacked(world, defender.id, attacker.id, now);

  const CombatProfile mine = profile(attacker.class_index);
  if (mine.is_ranged()) {
    launch(world, attacker, defender, now);
    return;
  }

  hit(world, defender, inputs_for(attacker, defender), attacker.id, now);
}

void CombatSystem::launch(World& world, Combatant& attacker, const Combatant& defender,
                          GameTime now) {
  const CombatProfile mine = profile(attacker.class_index);
  Projectile shot;
  shot.shooter = attacker.id;
  shot.target = defender.id;
  shot.class_index = mine.projectile;
  shot.origin = attacker.position;
  shot.aim = defender.position;
  shot.position = attacker.position;
  shot.launched = now;

  // A shot is an object: it takes a handle in the same monotonic space as a
  // unit, which is what the dumps require of anything the hash can see.
  shot.id = world.spawn(NativeClass::catapult_shot, nullptr);

  const DamageInputs in = inputs_for(attacker, defender);
  shot.attack = in.attack;
  shot.type = in.type;
  shot.attacker_level = in.attacker_level;
  shot.splash_radius = mine.splash_radius;
  shot.ignores_armour = in.ignores_armour;

  // Flight time from the ground distance at an invented constant speed. The
  // `shot_tan` / `shot_height` arc the data declares is not modelled: nothing
  // says what its units are.
  const std::int64_t distance = isqrt(distance_squared(attacker.position, defender.position));
  GameTime flight = distance * 1000 / kProjectileSpeed;
  if (flight < 1) flight = 1;
  shot.impact = now + flight;

  shots_.push_back(shot);
  events_.push_back(
      CombatEvent{CombatEvent::Kind::launch, now, attacker.id, defender.id, shot.attack, 0});
}

void CombatSystem::land(World& world, Projectile& shot, GameTime now) {
  shot.position = shot.aim;
  events_.push_back(
      CombatEvent{CombatEvent::Kind::impact, now, shot.shooter, shot.target, shot.attack, 0});

  // The shooter is re-found at every use, and the splash walks ids rather than
  // `units_`: each `hit` can kill, and a kill runs hooks to completion.
  const auto shooter_now = [this, &shot]() { return find(shot.shooter); };

  if (shot.ground) {
    land_on_ground(world, shot, now);
    world.despawn(shot.id);
    shot.id = kNoObject;
    return;
  }

  Combatant* target = mutable_find(shot.target);

  const auto apply = [&](Combatant& victim) {
    const Combatant* shooter = shooter_now();
    DamageInputs in;
    in.attack = shot.attack;
    in.type = shot.type;
    in.armour = armour_of(victim, shot.type);
    in.attacker_level = shot.attacker_level;
    in.defender_level = effective_level(victim);
    in.counter_percent =
        shooter == nullptr
            ? 100
            : counters_.damage_percent(shooter->class_index, victim.class_index, counter_mode_);
    in.ignores_armour = shot.ignores_armour;
    hit(world, victim, in, shot.shooter, now);
  };

  if (target != nullptr && target->alive && target->action != Action::dying) apply(*target);

  if (shot.splash_radius > 0) {
    const std::int64_t r2 = squared(static_cast<std::int64_t>(shot.splash_radius));
    std::vector<ObjectId> around;
    around.reserve(units_.size());
    for (const Combatant& other : units_) around.push_back(other.id);
    for (const ObjectId other_id : around) {
      Combatant* other = mutable_find(other_id);
      if (other == nullptr || other->id == shot.target) continue;
      if (!other->alive || other->action == Action::dying) continue;
      const Combatant* shooter = shooter_now();
      if (shooter != nullptr && !is_enemy(*shooter, *other)) continue;
      if (distance_squared(other->position, shot.aim) > r2) continue;
      apply(*other);
    }
  }

  // The shot leaves the world the moment it lands.
  world.despawn(shot.id);
  shot.id = kNoObject;
}

/// A ground shot's impact, 0x004e4520 -> 0x005ac640 -> 0x005a9770 ->
/// 0x005a8e10 (`docs/formats/siege-fire.md`, "A catapult's shot").
///
/// Every combatant -- every unit and building -- whose centre is inside the
/// box and the circle of `splash_radius + 512` about the point, **whoever owns
/// it**, takes, where its surface distance `d` (centre distance less its
/// class's `radius`) is under `R = splash_radius`: the class damage `D` for
/// `d < 1`, else `D * (R - d)^2 / R^2`, times the counter percent of the
/// shooter's class against its own, over 100, applied as `Obj::Damage`
/// applies it -- no armour, no level, no experience, nobody credited. The
/// original walks its grid cell by cell; this walks ids ascending, which only
/// orders the deaths' hooks among themselves.
void CombatSystem::land_on_ground(World& world, const Projectile& shot, GameTime now) {
  const std::int64_t radius = shot.splash_radius;
  if (radius <= 0) return;
  const std::int64_t reach = radius + 512;
  std::vector<ObjectId> around;
  around.reserve(units_.size());
  for (const Combatant& other : units_) around.push_back(other.id);
  for (const ObjectId id : around) {
    const Combatant* victim = find(id);
    if (victim == nullptr || !victim->alive || victim->action == Action::dying) continue;
    if (victim->position == kHeldPosition) continue;
    const std::int64_t dx = static_cast<std::int64_t>(victim->position.x) - shot.aim.x;
    const std::int64_t dy = static_cast<std::int64_t>(victim->position.y) - shot.aim.y;
    if (dx < -reach || dx > reach || dy < -reach || dy > reach) continue;
    const std::int64_t d2 = dx * dx + dy * dy;
    if (d2 > reach * reach) continue;
    const std::int64_t surface = isqrt(d2) - profile(victim->class_index).radius;
    if (surface >= radius) continue;
    const std::int64_t base = surface < 1 ? shot.attack
                                          : shot.attack * (radius - surface) * (radius - surface) /
                                                (radius * radius);
    const std::int64_t percent =
        counters_.damage_percent(shot.shooter_class, victim->class_index, counter_mode_);
    const std::int64_t blow = percent * base / 100;
    now_ = now;
    (void)apply_damage(world, id, static_cast<std::int32_t>(blow < 0 ? 0 : blow), shot.shooter);
  }
}

bool CombatSystem::valid_target(const Combatant& self, const Combatant& other) const {
  if (!other.alive || other.action == Action::dying || other.health <= 0) return false;
  if (other.position == kHeldPosition) return false;
  if (!is_enemy(self, other)) return false;
  if (!profile(other.class_index).can_be_attacked) return false;
  return unit_can_attack(self, other);
}

bool CombatSystem::launch_at_point(World& world, ObjectId catapult, Point from, Point at,
                                   GameTime release) {
  const Combatant* shooter = find(catapult);
  if (shooter == nullptr) return false;
  const CombatProfile mine = profile(shooter->class_index);
  Projectile shot;
  shot.shooter = catapult;
  shot.target = kNoObject;
  shot.class_index = mine.projectile;
  shot.origin = from;
  shot.aim = at;
  shot.position = from;
  shot.launched = release;
  shot.id = world.spawn(NativeClass::catapult_shot, nullptr);
  // The class's numbers, `[class+0x2a0]` and `[class+0x664]`, which is what
  // 0x005a8e10 reads -- not the object's own attack.
  shot.attack = mine.damage;
  shot.type = mine.damage_type;
  shot.splash_radius = mine.splash_radius;
  shot.ground = true;
  shot.shooter_class = shooter->class_index;
  // 0x004e47c0 measures the track from the launch point moved `shot_height`
  // south -- the ground under a point drawn that high -- to `at`, and
  // 0x004e4650 walks it ten units a 100 ms message, the first due 1 ms after
  // the shot is placed. The original's square root is a float's; it differs
  // from `isqrt` only where it rounds up to a whole multiple of ten, which
  // needs a distance within a ten-thousandth of one.
  const Point ground{from.x, from.y + mine.shot_height};
  shot.impact = release + catapult_shot_flight(isqrt(distance_squared(ground, at)));
  shots_.push_back(shot);
  events_.push_back(CombatEvent{CombatEvent::Kind::launch, release, catapult, kNoObject,
                                shot.attack, 0});
  return true;
}

bool CombatSystem::fire_arrow(World& world, ObjectId tower, ObjectId target, GameTime now) {
  Combatant* shooter = mutable_find(tower);
  const Combatant* victim = find(target);
  if (shooter == nullptr || victim == nullptr) return false;
  shooter->last_attack_time = now;
  ++shooter->attacks;
  // 0x0041e6c0: the victim's squad hears it was attacked, by the tower. No
  // army stamp: `Building::Attack` calls no virtual on its target.
  record_squad_attacked(world, target, tower, now);
  launch(world, *shooter, *victim, now);
  return true;
}

namespace {

/// A siege engine its crew has not finished assembling, which takes no part
/// in the fight: playtest #19, where an engine at one point of health shot at
/// whatever came into range while its builders were still walking to it, and
/// the swing replaced its construction stage with the finished engine firing.
///
/// **Inferred, from where a catapult's shots come from.** In `gbr.exe` an
/// engine fires through `Catapult::Attack` (0x004e2d10), and its callers are
/// `CATAPULT_AUTOFIRE.VS` and `CATAPULT_ATTACK.VS`; the first is queued by
/// `CATAPULT_IDLE.VS` only after `SetBuilt`, and the second is a player's or
/// the AI's order. This engine's acquisition (`act`) is its own model of the
/// first, so it is held to the same gate: `Catapult::IsBuilt`, the flag at
/// `[cat+0x208]`. An ordered attack on an unbuilt engine is not reproduced
/// either way -- its verifier reads no built flag, and nothing here orders one.
[[nodiscard]] bool unbuilt_engine(const World& world, ObjectId id) noexcept {
  const WorldObject* slot = world.find(id);
  return slot != nullptr && slot->object != nullptr &&
         slot->object->is_a(NativeClass::catapult) && !slot->state.flags.built;
}

/// **A building fires only when a script makes it** (`docs/formats/siege-fire.md`).
/// In `gbr.exe` a catapult's every shot is a `Catapult::Attack(point)` call
/// from its autofire or attack script and a tower's a `Building::Attack` call
/// from its guard script, each with the waits those entry points impose; no
/// building acquires or strikes on a clock of its own. So the acquisition
/// below is a unit's alone, and a building in it only waits. A synthetic roll
/// with no world object behind an id keeps the old behaviour: it has no
/// scripts to fire it.
[[nodiscard]] bool fires_by_script(const World& world, ObjectId id) noexcept {
  const WorldObject* slot = world.find(id);
  return slot != nullptr && slot->state.flags.is_building;
}

}  // namespace

void CombatSystem::act(World& world, Combatant& unit, GameTime now) {
  const CombatProfile mine = profile(unit.class_index);
  if (!mine.can_attack() || unbuilt_engine(world, unit.id) || fires_by_script(world, unit.id)) {
    unit.next_action_time = now + constants_.acquire_interval;
    return;
  }

  // Validate the current target; the dumps show one unresolved `target handle`
  // pointing at a destroyed object, so this has to tolerate a stale one.
  Combatant* target = mutable_find(unit.target);
  if (target != nullptr && (!target->alive || target->action == Action::dying ||
                            target->health <= 0 || target->position == kHeldPosition ||
                            !is_enemy(unit, *target) || !unit_can_attack(unit, *target))) {
    target = nullptr;
    unit.target = kNoObject;
    unit.attacks = 0;
  }
  if (target == nullptr) {
    const ObjectId picked = auto_target(unit.id);
    if (picked != kNoObject) {
      unit.target = picked;
      unit.attacks = 0;
      target = mutable_find(picked);
    }
  }

  if (target == nullptr || !in_attack_range(unit, *target)) {
    // Out of reach: stay engaged if we have a target (movement will close),
    // and look again on the next scan.
    if (target == nullptr && unit.action == Action::engaging) {
      unit.action = Action::idle;
      unit.anim = kAnimIdle;
    }
    settle_swing(world, unit, now);
    unit.next_action_time = now + constants_.acquire_interval;
    return;
  }

  if (unit.cannot_fight) {  // Ceasefire
    settle_swing(world, unit, now);
    unit.next_action_time = now + constants_.acquire_interval;
    return;
  }

  const ObjectId self = unit.id;
  strike(world, unit, *target, now);
  const std::int32_t interval = mine.attack_interval > 0 ? mine.attack_interval : 1;
  // Re-found: a fatal blow ran hooks, and `unit` points into `units_`.
  if (Combatant* again = mutable_find(self); again != nullptr) {
    again->next_action_time = now + interval;
  }
}

void CombatSystem::advance(World& world, const Turn& turn) {
  events_.clear();
  // The fake towers owed to an outpost or an engine created since the last
  // turn, before the roll is reconciled so that they are on it: where this
  // engine delivers the original's message 0x3d (`sim/siege_fire.hpp`).
  if (world_bound_) (void)post_fake_towers(world);
  // **Before the early return, not after it.** Objects appear and disappear
  // during a match, and a system that returned on an empty population would
  // never notice the first one that appeared. See `reconcile`.
  reconcile(world);
  if (units_.empty() && shots_.empty()) return;

  // Everything below is scheduled at an absolute game time, so the result does
  // not depend on where turn boundaries fall. `turn.start` is `gametime + 1`,
  // so the half-open interval to drain is (turn.start - 1, turn.end].
  const GameTime begin = turn.start - 1;
  const GameTime end = turn.end;

  now_ = begin;

  // Take positions and ownership from the world where the world has them.
  // Movement writes `ObjectState::position`; combat only reads it, and writes
  // health back so that every other system sees one number. A combatant with
  // no world object -- every synthetic test -- is left alone.
  if (world_bound_) {
    for (Combatant& unit : units_) {
      if (const ObjectState* state = world.state(unit.id); state != nullptr) {
        unit.position = state->position;
        unit.owner = state->owner;
      }
    }
  }

  // Any combatant that has never acted acts at the first opportunity.
  for (Combatant& unit : units_) {
    if (unit.next_action_time <= begin) unit.next_action_time = begin + 1;
  }

  // Drain in time order. The candidate scan is linear; battles are tens of
  // units and the alternative -- a heap keyed on a mutable time -- would need
  // its own ordering rule to stay deterministic, which is a worse trade.
  for (;;) {
    // Impacts and actions are scanned separately and impacts win an exact tie,
    // so that a shot already in the air lands before the volley that answers
    // it. Within each kind the ascending scan keeps the lowest id, which makes
    // the ordering total and therefore reproducible.
    bool have_shot = false;
    GameTime shot_when = 0;
    std::size_t shot_index = 0;
    for (std::size_t i = 0; i < shots_.size(); ++i) {
      if (shots_[i].id == kNoObject) continue;
      // No lower bound. A script's shot (`Catapult::Attack`, `Building::Attack`)
      // is launched in the script pass, after this drain has run over the same
      // slice of time, and can come due inside it; it lands at the head of the
      // next drain, before anything else, at the time it was due.
      const GameTime t = shots_[i].impact;
      if (t > end) continue;
      if (!have_shot || t < shot_when) {
        have_shot = true;
        shot_when = t;
        shot_index = i;
      }
    }

    bool have_unit = false;
    GameTime unit_when = 0;
    std::size_t unit_index = 0;
    for (std::size_t i = 0; i < units_.size(); ++i) {
      const GameTime t = units_[i].next_action_time;
      if (!units_[i].alive || t <= begin || t > end) continue;
      if (!have_unit || t < unit_when) {
        have_unit = true;
        unit_when = t;
        unit_index = i;
      }
    }

    if (!have_shot && !have_unit) break;

    if (have_shot && (!have_unit || shot_when <= unit_when)) {
      now_ = shot_when;
      land(world, shots_[shot_index], shot_when);
      continue;
    }

    now_ = unit_when;
    if (units_[unit_index].action == Action::dying) {
      // The dying state has duration -- 15 objects sit in it across the dumps
      // -- and its end is the object leaving the world.
      const ObjectId id = units_[unit_index].id;
      // The erase that ends a corpse detaches it, as 0x005db230 does; after a
      // death that already detached it, this finds nothing. It is what reaches
      // a unit that died in a holder, which `HeroSystem::on_death` leaves to
      // its erase.
      if (HeroSystem* heroes = hero_system_of(world); heroes != nullptr) {
        heroes->on_erase(world, id);
      }
      if (world_bound_) world.despawn(id);
      remove(id);
      continue;
    }
    const ObjectId acting = units_[unit_index].id;
    act(world, units_[unit_index], unit_when);
    // An action that left its next time in the past would spin. By id, not by
    // index, for the reason `act` re-finds its unit.
    if (Combatant* acted = mutable_find(acting);
        acted != nullptr && acted->next_action_time <= unit_when) {
      acted->next_action_time = unit_when + 1;
    }
  }

  // Interpolate every shot still in the air to the end of the turn, so that a
  // renderer reading the system between turns sees it move.
  for (Projectile& shot : shots_) {
    if (shot.id == kNoObject) continue;
    const GameTime span = shot.impact - shot.launched;
    const GameTime done = end - shot.launched;
    if (span <= 0 || done >= span) {
      shot.position = shot.aim;
      continue;
    }
    if (done <= 0) {
      shot.position = shot.origin;
      continue;
    }
    shot.position.x = shot.origin.x +
                      static_cast<std::int32_t>((static_cast<std::int64_t>(shot.aim.x) -
                                                 shot.origin.x) *
                                                done / span);
    shot.position.y = shot.origin.y +
                      static_cast<std::int32_t>((static_cast<std::int64_t>(shot.aim.y) -
                                                 shot.origin.y) *
                                                done / span);
  }

  shots_.erase(std::remove_if(shots_.begin(), shots_.end(),
                              [](const Projectile& s) { return s.id == kNoObject; }),
               shots_.end());

  if (world_bound_) {
    for (const Combatant& unit : units_) {
      // Health through `set_health` rather than through the state directly:
      // that is where a building's damage tier is recomputed, and a wall the
      // catapults have been working on all turn has to come out of this loop
      // in the tier its health puts it in.
      (void)world.set_health(unit.id, unit.health);
      if (ObjectState* state = world.mutable_state(unit.id); state != nullptr) {
        state->stamina = unit.stamina;
      }
    }
  }
}

void CombatSystem::hash(std::uint64_t& accumulator) const {
  const auto fold = [&accumulator](std::uint64_t value) {
    for (int byte = 0; byte < 8; ++byte) {
      accumulator ^= (value >> (byte * 8)) & 0xFF;
      accumulator *= 0x100000001B3ull;
    }
  };
  // Research writes this mid-game, so it is state and it moves the hash. The
  // difficulty addends beside it are match setup and are not folded.
  for (const auto& [player, percent] : experience_modifiers_) {
    fold(player);
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(percent)));
  }
  // Fields the original hashes, in the dumps' own order. Not the profile cache
  // (load-time state) and not the event log (a report, not state).
  for (const Combatant& unit : units_) {
    fold(unit.id);
    fold(static_cast<std::uint64_t>(unit.class_index));
    fold(unit.owner);
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(unit.position.x)));
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(unit.position.y)));
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(unit.health)));
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(unit.stamina)));
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(unit.level)));
    fold(static_cast<std::uint64_t>(unit.last_attack_time));
    fold(unit.target);
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(unit.attacks)));
    fold(unit.unit_flags);
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(unit.danger)));
    fold(static_cast<std::uint64_t>(unit.danger_time));
    fold(static_cast<std::uint64_t>(unit.action));
    fold(static_cast<std::uint64_t>(unit.next_action_time));
    // Not in the dumps, and state all the same: the stored stats are only as
    // fresh as their last rebuild, so two peers that rebuilt at different
    // moments disagree here first.
    for (const std::int32_t stat : {unit.stats.max_health, unit.stats.max_stamina,
                                    unit.stats.range, unit.stats.attack,
                                    unit.stats.armour_slash, unit.stats.armour_pierce}) {
      fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(stat)));
    }
  }
  for (const Projectile& shot : shots_) {
    fold(shot.id);
    fold(shot.shooter);
    fold(shot.target);
    fold(static_cast<std::uint64_t>(shot.impact));
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(shot.attack)));
  }
}

// ---------------------------------------------------------------------------
// host functions
// ---------------------------------------------------------------------------

CombatSystem* combat_system_of(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "combat") {
      return static_cast<CombatSystem*>(system);
    }
  }
  return nullptr;
}

Point catapult_shot_origin(const World& world, ObjectId catapult, Point at) noexcept {
  const Point position = world.resolve_position(catapult);
  const WorldObject* slot = world.find(catapult);
  if (slot == nullptr || slot->object == nullptr || slot->object->entity == nullptr) {
    return position;
  }
  const Entity& entity = *slot->object->entity;
  // 0x0053cee0(3, buffer, 64) -> 0x005fd290: the type-3 points' raw offsets
  // in table order. They are added to the position as they stand, with no
  // screen round trip.
  const Point heading{at.x - position.x, at.y - position.y};
  const std::uint32_t column = facing_column(heading, entity.variations());
  std::uint32_t seen = 0;
  for (const EntityPoint& point : entity.points()) {
    if (point.type != 3) continue;
    if (seen++ == column) return Point{position.x + point.x, position.y + point.y};
  }
  return position;
}

namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::Value;

constexpr const char* kNoWorld = "combat: no World behind CallContext::user";
constexpr const char* kNoSystem = "combat: no combat system registered with the world";

/// The combat system behind a call, or null.
///
/// `CallContext::user` is a `HostContext*` (sim/host_context.hpp) and the system
/// is found by name over `World::systems()`. Null is a real case, not a
/// defensive check: `engine/tests` runs every entry point with `user ==
/// nullptr` to prove it refuses rather than dereferences.
[[nodiscard]] CombatSystem* system_of(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  return world == nullptr ? nullptr : combat_system_of(*world);
}

/// Which half of `system_of` failed. Only called once it has returned null, so
/// the trap names the missing piece rather than "no context".
[[nodiscard]] const char* no_system_error(CallContext& ctx) noexcept {
  return world_of(ctx) == nullptr ? kNoWorld : kNoSystem;
}

[[nodiscard]] ObjectId id_of(const Value& value) noexcept {
  if (value.is_object()) return value.as_object().id;
  if (value.is_integer()) return static_cast<ObjectId>(value.as_integer());
  return kNoObject;
}

/// An object handle carrying the type id the embedder chose. `object_type` is
/// the one thing in `HostContext` that is not derivable from the world.
[[nodiscard]] Value ref_to(CallContext& ctx, ObjectId id) {
  if (id == kNoObject) return Value::object(script::ObjectRef{});
  const HostContext* context = host_context_of(ctx);
  return Value::object(context == nullptr ? script::TypeId{1} : context->object_type, id);
}

/// A read on the receiver that yields an integer.
template <std::int32_t (*Read)(const CombatSystem&, ObjectId)>
HostOutcome read_int(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  return HostOutcome::ok_with(Value::integer(Read(*combat, id_of(ctx.arg(0)))));
}

std::int32_t read_max_stamina(const CombatSystem& c, ObjectId id) {
  const Combatant* u = c.find(id);
  return u == nullptr ? 0 : c.max_stamina_of(*u);
}
std::int32_t read_attack(const CombatSystem& c, ObjectId id) {
  const Combatant* u = c.find(id);
  return u == nullptr ? 0 : c.attack_of(*u);
}
// `damage/0` and `armor_slash/0` have **no call site in any `.vs` file**: they
// are reached only from the inline `<valueN script>` of the class XML --
// `return .AsUnit.damage;`, `return .AsUnit.armor_slash;` -- which the info
// bar evaluates over the selection (`sim/infobar.hpp`). `Obj::damage` is the
// same function as `Obj::attack` over `[obj+0xdc]`, the note on `attack_of`
// says so, and `armor_slash` is `[obj+0xe4]`, the number the `slash` armour
// reads. `imcheck surface` counts neither, for the reason it counts none of
// the `Sel*` family.
std::int32_t read_armor_slash(const CombatSystem& c, ObjectId id) {
  const Combatant* u = c.find(id);
  return u == nullptr ? 0 : c.armour_of(*u, DamageType::slash);
}


/// Publish a combatant's health and stamina onto its world object.
///
/// `CombatSystem::advance` already does exactly this at the end of every turn
/// under `world_bound`, so that "every other system sees one number". Doing it
/// here too is that same write, not a new rule: `health/0` and `stamina/0`
/// belong to the object model now (see the note at the foot of this file), and
/// without this a script reading `.health` straight after `.Damage` would see
/// last turn's number until the turn ended. An unbound system -- every
/// synthetic test, and any embedder whose combatant ids are not world object
/// ids -- is left alone, which is the guard `advance` uses for the same reason.
void sync_to_world(CallContext& ctx, const CombatSystem& combat, ObjectId id) {
  if (!combat.world_bound()) return;
  const Combatant* unit = combat.find(id);
  if (unit == nullptr) return;
  World* world = world_of(ctx);
  if (world == nullptr) return;
  // The same re-tiering `advance` relies on; see the note there. A script's
  // `bld.Damage(500)` must leave the building in the tier `IsBroken` will read
  // on the next line, not on the next turn.
  (void)world->set_health(id, unit->health);
  if (ObjectState* state = world->mutable_state(id); state != nullptr) {
    state->stamina = unit->stamina;
  }
}

/// A world object the combat system does not hold, or null.
///
/// `gbr.exe` gives every object the same health and stamina fields and the
/// same generic virtuals: `+0x98` and `+0x9c` store health and stamina raw
/// (0x005a7a00, 0x005a7a30), `+0xa8` is the damage routine (0x005a7720),
/// `+0xb0` the base death (0x005b16f0). So `Mist` and `CoverOfMercy`
/// (`CVXScriptObj`, `CVXAreaEffect`) take `SetStamina`, `Heal` and `Damage`
/// like a soldier does: `DRUID_MIST.VS` gives its mist `SetStamina(10)`,
/// `MIST_IDLE.VS` runs while that stamina lasts, and `SUMMONING_BEHAVIOR.VS`
/// counts it down and ends the object with `Damage(health)`. This engine
/// registers neither class as a combatant (`is_combat_object`), so the
/// mutators below fall back to the world object's own `ObjectState` for it.
[[nodiscard]] WorldObject* script_object(CallContext& ctx, const CombatSystem& combat,
                                         ObjectId id) {
  if (combat.find(id) != nullptr) return nullptr;
  World* world = world_of(ctx);
  if (world == nullptr) return nullptr;
  WorldObject* slot = world->find(id);
  if (slot == nullptr || slot->internal != InternalKind::none) return nullptr;
  return slot;
}

/// The unit health store, `vtbl+0x98` on a unit (0x005d3a80): when the value
/// changes and reaches the unit's maximum, the disease bit (`[unit+0x194] &
/// 2`) is cleared and its effect handle dropped. `Heal` and `SetHealth`
/// reach it; the bit is `ObjectFlags::diseased`.
void cure_at_full_health(CallContext& ctx, const CombatSystem& combat, ObjectId id,
                         std::int32_t before) {
  const Combatant* unit = combat.find(id);
  World* world = world_of(ctx);
  if (unit == nullptr || world == nullptr || unit->health == before) return;
  if (unit->health < combat.max_health_of(*unit)) return;
  if (ObjectState* state = world->mutable_state(id); state != nullptr) {
    state->flags.diseased = false;
  }
}

/// `Damage(n)` -- 60 sites. `Obj::Damage` (0x005ab5d0) answers a bool.
///
/// An invalid receiver logs and answers **true**. A negative `n` logs
/// ("Negative damage in function 'Obj::Damage'") and answers false, changing
/// nothing -- a log line, not a script error. A building runs its fire points
/// first (0x004e0fd0, visual). Then the damage routine (0x005a7720): an
/// object already dead answers true and is left alone; otherwise `n` is
/// clamped to the health left, the new health stored, and an object now dead
/// goes through its death virtual; the answer is whether it is dead now. No
/// attacker is credited. The ambient-civilian immunity (`[obj+0x2c] &
/// 0x4000000`, answer 0) is not modelled: the engine carries no such bit and
/// no shipped script damages a villager.
HostOutcome host_damage(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const std::int32_t amount = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  const ObjectId victim = id_of(ctx.arg(0));
  World& world = *world_of(ctx);
  if (world.find(victim) == nullptr && combat->find(victim) == nullptr) {
    return HostOutcome::ok_with(Value::boolean(true));
  }
  if (amount < 0) return HostOutcome::ok_with(Value::boolean(false));
  if (WorldObject* slot = script_object(ctx, *combat, victim); slot != nullptr) {
    if (object_is_dead(*slot)) return HostOutcome::ok_with(Value::boolean(true));
    const std::int32_t left = slot->state.health > amount ? slot->state.health - amount : 0;
    (void)world.set_health(victim, left);
    if (left > 0) return HostOutcome::ok_with(Value::boolean(false));
    // The base death, 0x005b16f0: health 0, `ondie`, off the map, deleted.
    erase_object(ctx, world, victim);
    return HostOutcome::ok_with(Value::boolean(true));
  }
  combat->apply_damage(world, victim, amount);
  sync_to_world(ctx, *combat, victim);
  const Combatant* after = combat->find(victim);
  const bool dead = after == nullptr || !combat->is_alive(victim) ||
                    after->action == Action::dying || after->health <= 0;
  return HostOutcome::ok_with(Value::boolean(dead));
}

/// `Heal(n)` -- 45 sites. `Obj::Heal` (0x005ab690): an invalid receiver logs;
/// a negative `n` logs and changes nothing; otherwise health becomes
/// `min(health + n, maxhealth)` through the health store. The original has no
/// dead check -- it writes a corpse's health too; this engine keeps
/// `CombatSystem::heal`'s refusal of a dying unit, whose health is the dying
/// state's own business here.
HostOutcome host_heal(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const std::int32_t amount = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  if (amount < 0) return HostOutcome::ok_void();
  const ObjectId patient = id_of(ctx.arg(0));
  if (WorldObject* slot = script_object(ctx, *combat, patient); slot != nullptr) {
    World& world = *world_of(ctx);
    const std::int64_t raised = static_cast<std::int64_t>(slot->state.health) + amount;
    const std::int32_t cap = object_max_health(world, *slot);
    (void)world.set_health(patient, static_cast<std::int32_t>(raised > cap ? cap : raised));
    return HostOutcome::ok_void();
  }
  const std::int32_t before = combat->health(patient);
  combat->heal(patient, amount);
  cure_at_full_health(ctx, *combat, patient, before);
  sync_to_world(ctx, *combat, patient);
  return HostOutcome::ok_void();
}

/// `SetHealth(n)` -- `Obj::SetHealth` (0x005ab4e0) stores `n` raw through the
/// health store: no clamp, and no death even at 0.
HostOutcome host_set_health(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const ObjectId id = id_of(ctx.arg(0));
  if (!ctx.arg(1).is_integer()) return HostOutcome::ok_void();
  if (WorldObject* slot = script_object(ctx, *combat, id); slot != nullptr) {
    (void)world_of(ctx)->set_health(id, ctx.arg(1).as_integer());
    return HostOutcome::ok_void();
  }
  Combatant* unit = combat->find(id);
  if (unit == nullptr) return HostOutcome::ok_void();
  const std::int32_t before = unit->health;
  unit->health = ctx.arg(1).as_integer();
  cure_at_full_health(ctx, *combat, id, before);
  sync_to_world(ctx, *combat, id);
  return HostOutcome::ok_void();
}

/// `SetStamina(n)` -- `Obj::SetStamina` (0x005ab440) stores `n` raw through
/// `vtbl+0x9c`: no clamp, no dead check.
HostOutcome host_set_stamina(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const ObjectId id = id_of(ctx.arg(0));
  if (!ctx.arg(1).is_integer()) return HostOutcome::ok_void();
  if (WorldObject* slot = script_object(ctx, *combat, id); slot != nullptr) {
    if (ObjectState* state = world_of(ctx)->mutable_state(id); state != nullptr) {
      state->stamina = ctx.arg(1).as_integer();
    }
    return HostOutcome::ok_void();
  }
  Combatant* unit = combat->find(id);
  if (unit == nullptr) return HostOutcome::ok_void();
  unit->stamina = ctx.arg(1).as_integer();
  sync_to_world(ctx, *combat, unit->id);
  return HostOutcome::ok_void();
}

/// `HealStamina(int)` -- 11 sites, and eight of them are on a **group**.
///
/// `gbr.exe` registers it twice, `Obj::HealStamina` (0x005ab700) and
/// `Query::HealStamina` (0x00579100), and the group form is the dominant one:
/// `T_CalgacusArmy.HealStamina(10)` five times over, `Group("Ghouls")` twice,
/// `Guards.HealStamina(2)` once. A single-receiver implementation would answer
/// eight of the eleven by doing nothing at all, which is why this fans out over
/// `receiver_objects` rather than reading `arg(0)` as one object.
///
/// The body is `Heal`'s stamina sibling, with two guards `SetStamina` does not
/// have:
///
///   * **it adds and then clamps to `maxstamina`** -- `[obj+0xCC]` -- where
///     `Obj::SetStamina` (0x005ab440) is a raw pass-through with no clamp at
///     all;
///   * **a negative amount is a rejected no-op**, complaint and all
///     (*"Negative heal stamina in function 'Obj::HealStamina'"*, formatted
///     into the sink that is a bare `ret`). So `HealStamina(-n)` cannot drain,
///     and the three shipped druids pay for their spell with
///     `SetStamina(.stamina - cost)` on the line above rather than through
///     this.
///
/// The argument is an amount of points and not a target or a percentage: all
/// three pack sites pass `GetConst("RevitalizeAmount")`, and the container
/// sites pass 2 and 10 against a maximum in the hundreds.
HostOutcome host_heal_stamina(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const std::int32_t amount = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  // The single form refuses a negative amount (0x005ab72c); the group form,
  // `Query::HealStamina` (0x00579100), has no sign test but skips its dead
  // members, and refreshes the query first.
  const HostContext* context = host_context_of(ctx);
  const bool single = ctx.arg(0).is_object() && context != nullptr &&
                      ctx.arg(0).as_object().type == context->object_type;
  if (single && amount < 0) return HostOutcome::ok_void();
  for (const ObjectId id : receiver_objects(ctx, *world, ctx.arg(0))) {
    if (single) {
      if (WorldObject* slot = script_object(ctx, *combat, id); slot != nullptr) {
        const std::int32_t cap = class_int(*world, *slot, "maxstamina");
        const std::int64_t raised = static_cast<std::int64_t>(slot->state.stamina) + amount;
        slot->state.stamina = static_cast<std::int32_t>(raised > cap ? cap : raised);
        continue;
      }
    }
    Combatant* unit = combat->find(id);
    if (unit == nullptr) continue;
    if (!single && (!combat->is_alive(id) || unit->action == Action::dying || unit->health <= 0)) {
      continue;
    }
    const std::int32_t max = combat->max_stamina_of(*unit);
    const std::int64_t raised = static_cast<std::int64_t>(unit->stamina) + amount;
    unit->stamina = static_cast<std::int32_t>(raised > max ? max : raised);
    sync_to_world(ctx, *combat, id);
  }
  return HostOutcome::ok_void();
}

/// `Attack(target)` -- **a coroutine, not a call**, which is what
/// `HERO_IDLE.VS`'s `while (.Attack(u)) { ... }` needs and what this returned
/// without for a long time.
///
/// The evidence is the registrar, and it is decisive. `gbr.exe` has two of
/// them: 0x00699bb0 registers an ordinary entry point, and **0x00699eb0
/// registers a suspending one**. Fifty names go through the second, and the
/// list is exactly the family you would predict -- `Sleep`, every one of the
/// sixteen `Wait*`, `Unit::Stop`, `Unit::Idle`, all four `Goto*`,
/// `Hero::FormKeepMoving`, `Conversation::Run` -- with three that this tree
/// had no reason to suspect: `Unit::Attack` (0x005ddb50, registered at
/// 0x005dfaaa), `Building::Attack` (0x004e1750) and `Catapult::Attack`
/// (0x004e3ed6). The bodies confirm it: 0x005ddb50 opens by reading the
/// re-entry flag at `byte [cursor - 1]` and branching on it, which is the
/// two-phase shape `FormKeepMoving` (0x0052eaf0) uses and which only a
/// suspending entry point has.
///
/// **This is what spins in `hero_idle.vs`**, an open thread in the plan since
/// the `FormKeepMoving` correction, and the corpus sweep names the script but
/// not the statement. It is the inner `while (.Attack(u)) { if
/// (!.IsValidTarget(u)) break; if (!.IsVisible) break; }`: with a
/// non-suspending `Attack`, a hero with a live, visible, valid enemy runs that
/// loop until the scheduler's instruction budget is gone, every pass, forever.
/// Nothing else in that file can do it -- `Idle`, `GotoAttack` and
/// `FormKeepMoving` all suspend already, and the outer `while (1)` ends in
/// `if (.Stop(2000)) .Idle(2000);`, whose `Stop` suspends a walking hero and
/// whose `Idle` suspends a standing one.
///
/// **The slice is inferred, and it is the only invented number here.** The
/// original's suspension length is written to the scheduler's wait cell at
/// `[0x00a77eac]`, and `FormKeepMoving` writes its own `ms` argument there;
/// `Attack` takes no such argument, so the duration has to come from
/// somewhere. One strike is what one call means -- the loop is written to
/// re-issue per blow -- so the attacker's own `attack_interval` is used, which
/// is the same number `CombatSystem::advance` paces strikes by. The
/// alternatives, and neither is refuted: a fixed slice, or a suspension until
/// the strike actually lands.
///
/// A refused order does **not** suspend. 0x005ddbd2 pushes 0 and returns with
/// `eax = 0`, which is this protocol's "done"; only the accepted path returns
/// 1. So `while (.Attack(u))` leaves the instant the target stops being
/// attackable, without spending game time on the attempt.
/// `Catapult::Attack(point)` (0x004e2d10), suspending: one throw. Refused --
/// nothing launched, nothing suspended -- for a receiver that does not
/// resolve, a point on the engine itself, or an engine that carries towers
/// (`has_towers`, `[cat+0x22c]`). Otherwise the shot leaves at the attack
/// animation's action moment for the point and the script resumes when the
/// animation ends. `docs/formats/siege-fire.md`, "A catapult's shot", has
/// the state machine this stands for and what is approximated.
HostOutcome catapult_attack(CallContext& ctx, CombatSystem& combat, World& world) {
  const ObjectId catapult = id_of(ctx.arg(0));
  const WorldObject* slot = world.find(catapult);
  if (slot == nullptr) return HostOutcome::ok_void();
  const Point at = unpack_point(ctx.arg(1));
  const Point position = world.resolve_position(catapult);
  if (at == position || slot->state.flags.has_towers) return HostOutcome::ok_void();
  const std::int32_t release = catapult_release_ms(world, catapult);
  const Point from = catapult_shot_origin(world, catapult, at);
  if (!combat.launch_at_point(world, catapult, from, at, ctx.now + release)) {
    return HostOutcome::ok_void();
  }
  const std::int32_t throw_ms = catapult_throw_ms(world, catapult);
  HostOutcome out;
  out.status = script::HostStatus::suspend;
  out.suspend_for = throw_ms > 0 ? throw_ms : 1;
  return out;
}

/// `Building::Attack(obj)` (0x004df650), suspending: one arrow, or a wait
/// and another try. In the original's order: receiver and target resolve;
/// the settlement's fire rate (`settlement_fire_rate`) is not 0; the tower
/// may hit the target (`valid_target`) and it is in range (0x005a7800:
/// surface distance within `range`, and past `min_range` when that is 1 or
/// more). Any of those failing answers false at once. Then the tower's own
/// `attack_delay` since its last shot, and the settlement's `1000 / rate`
/// since any of its towers' last shot (`last_tower_fire_time`, reset if it
/// lies ahead): while either has time left the call suspends for it and is
/// made again (the original's return 1 with the wait in `[0x00a77eac]`).
/// Otherwise both clocks are stamped, the arrow is loosed and the answer is
/// true with no suspension. See `docs/formats/siege-fire.md`.
HostOutcome building_attack(CallContext& ctx, CombatSystem& combat, World& world) {
  const ObjectId tower = id_of(ctx.arg(0));
  const ObjectId target = id_of(ctx.arg(1));
  const WorldObject* slot = world.find(tower);
  const Combatant* self = combat.find(tower);
  const Combatant* other = combat.find(target);
  const auto no = [] { return HostOutcome::ok_with(Value::boolean(false)); };
  if (slot == nullptr || self == nullptr || world.find(target) == nullptr || other == nullptr) {
    return no();
  }
  Settlement* set = siege_settlement_of(world, tower);
  const std::int32_t rate =
      set == nullptr ? 0 : settlement_fire_rate(world, *set, combat.constants().base_fire_rate);
  if (rate <= 0) return no();
  if (!combat.valid_target(*self, *other)) return no();
  const CombatProfile mine = combat.profile(self->class_index);
  const std::int64_t d =
      isqrt(distance_squared(world.resolve_position(tower), world.resolve_position(target))) -
      mine.radius - combat.profile(other->class_index).radius;
  if ((mine.min_range >= 1 && d < mine.min_range) || d > mine.range) return no();

  const GameTime now = ctx.now;
  const auto wait = [](GameTime ms) {
    HostOutcome out;
    out.status = script::HostStatus::retry;
    out.suspend_for = ms;
    return out;
  };
  const GameTime delay = class_int(world, *slot, "attack_delay");
  const GameTime since_shot = now - self->last_attack_time;
  if (since_shot >= 0 && since_shot < delay) return wait(delay - since_shot);
  if (now < set->last_tower_fire_time) set->last_tower_fire_time = 0;
  const GameTime cadence = 1000 / rate;
  const GameTime since_any = now - set->last_tower_fire_time;
  if (cadence > since_any) return wait(cadence - since_any);

  set->last_tower_fire_time = now;
  (void)combat.fire_arrow(world, tower, target, now);
  return HostOutcome::ok_with(Value::boolean(true));
}

HostOutcome host_attack(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  // Three bodies behind one name: the receiver's type picks it in the
  // original, the argument and the receiver's class here. Only
  // `Catapult::Attack` takes a point; a building's object form is
  // `Building::Attack`, a unit's `Unit::Attack`.
  if (World* world = world_of(ctx); world != nullptr && combat->world_bound()) {
    if (ctx.count() >= 2 && is_point(ctx.arg(1))) return catapult_attack(ctx, *combat, *world);
    const WorldObject* slot = world->find(id_of(ctx.arg(0)));
    if (slot != nullptr && slot->state.flags.is_building) {
      return building_attack(ctx, *combat, *world);
    }
  }
  const ObjectId attacker = id_of(ctx.arg(0));
  const bool ok = combat->order_attack(attacker, id_of(ctx.arg(1)));
  if (!ok) return HostOutcome::ok_with(Value::boolean(false));

  HostOutcome out;
  out.status = script::HostStatus::suspend;
  out.value = Value::boolean(true);
  const Combatant* a = combat->find(attacker);
  const std::int32_t interval =
      a == nullptr ? kDefaultAttackInterval : combat->profile(a->class_index).attack_interval;
  // Never zero: a zero slice is a suspension that resumes in the same pass,
  // which is the busy loop this exists to stop.
  out.suspend_for = interval > 0 ? interval : 1;
  return out;
}

HostOutcome host_can_attack(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const Combatant* a = combat->find(id_of(ctx.arg(0)));
  const Combatant* d = combat->find(id_of(ctx.arg(1)));
  if (a == nullptr || d == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  // `Obj::CanAttack` (0x005ab060) is the receiver's `CanAttack` virtual and
  // the range test; the virtual is `unit_can_attack`, and the range half is
  // left out for the reason given there -- it must refuse exactly what
  // `Attack` refuses, or `RAM_ATTACK.VS`'s `if (!.CanAttack(t)) break;
  // .Attack(t);` loop spins on a refusal that never suspends.
  //
  // Before either, it refuses a dead target and then a dead receiver, each
  // through the dead virtual (`vtbl+0x50`), with a log line and `false`.
  // A dying combatant is dead to it: its health is already at zero.
  const auto dead = [&](const Combatant& c) {
    return !combat->is_alive(c.id) || c.action == Action::dying;
  };
  if (dead(*d) || dead(*a)) return HostOutcome::ok_with(Value::boolean(false));
  const bool ok = combat->profile(a->class_index).can_attack() &&
                  combat->profile(d->class_index).can_be_attacked && combat->is_enemy(*a, *d) &&
                  combat->unit_can_attack(*a, *d);
  return HostOutcome::ok_with(Value::boolean(ok));
}

HostOutcome host_is_valid_target(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const Combatant* a = combat->find(id_of(ctx.arg(0)));
  const Combatant* d = combat->find(id_of(ctx.arg(1)));
  if (a == nullptr || d == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  const CombatProfile theirs = combat->profile(d->class_index);
  // `Obj::IsValidTarget` (0x005aafd0) is nothing but the receiver's
  // `CanAttack` virtual, so a swordsman's `MELEE_ATTACK.VS` and
  // `UNIT_AI_ATTACK_GATE.VS` leave at their first test of a building. An
  // invalid target answers `false` without a log line, an invalid receiver
  // with one. It does **not** read the class's `target_factor`: a factor of
  // -1 keeps an animal out of every target sweep (0x005db650) and out of
  // `EnemyInRange` (0x004dbea0), but a script that names one gets the
  // virtual's answer.
  const bool ok = combat->is_enemy(*a, *d) && combat->is_alive(d->id) && d->health > 0 &&
                  theirs.can_be_attacked && combat->unit_can_attack(*a, *d);
  return HostOutcome::ok_with(Value::boolean(ok));
}

HostOutcome host_is_enemy(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const Combatant* a = combat->find(id_of(ctx.arg(0)));
  const Combatant* d = combat->find(id_of(ctx.arg(1)));
  const bool ok = a != nullptr && d != nullptr && combat->is_enemy(*a, *d);
  return HostOutcome::ok_with(Value::boolean(ok));
}

/// `InRange(target)`, measured where the two stand now. A combatant's
/// `position` is the copy taken from the world at the start of the turn, and a
/// script runs after movement has stepped its units, so a target that walked
/// during the turn was measured where it had been. `GotoAttack` measures the
/// same band from the world's live positions (`run_goto`), so the two could
/// disagree inside one script run: on Balcans an `EPRIEST_IDLE.VS` priest was
/// told it had arrived by `GotoAttack` and that it was out of range by
/// `InRange`, and looped between them until its instruction budget ran out.
/// The original keeps one position per object, which both read.
/// `range/0` -- `Obj::range` (0x005adde0): the stored `[obj+0xd4]`, 0 for a
/// receiver that resolves nothing. A combatant answers its stored range
/// (`range_of`, which a formation's `BonusRange` moves); any other object
/// answers the class's `range`, which is what its constructor stored and what
/// nothing ever rebuilds -- where this used to answer 0.
HostOutcome host_range(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const ObjectId id = id_of(ctx.arg(0));
  if (const Combatant* unit = combat->find(id); unit != nullptr) {
    return HostOutcome::ok_with(Value::integer(combat->range_of(*unit)));
  }
  World* world = world_of(ctx);
  const WorldObject* slot = world != nullptr ? world->find(id) : nullptr;
  if (slot == nullptr || slot->internal != InternalKind::none) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  return HostOutcome::ok_with(Value::integer(class_int(*world, *slot, "range")));
}

HostOutcome host_in_range(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  // `Catapult::InRange(point)` (0x004e3230), the only point form.
  if (ctx.count() >= 2 && is_point(ctx.arg(1))) {
    World* world = world_of(ctx);
    const bool in = world != nullptr && catapult_point_in_range(*world, *combat, id_of(ctx.arg(0)),
                                                                unpack_point(ctx.arg(1)));
    return HostOutcome::ok_with(Value::boolean(in));
  }
  const Combatant* a = combat->find(id_of(ctx.arg(0)));
  const Combatant* d = combat->find(id_of(ctx.arg(1)));
  if (a == nullptr || d == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  // `Unit::InRange` (0x005d72e0) answers `false` for a receiver inside a
  // holder (`[unit+0x154] != 0xffff`) before it measures anything.
  if (World* world = world_of(ctx); world != nullptr) {
    if (const ObjectState* state = world->state(a->id); state != nullptr && state->is_held()) {
      return HostOutcome::ok_with(Value::boolean(false));
    }
  }
  Combatant attacker = *a;
  Combatant defender = *d;
  if (World* world = world_of(ctx); world != nullptr && combat->world_bound()) {
    if (world->state(attacker.id) != nullptr) attacker.position = world->resolve_position(attacker.id);
    if (world->state(defender.id) != nullptr) defender.position = world->resolve_position(defender.id);
  }
  return HostOutcome::ok_with(Value::boolean(combat->in_attack_range(attacker, defender)));
}

/// `SetExperienceModifier(player, percent)` -- 1 site, and one third of what
/// `ONFINISH_RESEARCH.VS` and its 146 call sites were blocked on.
///
/// 0x005d2fb0 pops the modifier and then the player, decrements the player
/// **unconditionally**, range-checks it against 16, and writes the modifier
/// into the player record at `+0x84`. So the player is 1-based like every
/// other per-player argument here, and a number outside 1..16 writes nothing --
/// the original's own out-of-range arm stores through a null pointer at
/// 0x005d3006, which is unreachable because the identical comparison two
/// instructions earlier has already branched away.
///
/// The one caller completes the *Battle tactics* research with
/// `GetConst("BattleTacticsExpModifier")`, and `CombatConstants` has carried
/// that constant, and a note saying this entry point sets it, since the
/// experience formula was written.
HostOutcome host_set_experience_modifier(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const PlayerId player = ctx.arg(0).is_integer() ? player_from_script(ctx.arg(0).as_integer())
                                                  : kNoPlayer;
  if (player == kNoPlayer) return HostOutcome::ok_void();
  combat->set_player_experience_modifier(
      player, ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0);
  return HostOutcome::ok_void();
}

/// `u.RecalcBonuses()` -- 1 site: `ONFINISH_RESEARCH.VS` calls it on every
/// unit of the settlement the moment a research completes, which is how a
/// Statue of Amon-Ra reaches the units already standing.
///
/// 0x005e0380 resolves the receiver -- a miss prints and does nothing -- and
/// calls its `vtbl+0xc4`: on a unit the rebuild 0x005d9df0 (`recalc_bonuses`,
/// `docs/formats/unit-stats.md`), on a building a bare `ret` (0x00686eb0 in
/// every building vtable), which `recalc` matches by rebuilding units only.
HostOutcome host_recalc_bonuses(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  combat->recalc(id_of(ctx.arg(0)));
  return HostOutcome::ok_void();
}

/// `u.AddBonus(attack, slash, pierce, maxhealth, maxstamina)` -- 1 site, and
/// the last name standing between `3_Great_Losses_Egypt`'s `seq2.vs` and its
/// run. That site is `GoodHero.obj.AsUnit().AddBonus(0, -10, 0, 0, 0)`: ten
/// points of slash armour taken *off* the hero the mission is about, which is
/// the mission's handicap and not a reward.
///
/// **The five numbers are five stats, and the entry point reads four of them.**
/// `0x005e0ce0` pops the frame from the top down and the five ints occupy the
/// four bytes each above the receiver's eight, so the arguments sit at
/// `top-20` through `top-4`. It reads `top-20`, `top-16`, `top-12` and
/// `top-8`, stores `top-8` into the record's fourth *and* fifth slot, and
/// never touches `top-4`. The stack pointer is still moved by the full 28, so
/// nothing downstream notices. `Unit::RemoveBonus` (`0x005e07f0`), whose
/// signature is identical, pops all five: the two bodies differ by exactly one
/// argument, which is what makes this a slip in one of them rather than a
/// convention in both.
///
/// **The fifth argument is dropped here too, and the fourth is doubled, because
/// that is what the game does.** `argument 4 is maxhealth and maxstamina both`
/// is the rule the shipped executable runs, and the shipped call site cannot
/// tell the difference: it passes 0 for both. So the fault is faithful,
/// unobservable on shipped data, and asserted -- `test_combat.cpp` passes a
/// fourth and a fifth that differ and pins which one lands.
///
/// The receiver guard is the family's: `0x005e0d3f` prints
/// `The function 'Unit::AddBonus' called for an uninitialized or invalid
/// object` and returns, so an unknown id is a no-op rather than a trap, and
/// `add_bonus` answering false is that same no-op.
///
/// The original ends by calling the receiver's `vtbl+0xc4`, the rebuild,
/// which copies the class over the stored fields, adds the record back on and
/// the rest after it, and rescales health and stamina by the new maxima;
/// `add_bonus` does the same through `recalc`.
HostOutcome host_add_bonus(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const auto number = [&ctx](std::size_t index) {
    return ctx.arg(index).is_integer() ? ctx.arg(index).as_integer() : 0;
  };
  StatBonus delta;
  delta.attack = number(1);
  delta.armour_slash = number(2);
  delta.armour_pierce = number(3);
  delta.max_health = number(4);
  // Argument 5 -- `ctx.arg(5)` -- is not read. See the note above.
  delta.max_stamina = delta.max_health;
  (void)combat->add_bonus(id_of(ctx.arg(0)), delta);
  return HostOutcome::ok_void();
}

/// `BestTarget/0` is registered twice, `Catapult::BestTarget` (0x004e2f60)
/// and `Building::BestTarget` (0x004dcc60); both are in
/// `sim/siege_fire.cpp`. A unit receiver has no body of its own in the
/// original and keeps this engine's own acquisition (`auto_target`).
HostOutcome host_best_target(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const ObjectId id = id_of(ctx.arg(0));
  if (World* world = world_of(ctx); world != nullptr && combat->world_bound()) {
    if (is_catapult(*world, id)) {
      return HostOutcome::ok_with(ref_to(ctx, catapult_best_target(*world, *combat, id)));
    }
    if (const WorldObject* slot = world->find(id); slot != nullptr && slot->state.flags.is_building) {
      return HostOutcome::ok_with(ref_to(ctx, building_best_target(*world, *combat, id)));
    }
  }
  return HostOutcome::ok_with(ref_to(ctx, combat->auto_target(id)));
}

/// `BestTargetForPos` and `BestTargetInRange`'s core, 0x005dd060: a dead
/// receiver answers nothing (and prints), a held one nothing; otherwise the
/// search under `filter`, whose unit answer is one more search's pick
/// (`CombatSystem::acquire`).
HostOutcome best_target_with(CallContext& ctx, const TargetFilter& filter) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  return HostOutcome::ok_with(ref_to(ctx, combat->acquire(id_of(ctx.arg(0)), filter)));
}

/// The six `*InSquadSight` entry points' arguments to their one core,
/// 0x005dc950, by name. Each wrapper passes the core five words (the table
/// above `TargetFilter` in `sim/combat.hpp`); this is them.
struct SquadSightArgs {
  /// The `/1` forms' class name, resolved; `kNoClass` when none was passed or
  /// the name resolves to nothing.
  ClassIndex cls = kNoClass;
  /// Whether a class name was passed at all, resolved or not.
  bool named = false;
  bool undiseased = false;      ///< `_PreferUndiseased`
  bool no_independent = false;  ///< `BestNoIndependentTargetInSquadSight`
  bool exclusive = false;       ///< `Exclusive`
  bool mis_zero = false;        ///< `MisZeroDamage`
};

/// The `*InSquadSight` family, one rule for all six.
///
/// The wrappers (0x005dcbc0, 0x005dcb00, 0x005dcc80, 0x005dcd40, 0x005dce00,
/// 0x005dcf30) log and answer the invalid handle for an invalid receiver and,
/// through `vtbl+0x50`, for a dead one. The core, 0x005dc950, then has two
/// paths, and which one a unit takes is `squad_sight_squad`'s gate:
///
///   * **the squad path** -- a unit in a squad that stands outside any holder,
///     or whose squad carries `SF_ADVCHOOSER`. It draws no circle: it offers
///     the squad's `Enemies` (0x005dbb20), what the squad's look last saw of
///     the players it is at war with (`squad_look` in `sim/squad.hpp`), to
///     the predicate built with the five words **in their own slots**
///     (0x005dc9b7: name, undiseased, no-independents, exclusive, unarmed).
///     So here, and only here, `MisZeroDamage` skips an unarmed candidate,
///     `BestNoIndependent...` clears wildlife's bit out of the mask, and
///     `Exclusive` rejects a class mismatch -- a name nothing resolves
///     rejecting everything. `CombatSystem::best_target_in_list` is the walk.
///   * **the circle path** -- a unit in no squad, outside any holder: the grid
///     sweep `CombatSystem::best_target` models, with the predicate built one
///     word over (0x005dca31..0x005dca5a): the exclusive flag lands in the
///     independents slot, the unarmed flag in the exclusive slot (where with
///     no name it does nothing), and the unarmed slot gets 0. A held unit in
///     no squad, or held in a squad without `SF_ADVCHOOSER`, gets the invalid
///     handle on this path (0x005dc9a1 is not reached).
///
/// This replaced a reading in which the squad path walked a deque nothing
/// fills and so answered "no target" for every squad member; applied, that
/// left Alesia's armies standing out of reach. The deque is `Enemies`, and the
/// squad's look fills it every second: the copy at 0x00447a17 reaches it as
/// `[squad+0x64]+8`, which a search for pushes at `+0x6c` could not see.
///
/// Both paths score, prefer and stop as `docs/formats/target-score.md`
/// describes (`CombatSystem::best_target_in_list` and `acquire`), and on both
/// a unit answer's `danger` goes up by one (0x005dc9f2, 0x005dcad9). The
/// circle path sweeps around the unit's hero, out to the hero's sight, when it
/// follows one (0x005dca1d).
HostOutcome squad_sight_target(CallContext& ctx, const SquadSightArgs& args) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const ObjectId id = id_of(ctx.arg(0));
  const auto none = HostOutcome::ok_with(ref_to(ctx, kNoObject));
  const Combatant* self = combat->find(id);
  if (self == nullptr || !combat->is_alive(id) || self->action == Action::dying) return none;
  World* world = world_of(ctx);

  const HeroSystem* heroes = world == nullptr ? nullptr : hero_system_of(*world);
  if (const Squad* squad =
          heroes != nullptr ? squad_sight_squad(*world, *heroes, id) : nullptr;
      squad != nullptr) {
    // The five words in their own slots (0x005dc9b7).
    TargetFilter filter;
    filter.exclude_independents = args.no_independent;
    filter.require_armed = args.mis_zero;
    filter.prefer_undiseased = args.undiseased;
    filter.named = args.named;
    filter.named_class = args.cls;
    filter.exclusive = args.exclusive;
    return HostOutcome::ok_with(
        ref_to(ctx, combat->best_target_in_list(id, squad->enemies, filter)));
  }

  if (world != nullptr) {
    const ObjectState* state = world->state(id);
    if (state != nullptr && state->is_held()) return none;
  }
  // The circle path's slots, one word over (0x005dca31..0x005dca5a): the
  // exclusive flag clears wildlife's bit, the unarmed flag lands in the
  // exclusive slot, and nothing reaches the unarmed slot.
  TargetFilter filter;
  filter.exclude_independents = args.exclusive;
  filter.exclusive = args.mis_zero;
  filter.prefer_undiseased = args.undiseased;
  filter.named = args.named;
  filter.named_class = args.cls;
  filter.around_hero = true;
  return HostOutcome::ok_with(ref_to(ctx, combat->acquire(id, filter)));
}

/// The class name of a `/1` form, resolved against the class graph.
[[nodiscard]] SquadSightArgs named_class(CallContext& ctx, const CombatSystem& combat) {
  SquadSightArgs args;
  if (ctx.count() > 1 && ctx.arg(1).is_string()) {
    args.named = true;
    if (const ClassGraph* graph = combat.class_graph(); graph != nullptr) {
      args.cls = graph->find(ctx.arg(1).as_string());
    }
  }
  return args;
}

/// `BestTargetInSquadSight/0` -- 0x005dcbc0, the core with no flag.
HostOutcome host_best_target_in_squad_sight(CallContext& ctx) {
  return squad_sight_target(ctx, SquadSightArgs{});
}

/// `BestTargetInSquadSightMisZeroDamage/0` -- 0x005dcb00, what
/// `DATA\SUBAI\UNIT_IDLE.VS` runs. Its flag is the core's last word, which
/// the squad path hands to the predicate's damage slot (0x005db69e skips a
/// candidate whose own attack is not positive) and the circle path hands to
/// the exclusive slot, where with no class name it does nothing.
/// `UNIT_IDLE.VS` re-tests `u.attack > 0 || AsDruid || AsWagon` on what comes
/// back, which the circle path needs and the squad path makes redundant.
HostOutcome host_best_target_mis_zero_damage(CallContext& ctx) {
  SquadSightArgs args;
  args.mis_zero = true;
  return squad_sight_target(ctx, args);
}

/// `BestNoIndependentTargetInSquadSight/0` -- 0x005dcc80. Its flag clears
/// wildlife's bit (14) out of the predicate's mask on the squad path and is
/// dropped on the circle path (0x005dca31). `HERO_SNEAK.VS` is the one caller.
HostOutcome host_best_target_no_independent(CallContext& ctx) {
  SquadSightArgs args;
  args.no_independent = true;
  return squad_sight_target(ctx, args);
}

/// `BestTargetInSquadSight_PreferUndiseased/0` -- argument 2, which adds 200
/// to a diseased candidate's score at `0x005db6e0` on both paths: the
/// `ObjectFlags::diseased` bit `Unit::Disease` sets.
HostOutcome host_best_target_prefer_undiseased(CallContext& ctx) {
  SquadSightArgs args;
  args.undiseased = true;
  return squad_sight_target(ctx, args);
}

/// `BestTargetInSquadSight/1(class)` -- 6 sites, and the sole blocker of
/// `HERO_ENGAGE_UNIT_TYPE.VS` and `UNIT_ENGAGE_UNIT_TYPE.VS`.
///
/// **Its sibling's twin, and the difference is one argument.** 0x005dce00 and
/// `Exclusive`'s 0x005dcf30 both call 0x005dc950 with the string in the same
/// slot; the second passes 1 where the first passes 0, and that flag is what
/// 0x005db774 reads to decide whether a class mismatch is a rejection or a
/// 200-point penalty. So this one **prefers** the named class where `Exclusive`
/// **requires** it -- on the squad path. On the circle path the exclusive flag
/// lands in the independents slot and both forms prefer.
///
/// A name no class graph resolves is **not** a failure: the original's
/// string compare simply never matches, so every candidate takes the same
/// 200 and none earns the in-range bonus.
HostOutcome host_best_target_prefer_class(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  return squad_sight_target(ctx, named_class(ctx, *combat));
}

/// `BestTargetInSquadSightExclusive/1` -- 0x005dcf30. On the squad path a
/// class mismatch is rejected (0x005db774), so a name nothing resolves
/// answers no target. On the circle path its exclusive flag lands in the
/// independents slot (0x005dca31): the enemy mask loses bit 14, so wildlife
/// is excluded, and a mismatch costs the +200 the plain string form charges
/// instead of rejecting (0x005db789, the exclusive slot holding the zero
/// meant for the damage flag); a name nothing resolves therefore charges
/// every candidate the same there.
HostOutcome host_best_target_exclusive(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  SquadSightArgs args = named_class(ctx, *combat);
  args.exclusive = true;
  return squad_sight_target(ctx, args);
}

/// `BestTargetForPos/0` -- `0x005dd280`, which is `0x005dd060` with the
/// attacker's own position and the in-range flag set. The flag turns on the
/// `[min_range, range]` test at `0x005db820`, so this answers "what can I hit
/// without moving", which is what `UNIT_HOLD_POSITION.VS` wants.
HostOutcome host_best_target_for_pos(CallContext& ctx) {
  TargetFilter filter;
  filter.require_in_attack_range = true;
  return best_target_with(ctx, filter);
}

/// `BestTargetInRange/2(point, int)` -- `0x005dd2a0`, the same core with the
/// script's point as the sweep centre and its integer as the sweep radius. A
/// radius of zero or less falls back to the attacker's `sight` (`0x005dd11d`),
/// and so does any radius for an attacker that shoots (`0x005dd123`: the
/// class declares a `projectile_class` that is not `**Invalid**`,
/// 0x0053f5b0). An x of -1 is the attacker's own position.
HostOutcome host_best_target_in_range(CallContext& ctx) {
  TargetFilter filter;
  filter.centre_on_point = true;
  filter.centre = unpack_point(ctx.arg(1));
  if (ctx.arg(2).is_integer()) filter.radius = ctx.arg(2).as_integer();
  return best_target_with(ctx, filter);
}

/// `unit.TimeWithoutAttack` -- game time since this unit last struck.
///
/// `gbr.exe` 0x005d7f93 is three instructions: the clock at
/// `[0x00996ff4] + 0x1258`, minus `unit + 0x1a4`, pushed as an `int`. No clamp
/// and no special case, so a unit that has never attacked carries
/// `last attack time = 0` and reports the whole elapsed session -- which is a
/// large number, and is exactly what the original reports. An invalid receiver
/// diagnoses and pushes **0** (0x005d7f7f), the opposite end, so a dead unit
/// reads as having just attacked rather than as having waited forever.
///
/// This overturns the refusal recorded at the foot of this file, and the reason
/// it can is that the refusal was about the *writer*: nothing established what
/// `SetLastAttackTime`'s 40 sites expect a stamp to do to the next attack. The
/// reader does not depend on that. It reads `Combatant::last_attack_time`, the
/// dumps' `last attack time`, which `attack` already stamps with `now` on every
/// blow, and which `SetLastAttackTime` (0x005d7fc0, `m_set_last_attack_time`
/// in sim/world_host.cpp) stamps on a script's say-so.
HostOutcome host_time_without_attack(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const Combatant* unit = combat->find(id_of(ctx.arg(0)));
  if (unit == nullptr) return HostOutcome::ok_with(Value::integer(0));
  // **Clamped at zero, which the original has no need to do.** It reads one
  // clock: the stamp and `[0x00996ff4] + 0x1258` are the same counter, so the
  // difference cannot be negative. Here they are two. `World::time()` is the
  // boundary the last turn began at, while `CombatSystem` schedules inside the
  // turn on `[start, end]` where `start = time + 1` (`sim/tick.hpp`, and all
  // nine dumps agree), and its anti-spin nudge can put an action one unit past
  // `end`. So a unit that struck in the final instant of a turn reads back a
  // stamp slightly ahead of the clock.
  //
  // The skew is bounded by that nudge and the quantity is compared against
  // thousands, so what matters is not the unit but the sign: a negative is a
  // value no original ever returned, and "just attacked" is what the engine
  // means by it. Zero says exactly that and stays inside the original's range.
  const GameTime since = world->time() - unit->last_attack_time;
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(since > 0 ? since : 0)));
}

/// `.EnemiesInSight()` -- 2 sites, and the sole blocker of both.
///
/// `Unit::EnemiesInSight` (`gbr.exe` 0x005d74d0) sweeps one circle around the
/// unit -- centre from `vtbl + 0x3c`, radius `[unit + 0xd0]`, which is `sight`
/// -- and answers whether the sweep's counter came back non-zero. It is a
/// *predicate over the same population `best_target` scores*, not a second
/// notion of enemy, so it is built out of the same pieces here.
///
/// Two guards run before the sweep and both answer `false` outright:
/// `vtbl + 0x50` (the slot `Obj::IsAlive` negates), and `word [unit + 0x154]
/// != 0xffff`, which is the holder handle -- a garrisoned unit sees nothing.
///
/// Inside the sweep, the candidate clauses at 0x005d5373-0x005d53c6, in the
/// original's order:
///
///   * squared distance `<= sight * sight` -- `jg skip`, so the rim counts;
///   * the flag word is non-zero and carries `kSyncUnit`, so buildings, decor
///     and item holders are not "enemies in sight" however hostile they are;
///   * `kSyncHidden` set means the candidate must additionally pass an owner
///     mask test against the looker's side -- reproduced here as "a hidden
///     object is not seen", which is what that test comes to for the one side
///     doing the looking;
///   * not dead;
///   * **`[obj + 0xdc] > 1`** -- `Obj::attack`, and note the bound. The target
///     sweep's `require_armed` clause tests the same field `> 0`
///     (`TargetFilter`, and `UNIT_IDLE.VS` re-tests `u.attack > 0` on what it
///     gets back); this one wants *more than one*. A one-damage object is a
///     legal target and is not an enemy in sight.
///   * enemy of the looker.
///
/// Both shipped sites are asking "am I in a fight" --
/// `HERO_MOVEINFIGHT_VERIFY.VS`'s `if (!.EnemiesInSight()) return false;` and
/// `HERO_RETREAT_VERIFY.VS`'s `.EnemiesInSight() && pt.Dist(.posRH) > .sight/2`
/// -- which is exactly what the `> 1` clause is for: a wandering villager does
/// not keep a hero from moving.
HostOutcome host_enemies_in_sight(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(no_system_error(ctx));

  const ObjectId id = id_of(ctx.arg(0));
  const Combatant* self = combat->find(id);
  if (self == nullptr || !self->alive || self->action == Action::dying) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  // The holder guard. `Combatant` carries no holder, so this reads the world's
  // -- the same field `Obj::InHolder` answers from.
  const WorldObject* slot = world->find(id);
  if (slot != nullptr && slot->state.is_held()) {
    return HostOutcome::ok_with(Value::boolean(false));
  }

  // **The world object's `sight`, not the profile's.** `[unit + 0xd0]` is one
  // field in the original and `Obj::sight` is what this engine answers from it,
  // and `HERO_RETREAT_VERIFY.VS` reads both in one expression --
  // `.EnemiesInSight() && pt.Dist(.posRH) > .sight/2` -- so the two have to be
  // the same number. The profile is the fallback for a `CombatSystem`
  // populated with ids of its own, which has no world object to read.
  const std::int64_t sight =
      slot != nullptr ? slot->sight : combat->profile(self->class_index).sight;
  if (sight < 0) return HostOutcome::ok_with(Value::boolean(false));
  const std::int64_t reach2 = sight * sight;

  for (const Combatant& other : combat->combatants()) {
    if (other.id == self->id) continue;
    if (!other.alive || other.action == Action::dying) continue;
    if (distance_squared(self->position, other.position) > reach2) continue;
    const WorldObject* seen = world->find(other.id);
    if (seen != nullptr && (!seen->state.flags.is_unit || seen->state.flags.hidden)) continue;
    if (combat->attack_of(other) <= 1) continue;
    if (!combat->is_enemy(*self, other)) continue;
    // And the looker's `CanAttack` virtual must accept it (0x005d5180): a
    // sentry or a ram that cannot hit the candidate is not in a fight with it.
    if (!combat->unit_can_attack(*self, other)) continue;
    return HostOutcome::ok_with(Value::boolean(true));
  }
  return HostOutcome::ok_with(Value::boolean(false));
}

/// `.GetPointOnTarget(target)` -- 3 sites, all three on a catapult.
///
/// `Catapult::GetPointOnTarget` (`gbr.exe` 0x004e3080) has two arms and the
/// first is the whole of what this engine can answer:
///
///   * a target carrying `kSyncUnit` (0x004e3107) answers the target's own
///     aim point, `vtbl + 0xc8`, and nothing else happens;
///   * anything else -- a building, which is what a catapult usually shoots --
///     walks the target's **footprint**. 0x004e3155 asks the target for its
///     occupied cells twice, under codes 4 and 0x10, expands them into at most
///     0x40 offsets, appends a `{0, 0}` sentinel, and returns the first offset
///     `off` for which `isqrt(|catapult - (target.pos + off)|) - catapult.radius`
///     falls inside `[[catapult + 0xd8], [catapult + 0xd4]]` -- the min and max
///     range. The sentinel is what makes the target's own centre the last
///     candidate rather than the first.
///
/// **The footprint arm is not reproduced, and the reason is that the candidate
/// list would be a fiction.** This engine has no per-object occupied-cell list
/// to walk: `sim/path.hpp` stamps a `.pass` footprint into the grid at load and
/// keeps no per-object inverse, so the offsets would have to be invented. What
/// is reproduced is the arm's *decision* -- the point returned is on the target
/// and is chosen for being in range -- over the one candidate that is not
/// invented, the target's own position. So the sentinel is the whole list here,
/// and the answer is the target's position in both arms.
///
/// The range test still runs, because it is what the two callers are asking
/// about: `CATAPULT_ATTACK_VERIFY.VS` is `if (!.InRange(.GetPointOnTarget(other)))
/// return false;`, and out-of-range is what it wants to hear. When nothing is in
/// range the original returns candidate zero -- which for a building is a raw
/// footprint *offset*, un-translated, so a point near the map origin; that is
/// almost certainly a slip in the original and is not reproduced. This answers
/// the target's position, which is the same point the in-range arm answers and
/// leaves `InRange` to say no by itself.
///
/// The receiver is checked first and the target second; either one naming no
/// object prints (through the sink that is a bare `ret` in retail) and pushes
/// `(0, 0)` -- never a refusal. A unit target's aim point, `vtbl+0xc8`, is
/// its `posRH`: the town it garrisons while it is held, which is
/// `unit_pos_rh`'s answer, not `resolve_position`'s.
HostOutcome host_get_point_on_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(no_system_error(ctx));
  if (world->find(id_of(ctx.arg(0))) == nullptr) {
    return HostOutcome::ok_with(pack_point(Point{0, 0}));
  }
  const ObjectId target = id_of(ctx.arg(1));
  const WorldObject* slot = world->find(target);
  if (slot == nullptr) return HostOutcome::ok_with(pack_point(Point{0, 0}));
  if (slot->state.flags.is_unit) return HostOutcome::ok_with(pack_point(unit_pos_rh(*world, target)));
  return HostOutcome::ok_with(pack_point(world->resolve_position(target)));
}

/// The three are registered only on `Catapult` (0x004e3500, 0x004e3600,
/// 0x004e3710) and are the engine's target slot, `ObjectState::catapult_target`,
/// with its mirror onto the engine's towers (`sim/siege_fire.cpp`). A
/// receiver that is not a catapult has no body in the original; it keeps the
/// combatant's target here, which no shipped script reaches.
[[nodiscard]] World* catapult_world(CallContext& ctx, const Value& receiver) {
  World* world = world_of(ctx);
  return world != nullptr && is_catapult(*world, id_of(receiver)) ? world : nullptr;
}

HostOutcome host_get_target(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  if (World* world = catapult_world(ctx, ctx.arg(0)); world != nullptr) {
    return HostOutcome::ok_with(ref_to(ctx, world->state(id_of(ctx.arg(0)))->catapult_target));
  }
  const Combatant* unit = combat->find(id_of(ctx.arg(0)));
  return HostOutcome::ok_with(ref_to(ctx, unit == nullptr ? kNoObject : unit->target));
}

HostOutcome host_clear_target(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  if (World* world = catapult_world(ctx, ctx.arg(0)); world != nullptr) {
    catapult_clear_target(*world, id_of(ctx.arg(0)));
    return HostOutcome::ok_void();
  }
  combat->stop(id_of(ctx.arg(0)));
  return HostOutcome::ok_void();
}

HostOutcome host_set_target(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  if (World* world = catapult_world(ctx, ctx.arg(0)); world != nullptr) {
    catapult_set_target(*world, id_of(ctx.arg(0)), id_of(ctx.arg(1)));
    return HostOutcome::ok_void();
  }
  combat->order_attack(id_of(ctx.arg(0)), id_of(ctx.arg(1)));
  return HostOutcome::ok_void();
}

}  // namespace

/// `InvalidateDamageFormulaParams()` -- 5 sites, and a no-op with a reason.
///
/// The whole of `0x004c6b10` is a write of zero to one global dword and a
/// return. That dword is a *"the cached damage-formula constants are stale"*
/// flag; the entry point touches nothing else, and there is no registered way
/// to read it back.
///
/// `ONFINISH_RESEARCH.VS` calls it after each of five researches -- *Gambler's
/// Test*, *Gambler's Luck*, *Attack focus*, *Learning*, *Ferocity* -- and every
/// one of those changes an input to the formula. So the call is the original
/// telling its own cache to refill.
///
/// **There is no cache here to invalidate.** `DamageInputs` is flat and pure by
/// construction -- *"it takes no world, no object, and no hidden state"* -- and
/// every constant it uses is read at the point of the strike. A stub that
/// stored a flag would be storing something nothing reads; doing nothing is the
/// faithful answer, and it stops being faithful the moment this system starts
/// caching, which is what this comment is for.
HostOutcome host_invalidate_damage_formula(CallContext& ctx) {
  (void)ctx;
  return HostOutcome::ok_void();
}

void define_combat_host(script::HostRegistry& registry) {
  const auto member = [&registry](std::string_view name, std::uint16_t arity,
                                  script::HostFn fn) {
    // Arity in the registry counts the receiver, which is argument 0.
    registry.define(CallKind::member, name, arity, fn);
  };

  // Ordered by call frequency in docs/formats/vs-host-api.md.
  registry.define(CallKind::free_function, "InvalidateDamageFormulaParams", 0,
                  &host_invalidate_damage_formula);                          //   5

  member("IsValidTarget", 1, &host_is_valid_target);         //  81
  // `IsEnemy/1` is **not** defined here. `sim/player_host.cpp` owns it: the
  // corpus calls it with an object argument *and* with an integer player
  // (`set.IsEnemy(AIPlayer)`), and the relation is the asymmetric matrix in
  // `World::players()`, neither of which this domain's version handled. Both
  // domains defined it for a while and `host_setup.cpp`'s order decided which
  // one ran -- silently, because `define` replaces without saying so.
  member("BestTargetInSquadSight", 0, &host_best_target_in_squad_sight);  //  69
  member("Damage", 1, &host_damage);                         //  44
  member("TimeWithoutAttack", 0, &host_time_without_attack);  //   3

  // -- the rest of the target-selection family -----------------------------
  //
  // Six host entry points, one function in `gbr.exe` (`0x005dc950`), five
  // flags between them. `sim/combat.hpp`'s `TargetFilter` carries the
  // disassembly and says which half of each variant is measured; the short
  // version is that every filter registered here is a *hard* reject read off
  // the instruction, and the two variants whose rule is a score adjustment are
  // treated differently for the reason given at each.
  //
  // `BestTargetInSquadSightMisZeroDamage/0` is the one that matters: it is
  // 859 of the 885 traps a 50-turn headless run of Numantia hits, because
  // `DATA\SUBAI\UNIT_IDLE.VS` is the sub-AI every unit runs while idle.
  member("BestTargetInSquadSightMisZeroDamage", 0, &host_best_target_mis_zero_damage);
  member("BestNoIndependentTargetInSquadSight", 0, &host_best_target_no_independent);
  member("BestTargetInSquadSight_PreferUndiseased", 0, &host_best_target_prefer_undiseased);
  member("BestTargetInSquadSightExclusive", 1, &host_best_target_exclusive);
  member("BestTargetInSquadSight", 1, &host_best_target_prefer_class);  // 6
  member("BestTargetForPos", 0, &host_best_target_for_pos);
  member("BestTargetInRange", 2, &host_best_target_in_range);
  //
  // `BestTargetInGAIKA/0` is not claimed here either, and it is **not**
  // refused: `0x0043bd35` registers it out of the GAIKA slice, its core
  // (`0x00431090`) walks the squads standing in a node rather than sweeping a
  // circle, and it is bound in `sim/ai.cpp` beside the rest of the node table.
  // The half of it that belongs to this domain is the predicate and the score,
  // and that is `best_target_among` above -- so the two spellings cannot
  // disagree about what makes a target worth hitting.
  member("Attack", 1, &host_attack);                         //  40
  member("maxstamina", 0, &read_int<&read_max_stamina>);     //   6
  member("attack", 0, &read_int<&read_attack>);              //  16
  member("damage", 0, &read_int<&read_attack>);              //  inline only
  member("armor_slash", 0, &read_int<&read_armor_slash>);    //  inline only
  member("range", 0, &host_range);                           //  10
  member("Heal", 1, &host_heal);
  member("HealStamina", 1, &host_heal_stamina);  // 11, eight of them on a group
  member("SetHealth", 1, &host_set_health);
  member("SetStamina", 1, &host_set_stamina);
  member("CanAttack", 1, &host_can_attack);
  member("InRange", 1, &host_in_range);
  member("EnemiesInSight", 0, &host_enemies_in_sight);      //   2
  member("GetPointOnTarget", 1, &host_get_point_on_target);  //   3
  member("BestTarget", 0, &host_best_target);
  // Two thirds of what `ONFINISH_RESEARCH.VS` was blocked on; the third is
  // `Settlement::SetFoodProduction`, in `sim/economy.cpp`.
  registry.define(script::CallKind::free_function, "SetExperienceModifier", 2,
                  &host_set_experience_modifier);  // 1
  member("RecalcBonuses", 0, &host_recalc_bonuses);  // 1
  member("AddBonus", 5, &host_add_bonus);            // 1
  member("GetCurrentTarget", 0, &host_get_target);
  member("ClearTarget", 0, &host_clear_target);
  member("SetTarget", 1, &host_set_target);

  // -- entry points this domain deliberately does *not* claim ---------------
  //
  // `HostRegistry::define` replaces silently, so an entry point two domains
  // both define behaves like whichever registered last and the loser's own
  // tests keep passing. `sim/host_setup.hpp` fixes the order and
  // `engine/tests/test_host_setup.cpp` asserts that no domain loses an entry
  // to a later one; these twelve are the collisions that assertion found, and
  // combat gives up all twelve, and a thirteenth for a different and worse
  // reason.
  //
  // **`KillCommand/0` was outright wrong here, not merely duplicated.** This
  // domain read it as "kill the receiver" and answered it with
  // `apply_damage(id, health(id))`. `sim/command.cpp` reads it as "end the
  // running command" and cites the corpus: `ESH_FOODTRADE.VS` does
  // `wagon.AddCommand(false, "unload", hall); wagon.KillCommand();`, where
  // killing the mule would destroy the gold it was just loaded with, and
  // `AI HELPERS\GUARD.VS` pairs it with `AddCommand(true, ...)` as the
  // explicit two-step form of `SetCommand`. The declared surface
  // (`script/host_surface.cpp`) has no `Kill`, `Die` or `Destroy` member at
  // all, so there was never a "kill the object" entry point for this one to be
  // -- 43 call sites would have destroyed their receiver. It goes to
  // `command`, and nothing here replaces it: `CombatSystem::apply_damage` is
  // still how a unit dies, and no entry point in the inventory asks for it
  // directly.
  //
  // To the object model (`sim/world_host.cpp`), which answers for *every*
  // object rather than only for registered combatants -- `health` alone has
  // 129 call sites and buildings, wagons and item holders all have health,
  // while `CombatSystem::find` returns null for each of them and this domain's
  // reads would have answered 0:
  //
  //   health/0  maxhealth/0  stamina/0  IsAlive/0  IsDead/0  radius/0  sight/0
  //
  // `maxhealth`, `radius` and `sight` read the same class-graph properties on
  // both sides, so nothing is lost there. `IsAlive`/`IsDead` lose this
  // domain's knowledge of the dying state, which costs nothing measurable:
  // `enter_dying` clamps health to zero and a unit only enters it at health
  // <= 0, so `state.health > 0` -- what the object model tests -- gives the
  // same answer. `health` and `stamina` are exact whenever the system is
  // world-bound, because the host mutators below write through; see the note
  // on `sync_to_world`.
  //
  // To heroes (`sim/hero.cpp`), which owns unit progression -- `HeroSystem`
  // holds level and experience per unit and `HeroSystem::level` applies the
  // floor a hero's Discipline puts under its army, which a bare `Combatant`
  // field cannot:
  //
  //   level/0  inherentlevel/0  experience/0  SetLevel/1  SetExperience/1
  //
  // `Combatant::level` and `::experience` stay: they are what the damage
  // formula and `award_experience` read, and `CombatSystem` is still the one
  // that raises them. They are simply no longer what a script sees.
  //
  // `SetLastAttackTime` stays declared and unimplemented: nothing yet
  // establishes what its 40 call sites expect the stamp to do to the next
  // attack.
  //
  // **`TimeWithoutAttack` no longer does, and the paragraph that used to hold
  // them together was wrong to.** It read the two as one problem, and the
  // disassembly separates them: `0x005d7f93` is `now - unit[0x1a4]`, three
  // instructions over a field `attack` already writes, and nothing in it
  // depends on what a script's stamp means. The "plausible zero" the old note
  // feared was a guess about the reader's *value*; there is no guess left to
  // make. See `host_time_without_attack` above.
}

}  // namespace imperivm::core::sim
