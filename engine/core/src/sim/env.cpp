#include "imperivm/core/sim/env.hpp"

#include <algorithm>
#include <utility>

#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {

namespace {

constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;

void fold_byte(std::uint64_t& h, std::uint8_t byte) noexcept {
  h ^= byte;
  h *= kFnvPrime;
}

void fold_bytes(std::uint64_t& h, std::uint64_t value) noexcept {
  for (int i = 0; i < 8; ++i) fold_byte(h, static_cast<std::uint8_t>(value >> (i * 8)));
}

void fold_text(std::uint64_t& h, std::string_view text) noexcept {
  for (char c : text) fold_byte(h, static_cast<std::uint8_t>(c));
  fold_byte(h, 0);
}

/// Order on scopes: kind first, then id. Deliberately explicit, because this
/// order is the store's iteration order and therefore world state.
[[nodiscard]] int compare_scope(const EnvScope& a, const EnvScope& b) noexcept {
  if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
  if (a.id != b.id) return a.id < b.id ? -1 : 1;
  return 0;
}

[[nodiscard]] int compare_entry(const EnvEntry& e, const EnvScope& scope,
                                std::string_view key) noexcept {
  const int by_scope = compare_scope(e.scope, scope);
  if (by_scope != 0) return by_scope;
  const int by_key = e.key.compare(key);
  return by_key < 0 ? -1 : (by_key > 0 ? 1 : 0);
}

}  // namespace

// --------------------------------------------------------------------------
// value coercion
// --------------------------------------------------------------------------

namespace {

/// The C locale's `isspace`: what the CRT's `atol` and `sscanf`'s `%d` skip.
[[nodiscard]] constexpr bool c_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

/// `atol` as the executable's CRT has it (Visual Studio 2003, linked at
/// 0x00763919): skip leading C-locale white space, take one `+` or `-`, then
/// accumulate decimal digits in a 32-bit register **with no overflow check**,
/// negating at the end. `digits` says whether any digit was read, which is
/// what `sscanf`'s `%d` needs and `atol` ignores.
[[nodiscard]] std::int32_t crt_atol(std::string_view text, std::size_t& i, bool& digits) noexcept {
  while (i < text.size() && c_space(text[i])) ++i;
  bool negative = false;
  if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
    negative = text[i] == '-';
    ++i;
  }
  // Unsigned, so the wrap the original's `imul`/`add` performs is defined
  // behaviour here rather than signed overflow.
  std::uint32_t value = 0;
  digits = false;
  while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
    value = value * 10u + static_cast<std::uint32_t>(text[i] - '0');
    digits = true;
    ++i;
  }
  if (negative) value = 0u - value;
  return static_cast<std::int32_t>(value);
}

}  // namespace

std::int32_t env_parse_int(std::string_view text) noexcept {
  // `EnvReadInt`'s read helper (0x006ac740) hands the stored string to `atol`
  // and returns what it says: no digits is zero, and a digit string past 2^31
  // wraps rather than saturating.
  std::size_t i = 0;
  bool digits = false;
  return crt_atol(text, i, digits);
}

std::string env_format_int(std::int32_t value) {
  if (value == 0) return "0";
  // `-2147483648` has no positive counterpart; widen before negating.
  std::int64_t wide = value;
  const bool negative = wide < 0;
  if (negative) wide = -wide;
  char buffer[16];
  std::size_t n = 0;
  while (wide > 0) {
    buffer[n++] = static_cast<char>('0' + (wide % 10));
    wide /= 10;
  }
  std::string out;
  out.reserve(n + 1);
  if (negative) out.push_back('-');
  while (n > 0) out.push_back(buffer[--n]);
  return out;
}

std::string env_format_object(ObjectId id) {
  return "$$" + env_format_int(static_cast<std::int32_t>(id)) + "$$";
}

ObjectId env_parse_object(std::string_view text) noexcept {
  // 0x006ac790: `sscanf(value, "$$%d$$", &n)` and a test that **one**
  // conversion happened. So the two leading dollars must be there literally,
  // `%d` skips white space and takes a sign, and the trailing `$$` is never
  // checked -- `"$$12"` is object 12. The original then refuses `n` outside
  // 0..65535 and indexes its object table; this engine's ids are 32-bit and
  // never reused, so only the lower bound carries over (0 is `kNoObject` here).
  if (text.size() < 2 || text[0] != '$' || text[1] != '$') return kNoObject;
  std::size_t i = 2;
  bool digits = false;
  const std::int32_t id = crt_atol(text, i, digits);
  if (!digits || id <= 0) return kNoObject;
  return static_cast<ObjectId>(id);
}

// --------------------------------------------------------------------------
// EnvStore
// --------------------------------------------------------------------------

EnvStore::Slot EnvStore::locate(const EnvScope& scope, std::string_view key) const noexcept {
  std::size_t low = 0;
  std::size_t high = entries_.size();
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    const int order = compare_entry(entries_[mid], scope, key);
    if (order == 0) return {mid, true};
    if (order < 0) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return {low, false};
}

const std::string* EnvStore::find(const EnvScope& scope, std::string_view key) const noexcept {
  const Slot slot = locate(scope, key);
  return slot.found ? &entries_[slot.index].value : nullptr;
}

std::int32_t EnvStore::read_int(const EnvScope& scope, std::string_view key) const noexcept {
  const std::string* value = find(scope, key);
  return value == nullptr ? 0 : env_parse_int(*value);
}

std::string_view EnvStore::read_string(const EnvScope& scope,
                                       std::string_view key) const noexcept {
  const std::string* value = find(scope, key);
  return value == nullptr ? std::string_view{} : std::string_view{*value};
}

ObjectId EnvStore::read_object(const EnvScope& scope, std::string_view key) const noexcept {
  const std::string* value = find(scope, key);
  return value == nullptr ? kNoObject : env_parse_object(*value);
}

void EnvStore::assign(const EnvScope& scope, std::string_view key, std::string value) {
  const Slot slot = locate(scope, key);
  if (slot.found) {
    entries_[slot.index].value = std::move(value);
    return;
  }
  EnvEntry entry;
  entry.scope = scope;
  entry.key.assign(key);
  entry.value = std::move(value);
  entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(slot.index), std::move(entry));
}

void EnvStore::write_int(const EnvScope& scope, std::string_view key, std::int32_t value) {
  assign(scope, key, env_format_int(value));
}

void EnvStore::write_string(const EnvScope& scope, std::string_view key,
                            std::string_view value) {
  assign(scope, key, std::string(value));
}

void EnvStore::write_object(const EnvScope& scope, std::string_view key, ObjectId object) {
  assign(scope, key, env_format_object(object));
}

bool EnvStore::erase(const EnvScope& scope, std::string_view key) {
  const Slot slot = locate(scope, key);
  if (!slot.found) return false;
  entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(slot.index));
  return true;
}

std::uint64_t EnvStore::hash() const noexcept {
  std::uint64_t h = kFnvOffset;
  for (const EnvEntry& entry : entries_) {
    fold_byte(h, static_cast<std::uint8_t>(entry.scope.kind));
    fold_bytes(h, entry.scope.id);
    fold_text(h, entry.key);
    fold_text(h, entry.value);
  }
  return h;
}

// --------------------------------------------------------------------------
// AiVarStore
// --------------------------------------------------------------------------

const AiVarStore::PlayerVars* AiVarStore::find(std::int32_t player) const noexcept {
  std::size_t low = 0;
  std::size_t high = players_.size();
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (players_[mid].player == player) return &players_[mid];
    if (players_[mid].player < player) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return nullptr;
}

std::int32_t AiVarStore::get(std::int32_t player, std::int32_t var) const noexcept {
  if (var < 0) return 0;
  const PlayerVars* vars = find(player);
  if (vars == nullptr) return 0;
  const auto index = static_cast<std::size_t>(var);
  return index < vars->values.size() ? vars->values[index] : 0;
}

bool AiVarStore::bit(std::int32_t player, std::int32_t var, std::int32_t which) const noexcept {
  if (which < 0 || which >= 32) return false;
  return ((get(player, var) >> which) & 1) != 0;
}

bool AiVarStore::set(std::int32_t player, std::int32_t var, std::int32_t value) {
  if (player < 0 || player > kMaxPlayer) return false;
  if (var < 0 || var >= kMaxVar) return false;
  std::size_t low = 0;
  std::size_t high = players_.size();
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (players_[mid].player < player) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  if (low >= players_.size() || players_[low].player != player) {
    PlayerVars fresh;
    fresh.player = player;
    players_.insert(players_.begin() + static_cast<std::ptrdiff_t>(low), std::move(fresh));
  }
  PlayerVars& vars = players_[low];
  const auto index = static_cast<std::size_t>(var);
  if (vars.values.size() <= index) vars.values.resize(index + 1, 0);
  vars.values[index] = value;
  return true;
}

bool AiVarStore::set_bit(std::int32_t player, std::int32_t var, std::int32_t which, bool value) {
  if (which < 0 || which >= 32) return false;
  std::int32_t current = get(player, var);
  const std::int32_t mask = static_cast<std::int32_t>(1u << static_cast<unsigned>(which));
  current = value ? (current | mask) : (current & ~mask);
  return set(player, var, current);
}

std::uint64_t AiVarStore::hash() const noexcept {
  std::uint64_t h = kFnvOffset;
  for (const PlayerVars& vars : players_) {
    fold_bytes(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(vars.player)));
    for (std::int32_t value : vars.values) {
      fold_bytes(h, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
    }
    fold_byte(h, 0xFF);
  }
  return h;
}

// --------------------------------------------------------------------------
// research catalog
// --------------------------------------------------------------------------

namespace {

/// One `ParseStr` step: the head token up to the next comma, trimmed.
///
/// `VERIFY_RESEARCH.VS` reads the `param` attribute with `ParseStr(dest, dest)`
/// and the shipped attributes are comma-separated with irregular spacing
/// (`"ReqSet, Fights, , ReqPlr, Training, , SetsPlr, maxtrainlevel, 8, …"`), so
/// an empty field between two commas is a real, meaningful token.
[[nodiscard]] std::string_view next_token(std::string_view& rest) noexcept {
  const std::size_t comma = rest.find(',');
  std::string_view head = comma == std::string_view::npos ? rest : rest.substr(0, comma);
  rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
  while (!head.empty() && (head.front() == ' ' || head.front() == '\t')) head.remove_prefix(1);
  while (!head.empty() && (head.back() == ' ' || head.back() == '\t')) head.remove_suffix(1);
  return head;
}

}  // namespace

ResearchCommand parse_research_command(std::string_view name, std::string_view param) {
  ResearchCommand command;
  command.name.assign(name);

  std::string_view rest = param;
  while (!rest.empty()) {
    const std::string_view verb = next_token(rest);
    if (verb.empty()) continue;
    const std::string_view subject = next_token(rest);
    const std::string_view value = next_token(rest);

    if (verb == "NameSet" || verb == "NamePlr") {
      command.scope =
          verb == "NameSet" ? ResearchScope::settlement : ResearchScope::player;
      command.records.assign(subject);
      continue;
    }
    if (verb == "ReqSet" || verb == "ReqPlr" || verb == "NReqSet") {
      ResearchRequirement requirement;
      requirement.scope =
          verb == "ReqPlr" ? ResearchScope::player : ResearchScope::settlement;
      requirement.name.assign(subject);
      requirement.negated = verb == "NReqSet";
      command.requires_.push_back(std::move(requirement));
      continue;
    }
    if (verb == "SetsSet" || verb == "SetsPlr") {
      ResearchAssignment assignment;
      assignment.scope =
          verb == "SetsSet" ? ResearchScope::settlement : ResearchScope::player;
      assignment.key.assign(subject);
      assignment.value.assign(value);
      command.assigns.push_back(std::move(assignment));
      continue;
    }
    // Anything else is a verb we do not model. The third field of every
    // recognised triple is a UI explanation, never simulation state, so
    // dropping it is deliberate.
  }

  // A command with no `NameSet`/`NamePlr` records nothing, but the corpus never
  // ships one: all 145 name a ledger.
  if (command.scope != ResearchScope::none && command.records.empty()) {
    command.records = command.name;
  }
  return command;
}

void ResearchCatalog::build_from(const CommandTable& table) {
  commands_.clear();
  for (const CommandDef& def : table.commands()) {
    // `method="research"` is 145 of the 399 shipped rows; `immediate_research`
    // is 6 more and differs only in skipping `execdelay`
    // (`IMMEDIATE_RESEARCH.VS` is `RESEARCH.VS` without the `.Progress()`).
    if (def.method != "research" && def.method != "immediate_research") continue;
    ResearchCommand command = parse_research_command(def.name, def.param);
    command.cost_gold = def.cost_gold;
    command.cost_food = def.cost_food;
    add(std::move(command));
  }
}

void ResearchCatalog::add(ResearchCommand command) {
  const auto position = std::lower_bound(
      commands_.begin(), commands_.end(), command.name,
      [](const ResearchCommand& lhs, const std::string& key) { return lhs.name < key; });
  if (position != commands_.end() && position->name == command.name) {
    *position = std::move(command);
    return;
  }
  commands_.insert(position, std::move(command));
}

const ResearchCommand* ResearchCatalog::find(std::string_view name) const noexcept {
  const auto position = std::lower_bound(
      commands_.begin(), commands_.end(), name,
      [](const ResearchCommand& lhs, std::string_view key) { return lhs.name < key; });
  if (position == commands_.end() || position->name != name) return nullptr;
  return &*position;
}

// --------------------------------------------------------------------------
// unit catalog
// --------------------------------------------------------------------------

const RaceUnits* UnitCatalog::race(std::int32_t race) const noexcept {
  if (race < 0) return nullptr;
  const auto index = static_cast<std::size_t>(race);
  return index < races_.size() ? &races_[index] : nullptr;
}

const UnitRow* UnitCatalog::row(std::int32_t race_index, std::int32_t type) const noexcept {
  const RaceUnits* units = race(race_index);
  if (units == nullptr || type < 0) return nullptr;
  const auto index = static_cast<std::size_t>(type);
  return index < units->rows.size() ? &units->rows[index] : nullptr;
}

std::string_view UnitCatalog::block(std::int64_t index) const noexcept {
  constexpr std::int64_t kRaceCount = 8;
  constexpr std::int64_t kTypes = 6;
  if (index < 0 || races_.size() != static_cast<std::size_t>(kRaceCount)) return {};
  // Three six-wide tables, then five one-per-race tables.
  for (int column = 0; column < 3; ++column) {
    if (index < kRaceCount * kTypes) {
      const std::vector<UnitRow>& rows = races_[static_cast<std::size_t>(index / kTypes)].rows;
      if (rows.size() != static_cast<std::size_t>(kTypes)) return {};
      const UnitRow& row = rows[static_cast<std::size_t>(index % kTypes)];
      return column == 0 ? std::string_view{row.type}
                         : column == 1 ? std::string_view{row.tech} : std::string_view{row.train_cmd};
    }
    index -= kRaceCount * kTypes;
  }
  for (int column = 0; column < 5; ++column) {
    if (index < kRaceCount) {
      const RaceUnits& units = races_[static_cast<std::size_t>(index)];
      switch (column) {
        case 0: return units.arena.type;
        case 1: return units.arena.train_cmd;
        case 2: return units.arena.tech;
        case 3: return units.temple.type;
        default: return units.temple.train_cmd;
      }
    }
    index -= kRaceCount;
  }
  return {};
}

std::int32_t UnitCatalog::index_of(std::int32_t race_index,
                                   std::string_view type) const noexcept {
  const RaceUnits* units = race(race_index);
  if (units == nullptr) return -1;
  for (std::size_t i = 0; i < units->rows.size(); ++i) {
    if (units->rows[i].type == type) return static_cast<std::int32_t>(i);
  }
  return -1;
}

namespace {

/// The engine table, transcribed from `gbr.exe`'s string pool. See
/// `shipped_unit_catalog`'s declaration for the three-way corroboration.
///
/// An empty `tech` is the table's own hole, not ours: Germany has none at all
/// (`ESH_BUILDGERMANARMY.VS` carries `Axemen production` and its three siblings
/// in a script-side `StrArray` instead of calling `UTech`), and the Roman and
/// Egyptian rows have the gaps the barrack files have.
struct RawRow {
  const char* type;
  const char* tech;
  const char* train_cmd;
};

struct RawRace {
  const char* name;
  RawRow rows[6];
  RawRow arena;
  RawRow temple;
};

constexpr RawRace kRaces[] = {
    {"Gaul",
     {{"GSwordsman", "", "trainGSwordsman"},
      {"GArcher", "", "trainGArcher"},
      {"GAxeman", "Gaul Iron Axes", "trainGAxeman"},
      {"GSpearman", "Gaul Iron Spearheads", "trainGSpearman"},
      {"GHorseman", "Gaul Horseman", "trainGHorseman"},
      {"GWomanWarrior", "Gaul Fine Armor", "trainGWomanWarrior"}},
     {"GTridentWarrior", "Fights", "Hire Trident warrior"},
     {"GDruid", "", "trainGDruid"}},
    {"RepublicanRome",
     {{"RHastatus", "", "trainRHastatus"},
      {"RArcher", "Roman Archers", "trainRArcher"},
      {"RGladiator", "RGladiator Shows", "trainRGladiator"},
      {"RPrinciple", "Roman Principle", "trainRPrinciple"},
      {"RScout", "Roman Scout", "trainRScout"},
      {"RTribune", "Roman Full Armor", "trainRTribune"}},
     {"RChariot", "RGladiator Shows", "Equip Chariot"},
     {"RPriest", "", "trainRPriest"}},
    {"Carthage",
     {{"CLibyanFootman", "Forge spears", "trainCLibyanFootman"},
      {"CJavelinThrower", "Forge javelins", "trainCJavelinThrower"},
      {"CBerberAssassin", "Forge swords", "trainCBerberAssassin"},
      {"CMaceman", "Forge maces", "trainCMaceman"},
      {"CNumidianRider", "Forge pikes", "trainCNumidianRider"},
      {"CNoble", "Sacred Legion", "trainCNoble"}},
     {"CWarElephant", "People's Assembly", "Train War Elephant"},
     {"CShaman", "", "trainCShaman"}},
    {"Iberia",
     {{"IMilitiaman", "", "trainIMilitiaman"},
      {"IArcher", "", "trainIArcher"},
      {"IDefender", "Spears", "trainIDefender"},
      {"ICavalry", "Horseshoes", "trainICavalry"},
      {"ISlinger", "Slings", "trainISlinger"},
      {"IEliteGuard", "Battleaxes", "trainIEliteGuard"}},
     {"IMountaineer", "Tournaments", "Call Mountaineer"},
     {"IEnchantress", "", "trainIEnchantress"}},
    {"ImperialRome",
     {{"RHastatus", "", "trainMHastatus"},
      {"RArcher", "Roman Archers", "trainMArcher"},
      {"RVelit", "Roman Velit", "trainMVelit"},
      {"RPrinciple", "Roman Principle", "trainMPrinciple"},
      {"RScout", "Roman Scout", "trainMScout"},
      {"RPraetorian", "Roman Praetorian", "trainMPraetorian"}},
     {"RLiberatus", "MGladiator Shows", "Hire Liberati"},
     {"RPriest", "", "trainRPriest"}},
    {"Britain",
     {{"BSwordsman", "", "trainBSwordsman"},
      {"BBowman", "", "trainBBowman"},
      {"BBronzeSpearman", "Britain Spears", "trainBBronzeSpearman"},
      {"BShieldBearer", "Britain Large Shields", "trainBShieldBearer"},
      {"BJavelineer", "Britain Javelins", "trainBJavelineer"},
      {"BHighlander", "Britain Swords", "trainBHighlander"}},
     {"BVikingLord", "Britain Fights", "Hire Viking Lord"},
     // The temple-train table (0x0081a648) has **seven** strings for eight
     // races: `trainBDruid` -- the command `BTEMPLEOFTHOR.XML` offers -- is
     // missing, so everything after Iberia's Imperial Rome entry slides up one
     // slot. Britain's slot holds `trainEPriest`, Egypt's `trainTEnchantress`,
     // and Germany's is a null pointer. Reproduced as the executable has it.
     {"BDruid", "", "trainEPriest"}},
    {"Egypt",
     {{"ESwordsman", "", "trainESwordsman"},
      {"EArcher", "", "trainEArcher"},
      {"EAxetrower", "", "trainEAxeTrower"},
      {"EAnubisWarrior", "Cult of Anubis", "trainEAnubisWarrior"},
      {"EHorusWarrior", "Cult of Horus", "trainEHorusWarrior"},
      {"EGuardian", "", "trainEGuardian"}},
     // `ETEMPLEOFOSIRIS.XML` gates `Chariot of Osiris` on `Cult of Osiris`,
     // which `gbr.exe` does not carry; its arena-tech table (0x0081a608) holds
     // `Cult of Anubis` in the Egyptian slot (0x0081a620), the same string as
     // Egypt's fourth barrack tech. So an Egyptian AI's arena recruit waits on
     // the Anubis cult, as it does in the retail game.
     {"EChariot", "Cult of Anubis", "Chariot of Osiris"},
     // The slid temple-train slot; see Britain.
     {"EPriest", "", "trainTEnchantress"}},
    {"Germany",
     {{"TSwordsman", "", "trainTSwordsman"},
      {"TArcher", "", "trainTArcher"},
      {"TAxeman", "", "trainTAxeman"},
      {"THuntress", "", "trainTHuntress"},
      {"TTeutonRider", "", "trainTTeutonRider"},
      {"TMaceman", "", "trainTMaceman"}},
     {"TValkyrie", "TFights", "Call Valkyries"},
     // A null pointer in the original (0x0081a664), which `TempleUTrain`
     // hands to `strlen` (0x00695e10) -- a fault, not an answer. Empty here.
     {"TEnchantress", "", ""}},
};

[[nodiscard]] UnitRow to_row(const RawRow& raw) {
  UnitRow row;
  row.type = raw.type;
  row.tech = raw.tech;
  row.train_cmd = raw.train_cmd;
  return row;
}

}  // namespace

UnitCatalog shipped_unit_catalog() {
  std::vector<RaceUnits> races;
  races.reserve(sizeof(kRaces) / sizeof(kRaces[0]));
  for (const RawRace& raw : kRaces) {
    RaceUnits units;
    units.name = raw.name;
    units.rows.reserve(6);
    for (const RawRow& row : raw.rows) units.rows.push_back(to_row(row));
    units.arena = to_row(raw.arena);
    units.temple = to_row(raw.temple);
    races.push_back(std::move(units));
  }
  UnitCatalog catalog;
  catalog.set_races(std::move(races));
  return catalog;
}

// --------------------------------------------------------------------------
// EnvSystem
// --------------------------------------------------------------------------

void EnvSystem::start(World& world) {
  // The research tree is the command table's `method="research"` rows. Building
  // it here rather than asking the embedder to do it separately means there is
  // one parse of `DATA\COMMANDS\*.XML` and one place a research's cost can
  // come from -- the same `CommandDef` `GetCmdCost` reads.
  if (const CommandSystem* commands = command_system(world)) {
    if (!commands->table().empty()) research_.build_from(commands->table());
  }
}

void EnvSystem::advance(World& world, const Turn& turn) {
  // Deliberately nothing. The stores are written by scripts and read by
  // scripts; no timer touches them. Present because `System` is the seam that
  // gets state onto the world and into the world hash.
  (void)world;
  (void)turn;
}

void EnvSystem::hash(std::uint64_t& accumulator) const {
  const auto mix = [&accumulator](std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      accumulator ^= static_cast<std::uint8_t>(value >> (i * 8));
      accumulator *= kFnvPrime;
    }
  };
  mix(env_.hash());
  mix(ai_vars_.hash());
}

std::int32_t ini_number(std::string_view text) noexcept {
  // 0x0063f200, the INI's integer getter. Leading spaces and tabs go (the
  // trailing trim it also calls, 0x0063ec30, starts on the terminator and so
  // trims nothing -- harmless, because the parse stops at the first misfit).
  std::size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  std::int32_t sign = 1;
  if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
    if (text[i] == '-') sign = -1;
    ++i;
  }
  // 32-bit arithmetic as the original's; unsigned so a wrap is defined here.
  std::uint32_t value = 0;
  if (i + 1 < text.size() && text[i] == '0' && text[i + 1] == 'x') {
    for (i += 2; i < text.size(); ++i) {
      const char c = text[i];
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
      value = value * 16u + digit;
    }
  } else {
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
      value = value * 10u + static_cast<std::uint32_t>(text[i] - '0');
    }
  }
  return static_cast<std::int32_t>(sign < 0 ? 0u - value : value);
}

std::size_t EnvSystem::load_constants(const IniDocument& document) {
  const SectionIndex section = document.section("GamePlay");
  if (section == kNoSection) return 0;
  std::size_t taken = 0;
  for (const IniEntry& entry : document.entries_of(section)) {
    if (!entry.has_key || entry.key.empty()) continue;  // a bare line is not a constant
    // Every key has both readings, as the original's one INI store does:
    // `GetConstStr` the text, `GetConst` (and every engine reader) the number
    // `ini_number` makes of it -- `"500, 1000"` is 500 to the latter.
    set_constant_string(entry.key, entry.value);
    set_constant(entry.key, ini_number(entry.value));
    ++taken;
  }
  return taken;
}

bool EnvSystem::constant(std::string_view key, std::int32_t& out) const {
  const auto position =
      std::lower_bound(int_constants_.begin(), int_constants_.end(), key,
                       [](const std::pair<std::string, std::int32_t>& lhs,
                          std::string_view rhs) { return lhs.first < rhs; });
  if (position == int_constants_.end() || position->first != key) return false;
  out = position->second;
  return true;
}

bool EnvSystem::constant_string(std::string_view key, std::string_view& out) const {
  const auto position = std::lower_bound(
      str_constants_.begin(), str_constants_.end(), key,
      [](const std::pair<std::string, std::string>& lhs, std::string_view rhs) {
        return lhs.first < rhs;
      });
  if (position == str_constants_.end() || position->first != key) return false;
  out = position->second;
  return true;
}

void EnvSystem::set_constant(std::string_view key, std::int32_t value) {
  const auto position =
      std::lower_bound(int_constants_.begin(), int_constants_.end(), key,
                       [](const std::pair<std::string, std::int32_t>& lhs,
                          std::string_view rhs) { return lhs.first < rhs; });
  if (position != int_constants_.end() && position->first == key) {
    position->second = value;
    return;
  }
  int_constants_.insert(position, {std::string(key), value});
}

void EnvSystem::set_constant_string(std::string_view key, std::string_view value) {
  const auto position = std::lower_bound(
      str_constants_.begin(), str_constants_.end(), key,
      [](const std::pair<std::string, std::string>& lhs, std::string_view rhs) {
        return lhs.first < rhs;
      });
  if (position != str_constants_.end() && position->first == key) {
    position->second.assign(value);
    return;
  }
  str_constants_.insert(position, {std::string(key), std::string(value)});
}

// -- AI variables ----------------------------------------------------------

void EnvSystem::set_ai_var_names(std::vector<std::string> names) {
  ai_var_names_ = std::move(names);
}

void EnvSystem::seed_ai_vars(std::int32_t player, const AiProfile& profile,
                             AiDifficulty difficulty) {
  const std::vector<AiVariable> variables = profile.variables(difficulty);
  if (ai_var_names_.empty()) {
    ai_var_names_.reserve(variables.size());
    for (const AiVariable& variable : variables) ai_var_names_.push_back(variable.name);
  }
  for (std::size_t i = 0; i < variables.size(); ++i) {
    ai_vars_.set(player, static_cast<std::int32_t>(i), variables[i].value);
  }
}

std::int32_t EnvSystem::ai_var_id(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < ai_var_names_.size(); ++i) {
    if (ai_var_names_[i] == name) return static_cast<std::int32_t>(i);
  }
  return -1;
}

std::string_view EnvSystem::ai_var_name(std::int32_t id) const noexcept {
  if (id < 0) return {};
  const auto index = static_cast<std::size_t>(id);
  return index < ai_var_names_.size() ? std::string_view{ai_var_names_[index]}
                                      : std::string_view{};
}

// -- research --------------------------------------------------------------

namespace {

/// The ledger of a settlement's owner, **by the owner's script number**.
///
/// A player's ledger is the one `RESEARCH.VS` and `ONFINISH_RESEARCH.VS`
/// write with `EnvWriteString(.player, name, …)`, and `.player` is the owner's
/// index plus one (0x005ab290); `VERIFY_RESEARCH.VS` reads it back the same
/// way. gbr.exe's own readers agree: `IsResearched(Settlement, str)` builds
/// the path from the settlement's player id plus one (the `inc` at
/// 0x00433a15), and the training cap 0x00425d60 does the same for
/// `maxtrainlevel` (0x00425d85). This read the raw owner for a long time, so
/// every settlement-side question about a `NamePlr` research -- `IsResearched`,
/// `CanResearch`'s requirements, `TSResearch` -- answered from the ledger of
/// the player one slot below, and a computer player whose tactic waits on
/// "Rome Training 1" or "Fights" waited forever, its army held out of the AI.
[[nodiscard]] EnvScope owner_scope(const Settlement& s) noexcept {
  return EnvScope::for_player(player_to_script(s.owner));
}

}  // namespace

EnvSystem::ResearchSlot EnvSystem::slot_for(const Settlement& s, std::string_view name) const {
  ResearchSlot slot;
  const ResearchCommand* command = research_.find(name);
  if (command == nullptr || command->scope == ResearchScope::none) {
    slot.known = false;
    slot.scope = EnvScope::for_settlement(s.id);
    slot.key = name;
    return slot;
  }
  slot.known = true;
  slot.key = command->records;
  slot.scope = command->scope == ResearchScope::player
                   ? owner_scope(s)
                   : EnvScope::for_settlement(s.id);
  return slot;
}

namespace {

/// `EnvReadString(scope, name) == state`, the test both `VERIFY_RESEARCH.VS`
/// and `VERIFY_ISRESEARCHED.VS` write out by hand.
[[nodiscard]] bool ledger_is(const EnvStore& store, const EnvScope& scope,
                             std::string_view name, std::string_view state) {
  return store.read_string(scope, name) == state;
}

}  // namespace

bool EnvSystem::is_researched(const Settlement& s, std::string_view name) const {
  const ResearchSlot slot = slot_for(s, name);
  if (slot.known) return ledger_is(env_, slot.scope, slot.key, kResearched);
  // No catalog: read the settlement's ledger, then its owner's. That is
  // `VERIFY_ISRESEARCHED.VS`'s own fallback, and it is what makes
  // `ESH_NEEDTECH.VS` work with a single call for both kinds of research.
  if (ledger_is(env_, EnvScope::for_settlement(s.id), name, kResearched)) return true;
  return ledger_is(env_, owner_scope(s), name,
                   kResearched);
}

bool EnvSystem::is_researching(const Settlement& s, std::string_view name) const {
  const ResearchSlot slot = slot_for(s, name);
  if (slot.known) return ledger_is(env_, slot.scope, slot.key, kResearching);
  if (ledger_is(env_, EnvScope::for_settlement(s.id), name, kResearching)) return true;
  return ledger_is(env_, owner_scope(s), name,
                   kResearching);
}

bool EnvSystem::is_researched_for_player(std::int32_t player, std::string_view name) const {
  const ResearchCommand* command = research_.find(name);
  const std::string_view key = command == nullptr ? name : std::string_view{command->records};
  return ledger_is(env_, EnvScope::for_player(player), key, kResearched);
}

bool EnvSystem::is_researching_for_player(std::int32_t player, std::string_view name) const {
  const ResearchCommand* command = research_.find(name);
  const std::string_view key = command == nullptr ? name : std::string_view{command->records};
  return ledger_is(env_, EnvScope::for_player(player), key, kResearching);
}

bool EnvSystem::can_research(const Settlement& s, std::string_view name) const {
  if (is_researched(s, name)) return false;
  if (is_researching(s, name)) return false;
  const ResearchCommand* command = research_.find(name);
  if (command == nullptr) {
    // Nothing declares this name. With no catalog loaded that is the normal
    // case and "not already done" is the whole answer; with one loaded it means
    // the name is not a research command, and the engine says so
    // (`No such upgrade %s`). We cannot tell the two apart from here, so the
    // catalog's emptiness decides.
    return research_.size() == 0;
  }
  const EnvScope settlement_scope = EnvScope::for_settlement(s.id);
  const EnvScope player_scope = owner_scope(s);
  for (const ResearchRequirement& requirement : command->requires_) {
    const EnvScope& scope =
        requirement.scope == ResearchScope::player ? player_scope : settlement_scope;
    const bool done = ledger_is(env_, scope, requirement.name, kResearched);
    if (requirement.negated ? done : !done) return false;
  }
  return true;
}

bool EnvSystem::begin_research(const Settlement& s, std::string_view name) {
  const ResearchSlot slot = slot_for(s, name);
  env_.write_string(slot.scope, slot.key, kResearching);
  return true;
}

bool EnvSystem::finish_research(const Settlement& s, std::string_view name) {
  const ResearchSlot slot = slot_for(s, name);
  env_.write_string(slot.scope, slot.key, kResearched);
  const ResearchCommand* command = research_.find(name);
  if (command == nullptr) return true;
  const EnvScope settlement_scope = EnvScope::for_settlement(s.id);
  const EnvScope player_scope = owner_scope(s);
  for (const ResearchAssignment& assignment : command->assigns) {
    const EnvScope& scope =
        assignment.scope == ResearchScope::player ? player_scope : settlement_scope;
    // `ONFINISH_RESEARCH.VS` writes these with `EnvWriteString`, values and
    // all: `SetsPlr, maxtrainlevel, 4` stores the *text* "4", which
    // `UNIT_TRAIN.VS` then reads back with `EnvReadInt`.
    env_.write_string(scope, assignment.key, assignment.value);
  }
  return true;
}

bool EnvSystem::cancel_research(const Settlement& s, std::string_view name) {
  const ResearchSlot slot = slot_for(s, name);
  env_.write_string(slot.scope, slot.key, "");
  return true;
}

EnvSystem* env_of(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "env") {
      return static_cast<EnvSystem*>(system);
    }
  }
  return nullptr;
}

// --------------------------------------------------------------------------
// host functions
// --------------------------------------------------------------------------

namespace {

using script::CallContext;
using script::CallKind;
using script::HostFn;
using script::HostOutcome;
using script::ObjectRef;
using script::Value;

[[nodiscard]] EnvSystem* host_env(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  return world == nullptr ? nullptr : env_of(*world);
}

[[nodiscard]] HostOutcome no_env() {
  return HostOutcome::failed("no env system on the world");
}

[[nodiscard]] HostOutcome integer(std::int32_t value) {
  return HostOutcome::ok_with(Value::integer(value));
}

[[nodiscard]] HostOutcome boolean(bool value) {
  return HostOutcome::ok_with(Value::boolean(value));
}

[[nodiscard]] HostOutcome text(std::string_view value) {
  return HostOutcome::ok_with(Value::string(std::string(value)));
}

[[nodiscard]] Value invalid_handle() { return Value::object(ObjectRef{}); }

[[nodiscard]] std::int32_t int_arg(CallContext& ctx, std::size_t index) noexcept {
  if (ctx.count() <= index) return 0;
  const Value& value = ctx.arg(index);
  return value.is_integer() ? value.as_integer() : 0;
}

/// The key argument. A non-string is a hard error rather than an empty key: a
/// silently empty key would collide every miswritten call onto one slot.
[[nodiscard]] bool key_arg(CallContext& ctx, std::size_t index, std::string_view& out) {
  if (ctx.count() <= index || !ctx.arg(index).is_string()) return false;
  out = ctx.arg(index).as_string();
  return true;
}

/// How argument 0 of a scoped `Env*` call resolved.
enum class ScopeArg : std::uint8_t {
  ok,
  /// A handle naming nothing: gbr.exe's "uninitialized or invalid object".
  /// Not a trap -- see `env_target`.
  invalid_object,
  /// Neither an integer nor a handle. No overload of the original takes such
  /// a value, so its compiler never emits the call; here it is a type error.
  bad_shape,
};

/// Resolve argument 0 of an `Env*` call to a scope.
///
/// gbr.exe registers each name four times with a declared first-parameter
/// type, and the path each body formats says what the scope is:
///
///   * `int plr` -- `/<map>/Player%d/<key>` with the integer **verbatim**, no
///     range check and no adjustment (0x006adb20 and its five siblings). A
///     script that passes `.player` therefore addresses the 1-based slot, and
///     one that passes a raw owner index addresses the slot one below;
///   * `Settlement set` -- `/<map>/Settlement%d/<key>` with the settlement's
///     id (0x006ad3e0);
///   * `Building b` -- `/<map>/Settlement%d/Bld%d/<key>`, the building's own
///     settlement id and its object index (0x006acc60).
///
/// Here the value's shape picks the overload, as the original's compiler does
/// from its static type: an integer is a player, a `kTypeSettlement` handle a
/// settlement, any other handle a building. An ordinary object handle is
/// **not** mapped back to its settlement even when it is that settlement's
/// anchor: `ONFINISH_RESEARCH.VS` writes `EnvWriteString(this, "researching",
/// "no")` and `EnvWriteString(.settlement, name, "researched")` on one town
/// hall in one run, and the two paths differ.
///
/// The building scope is keyed by object id rather than by `Settlement/Bld`;
/// see `EnvScope` for why the relabelling is invisible to script.
///
/// A handle is invalid when it names no live object (the original's table
/// slot is empty) or, for a settlement, no settlement.
[[nodiscard]] ScopeArg scope_arg(CallContext& ctx, EnvScope& out) {
  if (ctx.count() == 0) return ScopeArg::bad_shape;
  const Value& value = ctx.arg(0);
  if (value.is_integer()) {
    out = EnvScope::for_player(value.as_integer());
    return ScopeArg::ok;
  }
  if (!value.is_object()) return ScopeArg::bad_shape;
  const ObjectRef ref = value.as_object();
  if (!ref.valid()) return ScopeArg::invalid_object;
  World* world = world_of(ctx);
  if (ref.type == kTypeSettlement) {
    EconomySystem* economy = world == nullptr ? nullptr : economy_of(*world);
    const Settlement* s =
        economy == nullptr ? nullptr : economy->settlements().for_object(ref.id);
    if (s == nullptr) return ScopeArg::invalid_object;
    out = EnvScope::for_settlement(s->id);
    return ScopeArg::ok;
  }
  if (world != nullptr && world->find(ref.id) == nullptr) return ScopeArg::invalid_object;
  out = EnvScope::for_building(ref.id);
  return ScopeArg::ok;
}

[[nodiscard]] HostOutcome bad_scope() {
  return HostOutcome::failed("Env*: the scope argument is neither a player number nor a handle");
}

[[nodiscard]] HostOutcome bad_key() {
  return HostOutcome::failed("Env*: the key argument is not a string");
}

/// The running script thread's `Context`, which `MakePath` (0x006ab0e0)
/// prepends to a root key that does not start with `/`.
///
/// The field is a string on the script thread (serialised as `Context`,
/// 0x0069cc58), null by default, and three launchers set it -- the sequence
/// runner to `/`, the AI's launcher to its seat, a conversation's phrase
/// runner to `/<name>/`; `script::ScriptRecord::context` has the addresses.
/// The VM hands it to every host call, so this is a read of the call's own
/// thread and nothing else: a synchronous call -- a class hook, `Run`, the
/// member `AIRun`, the `TS*` trampolines -- runs on a fresh thread with none,
/// as 0x0069dca0's does.
[[nodiscard]] std::string_view thread_context(CallContext& ctx) { return ctx.context; }

/// One resolved `Env*` call: which scope, which key, where a write's value sits.
///
/// Every `Env*` name is registered at **two** arities, and the shorter one drops
/// the scope argument. `gbr.exe` declares both — `int, str key` at 0x006ae7f7
/// beside `int, int plr, str key` at 0x006ae983, and the same pairing for the
/// other five — and the containers use the shorter form **403 times**, every one
/// of them in a `Sequences/` script.
struct EnvTarget {
  EnvScope scope;
  std::string key;
  std::size_t value_index = 0;  ///< where a write's value argument sits
  bool ok = false;
  /// The scope handle named nothing. The original prints "Parameter
  /// #Settlement in function '...' is uninitialized or invalid object." (or
  /// `#Building`) through the logger that is an empty function in the retail
  /// build (0x00686eb0), then **returns normally**: a read pushes its type's
  /// default -- 0, `""` (0x007ab85a) or the invalid handle -- and a write
  /// stores nothing. Every one of the twelve scoped bodies does exactly that.
  bool invalid_scope = false;
  HostOutcome failure;  ///< what to return when `ok` is false
};

[[nodiscard]] EnvTarget env_target(CallContext& ctx, bool root) {
  EnvTarget out;
  if (ctx.count() == 0) {
    out.failure = bad_scope();
    return out;
  }
  if (root) {
    if (!ctx.arg(0).is_string()) {
      out.failure = bad_key();
      return out;
    }
    // The root scope. The six root thunks (0x006ac890 and siblings) pass the
    // script's string straight to the registry, whose get (0x006ac5c0) and
    // set (0x006ac4a0) both funnel it through `MakePath` (0x006ab0e0): a path
    // whose first character is `/`, or a thread with no context, is used as
    // it is; anything else gets the thread's context prepended with no
    // separator. The registry is a `std::map` keyed by the resulting string
    // with `std::string::compare` (0x00409350), so keys are case-sensitive.
    out.scope = EnvScope::root();
    const std::string_view key = ctx.arg(0).as_string();
    const std::string_view context = thread_context(ctx);
    if (!context.empty() && (key.empty() || key.front() != '/')) {
      out.key.reserve(context.size() + key.size());
      out.key.append(context);
    }
    out.key.append(key);
    out.value_index = 1;
    out.ok = true;
    return out;
  }
  switch (scope_arg(ctx, out.scope)) {
    case ScopeArg::ok:
      break;
    case ScopeArg::invalid_object:
      out.invalid_scope = true;
      break;
    case ScopeArg::bad_shape:
      out.failure = bad_scope();
      return out;
  }
  std::string_view key;
  if (!key_arg(ctx, 1, key)) {
    out.failure = bad_key();
    return out;
  }
  out.key.assign(key);
  out.value_index = 2;
  out.ok = true;
  return out;
}

/// The settlement behind a member call's receiver, for the research members.
[[nodiscard]] Settlement* settlement_receiver(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  if (world == nullptr || ctx.count() == 0) return nullptr;
  EconomySystem* economy = economy_of(*world);
  if (economy == nullptr) return nullptr;
  const Value& value = ctx.arg(0);
  if (!value.is_object()) return nullptr;
  const ObjectRef ref = value.as_object();
  if (ref.type == kTypeSettlement) return economy->settlements().for_object(ref.id);
  if (ref.type != kTypeObj) return nullptr;
  const WorldObject* slot = world->find(ref.id);
  if (slot != nullptr && slot->settlement != kNoObject) {
    Settlement* s = economy->settlements().for_object(slot->settlement);
    if (s != nullptr) return s;
  }
  return economy->settlements().for_object(ref.id);
}

// -- the environment store ------------------------------------------------
//
// The twenty-four bodies are six names times four overloads (root, Settlement,
// Building, int), registered at 0x006ae7b0 (root) and 0x006ae941 (scoped).
// Each scoped body pops its arguments, resolves the scope (`scope_arg`), and
// formats the path with the map's name in front -- `/%s/Player%d/%s` and its
// two siblings -- which no script can observe, so the scope and key are kept
// as a pair here instead. Then:
//
//   * `EnvReadInt` -- the read helper 0x006ac740: look the path up (0x006ac5c0;
//     a missing key is `""`) and `atol` it (`env_parse_int`). Pushes an int.
//   * `EnvWriteInt` -- `ltoa(value, 10)` and set (0x006ac4a0): `map[path] =
//     text`. Decimal with a leading `-` is `env_format_int`.
//   * `EnvReadString` / `EnvWriteString` -- the same get and set with the
//     string as it is. An empty value is stored, never erased.
//   * `EnvReadObj` -- 0x006ac790 (`env_parse_object`) and then the live object
//     at that index; a key that decodes to nothing, or to an object no longer
//     there, pushes the invalid handle.
//   * `EnvWriteObj` -- `$$%d$$` of the object's index; an invalid handle is
//     written as index 0xffff (`$$65535$$`), which reads back as nothing. This
//     engine writes `$$0$$` for it (`env_format_object(kNoObject)`), which
//     also reads back as nothing; the stored text differs only to a script
//     that reads an object key with `EnvReadString`, which none does.
//
// No body suspends, draws a random number or touches anything but the
// registry. An invalid scope handle is the default answer (`EnvTarget`).

HostOutcome env_read_int(CallContext& ctx, bool root) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const EnvTarget target = env_target(ctx, root);
  if (!target.ok) return target.failure;
  if (target.invalid_scope) return integer(0);
  return integer(env->env().read_int(target.scope, target.key));
}

HostOutcome env_write_int(CallContext& ctx, bool root) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const EnvTarget target = env_target(ctx, root);
  if (!target.ok) return target.failure;
  if (target.invalid_scope) return HostOutcome::ok_void();
  env->env().write_int(target.scope, target.key, int_arg(ctx, target.value_index));
  return HostOutcome::ok_void();
}

HostOutcome env_read_string(CallContext& ctx, bool root) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const EnvTarget target = env_target(ctx, root);
  if (!target.ok) return target.failure;
  if (target.invalid_scope) return text({});
  return text(env->env().read_string(target.scope, target.key));
}

HostOutcome env_write_string(CallContext& ctx, bool root) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const EnvTarget target = env_target(ctx, root);
  if (!target.ok) return target.failure;
  if (ctx.count() <= target.value_index || !ctx.arg(target.value_index).is_string()) {
    return HostOutcome::failed("EnvWriteString: the value argument is not a string");
  }
  if (target.invalid_scope) return HostOutcome::ok_void();
  env->env().write_string(target.scope, target.key, ctx.arg(target.value_index).as_string());
  return HostOutcome::ok_void();
}

HostOutcome env_read_obj(CallContext& ctx, bool root) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const EnvTarget target = env_target(ctx, root);
  if (!target.ok) return target.failure;
  if (target.invalid_scope) return HostOutcome::ok_with(invalid_handle());
  const ObjectId id = env->env().read_object(target.scope, target.key);
  if (id == kNoObject) return HostOutcome::ok_with(invalid_handle());
  // The original indexes its object table with the decoded number, so an
  // object that has left the world reads as the invalid handle.
  World* world = world_of(ctx);
  if (world != nullptr && world->find(id) == nullptr) {
    return HostOutcome::ok_with(invalid_handle());
  }
  return HostOutcome::ok_with(Value::object(kTypeObj, id));
}

HostOutcome env_write_obj(CallContext& ctx, bool root) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const EnvTarget target = env_target(ctx, root);
  if (!target.ok) return target.failure;
  if (target.invalid_scope) return HostOutcome::ok_void();
  ObjectId id = kNoObject;
  if (ctx.count() > target.value_index && ctx.arg(target.value_index).is_object()) {
    const ObjectRef ref = ctx.arg(target.value_index).as_object();
    World* world = world_of(ctx);
    // A handle naming no live object is written as the invalid index.
    if (ref.valid() && (world == nullptr || world->find(ref.id) != nullptr)) id = ref.id;
  }
  env->env().write_object(target.scope, target.key, id);
  return HostOutcome::ok_void();
}

// The registry dispatches on `(name, arity)`, and the retail engine registers
// each of these six names with and without a scope argument. Both entries land
// on the same body here; only the argument layout differs, and it is decided
// by the arity rather than sniffed at run time, so a scoped call handed a
// string where a scope belongs still refuses instead of becoming a root write.
HostOutcome env_read_int_scoped(CallContext& ctx) { return env_read_int(ctx, false); }
HostOutcome env_read_int_root(CallContext& ctx) { return env_read_int(ctx, true); }
HostOutcome env_write_int_scoped(CallContext& ctx) { return env_write_int(ctx, false); }
HostOutcome env_write_int_root(CallContext& ctx) { return env_write_int(ctx, true); }
HostOutcome env_read_string_scoped(CallContext& ctx) { return env_read_string(ctx, false); }
HostOutcome env_read_string_root(CallContext& ctx) { return env_read_string(ctx, true); }
HostOutcome env_write_string_scoped(CallContext& ctx) { return env_write_string(ctx, false); }
HostOutcome env_write_string_root(CallContext& ctx) { return env_write_string(ctx, true); }
HostOutcome env_read_obj_scoped(CallContext& ctx) { return env_read_obj(ctx, false); }
HostOutcome env_read_obj_root(CallContext& ctx) { return env_read_obj(ctx, true); }
HostOutcome env_write_obj_scoped(CallContext& ctx) { return env_write_obj(ctx, false); }
HostOutcome env_write_obj_root(CallContext& ctx) { return env_write_obj(ctx, true); }


// -- AI variables ---------------------------------------------------------

HostOutcome ai_var(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const std::int32_t player = int_arg(ctx, 0);
  const std::int32_t var = int_arg(ctx, 1);
  if (ctx.count() >= 3) {
    // `AIVar(<AIPlayerNum>, <Variable>, <PlayerNum>)` -- a player-mask test.
    // All seven corpus call sites pass an `AIMV_*` variable.
    return boolean(env->ai_vars().bit(player, var, int_arg(ctx, 2)));
  }
  return integer(env->ai_vars().get(player, var));
}

HostOutcome set_ai_var(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const std::int32_t player = int_arg(ctx, 0);
  const std::int32_t var = int_arg(ctx, 1);
  // Only the arity-3 value form is declared by the shipped surface, and it is
  // the only one the 455 corpus call sites use. `gbr.exe` also registers
  // `void, int idPlayer, int var, int bit, bool value`; no script calls it, so
  // we do not invent the entry point.
  if (!env->ai_vars().set(player, var, int_arg(ctx, 2))) {
    return HostOutcome::failed("SetAIVar: player or variable id out of range");
  }
  return HostOutcome::ok_void();
}

// -- research -------------------------------------------------------------
//
// `IsResearched`, `IsResearching` and `Settlement::CanResearch` do not consult
// a research catalog: each looks the name up in the **command table**, any
// row of any building (the vtable call at `[0x009969dc]+0x14`, which is what
// `CommandTable::find` models), answers false with "No such upgrade %s" when
// there is none, and then reads the row's `param` string itself, three tokens
// at a time, until the **first** `NamePlr` or `NameSet` triple. That triple
// names the ledger key and its scope; the other verbs (`ReqSet`, `SetsPlr`,
// ...) are skipped without being read. A row with neither verb has no ledger.
//
// The settlement forms read a `NamePlr` key at the owner's script number (the
// owner's index plus one: 0x00433a15, 0x00434a4f) and a `NameSet` key at the
// settlement's id. The `int idPlayer` forms read `NamePlr` at the integer
// **verbatim**, with no adjustment, and answer false for a `NameSet` row ("%s
// is not player upgrade"). The disassembly settles the verbatim: the integer
// is saved at 0x00433be6 and pushed unchanged at 0x00433e39.
//
// A world with no command system -- the tests' and the conformance fixtures'
// -- falls back to `EnvSystem`'s catalog, which is the reading the shipped
// verifier scripts make (`ParseStr` over the same attribute) and the only one
// available without the table.

/// One token of a command row's `param`, as 0x006ab970 cuts it: skip leading
/// **spaces** (not tabs), take everything up to the next comma, drop trailing
/// spaces. `pos` becomes the index after the comma, or `npos` once the string
/// is exhausted -- after which every further token is empty.
[[nodiscard]] std::string_view param_token(std::string_view text, std::size_t& pos) noexcept {
  if (pos == std::string_view::npos) return {};
  const std::size_t start = text.find_first_not_of(' ', pos);
  if (start == std::string_view::npos) {
    pos = std::string_view::npos;
    return {};
  }
  const std::size_t comma = text.find(',', start);
  std::size_t end = text.size();
  if (comma == std::string_view::npos) {
    pos = std::string_view::npos;
  } else {
    end = comma;
    pos = comma + 1;
  }
  while (end > start && text[end - 1] == ' ') --end;
  return text.substr(start, end - start);
}

/// The ledger a command row names: the first `NamePlr`/`NameSet` triple.
struct RowLedger {
  ResearchScope scope = ResearchScope::none;
  std::string_view key;
};

[[nodiscard]] RowLedger row_ledger(std::string_view param) noexcept {
  std::size_t pos = 0;
  do {
    const std::string_view verb = param_token(param, pos);
    const std::string_view subject = param_token(param, pos);
    (void)param_token(param, pos);
    // `std::string::compare(0, size, "NamePlr", 7)`: exact and case-sensitive.
    if (verb == "NamePlr") return {ResearchScope::player, subject};
    if (verb == "NameSet") return {ResearchScope::settlement, subject};
  } while (pos != std::string_view::npos);
  return {};
}

/// The command table, when the world has one.
[[nodiscard]] const CommandTable* command_table(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  const CommandSystem* commands = world == nullptr ? nullptr : command_system(*world);
  return commands == nullptr ? nullptr : &commands->table();
}

/// `IsResearched` and `IsResearching` -- 0x00433790 / 0x00433b70 and
/// 0x00434000 / 0x004343e0, one body per receiver type, the two names
/// differing only in the literal they compare the ledger value with
/// (`"researched"`, `"researching"`; exact). An invalid settlement is false
/// ("Parameter #1 ... is uninitialized or invalid object"), never a trap.
/// Nothing is written, nothing suspends.
HostOutcome ledger_state(CallContext& ctx, std::string_view state) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  std::string_view name;
  if (!key_arg(ctx, 1, name)) return bad_key();
  const bool by_player = ctx.count() > 0 && ctx.arg(0).is_integer();
  const CommandTable* table = command_table(ctx);
  if (table == nullptr) {
    if (by_player) {
      const std::int32_t player = ctx.arg(0).as_integer();
      return boolean(state == kResearched ? env->is_researched_for_player(player, name)
                                          : env->is_researching_for_player(player, name));
    }
    const Settlement* s = settlement_receiver(ctx);
    if (s == nullptr) return boolean(false);
    return boolean(state == kResearched ? env->is_researched(*s, name)
                                        : env->is_researching(*s, name));
  }
  const CommandDef* row = table->find(name);
  if (row == nullptr) return boolean(false);  // "No such upgrade %s"
  const RowLedger ledger = row_ledger(row->param);
  if (by_player) {
    if (ledger.scope != ResearchScope::player) return boolean(false);
    return boolean(env->env().read_string(EnvScope::for_player(ctx.arg(0).as_integer()),
                                          ledger.key) == state);
  }
  const Settlement* s = settlement_receiver(ctx);
  if (s == nullptr) return boolean(false);
  if (ledger.scope == ResearchScope::none) return boolean(false);
  const EnvScope scope = ledger.scope == ResearchScope::player
                             ? owner_scope(*s)
                             : EnvScope::for_settlement(s->id);
  return boolean(env->env().read_string(scope, ledger.key) == state);
}

HostOutcome is_researched(CallContext& ctx) { return ledger_state(ctx, kResearched); }
HostOutcome is_researching(CallContext& ctx) { return ledger_state(ctx, kResearching); }

/// `set.CanResearch(name)` -- 53 sites. 0x00434870, in its order:
///
///   1. the receiver; an invalid one is false (no trap);
///   2. the `<cmd>` row by name, any building's, research or not; none is
///      *"No such upgrade %s"* and false. `hireheroE` and `BuySlaves` are
///      asked about as often as `Fights`, and neither is a research row;
///   3. **an idle building of the settlement that offers the row** (0x0042d010
///      with its idle flag set: a member whose state string is exactly
///      `"idle"` and whose class's command list holds the row); none is
///      false. `settlement_lab_for` models it from the row's sources;
///   4. the row's ledger (`row_ledger`), if it names one: true unless the value
///      there is `"researched"` or `"researching"`. A row with no
///      `NamePlr`/`NameSet` -- a hire, a purchase -- is true here.
///
/// **No requirement is checked.** `ReqSet`, `ReqPlr` and `NReqSet` are never
/// read by this body; they are `VERIFY_RESEARCH.VS`'s, which the command bar
/// runs, and `EnvSystem::can_research` -- which does check them -- is that
/// script's model, not this entry point's. A world with no command system
/// falls back to it.
HostOutcome can_research(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  std::string_view name;
  if (!key_arg(ctx, 1, name)) return bad_key();
  const Settlement* s = settlement_receiver(ctx);
  if (s == nullptr) return boolean(false);
  World* world = world_of(ctx);
  const CommandTable* table = command_table(ctx);
  if (table == nullptr) return boolean(env->can_research(*s, name));
  const CommandDef* row = table->find(name);
  if (row == nullptr) return boolean(false);
  if (settlement_lab_for(*world, *s, *row, /*must_be_idle=*/true) == kNoObject) {
    return boolean(false);
  }
  const RowLedger ledger = row_ledger(row->param);
  if (ledger.scope == ResearchScope::none) return boolean(true);
  const EnvScope scope = ledger.scope == ResearchScope::player
                             ? owner_scope(*s)
                             : EnvScope::for_settlement(s->id);
  const std::string_view value = env->env().read_string(scope, ledger.key);
  return boolean(value != kResearched && value != kResearching);
}

/// `Settlement::TSResearch(str)` -- 110 sites, the tactic scripts' poll loop:
///
///     while (!bFights) { bFights = set.TSResearch("Fights"); Sleep(...); }
///
/// `gbr.exe` 0x00426ab0 runs `data/ai/TSH_Research.vs` for it, the way every
/// `TS*` entry point runs its helper (`TSRecruitHero: Error running script!`
/// beside `TSH_HeroRecruit.vs`), and the script is six lines:
///
/// ```
///   if (IsResearched(set, tech)) return true;
///   if (GetCmdCost(tech, gold, food))
///     if (gold <= set.gold && food <= set.food) if (set.CanResearch(tech)) {
///       set.SpentGoldOnTech(gold);
///       set.Research(tech);
///     }
///   return false;
/// ```
///
/// So this runs the shipped file when the scheduler holds it, on the owner's
/// AI search path like the recruit helpers, and only falls back to a
/// transcription -- the catalog's cost, `begin_research` for `set.Research`
/// -- for a world with no command system and no such chunk, which is the
/// tests'. The fallback is what this whole entry point was for a long time,
/// and it is why a tactic script's `TSResearch("BuySlaves")` -- a market
/// purchase, not a research row -- answered false on every map forever.
///
/// The body itself (0x00426ab0): an invalid receiver prints "The function
/// 'TSResearch' called for an uninitialized or invalid object." and answers
/// false; otherwise it registers a scratch out-value (0x0067b480), runs the
/// helper synchronously through the AI's script launcher for the owner's
/// player index (0x0043d030, then 0x006a0360) with the out-value, the
/// settlement and the name, and answers whether the out-value came back
/// non-zero. A helper that fails to run prints "TSResearch: Error running
/// script!" and the out-value is still false; here that failure is
/// `run_script_now`'s, which is shared with every `TS*` helper and reports it
/// as the caller's trap (`sim/ai.cpp`).
HostOutcome ts_research(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  std::string_view name;
  if (!key_arg(ctx, 1, name)) return bad_key();
  const Settlement* s = settlement_receiver(ctx);
  if (s == nullptr) return boolean(false);

  World* world = world_of(ctx);
  if (world != nullptr && ctx.scheduler != nullptr && command_system(*world) != nullptr) {
    static constexpr std::string_view kFile = "TSH_Research.vs";
    AiSystem* ai = ai_system_of(*world);
    const std::uint32_t chunk = (ai == nullptr || s->owner == kNoPlayer)
                                    ? ctx.scheduler->find_chunk(kFile)
                                    : ai->find_script(*ctx.scheduler, s->owner, kFile);
    if (chunk < ctx.scheduler->chunk_count()) {
      const Value args[2] = {ctx.arg(0), ctx.arg(1)};
      Value result;
      const HostOutcome ran = run_script_now(ctx, chunk, args, "TSResearch failed", result);
      if (ran.status != script::HostStatus::ok) return ran;
      return boolean(result.is_integer() && result.as_integer() != 0);
    }
  }

  if (env->is_researched(*s, name)) return boolean(true);
  const ResearchCommand* command = env->research().find(name);
  if (command == nullptr) return boolean(false);  // `GetCmdCost` returned false
  if (command->cost_gold > s->warehouse.gold) return boolean(false);
  if (command->cost_food > s->warehouse.food) return boolean(false);
  if (!env->can_research(*s, name)) return boolean(false);
  env->begin_research(*s, name);
  return boolean(false);
}

// -- unit availability ----------------------------------------------------

/// `UEnabled(int mask, int bit)` -- `bool, int mask, int bit`, 0x00420ee0.
///
/// `(mask & (1 << (bit & 31))) != 0`: the shift count is masked to five bits
/// as the processor's `shl` does, so `UEnabled(m, 33)` tests bit 1. No world,
/// no range check, nothing else.
///
/// Not a research test and not unit-specific despite the name: `SQUADMONITOR.VS`
/// uses it on a flee-reason bitfield (`UEnabled(nFlee, 0)` .. `(nFlee, 3)`) and
/// every AI script uses it on `AIV_LogPlayer`, a player mask.
HostOutcome u_enabled(CallContext& ctx) {
  const std::uint32_t mask = static_cast<std::uint32_t>(int_arg(ctx, 0));
  const std::uint32_t bit = static_cast<std::uint32_t>(int_arg(ctx, 1)) & 31u;
  return boolean(((mask >> bit) & 1u) != 0);
}

/// `UType(int nType, int nRace)`, `UTech`, `UTrainCmd` -- 0x00420d60,
/// 0x00420de0, 0x00420e60. Each checks `nType` against 0..5 and answers `""`
/// outside it; `nRace` is **not** checked, and the string is
/// `table[nRace * 6 + nType]` -- so a race past Germany reads on into the next
/// table, which `UnitCatalog::block` answers.
[[nodiscard]] std::string_view unit_column(CallContext& ctx, EnvSystem& env, std::int64_t table) {
  const std::int64_t type = int_arg(ctx, 0);
  const std::int64_t race = int_arg(ctx, 1);
  if (type < 0 || type > 5) return {};
  return env.units().block(table + race * 6 + type);
}

HostOutcome u_type(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  return text(unit_column(ctx, *env, 0));
}

HostOutcome u_tech(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  return text(unit_column(ctx, *env, 48));
}

HostOutcome u_train_cmd(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  return text(unit_column(ctx, *env, 96));
}

/// `CheckUEnabled(OUT int mask, int idPlayer, int max, int nType, int nRace)`
/// -- 0x004243d0, 49 sites, `ESH_ENABLEDUNITS.VS` once per unit type.
///
/// In its order:
///
///   1. `idPlayer` is **1-based**: the body takes `idPlayer - 1` and refuses
///      16 and above; `nType` outside 0..5 is refused too. A refusal answers
///      false and leaves the mask as it was. (Below 1 the original reads the
///      record before the table; that is answered as a refusal here.)
///   2. `max < 0` -- `AI.INI`'s "no limit" -- sets bit `nType` and answers true.
///   3. `max == 0` -- "disabled" -- clears it and answers false.
///   4. otherwise it asks the player's class-count map (0x004441e0 on the
///      player record's `[+0x8c]`) how many of `UType(nType, nRace)` the player
///      has -- `Count(idPlayer, that class)`'s number -- and sets the bit and
///      answers true while that is **below** `max`, clears it and answers
///      false once it is not.
///
/// So the mask is "types this player may still train", not "types the profile
/// allows": an AI that has reached `AIV_MaxGArcher` archers stops being
/// offered them. `UType` here is the unchecked-race read above.
HostOutcome check_u_enabled(CallContext& ctx) {
  if (ctx.count() < 5) return HostOutcome::failed("CheckUEnabled: expects five arguments");
  const std::int32_t player = int_arg(ctx, 1);
  const std::int32_t max = int_arg(ctx, 2);
  const std::int32_t type = int_arg(ctx, 3);
  if (type < 0 || type > 5 || player < 1 || player > 16) return boolean(false);
  const std::uint32_t bit = 1u << static_cast<unsigned>(type);
  std::uint32_t mask = static_cast<std::uint32_t>(int_arg(ctx, 0));
  bool enabled = false;
  if (max < 0) {
    enabled = true;
  } else if (max > 0) {
    World* world = world_of(ctx);
    EnvSystem* env = world == nullptr ? nullptr : env_of(*world);
    if (env == nullptr) return no_env();
    const std::string_view cls =
        env->units().block(static_cast<std::int64_t>(int_arg(ctx, 4)) * 6 + type);
    enabled = player_class_count(*world, player_from_script(player), cls) < max;
  }
  mask = enabled ? (mask | bit) : (mask & ~bit);
  ctx.out(0) = Value::integer(static_cast<std::int32_t>(mask));
  return boolean(enabled);
}

/// `RUType(str strClass, int nRace) -> int` -- 0x00420c90. The class comes
/// first here, the reverse of the other three; `TSH_RecruitArmy.vs` is the
/// only caller. `nRace` **is** checked here (0..7, else -1), and the class is
/// compared byte for byte against that race's six types; no match is -1.
HostOutcome r_u_type(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  std::string_view type;
  if (ctx.count() == 0 || !ctx.arg(0).is_string()) return integer(-1);
  type = ctx.arg(0).as_string();
  return integer(env->units().index_of(int_arg(ctx, 1), type));
}

[[nodiscard]] const RaceUnits* race_arg(CallContext& ctx, EnvSystem& env) {
  return env.units().race(int_arg(ctx, 0));
}

// The five one-per-race tables -- `ArenaUType` 0x00422ad0, `ArenaTrainCmd`
// 0x00422a60, `ArenaUTech` 0x00420f10, `TempleUType` 0x00420f80,
// `TempleUTrain` 0x00420ff0 -- each check the race against 0..7 and answer
// `""` outside it. `TempleUTrain(Germany)` is the null pointer described at
// `kRaces`; the original faults on it and this answers `""`.

HostOutcome arena_u_type(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const RaceUnits* race = race_arg(ctx, *env);
  return text(race == nullptr ? std::string_view{} : std::string_view{race->arena.type});
}

HostOutcome arena_u_tech(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const RaceUnits* race = race_arg(ctx, *env);
  return text(race == nullptr ? std::string_view{} : std::string_view{race->arena.tech});
}

HostOutcome arena_train_cmd(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const RaceUnits* race = race_arg(ctx, *env);
  return text(race == nullptr ? std::string_view{} : std::string_view{race->arena.train_cmd});
}

HostOutcome temple_u_type(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const RaceUnits* race = race_arg(ctx, *env);
  return text(race == nullptr ? std::string_view{} : std::string_view{race->temple.type});
}

HostOutcome temple_u_train(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const RaceUnits* race = race_arg(ctx, *env);
  return text(race == nullptr ? std::string_view{} : std::string_view{race->temple.train_cmd});
}

// -- balance constants ----------------------------------------------------

/// `GetConst(str) -> int` -- 0x004c6270, over `DATA\CONST.INI`'s `[GamePlay]`.
///
/// The body asks the INI's integer getter (0x0063f200) for `GamePlay`/key
/// into a local and pushes the local. The getter finds the text (0x0063f120),
/// skips leading spaces and tabs, takes one `-` or `+`, and then reads `0x`
/// hexadecimal (lower-case `x`; both digit cases) or decimal, **stopping at
/// the first character that does not fit and keeping what it has** -- so
/// `"1,5 5,20"` is 1 and `"12.5"` is 12. `load_constants` stores that number
/// for every key (`ini_number`).
///
/// The economy's slice is asked first, so `EconomySystem::set_rules` stays
/// authoritative for the ~40 keys it holds as fields.
///
/// **A key the section does not have.** The getter then leaves the local as
/// it was, and `GetConst` never initialised it: it holds the VM stack pointer
/// it was loaded from. That is an address, not a number anybody can
/// reproduce; this answers zero. The one shipped key that reaches it --
/// `RecallDistance`, opening `DATA\SUBAI\HERO_DETACH_BEHAVIOR.VS` -- is
/// assigned to a local that is never read again, so no shipped behaviour
/// depends on the value.
HostOutcome get_const(CallContext& ctx) {
  if (ctx.count() < 1 || !ctx.arg(0).is_string()) {
    return HostOutcome::failed("GetConst: expects a constant name");
  }
  const std::string_view key = ctx.arg(0).as_string();
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetConst: no world in context");
  if (EconomySystem* economy = economy_of(*world)) {
    std::int32_t value = 0;
    if (economy_constant(economy->rules(), key, value)) return integer(value);
  }
  EnvSystem* env = env_of(*world);
  if (env == nullptr) return no_env();
  std::int32_t value = 0;
  if (!env->constant(key, value)) return integer(0);
  return integer(value);
}

/// `GetConstStr(str) -> str` -- 0x004c62f0: the INI's text for
/// `GamePlay`/key (0x0063f120), or `""` (0x007ab85a) for a key it does not
/// have. Every key has its text, numeric ones included. Two keys in the whole
/// corpus, both comma-separated lists the scripts walk with `ParseStr`:
/// `TributeTimes` and `TributeGold`.
HostOutcome get_const_str(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  if (ctx.count() < 1 || !ctx.arg(0).is_string()) {
    return HostOutcome::failed("GetConstStr: expects a constant name");
  }
  std::string_view value;
  if (!env->constant_string(ctx.arg(0).as_string(), value)) return text({});
  return text(value);
}

struct EnvHostDef {
  CallKind kind;
  const char* name;
  std::uint16_t arity;
  HostFn fn;
};

/// Ordered by call frequency in `docs/formats/vs-host-api.md`, which is the
/// order in which they pay.
constexpr EnvHostDef kEnvHosts[] = {
    {CallKind::free_function, "SetAIVar", 3, &set_ai_var},        // 455
    {CallKind::free_function, "AIVar", 2, &ai_var},               // 302, with the next
    {CallKind::free_function, "AIVar", 3, &ai_var},
    {CallKind::free_function, "EnvReadInt", 2, &env_read_int_scoped},    // 283
    {CallKind::free_function, "EnvWriteInt", 3, &env_write_int_scoped},  // 188
    // The root scope: the same six names one argument shorter. Both arities are
    // real registrations in `gbr.exe` (0x006ae7b0 declares the six root forms,
    // 0x006ae941 the six player forms), and the 24 containers call the root
    // form 403 times against the packs' zero. Ordered here by container call
    // count, which is a different ranking from the packs' and so is kept apart.
    {CallKind::free_function, "EnvReadInt", 1, &env_read_int_root},      // 191
    {CallKind::free_function, "EnvWriteInt", 2, &env_write_int_root},    // 164
    {CallKind::free_function, "EnvReadString", 1, &env_read_string_root},   // 34
    {CallKind::free_function, "EnvWriteString", 2, &env_write_string_root},  // 14
    {CallKind::free_function, "IsResearched", 2, &is_researched}, // 183
    {CallKind::free_function, "GetConst", 1, &get_const},         // 152
    {CallKind::free_function, "EnvReadString", 2, &env_read_string_scoped},    // 118
    {CallKind::member, "TSResearch", 1, &ts_research},                  // 110
    {CallKind::free_function, "EnvWriteString", 3, &env_write_string_scoped},  // 53
    {CallKind::member, "CanResearch", 1, &can_research},                // 53
    {CallKind::free_function, "CheckUEnabled", 5, &check_u_enabled},    // 49
    {CallKind::free_function, "UTech", 2, &u_tech},                     // 30
    {CallKind::free_function, "UEnabled", 2, &u_enabled},               // 27
    {CallKind::free_function, "IsResearching", 2, &is_researching},     // 23
    {CallKind::free_function, "UType", 2, &u_type},
    {CallKind::free_function, "UTrainCmd", 2, &u_train_cmd},
    {CallKind::free_function, "GetConstStr", 1, &get_const_str},  // 8
    {CallKind::free_function, "EnvReadObj", 2, &env_read_obj_scoped},    // 8
    {CallKind::free_function, "EnvWriteObj", 3, &env_write_obj_scoped},  // 6
    // The root forms of the object pair: `Obj, str key` at 0x006ae823 and
    // `void, str key, Obj o` at 0x006ae80d. No shipped script calls them;
    // they are the same bodies as the other root forms and cost a row each.
    {CallKind::free_function, "EnvReadObj", 1, &env_read_obj_root},
    {CallKind::free_function, "EnvWriteObj", 2, &env_write_obj_root},
    {CallKind::free_function, "RUType", 2, &r_u_type},
    {CallKind::free_function, "ArenaUTech", 1, &arena_u_tech},
    {CallKind::free_function, "ArenaUType", 1, &arena_u_type},
    {CallKind::free_function, "ArenaTrainCmd", 1, &arena_train_cmd},
    {CallKind::free_function, "TempleUType", 1, &temple_u_type},
    {CallKind::free_function, "TempleUTrain", 1, &temple_u_train},
};

}  // namespace

std::size_t register_env_host(script::HostRegistry& registry) {
  for (const EnvHostDef& def : kEnvHosts) {
    registry.define(def.kind, def.name, def.arity, def.fn);
  }
  return env_host_entry_count();
}

std::size_t env_host_entry_count() noexcept {
  return sizeof(kEnvHosts) / sizeof(kEnvHosts[0]);
}

}  // namespace imperivm::core::sim
