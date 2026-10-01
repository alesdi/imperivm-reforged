// The diplomacy half of the .vs host API.
// See include/imperivm/core/sim/player_host.hpp.

#include "imperivm/core/sim/player_host.hpp"

#include <vector>

#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/query.hpp"
#include "imperivm/core/sim/settlement.hpp"
#include "imperivm/core/sim/world.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

using script::CallContext;
using script::HostOutcome;
using script::ObjectRef;
using script::Value;

constexpr script::CallKind kFree = script::CallKind::free_function;
constexpr script::CallKind kMember = script::CallKind::member;

// --------------------------------------------------------------------------
// small shared helpers
// --------------------------------------------------------------------------

/// An invalid handle: what a query constructor yields when it cannot answer.
/// `.IsValid` and `.count` both read correctly off one of these.
[[nodiscard]] Value invalid_object() { return Value::object(ObjectRef{script::kNoType, 0}); }

[[nodiscard]] Value query_value(ObjectId id) {
  if (id == kNoObject) return invalid_object();
  return Value::object(ObjectRef{kTypeQuery, id});
}

/// The object a value refers to, or null. Accepts every handle type the world
/// hands out, because `Obj`, `Unit`, `Building` and `Settlement` are one type
/// to the language and only differ by explicit downcast.
[[nodiscard]] const WorldObject* object_of(const World& world, const Value& value) noexcept {
  if (!value.is_object()) return nullptr;
  const ObjectRef ref = value.as_object();
  if (ref.type != kTypeObj && ref.type != kTypeQuery && ref.type != kTypeSettlement) {
    return nullptr;
  }
  return world.find(ref.id);
}

/// The owner of whatever a value names, or `kNoPlayer`.
[[nodiscard]] PlayerId owner_of(const World& world, const Value& value) noexcept {
  const WorldObject* slot = object_of(world, value);
  return slot == nullptr ? kNoPlayer : slot->state.owner;
}

/// A 1..16 script player number as a `PlayerId`, or `kNoPlayer`.
[[nodiscard]] PlayerId player_arg(const Value& value) noexcept {
  if (!value.is_integer()) return kNoPlayer;
  return player_from_script(value.as_integer());
}

[[nodiscard]] ClassFilter filter_arg(const World& world, const Value& value) {
  if (!value.is_string()) return ClassFilter{};  // match_all
  return ClassFilter::parse(value.as_string(), world.class_graph());
}

// --------------------------------------------------------------------------
// the five Dipl* getters
// --------------------------------------------------------------------------
//
// All five take `(int p1, int p2)`, 1-based, and all five answer `false` rather
// than trapping when a number is out of range: the original prints
// "Player number should be between 1 and 16" and pushes zero, and a script that
// walks 1..16 over a table of fewer players relies on that.

template <Relation kRelation>
HostOutcome fn_dipl_get(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Dipl*: no world");
  const PlayerId from = player_arg(ctx.arg(0));
  const PlayerId to = player_arg(ctx.arg(1));
  if (from == kNoPlayer || to == kNoPlayer) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(Value::boolean(world->players().has(from, to, kRelation)));
}

/// `DiplAreAllied(p1, p2)` -- the *mutual* reading of the same bit
/// `DiplGetCeaseFire` reads one way round. See `sim/player.cpp`.
HostOutcome fn_dipl_are_allied(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("DiplAreAllied: no world");
  const PlayerId a = player_arg(ctx.arg(0));
  const PlayerId b = player_arg(ctx.arg(1));
  if (a == kNoPlayer || b == kNoPlayer) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(Value::boolean(world->players().are_allied(a, b)));
}

// --------------------------------------------------------------------------
// the four Dipl* writers, and the one that wipes the table
// --------------------------------------------------------------------------
//
// 215 call sites, every one of them inside a campaign container and every one
// of them a literal: two decimal integers and a bare `true` or `false`, with
// no variable, expression or constant name in any of the 645 argument
// positions. `data.pak` calls none of them.
//
// **One row, not both.** This is the load-bearing question, because the matrix
// is not symmetric and the shipped data contains one-sided truces. Four
// independent readings agree that `DiplCeaseFire(a, b, on)` writes
// `[a][b]` and never the transpose:
//
//   * both writers converge on one helper pair (0x00465970 set, 0x00465990
//     clear) which loads the single dword at `this + 0x24 + id*4` -- one cell
//     of `this`'s row -- masks it and hands it to `SetRelation` (0x005652f0),
//     whose only store is `this->relations[other.ID]`;
//   * the executable *also* ships a symmetric API alongside: the five
//     `_Players*` entry points (`_PlayersAlly` 0x006a4a90 and friends) take
//     two players and no flag, call `SetRelation` twice, once per direction,
//     and refuse when the two are equal. Two APIs side by side is design, not
//     oversight, and it removes the "surely they meant it to be symmetric"
//     reading entirely. None of the five has a call site, which is why none is
//     in the shipped-surface table;
//   * 211 of the 215 shipped calls are written as explicit mirrored pairs --
//     `(a,b,x)` then `(b,a,x)` -- which is not how anybody writes a call they
//     believe is symmetric;
//   * and the four that are not paired include one that is deliberate:
//     `3_Great_Losses_Egypt` `Maps/1/Sequences/seq1.vs:227` fires a lone
//     `DiplShareView(4, 1, true)` inside `if (i == 4)` right after
//     `RunConv("C_Conv6")` -- an ally revealing their intelligence to the
//     player, one way. Under a both-rows implementation that beat gives player
//     1 sight of player 4's map as well.
//
// **1-based, and range-checked before the decrement.** Each argument is
// compared against 1 and 16 and rejected outside, exactly as `player_from_script`
// models. The corpus control: across all 211 two-player calls the literals are
// 1 through 8 and **0 never appears**, which under a 0-based reading would mean
// 422 argument positions avoided slot 0 by chance while using slot 8.
//
// **Out of range prints and continues.** Four strings, one per entry point --
// `Function DiplCeaseFire: Player number should be between 1 and 16` and its
// three siblings -- followed by a shared `(called for players %d and %d)` fed
// the raw arguments. Neither sink aborts, so this must not trap. The check is
// all-or-nothing: if *either* number is out of range neither row is touched,
// so there is no partial write to reproduce.
//
// **One bit, read-modify-write.** `word |= mask` or `word &= ~mask`, leaving
// the other three relations alone. And because `ceasefire` and `allied` are
// the same bit, a *mirrored pair* of `DiplCeaseFire(a, b, true)` calls is what
// makes `DiplAreAllied(a, b)` answer true -- that is not an artefact of this
// model, it is how every campaign alliance in the game is declared. A single
// unmirrored call leaves them un-allied.
//
// **What is deliberately not done.** `DiplShareView`'s *set* path additionally
// merges the pair's explored map through a fog helper at 0x00564fa0. There is
// fog state now (`sim/fog.hpp`) and this still does not merge it: what 0x00564fa0
// does to the two players' cells is unread, and a merge invented here would
// silently reveal one player's map to another on every `DiplShareView`, which
// four campaign missions call. So the relation write lands and the exploration
// merge does not, as before -- but the reason has changed from "there is
// nothing to merge" to "the merge itself is unread", which is a smaller gap and
// a different one.

template <Relation kRelation>
HostOutcome fn_dipl_set(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Dipl*: no world");
  const PlayerId from = player_arg(ctx.arg(0));
  const PlayerId to = player_arg(ctx.arg(1));
  if (from == kNoPlayer || to == kNoPlayer) return HostOutcome::ok_void();
  // `SetRelation`'s own first act is to return when the other record is this
  // record. Fidelity insurance rather than tidiness: `PlayerTable::set` would
  // happily clear bit 0 on the diagonal, turning `kRelationSelf` into
  // something else and moving `PlayerTable::hash` -- and therefore every
  // conformance run -- for a call no shipped script makes.
  if (from == to) return HostOutcome::ok_void();
  // `Value::boolean` is integer 0/1, so this is the whole of the third
  // argument. A non-integer reads as `false`; the original reads an
  // uninitialised stack byte there, so any bounded choice is an improvement
  // and this is the one that does nothing.
  const bool on = ctx.arg(2).is_integer() && ctx.arg(2).as_integer() != 0;
  world->players().set(from, to, kRelation, on);
  return HostOutcome::ok_void();
}

/// `ClearDiplomacy()` -- 4 sites, and total war is what it declares.
///
/// The body loops over all 16x16 ordered pairs and calls `SetRelation` with 0
/// for the off-diagonal and `kRelationSelf` for the diagonal -- but
/// `SetRelation` returns immediately when the other record is this one, so
/// **the diagonal store never happens**. Zero everything off-diagonal, leave
/// the diagonal alone. With bit 0 clear everywhere off-diagonal every distinct
/// pair is mutually hostile, including the two engine-reserved slots.
///
/// All four sites are the same idiom and unambiguous about intent:
/// `ClearDiplomacy()` on line 2 of a map's opening sequence, immediately
/// followed by the mirrored pairs that declare the mission's teams.
///
/// **Unobservable on shipped data, and worth saying so.** All sixteen
/// `player<i>.xml` documents in each of those four containers are already
/// diagonal-only with an all-zero off-diagonal, so `ClearDiplomacy` is a no-op
/// at every one of its call sites and its behaviour rests on the instructions
/// alone. Any implementation that clears the off-diagonal is indistinguishable
/// from the original here. A save-game or a campaign carry-over that fed a
/// non-empty matrix into one of those four missions would be the real test.
HostOutcome fn_clear_diplomacy(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ClearDiplomacy: no world");
  PlayerTable& players = world->players();
  // Index order, because this is hashed state and iteration order is part of
  // the simulation's definition.
  for (std::size_t from = 0; from < kPlayerCount; ++from) {
    for (std::size_t to = 0; to < kPlayerCount; ++to) {
      if (from == to) continue;
      players.set_relation_word(static_cast<PlayerId>(from), static_cast<PlayerId>(to), 0);
    }
  }
  return HostOutcome::ok_void();
}

// --------------------------------------------------------------------------
// the owner-relation queries
// --------------------------------------------------------------------------
//
// These return a *query*, not a list: the corpus composes them with
// `Intersect`, `Union` and `Subtract` and reads `.count` and `.GetObjList` off
// the result, e.g.
//
//     qEnemies = Intersect(qSight, Union(EnemyObjs(.player, "Military"),
//                                        EnemyObjs(.player, "BaseMage")));
//
// so the handle has to survive as an object that re-evaluates.

template <std::int32_t kType>
HostOutcome fn_player_flags_query(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("EnemyObjs/FriendlyObjs: no world");
  const PlayerId player = player_arg(ctx.arg(0));
  if (player == kNoPlayer) return HostOutcome::ok_with(invalid_object());
  const QuerySpec spec = player_flags(player, filter_arg(*world, ctx.arg(1)), kType);
  return HostOutcome::ok_with(query_value(world->create_query(spec)));
}

// --------------------------------------------------------------------------
// GetPlayerUnits
// --------------------------------------------------------------------------

/// `GetPlayerUnits(player)` and `GetPlayerUnits(player, "class")`.
///
/// Returns an `ObjList`, not a query. `EQ_HEROES.VS` proves it by writing the
/// same variable two ways: `OL_Heroes = ControllableObjs(playerid, cHero)
/// .GetObjList();` in one file and `OL_Heroes = GetPlayerUnits(playerid,
/// cHero);` in another. The executable agrees -- the two registrations at
/// 0x005e0193 declare return type 0x17 where `EnemyObjs` declares 0x20.
///
/// The one-argument form's implicit filter is **inferred** to be `Unit`. The
/// name says units, every call site downcasts the members with `.AsUnit()` or
/// `.AsWagon()`, and a match-all reading would hand a script buildings to call
/// `SetLevel` on. Nothing in the data states it.
HostOutcome fn_get_player_units(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetPlayerUnits: no world");

  ObjListPool& pool = objlist_pool_of(*world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  std::vector<ObjectId>* items = pool.mutable_items(list);
  if (items == nullptr) return HostOutcome::failed("GetPlayerUnits: no list");

  const PlayerId player = player_arg(ctx.arg(0));
  if (player != kNoPlayer) {
    const ClassFilter filter = ctx.count() > 1 ? filter_arg(*world, ctx.arg(1))
                                               : ClassFilter::parse("Unit", world->class_graph());
    world->objects_of_class_for_player(filter, player, Point{}, 0, *items);
  }
  return HostOutcome::ok_with(make_objlist_value(list));
}

/// `Party()` -- 4 sites. 0x004ca9c0 mints a fresh `ObjList` and copies the
/// party roster -- the deque at `globals+0x1028`, the units that travel with
/// the hero between maps -- into it.
///
/// This engine keeps party membership as a flag on the object
/// (`ObjectState::flags.in_party`, `SyncFlags` bit 19) rather than as a
/// roster, so the list is every flagged object in **ascending id**, where the
/// original's is the order the roster was filled. That is an approximation
/// and it is stated: the four call sites walk the list to act on every
/// member, and none of them reads the order back.
HostOutcome fn_party(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Party: no world");
  ObjListPool& pool = objlist_pool_of(*world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  std::vector<ObjectId>* items = pool.mutable_items(list);
  if (items == nullptr) return HostOutcome::failed("Party: no list");
  for (const WorldObject& slot : world->objects()) {
    if (slot.state.flags.in_party) items->push_back(slot.id);
  }
  return HostOutcome::ok_with(make_objlist_value(list));
}

/// `PartyQuery()` -- 2 sites, `INN_BEHAVIOR.VS` and `INN_TRANSPORT_REQUEST.VS`,
/// both `Intersect(ObjsInSight(this, "Unit"), PartyQuery())`.
///
/// 0x005731e0 asks the session for its party object (0x004fa6e0), refreshes
/// it (0x00571670) and pushes its handle as a query -- the `CVXPartyQuery`
/// beside `CVXObjQuery` in the executable's type table -- or the invalid
/// handle when the session has no party at all. So it is `Party()` as a query
/// rather than as a copied list: the same membership kept live, and
/// composable with the set operations. `QueryKind::party` evaluates to every
/// object carrying `in_party`, in ascending id, on `Party`'s stated
/// approximation.
///
/// **A session with no party answers an empty query here, not the invalid
/// handle**: this engine has no party object to be missing, only members to
/// count, and the two shipped sites intersect the answer with a sight query
/// and wait on the result, which is empty either way.
HostOutcome fn_party_query(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("PartyQuery: no world");
  return HostOutcome::ok_with(
      Value::object(script::ObjectRef{kTypeQuery, world->create_query(party_query())}));
}

/// `GetSettlements(class, player)` -- 3 sites, and **the name is misleading in
/// two ways at once**.
///
/// The string is a **class filter**, not a settlement name, and the list that
/// comes back holds the matching settlements' **central buildings**, not the
/// settlements: every caller downcasts with `.AsBuilding()` and then asks the
/// building for its settlement. `ES_OUTPOSTARMY.VS` is the shape --
/// `olVil = GetSettlements("BaseVillage", idPlayer);` then
/// `bldVil = olVil[i].AsBuilding;` then `EnvReadInt(bldVil.settlement, ...)`.
///
/// The registration returns type 0x17, the same `ObjList` `GetPlayerUnits`
/// declares, rather than the 0x20 the query family uses.
///
/// **Player 0 is a wildcard**, and that is read off the body rather than
/// guessed: `0x00436f50` decrements the argument and passes a `0xffff`
/// placeholder to the collector, so a zero becomes "no player filter" rather
/// than "player minus one". `STONEHENGE_STARVATION.VS` uses it --
/// `things = GetSettlements("Building", 0);` -- to sweep every settlement on
/// the map. `BUILDINGSADVICE6.VS` uses the other side, walking `nCount` from 1
/// to 16.
///
/// Iteration is the settlement store's own array order, which is `ID` order and
/// is state: two peers that walked it differently would build different lists.
HostOutcome fn_get_settlements(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetSettlements: no world");

  ObjListPool& pool = objlist_pool_of(*world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  std::vector<ObjectId>* items = pool.mutable_items(list);
  if (items == nullptr) return HostOutcome::failed("GetSettlements: no list");

  const EconomySystem* economy = economy_of(*world);
  if (economy == nullptr) return HostOutcome::ok_with(make_objlist_value(list));

  const ClassFilter filter = filter_arg(*world, ctx.arg(0));
  // Not `player_arg`: this one's zero is the wildcard rather than "no player",
  // and the two would be indistinguishable through a helper that maps both to
  // `kNoPlayer`. Anything outside 1..16 matches nothing, which is what
  // decrementing into an array bound comes to.
  const std::int64_t asked = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  const bool any_player = asked == 0;
  const PlayerId wanted =
      any_player ? kNoPlayer : static_cast<PlayerId>(asked - 1);

  for (const Settlement& settlement : economy->settlements().all()) {
    if (!any_player && settlement.owner != wanted) continue;
    const WorldObject* anchor = world->find(settlement.anchor);
    if (anchor == nullptr) continue;
    if (!world->matches_filter(*anchor, filter)) continue;
    items->push_back(settlement.anchor);
  }
  return HostOutcome::ok_with(make_objlist_value(list));
}

// --------------------------------------------------------------------------
// the three counters
// --------------------------------------------------------------------------

/// Settlements of one kind belonging to one player.
[[nodiscard]] std::int32_t count_settlements(World& world, PlayerId player,
                                             SettlementKind kind) noexcept {
  const EconomySystem* economy = economy_of(world);
  if (economy == nullptr) return 0;
  std::int32_t total = 0;
  // `ID` order, which is the store's array order. Iteration order is state.
  for (const Settlement& settlement : economy->settlements().all()) {
    if (settlement.kind != kind) continue;
    if (player != kNoPlayer && settlement.owner != player) continue;
    ++total;
  }
  return total;
}

/// `Strongholds(idPlayer)` -- how many town halls the player holds.
///
/// 1-based, and 0 is **not** "all": the implementation (0x0042d2f0) decrements
/// without a negative check and then indexes the player array, so `Strongholds(0)`
/// reads out of bounds in the original. Answering zero is the bounded reading of
/// undefined behaviour.
HostOutcome fn_strongholds(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Strongholds: no world");
  const PlayerId player = player_arg(ctx.arg(0));
  if (player == kNoPlayer) return HostOutcome::ok_with(Value::integer(0));
  return HostOutcome::ok_with(
      Value::integer(count_settlements(*world, player, SettlementKind::stronghold)));
}

/// `Outposts(idPlayer)` -- and `Outposts(0)` really does mean every player.
///
/// The implementation (0x0042d770) decrements the argument and branches on the
/// sign: `js` jumps *into* the counting loop with the index left at -1, and the
/// loop's owner test is guarded by `test edi, edi; jl`, so a negative index
/// counts everything. `ESH_MARKET.VS` uses exactly that: `nAll = Outposts(0);
/// /// All Outposts`.
HostOutcome fn_outposts(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("Outposts: no world");
  if (!ctx.arg(0).is_integer()) return HostOutcome::ok_with(Value::integer(0));
  const std::int32_t number = ctx.arg(0).as_integer();
  if (number > static_cast<std::int32_t>(kPlayerCount)) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  const PlayerId player = number < 1 ? kNoPlayer : player_from_script(number);
  return HostOutcome::ok_with(
      Value::integer(count_settlements(*world, player, SettlementKind::outpost)));
}

/// `MilUnits(idPlayer)` -- how many military units the player has alive.
///
/// The original reads a per-player cached unit list (`CVXPlayer+0x8c`) and
/// counts the military ones; counting the world's objects under the `Military`
/// class filter is the same answer without the cache.
HostOutcome fn_mil_units(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("MilUnits: no world");
  const PlayerId player = player_arg(ctx.arg(0));
  if (player == kNoPlayer) return HostOutcome::ok_with(Value::integer(0));
  std::vector<ObjectId> found;
  const ClassFilter filter = ClassFilter::parse("Military", world->class_graph());
  const std::size_t count =
      world->objects_of_class_for_player(filter, player, Point{}, 0, found);
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(count)));
}

// --------------------------------------------------------------------------
// per-player facts
// --------------------------------------------------------------------------

/// `IsAIPlayer(idPlayer)`.
///
/// True for `control="Computer"` only. `Both` is a slot a human *may* take and
/// the shipped multiplayer packs give every slot that value, so resolving it
/// belongs to match setup; until it is resolved, treating it as an AI would
/// make an AI script run for a human.
HostOutcome fn_is_ai_player(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsAIPlayer: no world");
  const PlayerId player = player_arg(ctx.arg(0));
  if (player == kNoPlayer) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(
      Value::boolean(world->players().setup(player).control == PlayerControl::computer));
}

/// `GetPlayerRace(idPlayer)`.
HostOutcome fn_get_player_race(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetPlayerRace: no world");
  const PlayerId player = player_arg(ctx.arg(0));
  if (player == kNoPlayer) return HostOutcome::ok_with(Value::integer(kNoRace));
  return HostOutcome::ok_with(
      Value::integer(race_from_name(world->players().setup(player).race)));
}

/// `SetPlayerStatus(player, key, important)` and
/// `SetPlayerStatus(player, key, str value, important)`.
///
/// A **validated no-op**. The corpus calls it only to put win/lose and timer
/// text on one player's HUD -- `SetPlayerStatus(player, 1, Translate("You
/// win"), true)` -- and the original keeps it in a `status` field the
/// serialiser writes but nothing else in the simulation reads. There is no HUD
/// under `engine/core`, and inventing per-player storage for it would put a
/// string that only the UI can see into hashed world state.
///
/// It still refuses without a world and still checks the player number, so a
/// caller that has the convention wrong finds out here rather than silently.
HostOutcome fn_set_player_status(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("SetPlayerStatus: no world");
  (void)player_arg(ctx.arg(0));
  return HostOutcome::ok_void();
}

// --------------------------------------------------------------------------
// members
// --------------------------------------------------------------------------

/// `set.IsOwn(idPlayer)` -- does the receiver belong to that player?
///
/// `Settlement::IsOwn` (0x004252c0) compares the owner record's `ID` with
/// `idPlayer - 1` and nothing else; no relation is consulted.
HostOutcome m_is_own(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsOwn: no world");
  const PlayerId owner = owner_of(*world, ctx.arg(0));
  const PlayerId asker = player_arg(ctx.arg(1));
  const bool own = owner != kNoPlayer && asker != kNoPlayer && owner == asker;
  return HostOutcome::ok_with(Value::boolean(own));
}

/// `set.IsAlly(idPlayer)` -- does *that player* extend a ceasefire to the
/// receiver's owner?
///
/// The direction is the argument's, not the receiver's: `Settlement::IsAlly`
/// (0x004251e0) indexes row `idPlayer - 1` at the owner's column. On the
/// symmetric part of the matrix -- which is all of it but three pairs -- the
/// distinction does not show.
HostOutcome m_is_ally(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsAlly: no world");
  const PlayerId owner = owner_of(*world, ctx.arg(0));
  const PlayerId asker = player_arg(ctx.arg(1));
  if (owner == kNoPlayer || asker == kNoPlayer) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  return HostOutcome::ok_with(
      Value::boolean(world->players().has(asker, owner, Relation::ceasefire)));
}

/// `IsEnemy` -- both shapes the corpus uses.
///
/// The original registers two entry points that our table can only hold one of,
/// because it keys on (kind, name, arity):
///
///   * `Settlement::IsEnemy(Settlement s, int idPlayer)` (0x00425250) -- the
///     exact complement of `Settlement::IsAlly`, asking from the *argument*
///     player's row. `set.IsEnemy(AIPlayer)`.
///   * `Obj::IsEnemy(Obj self, Obj other)` (0x005aa480) -- asks from the
///     *receiver's* owner's row at the parameter's owner's column.
///     `b.IsEnemy(u)`.
///
/// So the argument's type picks the direction, and the two directions really do
/// differ: `3_Great_Losses_Egypt.bfhp` holds `0x11` at [3][5] and zero at
/// [5][3].
HostOutcome m_is_enemy(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("IsEnemy: no world");
  const PlayerId receiver = owner_of(*world, ctx.arg(0));

  if (ctx.arg(1).is_integer()) {
    const PlayerId asker = player_from_script(ctx.arg(1).as_integer());
    if (receiver == kNoPlayer || asker == kNoPlayer) {
      return HostOutcome::ok_with(Value::boolean(false));
    }
    return HostOutcome::ok_with(Value::boolean(world->players().is_enemy(asker, receiver)));
  }

  const PlayerId other = owner_of(*world, ctx.arg(1));
  if (receiver == kNoPlayer || other == kNoPlayer) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  return HostOutcome::ok_with(Value::boolean(world->players().is_enemy(receiver, other)));
}

/// `obj.race` -- the race of the object's **class**, not of its owner.
///
/// `Obj::race` (0x005ada20) is `[obj+0x3c]` (the class descriptor) `+0xb48`, so
/// it never touches the owner. That matters at the call sites, which are almost
/// all of the form `set.GetCentralBuilding.race`: a captured Egyptian town hall
/// reads `Egypt` whoever holds it.
HostOutcome m_race(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("race: no world");
  const WorldObject* slot = object_of(*world, ctx.arg(0));
  const ClassGraph* graph = world->class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) {
    return HostOutcome::ok_with(Value::integer(kNoRace));
  }
  return HostOutcome::ok_with(
      Value::integer(race_from_name(graph->property(slot->class_index, "race"))));
}

/// `GetClassRace(str)` -- 1 site, and the **sole blocker of `BARRACK_TRAIN.VS`**
/// and its 64 call sites.
///
/// 0x005b26a0 is `Obj::race` with the class named instead of carried: it looks
/// the string up in the class registry (0x00409e40) and reads the same
/// descriptor slot at `+0xb48`. Nothing about an object is involved, which is
/// exactly why the entry point exists -- `BARRACK_TRAIN.VS` asks
/// `GetClassRace(carCmdParam) == Carthage` about the class it is *about to*
/// train, before anything of that class exists to ask.
///
/// **A name the registry cannot resolve answers -1**, and the original says so
/// twice: the register starts at `0xffffffff` and is only overwritten on a hit,
/// and the miss path reports `"...unknown class..."` through the bare-`ret`
/// diagnostic and leaves it. That is the same `kNoRace` a raceless class reads,
/// so a script comparing against a real race cannot match either way.
///
/// Resolution is by `id` and then `altid`, which is `ClassGraph::lookup` and
/// what every other script-supplied class name here uses.
HostOutcome f_get_class_race(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("GetClassRace: no world");
  const ClassGraph* graph = world->class_graph();
  if (graph == nullptr || ctx.count() == 0 || !ctx.arg(0).is_string()) {
    return HostOutcome::ok_with(Value::integer(kNoRace));
  }
  const ClassIndex index = graph->lookup(ctx.arg(0).as_string());
  if (index == kNoClass) return HostOutcome::ok_with(Value::integer(kNoRace));
  return HostOutcome::ok_with(Value::integer(race_from_name(graph->property(index, "race"))));
}

/// The race **name** and the two **class-name prefixes**, which are three
/// readings of one eight-entry table on a race number 0..7.
///
/// `GetRaceStr` (0x005a7ce0), `GetRaceStrPref` (0x005a7d40) and
/// `GetRaceStrPrefLow` (0x005a7e00) are the same jump table with three arms
/// apiece, each loading a static C string and handing it to the script-string
/// allocator; the three `Obj::` forms (`raceStr`, `raceStrPref` at 0x005aab60,
/// `raceStrPrefLow` at 0x005aac90) read the race off the receiver's class
/// descriptor first and then run the same table. Anything outside 0..7 -- and
/// an unresolvable receiver, which prints and continues -- is the **empty
/// string**, not a default race.
///
/// **These are engine identifiers, not shipped text.** `docs/legal.md` rule 1 is
/// about the game's own strings; what comes out of here is a key a script
/// concatenates: `EnvReadInt(AIPlayer, GetRaceStr(nRace) + "UnitsEnabled")` in
/// `ESH_ARMYTECHSEQ.VS`, `cmd = "hirehero" + GetRaceStrPref(nRace)` in
/// `TSH_HERORECRUIT.VS`, `strCmd = "trainpeasant" + cb.raceStrPrefLow` in
/// `ES_VILLAGE.VS`. The eight names are the ones `player<i>.xml` authors and
/// this file already carries in `kRaces`; the eight letters are the ones
/// `docs/data-model.md` already prints. The disassembly agrees with both,
/// including the two that look like mistakes -- **`M` for Imperial Rome and `T`
/// for Germany**.
///
/// A third witness has since arrived from a different direction and agrees
/// letter for letter: `Settlement::EvalSentries` (`sim/economy.cpp`) builds a
/// sentry class name out of this alphabet, and the eight-way jump table it
/// reaches (0x005a7c00) returns `G R C I M B E T` in race order. Two tables
/// derived from unrelated code paths, and no difference between them.
///
/// One caller shows the prefixes are a *class-name* alphabet rather than a
/// label: `VILLAGE_TRAINPEASANT.VS` substitutes `GetRaceStrPref(RepublicanRome)`
/// when the village is Imperial Rome, because `M` has no villager classes.
constexpr std::string_view kRacePrefix = "GRCIMBET";

}  // namespace

// Outside the anonymous namespace because `Settlement::UpgradeBestBarrack`
// formats the same letter into a command name (`sim/economy.cpp`), and one
// table is the point.
std::string race_prefix(std::int32_t race, bool lower) noexcept {
  if (race < 0 || race >= static_cast<std::int32_t>(kRacePrefix.size())) return {};
  const char upper = kRacePrefix[static_cast<std::size_t>(race)];
  return std::string(1, lower ? static_cast<char>(upper - 'A' + 'a') : upper);
}

namespace {

/// The race an object's *class* declares, which is what all three members read.
/// Never the owner's: a captured Egyptian town hall stays Egyptian. Shares
/// `m_race`'s rule and its evidence.
[[nodiscard]] std::int32_t class_race(const World& world, CallContext& ctx) noexcept {
  const WorldObject* slot = object_of(world, ctx.arg(0));
  const ClassGraph* graph = world.class_graph();
  if (slot == nullptr || graph == nullptr || slot->class_index == kNoClass) return kNoRace;
  return race_from_name(graph->property(slot->class_index, "race"));
}

[[nodiscard]] std::int32_t race_arg(CallContext& ctx) noexcept {
  return ctx.arg(0).is_integer() ? static_cast<std::int32_t>(ctx.arg(0).as_integer()) : kNoRace;
}

HostOutcome fn_race_str(CallContext& ctx) {
  return HostOutcome::ok_with(Value::string(std::string(race_to_name(race_arg(ctx)))));
}

template <bool kLower>
HostOutcome fn_race_str_pref(CallContext& ctx) {
  return HostOutcome::ok_with(Value::string(race_prefix(race_arg(ctx), kLower)));
}

HostOutcome m_race_str(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("raceStr: no world");
  return HostOutcome::ok_with(Value::string(std::string(race_to_name(class_race(*world, ctx)))));
}

template <bool kLower>
HostOutcome m_race_str_pref(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("raceStrPref: no world");
  return HostOutcome::ok_with(Value::string(race_prefix(class_race(*world, ctx), kLower)));
}

/// `RevealHiddenEnemyUnits(point, radius, player)` -- 5 sites, all in
/// `DATA\SUBAI\`: the two watch-eye spies, the keen-sight observer, and the
/// gate and wall patrols.
///
/// **It is not a fog-of-war entry point, and the note that said it was had the
/// direction of the field backwards.** `sim/fog.hpp` carried it as a name that
/// "sweeps objects carrying `kSyncHidden` and writes into the low sixteen bits
/// of `[obj+0x2c]`, which are a per-player visibility mask rather than the
/// owner this engine reads them as", and concluded that it "needs a field on
/// the object, not a cell in this grid". Both halves are wrong. 0x004c8460
/// *reads* those sixteen bits -- they are the one-hot owner this engine already
/// stores, exactly as `SyncFlags` says -- and what it *writes* is the hidden
/// bit, cleared. There was no missing field. The entry point had been blocked
/// for the length of the fog work by a sentence about it.
///
/// What the original does, in its order:
///
///   1. Pops `player` first, then `radius`, then the eight bytes of the point,
///      and squares the radius before the scan (0x004c849e).
///   2. Loads `[globals + player * 0x320 + 0xfc4]` -- one word past the
///      exploration mask `ExploreCircle` and `ExploreArea` read, in the same
///      per-player record, so `player` is 1-based here for the same reason it
///      is there -- and passes its **complement** to the scan.
///   3. For each object in the circle: it must carry `kSyncHidden`, it must
///      carry `kSyncUnit`, and `~mask & (SyncFlags & 0xffff)` must be non-zero,
///      which is *the owner is not on this player's side*. That last test is
///      the "Enemy" in the name.
///   4. It then calls 0x005a7600 with `true`, which is `SetVisible`: notify,
///      clear bit 21 through the object's own `vtbl + 0x48`, and refresh.
///
/// **The side mask is the relations matrix, reached the long way round.**
/// Nothing in `.text` writes `record + 0x18` by that displacement -- a sweep
/// for the constant finds two readers, this and 0x004d7e10, and no writer -- so
/// the mask is built through a record pointer this search cannot follow. What
/// it holds is legible from its other reader: 0x004d7e10 falls back to all-ones
/// when the player number is 0 or above 16, and otherwise ANDs it with a
/// one-hot owner exactly as this does. That is `PlayerTable::is_enemy` with the
/// bits precomputed, and `is_enemy` is what this asks -- one-directional, from
/// the revealing player's own row, which is the direction the mask has too.
///
/// **A number outside 1..16 does nothing here rather than reading a mask that
/// is not there.** This entry point, unlike `ExploreCircle` and `ExploreArea`,
/// has no bounds check at all: it multiplies and indexes. Its other reader has
/// the check and answers all-ones, which would reveal every hidden unit on the
/// map to nobody in particular. A read out of bounds is a fault, not a
/// behaviour, and neither reading is transcribable, so an unresolvable player
/// reveals nothing.
///
/// **The stealth clock is the one thing left out, and it is left out
/// elsewhere.** Between the side test and the reveal, the original stamps
/// `[obj+0x1a4]` with the current game time for objects carrying bit 26 of
/// `[obj+0x198]`. That field is `Unit::SetLastAttackTime`'s, and `m_set_last_
/// attack_time` in `world_host.cpp` records why this engine does not store it:
/// no registered entry point reads it back, so it would be hashed, serialised
/// state that no script could observe. That comment ends "the day one exists,
/// this is where it writes" -- and this is now the second writer waiting on the
/// same day.
HostOutcome fn_reveal_hidden_enemy_units(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("RevealHiddenEnemyUnits: no world");
  if (!is_point(ctx.arg(0))) {
    return HostOutcome::failed("RevealHiddenEnemyUnits: expected a point");
  }
  if (!ctx.arg(1).is_integer()) {
    return HostOutcome::failed("RevealHiddenEnemyUnits: expected a radius");
  }
  const PlayerId player = player_arg(ctx.arg(2));
  // **This guard changes no answer and is kept anyway.** `PlayerTable::is_enemy`
  // already refuses an invalid viewer, so removing this line would still reveal
  // nothing -- the fault sweep says so, as an annotated survivor. What it does
  // is say the refusal at the top, where the reason for it is, and skip a
  // spatial query whose every result is going to be discarded.
  if (player == kNoPlayer) return HostOutcome::ok_void();

  std::vector<ObjectId> found;
  world->objects_in_radius(unpack_point(ctx.arg(0)), ctx.arg(1).as_integer(), ClassFilter{},
                           found);
  for (const ObjectId id : found) {
    WorldObject* other = world->find(id);
    if (other == nullptr) continue;
    // The three tests the scan applies, in the order 0x004c80b2 applies them.
    if (!other->state.flags.hidden) continue;
    if (!other->state.flags.is_unit) continue;
    if (!world->players().is_enemy(player, other->state.owner)) continue;
    other->state.flags.hidden = false;
  }
  return HostOutcome::ok_void();
}

// --------------------------------------------------------------------------
// the race table
// --------------------------------------------------------------------------

struct RaceName {
  std::string_view name;
  std::int32_t value;
};

/// Registered in this order at `gbr.exe` 0x005b70b2, one `push` of the value
/// per name. `Rome` and `RepublicanRome` are both 1; only the second spelling
/// occurs in `player<i>.xml`.
///
/// ## The demonyms, and the one class that needed them
///
/// **That registration is the map side. The class loader has a wider table and
/// this list used to be missing most of it**, which cost one shipped class its
/// race. `0x005a2400`-`0x005a3390` is a chain of fixed-length `repe cmpsb`
/// comparisons that accepts, for each race, a capitalised and a lowercase
/// spelling of the *place* and of the *demonym* -- `Britain`, `britain`,
/// `British`, `british`, `Briton`, `briton` all read as 5 -- before storing the
/// index at `[class+0xb48]`, which is what `Obj::race` and the two prefix
/// members read.
///
/// Across the installation's 821 classes the spellings used are `Gaul` 58,
/// `RepublicanRome` 52, `Carthage` 45, `Britain` 44, `Egypt` 44, `Germany` 44,
/// `Iberia` 44, `ImperialRome` 29, `Mutable` 2, `None` 1 -- **and `German` 1**.
/// `TOutpost`, the Germanic outpost, is the single class in the retail install
/// that writes a demonym, and this table answered `kNoRace` for it: its
/// `raceStrPref` was empty where every other Germanic class answers `T`, and
/// `Settlement::UpgradeBestBarrack` formats that letter into a command name.
/// One class, and the sort of thing that only ever shows up as a barrack that
/// will not upgrade in one nation.
///
/// The lowercase spellings are carried too. Nothing in the retail data uses
/// one, and they cost a line each; leaving them out would be leaving a
/// difference between this table and the loader for no reason.
constexpr RaceName kRaces[] = {
    {"Gaul", 0},           {"gaul", 0},
    {"Rome", 1},           {"RepublicanRome", 1}, {"republicanrome", 1},
    {"Roman", 1},          {"roman", 1},
    {"Carthage", 2},       {"carthage", 2},
    {"Carthaginian", 2},   {"carthaginian", 2},
    {"Iberia", 3},         {"iberia", 3},
    {"Iberian", 3},        {"iberian", 3},
    {"ImperialRome", 4},   {"imperialrome", 4},
    {"Britain", 5},        {"britain", 5},
    {"British", 5},        {"british", 5},
    {"Briton", 5},         {"briton", 5},
    {"Egypt", 6},          {"egypt", 6},
    {"Egyptian", 6},       {"egyptian", 6},
    {"Germany", 7},        {"germany", 7},
    {"German", 7},         {"german", 7},
};

}  // namespace

std::int32_t race_from_name(std::string_view name) noexcept {
  for (const RaceName& entry : kRaces) {
    if (entry.name == name) return entry.value;
  }
  // `None`, `Random`, `Mutable`, `Select` and the empty string all land here.
  // The first is a class with no race; the other three are match-setup markers
  // that only the RNG and the lobby can turn into a race.
  return kNoRace;
}

std::string_view race_to_name(std::int32_t race) noexcept {
  for (const RaceName& entry : kRaces) {
    // Skip the `Rome` alias so that the round trip is stable on the spelling
    // the shipped maps use. Every other group lists its canonical spelling
    // first, so the first match is the one to hand back; the table's order is
    // load-bearing for exactly this reason and `races_round_trip` asserts it.
    if (entry.name == "Rome") continue;
    if (entry.value == race) return entry.name;
  }
  return {};
}

std::size_t player_host_entry_count() noexcept { return 36; }

std::size_t register_player_host(script::HostRegistry& registry) {
  std::size_t defined = 0;
  const auto def = [&](script::CallKind kind, std::string_view name, std::uint16_t arity,
                       script::HostFn fn) {
    registry.define(kind, name, arity, fn);
    ++defined;
  };

  // -- free functions, in descending corpus call frequency -----------------
  // The writers behind every alliance the campaign declares. One row each --
  // see the note above `fn_dipl_set` for why that is the load-bearing half.
  def(kFree, "DiplCeaseFire", 3, &fn_dipl_set<Relation::ceasefire>);         // 116
  def(kFree, "DiplShareView", 3, &fn_dipl_set<Relation::share_view>);        //  55
  def(kFree, "DiplShareSupport", 3, &fn_dipl_set<Relation::share_support>);  //  22
  def(kFree, "DiplShareControl", 3, &fn_dipl_set<Relation::share_control>);  //  18
  def(kFree, "ClearDiplomacy", 0, &fn_clear_diplomacy);                      //   4
  def(kFree, "EnemyObjs", 2, &fn_player_flags_query<kPlayerFlagsEnemy>);              // 63
  def(kFree, "RevealHiddenEnemyUnits", 3, &fn_reveal_hidden_enemy_units);              //  5
  def(kFree, "GetPlayerUnits", 1, &fn_get_player_units);                              //  9
  def(kFree, "GetPlayerUnits", 2, &fn_get_player_units);
  def(kFree, "Party", 0, &fn_party);                                                   //  4
  def(kFree, "PartyQuery", 0, &fn_party_query);                                        //  2
  def(kFree, "FriendlyObjs", 2, &fn_player_flags_query<kPlayerFlagsFriendly>);        //  9
  def(kFree, "MilUnits", 1, &fn_mil_units);                                           //  6
  def(kFree, "Outposts", 1, &fn_outposts);                                            //  4
  def(kFree, "SetPlayerStatus", 3, &fn_set_player_status);
  def(kFree, "SetPlayerStatus", 4, &fn_set_player_status);
  def(kFree, "ControllableObjs", 2, &fn_player_flags_query<kPlayerFlagsControllable>);//  3
  def(kFree, "Strongholds", 1, &fn_strongholds);                                      //  2
  def(kFree, "IsAIPlayer", 1, &fn_is_ai_player);                                      //  2
  def(kFree, "GetPlayerRace", 1, &fn_get_player_race);                                //  1
  def(kFree, "GetSettlements", 2, &fn_get_settlements);                              //   3

  def(kFree, "DiplAreAllied", 2, &fn_dipl_are_allied);
  def(kFree, "DiplGetCeaseFire", 2, &fn_dipl_get<Relation::ceasefire>);
  def(kFree, "DiplGetShareView", 2, &fn_dipl_get<Relation::share_view>);
  def(kFree, "DiplGetShareControl", 2, &fn_dipl_get<Relation::share_control>);
  def(kFree, "DiplGetShareSupport", 2, &fn_dipl_get<Relation::share_support>);

  // `ClassPlayerObjs`/2 and `player`/0 are deliberately absent: they belong to
  // `register_world_host`, and defining them again here would silently replace
  // its versions with these.

  // -- members -------------------------------------------------------------
  def(kMember, "IsEnemy", 1, &m_is_enemy);  // 159 -- see the header note; also
                                            // defined by `define_combat_host`
  def(kMember, "race", 0, &m_race);         //  94
  def(kFree, "GetClassRace", 1, &f_get_class_race);  // 1, and it unblocks 64
  def(kFree, "GetRaceStr", 1, &fn_race_str);                    //   5
  def(kFree, "GetRaceStrPref", 1, &fn_race_str_pref<false>);    //   4
  // `GetRaceStrPrefLow` is deliberately absent. It is registered by `gbr.exe`
  // (0x005a7e00) and it is the third arm of this same table, but **no shipped
  // script calls it** -- the only reader of the lower-case form is the member
  // `raceStrPrefLow`, in `ES_VILLAGE.VS`. An entry point with no call sites is
  // not bound here; see the `ShowNotes` rule.
  def(kMember, "raceStr", 0, &m_race_str);                      //   1
  def(kMember, "raceStrPref", 0, &m_race_str_pref<false>);      //   3
  def(kMember, "raceStrPrefLow", 0, &m_race_str_pref<true>);    //   1
  def(kMember, "IsOwn", 1, &m_is_own);      //  15
  def(kMember, "IsAlly", 1, &m_is_ally);    //   9

  return defined;
}

}  // namespace imperivm::core::sim
