// The ten systems' halves of a saved game, and the five stores they hold.
//
// Declarations: each domain's own header. Layout and rationale:
// docs/formats/save.md. The envelope these sections live in is `sim/save.hpp`.
//
// ## Why one file and not ten
//
// Every `serialize`/`deserialize` pair below is declared in its own domain's
// header, next to the state it writes, and defined here. That split is
// deliberate. **The state vector is a cross-cutting fact**: what a save
// contains is one question, versioned by one number (`kStateVectorVersion`),
// and a field added to `CombatSystem` whose section nobody updated is a bug in
// the *save*, not in combat. Keeping the ten sections in one file makes that
// bug a diff that does not touch this file, which is a thing a reviewer can
// see. Ten copies of the same encode helpers in ten domain `.cpp`s would not.
//
// ## What a section contains, in general
//
// **Turn-mutable state, and nothing that a load rebuilds anyway.**
//
// Every system here holds two kinds of member. One kind changes as the
// simulation runs: a queue, a chain, a table of records, an accumulator. The
// other kind is configuration the embedder installed before the first turn and
// never touches again: `EconomyRules`, `CombatConstants`, the command table
// merged from `DATA/COMMANDS/*.XML`, the shipped unit catalog, a borrowed
// `const ClassGraph*`. Only the first kind is written.
//
// That is not a shortcut, it is the same rule `World::serialize` already
// follows when it declines to write the class graph, and it is what makes the
// contract of a load statable: **a save is applied to a session built from the
// same game data.** `GameSession::load` says so and enforces what it can (the
// map identity, the state-vector version, the system run order). A save is not
// a portable description of a game from nothing; the original's was not either
// -- it refused a load outright when the map had changed.
//
// A pointer is never written. An address in a save is a save that only loads in
// the process that wrote it.

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/area.hpp"
#include "imperivm/core/sim/campaign.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/fog.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/feeder.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/item.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/squad.hpp"

namespace imperivm::core::sim {
namespace {

/// **One version number for all of them.**
///
/// A per-system version would let one section move without disturbing the
/// others, which sounds like a feature and is not: the sections are not
/// independent. `CommandSystem` stores a `script::ScriptId` that the scheduler's
/// section has to agree about, `AiSystem` stores another, and the economy's
/// settlement ids index the world's. A save is loaded whole or refused whole,
/// so it has one version, and `sim::kStateVectorVersion` is already that
/// number. This one exists so a section carries it in its own bytes too, the
/// way `Scheduler::serialize` and `World::serialize` do.
/// **Reverting this number is invisible to any test in this build**, and that is
/// worth knowing rather than discovering. One constant feeds both `put_header`
/// and `open_header`, so a change to it moves the writer and the reader
/// together and every round trip still passes; a fault sweep that reverts it
/// survives. What catches a *missing* bump is the file a previous build wrote,
/// which no test here has, and what catches a missing *field* is the round trip
/// itself. `save_hero_section_round_trips` pins the half that is checkable --
/// that a section carrying the previous number is refused rather than decoded
/// short -- and this comment is the other half.
constexpr std::uint32_t kSectionVersion = 28;  // 28: `Goto`'s failure stamp (`MoveState::goto_failed_at`) where the order's start time was; 27: the free-spot search's flags (`MoveState::free_spot_tried`, `free_spot_aimed`); 26: a route's gate crossings (`MoveState::gate_crossings`); 25: a route's owned destination lock (`MoveState::dest_lock`); 24: avoidance -- the step, the wait and the march on every `MoveState`, and the ownerless locks; 23: the match's eight report counters; 22: the fog's two bits a slot and the partial cells' fine records; 21: the hero's skill-point balance derived, not saved; 20: the setup's four rules on the match; 19: the command's row name and the queue's progress bar; 18: the squad watermark and the finishing commands; 17: the AI manager flag; 16: `MoveState::walking`; 15: the unit a food wagon follows; 14: the ship transport orders; 13: `Unit::AddBonus`'s five addends; 12: the commands a script has taken away

// Four-byte tags, little-endian, so a hex dump of a section names itself.
constexpr std::uint32_t kMovementMagic = 0x564F4D49u;   // "IMOV"
constexpr std::uint32_t kCombatMagic = 0x424D4349u;     // "ICMB"
constexpr std::uint32_t kEconomyMagic = 0x4F434549u;    // "IECO"
constexpr std::uint32_t kFeederMagic = 0x44454649u;     // "IFED"
constexpr std::uint32_t kHeroMagic = 0x4F524849u;       // "IHRO"
constexpr std::uint32_t kEnvMagic = 0x564E4549u;        // "IENV"
constexpr std::uint32_t kCommandMagic = 0x444D4349u;    // "ICMD"
constexpr std::uint32_t kMatchMagic = 0x48434D49u;      // "IMCH"
constexpr std::uint32_t kAiMagic = 0x53494149u;         // "IAIS"
constexpr std::uint32_t kCampaignMagic = 0x504D4349u;   // "ICMP"
constexpr std::uint32_t kSettlementsMagic = 0x54455349u;  // "ISET"
constexpr std::uint32_t kSquadsMagic = 0x44515349u;     // "ISQD"
constexpr std::uint32_t kItemsMagic = 0x4D544949u;      // "IITM"
constexpr std::uint32_t kEnvStoreMagic = 0x53564E49u;   // "INVS"
constexpr std::uint32_t kAiVarsMagic = 0x52415649u;     // "IVAR"
constexpr std::uint32_t kAreaMagic = 0x41455249u;       // "IREA"
constexpr std::uint32_t kFogMagic = 0x474f4649u;        // "IFOG"

// -- encoding ---------------------------------------------------------------
//
// Everything goes through `sim::bytes`, which `sim/save.hpp` hoisted out of
// `Scheduler::serialize` for exactly this reason: one convention, not fifteen
// private copies that drift.

void put_header(std::vector<std::byte>& out, std::uint32_t magic) {
  bytes::put_u32(out, magic);
  bytes::put_u32(out, kSectionVersion);
}

[[nodiscard]] Status open_header(ByteReader& reader, std::uint32_t magic) {
  std::uint32_t got = 0;
  std::uint32_t version = 0;
  if (!reader.u32(got) || !reader.u32(version)) return FormatError::truncated;
  if (got != magic) return FormatError::bad_magic;
  if (version != kSectionVersion) return FormatError::unsupported;
  return Status();
}

/// Trailing bytes mean the writer and this reader disagree about the layout,
/// which is the one thing a version number was supposed to have caught.
[[nodiscard]] Status finish(ByteReader& reader) {
  return reader.remaining() == 0 ? Status() : Status(FormatError::malformed);
}

void put_bool(std::vector<std::byte>& out, bool value) { bytes::put_u8(out, value ? 1u : 0u); }

[[nodiscard]] bool get_bool(ByteReader& reader, bool& out) noexcept {
  std::uint8_t raw = 0;
  if (!reader.u8(raw)) return false;
  out = raw != 0;
  return true;
}

void put_point(std::vector<std::byte>& out, const Point& p) {
  bytes::put_i32(out, p.x);
  bytes::put_i32(out, p.y);
}

[[nodiscard]] bool get_point(ByteReader& reader, Point& p) noexcept {
  return bytes::get_i32(reader, p.x) && bytes::get_i32(reader, p.y);
}

void put_ids(std::vector<std::byte>& out, std::span<const ObjectId> ids) {
  bytes::put_u32(out, static_cast<std::uint32_t>(ids.size()));
  for (const ObjectId id : ids) bytes::put_u32(out, id);
}

/// Nothing is reserved from a count before the elements are read: a hostile
/// length would otherwise ask for gigabytes ahead of the read that refuses it.
/// The same reasoning `World::deserialize` records for the object table.
[[nodiscard]] bool get_ids(ByteReader& reader, std::vector<ObjectId>& out) {
  std::uint32_t count = 0;
  if (!reader.u32(count)) return false;
  out.clear();
  for (std::uint32_t i = 0; i < count; ++i) {
    ObjectId id = 0;
    if (!reader.u32(id)) return false;
    out.push_back(id);
  }
  return true;
}

/// An enumeration arriving from a file is range-checked before it becomes one.
/// A value outside the enum would index a switch it was never written for.
template <typename Enum>
[[nodiscard]] bool get_enum(ByteReader& reader, Enum& out, std::uint8_t limit) noexcept {
  std::uint8_t raw = 0;
  if (!reader.u8(raw) || raw >= limit) return false;
  out = static_cast<Enum>(raw);
  return true;
}

/// `Action` is not contiguous -- 0, 2, 3, 8 -- so it gets a whitelist rather
/// than a bound. A gap value would be a state the switch in `act` has no arm
/// for, which reads as `idle` and is silently a different simulation.
[[nodiscard]] bool get_action(ByteReader& reader, Action& out) noexcept {
  std::uint8_t raw = 0;
  if (!reader.u8(raw)) return false;
  switch (raw) {
    case 0:
    case 2:
    case 3:
    case 8:
      out = static_cast<Action>(raw);
      return true;
    default:
      return false;
  }
}

/// Nest one store's section inside another, length-prefixed.
///
/// A store insists on consuming its whole span -- that is what makes a
/// truncated section a refusal rather than a half-read -- so it has to be
/// handed exactly its own bytes. The length prefix is how, and it also means
/// the enclosing section can grow a field after the nested store without
/// moving it.
template <typename Store>
void put_nested(std::vector<std::byte>& out, const Store& store) {
  std::vector<std::byte> payload;
  store.serialize(payload);
  bytes::put_u32(out, static_cast<std::uint32_t>(payload.size()));
  out.insert(out.end(), payload.begin(), payload.end());
}

/// Decode a nested store into `out`, which is the caller's *local* copy: the
/// enclosing `deserialize` moves it into place only once everything else has
/// read cleanly, which is what makes the whole section atomic.
template <typename Store>
[[nodiscard]] Status get_nested(ByteReader& reader, Store& out) {
  std::uint32_t length = 0;
  std::span<const std::byte> payload;
  if (!reader.u32(length) || !reader.bytes(length, payload)) {
    return FormatError::truncated;
  }
  return out.deserialize(payload);
}

}  // namespace

// ===========================================================================
// the stores
// ===========================================================================

// -- SettlementStore --------------------------------------------------------

namespace {

void put_settlement(std::vector<std::byte>& out, const Settlement& s) {
  // The map's name leads, because it is the settlement's identity and a reader
  // that has it can say which settlement a truncated record was.
  bytes::put_string(out, s.name);
  bytes::put_u32(out, s.object);
  bytes::put_u32(out, s.id);
  bytes::put_u32(out, s.anchor);
  bytes::put_u8(out, s.owner);
  bytes::put_u8(out, static_cast<std::uint32_t>(s.kind));
  put_bool(out, s.can_be_captured);
  put_bool(out, s.can_be_attacked);
  bytes::put_i32(out, s.gold_rate);
  bytes::put_i32(out, s.food_rate);
  bytes::put_i32(out, s.population);
  bytes::put_i32(out, s.max_population);

  bytes::put_u32(out, s.warehouse.object);
  bytes::put_i32(out, s.warehouse.gold);
  bytes::put_i32(out, s.warehouse.food);
  bytes::put_i32(out, s.warehouse.max_gold);
  bytes::put_i32(out, s.warehouse.max_food);

  bytes::put_u32(out, s.holder.object);
  bytes::put_i32(out, s.holder.max_units);
  put_ids(out, s.holder.units);

  bytes::put_i32(out, s.first_to_repair);
  bytes::put_i64(out, s.last_tower_fire_time);
  bytes::put_i64(out, s.last_unit_exit_time);
  bytes::put_i64(out, s.last_capture_query_time);
  bytes::put_i32(out, s.exit_interval);
  bytes::put_i32(out, s.loan);
  bytes::put_i32(out, s.loyalty);
  bytes::put_i32(out, s.loyalty_decreased);
  bytes::put_i32(out, s.efficiency);
  bytes::put_i32(out, s.food_per_pop);
  bytes::put_i32(out, s.capture_health_percent);
  bytes::put_i32(out, s.anchor_max_health);
  bytes::put_i32(out, s.sentries);
  bytes::put_i32(out, s.max_sentries);
  bytes::put_i32(out, s.sentries_ready);
  bytes::put_u32(out, s.supplied);
  bytes::put_u8(out, static_cast<std::uint32_t>(s.outpost_trade));
  put_bool(out, s.burning);

  bytes::put_u32(out, static_cast<std::uint32_t>(s.buildings.size()));
  for (const SettlementBuilding& b : s.buildings) {
    bytes::put_u32(out, b.object);
    bytes::put_i32(out, b.max_health);
  }
  // Fixed-size and written without a count: the array's length is
  // `kSettlementTimerCount`, a compile-time fact of this build, and a count in
  // the file would be a second answer to it.
  for (const std::int32_t timer : s.timers) bytes::put_i32(out, timer);
}

[[nodiscard]] bool get_settlement(ByteReader& reader, Settlement& s) {
  std::uint32_t buildings = 0;
  if (!bytes::get_string(reader, s.name) || !reader.u32(s.object) || !reader.u32(s.id) ||
      !reader.u32(s.anchor) ||
      !reader.u8(s.owner) ||
      !get_enum(reader, s.kind, static_cast<std::uint8_t>(SettlementKind::other) + 6) ||
      !get_bool(reader, s.can_be_captured) || !get_bool(reader, s.can_be_attacked) ||
      !bytes::get_i32(reader, s.gold_rate) || !bytes::get_i32(reader, s.food_rate) ||
      !bytes::get_i32(reader, s.population) || !bytes::get_i32(reader, s.max_population) ||
      !reader.u32(s.warehouse.object) || !bytes::get_i32(reader, s.warehouse.gold) ||
      !bytes::get_i32(reader, s.warehouse.food) ||
      !bytes::get_i32(reader, s.warehouse.max_gold) ||
      !bytes::get_i32(reader, s.warehouse.max_food) || !reader.u32(s.holder.object) ||
      !bytes::get_i32(reader, s.holder.max_units) || !get_ids(reader, s.holder.units) ||
      !bytes::get_i32(reader, s.first_to_repair) ||
      !bytes::get_i64(reader, s.last_tower_fire_time) ||
      !bytes::get_i64(reader, s.last_unit_exit_time) ||
      !bytes::get_i64(reader, s.last_capture_query_time) ||
      !bytes::get_i32(reader, s.exit_interval) || !bytes::get_i32(reader, s.loan) ||
      !bytes::get_i32(reader, s.loyalty) || !bytes::get_i32(reader, s.loyalty_decreased) ||
      !bytes::get_i32(reader, s.efficiency) || !bytes::get_i32(reader, s.food_per_pop) ||
      !bytes::get_i32(reader, s.capture_health_percent) ||
      !bytes::get_i32(reader, s.anchor_max_health) || !bytes::get_i32(reader, s.sentries) ||
      !bytes::get_i32(reader, s.max_sentries) ||
      !bytes::get_i32(reader, s.sentries_ready) || !reader.u32(s.supplied) ||
      !get_enum(reader, s.outpost_trade,
                static_cast<std::uint8_t>(OutpostTrade::gold_interest) + 1) ||
      !get_bool(reader, s.burning) || !reader.u32(buildings)) {
    return false;
  }
  for (std::uint32_t i = 0; i < buildings; ++i) {
    SettlementBuilding b;
    if (!reader.u32(b.object) || !bytes::get_i32(reader, b.max_health)) return false;
    s.buildings.push_back(b);
  }
  for (std::int32_t& timer : s.timers) {
    if (!bytes::get_i32(reader, timer)) return false;
  }
  return true;
}

}  // namespace

void SettlementStore::serialize(std::vector<std::byte>& out) const {
  put_header(out, kSettlementsMagic);
  bytes::put_u32(out, static_cast<std::uint32_t>(settlements_.size()));
  for (const Settlement& s : settlements_) put_settlement(out, s);
}

Status SettlementStore::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kSettlementsMagic); !status.ok()) return status;
  std::uint32_t count = 0;
  if (!reader.u32(count)) return FormatError::truncated;

  std::vector<Settlement> settlements;
  for (std::uint32_t i = 0; i < count; ++i) {
    Settlement s;
    if (!get_settlement(reader, s)) return FormatError::truncated;
    settlements.push_back(std::move(s));
  }
  if (const Status status = finish(reader); !status.ok()) return status;
  settlements_ = std::move(settlements);
  return Status();
}

// -- SquadTable -------------------------------------------------------------

void SquadTable::serialize(std::vector<std::byte>& out) const {
  put_header(out, kSquadsMagic);
  bytes::put_u32(out, static_cast<std::uint32_t>(squads_.size()));
  for (const Squad& squad : squads_) {
    bytes::put_i32(out, squad.key.index);
    bytes::put_u8(out, squad.key.player);
    bytes::put_u32(out, squad.leader);
    put_ids(out, squad.members);
    bytes::put_i32(out, squad.state);
    bytes::put_i64(out, squad.state_time);
    bytes::put_u16(out, squad.flags);
    bytes::put_i32(out, squad.src_gaika);
    bytes::put_i32(out, squad.gaika_in);
    bytes::put_i32(out, squad.dest_gaika);
    bytes::put_i32(out, squad.order_dest);
    bytes::put_i32(out, squad.ai_dest);
    bytes::put_i64(out, squad.last_fight_time);
    bytes::put_u32(out, squad.last_attacker);
    bytes::put_i32(out, squad.eval);
  }
}

Status SquadTable::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kSquadsMagic); !status.ok()) return status;
  std::uint32_t count = 0;
  if (!reader.u32(count)) return FormatError::truncated;

  std::vector<Squad> squads;
  for (std::uint32_t i = 0; i < count; ++i) {
    Squad squad;
    if (!bytes::get_i32(reader, squad.key.index) || !reader.u8(squad.key.player) ||
        !reader.u32(squad.leader) || !get_ids(reader, squad.members) ||
        !bytes::get_i32(reader, squad.state) || !bytes::get_i64(reader, squad.state_time) ||
        !reader.u16(squad.flags) || !bytes::get_i32(reader, squad.src_gaika) ||
        !bytes::get_i32(reader, squad.gaika_in) || !bytes::get_i32(reader, squad.dest_gaika) ||
        !bytes::get_i32(reader, squad.order_dest) || !bytes::get_i32(reader, squad.ai_dest) ||
        !bytes::get_i64(reader, squad.last_fight_time) ||
        !reader.u32(squad.last_attacker) || !bytes::get_i32(reader, squad.eval)) {
      return FormatError::truncated;
    }
    squads.push_back(std::move(squad));
  }
  if (const Status status = finish(reader); !status.ok()) return status;
  squads_ = std::move(squads);
  return Status();
}

// -- ItemStore --------------------------------------------------------------

void ItemStore::serialize(std::vector<std::byte>& out) const {
  put_header(out, kItemsMagic);
  bytes::put_u32(out, static_cast<std::uint32_t>(items_.size()));
  for (const ItemInstance& item : items_) {
    bytes::put_u32(out, item.id);
    bytes::put_u32(out, item.type);
    bytes::put_u32(out, item.owner);
    bytes::put_i32(out, item.usecount);
    bytes::put_i32(out, item.customdata);
  }
}

Status ItemStore::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kItemsMagic); !status.ok()) return status;
  std::uint32_t count = 0;
  if (!reader.u32(count)) return FormatError::truncated;

  std::vector<ItemInstance> items;
  ObjectId previous = kNoObject;
  for (std::uint32_t i = 0; i < count; ++i) {
    ItemInstance item;
    if (!reader.u32(item.id) || !reader.u32(item.type) || !reader.u32(item.owner) ||
        !bytes::get_i32(reader, item.usecount) || !bytes::get_i32(reader, item.customdata)) {
      return FormatError::truncated;
    }
    // Sorted by id, which `lower_bound` binary-searches on. A table out of
    // order would make every lookup wrong in a way nothing later would notice.
    if (i != 0 && item.id <= previous) return FormatError::malformed;
    previous = item.id;
    items.push_back(item);
  }
  if (const Status status = finish(reader); !status.ok()) return status;
  items_ = std::move(items);
  // `catalog_` is left alone: it is game data, borrowed, and the same on both
  // sides of a load.
  return Status();
}

// -- EnvStore ---------------------------------------------------------------

void EnvStore::serialize(std::vector<std::byte>& out) const {
  put_header(out, kEnvStoreMagic);
  bytes::put_u32(out, static_cast<std::uint32_t>(entries_.size()));
  for (const EnvEntry& entry : entries_) {
    bytes::put_u8(out, static_cast<std::uint32_t>(entry.scope.kind));
    bytes::put_u32(out, entry.scope.id);
    bytes::put_string(out, entry.key);
    bytes::put_string(out, entry.value);
  }
}

Status EnvStore::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kEnvStoreMagic); !status.ok()) return status;
  std::uint32_t count = 0;
  if (!reader.u32(count)) return FormatError::truncated;

  std::vector<EnvEntry> entries;
  for (std::uint32_t i = 0; i < count; ++i) {
    EnvEntry entry;
    if (!get_enum(reader, entry.scope.kind,
                  static_cast<std::uint8_t>(EnvScopeKind::building) + 1) ||
        !reader.u32(entry.scope.id) || !bytes::get_string(reader, entry.key) ||
        !bytes::get_string(reader, entry.value)) {
      return FormatError::truncated;
    }
    entries.push_back(std::move(entry));
  }
  if (const Status status = finish(reader); !status.ok()) return status;
  entries_ = std::move(entries);
  return Status();
}

// -- AiVarStore -------------------------------------------------------------

void AiVarStore::serialize(std::vector<std::byte>& out) const {
  put_header(out, kAiVarsMagic);
  bytes::put_u32(out, static_cast<std::uint32_t>(players_.size()));
  for (const PlayerVars& vars : players_) {
    bytes::put_i32(out, vars.player);
    // The whole dense vector, trailing zeros included. `hash` folds all of it,
    // so its *length* is state: two stores with the same non-zero variables and
    // different lengths hash differently.
    bytes::put_u32(out, static_cast<std::uint32_t>(vars.values.size()));
    for (const std::int32_t value : vars.values) bytes::put_i32(out, value);
  }
}

Status AiVarStore::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kAiVarsMagic); !status.ok()) return status;
  std::uint32_t count = 0;
  if (!reader.u32(count)) return FormatError::truncated;

  std::vector<PlayerVars> players;
  for (std::uint32_t i = 0; i < count; ++i) {
    PlayerVars vars;
    std::uint32_t values = 0;
    if (!bytes::get_i32(reader, vars.player) || !reader.u32(values)) {
      return FormatError::truncated;
    }
    // `find` binary-searches on ascending `player`, and `set` maintains it.
    if (i != 0 && vars.player <= players.back().player) return FormatError::malformed;
    if (values > static_cast<std::uint32_t>(kMaxVar)) return FormatError::out_of_range;
    for (std::uint32_t v = 0; v < values; ++v) {
      std::int32_t value = 0;
      if (!bytes::get_i32(reader, value)) return FormatError::truncated;
      vars.values.push_back(value);
    }
    players.push_back(std::move(vars));
  }
  if (const Status status = finish(reader); !status.ok()) return status;
  players_ = std::move(players);
  return Status();
}

// ===========================================================================
// the systems, in `kSystemOrder`
// ===========================================================================

// -- movement ---------------------------------------------------------------

void MovementSystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kMovementMagic);
  bytes::put_u32(out, grid_generation_);
  bytes::put_u32(out, static_cast<std::uint32_t>(states_.size()));
  for (const Entry& entry : states_) {
    const MoveState& m = entry.state;
    bytes::put_u32(out, entry.id);
    put_point(out, m.facing);
    bytes::put_i32(out, m.speed);
    bytes::put_i32(out, m.speed_factor);
    bytes::put_i32(out, m.walk_anim);
    bytes::put_i32(out, m.formation_radius);
    bytes::put_string(out, m.formation);
    put_point(out, m.target);
    bytes::put_u32(out, m.target_object);
    bytes::put_i32(out, m.range);
    bytes::put_i32(out, m.min_range);
    put_bool(out, m.has_path);
    put_bool(out, m.goto_active);
    // `walking` is the memory that makes the walk cycle a transition rather
    // than a poll (`play_locomotion`). Left out, a loaded session replayed
    // every moving unit's walk from step zero on its first turn, and the
    // round-trip diverged in the object hash the moment entities were loaded
    // headless -- which is how it stayed invisible for as long as they were not.
    put_bool(out, m.walking);
    bytes::put_i64(out, m.progress);
    bytes::put_i64(out, m.last_moved);
    // `Goto`'s failure stamp, `[unit+0x150]`: the give-up and the re-search
    // draw read it, so a unit loaded mid-failure would otherwise give up late
    // and draw where the original does not.
    bytes::put_i64(out, m.goto_failed_at);
    // The route. Not hashed -- `pathfinder` is zero in all nine dumps -- and
    // written all the same, because a route is not recomputable: the grid it
    // was laid against can have moved under it, and a unit that came back with
    // an empty path would silently stop where it stood.
    bytes::put_u32(out, static_cast<std::uint32_t>(m.waypoints.size()));
    for (const Point& p : m.waypoints) put_point(out, p);
    bytes::put_i64(out, m.path_length);
    bytes::put_u32(out, m.path_generation);
    put_bool(out, m.path_complete);
    bytes::put_u8(out, static_cast<std::uint32_t>(m.last_outcome));
    // Avoidance: the step being walked, the wait and the march. Not hashed --
    // path media, the channel the original leaves at zero -- and written all
    // the same, for the route's reason: a unit that came back mid-hold without
    // its hold would step early, and one that came back without its sidestep
    // would jump sideways by the offset it was walking out of.
    bytes::put_i32(out, m.stride);
    put_bool(out, m.holding);
    bytes::put_i64(out, m.hold_until);
    bytes::put_i64(out, m.step_start);
    bytes::put_i64(out, m.step_end);
    put_point(out, m.offset_from);
    put_point(out, m.offset_to);
    bytes::put_i32(out, m.retry_time);
    bytes::put_u32(out, m.party);
    // The order's lock flag: the route's last point is this unit's owned
    // destination lock, which other units' free-spot tests read.
    put_bool(out, m.dest_lock);
    // The free-spot search's two flags: whether it has run for this
    // destination, and whether the route was re-aimed by it. Either keeps it
    // from running again, and it draws from the world's generator when it runs.
    put_bool(out, m.free_spot_tried);
    put_bool(out, m.free_spot_aimed);
    // The gates the route crosses, listed when it was laid: path media like
    // the route, and not recomputable from it -- a gate spawned since would
    // join a list the original never rebuilds.
    bytes::put_u32(out, static_cast<std::uint32_t>(m.gate_crossings.size()));
    for (const GateCrossing& crossing : m.gate_crossings) {
      bytes::put_u32(out, crossing.gate);
      bytes::put_i64(out, crossing.at);
    }
  }
  // The ownerless locks. The original keeps them as objects and its save
  // carries them as objects; rebuilding them at load would put back the ones a
  // map made and forget that a match's start made them once and for all.
  put_bool(out, static_locks_built_);
  bytes::put_u32(out, static_cast<std::uint32_t>(static_locks_.size()));
  for (const StaticLock& lock : static_locks_) {
    put_point(out, lock.at);
    bytes::put_i32(out, lock.radius);
  }
}

Status MovementSystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kMovementMagic); !status.ok()) return status;
  std::uint32_t generation = 0;
  std::uint32_t count = 0;
  if (!reader.u32(generation) || !reader.u32(count)) return FormatError::truncated;

  std::vector<Entry> states;
  for (std::uint32_t i = 0; i < count; ++i) {
    Entry entry;
    MoveState& m = entry.state;
    std::uint32_t waypoints = 0;
    if (!reader.u32(entry.id) || !get_point(reader, m.facing) ||
        !bytes::get_i32(reader, m.speed) || !bytes::get_i32(reader, m.speed_factor) ||
        !bytes::get_i32(reader, m.walk_anim) || !bytes::get_i32(reader, m.formation_radius) ||
        !bytes::get_string(reader, m.formation) || !get_point(reader, m.target) ||
        !reader.u32(m.target_object) || !bytes::get_i32(reader, m.range) ||
        !bytes::get_i32(reader, m.min_range) || !get_bool(reader, m.has_path) ||
        !get_bool(reader, m.goto_active) || !get_bool(reader, m.walking) ||
        !bytes::get_i64(reader, m.progress) ||
        !bytes::get_i64(reader, m.last_moved) || !bytes::get_i64(reader, m.goto_failed_at) ||
        !reader.u32(waypoints)) {
      return FormatError::truncated;
    }
    for (std::uint32_t w = 0; w < waypoints; ++w) {
      Point p;
      if (!get_point(reader, p)) return FormatError::truncated;
      m.waypoints.push_back(p);
    }
    if (!bytes::get_i64(reader, m.path_length) || !reader.u32(m.path_generation) ||
        !get_bool(reader, m.path_complete) ||
        !get_enum(reader, m.last_outcome,
                  static_cast<std::uint8_t>(MoveOutcome::exhausted) + 1)) {
      return FormatError::truncated;
    }
    if (!bytes::get_i32(reader, m.stride) ||
        !get_bool(reader, m.holding) || !bytes::get_i64(reader, m.hold_until) ||
        !bytes::get_i64(reader, m.step_start) || !bytes::get_i64(reader, m.step_end) ||
        !get_point(reader, m.offset_from) || !get_point(reader, m.offset_to) ||
        !bytes::get_i32(reader, m.retry_time) || !reader.u32(m.party) ||
        !get_bool(reader, m.dest_lock) || !get_bool(reader, m.free_spot_tried) ||
        !get_bool(reader, m.free_spot_aimed)) {
      return FormatError::truncated;
    }
    std::uint32_t crossings = 0;
    if (!reader.u32(crossings)) return FormatError::truncated;
    for (std::uint32_t c = 0; c < crossings; ++c) {
      GateCrossing crossing;
      if (!reader.u32(crossing.gate) || !bytes::get_i64(reader, crossing.at)) {
        return FormatError::truncated;
      }
      // Route order: the step reads the first one not yet passed.
      if (c != 0 && crossing.at < m.gate_crossings.back().at) return FormatError::malformed;
      m.gate_crossings.push_back(crossing);
    }
    // Sorted by id: `lower_bound` binary-searches on it and `advance` walks it
    // in that order, which is world state.
    if (i != 0 && entry.id <= states.back().id) return FormatError::malformed;
    states.push_back(std::move(entry));
  }
  bool built = false;
  std::uint32_t lock_count = 0;
  if (!get_bool(reader, built) || !reader.u32(lock_count)) return FormatError::truncated;
  std::vector<StaticLock> locks;
  for (std::uint32_t i = 0; i < lock_count; ++i) {
    StaticLock lock;
    if (!get_point(reader, lock.at) || !bytes::get_i32(reader, lock.radius)) {
      return FormatError::truncated;
    }
    locks.push_back(lock);
  }
  if (const Status status = finish(reader); !status.ok()) return status;
  states_ = std::move(states);
  grid_generation_ = generation;
  static_locks_ = std::move(locks);
  static_locks_built_ = built;
  lock_buckets_valid_ = false;
  return Status();
}

// -- combat -----------------------------------------------------------------

void CombatSystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kCombatMagic);
  bytes::put_i64(out, now_);
  bytes::put_i64(out, death_duration_);
  put_bool(out, world_bound_);

  bytes::put_u32(out, static_cast<std::uint32_t>(units_.size()));
  for (const Combatant& u : units_) {
    bytes::put_u32(out, u.id);
    bytes::put_u32(out, u.class_index);
    bytes::put_u8(out, u.owner);
    put_point(out, u.position);
    bytes::put_i32(out, u.health);
    bytes::put_i32(out, u.stamina);
    bytes::put_i32(out, u.experience);
    bytes::put_i32(out, u.level);
    bytes::put_i64(out, u.last_attack_time);
    bytes::put_u32(out, u.target);
    bytes::put_i32(out, u.attacks);
    bytes::put_u32(out, u.unit_flags);
    bytes::put_u8(out, static_cast<std::uint32_t>(u.action));
    bytes::put_i32(out, u.anim);
    bytes::put_i64(out, u.death_time);
    bytes::put_i64(out, u.next_action_time);
    bytes::put_i32(out, u.attack_bonus);
    bytes::put_i32(out, u.armour_bonus);
    bytes::put_i32(out, u.level_floor);
    bytes::put_i32(out, u.damage_multiplier_percent);
    // `Unit::AddBonus`'s record. Written rather than derived: nothing outside
    // the entry point produces it, so a load that dropped it would hand back a
    // unit at its class numbers and no reader could tell it had ever moved.
    bytes::put_i32(out, u.bonus.attack);
    bytes::put_i32(out, u.bonus.armour_slash);
    bytes::put_i32(out, u.bonus.armour_pierce);
    bytes::put_i32(out, u.bonus.max_health);
    bytes::put_i32(out, u.bonus.max_stamina);
    put_bool(out, u.ignores_armour);
    put_bool(out, u.cannot_fight);
    put_bool(out, u.in_settlement);
    put_bool(out, u.alive);
  }

  bytes::put_u32(out, static_cast<std::uint32_t>(shots_.size()));
  for (const Projectile& s : shots_) {
    bytes::put_u32(out, s.id);
    bytes::put_u32(out, s.shooter);
    bytes::put_u32(out, s.target);
    bytes::put_u32(out, s.class_index);
    put_point(out, s.origin);
    bytes::put_i64(out, s.launched);
    bytes::put_i64(out, s.impact);
    bytes::put_i32(out, s.attack);
    bytes::put_u8(out, static_cast<std::uint32_t>(s.type));
    bytes::put_i32(out, s.attacker_level);
    bytes::put_i32(out, s.splash_radius);
    put_bool(out, s.ignores_armour);
  }

  // Not a log: a caller drains this with `clear_events`, so an undrained event
  // is work still owed and a load that dropped it would owe less.
  bytes::put_u32(out, static_cast<std::uint32_t>(events_.size()));
  for (const CombatEvent& e : events_) {
    bytes::put_u8(out, static_cast<std::uint32_t>(e.kind));
    bytes::put_i64(out, e.time);
    bytes::put_u32(out, e.attacker);
    bytes::put_u32(out, e.defender);
    bytes::put_i32(out, e.damage);
    bytes::put_i32(out, e.remaining_health);
  }

  bytes::put_u32(out, static_cast<std::uint32_t>(level_addends_.size()));
  for (const auto& [player, addend] : level_addends_) {
    bytes::put_u8(out, player);
    bytes::put_i32(out, addend);
  }
  bytes::put_u32(out, static_cast<std::uint32_t>(allies_.size()));
  for (const auto& [a, b] : allies_) {
    bytes::put_u8(out, a);
    bytes::put_u8(out, b);
  }
  // Turn state, unlike the two above it: research writes this mid-game.
  bytes::put_u32(out, static_cast<std::uint32_t>(experience_modifiers_.size()));
  for (const auto& [player, percent] : experience_modifiers_) {
    bytes::put_u8(out, player);
    bytes::put_i32(out, percent);
  }
}

Status CombatSystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kCombatMagic); !status.ok()) return status;

  GameTime now = 0;
  GameTime death = 0;
  bool bound = false;
  std::uint32_t count = 0;
  if (!bytes::get_i64(reader, now) || !bytes::get_i64(reader, death) ||
      !get_bool(reader, bound) || !reader.u32(count)) {
    return FormatError::truncated;
  }
  if (death <= 0) return FormatError::malformed;  // `set_death_duration` forbids it

  std::vector<Combatant> units;
  for (std::uint32_t i = 0; i < count; ++i) {
    Combatant u;
    if (!reader.u32(u.id) || !reader.u32(u.class_index) || !reader.u8(u.owner) ||
        !get_point(reader, u.position) || !bytes::get_i32(reader, u.health) ||
        !bytes::get_i32(reader, u.stamina) || !bytes::get_i32(reader, u.experience) ||
        !bytes::get_i32(reader, u.level) || !bytes::get_i64(reader, u.last_attack_time) ||
        !reader.u32(u.target) || !bytes::get_i32(reader, u.attacks) ||
        !reader.u32(u.unit_flags) || !get_action(reader, u.action) ||
        !bytes::get_i32(reader, u.anim) || !bytes::get_i64(reader, u.death_time) ||
        !bytes::get_i64(reader, u.next_action_time) ||
        !bytes::get_i32(reader, u.attack_bonus) || !bytes::get_i32(reader, u.armour_bonus) ||
        !bytes::get_i32(reader, u.level_floor) ||
        !bytes::get_i32(reader, u.damage_multiplier_percent) ||
        !bytes::get_i32(reader, u.bonus.attack) ||
        !bytes::get_i32(reader, u.bonus.armour_slash) ||
        !bytes::get_i32(reader, u.bonus.armour_pierce) ||
        !bytes::get_i32(reader, u.bonus.max_health) ||
        !bytes::get_i32(reader, u.bonus.max_stamina) ||
        !get_bool(reader, u.ignores_armour) || !get_bool(reader, u.cannot_fight) ||
        !get_bool(reader, u.in_settlement) || !get_bool(reader, u.alive)) {
      return FormatError::truncated;
    }
    units.push_back(u);
  }

  std::uint32_t shots = 0;
  if (!reader.u32(shots)) return FormatError::truncated;
  std::vector<Projectile> projectiles;
  for (std::uint32_t i = 0; i < shots; ++i) {
    Projectile s;
    if (!reader.u32(s.id) || !reader.u32(s.shooter) || !reader.u32(s.target) ||
        !reader.u32(s.class_index) || !get_point(reader, s.origin) ||
        !bytes::get_i64(reader, s.launched) || !bytes::get_i64(reader, s.impact) ||
        !bytes::get_i32(reader, s.attack) ||
        !get_enum(reader, s.type, static_cast<std::uint8_t>(DamageType::siege) + 1) ||
        !bytes::get_i32(reader, s.attacker_level) ||
        !bytes::get_i32(reader, s.splash_radius) || !get_bool(reader, s.ignores_armour)) {
      return FormatError::truncated;
    }
    projectiles.push_back(s);
  }

  std::uint32_t events = 0;
  if (!reader.u32(events)) return FormatError::truncated;
  std::vector<CombatEvent> log;
  for (std::uint32_t i = 0; i < events; ++i) {
    CombatEvent e;
    if (!get_enum(reader, e.kind, static_cast<std::uint8_t>(CombatEvent::Kind::death) + 1) ||
        !bytes::get_i64(reader, e.time) || !reader.u32(e.attacker) ||
        !reader.u32(e.defender) || !bytes::get_i32(reader, e.damage) ||
        !bytes::get_i32(reader, e.remaining_health)) {
      return FormatError::truncated;
    }
    log.push_back(e);
  }

  std::uint32_t addends = 0;
  if (!reader.u32(addends)) return FormatError::truncated;
  std::vector<std::pair<PlayerId, std::int32_t>> level_addends;
  for (std::uint32_t i = 0; i < addends; ++i) {
    PlayerId player = kNoPlayer;
    std::int32_t addend = 0;
    if (!reader.u8(player) || !bytes::get_i32(reader, addend)) return FormatError::truncated;
    level_addends.emplace_back(player, addend);
  }

  std::uint32_t pairs = 0;
  if (!reader.u32(pairs)) return FormatError::truncated;
  std::vector<std::pair<PlayerId, PlayerId>> allies;
  for (std::uint32_t i = 0; i < pairs; ++i) {
    PlayerId a = kNoPlayer;
    PlayerId b = kNoPlayer;
    if (!reader.u8(a) || !reader.u8(b)) return FormatError::truncated;
    allies.emplace_back(a, b);
  }

  std::uint32_t modifiers = 0;
  if (!reader.u32(modifiers)) return FormatError::truncated;
  std::vector<std::pair<PlayerId, std::int32_t>> experience;
  for (std::uint32_t i = 0; i < modifiers; ++i) {
    PlayerId player = kNoPlayer;
    std::int32_t percent = 0;
    if (!reader.u8(player) || !bytes::get_i32(reader, percent)) return FormatError::truncated;
    experience.emplace_back(player, percent);
  }

  if (const Status status = finish(reader); !status.ok()) return status;

  now_ = now;
  death_duration_ = death;
  world_bound_ = bound;
  units_ = std::move(units);
  shots_ = std::move(projectiles);
  events_ = std::move(log);
  level_addends_ = std::move(level_addends);
  allies_ = std::move(allies);
  experience_modifiers_ = std::move(experience);
  return Status();
}


// -- economy ----------------------------------------------------------------

void EconomySystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kEconomyMagic);
  put_bool(out, started_);
  bytes::put_u32(out, next_wagon_);
  put_nested(out, store_);

  bytes::put_u32(out, static_cast<std::uint32_t>(wagons_.size()));
  for (const Wagon& w : wagons_) {
    bytes::put_u32(out, w.id);
    bytes::put_u32(out, w.source);
    bytes::put_u32(out, w.destination);
    bytes::put_u8(out, static_cast<std::uint32_t>(w.resource));
    bytes::put_i32(out, w.amount);
    bytes::put_i32(out, w.build_remaining);
    bytes::put_u32(out, w.object);
    bytes::put_u32(out, w.follow);
  }

  // One row per settlement, in settlement-id order, which is what
  // `record_gold_converted` indexes. Written whole rather than sparsely: a
  // statistics row is small and a sparse encoding would need a second key.
  bytes::put_u32(out, static_cast<std::uint32_t>(stats_.size()));
  for (const Statistics& s : stats_) bytes::put_i32(out, s.gold_converted);
}

Status EconomySystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kEconomyMagic); !status.ok()) return status;

  bool started = false;
  std::uint32_t next_wagon = 0;
  if (!get_bool(reader, started) || !reader.u32(next_wagon)) return FormatError::truncated;

  SettlementStore store;
  if (const Status status = get_nested(reader, store); !status.ok()) return status;

  std::uint32_t count = 0;
  if (!reader.u32(count)) return FormatError::truncated;
  std::vector<Wagon> wagons;
  for (std::uint32_t i = 0; i < count; ++i) {
    Wagon w;
    if (!reader.u32(w.id) || !reader.u32(w.source) || !reader.u32(w.destination) ||
        !get_enum(reader, w.resource, static_cast<std::uint8_t>(Resource::food) + 1) ||
        !bytes::get_i32(reader, w.amount) || !bytes::get_i32(reader, w.build_remaining) ||
        !reader.u32(w.object) || !reader.u32(w.follow)) {
      return FormatError::truncated;
    }
    wagons.push_back(w);
  }

  std::uint32_t rows = 0;
  if (!reader.u32(rows)) return FormatError::truncated;
  std::vector<Statistics> stats;
  for (std::uint32_t i = 0; i < rows; ++i) {
    Statistics s;
    if (!bytes::get_i32(reader, s.gold_converted)) return FormatError::truncated;
    stats.push_back(s);
  }
  if (const Status status = finish(reader); !status.ok()) return status;

  started_ = started;
  next_wagon_ = next_wagon;
  store_ = std::move(store);
  wagons_ = std::move(wagons);
  stats_ = std::move(stats);
  return Status();
}

// -- feeder -----------------------------------------------------------------

void FeederSystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kFeederMagic);
  // These six line up name for name with what `gbr.exe`'s persist stream
  // registers under `Feeder`: `unit_chain`, `total_unit_count`, `next_tick`,
  // `ch_quant`, `cf_quant`, `curr_quant`.
  bytes::put_i32(out, next_tick_);
  bytes::put_i64(out, food_quant_);
  bytes::put_i64(out, health_quant_);
  bytes::put_u32(out, food_cursor_);
  bytes::put_u32(out, health_cursor_);

  bytes::put_u32(out, static_cast<std::uint32_t>(chain_.size()));
  for (const FeedingUnit& link : chain_) {
    bytes::put_u32(out, link.unit);
    bytes::put_i32(out, link.food);
    bytes::put_i32(out, link.max_food);
    bytes::put_i32(out, link.max_health);
    put_bool(out, link.feeds);
    bytes::put_i64(out, link.search_due);
    bytes::put_i64(out, link.hungry_since);
  }
  put_ids(out, hungry_);
}

Status FeederSystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kFeederMagic); !status.ok()) return status;

  std::int32_t next_tick = 0;
  std::int64_t food_quant = 0;
  std::int64_t health_quant = 0;
  ObjectId food_cursor = 0;
  ObjectId health_cursor = 0;
  std::uint32_t count = 0;
  if (!bytes::get_i32(reader, next_tick) || !bytes::get_i64(reader, food_quant) ||
      !bytes::get_i64(reader, health_quant) || !reader.u32(food_cursor) ||
      !reader.u32(health_cursor) || !reader.u32(count)) {
    return FormatError::truncated;
  }

  std::vector<FeedingUnit> chain;
  for (std::uint32_t i = 0; i < count; ++i) {
    FeedingUnit link;
    if (!reader.u32(link.unit) || !bytes::get_i32(reader, link.food) ||
        !bytes::get_i32(reader, link.max_food) || !bytes::get_i32(reader, link.max_health) ||
        !get_bool(reader, link.feeds) || !bytes::get_i64(reader, link.search_due) ||
        !bytes::get_i64(reader, link.hungry_since)) {
      return FormatError::truncated;
    }
    chain.push_back(link);
  }
  std::vector<ObjectId> hungry;
  if (!get_ids(reader, hungry)) return FormatError::truncated;
  if (const Status status = finish(reader); !status.ok()) return status;

  next_tick_ = next_tick;
  food_quant_ = food_quant;
  health_quant_ = health_quant;
  food_cursor_ = food_cursor;
  health_cursor_ = health_cursor;
  chain_ = std::move(chain);
  hungry_ = std::move(hungry);
  return Status();
}

// -- hero -------------------------------------------------------------------

void HeroSystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kHeroMagic);
  bytes::put_i64(out, now_);

  bytes::put_u32(out, static_cast<std::uint32_t>(units_.size()));
  for (const UnitRecord& r : units_) {
    bytes::put_u32(out, r.id);
    bytes::put_u32(out, r.hero);
    bytes::put_i32(out, r.squad.index);
    bytes::put_u8(out, r.squad.player);
    bytes::put_i32(out, r.experience);
    bytes::put_i32(out, r.inherent_level);
    put_bool(out, r.has_freedom);
    bytes::put_i32(out, r.kills);
  }

  bytes::put_u32(out, static_cast<std::uint32_t>(heroes_.size()));
  for (const HeroRecord& r : heroes_) {
    bytes::put_u32(out, r.id);
    bytes::put_i32(out, r.squad.index);
    bytes::put_u8(out, r.squad.player);
    put_ids(out, r.army);
    // Section version 21: the skill-point balance is derived, not saved.
    bytes::put_i32(out, r.max_army);
    // The three per-skill arrays are `kHeroSkillCount` long by construction,
    // which is a compile-time fact of this build; a count in the file would be
    // a second answer to it, and `kStateVectorVersion` is what moves if the
    // skill list ever does.
    for (const std::int32_t points : r.skills) bytes::put_i32(out, points);
    for (const bool offered : r.offered) put_bool(out, offered);
    for (const GameTime until : r.effect_until) bytes::put_i64(out, until);
    bytes::put_i64(out, r.army_attacked_at);
    bytes::put_u32(out, r.army_attacked_unit);
    bytes::put_i32(out, r.party_orientation_request.x);
    bytes::put_i32(out, r.party_orientation_request.y);
    bytes::put_i32(out, r.party_orientation.x);
    bytes::put_i32(out, r.party_orientation.y);
  }

  put_nested(out, squads_);
  put_nested(out, items_);
  // The highest id `enrol_new_units_in_squads` has looked at. Not saved for
  // a while: a loaded match started the scan over from zero, and a unit that
  // had left its squad since was filed into a fresh one on the first turn
  // after the load and nowhere else -- the save sweep's first divergence on
  // a map where units train.
  bytes::put_u32(out, squad_watermark_);
}

Status HeroSystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kHeroMagic); !status.ok()) return status;
  GameTime now = 0;
  std::uint32_t count = 0;
  if (!bytes::get_i64(reader, now) || !reader.u32(count)) return FormatError::truncated;

  std::vector<UnitRecord> units;
  for (std::uint32_t i = 0; i < count; ++i) {
    UnitRecord r;
    if (!reader.u32(r.id) || !reader.u32(r.hero) || !bytes::get_i32(reader, r.squad.index) ||
        !reader.u8(r.squad.player) || !bytes::get_i32(reader, r.experience) ||
        !bytes::get_i32(reader, r.inherent_level) || !get_bool(reader, r.has_freedom) ||
        !bytes::get_i32(reader, r.kills)) {
      return FormatError::truncated;
    }
    // Sorted by id: `unit_slot` binary-searches on it.
    if (i != 0 && r.id <= units.back().id) return FormatError::malformed;
    units.push_back(r);
  }

  std::uint32_t hero_count = 0;
  if (!reader.u32(hero_count)) return FormatError::truncated;
  std::vector<HeroRecord> heroes;
  for (std::uint32_t i = 0; i < hero_count; ++i) {
    HeroRecord r;
    if (!reader.u32(r.id) || !bytes::get_i32(reader, r.squad.index) ||
        !reader.u8(r.squad.player) || !get_ids(reader, r.army) ||
        !bytes::get_i32(reader, r.max_army)) {
      return FormatError::truncated;
    }
    for (std::int32_t& points : r.skills) {
      if (!bytes::get_i32(reader, points)) return FormatError::truncated;
    }
    // `std::array<bool, N>` has no `bool&` to bind in a range-for that would
    // survive `get_bool`, so the index is spelled out.
    for (std::size_t s = 0; s < kHeroSkillCount; ++s) {
      bool offered = false;
      if (!get_bool(reader, offered)) return FormatError::truncated;
      r.offered[s] = offered;
    }
    for (GameTime& until : r.effect_until) {
      if (!bytes::get_i64(reader, until)) return FormatError::truncated;
    }
    if (!bytes::get_i64(reader, r.army_attacked_at) ||
        !reader.u32(r.army_attacked_unit) ||
        !bytes::get_i32(reader, r.party_orientation_request.x) ||
        !bytes::get_i32(reader, r.party_orientation_request.y) ||
        !bytes::get_i32(reader, r.party_orientation.x) ||
        !bytes::get_i32(reader, r.party_orientation.y)) {
      return FormatError::truncated;
    }
    if (i != 0 && r.id <= heroes.back().id) return FormatError::malformed;
    heroes.push_back(std::move(r));
  }

  SquadTable squads;
  if (const Status status = get_nested(reader, squads); !status.ok()) return status;
  ItemStore items;
  // The catalog is game data and is borrowed; carried across so the move below
  // does not drop it.
  items.set_catalog(items_.catalog());
  if (const Status status = get_nested(reader, items); !status.ok()) return status;
  ObjectId watermark = 0;
  if (!reader.u32(watermark)) return FormatError::truncated;
  if (const Status status = finish(reader); !status.ok()) return status;

  now_ = now;
  units_ = std::move(units);
  heroes_ = std::move(heroes);
  squads_ = std::move(squads);
  items_ = std::move(items);
  squad_watermark_ = watermark;
  return Status();
}

// -- env --------------------------------------------------------------------

void EnvSystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kEnvMagic);
  put_nested(out, env_);
  put_nested(out, ai_vars_);
}

Status EnvSystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kEnvMagic); !status.ok()) return status;
  EnvStore env;
  if (const Status status = get_nested(reader, env); !status.ok()) return status;
  AiVarStore ai_vars;
  if (const Status status = get_nested(reader, ai_vars); !status.ok()) return status;
  if (const Status status = finish(reader); !status.ok()) return status;
  env_ = std::move(env);
  ai_vars_ = std::move(ai_vars);
  return Status();
}

// -- command ----------------------------------------------------------------
//
// **The section whose absence is silent.** `CommandSystem` has no `hash`
// override, so nothing in `sim::verify_hashes` notices a load that dropped the
// queues; the game simply stops doing what it was in the middle of. That is why
// `imsave` checks section bytes as well as hashes, and why `test_save.cpp`
// injects the drop and requires it to be caught.

namespace {

/// `less_fold` from `sim/command.cpp`, which is in that file's anonymous
/// namespace and is one comparison. Duplicated rather than exported: the
/// alternative is a public ordering on `CommandTable` that only the loader
/// would use, and the check it serves is a validity check on a file.
[[nodiscard]] bool less_fold_name(std::string_view a, std::string_view b) noexcept {
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
    const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
    if (x != y) return x < y;
  }
  return a.size() < b.size();
}

}  // namespace

namespace {

void put_command(std::vector<std::byte>& out, const Command& c) {
  bytes::put_u32(out, c.id);
  bytes::put_string(out, c.verb);
  bytes::put_u8(out, static_cast<std::uint32_t>(c.arg_kind));
  put_point(out, c.point);
  bytes::put_u32(out, c.object);
  bytes::put_string(out, c.param);
  bytes::put_string(out, c.name);
  bytes::put_i32(out, c.cost_gold);
  bytes::put_i32(out, c.cost_food);
  bytes::put_i32(out, c.cost_pop);
  bytes::put_i32(out, c.cost_stamina);
  bytes::put_i32(out, c.delay);
  put_bool(out, c.user);
  bytes::put_u32(out, c.script);
  put_bool(out, c.started);
  bytes::put_i64(out, c.started_at);
}

[[nodiscard]] bool get_command(ByteReader& reader, Command& command) {
  return reader.u32(command.id) && bytes::get_string(reader, command.verb) &&
         get_enum(reader, command.arg_kind, static_cast<std::uint8_t>(CommandArgKind::object) + 1) &&
         get_point(reader, command.point) && reader.u32(command.object) &&
         bytes::get_string(reader, command.param) && bytes::get_string(reader, command.name) &&
         bytes::get_i32(reader, command.cost_gold) &&
         bytes::get_i32(reader, command.cost_food) && bytes::get_i32(reader, command.cost_pop) &&
         bytes::get_i32(reader, command.cost_stamina) && bytes::get_i32(reader, command.delay) &&
         get_bool(reader, command.user) && reader.u32(command.script) &&
         get_bool(reader, command.started) && bytes::get_i64(reader, command.started_at);
}

}  // namespace

void CommandSystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kCommandMagic);
  bytes::put_string(out, default_verb_);
  bytes::put_u32(out, static_cast<std::uint32_t>(queues_.size()));
  for (const Entry& entry : queues_) {
    bytes::put_u32(out, entry.id);
    bytes::put_i64(out, entry.queue.progress_start);
    bytes::put_i64(out, entry.queue.progress_end);
    bytes::put_u32(out, static_cast<std::uint32_t>(entry.queue.entries.size()));
    for (const Command& c : entry.queue.entries) put_command(out, c);
  }
  bytes::put_u32(out, static_cast<std::uint32_t>(disabled_.size()));
  for (const DisabledEntry& entry : disabled_) {
    bytes::put_u32(out, entry.id);
    bytes::put_u32(out, static_cast<std::uint32_t>(entry.names.size()));
    for (const std::string& name : entry.names) bytes::put_string(out, name);
  }
  // The commands whose `onfinish` is running, so that a loaded `cmdparam`
  // inside one still answers. See `CommandSystem::finish_command`.
  bytes::put_u32(out, static_cast<std::uint32_t>(finishing_.size()));
  for (const Command& c : finishing_) put_command(out, c);
}

Status CommandSystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kCommandMagic); !status.ok()) return status;
  std::string default_verb;
  std::uint32_t count = 0;
  if (!bytes::get_string(reader, default_verb) || !reader.u32(count)) {
    return FormatError::truncated;
  }

  std::vector<Entry> queues;
  for (std::uint32_t i = 0; i < count; ++i) {
    Entry entry;
    std::uint32_t commands = 0;
    if (!reader.u32(entry.id) || !bytes::get_i64(reader, entry.queue.progress_start) ||
        !bytes::get_i64(reader, entry.queue.progress_end) || !reader.u32(commands)) {
      return FormatError::truncated;
    }
    for (std::uint32_t c = 0; c < commands; ++c) {
      Command command;
      if (!get_command(reader, command)) return FormatError::truncated;
      entry.queue.entries.push_back(std::move(command));
    }
    // Sorted by id: `lower_bound` binary-searches on it and `advance` walks the
    // vector in that order, which is world state.
    if (i != 0 && entry.id <= queues.back().id) return FormatError::malformed;
    queues.push_back(std::move(entry));
  }
  std::uint32_t disabled_count = 0;
  if (!reader.u32(disabled_count)) return FormatError::truncated;
  std::vector<DisabledEntry> disabled;
  for (std::uint32_t i = 0; i < disabled_count; ++i) {
    DisabledEntry entry;
    std::uint32_t names = 0;
    if (!reader.u32(entry.id) || !reader.u32(names)) return FormatError::truncated;
    for (std::uint32_t n = 0; n < names; ++n) {
      std::string name;
      if (!bytes::get_string(reader, name)) return FormatError::truncated;
      // Sorted, for the reason the ids below are: this is iteration order, and
      // a save that arrived unsorted would answer `command_enabled` correctly
      // and re-save differently.
      if (n != 0 && !less_fold_name(entry.names.back(), name)) return FormatError::malformed;
      entry.names.push_back(std::move(name));
    }
    // An empty set is never written, so one in the file is a malformed file
    // rather than an object with nothing taken from it.
    if (entry.names.empty()) return FormatError::malformed;
    if (i != 0 && entry.id <= disabled.back().id) return FormatError::malformed;
    disabled.push_back(std::move(entry));
  }
  std::uint32_t finishing_count = 0;
  if (!reader.u32(finishing_count)) return FormatError::truncated;
  std::vector<Command> finishing;
  for (std::uint32_t i = 0; i < finishing_count; ++i) {
    Command command;
    if (!get_command(reader, command)) return FormatError::truncated;
    finishing.push_back(std::move(command));
  }
  if (const Status status = finish(reader); !status.ok()) return status;

  default_verb_ = std::move(default_verb);
  queues_ = std::move(queues);
  disabled_ = std::move(disabled);
  finishing_ = std::move(finishing);
  // `method_cache_` is a memo of the class graph and is left to refill itself.
  // It is `mutable` precisely because it is not state.
  method_cache_.clear();
  return Status();
}

// -- match ------------------------------------------------------------------

void MatchSystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kMatchMagic);
  bytes::put_u8(out, static_cast<std::uint32_t>(rules_.condition));
  bytes::put_string(out, rules_.param);
  bytes::put_u8(out, rules_.start_player);
  put_bool(out, rules_.single_only);
  bytes::put_string(out, rules_.map_name);
  bytes::put_i32(out, rules_.map_size);
  // Section version 20: the setup's rules. The population and gold have
  // already been applied to the settlements by the time a save is taken and
  // are here for `MatchRules` to read back whole; the fog flag is the one a
  // renderer would ask for.
  bytes::put_i32(out, rules_.world_population);
  bytes::put_i32(out, rules_.starting_gold);
  put_bool(out, rules_.fog_of_war);
  put_bool(out, rules_.exploration);

  bytes::put_u8(out, human_);
  put_bool(out, multiplayer_);
  // Section version 3. `GetDifficulty`'s 136 readers multiply it into levels,
  // counts and timers, so a game reloaded at the wrong difficulty diverges on
  // the next spawn rather than at the next comparison.
  bytes::put_i32(out, difficulty_);
  // Display text: `EndGame`'s three-argument form assigns it only for the local
  // player, so it is deliberately not hashed. Saved anyway -- a game reloaded
  // after its victory condition fired should still be able to say why.
  bytes::put_string(out, end_message_);

  // Fixed at `kPlayerCount`, written without a count for the same reason the
  // hero skill arrays are.
  for (const MatchPlayer& p : players_) {
    bytes::put_u8(out, static_cast<std::uint32_t>(p.control));
    bytes::put_i32(out, p.race);
    put_bool(out, p.participates);
    bytes::put_u8(out, static_cast<std::uint32_t>(p.outcome));
    // Section version 5. All five counters, `gold` included: nothing maintains
    // it yet, and leaving a field out of the section because it is always zero
    // is how a save format acquires a hole the day something fills it.
    bytes::put_i32(out, p.score.gold);
    bytes::put_i32(out, p.score.damage_taken);
    bytes::put_i32(out, p.score.damage_inflicted);
    bytes::put_i32(out, p.score.kill_healths);
    bytes::put_i32(out, p.score.die_healths);
    // Section version 23: the report's eight counters, saved and not hashed
    // (`PlayerScoreCounters`), in the record's own order.
    bytes::put_i32(out, p.score.food);
    bytes::put_i32(out, p.score.units_produced);
    bytes::put_i32(out, p.score.units_killed);
    bytes::put_i32(out, p.score.units_lost);
    bytes::put_i32(out, p.score.units_max);
    bytes::put_i32(out, p.score.gold_captured);
    bytes::put_i32(out, p.score.gold_townhall);
    bytes::put_i32(out, p.score.gold_outpost);
  }
}

Status MatchSystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kMatchMagic); !status.ok()) return status;

  MatchRules rules;
  PlayerId human = kNoPlayer;
  bool multiplayer = false;
  std::int32_t difficulty = 0;
  std::string message;
  if (!get_enum(reader, rules.condition,
                static_cast<std::uint8_t>(VictoryCondition::time_limit_score) + 1) ||
      !bytes::get_string(reader, rules.param) || !reader.u8(rules.start_player) ||
      !get_bool(reader, rules.single_only) || !bytes::get_string(reader, rules.map_name) ||
      !bytes::get_i32(reader, rules.map_size) ||
      !bytes::get_i32(reader, rules.world_population) ||
      !bytes::get_i32(reader, rules.starting_gold) || !get_bool(reader, rules.fog_of_war) ||
      !get_bool(reader, rules.exploration) || !reader.u8(human) ||
      !get_bool(reader, multiplayer) || !bytes::get_i32(reader, difficulty) ||
      !bytes::get_string(reader, message)) {
    return FormatError::truncated;
  }

  std::array<MatchPlayer, kPlayerCount> players{};
  for (MatchPlayer& p : players) {
    if (!get_enum(reader, p.control, static_cast<std::uint8_t>(PlayerControl::both) + 1) ||
        !bytes::get_i32(reader, p.race) || !get_bool(reader, p.participates) ||
        !get_enum(reader, p.outcome, static_cast<std::uint8_t>(MatchOutcome::lost) + 1) ||
        !bytes::get_i32(reader, p.score.gold) ||
        !bytes::get_i32(reader, p.score.damage_taken) ||
        !bytes::get_i32(reader, p.score.damage_inflicted) ||
        !bytes::get_i32(reader, p.score.kill_healths) ||
        !bytes::get_i32(reader, p.score.die_healths) || !bytes::get_i32(reader, p.score.food) ||
        !bytes::get_i32(reader, p.score.units_produced) || !bytes::get_i32(reader, p.score.units_killed) ||
        !bytes::get_i32(reader, p.score.units_lost) || !bytes::get_i32(reader, p.score.units_max) ||
        !bytes::get_i32(reader, p.score.gold_captured) || !bytes::get_i32(reader, p.score.gold_townhall) ||
        !bytes::get_i32(reader, p.score.gold_outpost)) {
      return FormatError::truncated;
    }
  }
  if (const Status status = finish(reader); !status.ok()) return status;

  rules_ = std::move(rules);
  human_ = human;
  multiplayer_ = multiplayer;
  difficulty_ = difficulty;
  end_message_ = std::move(message);
  players_ = players;
  return Status();
}

// -- ai ---------------------------------------------------------------------
//
// **The other section whose absence is silent.** `AiSystem::hash` is a no-op.
// A load that dropped this passes `verify_hashes` and then diverges once
// `AIGetPlayer` starts answering -1 for scripts that used to have an owner.

void AiSystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kAiMagic);

  bytes::put_u32(out, static_cast<std::uint32_t>(players_.size()));
  for (const AiPlayer& p : players_) {
    bytes::put_u8(out, p.player);
    put_bool(out, p.active);
    bytes::put_string(out, p.profile);
    bytes::put_u8(out, static_cast<std::uint32_t>(p.difficulty));
    bytes::put_u32(out, p.root);
    // The player's node view: the slot map and then the records, in rank
    // order. **Both**, rather than the records alone with the map rebuilt --
    // which is what the original's `CVXAI::Persist` does, writing `GAIKAMap`
    // and `LAIKAs` as two arrays, and it is the shape to copy because the
    // permutation is the behaviour.
    const std::span<const std::int32_t> slots = p.gaika.slots();
    bytes::put_u32(out, static_cast<std::uint32_t>(slots.size()));
    for (const std::int32_t slot : slots) bytes::put_i32(out, slot);
    const std::span<const Laika> records = p.gaika.records();
    bytes::put_u32(out, static_cast<std::uint32_t>(records.size()));
    for (const Laika& r : records) {
      bytes::put_i32(out, r.gaika);
      bytes::put_u32(out, r.flags);
      bytes::put_i32(out, r.strat);
      bytes::put_u32(out, r.strat_script);
      bytes::put_i32(out, r.enemies);
      bytes::put_i64(out, r.last_seen);
      bytes::put_i32(out, r.priority);
      bytes::put_i32(out, r.optimism);
    }
  }

  bytes::put_u32(out, static_cast<std::uint32_t>(owners_.size()));
  for (const ScriptOwner& o : owners_) {
    bytes::put_u32(out, o.script);
    bytes::put_u8(out, o.player);
    put_bool(out, o.root);
  }

  bytes::put_u32(out, static_cast<std::uint32_t>(settlements_.size()));
  for (const AiSettlementScripts& s : settlements_) {
    bytes::put_u32(out, s.settlement);
    bytes::put_i32(out, s.economy);
    bytes::put_u32(out, s.economy_script);
    bytes::put_i32(out, s.tactic);
    bytes::put_u32(out, s.tactic_script);
  }

  bytes::put_u32(out, since_prune_);

  // The AI-helper table, ascending by name -- which is `std::map`'s order and
  // therefore the order `helpers()` iterates in. `AiSystem::hash` is a no-op
  // (`aihash` is zero in all nine dumps), so nothing else would notice this
  // section going missing until `IsAIHelperRunning` started answering false
  // for a helper that is plainly still running.
  bytes::put_u32(out, static_cast<std::uint32_t>(helpers_.size()));
  for (const AiHelperEntry& h : helpers_) {
    bytes::put_string(out, h.name);
    bytes::put_u32(out, h.script);
  }

  // The ship-needs rows, ascending by `(player, lsa)`. `Ships` is a census
  // and needs nothing here; the three counters are what a script wrote.
  bytes::put_u32(out, static_cast<std::uint32_t>(ship_needs_.size()));
  for (const ShipNeed& n : ship_needs_) {
    bytes::put_u8(out, n.player);
    bytes::put_i32(out, n.lsa);
    bytes::put_i32(out, n.count);
  }

  // The ship transport orders, ascending by ship id. Saved and **not hashed**,
  // for the reason `sim/ai.hpp` gives beside the table: the original keeps the
  // pair on the ship, the dumps print no such field, and `aihash` is zero in
  // all nine of them.
  bytes::put_u32(out, static_cast<std::uint32_t>(ship_transports_.size()));
  for (const ShipTransport& t : ship_transports_) {
    bytes::put_u32(out, t.ship);
    bytes::put_string(out, t.order);
    bytes::put_i32(out, t.where.x);
    bytes::put_i32(out, t.where.y);
  }
  // Whether the manager exists; see `AiSystem::manager_started`.
  put_bool(out, manager_started_);
}

Status AiSystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kAiMagic); !status.ok()) return status;

  std::uint32_t count = 0;
  if (!reader.u32(count)) return FormatError::truncated;
  // `player_ai` indexes this vector by `PlayerId`, so a short table would be an
  // out-of-range read rather than a missing player.
  if (count != kPlayerCount) return FormatError::unsupported;
  std::vector<AiPlayer> players;
  for (std::uint32_t i = 0; i < count; ++i) {
    AiPlayer p;
    if (!reader.u8(p.player) || !get_bool(reader, p.active) ||
        !bytes::get_string(reader, p.profile) ||
        !get_enum(reader, p.difficulty, static_cast<std::uint8_t>(AiDifficulty::hard) + 1) ||
        !reader.u32(p.root)) {
      return FormatError::truncated;
    }
    std::uint32_t slot_count = 0;
    if (!reader.u32(slot_count)) return FormatError::truncated;
    std::vector<std::int32_t> slots(slot_count);
    for (std::int32_t& slot : slots) {
      if (!bytes::get_i32(reader, slot)) return FormatError::truncated;
    }
    std::uint32_t record_count = 0;
    if (!reader.u32(record_count)) return FormatError::truncated;
    std::vector<Laika> records(record_count);
    for (Laika& r : records) {
      std::uint32_t flags = 0;
      if (!bytes::get_i32(reader, r.gaika) || !reader.u32(flags) ||
          !bytes::get_i32(reader, r.strat) || !reader.u32(r.strat_script) ||
          !bytes::get_i32(reader, r.enemies) || !bytes::get_i64(reader, r.last_seen) ||
          !bytes::get_i32(reader, r.priority) || !bytes::get_i32(reader, r.optimism)) {
        return FormatError::truncated;
      }
      r.flags = static_cast<std::uint16_t>(flags);
    }
    // `adopt` checks the two agree: `records[map[g]].gaika == g` for every
    // node. A file whose permutation and map disagreed would make every
    // per-node accessor answer about a different node, silently.
    if (!p.gaika.adopt(std::move(slots), std::move(records))) return FormatError::malformed;
    players.push_back(std::move(p));
  }

  std::uint32_t owner_count = 0;
  if (!reader.u32(owner_count)) return FormatError::truncated;
  std::vector<ScriptOwner> owners;
  for (std::uint32_t i = 0; i < owner_count; ++i) {
    ScriptOwner o;
    if (!reader.u32(o.script) || !reader.u8(o.player) || !get_bool(reader, o.root)) {
      return FormatError::truncated;
    }
    // Sorted by script id, which `owner` binary-searches on.
    if (i != 0 && o.script <= owners.back().script) return FormatError::malformed;
    owners.push_back(o);
  }

  std::uint32_t settlement_count = 0;
  if (!reader.u32(settlement_count)) return FormatError::truncated;
  std::vector<AiSettlementScripts> settlements;
  for (std::uint32_t i = 0; i < settlement_count; ++i) {
    AiSettlementScripts s;
    if (!reader.u32(s.settlement) || !bytes::get_i32(reader, s.economy) ||
        !reader.u32(s.economy_script) || !bytes::get_i32(reader, s.tactic) ||
        !reader.u32(s.tactic_script)) {
      return FormatError::truncated;
    }
    if (i != 0 && s.settlement <= settlements.back().settlement) return FormatError::malformed;
    settlements.push_back(s);
  }

  std::uint32_t since_prune = 0;
  if (!reader.u32(since_prune)) return FormatError::truncated;

  std::uint32_t helper_count = 0;
  if (!reader.u32(helper_count)) return FormatError::truncated;
  std::vector<AiHelperEntry> helpers;
  for (std::uint32_t i = 0; i < helper_count; ++i) {
    AiHelperEntry h;
    if (!bytes::get_string(reader, h.name) || !reader.u32(h.script)) {
      return FormatError::truncated;
    }
    // Strictly ascending, which `helper` binary-searches on. A table out of
    // order would make every lookup wrong in a way nothing later would notice.
    if (i != 0 && !(helpers.back().name < h.name)) return FormatError::malformed;
    helpers.push_back(std::move(h));
  }

  std::uint32_t need_count = 0;
  if (!reader.u32(need_count)) return FormatError::truncated;
  std::vector<ShipNeed> needs;
  for (std::uint32_t i = 0; i < need_count; ++i) {
    ShipNeed n;
    if (!reader.u8(n.player) || !bytes::get_i32(reader, n.lsa) || !bytes::get_i32(reader, n.count)) {
      return FormatError::truncated;
    }
    needs.push_back(n);
  }

  std::uint32_t transport_count = 0;
  if (!reader.u32(transport_count)) return FormatError::truncated;
  std::vector<ShipTransport> transports;
  for (std::uint32_t i = 0; i < transport_count; ++i) {
    ShipTransport t;
    if (!reader.u32(t.ship) || !bytes::get_string(reader, t.order) ||
        !bytes::get_i32(reader, t.where.x) || !bytes::get_i32(reader, t.where.y)) {
      return FormatError::truncated;
    }
    transports.push_back(std::move(t));
  }
  bool manager_started = false;
  if (!get_bool(reader, manager_started)) return FormatError::truncated;

  if (const Status status = finish(reader); !status.ok()) return status;
  if (!adopt_ship_needs(std::move(needs))) return FormatError::malformed;
  if (!adopt_ship_transports(std::move(transports))) return FormatError::malformed;

  players_ = std::move(players);
  owners_ = std::move(owners);
  settlements_ = std::move(settlements);
  since_prune_ = since_prune;
  manager_started_ = manager_started;
  helpers_ = std::move(helpers);
  // `profiles_` is left alone: its entries hold borrowed `const AiProfile*`,
  // which is game data and an address, and whoever built this system installed
  // them before the first turn.
  return Status();
}

// -- areas ------------------------------------------------------------------
//
// Load-time data, and written anyway. See the declaration in `sim/area.hpp`.

// -- fog --------------------------------------------------------------------
//
// The cells as the original packs them -- one `uint16` a cell, two bits a
// slot -- and then the partial cells' fine records, in slot order and cell
// order, each 32 x 32 nibbles packed two to a byte. The map is **not hashed**
// -- `exploration` is one of the four channels the shipped build leaves at
// zero -- and is saved anyway, for `WorldObject::sight`'s reason: rebuilding it
// on load would make a save's meaning depend on where every unit happened to
// be standing.

void FogSystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kFogMagic);
  const std::span<const std::uint16_t> cells = map_.raw();
  bytes::put_i32(out, map_.cells());
  bytes::put_u32(out, static_cast<std::uint32_t>(cells.size()));
  for (const std::uint16_t cell : cells) bytes::put_u16(out, cell);
  const std::vector<ExplorationMap::FineEntry> records = map_.fine_entries();
  bytes::put_u32(out, static_cast<std::uint32_t>(records.size()));
  for (const ExplorationMap::FineEntry& entry : records) {
    bytes::put_u8(out, static_cast<std::uint32_t>(entry.slot));
    bytes::put_i32(out, entry.index);
    const auto& values = entry.record->values;
    for (std::size_t i = 0; i < values.size(); i += 2) {
      bytes::put_u8(out, static_cast<std::uint32_t>(values[i] | (values[i + 1] << 4)));
    }
  }
}

Status FogSystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kFogMagic); !status.ok()) return status;
  std::int32_t side = 0;
  std::uint32_t count = 0;
  if (!bytes::get_i32(reader, side) || !reader.u32(count)) return FormatError::truncated;
  if (side < 0) return FormatError::malformed;
  // The grid is square, so the count is a function of the side and a file that
  // disagrees is a file this writer could not have produced.
  if (static_cast<std::uint64_t>(count) !=
      static_cast<std::uint64_t>(side) * static_cast<std::uint64_t>(side)) {
    return FormatError::malformed;
  }
  std::vector<std::uint16_t> cells;
  cells.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint16_t cell = 0;
    if (!reader.u16(cell)) return FormatError::truncated;
    cells.push_back(cell);
  }
  std::uint32_t record_count = 0;
  if (!reader.u32(record_count)) return FormatError::truncated;
  // At most one record per cell per slot, so a count past that is malformed
  // rather than a reason to reserve.
  if (static_cast<std::uint64_t>(record_count) >
      static_cast<std::uint64_t>(count) * ExplorationMap::kSlots) {
    return FormatError::malformed;
  }
  std::vector<ExplorationMap::FineRecord> records;
  std::vector<ExplorationMap::FineEntry> entries;
  records.reserve(record_count);
  entries.reserve(record_count);
  for (std::uint32_t r = 0; r < record_count; ++r) {
    std::uint8_t slot = 0;
    std::int32_t index = 0;
    if (!reader.u8(slot) || !bytes::get_i32(reader, index)) return FormatError::truncated;
    ExplorationMap::FineRecord record;
    for (std::size_t i = 0; i < record.values.size(); i += 2) {
      std::uint8_t packed = 0;
      if (!reader.u8(packed)) return FormatError::truncated;
      record.values[i] = static_cast<std::uint8_t>(packed & 0x0F);
      record.values[i + 1] = static_cast<std::uint8_t>(packed >> 4);
    }
    records.push_back(record);
    entries.push_back(ExplorationMap::FineEntry{slot, index, nullptr});
  }
  for (std::size_t i = 0; i < entries.size(); ++i) entries[i].record = &records[i];
  if (const Status status = finish(reader); !status.ok()) return status;
  // `set_raw` refuses a record on a cell that is not partial, and a partial
  // cell with no record; either is a file this writer could not have produced.
  if (!map_.set_raw(side, cells, entries)) return FormatError::malformed;
  stamped_.clear();
  return Status();
}

void AreaSystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kAreaMagic);
  const std::span<const AreaTable::Entry> entries = areas_.entries();
  bytes::put_u32(out, static_cast<std::uint32_t>(entries.size()));
  for (const AreaTable::Entry& entry : entries) {
    bytes::put_u32(out, entry.id);
    bytes::put_u8(out, static_cast<std::uint32_t>(entry.shape.kind));
    put_point(out, entry.shape.center);
    bytes::put_i32(out, entry.shape.radius);
    bytes::put_i32(out, entry.shape.left);
    bytes::put_i32(out, entry.shape.top);
    bytes::put_i32(out, entry.shape.right);
    bytes::put_i32(out, entry.shape.bottom);
  }
}

Status AreaSystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kAreaMagic); !status.ok()) return status;
  std::uint32_t count = 0;
  if (!reader.u32(count)) return FormatError::truncated;

  // Rebuilt through the table's own `bind`, which is the whole of its mutating
  // interface, so this needs no reach into `AreaTable` and the ascending-id
  // invariant `lower_bound` depends on is re-established by the same code that
  // establishes it at load.
  AreaTable table;
  ObjectId previous = kNoObject;
  for (std::uint32_t i = 0; i < count; ++i) {
    ObjectId id = 0;
    AreaShape shape;
    if (!reader.u32(id) ||
        !get_enum(reader, shape.kind, static_cast<std::uint8_t>(AreaKind::circle) + 1) ||
        !get_point(reader, shape.center) || !bytes::get_i32(reader, shape.radius) ||
        !bytes::get_i32(reader, shape.left) || !bytes::get_i32(reader, shape.top) ||
        !bytes::get_i32(reader, shape.right) || !bytes::get_i32(reader, shape.bottom)) {
      return FormatError::truncated;
    }
    if (i != 0 && id <= previous) return FormatError::malformed;
    previous = id;
    if (!table.bind(id, shape)) return FormatError::malformed;
  }
  if (const Status status = finish(reader); !status.ok()) return status;
  areas_ = std::move(table);
  return Status();
}

// -- campaign ---------------------------------------------------------------

void CampaignSystem::serialize(std::vector<std::byte>& out) const {
  put_header(out, kCampaignMagic);
  bytes::put_u32(out, static_cast<std::uint32_t>(progress_.states.size()));
  for (const TerritoryState state : progress_.states) {
    bytes::put_i32(out, static_cast<std::int32_t>(state));
  }
  bytes::put_u32(out, static_cast<std::uint32_t>(progress_.conquered.size()));
  for (const std::int32_t territory : progress_.conquered) bytes::put_i32(out, territory);
  bytes::put_string(out, progress_.active_bonus);
  // The note board, as its own self-describing block with its own magic and
  // version -- the shape `ObjListPool` uses inside `World`'s section. The
  // catalogue is not written: it is configuration, rebuilt from the
  // container's `Notes.xml` documents, exactly like `ids_` and `sequences_`.
  std::vector<std::byte> notes;
  board_.serialize(notes);
  bytes::put_u32(out, static_cast<std::uint32_t>(notes.size()));
  out.insert(out.end(), notes.begin(), notes.end());
  // The conversation results, the same way and for the same reason. The
  // catalogue is not written either: it is the container's `.conv.xml`
  // documents and is rebuilt on both sides of a load.
  std::vector<std::byte> results;
  results_.serialize(results);
  bytes::put_u32(out, static_cast<std::uint32_t>(results.size()));
  out.insert(out.end(), results.begin(), results.end());
}

Status CampaignSystem::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  if (const Status status = open_header(reader, kCampaignMagic); !status.ok()) return status;

  CampaignProgress progress;
  std::uint32_t states = 0;
  if (!reader.u32(states)) return FormatError::truncated;
  for (std::uint32_t i = 0; i < states; ++i) {
    std::int32_t raw = 0;
    if (!bytes::get_i32(reader, raw)) return FormatError::truncated;
    if (raw < static_cast<std::int32_t>(TerritoryState::enemy) ||
        raw > static_cast<std::int32_t>(TerritoryState::disabled)) {
      return FormatError::malformed;
    }
    progress.states.push_back(static_cast<TerritoryState>(raw));
  }
  std::uint32_t conquered = 0;
  if (!reader.u32(conquered)) return FormatError::truncated;
  for (std::uint32_t i = 0; i < conquered; ++i) {
    std::int32_t territory = 0;
    if (!bytes::get_i32(reader, territory)) return FormatError::truncated;
    progress.conquered.push_back(territory);
  }
  if (!bytes::get_string(reader, progress.active_bonus)) return FormatError::truncated;

  std::uint32_t note_bytes = 0;
  std::span<const std::byte> notes;
  if (!reader.u32(note_bytes) || !reader.bytes(note_bytes, notes)) {
    return FormatError::truncated;
  }
  NoteBoard board;
  if (const Status status = board.deserialize(notes); !status.ok()) return status;

  std::uint32_t result_bytes = 0;
  std::span<const std::byte> results;
  if (!reader.u32(result_bytes) || !reader.bytes(result_bytes, results)) {
    return FormatError::truncated;
  }
  ConversationResults conversation_results;
  if (const Status status = conversation_results.deserialize(results); !status.ok()) {
    return status;
  }

  if (const Status status = finish(reader); !status.ok()) return status;

  // Through `restore`, not by assignment: it is the existing check that the
  // progress belongs to the conquest this system was configured with, and a
  // save from a different one is caught here rather than at the first
  // `GetTerritoryState`.
  const Status restored = restore(progress);
  if (!restored.ok()) return restored;
  board_ = std::move(board);
  results_ = std::move(conversation_results);
  return Status();
}

}  // namespace imperivm::core::sim
