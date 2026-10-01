// The campaign layer: `game.xml`'s flavour, `territories.xml`, and the state
// that crosses a mission boundary.
//
// See include/imperivm/core/sim/campaign.hpp for what is proven, what is
// inferred, and what this deliberately refuses to answer.

#include "imperivm/core/sim/campaign.hpp"

#include "imperivm/core/sim/env.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <utility>

#include "imperivm/core/formats/ini.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/xml.hpp"

namespace imperivm::core::sim {
namespace {

using script::CallContext;
using script::HostOutcome;
using script::Value;

constexpr script::CallKind kFree = script::CallKind::free_function;

/// The FNV prime, the same mixer `MatchSystem::hash` and `EnvSystem::hash` use.
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void mix(std::uint64_t& accumulator, std::uint64_t value) noexcept {
  accumulator = (accumulator ^ value) * kFnvPrime;
}

void mix_string(std::uint64_t& accumulator, std::string_view text) noexcept {
  for (const char c : text) mix(accumulator, static_cast<std::uint8_t>(c));
  mix(accumulator, 0xFFu);
}

[[nodiscard]] bool is_space(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
  while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
  return text;
}

[[nodiscard]] CampaignSystem* host_campaign(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  return world == nullptr ? nullptr : campaign_system_of(*world);
}

// -- the entry points ------------------------------------------------------

/// `ConquestBonus()`.
///
/// The conquest's `Sequences/seq0.vs` calls it exactly once, as
/// `EnvWriteString("/Bonus", ConquestBonus())`, then compares the stored string
/// against the seven territory bonus names. So it returns a `str` and the empty
/// string is a legal answer: the first mission of a conquest has conquered
/// nothing, and none of the seven `if`s in `seq0.vs` matches.
HostOutcome fn_conquest_bonus(CallContext& ctx) {
  const CampaignSystem* campaign = host_campaign(ctx);
  if (campaign == nullptr) return HostOutcome::failed("ConquestBonus: no campaign");
  return HostOutcome::ok_with(Value::string(std::string(campaign->active_bonus())));
}

/// `SetTerritoryState(str id, int state)`.
///
/// All seven call sites in the retail conquest pass a literal territory `id`
/// and the bare constant `tsOwned`. An unknown id is reported as a trap rather
/// than ignored: the seven ids are literals in the map scripts, so a miss means
/// the container's `territories.xml` and its map scripts disagree, and silently
/// dropping the write would lose the mission's only lasting effect.
HostOutcome fn_set_territory_state(CallContext& ctx) {
  CampaignSystem* campaign = host_campaign(ctx);
  if (campaign == nullptr) return HostOutcome::failed("SetTerritoryState: no campaign");
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("SetTerritoryState: id is not a string");
  if (!ctx.arg(1).is_integer()) {
    return HostOutcome::failed("SetTerritoryState: state is not an integer");
  }
  // The three values are proven, so a fourth is a script this build does not
  // understand rather than a state to store and hope about.
  const auto state = territory_state(ctx.arg(1).as_integer());
  if (!state) return HostOutcome::failed("SetTerritoryState: state is not one of ts*");
  if (!campaign->set_state(ctx.arg(0).as_string(), *state)) {
    return HostOutcome::failed("SetTerritoryState: no such territory");
  }
  return HostOutcome::ok_void();
}

/// `GetTerritoryState(str id)`.
///
/// Reached by nothing in the install; see the header on the arity. Answers with
/// the state the table holds, and refuses an id it does not know rather than
/// returning a number the caller would compare against `tsOwned`.
HostOutcome fn_get_territory_state(CallContext& ctx) {
  const CampaignSystem* campaign = host_campaign(ctx);
  if (campaign == nullptr) return HostOutcome::failed("GetTerritoryState: no campaign");
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("GetTerritoryState: id is not a string");
  if (campaign->find(ctx.arg(0).as_string()) < 0) {
    return HostOutcome::failed("GetTerritoryState: no such territory");
  }
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(campaign->state_of(ctx.arg(0).as_string()))));
}

}  // namespace

// --------------------------------------------------------------------------
// game.xml
// --------------------------------------------------------------------------

Result<CampaignKind> campaign_kind(std::int32_t game_type) {
  switch (game_type) {
    case 0: return CampaignKind::scenario;
    case 1: return CampaignKind::adventure;
    case 2: return CampaignKind::conquest;
    default: return FormatError::unsupported;
  }
}

Result<TerritoryState> territory_state(std::int32_t value) {
  switch (value) {
    case 0: return TerritoryState::enemy;
    case 1: return TerritoryState::owned;
    case 2: return TerritoryState::disabled;
    default: return FormatError::unsupported;
  }
}

Result<CampaignProperties> CampaignProperties::parse(std::span<const std::byte> xml) {
  auto doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  return from_document(*doc);
}

Result<CampaignProperties> CampaignProperties::from_document(const XmlDocument& doc) {
  const NodeIndex root = doc.root();
  if (root == kNoNode || doc.node(root).name != "game") return FormatError::bad_magic;
  const NodeIndex properties = doc.child(root, "properties");
  if (properties == kNoNode) return FormatError::malformed;

  // `game_type` is required, and there is no sensible default: a container
  // whose flavour is missing is one this reader does not understand. The four
  // blank templates all carry it.
  const auto kind = campaign_kind(doc.attribute_int(properties, "game_type", -1));
  if (!kind) return kind.error();

  CampaignProperties out;
  out.kind = *kind;
  out.name = std::string(doc.attribute(properties, "name"));
  out.author = std::string(doc.attribute(properties, "author"));
  out.description = std::string(doc.attribute(properties, "description"));
  out.start_map = doc.attribute_int(properties, "start_map", 1);
  out.last_edited_map = doc.attribute_int(properties, "last_edited_map", 1);
  return out;
}

// --------------------------------------------------------------------------
// the adventure series
// --------------------------------------------------------------------------

std::int32_t adventure_order(std::string_view file_name) noexcept {
  // The last path component, whichever separator the caller used. `gbr.exe`
  // spells its two literals with forward slashes and the install is Windows, so
  // both occur.
  if (const auto cut = file_name.find_last_of("/\\"); cut != std::string_view::npos) {
    file_name.remove_prefix(cut + 1);
  }
  std::size_t digits = 0;
  std::int32_t value = 0;
  while (digits < file_name.size() && file_name[digits] >= '0' && file_name[digits] <= '9') {
    // Six files per series; a name that overflows is not one of them.
    if (value > 100000) return -1;
    value = value * 10 + (file_name[digits] - '0');
    ++digits;
  }
  // The prefix has to be followed by the separator the twelve files use. Without
  // that check `2_Great_loses_Spain` and a hypothetical `2005.bfhp` would both
  // answer 2.
  if (digits == 0 || digits >= file_name.size() || file_name[digits] != '_') return -1;
  return value;
}

// --------------------------------------------------------------------------
// territories.xml
// --------------------------------------------------------------------------

std::vector<std::int32_t> parse_conquered_order(std::string_view text) {
  std::vector<std::int32_t> out;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t comma = text.find(',', pos);
    const std::string_view field =
        trim(text.substr(pos, comma == std::string_view::npos ? std::string_view::npos
                                                             : comma - pos));
    if (!field.empty()) {
      bool negative = false;
      std::size_t i = 0;
      if (field[0] == '-') {
        negative = true;
        i = 1;
      }
      std::int32_t value = 0;
      bool digits = false;
      for (; i < field.size(); ++i) {
        if (field[i] < '0' || field[i] > '9') {
          digits = false;
          break;
        }
        value = value * 10 + (field[i] - '0');
        digits = true;
      }
      // A field that is not an integer is dropped rather than aborting the
      // parse: the attribute is empty in the only shipped conquest, so the
      // exact spelling of a populated one is unverified and a strict reader
      // would refuse a save the original wrote.
      if (digits) out.push_back(negative ? -value : value);
    }
    if (comma == std::string_view::npos) break;
    pos = comma + 1;
  }
  return out;
}

// --------------------------------------------------------------------------
// sequences.xml
// --------------------------------------------------------------------------

Result<std::vector<SequenceRef>> parse_sequences(std::span<const std::byte> xml) {
  auto doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  const NodeIndex root = doc->root();
  if (root == kNoNode || doc->node(root).name != "sequences") return FormatError::bad_magic;

  std::vector<SequenceRef> out;
  for (NodeIndex i = doc->child(root, "sequence"); i != kNoNode;
       i = doc->next(i, "sequence")) {
    SequenceRef ref;
    ref.name = std::string(doc->attribute(i, "name"));
    ref.script = std::string(doc->attribute(i, "script"));
    ref.wizard = std::string(doc->attribute(i, "wizard"));
    // Absent means allowed. 262 of the 308 shipped sequences say nothing and
    // are autorun; the 46 that say `no` are the ones `RunSequence` starts, and
    // 32 of them are named by a literal somewhere in their own container.
    const std::string_view autorun = doc->attribute(i, "autorunallowed");
    ref.autorun_allowed = autorun != "no";
    out.push_back(std::move(ref));
  }
  return out;
}

std::string sequence_entry_path(std::string_view script, std::string_view base) {
  const std::size_t slash = script.find_first_of("/\\");
  // A path with no root component is taken as already relative to `base`. No
  // shipped manifest does that, but refusing would be a parse error over a
  // spelling the format does not forbid.
  const std::string_view tail =
      slash == std::string_view::npos ? script : script.substr(slash + 1);
  if (base.empty()) return std::string(tail);
  std::string out(base);
  out.push_back('/');
  out.append(tail);
  return out;
}

Result<ConquestMap> ConquestMap::parse(std::span<const std::byte> xml) {
  auto doc = XmlDocument::parse(xml);
  if (!doc) return doc.error();
  return from_document(*doc);
}

Result<ConquestMap> ConquestMap::from_document(const XmlDocument& doc) {
  const NodeIndex root = doc.root();
  if (root == kNoNode || doc.node(root).name != "conquestmap") return FormatError::bad_magic;

  ConquestMap out;
  out.name_ = std::string(doc.attribute(root, "name"));
  out.data_ = std::string(doc.attribute(root, "data"));
  out.choose_ = doc.attribute_int(root, "choose", 0) != 0;
  out.conquered_order_ = std::string(doc.attribute(root, "ConqueredOrder"));
  out.interface_id_ = doc.attribute_int(root, "interface", -1);

  // The colourisation knobs, in document order. Selected by suffix rather than
  // by a list of twelve names because the authoring copy outside the container
  // spells one of them `disable_colorize` where the container spells it
  // `disabled_colorize`, and a fixed list would silently drop it.
  const XmlNode& node = doc.node(root);
  for (std::uint32_t i = 0; i < node.attribute_count; ++i) {
    const XmlAttribute& attribute = doc.attributes()[node.attribute_begin + i];
    const std::string_view name = attribute.name;
    const bool knob = name.size() > 4 &&
                      (name.ends_with("_colorize") || name.ends_with("_hue") ||
                       name.ends_with("_sat"));
    if (!knob) continue;
    out.display_.emplace_back(std::string(name), doc.attribute_int(root, name, 0));
  }

  for (NodeIndex i = doc.child(root, "territory"); i != kNoNode; i = doc.next(i, "territory")) {
    Territory territory;
    territory.id = std::string(doc.attribute(i, "id"));
    territory.index = doc.attribute_int(i, "index", 0);
    // An out-of-range `state` is refused rather than clamped: the three values
    // are proven, and a fourth means a document this reader does not
    // understand.
    const auto state = territory_state(doc.attribute_int(i, "state", 0));
    if (!state) return state.error();
    territory.state = *state;
    territory.visual_name = std::string(doc.attribute(i, "visualname"));
    territory.map_name = std::string(doc.attribute(i, "mapname"));
    territory.description = std::string(doc.attribute(i, "description"));
    territory.bonus = std::string(doc.attribute(i, "bonus"));
    territory.bonus_description = std::string(doc.attribute(i, "bonus_descr"));
    territory.interface_id = doc.attribute_int(i, "interface", -1);
    // Kept only when it names one of the eight races, as 0x00506560 keeps it.
    if (territory.interface_id < 0 || territory.interface_id >= 8) territory.interface_id = -1;

    const std::string_view neighbours = doc.attribute(i, "neighbours");
    std::size_t pos = 0;
    while (pos <= neighbours.size()) {
      const std::size_t comma = neighbours.find(',', pos);
      const std::string_view field = trim(
          neighbours.substr(pos, comma == std::string_view::npos ? std::string_view::npos
                                                                 : comma - pos));
      if (!field.empty()) territory.neighbours.emplace_back(field);
      if (comma == std::string_view::npos) break;
      pos = comma + 1;
    }

    // A territory with no `id` cannot be named by `SetTerritoryState` or by
    // another territory's `neighbours`, which makes it unreachable rather than
    // merely odd.
    if (territory.id.empty()) return FormatError::malformed;
    out.territories_.push_back(std::move(territory));
  }

  if (out.territories_.empty()) return FormatError::malformed;
  return out;
}

std::int32_t ConquestMap::find(std::string_view id) const noexcept {
  for (std::size_t i = 0; i < territories_.size(); ++i) {
    if (territories_[i].id == id) return static_cast<std::int32_t>(i);
  }
  return -1;
}

Status ConquestMap::validate() const {
  for (std::size_t i = 0; i < territories_.size(); ++i) {
    for (std::size_t j = i + 1; j < territories_.size(); ++j) {
      if (territories_[i].id == territories_[j].id) return FormatError::malformed;
    }
    for (const std::string& neighbour : territories_[i].neighbours) {
      if (find(neighbour) < 0) return FormatError::malformed;
    }
  }
  return {};
}

Status ConquestMap::validate_bonuses(std::span<const std::string> sequence_names) const {
  for (const Territory& territory : territories_) {
    const bool known = std::find(sequence_names.begin(), sequence_names.end(),
                                 territory.bonus) != sequence_names.end();
    if (!known) return FormatError::not_found;
  }
  return {};
}

std::vector<TerritoryMap> resolve_maps(
    const ConquestMap& conquest,
    std::span<const std::pair<std::int32_t, std::string>> map_names) {
  std::vector<TerritoryMap> out;
  out.reserve(conquest.territories().size());
  for (std::size_t i = 0; i < conquest.territories().size(); ++i) {
    TerritoryMap entry;
    entry.territory = static_cast<std::int32_t>(i);
    const std::string& wanted = conquest.territories()[i].map_name;
    // Lowest map number wins a tie, so the answer does not depend on how the
    // caller walked the container. No retail container has a tie.
    for (const auto& [number, name] : map_names) {
      if (name != wanted) continue;
      if (entry.map_number < 0 || number < entry.map_number) entry.map_number = number;
    }
    out.push_back(entry);
  }
  return out;
}

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

void CampaignSystem::advance(World& world, const Turn& turn) {
  (void)world;
  (void)turn;
}

void CampaignSystem::hash(std::uint64_t& accumulator) const {
  for (const TerritoryState state : progress_.states) {
    mix(accumulator, static_cast<std::uint64_t>(static_cast<std::uint32_t>(
                         static_cast<std::int32_t>(state))));
  }
  for (const std::int32_t territory : progress_.conquered) {
    mix(accumulator, static_cast<std::uint64_t>(static_cast<std::uint32_t>(territory)));
  }
  mix_string(accumulator, progress_.active_bonus);
  // The active notes. State, and folded here rather than left out: 24
  // `IsNoteActive` sites branch on it, so two peers that disagree about which
  // notes are up take different branches.
  board_.hash(accumulator);
  // And the conversation results, for exactly the same reason: five
  // `ConvResult` sites branch on one, and one of the five branches all the way
  // to `ChangeMap`.
  results_.hash(accumulator);
}

void CampaignSystem::configure(const ConquestMap& conquest) {
  ids_.clear();
  bonuses_.clear();
  progress_ = CampaignProgress{};
  ids_.reserve(conquest.territories().size());
  bonuses_.reserve(conquest.territories().size());
  progress_.states.reserve(conquest.territories().size());
  for (const Territory& territory : conquest.territories()) {
    ids_.push_back(territory.id);
    bonuses_.push_back(territory.bonus);
    progress_.states.push_back(territory.state);
  }
}

Status CampaignSystem::restore(const CampaignProgress& progress) {
  if (progress.states.size() != ids_.size()) return FormatError::malformed;
  for (const std::int32_t territory : progress.conquered) {
    if (territory < 0 || static_cast<std::size_t>(territory) >= ids_.size()) {
      return FormatError::out_of_range;
    }
  }
  progress_ = progress;
  return {};
}

Status CampaignSystem::restore(const CampaignCarry& carry) {
  if (carry.territories != ids_) return FormatError::malformed;
  return restore(carry.progress);
}

CampaignCarry CampaignSystem::carry(std::string_view container,
                                    const MissionResult& result) const {
  CampaignCarry out;
  out.container.assign(container);
  out.territories = ids_;
  out.progress = progress_;
  if (apply_mission_result(out.progress, result) || result.won) {
    // The inference, in the one place it is acted on: the reward on the next
    // mission is the last conquered territory's.
    if (!out.progress.conquered.empty()) {
      out.progress.active_bonus.assign(bonus_of(out.progress.conquered.back()));
    }
  }
  return out;
}

std::int32_t CampaignSystem::find(std::string_view id) const noexcept {
  for (std::size_t i = 0; i < ids_.size(); ++i) {
    if (ids_[i] == id) return static_cast<std::int32_t>(i);
  }
  return -1;
}

std::string_view CampaignSystem::bonus_of(std::int32_t territory) const noexcept {
  if (territory < 0 || static_cast<std::size_t>(territory) >= bonuses_.size()) return {};
  return bonuses_[static_cast<std::size_t>(territory)];
}

TerritoryState CampaignSystem::state_of(std::string_view id,
                                        TerritoryState fallback) const noexcept {
  const std::int32_t index = find(id);
  if (index < 0) return fallback;
  return progress_.states[static_cast<std::size_t>(index)];
}

bool CampaignSystem::set_state(std::string_view id, TerritoryState state) {
  const std::int32_t index = find(id);
  if (index < 0) return false;
  progress_.states[static_cast<std::size_t>(index)] = state;
  if (state == TerritoryState::owned &&
      std::find(progress_.conquered.begin(), progress_.conquered.end(), index) ==
          progress_.conquered.end()) {
    progress_.conquered.push_back(index);
  }
  return true;
}

void CampaignSystem::set_active_bonus(std::string_view bonus) {
  progress_.active_bonus.assign(bonus);
}

CampaignSystem* campaign_system_of(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "campaign") {
      return static_cast<CampaignSystem*>(system);
    }
  }
  return nullptr;
}

const CampaignSystem* campaign_system_of(const World& world) noexcept {
  return campaign_system_of(const_cast<World&>(world));
}

// --------------------------------------------------------------------------
// applying a result
// --------------------------------------------------------------------------

bool apply_mission_result(CampaignProgress& progress, const MissionResult& result) {
  if (!result.won) return false;
  if (result.territory < 0) return false;
  if (static_cast<std::size_t>(result.territory) >= progress.states.size()) return false;
  if (std::find(progress.conquered.begin(), progress.conquered.end(), result.territory) !=
      progress.conquered.end()) {
    return false;
  }
  progress.conquered.push_back(result.territory);
  return true;
}

std::string_view last_conquered_bonus(const ConquestMap& conquest,
                                      const CampaignProgress& progress) noexcept {
  if (progress.conquered.empty()) return {};
  const std::int32_t territory = progress.conquered.back();
  if (territory < 0 || static_cast<std::size_t>(territory) >= conquest.territories().size()) {
    return {};
  }
  return conquest.territories()[static_cast<std::size_t>(territory)].bonus;
}

// --------------------------------------------------------------------------
// registration
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// the sequences a mission is made of
// --------------------------------------------------------------------------

namespace {

/// The three status strings, compared byte for byte in `gbr.exe` against
/// literals at 0x007ccf18, 0x007ccf20 and 0x007ccf0c.
constexpr std::string_view kStatusWaiting = "Waiting";
constexpr std::string_view kStatusRunning = "Running";
constexpr std::string_view kStatusFinished = "Finished";

/// `"/SequenceStatus/" + name`, the key both the getter (0x005b9a90) and the
/// setter (0x005b9990) build. Root scope, so the leading slash is the whole of
/// what makes it absolute -- see `MakePath` in `sim/env.hpp`.
[[nodiscard]] std::string status_key(std::string_view name) {
  std::string key = "/SequenceStatus/";
  key.append(name);
  return key;
}

void write_status(World& world, std::string_view name, std::string_view status) {
  EnvSystem* env = env_of(world);
  if (env == nullptr) return;
  env->env().write_string(EnvScope::root(), status_key(name), status);
}

[[nodiscard]] std::string_view read_status(World& world, std::string_view name) {
  EnvSystem* env = env_of(world);
  if (env == nullptr) return {};
  return env->env().read_string(EnvScope::root(), status_key(name));
}

}  // namespace

void CampaignSystem::add_sequence(std::string_view name, std::string_view script) {
  if (find_sequence(name) != nullptr) return;  // `%s duplicate sequence name`
  sequences_.push_back(SequenceEntry{std::string(name), std::string(script), script::kNoScript});
}

CampaignSystem::SequenceEntry* CampaignSystem::find_sequence(std::string_view name) noexcept {
  for (SequenceEntry& entry : sequences_) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

void CampaignSystem::reap_sequences(World& world, const script::Scheduler& scheduler) {
  for (SequenceEntry& entry : sequences_) {
    if (entry.running == script::kNoScript) continue;
    if (scheduler.alive(entry.running)) continue;
    entry.running = script::kNoScript;
    write_status(world, entry.name, kStatusFinished);
  }
}

void set_sequence_waiting(World& world, std::string_view name) {
  write_status(world, name, kStatusWaiting);
}

void set_sequence_running(World& world, std::string_view name) {
  write_status(world, name, kStatusRunning);
}

namespace {

/// `RunSequence(str)` -- 34 sites, all inside campaign containers, every
/// argument a string literal and every literal resolving to a `<sequence
/// name>` declared in its own container. 28 resolve inside a map's manifest
/// and 7 inside the conquest's root one; not one crosses scopes.
///
/// `Start` (0x005b9d40) has three branches and only the middle one is
/// surprising:
///
///   * no compiled script -> status `"Finished"`, return 0;
///   * status already `"Running"` -> **return 0 and do nothing at all**; and
///   * `"Waiting"` *or* `"Finished"` -> spawn, record the thread, set
///     `"Running"`, return 1.
///
/// That a *finished* sequence restarts is not a corner: it is the load-bearing
/// idiom at a live site. Numantia's `seq3.vs` runs
/// `RunSequence("BestTarget"); while (IsRunning("BestTarget")) Sleep(100);`
/// inside a `for` over 28 players, so it restarts a finished sequence 28 times
/// per pass, and an implementation that treats the second call as a no-op
/// deadlocks on iteration two.
///
/// The status is written **before** returning, not when the coroutine first
/// runs. It has to be: that same loop tests the predicate before the new
/// thread has executed a single statement.
///
/// An unknown name prints `Could not find sequence named '%s' in function
/// 'RunSequence'. Check the spelling.` (0x007cd170) through the sink that is a
/// bare `ret` in the retail build, and the calling script runs on. So this
/// does not trap -- the `GetSettlement/1` rule. Worth recording that the
/// original emits that same message in three cases, two of which it fits
/// badly: unknown name, already running, and never compiled.
///
/// Nothing is pushed. Return type 0 in the registration at 0x005bcb98.
HostOutcome fn_run_sequence(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RunSequence: no world");
  if (!ctx.arg(0).is_string()) return HostOutcome::failed("RunSequence: expected a name");
  CampaignSystem* campaign = campaign_system_of(*world);
  if (campaign == nullptr) return HostOutcome::ok_void();

  const std::string_view name = ctx.arg(0).as_string();
  CampaignSystem::SequenceEntry* entry = campaign->find_sequence(name);
  if (entry == nullptr) return HostOutcome::ok_void();
  if (read_status(*world, name) == kStatusRunning) return HostOutcome::ok_void();

  script::Scheduler* scheduler = ctx.scheduler;
  if (scheduler == nullptr) return HostOutcome::ok_void();
  // Resolved by exact path, never through `Scheduler::find_chunk`, whose
  // basename fallback would happily match a different map's `seq1.vs`.
  const std::uint32_t chunk = scheduler->find_chunk_exact(entry->script);
  if (chunk == script::kNoChunk) {
    // `Start`'s first branch: a sequence with no compiled script is `Finished`
    // rather than pending, which is what makes `IsFinished` true for a broken
    // one.
    write_status(*world, name, kStatusFinished);
    return HostOutcome::ok_void();
  }
  const script::ScriptId id = scheduler->spawn(chunk);
  if (id == script::kNoScript) return HostOutcome::ok_void();
  entry->running = id;
  write_status(*world, name, kStatusRunning);
  return HostOutcome::ok_void();
}

/// `IsRunning(str)`, `IsWaiting(str)`, `IsFinished(str)` -- 5, 1 and 3 sites.
///
/// **They never consult the sequence table.** All three build the same env key
/// from the raw argument and compare the value against their own literal, so
/// an unknown name reads the empty string and every one of them answers false,
/// silently and with no diagnostic. That asymmetry with `RunSequence` is real
/// and is preserved.
template <std::string_view const& kStatus>
HostOutcome fn_sequence_is(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsRunning: no world");
  if (!ctx.arg(0).is_string()) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(
      Value::boolean(read_status(*world, ctx.arg(0).as_string()) == kStatus));
}

/// `GiveNote(id)` -- 115 sites, every one of them a string literal.
///
/// **A note nothing declares cannot be given.** The add helper (0x005584a0)
/// opens by looking the id up in the catalogue (0x00557df0) and returns
/// immediately when it answers null, before it touches the active map. That
/// fires on shipped data exactly twice; `sim/note.hpp` names both, and they
/// are one note spelled two ways.
///
/// Nothing is pushed and nothing traps: the return type is 0 in the
/// registration at 0x00558c40, and an id that is not declared, is already
/// given, or is not a string at all leaves the board exactly as it was.
HostOutcome fn_give_note(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GiveNote: no world");
  CampaignSystem* campaign = campaign_system_of(*world);
  if (campaign == nullptr || !ctx.arg(0).is_string()) return HostOutcome::ok_void();
  const std::string_view id = ctx.arg(0).as_string();
  if (campaign->notes().find(id) == nullptr) return HostOutcome::ok_void();
  (void)campaign->note_board().give(id);
  return HostOutcome::ok_void();
}

/// `RemoveNote(id)` -- 65 sites.
///
/// **No catalogue gate**, which is the asymmetry worth keeping: 0x005581d0
/// goes straight to the active map, so a note can be taken off the list by an
/// id the container never declared -- it just was never on it. The 65 shipped
/// sites all name declared ids anyway.
HostOutcome fn_remove_note(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RemoveNote: no world");
  CampaignSystem* campaign = campaign_system_of(*world);
  if (campaign == nullptr || !ctx.arg(0).is_string()) return HostOutcome::ok_void();
  (void)campaign->note_board().remove(ctx.arg(0).as_string());
  return HostOutcome::ok_void();
}

/// `ClearNotes()` -- 16 sites, and it is how a mission ends: Zama's lose
/// condition is `WaitEmptyQuery(Q_Scipio, -1); ClearNotes(); EndGame(...)`.
/// Takes every note off the list and leaves the catalogue alone.
HostOutcome fn_clear_notes(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ClearNotes: no world");
  if (CampaignSystem* campaign = campaign_system_of(*world)) campaign->note_board().clear();
  return HostOutcome::ok_void();
}

/// `IsNoteActive(id)` -- 24 sites, and the one entry point of the four that is
/// a predicate over state rather than a write.
///
/// It consults the **board**, never the catalogue: 0x00557e40 is a `find` on
/// the active map and a comparison against `end()`. So a declared note that
/// was never given reads false, and -- because `RemoveNote` has no catalogue
/// gate either -- an undeclared id reads false rather than refusing.
HostOutcome fn_is_note_active(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsNoteActive: no world");
  CampaignSystem* campaign = campaign_system_of(*world);
  if (campaign == nullptr || !ctx.arg(0).is_string()) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  return HostOutcome::ok_with(
      Value::boolean(campaign->note_board().active(ctx.arg(0).as_string())));
}

}  // namespace

std::size_t register_campaign_host(script::HostRegistry& registry) {
  std::size_t defined = 0;
  const auto def = [&](script::CallKind kind, std::string_view name, std::uint16_t arity,
                       script::HostFn fn) {
    registry.define(kind, name, arity, fn);
    ++defined;
  };

  def(kFree, "ConquestBonus", 0, &fn_conquest_bonus);
  def(kFree, "SetTerritoryState", 2, &fn_set_territory_state);
  def(kFree, "GetTerritoryState", 1, &fn_get_territory_state);
  // The sequence runner. `StopSequence` and `IsSequenceRunning` are absent on
  // purpose: neither exists in `gbr.exe` and neither has a call site.
  def(kFree, "RunSequence", 1, &fn_run_sequence);                 // 34
  def(kFree, "IsRunning", 1, &fn_sequence_is<kStatusRunning>);    //  5
  def(kFree, "IsFinished", 1, &fn_sequence_is<kStatusFinished>);  //  3
  def(kFree, "IsWaiting", 1, &fn_sequence_is<kStatusWaiting>);    //  1
  // The notes a mission pins on the player's list. `ShowNotes/0` is the fifth
  // entry point in `gbr.exe`'s block at 0x00558c30 and is deliberately absent:
  // it opens an interface panel and has no call site in the installation.
  def(kFree, "GiveNote", 1, &fn_give_note);          // 115
  def(kFree, "RemoveNote", 1, &fn_remove_note);      //  65
  def(kFree, "IsNoteActive", 1, &fn_is_note_active); //  24
  def(kFree, "ClearNotes", 0, &fn_clear_notes);      //  16

  return defined;
}

std::size_t campaign_host_entry_count() noexcept { return 11; }

// --------------------------------------------------------------------------
// the campaign between missions
// --------------------------------------------------------------------------

namespace {

void put_text(std::vector<std::byte>& out, std::string_view text) {
  for (const char c : text) out.push_back(static_cast<std::byte>(c));
}

std::string joined(const std::vector<std::string>& items) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out += ',';
    out += items[i];
  }
  return out;
}

std::string joined_ints(std::span<const std::int32_t> items) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out += ',';
    out += std::to_string(items[i]);
  }
  return out;
}

/// Comma-separated fields, trimmed of spaces; empty fields are skipped.
std::vector<std::string_view> split_list(std::string_view text) {
  std::vector<std::string_view> out;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t end = text.find(',', start);
    if (end == std::string_view::npos) end = text.size();
    std::string_view field = text.substr(start, end - start);
    while (!field.empty() && field.front() == ' ') field.remove_prefix(1);
    while (!field.empty() && field.back() == ' ') field.remove_suffix(1);
    if (!field.empty()) out.push_back(field);
    if (end == text.size()) break;
    start = end + 1;
  }
  return out;
}

/// The whole field as a decimal integer, or a refusal: a number read half-way
/// is not a number.
bool whole_int(std::string_view text, std::int32_t& out) noexcept {
  if (text.empty()) return false;
  std::int64_t value = 0;
  bool negative = false;
  std::size_t i = 0;
  if (text[0] == '-') {
    negative = true;
    i = 1;
    if (text.size() == 1) return false;
  }
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + (text[i] - '0');
    if (value > 0x7FFFFFFF) return false;
  }
  out = static_cast<std::int32_t>(negative ? -value : value);
  return true;
}

}  // namespace

std::vector<std::byte> encode_campaign_carry(const CampaignCarry& carry) {
  std::vector<std::byte> out;
  std::vector<std::int32_t> states;
  states.reserve(carry.progress.states.size());
  for (const TerritoryState state : carry.progress.states) {
    states.push_back(static_cast<std::int32_t>(state));
  }
  put_text(out, "[Campaign]\r\n");
  put_text(out, "container=" + carry.container + "\r\n");
  put_text(out, "territories=" + joined(carry.territories) + "\r\n");
  put_text(out, "states=" + joined_ints(states) + "\r\n");
  put_text(out, "conquered=" + joined_ints(carry.progress.conquered) + "\r\n");
  put_text(out, "active_bonus=" + carry.progress.active_bonus + "\r\n");
  return out;
}

Result<CampaignCarry> decode_campaign_carry(std::span<const std::byte> ini) {
  const Result<IniDocument> document = IniDocument::parse(ini);
  if (!document.ok()) return document.error();
  const SectionIndex section = document->section("Campaign");
  if (section == kNoSection) return FormatError::malformed;

  CampaignCarry carry;
  carry.container.assign(document->value(section, "container"));
  if (carry.container.empty()) return FormatError::malformed;
  for (const std::string_view id : split_list(document->value(section, "territories"))) {
    carry.territories.emplace_back(id);
  }
  const std::vector<std::string_view> states = split_list(document->value(section, "states"));
  if (states.size() != carry.territories.size()) return FormatError::malformed;
  for (const std::string_view text : states) {
    std::int32_t value = 0;
    if (!whole_int(text, value)) return FormatError::malformed;
    const Result<TerritoryState> state = territory_state(value);
    if (!state.ok()) return state.error();
    carry.progress.states.push_back(*state);
  }
  for (const std::string_view text : split_list(document->value(section, "conquered"))) {
    std::int32_t value = 0;
    if (!whole_int(text, value)) return FormatError::malformed;
    if (value < 0 || static_cast<std::size_t>(value) >= carry.territories.size()) {
      return FormatError::out_of_range;
    }
    carry.progress.conquered.push_back(value);
  }
  carry.progress.active_bonus.assign(document->value(section, "active_bonus"));
  return carry;
}

}  // namespace imperivm::core::sim
