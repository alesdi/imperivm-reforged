#pragma once

/// `ObjList`, the collection type the scripts are written around.
///
/// `count` is the second most used member in the whole host API (541 sites) and
/// `ol[i]` is how nearly every script reaches an object it did not receive as a
/// parameter. Nothing in Part 5 composes until this type exists: `EnemyObjs`,
/// `FriendlyObjs`, `GetPlayerUnits`, `ControllableObjs`, `Strongholds`,
/// `Outposts`, `ObjsInCircle`, `ObjsInRange`, `ObjsInSight` and `GetObjList`
/// all return one.
///
/// ## It is a reference, not a value
///
/// `TSH_RECRUITARMY.VS` declares itself `void, Settlement set, str class, int
/// num, ObjList ol` and fills `ol` for its caller. A cloned-on-assign `ObjList`
/// would make that script a no-op. So `Host::clone_for_assign` must leave an
/// `ObjList` alone, exactly as `script/host.hpp` already says handle types
/// require.
///
/// ## Lifetime is the hard part, and the corpus decides it
///
/// `TS_CARTHAGETACTIC.VS` declares `ObjList ol;` **inside a loop body**, three
/// separate times. A pool that mints a slot per execution of a declaration
/// leaks one vector per iteration for as long as the script runs, and an AI
/// tactic script runs for the whole match.
///
/// The policy that fixes this without a destructor the language does not have:
/// **a slot is keyed by `(script, local slot index)`, not by execution.** A
/// declaration reaching `default_value` a second time clears and reuses the
/// same entry, which is also what re-entering a scope means to the script. The
/// pool then holds at most one entry per `ObjList` local per live script, which
/// is bounded by the program text, and a script's entries are released when the
/// scheduler tears the script down.
///
/// ## It is world state
///
/// A suspended script is world state -- the original's dumps name a running
/// script per object -- so a local holding an `ObjList` has to survive a save.
/// The pool serialises with the world, iterates in slot order, and is hashed
/// only to the extent the original hashes it, which for script state is not at
/// all: `scriptstate` is zero in all nine desync dumps. Keep it out of the
/// hash and out of anything that feeds the hash.
///
/// ## The corpus corroborates the reference reading, and says what assignment does
///
/// `DATA\AI\TSH_RECRUITARMY.VS` opens with a comment from whoever hit this in
/// 2003, and it is the strongest single piece of evidence about the type:
///
///     //BUG!!!! HACK!!!!
///     //BE CAREFUL NOT TO DIRECTLY ASSIGN TO ol!!!!!!!
///     //ol IS USED TO RETURN THE OBJECTS AND ASSIGNING TO IT WILL NOT CHANGE
///     //THE ORIGINAL LIST!!!!!!!!!
///     //ol MUST BE CHANGED USEING ONLY THE Add/Remove METHODS!!!!!!!!!
///
/// Both halves matter. Mutation through `Add`/`Remove` reaches the caller, so
/// the parameter is a *reference to a shared list* -- which is why
/// `Host::clone_for_assign` must leave an `ObjList` alone. And assignment does
/// *not* reach the caller, so `ol = x` **rebinds the local handle** rather than
/// copying `x`'s contents into the list `ol` used to name. A C++ reference
/// (`ObjList&`) would behave the opposite way on the second point, so the
/// original's `ObjList` is a counted handle -- a smart pointer -- and the local
/// holds the pointer, not the object. That is exactly the representation here.
///
/// ## How a declaration reaches its entry
///
/// `Op::declare_local` carries the slot and `VmEnv` carries the script, and the
/// one-argument `Host::default_value` threw both away -- so this file used to
/// bind an `ObjList` lazily, handing back `kNoObjList` and minting the entry on
/// the first mutating call. That workaround is gone. `script/host.hpp` now has
///
///     virtual Value default_value(std::string_view type_name, DeclarationSite);
///
/// which the VM calls and whose default forwards to the one-argument form, so a
/// host that does not care about the site is untouched. `WorldHost` overrides it
/// and answers `ObjList` with `pool.acquire(site.script, site.slot)`, so the
/// entry exists the moment the declaration runs and is keyed by *where* it is
/// written rather than by how many times it has run. `ObjList ol;` in a loop
/// body clears and reuses one entry, which is what re-entering a scope means.
///
/// ## What bounds the pool
///
/// Two mechanisms, and both are needed.
///
/// **Declarations** are bounded by the keying above: at most one entry per
/// `ObjList` local per live script, which is bounded by the program text.
///
/// **Temporaries** -- lists a host function returned rather than a script
/// declared -- are bounded by `sweep_objlists`, a reachability sweep the
/// scheduler runs at the end of every pass. They need it: `ol =
/// qryDef.GetObjList();` sits at the top of a `while (1)` / `Sleep(1000)` loop
/// in `DATA\AI HELPERS\GUARD.VS` and nine other files, and nothing else would
/// ever drop the one it replaced -- the language has no destructor and
/// `Op::store_local` does not tell the host which handle an assignment
/// displaced. Since the original's `ObjList` is a counted handle (above), a
/// sweep is that refcount arrived at from the other side. `install_objlist_
/// lifetime` wires both it and `release_script`; see those declarations.
///
/// ## What is still unknown
///
/// 1. **A list can outlive the script that owns it.** `TS_CARTHAGETACTIC.VS`
///    fills a local `ol` and passes it to `AIRun("TS_AttackAtWill.vs", set, ol,
///    ...)`, and the spawned script reads it for as long as it runs. The sweep
///    keeps it alive -- the child's local is a root -- but `release_script` does
///    not: if the parent is torn down first, the entry goes and the child is
///    left naming a stale handle, which reads as an empty list rather than
///    trapping. "Empty" is not "what the parent put there". Shared ownership (a
///    real refcount, or reparenting a list onto the spawned script) would fix
///    it; the corpus does not say which the original did, and the dumps cannot,
///    because an `ObjList` takes no object handle and so appears in no dump
///    block. What would settle it: a save/reload conformance replay in which a
///    parent tactic script ends while its `TS_AttackAtWill` child still runs.
///
/// 2. **`Add` may or may not deduplicate.** `DATA\TUTORIALS\BUILDINGSADVICE4.VS`
///    guards `OL_UnattachedVillages.Add(...)` with `.Contains(...)`, which is
///    consistent with either. Plain append is implemented, because that is what
///    the guard would be redundant against and the script keeps the guard.
///
/// 3. **`FilterClosest`'s tie-break is inferred.** Distance ascending, ties by
///    ascending object id. All six corpus sites ask for one element, so only
///    two objects at exactly equal distance could tell the difference.
///
/// One thing that looked unknown and is not: the cursor protocol. `Cur` (72
/// sites), `Next` (32) and `EOL` (27) never appear on an `ObjList` or a `Query`
/// in the corpus -- every one of the 131 receivers is a `SquadList`
/// (`DATA\AI\GS_CAPTURE.VS`, `GSH_SYNCHAPPROACH.VS`, `AIOSENDSQUAD.VS` and
/// eight more, all declared `SquadList SL;`). `ObjList` is indexed, never
/// walked with a cursor. The protocol belongs to `sim/squad.hpp`.

#include <cstdint>
#include <span>
#include <vector>

#include "imperivm/core/formats/result.hpp"
// `script/host.hpp` forward-declares `script::Scheduler`, which is all the
// teardown hook's declaration needs. `sim/world.hpp` includes this header, and
// dragging the VM's headers into everything that wants an object id is what
// `sim/world_host.hpp` exists to avoid.
#include "imperivm/core/script/host.hpp"
#include "imperivm/core/script/value.hpp"
#include "imperivm/core/sim/system.hpp"

namespace imperivm::core::sim {

/// The `TypeId` an `ObjList` handle carries. `sim/world_host.hpp` owns 1 to 4
/// and 8; `sim/squad.hpp` 6 and `sim/globals.hpp` 7. The whole set is pinned by
/// `no_two_handle_types_share_a_type_code`.
inline constexpr script::TypeId kTypeObjList = 5;

/// A pooled list's handle. Zero is never issued, so a default-constructed
/// `ObjList` value is distinguishable from a real empty one.
using ObjListId = std::uint32_t;

inline constexpr ObjListId kNoObjList = 0;

[[nodiscard]] script::Value make_objlist_value(ObjListId id) noexcept;
[[nodiscard]] bool is_objlist(const script::Value& value) noexcept;
/// `kNoObjList` when `value` is not an `ObjList`.
[[nodiscard]] ObjListId objlist_of(const script::Value& value) noexcept;

/// The lists every live script holds.
///
/// Deliberately a separate object rather than a member of `World`: `World` owns
/// the clock and the objects and stays small, and every other Part 5 domain
/// reaches its own state the same way.
class ObjListPool {
 public:
  // Declared and defined out of line because `Entry` is incomplete here: a
  // world holds a pool by value, and its destructor would otherwise need the
  // definition.
  ObjListPool();
  ~ObjListPool();
  ObjListPool(ObjListPool&&) noexcept;
  ObjListPool& operator=(ObjListPool&&) noexcept;
  ObjListPool(const ObjListPool&) = delete;
  ObjListPool& operator=(const ObjListPool&) = delete;

  /// Mint or reuse the entry for one declaration site.
  ///
  /// `script` is `CallContext::script` and `slot` is the local slot index. The
  /// same pair always names the same entry, which is what bounds the pool; the
  /// entry is cleared, so a re-entered scope sees an empty list.
  ObjListId acquire(script::ScriptId script, std::uint32_t slot);

  /// Mint an entry with no declaration site, for a list a host function
  /// returns. Released with the script that received it.
  ObjListId acquire_temporary(script::ScriptId script);

  /// Mint a handle that **is** somebody else's list rather than one of its own.
  ///
  /// `Settlement::Units` (`gbr.exe` 0x005c3c70) allocates nothing: it reaches
  /// the settlement's holder through the handle at `settlement + 0x5e`, points
  /// at the list embedded at `holder + 0x28`, bumps its refcount and hands that
  /// back. So the roster a script gets **is** the garrison, and
  /// `Settlement::UnitsCount` (0x005c1d80) reads `holder + 0x40`, which is that
  /// same list's size member -- the two cannot drift because they are one
  /// object.
  ///
  /// The handle is a temporary like any other and is released with the script
  /// that took it; what is *not* released is the list, because the pool does
  /// not own it. `AliasResolver` is how the pool reaches it, and it resolves on
  /// every access rather than caching a pointer: a settlement store is a
  /// vector, and `create` can move every row it holds.
  ///
  /// `owner` is the object whose collection this names -- the settlement's
  /// holder. An alias whose owner has gone reads as an empty list, which is
  /// what a stale handle already reads as here.
  ObjListId acquire_alias(script::ScriptId script, ObjectId owner);

  /// Where an aliased entry's storage lives, or null when the owner is gone.
  ///
  /// A function pointer and a `void*`, which is the shape
  /// `install_objlist_teardown` already uses and for the same reason: the pool
  /// is below `EconomySystem` and must not learn what a settlement is.
  /// `EconomySystem::start` installs it.
  using AliasResolver = std::vector<ObjectId>* (*)(void* user, ObjectId owner);
  void set_alias_resolver(AliasResolver resolver, void* user) noexcept;

  /// The object an entry aliases, or `kNoObject` when it owns its storage.
  [[nodiscard]] ObjectId alias_of(ObjListId id) const noexcept;

  /// Drop every entry a script owns. Called when the scheduler tears it down.
  void release_script(script::ScriptId script);

  [[nodiscard]] bool contains(ObjListId id) const noexcept;
  /// The script an entry belongs to, or `kNoScript` when `id` is unknown.
  /// The sweep needs it: an entry whose owner is not a live script is nobody's
  /// to collect, because its roots are somewhere this pool cannot see.
  [[nodiscard]] script::ScriptId owner_of(ObjListId id) const noexcept;
  /// Empty when `id` is unknown, which is what an invalid handle should read as
  /// rather than a trap: `ol.count` on a stale list is zero, not an error.
  [[nodiscard]] std::span<const ObjectId> items(ObjListId id) const noexcept;
  /// Null when `id` is unknown.
  [[nodiscard]] std::vector<ObjectId>* mutable_items(ObjListId id) noexcept;

  [[nodiscard]] std::size_t size() const noexcept;
  /// One past the largest handle ever issued. Ids run `1 .. capacity()`, so a
  /// mark vector for a sweep is exactly this long.
  [[nodiscard]] std::size_t capacity() const noexcept;

  /// Free every live entry that `keep` does not mark, indexed by `id - 1`.
  ///
  /// The collector half of `sweep_objlists`; kept here because only this class
  /// may decide an entry is dead. Returns how many were freed. An index past
  /// the end of `keep` is treated as marked, so a pool that grew during the
  /// mark phase cannot lose an entry nobody had the chance to see.
  ///
  /// A byte per entry rather than `std::span<const bool>`, because the natural
  /// buffer for a mark phase is a `std::vector<bool>`, that is a bitfield, and
  /// a bitfield has no `bool*` to make a span from.
  std::size_t release_unmarked(std::span<const char> keep);

  // -- the saved game ----------------------------------------------------

  /// Append the whole pool to `out`, dead slots included.
  ///
  /// **Why a serialise pair here rather than a wider public API.** The pool
  /// cannot be rebuilt through `acquire`/`acquire_temporary` and it never could
  /// be, for three separate reasons, each of which is enough on its own:
  ///
  ///   1. **A dead slot's position is state.** `emplace` reuses the lowest dead
  ///      index before it grows the vector, so the handle the *next* acquire
  ///      hands out depends on which slots are dead and where. A replay that
  ///      skipped the dead ones would renumber every list after the first hole,
  ///      and a handle sitting in a suspended script's local would then name a
  ///      different list.
  ///   2. **Nothing exposes an entry's slot.** `owner_of` gives the script;
  ///      there is no `slot_of`, and adding one would only be readable, not
  ///      writable.
  ///   3. **A temporary carries a private sentinel slot** (`kTemporarySlot`),
  ///      which the header deliberately does not name.
  ///
  /// Widening the public API to cover all three would mean exposing `live`, the
  /// slot, the sentinel and an `emplace_at(index, ...)` -- four new ways for a
  /// caller with no business here to corrupt the invariant that bounds the pool
  /// -- to serve exactly one caller. A serialise pair costs one member function
  /// and reaches `entries_` directly. So: a pair.
  ///
  /// Self-describing, with its own magic and version, the shape
  /// `Scheduler::serialize` established. It is written inside the world's
  /// section because the pool is a `World` member; see `World::serialize`.
  void serialize(std::vector<std::byte>& out) const;

  /// Replace the pool with the one in `bytes`.
  ///
  /// **Atomic**: everything is decoded into a local vector and moved in only
  /// once every entry has read cleanly, so a truncated save leaves the pool
  /// exactly as it was.
  ///
  /// The entries' *contents* are `ObjectId`s and are not checked against a
  /// world here -- a pool does not have one. A stale id reads as a dead object
  /// through `World::find`, which is what a script holding a list across a
  /// despawn already sees, so nothing is gained by refusing it.
  [[nodiscard]] Status deserialize(std::span<const std::byte> bytes);

  /// Take `other`'s entries and keep this pool's resolver.
  ///
  /// The entries are saved state; the resolver is a seam the embedder
  /// installed on *this* pool and the save knows nothing about. `World::
  /// deserialize` decodes into a fresh pool and used to move-assign it over
  /// this one, which carried the fresh pool's null resolver with it -- so
  /// after every load, `Settlement::Units` handed out aliases that read as
  /// empty, and the first `enter` that asked `.Units.Contains(this)` walked
  /// its unit back to a town it was already in. Nothing hashed it. This is
  /// the one way a decoded pool becomes the live one.
  void adopt_entries(ObjListPool&& other) noexcept;

 private:
  struct Entry;
  /// Take a slot for a new list, reusing a dead one before growing. Returns the
  /// index, not the handle.
  std::size_t emplace(script::ScriptId script, std::uint32_t slot);

  std::vector<Entry> entries_;
  AliasResolver alias_resolver_ = nullptr;
  void* alias_user_ = nullptr;
};

/// The pool a world's scripts share, created on first use.
[[nodiscard]] ObjListPool& objlist_pool_of(World& world);

/// Implement `ObjList` and the collection entry points on `registry`.
///
/// Returns the number defined, so a caller can assert the count rather than
/// trust it -- the convention `register_world_host` and `register_economy_hosts`
/// already follow.
///
/// Must run *after* `register_world_host`: `count/0` is defined here for both
/// receivers it has in the corpus (`ObjList` and `Query`, 541 sites between
/// them) and `register_world_host` deliberately no longer defines it.
std::size_t register_objlist_host(script::HostRegistry& registry);

/// Make `scheduler` release a script's lists when it tears the script down.
///
/// The other half of the lifetime policy. Without it the pool grows for the
/// life of the match: every `ObjList` any script ever declared stays live.
///
/// A function-pointer hook rather than a direct call because `script::Scheduler`
/// is a layer below `sim` and must not learn what a world is; the hook is handed
/// the scheduler's `user`, which is the `HostContext*` every host function
/// already receives. Fires from `Scheduler::compact`, once per script, in
/// ascending id order -- so it is as deterministic as the pass that triggers it.
void install_objlist_teardown(script::Scheduler& scheduler) noexcept;

/// Free every pooled list no live script can still reach. Returns how many.
///
/// This is what makes a *temporary* -- a list a host function returned rather
/// than one a script declared -- bounded. `ol = qryDef.GetObjList();` at the top
/// of a `while (1)` / `Sleep(1000)` loop mints one per second for as long as
/// `GUARD.VS` runs, and nothing else ever drops it: the language has no
/// destructor, and `Op::store_local` does not tell the host which handle an
/// assignment displaced. The original engine needed none of this because its
/// `ObjList` is a counted handle (see the note at the top of this file); a
/// reachability sweep is the same semantics arrived at from the other side.
///
/// **A `Value` lives in exactly three places** and all three are roots here:
/// `Frame::locals`, `Frame::stack` and `Execution::result`. Nothing else in the
/// engine holds an `ObjListId` -- a pool entry holds `ObjectId`s, not handles --
/// so the mark is complete. Any future C++ state that caches an `ObjListId`
/// across a pass has to become a root here, or it will be collected.
///
/// **Entries owned by a script the scheduler does not have are left alone.** A
/// list minted under a bare `Vm` run -- a command's `groupverifier`, say -- has
/// its roots somewhere this scheduler cannot see, so it is not this sweep's to
/// collect, and `release_script` stays the way those are freed.
///
/// Deterministic: the mark is an index walk over an id-sorted vector and the
/// collect is an index walk over the pool, so two peers free the same entries on
/// the same pass. That matters rather than being tidy -- freed slots are reused
/// in index order, and a handle in a suspended script's local is world state.
std::size_t sweep_objlists(const script::Scheduler& scheduler, ObjListPool& pool);

/// Install both halves of the lifetime policy on `scheduler`: the teardown hook
/// above, and `sweep_objlists` at the end of every pass.
///
/// The one call an embedder needs. Without it the pool grows for the life of
/// the match.
void install_objlist_lifetime(script::Scheduler& scheduler) noexcept;

}  // namespace imperivm::core::sim
