// The world, its objects, its queries and its tick.
// See include/imperivm/core/sim/world.hpp.

#include "imperivm/core/sim/world.hpp"

#include <algorithm>

// For the little-endian output helpers, which the save envelope and the world
// share so that the two cannot drift apart. `sim/save.hpp` includes this
// header, not the other way round, so there is no cycle.
#include "imperivm/core/sim/save.hpp"

namespace imperivm::core::sim {
namespace {

/// A whole decimal with an optional sign, and nothing else; false leaves
/// `out` alone. The map's attributes are written by the original's own
/// serialiser and are never anything else, so a partial parse is a bug.
bool parse_whole_int(std::string_view text, std::int32_t& out) noexcept {
  if (text.empty()) return false;
  std::size_t i = 0;
  bool negative = false;
  if (text[0] == '-' || text[0] == '+') {
    negative = text[0] == '-';
    i = 1;
  }
  if (i >= text.size()) return false;
  std::int64_t value = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10 + (text[i] - '0');
    if (value > 0x7FFFFFFF) return false;
  }
  out = static_cast<std::int32_t>(negative ? -value : value);
  return true;
}

/// FNV-1a, 64 bit. The same construction the entity cross-check tool uses, so
/// hashes from the two are read the same way.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

void hash_u64(std::uint64_t& state, std::uint64_t value) noexcept {
  for (int shift = 0; shift < 64; shift += 8) {
    state ^= (value >> shift) & 0xFF;
    state *= kFnvPrime;
  }
}

void hash_i32(std::uint64_t& state, std::int32_t value) noexcept {
  hash_u64(state, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
}

/// Parse a decimal integer out of a class property value.
///
/// Hand-written rather than `strtol`: the core has no locale, and a property
/// that is not a number must read as "absent" rather than as zero-with-garbage.
/// Returns `fallback` on anything that is not a whole signed decimal.
[[nodiscard]] std::int32_t parse_int(std::string_view text, std::int32_t fallback) noexcept {
  if (text.empty()) return fallback;
  std::size_t i = 0;
  bool negative = false;
  if (text[i] == '-' || text[i] == '+') {
    negative = text[i] == '-';
    ++i;
  }
  if (i >= text.size()) return fallback;
  std::int64_t value = 0;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return fallback;
    value = value * 10 + (c - '0');
    if (value > 0x7FFFFFFFLL) return fallback;  // out of range is not a number
  }
  return static_cast<std::int32_t>(negative ? -value : value);
}

/// The squared distance between two points, in 64 bits.
///
/// A full-map diagonal squared is about 5.4e8, which fits in 32 bits, but a
/// coordinate arriving out of range would silently wrap there. An overflow is a
/// desync rather than a wrong number, because two builds need not wrap the
/// same way, so the arithmetic is widened before it can happen.
[[nodiscard]] std::int64_t dist2(Point a, Point b) noexcept {
  const std::int64_t dx = static_cast<std::int64_t>(a.x) - static_cast<std::int64_t>(b.x);
  const std::int64_t dy = static_cast<std::int64_t>(a.y) - static_cast<std::int64_t>(b.y);
  return dx * dx + dy * dy;
}

}  // namespace

std::int32_t advance_elapsed(const AnimTimeline& timeline, std::int32_t elapsed, GameTime delta,
                             AnimRepeat repeat) noexcept {
  if (!timeline.valid()) return 0;
  if (elapsed < 0) elapsed = 0;
  if (delta < 0) delta = 0;

  const GameTime cycle = timeline.cycle();
  if (repeat == AnimRepeat::loop) {
    // Reduce the delta first so the sum cannot overflow, then reduce again.
    // (a + b) mod c does not depend on where the interval was split, which is
    // the whole reason batched and single-stepped advances agree.
    const GameTime reduced = delta % cycle;
    return static_cast<std::int32_t>((elapsed + reduced) % cycle);
  }
  // Holding: saturate at the end of the cycle rather than running the counter
  // up forever. `AnimTimeline::sample` reports `finished` at exactly `cycle`.
  if (delta >= cycle - elapsed) return static_cast<std::int32_t>(cycle);
  return static_cast<std::int32_t>(elapsed + delta);
}

// --------------------------------------------------------------------------
// SyncFlags
// --------------------------------------------------------------------------

/// `ObjectFlags` as one word.
///
/// Not `pack_sync_flags`: that word folds the owner in and drops the bits the
/// corpus shows but does not explain, so it is a *projection* for diffing
/// against a dump rather than a representation. A save has to come back
/// bit-identical, so the flags are written as themselves.
///
/// `in_party` was missing from this word until `unspawned` was added beside
/// it, so a save silently dropped every party membership -- the bit `Party()`
/// and `Unit::GetParty` both read, and the one that decides which units cross
/// an adventure's map boundary. Nothing caught it because no saved world in
/// the suite had a party member in it, which is the same blind spot the group
/// ordering had one layer down.
///
/// **A byte until `built` made it nine bits.** Widening rather than reusing a
/// spare, because there was no spare: `no_ai` took `0x80` and the eight below
/// it were full. The save version moves with the width; a version-7 world
/// cannot be read as a version-8 one and is refused rather than mis-decoded.
[[nodiscard]] std::uint32_t pack_object_flags(const ObjectFlags& flags) noexcept {
  return static_cast<std::uint32_t>(
      (flags.is_unit ? 0x001u : 0u) | (flags.is_building ? 0x002u : 0u) |
      (flags.is_hero ? 0x004u : 0u) | (flags.has_active_path ? 0x008u : 0u) |
      (flags.hidden ? 0x010u : 0u) | (flags.in_party ? 0x020u : 0u) |
      (flags.unspawned ? 0x040u : 0u) | (flags.no_ai ? 0x080u : 0u) |
      (flags.built ? 0x100u : 0u) | (flags.in_air ? 0x200u : 0u) |
      (flags.noselect ? 0x400u : 0u) | (flags.building ? 0x800u : 0u) |
      (flags.autocast ? 0x1000u : 0u) |
      (flags.on_minimap ? 0x2000u : 0u) | (flags.enemies_near ? 0x4000u : 0u) |
      (flags.friends_near ? 0x8000u : 0u) | (flags.gate_open ? 0x10000u : 0u) |
      (flags.messenger ? 0x20000u : 0u) | (flags.landing ? 0x40000u : 0u) | (flags.cursed ? 0x80000u : 0u) |
      (flags.entering ? 0x100000u : 0u) | (flags.summoning_death ? 0x200000u : 0u) |
      (flags.ondie_fired ? 0x400000u : 0u) | (flags.diseased ? 0x800000u : 0u) |
      (flags.commands_disabled ? 0x1000000u : 0u) |
      (flags.half_damage ? 0x2000000u : 0u) | (flags.training ? 0x4000000u : 0u));
}

[[nodiscard]] ObjectFlags unpack_object_flags(std::uint32_t bits) noexcept {
  ObjectFlags flags;
  flags.is_unit = (bits & 0x001u) != 0;
  flags.is_building = (bits & 0x002u) != 0;
  flags.is_hero = (bits & 0x004u) != 0;
  flags.has_active_path = (bits & 0x008u) != 0;
  flags.hidden = (bits & 0x010u) != 0;
  flags.in_party = (bits & 0x020u) != 0;
  flags.unspawned = (bits & 0x040u) != 0;
  flags.no_ai = (bits & 0x080u) != 0;
  flags.built = (bits & 0x100u) != 0;
  flags.in_air = (bits & 0x200u) != 0;
  flags.noselect = (bits & 0x400u) != 0;
  flags.building = (bits & 0x800u) != 0;
  flags.autocast = (bits & 0x1000u) != 0;
  flags.on_minimap = (bits & 0x2000u) != 0;
  flags.enemies_near = (bits & 0x4000u) != 0;
  flags.friends_near = (bits & 0x8000u) != 0;
  flags.gate_open = (bits & 0x10000u) != 0;
  flags.messenger = (bits & 0x20000u) != 0;
  flags.landing = (bits & 0x40000u) != 0;
  flags.cursed = (bits & 0x80000u) != 0;
  flags.entering = (bits & 0x100000u) != 0;
  flags.summoning_death = (bits & 0x200000u) != 0;
  flags.ondie_fired = (bits & 0x400000u) != 0;
  flags.diseased = (bits & 0x800000u) != 0;
  flags.commands_disabled = (bits & 0x1000000u) != 0;
  flags.half_damage = (bits & 0x2000000u) != 0;
  flags.training = (bits & 0x4000000u) != 0;
  return flags;
}

std::uint32_t pack_sync_flags(const ObjectState& state) noexcept {
  std::uint32_t word = kSyncLive;
  if (state.flags.is_unit) word |= kSyncUnit;
  if (state.flags.is_building) word |= kSyncBuilding;
  if (state.flags.is_hero) word |= kSyncHero;
  if (state.flags.has_active_path) word |= kSyncHasPath;
  if (state.flags.hidden) word |= kSyncHidden;
  if (state.flags.in_party) word |= kSyncParty;
  if (state.flags.unspawned) word |= kSyncUnspawned;
  if (state.flags.training) word |= kSyncTraining;
  // The owner is one-hot in the low 16 bits. `kNoPlayer` leaves it zero, which
  // in the corpus is `CVXDecorObj` and nothing else -- 1,111 objects, the only
  // type with no owner at all.
  if (state.owner < 16) word |= (1u << state.owner);
  return word;
}

PlayerId unpack_owner(std::uint32_t sync_flags) noexcept {
  const std::uint32_t owner_bits = sync_flags & kSyncOwnerMask;
  if (owner_bits == 0) return kNoPlayer;
  // Exactly one bit, or the word is malformed. Not one object in 7,580 has two
  // set, so reading a two-bit word as "the lowest one" would paper over a bug
  // rather than surface it.
  if ((owner_bits & (owner_bits - 1)) != 0) return kNoPlayer;
  PlayerId player = 0;
  std::uint32_t bit = owner_bits;
  while ((bit & 1u) == 0) {
    bit >>= 1;
    ++player;
  }
  return player;
}

ObjectFlags unpack_flags(std::uint32_t sync_flags) noexcept {
  ObjectFlags flags;
  flags.hidden = (sync_flags & kSyncHidden) != 0;
  flags.in_party = (sync_flags & kSyncParty) != 0;
  flags.unspawned = (sync_flags & kSyncUnspawned) != 0;
  flags.is_unit = (sync_flags & kSyncUnit) != 0;
  flags.is_building = (sync_flags & kSyncBuilding) != 0;
  flags.is_hero = (sync_flags & kSyncHero) != 0;
  flags.has_active_path = (sync_flags & kSyncHasPath) != 0;
  return flags;
}

ObjectFlags flags_for_native_class(NativeClass id) noexcept {
  ObjectFlags flags;
  flags.is_unit = native_class_is_a(id, NativeClass::unit);
  flags.is_building = native_class_is_a(id, NativeClass::building);
  flags.is_hero = native_class_is_a(id, NativeClass::hero);
  return flags;
}

std::string_view internal_type_name(InternalKind kind) noexcept {
  switch (kind) {
    case InternalKind::none: return "";
    case InternalKind::settlement: return "CVXSettlement";
    case InternalKind::holder: return "CVXHolder";
    case InternalKind::warehouse: return "CVXWarehouse";
    case InternalKind::item: return "CVXItem";
    case InternalKind::item_script: return "CVXItemScript";
    case InternalKind::ai_helper: return "CVXAIHelper";
    case InternalKind::player_bonus: return "CVXPlayerBonus";
    case InternalKind::player_scripts: return "CVXPlayerScripts";
    case InternalKind::query: return "";  // the query kind names it
    case InternalKind::count: break;
  }
  return "";
}

// --------------------------------------------------------------------------
// named object groups
// --------------------------------------------------------------------------

std::uint32_t RectTable::intern(const Rect& r) {
  for (std::size_t i = 0; i < rects_.size(); ++i) {
    if (rects_[i] == r) return static_cast<std::uint32_t>(i);
  }
  rects_.push_back(r);
  return static_cast<std::uint32_t>(rects_.size() - 1);
}

const RectTable::Rect* RectTable::find(std::uint32_t index) const noexcept {
  return index < rects_.size() ? &rects_[index] : nullptr;
}

std::int32_t GroupTable::find(std::string_view name) const noexcept {
  // A linear scan over interning order. Not a map: iteration order is state,
  // and the table is 2,181 entries at the shipped worst -- `5_Great_Loses_
  // German` has 339 `<group>` elements, the largest of the 29 maps.
  for (std::size_t i = 0; i < groups_.size(); ++i) {
    if (groups_[i].name == name) return static_cast<std::int32_t>(i);
  }
  return kNoGroup;
}

std::int32_t GroupTable::intern(std::string_view name) {
  const std::int32_t existing = find(name);
  if (existing != kNoGroup) return existing;
  groups_.push_back(Entry{std::string(name), {}});
  return static_cast<std::int32_t>(groups_.size() - 1);
}

std::string_view GroupTable::name(std::int32_t group) const noexcept {
  if (!valid(group)) return {};
  return groups_[static_cast<std::size_t>(group)].name;
}

std::span<const ObjectId> GroupTable::members(std::int32_t group) const noexcept {
  if (!valid(group)) return {};
  return groups_[static_cast<std::size_t>(group)].members;
}

bool GroupTable::contains(std::int32_t group, ObjectId id) const noexcept {
  const std::span<const ObjectId> list = members(group);
  // Linear, because the list is in first-add order rather than sorted. That is
  // also what the original does: `CVXGroup::Contains` (0x005718e0) walks its
  // member deque comparing one id at a time.
  return std::find(list.begin(), list.end(), id) != list.end();
}

bool GroupTable::add(std::int32_t group, ObjectId id) {
  if (!valid(group) || id == kNoObject) return false;
  std::vector<ObjectId>& list = groups_[static_cast<std::size_t>(group)].members;
  // Append after a linear dedup, which is `CVXGroup::Add` (0x00572410)
  // transcribed: a `std::find` over the deque and a `push_back` when it comes
  // back empty. Order is the order things joined, and it is state -- see
  // `GroupTable` in `sim/world.hpp` for why it is observable and what used to
  // be here instead.
  if (std::find(list.begin(), list.end(), id) != list.end()) return false;  // a group is a set
  list.push_back(id);
  return true;
}

bool GroupTable::remove(std::int32_t group, ObjectId id) {
  if (!valid(group)) return false;
  std::vector<ObjectId>& list = groups_[static_cast<std::size_t>(group)].members;
  const auto at = std::find(list.begin(), list.end(), id);
  if (at == list.end()) return false;
  // Erase in place, keeping the survivors in their relative order.
  // `CVXGroup::Remove` (0x00572210) is the same find followed by an erase of
  // the one element; a swap-with-last would be cheaper and would reorder the
  // list, which here is state.
  list.erase(at);
  return true;
}

std::size_t GroupTable::remove_from_all(ObjectId id) {
  if (id == kNoObject) return 0;
  std::size_t removed = 0;
  // Every group, in index order. An empty group is left in place: its index is
  // interned, a live query object may already hold it, and compacting would
  // renumber the rest -- the same reason `queries_` never compacts.
  for (Entry& entry : groups_) {
    const auto at = std::find(entry.members.begin(), entry.members.end(), id);
    if (at == entry.members.end()) continue;
    entry.members.erase(at);
    ++removed;
  }
  return removed;
}

void GroupTable::hash(std::uint64_t& accumulator) const noexcept {
  hash_u64(accumulator, groups_.size());
  for (std::size_t i = 0; i < groups_.size(); ++i) {
    hash_u64(accumulator, i);
    for (const char c : groups_[i].name) hash_u64(accumulator, static_cast<std::uint8_t>(c));
    hash_u64(accumulator, groups_[i].members.size());
    for (const ObjectId id : groups_[i].members) hash_u64(accumulator, id);
  }
}

std::int32_t NamedObjectTable::find(std::string_view name) const noexcept {
  for (std::size_t i = 0; i < names_.size(); ++i) {
    if (names_[i].name == name) return static_cast<std::int32_t>(i);
  }
  return kNoName;
}

bool NamedObjectTable::bind(std::string_view name, ObjectId id) {
  if (id == kNoObject) return false;
  // First binding wins. See the header for why the tie-break is inference and
  // which four maps it applies to.
  if (find(name) != kNoName) return false;
  names_.push_back(Entry{std::string(name), id});
  return true;
}

bool NamedObjectTable::rebind(std::string_view name, ObjectId id) {
  if (id == kNoObject) return false;
  const std::int32_t at = find(name);
  if (at == kNoName) return bind(name, id);
  names_[static_cast<std::size_t>(at)].object = id;
  return true;
}

std::string_view NamedObjectTable::name_of(ObjectId object) const noexcept {
  if (object == kNoObject) return {};
  std::string_view best;
  bool found = false;
  for (const Entry& entry : names_) {
    if (entry.object != object) continue;
    if (!found || entry.name < best) {
      best = entry.name;
      found = true;
    }
  }
  return best;
}

std::string_view NamedObjectTable::name(std::int32_t index) const noexcept {
  if (!valid(index)) return {};
  return names_[static_cast<std::size_t>(index)].name;
}

ObjectId NamedObjectTable::object(std::int32_t index) const noexcept {
  if (!valid(index)) return kNoObject;
  return names_[static_cast<std::size_t>(index)].object;
}

void NamedObjectTable::hash(std::uint64_t& accumulator) const noexcept {
  hash_u64(accumulator, names_.size());
  for (std::size_t i = 0; i < names_.size(); ++i) {
    hash_u64(accumulator, i);
    for (const char c : names_[i].name) hash_u64(accumulator, static_cast<std::uint8_t>(c));
    hash_u64(accumulator, names_[i].object);
  }
}

// --------------------------------------------------------------------------
// objects
// --------------------------------------------------------------------------

ObjectId World::allocate(InternalKind kind, std::unique_ptr<NativeObject> object,
                         const Entity* entity, ClassIndex class_index) {
  WorldObject slot;
  slot.id = next_id_++;
  slot.internal = kind;
  slot.class_index = class_index;
  slot.object = std::move(object);
  if (slot.object != nullptr) {
    // Written from `slot.id` in the same statement it is set, so the native
    // object's copy cannot drift from the authoritative one.
    slot.object->id = slot.id;
    slot.object->entity = entity;
    slot.object->class_index = class_index;
    slot.state.flags = flags_for_native_class(slot.object->native_class());
  }
  slot.sight = sight_for_class(class_index);
  // `initial_z`, the one class property that decides where an object *starts*
  // in the third dimension. `gbr.exe` 0x0051af6a: the `Flying` constructor
  // reads the descriptor's `+0xb44` and, when it is not the `0xff1b1e40`
  // sentinel, writes it into `z_from` and sets the airborne bit. **Exactly one
  // shipped class declares it** -- `EAGLE.SC.XML`, at 200 -- so every eagle on
  // every map is in the air from the moment it is placed and every crow is on
  // the ground, which is what their two idle scripts each expect to find.
  //
  // Here rather than in `sim/flying.cpp` because it is spawn-time state like
  // `sight` beside it, and a class property read at spawn is this function's
  // job wherever the value ends up being used.
  if (classes_ != nullptr && class_index != kNoClass) {
    const std::string_view text = classes_->property(class_index, "initial_z");
    if (!text.empty()) {
      slot.state.z_from = parse_int(text, 0);
      slot.state.flags.in_air = true;
    }
  }
  const ObjectId assigned = slot.id;
  objects_.push_back(std::move(slot));
  reindex(objects_.back());
  // **A sentry is born out of the AI's hands.** The `CVXUnit` constructor
  // (0x005d3170) zeroes the second flag word `[unit+0x194]` and then, for a
  // class that is a `Sentry` heir (0x005400c0, 0x005d331a), ORs in
  // `0x04040000`: bit 18, `UNITFLAG_NOAI`, and bit 26, the minimap bit. The
  // desync dumps print exactly that word, `unit flags=4040000`, on the
  // sentries, and bit 18 is the one the state-vector census found on 417
  // units "in guard or patrol, near-exclusively". It is what keeps a town's
  // own AI off its walls: a squad formed round a no-AI unit carries
  // `SF_NOAI` (0x00446c8e), and every `Squad::SetCmd` refuses such a squad
  // (0x00427361, 0x00427517, 0x0043ec15) -- so `GS_KILLENEMIES.VS`, which
  // orders every own squad in a node under attack to `ai_killall`, leaves the
  // sentries to their wall. A map-placed unit's word is then replaced
  // outright by its `UnitFlags` (0x005dd6a9), which `spawn_map_object` does.
  if (WorldObject& made = objects_.back(); made.state.flags.is_unit && classes_ != nullptr) {
    const ClassIndex sentry = classes_->find("Sentry");
    if (sentry != kNoClass && class_is_a(assigned, sentry)) {
      made.state.flags.no_ai = true;
      made.state.flags.on_minimap = true;
    }
  }
  if (objects_.back().object != nullptr) objects_.back().object->on_spawn();
  return assigned;
}

ObjectId World::spawn(NativeClass id, const Entity* entity, ClassIndex class_index) {
  return allocate(InternalKind::none, make_native_object(id), entity, class_index);
}

ObjectId World::spawn_internal(InternalKind kind) {
  // An internal object has no native class and therefore no native object.
  // Giving it a `Decor` carrier instead would make `is_a(decor)` true of a
  // settlement, which is a lie that query dispatch would believe.
  return allocate(kind, nullptr, nullptr, kNoClass);
}

std::size_t World::templates_in_group(std::int32_t group, std::vector<ObjectId>& out) const {
  out.clear();
  for (const ObjectId id : groups_.members(group)) {
    const WorldObject* slot = find(id);
    if (slot == nullptr || slot->internal != InternalKind::none) continue;
    if (!slot->state.flags.unspawned) continue;
    out.push_back(id);
  }
  return out.size();
}

ObjectId World::spawn_from_template(ObjectId templ, ObjectId holder) {
  const WorldObject* source = find(templ);
  if (source == nullptr || source->internal != InternalKind::none) return kNoObject;
  if (!source->state.flags.unspawned) return kNoObject;
  if (source->object == nullptr) return kNoObject;

  // Minted from the template, not from a class the caller named: the original
  // calls a virtual on the template itself (`vtbl + 0x30` at 0x005727ed) and
  // then casts the result, so the copy is whatever the template is.
  const ObjectState state = source->state;
  const ClassIndex class_index = source->class_index;
  const ObjectId settlement = source->settlement;
  const ObjectId copy = spawn(source->object->native_class(), source->object->entity, class_index);
  WorldObject* fresh = find(copy);
  if (fresh == nullptr) return kNoObject;

  // Everything the template carries, then the one bit cleared. That order is
  // the original's -- it clones the template through a virtual and *then*
  // clears `0x08000000` on the result (0x0057282b, `vtbl + 0x48`) -- and it is
  // also the order that makes the clear load-bearing: taking the copy's own
  // category flags instead and clearing on top of those works, but only
  // because `flags_for_native_class` never sets the bit, so the clear becomes
  // unfalsifiable. It survived its own fault injection that way.
  fresh->state = state;
  fresh->state.flags.unspawned = false;
  reindex(*fresh);  // the template's position, or its holder
  fresh->settlement = settlement;
  if (observer_ != nullptr && state.owner != kNoPlayer) observer_->owner_set(*this, copy, kNoPlayer, state.owner);

  if (holder != kNoObject) {
    // `SpawnGroupInHolder`. `put_in_holder` drops the position to
    // `kHeldPosition` on its own, which is the invariant a held object has.
    put_in_holder(copy, holder);
  }

  // Names follow the object into play. `bind` would refuse, because the name
  // is already bound -- to the template -- so this is the one caller that has
  // to overwrite. See `NamedObjectTable::rebind`.
  for (std::int32_t i = 0; i < static_cast<std::int32_t>(named_objects_.size()); ++i) {
    if (named_objects_.object(i) != templ) continue;
    named_objects_.rebind(named_objects_.name(i), copy);
  }

  // And every membership. Not just the group being spawned: the original walks
  // the whole table and tests each group's *template* half for the template
  // (`CVXGroup::Contains`, 0x00571950, which reads the deque at `+0x54`), so a
  // template in three groups yields a copy in three groups. The copy's flag is
  // already clear, so each `add` files it live.
  for (std::int32_t g = 0; g < static_cast<std::int32_t>(groups_.size()); ++g) {
    if (!groups_.contains(g, templ)) continue;
    groups_.add(g, copy);
  }
  return copy;
}

ObjectId World::spawn_of_class(ClassIndex class_index) {
  if (classes_ == nullptr || class_index == kNoClass) return kNoObject;
  const Result<NativeClass> native = native_class_from_name(classes_->at(class_index).cpp_class);
  if (!native) return kNoObject;

  // Only 821 of the 845 shipped classes declare `cpp_class` and 24 inherit it,
  // so this has to go through the graph's resolution rather than read the
  // element -- the same rule that made `CVXDecor 146, CVXBuilding 125` wrong.
  const Entity* entity = nullptr;
  if (entities_ != nullptr) {
    const std::string_view path = classes_->entity_path(class_index, Season::base);
    if (!path.empty()) entity = entities_->resolve(path);
  }

  const ObjectId id =
      *native == NativeClass::ship ? spawn_ship(entity, class_index).ship
                                   : spawn(*native, entity, class_index);
  WorldObject* slot = find(id);
  if (slot == nullptr) return kNoObject;
  // Undamaged. `populate_from_map` has a `healthperc` to scale by and two
  // shipped objects that use it; a script-placed object has no such attribute,
  // and the class maximum is the only figure in scope.
  slot->state.health = parse_int(classes_->property(class_index, "maxhealth"), 0);
  return id;
}

bool World::mutate_class(ObjectId id, ClassIndex to) {
  if (classes_ == nullptr || to == kNoClass) return false;
  WorldObject* slot = find(id);
  if (slot == nullptr || slot->internal != InternalKind::none || slot->object == nullptr) {
    return false;
  }
  const Result<NativeClass> native = native_class_from_name(classes_->at(to).cpp_class);
  if (!native) return false;

  // The art, resolved the way `spawn_of_class` resolves it: through the graph
  // rather than off the element, because 24 classes inherit `cpp_class`.
  const Entity* entity = nullptr;
  if (entities_ != nullptr) {
    const std::string_view path = classes_->entity_path(to, Season::base);
    if (!path.empty()) entity = entities_->resolve(path);
  }

  const ObjectState carried = slot->state;
  slot->object->on_destroy();
  slot->object = make_native_object(*native);
  slot->object->id = id;
  slot->object->entity = entity;
  slot->object->class_index = to;
  slot->class_index = to;

  // A fresh state, then the copy's own list written back over it.
  ObjectState fresh;
  fresh.flags = flags_for_native_class(*native);
  fresh.position = carried.position;
  fresh.owner = carried.owner;
  fresh.health = carried.health;
  fresh.stamina = carried.stamina;
  fresh.holder = carried.holder;
  fresh.flags.in_party = carried.flags.in_party;
  fresh.flags.messenger = carried.flags.messenger;
  slot->state = fresh;
  reindex(*slot);  // position and holder carried, so a no-op; kept for the invariant

  slot->sight = sight_for_class(to);
  if (const std::string_view text = classes_->property(to, "initial_z"); !text.empty()) {
    slot->state.z_from = parse_int(text, 0);
    slot->state.flags.in_air = true;
  }
  slot->object->on_spawn();
  return true;
}

bool World::despawn(ObjectId id) {
  for (auto it = objects_.begin(); it != objects_.end(); ++it) {
    if (it->id != id) continue;
    if (observer_ != nullptr) observer_->despawning(*this, *it);
    if (it->object != nullptr) it->object->on_destroy();
    index_.drop(*it);
    objects_.erase(it);  // stable: the survivors keep their relative order
    // Out of every group it was in. Ids are never reused, so a stale membership
    // could not alias a later object and leaving it would be *safe* -- but it
    // would also be unbounded growth in a hashed table, and it would make
    // `Group("X").count` disagree with `Group("X").GetObjList().count` for as
    // long as the world lived. Dropping it here keeps one rule: a group holds
    // objects the world holds, which is what
    // `while (Group("Oasis_Guards").count != 0)` needs in order to terminate.
    //
    // `named_objects_` is deliberately **not** touched. A named object's binding
    // outlives its object: `gbr.exe` gives `NamedObj` an `IsDead` member and the
    // message `Named unit %s is dead or not initialized!`, neither of which
    // means anything if the name stops resolving the moment the object dies.
    groups_.remove_from_all(id);
    // And its boarding list, for the same reason and with the same asymmetry a
    // group has none of: the row a *ship* owns dies with the ship, while a
    // listed unit that dies stays listed until something walks the row.
    // `sim/boarding.hpp` records why the two halves differ.
    boarding_.forget(id);
    return true;
  }
  return false;
}

const WorldObject* World::find(ObjectId id) const noexcept {
  // `objects_` is in spawn order, ids are monotonic, and `despawn` only erases,
  // so the vector is always sorted by id and this is a binary search rather
  // than the scan it used to be. That matters: a query evaluating over 1,542
  // objects resolves holders per candidate, and a linear find would make the
  // whole thing quadratic.
  if (id == kNoObject) return nullptr;
  const auto it = std::lower_bound(objects_.begin(), objects_.end(), id,
                                   [](const WorldObject& slot, ObjectId key) { return slot.id < key; });
  if (it == objects_.end() || it->id != id) return nullptr;
  return &*it;
}

WorldObject* World::find(ObjectId id) noexcept {
  return const_cast<WorldObject*>(static_cast<const World*>(this)->find(id));
}

// --------------------------------------------------------------------------
// object state
// --------------------------------------------------------------------------

const ObjectState* World::state(ObjectId id) const noexcept {
  const WorldObject* slot = find(id);
  return slot != nullptr ? &slot->state : nullptr;
}

ObjectState* World::mutable_state(ObjectId id) noexcept {
  WorldObject* slot = find(id);
  return slot != nullptr ? &slot->state : nullptr;
}

bool World::set_position(ObjectId id, Point to) noexcept {
  WorldObject* slot = find(id);
  if (slot == nullptr) return false;
  // A held object has no position of its own. See the header, and the 6,355
  // positioned objects in the dumps of which not one has a holder.
  if (slot->state.is_held()) return false;
  slot->state.position = to;
  reindex(*slot);
  return true;
}

bool World::set_owner(ObjectId id, PlayerId owner) noexcept {
  WorldObject* slot = find(id);
  if (slot == nullptr) return false;
  const PlayerId previous = slot->state.owner;
  slot->state.owner = owner;
  // 0x005dc3f0 books nothing when the owner is the one the unit already has.
  if (observer_ != nullptr && previous != owner && !slot->state.flags.unspawned) {
    observer_->owner_set(*this, id, previous, owner);
  }
  return true;
}

std::int32_t next_building_state(std::int32_t current, std::int32_t health,
                                 std::int32_t max_health,
                                 const BuildingStateRules& rules) noexcept {
  if (max_health <= 0) return current;
  // `health * 100 / maxhealth`, integer, truncating -- 0x004db3dc is
  // `imul eax, eax, 0x64` and 0x004db3e0 is a signed `idiv`.
  const std::int32_t pct =
      static_cast<std::int32_t>(static_cast<std::int64_t>(health) * 100 / max_health);

  // The raw tier. Strictly below each threshold, in this order.
  std::int32_t raw = 0;
  if (pct < rules.threshold2) {
    raw = 3;
  } else if (pct < rules.threshold1) {
    raw = 2;
  } else if (pct < rules.threshold0) {
    raw = 1;
  }
  if (raw == current) return current;

  // Hysteresis, and **only for a single step**. `thresholds[k]` is the boundary
  // between tier `k` and tier `k + 1`, so going one step *down* in health tests
  // the boundary the old tier sits on, and one step back *up* tests the one the
  // new tier sits on. 0x004db41b-0x004db44f, both directions.
  const std::int32_t boundary[3] = {rules.threshold0, rules.threshold1, rules.threshold2};
  if (raw == current + 1) {
    const std::int32_t index = current;
    if (index >= 0 && index < 3 && pct > boundary[index] - rules.hysteresis) return current;
  } else if (raw == current - 1) {
    const std::int32_t index = raw;
    if (index >= 0 && index < 3 && pct < boundary[index] + rules.hysteresis) return current;
  }
  return raw;
}

bool World::set_health(ObjectId id, std::int32_t health) noexcept {
  WorldObject* slot = find(id);
  if (slot == nullptr) return false;
  slot->state.health = health;
  // Re-tier. Only a building carries a tier -- the original's virtual is on the
  // building branch of the hierarchy and every reader of `[+0x204]` in
  // `gbr.exe` is a building or a gate -- so nothing else is touched, and a
  // world with no class graph tiers nothing, because there is no `maxhealth`
  // to divide by.
  if (slot->state.flags.is_building) {
    slot->state.damage_state = next_building_state(
        slot->state.damage_state, slot->state.health,
        classes_ == nullptr || slot->class_index == kNoClass
            ? 0
            : parse_int(classes_->property(slot->class_index, "maxhealth"), 0),
        building_states_);
  }
  return true;
}

bool World::set_stamina(ObjectId id, std::int32_t stamina) noexcept {
  WorldObject* slot = find(id);
  if (slot == nullptr) return false;
  slot->state.stamina = stamina;
  return true;
}

bool World::put_in_holder(ObjectId id, ObjectId holder) noexcept {
  if (id == holder) return false;
  WorldObject* slot = find(id);
  const WorldObject* container = find(holder);
  if (slot == nullptr || container == nullptr) return false;

  // Refuse a cycle: if the prospective holder is (transitively) inside `id`,
  // the pair would have no position between them and `resolve_position` would
  // have nothing to return.
  ObjectId walk = container->state.holder;
  for (std::size_t guard = 0; walk != kNoObject && guard <= objects_.size(); ++guard) {
    if (walk == id) return false;
    const WorldObject* next = find(walk);
    if (next == nullptr) break;
    walk = next->state.holder;
  }

  slot->state.holder = holder;
  slot->state.position = kHeldPosition;
  reindex(*slot);
  return true;
}

bool World::remove_from_holder(ObjectId id, Point at) noexcept {
  WorldObject* slot = find(id);
  if (slot == nullptr) return false;
  slot->state.holder = kNoObject;
  slot->state.position = at;
  reindex(*slot);
  return true;
}

Point World::resolve_position(ObjectId id) const noexcept {
  const WorldObject* slot = find(id);
  if (slot == nullptr) return kHeldPosition;
  // Bounded by the object count: a chain longer than that has a cycle, and
  // `put_in_holder` refuses to make one, so this is belt and braces.
  for (std::size_t guard = 0; guard <= objects_.size(); ++guard) {
    if (!slot->state.is_held()) return slot->state.position;
    const WorldObject* container = find(slot->state.holder);
    if (container == nullptr) return kHeldPosition;
    slot = container;
  }
  return kHeldPosition;
}

std::uint32_t World::sync_flags(ObjectId id) const noexcept {
  const WorldObject* slot = find(id);
  return slot != nullptr ? pack_sync_flags(slot->state) : 0u;
}

// --------------------------------------------------------------------------
// composite allocation
// --------------------------------------------------------------------------

World::SettlementIds World::spawn_settlement(PlayerId owner) {
  SettlementIds ids;
  ids.settlement = spawn_internal(InternalKind::settlement);
  ids.holder = spawn_internal(InternalKind::holder);
  ids.warehouse = spawn_internal(InternalKind::warehouse);
  set_owner(ids.settlement, owner);
  set_owner(ids.holder, owner);
  set_owner(ids.warehouse, owner);
  return ids;
}

World::ShipIds World::spawn_ship(const Entity* entity, ClassIndex class_index) {
  ShipIds ids;
  ids.ship = spawn(NativeClass::ship, entity, class_index);
  ids.holder = spawn_internal(InternalKind::holder);
  return ids;
}

World::SingletonIds World::spawn_singletons() {
  SingletonIds ids;
  ids.ai_helper = spawn_internal(InternalKind::ai_helper);
  ids.player_bonus = spawn_internal(InternalKind::player_bonus);
  ids.player_scripts = spawn_internal(InternalKind::player_scripts);
  return ids;
}

// --------------------------------------------------------------------------
// classes and filters
// --------------------------------------------------------------------------

std::int32_t World::sight_for_class(ClassIndex index) const noexcept {
  if (classes_ == nullptr || index == kNoClass) return 0;
  return parse_int(classes_->property(index, "sight"), 0);
}

bool World::class_is_a(ObjectId id, ClassIndex base) const noexcept {
  const WorldObject* slot = find(id);
  if (slot == nullptr || classes_ == nullptr) return false;
  ClassIndex current = slot->class_index;
  for (std::size_t guard = 0; current != kNoClass && guard <= classes_->size(); ++guard) {
    if (current == base) return true;
    const ClassIndex parent = classes_->at(current).parent_index;
    if (parent == current) return false;
    current = parent;
  }
  return false;
}

bool World::matches_filter(const WorldObject& slot, const ClassFilter& filter) const noexcept {
  if (filter.match_all) return true;
  // A filter that named classes and resolved none selects nothing. Widening it
  // to everything would turn a typo in the data into a world-wide query.
  if (filter.count == 0) return false;
  if (classes_ == nullptr || slot.class_index == kNoClass) return false;

  // Walk the object's ancestry once and test every filter entry against each
  // step, rather than materialising `ancestry()` per candidate: this runs once
  // per object per query evaluation and must not allocate.
  ClassIndex current = slot.class_index;
  for (std::size_t guard = 0; current != kNoClass && guard <= classes_->size(); ++guard) {
    for (std::uint8_t i = 0; i < filter.count; ++i) {
      if (filter.classes[i] == current) return true;
    }
    const ClassIndex parent = classes_->at(current).parent_index;
    if (parent == current) break;
    current = parent;
  }
  return false;
}

void World::collect(const WorldObject& slot, const ClassFilter& filter,
                    std::vector<ObjectId>& out) const {
  // Internal objects -- settlements, holders, warehouses, queries -- are not
  // candidates for a spatial query. They occupy handles and hold state, but
  // they are not *in* the world in the sense a class filter means.
  if (slot.internal != InternalKind::none) return;
  // A spawn template is not in play. `gbr.exe` makes this test in every sweep
  // separately -- the area-query grid walk at 0x004fc6f4, the `ObjList`
  // collector at 0x0041f4e1 and some forty other sites, all
  // `test [obj + 0x2c], 0x8000000` / `jne skip` -- and this is the one funnel
  // they all correspond to here, so making it once is making it everywhere.
  if (slot.state.flags.unspawned) return;
  if (!matches_filter(slot, filter)) return;
  out.push_back(slot.id);
}

// --------------------------------------------------------------------------
// spatial queries
// --------------------------------------------------------------------------

// Every query below is a test on `(slot, at)`, where `at` is the object's own
// position, over the objects that **stand on the map**: a held object is in no
// grid cell in the original and no area sweep there finds it, so none here
// does either (`sim/spatial_index.hpp` has the addresses). `scan` applies the
// test to every such object in spawn order, which is what each of these was
// written as. `sweep` gives the same answer from the grid: it gathers the
// objects the box can hold, puts them back into spawn order, and applies the
// same test and the same `collect`. See `sim/spatial_index.hpp` for why the
// grid can never be the order.

namespace {

/// Past this many cells a box is most of the map, and the straight scan is the
/// cheaper way to visit it. Either path gives the same answer.
constexpr std::int64_t kMaxSweepCells = 256;

[[nodiscard]] SpatialBox box_around(Point centre, std::int32_t radius) noexcept {
  return SpatialBox{std::int64_t{centre.x} - radius, std::int64_t{centre.y} - radius,
                    std::int64_t{centre.x} + radius, std::int64_t{centre.y} + radius};
}

}  // namespace

void World::rebuild_index() {
  index_.clear();
  for (WorldObject& slot : objects_) {
    slot.spatial = SpatialSlot{};
    index_.file(slot);
  }
}

template <typename Test>
std::size_t World::scan(const ClassFilter& filter, bool filtered, std::vector<ObjectId>& out,
                        Test&& test) const {
  out.clear();
  for (const WorldObject& slot : objects_) {  // spawn order, never any other
    if (slot.internal != InternalKind::none || slot.state.is_held()) continue;
    const Point at = slot.state.position;
    if (!test(slot, at)) continue;
    if (filtered) {
      collect(slot, filter, out);
    } else {
      out.push_back(slot.id);
    }
  }
  return out.size();
}

template <typename Test>
std::size_t World::sweep(const SpatialBox& box, const ClassFilter& filter, bool filtered,
                         std::vector<ObjectId>& out, Test&& test) const {
  if (SpatialIndex::cells_touched(box) > kMaxSweepCells) return scan(filter, filtered, out, test);
  out.clear();
  index_.gather(box, out);
  // Back into spawn order, which is ascending id order. Each object is filed
  // exactly once, so there is nothing to deduplicate.
  std::sort(out.begin(), out.end());
  std::size_t kept = 0;
  auto cursor = objects_.begin();
  for (const ObjectId id : out) {
    cursor = std::lower_bound(cursor, objects_.end(), id,
                              [](const WorldObject& slot, ObjectId key) { return slot.id < key; });
    const WorldObject& slot = *cursor;
    const Point at = slot.state.position;
    if (!test(slot, at)) continue;
    // `collect`'s own tests, applied in place.
    if (filtered && (slot.state.flags.unspawned || !matches_filter(slot, filter))) continue;
    out[kept++] = id;
  }
  out.resize(kept);
#ifdef IMPERIVM_SPATIAL_CHECK
  // A build with the check on asks the straight scan every time and stops on the first
  // disagreement: a stale grid is a desync, and a desync found at the query
  // that caused it is worth the whole cost of the check.
  {
    std::vector<ObjectId> expected;
    scan(filter, filtered, expected, test);
    if (expected != out || !index_.consistent(objects_)) __builtin_trap();
  }
#endif
  return out.size();
}

std::size_t World::objects_in_radius(Point center, std::int32_t radius, const ClassFilter& filter,
                                     std::vector<ObjectId>& out) const {
  if (radius <= 0) {
    return scan(filter, true, out,
                [](const WorldObject&, Point at) { return at != kHeldPosition; });
  }
  const std::int64_t limit = static_cast<std::int64_t>(radius) * static_cast<std::int64_t>(radius);
  return sweep(box_around(center, radius), filter, true, out,
               [&](const WorldObject&, Point at) {
                 return at != kHeldPosition && dist2(at, center) <= limit;
               });
}

std::size_t World::objects_in_rect(std::int32_t left, std::int32_t top, std::int32_t right,
                                   std::int32_t bottom, const ClassFilter& filter,
                                   std::vector<ObjectId>& out) const {
  const SpatialBox box{left, top, right, bottom};
  return sweep(box, filter, true, out, [&](const WorldObject&, Point at) {
    return at != kHeldPosition && box.contains(at);
  });
}

std::size_t World::objects_located_in_rect(std::int32_t left, std::int32_t top,
                                           std::int32_t right, std::int32_t bottom,
                                           std::vector<ObjectId>& out) const {
  const SpatialBox box{left, top, right, bottom};
  return sweep(box, ClassFilter{}, false, out,
               [&](const WorldObject&, Point at) { return box.contains(at); });
}

std::size_t World::objects_in_sight(ObjectId observer, const ClassFilter& filter,
                                    std::vector<ObjectId>& out) const {
  out.clear();
  const WorldObject* watcher = find(observer);
  if (watcher == nullptr) return 0;
  // The observer's own stored position, as 0x004ff080 reads it through
  // `vtbl+0x3c` (0x004ff133) -- so a held observer looks out from `(-1, -1)`,
  // the map's corner, and not from its holder; nothing it stands in is asked.
  const Point eye = watcher->state.position;

  const std::int32_t sight = watcher->sight;
  const auto seen = [&](const WorldObject& slot, Point at) {
    if (slot.id == observer) return false;  // nobody sees themselves
    return at != kHeldPosition;
  };
  if (sight <= 0) return scan(filter, true, out, seen);
  const std::int64_t limit = static_cast<std::int64_t>(sight) * static_cast<std::int64_t>(sight);
  return sweep(box_around(eye, sight), filter, true, out,
               [&](const WorldObject& slot, Point at) {
                 return seen(slot, at) && dist2(at, eye) <= limit;
               });
}

std::size_t World::objects_of_class_for_player(const ClassFilter& filter, PlayerId player,
                                               Point center, std::int32_t radius,
                                               std::vector<ObjectId>& out) const {
  const auto owned = [player](const WorldObject& slot) {
    return player == kNoPlayer || slot.state.owner == player;
  };
  if (radius <= 0) {
    // The whole map, and no position test at all: a held object whose holder
    // is gone is still somebody's.
    out.clear();
    for (const WorldObject& slot : objects_) {
      if (slot.internal != InternalKind::none) continue;
      if (!owned(slot)) continue;
      collect(slot, filter, out);
    }
    return out.size();
  }
  const std::int64_t limit = static_cast<std::int64_t>(radius) * static_cast<std::int64_t>(radius);
  return sweep(box_around(center, radius), filter, true, out,
               [&](const WorldObject& slot, Point at) {
                 return owned(slot) && at != kHeldPosition && dist2(at, center) <= limit;
               });
}

std::size_t World::objects_of_class_for_player_in_rect(const ClassFilter& filter, PlayerId player,
                                                      std::int32_t left, std::int32_t top,
                                                      std::int32_t right, std::int32_t bottom,
                                                      std::vector<ObjectId>& out) const {
  const SpatialBox box{left, top, right, bottom};
  return sweep(box, filter, true, out, [&](const WorldObject& slot, Point at) {
    if (player != kNoPlayer && slot.state.owner != player) return false;
    return at != kHeldPosition && box.contains(at);
  });
}

std::size_t World::objects_by_relation(const ClassFilter& filter, PlayerId viewer,
                                       std::int32_t flags_type,
                                       std::vector<ObjectId>& out) const {
  // This selected `owner == viewer` for a while -- the caller's *own* objects --
  // which made `EnemyObjs(p, cls)` return exactly the wrong set while still
  // composing, hashing and passing every test that only checked its shape.
  //
  // The three `flags_type` values are proven: they are the constructors of
  // `CVXPlayerFlagsQuery` in `gbr.exe`, 2 for `EnemyObjs` (0x005756b0), 1 for
  // `FriendlyObjs` (0x00575ab0) and 3 for `ControllableObjs` (0x005758b0).
  // **Which per-player mask each one consults is inferred from the names** --
  // the match function itself was not disassembled -- so the readings below are
  // the obvious ones and are labelled rather than assumed proven.
  //
  // Hostility is one-directional (`sim/player.hpp` has the addresses), so this
  // asks the viewer's row and never the transpose.
  out.clear();
  const PlayerTable& diplomacy = players();
  for (const WorldObject& slot : objects_) {
    if (slot.internal != InternalKind::none) continue;
    const PlayerId owner = slot.state.owner;
    if (owner == kNoPlayer) continue;

    bool wanted = false;
    switch (flags_type) {
      case 1:  // FriendlyObjs: everyone the viewer is not at war with, itself
               // included. The complement of `enemyflags`, which `SetRelation`
               // maintains as the exact complement of relation bit 0.
        wanted = owner == viewer || !diplomacy.is_enemy(viewer, owner);
        break;
      case 2:  // EnemyObjs.
        wanted = diplomacy.is_enemy(viewer, owner);
        break;
      case 3:  // ControllableObjs: the viewer's own, plus anyone who has granted
               // it share-control. No shipped map grants that off-diagonal, so
               // the second half is exercised by nothing.
        wanted = owner == viewer ||
                 diplomacy.has(owner, viewer, Relation::share_control);
        break;
      default:
        // An unknown type selects nothing rather than everything. A query that
        // silently widened would be the harder failure to notice.
        wanted = false;
        break;
    }
    if (!wanted) continue;
    collect(slot, filter, out);
  }
  return out.size();
}

std::size_t World::units_in_settlement(ObjectId settlement, const ClassFilter& filter,
                                       std::vector<ObjectId>& out,
                                       SettlementScope scope) const {
  out.clear();
  if (settlement == kNoObject) return 0;

  // The two halves, and the flags the original derives from the mode:
  // `CVXUnitsInSettlementQuery::Refresh` (0x004fb640) computes
  // `ring = (mode != 0)` and `garrison = (mode != 1)` and hands both to
  // `Settlement::CollectUnits` (0x005c58f0), which runs them as two
  // independent passes over one output list.
  const bool want_garrison = scope != SettlementScope::ring;
  const bool want_ring = scope != SettlementScope::garrison;

  if (want_garrison) {
    // **A divergence, and it is this engine's, not the original's.**
    // `Settlement::CollectUnits` walks the settlement's *holder* -- the vector
    // at `holder + 0x28`, reached through the handle at `settlement + 0x5e` --
    // in holder order, filtered by `IsHeirOf` and by nothing else. This walks
    // the objects that carry a settlement back-link instead, because on every
    // retail map that is the only place the answer exists: `sim/session.cpp`
    // populates `WorldObject::settlement` from `map.obj.xml` and calls
    // `EconomySystem::add_building` for nothing, so `Settlement::holder.units`
    // is empty at load and stays empty until a script issues `AddUnit`. The two
    // readings agree on a settlement built through `AddUnit` and disagree on
    // every settlement a map file declares; picking the holder here would
    // answer every shipped call site with an empty list. Closing this means
    // filing map-declared units into the holder at load, which moves the world
    // hash of every map, so it is recorded rather than done in passing.
    for (const WorldObject& slot : objects_) {
      if (slot.internal != InternalKind::none) continue;
      if (slot.settlement != settlement) continue;
      if (!slot.state.flags.is_unit) continue;
      collect(slot, filter, out);
    }
  }

  if (!want_ring) return out.size();

  // The ring: every object standing inside one of the settlement's buildings'
  // **own** sight. Not the settlement's sight -- there is no such number -- and
  // not one circle but one per building, unioned.
  //
  // `gbr.exe` 0x005c5a10-0x005c5abe walks the settlement's building vector at
  // `settlement + 0x68`, reads each building's sight from `[building + 0xd0]`,
  // fetches its position through `vtbl + 0x3c`, and hands `(pos, sight,
  // sight * sight)` to the grid sweep at 0x005c5120. The buildings come from
  // the back-link here for the same reason the garrison does not: on a retail
  // map `Settlement::buildings` is empty and the back-link is what the loader
  // writes. Unlike the garrison the two cannot disagree in the other
  // direction, because `EconomySystem::add_building` writes both.
  //
  // Four filters in the sweep's inner loop (0x005c5272-0x005c52ba), in its
  // order: a non-zero flag word, castable to the object class, **not dead**
  // (`vtbl + 0x50`, the slot `Obj::IsAlive` negates), `IsHeirOf` against the
  // class argument, and **not a spawn template** (`test [obj+0x2c],
  // 0x8000000`). `collect` makes the last one for every sweep in this file;
  // the dead test is made here rather than there because no other sweep in
  // `gbr.exe` makes it and widening `collect` would apply it to all of them.
  //
  // Held objects are skipped, as every area sweep skips them: a garrisoned
  // unit holds `(-1, -1)` and occupies no grid cell in the original, so it
  // cannot be swept by the ring, and resolving it here would make
  // `UnitsAroundSettlement` a superset of `UnitsInSettlement` -- collapsing the
  // very distinction the mode encodes.
  for (const WorldObject& tower : objects_) {
    if (tower.internal != InternalKind::none) continue;
    if (tower.settlement != settlement) continue;
    if (!tower.state.flags.is_building) continue;
    // A zero sight is not "no restriction" here, unlike `objects_in_radius`
    // above: the original squares the number and compares, so `sight == 0`
    // admits exactly what stands on the building's own point. Only a negative
    // is refused, and only because squaring one would widen the circle.
    if (tower.sight < 0) continue;
    const Point eye = resolve_position(tower.id);
    if (eye == kHeldPosition) continue;
    const std::int64_t limit =
        static_cast<std::int64_t>(tower.sight) * static_cast<std::int64_t>(tower.sight);
    for (const WorldObject& slot : objects_) {
      if (slot.internal != InternalKind::none) continue;
      if (slot.state.is_held()) continue;
      // Inclusive: 0x005c5304 is `cmp dist2, r2` / `jg skip`, so equality is
      // inside. The same rule the rectangle's four edges follow.
      if (dist2(slot.state.position, eye) > limit) continue;
      if (slot.state.health <= 0) continue;
      collect(slot, filter, out);
    }
  }

  // Sorted and deduplicated, and **only when the ring ran**. One object inside
  // two buildings' sight is collected twice by the loop above and once by the
  // original's, which is why 0x005c5ac3 sorts (0x004fb510) and uniques
  // (0x0050e300) -- and why the garrison branch, whose early exit at
  // 0x005c5a0a jumps clean past both, keeps holder order with no dedup at all.
  //
  // The range is the **whole** list, not just the part the ring appended:
  // 0x005c5ac3 reads `[list+0xc]` and `[list+0x10]` for begin and size. So in
  // `both` a garrison entry that the ring also found collapses to one, and the
  // garrison's own order is lost -- which is the price mode 2 pays and mode 0
  // does not. The sort is over the original's 16-bit handles, which are
  // `compat_handle(id)` here; ids are dense and well under 65,536 on every
  // shipped map, so ascending id is ascending handle.
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out.size();
}

std::size_t World::buildings_in_settlement(ObjectId settlement, const ClassFilter& filter,
                                           std::vector<ObjectId>& out) const {
  out.clear();
  if (settlement == kNoObject) return 0;
  for (const WorldObject& slot : objects_) {
    if (slot.internal != InternalKind::none) continue;
    if (slot.settlement != settlement) continue;
    if (!slot.state.flags.is_building) continue;
    collect(slot, filter, out);
  }
  return out.size();
}

std::size_t World::objects_in_group(std::int32_t group, const ClassFilter& filter,
                                    std::vector<ObjectId>& out) const {
  out.clear();
  // A walk, not a search: the list is already free of ids the world has
  // despawned -- `despawn` prunes it. The `find` is still here because a group
  // can be populated by a caller that never went through `spawn`, and a query
  // must not hand out a dead handle.
  //
  // The order that comes out is the order things joined the group, which is
  // this engine's one exception to `sim/query.hpp`'s ascending rule and is the
  // original's behaviour. `GroupTable` in `sim/world.hpp` carries the evidence
  // and the reason it is worth the exception.
  for (const ObjectId id : groups_.members(group)) {
    const WorldObject* slot = find(id);
    if (slot == nullptr) continue;
    collect(*slot, filter, out);
  }
  return out.size();
}

ObjectId World::named_object_alive(std::string_view name) const noexcept {
  const ObjectId id = named_objects_.object(name);
  return find(id) != nullptr ? id : kNoObject;
}

std::size_t World::contents_of(ObjectId holder, std::vector<ObjectId>& out) const {
  out.clear();
  if (holder == kNoObject) return 0;
  for (const WorldObject& slot : objects_) {
    if (slot.state.holder == holder) out.push_back(slot.id);
  }
  return out.size();
}

std::int64_t World::distance_squared(ObjectId a, ObjectId b) const noexcept {
  if (find(a) == nullptr || find(b) == nullptr) return -1;
  return dist2(resolve_position(a), resolve_position(b));
}

// --------------------------------------------------------------------------
// queries
// --------------------------------------------------------------------------

ObjectId World::create_query(const QuerySpec& spec) {
  const std::uint32_t index = static_cast<std::uint32_t>(queries_.size());
  queries_.push_back(spec);
  const ObjectId id = spawn_internal(InternalKind::query);
  WorldObject* slot = find(id);
  if (slot != nullptr) slot->query = index;
  return id;
}

const QuerySpec* World::query_spec(ObjectId id) const noexcept {
  const WorldObject* slot = find(id);
  if (slot == nullptr || slot->internal != InternalKind::query) return nullptr;
  if (slot->query >= queries_.size()) return nullptr;
  return &queries_[slot->query];
}

QuerySpec* World::mutable_query_spec(ObjectId id) noexcept {
  return const_cast<QuerySpec*>(static_cast<const World*>(this)->query_spec(id));
}

std::size_t World::evaluate_query(ObjectId id, std::vector<ObjectId>& out) const {
  return evaluate_query(id, out, 0);
}

std::size_t World::evaluate_query(ObjectId id, std::vector<ObjectId>& out,
                                  std::uint32_t depth) const {
  out.clear();
  // A set-op query names its operands by handle, and nothing stops a script
  // from making one name itself -- directly or round a longer loop. The dumps
  // cannot tell us what the original did about that; refusing at a fixed depth
  // is at least bounded, deterministic and identical on every peer, which an
  // infinite recursion is not.
  if (depth > kMaxQueryDepth) return 0;
  const QuerySpec* spec = query_spec(id);
  if (spec == nullptr) return 0;

  switch (spec->kind) {
    case QueryKind::objs_in_sight:
      return objects_in_sight(spec->subject, spec->filter, out);

    case QueryKind::map_area_circle: {
      // `ObjsInRange` anchors on an object and `ObjsInCircle` on a point; the
      // dumps show one runtime type for both, so the anchor wins when present.
      // The anchor's own stored position, read afresh at each evaluation
      // (0x004ff133), and a held anchor's is `(-1, -1)`: the sweep goes round
      // the map's corner, as `objects_in_sight` does. An anchor that is gone
      // answers nothing (0x004ff0cd).
      Point center = spec->center;
      if (spec->subject != kNoObject) {
        const WorldObject* anchor = find(spec->subject);
        if (anchor == nullptr) return 0;
        center = anchor->state.position;
      }
      return objects_in_radius(center, spec->radius, spec->filter, out);
    }

    case QueryKind::map_area_rect:
      return objects_in_rect(spec->left, spec->top, spec->right, spec->bottom, spec->filter, out);

    case QueryKind::class_player_area:
      // One kind, two shapes -- see `QuerySpec::area_is_rect` for why this is a
      // branch here and two kinds above.
      if (spec->area_is_rect) {
        return objects_of_class_for_player_in_rect(spec->filter, spec->player, spec->left,
                                                   spec->top, spec->right, spec->bottom, out);
      }
      return objects_of_class_for_player(spec->filter, spec->player, spec->center, spec->radius,
                                         out);

    case QueryKind::player_flags:
      return objects_by_relation(spec->filter, spec->player, spec->flags_type, out);

    case QueryKind::party: {
      // `PartyQuery()`: every object flagged `in_party`, ascending -- `Party()`'s
      // own rule, and its stated approximation of the roster's fill order.
      std::size_t found = 0;
      for (const WorldObject& slot : objects_) {
        if (!slot.state.flags.in_party) continue;
        out.push_back(slot.id);
        ++found;
      }
      return found;
    }

    case QueryKind::group:
      // `spec->group` is an index into the world's `GroupTable` -- seeded from
      // `map.obj.xml`'s `<group>` elements and extended by the scripts. A group
      // query carries no class filter (`Group(n)` takes one argument), so the
      // filter is `match_all` at every real call site; it is passed through
      // rather than ignored so that a spec built with one behaves.
      return objects_in_group(spec->group, spec->filter, out);

    case QueryKind::units_in_settlement:
      // One kind, three scopes -- `UnitsInSettlement`, `UnitsAroundSettlement`
      // and `UnitsGuardingSettlement` are one runtime type in `gbr.exe` with a
      // mode field at `+0x54`. See `SettlementScope`.
      return units_in_settlement(spec->subject, spec->filter, out, spec->settlement_scope);

    case QueryKind::buildings_in_settlement:
      return buildings_in_settlement(spec->subject, spec->filter, out);

    case QueryKind::set_op: {
      // Both operands are themselves query objects; that composition is what
      // makes a query a persistent object rather than a call.
      std::vector<ObjectId> lhs;
      std::vector<ObjectId> rhs;
      evaluate_query(spec->lhs, lhs, depth + 1);
      evaluate_query(spec->rhs, rhs, depth + 1);
      // The set algebra below is a merge, so it needs sorted operands -- and a
      // group query is the one kind that does not supply them: its members come
      // out in the order they joined, which `GroupTable` in `sim/world.hpp`
      // explains is state the command queue can see. Sorting here rather than
      // there keeps that order where it is observable and pays for it only in
      // the branch that cannot use it.
      //
      // The result is therefore ascending, which the original's `CVXSetOpQuery`
      // is not: it composes two unsorted deques with a linear `Contains` and
      // yields neither operand's order. A labelled divergence, affordable
      // because none of the 759 `Union`/`Intersect`/`Subtract` sites in the
      // shipped scripts indexes what it gets back.
      std::sort(lhs.begin(), lhs.end());
      std::sort(rhs.begin(), rhs.end());
      switch (spec->op) {
        case SetOp::intersect:
          std::set_intersection(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                                std::back_inserter(out));
          break;
        case SetOp::set_union:
          std::set_union(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), std::back_inserter(out));
          break;
        case SetOp::subtract:
          std::set_difference(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                              std::back_inserter(out));
          break;
      }
      return out.size();
    }

    case QueryKind::count:
      break;
  }
  return 0;
}

// --------------------------------------------------------------------------
// populating from a map
// --------------------------------------------------------------------------

ObjectId World::spawn_map_object(const MapObject& placed, Point at, ObjectId settlement,
                                 std::int32_t player, const EntityResolver* entities,
                                 bool* was_ship) {
  if (was_ship != nullptr) *was_ship = false;
  const ClassGraph& graph = *classes_;
  const ClassIndex class_index = graph.lookup(placed.class_name);
  if (class_index == kNoClass) return kNoObject;
  const Result<NativeClass> native = native_class_from_name(graph.at(class_index).cpp_class);
  if (!native) return kNoObject;

  const Entity* entity = nullptr;
  if (entities != nullptr) {
    const std::string_view path = graph.entity_path(class_index, Season::base);
    if (!path.empty()) entity = entities->resolve(path);
  }

  ObjectId id = kNoObject;
  if (*native == NativeClass::ship) {
    // A ship carries a holder for its boarded units at handle+1. 3 of 3 in
    // the corpus, and the only place a standalone `CVXHolder` occurs.
    const ShipIds ids = spawn_ship(entity, class_index);
    id = ids.ship;
    if (was_ship != nullptr) *was_ship = true;
  } else {
    id = spawn(*native, entity, class_index);
  }

  WorldObject* slot = find(id);
  if (slot == nullptr) return id;

  slot->state.position = at;
  reindex(*slot);
  slot->state.owner = player > 0 ? static_cast<PlayerId>(player - 1) : kNoPlayer;
  // A spawn template: placed, handled, and not in play until `SpawnGroup`
  // mints a copy of it. 10,610 of the 27,070 objects across the 29 shipped
  // maps carry this -- 39%, and 77% of `5_Great_Battles_Britain` -- so it is
  // not an edge case: without it every campaign map starts with its whole
  // reinforcement schedule already standing on the field. See
  // `kSyncUnspawned` in `sim/world.hpp`.
  slot->state.flags.unspawned = (placed.flags & kSyncUnspawned) != 0;
  // The two bits of the map's `UnitFlags` this engine can name. Both are on
  // the *second* flag word, `[obj+0x194]`, which is a different attribute
  // from `flags` above and was not read here at all until `Unit::GetFlags`
  // needed the first of them.
  //
  // **`no_ai` was documented as something the map cannot author**, and it is
  // the single most common thing in the attribute: 14,586 of 16,171 units.
  // The correlation settles the reading -- the bit is clear on *every* wild
  // animal in the retail install (226 deer, 207 crows, 101 wolves, 80 fish,
  // 46 eagles, 25 boars, no exceptions) and set on the soldiers. A campaign
  // places its troops for the mission script, which hands them to the AI with
  // `SetNoAIFlag(ol, false)` when it is ready; wildlife is the AI's from the
  // first tick. Loading it false would have put every placed soldier into
  // `TOWNHALL_AUTOTRAIN.VS`'s training list on turn one.
  slot->state.flags.no_ai = (placed.unit_flags & kUnitFlagNoAI) != 0;
  // And 46 birds start airborne, which `ObjectFlags::in_air` recorded as
  // something nothing sets. Bit 22 occurs only on flying units.
  slot->state.flags.in_air = (placed.unit_flags & kUnitFlagInAir) != 0;
  // The word is replaced, not merged (0x005dd6a9): a sentry the map places
  // keeps the bits its map gives it rather than the ones `allocate` set.
  slot->state.flags.on_minimap = (placed.unit_flags & kUnitFlagOnMinimap) != 0;
  // Health as the map authors it, not as the class declares it.
  //
  // The nine dumps agree with the class maximum because the corpus is
  // undamaged, not because the map has no say: every `<scriptobj>` with an
  // owner carries `healthperc`, and two objects in the whole retail install
  // carry something other than 100 (one 30, one 40). One object -- `Townhall`
  // num=0 in `randommap.BFHP` -- carries an absolute `health="2000"` instead,
  // which wins outright. Rounding is integer truncation, and the result is
  // floored at 1 for anything the map did not author as literally dead: a
  // `healthperc` of 30 on a 3-hitpoint class must not silently spawn a corpse.
  const std::int32_t max_health = parse_int(graph.property(class_index, "maxhealth"), 0);
  if (placed.health_absolute >= 0) {
    slot->state.health = placed.health_absolute;
  } else if (placed.health_percent >= 100) {
    slot->state.health = max_health;
  } else {
    const std::int32_t scaled = static_cast<std::int32_t>(
        static_cast<std::int64_t>(max_health) * placed.health_percent / 100);
    slot->state.health = scaled > 0 ? scaled : (max_health > 0 ? 1 : 0);
  }
  // `stamina`, as authored. **This used to be left alone**, because the
  // class graph's `maxstamina` (10 for units, 0 for buildings) and the map's
  // `stamina` (10 on units, 20 on buildings) disagreed for buildings and the
  // dumps record 20 on buildings. The disagreement is settled by the loader
  // itself: `Obj`'s attribute reader (0x005af280) writes the value straight
  // into `[obj+0xc4]` at 0x005af4e9, no clamp, no look at the class -- so
  // the dumps show the map's number because the map's number is what the
  // engine holds. Combat still seeds a *combatant's* stamina from
  // `maxstamina`, which is the number the stamina rules are written against.
  //
  // `inventorysize` is parsed by the same reader (0x005af576) and the result
  // dropped; the class's `inventory_size` is what bounds the holder.
  {
    std::int32_t value = 0;
    if (parse_whole_int(placed.attribute("stamina"), value)) slot->state.stamina = value;
    // `amount` is a wagon's cargo, `[obj+0x1cc]`, which `Wagon::amount`
    // (0x005ebdb0) reads back: the wagon loader (0x005ec690) writes it there
    // at 0x005ec704. 26 shipped traders and camels carry one.
    if (parse_whole_int(placed.attribute("amount"), value)) slot->state.cargo = value;
    // `display_name`, see `WorldObject::display_name`.
    slot->display_name.assign(placed.attribute("display_name"));
  }
  // `built` is left as the spawn made it -- false -- whatever the health. A
  // siege engine's reader is the building one, `vtbl+0x6c` (0x004dee10, then
  // `Obj`'s 0x005af280), and neither touches `[cat+0x208]`; `Built` is a key
  // of the class's persist serialiser (0x004e1c90), a saved game's field. So
  // a placed engine is built by its crew through `CATAPULT_IDLE.VS` like a
  // new one. No shipped map places one; see the combat test
  // `a_map_placed_siege_engine_is_unbuilt_and_holds_its_fire`.
  slot->settlement = settlement;
  if (observer_ != nullptr && slot->state.owner != kNoPlayer && !slot->state.flags.unspawned) {
    observer_->owner_set(*this, id, kNoPlayer, slot->state.owner);
  }
  return id;
}

World::PopulateReport World::populate_from_map(const MapObjectList& map, const ClassGraph& graph,
                                               const EntityResolver* entities) {
  set_class_graph(&graph);

  PopulateReport report;
  report.map_objects = map.objects().size();
  report.map_settlements = map.settlements().size();
  report.map_groups = map.groups().size();

  std::vector<ObjectId> settlement_ids(map.settlements().size(), kNoObject);
  // `num` -> the object spawned for it, for the `<group>` pass below. Kept as a
  // parallel vector in document order rather than derived afterwards, because a
  // `<scriptobj>` whose class the graph does not know is not spawned at all, so
  // the two lists are not index-for-index.
  std::vector<ObjectId> object_ids(map.objects().size(), kNoObject);
  std::size_t object_index = 0;

  // The settlement composite is allocated the moment the settlement is first
  // needed, immediately *before* its own objects. See the note on the header:
  // the ASUS tick-2 dump lays out
  //     0 Settlement, 1 Holder, 2 Warehouse, 3..13 that settlement's objects,
  //    14 Settlement, 15 Holder, 16 Warehouse, 17..  the next one's, ...
  // for all 39 settlements, and the triple is contiguous in 39 of 39.
  const auto settlement_for = [&](std::int32_t index) -> ObjectId {
    if (index < 0 || static_cast<std::size_t>(index) >= settlement_ids.size()) return kNoObject;
    ObjectId& id = settlement_ids[static_cast<std::size_t>(index)];
    if (id != kNoObject) return id;
    const MapSettlement& declared = map.settlements()[static_cast<std::size_t>(index)];
    // `player` is 1-based as authored, matching the object attribute, and 0
    // means unowned. map.md documents the convention for `<scriptobj>`; the
    // settlement attribute is parsed the same way and is assumed to share it.
    const PlayerId owner =
        declared.player > 0 ? static_cast<PlayerId>(declared.player - 1) : kNoPlayer;
    const SettlementIds ids = spawn_settlement(owner);
    id = ids.settlement;
    ++report.settlements;
    ++report.holders;
    ++report.warehouses;
    return id;
  };

  // Objects in document order -- which is `num` order in all 29 shipped maps,
  // and is what groups and scripts refer to.
  for (const MapObject& placed : map.objects()) {
    const std::size_t slot_index = object_index++;
    const ObjectId settlement = settlement_for(placed.settlement);

    bool ship = false;
    const ObjectId id = spawn_map_object(placed, Point{placed.x, placed.y}, settlement,
                                         placed.player, entities, &ship);
    if (id == kNoObject) {
      ++report.unresolved_class;
      continue;
    }
    if (ship) {
      ++report.ships;
      ++report.holders;
    }
    ++report.spawned;
    object_ids[slot_index] = id;
  }

  // A settlement no object referred to still exists; give it its handles after
  // the object pass rather than dropping it.
  for (std::size_t i = 0; i < settlement_ids.size(); ++i) {
    settlement_for(static_cast<std::int32_t>(i));
  }
  // Handed to the caller because the pairing is only knowable here: allocation
  // is by first reference, so index `i` of the map is not settlement `i` of the
  // world. `GameSession::seed_economy` uses it to carry `<settlement name>`.
  report.settlement_ids = settlement_ids;

  // The teleport pairs, resolved once, here, because here is the only place
  // that has both the map's settlement id table and the objects spawned for it.
  //
  // `destination_set` names the *settlement* the far teleport belongs to, by
  // that settlement's `id` attribute -- which is not its index: the two differ
  // on 97 settlements across the retail install. So the first pass builds
  // `id -> the teleport spawned inside it` and the second reads it back.
  //
  // The census that makes this well defined is on `MapObject::destination_set`:
  // 54 teleports, every one inside a settlement, no settlement with two, and
  // the relation symmetric in 54 of 54. A settlement naming no teleport, or a
  // second teleport in one settlement, leaves the pair at `kNoObject`, which is
  // the invalid handle every reader already tests for.
  std::vector<std::pair<std::int32_t, ObjectId>> teleport_of_settlement;
  for (std::size_t i = 0; i < map.objects().size(); ++i) {
    const MapObject& placed = map.objects()[i];
    if (placed.destination_set < 0 || object_ids[i] == kNoObject) continue;
    if (placed.settlement < 0 ||
        static_cast<std::size_t>(placed.settlement) >= map.settlements().size()) {
      continue;
    }
    teleport_of_settlement.emplace_back(map.settlements()[static_cast<std::size_t>(
                                           placed.settlement)].id,
                                       object_ids[i]);
  }
  for (std::size_t i = 0; i < map.objects().size(); ++i) {
    const MapObject& placed = map.objects()[i];
    if (placed.destination_set < 0 || object_ids[i] == kNoObject) continue;
    WorldObject* slot = find(object_ids[i]);
    if (slot == nullptr) continue;
    for (const auto& [settlement_id, teleport] : teleport_of_settlement) {
      if (settlement_id != placed.destination_set) continue;
      slot->state.teleport_destination = teleport;
      ++report.teleport_pairs;
      break;
    }
  }

  // The two `<group>` tables, after every object exists so that a member can be
  // resolved wherever in the document it was placed.
  //
  // `type` decides which table an element goes into, and that split is not
  // cosmetic: `Group` reads one and `GetNamedObj` the other, they are separate
  // runtime query types (`CVXGroupQuery`, `CVXNamedObjQuery`), and `gbr.exe`
  // warns that a name used for both makes the *group* unreachable. 483 typed
  // call sites in the shipped map scripts resolve into the expected table and
  // none into the other. See `sim/world.hpp`.
  //
  // Both tables are filled in document order, so a repeated name resolves the
  // same way on every peer: interned once for a group, first-binding-wins for a
  // named object.
  const auto object_for = [&](std::int32_t num) -> ObjectId {
    const MapObject* placed = map.find(num);
    if (placed == nullptr) return kNoObject;
    const auto at = static_cast<std::size_t>(placed - map.objects().data());
    return at < object_ids.size() ? object_ids[at] : kNoObject;
  };

  for (const MapGroup& authored : map.groups()) {
    if (authored.type == imperivm::core::kGroupAlias) {
      // A name for one object. Every one of the 1,378 shipped aliases has
      // exactly one member; a malformed one with several binds its first and
      // the rest are counted as duplicates rather than silently dropped.
      for (const std::int32_t num : authored.members) {
        const ObjectId id = object_for(num);
        if (id == kNoObject) {
          ++report.unresolved_members;
          continue;
        }
        if (named_objects_.bind(authored.name, id)) {
          ++report.memberships;
        } else {
          ++report.duplicate_names;
        }
      }
      continue;
    }

    const std::int32_t index = groups_.intern(authored.name);
    for (const std::int32_t num : authored.members) {
      const ObjectId id = object_for(num);
      if (id == kNoObject) {
        // Either the map names an object it does not declare -- which no
        // shipped map does, in 23,408 of 23,408 memberships -- or the object's
        // class was not in the graph and it was never spawned.
        ++report.unresolved_members;
        continue;
      }
      if (groups_.add(index, id)) ++report.memberships;
    }
  }
  report.groups = groups_.size();
  report.named_objects = named_objects_.size();

  // The three per-session singletons, after the world's objects. Every dump
  // shows them as one group of three consecutive handles, exactly once, and
  // late: 787/788/789 of a maximum handle of 900 in the ASUS tick-2 dump.
  // Query objects are later still -- 831..900 there -- and are created by
  // running scripts rather than by the loader, so nothing is minted here.
  spawn_singletons();
  report.singletons = 3;

  report.total_objects = objects_.size();
  report.object_ids = std::move(object_ids);
  return report;
}

// --------------------------------------------------------------------------
// systems
// --------------------------------------------------------------------------

bool World::add_system(System* system) {
  if (system == nullptr) return false;
  // Registering twice would run it twice and fold its hash twice, which is a
  // silent doubling of whatever it does.
  for (System* existing : systems_) {
    if (existing == system) return false;
  }
  systems_.push_back(system);
  return true;
}

void World::start() {
  for (System* system : systems_) system->start(*this);
}

// --------------------------------------------------------------------------
// animation
// --------------------------------------------------------------------------

bool World::has_anim(ObjectId id, std::int32_t slot) const noexcept {
  const WorldObject* found = find(id);
  return found != nullptr && found->object != nullptr && found->object->entity != nullptr &&
         found->object->entity->has_anim(slot);
}

bool World::play_anim(ObjectId id, std::int32_t slot, AnimRepeat repeat) {
  WorldObject* found = find(id);
  if (found == nullptr || found->object == nullptr || found->object->entity == nullptr) return false;

  const EntityAnim* anim = found->object->entity->anim(slot);
  if (anim == nullptr) return false;  // slots 0 and 16: ordinary, not an error

  AnimTimeline timeline = found->object->entity->timeline(*anim);
  if (!timeline.valid()) return false;

  found->timeline = std::move(timeline);
  found->repeat = repeat;
  found->animating = true;
  // A leg belongs to the animation that flies it; `Flying::PlayAnim` writes
  // its own after this (`sim/anim.cpp`).
  found->flight = FlightLeg{};
  AnimCursor& cursor = found->object->anim;
  cursor.anim_slot = slot;
  cursor.elapsed_ms = 0;
  cursor.step = found->timeline.sample(0, repeat).step;
  return true;
}

bool World::enter_state(ObjectId id, std::int32_t state_idx) {
  WorldObject* found = find(id);
  if (found == nullptr || found->object == nullptr || found->object->entity == nullptr) return false;

  const EntityState* state = found->object->entity->state(state_idx);
  if (state == nullptr) return false;

  found->object->anim.state_idx = state_idx;
  if (!state->has_anim()) {
    // A still pose. 987 of the 1,168 states are one.
    found->animating = false;
    found->object->anim.anim_slot = kNoAnim;
    found->object->anim.elapsed_ms = 0;
    found->object->anim.step = 0;
    found->timeline = AnimTimeline();
    found->flight = FlightLeg{};
    return true;
  }
  // The state's own animation loops for as long as the state is held; that is
  // the one loop-versus-hold split the data does support. See AnimRepeat.
  return play_anim(id, state->anim_idx, AnimRepeat::loop);
}

bool World::stop_anim(ObjectId id) {
  WorldObject* found = find(id);
  if (found == nullptr) return false;
  found->animating = false;
  return true;
}

// --------------------------------------------------------------------------
// the loop
// --------------------------------------------------------------------------

const Turn& World::advance() { return advance(clock_.turn_length()); }

const Turn& World::advance(std::int32_t turn_length) {
  const Turn& turn = clock_.advance(turn_length);
  run_turn(turn.length);
  return turn;
}

void World::advance(std::span<const std::int32_t> turn_lengths) {
  for (const std::int32_t length : turn_lengths) advance(length);
}

void World::advance_turns(std::uint64_t count) {
  for (std::uint64_t i = 0; i < count; ++i) advance(clock_.turn_length());
}

void World::run_turn(std::int32_t length) {
  // Objects in spawn order, never in any other. See the header.
  for (WorldObject& slot : objects_) {
    if (slot.object == nullptr) continue;  // internal objects do not animate
    slot.object->on_tick(static_cast<std::uint32_t>(clock_.turns()));
    if (!slot.animating || !slot.timeline.valid()) continue;

    AnimCursor& cursor = slot.object->anim;
    cursor.elapsed_ms = advance_elapsed(slot.timeline, cursor.elapsed_ms, length, slot.repeat);
    const AnimSample sample = slot.timeline.sample(cursor.elapsed_ms, slot.repeat);
    cursor.step = sample.step;
    if (sample.finished) slot.animating = false;
  }

  // Every gate's line, learnt as gates appear and dropped as they go. A
  // route's search lays the ones that bar its mover; see `sim/gate.hpp`.
  gate_lines_.refresh(*this);

  // Then the systems, in registration order. After the object pass rather than
  // before it so that a system observes the animation state of the turn it is
  // running in; the order between the two is a choice, and this is it.
  for (System* system : systems_) system->advance(*this, clock_.turn());
}

std::uint64_t World::state_hash() const noexcept {
  std::uint64_t state = kFnvOffset;
  hash_u64(state, clock_.turns());
  hash_u64(state, static_cast<std::uint64_t>(clock_.time()));
  hash_u64(state, static_cast<std::uint64_t>(clock_.turn_length()));
  hash_u64(state, static_cast<std::uint64_t>(clock_.config().game_speed));
  // The two seeds the dump's `[SEEDS]` block records. Both are world state and
  // both must match between peers, so both are in the hash.
  hash_u64(state, rng_.state());
  hash_u64(state, next_command_id_);
  hash_u64(state, objects_.size());

  for (const WorldObject& slot : objects_) {
    hash_u64(state, slot.id);
    hash_u64(state, static_cast<std::uint64_t>(slot.internal));
    // The common header, in the original's own terms.
    hash_i32(state, slot.state.position.x);
    hash_i32(state, slot.state.position.y);
    hash_u64(state, slot.state.owner);
    hash_u64(state, pack_sync_flags(slot.state));
    // And the flags the sync projection cannot carry. `pack_sync_flags` is a
    // *projection* onto the original's `+0x2c` word, for diffing against a
    // dump; two of this engine's flags do not live on that word in `gbr.exe`
    // and so are absent from it -- `no_ai` is bit 18 of a second word at
    // `+0x194`, and `built` is the plain int at `catapult + 0x208`. Both are
    // saved and both are real state, so folding only the projection left two
    // worlds that a save round-trip can tell apart and the hash could not.
    // `pack_object_flags` is the representation rather than the projection,
    // which is exactly the distinction its own comment draws.
    hash_u64(state, pack_object_flags(slot.state.flags));
    hash_u64(state, slot.state.holder);
    hash_i32(state, slot.state.health);
    hash_i32(state, slot.state.stamina);
    // Scripts' own scratch, and hashed for the reason `ObjectState::user`
    // gives: a flock steers on it.
    hash_i32(state, slot.state.user);
    hash_i32(state, slot.state.z_from);
    hash_i32(state, slot.state.z_to);
    // A mule's load. Hashed for `user`'s reason: `ES_OUTPOSTSELLGOLD.VS` sums
    // it over a group and sells on the total.
    hash_i32(state, slot.state.cargo);
    hash_i32(state, slot.state.cargo_resource);
    // A dying priest's grudge. Hashed for `user`'s reason: a script branches
    // on it and the branch deals damage.
    hash_u64(state, slot.state.jupiter_target);
    // Which ship last asked this unit to board. Hashed for `user`'s reason: two
    // `..._ONFINISH` scripts reach back through it to cancel a boarding.
    hash_u64(state, slot.state.ship_to_board);
    hash_u64(state, static_cast<std::uint32_t>(slot.state.damage_taken));
    // **The sight radius is state now**, and was not before: it used to be
    // resolved from the class at spawn and never moved, so it was written into
    // the save (a reload could not recompute it once the graph was gone) and
    // left out of the hash. `Obj::SetSight` moves it, and two peers that
    // disagree about how far a scout sees disagree about what it finds.
    hash_i32(state, slot.sight);
    // A raised shield and a damage tier. Both are hashed for the same reason
    // `user` above is: a script branches on them, so two peers that disagree
    // give two different orders. See `ObjectState`.
    hash_i32(state, slot.state.parry_mode);
    hash_i32(state, slot.state.damage_state);
    // The player's manual target for a tower. Hashed because a tower fires at
    // it -- see `ObjectState::ui_target`.
    hash_u64(state, slot.state.ui_target);
    // The two effect tags: who is burning this unit, and who shelters it.
    hash_u64(state, slot.state.mist);
    hash_u64(state, slot.state.sheltered_by);
    // The teleport pair and who has been through it. Both are read by scripts
    // that then move units, so two peers that disagreed would move them
    // somewhere else. See `ObjectState::teleport_destination`.
    hash_u64(state, slot.state.teleport_destination);
    hash_u64(state, slot.state.traversed_by);
    // A gate's portcullis motion: where it stands decides whether units pass.
    if (slot.object != nullptr && slot.object->is_a(NativeClass::gate)) {
      hash_u64(state, static_cast<std::uint64_t>(slot.gate.start));
      hash_i32(state, slot.gate.from);
    }
    hash_u64(state, slot.settlement);

    if (slot.internal == InternalKind::query) {
      // A query's *definition* is state: two worlds whose queries ask different
      // questions are different worlds even before either is evaluated.
      const QuerySpec& spec = slot.query < queries_.size() ? queries_[slot.query] : QuerySpec{};
      hash_u64(state, static_cast<std::uint64_t>(spec.kind));
      hash_u64(state, spec.subject);
      hash_i32(state, spec.center.x);
      hash_i32(state, spec.center.y);
      hash_i32(state, spec.radius);
      hash_i32(state, spec.left);
      hash_i32(state, spec.top);
      hash_i32(state, spec.right);
      hash_i32(state, spec.bottom);
      hash_u64(state, spec.area_is_rect ? 1u : 0u);
      hash_u64(state, static_cast<std::uint64_t>(spec.settlement_scope));
      hash_u64(state, spec.player);
      hash_i32(state, spec.flags_type);
      hash_i32(state, spec.group);
      hash_u64(state, spec.visible_only ? 1u : 0u);
      hash_u64(state, spec.lhs);
      hash_u64(state, spec.rhs);
      hash_u64(state, static_cast<std::uint64_t>(spec.op));
      hash_u64(state, spec.filter.match_all ? 1u : 0u);
      hash_u64(state, spec.filter.count);
      for (std::uint8_t i = 0; i < spec.filter.count; ++i) hash_u64(state, spec.filter.classes[i]);
    }

    if (slot.object == nullptr) continue;
    const AnimCursor& cursor = slot.object->anim;
    hash_i32(state, cursor.state_idx);
    hash_i32(state, cursor.anim_slot);
    hash_i32(state, cursor.elapsed_ms);
    hash_u64(state, cursor.step);
    hash_u64(state, cursor.variation);
    hash_u64(state, slot.animating ? 1u : 0u);
    hash_u64(state, static_cast<std::uint64_t>(slot.repeat));
  }

  // Named object groups. Hashed for the same reason the object table is: two
  // peers that disagree about who is in `Group("Attackers")` send different
  // armies to different places on the next `SetCommand`, and the disagreement
  // would otherwise only show up as the divergence it caused. Folded after the
  // objects and before the systems, which fixes its position in the stream.
  groups_.hash(state);
  // And the named objects. A binding outlives its object -- `NamedObj::IsDead`
  // says so -- which makes it state that despawn does not clean up, and all the
  // more reason for both peers to agree on it.
  named_objects_.hash(state);
  // And the ships' boarding lists. The same kind of thing again -- a relation
  // between objects that no object owns -- and state for the same reason:
  // `SHIP_BOARD.VS` loops while `AreUnitsToBoard` and `UNIT_BOARD_COMMON.VS`
  // stops when `NumUnitsToBoard` reaches zero, so two peers that disagree about
  // a row run two different loops.
  boarding_.hash(state);

  // Systems fold in registration order, which is why registration order is
  // part of the simulation's definition rather than a detail.
  //
  // The position and the name are folded in *before* each system's own
  // contribution, and that is load-bearing rather than decorative: a system
  // whose `hash` is commutative -- an XOR or a sum, both of which are the
  // obvious things to write -- would otherwise make two different pipelines
  // hash identically. Mixing here means run order shows up as a hash difference
  // whatever a domain does on its side, and folding the name means a system
  // that silently failed to register does too.
  hash_u64(state, systems_.size());
  for (std::size_t i = 0; i < systems_.size(); ++i) {
    hash_u64(state, i);
    for (const char c : systems_[i]->name()) hash_u64(state, static_cast<std::uint8_t>(c));
    systems_[i]->hash(state);
  }
  return state;
}

WorldHashes World::hashes() const noexcept {
  WorldHashes out;
  out.slots = state_hash();
  // `threads` and `netcmds` stay zero until the script scheduler and the
  // command pump register systems that fold them in. The other four stay zero
  // permanently: they are zero in all nine retail dumps, and making one of them
  // non-zero would mean a subsystem the shipped build kept out of the
  // determinism contract has been pulled into hashed state.
  std::uint64_t roll = kFnvOffset;
  hash_u64(roll, out.slots);
  hash_u64(roll, out.threads);
  hash_u64(roll, out.netcmds);
  hash_u64(roll, out.extrahash);
  out.hash_of_hashes = roll;
  return out;
}

// --------------------------------------------------------------------------
// the saved game
// --------------------------------------------------------------------------
//
// See docs/formats/save.md for the layout and for the evidence behind what is
// in it. Two rules shape every line below:
//
//   * **Iteration order is state**, so everything is written in the order it is
//     held in and read back into the same order. Nothing is sorted on the way
//     out and nothing is keyed by a container that would reorder it.
//   * **A field that is not written comes back as a default**, which is the
//     failure mode a save has to avoid. So the writer walks the members in
//     declaration order and the format document lists them in that order; a
//     member added to `World` without a line here is a member the document will
//     not account for.

namespace {

constexpr std::uint32_t kWorldMagic = 0x444C5749u;  // "IWLD"
constexpr std::uint32_t kWorldVersion = 34;  // 34: no `gate_passable`; 33: a gate's motion, `gate_passable`; 32: `display_name`; 31: `training`

/// `0xFF` for a slot with no native object -- every internal kind.
/// `NativeClass` is a `std::uint8_t` enum with 26 values, so the sentinel
/// cannot collide with one.
constexpr std::uint8_t kNoNativeClass = 0xFFu;
/// Every bit `pack_object_flags` can set. A word with any other bit is a save
/// from a build that knew a flag this one does not.
///
/// **Widen it in the same commit as the bit.** `landing` was added to
/// `pack_object_flags` and not here, so `deserialize` refused every save that
/// had it set -- and nothing failed, because the save fixture's flag pattern
/// had quietly stopped setting the last eight bits it declared. Two blind
/// spots that only bite together: the mask is only reachable through a saved
/// world that actually carries the bit. See
/// `save_the_fixture_sets_and_clears_every_writable_flag`.
constexpr std::uint32_t kObjectFlagMask = 0x7FFFFFFu;  // 27 bits since `training`, again since `gate_passable` went

void put_query_spec(std::vector<std::byte>& out, const QuerySpec& spec) {
  bytes::put_u8(out, static_cast<std::uint32_t>(spec.kind));
  bytes::put_u32(out, spec.subject);
  bytes::put_i32(out, spec.center.x);
  bytes::put_i32(out, spec.center.y);
  bytes::put_i32(out, spec.radius);
  bytes::put_i32(out, spec.left);
  bytes::put_i32(out, spec.top);
  bytes::put_i32(out, spec.right);
  bytes::put_i32(out, spec.bottom);
  bytes::put_u8(out, spec.area_is_rect ? 1u : 0u);
  bytes::put_u8(out, static_cast<std::uint32_t>(spec.settlement_scope));
  bytes::put_u8(out, spec.player);
  bytes::put_i32(out, spec.flags_type);
  bytes::put_i32(out, spec.group);
  bytes::put_u8(out, spec.visible_only ? 1u : 0u);
  bytes::put_u32(out, spec.lhs);
  bytes::put_u32(out, spec.rhs);
  bytes::put_u8(out, static_cast<std::uint32_t>(spec.op));
  bytes::put_u8(out, spec.filter.match_all ? 1u : 0u);
  bytes::put_u8(out, spec.filter.count);
  for (std::uint8_t i = 0; i < spec.filter.count; ++i) {
    bytes::put_u32(out, spec.filter.classes[i]);
  }
}

[[nodiscard]] bool get_query_spec(ByteReader& reader, QuerySpec& spec) {
  std::uint8_t kind = 0;
  std::uint8_t area_is_rect = 0;
  std::uint8_t settlement_scope = 0;
  std::uint8_t player = 0;
  std::uint8_t visible = 0;
  std::uint8_t op = 0;
  std::uint8_t match_all = 0;
  std::uint8_t count = 0;
  if (!reader.u8(kind) || !reader.u32(spec.subject) || !bytes::get_i32(reader, spec.center.x) ||
      !bytes::get_i32(reader, spec.center.y) || !bytes::get_i32(reader, spec.radius) ||
      !bytes::get_i32(reader, spec.left) || !bytes::get_i32(reader, spec.top) ||
      !bytes::get_i32(reader, spec.right) || !bytes::get_i32(reader, spec.bottom) ||
      !reader.u8(area_is_rect) || !reader.u8(settlement_scope) || !reader.u8(player) ||
      !bytes::get_i32(reader, spec.flags_type) ||
      !bytes::get_i32(reader, spec.group) || !reader.u8(visible) || !reader.u32(spec.lhs) ||
      !reader.u32(spec.rhs) || !reader.u8(op) || !reader.u8(match_all) || !reader.u8(count)) {
    return false;
  }
  // An enumeration arriving from a file is range-checked before it becomes
  // one: a `QueryKind` outside the enum would index the evaluation switch with
  // a value it was not written for.
  if (kind >= static_cast<std::uint8_t>(QueryKind::count)) return false;
  if (op > static_cast<std::uint8_t>(SetOp::subtract)) return false;
  if (settlement_scope > static_cast<std::uint8_t>(SettlementScope::both)) return false;
  if (count > ClassFilter::kMaxClasses) return false;
  spec.kind = static_cast<QueryKind>(kind);
  spec.area_is_rect = area_is_rect != 0;
  spec.settlement_scope = static_cast<SettlementScope>(settlement_scope);
  spec.player = player;
  spec.visible_only = visible != 0;
  spec.op = static_cast<SetOp>(op);
  spec.filter.match_all = match_all != 0;
  spec.filter.count = count;
  spec.filter.classes = {};
  for (std::uint8_t i = 0; i < count; ++i) {
    if (!reader.u32(spec.filter.classes[i])) return false;
  }
  return true;
}

void put_player_setup(std::vector<std::byte>& out, const PlayerSetup& setup) {
  bytes::put_string(out, setup.name);
  bytes::put_string(out, setup.race);
  bytes::put_string(out, setup.allowed_races);
  bytes::put_u8(out, static_cast<std::uint32_t>(setup.control));
  bytes::put_i32(out, setup.difficulty);
  bytes::put_u16(out, setup.colour);
  bytes::put_i32(out, setup.start.x);
  bytes::put_i32(out, setup.start.y);
  bytes::put_i32(out, setup.bonus);
  bytes::put_u8(out, setup.allied_flag ? 1u : 0u);
  bytes::put_string(out, setup.ai_script);
}

[[nodiscard]] bool get_player_setup(ByteReader& reader, PlayerSetup& setup) {
  std::uint8_t control = 0;
  std::uint16_t colour = 0;
  std::uint8_t allied = 0;
  if (!bytes::get_string(reader, setup.name) || !bytes::get_string(reader, setup.race) ||
      !bytes::get_string(reader, setup.allowed_races) || !reader.u8(control) ||
      !bytes::get_i32(reader, setup.difficulty) || !reader.u16(colour) ||
      !bytes::get_i32(reader, setup.start.x) || !bytes::get_i32(reader, setup.start.y) ||
      !bytes::get_i32(reader, setup.bonus) || !reader.u8(allied) ||
      !bytes::get_string(reader, setup.ai_script)) {
    return false;
  }
  if (control > static_cast<std::uint8_t>(PlayerControl::both)) return false;
  setup.control = static_cast<PlayerControl>(control);
  setup.colour = colour;
  setup.allied_flag = allied != 0;
  return true;
}

/// Put a clock back where it was.
///
/// **The clock's whole state is `(turn_length, game_speed, time, index,
/// last_length)`**; `Turn::start`, `Turn::end` and `Turn::time` are pure
/// functions of the time the turn began at and its length, exactly as the nine
/// dumps record (`gametimetickstart = gametime + 1`,
/// `gametimetickend = start + length`). So those three are not stored, for the
/// same reason `write_trace` does not store `hash_of_hashes`: a field whose only
/// legal value is derived is a field somebody will one day edit into a
/// contradiction. They are rebuilt here, and this is the only place that knows
/// how, so the derivation has one home rather than three.
///
/// This used to *replay* `index` turns whose lengths summed to `time`, because
/// `Clock` had no way to set accumulated time. `Clock::restore` now does, so the
/// loop is gone: the restore is O(1) rather than O(saved turns), and the shape
/// of the intermediate schedule -- which was arbitrary, unobservable and
/// therefore a lie the file did not tell -- is no longer invented at all.
[[nodiscard]] bool restore_clock(Clock& clock, const TickConfig& config, GameTime time,
                                 std::uint64_t index, std::int32_t last_length) {
  clock = Clock(config);
  if (index == 0) {
    // A world that has never advanced. Any other reading of these fields is a
    // save that did not come from this engine.
    return time == 0 && last_length == 0;
  }
  // `Clock::advance` refuses a non-positive length and substitutes the
  // configured one, which is positive, so every turn a real session ran was at
  // least one unit long. A save that says otherwise -- a zero-length last turn,
  // or a total shorter than the last turn alone, or fewer units of game time
  // than turns to account for them -- never occurred.
  if (last_length <= 0 || time < last_length) return false;
  if (static_cast<std::uint64_t>(time - last_length) < index - 1) return false;

  Turn turn;
  turn.index = index;
  turn.length = last_length;
  turn.time = time;
  turn.start = time - last_length + 1;
  turn.end = turn.start + last_length;
  clock.restore(time, turn);
  return true;
}

}  // namespace

void World::serialize(std::vector<std::byte>& out) const {
  bytes::put_u32(out, kWorldMagic);
  bytes::put_u32(out, kWorldVersion);

  // -- the clock. All of it is world state (sim/tick.hpp): two peers that
  // disagree on the turn length or the speed are running different
  // simulations, which is why the dump records both.
  bytes::put_i32(out, clock_.config().turn_length);
  bytes::put_i32(out, clock_.config().game_speed);
  bytes::put_i64(out, clock_.time());
  bytes::put_u64(out, clock_.turn().index);
  bytes::put_i32(out, clock_.turn().length);

  // -- the two seeds the dump's `[SEEDS]` block records, and the handle
  // counter. Handles are never reused, so `next_id_` is state in its own
  // right: a reload that restarted it would hand a live object's id to a new
  // one.
  bytes::put_u32(out, rng_.state());
  bytes::put_u32(out, next_command_id_);
  bytes::put_u32(out, next_id_);

  // -- the object table, in spawn order, which is the order it is ticked in.
  bytes::put_u32(out, static_cast<std::uint32_t>(objects_.size()));
  for (const WorldObject& slot : objects_) {
    bytes::put_u32(out, slot.id);
    bytes::put_u8(out, static_cast<std::uint32_t>(slot.internal));
    bytes::put_u8(out, slot.object != nullptr
                           ? static_cast<std::uint32_t>(slot.object->native_class())
                           : kNoNativeClass);
    bytes::put_u32(out, slot.class_index);
    bytes::put_u32(out, slot.settlement);
    // Derived from the class graph at spawn and never written since -- and
    // written here anyway. Recomputing it on load would make a save's meaning
    // depend on a `sight` property in the class data staying put, which is
    // exactly the silent drift a versioned save exists to prevent.
    bytes::put_i32(out, slot.sight);
    bytes::put_u32(out, slot.query);

    bytes::put_i32(out, slot.state.position.x);
    bytes::put_i32(out, slot.state.position.y);
    bytes::put_u8(out, slot.state.owner);
    bytes::put_u32(out, pack_object_flags(slot.state.flags));
    bytes::put_u32(out, slot.state.holder);
    bytes::put_i32(out, slot.state.health);
    bytes::put_i32(out, slot.state.stamina);
    bytes::put_i32(out, slot.state.user);
    bytes::put_i32(out, slot.state.z_from);
    bytes::put_i32(out, slot.state.z_to);
    bytes::put_i32(out, slot.state.cargo);
    bytes::put_i32(out, slot.state.cargo_resource);
    bytes::put_u32(out, slot.state.jupiter_target);
    bytes::put_u32(out, slot.state.ship_to_board);
    bytes::put_i32(out, slot.state.damage_taken);
    bytes::put_i32(out, slot.state.parry_mode);
    bytes::put_i32(out, slot.state.damage_state);
    bytes::put_u32(out, slot.state.ui_target);
    bytes::put_u32(out, slot.state.mist);
    bytes::put_u32(out, slot.state.sheltered_by);
    bytes::put_u32(out, slot.state.teleport_destination);
    bytes::put_u32(out, slot.state.traversed_by);

    // The animation cursor. `AnimTimeline` is not written: it is re-resolved
    // from the entity on load, and a timeline in a file would be a copy of game
    // data that could disagree with the game data.
    bytes::put_u8(out, slot.animating ? 1u : 0u);
    bytes::put_u8(out, static_cast<std::uint32_t>(slot.repeat));
    // World version 32: the map's `display_name`.
    bytes::put_string(out, slot.display_name);
    if (slot.object != nullptr) {
      const AnimCursor& cursor = slot.object->anim;
      bytes::put_i32(out, cursor.state_idx);
      bytes::put_i32(out, cursor.anim_slot);
      bytes::put_i32(out, cursor.elapsed_ms);
      bytes::put_u32(out, cursor.step);
      bytes::put_u32(out, cursor.variation);
      // World version 33: a gate's portcullis motion, which decides when it
      // lets units through.
      if (slot.object->is_a(NativeClass::gate)) {
        bytes::put_i64(out, slot.gate.start);
        bytes::put_i32(out, slot.gate.from);
      }
    }
  }

  // -- query specs, by index. Written whole, dead slots and all: a destroyed
  // query leaves its slot behind because compaction would renumber every live
  // query, and the numbering is hashed.
  bytes::put_u32(out, static_cast<std::uint32_t>(queries_.size()));
  for (const QuerySpec& spec : queries_) put_query_spec(out, spec);

  // -- the players. Not folded into `state_hash`, and written all the same:
  // combat cannot pick a target and the economy cannot decide who a tribute may
  // go to without the relation matrix, so a game that lost it would diverge on
  // the first fight rather than at the load.
  bytes::put_u32(out, static_cast<std::uint32_t>(kPlayerCount));
  for (std::size_t i = 0; i < kPlayerCount; ++i) {
    put_player_setup(out, players_.setup(static_cast<PlayerId>(i)));
  }
  for (std::size_t from = 0; from < kPlayerCount; ++from) {
    for (std::size_t to = 0; to < kPlayerCount; ++to) {
      bytes::put_u32(
          out, players_.relation_word(static_cast<PlayerId>(from), static_cast<PlayerId>(to)));
    }
  }

  // -- the two name tables, in interning / document order, which *is* their
  // index order and is what a `QuerySpec::group` refers to.
  bytes::put_u32(out, static_cast<std::uint32_t>(groups_.size()));
  for (std::size_t i = 0; i < groups_.size(); ++i) {
    const auto index = static_cast<std::int32_t>(i);
    bytes::put_string(out, groups_.name(index));
    const std::span<const ObjectId> members = groups_.members(index);
    bytes::put_u32(out, static_cast<std::uint32_t>(members.size()));
    for (const ObjectId id : members) bytes::put_u32(out, id);
  }

  // The rectangles, in interning order, which is index order and is what a
  // suspended script's locals refer to.
  bytes::put_u32(out, static_cast<std::uint32_t>(rects_.size()));
  for (const RectTable::Rect& r : rects_.all()) {
    bytes::put_i32(out, r.left);
    bytes::put_i32(out, r.top);
    bytes::put_i32(out, r.right);
    bytes::put_i32(out, r.bottom);
  }

  bytes::put_u32(out, static_cast<std::uint32_t>(named_objects_.size()));
  for (std::size_t i = 0; i < named_objects_.size(); ++i) {
    const auto index = static_cast<std::int32_t>(i);
    bytes::put_string(out, named_objects_.name(index));
    bytes::put_u32(out, named_objects_.object(index));
  }

  // -- the boarding lists, ascending by ship, members in notify order, which
  // is the order `BestCandidateToBoard` breaks a distance tie by.
  bytes::put_u32(out, static_cast<std::uint32_t>(boarding_.rows()));
  for (std::size_t i = 0; i < boarding_.rows(); ++i) {
    bytes::put_u32(out, boarding_.ship_at(i));
    const std::span<const ObjectId> members = boarding_.members_at(i);
    bytes::put_u32(out, static_cast<std::uint32_t>(members.size()));
    for (const ObjectId id : members) bytes::put_u32(out, id);
  }

  // -- the system run order. A manifest, not state: the systems own their own
  // sections of the save. It is here so that `deserialize` can refuse a save
  // whose pipeline is not this one, because run order is folded into `slots`
  // ahead of every system's contribution and a mismatch would explain every
  // hash difference under it.
  bytes::put_u32(out, static_cast<std::uint32_t>(systems_.size()));
  for (const System* system : systems_) {
    bytes::put_string(out, system == nullptr ? std::string_view() : system->name());
  }

  // -- the script-owned collections. Last, because it is the one part of the
  // world that is *not* hashed -- `scriptstate` is zero in all nine dumps --
  // and a reader skimming this function should meet the hashed state first.
  //
  // Saved all the same, because "hashed" and "saved" are different sets and
  // saved is the larger one: an `ObjList` handle sits in the local slot of a
  // suspended script, the shipped AI scripts suspend holding one constantly
  // (`ol = Group("GoldMules" + idPlayer).GetObjList()` and hundreds of sites
  // like it), and a load that dropped the pool would hand every one of them an
  // empty list without a single hash noticing.
  //
  // Length-prefixed rather than run to the end of the section, so that this
  // stays a field among fields and the next person to append one does not have
  // to move it.
  std::vector<std::byte> pool;
  objlists_.serialize(pool);
  bytes::put_u32(out, static_cast<std::uint32_t>(pool.size()));
  out.insert(out.end(), pool.begin(), pool.end());

  // And the script-side arrays, on the same terms and for the same reason: not
  // hashed, because `scriptstate` is zero in all nine dumps, and therefore
  // restored here rather than trusted to a hash check that cannot see them.
  std::vector<std::byte> arrays;
  arrays_.serialize(arrays);
  bytes::put_u32(out, static_cast<std::uint32_t>(arrays.size()));
  out.insert(out.end(), arrays.begin(), arrays.end());

  // And the squad lists, on exactly the same terms. A list carries a cursor as
  // well as members, so a save taken mid-walk resumes mid-walk: `GS_GUARD.VS`
  // is `SL.Lock; while (SL.EOL == false) { ... SL.Next(); }` around a `Sleep`
  // in the loop above it, and a load that rewound would order every squad in
  // the list a second time.
  std::vector<std::byte> squadlists;
  squadlists_.serialize(squadlists);
  bytes::put_u32(out, static_cast<std::uint32_t>(squadlists.size()));
  out.insert(out.end(), squadlists.begin(), squadlists.end());

  // And the conversations, which carry what `Init` bound and what `SetActor`
  // bound to it. A save taken between the two -- and 63 `SetActor` calls sit
  // between an `Init` and a `Run`, some of them with a `Sleep` in between --
  // has to come back with the same cast.
  std::vector<std::byte> conversations;
  conversations_.serialize(conversations);
  bytes::put_u32(out, static_cast<std::uint32_t>(conversations.size()));
  out.insert(out.end(), conversations.begin(), conversations.end());
}

Status World::deserialize(std::span<const std::byte> data, EntityResolver* entities) {
  ByteReader reader(data);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  if (!reader.u32(magic) || !reader.u32(version)) return FormatError::truncated;
  if (magic != kWorldMagic) return FormatError::bad_magic;
  if (version != kWorldVersion) return FormatError::unsupported;

  TickConfig config;
  GameTime time = 0;
  std::uint64_t turn_index = 0;
  std::int32_t turn_length = 0;
  if (!bytes::get_i32(reader, config.turn_length) || !bytes::get_i32(reader, config.game_speed) ||
      !bytes::get_i64(reader, time) || !bytes::get_u64(reader, turn_index) ||
      !bytes::get_i32(reader, turn_length)) {
    return FormatError::truncated;
  }
  // A non-positive turn length would stall the simulation for ever and a
  // non-positive speed would make every turn zero units long. Refused rather
  // than clamped: a save that says either did not come from a running game.
  if (!config.valid() || time < 0) return FormatError::malformed;

  std::uint32_t seed = 0;
  std::uint32_t next_command_id = 0;
  std::uint32_t next_id = 0;
  std::uint32_t object_count = 0;
  if (!reader.u32(seed) || !reader.u32(next_command_id) || !reader.u32(next_id) ||
      !reader.u32(object_count)) {
    return FormatError::truncated;
  }

  std::vector<WorldObject> objects;
  ObjectId previous = kNoObject;
  for (std::uint32_t i = 0; i < object_count; ++i) {
    // Nothing is reserved from `object_count`: a hostile count would otherwise
    // ask for gigabytes before the read that refuses it ever ran. Objects are
    // appended one at a time and the read that runs off the end fails. Same
    // reasoning `SelectionTable::deserialize` records.
    WorldObject slot;
    std::uint8_t internal = 0;
    std::uint8_t native = 0;
    std::uint8_t owner = 0;
    std::uint32_t flags = 0;
    std::uint8_t animating = 0;
    std::uint8_t repeat = 0;
    if (!reader.u32(slot.id) || !reader.u8(internal) || !reader.u8(native) ||
        !reader.u32(slot.class_index) || !reader.u32(slot.settlement) ||
        !bytes::get_i32(reader, slot.sight) || !reader.u32(slot.query) ||
        !bytes::get_i32(reader, slot.state.position.x) ||
        !bytes::get_i32(reader, slot.state.position.y) || !reader.u8(owner) ||
        !reader.u32(flags) || !reader.u32(slot.state.holder) ||
        !bytes::get_i32(reader, slot.state.health) ||
        !bytes::get_i32(reader, slot.state.stamina) ||
        !bytes::get_i32(reader, slot.state.user) ||
        !bytes::get_i32(reader, slot.state.z_from) ||
        !bytes::get_i32(reader, slot.state.z_to) ||
        !bytes::get_i32(reader, slot.state.cargo) ||
        !bytes::get_i32(reader, slot.state.cargo_resource) ||
        !reader.u32(slot.state.jupiter_target) ||
        !reader.u32(slot.state.ship_to_board) ||
        !bytes::get_i32(reader, slot.state.damage_taken) ||
        !bytes::get_i32(reader, slot.state.parry_mode) ||
        !bytes::get_i32(reader, slot.state.damage_state) ||
        !reader.u32(slot.state.ui_target) || !reader.u32(slot.state.mist) ||
        !reader.u32(slot.state.sheltered_by) ||
        !reader.u32(slot.state.teleport_destination) ||
        !reader.u32(slot.state.traversed_by) || !reader.u8(animating) ||
        !reader.u8(repeat) || !bytes::get_string(reader, slot.display_name)) {
      return FormatError::truncated;
    }
    if (internal >= static_cast<std::uint8_t>(InternalKind::count)) return FormatError::malformed;
    if (repeat > static_cast<std::uint8_t>(AnimRepeat::hold)) return FormatError::malformed;
    if ((flags & ~kObjectFlagMask) != 0) return FormatError::unsupported;
    // Ids are monotonic and never reused, and `find` binary-searches on that.
    // A table that is out of order would make every lookup wrong in a way no
    // later check would notice, so it is refused here.
    if (slot.id == kNoObject || slot.id <= previous) return FormatError::malformed;
    previous = slot.id;

    slot.internal = static_cast<InternalKind>(internal);
    slot.state.owner = owner;
    slot.state.flags = unpack_object_flags(flags);
    slot.animating = animating != 0;
    slot.repeat = static_cast<AnimRepeat>(repeat);

    if (native != kNoNativeClass) {
      if (native >= static_cast<std::uint8_t>(NativeClass::count)) return FormatError::malformed;
      // `object` is null exactly when `internal != none`, and everything from
      // `collect` to `pack_sync_flags` relies on it.
      if (slot.internal != InternalKind::none) return FormatError::malformed;
      slot.object = make_native_object(static_cast<NativeClass>(native));
      slot.object->id = slot.id;
      slot.object->class_index = slot.class_index;
      AnimCursor& cursor = slot.object->anim;
      if (!bytes::get_i32(reader, cursor.state_idx) || !bytes::get_i32(reader, cursor.anim_slot) ||
          !bytes::get_i32(reader, cursor.elapsed_ms) || !reader.u32(cursor.step) ||
          !reader.u32(cursor.variation)) {
        return FormatError::truncated;
      }
      if (slot.object->is_a(NativeClass::gate) &&
          (!bytes::get_i64(reader, slot.gate.start) || !bytes::get_i32(reader, slot.gate.from))) {
        return FormatError::truncated;
      }
      // Art binding and the resolved timeline are load-time state, rebuilt the
      // way `populate_from_map` and `play_anim` build them rather than stored.
      if (entities != nullptr && classes_ != nullptr && slot.class_index != kNoClass) {
        const std::string_view path = classes_->entity_path(slot.class_index, Season::base);
        if (!path.empty()) slot.object->entity = entities->resolve(path);
      }
      if (slot.object->entity != nullptr && cursor.anim_slot != kNoAnim) {
        slot.timeline = slot.object->entity->timeline_for_slot(cursor.anim_slot);
      }
    } else if (slot.internal == InternalKind::none) {
      // Neither a native object nor an internal kind is a slot that is nothing
      // at all: every consumer would skip it and the hash would still count it.
      // Refuse rather than materialise a ghost.
      return FormatError::malformed;
    }
    objects.push_back(std::move(slot));
  }

  std::uint32_t query_count = 0;
  if (!reader.u32(query_count)) return FormatError::truncated;
  std::vector<QuerySpec> queries;
  for (std::uint32_t i = 0; i < query_count; ++i) {
    QuerySpec spec;
    if (!get_query_spec(reader, spec)) return FormatError::truncated;
    queries.push_back(spec);
  }
  // A query object indexes this table. An index past its end would evaluate
  // against a default spec, which is a different question asked silently.
  for (const WorldObject& slot : objects) {
    if (slot.internal == InternalKind::query && slot.query >= queries.size()) {
      return FormatError::out_of_range;
    }
  }

  std::uint32_t players = 0;
  if (!reader.u32(players)) return FormatError::truncated;
  if (players != kPlayerCount) return FormatError::unsupported;
  PlayerTable table;
  for (std::size_t i = 0; i < kPlayerCount; ++i) {
    if (!get_player_setup(reader, table.setup(static_cast<PlayerId>(i)))) {
      return FormatError::truncated;
    }
  }
  for (std::size_t from = 0; from < kPlayerCount; ++from) {
    for (std::size_t to = 0; to < kPlayerCount; ++to) {
      std::uint32_t word = 0;
      if (!reader.u32(word)) return FormatError::truncated;
      table.set_relation_word(static_cast<PlayerId>(from), static_cast<PlayerId>(to), word);
    }
  }

  std::uint32_t group_count = 0;
  if (!reader.u32(group_count)) return FormatError::truncated;
  GroupTable groups;
  for (std::uint32_t i = 0; i < group_count; ++i) {
    std::string name;
    std::uint32_t members = 0;
    if (!bytes::get_string(reader, name) || !reader.u32(members)) return FormatError::truncated;
    // Interning in file order reproduces the indices exactly, which is what a
    // stored `QuerySpec::group` and every live `Group(...)` handle refer to. A
    // repeated name would silently merge two groups into one index and
    // renumber every group after it.
    if (groups.find(name) != GroupTable::kNoGroup) return FormatError::malformed;
    const std::int32_t index = groups.intern(name);
    for (std::uint32_t m = 0; m < members; ++m) {
      std::uint32_t id = 0;
      if (!reader.u32(id)) return FormatError::truncated;
      // Members are a set, in the order they joined the group. `add` appends
      // and refuses a repeat, so reading the file's ids in file order restores
      // both -- and a file that repeats one is refused rather than quietly
      // producing a group one member short of the world it was written from.
      //
      // This used to also require the ids to arrive strictly ascending, which
      // is what the table held until `GroupTable` was corrected to first-add
      // order. Leaving it in place refused every save of every real map: the
      // writer began emitting the true order and the reader still demanded the
      // sorted one, and the whole C++ suite stayed green because none of its
      // worlds is built by loading one.
      if (!groups.add(index, id)) return FormatError::malformed;
    }
  }

  std::uint32_t rect_count = 0;
  if (!reader.u32(rect_count)) return FormatError::truncated;
  RectTable rects;
  for (std::uint32_t i = 0; i < rect_count; ++i) {
    RectTable::Rect r;
    if (!bytes::get_i32(reader, r.left) || !bytes::get_i32(reader, r.top) ||
        !bytes::get_i32(reader, r.right) || !bytes::get_i32(reader, r.bottom)) {
      return FormatError::truncated;
    }
    // Interning in file order reproduces the indices, which is what a suspended
    // script's locals refer to. A repeat would collapse two indices into one
    // and renumber every rectangle after it, so it is refused rather than
    // silently deduplicated -- the same rule the group table applies.
    if (rects.intern(r) != i) return FormatError::malformed;
  }

  std::uint32_t name_count = 0;
  if (!reader.u32(name_count)) return FormatError::truncated;
  NamedObjectTable names;
  for (std::uint32_t i = 0; i < name_count; ++i) {
    std::string name;
    std::uint32_t id = 0;
    if (!bytes::get_string(reader, name) || !reader.u32(id)) return FormatError::truncated;
    // First-wins on a repeat is the live table's rule, so a save carrying a
    // repeat would lose the second binding and come back smaller than it went
    // out. Refused instead.
    if (!names.bind(name, id)) return FormatError::malformed;
  }

  std::uint32_t boarding_rows = 0;
  if (!reader.u32(boarding_rows)) return FormatError::truncated;
  BoardingTable boarding;
  ObjectId previous_ship = kNoObject;
  for (std::uint32_t i = 0; i < boarding_rows; ++i) {
    std::uint32_t ship = 0;
    std::uint32_t members = 0;
    if (!reader.u32(ship) || !reader.u32(members)) return FormatError::truncated;
    // Strictly ascending, and non-empty: `BoardingTable` binary-searches on the
    // first and drops a row that loses its last member, so a save that broke
    // either would come back as a table whose lookups quietly disagree with the
    // one that was written.
    if (i != 0 && !(previous_ship < ship)) return FormatError::malformed;
    if (members == 0) return FormatError::malformed;
    previous_ship = ship;
    std::vector<ObjectId> list;
    list.reserve(members);
    for (std::uint32_t m = 0; m < members; ++m) {
      std::uint32_t unit = 0;
      if (!reader.u32(unit)) return FormatError::truncated;
      // No duplicates: `notify` refuses one, so a repeat could only come from a
      // writer this reader does not agree with.
      if (std::find(list.begin(), list.end(), unit) != list.end()) return FormatError::malformed;
      list.push_back(unit);
    }
    boarding.assign(ship, list);
  }

  std::uint32_t system_count = 0;
  if (!reader.u32(system_count)) return FormatError::truncated;
  if (system_count != systems_.size()) return FormatError::unsupported;
  for (std::uint32_t i = 0; i < system_count; ++i) {
    std::string name;
    if (!bytes::get_string(reader, name)) return FormatError::truncated;
    const System* system = systems_[i];
    if (system == nullptr || system->name() != name) return FormatError::unsupported;
  }

  std::uint32_t pool_bytes = 0;
  std::span<const std::byte> pool_payload;
  if (!reader.u32(pool_bytes) || !reader.bytes(pool_bytes, pool_payload)) {
    return FormatError::truncated;
  }
  ObjListPool objlists;
  if (const Status status = objlists.deserialize(pool_payload); !status.ok()) {
    return status.error();
  }

  std::uint32_t array_bytes = 0;
  std::span<const std::byte> array_payload;
  if (!reader.u32(array_bytes) || !reader.bytes(array_bytes, array_payload)) {
    return FormatError::truncated;
  }
  ArrayPool arrays;
  if (const Status status = arrays.deserialize(array_payload); !status.ok()) {
    return status.error();
  }

  std::uint32_t squadlist_bytes = 0;
  std::span<const std::byte> squadlist_payload;
  if (!reader.u32(squadlist_bytes) || !reader.bytes(squadlist_bytes, squadlist_payload)) {
    return FormatError::truncated;
  }
  SquadListPool squadlists;
  if (const Status status = squadlists.deserialize(squadlist_payload); !status.ok()) {
    return status.error();
  }

  std::uint32_t conversation_bytes = 0;
  std::span<const std::byte> conversation_payload;
  if (!reader.u32(conversation_bytes) ||
      !reader.bytes(conversation_bytes, conversation_payload)) {
    return FormatError::truncated;
  }
  ConversationPool conversations;
  if (const Status status = conversations.deserialize(conversation_payload); !status.ok()) {
    return status.error();
  }

  // Trailing bytes mean the writer and this reader disagree about the layout,
  // which is the one thing a version number is supposed to have caught.
  if (reader.remaining() != 0) return FormatError::malformed;

  Clock restored;
  if (!restore_clock(restored, config, time, turn_index, turn_length)) {
    return FormatError::malformed;
  }

  // Everything decoded and checked. Only now is the world touched, so a save
  // that failed any check above leaves it exactly as it was.
  clock_ = restored;
  rng_.seed(seed);
  next_command_id_ = next_command_id;
  next_id_ = next_id;
  objects_ = std::move(objects);
  rebuild_index();  // derived, so never in the save: rebuilt from what was
  gate_lines_.forget();  // derived too: learnt again from the gates restored
  queries_ = std::move(queries);
  players_ = std::move(table);
  groups_ = std::move(groups);
  rects_ = std::move(rects);
  named_objects_ = std::move(names);
  boarding_ = std::move(boarding);
  // `objlists_` comes back too, dead slots and all. It is not hashed
  // (`scriptstate` is zero in all nine dumps) so nothing downstream would have
  // noticed its absence -- which is exactly why it is restored here rather than
  // trusted to the hash check. The entries only: the alias resolver on the
  // live pool is the economy's seam, not saved state, and a move-assignment
  // here took it away on every load. See `ObjListPool::adopt_entries`.
  objlists_.adopt_entries(std::move(objlists));
  arrays_ = std::move(arrays);
  squadlists_ = std::move(squadlists);
  conversations_ = std::move(conversations);
  return Status();
}

}  // namespace imperivm::core::sim
