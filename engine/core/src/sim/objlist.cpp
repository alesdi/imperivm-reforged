// `ObjList`, the collection type the scripts are written around.
// See include/imperivm/core/sim/objlist.hpp.
//
// This file holds the pool. The host entry points that read and write it --
// `count`, `Add`, `ClearDead`, `GetObjList` and the rest -- are defined by
// `register_objlist_host`, which is declared in the header and lives here too.

#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/squad.hpp"

#include "imperivm/core/sim/array.hpp"

#include <algorithm>
#include <optional>
#include <vector>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/globals.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/save.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {

/// One pooled list.
///
/// `script` and `slot` are the declaration site. A temporary -- a list a host
/// function returned rather than one a script declared -- carries
/// `kTemporarySlot`, which no local index can equal, so it never collides with
/// a declaration and is released with its script all the same.
struct ObjListPool::Entry {
  script::ScriptId script = script::kNoScript;
  std::uint32_t slot = 0;
  bool live = false;
  /// The object whose collection this entry *is*, or `kNoObject` when it owns
  /// `items`. See `ObjListPool::acquire_alias`.
  ObjectId alias = kNoObject;
  std::vector<ObjectId> items;
};

namespace {

constexpr std::uint32_t kTemporarySlot = 0xFFFFFFFFu;

/// Handles are one-based so that `kNoObjList` stays distinguishable from the
/// first real list.
[[nodiscard]] constexpr ObjListId id_of_index(std::size_t index) noexcept {
  return static_cast<ObjListId>(index + 1);
}

[[nodiscard]] constexpr std::size_t index_of_id(ObjListId id) noexcept {
  return static_cast<std::size_t>(id) - 1;
}

}  // namespace

script::Value make_objlist_value(ObjListId id) noexcept {
  return script::Value::object(kTypeObjList, id);
}

bool is_objlist(const script::Value& value) noexcept {
  return value.is_object() && value.as_object().type == kTypeObjList;
}

ObjListId objlist_of(const script::Value& value) noexcept {
  return is_objlist(value) ? value.as_object().id : kNoObjList;
}

ObjListPool::ObjListPool() = default;
ObjListPool::~ObjListPool() = default;
ObjListPool::ObjListPool(ObjListPool&&) noexcept = default;
ObjListPool& ObjListPool::operator=(ObjListPool&&) noexcept = default;

ObjListId ObjListPool::acquire(script::ScriptId script, std::uint32_t slot) {
  // Keyed by declaration site, not by execution: `TS_CARTHAGETACTIC.VS`
  // declares `ObjList ol;` inside a loop body, and a slot per execution would
  // grow without bound for as long as that tactic script runs. Re-entering the
  // scope clears the list, which is what the declaration means.
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    Entry& entry = entries_[i];
    if (entry.live && entry.script == script && entry.slot == slot) {
      entry.items.clear();
      return id_of_index(i);
    }
  }
  return id_of_index(emplace(script, slot));
}

ObjListId ObjListPool::acquire_temporary(script::ScriptId script) {
  return id_of_index(emplace(script, kTemporarySlot));
}

ObjListId ObjListPool::acquire_alias(script::ScriptId script, ObjectId owner) {
  // A temporary in every respect except where its storage is: the handle is
  // the script's and dies with it, the list is somebody else's and does not.
  const std::size_t index = emplace(script, kTemporarySlot);
  entries_[index].alias = owner;
  return id_of_index(index);
}

std::size_t ObjListPool::emplace(script::ScriptId script, std::uint32_t slot) {
  // A dead slot is reused before the vector grows, and the scan runs in index
  // order, so the same sequence of acquisitions always yields the same handles.
  // That matters: a handle can sit in a suspended script's local slot, and
  // suspended scripts are world state.
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    if (!entries_[i].live) {
      entries_[i].script = script;
      entries_[i].slot = slot;
      entries_[i].live = true;
      entries_[i].alias = kNoObject;
      entries_[i].items.clear();
      return i;
    }
  }
  entries_.push_back(Entry{script, slot, true, kNoObject, {}});
  return entries_.size() - 1;
}

void ObjListPool::release_script(script::ScriptId script) {
  for (Entry& entry : entries_) {
    if (entry.script != script) continue;
    entry.live = false;
    entry.script = script::kNoScript;
    // Freed rather than kept for reuse: an AI script can hold thousands of
    // object ids and there is no reason for a dead slot to keep that memory.
    entry.items.clear();
    entry.items.shrink_to_fit();
  }
}

bool ObjListPool::contains(ObjListId id) const noexcept {
  if (id == kNoObjList) return false;
  const std::size_t index = index_of_id(id);
  return index < entries_.size() && entries_[index].live;
}

std::span<const ObjectId> ObjListPool::items(ObjListId id) const noexcept {
  if (!contains(id)) return {};
  const Entry& entry = entries_[index_of_id(id)];
  if (entry.alias == kNoObject) return entry.items;
  // Resolved on every read, never cached: the collection an alias names lives
  // in a vector somebody else may have grown since. A resolver that answers
  // null -- no resolver installed, or an owner that has gone -- reads as an
  // empty list, which is what a stale handle already reads as here.
  const std::vector<ObjectId>* items = resolve_alias(entry.alias);
  return items == nullptr ? std::span<const ObjectId>{} : std::span<const ObjectId>(*items);
}

std::vector<ObjectId>* ObjListPool::resolve_alias(ObjectId owner) const noexcept {
  // Two owner kinds, and an object is at most one of them -- a settlement's
  // holder record or a hero -- so asking each in turn cannot pick the wrong
  // collection.
  if (alias_resolver_ != nullptr) {
    if (std::vector<ObjectId>* items = alias_resolver_(alias_user_, owner)) return items;
  }
  if (army_resolver_ != nullptr) return army_resolver_(army_user_, owner);
  return nullptr;
}

std::vector<ObjectId>* ObjListPool::mutable_items(ObjListId id) noexcept {
  if (!contains(id)) return nullptr;
  Entry& entry = entries_[index_of_id(id)];
  if (entry.alias == kNoObject) return &entry.items;
  return resolve_alias(entry.alias);
}

void ObjListPool::set_alias_resolver(AliasResolver resolver, void* user) noexcept {
  alias_resolver_ = resolver;
  alias_user_ = user;
}

void ObjListPool::set_army_resolver(AliasResolver resolver, void* user) noexcept {
  army_resolver_ = resolver;
  army_user_ = user;
}

ObjectId ObjListPool::alias_of(ObjListId id) const noexcept {
  if (!contains(id)) return kNoObject;
  return entries_[index_of_id(id)].alias;
}

const std::vector<ObjectId>* ObjListPool::roster_of(ObjectId owner) const noexcept {
  if (owner == kNoObject || alias_resolver_ == nullptr) return nullptr;
  return alias_resolver_(alias_user_, owner);
}

script::ScriptId ObjListPool::owner_of(ObjListId id) const noexcept {
  if (!contains(id)) return script::kNoScript;
  return entries_[index_of_id(id)].script;
}

std::size_t ObjListPool::size() const noexcept {
  std::size_t live = 0;
  for (const Entry& entry : entries_) live += entry.live ? 1 : 0;
  return live;
}

std::size_t ObjListPool::capacity() const noexcept { return entries_.size(); }

std::size_t ObjListPool::release_unmarked(std::span<const char> keep) {
  std::size_t freed = 0;
  for (std::size_t i = 0; i < entries_.size(); ++i) {
    Entry& entry = entries_[i];
    if (!entry.live) continue;
    // An entry past the end of `keep` was minted after the mark phase read the
    // pool, so nothing had the chance to mark it. Treated as reachable: losing
    // a list nobody could see is the one mistake a collector must not make.
    if (i >= keep.size() || keep[i] != 0) continue;
    entry.live = false;
    entry.script = script::kNoScript;
    entry.items.clear();
    entry.items.shrink_to_fit();
    ++freed;
  }
  return freed;
}

// --------------------------------------------------------------------------
// the saved game
// --------------------------------------------------------------------------
//
// See the declarations in `sim/objlist.hpp` for why this is a serialise pair
// rather than a wider public API, and docs/formats/save.md for the layout.

namespace {

constexpr std::uint32_t kPoolMagic = 0x4C424F49u;  // "IOBL"
constexpr std::uint32_t kPoolVersion = 2;  // 2: an entry can alias somebody else's list

}  // namespace

void ObjListPool::serialize(std::vector<std::byte>& out) const {
  bytes::put_u32(out, kPoolMagic);
  bytes::put_u32(out, kPoolVersion);
  // Every slot, in index order, dead ones included: the index *is* the handle
  // minus one, and where the holes are decides what the next acquire returns.
  bytes::put_u32(out, static_cast<std::uint32_t>(entries_.size()));
  for (const Entry& entry : entries_) {
    bytes::put_u32(out, entry.script);
    bytes::put_u32(out, entry.slot);
    bytes::put_u8(out, entry.live ? 1u : 0u);
    // The object an alias names, or `kNoObject`. Written before the items so
    // that an alias -- whose own `items` is always empty -- still round-trips
    // through the same shape as everything else.
    bytes::put_u32(out, entry.alias);
    // A dead entry's items are cleared by `release_script` and
    // `release_unmarked`, so this is zero for every hole. Written anyway rather
    // than made conditional: a length that is only present sometimes is a
    // second thing the reader has to get right.
    bytes::put_u32(out, static_cast<std::uint32_t>(entry.items.size()));
    for (const ObjectId id : entry.items) bytes::put_u32(out, id);
  }
}

Status ObjListPool::deserialize(std::span<const std::byte> data) {
  ByteReader reader(data);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t count = 0;
  if (!reader.u32(magic) || !reader.u32(version)) return FormatError::truncated;
  if (magic != kPoolMagic) return FormatError::bad_magic;
  if (version != kPoolVersion) return FormatError::unsupported;
  if (!reader.u32(count)) return FormatError::truncated;

  std::vector<Entry> entries;
  for (std::uint32_t i = 0; i < count; ++i) {
    // Nothing is reserved from `count` or from an item count: a hostile length
    // would otherwise ask for gigabytes before the read that refuses it ran.
    // Same reasoning `World::deserialize` records for the object table.
    Entry entry;
    std::uint8_t live = 0;
    std::uint32_t items = 0;
    if (!reader.u32(entry.script) || !reader.u32(entry.slot) || !reader.u8(live) ||
        !reader.u32(entry.alias) || !reader.u32(items)) {
      return FormatError::truncated;
    }
    entry.live = live != 0;
    // An alias owns nothing, so a saved one with contents is a save this pool
    // could not have written -- the same reasoning as the dead-entry check
    // below, one line further along.
    if (entry.alias != kNoObject && items != 0) return FormatError::malformed;
    // A dead entry with contents could not have come from this pool -- both
    // release paths clear the vector -- and would make `size()` and the sweep
    // disagree about what is holding memory.
    if (!entry.live && items != 0) return FormatError::malformed;
    for (std::uint32_t m = 0; m < items; ++m) {
      ObjectId id = 0;
      if (!reader.u32(id)) return FormatError::truncated;
      entry.items.push_back(id);
    }
    entries.push_back(std::move(entry));
  }
  if (reader.remaining() != 0) return FormatError::malformed;

  entries_ = std::move(entries);
  return Status();
}

void ObjListPool::adopt_entries(ObjListPool&& other) noexcept {
  entries_ = std::move(other.entries_);
}

ObjListPool& objlist_pool_of(World& world) { return world.objlists(); }

// --------------------------------------------------------------------------
// the host entry points
// --------------------------------------------------------------------------

namespace {

using script::CallContext;
using script::HostOutcome;
using script::ObjectRef;
using script::Value;

constexpr script::CallKind kFree = script::CallKind::free_function;
constexpr script::CallKind kMember = script::CallKind::member;

[[nodiscard]] Value invalid_object() { return Value::object(ObjectRef{script::kNoType, 0}); }

/// An object handle as a live object id, or `kNoObject`.
///
/// Accepts every handle type the object model mints -- `Obj`, `Query`,
/// `Settlement` -- because the language distinguishes them only by explicit
/// downcast and `ol.Add(set.GetCentralBuilding)` is as ordinary as
/// `ol.Add(hero)`. A point is *not* an object and is rejected here, which is
/// what keeps `ol.Add(pt)` from silently entering `(x << 16) | y` as an id.
/// Whether a value is a `NamedObj` handle -- a `<group type="0">` name.
[[nodiscard]] bool is_named_obj(const Value& value) noexcept {
  return value.is_object() && value.as_object().type == kTypeNamedObj;
}

/// A `NamedObj` read as the query it upcasts to: its object, or nothing.
[[nodiscard]] std::vector<ObjectId> named_obj_members(World& world, const Value& value) {
  return receiver_objects(world, value);
}

[[nodiscard]] ObjectId live_object_of(World& world, const Value& value) noexcept {
  if (!value.is_object()) return kNoObject;
  const ObjectRef ref = value.as_object();
  if (ref.type != kTypeObj && ref.type != kTypeQuery && ref.type != kTypeSettlement) {
    return kNoObject;
  }
  return world.find(ref.id) != nullptr ? ref.id : kNoObject;
}

/// The items of the list a *mutating* member was called on, or null.
///
/// Null has two causes, and the caller tells them apart because they mean
/// different things:
///
///   * the receiver is not an `ObjList` at all. `Clear/0` and `Add/1` are
///     shared entry points -- `bldEnter.Clear`, `teleport.Clear` and
///     `slTrain.Add` all ship, on `Building`, `Obj` and `SquadList` -- so
///     answering would hide another domain's gap. Refuse by name.
///   * the receiver is an `ObjList` whose pool entry is gone. A declaration
///     binds its entry at `default_value` now, so the only way here is a handle
///     that outlived the script owning it; see the header's note on that.
[[nodiscard]] std::vector<ObjectId>* mutable_receiver(const CallContext& ctx,
                                                      ObjListPool& pool) {
  if (ctx.count() == 0 || !is_objlist(ctx.arg(0))) return nullptr;
  return pool.mutable_items(objlist_of(ctx.arg(0)));
}

// -- members ---------------------------------------------------------------

/// `ol.count` and `q.count` -- 541 sites, the second most used member in the
/// whole host API, and the reason this domain exists.
///
/// One entry point for both receivers because the registry keys on (kind, name,
/// arity) and there is exactly one `count/0`. `sel.Count()` in
/// `DATA\SUBAI\DEBUG_DUMP.VS` is the same entry: member lookup is
/// case-insensitive and parentheses are optional.
///
/// **The bodies.** The original's `ObjList` is a deque of sixteen-bit object
/// handles with its size at `+0x18`, and a list value on the VM stack is a
/// handle, an offset and a cached pointer.
///
///   * `ObjList::count` (0x0055e0c0) resolves the list and answers that size
///     word -- members are counted, not resolved, so a dead unit's handle and
///     the invalid handle both count until something removes them. It then
///     drops the reference the call held (`[+0x1c]`), deleting a temporary
///     whose count reaches zero. It does not test the receiver: a list that
///     does not resolve is a null dereference there and a zero here.
///   * `Query::count` (0x00577b90) resolves the query, purges its cached list
///     (`vtbl+0x1c`, 0x00572c10: the gone and the dead leave it), asks its
///     list accessor (`vtbl+0x20`, 0x004f9490, which refreshes the list only
///     when its due time has come) and answers that list's size -- so a
///     `count` a second after the last refresh of a sight query answers the
///     list that refresh made, less its dead. An unresolvable receiver prints
///     `The function 'Query::count' called for an uninitialized or invalid
///     object.` and answers **0** (0x00577be4).
///   * `ItemHolder::count` (0x0053c660) is a third registration of the name: an
///     item holder's virtual at `vtbl+0x104`, 0 with the same diagnostic for an
///     invalid receiver. No shipped script calls `count` on an `ItemHolder`, and
///     a live object that is neither a list nor a query is refused below rather
///     than guessed at.
HostOutcome m_count(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("count: no world");
  if (is_objlist(ctx.arg(0))) {
    // A stale handle reads as zero rather than trapping. A declared list is
    // bound and answers for itself; this is the handle that outlived its list,
    // and `for (i = 0; i < ol.count; ...)` over it has to terminate rather than
    // kill the script. The pool's rule that an unknown id is an empty list is
    // what makes that fall out.
    const std::span<const ObjectId> items =
        objlist_pool_of(*world).items(objlist_of(ctx.arg(0)));
    return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(items.size())));
  }
  // A `NamedObj` is a one-member query, and gets here because `gbr.exe`
  // registers `NamedObj` as a **subtype** of `Query` (`0x005b6d96`) rather
  // than registering `count` on it -- so the compiler upcasts at zero cost and
  // the call boundary never inspects a type word. `sim/wait.cpp` already
  // relies on the same edge. A dead binding counts 0: the original's
  // `CVXNamedObjQuery` goes *empty* rather than invalid.
  if (is_named_obj(ctx.arg(0))) {
    return HostOutcome::ok_with(Value::integer(named_obj_members(*world, ctx.arg(0)).empty() ? 0 : 1));
  }
  const ObjectId id = live_object_of(*world, ctx.arg(0));
  const WorldObject* slot = id == kNoObject ? nullptr : world->find(id);
  // A handle naming nothing is `The function 'Query::count' called for an
  // uninitialized or invalid object.` (0x007c5978) and a **0**, not a trap:
  // 0x00577be4 prints and pushes zero. `LION_LEAD.VS` reaches it through
  // `Intersect(...).count` when either operand of the intersection was itself
  // invalid -- `Intersect` prints and answers the invalid handle for that --
  // and the lion's walk must go on. A *live* object that is not a query is a
  // type error the original cannot even express, and stays a refusal.
  if (slot == nullptr) return HostOutcome::ok_with(Value::integer(0));
  if (slot->internal != InternalKind::query) {
    return HostOutcome::failed("count: receiver is neither an ObjList nor a Query");
  }
  // The purge and the list accessor (0x00577b90): the cached list, refreshed
  // only when due, less what has died since.
  std::vector<ObjectId> found;
  return HostOutcome::ok_with(Value::integer(
      static_cast<std::int32_t>(world->read_query(id, script_clock(ctx, *world), found))));
}

/// `q.GetObjList()` -- 103 sites, and the only way a script turns a live query
/// into something it can index.
///
/// Every one of the 103 receivers in the corpus is a `Query`: `qryDef`,
/// `qEnemies`, `Alert`, `Group(...)`, `Subtract(qAll, qEnemy)` and so on. There
/// is no `ObjList` receiver, which would be a copy, and no other type.
///
/// The list is a snapshot. That is the whole distinction between the two types
/// -- a query re-evaluates and an `ObjList` needs `ClearDead` -- and it is why
/// `DATA\AI HELPERS\GUARD.VS` re-reads `ol = qryDef.GetObjList();` at the top of
/// every pass of its `while (1)` loop.
///
/// 0x0057a7c0: resolve the query, purge it (`vtbl+0x1c`) and read its list
/// (`vtbl+0x20`, refreshed when due), allocate a fresh list (0x0041edd0) and
/// copy the query's cached deque into it whole and in order (0x00438230 over
/// the accessor's `+8`), then push it as a new value with no owning handle. No
/// member is tested, so the copy is exactly the query's own order --
/// `World::read_query`'s, which is the query system's to answer for.
HostOutcome m_get_objlist(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetObjList: no world");
  std::vector<ObjectId> found;
  if (is_named_obj(ctx.arg(0))) {
    // The same one-member-query upcast `m_count` takes; 8 shipped sites.
    found = named_obj_members(*world, ctx.arg(0));
  } else {
    // An ObjList is not a copy source: all 103 corpus receivers are queries.
    if (is_objlist(ctx.arg(0))) return HostOutcome::failed("GetObjList: receiver is not a Query");
    const ObjectId id = live_object_of(*world, ctx.arg(0));
    const WorldObject* slot = id == kNoObject ? nullptr : world->find(id);
    // A handle naming nothing is "The function 'Query::GetObjList' called for
    // an uninitialized or invalid object." (0x007c66d8) and a fresh empty
    // list, not a trap: 0x0057a81d prints, allocates a 0x2c-byte ObjList and
    // pushes it as type 0x17 -- the shape `Query::count` takes at 0x00577be4
    // with its 0. `LION_LEAD.VS:86` reaches it once the lion is dead:
    // `.player` is -1, `FriendlyObjs(-1, ...)` is invalid, `Intersect`
    // propagates it, and the walk goes on with an empty `ol`. A live object
    // that is not a query is still a refusal, as for `count`.
    if (slot != nullptr && slot->internal != InternalKind::query) {
      return HostOutcome::failed("GetObjList: receiver is not a Query");
    }
    if (slot != nullptr) world->read_query(id, script_clock(ctx, *world), found);
  }
  ObjListPool& pool = objlist_pool_of(*world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  std::vector<ObjectId>* items = pool.mutable_items(list);
  if (items == nullptr) return HostOutcome::failed("GetObjList: the pool refused a new list");
  *items = std::move(found);
  return HostOutcome::ok_with(make_objlist_value(list));
}

/// `ship.GetUnitsOnBoard()` -- 22 sites, 21 of them inside map containers, and
/// **a copy where `Settlement::Units` is an alias**.
///
/// `0x005c89f0` reads the ship's holder handle at `[ship+0x1dc]`, allocates a
/// fresh 0x2c-byte list and **copy-constructs** from the holder's deque at
/// `[holder+0x28]` through 0x00438330 -- the same plain forward range copy
/// `Squad::Units` makes, and not the refcount-and-push `Settlement::Units`
/// makes. So a script may mutate what it gets back and the ship's manifest is
/// unharmed, which is what the shipped sites do: `5_Great_Battles_Britain`
/// map 3 takes the list, `ClearDead`s it and counts what is left.
///
/// **The holder is the object at `ship + 1`.** `World::spawn_ship` mints the
/// two together in that order, which is the invariant the dumps show from the
/// other side -- `sim/world.hpp` records that the three orphan `CVXHolder`s in
/// the corpus are exactly these, each at handle+1 of a `CVXShip`. Membership is
/// then the same back-link every other holder question in this engine uses,
/// `ObjectState::holder`, rather than a list on the holder: `Holder::units`
/// belongs to `Settlement` and a ship's holder is not a settlement's.
///
/// The result is in ascending object id, which is `World::objects()`' order and
/// therefore board order for units that boarded in the order they were spawned.
/// The original's is holder order, and the two differ only for a ship that has
/// unloaded and reloaded; recorded rather than reproduced, because this engine
/// has no per-holder list to keep an order in.
///
/// A receiver that is not a ship, or one whose neighbour is not a holder,
/// answers an **empty list** rather than refusing -- the original dereferences
/// an unresolved handle there and crashes, which is not a behaviour worth
/// reproducing.
HostOutcome m_get_units_on_board(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetUnitsOnBoard: no world");
  ObjListPool& pool = objlist_pool_of(*world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  std::vector<ObjectId>* out = pool.mutable_items(list);
  if (out == nullptr) return HostOutcome::failed("GetUnitsOnBoard: the pool refused a new list");

  const ObjectId ship = live_object_of(*world, ctx.arg(0));
  const WorldObject* holder = ship == kNoObject ? nullptr : world->find(ship + 1);
  if (holder != nullptr && holder->internal == InternalKind::holder) {
    for (const WorldObject& slot : world->objects()) {
      if (slot.state.holder == holder->id) out->push_back(slot.id);
    }
  }
  return HostOutcome::ok_with(make_objlist_value(list));
}

/// `u.GetUnitsInSameHolder()` -- 4 sites, and it is the sole blocker of four
/// scripts.
///
/// The receiver's holder, whole. `gbr.exe` 0x005df620 reads the unit's
/// `holder` field (`+0x154`, the u16 the serialiser names), resolves it, and
/// **assigns the holder's member vector into the new list** with one call to
/// 0x00438230 -- no loop, no predicate, no owner test and no self-exclusion. So
/// the receiver is in its own answer, and so is every enemy standing in the
/// same building.
///
/// **A unit in no holder gets an empty list rather than a refusal.** The
/// original indexes the handle table with `0xffff` without checking for it
/// first and relies on the slot being null; the diagnostic beside it
/// ("called for an uninitialized or invalid object") is for a receiver that
/// does not resolve at all, and it also yields an empty list.
///
/// **The order is the holder's.** 0x00438230 copies the holder's deque at
/// `+0x30` front to back, and for a settlement's holder that deque is the very
/// one `Settlement::Units` aliases (0x005c3c70). So where this engine keeps that
/// roster -- `Holder::units`, reached through `ObjListPool::roster_of` -- the
/// answer is a copy of it, in garrison order: two of the four shipped sites
/// pick a member with `ol[rand(ol.count)]`, so the order is the answer. Any
/// other holder has no roster here, and membership falls back to the
/// `ObjectState::holder` back-link in ascending object id, for the reason
/// `GetUnitsOnBoard` above records; that fallback differs from the original
/// only for such a holder that has been emptied and refilled.
HostOutcome m_get_units_in_same_holder(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetUnitsInSameHolder: no world");
  ObjListPool& pool = objlist_pool_of(*world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  std::vector<ObjectId>* out = pool.mutable_items(list);
  if (out == nullptr) {
    return HostOutcome::failed("GetUnitsInSameHolder: the pool refused a new list");
  }
  const ObjectId self = live_object_of(*world, ctx.arg(0));
  const WorldObject* slot = self == kNoObject ? nullptr : world->find(self);
  if (slot == nullptr) return HostOutcome::ok_with(make_objlist_value(list));
  if (const std::vector<ObjectId>* roster = pool.roster_of(slot->state.holder);
      roster != nullptr) {
    // Copied rather than assigned from a reference held across the call: the
    // roster lives in a settlement store that may move, and `out` is the pool's.
    const std::vector<ObjectId> copy = *roster;
    if (std::vector<ObjectId>* again = pool.mutable_items(list); again != nullptr) *again = copy;
    return HostOutcome::ok_with(make_objlist_value(list));
  }
  (void)world->contents_of(slot->state.holder, *out);
  return HostOutcome::ok_with(make_objlist_value(list));
}

/// `hero.army` -- 92 sites, and the hero domain's one collection-valued member.
///
/// **Here and not in `sim/hero.cpp`**, which is what that header's note asks
/// for: it declined to define this because "the collection types belong to the
/// script host, not to this system", and this file is that host. `HeroSystem`
/// still owns the membership; all this does is copy it into a pooled list.
///
/// `gbr.exe` (0x00530580) returns a **live reference** rather than a snapshot:
/// the value it pushes is `{hero handle, 0x1cc, hero + 0x1cc}`, the hero's own
/// army vector with its refcount bumped, so a mutation through the handle
/// reaches the hero, and a list held across a recruitment sees the recruit.
/// This is the alias `Settlement::Units` already is (`acquire_alias`), with the
/// hero as the owner: `HeroSystem::start` installs the pool's army resolver,
/// which answers `HeroRecord::army` on every access.
///
/// Of the corpus's uses -- `SetCommand` (18), bare `.army` in a count or a
/// condition (26), `count` (15), `AddToGroup`/`RemoveFromGroup`/
/// `RemoveFromAllGroups` (6), `KillCommand`, `AddCommand` -- the one that writes
/// is `.army.ClearDead` (4 sites), which now takes the dead out of the hero's
/// army at once rather than at `HeroSystem::advance`'s next prune.
///
/// An invalid receiver is **not** a refusal: 0x005305ad diagnoses and then
/// allocates a fresh empty list, so `h.army.count` on a dead hero is 0.
///
HostOutcome m_army(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("army: no world");
  ObjListPool& pool = objlist_pool_of(*world);
  HeroSystem* heroes = hero_system_of(*world);
  const ObjectId id = live_object_of(*world, ctx.arg(0));
  const HeroRecord* record = heroes == nullptr || id == kNoObject ? nullptr : heroes->hero(id);
  // The live list: an alias the hero owns, resolved on every access.
  if (record != nullptr) return HostOutcome::ok_with(make_objlist_value(pool.acquire_alias(ctx.script, id)));
  const ObjListId list = pool.acquire_temporary(ctx.script);
  if (pool.mutable_items(list) == nullptr) {
    return HostOutcome::failed("army: the pool refused a new list");
  }
  return HostOutcome::ok_with(make_objlist_value(list));
}

/// `ol.ClearDead()` -- 173 sites in 39 files, the second most used member of
/// this type.
///
/// Drops every id that `IsDead` would answer true for, which is an object the
/// world no longer holds *or* one whose health has reached zero
/// (`sim/world_host.cpp`'s `m_is_dead`). Reading it any narrower would leave a
/// list on which `ClearDead` had just run still containing dead objects, and
/// every call site immediately commands what survives:
/// `olArchers.ClearDead(); SetNoAIFlag(olArchers, false);`.
///
/// Order is preserved among the survivors -- there is nothing in the corpus
/// that would justify disturbing it, and iteration order is state.
///
/// 0x0055f430 walks the deque front to back and erases a member (0x0041ecc0,
/// continuing at the element that slid into its place) when its handle no
/// longer resolves, or when the object's virtual at `vtbl+0x50` answers
/// nonzero -- the slot `Obj::IsDead` (0x005aae50) asks and `Obj::IsAlive`
/// negates, which this engine reads as health at or below zero everywhere it
/// meets it. So this is exactly "drop what `IsDead` would call dead", and the
/// invalid handle -- which resolves to nothing -- goes with them. Erasing in
/// place keeps the survivors' order.
HostOutcome m_clear_dead(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ClearDead: no world");
  if (!is_objlist(ctx.arg(0))) {
    return HostOutcome::failed("ClearDead: receiver is not an ObjList");
  }
  std::vector<ObjectId>* items = objlist_pool_of(*world).mutable_items(objlist_of(ctx.arg(0)));
  // A stale list has nothing to clear, and trapping would punish a script for
  // holding a handle whose owner ended first.
  if (items == nullptr) return HostOutcome::ok_void();
  World& w = *world;
  items->erase(std::remove_if(items->begin(), items->end(),
                              [&w](ObjectId id) {
                                const WorldObject* slot = w.find(id);
                                return slot == nullptr || slot->state.health <= 0;
                              }),
               items->end());
  return HostOutcome::ok_void();
}

/// An object argument as the handle a list member would hold, or nothing.
///
/// What the original's list stores is the argument's sixteen-bit handle word,
/// taken as it stands: no member-adding body tests that it resolves. So a dead
/// object's id is kept as it is, and the invalid handle -- the `kNoType`
/// sentinel `Obj::Clear` and every failed lookup answer -- is `kNoObject`,
/// which is what `0xffff` is to this engine. Anything that is not an object
/// handle (a point, an integer) is not a member at all.
[[nodiscard]] std::optional<ObjectId> member_handle(const Value& value) noexcept {
  if (!value.is_object()) return std::nullopt;
  const ObjectRef ref = value.as_object();
  if (ref.type == script::kNoType) return kNoObject;
  if (ref.type != kTypeObj && ref.type != kTypeQuery && ref.type != kTypeSettlement) {
    return std::nullopt;
  }
  return ref.id;
}

/// `ol.Add(o)` -- 83 sites. **Appends `o` unless the list already holds it.**
///
/// 0x005601a0 pops the argument's handle, resolves the receiver, searches the
/// whole deque for an equal handle (0x0050e2a0, a front-to-back scan comparing
/// the sixteen-bit word) and pushes it at the back (0x0041f290) only when the
/// scan reaches the end; then it drops the call's reference to the list. There
/// is **no validity test on the argument**: the invalid handle and a dead
/// unit's handle are added like any other, `count` counts them, and
/// `ClearDead` is what removes them. `TS_CARTHAGETACTIC.VS` guards its
/// `ol.Add(hero)` with `hero.IsValid()` for exactly that reason. The header's
/// old guess that `Add` might not deduplicate is settled: it does.
HostOutcome m_add(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Add: no world");

  // **`SquadList::Add` is the same name on another type**, and this is where
  // its branch has to live: member lookup is case-insensitive and the registry
  // keys on `(kind, name, arity)`, so a second `define` in `sim/squad.cpp`
  // would have replaced this body rather than joined it. 27 of the 29 shipped
  // receivers are `ObjList`s and two -- `slTrain.Add`, in `DATA\AI` -- are
  // `SquadList`s.
  //
  // 0x004379b0: the receiver is the list *variable*; when it holds no live
  // list (0x0043ed70) a fresh slot is claimed (0x0043ed10) and stored into it,
  // and then the squad's handle is appended at the back (0x00437870 ->
  // 0x00435d90) -- **with no search for a duplicate** and no test that the
  // squad exists. Both shipped sites add the squad their own cursor walk is
  // standing on, so a duplicate never arises there; the rule is the binary's
  // all the same. A value that is not a squad handle at all (the invalid
  // handle `pack_squad` makes of a missing squad) carries no key here and adds
  // nothing.
  if (ctx.count() > 0 && is_squadlist(ctx.arg(0))) {
    std::vector<SquadKey>* squads =
        squadlist_pool_of(*world).mutable_items(squadlist_of(ctx.arg(0)));
    if (squads == nullptr) return HostOutcome::failed("Add: receiver is not a live SquadList");
    if (ctx.count() < 2 || !is_squad(ctx.arg(1))) return HostOutcome::ok_void();
    const SquadKey key = unpack_squad(ctx.arg(1));
    if (!key.valid()) return HostOutcome::ok_void();
    squads->push_back(key);
    return HostOutcome::ok_void();
  }

  std::vector<ObjectId>* items = mutable_receiver(ctx, objlist_pool_of(*world));
  if (items == nullptr) return HostOutcome::failed("Add: receiver is not a live ObjList");
  const std::optional<ObjectId> id = member_handle(ctx.arg(1));
  if (!id.has_value()) return HostOutcome::ok_void();
  if (std::find(items->begin(), items->end(), *id) == items->end()) items->push_back(*id);
  return HostOutcome::ok_void();
}

/// `ol.AddList(other)` -- 67 sites. **The union, in order**: each of `other`'s
/// members the receiver does not already hold is appended, in `other`'s order.
///
/// 0x0055ff00 walks `other` front to back and, for each member, scans the
/// receiver (0x0050e2a0) and pushes the member at the back (0x0041f290) only
/// when the scan finds no equal handle. The scan runs against the receiver as
/// it grows, so a member `other` holds twice is added once; and
/// `ol.AddList(ol)` finds every member already present and changes nothing.
///
/// Every argument in the corpus is an `ObjList`: another local (`ol2`,
/// `lToHealOut`), or one a host function returned
/// (`set.TSRecruitArmy("TArcher", n)`, `.ObjectsAround("BaseMage")`,
/// `squad.Units`). A `Query` argument never occurs and is refused, because
/// accepting one would silently pick a snapshot rule the corpus never asked
/// for.
HostOutcome m_add_list(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("AddList: no world");
  if (!is_objlist(ctx.arg(1))) return HostOutcome::failed("AddList: expected an ObjList");
  ObjListPool& pool = objlist_pool_of(*world);
  std::vector<ObjectId>* items = mutable_receiver(ctx, pool);
  if (items == nullptr) return HostOutcome::failed("AddList: receiver is not a live ObjList");
  // Copied before the append: `ol.AddList(ol)` is legal in the language, and
  // reading a span of a vector while appending to it is a dangling read the
  // moment it reallocates.
  const std::span<const ObjectId> source = pool.items(objlist_of(ctx.arg(1)));
  const std::vector<ObjectId> copy(source.begin(), source.end());
  for (const ObjectId id : copy) {
    if (std::find(items->begin(), items->end(), id) == items->end()) items->push_back(id);
  }
  return HostOutcome::ok_void();
}

/// `Clear()` -- **two entry points wearing one name**, 34 sites on a list and a
/// handful on an object.
///
/// `ol.Clear()` empties the list and leaves the handle bound, because
/// `TSH_RECRUITARMY.VS` says the caller is holding the same one. 0x0055f590
/// resolves the list, empties its deque (0x006a3cc0, which frees every block)
/// and drops the call's reference.
///
/// `Obj::Clear` (`gbr.exe` 0x005ab210) makes the *handle* invalid, and it does
/// it by writing back through the argument -- which this VM supports, because
/// `CallContext::arguments` is the out-parameter mechanism. `CROW_IDLE.VS` is
/// the site that matters and it says what the entry point is for:
///
///     bird.Clear();
///     if (crows.count == 0) bird = .FindNearBird();
///     ...
///     if (bird.IsValid())
///
/// -- clear it, maybe find one, and branch on whether you did. Without this the
/// receiver was refused by name, 207 times in one corpus pass, and the comment
/// that used to stand here named the gap without closing it.
///
/// **The receiver is a reference, and the body writes through it.** 0x005ab210
/// pops an eight-byte receiver -- a handle word and an offset -- resolves the
/// handle through the object table, adds the offset and stores the invalid
/// handle (`0xffff`, then a zero offset) at that address. That is the shape of
/// every by-reference value in this VM (`Hero::army` pushes its own as the hero
/// handle and the offset `0x1cc`): the pair names the *variable*, so the store
/// lands in it. This note used to read the same instructions as a write into
/// the object itself; `CROW_IDLE.VS` running in retail is what says the
/// reference reading is the right one. A receiver that resolves to nothing is
/// a store to a null address there and a plain invalidation here.
HostOutcome m_clear(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Clear: no world");
  if (!is_objlist(ctx.arg(0))) {
    // The object form. Any handle type, because `Building`, `Obj` and `Druid`
    // all carry a `Clear` in the shipped corpus and the sentinel is the same
    // one for every one of them: `kNoType`, which is what `.IsValid` reads.
    if (ctx.arg(0).is_object()) {
      ctx.out(0) = Value::object(script::ObjectRef{script::kNoType, 0});
      return HostOutcome::ok_void();
    }
    return HostOutcome::failed("Clear: receiver is not an ObjList");
  }
  std::vector<ObjectId>* items = objlist_pool_of(*world).mutable_items(objlist_of(ctx.arg(0)));
  if (items != nullptr) items->clear();
  return HostOutcome::ok_void();
}

/// Erase the first member equal to `id`, front to back; whether one was found.
/// 0x0041ee70: the scan is 0x0050e2a0 and the erase 0x0041ecc0, one element.
bool erase_first(std::vector<ObjectId>& items, ObjectId id) {
  const auto it = std::find(items.begin(), items.end(), id);
  if (it == items.end()) return false;
  items.erase(it);
  return true;
}

/// `ol.RemoveList(other)` -- 29 sites. **One occurrence per member of `other`.**
///
/// 0x0055f100 walks `other` front to back and, for each of its members, scans
/// the receiver for an equal handle and erases the first one it finds
/// (0x0041ecc0); a member the receiver lacks is passed over silently. So a
/// handle the receiver holds twice and `other` once survives once -- which
/// `Add`'s deduplication makes rare, and which is the rule all the same. Order
/// among the survivors is kept. `ol.RemoveList(ol)` walks the list it is
/// erasing from in the original, a case no shipped script writes; here the
/// argument is copied first, so it empties the list.
HostOutcome m_remove_list(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RemoveList: no world");
  if (!is_objlist(ctx.arg(0))) {
    return HostOutcome::failed("RemoveList: receiver is not an ObjList");
  }
  if (!is_objlist(ctx.arg(1))) return HostOutcome::failed("RemoveList: expected an ObjList");
  ObjListPool& pool = objlist_pool_of(*world);
  const std::span<const ObjectId> source = pool.items(objlist_of(ctx.arg(1)));
  const std::vector<ObjectId> drop(source.begin(), source.end());
  std::vector<ObjectId>* items = pool.mutable_items(objlist_of(ctx.arg(0)));
  if (items == nullptr) return HostOutcome::ok_void();
  for (const ObjectId id : drop) (void)erase_first(*items, id);
  return HostOutcome::ok_void();
}

/// `ol.Remove(o)` -- 17 sites. **The first occurrence of one handle.**
///
/// 0x0055f370 pops the argument's handle word and hands it to 0x0041ee70, which
/// scans the deque front to back, erases the first equal member and answers
/// whether it found one; when it did not, the body prints `ObjList::Remove:
/// this element does not exist in the list!` and carries on. The argument is
/// not resolved, so removing an object that has already been despawned --
/// exactly what a script does between two `ClearDead`s -- works, and so does
/// removing the invalid handle.
///
/// Two of the 17 pass an `ObjList` rather than an object
/// (`ol.Remove(ol.ObjEnemy(AIPlayer))`, `DATA\AI\ESH_MARKET.VS:80`), and both
/// are inside a `/* */` block, so they never ran. They are accepted as
/// `RemoveList` anyway: it is the only reading under which the dead code was
/// ever meant to work, and refusing would be a trap on a shape the language
/// clearly allowed.
HostOutcome m_remove(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Remove: no world");
  if (!is_objlist(ctx.arg(0))) return HostOutcome::failed("Remove: receiver is not an ObjList");
  ObjListPool& pool = objlist_pool_of(*world);

  std::vector<ObjectId> drop;
  if (is_objlist(ctx.arg(1))) {
    const std::span<const ObjectId> source = pool.items(objlist_of(ctx.arg(1)));
    drop.assign(source.begin(), source.end());
  } else {
    const std::optional<ObjectId> id = member_handle(ctx.arg(1));
    if (!id.has_value()) return HostOutcome::failed("Remove: expected an object");
    drop.push_back(*id);
  }

  std::vector<ObjectId>* items = pool.mutable_items(objlist_of(ctx.arg(0)));
  if (items == nullptr) return HostOutcome::ok_void();
  for (const ObjectId id : drop) (void)erase_first(*items, id);
  return HostOutcome::ok_void();
}

/// `ol.Contains(o)` and `q.Contains(o)` -- 15 sites, all of them a membership
/// test whose answer decides a branch, and **two registrations**:
///
///   * `ObjList::Contains` (0x0055e490) scans the deque front to back for the
///     argument's handle word and answers whether it met it. The argument is
///     not resolved, so a dead unit's handle and the invalid handle are found
///     when the list holds them.
///   * `Query::Contains` (0x00578d20) resolves the query, purges it
///     (`vtbl+0x1c`) and asks its `vtbl+0x24`: for a cached kind 0x004f9550,
///     the front-to-back handle scan of the cached list while it is fresh, and
///     once it is due the kind's one-object slot -- a refresh and the scan
///     (0x00571870) for most, a live test of the object that stores nothing for
///     the area, sight, player-flags and party kinds. An
///     unresolvable query prints `The function 'Query::Contains' called for an
///     uninitialized or invalid object.` and answers **false**.
///
/// The query form is not hypothetical: the tutorial's first sequence asks
/// `AreaObjs("EGate1", "Hero").Contains(Caesar.obj)`, and refusing it trapped
/// the sequence. A `NamedObj` receiver is the one-member query `count` already
/// serves.
HostOutcome m_contains(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Contains: no world");
  const std::optional<ObjectId> wanted = member_handle(ctx.arg(1));
  if (is_objlist(ctx.arg(0))) {
    if (!wanted.has_value()) return HostOutcome::ok_with(Value::boolean(false));
    const std::span<const ObjectId> items = objlist_pool_of(*world).items(objlist_of(ctx.arg(0)));
    return HostOutcome::ok_with(
        Value::boolean(std::find(items.begin(), items.end(), *wanted) != items.end()));
  }
  std::vector<ObjectId> members;
  if (is_named_obj(ctx.arg(0))) {
    members = named_obj_members(*world, ctx.arg(0));
  } else {
    const ObjectId id = live_object_of(*world, ctx.arg(0));
    const WorldObject* slot = id == kNoObject ? nullptr : world->find(id);
    if (slot == nullptr) return HostOutcome::ok_with(Value::boolean(false));
    if (slot->internal != InternalKind::query) {
      return HostOutcome::failed("Contains: receiver is neither an ObjList nor a Query");
    }
    // The purge, then the query's own membership slot (`vtbl + 0x24`): the
    // cached list when fresh; when due, a refresh or -- for the area, sight,
    // player-flags and party kinds -- a live test of the one object, which
    // stores nothing (`World::query_contains`).
    world->purge_query(id);
    if (!wanted.has_value()) return HostOutcome::ok_with(Value::boolean(false));
    return HostOutcome::ok_with(
        Value::boolean(world->query_contains(id, *wanted, script_clock(ctx, *world))));
  }
  if (!wanted.has_value()) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(
      Value::boolean(std::find(members.begin(), members.end(), *wanted) != members.end()));
}

// --------------------------------------------------------------------------
// the four filters
// --------------------------------------------------------------------------
//
// `ObjEnemy(player)`, `ObjAlly(player)`, `ObjClass(name)` and `ObjInjured()`
// each mint a **new** list (0x0041edd0, the same constructor `Party` uses) and
// copy in the members that pass one test, in the receiver's order. The
// receiver is left alone. Seven sites in four scripts between them. All four
// walk the receiver front to back, skip a member whose handle does not
// resolve, push a passing member at the back (0x0041f290), drop the call's
// reference to the receiver and push the new list as an ownerless value.

/// A fresh list to fill, or `kNoObjList` with the outcome to answer.
[[nodiscard]] ObjListId fresh_list(CallContext& ctx, World& world, std::vector<ObjectId>*& items) {
  const ObjListId out = objlist_pool_of(world).acquire_temporary(ctx.script);
  items = objlist_pool_of(world).mutable_items(out);
  return out;
}

/// The members of the receiver that satisfy `keep`, as a new list.
template <typename Keep>
HostOutcome filter_list(CallContext& ctx, const char* what, Keep keep) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(what);
  if (!is_objlist(ctx.arg(0))) return HostOutcome::failed(what);
  // Copied before the mint: the pool may reallocate under a span.
  const std::span<const ObjectId> view = objlist_pool_of(*world).items(objlist_of(ctx.arg(0)));
  const std::vector<ObjectId> members(view.begin(), view.end());
  std::vector<ObjectId>* items = nullptr;
  const ObjListId out = fresh_list(ctx, *world, items);
  if (items == nullptr) return HostOutcome::failed(what);
  for (const ObjectId id : members) {
    const WorldObject* slot = world->find(id);
    if (slot != nullptr && keep(*slot)) items->push_back(id);
  }
  return HostOutcome::ok_with(make_objlist_value(out));
}

/// `ol.ObjEnemy(player)` -- 2 sites. 0x005607b0 keeps a member when **its
/// owner's** relation row has bit 0 clear for `player`: the owner considers
/// the player an enemy, the direction `PlayerTable::is_enemy(owner, player)`
/// reads. Precisely: the player record is the one at `player - 1` (guarded only
/// by `player - 1 < 16`), its index word is `[record+8]`, and the member passes
/// when bit 0 of `[owner_record + 0x24 + index*4]` is clear, `owner_record`
/// being the member's `[obj+0x70]`. A player outside 1..16 reads a record that
/// is not there in the original; here it keeps nothing.
HostOutcome m_obj_enemy(CallContext& ctx) {
  const PlayerId player =
      ctx.arg(1).is_integer() ? player_from_script(ctx.arg(1).as_integer()) : kNoPlayer;
  return filter_list(ctx, "ObjEnemy: receiver is not an ObjList", [&](const WorldObject& slot) {
    return player != kNoPlayer && world_of(ctx)->players().is_enemy(slot.state.owner, player);
  });
}

/// `ol.ObjAlly(player)` -- 2 sites, and **the complement of `ObjEnemy`, not
/// "allied"**: 0x005609e0 keeps a member whose owner's row has bit 0 *set*
/// for `player`, which is everyone the owner is not at war with, neutrals
/// included.
HostOutcome m_obj_ally(CallContext& ctx) {
  const PlayerId player =
      ctx.arg(1).is_integer() ? player_from_script(ctx.arg(1).as_integer()) : kNoPlayer;
  return filter_list(ctx, "ObjAlly: receiver is not an ObjList", [&](const WorldObject& slot) {
    return player != kNoPlayer && !world_of(ctx)->players().is_enemy(slot.state.owner, player);
  });
}

/// `ol.ObjClass(name)` -- 2 sites. 0x00560c10 looks the class up by name
/// (0x00409e40) and keeps every member that **is a** that class (0x0059a220,
/// the walk up `[class+0xe4]`); a name no class carries skips the walk and
/// answers the empty list. The name string is released either way (0x00695e70).
HostOutcome m_obj_class(CallContext& ctx) {
  World* world = world_of(ctx);
  const ClassGraph* graph = world == nullptr ? nullptr : world->class_graph();
  const ClassIndex wanted = graph != nullptr && ctx.arg(1).is_string()
                                ? graph->lookup(ctx.arg(1).as_string())
                                : kNoClass;
  return filter_list(ctx, "ObjClass: receiver is not an ObjList", [&](const WorldObject& slot) {
    if (wanted == kNoClass || slot.class_index == kNoClass) return false;
    for (const ClassIndex ancestor : graph->ancestry(slot.class_index)) {
      if (ancestor == wanted) return true;
    }
    return false;
  });
}

/// `ol.ObjInjured()` -- 1 site. 0x00561090 keeps a member whose health is
/// below its maxhealth -- `[obj+0xc0] < [obj+0xc8]`, the pair `Obj::health`
/// and `Obj::maxhealth` read. `[obj+0xc8]` is the object's **own** maximum,
/// which `Unit::RecalcBonuses` rebuilds from the class plus whatever bonuses
/// the unit carries, so it is `object_max_health` here -- the same number
/// `.maxhealth` answers -- and not the class property: a unit whose bonus has
/// lifted its maximum above the class's is injured until it reaches the new
/// one.
HostOutcome m_obj_injured(CallContext& ctx) {
  return filter_list(ctx, "ObjInjured: receiver is not an ObjList", [&](const WorldObject& slot) {
    return slot.state.health < object_max_health(*world_of(ctx), slot);
  });
}

/// `q.NearestObj(pt)` and `q.NearestObj(o)` -- 19 sites in 11 scripts, and the
/// sole blocker of ten of them.
///
/// **Two registrations in `gbr.exe`, one entry point here.** `0x005794c0`
/// takes a point and `0x00579620` takes an object; both are registered twice
/// over, as the free function `NearestObj` and as the member `Query::NearestObj`
/// (the free form is the `NamedObj -> Query` upcast this file already serves
/// for `count` and `GetObjList`). The registry keys on `(kind, name, arity)`
/// and both forms are arity 1, so one body answers for both and branches on the
/// argument instead -- which is what the compiler did at the call site anyway.
///
/// The receiver is a **`Query`**, never an `ObjList`: both bodies begin by
/// purging the receiver (`vtbl + 0x1c`) and then reading its list (`vtbl +
/// 0x20`, refreshed when due), so they scan the cached list. Every one of the 19
/// shipped receivers is a query variable -- `Q_HannibalArmy.NearestObj(...)`,
/// `EnemyObjs(5, cMilitary).NearestObj(AreaCenter("A_Battle"))`.
///
/// **Distance is to the candidate's edge, not to its centre.** The point form
/// scores with `0x005a7900`, which is `isqrt(dx*dx + dy*dy) - candidate.radius`;
/// the object form scores with `0x005a77b0`, the same thing less the *other*
/// object's radius as well. Both are integer square roots of an integer sum,
/// so nothing here needs a float. The radius is the class's `[class+0x2dc]`;
/// the position is the object's own `[+0x24]`/`[+0x28]`, which this engine
/// answers with `resolve_position` -- the same for every object a query can
/// return, since a query's members stand on the map. The object argument is
/// resolved but not tested for death: a dead origin still measures.
///
/// Three details of the scan are read straight off the loop and reproduced:
///
///   * the running best starts at **10,000,000** (`0x00579576`), and the
///     comparison is `jge skip` -- strictly less wins. So a query whose every
///     member is further than ten million units away answers the invalid
///     object, and ties keep the **first** member in query order rather than
///     the lowest id. That is the opposite of `FilterClosest` below, which
///     sorts and breaks ties by id; the two really do differ, because one is a
///     scan and the other is a sort.
///   * a member whose handle no longer resolves is skipped, not counted.
///   * an unresolvable *receiver* prints a diagnostic and pushes the invalid
///     object (`0x0057952c`), so a dead query is an answer and not a refusal.
///     An unresolvable object argument is the same, with `Parameter #1 in
///     function 'Query::NearestObj' is uninitialized or invalid object.`
///     (0x00579620 tests the receiver first, then the argument).
///   * the receiver is purged before the scan (`vtbl+0x1c`) and the
///     reference the call held on it is dropped after (0x00571680).
HostOutcome m_nearest_obj(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("NearestObj: no world");

  // Where we are measuring from, and what to subtract for it. An object
  // argument brings its own radius into the score; a point brings nothing.
  Point origin{};
  std::int64_t other_radius = 0;
  if (is_point(ctx.arg(1))) {
    origin = unpack_point(ctx.arg(1));
  } else if (ctx.arg(1).is_object()) {
    const ObjectId id = live_object_of(*world, ctx.arg(1));
    const WorldObject* slot = id == kNoObject ? nullptr : world->find(id);
    if (slot == nullptr) return HostOutcome::ok_with(invalid_object());
    origin = world->resolve_position(id);
    other_radius = class_int(*world, *slot, "radius");
  } else {
    return HostOutcome::failed("NearestObj: expected a point or an object");
  }

  std::vector<ObjectId> members;
  if (is_named_obj(ctx.arg(0))) {
    members = named_obj_members(*world, ctx.arg(0));
  } else {
    const ObjectId id = live_object_of(*world, ctx.arg(0));
    const WorldObject* slot = id == kNoObject ? nullptr : world->find(id);
    if (slot == nullptr || slot->internal != InternalKind::query) {
      // Not a refusal: the original prints and pushes the invalid object.
      return HostOutcome::ok_with(invalid_object());
    }
    world->read_query(id, script_clock(ctx, *world), members);
  }

  // 0x00579576. Named rather than inlined because the value is the whole of
  // why an empty query and a query of very distant objects answer alike.
  constexpr std::int64_t kNoCandidate = 10'000'000;
  std::int64_t best = kNoCandidate;
  ObjectId winner = kNoObject;
  for (const ObjectId id : members) {
    // **Belt and braces.** Both producers above already drop what does not
    // resolve -- `World::evaluate_query` collects live objects and
    // `named_obj_members` answers nothing for a dead binding -- so no fault
    // injected into this file reaches it, and no test can. It is kept because
    // the original's loop makes the same test (`0x005795ba`) over a list it
    // does not own, and a future producer that hands back a raw id would
    // otherwise dereference it.
    const WorldObject* slot = world->find(id);
    if (slot == nullptr) continue;
    const Point at = world->resolve_position(id);
    const std::int64_t dx = static_cast<std::int64_t>(at.x) - origin.x;
    const std::int64_t dy = static_cast<std::int64_t>(at.y) - origin.y;
    const std::int64_t score =
        isqrt(dx * dx + dy * dy) - class_int(*world, *slot, "radius") - other_radius;
    if (score >= best) continue;  // `jge skip`: strictly less, so ties keep the first
    best = score;
    winner = id;
  }
  if (winner == kNoObject) return HostOutcome::ok_with(invalid_object());
  return HostOutcome::ok_with(Value::object(ObjectRef{kTypeObj, winner}));
}

/// `ol.FilterClosest(pt, n)` -- 6 sites, `n` is 1 at every one of them.
///
/// A new list of (at most) `n` members near `pt`. **It is a bounded heap, not a
/// sort**, and the answer's order is the heap's (0x005616c0, whose work is
/// 0x00561450):
///
///   1. `n <= 0` answers the empty list. Otherwise an array of `n` slots is
///      made, and `P` is the smallest power of two **above** `n` (1 -> 2,
///      3 -> 4, 4 -> 8).
///   2. The comparator (0x0055df20) asks whether slot `a` is **strictly
///      farther** from `pt` than `b`, by squared distance from each object's
///      position (`vtbl+0xc8`). A `b` that does not resolve is never nearer
///      (false); an `a` that does not resolve is always farther (true).
///   3. The receiver is walked front to back. While slots remain, the member
///      goes into the next one and is sifted up (0x0055eb40): while it is not
///      the root and its parent `(i-1)/2` is farther than it, the two swap. So
///      slot 0 holds the nearest.
///   4. Once the slots are full, each further member is a challenger. Indices
///      `P-2` down to `(P-1)/2` are visited; an index below `n`, or an odd one,
///      names slot `j` if `j < n` and slot `j/2` otherwise. Each named slot
///      that is farther than the current challenger becomes the challenger and
///      remembers its slot. If any slot did, the member is written over the
///      last one remembered and sifted up from there; otherwise it is dropped.
///   5. The answer is the filled slots in slot order.
///
/// With `n == 1` -- every shipped site -- that is: the first member, replaced
/// by each later one that is strictly nearer. **Ties keep the earlier member**,
/// not the lower id, which is what this used to infer.
HostOutcome m_filter_closest(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("FilterClosest: no world");
  if (!is_objlist(ctx.arg(0))) {
    return HostOutcome::failed("FilterClosest: receiver is not an ObjList");
  }
  if (!is_point(ctx.arg(1))) return HostOutcome::failed("FilterClosest: expected a point");
  if (!ctx.arg(2).is_integer()) return HostOutcome::failed("FilterClosest: expected a count");

  const Point origin = unpack_point(ctx.arg(1));
  ObjListPool& pool = objlist_pool_of(*world);
  const std::span<const ObjectId> view = pool.items(objlist_of(ctx.arg(0)));
  const std::vector<ObjectId> members(view.begin(), view.end());
  const std::int32_t wanted = ctx.arg(2).as_integer();

  // Step 2. Squared distance as a 64-bit sum; the original's is 32-bit, and
  // the two agree for every distance on a map.
  const auto farther = [&](ObjectId a, ObjectId b) {
    if (world->find(b) == nullptr) return false;
    if (world->find(a) == nullptr) return true;
    const auto d2 = [&](ObjectId id) {
      const Point at = world->resolve_position(id);
      const std::int64_t dx = static_cast<std::int64_t>(at.x) - origin.x;
      const std::int64_t dy = static_cast<std::int64_t>(at.y) - origin.y;
      return dx * dx + dy * dy;
    };
    return d2(a) > d2(b);
  };

  std::vector<ObjectId> slots;
  if (wanted > 0) {
    const std::size_t n = static_cast<std::size_t>(wanted);
    std::size_t span = 1;  // step 1: P
    for (std::size_t k = n; k > 1; k /= 2) span *= 2;
    if (span <= n) span *= 2;
    slots.reserve(std::min(n, members.size()));
    const auto sift_up = [&](std::size_t i) {  // step 3
      while (i >= 1) {
        const std::size_t parent = (i - 1) / 2;
        if (!farther(slots[parent], slots[i])) break;
        std::swap(slots[parent], slots[i]);
        i = parent;
      }
    };
    for (const ObjectId member : members) {
      if (slots.size() < n) {
        slots.push_back(member);
        sift_up(slots.size() - 1);
        continue;
      }
      // Step 4. `span - 2` is at least 0, since `span` is at least 2.
      ObjectId challenger = member;
      std::ptrdiff_t at = -1;
      const auto low = static_cast<std::ptrdiff_t>((span - 1) / 2);
      for (auto j = static_cast<std::ptrdiff_t>(span) - 2; j >= low; --j) {
        const auto uj = static_cast<std::size_t>(j);
        if (!(uj < n || (uj & 1u) != 0)) continue;
        const std::size_t k = uj < n ? uj : uj / 2;
        if (farther(slots[k], challenger)) {
          challenger = slots[k];
          at = static_cast<std::ptrdiff_t>(k);
        }
      }
      if (at >= 0) {
        slots[static_cast<std::size_t>(at)] = member;
        sift_up(static_cast<std::size_t>(at));
      }
    }
  }

  const ObjListId list = pool.acquire_temporary(ctx.script);
  std::vector<ObjectId>* out = pool.mutable_items(list);
  if (out == nullptr) return HostOutcome::failed("FilterClosest: the pool refused a new list");
  *out = std::move(slots);  // step 5
  return HostOutcome::ok_with(make_objlist_value(list));
}

// -- free functions --------------------------------------------------------

/// `MaxSetIdx` -- the settlement index space `IdxToSet` walks.
///
/// Both a free function and a bare name in the corpus (`for (i = 0; i <
/// MaxSetIdx; i += 1)` in `ESH_GOLDTRADE.VS`, `nMaxSet = MaxSetIdx();` in
/// `ECONOMYMONITOR.VS`), which is one entry point either way: the compiler
/// resolves a bare identifier as a zero-arity free function before it tries the
/// globals.
HostOutcome fn_max_set_idx(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("MaxSetIdx: no world");
  std::int32_t settlements = 0;
  for (const WorldObject& slot : world->objects()) {
    if (slot.internal == InternalKind::settlement) ++settlements;
  }
  return HostOutcome::ok_with(Value::integer(settlements));
}

/// `IdxToSet(i)` -- the `i`th settlement, in ascending object id order, which
/// is creation order.
///
/// **The original keeps a slot table, and this reproduces it while no
/// settlement has died.** Both entry points read one pointer vector, at
/// `[0x009a721c]+0x4c .. +0x50`:
///
///   * `MaxSetIdx` (0x00422f50) answers its length, null slots included;
///   * `IdxToSet` (0x0042c780) answers the settlement in slot `i`, and the
///     invalid handle when `i` is negative, past the end, or names a null slot.
///
/// A new settlement takes the **first null slot**, or appends when there is
/// none (0x005c3a80, called by both creators of a settlement at run time), so
/// a destroyed settlement leaves a hole that the next one fills. That is why
/// `DATA\SUBAI\HEN_IDLE.VS` guards its result with `if (!set.IsValid)
/// continue;`. This engine never despawns a settlement object, so the table
/// here has no holes and creation order is slot order. Not read: how the map
/// loader numbers the settlements it creates, which is assumed to be the map's
/// own order -- the order they are spawned in here.
///
/// `sim/economy.cpp` implemented this too, reading the argument as a
/// `SettlementId`. That copy is gone, because `MaxSetIdx` and `IdxToSet` have
/// to share one numbering -- every corpus site is `for (i = 0; i < MaxSetIdx;
/// i += 1) { s = IdxToSet(i); ... }` -- and `MaxSetIdx` counts world settlement
/// objects. Whichever reading turns out to be right, both entry points move
/// together.
HostOutcome fn_idx_to_set(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IdxToSet: no world");
  if (!ctx.arg(0).is_integer()) return HostOutcome::failed("IdxToSet: expected an index");
  std::int32_t remaining = ctx.arg(0).as_integer();
  if (remaining < 0) return HostOutcome::ok_with(invalid_object());
  for (const WorldObject& slot : world->objects()) {
    if (slot.internal != InternalKind::settlement) continue;
    if (remaining-- == 0) {
      return HostOutcome::ok_with(Value::object(ObjectRef{kTypeSettlement, slot.id}));
    }
  }
  // Out of range reads as an invalid handle, which is what the one corpus site
  // that could see it already tests for.
  return HostOutcome::ok_with(invalid_object());
}

}  // namespace

std::size_t register_objlist_host(script::HostRegistry& registry) {
  std::size_t defined = 0;
  const auto def = [&](script::CallKind kind, std::string_view name, std::uint16_t arity,
                       script::HostFn fn) {
    registry.define(kind, name, arity, fn);
    ++defined;
  };

  // In descending corpus call frequency, which is the order these were built
  // in and the order a reader should meet them.
  def(kMember, "count", 0, &m_count);              // 541 -- ObjList and Query
  def(kMember, "ClearDead", 0, &m_clear_dead);     // 173
  def(kMember, "GetObjList", 0, &m_get_objlist);   // 103
  def(kMember, "army", 0, &m_army);                // 92 -- the hero domain's list
  def(kMember, "Add", 1, &m_add);                  // 83
  def(kMember, "AddList", 1, &m_add_list);         // 67
  def(kMember, "Clear", 0, &m_clear);              // 34
  def(kMember, "RemoveList", 1, &m_remove_list);   // 29
  def(kMember, "Remove", 1, &m_remove);            // 17
  def(kMember, "Contains", 1, &m_contains);        // 12
  def(kMember, "FilterClosest", 2, &m_filter_closest);  // 6
  def(kMember, "ObjEnemy", 1, &m_obj_enemy);       // 2
  def(kMember, "ObjAlly", 1, &m_obj_ally);         // 2
  def(kMember, "ObjClass", 1, &m_obj_class);       // 2
  def(kMember, "ObjInjured", 0, &m_obj_injured);   // 1
  def(kMember, "NearestObj", 1, &m_nearest_obj);   // 19, all of them on a Query
  def(kMember, "GetUnitsOnBoard", 0, &m_get_units_on_board);  // 22
  // 4 sites, and the sole blocker of four scripts -- 127 call sites behind
  // one vector assignment.
  def(kMember, "GetUnitsInSameHolder", 0, &m_get_units_in_same_holder);  // 4
  // The free-function registration `gbr.exe` also carries (`NearestObj(q, pt)`,
  // arity 2 here because a free call counts no receiver) is **not** bound: no
  // shipped script writes it, and the member form is what the compiler emits
  // for `q.NearestObj(pt)`.

  def(kFree, "IdxToSet", 1, &fn_idx_to_set);       // 5
  def(kFree, "MaxSetIdx", 0, &fn_max_set_idx);     // 3

  return defined;
}

void install_objlist_teardown(script::Scheduler& scheduler) noexcept {
  scheduler.set_teardown_hook([](void* user, script::ScriptId id) {
    auto* context = static_cast<HostContext*>(user);
    if (context == nullptr || context->world == nullptr) return;
    objlist_pool_of(*context->world).release_script(id);
    // And the script's `IntArray`s and `StrArray`s, which are pooled the same
    // way and for the same reason. One hook rather than two, because the
    // scheduler has exactly one and because the two pools are released on
    // precisely the same event.
    context->world->arrays().release_script(id);
    // And its `SquadList`s. The scheduler has exactly one teardown hook and
    // every pool is released on precisely the same event.
    context->world->squadlists().release_script(id);
    // And its `Conversation`s. Four pools now, still one hook and still one
    // event: a script that ends releases everything it declared.
    context->world->conversations().release_script(id);
  });
}

std::size_t sweep_objlists(const script::Scheduler& scheduler, ObjListPool& pool) {
  const std::size_t capacity = pool.capacity();
  if (capacity == 0) return 0;

  // Mark. An entry survives if some live coroutine can still name it, and the
  // three places a `Value` can be are the whole of "can still name it".
  std::vector<char> keep(capacity, 0);
  const auto mark = [&keep, capacity](const script::Value& value) {
    if (!is_objlist(value)) return;
    const ObjListId id = objlist_of(value);
    if (id == kNoObjList) return;
    const std::size_t index = static_cast<std::size_t>(id) - 1;
    if (index < capacity) keep[index] = 1;
  };

  // An index walk over a vector kept sorted by script id: the same order on
  // every peer, which is the whole of the determinism this needs.
  for (const script::ScriptRecord& record : scheduler.scripts()) {
    if (record.dead) continue;
    for (const script::Frame& frame : record.execution.frames) {
      for (const script::Value& value : frame.locals) mark(value);
      for (const script::Value& value : frame.stack) mark(value);
    }
    mark(record.execution.result);
  }

  // An entry owned by a script this scheduler does not have is not this
  // sweep's to collect: its roots are somewhere unreachable from here -- a bare
  // `Vm` run, or an embedder holding a handle of its own -- and freeing it
  // would be the one mistake a collector cannot take back. Marking it keeps it.
  for (std::size_t index = 0; index < capacity; ++index) {
    if (keep[index] != 0) continue;
    const ObjListId id = static_cast<ObjListId>(index + 1);
    const script::ScriptId owner = pool.owner_of(id);
    if (owner == script::kNoScript || !scheduler.alive(owner)) keep[index] = 1;
  }

  return pool.release_unmarked(keep);
}

std::size_t sweep_queries(const script::Scheduler& scheduler, World& world) {
  // The roots are the query handles a live coroutine holds, in the same three
  // places `sweep_objlists` marks. A query handle travels as a `Query` value,
  // and an `Obj` value can carry one too (`object_of` accepts either tag).
  std::vector<ObjectId> roots;
  const auto mark = [&roots, &world](const script::Value& value) {
    if (!value.is_object()) return;
    const script::ObjectRef ref = value.as_object();
    if (ref.type != kTypeQuery && ref.type != kTypeObj) return;
    const auto id = static_cast<ObjectId>(ref.id);
    if (world.query_spec(id) != nullptr) roots.push_back(id);
  };
  for (const script::ScriptRecord& record : scheduler.scripts()) {
    if (record.dead) continue;
    for (const script::Frame& frame : record.execution.frames) {
      for (const script::Value& value : frame.locals) mark(value);
      for (const script::Value& value : frame.stack) mark(value);
    }
    mark(record.execution.result);
  }
  return world.collect_queries(roots);
}

void install_objlist_lifetime(script::Scheduler& scheduler) noexcept {
  install_objlist_teardown(scheduler);
  scheduler.set_pass_hook([](void* user, const script::Scheduler& sched) {
    auto* context = static_cast<HostContext*>(user);
    if (context == nullptr || context->world == nullptr) return;
    (void)sweep_objlists(sched, objlist_pool_of(*context->world));
    // And the scripts' queries, by the same reachability: the original's
    // reference count (`[query+0x48]`) brought to zero, from the other side.
    (void)sweep_queries(sched, *context->world);
  });
}

}  // namespace imperivm::core::sim
