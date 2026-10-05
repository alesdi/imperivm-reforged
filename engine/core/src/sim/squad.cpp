#include "imperivm/core/sim/ai.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/squad.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/array.hpp"
#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/heading.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/gaika.hpp"
#include "imperivm/core/sim/gaika_table.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/player.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

/// FNV-1a, the same mixer `World::state_hash` uses, so that a system's channel
/// and the slot channel are folded the same way.
constexpr std::uint64_t kFnvPrime = 0x100000001b3ull;

void fold(std::uint64_t& h, std::uint64_t value) noexcept {
  for (int byte = 0; byte < 8; ++byte) {
    h ^= static_cast<std::uint64_t>((value >> (byte * 8)) & 0xFF);
    h *= kFnvPrime;
  }
}

}  // namespace

bool Squad::contains(ObjectId id) const noexcept {
  return std::find(members.begin(), members.end(), id) != members.end();
}

std::size_t SquadTable::lower_bound(SquadKey key) const noexcept {
  const auto it = std::lower_bound(squads_.begin(), squads_.end(), key,
                                   [](const Squad& s, const SquadKey& k) {
                                     return squad_key_less(s.key, k);
                                   });
  return static_cast<std::size_t>(it - squads_.begin());
}

std::int32_t SquadTable::next_free_index(PlayerId player) const {
  // The table is sorted by (player, index), so the player's squads are one
  // contiguous run and the first gap in it is the answer.
  std::int32_t candidate = 1;
  for (const Squad& squad : squads_) {
    if (squad.key.player != player) continue;
    if (squad.key.index > candidate) break;
    if (squad.key.index == candidate) ++candidate;
  }
  return candidate;
}

SquadKey SquadTable::create(PlayerId player, ObjectId leader) {
  SquadKey key{next_free_index(player), player};
  create_at(key, leader);
  return key;
}

bool SquadTable::create_at(SquadKey key, ObjectId leader) {
  if (!key.valid()) return false;
  const std::size_t at = lower_bound(key);
  if (at < squads_.size() && squads_[at].key == key) return false;

  Squad squad;
  squad.key = key;
  squad.leader = leader;
  if (leader != kNoObject) squad.members.push_back(leader);
  squads_.insert(squads_.begin() + static_cast<std::ptrdiff_t>(at), std::move(squad));
  return true;
}

bool SquadTable::join(SquadKey key, ObjectId id) {
  Squad* squad = find(key);
  if (squad == nullptr || id == kNoObject) return false;
  if (squad->contains(id)) return true;
  squad->members.push_back(id);
  return true;
}

bool SquadTable::leave(SquadKey key, ObjectId id) {
  Squad* squad = find(key);
  if (squad == nullptr) return false;
  const auto it = std::find(squad->members.begin(), squad->members.end(), id);
  if (it == squad->members.end()) return false;
  squad->members.erase(it);
  // The dumps show no promotion on a leader's departure, and `DetachArmy`
  // disbands rather than reassigning, so a leaderless squad is a real state.
  if (squad->leader == id) squad->leader = kNoObject;
  return true;
}

bool SquadTable::destroy(SquadKey key) {
  const std::size_t at = lower_bound(key);
  if (at >= squads_.size() || !(squads_[at].key == key)) return false;
  squads_.erase(squads_.begin() + static_cast<std::ptrdiff_t>(at));
  return true;
}

const Squad* SquadTable::find(SquadKey key) const {
  if (!key.valid()) return nullptr;
  const std::size_t at = lower_bound(key);
  if (at >= squads_.size() || !(squads_[at].key == key)) return nullptr;
  return &squads_[at];
}

Squad* SquadTable::find(SquadKey key) {
  return const_cast<Squad*>(static_cast<const SquadTable*>(this)->find(key));
}

SquadKey SquadTable::squad_of(ObjectId id) const {
  if (id == kNoObject) return kNoSquad;
  for (const Squad& squad : squads_) {
    if (squad.contains(id)) return squad.key;
  }
  return kNoSquad;
}

void SquadTable::prune_empty() {
  squads_.erase(std::remove_if(squads_.begin(), squads_.end(),
                               [](const Squad& s) { return s.members.empty(); }),
                squads_.end());
}

void SquadTable::hash(std::uint64_t& accumulator) const noexcept {
  for (const Squad& squad : squads_) {
    fold(accumulator, static_cast<std::uint64_t>(static_cast<std::uint32_t>(squad.key.index)));
    fold(accumulator, squad.key.player);
    fold(accumulator, squad.leader);
    for (const ObjectId member : squad.members) fold(accumulator, member);
  }
}

// See `sim/squad.hpp` for the two chains this reproduces and for what running
// them once a turn gives up.
void revalue_squads(World& world, SquadTable& squads) {
  const CombatSystem* combat = combat_system_of(world);
  const GaikaTable& nodes = world.gaika();
  const LsaPartition& areas = world.lsa();
  for (Squad& squad : squads.mutable_squads()) {
    std::int32_t worth = 0;
    for (const ObjectId member : squad.members) {
      const WorldObject* slot = world.find(member);
      if (slot == nullptr) continue;
      worth += object_power(world, combat, *slot);
    }
    squad.eval = worth;

    // 0x00443df0 answers the member deque's **front**, which is `Squad::leader`
    // whenever there is one and the next member along when a leader has left
    // without being replaced. That is the unit 0x0041f530 compares the mover
    // against, so it is the unit the node follows.
    //
    // The original also files an emptied squad under no node at all
    // (0x00444809) and then destroys it, and `HeroSystem::advance` has already
    // run `prune_empty` by the time this does -- so a squad with no members is
    // not here to be filed anywhere. The guard is a guard, not that rule.
    //
    // Where the front member stands is its `posRH` (`unit_pos_rh`), which is
    // what 0x0041f530 is handed: a member in a town's garrison stands at the
    // town's central building, so a squad that marched home and went in is
    // filed under the town's node and not under wherever its holder record
    // happens to sit.
    if (!squad.members.empty() && world.find(squad.members.front()) != nullptr) {
      squad.gaika_in = nodes.at(areas, unit_pos_rh(world, squad.members.front()));
    }
    // 0x0041eb04, and it is the whole of what `SrcGAIKA` means: the first node
    // the squad was ever filed under, written once and never again.
    if (squad.src_gaika == kNoGaika) squad.src_gaika = squad.gaika_in;
  }
}


// --------------------------------------------------------------------------
// the squad-list pool
// --------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kSquadListMagic = 0x54534c53u;  // "SLST"
constexpr std::uint32_t kSquadListVersion = 1;
/// A ceiling on what one deserialised list may claim to hold, so a malformed
/// save cannot ask for gigabytes before the read that refuses it has run. Two
/// per player index times sixteen players is generous by three orders of
/// magnitude against anything the AI builds.
constexpr std::uint32_t kMaxSquadListMembers = 65536;

}  // namespace

SquadListId SquadListPool::acquire(script::ScriptId script, std::uint32_t slot) {
  // The same site always names the same entry, which is what bounds the pool --
  // `ArrayPool::acquire`'s rule, and for the reason `WorldHost::default_value`
  // gives about a declaration inside a loop body.
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    Entry& entry = entries_[i];
    if (entry.script != script || entry.slot != slot) continue;
    entry.live = true;
    entry.cursor = 0;
    entry.members.clear();
    return static_cast<SquadListId>(i + 1);
  }
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].live) continue;
    entries_[i] = Entry{script, slot, true, 0, {}};
    return static_cast<SquadListId>(i + 1);
  }
  entries_.push_back(Entry{script, slot, true, 0, {}});
  return static_cast<SquadListId>(entries_.size());
}

void SquadListPool::release_script(script::ScriptId script) {
  for (Entry& entry : entries_) {
    if (entry.script != script) continue;
    entry.live = false;
    entry.cursor = 0;
    entry.members.clear();
    entry.members.shrink_to_fit();
  }
}

bool SquadListPool::contains(SquadListId id) const noexcept {
  return id != kNoSquadList && id <= entries_.size() && entries_[id - 1].live;
}

script::ScriptId SquadListPool::owner_of(SquadListId id) const noexcept {
  if (!contains(id)) return script::kNoScript;
  return entries_[id - 1].script;
}

std::span<const SquadKey> SquadListPool::items(SquadListId id) const noexcept {
  if (!contains(id)) return {};
  return entries_[id - 1].members;
}

std::vector<SquadKey>* SquadListPool::mutable_items(SquadListId id) noexcept {
  if (!contains(id)) return nullptr;
  return &entries_[id - 1].members;
}

std::size_t SquadListPool::cursor(SquadListId id) const noexcept {
  if (!contains(id)) return 0;
  const Entry& entry = entries_[id - 1];
  return entry.cursor < entry.members.size() ? entry.cursor : entry.members.size();
}

void SquadListPool::set_cursor(SquadListId id, std::size_t at) noexcept {
  if (!contains(id)) return;
  Entry& entry = entries_[id - 1];
  entry.cursor = at < entry.members.size() ? at : entry.members.size();
}

void SquadListPool::serialize(std::vector<std::byte>& out) const {
  bytes::put_u32(out, kSquadListMagic);
  bytes::put_u32(out, kSquadListVersion);
  // Every slot in index order, dead ones included: the index *is* the handle
  // minus one, and where the holes are decides what the next acquire returns.
  bytes::put_u32(out, static_cast<std::uint32_t>(entries_.size()));
  for (const Entry& entry : entries_) {
    bytes::put_u32(out, entry.script);
    bytes::put_u32(out, entry.slot);
    bytes::put_u8(out, entry.live ? 1u : 0u);
    bytes::put_u32(out, static_cast<std::uint32_t>(entry.cursor));
    bytes::put_u32(out, static_cast<std::uint32_t>(entry.members.size()));
    for (const SquadKey& key : entry.members) {
      bytes::put_i32(out, key.index);
      bytes::put_u8(out, static_cast<std::uint32_t>(key.player));
    }
  }
}

Status SquadListPool::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t count = 0;
  if (!reader.u32(magic) || !reader.u32(version)) return FormatError::truncated;
  if (magic != kSquadListMagic) return FormatError::bad_magic;
  if (version != kSquadListVersion) return FormatError::unsupported;
  if (!reader.u32(count)) return FormatError::truncated;

  // Into a local first, moved in only once every entry has read cleanly, so a
  // truncated save leaves the pool it was loading into untouched.
  std::vector<Entry> entries;
  entries.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    Entry entry;
    std::uint8_t live = 0;
    std::uint32_t cursor = 0;
    std::uint32_t members = 0;
    if (!reader.u32(entry.script) || !reader.u32(entry.slot) || !reader.u8(live) ||
        !reader.u32(cursor) || !reader.u32(members)) {
      return FormatError::truncated;
    }
    if (members > kMaxSquadListMembers) return FormatError::malformed;
    entry.live = live != 0;
    entry.members.reserve(members);
    for (std::uint32_t m = 0; m < members; ++m) {
      SquadKey key;
      std::uint8_t player = 0;
      if (!bytes::get_i32(reader, key.index) || !reader.u8(player)) return FormatError::truncated;
      key.player = static_cast<PlayerId>(player);
      entry.members.push_back(key);
    }
    // Clamped rather than refused: a cursor past the end is what "walked to the
    // end" already means, and `cursor()` clamps on read for the same reason.
    entry.cursor = cursor < entry.members.size() ? cursor : entry.members.size();
    entries.push_back(std::move(entry));
  }
  entries_ = std::move(entries);
  return Status();
}

SquadListPool& squadlist_pool_of(World& world) { return world.squadlists(); }

// --------------------------------------------------------------------------
// the script handle
// --------------------------------------------------------------------------

script::Value make_squadlist_value(SquadListId id) noexcept {
  return script::Value::object(script::ObjectRef{kTypeSquadList, id});
}

bool is_squadlist(const script::Value& value) noexcept {
  return value.is_object() && value.as_object().type == kTypeSquadList;
}

SquadListId squadlist_of(const script::Value& value) noexcept {
  return is_squadlist(value) ? value.as_object().id : kNoSquadList;
}


script::Value pack_squad(SquadKey key) noexcept {
  if (!key.valid()) return script::Value::object(script::ObjectRef{script::kNoType, 0});
  const std::uint32_t player = static_cast<std::uint32_t>(key.player) & 0xFFu;
  const std::uint32_t index = static_cast<std::uint32_t>(key.index) & 0xFFFFu;
  return script::Value::object(kTypeSquad, (player << 16) | index);
}

SquadKey unpack_squad(const script::Value& value) noexcept {
  if (!is_squad(value)) return kNoSquad;
  const std::uint32_t packed = value.as_object().id;
  SquadKey key;
  key.index = static_cast<std::int32_t>(packed & 0xFFFFu);
  key.player = static_cast<PlayerId>((packed >> 16) & 0xFFu);
  // A packed zero index is the "no squad" the dumps print as `0(0)`; it must
  // come back as `kNoSquad` and not as a key whose player happens to be set.
  if (key.index == 0) return kNoSquad;
  return key;
}

bool is_squad(const script::Value& value) noexcept {
  return value.is_object() && value.as_object().type == kTypeSquad;
}

const Squad* squad_receiver(World& world, const script::Value& value) noexcept {
  // An early-out and not a guard: `unpack_squad` answers `kNoSquad` for any
  // other handle and `find` answers null for that. Injected as a fault it
  // survives, which is what says so.
  if (!is_squad(value)) return nullptr;
  const HeroSystem* heroes = hero_system_of(world);
  return heroes == nullptr ? nullptr : heroes->squads().find(unpack_squad(value));
}

// --------------------------------------------------------------------------
// the host slice -- see the declaration in sim/squad.hpp
// --------------------------------------------------------------------------

namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::HostRegistry;
using script::HostStatus;
using script::Value;

constexpr const char* kNoWorld = "squad: no World behind CallContext::user";
constexpr const char* kNoHeroes = "squad: no hero system registered with the world";
constexpr const char* kNoReceiver = "squad: receiver is not a squad handle";
constexpr const char* kNoSquadFound = "squad: receiver names no squad";
constexpr const char* kNoVerb = "squad: argument 4 is not a command name";
constexpr const char* kNoCommands = "squad: no command system registered with the world";

/// The receiver resolved all the way to the squad it names.
///
/// `SquadTable` lives on `HeroSystem` (sim/hero.hpp), found by name through
/// `World::systems()` -- the same seam `command_system` and `movement_system`
/// use, and for the same reason: a pointer cached on `HostContext` would have
/// to be added per domain and kept correct across a save.
struct Self {
  World* world = nullptr;
  HeroSystem* heroes = nullptr;
  Squad* squad = nullptr;
  const char* error = nullptr;

  [[nodiscard]] bool ok() const noexcept { return error == nullptr; }
};

[[nodiscard]] Self resolve(CallContext& ctx) {
  Self self;
  self.world = world_of(ctx);
  if (self.world == nullptr) {
    self.error = kNoWorld;
    return self;
  }
  self.heroes = hero_system_of(*self.world);
  if (self.heroes == nullptr) {
    self.error = kNoHeroes;
    return self;
  }
  if (ctx.count() == 0) {
    self.error = kNoReceiver;
    return self;
  }
  // The invalid handle `(kNoType, 0)` is not a wrong type: it is what
  // `Unit::GetSquad` answers for a unit in no squad, because `pack_squad`
  // sends `kNoSquad` there so that `.IsValid` reads it without a special case.
  // `HERO_AI_KILLALL.VS` writes `.GetSquad.AIDest == .GetSquad.GAIKAIn` on a
  // hero that may have no squad, and `TS_ATTACKATWILL.VS` writes
  // `u.GetSquad().TakeNearbyItems(u.sight())` the same way -- six trap hits
  // over the shipped maps, all on this line, once heroes were hired. So it
  // names no squad, which every entry point below already has an answer for,
  // and a value of some other type is still not a receiver.
  if (const script::Value& receiver = ctx.arg(0);
      receiver.is_object() && !receiver.as_object().valid()) {
    self.error = kNoSquadFound;
    return self;
  }
  if (!is_squad(ctx.arg(0))) {
    self.error = kNoReceiver;
    return self;
  }
  self.squad = self.heroes->squads().find(unpack_squad(ctx.arg(0)));
  if (self.squad == nullptr) self.error = kNoSquadFound;
  return self;
}

}  // namespace

void squad_set_cmd(World& world, Squad& squad, std::int32_t state, std::int32_t set_flags,
                   std::int32_t clear_flags, std::string_view verb, const Command& prototype,
                   GameTime now) {
  squad.state = state;
  squad.state_time = now;
  const auto set_mask = static_cast<std::uint16_t>(set_flags & 0xFFFF);
  const auto clear_mask = static_cast<std::uint16_t>(clear_flags & 0xFFFF);
  squad.flags = static_cast<std::uint16_t>((squad.flags | set_mask) & ~clear_mask);
  CommandSystem* commands = command_system(world);
  if (commands == nullptr) return;
  // Copied out before ordering: `set_command` can spawn a script, and a script
  // can leave a squad, and a span into a vector that moves is a crash waiting
  // for a bigger squad.
  const std::vector<ObjectId> members = squad.members;
  for (const ObjectId member : members) {
    (void)commands->set_command(world, member, verb, prototype);
  }
}

namespace {

/// `SetCmd(nState, nSetFlags, nClrFlags, cmd[, arg])`. 30 sites.
///
/// State, then flags, then the command to every member in join order. The
/// evidence for each of the three is in sim/squad.hpp's declaration; the
/// body is `squad_set_cmd`, which `Ship::ApplyAiTransport` shares.
HostOutcome set_cmd_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (ctx.count() < 5 || !ctx.arg(1).is_integer() || !ctx.arg(2).is_integer() ||
      !ctx.arg(3).is_integer() || !ctx.arg(4).is_string()) {
    return HostOutcome::failed(kNoVerb);
  }
  CommandSystem* commands = command_system(*self.world);
  if (commands == nullptr) return HostOutcome::failed(kNoCommands);

  const GameTime now = ctx.scheduler != nullptr ? ctx.scheduler->now() : self.world->time();
  const std::string verb = ctx.arg(4).as_string();

  // The argument, if any. `AddCommand`/`SetCommand` take the same object-or-
  // point pair, and `gbr.exe` declares the two overloads of this one under
  // separate names (`Squad::SetCmd`, `Squad::SetCmdObj`) for exactly that.
  Command prototype;
  if (ctx.count() >= 6) {
    const Value& argument = ctx.arg(5);
    if (is_point(argument)) {
      prototype.arg_kind = CommandArgKind::point;
      prototype.point = unpack_point(argument);
    } else if (argument.is_object() && argument.as_object().type == kTypeObj &&
               argument.as_object().id != kNoObject) {
      prototype.arg_kind = CommandArgKind::object;
      prototype.object = argument.as_object().id;
    }
  }

  squad_set_cmd(*self.world, *self.squad, ctx.arg(1).as_integer(), ctx.arg(2).as_integer(),
                ctx.arg(3).as_integer(), verb, prototype, now);
  return HostOutcome::ok_void();
}

/// `AIDest`. 36 sites. A read of stored state with `DestGAIKA` as the fallback
/// (0x004215c0: 0x00444140's answer, or `[squad+0x24]` when that is zero);
/// `squad_ai_dest` in sim/squad.hpp is the one reading.
HostOutcome ai_dest_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    // A receiver that names no squad is a real case rather than an error:
    // `SQUADMONITOR.VS` guards on `sq.AIDest > 0`, so "no destination" has to
    // be reachable. A null world or a non-squad receiver is still a trap.
    if (self.error == kNoSquadFound) return HostOutcome::ok_with(gaika_value(kNoGaika));
    return HostOutcome::failed(self.error);
  }
  return HostOutcome::ok_with(gaika_value(squad_ai_dest(*self.squad)));
}

/// `SrcGAIKA` (4 sites), `DestGAIKA` (2) and `OrderDest` (2) -- the three
/// siblings of `AIDest` and `GAIKAIn`, and reads of state this file already
/// declares and saves.
///
/// `Squad::SrcGAIKA` (0x00421220), `Squad::DestGAIKA` (0x00421270) and
/// `Squad::OrderDest` (0x00421620) are one instruction apart: unpack the packed
/// receiver, resolve the record, load one `uint16`. `Squad::Dump` prints the
/// third as `AIOrderDest`.
///
/// **`OrderDest` and `AIDest` are the two ends of the same decision**, which is
/// what makes both worth having: `OrderDest` is the node the squad was last
/// *ordered* to -- the input -- and `AIDest` is the one its AI is currently
/// working toward. `GS_SIEGE.VS` compares them to decide whether to re-order,
/// and says so beside the line: `if (squad.OrderDest != gaika) { squad.SendTo
/// (gaika, 1); continue; }  // issue order, to force squad's AIDest to gaika`.
///
/// All three answer `kNoGaika` for a receiver naming no squad, for `AIDest`'s
/// reason.
template <GaikaId Squad::*kField>
HostOutcome gaika_field_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_with(gaika_value(kNoGaika));
    return HostOutcome::failed(self.error);
  }
  return HostOutcome::ok_with(gaika_value(self.squad->*kField));
}

/// `sq.Count(class)` -- 8 sites, and the one entry point in this batch that is
/// *computed* rather than stored.
///
/// `0x00427980` walks the squad's membership and counts the members whose class
/// descends from the name. The corpus reads exactly that way --
/// `sq.Count("GDruid")`, `squad.Count("Military")`, `SL.Cur.Count("BaseMage")`
/// -- and `SQUADMONITOR.VS`'s `sq.Count("Horse") != sq.Size` is the shape that
/// settles it as a *subset* count rather than anything else.
///
/// The matcher is `ClassFilter`, which is the same tree test every other class
/// name in this engine goes through, and it carries the rule that matters here:
/// **a name the graph cannot resolve counts nothing rather than everything.**
HostOutcome count_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_with(Value::integer(0));
    return HostOutcome::failed(self.error);
  }
  const ClassFilter filter =
      ClassFilter::parse(ctx.arg(1).is_string() ? ctx.arg(1).as_string() : std::string_view{},
                         self.world->class_graph());
  if (filter.match_all) return HostOutcome::ok_with(Value::integer(0));
  std::int32_t found = 0;
  for (const ObjectId member : self.squad->members) {
    const WorldObject* slot = self.world->find(member);
    if (slot != nullptr && self.world->matches_filter(*slot, filter)) ++found;
  }
  return HostOutcome::ok_with(Value::integer(found));
}

/// `sq.Eval` -- 9 sites, and **it answers 0 for now, deliberately and
/// visibly.**
///
/// `0x00421120` is a plain field read of `[squad+0x1c]`: the squad's strength,
/// stored rather than computed, and the *input* to `GAIKA::Eval` rather than an
/// overload of it. So this is state, and **nothing in this engine writes it** --
/// the same standing `ai_dest` beside it already has and says it has.
///
/// That is a real cost and it is worth naming rather than burying. Three of the
/// shipped readers branch on it -- `if (sq.Eval == 0)` in `SQUADMONITOR.VS`,
/// `coming += sq.Eval` in `GSH_SYNCHAPPROACH.VS` -- so a permanent zero makes
/// the AI take the conservative branch every time. It is still the better of
/// the two available answers: the alternative is a trap, which stops
/// `SQUADMONITOR.VS` dead at its 479 call sites rather than letting it run and
/// choose cautiously. Whatever computes squad strength is a thread of its own,
/// and the day it lands this reader is already here and already correct.
HostOutcome eval_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_with(Value::integer(0));
    return HostOutcome::failed(self.error);
  }
  return HostOutcome::ok_with(Value::integer(self.squad->eval));
}

/// `sq.TestFlags(mask)` -- 8 sites. **All** the bits, not any of them.
///
/// 0x00421762 is `flags & mask`, then `cmpw` against the mask itself and
/// `sete`: the answer is true only when every bit asked for is set. Its
/// neighbour `Unit::GetFlags(mask)` (0x005d7e75) is `testl` + `setne`, which is
/// *any*, and the two are one instruction apart in intent and opposite in
/// result. The corpus can tell: `DATA\SUBAI\*` writes both
/// `TestFlags(SF_PEACEFUL)` (10 sites, one bit, the readings agree) and
/// `TestFlags(SF_PEACEFUL + SF_NOAI)` (2 sites, two bits, they do not).
///
/// A receiver that names no squad answers **false** rather than trapping, for
/// `AIDest`'s reason above: a unit outside any squad is ordinary, and every
/// shipped site is a guard whose false branch is written.
HostOutcome test_flags_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_with(Value::boolean(false));
    return HostOutcome::failed(self.error);
  }
  if (ctx.count() < 2 || !ctx.arg(1).is_integer()) {
    return HostOutcome::failed("TestFlags expects a flag mask");
  }
  const auto mask = static_cast<std::uint16_t>(ctx.arg(1).as_integer());
  return HostOutcome::ok_with(Value::boolean((self.squad->flags & mask) == mask));
}

/// `sq.TakeNearbyItems(range)` -- 2 sites, `SQUADMONITOR.VS` and
/// `TS_ATTACKATWILL.VS`, both on a hero's squad: `if (sq.TakeNearbyItems(
/// sqLeader.sight))`, a hero picking up what lies about.
///
/// `0x00429530` resolves the squad and its leader, and answers **false** unless
/// the leader is a hero (`SyncFlags` bit 24), the squad's destination node is
/// the one it is in (0x00444140 against `[squad+0x24]`: not on the march),
/// the leader's running command is not already `getitems` (0x007adfd0), and
/// the leader has room (`vtbl+0x104` items below `vtbl+0x108`, the count and
/// the cap `Obj::item_count` and `max_items` read). Then it sweeps `range`
/// around the leader for the **nearest item holder that still holds
/// something** (0x00423630 with the predicate at 0x00422820: a `CVXItemHolder`
/// whose count is non-zero, ranked by distance), builds a `getitems` command
/// aimed at it from the leader's class (0x005aef20; a class that does not bind
/// the method prints and answers false) and pushes it the way `SneakCommand`
/// does (0x005b08d0: the runner cloned behind it, the runner ended), raises
/// bit 19 of the leader's second flag word, and answers **true**.
///
/// The bit is written by five routines in `.text` and read by none, so it is
/// not kept. The node test passes here as it passes for every squad on this
/// engine, both sides being `kNoGaika`, and the sweep is `objects_in_radius`
/// over the holder class with the store's own count, ties to the lower id
/// where the original's cell order decides.
HostOutcome take_nearby_items_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_with(Value::boolean(false));
    return HostOutcome::failed(self.error);
  }
  const std::int32_t range = ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  World& world = *self.world;
  const ObjectId leader = self.squad->leader;
  const WorldObject* slot = world.find(leader);
  if (slot == nullptr || !slot->state.flags.is_hero) return HostOutcome::ok_with(Value::boolean(false));
  // Both sides are `kNoGaika` on this engine, so the test is an equivalence
  // the sweep labels; it is the original's, kept for the day a squad is sent.
  if (self.squad->ai_dest != self.squad->gaika_in) return HostOutcome::ok_with(Value::boolean(false));
  CommandSystem* commands = command_system(world);
  if (commands == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  const auto same_verb = [](std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i]))) {
        return false;
      }
    }
    return true;
  };
  if (same_verb(commands->command_name(leader, 0), "getitems")) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const ItemStore& items = self.heroes->items();
  if (items.count_for(leader) >= items.capacity_of(world, leader)) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const Point at = world.resolve_position(leader);
  std::vector<ObjectId> around;
  world.objects_in_radius(at, range, ClassFilter{}, around);
  ObjectId best = kNoObject;
  std::int64_t best_distance = 0;
  for (const ObjectId id : around) {
    const WorldObject* cand = world.find(id);
    if (cand == nullptr || cand->object == nullptr ||
        !cand->object->is_a(imperivm::core::NativeClass::item_holder)) {
      continue;
    }
    if (items.count_for(id) <= 0) continue;
    const Point there = world.resolve_position(id);
    const std::int64_t dx = there.x - at.x;
    const std::int64_t dy = there.y - at.y;
    const std::int64_t distance = dx * dx + dy * dy;
    if (best == kNoObject || distance < best_distance) {
      best = id;
      best_distance = distance;
    }
  }
  if (best == kNoObject) return HostOutcome::ok_with(Value::boolean(false));
  if (commands->script_for(world, leader, "getitems").empty()) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  Command order;
  order.arg_kind = CommandArgKind::object;
  order.object = best;
  const CommandQueue* queue = commands->find(leader);
  if (queue != nullptr && !queue->entries.empty()) {
    const Command clone = queue->entries.front();
    const std::string running = clone.verb;
    (void)commands->add_command(world, leader, /*front=*/true, running, clone);
    (void)commands->add_command(world, leader, /*front=*/true, "getitems", order);
    (void)commands->kill_command(world, leader);
  } else {
    (void)commands->add_command(world, leader, /*front=*/true, "getitems", order);
  }
  return HostOutcome::ok_with(Value::boolean(true));
}

/// `ClrCmd(nState, nSetFlags, nClrFlags)` -- 13 sites.
///
/// `SetCmd` without a *new* command -- and **not** without ending the old one.
/// `gbr.exe`'s signature table gives the two the same three leading arguments
/// and gives this one nothing after them, which is what says the state and the
/// flags are the part they share. What the body (0x004271d0) does, in order:
///
///   1. **refuses a squad carrying `SF_NOAI`** (`[squad+0x30] & 1`) outright --
///      no state, no flags, no clock. A squad a mission has pinned is not the
///      AI's to stop, which is `SendTo`'s rule one entry point over;
///   2. writes the state, stamps the clock, and computes the flags
///      set-then-clear;
///   3. **walks every member and ends what it is doing** -- `vtbl+0xc0(1)`,
///      the call `Settlement::IdleAllGates` makes on a gate: the pending tail
///      dropped and the runner ended, so the queue refills with the class's
///      default verb. No hero stop, unlike the three walkers that *issue*
///      orders. Each member also gets `[unit+0x194] |= 0x80000`, the bit the
///      other walkers name and this engine does not carry;
///   4. holds `kSquadLocked` over the walk and writes the computed word back
///      after it, so the lock is invisible afterwards.
///
/// Step 3 is what this used to leave out, and it is what the name means: every
/// shipped site is a squad being *stopped* -- `AIOSENDSQUAD.VS` for a squad
/// already in the node it was sent to, `GS_SIEGE.VS` and `GS_CAPTURE.VS` for an
/// approach that has arrived. Without it a squad "cleared" to `SS_IDLE` kept
/// walking wherever its last order sent it, and a town's own sentries, sent to
/// the node they stand in, never stopped to be re-posted by `WALL_PATROL.VS`
/// and `GATE_PATROL.VS`, which re-order only a sentry whose command is `idle`.
///
/// **It still stamps `state_time`.** `EVALRECRUIT.VS` reads `sq.StateTime` as
/// "how long since anything last happened to this squad".
HostOutcome clr_cmd_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (ctx.count() < 4 || !ctx.arg(1).is_integer() || !ctx.arg(2).is_integer() ||
      !ctx.arg(3).is_integer()) {
    return HostOutcome::failed("ClrCmd: expected three integers");
  }
  if ((self.squad->flags & kSquadFlagNoAi) != 0) return HostOutcome::ok_void();
  const GameTime now = ctx.scheduler != nullptr ? ctx.scheduler->now() : self.world->time();
  self.squad->state = ctx.arg(1).as_integer();
  self.squad->state_time = now;
  const auto set_mask = static_cast<std::uint16_t>(ctx.arg(2).as_integer() & 0xFFFF);
  const auto clear_mask = static_cast<std::uint16_t>(ctx.arg(3).as_integer() & 0xFFFF);
  const auto flags = static_cast<std::uint16_t>((self.squad->flags | set_mask) & ~clear_mask);
  CommandSystem* commands = command_system(*self.world);
  if (commands == nullptr) {
    self.squad->flags = flags;
    return HostOutcome::ok_void();
  }
  const SquadKey key = self.squad->key;
  self.squad->flags = static_cast<std::uint16_t>(flags | kSquadLocked);
  // Copied out first, as `squad_set_cmd` does: ending a command can spawn the
  // default verb's script, and a script can leave the squad.
  const std::vector<ObjectId> members = self.squad->members;
  for (const ObjectId member : members) {
    if (self.world->find(member) == nullptr) continue;
    (void)commands->clear_commands(*self.world, member);
    (void)commands->kill_command(*self.world, member);
  }
  if (Squad* squad = self.heroes->squads().find(key); squad != nullptr) squad->flags = flags;
  return HostOutcome::ok_void();
}

/// `sq.State` -- **89 sites, the most-read member on the type** and the largest
/// single unimplemented entry point in the whole install until now.
///
/// 0x00421170 unpacks the receiver -- a 16-bit word whose low nibble is the
/// player and whose top twelve bits are the index, which is this engine's
/// `SquadKey` pair under another encoding -- and reads `[squad+0x28]`. One of
/// the `SS_*` values `DATA/AI/AI.INI` declares as data, with `0` the
/// hardcoded `SS_IDLE`; carried as the raw integer because the names are a
/// profile's to define.
///
/// A receiver naming no squad answers `SS_IDLE` rather than trapping, for
/// `AIDest`'s reason: a unit outside any squad is ordinary and every shipped
/// site is a comparison whose false branch is written.
/// `sq.LastFightTime` -- 4 sites, all in `SQUADMONITOR.VS`, all of the shape
/// `if (sq.LastFightTime > GetTime - 3000)`.
///
/// 0x00421860 unpacks the handle and reads `[squad + 0xbc]` -- the game time a
/// member was last struck. Written by `record_squad_attacked`, once per tick.
///
/// A handle that names no squad answers **0**, which reads as "never fought"
/// and is what every shipped comparison wants; it is `State`'s rule and not a
/// separate decision.
HostOutcome last_fight_time_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_with(Value::integer(0));
    return HostOutcome::failed(self.error);
  }
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(self.squad->last_fight_time)));
}

/// `sq.GetLastAttacker` -- 4 sites, and it is the other half of the pair above.
///
/// 0x004218b0 reads `[squad + 0xc0]`, the 16-bit handle stamped beside the
/// time. `SQUADMONITOR.VS` reads the two together -- fought within three
/// seconds, attacker still valid, attacker a building or a sentry -- and then
/// walks the squad away along `sq.pos - sq.GetLastAttacker.pos`.
///
/// **The handle is not re-checked for liveness here**, which matters because
/// the shipped site checks it itself (`if (sq.GetLastAttacker.IsValid)`): the
/// stored id is whatever struck last, and it may since have died. Answering the
/// stale id and letting `IsValid` say so is the original's arrangement.
HostOutcome last_attacker_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) {
      return HostOutcome::ok_with(Value::object(kTypeObj, kNoObject));
    }
    return HostOutcome::failed(self.error);
  }
  return HostOutcome::ok_with(Value::object(kTypeObj, self.squad->last_attacker));
}

/// `sq.No` -- the squad's **number within its player**, and nothing else.
///
/// 0x004210f0 never resolves the squad at all: it takes the packed handle off
/// the stack and answers `(handle >> 4) & 0xfff`, which is the index half of
/// the `player | index << 4` packing 0x00443e30 resolves. This engine packs the
/// same pair differently -- `index | player << 16`, `unpack_squad` -- so the
/// shift is not copied and the *field* is, which is what the one call site
/// wants: `GS_SIEGE.VS` builds the debug line `"squad " + sq.No + "(" + sq.Size
/// + "/" + sq.Eval`.
///
/// Because nothing is resolved, a handle naming no live squad still answers its
/// own index rather than refusing -- reproduced, and it is why this does not go
/// through `resolve`.
HostOutcome squad_number_impl(CallContext& ctx) {
  if (ctx.count() == 0) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(Value::integer(unpack_squad(ctx.arg(0)).index));
}

/// `MilEval(player)`, `AllyMilEval(player)` and `EnemyMilEval(player)` -- 7
/// sites between them, and one sum read three ways.
///
/// The sum is 0x00443e70, over the squad list on the player record at
/// `[+0x8c]`: skip a squad whose flags word has any of **0x25** set -- bits 0,
/// 2 and 5, which `sim/globals.cpp` names `SF_NOAI`, `SF_PEACEFUL` and
/// `SF_SENTRIES` -- and add `Squad::Eval` for the rest. So the number is
/// *fielded strength*: what a mission has pinned, what will not fight, and what
/// is standing on a wall are all left out.
///
/// The three differ only in whose squads are added, and the relation is
/// 0x0044e250 read off the **asking** player's row:
///
///   * `MilEval` -- one player's own.
///   * `AllyMilEval` -- **everyone the asker does not call an enemy**, which is
///     the asker itself and its allies. The test is `relation != AI_ENEMY`, not
///     `relation == AI_ALLY`, so a player the asker has no opinion about counts
///     too.
///   * `EnemyMilEval` -- `relation == AI_ENEMY`, and only that.
///
/// All three take a **1-based** player and answer 0 for one outside 1..16.
///
/// **They answer 0 on every shipped map today**, because nothing in this engine
/// writes `Squad::Eval` -- the standing gap `sim/squad.hpp` records, and the
/// same one that empties `GetAIControlledUnits`. The chain is written out
/// because it is fully recovered and because 0 is the answer the original also
/// gives for a player with no armies, which is what the callers branch on.
[[nodiscard]] std::int32_t fielded_strength(const SquadTable& squads, PlayerId player) {
  constexpr std::uint16_t kNotFielded =
      kSquadFlagNoAi | kSquadFlagPeaceful | kSquadFlagSentries;
  std::int32_t total = 0;
  for (const Squad& squad : squads.squads()) {
    if (squad.key.player != player) continue;
    if ((squad.flags & kNotFielded) != 0) continue;
    total += squad.eval;
  }
  return total;
}

/// `AI_ENEMY`, or not. The two sweeps differ in nothing else.
template <bool kEnemies>
HostOutcome mil_eval_over(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  const PlayerId asking =
      ctx.count() > 0 && ctx.arg(0).is_integer() ? player_from_script(ctx.arg(0).as_integer())
                                                 : kNoPlayer;
  if (asking == kNoPlayer) return HostOutcome::ok_with(Value::integer(0));

  const PlayerTable& players = world->players();
  std::int32_t total = 0;
  for (PlayerId other = 0; other < kPlayerCount; ++other) {
    if (players.is_enemy(asking, other) != kEnemies) continue;
    total += fielded_strength(heroes->squads(), other);
  }
  return HostOutcome::ok_with(Value::integer(total));
}

HostOutcome mil_eval_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  const PlayerId asking =
      ctx.count() > 0 && ctx.arg(0).is_integer() ? player_from_script(ctx.arg(0).as_integer())
                                                 : kNoPlayer;
  if (asking == kNoPlayer) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(Value::integer(fielded_strength(heroes->squads(), asking)));
}

/// The file 0x00448b60 runs for an AI order of verb 1 -- a literal path in the
/// executable (0x007b15ac), so the root profile's copy whatever the player's
/// profile, and a file `AI.INI`'s `[Scripts]` table does not list. Its shipped
/// signature is `void, SquadList l, GAIKA g`.
constexpr std::string_view kSendSquadOrderScript = "data/ai/AIOSendSquad.vs";

/// Owners for the one-squad lists `SendTo` hands that script, one per nesting
/// depth -- `GAIKA::Recruit` runs `SendSquad.vs` synchronously and that calls
/// `SendTo`, so two can be live at once. Outside every id `Scheduler` issues
/// and outside `run_script_now`'s synthetic range, and released before the
/// call returns, so no list outlives it.
constexpr script::ScriptId kSendToListOwner = 0x50000000u;
int g_send_to_depth = 0;

/// `sq.SendTo(g, n)` -- 13 sites, and it is **how a script moves a squad**.
///
/// 0x00421520 refuses three things and then posts an order:
///
///   * a player outside 1..16, and a player with no AI. `[record+0x88]` is the
///     per-player AI structure, which is `AiSystem::player_ai` here -- so a
///     squad belonging to a player nobody started an AI for is not sent
///     anywhere, which is the same shape `AIStop` leaves behind.
///   * a squad with **`SF_NOAI`** set (`[squad+0x30] & 1`). A squad a mission
///     has pinned is not the AI's to move, which is the rule
///     `GetAIControlledUnits` applies to units one level down.
///
/// ## The order
///
/// 0x004494c0 files a 20-byte record on the AI's order list at `[ai+0x28]`:
/// a verb -- the wrapper always writes **1** -- the squad, the node, and `n`.
/// For verb 1 it first deletes the squad's previous order (0x00448a80), then
/// stores the record's index on the squad at `[squad+0x26]`, and re-files the
/// squad on the node table as heading to the new node rather than to its
/// `DestGAIKA` (0x0041ea60 into 0x004501a0). `Squad::OrderDest` and
/// `Squad::AIDest` both read the destination back through that index
/// (0x00444140), and it stays readable after the order has been carried out:
/// only `DelOrder` or the next `SendTo` frees the record. So `order_dest` and
/// `ai_dest` are written here, and `dest_gaika` is not -- the original's
/// `DestGAIKA` is the squad's own field and this does not touch it.
/// `GS_SIEGE.VS` line 95 says the same from the script side:
///
///     if (squad.OrderDest != gaika) { squad.SendTo(gaika, 1); continue; }
///     // issue order, to force squad's AIDest to gaika
///
/// ## What carries it out, which is a script
///
/// The list is drained by 0x00448b60, from the AI's 500 ms timer (0x0041d790:
/// timer 1 calls it through 0x00448dd0 and re-arms itself for 500). For verb
/// 1 it builds a `SquadList` holding the order's squad, adds every other
/// pending verb-1 order bound for the same node whose squad stands within 240
/// of it, marks them all taken, and runs **`data/ai/AIOSendSquad.vs`** with
/// that list and the node. That script, for each squad:
///
///   * already **in** the node: `ClrCmd(SS_IDLE, 0, SF_ADVCHOOSER)` -- it
///     moves nobody, and `ClrCmd` ends every member's command -- or, a fleeing
///     squad at a settlement, `SetCmd(SS_Enter, ..., "enter", ...)`;
///   * elsewhere: `SetCmd(SS_Approach, ...)` -- `move` before minute ten,
///     `advance` after, `sneak` for a hero's squad, `move` in `SS_Flee` -- to
///     `g.GetDestPoint(leader)`, through a teleport or onto a transport ship
///     when the route needs one.
///
/// **This runs that script synchronously, at the post, for the one squad, and
/// that is the inference -- labelled.** The original's drain is later (up to
/// half a second, one order per tick of the timer, the larger `n` first
/// (0x004488e0 compares `n / 5`), and not while the previous drain's script is
/// still running) and batches
/// nearby squads bound for the same node into one list. What a squad is told
/// is the same either way: the script decides per squad, from that squad's
/// own state, and the batch changes only that a ship-less crossing's early
/// `return` skips the rest of a list. The alternatives were an order list on
/// `AiSystem`, saved, with the timer and the batching; it is the next step if
/// the delay ever shows. What it replaces was a stand-in that walked every
/// member to the node's centre with `advance` -- including a town's own
/// sentries, sent to the node they stand in, which ignore passability and
/// stacked on the town hall.
///
/// `n` is the record's priority for that drain and nothing else; with no list
/// it is read and dropped. A node the table does not have is still posted --
/// the record holds any 16-bit value -- and the script decides what to do
/// with it. With no such script loaded the order is posted and nobody moves.
HostOutcome send_to_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_void();
    return HostOutcome::failed(self.error);
  }
  if (!PlayerTable::is_valid(self.squad->key.player)) return HostOutcome::ok_void();
  // `active`, **not** `player_ai(...) != nullptr`. `sim/ai.cpp`'s `Unit::AI`
  // carries the same warning for the same field: the array is dense and hands
  // back a slot for every player in range, so the null test is true of a human
  // and every squad in the world would be sendable.
  AiSystem* ai = ai_system_of(*self.world);
  const AiPlayer* running = ai == nullptr ? nullptr : ai->player_ai(self.squad->key.player);
  if (running == nullptr || !running->active) return HostOutcome::ok_void();
  // `SF_NOAI`: a squad a mission has pinned is not the AI's to move.
  if ((self.squad->flags & kSquadFlagNoAi) != 0) return HostOutcome::ok_void();
  if (ctx.count() < 3) return HostOutcome::ok_void();
  const GaikaId dest = gaika_of(ctx.arg(1));
  self.squad->order_dest = dest;
  self.squad->ai_dest = dest;

  // The drain. Compiled on demand, as `RunAIHelper` compiles its file: no
  // manifest lists this one.
  if (ctx.scheduler == nullptr) return HostOutcome::ok_void();
  HostContext* host = host_context_of(ctx);
  std::uint32_t chunk = script::kNoChunk;
  if (host != nullptr && host->library != nullptr) {
    chunk = host->library->chunk_for(kSendSquadOrderScript);
  }
  if (chunk == script::kNoChunk) chunk = ctx.scheduler->find_chunk_exact(kSendSquadOrderScript);
  if (chunk == script::kNoChunk) return HostOutcome::ok_void();
  if (g_send_to_depth >= 8) return HostOutcome::failed("SendTo: orders nested too deep");

  SquadListPool& pool = squadlist_pool_of(*self.world);
  const script::ScriptId owner = kSendToListOwner + static_cast<script::ScriptId>(g_send_to_depth);
  const SquadListId list = pool.acquire(owner, 0);
  if (std::vector<SquadKey>* items = pool.mutable_items(list); items != nullptr) {
    items->assign(1, self.squad->key);
  }
  pool.set_cursor(list, 0);
  const Value args[2] = {make_squadlist_value(list), gaika_value(dest)};
  Value ignored = Value::integer(0);
  ++g_send_to_depth;
  const HostOutcome ran = run_script_now(ctx, chunk, args, "SendTo: AIOSendSquad.vs failed", ignored);
  --g_send_to_depth;
  squadlist_pool_of(*self.world).release_script(owner);
  if (ran.status != HostStatus::ok) return ran;
  return HostOutcome::ok_void();
}

HostOutcome state_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_with(Value::integer(0));
    return HostOutcome::failed(self.error);
  }
  return HostOutcome::ok_with(Value::integer(self.squad->state));
}

/// `sq.StateTime` -- 3 sites, and **it is elapsed time, not a timestamp**.
///
/// 0x00421400 loads the clock, resolves the squad, and returns
/// `now - [squad+0x2c]`. The stored field is a stamp -- `TVXSquad` persists it
/// as `StateSetTime` -- and the entry point subtracts, which
/// `sim/squad.hpp`'s declaration used to deny. See the corrections table in
/// `docs/plan.html`: the claim was that the three corpus sites compare it
/// against `GetTime()`, and none of them does.
///
/// `EVALRECRUIT.VS` settles it twice over in eight lines: it substitutes
/// **10,000,000** for the value when the match is under three minutes old
/// (a duration meaning *ages ago*, absurd as a timestamp on a young match),
/// tests it against `1000 * AIVar(AIV_SquadRecruitWait)`, and divides it by
/// 1000 to get seconds. The `GetTime` that the wrong reading was drawn from is
/// on the line between, testing the *match* clock about something else.
HostOutcome state_time_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_with(Value::integer(0));
    return HostOutcome::failed(self.error);
  }
  const GameTime now = ctx.scheduler != nullptr ? ctx.scheduler->now() : self.world->time();
  const GameTime elapsed = now - self.squad->state_time;
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(elapsed > 0 ? elapsed : 0)));
}

/// `sq.Leader` -- 37 sites. **The front of the member deque**, or an invalid
/// object for an empty squad: 0x00421310 resolves the squad and hands its
/// front (0x00443df0) back as an object handle, `0xffff` when there is none.
///
/// This used to read `Squad::leader`, the field `create(owner, leader)` fills
/// for a hero's squad and `join` never fills -- and the comment above it said
/// the opposite, that the leader *was* `members[0]`. It was neither observable
/// nor wrong while every squad was a hero's; the moment units were squadded at
/// placement, `SQUADMONITOR.VS` logged `Inconsistent squad?!?` 32,780 times in
/// four minutes on one map and skipped every one of them, because the only
/// squads with a valid `Leader` were the heroes'. The front member is what the
/// rest of this file already means by a leader -- `squad_accepts`, `CalcGoAround`,
/// `NearestHospital` all read `members.front()` -- and now so does this.
HostOutcome leader_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  const auto none = HostOutcome::ok_with(Value::object(script::ObjectRef{script::kNoType, 0}));
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return none;
    return HostOutcome::failed(self.error);
  }
  if (self.squad->members.empty()) return none;
  return HostOutcome::ok_with(Value::object(kTypeObj, self.squad->members.front()));
}

/// `sq.GAIKAIn` -- 61 sites. Which AI node the squad is currently in.
///
/// A read of stored state, exactly as `AIDest` is, and one of the three GAIKA
/// fields `TVXSquad` persists -- which is what says it is world state rather
/// than a view something recomputes. Nothing here fills it, so on a squad no
/// loader has touched it answers `kNoGaika`, which is 0; `SQUADMONITOR.VS`
/// compares `sq.GAIKAIn == sq.AIDest` and both being 0 is a case it handles.
HostOutcome gaika_in_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_with(gaika_value(kNoGaika));
    return HostOutcome::failed(self.error);
  }
  return HostOutcome::ok_with(gaika_value(self.squad->gaika_in));
}

/// `sq.DelOrder()` -- 9 sites, six of them commented out (`GS_GUARD.VS` line
/// 27 is `//if (SL.Cur.OrderDest == gaika) squad.DelOrder();`). The three live
/// ones are `SQUADMONITOR.VS` lines 128 and 625 and `AIOSENDSQUAD.VS`'s
/// `ship.GetSquad.DelOrder`, on the transport it is about to board.
///
/// 0x00421670 hands the squad to 0x00448a80, which frees the squad's record on
/// the AI order list (0x00448a10) and sets the index at `[squad+0x26]` back to
/// -1. `OrderDest` and `AIDest` both read the destination through that index
/// (0x00444140), so **both** lose it: `OrderDest` answers no node and `AIDest`
/// falls back to `DestGAIKA`. This cleared `order_dest` alone while nothing
/// reached it; `SendTo` writes the two together, and this undoes the two
/// together.
HostOutcome del_order_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_void();
    return HostOutcome::failed(self.error);
  }
  self.squad->order_dest = kNoGaika;
  self.squad->ai_dest = kNoGaika;
  return HostOutcome::ok_void();
}

/// `GAIKA::ID` -- 33 sites, and **the identity function on an integer**.
///
/// `sim/gaika.hpp` gives six independent reasons a GAIKA value *is* its index;
/// this entry point exists because there are places an `int` is syntactically
/// required and a `GAIKA` would not typecheck -- string concatenation, an
/// `int`-typed host argument, a comparison against an `int` local. Not one
/// GAIKA-to-GAIKA comparison in the corpus writes `.ID` on either side.
///
/// **Registered here rather than in a domain of its own**, because a domain
/// for one body would have to be inserted into `host_domains()`'s asserted
/// order for no other reason, and this domain already owns the AI-side handles
/// and includes `sim/gaika.hpp`.
///
/// `gbr.exe` also registers `Item::id`, which returns a **string** where this
/// returns an int -- and member lookup here is case-insensitive, so the two
/// share one registry entry. No shipped site reaches the item form: all 47
/// `.ID` receivers in the corpus are GAIKAs (`g`, `gaika`, `gSrc`, `gDst`,
/// `gMAIKA`, `GetGaika`, `GAIKAIn`, `AIDest`). A receiver that is not an
/// integer answers `kNoGaika` rather than guessing at the other overload.
HostOutcome gaika_id_impl(CallContext& ctx) {
  if (ctx.count() == 0) return HostOutcome::ok_with(Value::integer(kNoGaika));
  return HostOutcome::ok_with(Value::integer(gaika_of(ctx.arg(0))));
}

/// `Unit::IsEnemyInSquadSight()` -- 20 sites, **132 of this engine's 197
/// remaining trap hits**, and it is always false.
///
/// 0x005d7400 resolves the receiver's squad through 0x00444190 -- which reads
/// the packed handle at `[unit+0x174]` and indexes the per-player squad table
/// -- and then answers `[squad+0x7c] != 0`. **That field has no setter in
/// `gbr.exe`.** A byte-pattern sweep of the whole `.text` for writes to
/// `[reg+0x7c]` finds seven, of which exactly two are on a squad and both are
/// constructors writing zero (0x0044440b, in a run of `edi = 0` stores either
/// side of it, and 0x0045402f, which writes the literal 0 beside `[esi+0x28]`
/// -- the state field, which is what identifies the object). The only reader is
/// this entry point.
///
/// So the original answers false too, on every unit, in every session -- the
/// same shape of failure this project keeps finding in its own tree, here in
/// the game's. The four scripts that ask (`rpriest_idle.vs` 88 hits,
/// `shaman_idle.vs` 33, and two more) take their else branch on the original
/// exactly as they will here.
///
/// **No field is added for it.** `Unit::user` is stored because a script can
/// read it back through a registered getter; this has no getter and no writer,
/// so storing it would be a hashed, serialised bit that nothing could ever
/// observe. If a setter turns up that the sweep missed -- a dword-displacement
/// encoding, or a write through a computed address -- this is where it goes.
HostOutcome is_enemy_in_squad_sight_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  return HostOutcome::ok_with(Value::boolean(false));
}

// --------------------------------------------------------------------------
// the SquadList cursor
// --------------------------------------------------------------------------

/// A `SquadList` receiver resolved to its pool entry, or nothing.
struct ListSelf {
  World* world = nullptr;
  SquadListPool* pool = nullptr;
  SquadListId id = kNoSquadList;
  const char* error = nullptr;

  [[nodiscard]] bool ok() const noexcept { return error == nullptr; }
};

[[nodiscard]] ListSelf resolve_list(CallContext& ctx) {
  ListSelf self;
  self.world = world_of(ctx);
  if (self.world == nullptr) {
    self.error = kNoWorld;
    return self;
  }
  self.pool = &squadlist_pool_of(*self.world);
  if (ctx.count() == 0 || !is_squadlist(ctx.arg(0))) {
    self.error = "not a SquadList receiver";
    return self;
  }
  self.id = squadlist_of(ctx.arg(0));
  return self;
}

/// `SL.EOL` -- 27 sites, and the loop condition of every one of them.
///
/// 0x0042bae0 answers true for a list that does not resolve *and* for one whose
/// cursor is at the end, which is the same two-way degeneracy every reader here
/// has: `while (SL.EOL == false)` on a stale handle must terminate, not trap.
HostOutcome eol_impl(CallContext& ctx) {
  const ListSelf self = resolve_list(ctx);
  if (!self.ok()) {
    if (self.error == kNoWorld) return HostOutcome::failed(self.error);
    return HostOutcome::ok_with(Value::boolean(true));
  }
  return HostOutcome::ok_with(
      Value::boolean(self.pool->cursor(self.id) >= self.pool->items(self.id).size()));
}

/// `SL.Cur` -- 72 sites, the squad under the cursor.
///
/// At the end of the list -- or on a handle that names nothing -- it answers an
/// invalid squad rather than refusing. `GS_GUARD.VS` reads `squad = SL.Cur;`
/// immediately after testing `SL.EOL`, so the guarded path is the only one the
/// corpus takes; the unguarded answer still has to be a value, because every
/// squad-taking entry point already treats an unknown key as "no squad".
HostOutcome cur_impl(CallContext& ctx) {
  const ListSelf self = resolve_list(ctx);
  if (!self.ok()) {
    if (self.error == kNoWorld) return HostOutcome::failed(self.error);
    return HostOutcome::ok_with(pack_squad(kNoSquad));
  }
  const std::span<const SquadKey> items = self.pool->items(self.id);
  const std::size_t at = self.pool->cursor(self.id);
  if (at >= items.size()) return HostOutcome::ok_with(pack_squad(kNoSquad));
  return HostOutcome::ok_with(pack_squad(items[at]));
}

/// `SL.Next()` -- 32 sites. Advance, and answer whether the cursor now stands
/// on a squad.
///
/// 0x0042bb80 pushes a **bool**, and it is what 0x004201d0 answers: false at
/// once when the cursor is already at the end; otherwise step, and true only
/// when the step did not land on the end. So `Next` off the **last** squad
/// answers false. This answered "did it move" -- true there -- on the grounds
/// that every shipped site discards the value, and one does not:
/// `AIOSENDSQUAD.VS` ends its walk with `if (!l.Next()) break;`, and with the
/// other answer it ran its body once more on no squad at all. Advancing off
/// the end leaves the cursor at the end, which is what makes `EOL` stay true.
HostOutcome next_impl(CallContext& ctx) {
  const ListSelf self = resolve_list(ctx);
  if (!self.ok()) {
    if (self.error == kNoWorld) return HostOutcome::failed(self.error);
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const std::size_t size = self.pool->items(self.id).size();
  const std::size_t at = self.pool->cursor(self.id);
  if (at >= size) return HostOutcome::ok_with(Value::boolean(false));
  self.pool->set_cursor(self.id, at + 1);
  return HostOutcome::ok_with(Value::boolean(at + 1 < size));
}

/// `SL.Rewind()` -- 12 sites. Back to the first squad.
///
/// 0x0042bb30 answers a bool too, and true means the list was not empty.
HostOutcome rewind_impl(CallContext& ctx) {
  const ListSelf self = resolve_list(ctx);
  if (!self.ok()) {
    if (self.error == kNoWorld) return HostOutcome::failed(self.error);
    return HostOutcome::ok_with(Value::boolean(false));
  }
  self.pool->set_cursor(self.id, 0);
  return HostOutcome::ok_with(Value::boolean(!self.pool->items(self.id).empty()));
}

/// `Size` -- one name over **four** receivers, and this body used to answer for
/// one of them.
///
/// `gbr.exe` registers the name four times and the argument codes name the
/// receiver each time:
///
///     Squad::Size     0x00422ca0  [1, 1, 41]    the members of a squad
///     SquadList::Size 0x0042bc40  [1, 1, 40]    the squads in a list
///     IntArray::size  0x00697f20  [1, 1, 268]   0x0c | 0x100, by reference
///     StrArray::size  0x00698070  [1, 1, 269]   0x0d | 0x100
///
/// Member lookup here is case-insensitive, so `Size` and `size` are the same
/// registry key and one body has to answer for all four. It did not: it called
/// `resolve_list`, and `resolve_list` fails on anything that is not a
/// `SquadList` and this returned **0** rather than refusing. That is the
/// failure mode `sim/squad.hpp` warned about when it said a colliding name
/// takes a branch inside the existing body and never a second `define` -- and
/// the branch was the half that never got written.
///
/// What the silence cost, measured rather than guessed:
///
///   * **17 of the 18 `Size` sites are `Squad` receivers.** `SQUADMONITOR.VS`
///     alone reads `sq.Size` eleven times -- `if (sq.food < sq.Size * 3 / 2)`,
///     `needed = sq.Size * dist / 700`, `if (sq.Size == 1)`, `if (sq.Size > 6)`
///     -- so a permanent zero told the AI every army it owns is empty. Only
///     `GSH_SYNCHAPPROACH.VS:12`'s `if (SL.Size == 1)` is the list.
///   * **All five `size` sites are arrays.** Four are `TSH_HEROSKILLS.VS`'s
///     own guards, which is the reason `Hero::TSAdvHeroSkills` could not be
///     bound before this: `if (aSkills.size()==0) return;` would have made the
///     helper a silent no-op at all 68 of its call sites. The fifth is
///     `BUILD_CATAPULT.VS:25`'s `for (i = 0; i < race_num.size; i += 1)`, whose
///     loop chooses which race's catapult to place; with a zero bound it never
///     ran and every AI catapult in the game came out race 0.
///
/// `Squad::Size` is `[squad+0x50]`, which is `_Mysize` of the MSVC deque whose
/// header starts at `+0x40` -- the membership deque `Squad::Count` walks. So it
/// is `members.size()`, not a stored number, and the two cannot disagree.
///
/// Every receiver answers 0 rather than refusing when it names nothing, which
/// is what all three of the original bodies do and what the loop bounds above
/// need: `for (i = 0; i < x.Size; ...)` on a stale handle must run zero times.
/// A missing world is still a refusal, because that is a broken host rather
/// than a stale handle.
HostOutcome size_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  if (ctx.count() == 0) return HostOutcome::ok_with(Value::integer(0));
  const script::Value& self = ctx.arg(0);

  if (is_script_array(self)) {
    return HostOutcome::ok_with(
        Value::integer(static_cast<std::int32_t>(world->arrays().size(array_of(self)))));
  }
  if (is_squadlist(self)) {
    return HostOutcome::ok_with(Value::integer(
        static_cast<std::int32_t>(squadlist_pool_of(*world).items(squadlist_of(self)).size())));
  }
  if (is_squad(self)) {
    HeroSystem* heroes = hero_system_of(*world);
    if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
    const Squad* squad = heroes->squads().find(unpack_squad(self));
    return HostOutcome::ok_with(
        Value::integer(squad == nullptr ? 0 : static_cast<std::int32_t>(squad->members.size())));
  }
  return HostOutcome::ok_with(Value::integer(0));
}

/// `Lock` and `Unlock` -- 20 sites each, and **they write a flag bit**.
///
/// One entry point per name for both receiver shapes, because this registry
/// keys on `(kind, name, arity)` and `gbr.exe` registers each twice -- once on
/// `Squad` (0x004217e0, 0x00421820) and once on `SquadList` (0x0042bce0,
/// 0x0042bde0), the second being the first applied to every member. See
/// `kSquadLocked` for the bit, for why the `SF_*` constants have a hole where
/// it is, and for what honours it (the AI, not this engine, not yet).
template <bool kOn>
HostOutcome lock_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  if (ctx.count() == 0) return HostOutcome::ok_void();

  const auto apply = [&](SquadKey key) {
    Squad* squad = heroes->squads().find(key);
    if (squad == nullptr) return;
    squad->flags = static_cast<std::uint16_t>(kOn ? (squad->flags | kSquadLocked)
                                                  : (squad->flags & ~kSquadLocked));
  };

  if (is_squad(ctx.arg(0))) {
    apply(unpack_squad(ctx.arg(0)));
    return HostOutcome::ok_void();
  }
  if (is_squadlist(ctx.arg(0))) {
    const SquadListPool& pool = squadlist_pool_of(*world);
    // Copied, because `apply` reaches the squad table and a table that grows
    // must not invalidate the span being walked.
    const std::span<const SquadKey> items = pool.items(squadlist_of(ctx.arg(0)));
    const std::vector<SquadKey> keys(items.begin(), items.end());
    for (const SquadKey& key : keys) apply(key);
  }
  return HostOutcome::ok_void();
}

/// The four presence bits `GAIKA::GetSquads` filters on, from the executable's
/// own constant table (`imcheck globals`): a squad heading *to* this node, one
/// heading away from it, one sitting in it, and the all-bits value the corpus
/// spells `AI_ALL`.
constexpr std::int32_t kAiComing = 1;
constexpr std::int32_t kAiLeaving = 2;
constexpr std::int32_t kAiStaying = 4;
/// And the relation bits, from the same table. `AI_FRIENDLY` is 3, which is
/// `AI_OWN | AI_ALLY` -- so these compose and the corpus writes
/// `AI_OWN + AI_ALLY` for it at one site.
constexpr std::int32_t kAiOwn = 1;
constexpr std::int32_t kAiAlly = 2;
constexpr std::int32_t kAiEnemy = 4;

/// The player argument, **1-based on the way in**, as every per-player AI
/// accessor in this corpus is.
///
/// **This used to be read as 0-based here and that was wrong**, which is a
/// correction rather than a new rule: `GAIKA::GetSquads` (0x00439500) pops the
/// argument and `dec`s it at 0x004395a5 before the membership loop ever sees
/// it, and the whole corpus passes `AIPlayer` -- which `AIGetPlayer` returns
/// 1-based. The old reading made `gaika.GetSquads(SL, AI_STAYING, AIPlayer,
/// AI_OWN)` hand back the *next* player's squads, and nothing caught it because
/// no squad has a node yet and every list comes back empty either way. The
/// census below shares this function so the two cannot disagree again.
///
/// Out of range names nobody, which selects nothing -- the same answer the
/// original gives after the decrement puts the index outside its table.
[[nodiscard]] PlayerId player_arg(CallContext& ctx, std::size_t index) noexcept {
  if (ctx.count() <= index || !ctx.arg(index).is_integer()) return kNoPlayer;
  const std::int32_t id = ctx.arg(index).as_integer();
  return (id >= 1 && id <= 16) ? static_cast<PlayerId>(id - 1) : kNoPlayer;
}

/// Where a squad stands relative to a node, as a presence mask.
///
/// **Reconstructed from the squad's own side, and that is the inference in this
/// whole family.** Retail walks a deque stored *on* the node (0x004395f4 reads
/// `[gaika+0x18]`). `sim/gaika.hpp` now has a node table, but it holds no such
/// deque and its numbering is this engine's own approximation rather than the
/// original's, so membership is still read back from the three GAIKA fields the
/// squad carries: `gaika_in == g` and going nowhere else is staying,
/// heading to `g` from elsewhere is coming, `gaika_in == g` with a destination
/// elsewhere is leaving. Likely, and unproven.
///
/// **Where a squad is heading is `AIDest`, not `DestGAIKA`.** Posting an
/// order re-files the squad on the node table from its `DestGAIKA` to the
/// order's node (0x004494c0 calls 0x0041ea60, which hands both to 0x004501a0),
/// and deleting the order files it back -- so what the node deques hold is the
/// order's destination while there is one, which is `squad_ai_dest`.
/// `GetAIControlledUnits` below reads it the same way. This read `dest_gaika`
/// while `SendTo` wrote that field too; it writes the order alone now.
///
/// Kept as one function because `GetSquads` and the census below have to agree:
/// `GETGAIKASTRAT.VS` calls `Eval` and `Count` on the same node in consecutive
/// lines and compares the results, and `SQUADMONITOR.VS` walks a `GetSquads`
/// list and then evaluates the same node. Two copies of this rule that drifted
/// would make those disagree in a way no test here would catch.
[[nodiscard]] std::int32_t squad_presence(GaikaId node, const Squad& squad) noexcept {
  if (node == kNoGaika) return 0;
  const GaikaId heading = squad_ai_dest(squad);
  if (squad.gaika_in == node) {
    return (heading == kNoGaika || heading == node) ? kAiStaying : kAiLeaving;
  }
  return heading == node ? kAiComing : 0;
}

/// One player's relation to another as an `AI_*` mask: `AI_OWN` wins over
/// `AI_ALLY` -- a player is not its own ally -- and `AI_ENEMY` is asked
/// independently, so a table that somehow held both would report both rather
/// than silently pick one. 0x0044e250 is exclusive between the three; this is
/// not, and the difference has never been observable.
///
/// The `kNoPlayer` test on the *other* side is belt and braces over
/// `PlayerTable`'s own validity checks, which already answer false for it. No
/// test can distinguish it, and the sweep says as much.
[[nodiscard]] std::int32_t relation_between(const PlayerTable& players, PlayerId player,
                                            PlayerId other) noexcept {
  if (player == kNoPlayer || other == kNoPlayer) return 0;
  std::int32_t mask = 0;
  if (other == player) {
    mask |= kAiOwn;
  } else if (players.are_allied(player, other)) {
    mask |= kAiAlly;
  }
  if (players.is_enemy(player, other)) mask |= kAiEnemy;
  return mask;
}

/// And how a squad stands relative to the asking player, which is the same
/// question asked of its owner. Shared so that the census and `Count/3` cannot
/// drift apart about it.
[[nodiscard]] std::int32_t squad_relation(const PlayerTable& players, PlayerId player,
                                          const Squad& squad) noexcept {
  return relation_between(players, player, squad.key.player);
}

/// `gaika.GetSquads(SL, presence, player, relation)` -- 18 sites, all in
/// `DATA\AI`, and the call that fills every list the cursor above walks.
///
/// `GS_GUARD.VS` is the whole idiom in five lines:
///
///     gaika.GetSquads( SL, AI_ALL, AIPlayer, AI_OWN );
///     SL.Lock;
///     while (SL.EOL == false) { squad = SL.Cur; SL.Next(); ... }
///     SL.Unlock;
///
/// The list argument is **by reference** -- `gbr.exe` gives it type 0x128,
/// which is `SquadList` plus the by-reference bit, the same arrangement
/// `point::Rot` uses -- so the entry point fills the caller's list rather than
/// returning one, which is why no host function here ever mints a `SquadList`
/// and why the pool needs no mark-and-sweep.
///
/// **Two arities ship**: four arguments at 16 sites and three at two, the
/// three-argument form omitting the relation. `gbr.exe` registers four (one to
/// four arguments after the receiver, 0x00437a20 through 0x00439500); the two
/// the corpus never calls are registered here anyway, defaulting the way the
/// shipped ones do, because a declared name with no body traps by name and
/// these are the same body with fewer arguments.
///
/// ## What is inferred, and it is the membership rule
///
/// Retail walks a deque stored **on the GAIKA node** (0x004395f4 reads
/// `[gaika+0x18]`), and the node table `sim/gaika.hpp` now carries holds no
/// such deque. So "a squad is in GAIKA *g*" is reconstructed from the
/// squad's own side -- `gaika_in == g` for staying, `dest_gaika == g` for
/// coming, `gaika_in == g` with a destination elsewhere for leaving -- which is
/// likely and unproven. Those three fields are among the eight `TVXSquad`
/// persists, which is what says they are world state rather than a view.
///
/// **Every list this fills is empty today, and that is worth saying plainly.**
/// Nothing in this engine assigns a GAIKA to a squad, so no squad's fields name
/// any node and no presence test can match. The loops above run zero times and
/// terminate, which is the correct behaviour for an AI with no territory graph
/// -- the same standing `Squad::AIDest` has had since it was written. What
/// would change it is the GAIKA node itself; see `docs/plan.html`.
HostOutcome get_squads_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  if (ctx.count() < 2 || !is_squadlist(ctx.arg(1))) {
    return HostOutcome::failed("GetSquads: expected a SquadList");
  }

  const GaikaId node = gaika_of(ctx.arg(0));
  const std::int32_t presence = ctx.count() > 2 && ctx.arg(2).is_integer()
                                    ? ctx.arg(2).as_integer()
                                    : (kAiComing | kAiLeaving | kAiStaying);
  const PlayerId player = player_arg(ctx, 3);
  // The three-argument form omits the relation. `AI_OWN` is what all sixteen
  // four-argument sites but one pass, and it is what "this player's squads"
  // means, so it is the default rather than "any".
  const std::int32_t relation =
      ctx.count() > 4 && ctx.arg(4).is_integer() ? ctx.arg(4).as_integer() : kAiOwn;

  std::vector<SquadKey>* out = squadlist_pool_of(*world).mutable_items(squadlist_of(ctx.arg(1)));
  if (out == nullptr) return HostOutcome::failed("GetSquads: receiver is not a live SquadList");
  out->clear();

  const PlayerTable& players = world->players();
  for (const Squad& squad : heroes->squads().squads()) {
    if ((squad_presence(node, squad) & presence) == 0) continue;
    // A relation mask of zero -- `AI_NONE` -- selects nothing, which is what a
    // filter that names no relation should do and is the rule
    // `ClassFilter::parse` already follows for a name it could not resolve.
    if ((squad_relation(players, player, squad) & relation) == 0) continue;
    out->push_back(squad.key);
  }
  // A freshly filled list is walked from the beginning; `GS_GUARD.VS` refills
  // and re-walks the same local every iteration of its outer loop.
  squadlist_pool_of(*world).set_cursor(squadlist_of(ctx.arg(1)), 0);
  return HostOutcome::ok_void();
}

/// `gaika.GetAIControlledUnits(class, player, num, bTakePeaceful)` -- 4 sites,
/// and every one of them is the "steal from the AI" opening of a recruiter that
/// `sim/ai.cpp` now runs.
///
/// **It answers about units, but it walks squads**, which is the thing to get
/// right: 0x0043a920 iterates the deque of squad handles the GAIKA node owns at
/// `+0x18` -- the same container `GAIKA::Empty` (0x00422dc0) tests for null and
/// `GetSquads` above walks -- selects squads by four tests, and then emits the
/// *member units* of the survivors. Not the squads, and nothing derived from
/// them.
///
/// ## The filter chain, in the order the original applies it
///
/// Per squad:
///
///   1. **strength is not zero** (`[squad+0x1c]`, which is `Squad::Eval`). The
///      same guard the census below applies, for the same reason: a squad that
///      exists with nothing in it is not an army.
///   2. **the owner is exactly the player asked for**, 1-based on the way in
///      like every per-player AI accessor here. 0x0044e250 is a four-way player
///      relation -- invalid, same, allied, enemy -- and the caller accepts only
///      *same*, so allies are refused as firmly as enemies. A player argument of
///      **zero or less skips the test entirely**: the original decrements first
///      and branches around the call on a negative index.
///   3. **`SF_PEACEFUL` is excluded unless the boolean argument says otherwise.**
///      This is the whole of what that argument does -- it gates bit 2 of the
///      squad flags word and nothing else -- and it is why the temple recruiter
///      passes `true` where the other three pass `false`: a temple will take
///      people out of a peaceful squad and a barracks will not.
///   4. **the squad is in this node, or heading to it.** "Heading to" is the
///      node of its current *order* when it has one and `DestGAIKA` when it does
///      not, which is `Squad.OrderDest` and `Squad.DestGAIKA` -- both stored
///      fields here already.
///
/// Then per member unit of each surviving squad: the slot exists, it is a
/// **unit** rather than a building (`[obj+0x2c] & 0x400000`), it is **alive**,
/// its class **descends from** the name argument (0x0059a220 is `IsHeirOf`, so
/// `"Hero"` matches every hero subclass and the match is neither exact nor
/// race-qualified), and **`UNITFLAG_NOAI` is clear** -- a unit a mission script
/// has pinned is not the AI's to lend out. That last test is the one the entry
/// point is named after.
///
/// **`num` is a countdown over units, and it cuts the walk short.** The original
/// decrements it after each append and breaks out of *both* loops on zero, so a
/// quota met halfway through a squad abandons the rest of that squad and every
/// squad after it. It is never compared, only decremented, so a `num` of zero or
/// less is "no limit" rather than "take nothing" -- reproduced here, though no
/// shipped site passes one.
///
/// There is **no ranking**: candidates come back in node-deque order and then
/// member order, truncated. Two things in the original body look like ranking
/// and neither runs -- a four-out-parameter call whose outputs are never read,
/// and a hostility refinement behind a condition that cannot hold -- so nothing
/// here reproduces them and nothing here claims to know what they meant.
///
/// **No RNG, and no mutation.** It is a pure query: the detaching and the
/// `SetNoAIFlag` that follow at every call site are the *callers'* work, and
/// the same units come back again on the next call until a caller changes the
/// world.
///
/// ## It answers an empty list today, and says so plainly
///
/// Two independent reasons, both of them facts about this engine rather than
/// about the original: nothing assigns a GAIKA to any squad, so test 4 selects
/// nothing; and `Squad::Eval` is stored-but-unfilled, so test 1 would select
/// nothing either. The filter chain is written out in full anyway because it is
/// fully recovered and because it is what has to be right the day either of
/// those changes.
///
/// An empty list is the right answer rather than a refusal, and that is
/// measured too: the original's own diagnostics for an invalid node index and
/// for an unknown class name both print through 0x00686eb0, which is a bare
/// `ret` in retail, and return the empty list. The four recruiters then take
/// their "found nobody" branch and go on to spend gold, which is a mission that
/// keeps running.
HostOutcome m_get_ai_controlled_units(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);

  ObjListPool& pool = objlist_pool_of(*world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  const script::Value answer = make_objlist_value(list);
  std::vector<ObjectId>* out = pool.mutable_items(list);
  if (out == nullptr) return HostOutcome::ok_with(answer);

  const GaikaId node = gaika_of(ctx.arg(0));
  // An index naming no node selects nothing, which is the original's silent
  // answer and not an error. `kNoGaika` has to be refused here rather than
  // matched, or every squad that is nowhere would count as being in it.
  if (node == kNoGaika || world->gaika().find(node) == nullptr) {
    return HostOutcome::ok_with(answer);
  }

  const ClassFilter filter =
      ClassFilter::parse(ctx.count() > 1 && ctx.arg(1).is_string() ? ctx.arg(1).as_string()
                                                                   : std::string_view{},
                         world->class_graph());
  if (filter.match_all) return HostOutcome::ok_with(answer);

  const std::int32_t asked =
      ctx.count() > 2 && ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  // **Zero or less skips the owner test entirely.** The original decrements the
  // argument first and branches around the relation call on a negative index,
  // leaving the accept flag set; it is not a player id that matches nobody.
  const bool filter_by_owner = asked > 0;
  const std::int32_t owner = asked - 1;

  // 64-bit because the countdown below is the original's `dec`/`je` and nothing
  // else: a 32-bit `dec` from `INT32_MIN` wraps where this would be undefined,
  // and no list this can build comes within 2^63 of the difference.
  std::int64_t remaining =
      ctx.count() > 3 && ctx.arg(3).is_integer() ? ctx.arg(3).as_integer() : 0;
  const bool take_peaceful =
      ctx.count() > 4 && ctx.arg(4).is_integer() && ctx.arg(4).as_integer() != 0;

  for (const Squad& squad : heroes->squads().squads()) {
    if (squad.eval == 0) continue;
    if (filter_by_owner) {
      // 0x0044e250 is a four-way relation -- invalid, same, allied, enemy --
      // and the caller accepts only *same*, so an ally is refused as firmly as
      // an enemy and nothing here consults the alliance table. Its *invalid*
      // answer is what makes an out-of-range player argument match nobody
      // rather than everybody.
      //
      // The original range-checks **both** indices and this checks one, which
      // is not a simplification of the rule but of the code: once equality is
      // also required, a squad past the table can only match an asking index
      // past the table, and that one is already refused. The second test was
      // written, injected as a fault, and could not be made to fail -- and a
      // guard no case can distinguish is one more thing to keep correct rather
      // than insurance.
      if (owner >= static_cast<std::int32_t>(kPlayerCount)) continue;
      if (static_cast<std::int32_t>(squad.key.player) != owner) continue;
    }
    if (!take_peaceful && (squad.flags & kSquadFlagPeaceful) != 0) continue;
    const GaikaId heading = squad.order_dest != kNoGaika ? squad.order_dest : squad.dest_gaika;
    if (heading != node && squad.gaika_in != node) continue;

    for (const ObjectId member : squad.members) {
      const WorldObject* slot = world->find(member);
      if (slot == nullptr) continue;
      if (!slot->state.flags.is_unit) continue;
      if (slot->state.health <= 0) continue;
      if (!world->matches_filter(*slot, filter)) continue;
      if (slot->state.flags.no_ai) continue;
      out->push_back(member);
      // The original's `dec`/`je`, and it is not a comparison: `num` is never
      // read as a bound, only decremented, so **zero or less is "no limit"**
      // rather than "take nothing" -- the counter walks down through the
      // negatives and never lands on zero. No shipped site passes one.
      if (--remaining == 0) return HostOutcome::ok_with(answer);
    }
  }
  return HostOutcome::ok_with(answer);
}

/// `NumSquads(idPlayer)` and `GetSquad(idPlayer, n)` -- 3 sites each, and one
/// idiom: walk a player's squads by position.
///
///     nsq = NumSquads(player);
///     for (i = 0; i < nsq; i += 1) { s = GetSquad(player, i); ... }
///
/// `PLAYER_HEROES_WISDOM.VS` and `PLAYER_WARRIORS_WISDOM.VS` are that loop
/// verbatim, once a minute, handing every squad leader a point of experience.
/// `SQUADMONITOR.VS` writes the same walk from `i = 1`.
///
/// **A squad's index *is* its position in its player's vector**, which is what
/// makes the pair work: `GetSquad` (0x00421090) does no lookup at all -- it
/// packs `((n << 4) & 0xffff) | ((idPlayer - 1) & 0xf)` and hands the handle
/// back, and it is the `Squad::*` accessors that later find nothing if the
/// index names no squad. Reproduced literally, masks included; this engine's
/// `SquadKey` is the same two fields and `SquadKey::valid()` already says index
/// 0 is nobody, which is why `SQUADMONITOR.VS` can start at 1 and the two
/// wisdom scripts can start at 0 and both be right.
///
/// **`NumSquads` answers the highest index in use plus one, not the count**,
/// and the difference is this engine's to state. The original's vector is dense
/// -- a squad's index is a position -- so its length and its population are the
/// same number. `SquadTable` here allocates the lowest free index instead, so a
/// player whose squads are 1, 2 and 5 has three squads and needs a bound of six:
/// answering the count would silently skip the last one at every shipped site.
/// The two agree whenever nothing has been destroyed, which is every save this
/// engine has ever loaded.
///
/// The original's own bound is `(end - begin) / 4` on the player's squad vector
/// at `[aiRecord + 0x8c]`, guarded by a **signed** `player - 1 < 16` -- so a
/// player of 0 or less indexes below the player table there. That is a fault
/// rather than a behaviour: out of range answers 0 here.
HostOutcome fn_num_squads(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  const PlayerId player = player_arg(ctx, 0);
  if (player == kNoPlayer) return HostOutcome::ok_with(Value::integer(0));
  std::int32_t highest = -1;
  for (const Squad& squad : heroes->squads().squads()) {
    if (squad.key.player != player) continue;
    if (squad.key.index > highest) highest = squad.key.index;
  }
  return HostOutcome::ok_with(Value::integer(highest + 1));
}

HostOutcome fn_get_squad(CallContext& ctx) {
  const PlayerId player = player_arg(ctx, 0);
  const std::int32_t index =
      ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  // The original's packing, masks and all. An out-of-range player would fold
  // onto some other player's four bits there; refusing is the one departure,
  // and it hands back the same "names no squad" handle an unused index does.
  if (player == kNoPlayer) return HostOutcome::ok_with(pack_squad(kNoSquad));
  SquadKey key;
  key.player = player;
  key.index = static_cast<std::int32_t>(static_cast<std::uint32_t>(index) & 0xfffu);
  return HostOutcome::ok_with(pack_squad(key));
}

/// The region census: `GAIKA::Eval` and `GAIKA::Count`, four arities between
/// them, and **one shape**.
///
/// Both walk the squads in a node, classify each by presence against the
/// caller's first argument and by relation against the caller's player, and
/// accumulate into three totals. `Eval` accumulates each squad's *strength*
/// (`Squad::eval`, the `int` at `squad+0x1c`); `Count` accumulates *bodies*.
/// `GETGAIKASTRAT.VS` calls the two back to back on one node with parallel
/// variable names -- `nOwn/nAlly/nEnemy` then `nOwnC/nAllyC/nEnemyC` -- which
/// is the clearest evidence in the corpus that the pair differs only in what it
/// sums.
///
/// The out-parameter order is `own, ally, enemy, enemy_hidden`, left to right in
/// the source; `gbr.exe` types all four as `0x101`, an `int` by reference.
///
/// Two squads are skipped that a naive walk would count, and both are the
/// original's:
///
///   * one whose **strength is zero** -- a squad that exists but has nothing in
///     it -- and this is why `Eval` and `Count` can disagree about whether a
///     node is occupied at all;
///   * one carrying **`SF_PEACEFUL`** (`[squad+0x30] & 4`) -- the wildlife and
///     the neutral-passive, which are not anybody's army.
///
/// **The second of those used to be written here as "player 14 or 15", and
/// that was a guess standing in for a flag this file had not yet named.** It
/// has one now: `kSquadFlagPeaceful` is bit 2 of the same sixteen-bit word
/// `Squad::GetFlags` (0x004216d0) reads at `[squad+0x30]`, and
/// `squad_flags_by_class` already sets it from `Peaceful` and `Animal`. The two
/// readings agree on the shipped maps and disagree everywhere else: a peaceful
/// squad belonging to a real player was counted, and a wildlife squad that a
/// mission had made hostile was not.
///
/// **`enemy_hidden` is 0 and stays 0.** What fills it there is the *memory*
/// adjustment at 0x0044eb80, which the walk runs over any squad whose relation
/// came back `AI_ENEMY`: it reaches the asking player's AI record, looks the
/// squad up in the per-node table the player *remembers* rather than sees, and
/// rewrites the presence to `AI_STAYING` or to nothing accordingly. This engine
/// has no fog-of-war memory of enemy force to draw on, so the adjustment is a
/// no-op here and the honest answer is zero; the corpus survives it, because
/// every one of the twelve shipped readers adds it to `enemy` or ignores it.
///
/// **This note used to name 0x00420c00 as that second pass, and that was
/// wrong.** 0x00420c00 is the node's *settlement* query -- see
/// `count_class_impl`, which reads it -- and the census calls it once after the
/// squad walk to add the settlement's sentries to whichever of the three
/// totals the settlement owner's relation selects. That contribution is still
/// missing here, and it is missing for a reason rather than by oversight: for
/// `Count` it is the settlement's ready-sentry count, which this engine has,
/// but for `Eval` it is the sentry *strength*, which is
/// `Settlement::EvalSentries` (0x005c4a60, still unwritten) times that count.
/// Adding it to one and not the other would make `Eval` and `Count` disagree
/// about a node in a way the original never does, and `GETGAIKASTRAT.VS` reads
/// them back to back. See `docs/plan.html`.
///
/// ## These used to answer zero everywhere, and they no longer do
///
/// This note said that nothing assigned a GAIKA to a squad, so every total was
/// zero. That stopped being true when the node table was built and squads were
/// filed under nodes (`revalue_squads` sets `gaika_in`), and nothing here had
/// to change for it, which is what binding the shape early was for. On
/// Crossroads `GS_CAPTURE.VS` now prints its own count -- *GAIKA Own: 11 ...
/// Enemy: 0* -- and reaches `SS_Capture` tens of thousands of times in 12,000
/// turns. The approximation the totals inherit is the node table's; see
/// `sim/gaika.hpp`.
struct Census {
  std::int32_t own = 0;
  std::int32_t ally = 0;
  std::int32_t enemy = 0;
  std::int32_t enemy_hidden = 0;

  [[nodiscard]] std::int32_t selected(std::int32_t sides) const noexcept {
    std::int32_t total = 0;
    if ((sides & kAiOwn) != 0) total += own;
    if ((sides & kAiAlly) != 0) total += ally;
    if ((sides & kAiEnemy) != 0) total += enemy;
    return total;
  }
};

template <bool kBodies>
[[nodiscard]] Census census_of(World& world, HeroSystem& heroes, GaikaId node,
                               std::int32_t presence, PlayerId player) {
  Census out;
  const PlayerTable& players = world.players();
  for (const Squad& squad : heroes.squads().squads()) {
    if (squad.eval == 0) continue;
    if ((squad.flags & kSquadFlagPeaceful) != 0) continue;
    if ((squad_presence(node, squad) & presence) == 0) continue;
    const std::int32_t relation = squad_relation(players, player, squad);
    const std::int32_t amount =
        kBodies ? static_cast<std::int32_t>(squad.members.size()) : squad.eval;
    // Not `else if`: the relations are a mask, and a squad that read as both
    // own and enemy would land in both totals rather than in whichever branch
    // came first. `squad_relation` cannot produce that today; the accumulation
    // does not depend on it.
    if ((relation & kAiOwn) != 0) out.own += amount;
    if ((relation & kAiAlly) != 0) out.ally += amount;
    if ((relation & kAiEnemy) != 0) out.enemy += amount;
  }
  return out;
}

/// The receiver, the presence mask and the player, which all four arities share.
struct CensusCall {
  GaikaId node = kNoGaika;
  std::int32_t presence = 0;
  PlayerId player = kNoPlayer;
};

[[nodiscard]] CensusCall census_args(CallContext& ctx) {
  CensusCall call;
  call.node = gaika_of(ctx.arg(0));
  // `AI_ALL` is 0xFFFF and `AI_NONE` is 0; anything that is not an integer is
  // neither, and selects nothing rather than everything.
  call.presence = ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  call.player = player_arg(ctx, 2);
  return call;
}

/// `g.Eval(presence, idPlayer, &own, &ally, &enemy, &enemy_hidden)` -- 19 sites,
/// and `g.Count(...)` with the same shape -- 5.
template <bool kBodies>
HostOutcome census_out_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  if (ctx.count() < 7) return HostOutcome::failed("Eval: expected four out-parameters");
  const CensusCall call = census_args(ctx);
  const Census census = census_of<kBodies>(*world, *heroes, call.node, call.presence, call.player);
  ctx.out(3) = Value::integer(census.own);
  ctx.out(4) = Value::integer(census.ally);
  ctx.out(5) = Value::integer(census.enemy);
  ctx.out(6) = Value::integer(census.enemy_hidden);
  return HostOutcome::ok_void();
}

/// `int g.Eval(presence, idPlayer[, sides])` -- 8 sites at three arguments and
/// 2 at two.
///
/// The two-argument form answers the *own* total, which is what
/// `GSH_INTERRUPTPASSING.VS:9`'s `lown = g.Eval(AI_LEAVING, AIPlayer)` names it.
/// So the missing third argument defaults to `AI_OWN` rather than to `AI_ALL`,
/// the same way `GetSquads`'s missing relation does and for the same reason.
HostOutcome eval_total_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  const CensusCall call = census_args(ctx);
  const std::int32_t sides =
      ctx.count() > 3 && ctx.arg(3).is_integer() ? ctx.arg(3).as_integer() : kAiOwn;
  const Census census = census_of<false>(*world, *heroes, call.node, call.presence, call.player);
  return HostOutcome::ok_with(Value::integer(census.selected(sides)));
}

/// `g.EvalNeighbors(presence, idPlayer, &own, &ally, &enemy, bool)` -- 1 site.
///
/// The same census over the node's **neighbours** rather than the node, with
/// three out-parameters instead of four and a trailing `bool` whose meaning is
/// not established. It is bound on the same standing as its four siblings.
///
/// **The adjacency it needs now exists** -- `GaikaTable::neighbours`, built for
/// `ControlledNeighbors` -- and so do the squad fields, so the two things this
/// note used to be waiting for are both here. What is still unread is the rest
/// of 0x00430800: the trailing `bool` gates a filter that walks the asking
/// player's own node view (`[ai+0x88]`, a per-node record reached through a
/// vector of ids at `[+0x40]`) and then refuses a neighbour on three
/// settlement predicates, 0x00440100, 0x0043ffc0 and 0x004401c0. Answering the
/// census over every neighbour regardless would be a different function, so
/// this stays a zero until those are read.
HostOutcome eval_neighbors_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  if (hero_system_of(*world) == nullptr) return HostOutcome::failed(kNoHeroes);
  if (ctx.count() < 7) {
    return HostOutcome::failed("EvalNeighbors: expected three out-parameters");
  }
  ctx.out(3) = Value::integer(0);
  ctx.out(4) = Value::integer(0);
  ctx.out(5) = Value::integer(0);
  return HostOutcome::ok_void();
}

/// Whether `klass` is `base` or descends from it -- `World::class_is_a` asked
/// of a class rather than of an object. Both of the original's spellings
/// (0x0059a220 by handle, 0x0059c020 by name) walk the same `[class+0xe4]`
/// parent chain, and both answer false for a null on either side, which is
/// what makes an unresolvable class name count nothing rather than everything.
[[nodiscard]] bool class_is_heir(const ClassGraph& graph, ClassIndex klass,
                                 ClassIndex base) noexcept {
  // The null test is an early-out and **not** a guard: the walk below can never
  // reach `kNoClass` as a `current`, so a null `base` would fall out false
  // anyway. Injected as a fault and survived, which is what says so.
  if (klass == kNoClass || base == kNoClass) return false;
  ClassIndex current = klass;
  for (std::size_t guard = 0; current != kNoClass && guard <= graph.size(); ++guard) {
    if (current == base) return true;
    const ClassIndex parent = graph.at(current).parent_index;
    if (parent == current) return false;
    current = parent;
  }
  return false;
}

/// `int gaika.Count(sides, idPlayer, "class")` -- 2 sites, both in
/// `GS_CAPTURE.VS`, and **the entry point this file refused to bind twice**.
///
/// The refusal said the first argument's meaning was not settled, because the
/// corpus contradicts itself -- the two live sites write `gaika.Count(AI_ENEMY,
/// AIPlayer, "Peasant")` and a commented-out line above them writes
/// `gaika.Count(AIPlayer, AI_OWN, "GDruid")` -- and the values cannot tell the
/// two orders apart, `AI_ENEMY` and `AI_STAYING` both being 4. The executable
/// settles it and the dead line is simply stale: 0x004330b0 pops the frame from
/// the top down, so the positions are (sides, player, class), and it **`dec`s
/// the second argument** before anything reads it, which no side mask would
/// survive and which every 1-based player argument in this corpus wants. That
/// is the same `dec` `GAIKA::GetSquads` does at 0x004395a5 and the same
/// convention `player_arg` above encodes. This body reads the raw integer
/// rather than calling it, for one reason: `player_arg` folds "0 or less" and
/// "past the sixteenth player" into one `kNoPlayer`, and 0x004330b0 treats them
/// differently -- the first forces `AI_OWN`, the second names nobody.
///
/// ## What it counts
///
/// The squads standing in the node, and inside each one the **members whose
/// class is or descends from the named one**. It is `Count/6`'s walk with two
/// differences and a tail:
///
///   * **presence is fixed, not a parameter.** `Count/6` filters on the caller's
///     presence mask; this one requires `AI_STAYING` outright, so a squad
///     marching in or out of the node is not counted however the caller asks.
///   * **`SF_PEACEFUL` squads are counted.** Both walk the same classifier
///     (0x0044e1f0) and the third argument is the flag gate: the census passes
///     0 and gets the skip, this passes 1 and does not. Wildlife inside a node
///     is a body here and is not a body there, which is a real asymmetry and
///     not a slip -- the census is measuring armies and this is answering "how
///     many `Peasant`s are standing here".
///
/// The `sides` mask is tested against the **relation**, the way `Eval/3`'s
/// trailing argument is, and not against the presence. A caller naming no
/// player -- the argument 0 or less, so the decremented index is negative --
/// gets `AI_OWN` for every squad rather than an answer from the alliance table,
/// which is the original's branch at 0x00433280 and is why `Count(AI_OWN, 0,
/// ...)` counts everything rather than nothing.
///
/// ## The tail, which is the half that answers something today
///
/// After the squads, 0x004330b0 asks the node's settlement (0x00420c00) for its
/// sentries and adds them when the asked-for class is `Sentry` **or something
/// under it**. The direction is worth stating because it is the surprising one:
/// 0x0059c020 walks the *asked-for* class's ancestors looking for `Sentry`, so
/// `Sentry` and `RSentry` collect the roster and `Unit` -- which every sentry
/// is -- does not. The settlement's presence is hardcoded `AI_STAYING` there,
/// its relation comes from the same alliance table, and the amount added is the
/// **ready-sentry count** -- `[settlement+0xf4]`, what `GetSentry` hands out and
/// `PutSentry` gives back, which is `Settlement::sentries_ready` here.
///
/// The original gates that on two numbers, `[+0xf4] > 0` and `[+0xf8] > 0`, and
/// only the first is reproduced. `[+0xf8]` is the sentries' *strength*, which
/// 0x005c4930 recomputes as the per-sentry eval times `[+0xf4]` on every write,
/// so the second gate carries nothing the first does not **unless the per-sentry
/// eval is zero** -- and that number is `Settlement::EvalSentries` (0x005c4a60),
/// which is still unwritten here. It reads the settlement's own building class,
/// appends `Sentry` to its name, and scales that class's maximum health by a
/// per-player factor, so it is zero only for a settlement whose sentry class
/// does not exist or has no health. Stated rather than guessed at; the day
/// `EvalSentries` lands, this gate gains its second half.
///
/// **So this is the rare AI entry point that is not answering zero.** The squad
/// half is zero for the reason `census_of` gives -- nothing in this engine
/// assigns a node to a squad -- but `sentries_ready` is live world state that
/// the economy writes, saves and hashes, so `GS_CAPTURE.VS` asking how many
/// sentries stand in a settlement it means to take gets a real number back.
[[nodiscard]] std::int32_t count_class_in_node(World& world, HeroSystem& heroes,
                                               const ClassGraph& graph, GaikaId node,
                                               std::int32_t sides, std::int32_t raw_player,
                                               ClassIndex wanted) {
  // `P = idPlayer - 1`, and the two ways it can name nobody are **not** the
  // same answer. A negative `P` -- the argument 0 or less -- makes the walk
  // force `AI_OWN` rather than ask the alliance table, so every squad in the
  // node matches an `AI_OWN` mask. An index past the sixteenth player asks the
  // table anyway and gets 0 back, which matches no mask at all.
  const bool unnamed = raw_player <= 0;
  const PlayerId player =
      (raw_player >= 1 && raw_player <= 16) ? static_cast<PlayerId>(raw_player - 1) : kNoPlayer;

  std::int32_t total = 0;
  const PlayerTable& players = world.players();
  for (const Squad& squad : heroes.squads().squads()) {
    if (squad.eval == 0) continue;
    const std::int32_t relation = unnamed ? kAiOwn : squad_relation(players, player, squad);
    if ((relation & sides) == 0) continue;
    if ((squad_presence(node, squad) & kAiStaying) == 0) continue;
    for (const ObjectId member : squad.members) {
      if (world.class_is_a(member, wanted)) ++total;
    }
  }

  const ClassIndex sentry = graph.find("Sentry");
  if (!class_is_heir(graph, wanted, sentry)) return total;
  const GaikaNode* here = world.gaika().find(node);
  if (here == nullptr || here->settlement == kNoObject) return total;
  const EconomySystem* economy = economy_of(world);
  if (economy == nullptr) return total;
  const Settlement* set = economy->settlements().for_object(here->settlement);
  if (set == nullptr || set->sentries_ready <= 0) return total;
  const std::int32_t relation =
      unnamed ? kAiOwn
              : (player == kNoPlayer ? 0 : relation_between(players, player, set->owner));
  if ((relation & sides) == 0) return total;
  return total + set->sentries_ready;
}

HostOutcome count_class_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  const ClassGraph* graph = world->class_graph();
  // No graph is every synthetic world: no class name resolves, so nothing is
  // an heir of anything and the answer is the same zero an unknown name gives.
  if (graph == nullptr) return HostOutcome::ok_with(Value::integer(0));
  const std::int32_t sides =
      ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  const std::int32_t raw_player =
      ctx.count() > 2 && ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  const ClassIndex wanted =
      ctx.count() > 3 && ctx.arg(3).is_string() ? graph->lookup(ctx.arg(3).as_string()) : kNoClass;
  // Also an early-out rather than a guard, and for the same reason: nothing is
  // an heir of `kNoClass`, so an unresolvable name already counts nothing. It
  // is here because it says so at the top instead of at the bottom of a walk
  // over every squad in the world.
  if (wanted == kNoClass) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(Value::integer(count_class_in_node(
      *world, *heroes, *graph, gaika_of(ctx.arg(0)), sides, raw_player, wanted)));
}

/// `bool gaika.AllEnemiesInHolder(idPlayer)` -- 2 sites, both in
/// `GS_CAPTURE.VS`, and the question a capture strategy asks before it commits:
/// *is the garrison the only thing left, or is there still an army in the
/// open?* Both sites read it after the strength census has already said the
/// defence outweighs the attack, and both take the same two branches -- if
/// everything hostile is indoors the strategy switches to `SS_Siege`, and if
/// anything is still standing outside it breaks off.
///
/// ## Two early refusals, and both answer *false*
///
/// 0x00430dc0 answers false without looking at a single squad when either:
///
///   * the node **has no settlement** (`[gaika+0x14]` null) -- a region node has
///     nothing to be inside, so "all of them are indoors" is not a claim it can
///     make; or
///   * the settlement's owner is **not the asking player's enemy**. The test is
///     `[owner_record + our_id*4 + 0x24] & 1`, which is bit 0 of the
///     *settlement owner's* row for us and not of ours for it. That direction
///     is the engine's own asymmetric rule -- see `PlayerTable::is_enemy`, which
///     is written the same way round and for the same evidence -- so this is
///     `is_enemy(set->owner, player)` and deliberately not the other order. The
///     shipped data contains one-sided truces, so the two can differ.
///
/// ## The walk
///
/// Then the node's squads, with `Count/3`'s guards and **one different mask**:
///
///   * zero strength is skipped, as everywhere in this family;
///   * the relation must carry `AI_ENEMY`;
///   * the presence must carry `AI_STAYING` **or `AI_LEAVING`** -- the mask is 6
///     rather than 4. A squad on its way *out* of the node is still standing in
///     it and still has to be indoors for the answer to be yes; a squad merely
///     *coming* is not here yet and is not asked about. `Count/3` masks with 4
///     and `BestTargetInGAIKA` with 6, so the family really does use both.
///   * `SF_PEACEFUL` **is** skipped here: 0x00430f73 passes 0 as the classifier's
///     third argument where `Count/3` passes 1. Wildlife in the open does not
///     stop a siege.
///
/// And the test on a squad that survives all four is **its first member alone**
/// (0x00443df0, the front of the member deque, which is the leader when there is
/// one): if that unit is not inside a holder, the answer is false. Not every
/// member -- one unit decides for its whole squad, and a squad whose leader
/// walked inside while the rest stand in the street reads as indoors. That is
/// the original's reading and it is not obviously intended; it is reproduced
/// because a squad is meant to be somewhere as a body, and guessing the other
/// way would be inventing a rule.
///
/// ## The dead tail
///
/// After the loop 0x00431025 calls the node's settlement query (0x00420c00,
/// which `count_class_impl` above reads for its sentries) with four
/// out-parameters and then **reads none of them** -- the value it returns is
/// the flag the loop set, and the four locals the query filled are never
/// touched again. The query has no side effects, so the call is dead code
/// there. It is named here rather than reproduced, because reproducing it would
/// be writing a statement whose only effect is to look faithful.
///
/// **It answers false on every shipped map today.** Not because of the squad
/// walk -- which finds nothing, for the reason the whole GAIKA family gives --
/// but because of the first refusal: `GaikaTable` does bind settlements to
/// nodes, so the second refusal is real, and a hostile settlement with no
/// enemy squad standing in the open is exactly the `true` this returns. Both
/// callers read `true` as "commit to the siege", so the conservative answer is
/// the one that comes out while squads have no node.
HostOutcome all_enemies_in_holder_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  const auto no = HostOutcome::ok_with(Value::boolean(false));

  const GaikaId node = gaika_of(ctx.arg(0));
  const GaikaNode* here = world->gaika().find(node);
  // The `kNoObject` half is a third early-out rather than a guard, and the
  // sweep says so: `SettlementStore::for_object` refuses a null handle by its
  // own first line, so the lookup below would answer nothing anyway. The
  // `nullptr` half is load-bearing -- a node id nobody minted is dereferenced
  // without it.
  if (here == nullptr || here->settlement == kNoObject) return no;

  // 1-based, and out of range refuses. The original decrements and then indexes
  // the player table with no bound at either end -- `[players + P*0x320 +
  // 0x12d4]` for a negative `P` reads below the table -- so this is the fault
  // rather than the behaviour, and the file's standing answer to that fault is
  // the one `fn_num_squads` gives.
  //
  // It is an **early-out and not a guard**: `PlayerTable::is_enemy` already
  // answers false for a `kNoPlayer` viewer, so the enemy test three lines down
  // would refuse anyway. Injected as a fault and survived, which is what says
  // so. It is here because it agrees with that refusal in words -- a player who
  // names nobody has no enemy, and no enemy is not "all of them are indoors".
  const PlayerId player = player_arg(ctx, 1);
  if (player == kNoPlayer) return no;

  const EconomySystem* economy = economy_of(*world);
  if (economy == nullptr) return no;
  const Settlement* set = economy->settlements().for_object(here->settlement);
  if (set == nullptr) return no;
  const PlayerTable& players = world->players();
  if (!players.is_enemy(set->owner, player)) return no;

  constexpr std::int32_t kHereAtAll = kAiStaying | kAiLeaving;
  for (const Squad& squad : heroes->squads().squads()) {
    if (squad.eval == 0) continue;
    if ((squad_relation(players, player, squad) & kAiEnemy) == 0) continue;
    if ((squad.flags & kSquadFlagPeaceful) != 0) continue;
    if ((squad_presence(node, squad) & kHereAtAll) == 0) continue;
    if (squad.members.empty()) continue;
    const WorldObject* first = world->find(squad.members.front());
    if (first == nullptr) continue;
    if (first->state.holder == kNoObject) return no;
  }
  return HostOutcome::ok_with(Value::boolean(true));
}

// --------------------------------------------------------------------------
// the squad former: `Squadize`, and the regrouper under it
// --------------------------------------------------------------------------
//
// **This is the first code in this tree that decides which army a body belongs
// to**, and that is worth saying at the top rather than in a footnote. Every
// other GAIKA and squad entry point here reads a membership somebody else
// built; this one builds one. `docs/plan.html` carries the decision to.
//
// ## Where the rule comes from
//
// `Squadize` (0x00425530) and its worker (0x00447330) are the small half, and
// **0x00447330 already has a body here**: `regroup_into_fresh_squads`, written
// for `SquadList::Train` and reused by the siege planner. The rule proper is
// `AddToSquad` (0x00446a70), about 1,900 bytes, which that worker calls once
// per listed object and which nothing here had needed until now.
//
// ## The three things the original tests that this engine cannot
//
// Named once, so that no reader has to wonder whether they were missed:
//
//   * **`[obj+0x194] & 0x800000`** refuses an object outright, before anything
//     else. It is one of the three `UnitFlags` bits `docs/formats/map.md` still
//     lists as open, and `World::populate_from_map` keeps only the two it can
//     name -- so no object here carries it and the refusal can never fire. It
//     is named rather than dropped, because the day the bit is understood this
//     is where it goes.
//   * **Bit 30 of `[obj+0x198]`/`[obj+0x19c]`**, a 64-bit per-object attribute
//     word this engine has no counterpart for. The merge predicate requires two
//     objects to *agree* on it, so with the word absent both sides read false
//     and the clause is always satisfied -- an equivalence given the word's
//     absence rather than a guess about the rule. What the bit means is not a
//     mystery, only unmodelled: its four readers all use it as an *exclusion*
//     beside `IsHeirOf("RamUnit")` (0x00427b29) or beside the hero test
//     (0x0044400c), and 0x004385e5 requires `IsHeirOf("Military")` **and** the
//     bit clear. It marks a body that does not count as an ordinary soldier.
//   * **`[squad+0xbc]`**, copied onto a new squad from the one an object came
//     from and raised to the maximum of the two on a merge. It has **no reader
//     anywhere in the installation** -- not one of the 885 scripts -- so nothing
//     observable depends on it and it is not carried.
//
// The two numbers in the rule -- a **256-unit** radius and a **ten-member** cap
// -- are read from the executable and are not inferences.

/// Defined further down beside `SquadList::Train`, which needed it first.
[[nodiscard]] std::uint16_t squad_flags_by_class(const World& world, ObjectId id);

/// Whether `id` is a hero: `[obj+0x2c] & 0x01000000`, which this tree already
/// names `kSyncHero` and measured as set on all 31 heroes in the corpus and on
/// nothing else. The regrouper asks it three times and it is the pivot of the
/// whole rule -- **a hero leads its own squad, and nothing ever merges into
/// one.**
[[nodiscard]] bool is_hero_object(const World& world, ObjectId id) noexcept {
  const WorldObject* slot = world.find(id);
  return slot != nullptr && slot->state.flags.is_hero;
}

[[nodiscard]] bool is_native(const World& world, ObjectId id, NativeClass base) noexcept {
  const WorldObject* slot = world.find(id);
  return slot != nullptr && slot->object != nullptr && slot->object->is_a(base);
}

/// The radius two bodies must be within to belong to one squad, in world units.
/// 0x00446d72 and 0x00446eca both take an integer square root (0x00417990) and
/// compare it against `0x100`. Two `LsaPartition` slots, which is small on
/// purpose: a squad is a knot of men, not a front.
inline constexpr std::int64_t kSquadRadius = 256;

/// `cmp dword ptr [cand+0x50], 0xa` at 0x00446dfa -- a squad already holding
/// ten takes no more.
inline constexpr std::size_t kSquadCapacity = 10;

/// The state a squad carries between step 1 and the second pass, 0x004474f7.
/// See `squadize_impl`: it is what stops a body joining a squad that existed
/// before the call, and it is never observable because the second pass writes
/// over it on every squad that reaches the caller.
inline constexpr std::int32_t kSquadizePlaceholder = 0xffff;

/// How deep the leader hand-off may recurse. **Not the original's**, which has
/// no such bound: it re-homes an emptied squad's followers one at a time and
/// each of those takes a path that does not recurse again. The cap is here
/// because a body in `engine/core` may not grow its stack on hostile state, and
/// one level is what the rule can actually produce.
inline constexpr int kRehomeDepth = 1;

[[nodiscard]] bool within_squad_radius(const World& world, ObjectId a, ObjectId b) noexcept {
  const Point pa = world.resolve_position(a);
  const Point pb = world.resolve_position(b);
  const std::int64_t dx = static_cast<std::int64_t>(pa.x) - pb.x;
  const std::int64_t dy = static_cast<std::int64_t>(pa.y) - pb.y;
  // The original rounds through an integer square root and compares against
  // 256; comparing the square against 65,536 is the same predicate without the
  // rounding, and `engine/core` has no floating point to lose either way.
  return dx * dx + dy * dy <= kSquadRadius * kSquadRadius;
}

/// Move `id` from whatever squad it is in into `key`, keeping `UnitRecord`'s
/// copy of the membership in step.
///
/// **The record matters and the table alone is not enough**: `UnitRecord::squad`
/// is folded into the world hash (`HeroSystem::hash`), so a unit the table moved
/// and the record did not is a divergence rather than a cosmetic drift.
/// `regroup_into_fresh_squads` maintains the same pair for the same reason.
void move_member(HeroSystem& heroes, ObjectId id, SquadKey key) {
  SquadTable& table = heroes.squads();
  const SquadKey was = table.squad_of(id);
  if (was == key) return;
  if (was != kNoSquad) (void)table.leave(was, id);
  (void)table.join(key, id);
  if (UnitRecord* record = heroes.unit(id); record != nullptr) record->squad = key;
}

/// A unit attached to a hero is not moved, by anything here.
///
/// The original has no such case to make, because there a hero's army *is* its
/// squad and moving a unit out of it is the detachment. Here the two are
/// separate structures -- `HeroRecord::army` and the squad table -- and moving
/// one without the other leaves a unit listed in an army it is not in.
/// `regroup_into_fresh_squads` refuses attached units for exactly this reason
/// and this agrees with it rather than inventing a second answer.
[[nodiscard]] bool is_attached(const HeroSystem& heroes, ObjectId id) noexcept {
  const UnitRecord* record = heroes.unit(id);
  return record != nullptr && record->hero != kNoObject;
}

SquadKey add_to_squad(World& world, HeroSystem& heroes, ObjectId id, GaikaId dest, int depth);

/// The hero arm (0x00446ad7): **a hero's squad is its army**, which
/// `sim/hero.hpp` measured over the 24 heroes in the dumps long before anything
/// here could create a squad -- every member of a hero's army prints the hero's
/// squad, and the squad is exactly `1 + army size`, 24 of 24. The original says
/// the same thing from the other side: a hero with no squad gets one and takes
/// its whole carried deque into it, and a hero that already has one keeps it and
/// only learns the new destination.
///
/// Here a registered hero always has one -- `HeroSystem::register_hero` mints
/// it -- so only the second half runs for it. The first is what a hero a
/// script placed after `start` gets when it is squadded before anything has
/// registered it, and `register_hero` adopts that squad when it does.
SquadKey hero_squad(World& world, HeroSystem& heroes, ObjectId id, GaikaId dest) {
  SquadTable& table = heroes.squads();
  const SquadKey existing = table.squad_of(id);
  if (existing != kNoSquad) {
    if (Squad* squad = table.find(existing); squad != nullptr) squad->dest_gaika = dest;
    return existing;
  }
  const WorldObject* slot = world.find(id);
  if (slot == nullptr) return kNoSquad;
  const SquadKey made = table.create(slot->state.owner, id);
  if (Squad* squad = table.find(made); squad != nullptr) {
    squad->state = 0;
    squad->flags = static_cast<std::uint16_t>(
        (slot->state.flags.no_ai ? kSquadFlagNoAi : 0) | squad_flags_by_class(world, id));
    squad->src_gaika = kNoGaika;
    squad->gaika_in = kNoGaika;
    squad->dest_gaika = dest;
    squad->state_time = world.time();
  }
  if (const HeroRecord* record = heroes.hero(id); record != nullptr) {
    // A copy: `move_member` writes the table, and the army is read through the
    // record it also writes.
    const std::vector<ObjectId> army = record->army;
    for (const ObjectId member : army) {
      if (member != id) move_member(heroes, member, made);
    }
  }
  return made;
}

/// Whether `candidate` will take `id`. Nine clauses, in the original's order
/// (0x00446dca onwards), every one of them read rather than inferred -- and see
/// the block comment at the head of this section for the tenth, which this
/// engine has no word to ask.
[[nodiscard]] bool squad_accepts(const World& world, const Squad& candidate, ObjectId id,
                                 GaikaId dest, std::int32_t state, std::uint16_t flags,
                                 GaikaId ai_dest) {
  if (candidate.dest_gaika != dest) return false;
  if (candidate.state != state) return false;
  if (candidate.flags != flags) return false;
  if (candidate.members.size() >= kSquadCapacity) return false;
  if (squad_ai_dest(candidate) != ai_dest) return false;
  if (candidate.members.empty()) return false;
  // The *front of the member deque* (0x00443df0), which is what this family
  // means by a leader everywhere: `Squad::leader` is a separate field that a
  // squad built by `join` alone does not carry.
  const ObjectId leader = candidate.members.front();
  if (leader == id) return false;
  if (is_hero_object(world, leader)) return false;
  if (is_native(world, leader, NativeClass::ship)) return false;
  if (is_native(world, leader, NativeClass::druid) != is_native(world, id, NativeClass::druid)) {
    return false;
  }
  return within_squad_radius(world, id, leader);
}

/// `AddToSquad(Obj, GAIKA dest)` -- 0x00446a70, and the whole of the rule.
///
/// The arms are the original's, in its order, and each is a different answer to
/// *where does this body belong*:
///
///   1. an object carrying the unnamed `UnitFlags` bit belongs nowhere -- a
///      named no-op here; see the head of this section;
///   2. a **hero** leads its own squad and takes its army with it;
///   3. a unit **inside a holder** belongs wherever its holder belongs, and to
///      nothing at all when the holder belongs to nothing: a garrison moves as
///      the building's, a crew as the ship's;
///   4. a unit **already close enough to its own leader**, in a squad already
///      pointed where the caller wants it and not leading that squad itself,
///      stays exactly where it is;
///   5. a **ship** never merges, and neither does a member of a **locked**
///      squad -- `kSquadLocked`, whose two readers `sim/squad.hpp` named as
///      0x00446c36 and 0x00446d89 before anything here consulted the bit. These
///      are those two sites, and this is the day the header was waiting for;
///   6. otherwise the first squad of the player's that will take it, scanned in
///      key order -- and when the object was leading a squad, everything it
///      leaves behind is re-homed after it;
///   7. and failing all of that, a leader keeps its squad and a follower starts
///      one of its own.
SquadKey add_to_squad(World& world, HeroSystem& heroes, ObjectId id, GaikaId dest, int depth) {
  SquadTable& table = heroes.squads();
  const WorldObject* slot = world.find(id);
  if (slot == nullptr) return kNoSquad;

  // 2.
  if (slot->state.flags.is_hero) return hero_squad(world, heroes, id, dest);

  // 3. A held unit follows its holder. The original reads `[obj+0x170]`, the
  //    holder handle, and answers **null** when the holder is in no squad --
  //    not "leave it where it is". `Squadize` then dereferences that null at
  //    0x0044756c (`mov ecx, [eax+0x50]`), which is a latent crash there and is
  //    the one place this engine deliberately does not follow: `kNoSquad` comes
  //    back, the caller lists nothing, and the body stays in the fresh squad
  //    the walk gave it -- which is where the original leaves it too, since it
  //    never takes that squad away either.
  if (slot->state.holder != kNoObject) {
    const SquadKey holders = table.squad_of(slot->state.holder);
    if (holders == kNoSquad) return kNoSquad;
    if (!is_attached(heroes, id)) move_member(heroes, id, holders);
    table.prune_empty();
    return table.squad_of(id);
  }

  const SquadKey mine = table.squad_of(id);
  const Squad* current = table.find(mine);
  const ObjectId leader =
      current != nullptr && !current->members.empty() ? current->members.front() : kNoObject;
  const bool leads = leader == id;

  const GaikaId ai_dest = current != nullptr ? squad_ai_dest(*current) : kNoGaika;
  const std::int32_t state = current != nullptr ? current->state : 0;
  // A unit in no squad starts from its class (0x00446c6a): `SF_PEACEFUL` for a
  // `Peaceful` or `Animal`, `SF_SENTRIES` for a `Sentry`, plus `SF_NOAI` from
  // the unit's own flag (0x00446c8e). This used to read 0, which `Squadize`
  // could never see -- it mints a fresh squad first -- and the spawn hook can:
  // a deer squadded at placement with flags 0 would be an army to the census.
  const std::uint16_t flags =
      current != nullptr
          ? current->flags
          : static_cast<std::uint16_t>((slot->state.flags.no_ai ? kSquadFlagNoAi : 0) |
                                       squad_flags_by_class(world, id));
  const GaikaId src = current != nullptr ? current->src_gaika : kNoGaika;
  const bool locked = current != nullptr && (current->flags & kSquadLocked) != 0;

  // A *follower* of a locked squad is left alone at once (0x00446c36).
  //
  // **Unreachable from `Squadize`, and kept anyway.** Every body that reaches
  // here through the one caller this engine has arrives in the fresh squad step
  // 1 just minted, and a fresh squad is never locked. The clause is the
  // original's and is reachable *there*, where `AddToSquad` has other callers;
  // no test here can tell it from its absence, and the sweep says as much.
  if (!leads && locked) return mine;
  // And so is a unit attached to a hero, for the reason `is_attached` gives.
  if (is_attached(heroes, id)) return mine;

  // 4.
  if (current != nullptr && current->dest_gaika == dest && !leads && leader != kNoObject &&
      !is_hero_object(world, leader) && within_squad_radius(world, id, leader)) {
    return mine;
  }

  // 5. and 6.
  const bool may_merge = !is_native(world, id, NativeClass::ship) && !locked;
  if (may_merge) {
    for (const Squad& candidate : table.squads()) {
      if (candidate.key.player != slot->state.owner) continue;
      if (candidate.key == mine) continue;
      if (!squad_accepts(world, candidate, id, dest, state, flags, ai_dest)) continue;

      // The destination the orphans are re-homed towards is the *old squad's*
      // when the object was leading it, and the caller's otherwise: 0x00446ef4.
      const SquadKey target = candidate.key;
      const GaikaId onward = leads && current != nullptr ? current->dest_gaika : dest;
      std::vector<ObjectId> orphans;
      if (leads && current != nullptr) {
        orphans.assign(current->members.begin(), current->members.end());
      }
      move_member(heroes, id, target);
      if (depth < kRehomeDepth) {
        for (const ObjectId orphan : orphans) {
          if (orphan == id || world.find(orphan) == nullptr) continue;
          (void)add_to_squad(world, heroes, orphan, onward, depth + 1);
        }
      }
      table.prune_empty();
      return table.squad_of(id);
    }
  }

  // 7a. A leader keeps its squad, learns the destination, and hands the rest on.
  if (leads && current != nullptr) {
    const SquadKey kept = mine;
    if (Squad* squad = table.find(kept); squad != nullptr) squad->dest_gaika = dest;
    if (locked) return kept;
    std::vector<ObjectId> followers;
    if (const Squad* squad = table.find(kept); squad != nullptr) {
      followers.assign(squad->members.begin(), squad->members.end());
    }
    if (depth < kRehomeDepth) {
      for (const ObjectId follower : followers) {
        if (follower == id || world.find(follower) == nullptr) continue;
        (void)add_to_squad(world, heroes, follower, dest, depth + 1);
      }
    }
    table.prune_empty();
    return table.squad_of(id);
  }

  // 7b. A follower nobody will take starts a squad of its own.
  const SquadKey made = table.create(slot->state.owner);
  move_member(heroes, id, made);
  if (Squad* squad = table.find(made); squad != nullptr) {
    squad->flags = flags;
    squad->state = state;
    squad->src_gaika = src;
    squad->dest_gaika = dest;
    squad->state_time = world.time();
  }
  table.prune_empty();
  return made;
}

}  // namespace

SquadKey add_to_squad(World& world, HeroSystem& heroes, ObjectId id, GaikaId dest) {
  return add_to_squad(world, heroes, id, dest, 0);
}

/// See the declaration. The four tests are 0x0041e820's, in its order: the
/// manager exists, the unit is alive (`vtbl+0x50`, `IsDead`), it is not
/// unspawned (`kSyncUnspawned`), it has an owner (`[obj+0x70]`). The
/// `[obj+0x194] & 0x800000` test beside them is a `UnitFlags` bit this engine
/// does not carry and no shipped map sets in its `UnitFlags=` attribute.
///
/// **The node is the one under the unit** (0x0044e650, `GetGAIKA(pos)`), and
/// a unit already in a squad is skipped rather than re-homed: the hook's own
/// re-home arm is for a unit that arrives already filed -- a loaded one --
/// and this engine restores squads whole.
///
/// Once a turn, over the ids the last turn had not seen, which is the same
/// "on the instant there, once a turn here" reading `Squad::GAIKAIn` and
/// `Squad::Eval` already live with; a script that places a unit and reads
/// `MilEval` on the same line sees last turn's number, and none does.
ObjectId enrol_new_units_in_squads(World& world, HeroSystem& heroes, ObjectId after) {
  const AiSystem* ai = ai_system_of(world);
  if (ai == nullptr || !ai->manager_started()) return after;
  ObjectId highest = after;
  // Copied: `add_to_squad` creates squads, and the object table is walked by
  // id rather than by reference for the same reason every walker here copies.
  std::vector<ObjectId> fresh;
  for (const WorldObject& slot : world.objects()) {
    if (slot.id > highest) highest = slot.id;
    if (slot.id <= after) continue;
    if (slot.object == nullptr || !slot.state.flags.is_unit) continue;
    if (slot.state.health <= 0) continue;
    if (slot.state.flags.unspawned) continue;
    if (slot.state.owner == kNoPlayer) continue;
    fresh.push_back(slot.id);
  }
  const GaikaTable& nodes = world.gaika();
  const LsaPartition& areas = world.lsa();
  for (const ObjectId id : fresh) {
    if (heroes.squads().squad_of(id) != kNoSquad) continue;
    const GaikaId under = nodes.at(areas, world.resolve_position(id));
    (void)add_to_squad(world, heroes, id, under, 0);
  }
  return highest;
}

namespace {


/// `squad.InvadeThroughGate(gate, nState)` -- 1 site, in `GS_SIEGE.VS`, and
/// **the last name standing between a 111-site strategy and its run**.
///
/// The site is the branch a besieging squad takes when the wall has already
/// given way: `else if (state == SS_Enter) squad.InvadeThroughGate(gate,
/// SS_Enter);`. Everything in it is an order, and the order is the same two
/// verbs the human player would give -- walk to the hole, then take the town.
///
/// ## Five guards, and any one of them is silence
///
/// 0x004299e0 does nothing at all unless:
///
///   1. the gate argument resolves to an object;
///   2. that object is a **building** (`[obj+0x2c] & 0x00800000`, `kSyncBuilding`);
///   3. it is **very broken** -- `health < maxhealth / 20`, the identical
///      five-instruction sequence `Building::IsVeryBroken` (0x00424f90) runs
///      and now shares with it as `is_very_broken`, and the
///      script has *already* asked that question one line above. Asking twice
///      is the original's, not a slip: `IsVeryBroken` is what chose `SS_Enter`
///      and this is the entry point refusing to act on a gate that has been
///      repaired since;
///   4. the squad handle names a live squad;
///   5. the squad does **not** carry `SF_NOAI`. A squad a mission script owns
///      is not marched anywhere by the AI.
///
/// ## What it does
///
/// The squad takes the caller's state and the current time, and then **every
/// member** gets its queue replaced with two orders: `advance` on the *gate's
/// position*, and `capture` on the **gate's settlement's central building** --
/// `[gate+0x148]` is the object's settlement, the field `Building::settlement`
/// reads, and `[settlement+0x8c]` its central building. A member whose class
/// binds neither verb gets neither; the original resolves each through
/// 0x005aef20 and skips a null.
///
/// Two details in the walk are the original's and are reproduced:
///
///   * **it stops at a hero.** After ordering a member, if that member carries
///     `kSyncHero` the walk ends and the rest of the squad gets nothing
///     (0x00429bcb). A hero leads; whoever is behind it in the deque is not
///     ordered separately.
///   * **the lock is temporary.** `kSquadLocked` is raised before the walk and
///     the *pre-walk* flags word is written back after it, on every exit path
///     including the early one where the settlement cannot be found. It is a
///     re-entrancy guard for the duration of the orders and not a lasting mark,
///     which is what makes it invisible to `Squad::GetFlags` afterwards.
///
/// **One write is not reproduced.** Each ordered member gets
/// `[unit+0x194] |= 0x80000`, bit 19 of the map's `UnitFlags` word --  one of
/// the bits `docs/formats/map.md` still lists as open, and one this engine's
/// loader does not keep. Nothing here can carry it and nothing here would read
/// it; it is named so that the day the bit is understood this is one of its
/// writers.
HostOutcome invade_through_gate_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  const auto done = HostOutcome::ok_void();

  const WorldObject* gate =
      ctx.count() > 1 && ctx.arg(1).is_object() ? world->find(ctx.arg(1).as_object().id) : nullptr;
  if (gate == nullptr) return done;
  if (!gate->state.flags.is_building) return done;
  // `IsVeryBroken`, shared so that the two guards cannot drift: this is the
  // identical five-instruction sequence at 0x00429a41 and 0x00424fbf.
  if (!is_very_broken(*world, *gate)) return done;

  const SquadKey key = unpack_squad(ctx.arg(0));
  Squad* squad = heroes->squads().find(key);
  if (squad == nullptr) return done;
  if ((squad->flags & kSquadFlagNoAi) != 0) return done;

  squad->state = ctx.count() > 2 && ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  squad->state_time = world->time();

  const EconomySystem* economy = economy_of(*world);
  const Settlement* set =
      economy == nullptr ? nullptr : economy->settlements().for_object(gate->settlement);
  // The lock goes up and comes straight back down; with no settlement to
  // capture there is nothing to walk and nothing to undo, so it is never
  // raised here at all. The original raises it one instruction earlier and
  // restores it on this path too, which is the same word either way.
  if (set == nullptr || set->anchor == kNoObject) return done;

  CommandSystem* commands = command_system(*world);
  if (commands == nullptr) return done;

  const Point at = world->resolve_position(gate->id);
  const std::vector<ObjectId> members(squad->members.begin(), squad->members.end());
  for (const ObjectId member : members) {
    if (world->find(member) == nullptr) continue;
    if (!commands->script_for(*world, member, "advance").empty()) {
      Command advance;
      advance.arg_kind = CommandArgKind::point;
      advance.point = at;
      (void)commands->set_command(*world, member, "advance", advance);
    }
    if (!commands->script_for(*world, member, "capture").empty()) {
      Command capture;
      capture.arg_kind = CommandArgKind::object;
      capture.object = set->anchor;
      (void)commands->add_command(*world, member, false, "capture", capture);
    }
    const WorldObject* slot = world->find(member);
    if (slot != nullptr && slot->state.flags.is_hero) break;
  }
  return done;
}


/// `bool squad.UseTeleport(tel, nState, cmd, pt)` -- 1 site, in
/// `AIOSENDSQUAD.VS`, and the one entry point in this family that gives a squad
/// a *three-order* itinerary.
///
///     if (teleport.IsValid) l.Cur.UseTeleport(teleport, state, cmd, pt);
///     else                  l.Cur.SetCmd(state, 0, SF_ADVCHOOSER, cmd, pt);
///
/// The two arms are the same journey with and without a shortcut, which is why
/// they share `state`, `cmd` and `pt`: walk to the far node, or walk to the
/// teleport, step through it, and walk on from there.
///
/// ## Three guards, and each answers false
///
/// The teleport argument must resolve and must be a **`CVXTeleport`** -- a
/// class test, unlike `GetEnterPoint`'s receiver, which takes any object; the
/// squad handle must name a live squad; and that squad must have a first
/// member. The `cmd` string is released on every one of those paths, which is
/// how the original says they are ordinary answers rather than failures.
///
/// ## What each member is given
///
/// The squad takes the caller's state and the clock, `kSquadLocked` goes up for
/// the walk and the pre-walk flags word is written back after it -- the same
/// temporary lock `InvadeThroughGate` uses and for the same reason. Then, per
/// member, its queue is replaced with up to three orders:
///
///   1. **`cmd` at the teleport's own door**, and only when `cmd` is one of
///      `move`, `advance` or `sneak`. 0x004298ac compares against exactly those
///      three, one fixed-length `repe cmpsb` each, and any other verb skips
///      this order entirely -- the squad is teleported without being walked to
///      the teleport first, which is the original's behaviour and not an
///      oversight: `AIOSENDSQUAD.VS` only ever passes one of the three.
///   2. **`teleport`**, with the teleport object as its argument.
///   3. **`cmd` again, at the caller's `pt`** -- where the squad was going.
///
/// The walk **stops at a hero**, exactly as `InvadeThroughGate`'s does, and
/// each ordered member gets the same unreproduced `[unit+0x194] |= 0x80000`.
///
/// ## The door
///
/// Order 1's point is `[tel+0x20c]` plus the nearest element of the object's
/// **type-1 point list** to the squad's leader (0x005cda00 and 0x004db650),
/// which is `GetEnterPoint` exactly -- so the two share `enter_point_near`
/// rather than each deriving the list. One difference is worth naming: the
/// original passes a literal **0** for the water/land selector here where
/// `GetEnterPoint` passes "is the unit a ship", so a teleport's door is always
/// taken off the land list however the leader swims.
///
/// **That it is the *leader* rather than any other member is not pinned by a
/// test**, and the sweep says so. Measuring it needs a teleport with an
/// authored door list, which needs a height layer for the round trip to keep a
/// point at all, and the GAIKA bench has none; where the layers do exist there
/// is no teleport. The picker and the list selector are covered there
/// (`entrance_enter_point_near_picks_the_nearest_of_the_chosen_list`); the
/// choice of unit is read from 0x004297ba and written here, and that is all.
HostOutcome use_teleport_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  const auto no = HostOutcome::ok_with(Value::boolean(false));

  const WorldObject* teleport =
      ctx.count() > 1 && ctx.arg(1).is_object() ? world->find(ctx.arg(1).as_object().id) : nullptr;
  if (teleport == nullptr || teleport->object == nullptr) return no;
  if (!teleport->object->is_a(NativeClass::teleport)) return no;

  Squad* squad = heroes->squads().find(unpack_squad(ctx.arg(0)));
  if (squad == nullptr || squad->members.empty()) return no;
  const ObjectId leader = squad->members.front();

  CommandSystem* commands = command_system(*world);
  if (commands == nullptr) return no;

  squad->state = ctx.count() > 2 && ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  squad->state_time = world->time();

  const std::string verb =
      ctx.count() > 3 && ctx.arg(3).is_string() ? std::string(ctx.arg(3).as_string()) : std::string();
  const Point onward = ctx.count() > 4 && is_point(ctx.arg(4)) ? unpack_point(ctx.arg(4)) : Point{};
  // The three verbs 0x004298ac compares against, and nothing else walks to the
  // teleport first.
  const bool walks = verb == "move" || verb == "advance" || verb == "sneak";
  const Point door = enter_point_near(*world, teleport->id, leader, false);

  const std::vector<ObjectId> members(squad->members.begin(), squad->members.end());
  for (const ObjectId member : members) {
    if (world->find(member) == nullptr) continue;
    // The first order issued replaces the queue -- the original aborts through
    // `vtbl+0xc0` before it queues anything -- and the rest are appended.
    bool replaced = false;
    const auto issue = [&](std::string_view name, const Command& what) {
      if (commands->script_for(*world, member, name).empty()) return;
      if (replaced) {
        (void)commands->add_command(*world, member, false, name, what);
      } else {
        (void)commands->set_command(*world, member, name, what);
        replaced = true;
      }
    };
    if (walks) {
      Command to_door;
      to_door.arg_kind = CommandArgKind::point;
      to_door.point = door;
      issue(verb, to_door);
    }
    {
      Command step;
      step.arg_kind = CommandArgKind::object;
      step.object = teleport->id;
      issue("teleport", step);
    }
    if (!verb.empty()) {
      Command onwards;
      onwards.arg_kind = CommandArgKind::point;
      onwards.point = onward;
      issue(verb, onwards);
    }
    const WorldObject* slot = world->find(member);
    if (slot != nullptr && slot->state.flags.is_hero) break;
  }
  return HostOutcome::ok_with(Value::boolean(true));
}

/// 32-bit products and sums, wrapping as the original's `imul`/`add` wrap.
///
/// Map coordinates are under 2^15 and node centres likewise, so nothing a
/// shipped map can hold makes these overflow; the wrap is kept so that the
/// side-of-line sign below is the original's sign in every case rather than in
/// every case that has come up.
[[nodiscard]] constexpr std::int32_t wrap32(std::int64_t v) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(v));
}
[[nodiscard]] constexpr std::int32_t mul32(std::int32_t a, std::int32_t b) noexcept {
  return wrap32(static_cast<std::int64_t>(a) * b);
}
[[nodiscard]] constexpr std::int32_t add32(std::int32_t a, std::int32_t b) noexcept {
  return wrap32(static_cast<std::int64_t>(a) + b);
}
[[nodiscard]] constexpr std::int32_t sub32(std::int32_t a, std::int32_t b) noexcept {
  return wrap32(static_cast<std::int64_t>(a) - b);
}

/// `bool sq.CalcGoAround` -- 2 sites, both in `SQUADMONITOR.VS`, and **the head
/// of the last reachable blocked script's list**.
///
/// Both sites are the same moment: a marching squad has run into more than it
/// can handle in the node it is crossing, its destination is still ahead, and
/// the monitor would rather detour than fight or flee.
///
///     SetMAIKA(sq.SrcGAIKA, sq.AIDest, sq.GAIKAIn);
///     if (AIVar(AIPlayer, AIV_GoAround) != 0)
///     if (sq.CalcGoAround) { Sleep(100); continue; }
///
/// ## What 0x0042e300 decides
///
/// The squad must resolve, must have a front member, must stand in a node with
/// at least one neighbour, and must be going somewhere: the destination is
/// `AIDest` (0x00444140, with `DestGAIKA` behind it -- `squad_ai_dest`, the
/// one reading), it must name a node, and it must not be the node the squad is
/// in. Any of those failing is **false**, and nothing has been touched.
///
/// Then, with `P` the front member's position, `C` the current node's centre
/// and `D` the destination's, every neighbour `N` of the current node is
/// scored and the best one that a script agrees to becomes the detour:
///
///   * **the side test.** `f(N) = (Px-Dx)*Ny + (Dy-Py)*Nx + (Dx*Py - Dy*Px)`,
///     which is `-cross(D-P, N-P)`: which side of the leader's line of march
///     to the destination `N` lies on. A neighbour is kept only when it lies on
///     the **same side as the current node's centre** -- `f(N) < 0` exactly
///     when `f(C) < 0`, zero counting as not-negative on both sides -- and the
///     test is skipped entirely when `f(C)` is zero, which is what it is when
///     the leader stands on the node's own centre;
///   * **the score.** `|(N-P) . (C-P)|`, the absolute dot product of the two
///     directions out of the leader: to the neighbour and to the centre of the
///     node it is in. The smallest wins, so the preferred detour is the
///     neighbour most nearly **sideways** from where the squad stands. A
///     neighbour scoring **more** than the best so far is dropped; one scoring
///     the same is kept and replaces it, so a tie goes to the later neighbour
///     in the table's order;
///   * **the script.** A candidate that survives both is put to
///     `CheckMAIKA.vs(idPlayer, gSrc, gDst, gMAIKA)` -- the squad's owner
///     1-based, its `SrcGAIKA`, the destination, and the neighbour -- through
///     the same trampoline `GetEconomyScript` and `MinNeed` use (0x0043d670
///     beside 0x0043d740), resolved on the owner's AI search path. The shipped
///     script asks the neighbour's own garrison need and answers false when
///     the squad cannot meet it, raising the neighbour's priority as it goes;
///     a **false** drops the candidate. That the script runs *after* the score
///     test and only for candidates still in the running is load-bearing: it
///     has side effects, and they land in this order.
///
///     The trampoline answers **true when the script cannot be run at all**
///     (0x0043d70c: a non-zero status from the runner is `1`), which is the
///     opposite of the `int` trampoline beside it and is reproduced for a
///     missing file. A file that is there and traps is reported as this
///     call's failure, which is `run_script_now`'s rule everywhere.
///
/// No survivor is **false**. So is a survivor that is the squad's `SrcGAIKA`
/// -- going back is not going around -- or the destination itself: the way
/// round would be the way there.
///
/// ## What it does
///
/// `SrcGAIKA` is overwritten with the detour node, which is how the monitor's
/// `SetMAIKA(sq.SrcGAIKA, ...)` on the next pass records the detour as the
/// way-point. Then `kSquadLocked` goes up, every member is walked in deque
/// order, and the pre-walk flags word is written back afterwards -- the same
/// temporary lock `InvadeThroughGate` and `UseTeleport` hold, for the same
/// reason. Per member:
///
///   1. a `move` to the detour node's centre is built from the member's class
///      (0x005aef20 with one point); a class that binds no `move` gets nothing
///      and does **not** end the walk, because the hero test below is only
///      reached by a member that was ordered;
///   2. the member's **running command is rebuilt** from its kind
///      (0x005ae130 with 0, then three type tests): a plain verb as a plain
///      verb, a point-verb re-aimed at its point, an object-verb re-aimed at
///      its object. A runner of any other shape, or no runner, rebuilds to
///      nothing;
///   3. the pending tail is dropped, the `move` and then the rebuilt runner
///      are appended, and the runner is ended -- `vtbl+0xc0(0)`, `vtbl+0xbc`
///      twice, 0x005b07d0 with 0 -- so `[run, rest...]` becomes
///      `[move, run']`: *step aside first, then carry on with what you were
///      doing*. `SneakCommand` is the same idea the other way round;
///   4. the walk **stops at a hero**, exactly as the other two walkers' do.
///
/// Each ordered member gets the same unreproduced `[unit+0x194] |= 0x80000`
/// the other walkers name. The answer is **true**.
///
/// Here `set_command` is the drop-and-append of the `move` and `add_command`
/// the append of the rebuilt runner, which is a copy of the queue's front less
/// its id and coroutine -- `sneak_command_impl`'s clone, taken before the queue
/// is replaced.
HostOutcome calc_go_around_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  const auto no = HostOutcome::ok_with(Value::boolean(false));
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return no;
    return HostOutcome::failed(self.error);
  }
  World& world = *self.world;
  Squad& squad = *self.squad;
  if (squad.members.empty()) return no;
  const ObjectId leader = squad.members.front();
  if (world.find(leader) == nullptr) return no;

  const GaikaTable& nodes = world.gaika();
  const GaikaId here = squad.gaika_in;
  const std::span<const GaikaId> around = nodes.neighbours(here);
  if (around.empty()) return no;
  const GaikaId target = squad_ai_dest(squad);
  const GaikaNode* dst = nodes.find(target);
  if (dst == nullptr || target == here) return no;
  // `neighbours` is non-empty only for an id that names a node.
  const GaikaNode& cur = *nodes.find(here);

  const Point p = world.resolve_position(leader);
  const std::int32_t px = p.x, py = p.y;
  const std::int32_t cx = cur.center.x, cy = cur.center.y;
  const std::int32_t dx = dst->center.x, dy = dst->center.y;
  // f(N) = (Px-Dx)*Ny + (Dy-Py)*Nx + (Dx*Py - Dy*Px), in the original's
  // 32-bit arithmetic.
  const std::int32_t fa = sub32(px, dx);
  const std::int32_t fb = sub32(dy, py);
  const std::int32_t fc = sub32(mul32(dx, py), mul32(dy, px));
  const auto side = [&](std::int32_t nx, std::int32_t ny) {
    return add32(add32(mul32(fa, ny), mul32(fb, nx)), fc);
  };
  const std::int32_t s = side(cx, cy);
  // |(N-P).(C-P)| = |Nx*(Cx-Px) + Ny*(Cy-Py) + (Px*(Px-Cx) - Py*(Cy-Py))|.
  const std::int32_t ga = sub32(cx, px);
  const std::int32_t gb = sub32(cy, py);
  const std::int32_t gc = sub32(mul32(px, sub32(px, cx)), mul32(py, gb));
  const auto score = [&](std::int32_t nx, std::int32_t ny) {
    const std::int32_t d = add32(add32(mul32(nx, ga), mul32(ny, gb)), gc);
    return d < 0 ? wrap32(-static_cast<std::int64_t>(d)) : d;
  };

  const PlayerId owner = squad.key.player;
  GaikaId best = kNoGaika;
  std::int32_t best_score = 0;
  for (const GaikaId id : around) {
    const GaikaNode* node = nodes.find(id);
    if (node == nullptr) continue;
    const std::int32_t nx = node->center.x, ny = node->center.y;
    if (s != 0 && ((side(nx, ny) < 0) != (s < 0))) continue;
    const std::int32_t d = score(nx, ny);
    if (best != kNoGaika && d > best_score) continue;
    // `CheckMAIKA.vs = bool, int idPlayer, GAIKA gSrc, GAIKA gDst, GAIKA gMAIKA`.
    const std::uint32_t chunk = ai_script_chunk(ctx, owner, "CheckMAIKA.vs");
    if (chunk != script::kNoChunk) {
      const Value args[4] = {Value::integer(owner == kNoPlayer ? 0 : owner + 1),
                             gaika_value(squad.src_gaika), gaika_value(target), gaika_value(id)};
      Value answer = Value::boolean(true);
      const HostOutcome ran = run_script_now(ctx, chunk, args, "CalcGoAround: CheckMAIKA.vs failed", answer);
      if (ran.status != HostStatus::ok) return ran;
      if (!answer.truthy_scalar()) continue;
    }
    best = id;
    best_score = d;
  }
  if (best == kNoGaika) return no;
  if (best == squad.src_gaika || best == target) return no;
  squad.src_gaika = best;

  CommandSystem* commands = command_system(world);
  if (commands == nullptr) return HostOutcome::ok_with(Value::boolean(true));
  const Point step = nodes.find(best)->center;
  // The lock is held for the walk and the pre-walk word restored after it; a
  // script the walk spawns sees it up.
  const std::uint16_t before = squad.flags;
  squad.flags |= kSquadLocked;
  const std::vector<ObjectId> members(squad.members.begin(), squad.members.end());
  for (const ObjectId member : members) {
    if (world.find(member) == nullptr) continue;
    if (commands->script_for(world, member, "move").empty()) continue;
    // The runner, rebuilt from its kind, before the queue is replaced. The
    // original's three type tests are the three values `CommandArgKind` has,
    // so every runner here has a shape it rebuilds.
    std::optional<Command> resume;
    if (const CommandQueue* q = commands->find(member); q != nullptr && !q->entries.empty()) {
      resume = q->entries.front();
    }
    Command aside;
    aside.arg_kind = CommandArgKind::point;
    aside.point = step;
    (void)commands->set_command(world, member, "move", aside);
    if (resume.has_value()) {
      const std::string verb = resume->verb;
      (void)commands->add_command(world, member, false, verb, *resume);
    }
    const WorldObject* slot = world.find(member);
    if (slot != nullptr && slot->state.flags.is_hero) break;
  }
  // `squads().find` again rather than `squad`: the walk ran scripts, and a
  // script can create a squad, which can move the table under a reference.
  if (Squad* again = self.heroes->squads().find(squad.key); again != nullptr) {
    again->flags = before;
  }
  return HostOutcome::ok_with(Value::boolean(true));
}

/// `GAIKA NearestHospital(sq)` -- 1 site, in `SQUADMONITOR.VS`, where a squad
/// that has decided to flee asks where to flee *to*:
///
///     GAIKA gFlee; gFlee = NearestHospital(sq);
///     if (gFlee == 0) { … EnvReadInt(AIPlayer, "HomeGaika") … }
///
/// ## What a hospital is
///
/// 0x004316b0 walks every node in id order and keeps the one whose settlement
/// passes four gates and scores lowest. The gates, in the original's order:
///
///   1. the node **has a settlement**;
///   2. the settlement is **the squad's own player's** -- `[[set+0x90]+8]`, the
///      owner's index, against the handle's low nibble;
///   3. its **holder has room**: `[holder+0x14] < [holder+0x10]`, the pair
///      `Settlement::IsFull` (0x005c1d20) reads as count and cap and this
///      engine keeps as `Holder::full`;
///   4. its **store holds 300 food or more**: `[[set+0x5c]+0x1c]`, the word
///      `Settlement::food` (0x005c2080) reads, against the literal 0x12c. A
///      hospital feeds the wounded.
///
/// A settlement failing any gate is not scored, and no message is printed.
///
/// ## The score
///
/// The **enemy strength staying in the node** is summed first: every squad
/// filed under the node whose strength is non-zero, whose owner the squad's
/// player counts an enemy (`test bl, 4` on 0x0044e250's answer), and whose
/// presence there carries `AI_STAYING` -- after the same memory adjustment
/// (0x0044eb80) the census runs, which is a no-op here for the census's
/// reason. **One exclusion differs from the census's.** A squad carrying
/// `SF_PEACEFUL` is left out only when its owner is player 14 or 15
/// (0x004319c6, 0x004319cb): the wildlife and the neutral-passive. A peaceful
/// squad of a real player counts, where `GAIKA::Eval` skips it on the flag
/// alone. The node's settlement query (0x00420c00) is then asked for its
/// garrison and the answer added when it reads as an enemy staying; the
/// settlement is the asking player's own by gate 2, so that term is
/// identically zero and is not reproduced.
///
/// A hospital whose node holds enemies worth **more than three quarters** of
/// the squad's own strength (`3 * eval / 4`, truncating) is passed over. What
/// remains is scored by distance:
///
///   * the plain distance from the squad's front member to the settlement's
///     central building, `isqrt(dx^2 + dy^2)` through the table at
///     0x0067c3d0, on 16-bit coordinates;
///   * **times 3/2, truncating, when the central building is a heir of
///     `BaseVillage`** (0x00440010) -- a village is a worse hospital than a
///     town by half again;
///   * **doubled when the enemies there are worth more than half** the
///     squad's strength (`eval / 2`, truncating): reachable, but not restful;
///   * **times the area-path length** -- 0x00441fe0 with an empty transport
///     string and no rendezvous, the same worker `CheckLsaPath` is, from the
///     area of the node the squad stands in to the hospital's, for the
///     squad's player. No path at all drops the hospital; a path of one area
///     leaves the distance alone; a crossing multiplies it by the areas
///     crossed, both ends counted. The worker's side effect -- bumping the
///     player's ship need on an unreachable sea -- is the worker's and
///     happens here as it happens there.
///
/// The lowest score wins and **a tie keeps the earlier node** (the skip is on
/// `best <= score`). The answer is that node's id, or 0 when nothing passed,
/// which the caller reads as "go home instead".
///
/// A receiver that names no squad, or a squad with no front member, answers
/// 0 without walking anything.
HostOutcome nearest_hospital_impl(CallContext& ctx) {
  const auto none = HostOutcome::ok_with(gaika_value(kNoGaika));
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return none;
    return HostOutcome::failed(self.error);
  }
  World& world = *self.world;
  const Squad& squad = *self.squad;
  if (squad.members.empty()) return none;
  const ObjectId leader = squad.members.front();
  if (world.find(leader) == nullptr) return none;

  const EconomySystem* economy = economy_of(world);
  if (economy == nullptr) return none;
  const GaikaTable& nodes = world.gaika();
  const PlayerTable& players = world.players();
  AiSystem* ai = ai_system_of(world);
  const PlayerId player = squad.key.player;
  const Point at = world.resolve_position(leader);
  const std::int32_t strength = squad.eval;
  const GaikaNode* standing = nodes.find(squad.gaika_in);
  const LsaId from = standing == nullptr ? kNoLsa : standing->lsa;
  const ClassGraph* graph = world.class_graph();
  const ClassIndex village = graph == nullptr ? kNoClass : graph->find("BaseVillage");

  // `[squad+0x30] & 4` on a squad of player 14 or 15 -- the wildlife and the
  // neutral-passive -- and only there. See the header for how this differs
  // from the census.
  constexpr PlayerId kNeutralWildlife = 14;
  constexpr PlayerId kNeutralPassive = 15;
  constexpr std::int32_t kHospitalFood = 300;

  GaikaId best = kNoGaika;
  std::int32_t best_score = 0;
  for (GaikaId id = 1; id <= nodes.count(); ++id) {
    const GaikaNode* node = nodes.find(id);
    if (node == nullptr || node->settlement == kNoObject) continue;
    const Settlement* set = economy->settlements().for_object(node->settlement);
    if (set == nullptr) continue;
    if (set->owner != player) continue;
    if (set->holder.full()) continue;
    if (set->warehouse.food < kHospitalFood) continue;

    std::int32_t enemy = 0;
    for (const Squad& other : self.heroes->squads().squads()) {
      // The original's first test, and an equivalence here: a strengthless
      // squad adds nothing to a sum. Kept because it is what 0x004318f1 does
      // and because the sum is the only thing the walk produces.
      if (other.eval == 0) continue;
      if ((relation_between(players, player, other.key.player) & kAiEnemy) == 0) continue;
      if ((squad_presence(id, other) & kAiStaying) == 0) continue;
      if ((other.flags & kSquadFlagPeaceful) != 0 &&
          (other.key.player == kNeutralWildlife || other.key.player == kNeutralPassive)) {
        continue;
      }
      enemy += other.eval;
    }
    if (enemy > (3 * strength) / 4) continue;

    // The central building's position (0x005c1060), which for a settlement
    // node is also the node's centre; the sweep says the two cannot be told
    // apart here, and this reads the one the original reads.
    const Point there = world.resolve_position(set->anchor);
    const std::int64_t dx = there.x - at.x;
    const std::int64_t dy = there.y - at.y;
    std::int32_t distance = static_cast<std::int32_t>(isqrt(dx * dx + dy * dy));
    // 0x00440010 is `IsHeirOf(central building, "BaseVillage")` by name, and
    // a graph without the name answers false -- a town, not a village.
    if (village != kNoClass && world.class_is_a(set->anchor, village)) {
      distance = (3 * distance) / 2;
    }
    if (enemy > strength / 2) distance *= 2;
    const LsaRoute route = lsa_route(world, ai, from, node->lsa, player);
    if (route.answer == 0) continue;
    const std::int32_t score = route.answer * distance;
    if (best != kNoGaika && best_score <= score) continue;
    best = id;
    best_score = score;
  }
  return HostOutcome::ok_with(gaika_value(best));
}

/// `int sq.EvalAttach(leader, min)` -- 1 site, in `SQUADMONITOR.VS`, where a
/// leaderless band standing in a node looks over the hero-led squads of its
/// own player standing there and scores each as somewhere to attach itself:
///
///     for (sq.GAIKAIn.GetSquads(SL, AI_STAYING + AI_LEAVING, AIPlayer, AI_OWN); …)
///       eval = sq.EvalAttach(SL.Cur.Leader, min);
///       if (eval == 0) continue;
///
/// ## Four refusals, each 0
///
/// 0x00424550 resolves the leader through the object table and answers 0
/// unless it exists, is a **hero** (`[obj+0x2c] & 0x01000000`), leads a squad
/// of **fifty or fewer** (`[squad+0x50]`, the deque's size, against 0x32), and
/// that squad is bound where the receiver is bound: `AIDest` against `AIDest`,
/// each through 0x00444140 with `DestGAIKA` behind it -- `squad_ai_dest`, the
/// one reading -- and two `kNoGaika`s are equal. A hero whose squad this
/// engine does not know is refused the same way, which is a case the original
/// cannot reach: a hero there is always in a squad.
///
/// ## The score
///
/// Two factors. The first is **how the hero's squad stands against `min`**,
/// the caller's floor on a squad worth joining:
///
///     n >= min :  n - min + 1
///     n <  min :  n - min + 100
///
/// -- so a squad already at the floor scores 1 and grows by one per body
/// above it, while one *short* of the floor scores 100 less the shortfall:
/// the under-strength squads are worth far more, and the nearer to full the
/// better. `min` is the script's `nMinSquadSize` and the two ranges never
/// meet on shipped data, where `min` is well under 50.
///
/// The second is the **hero's effective level** (`vtbl+0x114`, the slot
/// `effective_level_of` reads for `AttackSetForTraining`): `level / 10 + 1`,
/// truncating toward zero as the `imul`-by-0x66666667 sequence does. The two
/// multiply.
HostOutcome eval_attach_impl(CallContext& ctx) {
  const auto zero = HostOutcome::ok_with(Value::integer(0));
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return zero;
    return HostOutcome::failed(self.error);
  }
  World& world = *self.world;
  const WorldObject* hero =
      ctx.count() > 1 && ctx.arg(1).is_object() ? world.find(ctx.arg(1).as_object().id) : nullptr;
  if (hero == nullptr || !hero->state.flags.is_hero) return zero;
  const std::int32_t min = ctx.count() > 2 && ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;

  const SquadTable& table = self.heroes->squads();
  const Squad* theirs = table.find(table.squad_of(hero->id));
  if (theirs == nullptr) return zero;
  const std::int32_t n = static_cast<std::int32_t>(theirs->members.size());
  if (n > 50) return zero;
  if (squad_ai_dest(*self.squad) != squad_ai_dest(*theirs)) return zero;

  const std::int32_t standing = n >= min ? n - min + 1 : n - min + 100;
  const std::int32_t level = effective_level_of(world, hero->id);
  return HostOutcome::ok_with(Value::integer((level / 10 + 1) * standing));
}

/// `int sq.FoodComing` and `bool sq.SendFoodWagon(amount, range)` -- one site
/// each, five lines apart in `SQUADMONITOR.VS`, and **the last two names
/// between the monitor and its run**:
///
///     dist = sq.pos.Dist(sq.AIDest.Center) + 200;
///     food = sq.food + sq.FoodComing;
///     needed = sq.Size * dist / 700;
///     if (food + 100 < needed)
///     if (sq.SendFoodWagon(needed - food, 3000)) …
///
/// ## What the original keeps, and what this engine keeps instead
///
/// There a food wagon is a **unit**: a `Wagon`-class peasant with a cargo
/// (`[obj+0x1cc]`) and a kind (`[obj+0x1d4]`, 1 for food), standing in one of
/// its player's peaceful squads, running `follow` on the soldier it was sent
/// after. `FoodComing` (0x0042a5b0) walks the player's squads, takes the
/// peaceful ones, and for every member that is a wagon carrying food whose
/// running command is an object-command aimed at a **unit in the asking
/// squad** (`[unit+0x174]` against the handle), adds the cargo.
///
/// Here a wagon in flight is a row in `EconomySystem::wagons()` with a build
/// timer and no object -- the journey is abstracted away for every wagon the
/// economy moves -- and `Wagon::follow` is the unit it was sent after. So the
/// walk is over that table: every food wagon whose `follow` is currently a
/// member of this squad, built or not, counts its cargo. That is the same
/// membership test, read off the squad rather than off the unit, and the same
/// sum.
///
/// ## `SendFoodWagon`, in the original's order
///
/// 0x0042a750 answers false for a receiver naming no squad or a squad with no
/// front member. Then, with the front member's position:
///
///   1. **an existing wagon to redirect.** The same walk as above, keeping a
///      food wagon whose cargo is **not more than `amount`**, whose running
///      command is not aimed at a unit -- one already following somebody is
///      left alone -- and, when aimed at a building, only one whose settlement's
///      central building is a heir of `BaseTownhall` (0x0043ffc0); within
///      `range` of the leader, nearest first, strictly. **Not reproduced**: a
///      wagon here has no object and no command, so nothing could pass the
///      command test, and a phase that can select nothing is written down
///      rather than written;
///   2. **a settlement to load one at.** Every settlement the player owns
///      whose store holds **100 food or more** and whose central building is
///      within `range` of the leader; the nearest, strictly. No class test on
///      this pass -- a village will do;
///   3. **which.** No settlement: the wagon, if there was one. Both: the wagon
///      when the settlement is at least **1,200** further than it; otherwise
///      the settlement. With phase 1 empty the settlement always wins here,
///      and that arm of the decision is named rather than written;
///   4. the mule is loaded with `amount` **clamped to the store's food**
///      through the same body `CreateMuleFood` is (0x005eed90 with kind 1 and
///      the flag that picks the race's mule), and a load of nothing is false;
///   5. it is ordered **`follow`** on the leader (0x005aef20 with the unit
///      handle; a class binding no `follow` is false), the way every walker
///      here orders -- `vtbl+0xc0(0)`, `vtbl+0xbc`, 0x005b07d0 -- and the
///      answer is true.
///
/// Here step 4 is `create_feeding_mule`, which is `create_mule` with
/// `Wagon::follow` set and no destination, and step 5 is that field: the
/// wagon "follows" by being drawn down by the squad's hungry members once it
/// is built, through the feeder's second path. `max_load` is not applied,
/// which is `CreateMuleFood`'s standing here and is named there.
HostOutcome food_coming_impl(CallContext& ctx) {
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return HostOutcome::ok_with(Value::integer(0));
    return HostOutcome::failed(self.error);
  }
  const EconomySystem* economy = economy_of(*self.world);
  if (economy == nullptr) return HostOutcome::ok_with(Value::integer(0));
  const std::vector<ObjectId>& members = self.squad->members;
  std::int32_t total = 0;
  for (const Wagon& w : economy->wagons()) {
    // The kind test is the original's (`[wagon+0x1d4] == 1`) and an
    // invariant here: `create_feeding_mule` is the only writer of `follow`
    // and loads food. Injected as a fault it survives, which is what says so.
    if (!w.follows() || w.resource != Resource::food) continue;
    if (std::find(members.begin(), members.end(), w.follow) == members.end()) continue;
    total += w.amount;
  }
  return HostOutcome::ok_with(Value::integer(total));
}

HostOutcome send_food_wagon_impl(CallContext& ctx) {
  const auto no = HostOutcome::ok_with(Value::boolean(false));
  Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoSquadFound) return no;
    return HostOutcome::failed(self.error);
  }
  World& world = *self.world;
  const Squad& squad = *self.squad;
  // Both are the original's early-outs (0x0042a7cf) and both are covered by
  // `create_feeding_mule` refusing to follow nothing; an empty squad would
  // fall out false there too. Kept for the order of refusals, not for the
  // answer.
  if (squad.members.empty()) return no;
  const ObjectId leader = squad.members.front();
  if (world.find(leader) == nullptr) return no;
  EconomySystem* economy = economy_of(world);
  if (economy == nullptr) return no;
  const std::int32_t amount = ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  const std::int32_t range = ctx.count() > 2 && ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  const Point at = world.resolve_position(leader);
  constexpr std::int32_t kLoadableFood = 100;

  // Phase 2: the nearest own settlement with food to spare within range.
  const Settlement* source = nullptr;
  std::int64_t nearest = 0;
  for (const Settlement& set : economy->settlements().all()) {
    if (set.owner != squad.key.player) continue;
    if (set.warehouse.food < kLoadableFood) continue;
    const Point there = world.resolve_position(set.anchor);
    const std::int64_t dx = there.x - at.x;
    const std::int64_t dy = there.y - at.y;
    const std::int64_t distance = isqrt(dx * dx + dy * dy);
    if (distance > range) continue;
    if (source != nullptr && distance >= nearest) continue;
    source = &set;
    nearest = distance;
  }
  if (source == nullptr) return no;
  // The original's clamp, kept as its own step: `create_mule` takes what the
  // warehouse can pay anyway, so a fault here changes nothing, and the sweep
  // says so.
  const std::int32_t load = std::min(amount, source->warehouse.food);
  return HostOutcome::ok_with(
      Value::boolean(economy->create_feeding_mule(source->id, leader, load) != 0));
}

/// `Squadize(ObjList ol, SquadList sl, int nState)` -- 2 sites, both in
/// `GS_CAPTURE.VS`, and **the last name that stood between a reachable shipped
/// script and its run**.
///
/// Both sites are one idiom: gather the units a capture strategy wants fighting
/// rather than riding, hand them here, and order every squad that comes back.
///
///     Squadize(olOut, sl, SS_KillAll);
///     sl.Rewind; sl.Lock;
///     for (0; !sl.EOL; sl.Next)
///         sl.Cur.SetCmd(SS_KillAll, 0, SF_ADVCHOOSER, "ai_killall");
///     sl.Unlock;
///
/// ## The shape
///
/// The caller's `SquadList` is emptied first -- 0x00447337 destroys the old
/// contents rather than appending to them. Then, **per object and in list
/// order**:
///
///   1. give it a fresh single-member squad of its own, inheriting flags, both
///      GAIKAs and `StateSetTime` from whatever squad it was in and deriving
///      them from its class and its position when it was in none. That is
///      `regroup_into_fresh_squads`, which is this same 0x00447330 and already
///      had a body here;
///   2. hand it to `add_to_squad`, which is where the rule lives;
///   3. and append the squad that comes back **only when it has exactly one
///      member**, which is what makes the list the set of *seeds* rather than
///      one entry per body.
///
/// **The interleaving is load-bearing.** A fresh single-member squad is itself
/// a merge candidate, so the second object's search sees the first object's
/// settled squad. Making all the fresh squads first and regrouping afterwards
/// is a different function, and that is why step 1 runs one object at a time
/// rather than over the whole list.
///
/// Then every squad the list names takes the caller's `nState` and the current
/// time, in a second pass.
///
/// ## The placeholder is not decoration, and this note used to say it was
///
/// Step 1 stamps **`0xffff`** on every fresh squad (0x004474f7) and the second
/// pass overwrites it. Passing the caller's `nState` into step 1 instead looks
/// equivalent -- every squad the list names is a fresh one, so it ends up with
/// the same state either way -- and it is not, because the state is one of the
/// nine clauses the merge predicate tests. With `0xffff` in it, a body can
/// never merge into a squad that existed before this call: no real state is
/// `0xffff`. With `nState` in it, a body merges into any pre-existing squad
/// that happens to be in the state being asked for -- and `GS_CAPTURE.VS`
/// passes `SS_KillAll` over a map full of squads already in `SS_KillAll`, so
/// that is not a corner. **`Squadize` forms squads out of the bodies it was
/// handed and joins them to nothing else**, and the placeholder is what says
/// so. It was written the other way here first and the fault sweep found it.
HostOutcome squadize_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  HeroSystem* heroes = hero_system_of(*world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  if (ctx.count() < 2 || !is_objlist(ctx.arg(0)) || !is_squadlist(ctx.arg(1))) {
    return HostOutcome::failed("Squadize: expected an ObjList and a SquadList");
  }
  const SquadListId list = squadlist_of(ctx.arg(1));
  std::vector<SquadKey>* out = squadlist_pool_of(*world).mutable_items(list);
  if (out == nullptr) return HostOutcome::failed("Squadize: receiver is not a live SquadList");
  out->clear();
  squadlist_pool_of(*world).set_cursor(list, 0);

  const std::int32_t wanted =
      ctx.count() > 2 && ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  const GameTime now = world->time();

  // A copy: the walk creates and destroys squads, and one shipped site hands in
  // a list it goes on to read afterwards.
  const std::span<const ObjectId> items = objlist_pool_of(*world).items(objlist_of(ctx.arg(0)));
  const std::vector<ObjectId> bodies(items.begin(), items.end());

  SquadTable& table = heroes->squads();
  for (const ObjectId id : bodies) {
    const WorldObject* slot = world->find(id);
    if (slot == nullptr) continue;
    // `[obj+0x70]`, the owner record: an object nobody owns is skipped before
    // anything else, because the squad it would get would belong to nobody.
    if (slot->state.owner == kNoPlayer) continue;

    const ObjectId one[1] = {id};
    regroup_into_fresh_squads(*world, *heroes, std::span<const ObjectId>(one, 1),
                              kSquadizePlaceholder, now);

    // The destination the regrouper is asked for is the one step 1 just settled
    // on, read back rather than recomputed so the two cannot disagree.
    const Squad* fresh = table.find(table.squad_of(id));
    const GaikaId dest = fresh != nullptr ? fresh->dest_gaika : kNoGaika;

    const SquadKey landed = add_to_squad(*world, *heroes, id, dest, 0);
    const Squad* settled = table.find(landed);
    // Exactly one member, which is the original's whole test (0x00447572) and
    // needs no guard against listing a squad twice: a squad two bodies both
    // landed in has two members by the time the second one asks.
    if (settled == nullptr || settled->members.size() != 1) continue;
    out->push_back(landed);
  }

  for (const SquadKey key : *out) {
    if (Squad* squad = table.find(key); squad != nullptr) {
      squad->state = wanted;
      squad->state_time = now;
    }
  }
  return HostOutcome::ok_void();
}

/// The number of `define` calls `register_squad_host` makes. Kept next to the
/// list so the two cannot drift.
// --------------------------------------------------------------------------
// SquadList::Train, the settlement-entry strategy's one native decision
// --------------------------------------------------------------------------

/// `SL.Train(nTrainState, nEnterState, nStopOn, nStartOn, objEnter)` -- **one
/// call site, and it was the sole blocker of `GS_ENTERSETTLEMENT.VS`**, the
/// 67-site strategy an AI runs over a node whose settlement it means to fill.
/// The script gathers the squads it wants inside into a list and hands them
/// here with `slTrain.Train(SS_Train, SS_Enter, 4, 10, set.GetCentralBuilding)`;
/// what comes back is a code it only prints: 1 started training, 2 continues
/// it, 3 stopped it.
///
/// `SquadList::Train` (0x004366c0) is registered as `int, SquadList SL, int
/// nTrainState, int nEnterState, int nStopOn, int nStartOn, Obj objEnter`. It
/// is not an accessor: it sorts every unit of every listed squad into three
/// bins, decides from the bin sizes and the two thresholds who trains and who
/// goes inside, issues the commands, and regroups both sets into fresh squads
/// carrying the two states. In order:
///
/// ## The three bins
///
/// Every squad in the list whose flags carry `SF_NOAI` (bit 0 of
/// `[squad+0x30]`) is skipped whole; a member handle that resolves to nothing
/// is skipped. Each remaining unit goes to exactly one bin:
///
///   * **A, already training** (0x00429100): any command in the unit's queue
///     named `train`, `waitarmytrain` or `unittrain`. The last two are what
///     `HERO_TRAIN.VS` queues on a hero and its army.
///   * **B, able to train** (0x00425d60), all five in order: the player's
///     `maxtrainlevel` in the environment store, read **once per call** through
///     `/%s/Player%d/maxtrainlevel` and kept **less one**, is at least 1; the
///     unit has at least half its health (`health * 2 >= maxhealth`); its level
///     is **below** that lessened figure, so with `maxtrainlevel` 5 a level-4
///     unit does not train here where `UNIT_TRAIN.VS` would still let it; it is
///     not a `BaseMage`, whose `train` binding is `basemage_do_nothing.vs`; and
///     its class binds `train` at all (0x005aef20 resolves the verb on the
///     class and answers null otherwise).
///   * **C, the rest.**
///
/// The level is the one `Unit::inherentlevel` answers: the original's
/// 0x005d77a0 and this test both run the experience through 0x005d28c0, so
/// the two agree by construction there, and `HeroSystem::inherent_level` is
/// what that host reads here.
///
/// ## The decision
///
/// With `a` and `b` the bin sizes and `D` the units that will train, `E` the
/// units sent inside:
///
///   * `a == 0`: if `a + b >= nStartOn`, the answer is **1** and `D = B`;
///     otherwise nothing is decided.
///   * `a > 0` and `a + b > nStopOn`: if `b > 0`, the answer is **2** and
///     `D = B`; otherwise nothing is decided. The units already training are
///     left alone in both cases.
///   * `a > 0` and `a + b <= nStopOn`: the answer is **3** and `E = A + B` --
///     training is over, everybody goes inside.
///
/// Then **`E` gains all of `C`** whatever was decided, so the units that cannot
/// train are sent inside on every call.
///
/// ## What is done to each set
///
/// Every unit in `D` is detached from its hero first (0x005db190, `CVXUnit ::
/// DetachHero`). Every unit in `E` that is **not inside a holder**
/// (`[unit+0x154] == 0xffff`) and whose running command is not already named
/// `enter` gets `SetCommand("enter", objEnter)`; the first such order turns an
/// undecided answer into **4**, so the caller can tell "sent some inside" from
/// "did nothing". The `E` units are then regrouped under `nEnterState`, each
/// `D` unit is started (below) and the `D` units are regrouped under
/// `nTrainState`.
///
/// Starting a unit (0x004291d0) that is not already in `A`: its queue is
/// aborted, and a unit that is inside a holder is first sent to `centre +
/// vec_of_angle(radius + rand(64, 128), rand(0, 359))`, where the centre is
/// the position of the holder's settlement's central building and the radius
/// is that building's class `radius` (`[class+0x2dc]`, the attribute the
/// loader stores there at 0x005a1a8c). The two draws happen in that order for
/// every held unit. Then `train` is queued behind the move, or set outright
/// when there was none.
///
/// ## Regrouping (0x00447330)
///
/// Each unit leaves the squad it was in and gets a fresh squad of its own.
/// The new squad copies the old one's flags, `SrcGAIKA`, `DestGAIKA` and
/// `LastFightTime`; a unit with no squad starts from the flags its class
/// implies -- `SF_PEACEFUL` for a `Peaceful` or `Animal`, `SF_SENTRIES` for a
/// `Sentry`, nothing otherwise -- and `DestGAIKA` at the node under its feet.
/// Bit 0 is then rewritten from the unit's own no-AI flag and the lock bit
/// cleared. The state is applied in a final pass over the squads made, with
/// `state_time` set to now.
///
/// The original then runs its per-unit squad assignment (0x00446a70) on every
/// regrouped unit, and for a unit **still attached to a hero** that moves it
/// straight back into the hero's squad, which is then not one of the fresh
/// single-member squads the final pass writes the state to. So here a unit
/// attached to a hero stays where it is and takes no state, which is the same
/// place it ends up. **A hero keeps its own squad and that squad takes the
/// state** -- the one case the reading does not settle, because the
/// assignment's leader branch was not followed to its end; it is labelled
/// rather than guessed. `GAIKAIn`, `OrderDest`, `AIDest` and the last attacker
/// are left at their defaults on a fresh squad, where the original leaves
/// whatever the pooled slot held.
///
/// ## What the answer is not
///
/// 0 is a receiver that names no list, and an empty one. Nothing here spends
/// or consults gold: what training costs is the barracks' business.
struct TrainScan {
  World& world;
  HeroSystem& heroes;
  CommandSystem& commands;
  ClassIndex mage = kNoClass;
  /// `maxtrainlevel - 1`, resolved on the first unit that needs it.
  std::int32_t cap = -1;
  bool cap_known = false;
};

constexpr std::string_view kTrainingVerbs[] = {"train", "waitarmytrain", "unittrain"};

[[nodiscard]] bool same_verb(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    const char x = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] + ('a' - 'A')) : a[i];
    const char y = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] + ('a' - 'A')) : b[i];
    if (x != y) return false;
  }
  return true;
}

/// 0x00429100: a `train`-family command anywhere in the queue.
[[nodiscard]] bool is_training(const CommandSystem& commands, ObjectId unit) {
  const CommandQueue* queue = commands.find(unit);
  if (queue == nullptr) return false;
  for (const Command& command : queue->entries) {
    for (const std::string_view verb : kTrainingVerbs) {
      if (same_verb(command.verb, verb)) return true;
    }
  }
  return false;
}

/// 0x00425d60, in the original's order.
[[nodiscard]] bool can_train(TrainScan& scan, const WorldObject& slot) {
  if (!scan.cap_known) {
    const EnvSystem* env = env_of(scan.world);
    const std::int32_t declared =
        env == nullptr ? 0
                       : env->env().read_int(EnvScope::for_player(slot.state.owner), "maxtrainlevel");
    scan.cap = declared - 1;
    scan.cap_known = true;
  }
  // `< 1` is the original's test; a level is never below 0 here, so the level
  // comparison below excludes a cap of 0 on its own, and a fault that reads
  // `< 0` survives the suite as an equivalence.
  if (scan.cap < 1) return false;
  if (slot.state.health * 2 < scan.heroes.max_health_of(scan.world, slot.id)) return false;
  if (scan.heroes.inherent_level(slot.id) >= scan.cap) return false;
  if (scan.mage != kNoClass && scan.world.class_is_a(slot.id, scan.mage)) return false;
  return !scan.commands.script_for(scan.world, slot.id, "train").empty();
}

[[nodiscard]] std::int32_t class_int_of(const World& world, ObjectId id, std::string_view key) {
  const WorldObject* slot = world.find(id);
  const ClassGraph* graph = world.class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) return 0;
  const std::string_view text = graph->property(slot->class_index, key);
  std::int32_t out = 0;
  bool any = false;
  bool negative = false;
  std::size_t i = 0;
  while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
  if (i < text.size() && text[i] == '-') {
    negative = true;
    ++i;
  }
  for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
    out = out * 10 + (text[i] - '0');
    any = true;
  }
  if (!any) return 0;
  return negative ? -out : out;
}

/// The flags a unit with no squad starts from: 0x005402f0 (`Peaceful`, then
/// `Animal`) and 0x005400c0 (`Sentry`), each an `IsHeirOf` on the class.
[[nodiscard]] std::uint16_t squad_flags_by_class(const World& world, ObjectId id) {
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return 0;
  for (const std::string_view name : {"Peaceful", "Animal"}) {
    const ClassIndex index = graph->find(name);
    if (index != kNoClass && world.class_is_a(id, index)) return kSquadFlagPeaceful;
  }
  const ClassIndex sentry = graph->find("Sentry");
  if (sentry != kNoClass && world.class_is_a(id, sentry)) return kSquadFlagSentries;
  return 0;
}

/// 0x004291d0: abort, walk out of a holder to a drawn point, then `train`.
void start_training(TrainScan& scan, const WorldObject& slot) {
  // 0x004291d0's own guard. Unreachable from `Train`, whose trainees are by
  // construction the units that were *not* training; kept because the routine
  // is the original's and `HERO_TRAIN.VS`'s idiom reaches the same code.
  if (is_training(scan.commands, slot.id)) return;
  bool queue_replaced = false;
  if (slot.state.holder != kNoObject) {
    // Both draws happen for every held unit, in this order, before anything
    // asks whether the move can be issued.
    const std::int32_t reach = scan.world.rng().between(64, 128);
    const std::int32_t angle = scan.world.rng().between(0, 359);
    const EconomySystem* economy = economy_of(scan.world);
    const Settlement* set =
        economy == nullptr ? nullptr : economy->settlements().for_object(slot.state.holder);
    if (set != nullptr && set->anchor != kNoObject &&
        !scan.commands.script_for(scan.world, slot.id, "move").empty()) {
      const Point centre = scan.world.resolve_position(set->anchor);
      const Point offset =
          vec_of_angle(class_int_of(scan.world, set->anchor, "radius") + reach, angle);
      Command move;
      move.arg_kind = CommandArgKind::point;
      move.point = Point{centre.x + offset.x, centre.y + offset.y};
      (void)scan.commands.set_command(scan.world, slot.id, "move", move);
      queue_replaced = true;
    }
  }
  if (scan.commands.script_for(scan.world, slot.id, "train").empty()) {
    // The abort still happened in the original; with nothing to queue this
    // engine's queue refills with the default verb, which is what an aborted
    // queue does anyway.
    if (!queue_replaced) (void)scan.commands.kill_command(scan.world, slot.id);
    return;
  }
  const Command train;
  if (queue_replaced) {
    (void)scan.commands.add_command(scan.world, slot.id, false, "train", train);
  } else {
    (void)scan.commands.set_command(scan.world, slot.id, "train", train);
  }
}

HostOutcome train_impl(CallContext& ctx) {
  const ListSelf list = resolve_list(ctx);
  if (!list.ok()) {
    if (list.error == kNoWorld) return HostOutcome::failed(list.error);
    return HostOutcome::ok_with(Value::integer(0));
  }
  if (ctx.count() < 6 || !ctx.arg(1).is_integer() || !ctx.arg(2).is_integer() ||
      !ctx.arg(3).is_integer() || !ctx.arg(4).is_integer()) {
    return HostOutcome::failed("Train: expected four integers and an object");
  }
  World& world = *list.world;
  HeroSystem* heroes = hero_system_of(world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);
  CommandSystem* commands = command_system(world);
  if (commands == nullptr) return HostOutcome::failed(kNoCommands);
  const std::span<const SquadKey> keys = list.pool->items(list.id);
  if (keys.empty()) return HostOutcome::ok_with(Value::integer(0));

  const std::int32_t train_state = ctx.arg(1).as_integer();
  const std::int32_t enter_state = ctx.arg(2).as_integer();
  const std::int32_t stop_on = ctx.arg(3).as_integer();
  const std::int32_t start_on = ctx.arg(4).as_integer();
  const ObjectId enter_target =
      ctx.arg(5).is_object() && ctx.arg(5).as_object().type == kTypeObj ? ctx.arg(5).as_object().id
                                                                          : kNoObject;
  const GameTime now = ctx.scheduler != nullptr ? ctx.scheduler->now() : world.time();

  TrainScan scan{world, *heroes, *commands};
  if (const ClassGraph* graph = world.class_graph()) scan.mage = graph->find("BaseMage");

  std::vector<ObjectId> training;   // A
  std::vector<ObjectId> trainable;  // B
  std::vector<ObjectId> rest;       // C
  for (const SquadKey key : std::vector<SquadKey>(keys.begin(), keys.end())) {
    const Squad* squad = heroes->squads().find(key);
    if (squad == nullptr || (squad->flags & kSquadFlagNoAi) != 0) continue;
    for (const ObjectId member : squad->members) {
      // A member that resolves to nothing is skipped, as the original skips
      // it; counting it among the rest would change nothing observable, since
      // the rest bin sways no decision and a dead id is skipped downstream.
      const WorldObject* slot = world.find(member);
      if (slot == nullptr) continue;
      if (is_training(*commands, member)) {
        training.push_back(member);
      } else if (can_train(scan, *slot)) {
        trainable.push_back(member);
      } else {
        rest.push_back(member);
      }
    }
  }

  std::int32_t answer = 0;
  std::vector<ObjectId> to_train;
  std::vector<ObjectId> to_enter;
  const auto a = static_cast<std::int32_t>(training.size());
  const auto b = static_cast<std::int32_t>(trainable.size());
  if (a == 0) {
    if (a + b >= start_on) {
      answer = 1;
      to_train = trainable;
    }
  } else if (a + b > stop_on) {
    if (b > 0) {
      answer = 2;
      to_train = trainable;
    }
  } else {
    answer = 3;
    to_enter = training;
    to_enter.insert(to_enter.end(), trainable.begin(), trainable.end());
  }
  to_enter.insert(to_enter.end(), rest.begin(), rest.end());

  for (const ObjectId id : to_train) (void)heroes->detach(id);

  for (const ObjectId id : to_enter) {
    const WorldObject* slot = world.find(id);
    if (slot == nullptr || slot->state.holder != kNoObject) continue;
    if (const CommandQueue* queue = commands->find(id);
        queue != nullptr && queue->running() != nullptr &&
        same_verb(queue->running()->verb, "enter")) {
      continue;
    }
    if (commands->script_for(world, id, "enter").empty()) continue;
    if (answer == 0) answer = 4;
    Command enter;
    if (enter_target != kNoObject) {
      enter.arg_kind = CommandArgKind::object;
      enter.object = enter_target;
    }
    (void)commands->set_command(world, id, "enter", enter);
  }
  regroup_into_fresh_squads(world, *heroes, to_enter, enter_state, now);

  for (const ObjectId id : to_train) {
    if (const WorldObject* slot = world.find(id)) start_training(scan, *slot);
  }
  regroup_into_fresh_squads(world, *heroes, to_train, train_state, now);

  return HostOutcome::ok_with(Value::integer(answer));
}

constexpr std::size_t kEntryCount = 55;

}  // namespace

/// 0x00447330: fresh squads for `units`, then `state` on each.
void regroup_into_fresh_squads(World& world, HeroSystem& heroes, std::span<const ObjectId> units,
                               std::int32_t state, GameTime now) {
  SquadTable& squads = heroes.squads();
  for (const ObjectId id : units) {
    const WorldObject* slot = world.find(id);
    if (slot == nullptr) continue;
    UnitRecord& record = heroes.register_unit(world, id);
    // Attached: the assignment pass puts it straight back into the hero's
    // squad, which is not one the state is written to. It never left here.
    if (record.hero != kNoObject) continue;
    if (heroes.is_hero(id)) {
      // Labelled, not read: a hero keeps its squad and the state lands on it.
      if (Squad* own = squads.find(record.squad)) {
        own->state = state;
        own->state_time = now;
      }
      continue;
    }
    // Membership is the table's, not the record's: a unit put into a squad by
    // `SquadTable::join` alone has a stale record, and leaving the wrong squad
    // would leave it listed twice.
    const SquadKey old_key = squads.squad_of(id);
    const Squad* old = squads.find(old_key);
    std::uint16_t flags = old != nullptr ? old->flags : squad_flags_by_class(world, id);
    flags = slot->state.flags.no_ai ? static_cast<std::uint16_t>(flags | kSquadFlagNoAi)
                                    : static_cast<std::uint16_t>(flags & ~kSquadFlagNoAi);
    flags = static_cast<std::uint16_t>(flags & ~kSquadLocked);
    const GaikaId dest = old != nullptr ? old->dest_gaika
                                        : world.gaika().at(world.lsa(), world.resolve_position(id));
    const GaikaId src = old != nullptr ? old->src_gaika : kNoGaika;
    const GameTime fought = old != nullptr ? old->last_fight_time : 0;
    if (old != nullptr) (void)squads.leave(old_key, id);

    const SquadKey key = squads.create(slot->state.owner);
    (void)squads.join(key, id);
    record.squad = key;
    Squad* fresh = squads.find(key);
    if (fresh == nullptr) continue;
    fresh->flags = flags;
    fresh->dest_gaika = dest;
    fresh->src_gaika = src;
    fresh->last_fight_time = fought;
    fresh->state = state;
    fresh->state_time = now;
  }
  squads.prune_empty();
}

std::size_t squad_host_entry_count() noexcept { return kEntryCount; }

std::size_t register_squad_host(HostRegistry& registry) {
  std::size_t defined = 0;
  const auto member = [&](std::string_view name, std::uint16_t arity, script::HostFn fn) {
    registry.define(CallKind::member, name, arity, fn);
    ++defined;
  };
  const auto free_fn = [&](std::string_view name, std::uint16_t arity, script::HostFn fn) {
    registry.define(CallKind::free_function, name, arity, fn);
    ++defined;
  };

  // Descending corpus call frequency.
  member("State", 0, &state_impl);            // 89, the most-read member there is
  member("GAIKAIn", 0, &gaika_in_impl);       // 61
  member("Leader", 0, &leader_impl);          // 37
  member("LastFightTime", 0, &last_fight_time_impl);  //  4
  member("GetLastAttacker", 0, &last_attacker_impl);  //  4
  member("AIDest", 0, &ai_dest_impl);         // 36
  member("Eval", 0, &eval_impl);              //  9
  member("Count", 1, &count_impl);            //  8
  member("SrcGAIKA", 0, &gaika_field_impl<&Squad::src_gaika>);    //  4
  member("DestGAIKA", 0, &gaika_field_impl<&Squad::dest_gaika>);  //  2
  member("OrderDest", 0, &gaika_field_impl<&Squad::order_dest>);  //  2
  member("ID", 0, &gaika_id_impl);            // 33, and it is GAIKA's -- see the body
  member("SetCmd", 4, &set_cmd_impl);         // 30, with /5
  member("SetCmd", 5, &set_cmd_impl);
  member("IsEnemyInSquadSight", 0, &is_enemy_in_squad_sight_impl);  // 20
  member("ClrCmd", 3, &clr_cmd_impl);         // 13
  member("TestFlags", 1, &test_flags_impl);   //  8
  member("TakeNearbyItems", 1, &take_nearby_items_impl);  //  2
  member("StateTime", 0, &state_time_impl);   //  3
  member("No", 0, &squad_number_impl);        //  1, and it is a debug string
  member("SendTo", 2, &send_to_impl);         // 13
  free_fn("MilEval", 1, &mil_eval_impl);            //  4
  free_fn("AllyMilEval", 1, &mil_eval_over<false>);  // 2
  free_fn("EnemyMilEval", 1, &mil_eval_over<true>);  // 1
  member("DelOrder", 0, &del_order_impl);     //  9, six commented out

  // The `SquadList` cursor. `sim/objlist.hpp` says why it lives here: every one
  // of the 131 `Cur`/`Next`/`EOL` receivers in the corpus is a `SquadList`.
  member("Cur", 0, &cur_impl);                // 72
  member("Next", 0, &next_impl);              // 32
  member("EOL", 0, &eol_impl);                // 27
  member("Lock", 0, &lock_impl<true>);        // 20, on both receiver shapes
  member("Unlock", 0, &lock_impl<false>);     // 20
  // 18 `Size` sites plus 5 lower-case `size` ones; one key, four receivers.
  member("Size", 0, &size_impl);              // 23
  member("Rewind", 0, &rewind_impl);          // 12
  // The four arities `gbr.exe` registers; the corpus calls two of them.
  member("GetSquads", 1, &get_squads_impl);
  member("GetSquads", 2, &get_squads_impl);
  member("GetSquads", 3, &get_squads_impl);   //  2
  member("GetSquads", 4, &get_squads_impl);   // 16
  member("Train", 5, &train_impl);           //  1, and a whole strategy behind it
  // Units, not squads, and it walks the same container; see the body.
  member("GetAIControlledUnits", 4, &m_get_ai_controlled_units);  // 4

  // Walking a player's squads by position; see `fn_num_squads`.
  free_fn("NumSquads", 1, &fn_num_squads);  // 3
  free_fn("GetSquad", 2, &fn_get_squad);    // 3

  // The region census. Four arities, one shape; see `census_of`.
  member("Eval", 6, &census_out_impl<false>);   // 19
  member("Eval", 3, &eval_total_impl);          //  8
  member("Eval", 2, &eval_total_impl);          //  2
  member("Count", 6, &census_out_impl<true>);   //  5
  member("EvalNeighbors", 6, &eval_neighbors_impl);  // 1
  // Read to the end at last; see `count_class_impl` for what the argument
  // order turned out to be and which line of the corpus was lying about it.
  member("Count", 3, &count_class_impl);        //  2
  member("AllEnemiesInHolder", 1, &all_enemies_in_holder_impl);  //  2
  member("InvadeThroughGate", 2, &invade_through_gate_impl);     //  1
  member("UseTeleport", 4, &use_teleport_impl);                  //  1
  member("CalcGoAround", 0, &calc_go_around_impl);               //  2
  member("EvalAttach", 2, &eval_attach_impl);                    //  1
  member("FoodComing", 0, &food_coming_impl);                    //  1
  member("SendFoodWagon", 2, &send_food_wagon_impl);             //  1
  // The squad former, and a free function rather than a member.
  free_fn("Squadize", 3, &squadize_impl);       //  2
  free_fn("NearestHospital", 1, &nearest_hospital_impl);  //  1

  // **`Add/1`, `IsValid/0` and `Select/1` are deliberately not here**, and the
  // overlap check in `test_host_setup.cpp` is what said so: member lookup is
  // case-insensitive and all three already belong to somebody, so defining
  // them would have replaced the existing body silently. `sim/squad.hpp`
  // warned about exactly this class of name.
  //
  //   * `Add/1` is `sim/objlist.cpp`'s -- 27 of the 29 shipped receivers are
  //     `ObjList`s and two (`slTrain.Add`) are `SquadList`s, so the `SquadList`
  //     branch goes *inside* that body, where it now is.
  //   * `IsValid/0` is `sim/world_host.cpp`'s, over 257 shipped sites, and not
  //     one of them has a `SquadList` receiver.
  //   * `Select/1` is not a `SquadList` entry point in any useful sense: of its
  //     11 sites, 10 are `wagon.Select` and `obj.Select` and one is `l.Select`.
  //     Claiming the name here would answer for ten call sites this domain
  //     knows nothing about. What 0x0042bee0 does with its argument -- it
  //     indexes the per-player squad table by `n - 1`, so the argument is a
  //     *player* -- is written down in `docs/plan.html` for whoever takes the
  //     wagon's.

  return defined;
}

}  // namespace imperivm::core::sim
