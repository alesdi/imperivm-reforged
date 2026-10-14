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
     // `trainBDruid` is the command in `BTEMPLEOFTHOR.XML`, but it is the one
     // string of this table that `gbr.exe` does not carry. Reproduced as the
     // hole it is rather than filled in.
     {"BDruid", "", ""}},
    {"Egypt",
     {{"ESwordsman", "", "trainESwordsman"},
      {"EArcher", "", "trainEArcher"},
      {"EAxetrower", "", "trainEAxeTrower"},
      {"EAnubisWarrior", "Cult of Anubis", "trainEAnubisWarrior"},
      {"EHorusWarrior", "Cult of Horus", "trainEHorusWarrior"},
      {"EGuardian", "", "trainEGuardian"}},
     // `ETEMPLEOFOSIRIS.XML` gates `Chariot of Osiris` on `Cult of Osiris`, but
     // that string is absent from `gbr.exe` while the other seven races' arena
     // techs are present, so the engine's Egyptian slot reads as empty here.
     {"EChariot", "", "Chariot of Osiris"},
     {"EPriest", "", "trainEPriest"}},
    {"Germany",
     {{"TSwordsman", "", "trainTSwordsman"},
      {"TArcher", "", "trainTArcher"},
      {"TAxeman", "", "trainTAxeman"},
      {"THuntress", "", "trainTHuntress"},
      {"TTeutonRider", "", "trainTTeutonRider"},
      {"TMaceman", "", "trainTMaceman"}},
     {"TValkyrie", "TFights", "Call Valkyries"},
     {"TEnchantress", "", "trainTEnchantress"}},
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

std::size_t EnvSystem::load_constants(const IniDocument& document) {
  const SectionIndex section = document.section("GamePlay");
  if (section == kNoSection) return 0;
  std::size_t taken = 0;
  for (const IniEntry& entry : document.entries_of(section)) {
    if (!entry.has_key || entry.key.empty()) continue;  // a bare line is not a constant
    std::int32_t value = 0;
    if (parse_int(entry.value, value)) {
      // `parse_int` refuses a value it can only read half of, which is the
      // guard `ProductionInterval` needed: `2000` must never become `20`.
      set_constant(entry.key, value);
    } else {
      set_constant_string(entry.key, entry.value);
    }
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
/// 0x0069cc58), null by default, and three launchers set it:
///
///   * **the sequence runner** (`Start`, 0x005b9d40) -- after spawning a
///     sequence's thread it looks that thread up and sets its context to the
///     one-character string `/` (0x005b9da7 .. 0x005b9db7). So inside every
///     campaign sequence a relative root key is made absolute: `"Bonus"` and
///     `"/Bonus"` are **one** slot there;
///   * the object-attached launcher (0x0041ccb0, 0x0041cd00), which sets the
///     object's id formatted `%d`; and
///   * the conversation phrase runner (0x0050aa90, 0x0050b070), which sets a
///     name followed by `/`.
///
/// Only the first is modelled. Every root-form `Env*` call in the installation
/// -- 381 literal sites -- is inside a `Sequences/` script, so the other two
/// would prefix keys no shipped script ever passes them; they answer "no
/// context" here, which is the null the original starts every thread with.
///
/// A sequence's thread is the one `CampaignSystem` records as running it,
/// which is saved state already (`SequenceEntry::running`).
[[nodiscard]] std::string_view thread_context(CallContext& ctx) {
  if (ctx.script == script::kNoScript) return {};
  World* world = world_of(ctx);
  const CampaignSystem* campaign = world == nullptr ? nullptr : campaign_system_of(*world);
  if (campaign == nullptr) return {};
  for (const CampaignSystem::SequenceEntry& entry : campaign->sequences()) {
    if (entry.running == ctx.script) return "/";
  }
  return {};
}

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

[[nodiscard]] HostOutcome no_settlement() {
  return HostOutcome::failed("receiver does not resolve to a settlement");
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

HostOutcome is_researched(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  std::string_view name;
  if (!key_arg(ctx, 1, name)) return bad_key();
  // `gbr.exe` declares both overloads at arity 2:
  //   bool, Settlement set, str research
  //   bool, int idPlayer,   str research
  if (ctx.count() > 0 && ctx.arg(0).is_integer()) {
    return boolean(env->is_researched_for_player(ctx.arg(0).as_integer(), name));
  }
  const Settlement* s = settlement_receiver(ctx);
  if (s == nullptr) return no_settlement();
  return boolean(env->is_researched(*s, name));
}

HostOutcome is_researching(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  std::string_view name;
  if (!key_arg(ctx, 1, name)) return bad_key();
  if (ctx.count() > 0 && ctx.arg(0).is_integer()) {
    return boolean(env->is_researching_for_player(ctx.arg(0).as_integer(), name));
  }
  const Settlement* s = settlement_receiver(ctx);
  if (s == nullptr) return no_settlement();
  return boolean(env->is_researching(*s, name));
}

/// `set.CanResearch(name)` -- 53 sites. 0x00434870, in its order:
///
///   1. the `<cmd>` row by name, any building's, research or not; none is
///      *"No such upgrade %s"* and false. `hireheroE` and `BuySlaves` are
///      asked about as often as `Fights`, and neither is a research row.
///   2. **an idle building of the settlement that offers the row**
///      (0x0042d010 with the idle flag set); none is false. This is the test
///      that makes the answer a settlement's rather than a ledger's, and it
///      is why a stronghold with no temple cannot hire a hero.
///   3. the ledger, only for a row that names `NamePlr` or `NameSet`: not
///      `researched`, not `researching`. A row with neither -- a hire, a
///      purchase -- skips this and is researchable whenever its lab is idle.
///
/// `EnvSystem::can_research` is the ledger half with the catalog's
/// requirements on top, and answers the whole question for a world with no
/// command system -- the tests' and the conformance fixtures' -- which is
/// where the older reading lived: a name outside the research catalog was
/// "no such upgrade" there, so every `CanResearch("hirehero…")` in the
/// installation answered no and no computer player ever hired a hero.
HostOutcome can_research(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  std::string_view name;
  if (!key_arg(ctx, 1, name)) return bad_key();
  const Settlement* s = settlement_receiver(ctx);
  if (s == nullptr) return no_settlement();
  World* world = world_of(ctx);
  const CommandSystem* commands = world == nullptr ? nullptr : command_system(*world);
  if (commands == nullptr) return boolean(env->can_research(*s, name));
  const CommandDef* row = commands->table().find(name);
  if (row == nullptr) return boolean(false);
  if (settlement_lab_for(*world, *s, *row, /*must_be_idle=*/true) == kNoObject) {
    return boolean(false);
  }
  if (env->research().find(name) == nullptr) return boolean(true);
  return boolean(env->can_research(*s, name));
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
HostOutcome ts_research(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  std::string_view name;
  if (!key_arg(ctx, 1, name)) return bad_key();
  const Settlement* s = settlement_receiver(ctx);
  if (s == nullptr) return no_settlement();

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

/// `UEnabled(int mask, int bit)` -- `bool, int mask, int bit` in `gbr.exe`.
///
/// Not a research test and not unit-specific despite the name: `SQUADMONITOR.VS`
/// uses it on a flee-reason bitfield (`UEnabled(nFlee, 0)` .. `(nFlee, 3)`) and
/// every AI script uses it on `AIV_LogPlayer`, a player mask.
HostOutcome u_enabled(CallContext& ctx) {
  const std::int32_t mask = int_arg(ctx, 0);
  const std::int32_t bit = int_arg(ctx, 1);
  if (bit < 0 || bit >= 32) return boolean(false);
  return boolean(((mask >> bit) & 1) != 0);
}

/// `CheckUEnabled(OUT int mask, int idPlayer, int max, int nType, int nRace)`.
///
/// `ESH_ENABLEDUNITS.VS` is the only real caller and it is the whole
/// specification: zero the mask, then run one call per unit type passing that
/// type's `AIV_Max*` value as `max`, then store the mask as
/// `<Race>UnitsEnabled`. `AI.INI` documents the value space of those variables
/// as `0 - disable, -1: no limit`, so a zero clears the bit and anything else
/// sets it. `ESH_BUILDARMY.VS`'s single call, `CheckUEnabled(nEnabled,
/// AIPlayer, 0, 5, Carthage)`, uses exactly the clearing half.
HostOutcome check_u_enabled(CallContext& ctx) {
  if (ctx.count() < 5) return HostOutcome::failed("CheckUEnabled: expects five arguments");
  const std::int32_t max = int_arg(ctx, 2);
  const std::int32_t type = int_arg(ctx, 3);
  if (type < 0 || type >= 32) return boolean(false);
  const std::int32_t bit = static_cast<std::int32_t>(1u << static_cast<unsigned>(type));
  std::int32_t mask = int_arg(ctx, 0);
  const bool enabled = max != 0;
  mask = enabled ? (mask | bit) : (mask & ~bit);
  ctx.out(0) = Value::integer(mask);
  return boolean(enabled);
}

[[nodiscard]] const UnitRow* row_arg(CallContext& ctx, EnvSystem& env) {
  return env.units().row(int_arg(ctx, 1), int_arg(ctx, 0));
}

HostOutcome u_type(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const UnitRow* row = row_arg(ctx, *env);
  return text(row == nullptr ? std::string_view{} : std::string_view{row->type});
}

HostOutcome u_tech(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const UnitRow* row = row_arg(ctx, *env);
  return text(row == nullptr ? std::string_view{} : std::string_view{row->tech});
}

HostOutcome u_train_cmd(CallContext& ctx) {
  EnvSystem* env = host_env(ctx);
  if (env == nullptr) return no_env();
  const UnitRow* row = row_arg(ctx, *env);
  return text(row == nullptr ? std::string_view{} : std::string_view{row->train_cmd});
}

/// `RUType(str strClass, int nRace) -> int`. The class comes first here, which
/// is the reverse of the other three; `TSH_RecruitArmy.vs` is the only caller.
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

/// `GetConst(str) -> int` over `DATA\CONST.INI`'s `[GamePlay]` section.
///
/// The economy's slice is asked first, so `EconomySystem::set_rules` stays
/// authoritative for the ~40 keys it holds as fields; everything else comes
/// from the generated table.
///
/// An unknown key reads **zero**, not an error. `DATA\SUBAI\HERO_DETACH_
/// BEHAVIOR.VS` -- a shipped, live behaviour -- opens with
/// `GetConst("RecallDistance")`, and `RecallDistance` is in no section of
/// `CONST.INI`: of the 74 distinct literals the corpus passes to `GetConst`,
/// 73 are `[GamePlay]` keys and that one is in no section of the file. Trapping there would kill a script the retail engine runs happily.
/// (The value is dead -- that local is never read again -- so the *magnitude*
/// of the fallback is unconstrained by the corpus, and zero is simply what an
/// INI reader with no default returns.)
///
/// Zero is therefore also what every key reads as before `load_constants` has
/// run, apart from the economy's slice. That is deliberate: this domain does
/// not carry a second copy of `CONST.INI`, so a build with no game data has no
/// balance numbers rather than plausible invented ones.
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

/// `GetConstStr(str) -> str`. Two keys in the whole corpus, both of them
/// comma-separated lists the scripts walk with `ParseStr`: `TributeTimes` and
/// `TributeGold`.
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
