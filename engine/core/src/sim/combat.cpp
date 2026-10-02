// Combat. The model, its evidence, and what is invented: sim/combat.hpp.
//
// No floating point, no clock, no allocation in the inner loop that is not a
// growing vector, and no unordered container anywhere: iteration order is world
// state (docs/engine/architecture.md).

#include "imperivm/core/sim/combat.hpp"

#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/hooks.hpp"

#include <algorithm>
#include <utility>

#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
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
  return constants_.experience_modifier;
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
  const auto it = std::lower_bound(
      units_.begin(), units_.end(), combatant.id,
      [](const Combatant& c, ObjectId id) { return c.id < id; });
  if (it != units_.end() && it->id == combatant.id) {
    *it = combatant;
    return *it;
  }
  return *units_.insert(it, combatant);
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

bool CombatSystem::reclass(ObjectId id, ClassIndex to) noexcept {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr) return false;
  unit->class_index = to;
  unit->target = kNoObject;
  unit->action = Action::idle;
  unit->anim = kAnimIdle;
  unit->attacks = 0;
  unit->last_attack_time = 0;
  unit->next_action_time = 0;
  unit->attack_bonus = 0;
  unit->armour_bonus = 0;
  unit->level_floor = 0;
  unit->damage_multiplier_percent = 100;
  unit->ignores_armour = false;
  unit->bonus = StatBonus{};
  return true;
}

std::size_t CombatSystem::reconcile(World& world) {
  // Without world ids there is nothing to reconcile against, and reconciling
  // anyway would delete every combatant a synthetic test registered.
  if (!world_bound_) return units_.size();

  const std::span<const WorldObject> objects = world.objects();

  // A linear merge of two sequences that are both sorted ascending by id.
  // Rebuilt into a fresh vector rather than patched in place, because an insert
  // in the middle of `units_` is O(n) and doing that per new object on a map
  // that spawns a hundred at once is O(n^2) for no reason. The result is in
  // ascending id order either way, which is what `add`'s contract promises and
  // what makes iteration order reproducible.
  std::vector<Combatant> merged;
  merged.reserve(units_.size());

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
    fresh.experience = 0;
    fresh.level = 1;
    merged.push_back(fresh);
  }

  units_ = std::move(merged);
  return units_.size();
}

void CombatSystem::start(World& world) {
  // The world's diplomacy replaces this system's own ally table from here on.
  players_ = &world.players();
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
  // Reach is measured surface to surface: the class `range` plus both radii.
  // INFERRED -- the dumps' `Min Range`/`Range` are path arrival tolerances and
  // cluster at exactly these class numbers, but nothing states the sum.
  const std::int64_t reach =
      static_cast<std::int64_t>(mine.range) + mine.radius + theirs.radius;
  if (d2 > squared(reach)) return false;
  if (mine.min_range > 0) {
    // A catapult's `min_range` of 301 really is a dead zone. Measured centre to
    // centre, because the far edge is what the class number describes.
    if (d2 < squared(static_cast<std::int64_t>(mine.min_range))) return false;
  }
  return true;
}

ObjectId CombatSystem::best_target(ObjectId id) const {
  return best_target(id, TargetFilter{});
}

// The target-selection family, as `sim/combat.hpp`'s `TargetFilter` note
// documents it. Every filter below is a hard reject read off `0x005db650` in
// `gbr.exe`. The two variants whose rule is a *score* adjustment are not here:
// this system's score is not the executable's, and a 200-point nudge on the
// wrong scale is not the same rule. `prefer_class` is a third of that kind and
// is the one exception: it is taken to the limit the nudge points at rather
// than scaled, because dropping it would drop the argument entirely and leave
// two scripts trapping. Its declaration says exactly what that costs.
ObjectId CombatSystem::best_target(ObjectId id, const TargetFilter& filter) const {
  const Combatant* self = find(id);
  if (self == nullptr || !self->alive || self->action == Action::dying) return kNoObject;
  // Inside a holder, nothing: see `targetable`.
  if (self->position == kHeldPosition) return kNoObject;
  const CombatProfile mine = profile(self->class_index);
  if (!mine.can_attack()) return kNoObject;

  // The sweep circle. `BestTargetInRange` is the only variant that moves it:
  // `0x005dd060` passes the script's point as the grid-sweep centre and keeps
  // the attacker's own position as the predicate's origin, so the *search* and
  // the *score* measure from different places, and only there.
  const Point centre = filter.centre_on_point ? filter.centre : self->position;
  const std::int64_t sight = mine.sight > 0 ? mine.sight : mine.range;
  const std::int64_t reach = filter.radius > 0 ? filter.radius : sight;
  const std::int64_t reach2 = squared(reach);

  ObjectId best = kNoObject;
  std::int64_t best_score = 0;
  // `BestTargetInSquadSight/1`'s preference, as a rank ahead of the score
  // rather than as a term inside it. See `TargetFilter::prefer_class`.
  bool best_preferred = false;
  for (const Combatant& other : units_) {
    if (!targetable(*self, other)) continue;
    // `BestTargetInSquadSightExclusive`. Exact class, not ancestry.
    if (filter.only_class != kNoClass && other.class_index != filter.only_class) continue;
    // `BestNoIndependentTargetInSquadSight`. Bit 14 of the enemy mask, alone.
    if (filter.exclude_independents && other.owner == kNeutralWildlife) continue;
    // `BestTargetInSquadSightMisZeroDamage`. The candidate's own attack, which
    // is `[obj+0xdc]` there and `Obj::attack` here -- the same number
    // `UNIT_IDLE.VS` re-tests as `u.attack > 0` on the result it gets back.
    if (filter.require_armed && attack_of(other) <= 0) continue;

    const std::int64_t d2 = distance_squared(centre, other.position);
    if (d2 > reach2) continue;
    // `BestTargetForPos`. Only what the attacker could strike from here.
    if (filter.require_in_attack_range && !in_attack_range(*self, other)) continue;

    const std::int64_t score = target_score(*self, other);
    const bool preferred =
        filter.prefer_class != kNoClass && other.class_index == filter.prefer_class;
    // Preference first, score second. Both comparisons are strict, so ties keep
    // the lower id, which the ascending iteration already gives -- and a
    // preferred candidate that is already best must not be displaced by a later
    // preferred one that only ties it.
    const bool better = best == kNoObject || (preferred && !best_preferred) ||
                        (preferred == best_preferred && score < best_score);
    if (better) {
      best = other.id;
      best_score = score;
      best_preferred = preferred;
    }
  }
  return best;
}

// `Unit::BestTargetInGAIKA`'s half. Same predicate, same score, no circle --
// see the declaration in `sim/combat.hpp` for why there is no circle, and
// `sim/ai.cpp` for where the candidates come from.
ObjectId CombatSystem::best_target_among(ObjectId id,
                                         std::span<const ObjectId> candidates) const {
  const Combatant* self = find(id);
  if (self == nullptr || !self->alive || self->action == Action::dying) return kNoObject;
  if (!profile(self->class_index).can_attack()) return kNoObject;

  ObjectId best = kNoObject;
  std::int64_t best_score = 0;
  for (const ObjectId candidate : candidates) {
    const Combatant* other = find(candidate);
    if (other == nullptr) continue;
    if (!targetable(*self, *other)) continue;
    const std::int64_t score = target_score(*self, *other);
    // Strictly less, so a tie keeps the **first offered** rather than the
    // lowest id: nothing sorts this walk, and the order it arrives in is the
    // order the original's own two loops produce.
    if (best == kNoObject || score < best_score) {
      best = other->id;
      best_score = score;
    }
  }
  return best;
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
  return theirs.can_be_attacked && !theirs.never_targeted();
}

std::int64_t CombatSystem::target_score(const Combatant& self,
                                        const Combatant& other) const noexcept {
  // distance^2 * target_factor / target_priority. Smallest wins. Always from
  // the attacker's own position -- `BestTargetInRange` moves the *sweep*
  // centre and leaves the score's origin where it was (`0x005dd060`).
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

void CombatSystem::award_experience(Combatant& attacker, const Combatant& defender) {
  // CONST.INI: exp_gain = target_level / LevelExpDivider + 1, scaled by the
  // **attacker's own** `BattleTacticsExpModifier` -- the research is per player
  // and `SetExperienceModifier` writes it there.
  attacker.experience += constants_.experience_gain(
      defender.base_effective_level(), player_experience_modifier(attacker.owner));
  const std::int32_t level = constants_.level_for_experience(attacker.experience);
  if (level > attacker.level) attacker.level = level;
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
  if (killed) enter_dying(world, defender, now);
  record_score(world, source, scored, removed, killed);
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

std::int32_t CombatSystem::attack_of(const Combatant& unit) const {
  return profile(unit.class_index).damage + unit.attack_bonus + unit.bonus.attack;
}

std::int32_t CombatSystem::armour_of(const Combatant& unit, DamageType type) const {
  const std::int32_t from_record = type == DamageType::slash    ? unit.bonus.armour_slash
                                   : type == DamageType::pierce ? unit.bonus.armour_pierce
                                                                : 0;
  return profile(unit.class_index).armour_against(type) + unit.armour_bonus + from_record;
}

std::int32_t CombatSystem::max_health_of(const Combatant& unit) const {
  return profile(unit.class_index).max_health + unit.bonus.max_health;
}

std::int32_t CombatSystem::max_stamina_of(const Combatant& unit) const {
  return profile(unit.class_index).max_stamina + unit.bonus.max_stamina;
}

bool CombatSystem::add_bonus(ObjectId id, const StatBonus& delta) {
  Combatant* unit = mutable_find(id);
  if (unit == nullptr) return false;
  unit->bonus.attack += delta.attack;
  unit->bonus.armour_slash += delta.armour_slash;
  unit->bonus.armour_pierce += delta.armour_pierce;
  unit->bonus.max_health += delta.max_health;
  unit->bonus.max_stamina += delta.max_stamina;
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

  const ObjectId attacker_id = attacker.id;
  const ObjectId defender_id = defender.id;
  hit(world, defender, inputs_for(attacker, defender), attacker.id, now);
  // Experience is paid per damaging attack, not per kill -- see
  // CombatConstants. Paid before the defender is reaped so a killing blow
  // still counts. Both re-found: a fatal blow ran two hooks in between.
  Combatant* a = mutable_find(attacker_id);
  const Combatant* d = find(defender_id);
  if (a != nullptr && d != nullptr) award_experience(*a, *d);
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

}  // namespace

void CombatSystem::act(World& world, Combatant& unit, GameTime now) {
  const CombatProfile mine = profile(unit.class_index);
  if (!mine.can_attack() || unbuilt_engine(world, unit.id)) {
    unit.next_action_time = now + constants_.acquire_interval;
    return;
  }

  // Validate the current target; the dumps show one unresolved `target handle`
  // pointing at a destroyed object, so this has to tolerate a stale one.
  Combatant* target = mutable_find(unit.target);
  if (target != nullptr && (!target->alive || target->action == Action::dying ||
                            target->health <= 0 || target->position == kHeldPosition ||
                            !is_enemy(unit, *target))) {
    target = nullptr;
    unit.target = kNoObject;
    unit.attacks = 0;
  }
  if (target == nullptr) {
    const ObjectId picked = best_target(unit.id);
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
      const GameTime t = shots_[i].impact;
      if (t <= begin || t > end) continue;
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
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(unit.experience)));
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(unit.level)));
    fold(static_cast<std::uint64_t>(unit.last_attack_time));
    fold(unit.target);
    fold(static_cast<std::uint64_t>(static_cast<std::uint32_t>(unit.attacks)));
    fold(unit.unit_flags);
    fold(static_cast<std::uint64_t>(unit.action));
    fold(static_cast<std::uint64_t>(unit.next_action_time));
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
std::int32_t read_range(const CombatSystem& c, ObjectId id) {
  const Combatant* u = c.find(id);
  return u == nullptr ? 0 : c.profile(u->class_index).range;
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

HostOutcome host_damage(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const std::int32_t amount = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  // The original refuses this: "Negative damage in function 'Obj::Damage'".
  if (amount < 0) return HostOutcome::failed("Negative damage in function 'Obj::Damage'");
  const ObjectId victim = id_of(ctx.arg(0));
  // `system_of` found the system *through* the world, so a non-null system
  // means a non-null world. No second guard, for the reason `hit`'s `onkill`
  // fire gives: a branch nothing can reach is a claim this engine is not
  // making.
  combat->apply_damage(*world_of(ctx), victim, amount);
  sync_to_world(ctx, *combat, victim);
  return HostOutcome::ok_void();
}

HostOutcome host_heal(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const std::int32_t amount = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  const ObjectId patient = id_of(ctx.arg(0));
  combat->heal(patient, amount);
  sync_to_world(ctx, *combat, patient);
  return HostOutcome::ok_void();
}

HostOutcome host_set_health(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  Combatant* unit = combat->find(id_of(ctx.arg(0)));
  if (unit == nullptr) return HostOutcome::ok_void();
  unit->health = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : unit->health;
  sync_to_world(ctx, *combat, unit->id);
  return HostOutcome::ok_void();
}

HostOutcome host_set_stamina(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  Combatant* unit = combat->find(id_of(ctx.arg(0)));
  if (unit == nullptr) return HostOutcome::ok_void();
  unit->stamina = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : unit->stamina;
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
  // Before the receiver is even resolved, as in the original: the sign test at
  // 0x005ab72c comes after the receiver check but nothing between them has an
  // effect, and refusing early makes the no-op unmistakable.
  if (amount < 0) return HostOutcome::ok_void();
  for (const ObjectId id : receiver_objects(*world, ctx.arg(0))) {
    Combatant* unit = combat->find(id);
    if (unit == nullptr) continue;
    const std::int32_t max = combat->max_stamina_of(*unit);
    const std::int64_t raised = static_cast<std::int64_t>(unit->stamina) + amount;
    unit->stamina = static_cast<std::int32_t>(raised > max ? max : raised);
    sync_to_world(ctx, *combat, id);
  }
  return HostOutcome::ok_void();
}

HostOutcome host_set_level(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  Combatant* unit = combat->find(id_of(ctx.arg(0)));
  if (unit == nullptr) return HostOutcome::ok_void();
  if (ctx.arg(1).is_integer()) unit->level = ctx.arg(1).as_integer();
  return HostOutcome::ok_void();
}

HostOutcome host_set_experience(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  Combatant* unit = combat->find(id_of(ctx.arg(0)));
  if (unit == nullptr) return HostOutcome::ok_void();
  if (ctx.arg(1).is_integer()) unit->experience = ctx.arg(1).as_integer();
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
/// Nothing else in that file can do it -- `Stop`, `Idle`, `GotoAttack` and
/// `FormKeepMoving` all suspend already, and the outer `while (1)` ends in
/// `if (.Stop(2000)) .Idle(2000);`.
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
HostOutcome host_attack(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
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
  const bool ok = combat->profile(a->class_index).can_attack() &&
                  combat->profile(d->class_index).can_be_attacked && combat->is_enemy(*a, *d);
  return HostOutcome::ok_with(Value::boolean(ok));
}

HostOutcome host_is_valid_target(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const Combatant* a = combat->find(id_of(ctx.arg(0)));
  const Combatant* d = combat->find(id_of(ctx.arg(1)));
  if (a == nullptr || d == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  const CombatProfile theirs = combat->profile(d->class_index);
  const bool ok = combat->is_enemy(*a, *d) && combat->is_alive(d->id) && d->health > 0 &&
                  theirs.can_be_attacked && !theirs.never_targeted();
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

HostOutcome host_in_range(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const Combatant* a = combat->find(id_of(ctx.arg(0)));
  const Combatant* d = combat->find(id_of(ctx.arg(1)));
  const bool ok = a != nullptr && d != nullptr && combat->in_attack_range(*a, *d);
  return HostOutcome::ok_with(Value::boolean(ok));
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

/// `u.RecalcBonuses()` -- 1 site, and **a no-op here**.
///
/// 0x005e0380 resolves the receiver and calls its `vtbl+0xc4`, which rebuilds
/// the unit's cached attack and armour bonuses from the sources that feed them.
/// `ONFINISH_RESEARCH.VS` calls it on every unit in the settlement the moment a
/// research completes, which is exactly what a cache needs and a live read does
/// not.
///
/// This engine keeps no such cache: `Combatant::attack_bonus`, `armour_bonus`,
/// `level_floor` and the rest are written by whoever owns the source at the
/// moment it changes -- the hero agent for skills, the economy for
/// `in_settlement` -- and read straight out of the combatant when a blow lands.
/// So there is nothing to rebuild, and a no-op is the same observable behaviour
/// rather than an omission. **The day a research writes a derived number that
/// something else has already copied, this stops being a no-op.**
///
/// The receiver is resolved anyway, so that the drop is a decision about a
/// named unit rather than a body that ignores its argument.
HostOutcome host_recalc_bonuses(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  (void)combat->find(id_of(ctx.arg(0)));
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
/// **Nothing is recalculated afterwards, because nothing here is cached.** The
/// original ends by calling the receiver's `vtbl+0xc4`, which rebuilds the
/// five fields from the class and adds the record back on. `CombatSystem`
/// derives all five on the spot -- see `attack_of` and the three beside it --
/// so the rebuild has already happened by the time anybody reads one. That is
/// also why `RecalcBonuses` next door is a no-op and stays one.
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

HostOutcome host_best_target(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  return HostOutcome::ok_with(ref_to(ctx, combat->best_target(id_of(ctx.arg(0)))));
}

/// One acquisition under one variant's filter. The filter itself is
/// `sim/combat.hpp`'s `TargetFilter`, which carries the disassembly.
HostOutcome best_target_with(CallContext& ctx, const TargetFilter& filter) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  return HostOutcome::ok_with(ref_to(ctx, combat->best_target(id_of(ctx.arg(0)), filter)));
}

/// `BestTargetInSquadSightMisZeroDamage/0` -- 859 of Numantia's traps, because
/// `DATA\SUBAI\UNIT_IDLE.VS` is what every idle unit runs. The one difference
/// from `BestTargetInSquadSight/0` is argument 5 of `0x005dc950`, which makes
/// `0x005db6a5` skip any candidate whose own `damage` is not positive: an idle
/// unit auto-engages things that can fight back, and leaves peasants, wagons
/// and wildlife alone until it is told otherwise.
HostOutcome host_best_target_mis_zero_damage(CallContext& ctx) {
  TargetFilter filter;
  filter.require_armed = true;
  return best_target_with(ctx, filter);
}

/// `BestNoIndependentTargetInSquadSight/0` -- argument 3, which clears bit 14
/// of the attacker's enemy mask at `0x005d45b9`. `DATA\SUBAI\HERO_SNEAK.VS` is
/// the caller: a sneaking hero picks fights with real players, not with the
/// map's wildlife and garrisons.
HostOutcome host_best_target_no_independent(CallContext& ctx) {
  TargetFilter filter;
  filter.exclude_independents = true;
  return best_target_with(ctx, filter);
}

/// `BestTargetInSquadSight_PreferUndiseased/0` -- argument 2, which adds 200
/// to a diseased candidate's score at `0x005db6e0`. **This engine has no
/// disease**: nothing sets it, and `Unit::Disease` and `Unit::IsDiseased` are
/// both unimplemented, so no candidate can carry the penalty and this returns
/// the object the executable would. It is answered rather than left trapping
/// on that ground alone -- see `TargetFilter` -- and the moment a disease
/// state exists this must stop being an alias.
HostOutcome host_best_target_prefer_undiseased(CallContext& ctx) {
  return host_best_target(ctx);
}

/// `BestTargetInSquadSightExclusive/1` -- arguments 1 and 4 together, which
/// make `0x005db774` reject every candidate whose class name is not the one
/// passed. Exact class: the executable compares the class object's own name,
/// so a subclass of the named class is not a match.
///
/// An unresolvable name, or no class graph at all, yields no candidate. That is
/// the same answer the executable gives -- its string compare simply never
/// matches -- and not a failure.
/// `BestTargetInSquadSight/1(class)` -- 6 sites, and the sole blocker of
/// `HERO_ENGAGE_UNIT_TYPE.VS` and `UNIT_ENGAGE_UNIT_TYPE.VS`.
///
/// **Its sibling's twin, and the difference is one argument.** 0x005dce00 and
/// `Exclusive`'s 0x005dcf30 both call 0x005dc950 with the string in the same
/// slot; the second passes 1 where the first passes 0, and that flag is what
/// 0x005db774 reads to decide whether a class mismatch is a rejection or a
/// 200-point penalty. So this one **prefers** the named class where `Exclusive`
/// **requires** it, and reading the pair as two spellings of one filter would
/// have been the mistake.
///
/// `TargetFilter::prefer_class` records how far the preference is reproduced
/// and where it is not.
///
/// A name no class graph resolves is **not** a failure here: the original's
/// string compare simply never matches, so every candidate takes the same
/// penalty and the ranking is the unfiltered one. That is the opposite of
/// `Exclusive`, where an unresolvable name matches nothing and the answer is no
/// candidate at all -- and the difference is not a choice, it is what a
/// rejection and a penalty each do when the test never passes.
HostOutcome host_best_target_prefer_class(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const ClassGraph* graph = combat->class_graph();
  TargetFilter filter;
  if (graph != nullptr && ctx.arg(1).is_string()) {
    filter.prefer_class = graph->find(ctx.arg(1).as_string());
  }
  return best_target_with(ctx, filter);
}

HostOutcome host_best_target_exclusive(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const ClassGraph* graph = combat->class_graph();
  ClassIndex wanted = kNoClass;
  if (graph != nullptr && ctx.arg(1).is_string()) wanted = graph->find(ctx.arg(1).as_string());
  if (wanted == kNoClass) return HostOutcome::ok_with(ref_to(ctx, kNoObject));
  TargetFilter filter;
  filter.only_class = wanted;
  return best_target_with(ctx, filter);
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
/// radius of zero or less falls back to the attacker's `sight` (`0x005dd11d`).
///
/// The executable has a second fallback to `sight` at `0x005dd123`, taken when
/// the attacker's class name is the literal `**Invalid**`. That is a
/// degenerate-object guard, not a rule about real units, and it is not
/// reproduced.
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
/// blow. `SetLastAttackTime` stays declared and unimplemented.
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
/// A target that is not a combatant, or a receiver that is not, answers the
/// target's world position when there is one and `(0, 0)` otherwise -- never a
/// refusal, because the original prints (through the sink that is a bare `ret`
/// in retail) and pushes a point either way.
HostOutcome host_get_point_on_target(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const ObjectId target = id_of(ctx.arg(1));
  const WorldObject* slot = world->find(target);
  if (slot == nullptr) return HostOutcome::ok_with(pack_point(Point{0, 0}));
  return HostOutcome::ok_with(pack_point(world->resolve_position(target)));
}

HostOutcome host_get_target(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  const Combatant* unit = combat->find(id_of(ctx.arg(0)));
  return HostOutcome::ok_with(ref_to(ctx, unit == nullptr ? kNoObject : unit->target));
}

HostOutcome host_clear_target(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
  combat->stop(id_of(ctx.arg(0)));
  return HostOutcome::ok_void();
}

HostOutcome host_set_target(CallContext& ctx) {
  CombatSystem* combat = system_of(ctx);
  if (combat == nullptr) return HostOutcome::failed(no_system_error(ctx));
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
  member("BestTargetInSquadSight", 0, &host_best_target);    //  69
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
  // **`BestTargetInSquadSight/1(str)` is deliberately left trapping.** Its
  // rule is known exactly -- `0x005dce00` passes the class string with no
  // other flag, and `0x005db789` adds 200 to the score of a candidate whose
  // class name does not match -- and it is not expressible here. The
  // executable's score is `target_factor + 40 * recent_target_count +
  // surface_distance`, a linear sum in world units; this system's is
  // `distance^2 * target_factor / target_priority`. 200 on the first scale
  // reorders candidates a couple of hundred units apart. 200 on the second is
  // indistinguishable from nothing, so implementing it would turn a soft
  // preference into a no-op and hand back a plausible object instead of the
  // right one. The corpus never calls it, so nothing is lost by waiting for a
  // score this engine can put it on.
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
  member("range", 0, &read_int<&read_range>);                //  10
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
