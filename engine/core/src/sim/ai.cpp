// The AI bootstrap. See include/imperivm/core/sim/ai.hpp for what `gbr.exe`
// says `AIStart` does, for why `GAIKACount` is not here, and for the evidence
// behind every choice below.

#include "imperivm/core/sim/ai.hpp"

#include <algorithm>
#include <utility>

#include "imperivm/core/script/bytecode.hpp"
#include "imperivm/core/script/vm.hpp"
#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/gaika_table.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/squad.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

using script::CallContext;
using script::HostOutcome;
using script::ScriptId;
using script::Value;

[[nodiscard]] std::string upper(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c);
  }
  return out;
}

/// Lowercased, with `\` folded to `/`, which is how the scheduler compares two
/// script paths. Restated here rather than shared because `script/scheduler.cpp`
/// keeps its copy private, and a *second* normalisation that disagreed would be
/// worse than one that is checked by test.
[[nodiscard]] std::string normalise(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const char folded = c == '\\' ? '/' : c;
    out.push_back(folded >= 'A' && folded <= 'Z' ? static_cast<char>(folded + ('a' - 'A'))
                                                 : folded);
  }
  return out;
}

}  // namespace

// --------------------------------------------------------------------------
// GaikaView
// --------------------------------------------------------------------------

void GaikaView::clear() noexcept {
  map_.clear();
  laika_.clear();
}

void GaikaView::reset(std::int32_t nodes) {
  clear();
  if (nodes < 0) return;
  // One more than the real nodes: index 0 is the reserved one, and the
  // original's constructor sizes both vectors to the whole `GAIKAs` vector,
  // which includes it.
  const auto slots = static_cast<std::size_t>(nodes) + 1;
  map_.resize(slots);
  laika_.resize(slots);
  for (std::size_t i = 0; i < slots; ++i) {
    map_[i] = static_cast<std::int32_t>(i);
    laika_[i] = Laika{};
    laika_[i].gaika = static_cast<GaikaId>(i);
  }
}

const Laika* GaikaView::find(GaikaId gaika) const noexcept {
  if (gaika < 0 || static_cast<std::size_t>(gaika) >= map_.size()) return nullptr;
  // **The test is on the slot, not on the id**, which is the original's and is
  // not the same guard written differently: slot 0 is the reserved node's, and
  // it is where node 0 lives *because* `set_priority` never moves anything into
  // or out of it. A view whose sort had a bug would answer nothing for whatever
  // it had wrongly put there, which is the failure this shape reports rather
  // than hides.
  const std::int32_t slot = map_[static_cast<std::size_t>(gaika)];
  if (slot <= 0 || static_cast<std::size_t>(slot) >= laika_.size()) return nullptr;
  return &laika_[static_cast<std::size_t>(slot)];
}

Laika* GaikaView::find(GaikaId gaika) noexcept {
  return const_cast<Laika*>(static_cast<const GaikaView*>(this)->find(gaika));
}

GaikaId GaikaView::ranked(std::int32_t index) const noexcept {
  if (index < 0 || static_cast<std::size_t>(index) >= laika_.size()) return kNoGaika;
  return laika_[static_cast<std::size_t>(index)].gaika;
}

void GaikaView::swap_slots(std::int32_t a, std::int32_t b) noexcept {
  auto& first = laika_[static_cast<std::size_t>(a)];
  auto& second = laika_[static_cast<std::size_t>(b)];
  // The map is repaired from each record's own `gaika`, which is what keeps
  // `laika[map[g]].gaika == g` true at every step rather than only at the end.
  map_[static_cast<std::size_t>(first.gaika)] = b;
  map_[static_cast<std::size_t>(second.gaika)] = a;
  std::swap(first, second);
}

void GaikaView::set_priority(GaikaId gaika, std::int32_t priority) {
  if (gaika <= kNoGaika) return;
  if (static_cast<std::size_t>(gaika) >= map_.size()) return;
  std::int32_t slot = map_[static_cast<std::size_t>(gaika)];
  if (slot <= 0 || static_cast<std::size_t>(slot) >= laika_.size()) return;

  const std::int32_t previous = laika_[static_cast<std::size_t>(slot)].priority;
  // **No change, no write and no flag.** `Prioritized` means "SetPriority has
  // run on this node with a value that differed", which is what makes
  // `GAIKAMONITOR.VS`'s wait on the lowest-ranked node's flag terminate only
  // after a real pass.
  if (previous == priority) return;
  laika_[static_cast<std::size_t>(slot)].priority = priority;
  laika_[static_cast<std::size_t>(slot)].flags |= kLaikaPrioritized;

  const auto count = static_cast<std::int32_t>(laika_.size());
  if (priority > previous) {
    // Toward the front, and never past slot 1: slot 0 is the reserved node.
    while (slot > 1 && laika_[static_cast<std::size_t>(slot) - 1].priority < priority) {
      swap_slots(slot, slot - 1);
      --slot;
    }
  } else {
    while (slot + 1 < count && laika_[static_cast<std::size_t>(slot) + 1].priority > priority) {
      swap_slots(slot, slot + 1);
      ++slot;
    }
  }
  // Both loops stop on equality, so the sort is **stable** and a run of equal
  // priorities keeps the order it was built in.
}

bool GaikaView::adopt(std::vector<std::int32_t> map, std::vector<Laika> records) {
  if (map.size() != records.size()) return false;
  for (std::size_t g = 0; g < map.size(); ++g) {
    const std::int32_t slot = map[g];
    if (slot < 0 || static_cast<std::size_t>(slot) >= records.size()) return false;
    if (static_cast<std::size_t>(records[static_cast<std::size_t>(slot)].gaika) != g) return false;
  }
  map_ = std::move(map);
  laika_ = std::move(records);
  return true;
}

// --------------------------------------------------------------------------
// AiSystem
// --------------------------------------------------------------------------

void AiSystem::advance(World& world, const Turn& turn) {
  // The AI is its scripts and the scheduler runs those; the original's `CVXAI`
  // has no per-frame tick either -- `CVXAI::Start` runs `Main.vs` and stops.
  //
  // **What runs here is the per-node visibility sweep** (0x0044ecc0,
  // `sweep_gaika`), on every node. The original runs it on a node when a unit
  // enters the world in it or crosses into or out of it (0x0041f310,
  // 0x0041f530 on the old node and the new, 0x0041e970, 0x0041eee0), and on
  // every node once the table is built (0x0044f2b0). Its effect only changes
  // when who stands in a node changes, and here that is `revalue_squads`'
  // once-a-tick refresh of where each squad stands; sweeping every node once
  // a tick after it is the same sweep at the same granularity. For each
  // seated AI the sweep marks a node `Explored` and `Revealed` while that
  // player or one sharing its view stands in it (a squad there, or the
  // settlement), and when that stops clears `Revealed`, stamps `LastSeen` and
  // remembers the enemy strength standing there (`Laika::enemies`), which the
  // `Eval` memory tail reads.
  //
  // **This replaced a reading.** Until the twelfth round a node was marked
  // explored from the fog's exploration map (its centre or any slot of it
  // explored for the player), `LastSeen` was "now" for any explored node and
  // `Revealed` never set. The original never consults the exploration map for
  // a node: 0x0041cb80 reads the bit the sweep sets, or a session override
  // (`laika_explored`).
  //
  // The view is saved and not hashed; the sweep is a function of it and of
  // the squads, so a load continues identically.
  (void)turn;
  const std::int32_t nodes = world.gaika().count();
  for (GaikaId id = 1; id <= static_cast<GaikaId>(nodes); ++id) {
    sweep_gaika(world, id, turn.time);
  }
}

void AiSystem::add_profile(std::string_view name, const AiProfile* profile,
                           std::string_view script_prefix) {
  ProfileEntry entry;
  entry.name = upper(name);
  entry.profile = profile;
  entry.script_prefix = std::string(script_prefix);
  const auto at = std::lower_bound(
      profiles_.begin(), profiles_.end(), entry.name,
      [](const ProfileEntry& e, const std::string& key) { return e.name < key; });
  if (at != profiles_.end() && at->name == entry.name) {
    *at = std::move(entry);
    return;
  }
  profiles_.insert(at, std::move(entry));
}

const AiSystem::ProfileEntry* AiSystem::profile_entry(std::string_view name) const noexcept {
  const std::string key = upper(name);
  const auto at = std::lower_bound(
      profiles_.begin(), profiles_.end(), key,
      [](const ProfileEntry& e, const std::string& k) { return e.name < k; });
  if (at == profiles_.end() || at->name != key) return nullptr;
  return &*at;
}

const AiProfile* AiSystem::profile_for(std::string_view name) const noexcept {
  const ProfileEntry* entry = profile_entry(name);
  return entry == nullptr ? nullptr : entry->profile;
}

bool AiSystem::has_profile(std::string_view name) const noexcept {
  return profile_entry(name) != nullptr;
}

const AiSystem::ProfileEntry* AiSystem::profile_of_player(PlayerId player) const noexcept {
  const AiPlayer* ai = player_ai(player);
  if (ai == nullptr || !ai->active) return nullptr;
  return profile_entry(ai->profile);
}

const AiPlayer* AiSystem::player_ai(PlayerId player) const noexcept {
  if (player >= players_.size()) return nullptr;
  return &players_[player];
}

std::size_t AiSystem::active_count() const noexcept {
  std::size_t count = 0;
  for (const AiPlayer& ai : players_) {
    if (ai.active) ++count;
  }
  return count;
}

AiStartStatus AiSystem::start(PlayerId player, std::string_view profile, AiDifficulty difficulty,
                              script::Scheduler& scheduler) {
  // 1..16 in the original's numbering; here the table is 0-based and the same
  // width, so the bound is the table's.
  if (player >= players_.size()) return AiStartStatus::bad_player;

  // The original uppercases and then refuses an unknown name outright, rather
  // than falling back to the root profile.
  const std::string key = upper(profile);
  const ProfileEntry* entry = profile_entry(key);
  if (entry == nullptr) return AiStartStatus::unknown_profile;

  // Starting an AI a player already has is a restart, not an error: the core
  // destroys the old object before allocating the new one.
  stop(player, scheduler);

  AiPlayer& ai = players_[player];
  ai.player = player;
  ai.profile = key;
  ai.difficulty = difficulty;

  const std::uint32_t chunk = find_script(scheduler, player, "Main.vs");
  if (chunk == script::kNoChunk) {
    ai.active = false;
    return AiStartStatus::no_main_script;
  }

  // `CVXAI::Start` runs exactly one script, with no arguments and signature
  // `void`, through the AI's own launcher asked for no particular id
  // (0x0041e338 -> 0x0041cd00 with -1): the first free id of the `AIRun`
  // range, which on a fresh AI is 3, tagged with the seat.
  ai.active = true;
  const std::int32_t slot = run(player, chunk, {}, scheduler);
  if (slot < 0) {
    ai.active = false;
    return AiStartStatus::no_main_script;
  }
  ai.root = run_script(player, slot, scheduler);
  return AiStartStatus::ok;
}

std::string ai_thread_context(PlayerId seat) {
  return env_format_int(static_cast<std::int32_t>(seat));
}

void AiSystem::seed_gaika_view(PlayerId player, std::int32_t nodes) {
  if (player >= players_.size()) return;
  players_[player].gaika.reset(nodes);
}

GaikaView* AiSystem::gaika_view(PlayerId player) noexcept {
  return player < players_.size() ? &players_[player].gaika : nullptr;
}

const GaikaView* AiSystem::gaika_view(PlayerId player) const noexcept {
  return player < players_.size() ? &players_[player].gaika : nullptr;
}

bool AiSystem::stop(PlayerId player, script::Scheduler& scheduler) {
  if (player >= players_.size()) return false;
  AiPlayer& ai = players_[player];
  if (!ai.active) return false;

  // The AI object goes, and its thread manager with it: every thread in it is
  // stopped. Those are exactly the live threads whose context is the seat,
  // since the AI's launcher is the only one that writes a seat. In id order;
  // `kill` only marks, so the walk is over a vector nothing reshapes.
  const std::string tag = ai_thread_context(player);
  for (const script::ScriptRecord& record : scheduler.scripts()) {
    if (!record.dead && record.context == tag) scheduler.kill(record.id);
  }

  ai.active = false;
  ai.root = script::kNoScript;
  ai.profile.clear();
  ai.difficulty = AiDifficulty::none;
  // The view, the `AIRun` ids and the settlement slots live on the AI object
  // in the original and are constructed with it, so they go with it -- and a
  // restart rebuilds them from scratch, which is why `start` calls `stop`
  // first. Another seat's slots for the same settlements are its own.
  ai.gaika.clear();
  ai.runs.fill(script::kNoScript);
  ai.settlements.clear();
  return true;
}

// -- whose thread -----------------------------------------------------------

std::int32_t AiSystem::thread_seat(std::string_view context) noexcept {
  if (context.empty()) return -1;
  return env_parse_int(context);
}

std::int32_t AiSystem::script_player(ScriptId script,
                                     const script::Scheduler& scheduler) const {
  const std::string_view context = scheduler.context(script);
  if (context.empty()) return -1;
  return thread_seat(context) + 1;
}

// -- the AIRun range --------------------------------------------------------

std::int32_t AiSystem::run(PlayerId seat, std::uint32_t chunk,
                           std::span<const script::Value> args, script::Scheduler& scheduler,
                           ScriptId parent) {
  if (seat >= players_.size()) return -1;
  AiPlayer& ai = players_[seat];
  if (!ai.active) return -1;
  for (std::size_t i = 0; i < ai.runs.size(); ++i) {
    if (ai.runs[i] != script::kNoScript && scheduler.alive(ai.runs[i])) continue;
    // Started at once with the manager's default interval (0x006a03d0 with
    // 0 and 100), then tagged (0x0041cd8c .. 0x0041cdaa).
    const ScriptId spawned = scheduler.spawn(chunk, args, script::ObjectRef{}, parent);
    if (spawned == script::kNoScript) return -1;
    (void)scheduler.set_context(spawned, ai_thread_context(seat));
    ai.runs[i] = spawned;
    return kFirstAiRunSlot + static_cast<std::int32_t>(i);
  }
  return -1;
}

bool AiSystem::break_run(PlayerId seat, std::int32_t handle, script::Scheduler& scheduler) {
  const ScriptId script = run_script(seat, handle, scheduler);
  if (script == script::kNoScript) return false;
  // 0x0069fee0: at once, or -- for the thread running right now -- by
  // flagging it to stop after the current call; `kill` is safe on either.
  return scheduler.kill(script);
}

ScriptId AiSystem::run_script(PlayerId seat, std::int32_t handle,
                              const script::Scheduler& scheduler) const {
  if (seat >= players_.size()) return script::kNoScript;
  if (handle < kFirstAiRunSlot || handle >= kAiRunSlotEnd) return script::kNoScript;
  const ScriptId script = players_[seat].runs[static_cast<std::size_t>(handle - kFirstAiRunSlot)];
  return scheduler.alive(script) ? script : script::kNoScript;
}

// -- per-settlement slots ---------------------------------------------------

AiSettlementScripts* AiSystem::slot(PlayerId seat, SettlementId settlement) {
  if (seat >= players_.size()) return nullptr;
  std::vector<AiSettlementScripts>& table = players_[seat].settlements;
  const auto at = std::lower_bound(
      table.begin(), table.end(), settlement,
      [](const AiSettlementScripts& e, SettlementId key) { return e.settlement < key; });
  if (at != table.end() && at->settlement == settlement) return &*at;
  AiSettlementScripts fresh;
  fresh.settlement = settlement;
  return &*table.insert(at, fresh);
}

const AiSettlementScripts* AiSystem::slot(PlayerId seat, SettlementId settlement) const {
  if (seat >= players_.size()) return nullptr;
  const std::vector<AiSettlementScripts>& table = players_[seat].settlements;
  const auto at = std::lower_bound(
      table.begin(), table.end(), settlement,
      [](const AiSettlementScripts& e, SettlementId key) { return e.settlement < key; });
  if (at == table.end() || at->settlement != settlement) return nullptr;
  return &*at;
}

std::int32_t AiSystem::economy_script(PlayerId seat, SettlementId settlement,
                                      const script::Scheduler& scheduler) const {
  const AiSettlementScripts* entry = slot(seat, settlement);
  if (entry == nullptr || entry->economy == 0) return 0;
  return scheduler.alive(entry->economy_script) ? entry->economy : 0;
}

std::int32_t AiSystem::tactic_script(PlayerId seat, SettlementId settlement,
                                     const script::Scheduler& scheduler) const {
  const AiSettlementScripts* entry = slot(seat, settlement);
  if (entry == nullptr || entry->tactic == 0) return 0;
  return scheduler.alive(entry->tactic_script) ? entry->tactic : 0;
}

ScriptId AiSystem::economy_thread(PlayerId seat, SettlementId settlement,
                                  const script::Scheduler& scheduler) const {
  const AiSettlementScripts* entry = slot(seat, settlement);
  if (entry == nullptr || !scheduler.alive(entry->economy_script)) return script::kNoScript;
  return entry->economy_script;
}

ScriptId AiSystem::tactic_thread(PlayerId seat, SettlementId settlement,
                                 const script::Scheduler& scheduler) const {
  const AiSettlementScripts* entry = slot(seat, settlement);
  if (entry == nullptr || !scheduler.alive(entry->tactic_script)) return script::kNoScript;
  return entry->tactic_script;
}

void AiSystem::set_economy_script(PlayerId seat, SettlementId settlement, std::int32_t id,
                                  ScriptId script) {
  AiSettlementScripts* entry = slot(seat, settlement);
  if (entry == nullptr) return;
  entry->economy = id;
  entry->economy_script = script;
}

void AiSystem::set_tactic_script(PlayerId seat, SettlementId settlement, std::int32_t id,
                                 ScriptId script) {
  AiSettlementScripts* entry = slot(seat, settlement);
  if (entry == nullptr) return;
  entry->tactic = id;
  entry->tactic_script = script;
}

std::span<const AiSettlementScripts> AiSystem::settlement_scripts(PlayerId seat) const noexcept {
  if (seat >= players_.size()) return {};
  return players_[seat].settlements;
}

// -- script lookup ----------------------------------------------------------

std::uint32_t AiSystem::find_script(const script::Scheduler& scheduler, PlayerId player,
                                    std::string_view file) const {
  const ProfileEntry* entry = profile_of_player(player);
  if (entry != nullptr && !entry->script_prefix.empty()) {
    const std::string wanted = normalise(entry->script_prefix + std::string(file));
    for (std::size_t i = 0; i < scheduler.chunk_count(); ++i) {
      if (normalise(scheduler.chunk(static_cast<std::uint32_t>(i)).source_name) == wanted) {
        return static_cast<std::uint32_t>(i);
      }
    }
  }
  // No profile directory, or the profile does not override this file: fall back
  // to the scheduler's own resolution, which is the root `DATA/AI` copy.
  return scheduler.find_chunk(file);
}

std::string AiSystem::script_file_for(PlayerId player, AiEnum family, std::int32_t value) const {
  const ProfileEntry* entry = profile_of_player(player);
  if (entry == nullptr || entry->profile == nullptr) return {};
  const std::string_view name = entry->profile->constant_name(family, value);
  if (name.empty()) return {};
  return std::string(name) + ".vs";
}

// --------------------------------------------------------------------------
// the world-level entry points
// --------------------------------------------------------------------------

namespace {

[[nodiscard]] std::size_t ship_need_slot(std::span<const AiSystem::ShipNeed> rows, PlayerId player,
                                         LsaId lsa) noexcept {
  std::size_t lo = 0;
  std::size_t hi = rows.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    const AiSystem::ShipNeed& row = rows[mid];
    if (row.player < player || (row.player == player && row.lsa < lsa)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

}  // namespace

std::int32_t AiSystem::ship_needs(PlayerId player, LsaId lsa) const noexcept {
  const std::size_t at = ship_need_slot(ship_needs_, player, lsa);
  if (at < ship_needs_.size() && ship_needs_[at].player == player && ship_needs_[at].lsa == lsa) {
    return ship_needs_[at].count;
  }
  return 0;
}

void AiSystem::add_ship_need(PlayerId player, LsaId lsa) {
  const std::size_t at = ship_need_slot(ship_needs_, player, lsa);
  if (at < ship_needs_.size() && ship_needs_[at].player == player && ship_needs_[at].lsa == lsa) {
    ++ship_needs_[at].count;
    return;
  }
  ship_needs_.insert(ship_needs_.begin() + static_cast<std::ptrdiff_t>(at),
                     ShipNeed{player, lsa, 1});
}

void AiSystem::clear_ship_needs(PlayerId player, LsaId lsa) noexcept {
  const std::size_t at = ship_need_slot(ship_needs_, player, lsa);
  if (at < ship_needs_.size() && ship_needs_[at].player == player && ship_needs_[at].lsa == lsa) {
    ship_needs_[at].count = 0;
  }
}

bool AiSystem::adopt_ship_needs(std::vector<ShipNeed> rows) {
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const ShipNeed& a = rows[i - 1];
    const ShipNeed& b = rows[i];
    if (!(a.player < b.player || (a.player == b.player && a.lsa < b.lsa))) return false;
  }
  ship_needs_ = std::move(rows);
  return true;
}

// -- the ship transport order ----------------------------------------------

namespace {

/// The insertion point for `ship` in a table sorted ascending by id.
[[nodiscard]] std::size_t transport_slot(const std::vector<AiSystem::ShipTransport>& rows,
                                         ObjectId ship) noexcept {
  std::size_t lo = 0;
  std::size_t hi = rows.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (rows[mid].ship < ship) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

/// What a ship with no row reads as: the values the original's constructor
/// writes. Returned by reference, so it has to outlive the call.
const AiSystem::ShipTransport kNoTransport{};

}  // namespace

const AiSystem::ShipTransport& AiSystem::ship_transport(ObjectId ship) const noexcept {
  const std::size_t at = transport_slot(ship_transports_, ship);
  if (at < ship_transports_.size() && ship_transports_[at].ship == ship) {
    return ship_transports_[at];
  }
  return kNoTransport;
}

void AiSystem::set_ship_transport(ObjectId ship, std::string_view order, Point where) {
  if (ship == kNoObject) return;
  const std::size_t at = transport_slot(ship_transports_, ship);
  if (at < ship_transports_.size() && ship_transports_[at].ship == ship) {
    ship_transports_[at].order = std::string(order);
    ship_transports_[at].where = where;
    return;
  }
  ship_transports_.insert(ship_transports_.begin() + static_cast<std::ptrdiff_t>(at),
                          ShipTransport{ship, std::string(order), where});
}

void AiSystem::clear_ship_transport(ObjectId ship) noexcept {
  const std::size_t at = transport_slot(ship_transports_, ship);
  if (at < ship_transports_.size() && ship_transports_[at].ship == ship) {
    ship_transports_.erase(ship_transports_.begin() + static_cast<std::ptrdiff_t>(at));
  }
}

bool AiSystem::adopt_ship_transports(std::vector<ShipTransport> rows) {
  for (std::size_t i = 1; i < rows.size(); ++i) {
    if (!(rows[i - 1].ship < rows[i].ship)) return false;
  }
  ship_transports_ = std::move(rows);
  return true;
}

AiSystem* ai_system_of(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "ai") return static_cast<AiSystem*>(system);
  }
  return nullptr;
}

Laika* laika_for(World& world, std::int32_t viewer, GaikaId node) noexcept {
  if (viewer < 0 || viewer >= static_cast<std::int32_t>(kPlayerCount)) return nullptr;
  AiSystem* ai = ai_system_of(world);
  GaikaView* view = ai == nullptr ? nullptr : ai->gaika_view(static_cast<PlayerId>(viewer));
  return view == nullptr ? nullptr : view->find(node);
}

// 0x0041cb80 takes one of two overrides before the bit, chosen by 0x005bde80
// (a single-player game whose settings carry `+0xdc4`): the session's
// exploration setting (`+0xda0 == 0`), or else the exploration map's
// explored-everywhere flag (`[[world+0xb0]+0xf0]`, which only 0x005157c0 --
// `ExploreAll`, and the load with exploration off -- sets). The match rule
// is read for both: a load with exploration off is the one way the flag is
// set at the start, and the rule is the one the session keeps. A script's
// `ExploreAll` in a match with exploration on is where the two part (the
// scenario path would answer yes everywhere from then on); labelled.
bool laika_explored(World& world, const Laika& record) noexcept {
  if (const MatchSystem* match = match_system_of(world);
      match != nullptr && !match->rules().exploration) {
    return true;
  }
  return (record.flags & kLaikaExplored) != 0;
}

bool laika_revealed(World& world, const Laika& record) noexcept {
  const MatchSystem* match = match_system_of(world);
  if (match != nullptr && !match->rules().fog_of_war) return laika_explored(world, record);
  return (record.flags & kLaikaRevealed) != 0;
}

// 0x0044ecc0. See `AiSystem::advance` for when, and docs/formats/gaika.md.
void sweep_gaika(World& world, GaikaId node, GameTime now) {
  AiSystem* ai = ai_system_of(world);
  HeroSystem* heroes = hero_system_of(world);
  const GaikaNode* here = world.gaika().find(node);
  if (ai == nullptr || here == nullptr) return;
  // Who is in the node: the owner of every squad on its deque that stands in
  // it, and the owner of its settlement.
  std::uint32_t present = 0;
  if (heroes != nullptr) {
    SquadTable& table = heroes->squads();
    for (const SquadKey& key : table.filed_under(node)) {
      const Squad* squad = table.find(key);
      if (squad != nullptr && squad->gaika_in == node && key.player < kPlayerCount) {
        present |= 1u << key.player;
      }
    }
  }
  if (here->settlement != kNoObject) {
    if (const EconomySystem* economy = economy_of(world); economy != nullptr) {
      const Settlement* set = economy->settlements().for_object(here->settlement);
      if (set != nullptr && set->owner < kPlayerCount) present |= 1u << set->owner;
    }
  }
  const PlayerTable& players = world.players();
  for (PlayerId p = 0; p < kPlayerCount; ++p) {
    Laika* record = laika_for(world, static_cast<std::int32_t>(p), node);
    if (record == nullptr) continue;
    // The player sees with its own squads and with those of every player its
    // row shares its view with (flag 0x10, `share_view`).
    std::uint32_t seers = 1u << p;
    for (PlayerId q = 0; q < kPlayerCount; ++q) {
      if (q != p && players.has(p, q, Relation::share_view)) seers |= 1u << q;
    }
    if ((seers & present) != 0) {
      record->flags |= kLaikaExplored | kLaikaRevealed;
      continue;
    }
    if ((record->flags & kLaikaRevealed) == 0) continue;
    // Sight lost: remember when, and what enemy strength was standing there.
    record->flags = static_cast<std::uint16_t>(record->flags & ~kLaikaRevealed);
    record->last_seen = now;
    record->enemies =
        heroes == nullptr ? 0 : seen_enemy_staying(world, *heroes, node, static_cast<std::int32_t>(p));
  }
}

AiStartStatus ai_start(World& world, PlayerId player, std::string_view profile,
                       AiDifficulty difficulty, script::Scheduler& scheduler) {
  AiSystem* ai = ai_system_of(world);
  if (ai == nullptr) return AiStartStatus::no_system;
  const AiStartStatus status = ai->start(player, profile, difficulty, scheduler);
  // **The view is `AIStart`'s last act**, and it is here rather than inside
  // `AiSystem::start` because the node count is the world's and that method
  // takes only a scheduler. The original's constructor sizes both arrays from
  // `GAIKACount` in the same breath as allocating the AI object.
  if (status == AiStartStatus::ok) ai->seed_gaika_view(player, world.gaika().count());
  // The order queue is the AI object's too (0x00449250, constructed with it),
  // so an AI that replaced another starts from an empty one -- and the timer
  // `CVXAI::Start` arms with no delay is the reset queue's own `due` of 0. The
  // two statuses are the ones `start` reaches only after its `stop`.
  if (status == AiStartStatus::ok || status == AiStartStatus::no_main_script) {
    if (HeroSystem* heroes = hero_system_of(world)) heroes->squads().reset_orders(player);
  }
  return status;
}

bool ai_stop(World& world, PlayerId player, script::Scheduler& scheduler) {
  AiSystem* ai = ai_system_of(world);
  if (ai == nullptr || !ai->stop(player, scheduler)) return false;
  // The queue goes with the AI object (0x00448e90): every record freed, every
  // squad of the player answering no `OrderDest`. `stop` has already killed
  // the runner, which it owns.
  if (HeroSystem* heroes = hero_system_of(world)) heroes->squads().reset_orders(player);
  return true;
}

std::size_t ai_start_players(World& world, script::Scheduler& scheduler,
                             std::vector<std::pair<PlayerId, AiStartStatus>>* refused) {
  AiSystem* ai = ai_system_of(world);
  if (ai == nullptr) return 0;
  // The manager exists from here on whether or not any player starts: the
  // original writes `[0x8c8e58]` before it looks at a single player record.
  const bool first_start = !ai->manager_started();
  ai->start_manager();
  std::size_t started = 0;
  for (PlayerId player = 0; player < kPlayerCount; ++player) {
    const PlayerSetup& setup = world.players().setup(player);
    if (setup.control != PlayerControl::computer) continue;
    const AiPlayer* existing = ai->player_ai(player);
    if (existing != nullptr && existing->active) continue;
    // Through `ai_start` rather than `AiSystem::start`, so that the player's
    // node view is seeded the same way `AIStart` seeds it.
    const AiStartStatus status =
        ai_start(world, player, setup.ai_script, ai_difficulty_overlay(setup.difficulty),
                 scheduler);
    if (status == AiStartStatus::ok) {
      ++started;
    } else if (refused != nullptr) {
      refused->emplace_back(player, status);
    }
  }
  // Then the pending list (0x00450aa0, after the player records): every unit
  // the register saw before the AI existed -- the map's own, pushed by the
  // loader (0x0054f181), and any a script placed since -- is registered now,
  // in the order it was pushed, which is id order here. The register's own
  // gates are asked again at the drain; nothing there asks whether the unit
  // still lives or stands in a holder.
  if (first_start) {
    if (HeroSystem* heroes = hero_system_of(world); heroes != nullptr) {
      std::vector<ObjectId> pending;
      for (const WorldObject& slot : world.objects()) {
        if (slot.object != nullptr && slot.state.flags.is_unit && !slot.state.flags.ai_registered) {
          pending.push_back(slot.id);
        }
      }
      for (const ObjectId id : pending) heroes->ai_register(world, id);
    }
  }
  return started;
}

// --------------------------------------------------------------------------
// host entry points
// --------------------------------------------------------------------------

namespace {

[[nodiscard]] AiSystem* host_ai(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  return world == nullptr ? nullptr : ai_system_of(*world);
}

[[nodiscard]] EconomySystem* host_economy(CallContext& ctx) noexcept {
  World* world = world_of(ctx);
  return world == nullptr ? nullptr : economy_of(*world);
}

/// Resolve argument 0 as a settlement, the way `sim/economy.cpp` does: a
/// `(kTypeSettlement, settlement object id)` handle, or an ordinary object
/// handle through the world's back-link.
[[nodiscard]] Settlement* receiver_settlement(CallContext& ctx) noexcept {
  EconomySystem* economy = host_economy(ctx);
  if (economy == nullptr || ctx.count() == 0) return nullptr;
  const Value& value = ctx.arg(0);
  if (!value.is_object()) return nullptr;
  const script::ObjectRef ref = value.as_object();
  if (ref.type == kTypeSettlement) return economy->settlements().for_object(ref.id);
  if (ref.type != kTypeObj) return nullptr;
  World* world = world_of(ctx);
  if (world != nullptr) {
    const WorldObject* slot = world->find(ref.id);
    if (slot != nullptr && slot->settlement != kNoObject) {
      if (Settlement* s = economy->settlements().for_object(slot->settlement); s != nullptr) {
        return s;
      }
    }
  }
  return economy->settlements().for_object(ref.id);
}

/// Run a `.vs` script to completion, right now, and hand back its `return`.
///
/// **This is a capability, not a convenience.** Ten of the 65 entry points
/// `AI.INI`'s `[Scripts]` table declares have a non-`void` return type --
/// `GetEconomyScript.vs = int, Settlement set, int idPlayer` is one -- and the
/// engine calls them synchronously and uses the value. `Settlement::GetEconomyScript`
/// (0x004260f0) is three instructions of argument shuffling around
/// 0x0043d740, which pushes the literal string `GetEconomyScript.vs`, runs it,
/// and returns the `int` it produced. `AIRun` cannot express that: it spawns a
/// peer and yields an id.
///
/// Nothing about it is reentrant-unsafe here: `Scheduler::chunks_` only grows
/// through `add_chunk`, which no script can reach, so the chunk reference held
/// across the nested `run` cannot dangle even if the callee spawns.
///
/// Two things it refuses rather than guesses at:
///
///   * a script that **suspends**. `Sleep` inside a synchronous call has no
///     meaning -- the caller is a host function halfway through a stack frame
///     -- and quietly dropping the suspended `Execution` would leave whatever
///     the script did before the `Sleep` half-applied. Neither
///     `GetEconomyScript.vs` nor `GetTacticScript.vs` contains a `Sleep`.
///   * a script that **traps**, whose trap is reported as this call's failure
///     so that the name in the report is the name of the file that failed.
///
/// ## A third refusal used to be here, and it was over-broad
///
/// This function used to refuse any callee that **declared a pooled type**, on
/// the grounds that `Host::default_value` keys `ObjList`, `SquadList` and
/// `Query` values by `(script id, slot)` and the synthetic id is per nesting
/// depth, so two callees at the same depth would share a pool entry. That was
/// true and it was still the wrong remedy, because it also refused four shipped
/// entry points -- `TSRecruitArmy`, `TSRecruitHero`, `TSTempleRecruit`,
/// `TSArenaRecruit`, 59 call sites between them -- whose helpers all open with
/// `ObjList ol2; ObjList oll;` and neither of which ever lets one escape.
///
/// What sharing a pool entry actually costs is worth stating exactly, because
/// it is *not* corruption. `ObjListPool::acquire` clears the entry it finds, so
/// a second callee reaching the same `(id, slot)` gets an empty list -- which
/// is what a fresh local declaration means anyway. The only real hazard is a
/// handle **escaping** the callee into something that outlives the call, and
/// then being cleared under the holder's feet by the next call at that depth.
///
/// So the fix is the one a real script already gets: **release the synthetic
/// id's pooled state when the call ends.** `Scheduler` runs its teardown hook
/// for every script that dies, and that hook releases all four pools; this call
/// bypasses the scheduler's lifecycle entirely, so nothing was ever running it
/// here and every synchronous callee's lists, arrays, squad lists and
/// conversations were immortal. Running it makes each synchronous call start
/// from a clean pool and end leaving nothing behind, which removes the sharing
/// hazard and fixes the leak in the same line. An escaped handle is then dead
/// rather than aliased -- `contains()` answers false and the list reads empty,
/// which is degradation and not corruption, and is precisely what happens to a
/// handle that outlives the ordinary script that minted it.
///
/// The synthetic id is `kSyntheticScript + depth`, outside anything
/// `Scheduler` issues (ids start at 1 and only increase). **It has no
/// context**: 0x0069dca0 runs the callee on a thread it has just made, and
/// nothing sets that thread's `+0x44`, so `AIGetPlayer` inside a synchronous
/// callee answers 0 and the free `AIRun` refuses, whoever called it. No
/// shipped callee calls either.
constexpr ScriptId kSyntheticScript = 0x40000000u;
constexpr int kMaxSyncDepth = 8;
int g_sync_depth = 0;

}  // namespace

/// `out_locals`, when given, receives the callee's final local slots. That is
/// how a `*`-marked out-parameter comes back: `TSH_HeroRecruit.vs` is declared
/// `void, Settlement set, Hero *h` and assigns to `h`, and a frame that has
/// finished is not popped, so slot 1 holds what the script put there. Only
/// `TSRecruitHero` needs it and it is a pointer rather than a second overload
/// so that the one caller that does is the only one that pays for the copy.
///
/// **A callee that fails traps the caller here, and that is a known gap for
/// the callers still on it.** The original's synchronous launcher (0x006a0360)
/// reports a failed run as a non-zero status, and every trampoline read in the
/// tenth round's audit (0x0043d740, 0x0043d820, the four recruiters,
/// 0x00426c00) carries on with what it had -- 0, the list it minted, whatever
/// the by-reference slot holds -- after at most a message through the retail
/// no-op sink; 0x00426680 counts the skill as weight 0, which
/// `m_ai_get_skill_to_develop` does too. `run_script_or_report` is that
/// reading: the failure goes to the pass's report, where the trap sweep still
/// sees it, and the caller answers its default. The four recruiters,
/// `GetEconomyScript`, `GetTacticScript`, `GAIKA::GetStrat`,
/// `GAIKA::CalcPriority` and `Hero::TSAdvHeroSkills` use it; the rest still
/// trap, which keeps a broken callee loud. A missing script
/// file is not a failure of that kind, and each caller answers its own
/// default for it.
///
/// **Exported, not local.** `sim/squad.cpp` runs `CheckMAIKA.vs` from
/// `Squad::CalcGoAround` through the same trampoline shape (0x0043d670 beside
/// 0x0043d740), and one runner is one set of refusals.
namespace {

/// One synchronous run, as both readings of its failure need it.
struct SyncRun {
  script::ExecStatus status = script::ExecStatus::failed;
  script::Trap trap;
  ScriptId id = script::kNoScript;
};

[[nodiscard]] SyncRun run_now(CallContext& ctx, std::uint32_t chunk_index,
                              std::span<const Value> args, Value& result,
                              std::vector<Value>* out_locals) {
  SyncRun out;
  const script::Chunk& chunk = ctx.scheduler->chunk(chunk_index);
  const ScriptId synthetic = kSyntheticScript + static_cast<ScriptId>(g_sync_depth);
  out.id = synthetic;

  script::VmEnv env;
  env.registry = ctx.scheduler->registry();
  env.host = ctx.host;
  env.scheduler = ctx.scheduler;
  env.user = ctx.user;
  env.script = synthetic;
  // No context: see `kSyntheticScript`.
  env.now = ctx.scheduler->now();
  env.call_trace = env.scheduler != nullptr ? env.scheduler->call_trace() : nullptr;
  env.trace_user = env.scheduler != nullptr ? env.scheduler->trace_user() : nullptr;

  script::Execution execution = script::start(chunk, args, ctx.host);
  execution.chunk_index = chunk_index;
  ++g_sync_depth;
  out.status = script::run(execution, chunk, env);
  --g_sync_depth;
  // And the pooled state the callee declared. See the header: this is the
  // scheduler's own teardown, run by hand because a synchronous call never
  // enters the scheduler's lifecycle and so never leaves it either.
  if (const script::Scheduler::TeardownHook hook = ctx.scheduler->teardown_hook();
      hook != nullptr) {
    hook(ctx.user, synthetic);
  }
  // The frame is not popped whatever ended the run, so a by-reference slot
  // holds whatever the callee had put there by then -- which is what the
  // original's caller reads back after a failed run too.
  if (out_locals != nullptr && !execution.frames.empty()) {
    *out_locals = execution.frames.front().locals;
  }
  if (out.status == script::ExecStatus::finished) {
    result = execution.result;
  } else {
    out.trap = execution.trap;
    if (out.status == script::ExecStatus::suspended) {
      out.trap.detail = "the script suspended inside a synchronous call";
      out.trap.source_name = chunk.source_name;
    }
    if (out.trap.source_name.empty()) out.trap.source_name = chunk.source_name;
  }
  return out;
}

}  // namespace

HostOutcome run_script_now(CallContext& ctx, std::uint32_t chunk_index,
                           std::span<const Value> args, const char* what, Value& result,
                           std::vector<Value>* out_locals) {
  if (ctx.scheduler == nullptr) return HostOutcome::failed("no scheduler");
  if (chunk_index >= ctx.scheduler->chunk_count()) return HostOutcome::failed(what);
  if (g_sync_depth >= kMaxSyncDepth) {
    return HostOutcome::failed("synchronous AI script nesting too deep");
  }
  const SyncRun ran = run_now(ctx, chunk_index, args, result, out_locals);
  if (ran.status != script::ExecStatus::finished) {
    // Name the callee's own failure, not just the caller's. `HostOutcome::error`
    // is a static string, so the composed text lives in a small ring of
    // statics: the VM copies it into its trap before the next call can reuse
    // the slot, and a nested synchronous call composes its own on the way out
    // after the inner one has been consumed. "CalcPriority failed" on its own
    // sent the reader to the wrong file; "CalcPriority failed: unknown global
    // 'X' (DATA/AI/CalcGAIKAPriority.vs:12)" names the line.
    static std::string ring[4];
    static std::size_t next = 0;
    std::string& text = ring[next++ % 4];
    text = what;
    if (!ran.trap.detail.empty() || !ran.trap.source_name.empty()) {
      text += ": " + ran.trap.detail + " (" + ran.trap.source_name + ":" +
              std::to_string(ran.trap.line) + ")";
    }
    return HostOutcome::failed(text.c_str());
  }
  return HostOutcome::ok_void();
}

bool run_script_or_report(CallContext& ctx, std::uint32_t chunk_index,
                          std::span<const Value> args, Value& result,
                          std::vector<Value>* out_locals) {
  // `NOTFOUND`: 0x006a0360 answers its own non-zero status for a name the
  // library does not hold, and the caller prints and carries on.
  if (ctx.scheduler == nullptr || chunk_index >= ctx.scheduler->chunk_count()) return false;
  if (g_sync_depth >= kMaxSyncDepth) {
    ctx.scheduler->report_trap(script::FailedScript{
        kSyntheticScript + static_cast<ScriptId>(g_sync_depth),
        ctx.scheduler->chunk(chunk_index).source_name,
        script::Trap{script::TrapCode::host_error, "synchronous AI script nesting too deep",
                     ctx.scheduler->chunk(chunk_index).source_name, 0}});
    return false;
  }
  const SyncRun ran = run_now(ctx, chunk_index, args, result, out_locals);
  if (ran.status == script::ExecStatus::finished) return true;
  // Reported with the pass, as the member `AIRun`'s callee is, so the trap
  // sweep still sees a callee that broke; the caller goes on as the
  // original's does.
  ctx.scheduler->report_trap(script::FailedScript{ran.id, ran.trap.source_name, ran.trap});
  return false;
}

std::uint32_t ai_script_chunk(CallContext& ctx, PlayerId player, std::string_view file) {
  if (ctx.scheduler == nullptr) return script::kNoChunk;
  AiSystem* ai = host_ai(ctx);
  if (ai == nullptr || player == kNoPlayer) return ctx.scheduler->find_chunk(file);
  return ai->find_script(*ctx.scheduler, player, file);
}

namespace {

/// `AIGetPlayer` (0x00421cb0) -- the 1-based player whose AI owns the running
/// script, and **0** for a script no AI owns.
///
/// The body is one call and an increment: 0x0043caa0 reads the running
/// script's owner tag (the text field at +0x44 of the current script record),
/// answers -1 when the field is null and its `atol` otherwise, and the entry
/// point pushes that **plus one**. The tag is written by the AI's own spawn
/// (0x0041cd00 formats `[ai+0x24]`, the 0-based seat, with `%d` and stores it
/// through 0x0069d040), so an AI-owned script reads its seat + 1 and anything
/// else reads -1 + 1 = 0 -- and a sequence, whose context is `/`, reads
/// `atol("/") + 1 = 1`, which no shipped sequence asks. No argument, no
/// receiver, no failure arm. The tag is the calling thread's own
/// (`CallContext::context`); all nine shipped sites run inside an AI.
HostOutcome f_ai_get_player(CallContext& ctx) {
  return HostOutcome::ok_with(Value::integer(AiSystem::thread_seat(ctx.context) + 1));
}

/// The handle every `AIRun` failure answers: 0x00422f90, 0x00424a20 and
/// 0x00424c30 all start from -1 and keep it when 0x0041cd00 refuses.
constexpr std::int32_t kNoAiScript = -1;

/// `AIRun(name)` and `AIRun(name, set, ol, obj, n)` -- 72 sites between the
/// spellings, and **a thread, not a call**.
///
/// 0x00422f90 (name only) and 0x00424a20 (name and four parameters) take the
/// calling thread's AI seat from its context (0x0043caa0: `atol`, -1 for
/// none), refuse a seat outside 0..15 or one with no AI record (`+0x88`) or an
/// empty name, and hand the script to that AI's launcher 0x0041cd00. That
/// resolves the file on the seat's AI search path (0x0043d030 -- the
/// profile's own directory first, then the plain name; `find_script`), checks
/// the library holds it (0x006a0270), takes the **first free thread id from 3
/// to 51** in the AI's own thread manager (0x0069f950 per id; -1 when all 49
/// are taken), starts it with no delay and a default interval of 100
/// (0x006a03d0 -> 0x0069fd80), tags it with the seat, and answers **the id**
/// (`AiSystem::run`). The four-parameter form binds the settlement and object
/// as handles and the list as the caller's own list object, against the
/// signature `void, Settlement set, ObjList ol, Obj obj, int nParam`. Every
/// failure prints "Cannot run '%s' script" and answers -1. 0x00424c30 is the
/// same with the player passed explicitly first; no shipped script calls it,
/// and it is not bound.
///
/// This used to answer the scheduler's own monotonic id, spawn whatever the
/// caller, and resolve the name through the library's leaf fallback rather
/// than the seat's directory -- so a `DEFENSIVE` AI's `Main.vs` ran the root
/// profile's monitors.
HostOutcome f_ai_run(CallContext& ctx) {
  AiSystem* ai = host_ai(ctx);
  if (ai == nullptr) return HostOutcome::failed("AIRun: no world");
  if (ctx.scheduler == nullptr) return HostOutcome::failed("AIRun needs a scheduler");
  if (ctx.count() < 1 || !ctx.arg(0).is_string()) {
    return HostOutcome::failed("AIRun expects a script name");
  }
  const std::int32_t seat = AiSystem::thread_seat(ctx.context);
  const std::string_view name = ctx.arg(0).as_string();
  const Value refused = Value::integer(kNoAiScript);
  if (seat < 0 || seat >= static_cast<std::int32_t>(kPlayerCount) || name.empty()) {
    return HostOutcome::ok_with(refused);
  }
  const auto player = static_cast<PlayerId>(seat);
  const AiPlayer* owner = ai->player_ai(player);
  if (owner == nullptr || !owner->active) return HostOutcome::ok_with(refused);
  const std::uint32_t chunk = ai->find_script(*ctx.scheduler, player, name);
  if (chunk == script::kNoChunk) return HostOutcome::ok_with(refused);
  const std::int32_t handle =
      ai->run(player, chunk, ctx.arguments.subspan(1), *ctx.scheduler, ctx.script);
  return HostOutcome::ok_with(Value::integer(handle));
}

/// `AIBreakScript(handle)` -- 74 sites. 0x00421cd0 takes the calling thread's
/// AI seat (0x0043caa0), and when that seat has an AI record calls 0x0041cdc0
/// on it, which acts only on a handle from 3 to 51: if the AI's thread
/// manager holds that id (0x0069f950) it is stopped (0x0069fee0) -- at once,
/// or, when it is the thread running right now, by flagging it (`[vm+0x40]`)
/// so the interpreter stops after the current call. Anything else is silently
/// nothing; so is a caller with no seat. 0x00421d20 is the same with the
/// player passed first; no shipped script calls it. Because the handle is an
/// id and not a thread, a handle whose thread has ended and whose id a later
/// `AIRun` of the same seat has taken stops the newer thread, as there.
HostOutcome f_ai_break_script(CallContext& ctx) {
  AiSystem* ai = host_ai(ctx);
  if (ai == nullptr) return HostOutcome::failed("AIBreakScript: no world");
  if (ctx.scheduler == nullptr) return HostOutcome::failed("AIBreakScript needs a scheduler");
  if (ctx.count() != 1 || !ctx.arg(0).is_integer()) {
    return HostOutcome::failed("AIBreakScript expects a script handle");
  }
  const std::int32_t seat = AiSystem::thread_seat(ctx.context);
  if (seat < 0 || seat >= static_cast<std::int32_t>(kPlayerCount)) return HostOutcome::ok_void();
  const auto player = static_cast<PlayerId>(seat);
  const AiPlayer* owner = ai->player_ai(player);
  if (owner == nullptr || !owner->active) return HostOutcome::ok_void();
  (void)ai->break_run(player, ctx.arg(0).as_integer(), *ctx.scheduler);
  return HostOutcome::ok_void();
}

/// `AIStart(player, profile, difficulty)` -- 20 sites, every one of them inside
/// a map container, and the sole blocker of ten scripts.
///
/// **The claim this replaces was measured over the wrong corpus.** This
/// header said "zero call sites across all 577 `.vs` files, which is why
/// `script/host_surface.cpp` -- an inventory derived from call sites -- does
/// not declare it", and both halves were true of `data.pak` and false of the
/// installation. `4_Great_Battles_Egypt` map 4 hands player 4 to the AI when
/// its ambush fires; `5_Great_Battles_Britain` map 3 does it twice. The
/// inventory covers the containers now, so the name is declared and this
/// attaches behaviour to it like every other domain.
///
/// The bootstrap itself is `AiSystem::start`, which has been written, hashed
/// and tested since the AI domain landed and was reachable only from C++.
/// Nothing here is new but the argument handling:
///
///   * the player is validated **1..16** and passed down 0-based, which is
///     `player_arg`'s rule everywhere else. `Function "AIStart": Player number
///     should be between 1 and 16` is the original's own message.
///   * the profile name must match a listed profile **exactly, case and
///     all**. The wrapper does copy the argument into a stack buffer and fold
///     `a`..`z` there (0x00435030-0x0043505c), but the key it then pushes to
///     the lookup (0x0043506d: the address of the slot holding the *original*
///     string) is not that buffer, so the fold is dead. The lookup, 0x00426ea0,
///     walks the profile list comparing length and bytes with no folding, and
///     the list holds uppercased directory names (0x00443190). So
///     `"DEFENSIVE"` would match and `"Defensive"` is refused with `AIStart:
///     invalid AI profile`, starting nothing. The core (0x00434ca0) uppercases
///     the stored copy afterwards, a no-op on a name that matched. Here that is
///     a refusal of any argument with a lower-case letter in it, ahead of
///     `AiSystem::start`'s folded lookup. The two-argument form (0x00434e60)
///     has the same dead fold and the same exact lookup.
///   * the difficulty argument is 0, 1 or 2 -- the core increments before
///     validating against 1..3 -- and anything else refuses with
///     `AIStart: invalid diffivulty`, the typo included. `ai_start_difficulty`
///     already carries that mapping.
///
/// Every shipped site passes `"DEFAULT"`, and eleven of the twenty pass
/// `GetDifficulty()` rather than a literal.
///
/// **The empty profile is refused here, and only here.** The wrapper looks
/// the uppercased name up in the profile list (0x00435077, a plain equality
/// search), and that list is filled by walking `data/ai/`'s subdirectories
/// (0x004433b0 -> 0x00443190): each directory adds its uppercased name, and
/// the root is loaded with no name at all, so no entry is ever empty. This
/// engine's `AiSystem` keys the root under `""` because the core path needs
/// it -- a map's `playerdata/@AI` and a dropped seat's takeover reach
/// 0x00434ca0 without the list check, as 0x00434de0 does with `"Default"` --
/// so the script's entry point has to say no to `""` itself.
///
/// **A seat that already has an AI gets a new one.** The core destroys the
/// old object before building the new (0x00434cd0-0x00434cdd), and that is
/// `AiSystem::start`'s restart. The three checks come first in the wrapper,
/// so a refused call leaves the old AI running.
///
/// **What the shipped `"DEFAULT"` meets, inferred.** No `DATA\AI\DEFAULT\`
/// directory ships -- `data.pak`'s only profile directory is `DEFENSIVE` --
/// so the twenty shipped calls look to be refused in the retail game too;
/// the walk's listing function (0x0063a4a0) was not followed to the packs
/// to prove it. This engine refuses them alike, and does not yet register
/// `DEFENSIVE`, which the original's list would hold (`GameSession`
/// registers only the root).
///
/// **A refusal is not a trap.** All three of the original's refusals print
/// through the sink that is a bare `ret` in retail and return normally, so a
/// mission that names a profile the installation does not have keeps running
/// with one fewer AI -- which is what it does in the retail game too.
HostOutcome f_ai_start(CallContext& ctx) {
  World* world = world_of(ctx);
  AiSystem* ai = host_ai(ctx);
  if (world == nullptr || ai == nullptr) return HostOutcome::failed("AIStart: no world");
  if (ctx.scheduler == nullptr) return HostOutcome::failed("AIStart: no scheduler");
  if (!ctx.arg(0).is_integer() || !ctx.arg(1).is_string() || !ctx.arg(2).is_integer()) {
    return HostOutcome::ok_void();
  }
  const PlayerId player = player_from_script(ctx.arg(0).as_integer());
  if (player == kNoPlayer) return HostOutcome::ok_void();
  if (ctx.arg(1).as_string().empty()) return HostOutcome::ok_void();  // `invalid AI profile`
  // An exact match against uppercased names: a lower-case letter matches none.
  if (upper(ctx.arg(1).as_string()) != ctx.arg(1).as_string()) return HostOutcome::ok_void();
  const AiDifficulty difficulty = ai_start_difficulty(ctx.arg(2).as_integer());
  if (difficulty == AiDifficulty::none) return HostOutcome::ok_void();
  // `AiSystem::add_profile` uppercases on the way in and `start` looks up by
  // the uppercased name; the check above has made that fold the identity.
  (void)ai_start(*world, player, ctx.arg(1).as_string(), difficulty, *ctx.scheduler);
  return HostOutcome::ok_void();
}

/// The pathfinder bootstrap: `SPFFindAreas_Quant`, `SPFInitData`,
/// `SPFInitDirectionData`, `SPFInitPointToAreaData`, `SPFInitConnectData`,
/// `SPFCalcConnectHighRes_Quant`, `SPFIncreaseConnect`,
/// `SPFIncreaseConnect_Quant`, `SPFDone` -- nine names, one site each, all of
/// them `DATA\PATHFINDERINIT.VS`, which is the whole of their corpus:
///
///     while (SPFFindAreas_Quant()) Sleep(30);
///     SPFInitData(); Sleep(250); ... SPFInitConnectData(); Sleep(250);
///     while (SPFCalcConnectHighRes_Quant()) Sleep(23);
///     SPFIncreaseConnect(); Sleep(250);
///     while (SPFIncreaseConnect_Quant()) Sleep(35);
///     SPFDone();
///
/// The nine are the driver interface of the original's *smart pathfinder*, the
/// movement structure at 0x008c528c. Each is a few instructions into a
/// builder at 0x008c529c, read without a null test:
///
///   * the three `_Quant` calls do one slice of a pass and answer **whether
///     more remains**: 0x004129a0 answers 0x00411af0's result as a boolean;
///     0x00415a40 runs one row of the connect pass (0x00415880 over the row
///     counter at `[builder+0x18]`), advances the counter and answers whether
///     it is still within the last row, `[builder+0x14]`; 0x00415a90 answers
///     0x004159b0's result as a boolean;
///   * the five steps run one stage whole and answer nothing: 0x004166b0 calls
///     0x00416520; 0x0040dd10 calls 0x0040db30 and 0x00412980 calls 0x00411c00,
///     each with the map's size and data from `[builder+4]`; 0x0040d1d0 only
///     sets the connect pass's range -- 2 and 2 at `+8`/`+0xc`, the extent at
///     `[[builder+4]+0x20]` less 3 at `+0x10`/`+0x14`, and the counter `+0x18`
///     to the first;
///     0x004161d0 calls 0x00415e30 and zeroes `[builder+0x2c]`;
///   * `SPFDone` (0x0040f8c0) destroys the live structure through its virtual
///     destructor, installs the built one (`[builder]`), tears the builder down
///     (0x0040f7f0) and frees it.
///
/// It is the builder that starts the script: 0x004165a0 does nothing while a
/// builder exists or the script is already running (0x006a2110), otherwise
/// allocates the builder and starts `data/pathfinderinit.vs` attached to the
/// world at priority 100 (0x006a07e0), freeing the builder again if that
/// fails. Its two callers (0x0040acb0, which also creates the live structure,
/// and 0x006a4140) are the original's moments to build; the script exists to
/// spread the build over frames, `Sleep` by `Sleep`.
///
/// **Here** each command is `MovementSystem::smart_build`, which is
/// `PathFinder::smart_build` on the builder `GameSession::start_match` makes
/// when it starts the script (`sim/smart_path.hpp`). The stages are the
/// original's; what this engine does differently is in
/// docs/formats/pathfinder.md, "What differs". With no builder -- a world
/// whose session never started the build -- each answers false or nothing
/// where the original would read a null builder.
HostOutcome spf_call(CallContext& ctx, SpfCall call, bool answers) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SPF: no world");
  MovementSystem* movement = movement_system(*world);
  const bool more = movement != nullptr && movement->smart_build(call);
  return answers ? HostOutcome::ok_with(Value::boolean(more)) : HostOutcome::ok_void();
}

HostOutcome f_spf_find_areas(CallContext& ctx) { return spf_call(ctx, SpfCall::find_areas, true); }
HostOutcome f_spf_init_data(CallContext& ctx) { return spf_call(ctx, SpfCall::init_data, false); }
HostOutcome f_spf_init_direction(CallContext& ctx) {
  return spf_call(ctx, SpfCall::init_direction, false);
}
HostOutcome f_spf_init_pta(CallContext& ctx) {
  return spf_call(ctx, SpfCall::init_point_to_area, false);
}
HostOutcome f_spf_init_connect(CallContext& ctx) {
  return spf_call(ctx, SpfCall::init_connect, false);
}
HostOutcome f_spf_connect(CallContext& ctx) { return spf_call(ctx, SpfCall::connect_row, true); }
HostOutcome f_spf_increase(CallContext& ctx) { return spf_call(ctx, SpfCall::increase, false); }
HostOutcome f_spf_increase_step(CallContext& ctx) {
  return spf_call(ctx, SpfCall::increase_step, true);
}
HostOutcome f_spf_done(CallContext& ctx) { return spf_call(ctx, SpfCall::done, false); }

/// `AIStop(player)` -- 1 site, `3_Great_Losses_Egypt.bfhp`'s `Maps/1/Sequences/
/// seq2.vs`, and one of the seven messages the trap sweep still prints.
///
/// 0x00422720 checks the player against 1..16 and prints `Function "AIStop":
/// Player number should be between 1 and 16` when it is not -- and then
/// **carries on** with the bad index into the player table, which is a read
/// off the end of it; here an out-of-range player is refused quietly, the
/// one reading that is not a fault. Otherwise it takes the player's AI
/// pointer at `[player+0x1034]`, calls its virtual destructor with the
/// delete flag, and nulls the slot; a player with no AI is a no-op. That is
/// `AiSystem::stop`: every coroutine the player owns is killed, `Main.vs`
/// first, and the record is put back to what `AIStart` found. A call with no
/// scheduler has nothing to kill and refuses, as `AIStart` does.
HostOutcome f_ai_stop(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("AIStop: no world");
  if (ctx.scheduler == nullptr) return HostOutcome::failed("AIStop: no scheduler");
  if (!ctx.arg(0).is_integer()) return HostOutcome::ok_void();
  const PlayerId player = player_from_script(ctx.arg(0).as_integer());
  // `stop` refuses an out-of-range player on its own, so this guard is an
  // equivalence the sweep labels; it is here because the refusal is the
  // reading, where the original reads off the end of the table.
  if (player == kNoPlayer) return HostOutcome::ok_void();
  (void)ai_stop(*world, player, *ctx.scheduler);
  return HostOutcome::ok_void();
}

/// `obj.AI` -- 25 call sites, and **36,957 of the 38,303 trap hits** a 25-map
/// sweep of the installation records, because `DATA\SUBAI\UNIT_IDLE.VS` is the
/// sub-AI every unit runs while idle and it tests this on every pass. The two
/// figures rank different things and both are worth saying: it barely moves
/// coverage and it collapses the trap tally.
///
/// **It is two questions, not one.** 0x004254b0 reads, in order:
///
///   1. `[obj+0x70]` -- the object's player record. Null gives **false**.
///      `Obj::player` (0x005ab2d3) is `[[obj+0x70]+0x8] + 1`, which is what
///      identifies the field.
///   2. `[player+0x88]` -- that player's AI. Null gives **false**. A human
///      player has none, and this is the half the name is really about.
///   3. otherwise `[obj+0x174]` unpacks to `(index << 4) | player` -- a
///      `SquadKey`, four bits of player and twelve of index, the same pair
///      `sim/squad.hpp` carries -- the squad is looked up, and the answer is
///      the **negation** of bit 0 of its flags word, `SF_NOAI`.
///
/// So `AI` is "this unit is run by the computer and has not been pinned", which
/// is exactly how the corpus guards with it: `if (.AI)` before AI-only
/// behaviour, `if (!.AI)` before the player-only kind.
///
/// An object in no squad reads as flags 0 and so is not pinned; step 2 still
/// decides it. That matches the original, which does not special-case squad
/// index 0 -- it indexes the vector at 0 and reads whatever the reserved slot
/// holds, and the reserved slot's flags are zero.
///
/// The squad half needs the hero system, which owns `SquadTable`; with no hero
/// system registered there are no squads at all, and no squad means not pinned.
HostOutcome m_ai(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("AI: no world");
  if (ctx.count() == 0 || !ctx.arg(0).is_object()) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const WorldObject* slot = world->find(ctx.arg(0).as_object().id);
  // The `kNoPlayer` half is an early-out, not a guarantee: `player_ai` bounds-
  // checks and `kNoPlayer` is 255 against a table of 16, so injecting a fault
  // into this alone changes no test. The property -- an unowned object is not
  // AI-controlled -- is asserted directly by the test rather than through
  // either guard.
  if (slot == nullptr || slot->state.owner == kNoPlayer) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  AiSystem* ai = ai_system_of(*world);
  // `active`, **not** `player_ai(...) != nullptr`. That was the obvious
  // translation of the original's `[player+0x88] != 0` and it is wrong here:
  // `AiSystem::player_ai` indexes a dense array and hands back a pointer for
  // every player in range, running or not, so the null test is true of a human
  // player and `AI` would answer true for every unit on the map. `AiPlayer` is
  // the slot; `AiPlayer::active` is the AI.
  const AiPlayer* running = ai == nullptr ? nullptr : ai->player_ai(slot->state.owner);
  if (running == nullptr || !running->active) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  std::uint16_t flags = 0;
  if (HeroSystem* heroes = hero_system_of(*world); heroes != nullptr) {
    const SquadKey key = heroes->squad_of(slot->id);
    if (const Squad* squad = heroes->squads().find(key); squad != nullptr) flags = squad->flags;
  }
  return HostOutcome::ok_with(Value::boolean((flags & kSquadFlagNoAi) == 0));
}

/// The economy or tactic slot of `set` **as its current owner's AI sees it**:
/// what `set.EconomyScript` (0x00426190) and `set.TacticScript` (0x00426290)
/// answer, and what `RunEconomyScript` tests before it stops anything.
///
/// The two entry points resolve the handle and take the settlement's owner
/// record (`[set+0x90]`) and that player's AI (`[player+0x88]`); **no AI
/// answers 0** -- a human's settlement has no economy script whatever ran on
/// it before. With an AI they call 0x0041d2a0 / 0x0041d410 on it, which read
/// the AI's own per-settlement array (`[ai+0x70]` for economy, `[ai+0x80]` for
/// tactic, a 16-bit id at the settlement's index `[set+0x1c]`), first asking
/// the AI's thread manager whether the settlement's fixed thread id (economy
/// `base + 0x34 + 2*index`, tactic one more; `base` is the helper-table count)
/// still runs (0x0069f950) and zeroing the stored id when it does not. The
/// answer is the stored id masked to 16 bits.
///
/// **Both the array and the threads are the AI's**, so the slot is per
/// (AI, settlement), and so is it here (`AiPlayer::settlements`): a live
/// script another seat started -- the settlement has changed hands and the
/// old owner's economy script has not yet noticed -- is in the old owner's
/// array and not the new owner's, and a seat that takes a settlement back
/// finds its own entry as it left it. The receiver failing to resolve is a
/// null dereference in the original; here it traps.
[[nodiscard]] std::int32_t owner_slot(AiSystem& ai, const Settlement& set, AiEnum family,
                                      const script::Scheduler& scheduler,
                                      script::ScriptId* running = nullptr) {
  const AiPlayer* seat = ai.player_ai(set.owner);
  if (seat == nullptr || !seat->active) return 0;
  const bool economy = family == AiEnum::economy_script;
  const std::int32_t id = economy ? ai.economy_script(set.owner, set.id, scheduler)
                                  : ai.tactic_script(set.owner, set.id, scheduler);
  if (id == 0) return 0;
  if (running != nullptr) {
    *running = economy ? ai.economy_thread(set.owner, set.id, scheduler)
                       : ai.tactic_thread(set.owner, set.id, scheduler);
  }
  return id & 0xffff;
}

/// `set.EconomyScript` / `set.TacticScript` -- `owner_slot`, which describes
/// both bodies.
HostOutcome m_economy_script(CallContext& ctx) {
  AiSystem* ai = host_ai(ctx);
  if (ai == nullptr) return HostOutcome::failed("EconomyScript: no world");
  if (ctx.scheduler == nullptr) return HostOutcome::failed("EconomyScript: no scheduler");
  const Settlement* set = receiver_settlement(ctx);
  if (set == nullptr) return HostOutcome::failed("EconomyScript: receiver is not a settlement");
  return HostOutcome::ok_with(
      Value::integer(owner_slot(*ai, *set, AiEnum::economy_script, *ctx.scheduler)));
}

HostOutcome m_tactic_script(CallContext& ctx) {
  AiSystem* ai = host_ai(ctx);
  if (ai == nullptr) return HostOutcome::failed("TacticScript: no world");
  if (ctx.scheduler == nullptr) return HostOutcome::failed("TacticScript: no scheduler");
  const Settlement* set = receiver_settlement(ctx);
  if (set == nullptr) return HostOutcome::failed("TacticScript: receiver is not a settlement");
  return HostOutcome::ok_with(
      Value::integer(owner_slot(*ai, *set, AiEnum::tactic_script, *ctx.scheduler)));
}

/// `set.GetEconomyScript(idPlayer)` / `set.GetTacticScript(idPlayer)`
/// (0x004260f0 / 0x004261f0).
///
/// Each pops the player and the settlement and calls 0x0043d740 / 0x0043d820
/// with **`idPlayer - 1`**: the seat whose script directory is searched is the
/// argument's, not the running script's. That resolves the file through
/// 0x0043d030 -- `<the seat's AI profile name>/<file>` when the seat (0..15)
/// has an AI with a non-empty profile name and that file exists, the plain
/// name otherwise, which is `ai_script_chunk` -- and runs it synchronously
/// (0x006a0360) with the declared `int, Settlement set, int idPlayer`: the
/// settlement's id and `idPlayer` again, so the arguments pass straight
/// through. The answer is the script's return value when the run succeeded
/// **and the value is not negative**, masked to 16 bits; a negative value, a
/// script that returns nothing, a missing file or a failed run all answer 0.
/// No receiver check: an invalid handle is dereferenced. A failed run -- a
/// callee that traps or suspends, or a file the library does not hold -- is
/// 0x006a0360's non-zero status, and the trampoline answers 0 for it
/// (0x0043d7cb, 0x0043d8ab): here the failure goes to the pass's report
/// (`run_script_or_report`) and the caller goes on with 0.
HostOutcome get_script_common(CallContext& ctx, const char* file, const char* what) {
  AiSystem* ai = host_ai(ctx);
  if (ai == nullptr) return HostOutcome::failed(what);
  if (ctx.scheduler == nullptr) return HostOutcome::failed(what);
  if (ctx.count() != 2) return HostOutcome::failed(what);
  const std::int64_t seat = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  const PlayerId owner = seat >= 1 && seat <= 16 ? static_cast<PlayerId>(seat - 1) : kNoPlayer;
  const std::uint32_t chunk = ai_script_chunk(ctx, owner, file);
  if (chunk == script::kNoChunk) return HostOutcome::ok_with(Value::integer(0));
  Value result;
  if (!run_script_or_report(ctx, chunk, ctx.arguments, result)) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  if (!result.is_integer() || result.as_integer() < 0) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  return HostOutcome::ok_with(Value::integer(result.as_integer() & 0xffff));
}

HostOutcome m_get_economy_script(CallContext& ctx) {
  return get_script_common(ctx, "GetEconomyScript.vs", "GetEconomyScript failed");
}

HostOutcome m_get_tactic_script(CallContext& ctx) {
  return get_script_common(ctx, "GetTacticScript.vs", "GetTacticScript failed");
}

/// `set.RunEconomyScript(id)` / `set.RunTacticScript(id)` (0x00426140 /
/// 0x00426240).
///
/// Each pops the id and the settlement and calls 0x0041d310 / 0x0041d480 **on
/// the settlement owner's AI** (`this` is `[[set+0x90]+0x88]`, loaded at
/// 0x0042616b; no null test, so a settlement whose owner has no AI is a crash
/// in the original and nothing at all here). That routine, in order:
///
///   1. reads the owner's slot (`owner_slot`'s rule) and, when it is
///      non-zero, **stops the thread running in the settlement's fixed id**
///      (0x0069fee0) -- a second `RunEconomyScript` replaces the first script
///      rather than running beside it;
///   2. **writes the id, unconditionally** -- 0 included, which is how a
///      caller stops the slot's script and clears it;
///   3. for a non-zero id, takes its name from the constant table (0x0043f1b0;
///      an id past the table's end names `""`), appends `.vs`, and starts it in
///      the owner's thread manager at the settlement's fixed id through
///      0x0041cd00, with the signature `void, Settlement set` and the
///      settlement as its argument, tagged with the owner's seat (its
///      context) so `AIGetPlayer` inside it answers the owner. A name with no script file
///      makes 0x0041cd00 answer -1 and start nothing: no message, and the id
///      stays written, which the next read turns back into 0 because no
///      thread runs at that id.
///
/// The constant names here come from the profile family by family
/// (`AiProfile::constant_name`), which is this engine's numbering of the same
/// names (`sim/ai_profile.hpp` says why it differs from retail's single
/// table). The spawned script's parent is the caller, which nothing reads;
/// whose AI it is, is its context.
HostOutcome run_script_common(CallContext& ctx, AiEnum family, const char* what) {
  AiSystem* ai = host_ai(ctx);
  if (ai == nullptr) return HostOutcome::failed(what);
  if (ctx.scheduler == nullptr) return HostOutcome::failed(what);
  if (ctx.count() != 2 || !ctx.arg(1).is_integer()) return HostOutcome::failed(what);
  const Settlement* set = receiver_settlement(ctx);
  if (set == nullptr) return HostOutcome::failed(what);
  const PlayerId owner = set->owner;
  const AiPlayer* seat = ai->player_ai(owner);
  if (seat == nullptr || !seat->active) return HostOutcome::ok_void();

  // Step 1: the owner's own running script goes first.
  script::ScriptId running = script::kNoScript;
  if (owner_slot(*ai, *set, family, *ctx.scheduler, &running) != 0) {
    ctx.scheduler->kill(running);
  }
  // Step 2: the 16-bit id is written whatever it is.
  const std::int32_t id = static_cast<std::int32_t>(ctx.arg(1).as_integer() & 0xffff);
  const auto write = [&](ScriptId script) {
    if (family == AiEnum::economy_script) {
      ai->set_economy_script(owner, set->id, id, script);
    } else {
      ai->set_tactic_script(owner, set->id, id, script);
    }
  };
  write(script::kNoScript);
  if (id == 0) return HostOutcome::ok_void();

  // Step 3.
  const std::string file = ai->script_file_for(owner, family, id);
  if (file.empty()) return HostOutcome::ok_void();
  const std::uint32_t chunk = ai->find_script(*ctx.scheduler, owner, file);
  if (chunk == script::kNoChunk) return HostOutcome::ok_void();
  const Value argument = ctx.arg(0);
  const ScriptId spawned = ctx.scheduler->spawn(chunk, std::span<const Value>(&argument, 1),
                                                script::ObjectRef{}, ctx.script);
  if (spawned == script::kNoScript) return HostOutcome::ok_void();
  (void)ctx.scheduler->set_context(spawned, ai_thread_context(owner));
  write(spawned);
  return HostOutcome::ok_void();
}

/// `hero.TSAdvHeroSkills(aSkills, aSkillLevels)` -- **68 sites, the largest
/// unimplemented name in the census**, and none of the behaviour is native.
///
/// `Hero::TSAdvHeroSkills` (0x00426c00) is registered `[0, 3, 24, 268, 268]`:
/// void, a `Hero` receiver, then two `IntArray`s by reference. Its whole body
/// is the trampoline this file already runs twice -- resolve a `.vs` file on a
/// player's AI search path and run it synchronously against the arguments
/// already on the stack -- and it carries the three literals that prove it: the
/// file name `TSH_HeroSkills.vs`, the guard message `The function
/// 'TSHeroSkills' called for an uninitialized or invalid object.` (the internal
/// name, without the `Adv`), and `Error running script!`.
///
/// So the behaviour is `DATA\AI\TSH_HEROSKILLS.VS`, which this project
/// already reads and compiles, and which is a greedy spend against a priority
/// list: walk `aSkills` in order pairing each id with the target level at the
/// same index in `aSkillLevels`, pour points into the first skill still below
/// its target, then **start the walk again from the top** -- so a
/// high-priority skill is always topped up before a lower one gets anything.
/// It stops when the points run out or a whole pass finds nothing under target.
/// All 68 sites are the eleven `TS_*TACTIC*.VS` race tactic scripts calling it
/// at every phase boundary behind nothing but `if (hero.IsAlive)`, which is
/// only safe because a hero with no unspent points is a no-op -- and it is.
///
/// **The player is the hero's owner, not the caller's.** The original reads it
/// from the hero's own player pointer; `GetTacticScript` reads it from the
/// running script. The difference is invisible at every shipped site (a tactic
/// script only ever advances its own player's heroes) and it is still the
/// original's rule, so it is this one's.
///
/// The arguments pass straight through: the receiver and the two handles are
/// exactly the helper's three declared parameters, in order. The by-reference
/// bit costs nothing here because an `IntArray` value *is* a pool handle -- the
/// callee reads the caller's array through it -- and the helper never assigns
/// to either one.
///
/// **What had to be fixed first, and it was not this file.** `aSkills.size()`
/// is the helper's own guard on line 9. `size/0` answered only for `SquadList`
/// receivers, so on an `IntArray` it returned 0 and the helper would have
/// returned at line 9 every time: 68 call sites counted as cleared and not one
/// hero ever gaining a skill, which is strictly worse than the trap it
/// replaced. `sim/squad.cpp`'s `size_impl` now branches on all four receivers,
/// and this entry point should not be read without that one.
HostOutcome m_ts_adv_hero_skills(CallContext& ctx) {
  static constexpr const char* kWhat = "TSAdvHeroSkills failed";
  static constexpr std::string_view kFile = "TSH_HeroSkills.vs";
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kWhat);
  if (ctx.scheduler == nullptr) return HostOutcome::failed(kWhat);
  // **Belt and braces, and kept on `get_script_common`'s precedent.** The
  // registry keys on arity and the VM pushes exactly the declared count, so
  // no call that reaches here can have any other; a fault injected into this
  // line alone survives the whole suite. What it is protecting is the
  // pass-through below, which hands `ctx.arguments` to a callee whose
  // parameter list is fixed at three.
  if (ctx.count() != 3) return HostOutcome::failed(kWhat);
  // The original's own guard (0x00426c00): only the **hero** is tested -- the
  // two arrays are resolved and passed on as they are -- and a hero naming
  // nothing prints and returns without running the helper. A receiver that is
  // not an object handle at all is the same case, and the helper's `if
  // (!hero.IsValid)` would answer it identically if it ran. The search path
  // is the hero's player (`[[hero+0x70]+8]` into 0x0043d030). A failed run --
  // a callee that traps or suspends, or a file the library does not hold --
  // prints `TSHeroSkills: Error running script!` and returns (0x00426d0f):
  // reported with the pass here, and the caller goes on.
  if (!ctx.arg(0).is_object()) return HostOutcome::ok_void();

  // The handle resolves through the object table (`[0xa2a610 + index]`); one
  // that names no object is the invalid-object message, not a run.
  const WorldObject* slot = world->find(ctx.arg(0).as_object().id);
  if (slot == nullptr) return HostOutcome::ok_void();
  const PlayerId owner = slot->state.owner;
  AiSystem* ai = ai_system_of(*world);
  const std::uint32_t chunk = (ai == nullptr || owner == kNoPlayer)
                                  ? ctx.scheduler->find_chunk(kFile)
                                  : ai->find_script(*ctx.scheduler, owner, kFile);
  Value result;
  (void)run_script_or_report(ctx, chunk, ctx.arguments, result);
  return HostOutcome::ok_void();
}

/// `hero.AIGetSkillToDevelop(skill, level)` -- **one call site, and it is the
/// sole blocker of `HERO_SKILL_BEHAVIOUR.VS`**, the 74-site idle behaviour
/// every AI-controlled hero runs. The script keeps a skill it is "developing"
/// and asks this only when it has unspent points and no current pick; what
/// comes back through the two out-parameters is a skill id and the level to
/// take it to, or -1 in both when nothing is worth developing.
///
/// `Hero::AIGetSkillToDevelop` (0x00426680) is registered `[0, 3, 24, 257,
/// 257]`: void, a `Hero` receiver and two `int` by reference. Like
/// `TSAdvHeroSkills` beside it, nothing in the body decides anything about
/// skills; it is a **weighted draw over the answers of one script run per
/// skill**, and the script is the AI profile's.
///
/// ## What the body does, in order
///
/// Both out-parameters are set to -1 first, before the receiver is looked at,
/// and an invalid receiver prints `The function 'Hero::AIGetSkillToDevelop'
/// called for an uninitialized or invalid object.` and returns with them so.
///
/// Then one pass over the 25 skills in `SKILLS.INI` order -- the executable's
/// own name table at 0x00824b00 is the same 25 strings in the same order as
/// `hero_skill_name`, checked entry by entry. A skill is **skipped** when the
/// hero's byte for it (`[hero+0x1fc+i]`) is negative, which is how a skill the
/// hero's class does not offer is stored there, or is already 10 or more. For
/// each of the rest the body formats `HeroSkill %s.vs` with the skill's name,
/// resolves it on the **hero's owner's** AI search path (0x0043d030 with the
/// player read off the hero's own player pointer), and when no such file
/// exists resolves `HEROSKILL DEFAULT.VS` the same way. The shipped
/// `DATA\AI\HEROSKILL DEFAULT.VS` declares the contract:
///
///     // void, Hero hero, int skill, int *skill_points, int *weight
///
/// `skill_points` arrives holding the hero's current points and is where the
/// script leaves its goal; `weight` arrives as **100** and is what the script
/// scales -- its own comment says `weight *= 5`, `weight /= 2`, or `weight = 0`
/// to hold a skill back. The shipped default writes `skill_points =
/// MIN(skill_points + 5, 10)` and leaves the weight alone; **no per-skill file
/// ships in `data.pak`** or in any container, so on a retail install every
/// skill takes the default and the draw is uniform over the developable ones.
///
/// The script runs synchronously and its answer is read straight back: a run
/// that fails counts as weight 0, and so does a weight of 0 or less **or a
/// goal that is not above the current points** -- the original re-reads the
/// hero's byte for that comparison rather than trusting what it passed in.
/// Everything that survives adds its weight to a running total.
///
/// A total of 0 returns with both -1s and consumes no random number. Otherwise
/// the draw is `rand(1, total)` -- the game RNG's `[low, high]` entry at slot
/// 0x14 of the generator under `[0x996ff4]+0x12a0`, the same one `GetExitPoint`
/// draws `[0, 10000]` from -- and the pick is the first skill, in table order,
/// at which subtracting weights takes the draw to zero or below. Its goal is
/// clamped to 10 on the way out. The two literals are 10 both times; this
/// engine reads them through `HeroConstants::max_skill_points`, which is the
/// same inferred figure `set_skill` clamps against, so the two cannot drift
/// apart.
///
/// ## What is not reproduced
///
/// The original hands the script a fresh handle to the hero and two
/// by-reference slots; this engine passes the receiver value and reads the
/// callee's parameter slots 2 and 3 back out of the finished frame, which is
/// `TSRecruitHero`'s mechanism (see `run_script_now`'s `out_locals`). The
/// diagnostic for a failed run is not printed, because the original prints
/// nothing for it either -- a failed skill script is simply a skill with no
/// weight.
///
/// ## Twenty-eight faults, twenty-four caught, four labelled
///
/// The survivors, each an equivalence rather than a gap: the status test on a
/// failed run is redundant with the slot-count test below it, because
/// `run_script_now` fills `out_locals` only on success; a weight of exactly 0
/// adds nothing to the total and is passed over by the draw, so `w < 0` and
/// `w <= 0` agree everywhere; `rng().between(1, total)` and `rng().below(total)
/// + 1` are the same multiply-shift over the same draw; and the slot-count
/// test guards an out-of-range read that no test can see without a sanitiser,
/// since a frame short of four slots is a file declared against the wrong
/// signature and its run either fails or leaves nothing readable there.
HostOutcome m_ai_get_skill_to_develop(CallContext& ctx) {
  static constexpr const char* kWhat = "AIGetSkillToDevelop failed";
  static constexpr std::string_view kDefaultFile = "HEROSKILL DEFAULT.VS";
  static constexpr std::int32_t kDefaultWeight = 100;
  // The registry keys on arity, so nothing else can reach here; what this
  // protects is the two `ctx.out` writes below.
  if (ctx.count() != 3) return HostOutcome::failed(kWhat);
  // The answer for everything that does not pick, written before the receiver
  // is looked at, which is the original's order.
  ctx.out(1) = Value::integer(-1);
  ctx.out(2) = Value::integer(-1);
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kWhat);
  if (ctx.scheduler == nullptr) return HostOutcome::failed(kWhat);
  // The original's own guard: a receiver naming nothing prints and returns
  // with both -1s. A handle to something that is not a hero has no skill table
  // to walk and is the same case.
  if (!ctx.arg(0).is_object()) return HostOutcome::ok_void();
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::ok_void();
  const ObjectId hero_id = ctx.arg(0).as_object().id;
  const HeroRecord* record = heroes->hero(hero_id);
  if (record == nullptr) return HostOutcome::ok_void();

  const WorldObject* slot = world->find(hero_id);
  const PlayerId owner = slot == nullptr ? kNoPlayer : slot->state.owner;
  const std::int32_t cap = heroes->constants().max_skill_points;
  AiSystem* ai = ai_system_of(*world);
  const auto resolve = [&](std::string_view file) -> std::uint32_t {
    return (ai == nullptr || owner == kNoPlayer) ? ctx.scheduler->find_chunk(file)
                                                 : ai->find_script(*ctx.scheduler, owner, file);
  };

  std::array<std::int32_t, kHeroSkillCount> goal{};
  std::array<std::int32_t, kHeroSkillCount> weight{};
  std::int32_t total = 0;
  for (std::size_t i = 0; i < kHeroSkillCount; ++i) {
    const std::int32_t current = record->skills[i];
    if (!record->offered[i] || current >= cap) continue;

    std::string file = "HeroSkill ";
    file += hero_skill_name(static_cast<HeroSkill>(i));
    file += ".vs";
    std::uint32_t chunk = resolve(file);
    if (chunk == script::kNoChunk) chunk = resolve(kDefaultFile);
    if (chunk == script::kNoChunk) continue;

    const Value args[4] = {ctx.arg(0), Value::integer(static_cast<std::int32_t>(i)),
                           Value::integer(current), Value::integer(kDefaultWeight)};
    Value ignored;
    std::vector<Value> locals;
    const HostOutcome ran = run_script_now(ctx, chunk, args, kWhat, ignored, &locals);
    if (ran.status != script::HostStatus::ok) continue;
    // Slots 2 and 3 are `int *skill_points` and `int *weight`, positionally:
    // the contract is the shipped default's declaration and a file with a
    // different parameter list is the wrong file, not a reason to search.
    if (locals.size() < 4 || !locals[2].is_integer() || !locals[3].is_integer()) continue;
    const std::int32_t w = locals[3].as_integer();
    const std::int32_t g = locals[2].as_integer();
    if (w <= 0 || g <= current) continue;
    goal[i] = g;
    weight[i] = w;
    total += w;
  }
  if (total <= 0) return HostOutcome::ok_void();

  std::int32_t draw = world->rng().between(1, total);
  for (std::size_t i = 0; i < kHeroSkillCount; ++i) {
    draw -= weight[i];
    if (draw > 0) continue;
    ctx.out(1) = Value::integer(static_cast<std::int32_t>(i));
    ctx.out(2) = Value::integer(std::min(goal[i], cap));
    return HostOutcome::ok_void();
  }
  return HostOutcome::ok_void();
}

/// The recruiter family: `set.TSRecruitArmy(class, num)`, `set.TSArenaRecruit(num)`,
/// `set.TSTempleRecruit(num)` and `set.TSRecruitHero()` -- **59 call sites**,
/// and, like `TSAdvHeroSkills`, not one line of the behaviour is native.
///
/// All four are `m_ts_adv_hero_skills`'s shape and each carries the same three
/// literals that prove it. `Settlement::TSRecruitArmy` (0x00431b30) holds
/// `TSH_RecruitArmy.vs`, `The function 'TSRecruitArmy' called for an
/// uninitialized or invalid object.` and `TSRecruitArmy: Error running
/// script!`; `TSArenaRecruit` (0x00431ca0), `TSTempleRecruit` (0x00431dd0) and
/// `TSRecruitHero` (0x004269a0) hold the same three with their own names. Each
/// body is the argument shuffle, one call into 0x0043d030 (resolve the file on
/// a player's AI search path) and 0x006a0360 (run it), and a return. There is
/// no recruiting logic anywhere in the four.
///
/// So the behaviour is four shipped files this project already reads and
/// compiles, and each does the same three things in order: **steal** units the
/// AI already controls near the settlement, **subtract** what is already queued
/// in the settlement's barracks, arenae or temples, then **spend** on the
/// remainder one training command at a time until the gold or food runs out.
///
/// ## The list comes back through an out-parameter, and it must not be assigned
///
/// `TSH_RecruitArmy.vs` opens with seven lines of shouting about it:
///
///     //BUG!!!! HACK!!!!
///     //BE CAREFUL NOT TO DIRECTLY ASSIGN TO ol!!!!!!!
///     //ol IS USED TO RETURN THE OBJECTS AND ASSIGNING TO IT WILL NOT CHANGE
///     //THE ORIGINAL LIST!!!!!!!!!
///
/// That is a fact about this engine too and for the same reason: an `ObjList`
/// value *is* a pool handle, so `ol.Add(u)` and `ol.AddList(ol2)` reach the
/// caller's list and `ol = something` only rebinds the callee's slot. The three
/// list-returning entry points therefore mint the list here, pass it as the
/// helper's last parameter, and hand back **the same handle** whatever the
/// helper did with its own locals.
///
/// `TSRecruitHero` is the exception and it is a real by-reference parameter:
/// `TSH_HeroRecruit.vs` is declared `void, Settlement set, Hero *h` and assigns
/// `h = hh`. So that one reads slot 1 back out of the finished frame; see
/// `run_script_now`'s `out_locals`.
///
/// ## The player is the settlement's owner
///
/// Measured, not assumed: 0x00431be8 loads the settlement's player through
/// `[settlement+0x90]` and passes it as 0x0043d030's first argument, so the
/// search path is the *settlement's* AI profile and not the running script's.
/// The two agree at every shipped site -- a tactic script only ever recruits
/// for its own player -- and the original's rule is still the one to keep.
///
/// ## An invalid receiver prints and returns an empty list
///
/// `TSRecruitArmy` tests the receiver at 0x00431bd7 and, when it is null,
/// pushes `The function 'TSRecruitArmy' called for an uninitialized or invalid
/// object.` and jumps *past* the script call to the return. The message goes
/// through 0x00686eb0, which is a bare `ret` in retail, so the mission keeps
/// running with an empty list -- which is what the callers then walk zero times.
/// A refusal here would trap instead, so this returns the empty list.
///
/// Nothing in the family consumes the game RNG.
[[nodiscard]] HostOutcome recruit_into_list(CallContext& ctx, std::string_view file,
                                            const char* what, std::size_t arity) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(what);
  if (ctx.scheduler == nullptr) return HostOutcome::failed(what);
  // Belt and braces, on `m_ts_adv_hero_skills`'s precedent: the registry keys
  // on arity and the VM pushes exactly the declared count, so nothing that
  // reaches here can have any other. What it protects is the argument vector
  // below, whose length the helper's parameter list fixes.
  if (ctx.count() != arity) return HostOutcome::failed(what);

  ObjListPool& pool = objlist_pool_of(*world);
  const Value out = make_objlist_value(pool.acquire_temporary(ctx.script));

  const Settlement* set = receiver_settlement(ctx);
  if (set == nullptr) return HostOutcome::ok_with(out);

  std::vector<Value> args(ctx.arguments.begin(), ctx.arguments.end());
  args.push_back(out);

  AiSystem* ai = ai_system_of(*world);
  const std::uint32_t chunk = (ai == nullptr || set->owner == kNoPlayer)
                                  ? ctx.scheduler->find_chunk(file)
                                  : ai->find_script(*ctx.scheduler, set->owner, file);
  // A run that fails prints "<name>: Error running script!" into the retail
  // no-op sink and answers the list as it stands (0x00431c2b .. 0x00431c37).
  Value ignored;
  (void)run_script_or_report(ctx, chunk, args, ignored);
  return HostOutcome::ok_with(out);
}

HostOutcome m_ts_recruit_army(CallContext& ctx) {
  return recruit_into_list(ctx, "TSH_RecruitArmy.vs", "TSRecruitArmy failed", 3);
}

HostOutcome m_ts_arena_recruit(CallContext& ctx) {
  return recruit_into_list(ctx, "TSH_ArenaRecruit.vs", "TSArenaRecruit failed", 2);
}

HostOutcome m_ts_temple_recruit(CallContext& ctx) {
  return recruit_into_list(ctx, "TSH_TempleRecruit.vs", "TSTempleRecruit failed", 2);
}

/// `set.TSRecruitHero()` -- the by-reference one. See `recruit_into_list`.
HostOutcome m_ts_recruit_hero(CallContext& ctx) {
  static constexpr const char* kWhat = "TSRecruitHero failed";
  static constexpr std::string_view kFile = "TSH_HeroRecruit.vs";
  // What the original leaves on the stack when it refuses, and what the helper
  // itself starts from: an unbound handle, which is `hero.IsValid == false`.
  const Value invalid = Value::object(script::ObjectRef{script::kNoType, 0});
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kWhat);
  if (ctx.scheduler == nullptr) return HostOutcome::failed(kWhat);
  if (ctx.count() != 1) return HostOutcome::failed(kWhat);

  const Settlement* set = receiver_settlement(ctx);
  if (set == nullptr) return HostOutcome::ok_with(invalid);

  const Value args[2] = {ctx.arg(0), invalid};
  AiSystem* ai = ai_system_of(*world);
  const std::uint32_t chunk = (ai == nullptr || set->owner == kNoPlayer)
                                  ? ctx.scheduler->find_chunk(kFile)
                                  : ai->find_script(*ctx.scheduler, set->owner, kFile);
  // A failed run answers whatever the by-reference slot holds by then, as
  // the original's caller reads its own argument memory back.
  Value ignored;
  std::vector<Value> locals;
  (void)run_script_or_report(ctx, chunk, args, ignored, &locals);
  // Slot 1 is `Hero *h`. Read positionally rather than by scanning `is_out`,
  // because the helper's signature is fixed by `AI.INI` and a file that
  // declared a *different* second parameter would be the wrong file, not a
  // reason to go looking for another slot to trust.
  if (locals.size() > 1 && locals[1].is_object()) return HostOutcome::ok_with(locals[1]);
  return HostOutcome::ok_with(invalid);
}

HostOutcome m_run_economy_script(CallContext& ctx) {
  return run_script_common(ctx, AiEnum::economy_script, "RunEconomyScript failed");
}

HostOutcome m_run_tactic_script(CallContext& ctx) {
  return run_script_common(ctx, AiEnum::tactic_script, "RunTacticScript failed");
}

}  // namespace

// --------------------------------------------------------------------------
// the AI helpers
// --------------------------------------------------------------------------

script::ScriptId AiSystem::helper(std::string_view name) const noexcept {
  const auto at = std::lower_bound(
      helpers_.begin(), helpers_.end(), name,
      [](const AiHelperEntry& row, std::string_view probe) { return row.name < probe; });
  if (at == helpers_.end() || at->name != name) return script::kNoScript;
  return at->script;
}

void AiSystem::set_helper(std::string_view name, script::ScriptId script) {
  const auto at = std::lower_bound(
      helpers_.begin(), helpers_.end(), name,
      [](const AiHelperEntry& row, std::string_view probe) { return row.name < probe; });
  if (at != helpers_.end() && at->name == name) {
    // No branch, deliberately: 0x004d3268 stores over whatever was there.
    at->script = script;
    return;
  }
  helpers_.insert(at, AiHelperEntry{std::string(name), script});
}

bool AiSystem::forget_helper(std::string_view name) {
  const auto at = std::lower_bound(
      helpers_.begin(), helpers_.end(), name,
      [](const AiHelperEntry& row, std::string_view probe) { return row.name < probe; });
  if (at == helpers_.end() || at->name != name) return false;
  helpers_.erase(at);
  return true;
}

void AiSystem::reap_helpers(const script::Scheduler& scheduler) {
  const auto dead = std::remove_if(helpers_.begin(), helpers_.end(),
                                   [&](const AiHelperEntry& entry) {
                                     return !scheduler.alive(entry.script);
                                   });
  helpers_.erase(dead, helpers_.end());
}

namespace {

/// `"data/ai helpers/" + file + ".vs"`, which is `gbr.exe`'s format string at
/// 0x007b908c character for character -- lower case, forward slash, one space
/// in `ai helpers`.
///
/// The pack stores the four files as `DATA\AI HELPERS\GUARD AREA.VS` and
/// friends, all caps with backslashes, and the shipped call sites spell the
/// argument `"siege"`, `"guard area"`, `"guard Area"` and `"siege gate"`. Both
/// resolvers below fold case and separators -- `Installation`'s `normalise`
/// upward, `Scheduler`'s downward -- so the string this builds matches through
/// either.
[[nodiscard]] std::string ai_helper_path(std::string_view file) {
  std::string path = "data/ai helpers/";
  path.append(file);
  path.append(".vs");
  return path;
}

/// `RunAIHelper(name, file, p1, ..., pN)`.
///
/// **Registered eight times, arities 2 through 9, every argument a string.**
/// `gbr.exe` 0x004d4160 registers all eight against one name string
/// (0x007b90c8) with eight distinct bodies, and the bodies are marshalling
/// thunks over one core (0x004d2ee0): each pops N strings, pads the parameter
/// list to seven with the empty string at 0x7ab85a, and passes `N - 2` as the
/// count. So arity is *not* a signature here -- it is "how many parameters to
/// hand the helper", and this engine's registry keys on it, so all eight are
/// bound to this one body.
///
/// The core (0x004d2ee0, a method of the **one global helper manager** at
/// `[0x996888]`, not of any player's AI) first takes a free thread id from 0 to
/// 127 in that manager (0x0069f910; none free is a silent nothing -- not
/// reproduced, this engine has no such cap and no shipped mission runs 128
/// helpers at once), formats the path, compiles the file or finds it in its
/// `compiledhelpers` cache (0x004d1b10), and then validates the script's own
/// signature (0x004d2ff8 onward): the return type must be void, the declared
/// parameter count must **equal** the supplied count, and every declared
/// parameter must be a by-value `str` (type 0xb, no reference). A file that
/// fails any of it prints `Invalid parms for %s AI helper` and starts nothing.
/// All four shipped helpers open `// void, str GroupName, str Target`, so on
/// retail data only the arity-4 form can ever spawn anything -- which is also
/// the only form any of the 285 sites uses, and the only one bound here (see
/// the registration).
///
/// **The helper belongs to no player.** It runs in the helper manager, which
/// tags nothing, so `AIGetPlayer` inside one answers 0 whoever started it, and
/// its life is not tied to its caller's. Here it is spawned with no parent and
/// no ownership record (it used to be adopted for the caller's AI, which no
/// shipped helper could see: SIEGE.VS takes its player from `ol[0].player`,
/// and every shipped caller is a map sequence, which no AI owns).
///
/// **There is no already-running branch.** The only lookup the original makes
/// before spawning is on the *path*, into a `compiledhelpers` vector; the
/// instance name is never consulted. The tail at 0x004d321c is unconditional:
/// `idmap[name] = id`. So `RunAIHelper` on a live key starts a second
/// coroutine and forgets the first, which keeps running unreachable. See
/// `AiSystem::set_helper`.
///
/// Failure is silent in every direction. A missing file formats *"AI helper %s
/// not found"* (0x007b9074) and a signature mismatch *"Invalid parms for %s AI
/// helper"* (0x007b9054), both through 0x00686eb0, which is a bare `ret` in
/// the retail build. Nothing is pushed either way -- the return type is 0 --
/// and nothing is inserted into the table, so `IsAIHelperRunning` answers
/// false forever after.
HostOutcome f_run_ai_helper(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RunAIHelper: no world");
  if (ctx.scheduler == nullptr) return HostOutcome::failed("RunAIHelper: no scheduler");
  if (ctx.count() < 2 || !ctx.arg(0).is_string() || !ctx.arg(1).is_string()) {
    return HostOutcome::failed("RunAIHelper: expected an instance name and a helper file");
  }
  AiSystem* ai = ai_system_of(*world);
  if (ai == nullptr) return HostOutcome::ok_void();

  const std::string path = ai_helper_path(ctx.arg(1).as_string());
  // Compiled on demand, which is what the original does and what no other
  // spawn site in this engine needs. `HostContext::library` is the seam; with
  // none, only a file something else already primed can be found.
  HostContext* host = host_context_of(ctx);
  std::uint32_t chunk = script::kNoChunk;
  if (host != nullptr && host->library != nullptr) chunk = host->library->chunk_for(path);
  if (chunk == script::kNoChunk) chunk = ctx.scheduler->find_chunk_exact(path);
  // `find_chunk_exact`, never `find_chunk`: its basename fallback would match
  // `DATA\SUBAI\TOWNHALL_BEHAVIOR_GUARD.VS` for `guard`, and `data.pak` holds
  // ten other files whose basename ends in `GUARD.VS`.
  if (chunk == script::kNoChunk) return HostOutcome::ok_void();

  // `Invalid parms for %s AI helper`: void, exactly as many parameters as were
  // supplied, every one a by-value `str`.
  const std::size_t supplied = ctx.count() - 2;
  const script::Chunk& compiled = ctx.scheduler->chunk(chunk);
  if (compiled.return_type != script::TypeTag::void_) return HostOutcome::ok_void();
  if (compiled.parameter_count != supplied || compiled.locals.size() < supplied) {
    return HostOutcome::ok_void();
  }
  for (std::size_t i = 0; i < supplied; ++i) {
    const script::LocalInfo& parameter = compiled.locals[i];
    if (parameter.type_name != "str" || parameter.is_out) return HostOutcome::ok_void();
  }

  // Arguments 2..N-1 become the helper's parameters, in order. The original
  // pads to seven with the empty string and then hands the spawn only the
  // first `count`, so the padding is unobservable and is not reproduced.
  std::vector<Value> parameters;
  parameters.reserve(supplied);
  for (std::size_t i = 2; i < ctx.count(); ++i) parameters.push_back(ctx.arg(i));

  const script::ScriptId spawned =
      ctx.scheduler->spawn(chunk, parameters, script::ObjectRef{}, script::kNoScript);
  if (spawned == script::kNoScript) return HostOutcome::ok_void();
  ai->set_helper(ctx.arg(0).as_string(), spawned);
  return HostOutcome::ok_void();
}

/// `IsAIHelperRunning(name)` (0x004d1d80) -- 236 sites.
///
/// 0x004d1d80 pops the name and answers 0x004d1cd0, which is a
/// `std::map::find` on the helper manager's name table and a comparison
/// against `end()`; it never asks the thread manager anything. What makes
/// that a liveness test is **when the table loses an entry**: the thread
/// manager deletes a thread the moment it finishes or fails (0x0069f7a0, state
/// 1 or 3) and calls the manager's completion hook at once (vtable +0x1c,
/// 0x004d2160), which erases the node whose value is that id; a stop from
/// outside does the same inside the stopping call (0x0069fee0). So a helper
/// that has ended is out of the table before any other script runs, and the
/// predicate answers false from then on. This engine erases by polling
/// (`AiSystem::reap_helpers`, once a turn), so it answers the same thing by
/// asking the scheduler as well: present in the table **and** still alive.
/// (It used to read the table alone, and answered true for the rest of the
/// turn after a helper returned.) The one window the original does keep -- a
/// helper that stops itself runs on to the end of its step with its entry
/// still there (0x0069fee0 only flags the running thread) -- is not
/// reproduced; `StopAIHelper` describes that case.
HostOutcome f_is_ai_helper_running(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsAIHelperRunning: no world");
  if (ctx.count() < 1 || !ctx.arg(0).is_string()) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  AiSystem* ai = ai_system_of(*world);
  if (ai == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  const script::ScriptId id = ai->helper(ctx.arg(0).as_string());
  const bool live = id != script::kNoScript &&
                    (ctx.scheduler == nullptr || ctx.scheduler->alive(id));
  return HostOutcome::ok_with(Value::boolean(live));
}

/// `StopAIHelper(name)` -- 179 sites.
///
/// 0x004d4100 pops the name and calls the helper manager's 0x004d32e0: find,
/// and on a miss **return having done nothing at all** -- no diagnostic, no
/// trap. On a hit, stop the thread with notification (0x0069fee0 with 1),
/// which deletes it and calls the completion hook inside the call, and that
/// hook (0x004d2160) erases the entry; here the kill and the erase are the
/// two lines below.
///
/// **It does nothing to the objects the helper was commanding.** No command
/// clearing, no group release: whatever `SetCommand` or `AttackArea` the helper
/// last wrote stays on the units. That is worth stating because it is what
/// makes stopping and restarting a helper a cheap idiom at these 179 sites.
///
/// One case of the original's is not reproduced and cannot be: a helper that
/// stops *itself* sets an abort flag on its own VM context (0x0069ff28) and
/// keeps running to its next yield, where this kills it outright. `kill` here
/// marks the record dead and `compact` removes it, so a self-stopping helper
/// loses whatever it would have run between the call and its next yield. No
/// shipped site does it -- all 179 name a helper started elsewhere.
HostOutcome f_stop_ai_helper(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("StopAIHelper: no world");
  if (ctx.scheduler == nullptr) return HostOutcome::failed("StopAIHelper: no scheduler");
  if (ctx.count() < 1 || !ctx.arg(0).is_string()) return HostOutcome::ok_void();
  AiSystem* ai = ai_system_of(*world);
  if (ai == nullptr) return HostOutcome::ok_void();

  const std::string_view name = ctx.arg(0).as_string();
  const script::ScriptId id = ai->helper(name);
  if (id == script::kNoScript) return HostOutcome::ok_void();
  ctx.scheduler->kill(id);
  ai->forget_helper(name);
  return HostOutcome::ok_void();
}

}  // namespace

// --------------------------------------------------------------------------
// the node table
// --------------------------------------------------------------------------
//
// `sim/gaika.hpp` describes the table and docs/formats/gaika.md the
// partition that cuts it, which is the original's. This section is only the
// reading of it. The table itself is built once by `GameSession` and
// lives on the world beside the class graph, because it is map-derived
// configuration and not turn state.

/// The node a GAIKA-valued receiver names, or null.
[[nodiscard]] const GaikaNode* node_of(CallContext& ctx, std::size_t at = 0) {
  World* world = world_of(ctx);
  if (world == nullptr || ctx.count() <= at) return nullptr;
  return world->gaika().find(gaika_of(ctx.arg(at)));
}

/// `g.LSA` -- 42 sites, the largest name in the family.
///
/// The area the node stands in. `IsWaterLsa` and `CheckLsaPath` are the only
/// consumers and neither compares the id against anything but another id, which
/// is what lets this engine number the areas itself; `sim/lsa.hpp` says so at
/// length. A node with no area -- one whose centre fell off the terrain layer
/// -- answers `kNoLsa`, which `IsWaterLsa` then reads as "not water".
///
/// **The bodies.** `GAIKA::LSA` (0x004225d0) is 0x0043cb00: node 0 answers 0,
/// any other id the 16-bit area field of the node record (`+0x10`) through a
/// bare index (0x0044e8e0) -- an id past the table reads off its end there and
/// answers `kNoLsa` here, as node 0 does in both. The same name is registered
/// four more times: `LSA(x, y)` (0x00422600) and `LSA(pt)` (0x00422630) are
/// 0x0043cb30, the area of the node the point falls in (0x0044e3f0, then the
/// node's `+0x10`); `obj.LSA` (0x00425fd0) is 0x0043cb70, the same for the
/// object's **raw** position (`vtbl+0x3c`, not the holder's `posRH` that
/// `GetGAIKA(obj)` falls back to -- a garrisoned unit's negative position is
/// no node, so area 0) with no test of the handle; and a no-argument `LSA()`
/// (0x006a8720), a debugging command that calls two builders (0x0044e050,
/// 0x00450a50) and answers nothing, left unbound. No shipped script calls any
/// but the GAIKA form; the others are bound below, the member one through this
/// body by the receiver's type. The point-to-node step is `GaikaTable::at`,
/// the original's slot-grid read (0x0044e3f0, `sim/gaika_table.hpp`).
[[nodiscard]] LsaId lsa_at(World& world, Point where) {
  const GaikaNode* node = world.gaika().find(world.gaika().at(world.lsa(), where));
  return node == nullptr ? kNoLsa : node->lsa;
}

HostOutcome m_gaika_lsa(CallContext& ctx) {
  if (ctx.count() > 0 && ctx.arg(0).is_object()) {
    World* world = world_of(ctx);
    if (world == nullptr) return HostOutcome::ok_with(Value::integer(kNoLsa));
    const WorldObject* slot = world->find(ctx.arg(0).as_object().id);
    if (slot == nullptr || slot->state.is_held()) return HostOutcome::ok_with(Value::integer(kNoLsa));
    return HostOutcome::ok_with(Value::integer(lsa_at(*world, world->resolve_position(slot->id))));
  }
  const GaikaNode* node = node_of(ctx);
  return HostOutcome::ok_with(Value::integer(node == nullptr ? kNoLsa : node->lsa));
}

/// `LSA(pt)` and `LSA(x, y)` -- see `m_gaika_lsa`.
HostOutcome f_lsa_at(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::ok_with(Value::integer(kNoLsa));
  Point where{};
  if (ctx.count() == 1 && is_point(ctx.arg(0))) {
    where = unpack_point(ctx.arg(0));
  } else if (ctx.count() == 2 && ctx.arg(0).is_integer() && ctx.arg(1).is_integer()) {
    where = Point{static_cast<std::int32_t>(ctx.arg(0).as_integer()),
                  static_cast<std::int32_t>(ctx.arg(1).as_integer())};
  } else {
    return HostOutcome::ok_with(Value::integer(kNoLsa));
  }
  return HostOutcome::ok_with(Value::integer(lsa_at(*world, where)));
}

/// `SS_STR(state)` -- 3 sites, and the inverse of the `SS_*` constants.
///
/// 0x0041f870 answers the literal `"SS_IDLE"` for a state of **zero or less**
/// and otherwise looks the number up through 0x0043f1b0. `sim/ai_profile.hpp`
/// says why that lookup cannot be a table here: the `SS_*` names are *data*,
/// declared in `AI.INI`'s `[SquadStates]`, and a profile overlay may add its
/// own -- which is exactly what `AiProfile::constant_name` exists for, and
/// what its own comment already names this entry point as the consumer of.
///
/// The profile is the **root** one. The original reads a single global table
/// rather than the asking player's, and there is no player argument to ask
/// with; an overlay that renamed a state would still print the root's name.
///
/// A state the profile does not declare answers the **empty string**, not the
/// sentinel: `SS_IDLE` is what zero means, and an undeclared number is not
/// zero. All three shipped sites concatenate the answer into a debug line.
HostOutcome f_ss_str(CallContext& ctx) {
  const auto text = [](std::string_view value) {
    return HostOutcome::ok_with(Value::string(std::string(value)));
  };
  const std::int32_t state =
      ctx.count() > 0 && ctx.arg(0).is_integer() ? ctx.arg(0).as_integer() : 0;
  if (state <= 0) return text(ai_enum_sentinel(AiEnum::squad_state));
  World* world = world_of(ctx);
  AiSystem* ai = world == nullptr ? nullptr : ai_system_of(*world);
  const AiProfile* profile = ai == nullptr ? nullptr : ai->profile_for({});
  if (profile == nullptr) return text({});
  return text(profile->constant_name(AiEnum::squad_state, state));
}

/// `SetMAIKA(gSrc, gDst, gMAIKA)` -- 3 sites, all in `SQUADMONITOR.VS`.
///
/// 0x00421b70 refuses seven ways -- no node table, any of the three ids zero,
/// or any two of them equal -- and otherwise hands the triple, in argument
/// order, to 0x0044fc60, which puts it at the front of the node manager's
/// MAIKA list (`SquadTable::record_route`: an equal triple is moved to the
/// front, the list keeps 512). Its reader is `GAIKA::Recruit`'s routing
/// filter (0x0043eda0), through `CheckMAIKA.vs`; see `recruit_impl`. The
/// native writer is the attacked hook, `record_squad_attacked`.
///
/// It was accepted and dropped here, on the reading that nothing read the
/// list back; the reader is a native one, and it is what decides whether a
/// squad across a recorded way-point is recruited at all.
HostOutcome f_set_maika(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetMAIKA: no world");
  const GaikaId src = gaika_of(ctx.arg(0));
  const GaikaId dst = gaika_of(ctx.arg(1));
  const GaikaId way = gaika_of(ctx.arg(2));
  if (world->gaika().count() == 0) return HostOutcome::ok_void();
  if (src == kNoGaika || dst == kNoGaika || way == kNoGaika) return HostOutcome::ok_void();
  if (src == dst || src == way || dst == way) return HostOutcome::ok_void();
  if (HeroSystem* heroes = hero_system_of(*world); heroes != nullptr) {
    heroes->squads().record_route(src, dst, way);
  }
  return HostOutcome::ok_void();
}

/// `g.MinNeed(player, own, ally, enemy)` and `g.MaxNeed(...)` -- 15 sites, and
/// the largest name left on the board. **They run a script.**
///
/// 0x0042c4a0 and 0x0042c580 are one body with a flag: both hand their five
/// arguments to 0x0043d120, which composes the asking player's AI profile
/// directory with `"GetArmyNeed.vs"` (0x007b0cf0) and runs it synchronously
/// through the same runner `RunAIHelper` uses. `MinNeed` passes 1 for that
/// flag and `MaxNeed` passes 0, and the shipped script's own signature says
/// what it is: `int GetArmyNeed(GAIKA g, int idPlayer, bool bMin, int own, int
/// ally, int enemy)`.
///
/// So neither entry point computes a need. What they *add* is the last step:
///
///     if (need > 0 && optimism != 0)  need = max(1, need * 100 / optimism)
///
/// -- the need as a **percentage of this player's optimism about this node**,
/// floored at 1 so that a real need never rounds away to nothing. `optimism`
/// is the `Laika` field at `[record + 0x14]`, twenty bytes into the
/// twenty-four-byte record, and it is reached through the per-player node map
/// exactly as `Explored` and `GetPriority` reach theirs.
///
/// **The script runs first and unconditionally.** The four gates -- a node id
/// of zero, a player outside 1..16, a player with no AI, and a node with no
/// record in that player's view -- all fail into `optimism = 0`, which leaves
/// the raw number the script returned. None of them stops the script running,
/// and none of them is an error.
template <bool kMin>
HostOutcome m_gaika_need(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("MinNeed: no world");
  if (ctx.count() < 5) return HostOutcome::ok_with(Value::integer(0));
  const GaikaId node = gaika_of(ctx.arg(0));
  const std::int32_t asked = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;

  // `GetArmyNeed.vs(g, idPlayer, bMin, own, ally, enemy)`, in that order, with
  // the player still 1-based -- the wrapper decrements it for its own gate and
  // hands the script the number the caller wrote.
  AiSystem* ai = ai_system_of(*world);
  const PlayerId player = player_from_script(asked);
  const std::uint32_t chunk =
      (ai == nullptr || player == kNoPlayer || ctx.scheduler == nullptr)
          ? (ctx.scheduler == nullptr ? 0u : ctx.scheduler->find_chunk("GetArmyNeed.vs"))
          : ai->find_script(*ctx.scheduler, player, "GetArmyNeed.vs");
  const Value args[6] = {Value::integer(node),         Value::integer(asked),
                         Value::boolean(kMin),         ctx.arg(2),
                         ctx.arg(3),                   ctx.arg(4)};
  Value returned = Value::integer(0);
  const HostOutcome ran = run_script_now(ctx, chunk, args, "GetArmyNeed failed", returned);
  if (ran.status != script::HostStatus::ok) return ran;
  std::int32_t need = returned.is_integer() ? returned.as_integer() : 0;

  // The divisor, and every way of not having one.
  std::int32_t optimism = 0;
  if (node != kNoGaika && player != kNoPlayer && ai != nullptr) {
    if (GaikaView* view = ai->gaika_view(player); view != nullptr) {
      if (const Laika* record = view->find(node); record != nullptr) {
        optimism = record->optimism;
      }
    }
  }
  // **Unsigned**, both the product and the quotient (0x0042c548): a negative
  // optimism -- `AreaAIOptimism` writes any integer -- divides by a number
  // past two billion and floors the need at 1, rather than leaving it raw.
  if (need > 0 && optimism != 0) {
    const std::uint32_t share =
        static_cast<std::uint32_t>(need) * 100u / static_cast<std::uint32_t>(optimism);
    need = share == 0 ? 1 : static_cast<std::int32_t>(share);
  }
  return HostOutcome::ok_with(Value::integer(need));
}

/// `g.Empty()` -- 1 site, and it is **a test on the node's squad container, not
/// on the ground**.
///
/// 0x00422dc0 answers `true` for a world with no node table and for an index
/// naming no node, and otherwise `[node + 0x18] == 0` -- the *pointer* to the
/// deque of squad handles the node owns, which is null until something puts a
/// squad there. `sim/squad.cpp` walks that same container for
/// `GetAIControlledUnits` and `GetSquads`.
///
/// That deque is `SquadTable::filed_under`, and it holds a squad that is
/// **standing in the node or heading for it** -- so a node an army is
/// marching on is not empty before the army arrives. This read "no squad
/// whose `GAIKAIn` is this node", which missed every squad on its way.
/// 0x0044f300 frees the deque when its last entry is erased, so an emptied
/// deque is null again and empty is "no entry", nothing more.
HostOutcome m_gaika_empty(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr || ctx.count() == 0) {
    return HostOutcome::ok_with(Value::boolean(true));
  }
  const GaikaId node = gaika_of(ctx.arg(0));
  if (node == kNoGaika || world->gaika().find(node) == nullptr) {
    return HostOutcome::ok_with(Value::boolean(true));
  }
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::ok_with(Value::boolean(true));
  return HostOutcome::ok_with(Value::boolean(heroes->squads().filed_under(node).empty()));
}

/// `g.MilitaryPresence(idPlayer)` -- 2 sites, and the sole blocker of both
/// `GAIKAMONITOR.VS` and its defensive twin, which are 45 call sites of AI
/// between them.
///
/// **Is there an army of mine standing in this node** -- mine, not a friend's.
/// 0x004313e0 walks the same per-node squad container `BestTargetInGAIKA`
/// walks, applies four tests to each squad, and answers true on the first that
/// passes all four. The tests, in the original's order:
///
///   * `[squad+0x1c] != 0` -- the squad's `eval`, so a squad with no strength
///     assessed does not count as a presence;
///   * the category `0x0044e250(viewer, owner)` **masked with 1**. It answers 1
///     only when the two players are the same -- an ally is 2, an enemy 4, and
///     a viewer past the sixteenth player 0 -- so the walk counts the viewer's
///     own squads and nobody else's. **A negative viewer takes category 1 for
///     every squad** (0x0043151d), so `MilitaryPresence(0)` -- "no player"
///     after the entry point's `dec` -- asks whether anybody at all is here;
///   * `0x0044e1f0(squad, node, 0)` masked with 6. Its 4 and 2 both mean "in
///     this node" and its 1 means "heading here", so a squad marching towards
///     the node is not a presence in it. 0x0044eb80 is called only for
///     category 4, which the mask has already refused;
///   * `[squad+0x30] & 4` clear -- `kSquadFlagPeaceful`.
///
/// **The fourth test cannot fail once the third has passed**, and it is not
/// reproduced twice. `0x0044e1f0` opens by answering 0 for a squad carrying
/// that same flag when its third argument is zero, which is what this caller
/// passes -- so a peaceful squad has already left through the `& 6` gate by the
/// time 0x004315db re-reads the flag. One test is written here and this
/// paragraph is why there is not a second.
///
/// **When no squad passes, the node's settlement is asked** (0x00420c00): a
/// settlement whose central building is a `BaseTownhall` (0x0043ffc0, a
/// stronghold here) and whose sentry strength `[+0xf8]` (`sentry_strength`) is
/// positive is a presence, always "staying", under the category
/// 0x0044e250(viewer, its owner) -- 1 for a negative viewer -- so the viewer's
/// own manned wall is an army in its own node, and nobody else's is.
///
/// Both callers read it as `if (!gaika.MilitaryPresence(AIPlayer)) continue;`
/// at the head of a `for` over every node, so false moves the walk on.
HostOutcome m_gaika_military_presence(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("MilitaryPresence: no world");
  const auto no = HostOutcome::ok_with(Value::boolean(false));
  const auto yes = HostOutcome::ok_with(Value::boolean(true));
  if (ctx.count() < 2) return no;
  const GaikaId node = gaika_of(ctx.arg(0));
  const GaikaNode* here = node == kNoGaika ? nullptr : world->gaika().find(node);
  if (here == nullptr) return no;

  // The argument is 1-based like every player number a script writes, and the
  // entry point decrements it and nothing else: negative is everybody, past
  // the sixteenth player is nobody.
  const std::int32_t viewer = (ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0) - 1;
  constexpr std::int32_t kAiOwn = 1;  // `AI_OWN`, 0x0044e250's same-player answer
  const PlayerTable& players = world->players();
  const auto category = [&](PlayerId owner) {
    return viewer < 0 ? kAiOwn : ai_relation(players, viewer, owner);
  };

  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return no;
  for (const SquadKey& key : squads_filed_under(heroes->squads(), node)) {
    const Squad* filed = heroes->squads().find(key);
    if (filed == nullptr) continue;
    const Squad& squad = *filed;
    if (squad.eval == 0) continue;
    if ((category(squad.key.player) & kAiOwn) == 0) continue;
    if ((squad.flags & kSquadFlagPeaceful) != 0) continue;
    if (squad.gaika_in != node) continue;
    return yes;
  }

  // The settlement, 0x00420c00.
  const EconomySystem* economy = economy_of(*world);
  if (here->settlement == kNoObject || economy == nullptr) return no;
  const Settlement* set = economy->settlements().for_object(here->settlement);
  if (set == nullptr || set->kind != SettlementKind::stronghold) return no;
  if (sentry_strength(*world, *set) <= 0) return no;
  return (category(set->owner) & kAiOwn) != 0 ? yes : no;
}

/// A point `length` units beyond `from`, on the ray that leaves `behind` and
/// passes through it -- 0x00422bd0, whose scale factor arrives in `ebx`.
///
/// Integer throughout: the length is an `isqrt` and each component is scaled by
/// a 32-bit multiply and a signed divide, so the result is a little short of
/// `length` from `from` rather than exactly it. Two coincident points step
/// nowhere, which is the original's own `len <= 0` arm.
[[nodiscard]] Point step_beyond(Point from, Point behind, std::int32_t length) noexcept {
  const std::int64_t dx = static_cast<std::int64_t>(from.x) - behind.x;
  const std::int64_t dy = static_cast<std::int64_t>(from.y) - behind.y;
  const std::int64_t len = isqrt(dx * dx + dy * dy);
  // The original's own `len <= 0` arm, and here it is also what keeps the
  // divide below from being a division by zero. No test can tell it from its
  // absence -- two coincident points have a zero numerator too -- which is why
  // it is called out rather than left to look like dead weight.
  if (len <= 0) return from;
  return Point{from.x + static_cast<std::int32_t>(dx * length / len),
               from.y + static_cast<std::int32_t>(dy * length / len)};
}

/// `point g.GetDestPoint(unit)` -- 1 site, in `AIOSENDSQUAD.VS`, and the point
/// a squad ordered to a node is actually sent to.
///
///     pt = g.GetDestPoint(l.Cur.Leader());
///     ...
///     l.Cur.SetCmd(state, 0, SF_ADVCHOOSER, cmd, pt);
///
/// **It is not the node's centre**, which is the whole reason the entry point
/// exists: an army sent at a walled town wants the ground *outside its nearest
/// gate*, not the town square it cannot walk into.
///
/// ## The answers, in the original's order
///
/// 0x00427b80, and the argument decides which gate rather than which town:
///
///   * a `GAIKA` naming no node answers **(0, 0)** -- the accumulator is zeroed
///     before the table is asked and nothing else writes it;
///   * a node with **no settlement** answers the node's own centre;
///   * from here the running answer is the settlement's **central building
///     position**, and every remaining branch either improves on it or leaves
///     it;
///   * when the central building is a heir of **`BaseTownhall`** (0x0043ffc0):
///     the nearest **gate** among the settlement's buildings to the *unit*,
///     then a point **450 world units beyond it, along the line from the town
///     centre through that gate**. No gate leaves the answer at the central
///     building. An offset point that falls outside the map rectangle is
///     refused -- the original tests all four edges inclusively and does not
///     clamp -- and the answer is then **the gate itself**, because the gate
///     loop wrote it into the running answer before the step was tried;
///   * when it is a heir of **`Outpost`** (0x00440060) there is a second
///     branch, and it is the one thing here not read to the end. Its guards
///     are: the settlement's owner is not **player 15**, the neutral-passive
///     pseudo-player; the settlement's owner is hostile to the *unit's* owner,
///     read off the settlement owner's row and so the same one-directional
///     rule `Gate::AllEnemiesInHolder` uses; and an object the settlement holds
///     at `[+0x5e]` resolves and has a non-null `[+0x40]`. What survives those
///     is a **1,300**-unit offset from the central building along a direction
///     built from two points this project has not identified, and it is bounds
///     tested the same way. **This engine answers the central building position
///     for an outpost**, which is where the original's own running answer sits
///     when any of those guards refuses, and is a place a squad can walk to.
///
/// The order of the two class tests is the executable's and **not** this
/// engine's `SettlementKind`: `kind_of` asks `Outpost` before `BaseTownhall`
/// because an outpost is the more specific thing, and 0x00427c19 asks
/// `BaseTownhall` first. They differ only for a class that descends from both,
/// which the retail install has none of; the tests are written the original's
/// way round anyway, because agreeing by accident is not agreeing.
HostOutcome m_gaika_dest_point(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetDestPoint: no world");
  const auto answer = [](Point p) { return HostOutcome::ok_with(pack_point(p)); };

  const GaikaId node = gaika_of(ctx.arg(0));
  const GaikaNode* here = world->gaika().record(node);
  if (here == nullptr) return answer(Point{0, 0});
  if (here->settlement == kNoObject) return answer(here->center);

  const EconomySystem* economy = economy_of(*world);
  const Settlement* set =
      economy == nullptr ? nullptr : economy->settlements().for_object(here->settlement);
  if (set == nullptr || set->anchor == kNoObject) return answer(here->center);
  // The settlement's **central building**, read off the settlement as the
  // original reads it -- not the node's centre, which is the middle of the
  // building's snapped entrance slot.
  const Point centre = world->resolve_position(set->anchor);

  const ClassGraph* graph = world->class_graph();
  const WorldObject* anchor = world->find(set->anchor);
  if (graph == nullptr || anchor == nullptr || anchor->class_index == kNoClass) return answer(centre);
  const auto heir_of = [&](std::string_view base) {
    const ClassIndex index = graph->find(base);
    return index != kNoClass && world->class_is_a(set->anchor, index);
  };
  // `BaseTownhall` first, which is the executable's order and not `kind_of`'s.
  if (!heir_of("BaseTownhall")) return answer(centre);

  if (!ctx.arg(1).is_object()) return answer(centre);
  const WorldObject* unit = world->find(ctx.arg(1).as_object().id);
  if (unit == nullptr) return answer(centre);
  const Point from = world->resolve_position(unit->id);

  // The nearest gate to the unit, with the original's own initial best of
  // 200,000 -- a distance no pair of points on a shipped map reaches, so it is
  // "nothing found" and is kept as the constant it is.
  Point best = centre;
  std::int64_t best_distance = 200000;
  for (const SettlementBuilding& building : set->buildings) {
    const WorldObject* slot = world->find(building.object);
    if (slot == nullptr || slot->object == nullptr) continue;
    if (!slot->object->is_a(NativeClass::gate)) continue;
    const Point at = world->resolve_position(building.object);
    const std::int64_t dx = static_cast<std::int64_t>(at.x) - from.x;
    const std::int64_t dy = static_cast<std::int64_t>(at.y) - from.y;
    const std::int64_t distance = isqrt(dx * dx + dy * dy);
    if (distance >= best_distance) continue;
    best_distance = distance;
    best = at;
  }
  if (best == centre) return answer(centre);

  // 450 units beyond the gate, on the ray from the town centre through it --
  // 0x00422bd0 scaled to `ebx`, which is 0x1c2 here.
  const Point out = step_beyond(best, centre, 450);
  // The bounds test, which the original runs against the map rectangle and
  // which **refuses** rather than clamping. A world with no match to ask has no
  // rectangle to be outside of, so it skips the test rather than failing it:
  // refusing everything there would make the entry point answer the town centre
  // in every synthetic world, which is a different function.
  const MatchSystem* match = match_system_of(*world);
  const std::int32_t extent = match == nullptr ? 0 : match->rules().map_size;
  if (extent <= 0) return answer(out);
  const std::int32_t edge = extent - 1;
  // Refused, the running answer is the **gate**: the gate loop has already
  // moved it there (0x00427d9c onward) and the step only replaces it when it
  // passes all four edges.
  if (out.x < 0 || out.x > edge || out.y < 0 || out.y > edge) return answer(best);
  return answer(out);
}

/// `g.Center` -- 23 sites, and the free spelling `GetGaikaCenter(int)` beside
/// it, which `gbr.exe` declares as the same operation with the argument typed
/// `int` instead of `GAIKA`.
///
/// The record's point as the partition set it (docs/formats/gaika.md): a
/// settlement node's is the middle of its central building's snapped entrance
/// slot, a region node's the slot step 10 chose.
///
/// `GAIKA::Center` (0x00422e20) answers the node record's point at `+8`/`+0xc`
/// through the bare index 0x0044e8e0, and `(0, 0)` only when the node vector
/// does not exist; node 0 is the reserved record, whose point is `(-1, -1)`
/// (0x0044cff0 builds it with the record constructor 0x0044f4a0 and never
/// fills it). An id past the vector reads beyond it there; here it is
/// `(0, 0)`.
/// `GetGaikaCenter(int)` (0x00421bc0) is the same read with no vector test.
/// **`rc.Center` (0x00697ba0) shares the key**, `(Center, 0)` on a `rect`:
/// `((l + r) / 2, (t + b) / 2)`, C division truncating toward zero. No shipped
/// script calls it (`sim/world_host.cpp`'s `m_rect_center` has the history);
/// it is answered here by the receiver's type now that the GAIKA half exists.
HostOutcome m_gaika_center(CallContext& ctx) {
  if (ctx.count() > 0 && is_rect(ctx.arg(0))) {
    World* world = world_of(ctx);
    const RectTable::Rect* r = world == nullptr ? nullptr : unpack_rect(*world, ctx.arg(0));
    if (r == nullptr) return HostOutcome::ok_with(pack_point(Point{}));
    return HostOutcome::ok_with(
        pack_point(Point{(r->left + r->right) / 2, (r->top + r->bottom) / 2}));
  }
  World* world = world_of(ctx);
  const GaikaNode* node = world == nullptr || ctx.count() == 0
                              ? nullptr
                              : world->gaika().record(gaika_of(ctx.arg(0)));
  return HostOutcome::ok_with(pack_point(node == nullptr ? Point{} : node->center));
}

/// `g.GetDistToPlayers(player, &own, &ally, &enemy)` -- 3 sites, the sole
/// blocker of all three, and the three nearest **town halls** by side.
///
/// `0x0042c1c0` walks the world's settlement vector and keeps only those whose
/// central building is a heir of `BaseTownhall` (`0x0043ffc0`, which resolves
/// that class by name once and caches it). This engine already has that
/// question answered: `SettlementKind::stronghold` is defined as *"a town hall:
/// `BaseTownhall` and its races"*, decided by the same class test at load. So
/// the "Home" in the shipped variable names -- `nHomeDist`, `nAllyHomeDist`,
/// `nEnemyHomeDist` -- is literal: villages, outposts, shipyards and tents are
/// not homes and are not measured.
///
/// **The three out-parameters start at &minus;1 and stay there.** That is the
/// original's initialisation (0x0042c253), not an error code, and the callers
/// read it as one: `ESH_FOODTRADE.VS` and `ESH_GOLDTRADE.VS` both follow the
/// call with `if (nEDist < 0) nEDist = 32000;` -- *no enemy home found means
/// infinitely far*, which is a different answer from zero and would invert
/// their `nEval = nEDist - nDist` ranking if this returned zero instead.
///
/// **The distance is doubled when the two are not in the same area.**
/// `0x0042c31c` compares the u16 at `[node+0x10]` on the settlement's own node
/// against the receiver's -- which is `GAIKA::LSA` -- and only when they match
/// does the straight-line distance stand; otherwise, *and also when the
/// settlement's node cannot be resolved at all*, it is doubled. That is a
/// crossing penalty rather than a path: the original does not path-find here
/// either, and a doubled straight line is what it charges for going round.
///
/// Two details of the walk are worth naming because they are not the obvious
/// choice:
///
///   * **The distance is measured from the settlement, not from its node.**
///     0x0042c2d7 asks the settlement for its centre and 0x0042c313 asks for
///     its node separately, one for the metric and one for the area test. Here
///     the two coincide -- `GaikaTable::build` gives a settlement node the
///     position of `Settlement::anchor` -- but they are kept apart in the code
///     because they are two questions, and because a settlement whose node did
///     not resolve still contributes a (doubled) distance in the original.
///   * **`own` is the receiver player's own homes**, not "unowned". The side
///     classifier at `0x0044e250` answers 1 for the same player, then 2 or 4
///     from bit 0 of the *asking* player's relation row -- the same one bit
///     `PlayerTable::is_enemy` reads, and one-directional in the same
///     direction. The three return values are 1, 2 and 4, which is the
///     `kAiOwn` / `kAiAlly` / `kAiEnemy` mask the squad census already uses.
///
/// A player number outside 1..16 classifies nothing, so all three answers stay
/// at &minus;1; so does a receiver that names no node. Neither traps: every
/// shipped caller is inside an evaluation loop that runs once per candidate.
HostOutcome m_gaika_dist_to_players(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetDistToPlayers: no world");
  if (ctx.count() < 5) {
    return HostOutcome::failed("GetDistToPlayers: expected three out-parameters");
  }
  // The initialisation is the answer for everything the walk does not reach.
  ctx.out(2) = Value::integer(-1);
  ctx.out(3) = Value::integer(-1);
  ctx.out(4) = Value::integer(-1);

  const GaikaNode* node = node_of(ctx);
  if (node == nullptr) return HostOutcome::ok_void();
  const PlayerId asking = ctx.arg(1).is_integer()
                              ? player_from_script(ctx.arg(1).as_integer())
                              : kNoPlayer;
  if (asking == kNoPlayer) return HostOutcome::ok_void();
  EconomySystem* economy = economy_of(*world);
  if (economy == nullptr) return HostOutcome::ok_void();

  const GaikaTable& table = world->gaika();
  const PlayerTable& players = world->players();
  for (const Settlement& home : economy->settlements().all()) {
    if (home.kind != SettlementKind::stronghold) continue;
    // The classifier answers 0 for a player past the sixteenth, and 0 is none
    // of its three cases, so an unowned home is skipped rather than counted
    // as anybody's. `kNoPlayer` is 0xFF and lands here.
    if (!PlayerTable::is_valid(home.owner)) continue;

    const Point at = world->resolve_position(home.anchor);
    const std::int64_t dx = at.x - node->center.x;
    const std::int64_t dy = at.y - node->center.y;
    std::int64_t distance = isqrt(dx * dx + dy * dy);
    const GaikaNode* theirs = table.find(table.for_settlement(home.object));
    if (theirs == nullptr || theirs->lsa != node->lsa) distance += distance;

    // The classifier's three answers are 1, 2 and 4 -- the `kAiOwn` /
    // `kAiAlly` / `kAiEnemy` values the squad census uses -- and it decides
    // between them in this order: same player first, then bit 0 of the
    // asking player's row. The out-parameters sit at 2, 3 and 4 in that
    // same order, so the classification indexes them directly.
    const std::size_t slot = home.owner == asking      ? 2
                             : players.is_enemy(asking, home.owner) ? 4
                                                       : 3;
    const std::int32_t best = ctx.out(slot).as_integer();
    if (best == -1 || distance < best) {
      ctx.out(slot) = Value::integer(static_cast<std::int32_t>(distance));
    }
  }
  return HostOutcome::ok_void();
}

/// `u.BestTargetInGAIKA()` -- 70 sites, the sole blocker of `HERO_AI_KILLALL.VS`
/// and `UNIT_AI_KILLALL.VS`, and the last member of the target-selection family
/// to be bound. `sim/combat.cpp` holds the other six and says why this one is
/// not among them: `gbr.exe` registers it out of the GAIKA slice (0x0043bd35),
/// its core is 0x00431090 rather than the family's 0x005dc950, and what it
/// searches is a *node* rather than a circle.
///
/// ## What the original does, in its order
///
/// The receiver's node first (0x0044e650, which is `GetGAIKA(pos)` and follows
/// the holder when the object itself is inside one). No node table, or no node,
/// answers an invalid handle and nothing else runs.
///
/// Then the **squads standing in that node**, out of the deque the node owns at
/// `+0x18` -- the same container `GetAIControlledUnits` walks, and `sim/squad.cpp`
/// carries its shape. A squad is kept when all four hold:
///
///   * it resolves (0x00443e30 on the packed `player | index << 4` handle);
///   * **`Squad::Eval` is non-zero** (`[squad+0x1c]`), which is the same
///     "a squad with nothing in it is not an army" guard the census applies;
///   * the relation from the receiver's player to the squad's owner has the
///     **enemy** bit -- 0x0044e250's four-way answer, masked with 4, read off
///     the *receiver's* own diplomacy row;
///   * it is **physically here**. 0x0044e1f0 answers 4 for a squad whose order
///     destination and `GAIKAIn` are both this node, 2 for one that is here and
///     heading elsewhere, 1 for one that is only *heading* here, and 0
///     otherwise -- and the caller masks with 6, so "on its way" is refused and
///     "here, but leaving" is not. It also refuses `SF_PEACEFUL` outright: the
///     boolean that would allow it is written in as false.
///
/// **The refinement between those two tests cannot change the answer, and that
/// is measured rather than assumed.** 0x0044eb80 re-reads the value through the
/// asking player's AI memory and can do exactly two things to it: promote 2 to
/// 4, or demote 1 to 0. Both pairs sit on the same side of the `& 6` mask, so
/// the entry point is blind to it -- which is why nothing below models the AI
/// memory it would need.
///
/// Each surviving squad's members are then offered to a **plain, unflagged**
/// target filter (0x00421d70 constructs it with all six of 0x005d45a0's flags
/// zero, so it is `BestTarget`'s filter exactly) by 0x004282c0, in order, after
/// two tests of their own: the unit is not dead, and its holder handle at
/// `[obj+0x154]` is `0xffff`. The first member to fail either -- or to be
/// missing -- **ends that squad's offer**: the helper returns there. It always
/// returns 0, so the walk always goes on to the next squad, and the
/// node-settlement query after the walk (0x00420c00) writes four locals nothing
/// reads. `CombatSystem::best_target_among` is the filter, and it is
/// deliberately the same code the other six run, score and all
/// (`docs/formats/target-score.md`). The filter's early stop (0x005db650
/// answering 0 for a candidate it scores under 50) ends only the squad the
/// helper is offering, and this flat list does not stop there.
///
/// **There is no radius.** Every other member of the family gets its reach from
/// the grid sweep it rides on; this one has no sweep, so a candidate on the far
/// side of the node scores by distance and is never excluded by it.
///
/// ## The fallback, which is the half that answers on this engine
///
/// If the filter found nothing, the original walks the world's settlement
/// vector (`[0x9a721c] + 0x4c`, the same one `Settlement::BestToSupply` walks),
/// resolves each settlement's **central building** through the object table,
/// and takes the **first** one that is
///
///   * an heir of `"Catapult"` -- a `Building` that declares
///     `is_central_building = 1`, so a placed siege engine has a settlement of
///     its own and appears in that vector like a town hall does;
///   * an **enemy of the receiver in the other direction**: the test is bit 0
///     of the *catapult owner's* relation row indexed by the receiver's player,
///     not the receiver's row indexed by theirs. This engine's `is_enemy` is
///     one-directional for exactly this reason, and a one-sided truce makes the
///     two spellings disagree -- so the arguments below are in the order the
///     instruction has them and not the order the rest of this file uses;
///   * standing in the receiver's own node.
///
/// It is not scored and not sorted: settlement order wins.
///
/// ## The node test
///
/// The node is the original's partition (`sim/gaika_table.hpp`,
/// docs/formats/gaika.md), so "standing in the receiver's own node" is the
/// region test it is there: a catapult's settlement is not one the partition
/// seeds (its central building is none of the five seeded bases), so the
/// catapult stands in whatever node its slot was cut into, and any catapult
/// in that node matches. Until the fifteenth round every settlement had a node
/// centred on itself, and the fallback matched only when the catapult was the
/// nearest centre to the receiver.
HostOutcome m_best_target_in_gaika(CallContext& ctx) {
  const Value none = Value::object(script::ObjectRef{script::kNoType, 0});
  World* world = world_of(ctx);
  if (world == nullptr || ctx.count() < 1 || !ctx.arg(0).is_object()) {
    return HostOutcome::ok_with(none);
  }
  const script::ObjectRef receiver = ctx.arg(0).as_object();
  const ObjectId self = receiver.id;
  const WorldObject* caster = world->find(self);
  if (caster == nullptr) return HostOutcome::ok_with(none);
  const auto answer = [&](ObjectId id) {
    return HostOutcome::ok_with(id == kNoObject ? none
                                                : Value::object(receiver.type, id));
  };

  const GaikaTable& table = world->gaika();
  const LsaPartition& areas = world->lsa();
  // `GetGAIKA(Obj)`'s question (0x0044e650): the stored position, or for a
  // held unit its `posRH` -- the town it garrisons, not the holder record's
  // (0, 0), which is the node in the map's corner.
  const GaikaId node = table.at(areas, unit_pos_rh(*world, self));
  // 0x004310c0: no table, or no node, and the search never starts. The
  // original tests those separately -- the table pointer, then the node pointer
  // 0x0044e8e0 hands back -- and `GaikaTable::at` collapses both into
  // `kNoGaika`, because it cannot answer an id the table does not have. The
  // second test was written, injected as a fault, and could not be made to
  // fail; `sim/squad.cpp`'s bounds check on the same walk was dropped for the
  // same reason.
  if (node == kNoGaika) return answer(kNoObject);

  const PlayerTable& players = world->players();
  const PlayerId mine = caster->state.owner;

  // -- the squads standing here ------------------------------------------
  if (HeroSystem* heroes = hero_system_of(*world); heroes != nullptr) {
    std::vector<ObjectId> candidates;
    for (const SquadKey& key : squads_filed_under(heroes->squads(), node)) {
      const Squad* filed = heroes->squads().find(key);
      if (filed == nullptr) continue;
      const Squad& squad = *filed;
      if (squad.eval == 0) continue;
      if ((squad.flags & kSquadFlagPeaceful) != 0) continue;
      if (!players.is_enemy(mine, squad.key.player)) continue;
      // 0x0044e1f0's 4 and 2 both mean "in this node"; its 1 means "heading
      // here", and the `& 6` mask refuses that. Where the squad is *going* is
      // therefore not read at all once it is standing here.
      if (squad.gaika_in != node) continue;
      // 0x004282c0 **stops** at the first member that is missing, dead or
      // inside a holder (`[obj+0x154] != 0xffff`) and offers none of the rest
      // of that squad -- it returns rather than skips -- so a squad whose
      // front man is garrisoned is not a target at all.
      for (const ObjectId member : squad.members) {
        const WorldObject* slot = world->find(member);
        if (slot == nullptr) break;
        if (slot->state.health <= 0) break;
        if (slot->state.holder != kNoObject) break;
        candidates.push_back(member);
      }
    }
    if (CombatSystem* combat = combat_system_of(*world); combat != nullptr) {
      const ObjectId best = combat->best_target_among(self, candidates);
      if (best != kNoObject) return answer(best);
    }
  }

  // -- the fallback: the first enemy catapult in this node ----------------
  EconomySystem* economy = economy_of(*world);
  const ClassGraph* graph = world->class_graph();
  if (economy == nullptr || graph == nullptr) return answer(kNoObject);
  // `lookup` rather than `find`, for the reason `ClassFilter::parse` gives:
  // the original resolves a class name by `id` and then by `altid`.
  const ClassIndex catapult = graph->lookup("Catapult");
  // A graph that does not know the name matches *nothing*, which is what the
  // original's string compare does. `ClassFilter::parse` would match
  // everything, so the index is resolved first and `of` used instead.
  if (catapult == kNoClass) return answer(kNoObject);
  const ClassFilter is_catapult = ClassFilter::of(catapult);

  for (const Settlement& other : economy->settlements().all()) {
    const WorldObject* building = world->find(other.anchor);
    if (building == nullptr) continue;
    if (!world->matches_filter(*building, is_catapult)) continue;
    // The transpose, deliberately: `[their record + 0x24 + my_player * 4] & 1`.
    if (!players.is_enemy(building->state.owner, mine)) continue;
    if (table.at(areas, world->resolve_position(other.anchor)) != node) continue;
    return answer(building->id);
  }
  return answer(kNoObject);
}

// --------------------------------------------------------------------------
// the per-player node view
// --------------------------------------------------------------------------

/// The lookup **every** per-player node accessor does, and the rule that goes
/// with it: *every* failure answers the type's zero and **none of them is an
/// error**. These are called in tight polling loops and a script that asks
/// about a node it cannot see gets a quiet zero, not a trap.
///
/// The steps are the original's: the player argument is **1-based**, is
/// rejected outside 1..16, selects the player's AI object -- null if that
/// player has no AI -- and then the node id selects a record, with node 0
/// answering nothing because it is the reserved one and not a place.
[[nodiscard]] Laika* laika_of(CallContext& ctx, std::size_t player_index) noexcept {
  AiSystem* ai = host_ai(ctx);
  if (ai == nullptr) return nullptr;
  if (ctx.count() <= player_index || !ctx.arg(player_index).is_integer()) return nullptr;
  const std::int32_t asked = ctx.arg(player_index).as_integer();
  if (asked < 1 || asked > 16) return nullptr;
  GaikaView* view = ai->gaika_view(static_cast<PlayerId>(asked - 1));
  if (view == nullptr) return nullptr;
  return view->find(gaika_of(ctx.arg(0)));
}

[[nodiscard]] GaikaView* view_of(CallContext& ctx, std::size_t player_index) noexcept {
  AiSystem* ai = host_ai(ctx);
  if (ai == nullptr) return nullptr;
  if (ctx.count() <= player_index || !ctx.arg(player_index).is_integer()) return nullptr;
  const std::int32_t asked = ctx.arg(player_index).as_integer();
  if (asked < 1 || asked > 16) return nullptr;
  return ai->gaika_view(static_cast<PlayerId>(asked - 1));
}

/// One flag bit, read.
///
/// Six bits live in `Laika::flags`, each measured at the accessor that reads
/// it: `Prioritized` 0x01, `Explored` 0x02, `Revealed` 0x04, `NoRecruit` 0x08,
/// `NoAttack` 0x10, `ControlFlag` 0x20.
///
/// ## Two of them have global overrides
///
/// `Explored` consults the exploration setting before the bit, and `Revealed`
/// the fog-of-war setting, tail-calling `Explored` when fog is **off**:
/// `laika_explored` and `laika_revealed`, which read `MatchRules` (the
/// scenario path's explored-everywhere flag is labelled at the first).
///
/// ## Both bits are the visibility sweep's
///
/// `sweep_gaika` (0x0044ecc0), which `AiSystem::advance` runs on every node,
/// sets both while the player stands in the node and clears `Revealed` when
/// sight is lost.
template <std::uint16_t kBit>
HostOutcome m_laika_flag(CallContext& ctx) {
  const Laika* record = laika_of(ctx, 1);
  if (record == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  World* world = world_of(ctx);
  if constexpr (kBit == kLaikaExplored) {
    if (world != nullptr) return HostOutcome::ok_with(Value::boolean(laika_explored(*world, *record)));
  } else if constexpr (kBit == kLaikaRevealed) {
    if (world != nullptr) return HostOutcome::ok_with(Value::boolean(laika_revealed(*world, *record)));
  }
  return HostOutcome::ok_with(Value::boolean((record->flags & kBit) != 0));
}

/// `g.CanExplore(player)` -- 1 site, and one of the two names between
/// `DATA\AI\RECRUITER.VS` and running. The recruiter asks it of a node it has
/// not explored, just before deciding whether to send an army there at all:
/// *can this player get to it to look?*
///
/// 0x0042afc0 answers yes on either of two grounds, and both are a `Explored`
/// test (0x0041cb80, the same body `GAIKA::Explored` calls) on somebody else's
/// node:
///
///   1. **a neighbour of this node is explored** -- the adjacency list at
///      `[node+0x1e]`/`[node+0x20]`, which `GaikaTable::neighbours` is;
///   2. **or a teleport standing in this node comes out somewhere explored**.
///      The walk is over the world's settlement vector, keeping the entries
///      that cast to `Teleport` -- the class token `Obj::AsTeleport` uses --
///      and it asks `GetGAIKA(point)` of the teleport and again of its far end,
///      so a teleport is *in* the node its own position falls in.
///
/// The player is the script's 1-based number and the original decrements it and
/// refuses 16 or more without a lower bound; `laika_of`'s `1..16` is the same
/// window with the bottom closed, and it is used here so that the two cannot
/// disagree about who has a view.
///
/// **The teleport arm reads the original's partition** (fifteenth round). A
/// teleport is a settlement, but not one the partition seeds -- its central
/// building is none of the five seeded bases -- so it shares the node of
/// whatever region its slot was cut into, as it does there. Until this round
/// every settlement had a node of its own and a teleport was always its own
/// node's centre.
///
/// It used to answer false on every shipped map, because nothing set
/// `Explored`; `AiSystem::advance` does now, so a node next to one a player has
/// seen can be chosen.
HostOutcome m_gaika_can_explore(CallContext& ctx) {
  const Value no = Value::boolean(false);
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("CanExplore: no world");
  if (ctx.count() < 2 || !ctx.arg(1).is_integer()) {
    return HostOutcome::failed("CanExplore: a player");
  }
  AiSystem* ai = host_ai(ctx);
  const GaikaTable& table = world->gaika();
  const GaikaId here = gaika_of(ctx.arg(0));
  // The node resolution, which is where the original stops when 0x0044e8e0
  // hands back nothing. Both arms below would answer no on their own for an id
  // the table does not have -- an empty neighbour list, and a node id no
  // teleport can be standing in -- so this is an equivalence kept for the
  // reading rather than for the answer.
  if (ai == nullptr || table.find(here) == nullptr) return HostOutcome::ok_with(no);
  const std::int32_t asked = ctx.arg(1).as_integer();
  if (asked < 1 || asked > 16) return HostOutcome::ok_with(no);
  GaikaView* view = ai->gaika_view(static_cast<PlayerId>(asked - 1));
  if (view == nullptr) return HostOutcome::ok_with(no);

  const auto explored = [&](GaikaId id) {
    const Laika* record = view->find(id);
    return record != nullptr && laika_explored(*world, *record);
  };
  for (const GaikaId next_door : table.neighbours(here)) {
    if (explored(next_door)) return HostOutcome::ok_with(Value::boolean(true));
  }

  // The teleports, which this engine keeps as settlements carrying a
  // destination -- `FindTeleport` walks the same vector for the same reason.
  const EconomySystem* economy = economy_of(*world);
  if (economy == nullptr) return HostOutcome::ok_with(no);
  const LsaPartition& areas = world->lsa();
  for (const Settlement& gate : economy->settlements().all()) {
    const WorldObject* mouth = world->find(gate.object);
    // The pair test is the original's ("a teleport with no pair is skipped a
    // line later", as `FindTeleport` puts it) and an equivalence here, because
    // the far end of an unpaired one resolves to nothing two lines down.
    if (mouth == nullptr || mouth->state.teleport_destination == kNoObject) continue;
    if (table.at(areas, world->resolve_position(mouth->id)) != here) continue;
    const WorldObject* far_end = world->find(mouth->state.teleport_destination);
    if (far_end == nullptr) continue;
    if (explored(table.at(areas, world->resolve_position(far_end->id)))) {
      return HostOutcome::ok_with(Value::boolean(true));
    }
  }
  return HostOutcome::ok_with(no);
}

/// And one written. `Set*` takes the value as its second argument.
template <std::uint16_t kBit>
HostOutcome m_laika_set_flag(CallContext& ctx) {
  Laika* record = laika_of(ctx, 1);
  if (record == nullptr) return HostOutcome::ok_void();
  const bool on = ctx.count() > 2 && ctx.arg(2).is_integer() && ctx.arg(2).as_integer() != 0;
  if (on) {
    record->flags |= kBit;
  } else {
    record->flags = static_cast<std::uint16_t>(record->flags & ~kBit);
  }
  return HostOutcome::ok_void();
}

/// The two synchronous-script entry points in this family, which differ in
/// their file, in the order they marshal their two arguments, and in the width
/// of the answer.
///
/// `GetStrat` (0x00421aa0 -> 0x0043d590) and `CalcPriority` (0x00421a00 ->
/// 0x0043d900) both hand **`idPlayer - 1`** to the path resolver 0x0043d030
/// with no range test of their own: a seat 0..15 with an AI and a profile
/// name searches that profile's directory first, and anything else -- a seat
/// of 16 or more included -- **still runs** the plain file. (A seat below 0
/// indexes before the player table there; it is read as "no AI" here.) A run
/// that fails answers 0, as does a negative return; `GetStrat` keeps the low
/// 16 bits of the rest and `CalcPriority` all 32. A failed run is a callee
/// that traps or suspends: 0x006a0360 answers non-zero and both trampolines
/// answer 0 (0x0043d61b, 0x0043d9ab), so here the failure goes to the pass's
/// report (`run_script_or_report`) and the caller goes on with 0.
[[nodiscard]] HostOutcome gaika_script_call(CallContext& ctx, std::string_view file,
                                            const char* what, bool player_first) {
  AiSystem* ai = host_ai(ctx);
  if (ai == nullptr || ctx.scheduler == nullptr) return HostOutcome::failed(what);
  if (ctx.count() < 2 || !ctx.arg(1).is_integer()) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  const std::int32_t asked = ctx.arg(1).as_integer();
  const PlayerId owner =
      asked >= 1 && asked <= 16 ? static_cast<PlayerId>(asked - 1) : kNoPlayer;

  const std::uint32_t chunk = ai_script_chunk(ctx, owner, file);
  // A file the installation does not have is 0 rather than a refusal: the
  // original's runner reports an error code and the caller turns it into 0.
  if (chunk == script::kNoChunk) return HostOutcome::ok_with(Value::integer(0));

  const Value args[2] = {player_first ? ctx.arg(1) : ctx.arg(0),
                         player_first ? ctx.arg(0) : ctx.arg(1)};
  Value result;
  if (!run_script_or_report(ctx, chunk, args, result)) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  const std::int32_t value = result.is_integer() ? result.as_integer() : 0;
  if (value < 0) return HostOutcome::ok_with(Value::integer(0));
  // `GetStrat` is the one passing the node first, and the one masked.
  return HostOutcome::ok_with(Value::integer(player_first ? value : (value & 0xffff)));
}

/// `g.GetPriority(idPlayer)` -- 7 sites.
HostOutcome m_gaika_get_priority(CallContext& ctx) {
  const Laika* record = laika_of(ctx, 1);
  return HostOutcome::ok_with(Value::integer(record == nullptr ? 0 : record->priority));
}

/// `g.SetPriority(idPlayer, n)` -- 2 sites, and **the one write that reorders
/// the view**.
///
/// It writes the value, sets `Prioritized` *only if the value changed*, and
/// then moves the record to its place in the priority-descending order by
/// adjacent swaps -- stably, and repairing the slot map as it goes. See
/// `GaikaView::set_priority`; the ranking is what `LAIKA(player, i)` reads and
/// what makes `GAIKAMONITOR.VS` walk the AI's own priorities in order.
HostOutcome m_gaika_set_priority(CallContext& ctx) {
  GaikaView* view = view_of(ctx, 1);
  if (view == nullptr) return HostOutcome::ok_void();
  if (ctx.count() < 3 || !ctx.arg(2).is_integer()) return HostOutcome::ok_void();
  view->set_priority(gaika_of(ctx.arg(0)), ctx.arg(2).as_integer());
  return HostOutcome::ok_void();
}

/// `g.LastSeen(idPlayer)` -- when this player last had sight of the node, in
/// milliseconds, comparable against `GetTime()`.
///
/// `GETARMYNEED.VS` is the whole of its use: `if (GetTime - g.LastSeen(idPlayer)
/// >= 600000)` with the comment *"expect only few enemies if non-stronghold
/// GIAKA not visited for 10 min"*, and a five-minute variant beside it.
///
/// The per-node visibility sweep (`sweep_gaika`, 0x0044ecc0) stamps it when
/// the player loses sight of the node, and fills `enemies` with the strength
/// standing there at that moment. A node never left answers 0, so a reader
/// takes its "long unvisited" branch; the shipped readers ask it only of a
/// node that is not `Revealed`.
HostOutcome m_gaika_last_seen(CallContext& ctx) {
  const Laika* record = laika_of(ctx, 1);
  const GameTime seen = record == nullptr ? 0 : record->last_seen;
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(seen)));
}

/// `LAIKA(idPlayer, index)` -- 5 sites, and **an ordered accessor**: the node
/// in rank slot `index` of that player's priority-sorted list.
///
/// `GAIKAMONITOR.VS` opens `while (!LAIKA(AIPlayer, GAIKACount() - 1)
/// .Prioritized(AIPlayer)) Sleep(2000)` -- wait until the lowest-ranked node
/// has been given a priority, which is "wait until `PRIORITIZE.VS` has made one
/// full pass". `RECRUITER.VS` spells the same wait as `GetGAIKA(GAIKACount - 1)`,
/// and the two agree before any sorting has happened because the view starts
/// as the identity permutation.
HostOutcome f_laika(CallContext& ctx) {
  const GaikaView* view = view_of(ctx, 0);
  const std::int32_t index =
      ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  return HostOutcome::ok_with(
      Value::integer(view == nullptr ? kNoGaika : view->ranked(index)));
}

/// `g.GetStrat(idPlayer)` -- **11 sites, and it is not a field read.**
///
/// `0x00421aa0` runs `GetGAIKAStrat.vs` synchronously and hands back what it
/// returned, so this is `run_script_now`'s fifth customer and the answer is
/// *"ask this player's profile which strategy **should** be running in this
/// node, right now"* -- a pure query that writes nothing.
///
/// The file is resolved on **the argument player's** AI search path, not the
/// caller's, which is the same rule `TSAdvHeroSkills` follows for the hero's
/// owner. Its declared signature is `int, GAIKA gaika, int idPlayer` -- which
/// is this call's two arguments in this order, so they pass straight through.
///
/// **A negative return is clamped to 0, and the rest is truncated to sixteen
/// bits**, both the original's (0x0043d590, 0x00421aa0); see
/// `gaika_script_call`.
///
/// A missing file answers 0 -- `GS_NONE` -- rather than refusing, which is the
/// original's own answer and what `GAIKAMONITOR.VS`'s `if (Strat != 0)` is
/// written for. Worth knowing before reading the corpus: `GS_SIEGE.VS` and
/// `GS_CAPTURE.VS` call this inside `while` conditions, so eleven call sites
/// are a great many executions.
HostOutcome m_gaika_get_strat(CallContext& ctx) {
  return gaika_script_call(ctx, "GetGAIKAStrat.vs", "GetStrat failed", /*player_first=*/false);
}

/// `g.CalcPriority(idPlayer)` -- 1 site, the same shape, `CalcGAIKAPriority.vs`.
///
/// **Its arguments are the other way round**, and that is not a misreading:
/// `CALCGAIKAPRIORITY.VS` is declared `int, int idPlayer, GAIKA g` where
/// `GETGAIKASTRAT.VS` is declared `int, GAIKA gaika, int idPlayer`, and
/// `AI.INI`'s `[Scripts]` table says so too. The two host entry points take
/// `(GAIKA, int)` alike and marshal differently.
///
/// Unlike `GetStrat` it keeps the full 32 bits -- priorities run to 100 and
/// beyond -- and clamps only the negative.
HostOutcome m_gaika_calc_priority(CallContext& ctx) {
  return gaika_script_call(ctx, "CalcGAIKAPriority.vs", "CalcPriority failed",
                           /*player_first=*/true);
}

/// `g.StratRunning(idPlayer)` -- 5 sites, and it is the **field**, self-cleaning.
///
/// `0x00421ae0` reads the node's `strat` and, before answering, asks the
/// scheduler whether the coroutine that strategy spawned is still alive; if it
/// is not, it **writes 0 back into the field**. So `StratRunning` is "is one
/// running", `GetStrat` is "which should be", and `GAIKAMONITOR.VS` is the two
/// together:
///
///     if (gaika.StratRunning(AIPlayer) != GS_NONE) continue;
///     Strat = gaika.GetStrat(AIPlayer);
///     if (Strat != 0) { gaika.RunStrat( AIPlayer, Strat ); Sleep( 500 ); }
///
/// The original keeps the coroutine in a parallel slot table at index
/// `0x34 + gaika` rather than in the record; `Laika::strat_script` is the same
/// information one indirection nearer, and `AiSystem::economy_script` already
/// derives its answer from liveness the same way.
HostOutcome m_gaika_strat_running(CallContext& ctx) {
  Laika* record = laika_of(ctx, 1);
  if (record == nullptr || ctx.scheduler == nullptr) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  if (record->strat != 0 && !ctx.scheduler->alive(record->strat_script)) {
    record->strat = 0;
    record->strat_script = script::kNoScript;
  }
  return HostOutcome::ok_with(Value::integer(record->strat));
}

/// `g.RunStrat(idPlayer, strat)` -- 2 sites, and the write half of the pair.
///
/// `0x0041d1d0` kills whatever is running in the node first, writes the id --
/// **unconditionally, including 0, which is how a caller stops a strategy** --
/// and then, for a non-zero id, spawns the `.vs` file the profile's
/// `[GAIKAStrat]` table names. The spawned script takes **one** argument, the
/// GAIKA id: `AI.INI` declares `GS_Siege.vs = void, GAIKA gaika` and the
/// original pushes the signature string `"void, GAIKA gaika"` verbatim. It gets
/// its player from `AIGetPlayer()`, which is why every `GS_*.vs` opens with
/// that call and why the spawn is adopted here.
///
/// Structurally this is `RunEconomyScript` with the node's `strat` field where
/// the settlement's slot would be.
HostOutcome m_gaika_run_strat(CallContext& ctx) {
  AiSystem* ai = host_ai(ctx);
  if (ai == nullptr || ctx.scheduler == nullptr) return HostOutcome::ok_void();
  if (ctx.count() < 3 || !ctx.arg(1).is_integer() || !ctx.arg(2).is_integer()) {
    return HostOutcome::ok_void();
  }
  const std::int32_t asked = ctx.arg(1).as_integer();
  if (asked < 1 || asked > 16) return HostOutcome::ok_void();
  const auto owner = static_cast<PlayerId>(asked - 1);
  Laika* record = laika_of(ctx, 1);
  if (record == nullptr) return HostOutcome::ok_void();

  if (record->strat != 0) ctx.scheduler->kill(record->strat_script);
  const std::int32_t strat = ctx.arg(2).as_integer();
  record->strat = strat;
  record->strat_script = script::kNoScript;
  if (strat == 0) return HostOutcome::ok_void();

  const std::string file = ai->script_file_for(owner, AiEnum::gaika_strategy, strat);
  if (file.empty()) return HostOutcome::ok_void();
  const std::uint32_t chunk = ai->find_script(*ctx.scheduler, owner, file);
  if (chunk == script::kNoChunk) return HostOutcome::ok_void();
  const Value argument = ctx.arg(0);
  const script::ScriptId spawned = ctx.scheduler->spawn(
      chunk, std::span<const Value>(&argument, 1), script::ObjectRef{}, ctx.script);
  if (spawned == script::kNoScript) return HostOutcome::ok_void();
  (void)ctx.scheduler->set_context(spawned, ai_thread_context(owner));
  record->strat_script = spawned;
  return HostOutcome::ok_void();
}

/// The `AreaAI*` family -- `AreaAINoRecruit` (8 sites), `AreaAIMaxPriority` (2)
/// and `AreaAISetAttackOptimism` (2) -- and **all twelve sites are one file**.
///
/// `6_Great_loses_Boudicca`'s ninth sequence, immediately after two `AIStart`
/// calls and a `Sleep(9000)` -- the sleep is there because the views have to
/// exist first:
///
///     AreaAIMaxPriority("A_NearBrit3", 5, true);
///     AreaAISetAttackOptimism("A_NearBrit3", 5, 300);
///     AreaAINoRecruit("A_Explore13", 5, true);
///
/// *"Drive this AI at the circle around the British position, make it three
/// times more optimistic about attacking into it, and keep it out of these
/// scripted areas."* This is scenario scripting rather than gameplay: it runs
/// once, from a map sequence, to bias an AI.
///
/// ## One shape, three writes
///
/// Each resolves the **named map area** -- a `<group type="0">` naming one
/// `AdvArea` object, which is `area_named`'s job and not this file's -- then
/// walks the whole node table and applies its write to every node whose
/// **centre** falls inside. Membership is by the centre and not by overlap, so
/// a large node whose centre sits outside a small area is untouched. The
/// circle test is the **query** rule, `d2 <= r*r`, which `AreaShape` already
/// distinguishes from the sampler's; rectangles are inclusive and the two rules
/// agree there.
///
/// `AreaAIMaxPriority`'s value is not the boolean: it writes **100 for true and
/// 50 for false**, through `SetPriority` -- so it re-sorts the view and sets
/// `Prioritized`. 100 is `MaxGAIKAPriority`, and that is the whole point of the
/// call: `PRIORITIZE.VS` opens with `if (nOld >= nMaxPriority) continue; //
/// adventure boosted`, so a node pinned at 100 is one the AI's own priority
/// pass will never lower again.
///
/// **A name that resolves to nothing does nothing**, and prints -- through the
/// sink that is a bare `ret` in retail. Its message names `AreaAINoRecruit`
/// whichever of the four was called, which is the executable's copy-paste and
/// is worth knowing before trusting a log.
///
/// `AreaAINoAttack` is registered by `gbr.exe` and is **not** bound: no script
/// in the installation calls it, and `ShowNotes`'s precedent settles that.
enum class AreaAiWrite : std::uint8_t { no_recruit, max_priority, optimism };

template <AreaAiWrite kWrite>
HostOutcome f_area_ai(CallContext& ctx) {
  World* world = world_of(ctx);
  AiSystem* ai = host_ai(ctx);
  if (world == nullptr || ai == nullptr) return HostOutcome::ok_void();
  if (ctx.count() < 3 || !ctx.arg(0).is_string() || !ctx.arg(1).is_integer() ||
      !ctx.arg(2).is_integer()) {
    return HostOutcome::ok_void();
  }
  const std::int32_t asked = ctx.arg(1).as_integer();
  if (asked < 1 || asked > 16) return HostOutcome::ok_void();
  GaikaView* view = ai->gaika_view(static_cast<PlayerId>(asked - 1));
  if (view == nullptr) return HostOutcome::ok_void();
  const AreaShape* shape = area_named(*world, ctx.arg(0).as_string());
  if (shape == nullptr) return HostOutcome::ok_void();

  const std::int32_t value = ctx.arg(2).as_integer();
  const GaikaTable& table = world->gaika();
  for (GaikaId node = 1; node <= table.count(); ++node) {
    const GaikaNode* record = table.find(node);
    if (record == nullptr) continue;
    if (!shape->contains_by_query_rule(record->center)) continue;
    if constexpr (kWrite == AreaAiWrite::max_priority) {
      // Through `SetPriority`, so the view re-sorts and `Prioritized` is set.
      view->set_priority(node, value != 0 ? kMaxGaikaPriority : kMaxGaikaPriority / 2);
    } else {
      Laika* laika = view->find(node);
      if (laika == nullptr) continue;
      if constexpr (kWrite == AreaAiWrite::no_recruit) {
        if (value != 0) {
          laika->flags |= kLaikaNoRecruit;
        } else {
          laika->flags = static_cast<std::uint16_t>(laika->flags & ~kLaikaNoRecruit);
        }
      } else {
        laika->optimism = value;
      }
    }
  }
  return HostOutcome::ok_void();
}

/// `GAIKACount()` -- 6 sites.
///
/// **This is the entry point `sim/ai.hpp` refused to bind, and the reason it
/// gave was correct**: `PRIORITIZE.VS` is `while (1) { for (i = 1; i <
/// GAIKACount; i += 1) { …; Sleep(…); } }`, whose only `Sleep` is inside the
/// loop body, so a count of 0 or 1 turns the outer `while` into a runaway that
/// trips the instruction budget every tick for ever. A lie that also hangs.
///
/// It is bound now because there is something to count: a shipped map has one
/// node per settlement and one per empty region, which is tens. The hazard is
/// gone rather than accepted -- and a map with no settlements and no terrain
/// layer still answers 0, so the guard is worth keeping in mind rather than
/// assuming away.
///
/// **It answers one more than the number of real nodes, and that is a
/// correction.** The original counts its whole `GAIKAs` vector, whose index 0
/// is a reserved node that is not a place -- which is why every shipped walk is
/// `for (i = 1; i < GAIKACount; i += 1)` and why `GAIKACount() - 1` is *the
/// last real node* in `GAIKAMONITOR.VS` and `RECRUITER.VS`. `GaikaTable` here
/// numbers only real nodes, so answering its `count()` made those walks stop
/// one short and made "the last node" name the second-to-last. `gaika_table.hpp`
/// used to record that as the corpus's own arithmetic; it was this entry
/// point's off-by-one.
///
/// A table with no nodes still answers 0 rather than 1: "no node graph at all"
/// is the original's "no AI singleton", and 1 would be the runaway above.
HostOutcome f_gaika_count(CallContext& ctx) {
  World* world = world_of(ctx);
  const std::int32_t nodes = world == nullptr ? 0 : world->gaika().count();
  return HostOutcome::ok_with(Value::integer(nodes == 0 ? 0 : nodes + 1));
}

/// `IsWaterLsa(id)` -- 23 sites, free.
///
/// 0x0042da30 pops the id and answers whether the area record's first word
/// -- its type, at `[[0x008c8e54]+0x24] + id * 0xc0` -- is 1, water. Nothing
/// else is read and nothing written. The id is not range-checked there, so an
/// id past the table reads off its end; here it names no area and answers
/// false, as the reserved record 0 (type 0) does in both. Water is deep water,
/// terrain index 13, which is what the areas were split on (`sim/lsa.hpp`).
HostOutcome f_is_water_lsa(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  const LsaId id = ctx.count() > 0 && ctx.arg(0).is_integer()
                       ? static_cast<LsaId>(ctx.arg(0).as_integer())
                       : kNoLsa;
  return HostOutcome::ok_with(Value::boolean(world->lsa().water(id)));
}

/// `set.GetGaika` -- 27 sites, and the exact inverse of `GAIKA::settlement`.
///
/// Every settlement gets a node, so this only answers `kNoGaika` for a
/// receiver that is not a settlement at all. `CVXGlobalAI::Persist` names a
/// `gaikaset`/`setgaika` pair, which is the original keeping the same relation
/// both ways.
HostOutcome m_settlement_gaika(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::ok_with(Value::integer(kNoGaika));
  EconomySystem* economy = economy_of(*world);
  if (economy == nullptr || ctx.count() == 0 || !ctx.arg(0).is_object()) {
    return HostOutcome::ok_with(Value::integer(kNoGaika));
  }
  const Settlement* s = economy->settlements().for_object(ctx.arg(0).as_object().id);
  return HostOutcome::ok_with(
      Value::integer(s == nullptr ? kNoGaika : world->gaika().for_settlement(s->object)));
}

/// `GetGAIKA(x)` -- **three registrations at one arity**, told apart by the
/// argument's runtime type, which is the arrangement `SpawnGroupInHolder` and
/// `GetGAIKA`'s own siblings already use.
///
/// `gbr.exe` registers it three times: `[42, 1, 1]` from an `int` (0x00746ea0),
/// `[42, 1, 20]` from an `Obj` (0x00424660) and `[42, 1, 6]` from a `point`
/// (0x005bd090, the same body `point::GetGAIKA` gets). This registry keys on
/// `(kind, name, arity)`, so one body serves all three and dispatches on what
/// it is handed.
///
///   * **An `int` is the identity**, with no bounds check on either side --
///     0x00746ea0 is `xor eax,eax; ret`, which leaves the VM stack untouched so
///     the argument *is* the result. `GAIKA::ID` shares the pointer. That was
///     established before there was a table and it does not change now: a
///     script that walks `GetGAIKA(i)` over `1 .. GAIKACount` is naming nodes
///     by index, and clamping here would break the walk rather than protect it.
///   * **A `point` is the node the place belongs to**: the partition's slot
///     grid read at the point's slot (0x0044e3f0, `GaikaTable::at`). A
///     negative coordinate is no node; there is no upper bound, so a point
///     past the right edge reads the next row's slot, and one past the whole
///     grid is no node here (the original reads past its buffer).
///   * **An `Obj` is the node its position belongs to**, which is the same
///     question asked one hop earlier. 0x0044e650 reads the raw position and,
///     when its `x` is negative, the unit's `posRH` (0x005d3db0): a unit in
///     a town's garrison is in the town's node (`unit_pos_rh`), and one on a
///     ship in the ship's. A position still negative after that is no node.
HostOutcome f_gaika_at_point(CallContext& ctx) {
  if (ctx.count() == 0) return HostOutcome::ok_with(Value::integer(kNoGaika));
  // The identity, and it must stay first: an integer is a GAIKA already.
  if (ctx.arg(0).is_integer()) {
    return HostOutcome::ok_with(Value::integer(ctx.arg(0).as_integer()));
  }
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::ok_with(Value::integer(kNoGaika));
  Point where{};
  if (is_point(ctx.arg(0))) {
    where = unpack_point(ctx.arg(0));
  } else if (ctx.arg(0).is_object()) {
    const WorldObject* slot = world->find(ctx.arg(0).as_object().id);
    if (slot == nullptr) return HostOutcome::ok_with(Value::integer(kNoGaika));
    where = unit_pos_rh(*world, slot->id);
  } else {
    return HostOutcome::ok_with(Value::integer(kNoGaika));
  }
  return HostOutcome::ok_with(Value::integer(world->gaika().at(world->lsa(), where)));
}

/// `GetGAIKA(x, y)` (0x00421910): the point form's body (0x005bd090) with the
/// coordinates as two integers -- 0x0044e3f0, 0 when the node vector does not
/// exist, the answer masked to 16 bits. No shipped site; bound because it is
/// the point form spelt differently.
HostOutcome f_gaika_at_xy(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr || ctx.count() != 2 || !ctx.arg(0).is_integer() ||
      !ctx.arg(1).is_integer()) {
    return HostOutcome::ok_with(Value::integer(kNoGaika));
  }
  const Point where{static_cast<std::int32_t>(ctx.arg(0).as_integer()),
                    static_cast<std::int32_t>(ctx.arg(1).as_integer())};
  return HostOutcome::ok_with(Value::integer(world->gaika().at(world->lsa(), where)));
}

/// `FindTeleport(player, ptSrc, ptDst)` -- 5 sites in 5 scripts, and the
/// sole blocker of `UNIT_ATTACH.VS` and `WAGON_FOLLOW.VS`; 493 of the trap
/// sweep's hits once `Breakpoint` stopped being one.
///
/// 0x0042e820 answers **which teleport to take from `ptSrc` to `ptDst`**, or
/// the invalid handle (`0xffff`) for "walk". It pops `ptDst`, `ptSrc` and the
/// player, which it decrements and never range-checks. In order:
///
///   1. Both points are placed in their LSA -- 0x0043cb30, the area of the node
///      the point falls in (0x0044e3f0, then the node's `+0x10`), which this
///      engine's node table makes the same as the area the point falls in. The
///      straight walk is `isqrt(dx² + dy²)` (0x0067c3d0). Under 2000 units
///      (`1999 <` fails) *and* in the same area, nothing is answered.
///   2. The budget a teleport has to beat is **two thirds of the straight
///      walk**, unsigned and truncated, when the areas are the same, and
///      **unbounded** when they differ -- the comparison is unsigned and the
///      initial value is `0xffffffff`.
///   3. Every settlement's own object (the world's settlement vector,
///      `[world+0x4c]`, the object id at `+0x8c`) is tried in order and kept
///      when it is a teleport (a dynamic cast to `CVXTeleport`; a teleport with
///      no pair resolves no far end a few lines later, so "has a destination"
///      decides the same cases), when the node **its position** falls in
///      (0x0044e650, none -- skipped) is in `ptSrc`'s area (0x0043cb00), and
///      when its far end (`[teleport+0x208]`, 0x005cd930) resolves and stands
///      in `ptDst`'s area (0x0043cb70). Its cost is the walk from `ptSrc` to
///      it plus the walk from its far end to `ptDst`, and the least under the
///      budget wins -- strictly less, so a tie keeps the earlier.
///   4. **Between the area test and the far end, a computer player's view is
///      asked** (0x0042e974): when the player index is under 16 and the player
///      has an AI, and that AI's view has a record for the teleport's node --
///      the node within the slot vector and its slot non-zero -- the teleport
///      is skipped unless 0x0041cb80 answers true. That is the test
///      `GAIKA::Explored` (0x0042ae70) ends in: two session overrides, then the
///      record's `Explored` bit. A player with no AI, or a node the view has no
///      record for, is not gated at all.
///
/// **Fixed in the tenth round:** the gate in step 4 was left out, on the
/// reading that it turned on two unidentified session flags. It turns on them
/// only through 0x0041cb80, the same function `Explored` answers with, and the
/// engine already answers that from the bit -- the overrides are recorded at
/// `m_laika_flag` as not carried. So a computer player now uses only teleports
/// standing in nodes it has explored, as the original's does.
HostOutcome f_find_teleport(CallContext& ctx) {
  const Value invalid = Value::object(script::ObjectRef{script::kNoType, 0});
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FindTeleport: no world");
  if (ctx.count() < 3 || !is_point(ctx.arg(1)) || !is_point(ctx.arg(2))) {
    return HostOutcome::ok_with(invalid);
  }
  EconomySystem* economy = economy_of(*world);
  if (economy == nullptr) return HostOutcome::ok_with(invalid);

  const Point src = unpack_point(ctx.arg(1));
  const Point dst = unpack_point(ctx.arg(2));
  const LsaPartition& areas = world->lsa();
  const LsaId from = areas.at(src);
  const LsaId to = areas.at(dst);
  const auto walk = [](Point a, Point b) {
    const std::int64_t dx = static_cast<std::int64_t>(b.x) - a.x;
    const std::int64_t dy = static_cast<std::int64_t>(b.y) - a.y;
    return isqrt(dx * dx + dy * dy);
  };
  const std::int64_t straight = walk(src, dst);
  if (straight < 2000 && from == to) return HostOutcome::ok_with(invalid);

  std::uint32_t budget = from == to ? static_cast<std::uint32_t>(straight * 2 / 3) : 0xFFFFFFFFu;
  ObjectId best = kNoObject;
  const GaikaTable& gaika = world->gaika();
  // Step 4's view: the asking player's, when it is a computer player.
  const std::int32_t asked = ctx.arg(0).is_integer() ? ctx.arg(0).as_integer() : 0;
  AiSystem* ai = host_ai(ctx);
  const GaikaView* view = ai != nullptr && asked >= 1 && asked <= 16
                              ? ai->gaika_view(static_cast<PlayerId>(asked - 1))
                              : nullptr;
  for (const Settlement& s : economy->settlements().all()) {
    const WorldObject* teleport = world->find(s.object);
    if (teleport == nullptr || teleport->state.teleport_destination == kNoObject) continue;
    const GaikaId stands_in = gaika.at(areas, world->resolve_position(teleport->id));
    const GaikaNode* node = gaika.find(stands_in);
    if (node == nullptr || node->lsa != from) continue;
    if (view != nullptr) {
      const Laika* record = view->find(stands_in);
      if (record != nullptr && !laika_explored(*world, *record)) continue;
    }
    const WorldObject* far_end = world->find(teleport->state.teleport_destination);
    if (far_end == nullptr) continue;
    const Point out = world->resolve_position(far_end->id);
    if (areas.at(out) != to) continue;
    const std::int64_t cost = walk(out, dst) + walk(src, world->resolve_position(teleport->id));
    if (static_cast<std::uint32_t>(cost) < budget) {
      best = teleport->id;
      budget = static_cast<std::uint32_t>(cost);
    }
  }
  if (best == kNoObject) return HostOutcome::ok_with(invalid);
  return HostOutcome::ok_with(Value::object(script::ObjectRef{kTypeObj, best}));
}

// -- ship needs ------------------------------------------------------------
//
// See the note in `sim/ai.hpp`. All four take `(idPlayer, lsa)`; the player
// is the script's 1-based number and the LSA an id from `WaterLsa`. Each body
// pops `lsa` (pushed last) and then the player, and none range-checks either:
//
//   * `ShipNeeds` (0x004226f0) answers 0x0043ef00(player - 1, lsa), the int at
//     `+0x14 + (player-1)*4` of the area record -- the need counter;
//   * `IncShipNeeds` (0x0042d9f0) adds one to, and `ClrShipNeeds` (0x0042d9b0)
//     zeroes, the int at `+0x10 + player*4` with the player **not**
//     decremented. That is the same word: `0x10 + 4p` is `0x14 + 4(p-1)`, so
//     the three agree on one counter per 1-based player, and this file keeps
//     it at the 0-based index;
//   * `Ships` (0x00422660) answers 0x0043ee40(player - 1, lsa), the int at
//     `+0x54 + (player-1)*4` -- the ship count, which `sim/ai.hpp` says this
//     engine computes rather than keeps (see `f_ships`).
//
// A player outside 1..16 is refused here (0, or nothing written), where the
// original reads or writes a neighbouring word of the record -- player 0's
// counter is the word at `+0x10`, player 17's the first ship count. No
// shipped script passes such a number: every caller hands a player it read
// from a squad or a settlement.

/// The two arguments, or `false`.
[[nodiscard]] bool ship_need_args(CallContext& ctx, PlayerId& player, LsaId& lsa) noexcept {
  if (ctx.count() < 2 || !ctx.arg(0).is_integer() || !ctx.arg(1).is_integer()) return false;
  player = player_from_script(ctx.arg(0).as_integer());
  lsa = ctx.arg(1).as_integer();
  return player != kNoPlayer;
}

/// `ShipNeeds(player, lsa)` -- 1 site.
HostOutcome f_ship_needs(CallContext& ctx) {
  AiSystem* ai = host_ai(ctx);
  PlayerId player = kNoPlayer;
  LsaId lsa = kNoLsa;
  if (ai == nullptr || !ship_need_args(ctx, player, lsa)) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(Value::integer(ai->ship_needs(player, lsa)));
}

/// `IncShipNeeds(player, lsa)` -- 2 sites.
HostOutcome f_inc_ship_needs(CallContext& ctx) {
  AiSystem* ai = host_ai(ctx);
  PlayerId player = kNoPlayer;
  LsaId lsa = kNoLsa;
  if (ai != nullptr && ship_need_args(ctx, player, lsa)) ai->add_ship_need(player, lsa);
  return HostOutcome::ok_void();
}

/// `ClrShipNeeds(player, lsa)` -- 1 site.
HostOutcome f_clr_ship_needs(CallContext& ctx) {
  AiSystem* ai = host_ai(ctx);
  PlayerId player = kNoPlayer;
  LsaId lsa = kNoLsa;
  if (ai != nullptr && ship_need_args(ctx, player, lsa)) ai->clear_ship_needs(player, lsa);
  return HostOutcome::ok_void();
}

/// `Ships(player, lsa)` -- 2 sites: the player's living ships standing in the
/// area. A census, where the original keeps a count; see `sim/ai.hpp`.
///
/// **What the kept count counts**, so the difference is on record. The word
/// at `+0x54 + owner*4` is raised by 0x0041f310 and lowered by 0x0041eee0
/// (through 0x0044e900/0x0044e920), the hooks an object passes through when
/// it is registered with, or removed from, the AI's tracking (19 and 6
/// callers: creation, death, boarding, leaving a holder and the like). For a
/// ship -- a cast to `CVXShip` -- the area is the one its position lies in
/// (0x0044e940) when `[ship+0x154]` is `0xffff`, and otherwise one read
/// through that holder chain (`+0x1f6`); the owner is `[[ship+0x70]+8]`. Both
/// hooks skip objects carrying flag 0x8000000 at `+0x2c` or 0x800000 at
/// `+0x194`; the raise also needs `[0x8c8e50]` set and `[0xa8734c]` clear,
/// the lowering needs the AI manager (`[0x8c8e58]`), an owner and a squad
/// (`[+0x174]`), and clamps at zero; both touch the count only while the area
/// table (`[0x8c8e54]`) exists. The census agrees whenever the
/// hooks have kept up with where every living ship is; it cannot reproduce a
/// count the asymmetric gates let drift, nor the holder-chain area, nor the
/// two unnamed flags. A design difference (`sim/ai.hpp`), kept.
///
/// The census behind `Ships` and `CheckLsaPath`: `fn` sees every living ship of
/// `player` standing in `lsa`, ascending by id, and stops the walk by answering
/// `true`. Answers whether it did.
template <typename Fn>
[[nodiscard]] bool each_ship_in_area(const World& world, PlayerId player, LsaId lsa, Fn&& fn) {
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::none || slot.object == nullptr) continue;
    if (!slot.object->is_a(NativeClass::ship) || slot.state.owner != player) continue;
    if (slot.state.health <= 0) continue;
    if (world.lsa().at(world.resolve_position(slot.id)) != lsa) continue;
    if (fn(slot)) return true;
  }
  return false;
}

HostOutcome f_ships(CallContext& ctx) {
  World* world = world_of(ctx);
  PlayerId player = kNoPlayer;
  LsaId lsa = kNoLsa;
  if (world == nullptr || !ship_need_args(ctx, player, lsa)) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  std::int32_t ships = 0;
  (void)each_ship_in_area(*world, player, lsa, [&](const WorldObject&) {
    ++ships;
    return false;
  });
  return HostOutcome::ok_with(Value::integer(ships));
}

// -- CheckLsaPath ----------------------------------------------------------
//
// `CheckLsaPath(lsaSrc, lsaDst, player)` -- 3 sites, and the sole blocker of
// both `EVALRECRUIT.VS` files. The executable's registration names it
// `int, int lsaSrc, int lsaDst, int playerId`; the body (0x00423120) pops the
// three -- player on top, then `lsaDst`, then `lsaSrc` -- and hands them, the
// player less one, to 0x00441b70 through 0x00441fe0, a thunk that adds no
// output list, with an empty transport string and a `(-1, -1)` rendezvous.
// `Squad::NearestHospital` makes the same call (0x00431ac9) with the same
// empty order; `PrepareAiTransportShip` reaches the worker through the other
// thunk, 0x00442010, with its own order and point and a list to fill. No
// argument is range-checked by the body.
//
// ## The worker is a breadth-first search over the area graph
//
// `sim/lsa.hpp` now carries the graph: neighbours are areas of the *other*
// type that touch on the slot grid, so every path alternates land and sea.
// 0x00441b70:
//
//   1. `src == dst` answers **1** before anything is looked at -- two
//      unknown ids that are equal answer 1 too.
//   2. Areas of different **type** (the record's first word: 1 water, 2
//      ground, 0 the reserved record) answer **0**: a land area is never on a
//      path to a sea, whatever ships there are. The original reads the two
//      words unchecked, so an id past the table reads off it; here such an id
//      is type 0 and is refused.
//   3. Distances start at `0xffff`, `src` at 0, and a FIFO walk relaxes with a
//      strict `>` -- a plain breadth-first search. A **water** neighbour is
//      entered only when the player has a ship available in it (below), and
//      the answer to that is memoised per area for the walk. A **land**
//      neighbour is entered from a sea freely; the instruction that would
//      refuse a land-to-land step (`type == 2` on the area being left) can
//      never fire on a graph that has no such edge and is not reproduced.
//   4. Along the way every water neighbour looked at -- relaxed or not --
//      in which the player has **no ship at all** (the `shipcount` word,
//      `Ships` here) is a candidate for "where a ship would have helped":
//      the first seen is taken, a later one only when its distance is
//      strictly less. An unreached area carries `0xffff`, and the original
//      compares it as it is.
//   5. `dst` unreached answers **0** -- after bumping the player's
//      `shipneed` on that candidate, if there is one, which is exactly what
//      `IncShipNeeds` does by hand. Reached, the answer is the **number of
//      areas on the path**, both ends included: `dist + 1`.
//   6. The path itself is walked **back from `dst`**: at each area the
//      first neighbour in its (ascending) list whose distance is one less is
//      taken, and pushed on the front of the output list (0x00440c40), until
//      `src` is reached -- so the list reads `src` first. The scan index is
//      not reset when it steps, a quirk that can only change which of two
//      equally short routes is named, never the count. Only
//      `PrepareAiTransportShip` reads the list, and only element 1 of a
//      three-area route: there the original names the first sea in `dst`'s
//      list that was reached, and `lsa_route`'s parent links name the first
//      sea reached in `src`'s list that touches `dst` -- the same area,
//      because both lists ascend and both seas sit at distance 1.
//
// The player argument is the script's 1-based number, decremented once and
// used as a table index throughout; one outside the table makes the ship
// test match nothing and the bump write off the record, so it is refused
// here -- the walk still runs, and answers 0 for two different areas.
//
// ## A ship is "available" by its current command, and by its transport order
//
// 0x00441730 walks the area's ship list (the record's object list at `+0x94`,
// 0x0044e9a0; only ships are entered in it, by 0x0041f310) and answers the
// first ship that is alive (`vtbl+0x50`, IsDead, false), owned by the player
// (`[[obj+0x70]+8]` against the 0-based index the caller passes), has a
// running command (0x005ae130 with 0; none -- try the next), and whose verb is
//
//   * `"idle"` (the literal at 0x007add84) -- available;
//   * `"boardunit"` -- available when the ship's pending transport order
//     equals the **caller's**: the string at `[ship+0x20c]` (0x005c6730)
//     compared whole with the caller's text (0x00426df0), and the point at
//     `[ship+0x228]`/`[+0x22c]` (0x005c6740) equal to the caller's on both
//     coordinates. `CheckLsaPath` (0x00423120) and `Squad::NearestHospital`
//     (the call at 0x00431ac9) pass `""` and `(-1, -1)`, so for them a
//     boarding ship counts only while it carries no order -- once
//     `PrepareAiTransportShip` has given it one it is somebody's crossing and
//     no longer free. `PrepareAiTransportShip` passes its own verb and point,
//     so for it a ship already boarding for this very crossing counts;
//   * `"advance"` -- available when the ship's AI destination shows no enemy:
//     the ship's squad (`[obj+0x174]`, 0x00444190; none -- available), its AI
//     destination (0x00444140, with `DestGAIKA` `[+0x24]` as the fallback --
//     `squad_ai_dest`), the AI manager (none -- available), and that node's
//     squad deque walked as `Eval` walks it: each squad with strength whose
//     category (0x0044e250 off the ship owner's row) has the enemy bit and
//     whose presence (0x0044e1f0 with peaceful squads absent, refined by
//     0x0044eb80) is any of coming, leaving or staying adds its strength, and
//     the node's settlement adds its wall's strength under the same tests
//     (0x00420c00). A total of zero makes the ship available
//     (`enemy_strength_present`; fifteenth round -- it used to count every
//     advancing ship, because the census had no node table to walk);
//   * anything else -- not this ship; try the next.
//
// The comparisons are byte-for-byte on the verb, so they are exact and
// case-sensitive, as the original's `repe cmpsb` is. The first ship that
// passes is the answer; the route search reads it as a boolean.
//
// **Fixed in the tenth round:** the route search accepted every boarding
// ship whatever order it carried, on the argument that every ship carries
// `""` and `(-1, -1)` -- true until `PrepareAiTransportShip` became their
// writer. A ship tasked for one squad's crossing then also opened the sea to
// every other `CheckLsaPath`, and to `PrepareAiTransportShip`'s own search
// for a different order.
//
// **The scan order is this engine's and not the original's.** There the area
// keeps a list of the ships standing in it and the walk is that list's order;
// here `each_ship_in_area` sweeps the world ascending by object id. The two
// agree on *which ships qualify* and can disagree on which qualifying ship is
// first, which only `PrepareAiTransportShip` can see, and only when more than
// one is free in the same sea.

/// 0x00441730, above: the first ship of `player` in `lsa` that can carry the
/// crossing `(order, where)`, or `kNoObject`. With no AI system every ship
/// reads as carrying no order, the constructed `""` and `(-1, -1)`.
[[nodiscard]] ObjectId ship_for_crossing(World& world, const AiSystem* ai, PlayerId player,
                                         LsaId lsa, std::string_view order, Point where) {
  const CommandSystem* commands = command_system(world);
  if (commands == nullptr) return kNoObject;
  const AiSystem::ShipTransport none{};
  ObjectId found = kNoObject;
  (void)each_ship_in_area(world, player, lsa, [&](const WorldObject& ship) {
    if (commands->command_count(ship.id) == 0) return false;
    const std::string_view verb = commands->command_name(ship.id, 0);
    bool available = verb == "idle";
    if (verb == "advance") {
      // The advance clause: the ship's squad, its AI destination (the order's
      // node, else `DestGAIKA`), and the enemy strength there as the ship's
      // owner sees it. No squad, no manager or no such node: available.
      HeroSystem* heroes = hero_system_of(world);
      const Squad* squad = heroes == nullptr ? nullptr : squad_containing(*heroes, ship.id);
      const GaikaId node = squad == nullptr ? kNoGaika : squad_ai_dest(*squad);
      available = squad == nullptr || ai == nullptr || !ai->manager_started() ||
                  world.gaika().find(node) == nullptr ||
                  enemy_strength_present(world, *heroes, node,
                                         static_cast<std::int32_t>(ship.state.owner)) == 0;
    }
    if (verb == "boardunit") {
      const AiSystem::ShipTransport& carrying = ai == nullptr ? none : ai->ship_transport(ship.id);
      available = carrying.order == order && carrying.where.x == where.x &&
                  carrying.where.y == where.y;
    }
    if (available) found = ship.id;
    return available;
  });
  return found;
}

/// `CheckLsaPath`'s answer, and the route behind it. `LsaRoute` is declared in
/// the header; `Squad::NearestHospital` (sim/squad.cpp) asks the same
/// question of every hospital it weighs, through the same worker.
///
/// **The route is here because `PrepareAiTransportShip` needs the middle of
/// it.** The original's 0x00442010 fills a list of area ids and its caller
/// reads element 1 -- the water the crossing goes through -- so the search that
/// answers the length has to be the search that names the hop, or the two can
/// disagree about which sea a ship is wanted in. `(order, where)` is the
/// transport order a boarding ship must already carry to count as a way
/// across: `""` and `(-1, -1)` for `CheckLsaPath` and `NearestHospital`, the
/// caller's own for `PrepareAiTransportShip`.
LsaRoute lsa_route(World& world, AiSystem* ai, LsaId src, LsaId dst, PlayerId player,
                   std::string_view order, Point where) {
  LsaRoute out;
  if (src == dst) {
    out.answer = 1;
    out.path = {src};
    return out;
  }

  const LsaPartition& areas = world.lsa();
  // The original compares the type words of the two records: 1 water, 2
  // ground, and 0 on the reserved record. An id past the table is refused
  // rather than read.
  const auto type = [&](LsaId id) -> int {
    const LsaArea* area = areas.find(id);
    return area == nullptr ? 0 : (area->water ? 1 : 2);
  };
  if (type(src) == 0 || type(src) != type(dst)) return out;

  constexpr std::int32_t kUnreached = 0xFFFF;
  const std::size_t n = areas.size();
  std::vector<std::int32_t> dist(n + 1, kUnreached);
  std::vector<std::int8_t> crossable(n + 1, -1);
  // Who relaxed each area, so the route can be walked back from `dst`.
  std::vector<LsaId> came_from(n + 1, kNoLsa);
  std::vector<LsaId> queue;
  dist[static_cast<std::size_t>(src)] = 0;
  queue.push_back(src);
  std::int32_t best = -1;
  LsaId best_area = kNoLsa;
  for (std::size_t head = 0; head < queue.size(); ++head) {
    const LsaId here = queue[head];
    const std::int32_t next = dist[static_cast<std::size_t>(here)] + 1;
    for (const LsaId there : areas.neighbours(here)) {
      const auto at = static_cast<std::size_t>(there);
      if (areas.water(there)) {
        // Strictly greater, as the original relaxes; `>=` would only enqueue
        // an area a second time at the same distance. The memo is the
        // original's too, and the test it saves is a pure function of the
        // area, so neither is a difference a sweep can see.
        if (dist[at] > next) {
          if (crossable[at] < 0) {
            const bool way_across =
                player != kNoPlayer &&
                ship_for_crossing(world, ai, player, there, order, where) != kNoObject;
            crossable[at] = way_across ? 1 : 0;
          }
          if (crossable[at] != 0) {
            dist[at] = next;
            came_from[at] = here;
            queue.push_back(there);
          }
        }
        // Looked at, relaxed or not: a sea the player has no ship in.
        if (player != kNoPlayer &&
            !each_ship_in_area(world, player, there, [](const WorldObject&) { return true; })) {
          if (best < 0 || dist[at] < best) {
            best = dist[at];
            best_area = there;
          }
        }
      } else if (dist[at] > next) {
        dist[at] = next;
        came_from[at] = here;
        queue.push_back(there);
      }
    }
  }
  const std::int32_t reached = dist[static_cast<std::size_t>(dst)];
  if (reached == kUnreached) {
    if (ai != nullptr && best >= 0 && player != kNoPlayer) ai->add_ship_need(player, best_area);
    return out;
  }
  out.answer = reached + 1;
  for (LsaId at = dst; at != kNoLsa; at = came_from[static_cast<std::size_t>(at)]) {
    out.path.push_back(at);
    if (at == src) break;
  }
  std::reverse(out.path.begin(), out.path.end());
  return out;
}

HostOutcome f_check_lsa_path(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("CheckLsaPath: no world");
  if (ctx.count() < 3 || !ctx.arg(0).is_integer() || !ctx.arg(1).is_integer() ||
      !ctx.arg(2).is_integer()) {
    return HostOutcome::failed("CheckLsaPath: three integers");
  }
  const LsaRoute route = lsa_route(*world, host_ai(ctx), ctx.arg(0).as_integer(),
                                   ctx.arg(1).as_integer(),
                                   player_from_script(ctx.arg(2).as_integer()));
  return HostOutcome::ok_with(Value::integer(route.answer));
}


/// `Ship PrepareAiTransportShip(srcLsa, dstLsa, player, cmd, pt)` -- 1 site,
/// and **the last name standing between a reachable shipped script and its
/// run**.
///
/// `AIOSENDSQUAD.VS` reaches it when `CheckLsaPath` answered 3 -- land, sea,
/// land -- and it is how an AI squad gets across water:
///
///     ship = PrepareAiTransportShip( l.Cur.GAIKAIn.LSA, g.LSA, l.Cur.Player, cmd, pt );
///     if (!ship.IsValid()) return;
///     ship.GetSquad.DelOrder;
///     l.Cur.SetCmd( state, 0, 0, "boardship", ship );
///
/// ## What it does
///
/// 0x0042e180 is short and hands the work to two places. It pops the point,
/// the order text, the player (less one, unchecked) and the two areas, and
/// runs the **same** search `CheckLsaPath` runs (0x00442010, the thunk that
/// passes a list) -- **with its own order and point**, not `CheckLsaPath`'s
/// empty ones, so a ship already boarding for exactly this crossing is a way
/// across and one boarding for any other is not. It refuses anything but a
/// length of 3 and takes **element 1 of the route** -- the water in the
/// middle. Then it asks 0x00441730 for a ship of the player's standing in
/// that water, under the same order and point, and if it finds one writes the
/// caller's `cmd` and `pt` onto it (0x005c7dd0: the string at `[ship+0x20c]`,
/// the point at `+0x228`/`+0x22c`) and hands the ship back by id. Anything
/// else is the invalid handle (`0xffff`), which the script tests. The order
/// text is released on every path (0x00695e70).
///
/// The route is why `lsa_route` exists: the original's search fills a list of
/// area ids and this reads index 1 out of it, so the search that answers the
/// length has to be the search that names the sea.
///
/// ## Which ship
///
/// `ship_for_crossing` is 0x00441730, read once and described above
/// `lsa_route`: living, the player's, standing in that area, and running
/// `idle`, `advance` towards a node with no enemy, or `boardunit` while carrying this
/// very order. The scan order is the world's rather than the area list's, as
/// said there.
///
/// **Fixed in the tenth round:** the route search here used the empty order,
/// so a ship boarding for this crossing did not open the sea for its own
/// second squad (the original's search passes the caller's order) and one
/// boarding for another crossing did (it carries an order, and the old test
/// accepted any boarding ship).
HostOutcome f_prepare_ai_transport_ship(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("PrepareAiTransportShip: no world");
  const auto none = HostOutcome::ok_with(Value::object(script::ObjectRef{script::kNoType, 0}));
  if (ctx.count() < 3 || !ctx.arg(0).is_integer() || !ctx.arg(1).is_integer() ||
      !ctx.arg(2).is_integer()) {
    return none;
  }
  const PlayerId player = player_from_script(ctx.arg(2).as_integer());
  // An early-out rather than a guard, and the sweep says so: the route search
  // will not cross water for a player it cannot name, so an unnamed one gets a
  // length of 0 and is refused one line later anyway.
  if (player == kNoPlayer) return none;

  const std::string order =
      ctx.count() > 3 && ctx.arg(3).is_string() ? std::string(ctx.arg(3).as_string()) : std::string();
  const Point where =
      ctx.count() > 4 && is_point(ctx.arg(4)) ? unpack_point(ctx.arg(4)) : Point{-1, -1};

  AiSystem* ai = host_ai(ctx);
  const LsaRoute route = lsa_route(*world, ai, ctx.arg(0).as_integer(), ctx.arg(1).as_integer(),
                                   player, order, where);
  // Three, and nothing else: one land hop is a march and more than one sea is
  // a crossing this entry point has no second ship for.
  //
  // **The length guard is also what makes the route's direction not matter.**
  // Element 1 of a three-element path is the middle whichever end it was built
  // from, so the `reverse` in `lsa_route` is unobservable *here* -- it is there
  // because the route is a route and a caller that ever wants element 2 would
  // need it. Injected as a fault and survived, for that reason.
  if (route.answer != 3 || route.path.size() < 2) return none;
  const LsaId crossing = route.path[1];

  const ObjectId found = ship_for_crossing(*world, ai, player, crossing, order, where);
  if (found == kNoObject) return none;

  // 0x005c7dd0 and its neighbour, the pair `Ship::HasAiTransport` reads.
  if (ai != nullptr) ai->set_ship_transport(found, order, where);
  return HostOutcome::ok_with(Value::object(script::ObjectRef{kTypeObj, found}));
}

// -- ApproachingSquads -----------------------------------------------------

/// `g.ApproachingSquads(player, dist)` -- 1 site, the sole blocker of
/// `GETGAIKASTRAT.VS`: the strength on its way to this node.
///
/// 0x0042c060 resolves the node and its centre -- the settlement's position
/// when it has one, the node's own otherwise, which is the same point here --
/// then walks the player's squad vector and sums `Eval` over the squads that
///
///   * are **not already in** the node (`GAIKAIn != g`);
///   * have the node as their **AI destination** -- 0x00444140's computed half
///     with `DestGAIKA` as the fallback, which is `Squad::AIDest` (0x004215c0)
///     to the instruction, so `squad_ai_dest` is the one reading for both;
///   * have a **front member** (0x00443df0, the first of `members`) that
///     resolves -- not a leader: the AI's own squads have none, and reading
///     the leader here skipped every one of them, so the strength a
///     computer player had marching on a node read 0 to `GETGAIKASTRAT.VS`;
///   * whose front member's **stored** position (`vtbl+0x3c`, so a member
///     inside a holder reads `(-1, -1)` rather than its holder's) is
///     **strictly within** `dist` of the centre -- `isqrt(dx² + dy²) < dist`.
///
/// A squad with no strength is walked and adds nothing. The player is tested
/// only against 16 there, and a 0 reads the record before the table; here a
/// player outside `1..16` answers 0, as does a GAIKA that names no node.
/// `g.ControlledNeighbors(player)` -- 1 site, and the sole blocker of
/// `DATA\AI\DEFENSIVE\MAIN.VS`.
///
/// 0x00422950 walks the node's neighbour list and counts the ones **whose
/// settlement is that player's**: the neighbour's settlement pointer must
/// resolve, and its owner must equal the argument less one. So it answers "how
/// many of the places next door does this player hold", which is what a
/// defensive AI asks before deciding it is surrounded.
///
/// Three details are the original's rather than this reading's:
///
///   * **The player is 1-based**, `dec`remented once and compared raw. A 0 or a
///     16 matches no owner and the answer is 0, without a trap.
///   * **The node's own settlement is not counted**, because a node is never
///     its own neighbour: 0x0044a3c0 refuses a pair whose two slots carry the
///     same id.
///   * **A neighbour with no settlement is skipped** before its owner is
///     looked at, which is every region node.
///
/// With no global AI at all the original answers 0 without walking anything,
/// and a table with no nodes is the same thing here.
HostOutcome m_gaika_controlled_neighbors(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ControlledNeighbors: no world");
  if (ctx.count() < 2 || !ctx.arg(1).is_integer()) {
    return HostOutcome::failed("ControlledNeighbors: a player");
  }
  const PlayerId player = player_from_script(ctx.arg(1).as_integer());
  const GaikaTable& table = world->gaika();
  if (player == kNoPlayer) return HostOutcome::ok_with(Value::integer(0));

  const EconomySystem* economy = economy_of(*world);
  std::int32_t held = 0;
  for (const GaikaId id : table.neighbours(gaika_of(ctx.arg(0)))) {
    const GaikaNode* node = table.find(id);
    // The settlement test comes first, as it does there. It is an
    // equivalence in this engine -- `SettlementStore::for_object(kNoObject)`
    // finds nothing on its own -- and it is kept because the order is the
    // original's and because a store whose sub-object handles are unallocated
    // is exactly where that would stop being true.
    if (node == nullptr || node->settlement == kNoObject) continue;
    // `[settlement+0x90]` then `[+8]`: the settlement's own owner, which is
    // the store's `owner` here and not the anchor building's.
    const Settlement* settlement =
        economy == nullptr ? nullptr : economy->settlements().for_object(node->settlement);
    if (settlement != nullptr && settlement->owner == player) ++held;
  }
  return HostOutcome::ok_with(Value::integer(held));
}

HostOutcome m_gaika_approaching_squads(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ApproachingSquads: no world");
  if (ctx.count() < 3 || !ctx.arg(1).is_integer() || !ctx.arg(2).is_integer()) {
    return HostOutcome::failed("ApproachingSquads: two integers");
  }
  const GaikaId here = gaika_of(ctx.arg(0));
  const GaikaNode* node = world->gaika().find(here);
  const PlayerId player = player_from_script(ctx.arg(1).as_integer());
  const std::int64_t within = ctx.arg(2).as_integer();
  const HeroSystem* heroes = hero_system_of(*world);
  if (node == nullptr || player == kNoPlayer || heroes == nullptr) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  std::int32_t coming = 0;
  for (const Squad& squad : heroes->squads().squads()) {
    if (squad.key.player != player || squad.gaika_in == here) continue;
    if (squad_ai_dest(squad) != here) continue;
    if (squad.members.empty()) continue;
    const WorldObject* front = world->find(squad.members.front());
    if (front == nullptr) continue;
    const Point at = front->state.position;
    const std::int64_t dx = static_cast<std::int64_t>(at.x) - node->center.x;
    const std::int64_t dy = static_cast<std::int64_t>(at.y) - node->center.y;
    if (isqrt(dx * dx + dy * dy) < within) coming += squad.eval;
  }
  return HostOutcome::ok_with(Value::integer(coming));
}

// -- the two evaluations ---------------------------------------------------
//
// Both read the executable's relation category, 0x0044e250: **1** for a
// player and itself, **2** for a pair whose relation row has bit 0 set (not at
// war, allied or merely neutral), **4** for a pair at war, and **0** for a
// player outside the table. `PlayerTable::is_enemy` is the bit 0 test with the
// self rule folded in, so the three categories are `self`, `!is_enemy` and
// `is_enemy` here.

/// The three sums of 0x00423850's sweep, which `Eval` runs, and of its twin
/// 0x00423a70, which `Unit::BestMDPos` runs on the very same `{point, range,
/// range squared}` / `{flag mask -1, player, own, ally, enemy}` pair. The two
/// walk the same cells and keep the same objects; they differ only in what a
/// unit adds, which is `bodies`: 0x00423a70 `inc`s each bucket where
/// 0x00423850 adds the unit's worth.
///
/// The sweep clamps the square `pt +- range` to the map and visits the 256-unit
/// grid cells it covers; a cell wholly inside the circle (0x00416810 answers
/// 2) is taken whole, a cell the circle cuts (1) object by object against the
/// squared range, inclusive. So the set is "on the grid, within `range` of
/// `pt`", and a **negative range inverts the square and visits nothing**. Every
/// object whose flag word meets the mask -- all of them, at -1 -- goes to the
/// visitor 0x004231f0, which keeps only **units** (bit 22 of `+0x2c`), alive or
/// not, and a held unit is not on the grid at all. Each kept unit adds its
/// census valuation (0x004439d0, `object_power`, masked to 16 bits) to the
/// bucket 0x0044e250(player, its owner) names: 1 own, 2 ally (bit 0 of the
/// viewer's relation word for the owner, so a one-sided truce is an ally), 4
/// enemy, and 0 -- counted nowhere -- for an owner past the sixteenth.
struct EvalSums {
  std::int64_t own = 0;
  std::int64_t ally = 0;
  std::int64_t enemy = 0;
};

[[nodiscard]] EvalSums eval_around(const World& world, const CombatSystem* combat, PlayerId player,
                                   Point at, std::int64_t range, bool bodies = false) {
  EvalSums sums;
  if (range < 0) return sums;
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::none || !slot.state.flags.is_unit) continue;
    if (slot.state.is_held()) continue;
    const std::int64_t dx = static_cast<std::int64_t>(slot.state.position.x) - at.x;
    const std::int64_t dy = static_cast<std::int64_t>(slot.state.position.y) - at.y;
    if (dx * dx + dy * dy > range * range) continue;
    if (!PlayerTable::is_valid(slot.state.owner)) continue;
    const std::int64_t worth = bodies ? 1 : object_power(world, combat, slot);
    if (slot.state.owner == player) {
      sums.own += worth;
    } else if (world.players().is_enemy(player, slot.state.owner)) {
      sums.enemy += worth;
    } else {
      sums.ally += worth;
    }
  }
  return sums;
}

/// `Eval(player, pt, range, *own, *ally, *enemy)` -- 2 sites. 0x00426580 pops
/// the three out-parameters, the range, the point and the player, zeroes the
/// three sums and runs `eval_around`'s sweep with `player - 1` as the viewer,
/// then writes own, ally and enemy back -- always, zero when nothing counted.
/// The player is not range-checked: past 16 the viewer is outside the table
/// and 0x0044e250 answers 0 for everything (all three stay 0, as here); 0 or
/// less makes it index a record before the table, which is not a player, and
/// here answers zeros (labelled; no shipped site passes it).
HostOutcome f_eval(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Eval: no world");
  if (ctx.count() < 6) return HostOutcome::failed("Eval: six arguments");
  EvalSums sums;
  const PlayerId player =
      ctx.arg(0).is_integer() ? player_from_script(ctx.arg(0).as_integer()) : kNoPlayer;
  if (player != kNoPlayer && is_point(ctx.arg(1)) && ctx.arg(2).is_integer()) {
    sums = eval_around(*world, combat_system_of(*world), player, unpack_point(ctx.arg(1)),
                       ctx.arg(2).as_integer());
  }
  ctx.out(3) = Value::integer(static_cast<std::int32_t>(sums.own));
  ctx.out(4) = Value::integer(static_cast<std::int32_t>(sums.ally));
  ctx.out(5) = Value::integer(static_cast<std::int32_t>(sums.enemy));
  return HostOutcome::ok_void();
}

/// One candidate crowd: an enemy squad filed under the caster's node, the front
/// member whose position is offered, and how far that is from the caster.
struct Crowd {
  const Squad* squad = nullptr;
  ObjectId front = kNoObject;
  Point where{};
  std::int64_t distance = 0;
};

/// What both spell-siting entry points establish before they measure anything.
struct SpellSite {
  bool ok = false;
  PlayerId mine = kNoPlayer;
  Point from{};
  std::vector<Crowd> crowds;
};

/// **The candidate walk `BestMDPos` and `BestProtPos` share**, instruction for
/// instruction: 0x00439de0 and 0x0043a330 have the same first loop, and their
/// second loops open with the same three lines.
///
/// The caster's node is its **squad's** `GAIKAIn` -- 0x00444190 hands back the
/// squad, `[squad+0x22]` the node -- so a unit in no squad, or in a squad filed
/// nowhere, has nothing to search before anything is measured. A squad is a
/// candidate when it has strength (`Squad::Eval`), the caster's player counts
/// it an enemy (bit 2 of 0x0044e250, read off the caster's own row), and it is
/// *here rather than heading here*: 0x0044e1f0 masked with 6, the pair
/// `BestTargetInGAIKA` documents, including 0x0044eb80's refinement, which
/// cannot cross the mask -- and 0x0044e1f0's third argument is 0, so **a
/// peaceful squad is never a candidate**, wildlife at war with everybody
/// included.
///
/// Both ends of the distance are `posRH` (`vtbl+0xc8`): the caster's and the
/// squad's **first member's** (0x00443df0), and that member's `posRH` is also
/// the point offered. For a garrisoned unit that is the town it stands in
/// (`unit_pos_rh`), not the holder record `resolve_position` stops at.
///
/// The distance band belongs to the second loop in both originals and is
/// applied here instead, which nothing can tell apart: the collected list is
/// not read between the loops.
[[nodiscard]] SpellSite crowds_in_casters_node(World& world, ObjectId self,
                                               std::int64_t min_range, std::int64_t max_range) {
  SpellSite site;
  const WorldObject* caster = world.find(self);
  HeroSystem* heroes = hero_system_of(world);
  if (caster == nullptr || heroes == nullptr) return site;
  const Squad* mine_squad = heroes->squads().find(heroes->squads().squad_of(self));
  if (mine_squad == nullptr || mine_squad->gaika_in == kNoGaika) return site;
  const PlayerId mine = caster->state.owner;
  // A caster whose player index is negative makes 0x0044e250 answer 1, which
  // fails the enemy bit, which empties the candidate list.
  if (!PlayerTable::is_valid(mine)) return site;

  const GaikaId node = mine_squad->gaika_in;
  const PlayerTable& players = world.players();
  site.ok = true;
  site.mine = mine;
  site.from = unit_pos_rh(world, self);
  for (const SquadKey& key : squads_filed_under(heroes->squads(), node)) {
    const Squad* filed = heroes->squads().find(key);
    if (filed == nullptr) continue;
    const Squad& squad = *filed;
    if (squad.eval == 0) continue;
    if (!players.is_enemy(mine, squad.key.player)) continue;
    if ((squad.flags & kSquadFlagPeaceful) != 0) continue;
    if (squad.gaika_in != node) continue;
    if (squad.members.empty()) continue;
    const ObjectId front = squad.members.front();
    if (world.find(front) == nullptr) continue;
    const Point where = unit_pos_rh(world, front);
    const std::int64_t dx = static_cast<std::int64_t>(where.x) - site.from.x;
    const std::int64_t dy = static_cast<std::int64_t>(where.y) - site.from.y;
    const std::int64_t distance = isqrt(dx * dx + dy * dy);
    if (distance < min_range || distance > max_range) continue;
    site.crowds.push_back(Crowd{&squad, front, where, distance});
  }
  return site;
}

/// `Unit::BestMDPos(MDRange, MinRange, MaxRange, MinEval, bProtectFriendly)` --
/// **where to drop a mass-damage spell**, and 2 sites that are the sole
/// blockers of `DATA\SUBAI\GDRUID_IDLE.VS` and `DATA\SUBAI\GHOST_IDLE.VS`.
///
/// Both callers read it the same way: a point whose `x` is negative means *no*,
/// and 0x00439de0 answers `(-1, -1)` for every refusal there is. Both ask for
/// a crowd between 0 and 1000 away with a `MinEval` of 500; the druid's spell
/// reaches 250 and the ghost's its own attack range.
///
/// **Not a circle.** The candidate set is `crowds_in_casters_node`'s, above.
///
/// ## What it scores
///
/// For each candidate the routine builds the very accumulator `Eval` builds --
/// `{point, MDRange, MDRange squared}` against `{a flag mask of -1, the
/// caster's player, own, ally, enemy}` -- but hands it to **0x00423a70, not
/// `Eval`'s 0x00423850**: the same cells and the same units, and each unit
/// counts **1**, not its worth. So the score is a **head count** of the enemy
/// units within `MDRange`, and the shipped `MinEval` of 500 asks for five
/// hundred of them -- which is why a retail druid or ghost so rarely finds a
/// site. With `bProtectFriendly` the caster's own and its allies' heads are
/// **subtracted**, which is the whole of what the flag means.
///
/// A candidate survives when its score is at least `MinEval`. What then ranks
/// them is the score **plus `MaxRange - distance`**, so a nearer crowd beats an
/// equal one further off; a tie keeps the first offered, which is squad order.
/// The winner is answered only if that ranked value is greater than 3 -- a
/// literal in the original, not a derived bound -- and the search starts from
/// `MinEval - 1`, which can refuse nothing the literal would have allowed.
///
/// ## What is approximated
///
///   * **`Squad::GAIKAIn` and `Squad::Eval` are refreshed once a turn**
///     (`revalue_squads`), where the original writes them inside the move and
///     inside the health write. See `sim/squad.hpp`.
///   * **0x00420c00 is not called.** The original asks its node for five things
///     between the two loops -- a settlement's two counters, a relation
///     category and two constants -- and then overwrites four of the five slots
///     before reading any of them, using the fifth as a scratch point buffer.
///     The call cannot affect the answer, and it is left out rather than
///     reproduced.
HostOutcome m_best_md_pos(CallContext& ctx) {
  const Value none = pack_point(Point{-1, -1});
  World* world = world_of(ctx);
  if (world == nullptr || ctx.count() < 6 || !ctx.arg(0).is_object()) {
    return HostOutcome::ok_with(none);
  }
  for (std::size_t at = 1; at < 5; ++at) {
    if (!ctx.arg(at).is_integer()) return HostOutcome::ok_with(none);
  }
  const std::int64_t md_range = ctx.arg(1).as_integer();
  const std::int64_t min_range = ctx.arg(2).as_integer();
  const std::int64_t max_range = ctx.arg(3).as_integer();
  const std::int64_t min_eval = ctx.arg(4).as_integer();
  // `movzx`ed off the VM stack as a byte, so anything non-zero protects.
  const bool protect_friendly = ctx.arg(5).is_integer() && ctx.arg(5).as_integer() != 0;

  const SpellSite site =
      crowds_in_casters_node(*world, ctx.arg(0).as_object().id, min_range, max_range);
  if (!site.ok) return HostOutcome::ok_with(none);
  const CombatSystem* combat = combat_system_of(*world);

  std::int64_t best = min_eval - 1;
  bool found = false;
  Point answer{};
  for (const Crowd& crowd : site.crowds) {
    const EvalSums sums =
        eval_around(*world, combat, site.mine, crowd.where, md_range, /*bodies=*/true);
    std::int64_t score = sums.enemy;
    if (protect_friendly) score -= sums.ally + sums.own;
    if (score < min_eval) continue;
    const std::int64_t ranked = score + (max_range - crowd.distance);
    if (ranked <= best) continue;
    best = ranked;
    found = true;
    answer = crowd.where;
  }
  // The literal 3, and it is tested against the *ranked* value rather than the
  // score, so a candidate can pass `MinEval` and still be refused here.
  if (!found || best <= 3) return HostOutcome::ok_with(none);
  return HostOutcome::ok_with(pack_point(answer));
}

/// What 0x00423280 counts in a circle, which is **bodies rather than
/// strength** -- the one place the AI's two circle sweeps differ.
struct ProtectCensus {
  std::int64_t own = 0;         ///< `[acc+0x08]`, the caster's own units
  std::int64_t allied = 0;      ///< `[acc+0x0c]`, everyone not at war with it
  std::int64_t enemy = 0;       ///< `[acc+0x10]`
  /// `[acc+0x14]`: of the friendly units, how many are **not** already under a
  /// cover of mercy. See `protect_census` for the assignment that makes this
  /// almost always 0 or 1.
  std::int64_t unshielded = 0;
  /// `[acc+0x18]`: the caster's own `Sacrifice` standing in the circle.
  ObjectId sacrifice = kNoObject;
};

/// 0x00423280, the visitor 0x00423cd0 hands every object in the circle.
///
/// The circle is the same one `Eval` sweeps -- squared distance against squared
/// radius, so the edge counts -- and the object set is the same: what stands on
/// the grid, which a garrisoned unit does not.
///
/// A **unit** (`[obj+0x2c] & 0x400000`) is bucketed by 0x0044e250's relation
/// category against the caster: 1 counts as its own, 2 as an ally, 4 as an
/// enemy, and a player outside the table answers 0 and is ignored. Anything
/// **else** is offered to a cast to `Sacrifice` -- the class token
/// `Obj::AsSacrifice` uses -- and the first one that casts *and* belongs to the
/// caster is kept; one belonging to somebody else clears the slot and the walk
/// goes on looking.
///
/// **The shield count is the original's slip, reproduced.** The ally arm adds
/// to `[acc+0x14]` and the own arm **assigns** to it, so what survives the
/// sweep is the shielding of the *last* friendly unit visited plus whatever
/// allies came after it -- not a count of the crowd. It matters only through
/// the two-thirds rule in `BestProtPos`, where it turns "most of this crowd is
/// unshielded" into "the last man looked at is unshielded, so there had better
/// be two of them". Reproduced rather than corrected, and the one thing that
/// does not carry over is *which* unit is last: the original's sweep runs
/// cell by cell over the object grid and this walk runs in object order.
[[nodiscard]] ProtectCensus protect_census(const World& world, PlayerId mine, Point at,
                                           std::int64_t range) {
  ProtectCensus census;
  // 0x00423cd0's square inverts for a negative range and visits no cell.
  if (range < 0) return census;
  const PlayerTable& players = world.players();
  for (const WorldObject& slot : world.objects()) {
    if (slot.internal != InternalKind::none) continue;
    if (slot.state.is_held()) continue;
    const std::int64_t dx = static_cast<std::int64_t>(slot.state.position.x) - at.x;
    const std::int64_t dy = static_cast<std::int64_t>(slot.state.position.y) - at.y;
    if (dx * dx + dy * dy > range * range) continue;

    if (!slot.state.flags.is_unit) {
      if (census.sacrifice != kNoObject) continue;
      if (slot.object == nullptr || !slot.object->is_a(NativeClass::sacrifice)) continue;
      // Stored, then dropped again when it turns out to be somebody else's --
      // which is why a second one further along still gets its chance.
      if (slot.state.owner == mine) census.sacrifice = slot.id;
      continue;
    }
    if (!PlayerTable::is_valid(slot.state.owner)) continue;
    // `ObjectFlags::half_damage`, whose one reader halves the blow: a unit
    // already under a cover of mercy.
    const std::int64_t bare = slot.state.flags.half_damage ? 0 : 1;
    if (slot.state.owner == mine) {
      ++census.own;
      census.unshielded = bare;  // the assignment, deliberately
    } else if (players.is_enemy(mine, slot.state.owner)) {
      ++census.enemy;
    } else {
      ++census.allied;
      census.unshielded += bare;
    }
  }
  return census;
}

/// `Unit::BestProtPos(Range, MinRange, MaxRange, MinEval)` -- **whom to shield**,
/// and 1 site that is the sole blocker of `DATA\SUBAI\ENCHANTRESS_IDLE.VS`.
///
/// The enchantress asks for a crowd within 80 worth 50 and then casts
/// `coverofmercy` at the answer's position, having first checked that nobody is
/// already covering it. 0x0043a330 answers an `Obj`, and the invalid handle
/// means no.
///
/// **It is `BestMDPos`'s search with a different question asked of each
/// crowd.** The candidate walk is `crowds_in_casters_node`'s, to the
/// instruction; what changes is the scoring, which counts bodies rather than
/// summing strength, and the answer, which is an object rather than a point.
///
/// ## The three tests, and the score
///
/// Around each candidate's position, within `Range`, `protect_census` counts
/// the caster's own units, its allies, its enemies and the shielding of the
/// friendly side. With `A` the friendly count and `E` the enemy count:
///
///   * **at most two thirds of the friendly side may be unshielded**:
///     `3 * unshielded <= 2 * A`. Read the census's note on what that actually
///     measures -- the original assigns where it means to add;
///   * **neither side may be less than a tenth of the whole**:
///     `A >= (E + A) * 10 / 100` and `E >= (E + A) * 10 / 100`, which is how a
///     rout is told from a battle;
///   * and the score, `E * A`, must be at least `MinEval` -- the *size of the
///     engagement*, biggest when the two sides are matched.
///
/// Ranking is `BestMDPos`'s: the score plus `MaxRange - distance`, strictly
/// greater to win, so a nearer fight beats an equal one and a tie keeps the
/// first offered.
///
/// ## Two answers, and the order they are preferred in
///
/// A candidate whose circle held the caster's own `Sacrifice` is recorded
/// against **that object**; every other candidate is recorded against its own
/// crowd. The two are ranked separately, and at the end the **crowd** is
/// preferred: only when no crowd cleared the literal 3 does the sacrifice get
/// its turn, and it must clear the same 3. So the sacrifice is a fallback, not
/// a prize.
///
/// ## What is approximated
///
///   * Everything `BestMDPos` lists: the two squad fields
///     refreshed once a turn, and 0x00420c00 left out for the same reason.
///   * **Which friendly unit is "last"** in the shield count, above.
///   * **No shipped map can reach the sacrifice arm.** `sim/world_host.cpp`
///     records why: the ritual machinery that spawns a `Sacrifice` is not here,
///     so only a synthetic world holds one. The arm is written out because it
///     is fully read, on `GetAIControlledUnits`'s precedent.
HostOutcome m_best_prot_pos(CallContext& ctx) {
  const Value none = Value::object(script::ObjectRef{script::kNoType, 0});
  World* world = world_of(ctx);
  if (world == nullptr || ctx.count() < 5 || !ctx.arg(0).is_object()) {
    return HostOutcome::ok_with(none);
  }
  for (std::size_t at = 1; at < 5; ++at) {
    if (!ctx.arg(at).is_integer()) return HostOutcome::ok_with(none);
  }
  const script::ObjectRef receiver = ctx.arg(0).as_object();
  const std::int64_t range = ctx.arg(1).as_integer();
  const std::int64_t min_range = ctx.arg(2).as_integer();
  const std::int64_t max_range = ctx.arg(3).as_integer();
  const std::int64_t min_eval = ctx.arg(4).as_integer();

  const SpellSite site = crowds_in_casters_node(*world, receiver.id, min_range, max_range);
  if (!site.ok) return HostOutcome::ok_with(none);

  std::int64_t crowd_best = min_eval - 1;
  std::int64_t sacred_best = min_eval - 1;
  // The original keeps the squad and resolves its front member at the end;
  // the member cannot change while the loop runs, so it is kept here instead.
  ObjectId crowd_pick = kNoObject;
  ObjectId sacred_pick = kNoObject;
  for (const Crowd& crowd : site.crowds) {
    const ProtectCensus census = protect_census(*world, site.mine, crowd.where, range);
    const std::int64_t friends = census.own + census.allied;
    if (3 * census.unshielded > 2 * friends) continue;
    // `(E + A) * 10 / 100`, as the original divides it.
    const std::int64_t tenth = (census.enemy + friends) * 10 / 100;
    if (friends < tenth || census.enemy < tenth) continue;
    std::int64_t score = census.enemy * friends;
    if (score < min_eval) continue;
    score += max_range - crowd.distance;
    if (census.sacrifice != kNoObject) {
      if (score <= sacred_best) continue;
      sacred_best = score;
      sacred_pick = census.sacrifice;
    } else {
      if (score <= crowd_best) continue;
      crowd_best = score;
      crowd_pick = crowd.front;
    }
  }
  if (crowd_pick != kNoObject && crowd_best > 3) {
    return HostOutcome::ok_with(Value::object(receiver.type, crowd_pick));
  }
  if (sacred_pick != kNoObject && sacred_best > 3) {
    return HostOutcome::ok_with(Value::object(receiver.type, sacred_pick));
  }
  return HostOutcome::ok_with(none);
}

/// `EnemyPlayersEval(player, *min, *max, *avg)` -- 1 site. 0x00425e20: for
/// every player at war with `player`, the sum of `eval` over its squads that
/// are neither `NoAI`, `Peaceful` nor `Sentries` (flags 0x25); the least and
/// the most of those sums, and their mean, into the three out-parameters --
/// which start at zero and are written only when positive, so an enemy with
/// no army leaves `min` at zero rather than setting it. Returns the total.
HostOutcome f_enemy_players_eval(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("EnemyPlayersEval: no world");
  if (ctx.count() < 4) return HostOutcome::failed("EnemyPlayersEval: four arguments");
  ctx.out(1) = Value::integer(0);
  ctx.out(2) = Value::integer(0);
  ctx.out(3) = Value::integer(0);
  const PlayerId player =
      ctx.arg(0).is_integer() ? player_from_script(ctx.arg(0).as_integer()) : kNoPlayer;
  const HeroSystem* heroes = hero_system_of(*world);
  if (player == kNoPlayer || heroes == nullptr) return HostOutcome::ok_with(Value::integer(0));
  constexpr std::uint16_t kSkipped = kSquadFlagNoAi | kSquadFlagPeaceful | kSquadFlagSentries;
  std::int32_t least = -1;
  std::int32_t most = -1;
  std::int32_t total = 0;
  std::int32_t counted = 0;
  for (PlayerId other = 0; other < kPlayerCount; ++other) {
    if (other == player || !world->players().is_enemy(player, other)) continue;
    std::int32_t worth = 0;
    for (const Squad& squad : heroes->squads().squads()) {
      if (squad.key.player != other || (squad.flags & kSkipped) != 0) continue;
      worth += squad.eval;
    }
    if (least < 0 || worth < least) least = worth;
    if (most < 0 || worth > most) most = worth;
    total += worth;
    ++counted;
  }
  // `> 0`, as the original tests; a `>= 0` would write the zero the slot
  // already holds, which is why a sweep cannot tell the two apart.
  if (least > 0) ctx.out(1) = Value::integer(least);
  if (most > 0) ctx.out(2) = Value::integer(most);
  if (counted > 0) ctx.out(3) = Value::integer(total / counted);
  return HostOutcome::ok_with(Value::integer(total));
}

/// `g.Recruit(idPlayer, nMin, nMax, nAvail)` -- 2 sites, both in
/// `DATA\AI\RECRUITER.VS`, and the **last name standing between that file and
/// running**. It is the AI's whole army-dispatch decision, and it is the
/// largest script-driving entry point in the game: 0x00437b80 computes nothing
/// itself beyond bookkeeping, and asks three shipped scripts for every judgment
/// it makes.
///
/// `nAvail` is an **in/out** parameter -- `int, GAIKA g, int idPlayer, int
/// nMin, int nMax, int *nAvail` is the signature the executable carries -- and
/// the recruiter's loop reads it back as "army left" after every call.
///
/// ## The three scripts, and what each one is asked
///
///   * **`EvalRecruit.vs`** (`int, Squad sq, GAIKA g, GAIKA *gDepend`), once
///     per squad: *how good a candidate is this squad for this node?* A
///     positive number is a candidate and everything else is a refusal; the
///     shipped file returns `100000 - dist * lsaPathLen`, so nearer is better.
///     `gDepend` comes back set to the squad's **current** destination when
///     that node's priority is at least this node's -- "sending this squad
///     costs somebody else something".
///   * **`CalcMaxTake.vs`** (`int, int idPlayer, GAIKA g, int *pOverneed`),
///     once per node that some candidate depends on: `own - MinNeed` as the
///     result and `own - MaxNeed` through the out-parameter. So the first is
///     how much strength that node can give up before it drops below its own
///     minimum, and the second how much before it drops below its maximum.
///   * **`SendSquad.vs`** (`bool, Squad sq, GAIKA g`), once per squad actually
///     dispatched, which is where `Squad::SendTo` is called.
///
/// All three are resolved through the asking player's profile directory, which
/// is why `DATA\AI\DEFENSIVE\EVALRECRUIT.VS` exists and gets used: the
/// defensive profile overrides the candidate test and nothing else.
///
/// ## What the body does with the answers
///
/// The unit of account throughout is `Squad::Eval` -- `MilEval`'s per-squad
/// term, and the same number `nAvail` is denominated in.
///
///  1. **Read `AssUnderFireGaika`.** The env integer on the asking player's
///     scope, compared against this node: the recruiter writes it immediately
///     before the call it makes for a stronghold under attack. It is read here
///     only to skip step 5; `EvalRecruit.vs` reads the same key itself and is
///     where it relaxes the candidate tests.
///  2. **Refuse early**: `nAvail < nMin`, a player with no AI, no squads.
///  3. **Score every squad, in index order.** Squads flagged `SF_NOAI`,
///     `SF_PEACEFUL` or `SF_SENTRIES` (0x25, `fielded_strength`'s mask) are
///     passed over without asking. For the rest, `EvalRecruit.vs` gives a score
///     and possibly a dependency; a squad whose strength alone exceeds what its
///     dependency node can give up is dropped, and the amount by which the
///     running take from a node exceeds its budget is accumulated as
///     **overflow**. `total` is the strength of everything still standing.
///  4. **`total - overflow < nMin` gives up**, and this is the cheap "no
///     chance" test -- the recruiter's own `[tried]` branch.
///  5. **Sort by score, best first, and re-run the budget** -- but only when
///     something overflowed. Step 3 spends each node's budget in squad-index
///     order, which is arbitrary; this pass spends it best-first, drops what no
///     longer fits, and **clears the dependency of anything that fits inside
///     the node's `Overneed`** -- a take that leaves the donor above its
///     *maximum* need is free, and a free take is not a dependency at all.
///     A second `< nMin` test follows.
///  6. **Send.** Two passes: first every candidate with no dependency, until
///     `nMax` is reached; then, only while the total sent is still short of
///     `nMin`, the encumbered ones. So a node takes what is free before it
///     takes what somebody else wanted, and it only ever robs a peer to reach
///     its minimum.
///  7. `*nAvail -= sent`, floored at zero, and the answer is `sent`.
///
/// ## The routing filter (0x0043816e..0x004382b0)
///
/// Between steps 5 and 6 the original runs a fourth filter, gated on two
/// conditions: the under-fire flag is clear **and the MAIKA list at
/// `[0x8c8e54]+0x38` is not empty** (`SquadTable::routes`, which `SetMAIKA`
/// and the attacked hook fill). Walking the ranking best-first to the first
/// refused entry, each live candidate standing in some node other than this
/// one (`GAIKAIn`, `[squad+0x22]`) is put to 0x0043eda0 -- once per source
/// node, the verdict cached by node -- which walks the list newest first for
/// a record from that node to this one and asks `CheckMAIKA.vs(idPlayer,
/// gSrc, gDst, gMAIKA)` of each until one answers **false**: that record's
/// way-point is the answer, and a squad with a way-point is dropped (marked
/// -2) and its strength counted. If the drops take the total below `nMin`
/// the whole recruit is abandoned. The script's trampoline answers true when
/// it cannot be run (0x0043d70c), so with no `CheckMAIKA.vs` nothing is
/// dropped. A way-point of 0 is never the answer, and cannot be recorded.
///
/// ## Four narrowings, named
///
///   * **The player window is closed at the bottom.** The original tests only
///     `player - 1 >= 16` and a `player` of 0 reads the record before the
///     array -- the same off-by-one `ExploreCircle` has, and a fault rather
///     than a behaviour. `1..16` here, as everywhere else.
///   * **The sort is given a tie-break.** The original is `std::sort` on the
///     score alone, and equal scores come out in an order the standard does not
///     fix. Ties break on the squad's index here, which is the order step 3
///     visited them in, so the ranking is a total order and the same one on
///     every platform. Rule 2 of the standing decisions: iteration order is
///     state.
///   * **A squad that disappears mid-pass is skipped rather than read.** Each
///     of the three scripts can run arbitrary code, so the squad table can move
///     under the loop; the candidate list holds keys and every use re-finds.
///     The original holds a raw pointer across its script call and re-reads
///     `[squad+0x1c]` through it.
///   * **`GAIKACount` is this table's count**, so `gDepend` is bounds-checked
///     against the nodes this engine built rather than the original's. The
///     original zeroes an out-of-range dependency; `GaikaTable::find` rejects
///     the same set plus the reserved node 0, which `kNoGaika` already means.
///
/// ## Thirteen faults, ten caught, three labelled
///
/// The three survivors are equivalences rather than gaps, and each is worth
/// stating because each looks load-bearing:
///
///   * **Step 4's `total - overflow` cannot change an outcome.** Per donor node
///     the accumulated overflow telescopes to exactly `max(0, spent - take)`,
///     so `total - overflow` is `free + sum of min(spent, take)` -- and the
///     re-run's greedy best-first fit is bounded by the same sum. The early
///     test is therefore never *stricter* than the one that follows it, and
///     when nothing overflowed it is subtracting zero. It is kept because it
///     is the original's, and because it is the recruiter's cheap "no chance"
///     answer: it is what avoids the sort and the second sweep, not what
///     decides anything.
///   * **The sort's tie-break cannot be observed from one platform.** Equal
///     scores under `std::sort` come out in whatever order the implementation
///     leaves them, so a test here would be asserting this libc++ rather than
///     the rule. It is a determinism guard, and the thing it guards against is
///     two builds of this engine disagreeing.
///   * **The bounds check on `gDepend` guards an out-of-range write** that no
///     test can see without a sanitiser: the ids it rejects are the ones past
///     the node count, and they would index the budget vector out of bounds.
///     Zero is the only rejected id a test could reach, and `kNoGaika` already
///     means it.
///
/// **It sends nothing on every shipped map today**, and for a reason that is
/// not this entry point's: nothing here writes `Squad::Eval`, so step 3's
/// short-circuit -- the original's own `if (squad->Eval == 0) return 0` ahead
/// of `EvalRecruit.vs` -- refuses every squad, `total` is 0, and step 4 gives
/// up. `sim/squad.hpp` records that standing gap; this is the fourth reader of
/// it, and it is bound anyway so that `RECRUITER.VS` runs its loop and sleeps
/// rather than trapping on the first node it looks at.
HostOutcome m_gaika_recruit(CallContext& ctx) {
  const Value none = Value::integer(0);
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Recruit: no world");
  if (ctx.count() < 5) return HostOutcome::failed("Recruit: five arguments");

  const GaikaId node = gaika_of(ctx.arg(0));
  const auto number = [&](std::size_t at) {
    return ctx.arg(at).is_integer() ? ctx.arg(at).as_integer() : 0;
  };
  const std::int32_t asked = number(1);
  const std::int32_t min_need = number(2);
  const std::int32_t max_need = number(3);
  std::int32_t avail = number(4);

  // Step 1. Read before every gate, as it is there, and used only at step 5.
  const PlayerId player = player_from_script(asked);
  EnvSystem* env = env_of(*world);
  const bool under_fire =
      env != nullptr && player != kNoPlayer && node != kNoGaika &&
      env->env().read_int(EnvScope::for_player(asked), "AssUnderFireGaika") == node;

  // Step 2.
  if (avail < min_need) return HostOutcome::ok_with(none);
  AiSystem* ai = ai_system_of(*world);
  // `active`, **not** `player_ai(...) != nullptr`: the slot exists for every
  // player in range either way, so what the original's `[record+0x88]` means
  // here is that somebody started an AI. `Squad::SendTo` -- which this entry
  // point ends up calling through `SendSquad.vs` -- gates on the same field for
  // the same reason, and would refuse every send if this did not.
  const AiPlayer* running = player == kNoPlayer || ai == nullptr ? nullptr : ai->player_ai(player);
  if (running == nullptr || !running->active) return HostOutcome::ok_with(none);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr || ctx.scheduler == nullptr) return HostOutcome::ok_with(none);

  std::vector<SquadKey> roster;
  for (const Squad& squad : heroes->squads().squads()) {
    if (squad.key.player == player) roster.push_back(squad.key);
  }
  if (roster.empty()) return HostOutcome::ok_with(none);

  // The three files, resolved once each through this player's profile.
  const std::uint32_t eval_chunk = ai->find_script(*ctx.scheduler, player, "EvalRecruit.vs");
  const std::uint32_t take_chunk = ai->find_script(*ctx.scheduler, player, "CalcMaxTake.vs");
  const std::uint32_t send_chunk = ai->find_script(*ctx.scheduler, player, "SendSquad.vs");

  /// One squad's standing. `score` doubles as the state: positive is a live
  /// candidate, 0 is refused and ends every ranked walk, and the negatives are
  /// the markers the original writes -- dropped by the budget, dropped by the
  /// routing filter, already sent.
  struct Candidate {
    SquadKey squad;
    GaikaId depend = kNoGaika;
    std::int32_t score = 0;
  };
  /// What one donor node can give up, and how much of it is spoken for.
  struct Budget {
    std::int32_t take = 0;
    std::int32_t spent = 0;
    std::int32_t overneed = 0;
    bool known = false;
  };
  std::vector<Candidate> ranked;
  ranked.reserve(roster.size());
  std::vector<Budget> budgets(static_cast<std::size_t>(world->gaika().count()) + 1);

  const SquadTable& squads = heroes->squads();
  const auto strength_of = [&](SquadKey key) {
    const Squad* squad = squads.find(key);
    return squad == nullptr ? 0 : squad->eval;
  };

  // Step 3.
  constexpr std::uint16_t kNotFielded =
      kSquadFlagNoAi | kSquadFlagPeaceful | kSquadFlagSentries;
  std::int32_t total = 0;
  std::int32_t overflow = 0;
  for (const SquadKey key : roster) {
    Candidate candidate{key, kNoGaika, 0};
    const Squad* squad = squads.find(key);
    // The flag mask, and the original's own short circuit ahead of the script:
    // a squad worth nothing is refused without asking.
    if (squad == nullptr || (squad->flags & kNotFielded) != 0 || squad->eval == 0) {
      ranked.push_back(candidate);
      continue;
    }
    const Value args[3] = {pack_squad(key), gaika_value(node), gaika_value(kNoGaika)};
    Value returned = Value::integer(0);
    std::vector<Value> locals;
    const HostOutcome ran =
        run_script_now(ctx, eval_chunk, args, "EvalRecruit failed", returned, &locals);
    if (ran.status != script::HostStatus::ok) return ran;
    const std::int32_t score = returned.is_integer() ? returned.as_integer() : 0;
    if (score <= 0) {
      ranked.push_back(candidate);
      continue;
    }
    candidate.score = score;
    // `gDepend` is the third declared local, and a frame that has finished is
    // not popped. Out of range is no dependency, which is the original's own
    // bounds check against the node count.
    const GaikaId depend = locals.size() > 2 ? gaika_of(locals[2]) : kNoGaika;
    candidate.depend = world->gaika().find(depend) == nullptr ? kNoGaika : depend;

    const std::int32_t worth = strength_of(key);
    if (candidate.depend != kNoGaika) {
      Budget& budget = budgets[static_cast<std::size_t>(candidate.depend)];
      if (!budget.known) {
        const Value take_args[3] = {Value::integer(asked), gaika_value(candidate.depend),
                                    Value::integer(0)};
        Value took = Value::integer(0);
        std::vector<Value> take_locals;
        const HostOutcome took_ran = run_script_now(ctx, take_chunk, take_args,
                                                    "CalcMaxTake failed", took, &take_locals);
        if (took_ran.status != script::HostStatus::ok) return took_ran;
        // 0x0043d470 floors a negative take at 0; unfloored it refuses the
        // same squads (a positive worth exceeds either), so the floor is not
        // written.
        budget.take = took.is_integer() ? took.as_integer() : 0;
        budget.overneed = take_locals.size() > 2 && take_locals[2].is_integer()
                              ? take_locals[2].as_integer()
                              : 0;
        budget.known = true;
      }
      // One squad heavier than the whole budget is never worth planning around.
      if (worth > budget.take) {
        candidate.score = 0;
        ranked.push_back(candidate);
        continue;
      }
      const std::int32_t left = budget.take - budget.spent > 0 ? budget.take - budget.spent : 0;
      if (worth > left) overflow += worth - left;
      budget.spent += worth;
    }
    total += worth;
    ranked.push_back(candidate);
  }

  // Step 4.
  total -= overflow;
  if (total < 0) total = 0;
  if (total < min_need) return HostOutcome::ok_with(none);

  // Step 5. Descending by score; the index is the tie-break, and the reason it
  // is here is in the header note.
  std::sort(ranked.begin(), ranked.end(), [](const Candidate& a, const Candidate& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.squad.index < b.squad.index;
  });
  if (overflow > 0) {
    for (Budget& budget : budgets) budget.spent = 0;
    total = 0;
    for (Candidate& candidate : ranked) {
      if (candidate.score <= 0) break;
      const std::int32_t worth = strength_of(candidate.squad);
      if (candidate.depend != kNoGaika) {
        Budget& budget = budgets[static_cast<std::size_t>(candidate.depend)];
        if (budget.spent + worth > budget.take) {
          candidate.score = -1;
          continue;
        }
        // A take the donor does not even need is not a dependency.
        if (budget.spent + worth <= budget.overneed) candidate.depend = kNoGaika;
        budget.spent += worth;
      }
      total += worth;
    }
    if (total < min_need) return HostOutcome::ok_with(none);
  }
  // The routing filter. See the header.
  if (!under_fire && !squads.routes().empty()) {
    const std::uint32_t check_chunk = ai->find_script(*ctx.scheduler, player, "CheckMAIKA.vs");
    // 0x0043eda0 for one source node: the first record from `src` to `node`
    // whose way-point `CheckMAIKA.vs` refuses.
    const auto way_point = [&](GaikaId src, GaikaId& way, HostOutcome& failure) {
      way = kNoGaika;
      // A copy: the script can call `SetMAIKA`, and the walk is over the list
      // as it stood (the original walks the live deque by index).
      const std::vector<SquadTable::Route> routes(squads.routes().begin(),
                                                  squads.routes().end());
      for (const SquadTable::Route& route : routes) {
        if (route.src != src || route.dst != node) continue;
        bool passable = true;
        if (check_chunk != script::kNoChunk) {
          const Value args[4] = {Value::integer(asked), gaika_value(route.src),
                                 gaika_value(route.dst), gaika_value(route.way)};
          Value answer = Value::boolean(true);
          failure = run_script_now(ctx, check_chunk, args, "Recruit: CheckMAIKA.vs failed", answer);
          if (failure.status != script::HostStatus::ok) return;
          passable = answer.truthy_scalar();
        }
        if (!passable && route.way != kNoGaika) {
          way = route.way;
          return;
        }
      }
    };
    std::vector<std::int8_t> verdict(static_cast<std::size_t>(world->gaika().count()) + 1, -1);
    std::int32_t dropped = 0;
    for (Candidate& candidate : ranked) {
      if (candidate.score == 0) break;
      if (candidate.score < 0) continue;
      const Squad* squad = squads.find(candidate.squad);
      if (squad == nullptr) continue;
      const GaikaId in = squad->gaika_in;
      if (in == kNoGaika || in == node) continue;
      if (in < 0 || static_cast<std::size_t>(in) >= verdict.size()) continue;
      std::int8_t& clear = verdict[static_cast<std::size_t>(in)];
      if (clear < 0) {
        GaikaId way = kNoGaika;
        HostOutcome failure = HostOutcome::ok_void();
        way_point(in, way, failure);
        if (failure.status != script::HostStatus::ok) return failure;
        clear = way == kNoGaika ? 1 : 0;
      }
      if (clear == 0) {
        dropped += squad->eval;
        candidate.score = -2;
      }
    }
    if (total - dropped < min_need) return HostOutcome::ok_with(none);
  }

  // Step 6, the free half.
  const auto send = [&](SquadKey key, HostOutcome& failure) {
    const Value args[2] = {pack_squad(key), gaika_value(node)};
    Value returned = Value::boolean(false);
    failure = run_script_now(ctx, send_chunk, args, "SendSquad failed", returned);
    return failure.status == script::HostStatus::ok && returned.truthy_scalar();
  };
  std::int32_t sent = 0;
  for (Candidate& candidate : ranked) {
    if (candidate.score == 0) break;
    if (candidate.score < 0 || candidate.depend != kNoGaika) continue;
    HostOutcome failure = HostOutcome::ok_void();
    if (!send(candidate.squad, failure)) {
      if (failure.status != script::HostStatus::ok) return failure;
      continue;
    }
    sent += strength_of(candidate.squad);
    candidate.score = -4;
    if (sent >= max_need) break;
  }
  // And the encumbered half, only while the minimum is still short.
  for (Candidate& candidate : ranked) {
    if (sent >= min_need) break;
    if (candidate.score == 0) break;
    if (candidate.score < 0) continue;
    HostOutcome failure = HostOutcome::ok_void();
    if (!send(candidate.squad, failure)) {
      if (failure.status != script::HostStatus::ok) return failure;
      continue;
    }
    sent += strength_of(candidate.squad);
  }

  // Step 7.
  avail -= sent;
  ctx.out(4) = Value::integer(avail < 0 ? 0 : avail);
  return HostOutcome::ok_with(Value::integer(sent));
}

std::size_t register_ai_host(script::HostRegistry& registry) {
  const std::size_t before = registry.implemented();
  constexpr script::CallKind kFree = script::CallKind::free_function;
  constexpr script::CallKind kMember = script::CallKind::member;

  registry.define(kFree, "AIGetPlayer", 0, &f_ai_get_player);
  // `AIRun` in five free arities, as the scheduler bound them before the
  // AI's launcher was this domain's; `AIBreakScript` beside it.
  for (std::uint16_t arity = 1; arity <= 5; ++arity) {
    registry.define(kFree, "AIRun", arity, &f_ai_run);  // 72 between the spellings
  }
  registry.define(kFree, "AIBreakScript", 1, &f_ai_break_script);  // 74
  registry.define(kFree, "AIStart", 3, &f_ai_start);  // 20, all in containers
  registry.define(kFree, "AIStop", 1, &f_ai_stop);     //  1, in a container, and a trap message
  // The one- and two-argument forms (0x00434e20, 0x00434e60) are **not**
  // bound: no script in the installation writes either, and the surface
  // has no entry for them to fill. `ShowNotes`'s precedent. Read, for the day
  // one is: the one-argument form checks 1..16 and calls the core with no
  // name and difficulty 0, so the new AI keeps what its constructor
  // (0x0041e100) seeds -- the seat's own playerdata profile string and its
  // difficulty, anything outside 1..3 made 3. The two-argument form is the
  // three-argument one's exact, case-sensitive profile check with difficulty
  // 0, the same "keep the seat's".

  // The AI-helper runner. `RunAIHelper` is registered **eight times** in
  // `gbr.exe` (0x004d4160, arities 2..9, every argument a string; bodies
  // 0x004d3400, 0x004d34c0, 0x004d35b0, 0x004d36d0, 0x004d3850, 0x004d3a10,
  // 0x004d3c20, 0x004d3e70), each a thunk that pops its strings, pads the
  // parameters to seven with "" and calls the one core with `arity - 2` as the
  // count. All eight are bound to the one body, as `AIRun` and `Run` bind
  // theirs: arity 4 has all 285 sites, and the others are what the original
  // registers -- with the core's signature check (0x004d2ff8) they start
  // nothing on retail data, whose four helpers all take two strings.
  for (std::uint16_t arity = 2; arity <= 9; ++arity) {
    registry.define(kFree, "RunAIHelper", arity, &f_run_ai_helper);         // 285 at 4
  }
  // -- the node table; see `sim/gaika.hpp` -----------------------------------
  registry.define(kMember, "LSA", 0, &m_gaika_lsa);              // 42, and `obj.LSA` (0)
  registry.define(kFree, "LSA", 1, &f_lsa_at);                   //  0, `LSA(pt)`
  registry.define(kFree, "LSA", 2, &f_lsa_at);                   //  0, `LSA(x, y)`
  registry.define(kMember, "GetGaika", 0, &m_settlement_gaika);  // 27
  registry.define(kMember, "Center", 0, &m_gaika_center);        // 23
  registry.define(kMember, "GetDestPoint", 1, &m_gaika_dest_point);  //  1
  registry.define(kMember, "Empty", 0, &m_gaika_empty);          //  1
  registry.define(kMember, "MilitaryPresence", 1, &m_gaika_military_presence);  // 2
  registry.define(kMember, "ControlledNeighbors", 1, &m_gaika_controlled_neighbors);  // 1
  registry.define(kMember, "Recruit", 4, &m_gaika_recruit);      //  2
  registry.define(kMember, "MinNeed", 4, &m_gaika_need<true>);   // 12
  registry.define(kMember, "MaxNeed", 4, &m_gaika_need<false>);  //  3
  registry.define(kFree, "SS_STR", 1, &f_ss_str);                //  3
  registry.define(kFree, "SetMAIKA", 3, &f_set_maika);           //  3
  registry.define(kMember, "GetDistToPlayers", 4, &m_gaika_dist_to_players);  // 3
  // Registered on `Unit` in `gbr.exe` and reached from `Hero` too; one entry
  // either way, because the registry is keyed on the name and the arity. It
  // lives in this file rather than beside the other six because the candidate
  // set is the node's -- see the note on the body, and `sim/combat.cpp` for
  // the half of it that stayed in combat.
  registry.define(kMember, "BestTargetInGAIKA", 0, &m_best_target_in_gaika);  // 70
  registry.define(kMember, "BestMDPos", 5, &m_best_md_pos);                  //  2
  registry.define(kMember, "BestProtPos", 4, &m_best_prot_pos);              //  1
  registry.define(kFree, "IsWaterLsa", 1, &f_is_water_lsa);      // 23
  registry.define(kFree, "FindTeleport", 3, &f_find_teleport);    //  5
  registry.define(kFree, "ShipNeeds", 2, &f_ship_needs);          //  1
  registry.define(kFree, "IncShipNeeds", 2, &f_inc_ship_needs);   //  2
  registry.define(kFree, "ClrShipNeeds", 2, &f_clr_ship_needs);   //  1
  registry.define(kFree, "Ships", 2, &f_ships);                   //  2
  registry.define(kFree, "CheckLsaPath", 3, &f_check_lsa_path);   //  3
  registry.define(kFree, "PrepareAiTransportShip", 5, &f_prepare_ai_transport_ship);  // 1
  // The pathfinder bootstrap, nine names over the one file that drives it.
  registry.define(kFree, "SPFFindAreas_Quant", 0, &f_spf_find_areas);          // 1
  registry.define(kFree, "SPFCalcConnectHighRes_Quant", 0, &f_spf_connect);     // 1
  registry.define(kFree, "SPFIncreaseConnect_Quant", 0, &f_spf_increase_step);  // 1
  registry.define(kFree, "SPFInitData", 0, &f_spf_init_data);                   // 1
  registry.define(kFree, "SPFInitDirectionData", 0, &f_spf_init_direction);     // 1
  registry.define(kFree, "SPFInitPointToAreaData", 0, &f_spf_init_pta);         // 1
  registry.define(kFree, "SPFInitConnectData", 0, &f_spf_init_connect);         // 1
  registry.define(kFree, "SPFIncreaseConnect", 0, &f_spf_increase);             // 1
  registry.define(kFree, "SPFDone", 0, &f_spf_done);                            // 1
  registry.define(kMember, "ApproachingSquads", 2, &m_gaika_approaching_squads);  // 1
  registry.define(kFree, "Eval", 6, &f_eval);                     //  2
  registry.define(kFree, "EnemyPlayersEval", 4, &f_enemy_players_eval);  // 1
  registry.define(kFree, "GAIKACount", 0, &f_gaika_count);       //  6
  // -- the per-player view; see `laika_of` for the lookup they share --------
  registry.define(kMember, "Explored", 1, &m_laika_flag<kLaikaExplored>);          // 10
  registry.define(kMember, "CanExplore", 1, &m_gaika_can_explore);                 //  1
  registry.define(kMember, "GetPriority", 1, &m_gaika_get_priority);               //  7
  registry.define(kFree, "LAIKA", 2, &f_laika);                                    //  5
  registry.define(kMember, "SetControlFlag", 2, &m_laika_set_flag<kLaikaControlFlag>);  // 3
  registry.define(kMember, "Prioritized", 1, &m_laika_flag<kLaikaPrioritized>);    //  3
  registry.define(kMember, "SetPriority", 2, &m_gaika_set_priority);               //  2
  registry.define(kMember, "NoAttack", 1, &m_laika_flag<kLaikaNoAttack>);          //  2
  registry.define(kMember, "Revealed", 1, &m_laika_flag<kLaikaRevealed>);          //  1
  registry.define(kMember, "NoRecruit", 1, &m_laika_flag<kLaikaNoRecruit>);        //  1
  registry.define(kMember, "LastSeen", 1, &m_gaika_last_seen);                     //  1
  registry.define(kMember, "GetControlFlag", 1, &m_laika_flag<kLaikaControlFlag>);  // 1
  // The strategy trio: one field, one script, one spawn.
  registry.define(kMember, "GetStrat", 1, &m_gaika_get_strat);       // 11
  registry.define(kMember, "StratRunning", 1, &m_gaika_strat_running);  // 5
  registry.define(kMember, "RunStrat", 2, &m_gaika_run_strat);       //  2
  registry.define(kMember, "CalcPriority", 1, &m_gaika_calc_priority);  // 1
  // The map-sequence writes, all twelve sites in one adventure's ninth
  // sequence; see `f_area_ai`.
  registry.define(kFree, "AreaAINoRecruit", 3, &f_area_ai<AreaAiWrite::no_recruit>);   // 8
  registry.define(kFree, "AreaAIMaxPriority", 3, &f_area_ai<AreaAiWrite::max_priority>);  // 2
  registry.define(kFree, "AreaAISetAttackOptimism", 3,
                  &f_area_ai<AreaAiWrite::optimism>);                                  // 2
  registry.define(kFree, "GetGaikaCenter", 1, &m_gaika_center);  //  0, the `int` spelling
  // **One body for three registrations**, and it replaces the identity that
  // used to stand here alone -- see `f_gaika_at_point` on why the integer arm
  // has to keep being the identity.
  registry.define(kFree, "GetGAIKA", 1, &f_gaika_at_point);      // 27 with `Settlement::GetGaika`
  registry.define(kFree, "GetGAIKA", 2, &f_gaika_at_xy);         //  0
  registry.define(kFree, "IsAIHelperRunning", 1, &f_is_ai_helper_running);  // 236
  registry.define(kFree, "StopAIHelper", 1, &f_stop_ai_helper);             // 179

  registry.define(kMember, "AI", 0, &m_ai);
  registry.define(kMember, "EconomyScript", 0, &m_economy_script);
  registry.define(kMember, "GetEconomyScript", 1, &m_get_economy_script);
  registry.define(kMember, "RunEconomyScript", 1, &m_run_economy_script);
  registry.define(kMember, "TacticScript", 0, &m_tactic_script);
  registry.define(kMember, "GetTacticScript", 1, &m_get_tactic_script);
  registry.define(kMember, "RunTacticScript", 1, &m_run_tactic_script);
  registry.define(kMember, "TSAdvHeroSkills", 2, &m_ts_adv_hero_skills);  // 68
  registry.define(kMember, "AIGetSkillToDevelop", 2, &m_ai_get_skill_to_develop);  // 1
  // The recruiter family, all four of them trampolines; see `recruit_into_list`.
  registry.define(kMember, "TSRecruitArmy", 2, &m_ts_recruit_army);      // 33
  registry.define(kMember, "TSRecruitHero", 0, &m_ts_recruit_hero);      // 15
  registry.define(kMember, "TSTempleRecruit", 1, &m_ts_temple_recruit);  //  8
  registry.define(kMember, "TSArenaRecruit", 1, &m_ts_arena_recruit);    //  3

  return registry.implemented() - before;
}

}  // namespace imperivm::core::sim
