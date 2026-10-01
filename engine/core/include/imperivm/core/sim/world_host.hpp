#pragma once

/// The object-model half of the `.vs` host API, bound to a `World`.
///
/// `docs/formats/vs-host-api.md` inventories 185 free functions and 520 members
/// over 17,131 call sites, and `script/host.hpp` declares the whole surface up
/// front so that a script compiles today, runs to its first unimplemented call,
/// and says exactly which one it was. This header fills in the entries the
/// object model owns -- identity, position, owner, health, the class tree, the
/// query constructors and accessors, and `rand` -- with `HostRegistry::define`
/// and nothing else. The other four domains fill in theirs the same way.
///
/// Kept out of `sim/world.hpp` on purpose: four domains include that header and
/// none of them should be made to compile the script VM's headers to get an
/// object id.
///
/// ## How a world value is represented
///
/// A `script::Value` is an integer, a string, or an opaque `(TypeId, uint32)`
/// pair the host gives meaning to. Three of those meanings are defined here.
///
///   * **Objects** are `(kTypeObj, ObjectId)`. One type id for every handle
///     type in the language -- `Obj`, `Unit`, `Building`, `Hero` and the rest
///     -- because the language distinguishes them only by explicit downcast,
///     and a failed downcast is an invalid handle rather than a different type.
///   * **Queries** are `(kTypeQuery, ObjectId)`. A query *is* an object with a
///     handle; see sim/query.hpp for why that is not an implementation choice.
///   * **Points** are carried in the reference itself: `x` in `id` and `y` in
///     `aux`, each a full signed 32-bit integer. Points are value types in VS
///     -- `pt.SetLen(15)` mutates `pt` alone -- so representing them by an
///     index into a pool would need a clone-on-assign rule *and* would leak a
///     slot per point a script ever built. Carried inline, a point is a
///     genuine value: it copies, compares, serialises and hashes with no side
///     table at all. **Thirty-two bits, not sixteen**, because that is what
///     `gbr.exe` computes in: a point is eight bytes on its VM stack, and
///     `point * int` (`0x00696f00`) is a 32-bit `imul` per component with no
///     truncation, `+` (`0x00696e80`) and `/` (`0x00696f40`) the same. The map
///     is bounded by 16,383, but an expression is not: `WALL_PATROL.VS` and
///     `SENTRY_PATROL.VS` write `(route1*4 + route0)/5`, whose `route1*4`
///     passes 32,767 for any wall east or south of 8,191, and sixteen bits
///     wrapped it into a point across the map -- every sentry of a town
///     there walked off its wall.
///
/// `Settlement` gets its own type id because settlements are internal objects
/// with no native class, so a bare `Obj` handle to one would pass a class
/// filter it should not.

#include <cstdint>

#include "imperivm/core/script/host.hpp"
#include "imperivm/core/sim/host_context.hpp"
#include "imperivm/core/script/value.hpp"
#include "imperivm/core/sim/world.hpp"

namespace imperivm::core::script {
/// Forward-declared: `install_deferred_erase` needs only the name, and this
/// header is included by every domain.
class Scheduler;
}  // namespace imperivm::core::script

namespace imperivm::core::sim {

class AiProfile;

/// A handle to a simulated object. Every VS handle type is this one.
inline constexpr script::TypeId kTypeObj = 1;
/// A packed `(x, y)`; see the header note.
inline constexpr script::TypeId kTypePoint = 2;
/// A query object's handle.
inline constexpr script::TypeId kTypeQuery = 3;
/// A settlement's handle.
inline constexpr script::TypeId kTypeSettlement = 4;
/// A rectangle's index into `World::rects()`.
///
/// **Appended, not inserted.** A type code is save format: `script::write_value`
/// puts `(type, id)` for every object value in a suspended frame, so renumbering
/// an existing code would silently reinterpret every handle in every save.
///
/// 8 and not 5, which is what this constant shipped as for one commit. The
/// codes are handed out in four headers and the other three each carry the
/// running census, so "append after `kTypeSettlement`=4" reads as 5 and is
/// wrong: `sim/objlist.hpp` has 5, `sim/squad.hpp` 6 and `sim/globals.hpp` 7.
/// Nothing detected the overlap because `is_rect` and `is_objlist` then agreed
/// on every value and no test passed one where the other was expected --
/// `no_two_handle_types_share_a_type_code` is that test, and it enumerates all
/// eight so the next addition collides loudly instead of silently.
inline constexpr script::TypeId kTypeRect = 8;

/// Pack a world point into a script value.
[[nodiscard]] script::Value pack_point(Point p) noexcept;
/// Unpack one. Garbage in gives `(0, 0)`; callers check `is_point` first.
[[nodiscard]] Point unpack_point(const script::Value& value) noexcept;
[[nodiscard]] bool is_point(const script::Value& value) noexcept;

/// Intern a rectangle in `world` and pack its index into a script value.
[[nodiscard]] script::Value pack_rect(World& world, const RectTable::Rect& r);
/// The rectangle a value names, or null when it is not one this world holds.
[[nodiscard]] const RectTable::Rect* unpack_rect(const World& world,
                                                 const script::Value& value) noexcept;
[[nodiscard]] bool is_rect(const script::Value& value) noexcept;

/// Object-model services the interpreter cannot supply for itself: what a
/// `point` is, what adding two of them means, what an object prints as.
///
/// Deliberately partial. Anything not answered here -- `aSkills[i]` on an
/// `IntArray`, a global whose family has no table loaded -- falls through to
/// `script::Host`, whose defaults refuse, which is the designed behaviour: a
/// wrong answer is a divergence the conformance harness has to chase, and a
/// refusal is a name and a line number.
///
/// The 245 global constants and ambient globals are answered here, through
/// `sim/globals.hpp`. Not all 245 resolve, and the ones that do not refuse on
/// purpose; that header says which and why.
class WorldHost : public script::Host {
 public:
  explicit WorldHost(World& world) noexcept : world_(&world) {}

  [[nodiscard]] World& world() const noexcept { return *world_; }

  script::Value default_value(std::string_view type_name) override;
  /// The same, told where the declaration is.
  ///
  /// `ObjList` is the one type that needs the site, and the only reason this
  /// overload is here: its pool keys an entry by `(script, slot)` so that
  /// `ObjList ol;` inside a loop body reuses one entry rather than minting one
  /// per iteration. Everything else forwards to the one-argument form, which is
  /// all a `point` or a handle ever wanted.
  script::Value default_value(std::string_view type_name, DeclarationSite site) override;
  script::Value clone_for_assign(const script::Value& value) override;
  Result<bool> truthy(const script::Value& value) override;
  Result<script::Value> binary(script::BinaryOp op, const script::Value& lhs,
                               const script::Value& rhs) override;
  /// `ol[i]`, and nothing else yet: `aSkills[i]` is an `IntArray` and belongs
  /// to another domain, so it falls through to `script::Host`, which refuses.
  Result<script::Value> index_get(const script::Value& container,
                                  const script::Value& key) override;
  /// `ol[i] = o`. See the .cpp on why an out-of-range write refuses rather than
  /// growing the list.
  Status index_set(script::Value& container, const script::Value& key,
                   const script::Value& value) override;
  Result<std::string> to_string(const script::Value& value) override;

  /// A bare name that is neither a local nor a parameter: `sim/globals.hpp`.
  ///
  /// Delegates to `resolve_global`, which answers the engine's own 109
  /// constants from a compiled-in table, the `AIV_*` id space from the world's
  /// `EnvSystem`, and the four `AI.INI` families from `ai_profile()`. A name
  /// none of the three owns refuses, which the VM reports as an
  /// `unknown-global` trap naming the identifier and its line.
  Result<script::Value> global(std::string_view name) override;

  /// The AI profile the `SS_` / `GS_` / `ES_` / `TS_` constants come from, or
  /// null.
  ///
  /// **Not on `World` and not on `HostContext`, and neither is an oversight.**
  /// `Host::global` is handed a name and nothing else -- no `CallContext`, so
  /// no `user` pointer -- which rules out `HostContext`. And the profile is not
  /// world state: it is parsed from `DATA\AI\AI.INI`, it is identical on every
  /// peer, and `sim/ai_profile.hpp` already owns it. So it hangs on the host,
  /// the way `HostContext::translations` hangs off the context for the same
  /// reason. Null makes all 42 of those names refuse, which is the right
  /// behaviour for a world whose profile was never loaded: an AI that traps by
  /// name is easier to find than one running on invented ordinals.
  void set_ai_profile(const AiProfile* profile) noexcept { ai_profile_ = profile; }
  [[nodiscard]] const AiProfile* ai_profile() const noexcept { return ai_profile_; }

 private:
  World* world_ = nullptr;
  const AiProfile* ai_profile_ = nullptr;
};

/// Define every entry point this domain owns.
///
/// Call after `declare_shipped_surface`, which supplies the names and arities;
/// this only attaches behaviour. The world reaches the host functions through
/// `CallContext::user`, which the embedder sets to a `HostContext*` --
/// **not** a bare `World*`; see `sim/host_context.hpp` for what went wrong
/// while each domain chose its own type for that pointer.
///
/// Returns the number of entry points defined, so a caller can assert against
/// the count rather than trusting it.
/// The objects a member call's receiver stands for, in the order it yields
/// them, or empty when it stands for none.
///
/// **One rule, one definition.** `gbr.exe` registers the whole receiver-taking
/// family separately per receiver type -- `SetCommand` three times, on `Obj`
/// (`0x005b7568`), `ObjList` (`0x00563659`) and `Query` (`0x0057af1a`), each
/// with its own body -- and the `Query` body walks the query's own members and
/// applies the operation once per member. That is not a coercion to an
/// `ObjList` and not a subtype trick; it is a third registration. `NamedObj`
/// needs none of its own, because it is a registered *subtype* of `Query`
/// (`0x005b6d96`), so the compiler upcasts at zero cost.
///
/// This engine keys its registry on `(kind, name, arity)`, so one body answers
/// all of them -- and the expansion from receiver to objects is the part that
/// has to be shared. It was written twice instead, and the two disagreed on
/// two of the four types for a whole commit: `membership_receivers` here took
/// all four, `receivers_of` in `sim/command.cpp` took only `Obj` and
/// `ObjList`, and **545 shipped command-family call sites** were unreachable
/// because a bare `<group>` name resolves to a `Query`. Three near-copies of
/// one rule is how that survived; a fourth would do it again.
///
/// `accept_query` is `false` for the one entry point that genuinely refuses
/// one: `ExecCmd` is registered on `Obj` and `ObjList` and **not** on `Query`.
/// No shipped site passes it one, so this is unobservable on retail data and
/// is a stated choice rather than an implementation detail -- the receiver
/// table is readable ground truth, and a widening nothing evidences is how a
/// silent divergence starts.
///
/// The ids are **copied**, never viewed: issuing a command can spawn, despawn
/// or re-group, and a span into a container that moves is a use-after-free
/// waiting for a bigger list.
[[nodiscard]] std::vector<ObjectId> receiver_objects(World& world, const script::Value& receiver,
                                                     bool accept_query = true);

/// Whether `receiver` is a shape a receiver-taking member accepts at all.
///
/// The companion to `receiver_objects`, and the two answer different
/// questions: this one says whether the *handle* is a receiver, that one says
/// which objects it stands for. A live query that currently selects nothing is
/// a receiver -- the call applies to no members and that is not an error, the
/// same way an empty `ObjList` receiver is not -- while a point, an integer or
/// a dead handle is not a receiver. The reading members refuse one by name;
/// the ordering members ask `receiver_departed` below and apply to nothing.
///
/// Kept separate rather than folded into an empty-vector check because those
/// two cases were the same value and mean opposite things.
[[nodiscard]] bool is_receiver(World& world, const script::Value& receiver,
                               bool accept_query = true) noexcept;

/// Whether `receiver` is object-shaped and names nothing the world holds: an
/// invalid handle, a `NamedObj` whose binding has died, a unit that has left
/// the world. The *reading* members refuse such a handle by name; the
/// *ordering* members do not. `Obj::SetCommand` (0x005b0a60) and
/// `Obj::AddCommand` format `... called for invalid object` into the sink at
/// 0x686eb0, which is a bare `ret`, and return 0, so the script goes on --
/// `Maps/1/Sequences/seq7.vs` in Great Losses Rome orders `NO_Hero2` twelve
/// seconds after the hero can have died, and the rest of the sequence runs.
[[nodiscard]] bool receiver_departed(World& world, const script::Value& receiver) noexcept;

/// Kill every coroutine owned by an object the world no longer holds, and
/// forget its command queue. `Erase` does both itself (`perform_erase`); the
/// death path cannot, because `CombatSystem::advance` despawns the corpse at
/// the end of its dying state and holds no scheduler. Once per turn, after
/// the systems and before the scripts, so no script resumes on a handle this
/// turn's combat freed: the original frees a corpse through the same routine
/// an erase takes, and 0x00687c80 cancels the object's pending scheduled
/// messages there -- a cancelled message is a `Sleep` that never wakes.
/// `UNIT_AI_KILLALL.VS:24` resumed on a freed handle otherwise, and its
/// `.AddCommand` trapped. Returns how many scripts were killed.
///
/// **A corpse's scripts go too, at death rather than at removal.** All 15
/// dying objects in the nine dumps (`action=8`) print an empty `method=` and
/// an empty command queue, so the original has stopped the unit's behaviour
/// by the time it is lying there. It matters here because the dying state
/// lasts the whole death animation (`CombatSystem::kDefaultDeathDuration`),
/// 8.8 to 26 seconds, and a corpse whose `UNIT_ENGAGE.VS` kept running would
/// go on calling `GotoAttack` and walk. The class's `ondie` hook is out of
/// its reach twice over: it has no owner, and it has already run to its end
/// inside the death that fired it (`sim/hooks.hpp`).
std::size_t reap_departed(script::Scheduler& scheduler, World& world);

/// `Unit::HasFreedom` (0x005d7d30): bit 30 of the unit's specials word
/// (`[obj+0x198] & 0x40000000`, the `freedom` special, which the class's
/// `unit_specials` fills at construction) **or** a class descended from
/// `RamUnit` (0x00540460 asks `IsHeirOf` of that name). The sentries are why
/// it matters: every sentry class lists `Freedom`, `SQUADMONITOR.VS` attaches
/// a free unit to a hero only `if (!sqLeader.HasFreedom)`, and `UNIT_ATTACH.VS`
/// refuses one that has it -- so with a freedom that was never set, the AI
/// signed its towns' sentries into its heroes' armies and marched them off.
[[nodiscard]] bool unit_has_freedom(World& world, ObjectId id);

/// Start the class `<behavior>` scripts of object `id`: one coroutine per
/// entry of `ClassGraph::resolved_behaviors`, in slot order, each owned by the
/// object and handed the object as its one argument. Returns how many started.
///
/// What `gbr.exe`'s object start virtual (vtbl+0x2c, 0x005aec40) does once an
/// object is constructed: it walks the class's behaviour list (`[class+0x1ec]`
/// entries, 0x0059aec0) and spawns entry `i` into the object's script slot
/// `i + 1` through 0x006a07e0, passing the object's handle (`[obj+8]`) as the
/// argument list. Slot 0 is the command slot. The coroutines are the object's:
/// they live in its own timer map beside its command script (keys
/// `0x4ba74bd + slot`, 0x0069fe80), so they are saved with it (0x006a0410),
/// they end when it is destroyed, and nothing restarts them when it changes
/// hands -- `OUTPOST_BEHAVIOR.VS` and `TOWNHALL_SENTRIES_CONTROL.VS` watch
/// `.player` themselves. Owner here, so `perform_erase` and `reap_departed`
/// take them with everything else the object runs.
///
/// A path `library` cannot build is skipped: the original's loader keeps only
/// the behaviours that compiled (0x005a059c), so they were never in its list.
std::size_t start_behaviors(script::Scheduler& scheduler, ScriptLibrary& library,
                            const World& world, ObjectId id);

/// Kill the coroutines `start_behaviors` would have started for `id` as a
/// member of `class_index`: those it owns, spawned by nobody, running one of
/// that class's behaviour files. Returns how many were killed. For
/// `Unit::Mutate`, whose original destroys the object and constructs a new
/// one (0x005deef0), which starts the new class's behaviours in its turn.
std::size_t stop_behaviors(script::Scheduler& scheduler, ScriptLibrary& library,
                           const World& world, ObjectId id, ClassIndex class_index);

std::size_t register_world_host(script::HostRegistry& registry);

/// Install the scheduler hook that drains `World::deferred_erase`.
///
/// `Erase` on the object whose own script is running defers, exactly as
/// `gbr.exe`'s latch at `[0x009bdb14]` does, and this is the runner's side of
/// that: the hook fires after each script's slice and performs whatever the
/// slice asked for and could not do to itself. See `script::Scheduler::StepHook`
/// for why it must be per-step rather than per-pass.
///
/// Separate from `install_objlist_lifetime` because it is a different hook on
/// the same scheduler and because a domain installs its own; an embedder that
/// wants deferral calls both.
void install_deferred_erase(script::Scheduler& scheduler) noexcept;

/// Integer square root, floor. Exposed because `DistTo` needs it and so will
/// every other distance in the simulation, and there is exactly one right
/// answer only if everybody uses the same one -- `sqrt` is floating point and
/// therefore forbidden (docs/engine/architecture.md, rule 1).
[[nodiscard]] std::int64_t isqrt(std::int64_t value) noexcept;

/// A class property as an integer, walking up the class tree; 0 when there is
/// no graph, no class, or the value is not a plain integer.
///
/// Exported because `maxhealth` has two readers now: `Obj::maxhealth` and
/// `WaitHealthBetween`, which sums it over a query. A second copy of the parse
/// is how two readings of one property drift apart.
/// The element of `building`'s type-1 point list nearest `unit`, or the
/// building's own position when it has none -- `Building::GetEnterPoint`'s
/// picker, without the fallbacks that entry point wraps it in.
///
/// Exported because `Squad::UseTeleport` needs the same door: 0x005cda00 plus
/// 0x004db650 is the same pair `0x004dce60` runs, and one derivation of the
/// list is the point. `water` picks the deep-water list over the land one;
/// `GetEnterPoint` passes whether the unit is a ship and `UseTeleport` passes a
/// literal false.
[[nodiscard]] Point enter_point_near(const World& world, ObjectId building, ObjectId unit,
                                     bool water);

/// `Building::IsVeryBroken` -- `health < maxhealth / 20`, truncating.
///
/// Exported because it has two readers: the entry point of that name and
/// `Squad::InvadeThroughGate`, which refuses to march a squad through a gate
/// that is not one. `gbr.exe` guards both with the same five instructions
/// (0x00424fbf and 0x00429a41), so one function here is the point.
[[nodiscard]] bool is_very_broken(const World& world, const WorldObject& slot) noexcept;

[[nodiscard]] std::int32_t class_int(const World& world, const WorldObject& slot,
                                     std::string_view key) noexcept;

/// A class property as a flag: present, and not the string `"0"`.
///
/// Exported for the same reason `class_int` is. `water_unit` has two readers
/// now -- `Obj::IsWaterUnit` and the landing test in `sim/flying.cpp`, which
/// requires a landing spot to be water *if and only if* the class is a water
/// unit -- and the two must agree about what an absent property means.
[[nodiscard]] bool class_flag(const World& world, const WorldObject& slot,
                              std::string_view key) noexcept;

/// `point::Rot(degrees)`'s arithmetic, without the host-call wrapper.
///
/// Exported because `Flying::AdjustFlyDir` turns a heading by fifteen degrees
/// through the *same* constant: `0x7bea98` is 0.26179938779166667, which is
/// exactly `15 * 0.017453292519444445`, the first of the executable's three
/// approximations of pi. Reproducing that with a second table would be two
/// tables for one constant, which is how `Rot(90)`'s vanishing-cosine
/// behaviour would quietly stop applying to one of its callers.
[[nodiscard]] Point rotate_like_gbr(Point v, std::int32_t degrees) noexcept;

/// The catapult class a race builds: `gbr.exe` 0x004e3b30's eight-way table,
/// `GCatapult`, `RCatapult`, `CCatapult`, `ICatapult`, `RCatapult` again for
/// Imperial Rome, `BCatapult`, `ECatapult`, `TCatapult`, in `Race` order.
/// Empty for anything outside the table, which the original answers with a
/// null class. Shared by `PlaceCatapult` and the siege planner.
[[nodiscard]] std::string_view catapult_class_for_race(std::int32_t race) noexcept;

/// 0x004e3be0: the second half of `PlaceCatapult`, on a resolved class. Creates
/// the engine at `at` for the 1-based `player`, at one point of health, reveals
/// the build sight, and allocates the settlement every placed siege engine
/// anchors -- the record `GetHolderSett` on a crewman walks and whose holder
/// caps the crew. `kNoObject` when nothing was created.
[[nodiscard]] ObjectId place_catapult(World& world, ClassIndex class_index, Point at,
                                      std::int32_t player);

}  // namespace imperivm::core::sim
