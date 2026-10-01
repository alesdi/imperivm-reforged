#include "imperivm/core/sim/orders.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "imperivm/core/formats/byte_reader.hpp"
#include "imperivm/core/script/value.hpp"
#include "imperivm/core/script/vm.hpp"
#include "imperivm/core/sim/env.hpp"
#include "imperivm/core/sim/feedback.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/player_host.hpp"
#include "imperivm/core/sim/world_host.hpp"

namespace imperivm::core::sim {
namespace {

// -- little-endian output, the convention `Scheduler::serialize` uses ---------

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

constexpr std::uint32_t kSaveMagic = 0x4C455349u;  // "ISEL"
/// 2: the per-player `ever_` latch went -- `WasSelectionAssigned` turned out to
/// read an environment key and never this table -- and the `_LastSelectionTime`
/// stamps arrived. A version-1 file describes a different set of facts, so it
/// is refused rather than widened.
constexpr std::uint32_t kSaveVersion = 2;

// -- class-graph helpers -----------------------------------------------------

/// The class of `id`, or `kNoClass`.
ClassIndex class_of(const World& world, ObjectId id) noexcept {
  const WorldObject* slot = world.find(id);
  return slot == nullptr ? kNoClass : slot->class_index;
}

/// Whether `derived` is `base` or descends from it, over the graph rather than
/// over the world -- `World::class_is_a` needs an object, and a
/// `<defaultcmd target>` is compared against a class either way.
bool class_is_a(const ClassGraph& graph, ClassIndex derived, ClassIndex base) noexcept {
  if (base == kNoClass || derived == kNoClass) return false;
  for (ClassIndex current = derived; current != kNoClass;) {
    if (current == base) return true;
    const ClassIndex parent = graph.at(current).parent_index;
    if (parent == current) break;  // hostile data: a self-parent
    current = parent;
  }
  return false;
}

/// The nearest declaration of `sig` walking up from `actor`, or null.
///
/// The class graph has already merged same-sig declarations *within* a class;
/// see the overload note in the header for what that costs.
const ClassMethod* find_method(const ClassGraph& graph, ClassIndex actor, std::string_view sig) {
  for (ClassIndex current = actor; current != kNoClass;) {
    const ClassDefinition& definition = graph.at(current);
    const auto at = std::lower_bound(
        definition.methods.begin(), definition.methods.end(), sig,
        [](const ClassMethod& entry, std::string_view needle) { return entry.sig < needle; });
    if (at != definition.methods.end() && at->sig == sig) return &*at;
    const ClassIndex parent = definition.parent_index;
    if (parent == current) break;
    current = parent;
  }
  return nullptr;
}

}  // namespace

// --------------------------------------------------------------------------
// Selection
// --------------------------------------------------------------------------

bool Selection::contains(ObjectId id) const noexcept {
  return std::find(ids_.begin(), ids_.end(), id) != ids_.end();
}

std::size_t Selection::index_of(ObjectId id) const noexcept {
  const auto at = std::find(ids_.begin(), ids_.end(), id);
  return static_cast<std::size_t>(at - ids_.begin());
}

bool Selection::add(ObjectId id) {
  if (id == kNoObject || contains(id)) return false;
  ids_.push_back(id);
  return true;
}

bool Selection::remove(ObjectId id) {
  const auto at = std::find(ids_.begin(), ids_.end(), id);
  if (at == ids_.end()) return false;
  ids_.erase(at);
  return true;
}

bool Selection::toggle(ObjectId id) {
  if (remove(id)) return false;
  return add(id);
}

void Selection::assign(std::span<const ObjectId> ids) {
  ids_.clear();
  for (const ObjectId id : ids) add(id);
}

bool Selection::swap(ObjectId from, ObjectId to) {
  const auto at = std::find(ids_.begin(), ids_.end(), from);
  if (at == ids_.end()) return false;
  ids_.erase(at);
  // Deselect's helper and then Select's, which is what 0x004c84e0 calls and in
  // that order: the replacement appends. `add` refuses `kNoObject` and refuses
  // a duplicate, which is the rest of the original's behaviour here.
  (void)add(to);
  return true;
}

std::size_t Selection::prune(const World& world) {
  const std::size_t before = ids_.size();
  const auto at = std::remove_if(ids_.begin(), ids_.end(), [&](ObjectId id) {
    const WorldObject* slot = world.find(id);
    return slot == nullptr || slot->state.health <= 0;
  });
  ids_.erase(at, ids_.end());
  return before - ids_.size();
}

// --------------------------------------------------------------------------
// SelectionTable
// --------------------------------------------------------------------------

Selection& SelectionTable::player(PlayerId id) noexcept {
  if (!PlayerTable::is_valid(id)) return scratch_;
  return selections_[id];
}

const Selection& SelectionTable::player(PlayerId id) const noexcept {
  if (!PlayerTable::is_valid(id)) return scratch_;
  return selections_[id];
}

bool SelectionTable::select(PlayerId id, ObjectId object) { return player(id).add(object); }

bool SelectionTable::deselect(PlayerId id, ObjectId object) { return player(id).remove(object); }

void SelectionTable::clear(PlayerId id) noexcept { player(id).clear(); }

void SelectionTable::assign(PlayerId id, std::span<const ObjectId> objects) {
  player(id).assign(objects);
}

void SelectionTable::note_selection_changed(PlayerId id, GameTime now) {
  for (const ObjectId object : player(id).ids()) {
    const auto at = std::lower_bound(
        stamps_.begin(), stamps_.end(), object,
        [](const std::pair<ObjectId, GameTime>& entry, ObjectId needle) {
          return entry.first < needle;
        });
    if (at != stamps_.end() && at->first == object) {
      at->second = now;
    } else {
      stamps_.emplace(at, object, now);
    }
  }
}

GameTime SelectionTable::last_selected(ObjectId object) const noexcept {
  const auto at = std::lower_bound(
      stamps_.begin(), stamps_.end(), object,
      [](const std::pair<ObjectId, GameTime>& entry, ObjectId needle) {
        return entry.first < needle;
      });
  return at != stamps_.end() && at->first == object ? at->second : -1;
}

std::size_t SelectionTable::swap_object(ObjectId from, ObjectId to) {
  std::size_t changed = 0;
  for (Selection& selection : selections_) {
    if (selection.swap(from, to)) ++changed;
  }
  return changed;
}

std::size_t SelectionTable::forget(ObjectId object) {
  std::size_t changed = 0;
  for (Selection& selection : selections_) {
    if (selection.remove(object)) ++changed;
  }
  return changed;
}

std::size_t SelectionTable::prune(const World& world) {
  std::size_t dropped = 0;
  for (Selection& selection : selections_) dropped += selection.prune(world);
  return dropped;
}

void SelectionTable::serialize(std::vector<std::byte>& out) const {
  put_u32(out, kSaveMagic);
  put_u32(out, kSaveVersion);
  put_u32(out, static_cast<std::uint32_t>(kPlayerCount));
  for (std::size_t player_id = 0; player_id < kPlayerCount; ++player_id) {
    const std::span<const ObjectId> ids = selections_[player_id].ids();
    put_u32(out, static_cast<std::uint32_t>(ids.size()));
    for (const ObjectId id : ids) put_u32(out, id);
  }
  put_u32(out, static_cast<std::uint32_t>(stamps_.size()));
  for (const auto& [object, when] : stamps_) {
    put_u32(out, object);
    put_u64(out, static_cast<std::uint64_t>(when));
  }
}

Status SelectionTable::deserialize(std::span<const std::byte> bytes) {
  ByteReader reader(bytes);
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  std::uint32_t players = 0;
  if (!reader.u32(magic) || !reader.u32(version) || !reader.u32(players)) {
    return FormatError::truncated;
  }
  if (magic != kSaveMagic) return FormatError::bad_magic;
  if (version != kSaveVersion) return FormatError::unsupported;
  if (players != kPlayerCount) return FormatError::unsupported;

  std::array<Selection, kPlayerCount> loaded{};
  for (std::size_t player_id = 0; player_id < kPlayerCount; ++player_id) {
    std::uint32_t count = 0;
    if (!reader.u32(count)) return FormatError::truncated;
    // No length bound is needed and none is written: nothing is reserved from
    // `count`, ids are appended one at a time as they are read, and the read
    // that runs off the end refuses. A separate guard here would be dead code,
    // which the mutation pass over this file proved by deleting it and finding
    // no test that noticed.
    for (std::uint32_t i = 0; i < count; ++i) {
      std::uint32_t id = 0;
      if (!reader.u32(id)) return FormatError::truncated;
      loaded[player_id].add(id);
    }
  }

  std::uint32_t stamped = 0;
  if (!reader.u32(stamped)) return FormatError::truncated;
  std::vector<std::pair<ObjectId, GameTime>> stamps;
  for (std::uint32_t i = 0; i < stamped; ++i) {
    std::uint32_t object = 0;
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    if (!reader.u32(object) || !reader.u32(low) || !reader.u32(high)) {
      return FormatError::truncated;
    }
    const std::uint64_t when = (static_cast<std::uint64_t>(high) << 32) | low;
    // Ascending and unique is the invariant every read of `stamps_` relies on,
    // and a file is not a promise. Out-of-order or repeated entries are
    // refused rather than sorted: a save that has them was not written by this
    // code, and quietly repairing it would hide that.
    if (!stamps.empty() && object <= stamps.back().first) return FormatError::malformed;
    stamps.emplace_back(object, static_cast<GameTime>(when));
  }

  selections_ = std::move(loaded);
  stamps_ = std::move(stamps);
  scratch_.clear();
  return Status{};
}

// --------------------------------------------------------------------------
// selectable / commandable
// --------------------------------------------------------------------------

bool is_selectable(const World& world, ObjectId id) noexcept {
  const WorldObject* slot = world.find(id);
  if (slot == nullptr) return false;
  if (slot->state.health <= 0) return false;
  if (slot->state.is_held()) return false;
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr || slot->class_index == kNoClass) return true;
  const std::string_view flag = graph->property(slot->class_index, "non_selectable");
  return flag.empty() || flag == "0";
}

bool is_commandable(const World& world, ObjectId id, PlayerId issuer) noexcept {
  const WorldObject* slot = world.find(id);
  if (slot == nullptr || slot->state.health <= 0) return false;
  // `SetCmdEnable(false)`: the inferred reader of `[obj+0x9c]`, see
  // `ObjectFlags::commands_disabled`.
  if (slot->state.flags.commands_disabled) return false;
  if (!PlayerTable::is_valid(issuer) || !PlayerTable::is_valid(slot->state.owner)) return false;
  return world.players().has(slot->state.owner, issuer, Relation::share_control);
}

// --------------------------------------------------------------------------
// candidates
// --------------------------------------------------------------------------

std::size_t default_command_candidates(const ClassGraph& graph, ClassIndex actor_class,
                                       ClassIndex target_class, bool modifier,
                                       std::vector<OrderCandidate>& out) {
  if (actor_class == kNoClass || actor_class >= graph.size()) return 0;
  const std::size_t before = out.size();

  /// One target's accumulated list, and which class last contributed to it.
  struct Row {
    std::string_view target;
    ClassIndex named = kNoClass;  ///< `kNoClass` for `target=""`
    ClassIndex from = kNoClass;
    std::vector<DefaultCommand> cmds;
  };
  std::vector<Row> rows;  ///< keyed on the target string, in first-seen order

  // Root first, so that a derived class's block overwrites the one it inherits
  // for the same target. See the header: `RamUnit` declares `target="Unit"` as
  // (approach) alone, with no `<nodefcmdinherit/>`, and it only means anything
  // if that erases `Unit`'s (attack, stay_hidden, approach).
  const std::vector<ClassIndex> chain = graph.ancestry(actor_class);
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const ClassDefinition& definition = graph.at(*it);
    if (definition.no_defcmd_inherit) rows.clear();  // `Sentry`, `Wagon`
    for (const DefaultCommandBlock& block : definition.default_cmds) {
      const auto at = std::find_if(rows.begin(), rows.end(),
                                   [&](const Row& row) { return row.target == block.target; });
      if (at == rows.end()) {
        rows.push_back(Row{block.target,
                           block.target.empty() ? kNoClass : graph.lookup(block.target), *it,
                           block.cmds});
        continue;
      }
      if (at->from != *it) {
        // An inherited block: replaced in place, keeping the position the
        // target was first seen at.
        at->from = *it;
        at->cmds = block.cmds;
        continue;
      }
      // A second block for the same target *in the same file*. `HERO.SC.XML`
      // declares `target="Unit"` twice, eight lines apart; replacing here
      // would make the first of the two dead code.
      at->cmds.insert(at->cmds.end(), block.cmds.begin(), block.cmds.end());
    }
  }

  // Most derived matching `<defaultcmd target>` wins. `UNIT.SC.XML`'s "no
  // capture for towers, gates and walls" comment is only true if
  // `target="Tower"` shadows `target="Building"`, and a hero is only ever
  // joined rather than walked up to if `target="Hero"` shadows `target="Unit"`.
  const Row* best = nullptr;
  std::uint32_t best_depth = 0;
  for (const Row& row : rows) {
    if (target_class == kNoClass) {
      if (row.named == kNoClass && row.target.empty()) best = &row;
      continue;
    }
    if (row.named == kNoClass || !class_is_a(graph, target_class, row.named)) continue;
    const std::uint32_t depth = graph.depth(row.named);
    if (best == nullptr || depth > best_depth) {
      best = &row;
      best_depth = depth;
    }
  }
  if (best == nullptr) return 0;

  for (const DefaultCommand& cmd : best->cmds) {
    // `ctrl="1"` partitions the list rather than extending it, or `Unit`'s
    // unconditional `move` would shadow `advance` forever.
    if (cmd.ctrl != modifier) continue;
    out.push_back(OrderCandidate{cmd.name, best->from, best->named});
  }
  return out.size() - before;
}

// --------------------------------------------------------------------------
// ScriptOrderVerifier
// --------------------------------------------------------------------------

OrderVerdict ScriptOrderVerifier::verify(std::string_view vs_path, ObjectId actor,
                                         const OrderTarget& target) {
  if (scheduler_ == nullptr || context_ == nullptr) return OrderVerdict::unknown;
  std::uint32_t chunk_index = scheduler_->find_chunk(vs_path);
  // Compiled on first ask, the way a queued command's own script is. A
  // `verify=` is preloaded by nothing -- only the `[Scripts]` manifest and the
  // `idle` methods are -- so a player's first right click on an enemy used to
  // block on `attack` until some computer player's attack had happened to
  // compile the same verifier through the command system. Compiling is not
  // world state: chunks are named in a save, never numbered.
  if (chunk_index == script::kNoChunk && context_->library != nullptr) {
    chunk_index = context_->library->chunk_for(vs_path);
  }
  if (chunk_index == script::kNoChunk || chunk_index >= scheduler_->chunk_count()) {
    ++missing_;
    return OrderVerdict::unknown;
  }
  const script::Chunk& chunk = scheduler_->chunk(chunk_index);

  // `bool f(Obj this, Obj other)` for an object target, `bool f(Obj this,
  // point pt)` for a point one -- the shape follows the target, which is what
  // every shipped verifier's header comment declares.
  const std::array<script::Value, 2> args{
      script::Value::object(context_->object_type, actor),
      target.is_object() ? script::Value::object(context_->object_type, target.object)
                         : pack_point(target.point),
  };

  script::Execution execution = script::start(chunk, args, scheduler_->host());
  script::VmEnv env;
  env.registry = scheduler_->registry();
  env.host = scheduler_->host();
  env.scheduler = scheduler_;
  env.user = context_;
  env.script = script::kNoScript;
  env.now = scheduler_->now();
  env.call_trace = env.scheduler != nullptr ? env.scheduler->call_trace() : nullptr;
  env.trace_user = env.scheduler != nullptr ? env.scheduler->trace_user() : nullptr;

  const script::ExecStatus status = script::run(execution, chunk, env);
  if (status != script::ExecStatus::finished) {
    // A trap, a suspension, or a blown budget. None of the shipped verifiers
    // can suspend -- they have no `Sleep` -- so this is a trap in practice, and
    // an unimplemented host entry point is the likely cause.
    ++traps_;
    last_trap_ = execution.trap.source_name + ":" + std::to_string(execution.trap.line) + ": " +
                 execution.trap.detail;
    return OrderVerdict::unknown;
  }
  return execution.result.truthy_scalar() ? OrderVerdict::pass : OrderVerdict::fail;
}

// --------------------------------------------------------------------------
// resolution
// --------------------------------------------------------------------------

DefaultOrder resolve_default_order(const World& world, const CommandTable& commands,
                                   ObjectId actor, const OrderTarget& target, bool modifier,
                                   OrderVerifier* verifier) {
  DefaultOrder order;
  const ClassGraph* graph = world.class_graph();
  if (graph == nullptr) return order;

  const ClassIndex actor_class = class_of(world, actor);
  if (actor_class == kNoClass) return order;

  // A target that is not in the world is a click on the ground where it used
  // to be: the point half of the order is still good.
  ClassIndex target_class = kNoClass;
  if (target.is_object()) target_class = class_of(world, target.object);

  std::vector<OrderCandidate> candidates;
  default_command_candidates(*graph, actor_class, target_class, modifier, candidates);

  for (const OrderCandidate& candidate : candidates) {
    const CommandDef* def = commands.find(candidate.command);
    if (def == nullptr) {
      // The executable's own `Unknown Command: %s`. `stay_hidden` is one in the
      // shipped data: three `<defaultcmd>` blocks name it and no `<commands>`
      // file declares it.
      ++order.unknown_commands;
      continue;
    }

    const std::string_view sig = def->method.empty() ? def->name : def->method;
    const ClassMethod* method = find_method(*graph, actor_class, sig);
    if (method == nullptr) continue;  // the class binds no script for it at all

    if (method->verify.empty()) {
      ++order.unverified;
    } else {
      const OrderVerdict verdict =
          verifier == nullptr ? OrderVerdict::unknown
                              : verifier->verify(method->verify, actor, target);
      if (verdict == OrderVerdict::fail) continue;
      if (verdict == OrderVerdict::unknown) {
        // Taking a later candidate would emit a verb the original might not.
        order.status = DefaultOrderStatus::blocked;
        order.blocked_by = candidate.command;
        return order;
      }
    }

    order.status = DefaultOrderStatus::resolved;
    order.command = def->name;
    order.verb = sig;
    order.def = def;
    return order;
  }

  return order;
}

// --------------------------------------------------------------------------
// issuing
// --------------------------------------------------------------------------

std::uint32_t issue_order(World& world, ObjectId actor, const CommandDef& def,
                          const OrderTarget& target, OrderMode mode, bool user) {
  CommandSystem* system = command_system(world);
  if (system == nullptr) return 0;

  Command prototype;
  // The object wins over the point when both are present, for the reason
  // `exec_cmd_impl` gives: a targeted verb is aimed at the object and the
  // shipped sites pass a dummy point beside it.
  if (target.is_object() && world.find(target.object) != nullptr) {
    prototype.arg_kind = CommandArgKind::object;
    prototype.object = target.object;
  } else {
    prototype.arg_kind = CommandArgKind::point;
    prototype.point = target.point;
  }
  prototype.param = def.param;
  prototype.name = def.name;
  prototype.cost_gold = def.cost_gold;
  prototype.cost_food = def.cost_food;
  prototype.cost_pop = def.cost_pop;
  prototype.cost_stamina = def.cost_stamina;
  prototype.delay = def.exec_delay;
  // `Unit.GetCommanded`: set when the command came from a player action rather
  // than from a script's own decision. A click is the player action; the
  // script form is issued as nobody and says so.
  prototype.user = user;

  const std::string_view verb = def.method.empty() ? def.name : def.method;
  // A `traincommand="yes"` row is never inserted replacing: the per-object
  // issue hands the object's insert (`vtbl+0xb8`) `replace && !traincommand`
  // (0x004efbbb, the flag at `[def+0x1b4]`), whoever asked for a replace.
  if (mode == OrderMode::replace && !def.train_command) {
    return system->set_command(world, actor, verb, prototype);
  }
  return system->append_order(world, actor, verb, prototype);
}

/// The group-order spread, and it is four rules rather than one.
///
/// `0x004f4ff0` is where a right click on a selection lands -- and where
/// `ObjList::ExecCmd` lands too, so the player and the scripts share it. It does
/// not hand every unit the click point. It **subdivides the selection into
/// spatial clusters, buckets each cluster by unit class, and gives each unit its
/// own destination**, and the arrangement differs by whether the unit belongs to
/// a hero's army.
///
/// ## The clustering
///
/// `delta = max(SubdivDelta, 45 * isqrt(N - 1))` over the whole selection --
/// `DATA\CONST.INI` says `SubdivDelta = 300 ;selection subdivision for offsets`,
/// which is the file naming this very mechanism. Then, until the working set is
/// empty: take a **randomly chosen** member as the seed, and pull in every
/// remaining member that is within `delta` of it *and belongs to the same army*.
/// So a selection spread wider than `delta` moves as several groups, each about
/// its own centre, and one cluster never mixes two heroes' warriors.
///
/// The seed is drawn from the world RNG, which means **a group order advances
/// the shared stream** -- once per cluster, plus once per scattered unit below.
/// That is the original's arrangement, where the order is a replicated player
/// command; this engine has no replication layer yet, so an order issued on one
/// peer and not another would desynchronise. Recorded rather than worked around,
/// because inventing a private stream here would diverge from the original for a
/// problem this engine does not have yet.
///
/// ## The two arrangements
///
/// Within a cluster the members are bucketed by **class**, and each bucket of
/// `n` gets `r = 45 * isqrt(n - 1)`. Then, per unit:
///
///   * a unit **in a hero's army** is scattered: an independent uniform draw in
///     `[-r, r]` on each axis. This is the branch that can *restore* a shape --
///     nine warriors get +/-90, twenty-five get +/-180 -- and it is the one this
///     engine was missing;
///   * a unit **on its own** keeps its offset from its cluster's centroid, which
///     preserves the shape it is already in and cannot recover one it has lost.
///     That is the original's behaviour too, and it is worth knowing before
///     reading a line of loose units as a bug.
///
/// The result is clamped into the map. Direction of travel is not used anywhere
/// in this path, and there is no ring, grid or spiral.
///
/// **A first version of this offset every unit by its distance from the whole
/// selection's centroid.** That is the loose-unit rule with the clustering, the
/// class bucketing and the army scatter all missing, and the army scatter is the
/// half that matters: without it a group that has been flattened -- by a click
/// against the map edge, say -- stays flat for ever.
/// `SubdivDelta`'s compiled default, for a session with no `CONST.INI`. The
/// shipped file says 300 and names the mechanism in its own comment:
/// `SubdivDelta = 300 ;selection subdivision for offsets`.
constexpr std::int32_t kDefaultSubdivDelta = 300;

/// The map's inclusive high corner, in world units. Restated rather than
/// shared, as `sim/flying.cpp`, `sim/entrance.cpp` and `sim/world_host.cpp`
/// each do, and for the reason those give.
[[nodiscard]] std::int32_t map_high_corner(const World& world) noexcept {
  if (const MatchSystem* match = match_system_of(world); match != nullptr) {
    const std::int32_t size = match->rules().map_size;
    if (size > 0) return size - 1;
  }
  const std::uint32_t extent = world.height().extent_x();
  return extent > 0 ? static_cast<std::int32_t>(extent) - 1 : 0;
}

[[nodiscard]] std::int32_t subdiv_delta(World& world) noexcept {
  std::int32_t value = kDefaultSubdivDelta;
  if (const EnvSystem* env = env_of(world); env != nullptr) {
    (void)env->constant("SubdivDelta", value);
  }
  return value;
}

/// The hero whose army `unit` belongs to, or `kNoObject`.
///
/// The original tests a `uint16` on the unit that names the *formation object*,
/// which hangs off a hero; the hero is the same partition one step nearer, and
/// it is what this engine stores.
[[nodiscard]] ObjectId army_of(World& world, ObjectId unit) noexcept {
  HeroSystem* heroes = hero_system_of(world);
  if (heroes == nullptr) return kNoObject;
  const UnitRecord* record = heroes->unit(unit);
  return record == nullptr ? kNoObject : record->hero;
}

/// One cluster: its members in selection order, and their centroid.
struct OrderCluster {
  std::vector<ObjectId> members;
  Point centre;
};

/// Subdivide a selection the way `0x004f1240` does.
[[nodiscard]] std::vector<OrderCluster> cluster_selection(World& world,
                                                          std::span<const ObjectId> actors) {
  std::vector<OrderCluster> clusters;
  if (actors.empty()) return clusters;
  const auto n = static_cast<std::int32_t>(actors.size());
  const std::int64_t delta =
      std::max<std::int64_t>(subdiv_delta(world), 45 * isqrt(n - 1));

  std::vector<ObjectId> working(actors.begin(), actors.end());
  while (!working.empty()) {
    // **A random seed, not the first member.** It decides which units end up
    // together, so it is part of the answer and part of the RNG stream.
    const std::int32_t pick =
        world.rng().below(static_cast<std::int32_t>(working.size()));
    const ObjectId seed = working[static_cast<std::size_t>(pick)];
    const Point seed_at = world.resolve_position(seed);
    const ObjectId army = army_of(world, seed);

    OrderCluster cluster;
    std::vector<ObjectId> rest;
    cluster.members.push_back(seed);
    for (const ObjectId id : working) {
      if (id == seed) continue;
      const Point at = world.resolve_position(id);
      const std::int64_t dx = at.x - seed_at.x;
      const std::int64_t dy = at.y - seed_at.y;
      // Same army and near enough. A cluster never mixes two heroes' warriors,
      // whatever the distance.
      if (army_of(world, id) == army && isqrt(dx * dx + dy * dy) <= delta) {
        cluster.members.push_back(id);
      } else {
        rest.push_back(id);
      }
    }

    std::int64_t sx = 0;
    std::int64_t sy = 0;
    for (const ObjectId id : cluster.members) {
      const Point at = world.resolve_position(id);
      sx += at.x;
      sy += at.y;
    }
    const auto size = static_cast<std::int64_t>(cluster.members.size());
    cluster.centre = Point{static_cast<std::int32_t>(sx / size),
                           static_cast<std::int32_t>(sy / size)};
    clusters.push_back(std::move(cluster));
    working = std::move(rest);
  }
  return clusters;
}

OrderReport issue_default_order(World& world, const CommandTable& commands,
                                std::span<const ObjectId> actors, const OrderTarget& target,
                                OrderMode mode, bool modifier, PlayerId issuer,
                                OrderVerifier* verifier) {
  OrderReport report;
  // A targeted verb is aimed at the *object* and takes no spread: every member
  // attacking one soldier wants that soldier, not a ring around it. The test is
  // `issue_order`'s own so the two cannot disagree about which argument an
  // order carries -- and it means an order at a stale handle, which falls back
  // to its point, spreads like the point order it is.
  //
  // Because it is the same predicate, the *live* half of it cannot be observed:
  // `issue_order` never reads `aimed.point` in that case, so a fault that
  // spreads anyway survives the suite. By construction rather than for want of
  // a test -- the two branches ask one question once.
  const bool aimed_at_object = target.is_object() && world.find(target.object) != nullptr;
  const std::int32_t high = map_high_corner(world);

  for (const OrderCluster& cluster : cluster_selection(world, actors)) {
    // Bucketed by class within the cluster, in selection order. Two classes in
    // one cluster are two sub-orders, each with its own `r`.
    std::vector<ClassIndex> classes;
    for (const ObjectId id : cluster.members) {
      const WorldObject* slot = world.find(id);
      const ClassIndex which = slot == nullptr ? kNoClass : slot->class_index;
      if (std::find(classes.begin(), classes.end(), which) == classes.end()) {
        classes.push_back(which);
      }
    }

    for (const ClassIndex which : classes) {
      std::vector<ObjectId> bucket;
      for (const ObjectId id : cluster.members) {
        const WorldObject* slot = world.find(id);
        if ((slot == nullptr ? kNoClass : slot->class_index) == which) bucket.push_back(id);
      }
      const auto n = static_cast<std::int32_t>(bucket.size());
      const std::int32_t radius = 45 * static_cast<std::int32_t>(isqrt(n - 1));

      for (const ObjectId actor : bucket) {
        // The script form vouches for control; see the header. Liveness is
        // still the engine's to test, as `is_commandable`'s first half does.
        const bool allowed =
            issuer == kNoPlayer
                ? (world.find(actor) != nullptr && world.find(actor)->state.health > 0 &&
                   !world.find(actor)->state.flags.commands_disabled)
                : is_commandable(world, actor, issuer);
        if (!allowed) {
          ++report.refused;
          continue;
        }
        const DefaultOrder order =
            resolve_default_order(world, commands, actor, target, modifier, verifier);
        report.unknown_commands += order.unknown_commands;

        OrderTarget aimed = target;
        if (order.def != nullptr && order.def->offset && !aimed_at_object) {
          const Point at = world.resolve_position(actor);
          if (army_of(world, actor) != kNoObject) {
            // In a hero's army: an independent draw per axis. This is the arm
            // that can restore a shape rather than only preserve one.
            aimed.point = Point{target.point.x + world.rng().between(-radius, radius),
                                target.point.y + world.rng().between(-radius, radius)};
          } else {
            // On its own: keep the offset from the cluster's centre.
            aimed.point = Point{target.point.x + at.x - cluster.centre.x,
                                target.point.y + at.y - cluster.centre.y};
          }
          // **Only when there is a map to clamp into.** A world with no match
          // rules and no height layer declares no extent, and a high corner of
          // zero would pin every destination on the origin -- which is not a
          // clamp, it is a bug wearing one. A synthetic world is the case, and
          // it is the same reading `GetMapRect` takes of a zero extent.
          if (high > 0) {
            const auto clamp = [high](std::int32_t v) {
              if (v < 0) v = 0;
              if (v > high) v = high;
              return v;
            };
            aimed.point = Point{clamp(aimed.point.x), clamp(aimed.point.y)};
          }
        }

        switch (order.status) {
          case DefaultOrderStatus::resolved:
            if (issue_order(world, actor, *order.def, aimed, mode, issuer != kNoPlayer) != 0) {
              ++report.issued;
            } else {
              ++report.unresolved;
            }
            break;
          case DefaultOrderStatus::blocked: ++report.blocked; break;
          case DefaultOrderStatus::none: ++report.unresolved; break;
        }
      }
    }
  }
  return report;
}

OrderReport issue_default_order(World& world, const CommandTable& commands,
                                const SelectionTable& selections, const OrderTarget& target,
                                OrderMode mode, bool modifier, PlayerId issuer,
                                OrderVerifier* verifier) {
  // A copy, because issuing runs scripts' host functions and a verifier could
  // in principle mutate the selection under an iterator.
  const std::span<const ObjectId> live = selections.player(issuer).ids();
  const std::vector<ObjectId> actors(live.begin(), live.end());
  return issue_default_order(world, commands, actors, target, mode, modifier, issuer, verifier);
}

// --------------------------------------------------------------------------
// what the match loop calls
// --------------------------------------------------------------------------

const CommandTable* order_command_table(World& world) noexcept {
  const CommandSystem* system = command_system(world);
  return system == nullptr ? nullptr : &system->table();
}

DefaultOrder resolve_default_order(World& world, ObjectId actor, const OrderTarget& target,
                                   bool modifier, OrderVerifier* verifier) {
  const CommandTable* commands = order_command_table(world);
  if (commands == nullptr) return DefaultOrder{};
  return resolve_default_order(world, *commands, actor, target, modifier, verifier);
}

OrderReport issue_default_order(World& world, const SelectionTable& selections,
                                const OrderTarget& target, OrderMode mode, bool modifier,
                                PlayerId issuer, OrderVerifier* verifier) {
  const CommandTable* commands = order_command_table(world);
  if (commands == nullptr) {
    // Nothing can be issued, and saying so per actor is more useful than one
    // zeroed report: the caller sees a count that matches its selection.
    OrderReport report;
    report.refused = selections.player(issuer).size();
    return report;
  }
  return issue_default_order(world, *commands, selections, target, mode, modifier, issuer,
                             verifier);
}

// --------------------------------------------------------------------------
// the host entry points
// --------------------------------------------------------------------------
//
// Nine declared names, eight bound. Six are about the *local* selection -- see
// the file header for why the player id three of them carry is a guard and not
// an index, and for why `GetSelection/0` is the one left alone -- and the two
// `*SelectionAssigned` questions are about the numbered control groups, which
// is `ShortcutTable`'s business and not this file's.

namespace {

using script::CallContext;
using script::HostOutcome;
using script::Value;

/// The object a value names, or null.
///
/// The three type ids that mean *a world object* -- `Obj`, `Query` and
/// `Settlement`, which the language distinguishes only by an explicit downcast
/// that has already happened by the time a value gets here. The other handle
/// types (`ObjList`, `Rect`, `Squad`, `Globals`) are also `is_object()` values,
/// and their id is an index into a side table: read as an object id it names
/// whatever object happens to wear that number. `_GetSelection().Select(2)`
/// compiles and reaches this function, so the filter is load bearing rather
/// than defensive.
[[nodiscard]] const WorldObject* object_arg(const World& world, const Value& value) noexcept {
  if (!value.is_object()) return nullptr;
  const script::ObjectRef ref = value.as_object();
  if (ref.type != kTypeObj && ref.type != kTypeQuery && ref.type != kTypeSettlement) {
    return nullptr;
  }
  return world.find(ref.id);
}

/// The table and the screen it belongs to, or nulls.
struct Screen {
  SelectionTable* table = nullptr;
  PlayerId local = kNoPlayer;

  [[nodiscard]] bool ok() const noexcept { return table != nullptr && local != kNoPlayer; }
  /// Whether a script's 1-based player number names this screen.
  [[nodiscard]] bool is_mine(const Value& value) const noexcept {
    return value.is_integer() && player_from_script(value.as_integer()) == local;
  }
};

/// `/Player<n>/SelectionAssigned`, the one registry path in the binary with no
/// `/<map>/` prefix. Built here rather than composed from `EnvScope::player`
/// because that scope *is* the prefixed form, and this key is deliberately not
/// under it: it outlives the map.
[[nodiscard]] std::string selection_assigned_key(std::int32_t player) {
  return "/Player" + std::to_string(player) + "/SelectionAssigned";
}

[[nodiscard]] Screen screen_of(CallContext& ctx) noexcept {
  HostContext* host = host_context_of(ctx);
  if (host == nullptr) return Screen{};
  return Screen{host->selections, host->local_player};
}

/// `ClearSelection(player)` -- 18 sites, 9 in the packs and 9 in containers.
///
/// The packs pass `wagon.player` (the eight `CREATE_*_MULE_*` and `_BOAT_`
/// scripts, each about to select the caravan it just built) and
/// `GROUP_UNITSOUT.VS` passes the issuing player; the containers pass the
/// literal `1`, because a cutscene knows who is watching.
///
/// 0x005a8120 empties the one selection **only when the argument names the
/// local player**, and there is no complaint when it does not.
HostOutcome fn_clear_selection(CallContext& ctx) {
  const Screen screen = screen_of(ctx);
  if (!screen.ok() || ctx.count() < 1 || !screen.is_mine(ctx.arg(0))) {
    return HostOutcome::ok_void();
  }
  screen.table->clear(screen.local);
  // No stamp. The original's handler does run here, and does stamp every member
  // of the *new* selection -- which is now nobody, so the call would be a no-op
  // either way. Written down because a fault that adds it back survives the
  // suite, and a surviving fault should say why rather than look like a gap.
  return HostOutcome::ok_void();
}

/// `obj.Select(player)` -- 11 sites, and every one of them is preceded by a
/// `ClearSelection` on the same player.
///
/// Four gates, in 0x005abfc0's order: the handle, `IsAlive`, the local-player
/// comparison, and the noselect bit. The first two print into the discard sink,
/// so all four are an ordinary return here.
HostOutcome fn_select(CallContext& ctx) {
  World* world = world_of(ctx);
  const Screen screen = screen_of(ctx);
  if (world == nullptr || !screen.ok() || ctx.count() < 2) return HostOutcome::ok_void();
  const WorldObject* slot = object_arg(*world, ctx.arg(0));
  if (slot == nullptr || slot->state.health <= 0) return HostOutcome::ok_void();
  if (!screen.is_mine(ctx.arg(1))) return HostOutcome::ok_void();
  // `SetNoselectFlag`'s bit, tested inside the insert helper (0x005e7d80) and
  // not by the entry point, which is why a flagged object is dropped in silence
  // rather than reported the way a dead one is.
  if (slot->state.flags.noselect) return HostOutcome::ok_void();
  screen.table->select(screen.local, slot->id);
  screen.table->note_selection_changed(screen.local, ctx.now);
  return HostOutcome::ok_void();
}

/// `obj.Deselect()` -- 2 sites: a builder that has been given its command and a
/// hero about to be walked out of a ruin.
///
/// The no-argument form (0x005ac0e0) makes **no** local-player comparison. It
/// cannot: there is nobody to compare against. So it removes from whichever
/// selection holds the object, which with one selection is the same statement.
HostOutcome fn_deselect(CallContext& ctx) {
  World* world = world_of(ctx);
  const Screen screen = screen_of(ctx);
  if (world == nullptr || screen.table == nullptr) return HostOutcome::ok_void();
  const WorldObject* slot = object_arg(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_void();
  // Unconditional, both halves: 0x005e7e00 erases whether or not the object was
  // there and then runs the handler either way, so deselecting something that
  // was not selected still restamps what is.
  (void)screen.table->forget(slot->id);
  if (screen.local != kNoPlayer) screen.table->note_selection_changed(screen.local, ctx.now);
  return HostOutcome::ok_void();
}

/// `SwapSelectedObj(from, to)` -- 7 sites, and they are one idiom: an object
/// that is about to stop existing hands its place to the one replacing it.
/// Three druid summonings, two priests dying into a ghost, a catapult being
/// rebuilt, and the Book of Death.
///
/// Remove and append, guarded on `from` being selected; see the header. No
/// player comparison, because there is no player argument.
HostOutcome fn_swap_selected(CallContext& ctx) {
  World* world = world_of(ctx);
  const Screen screen = screen_of(ctx);
  if (world == nullptr || screen.table == nullptr || ctx.count() < 2) {
    return HostOutcome::ok_void();
  }
  const WorldObject* from = object_arg(*world, ctx.arg(0));
  if (from == nullptr) return HostOutcome::ok_void();
  const WorldObject* to = object_arg(*world, ctx.arg(1));
  if (screen.table->swap_object(from->id, to == nullptr ? kNoObject : to->id) == 0) {
    return HostOutcome::ok_void();
  }
  if (screen.local != kNoPlayer) screen.table->note_selection_changed(screen.local, ctx.now);
  return HostOutcome::ok_void();
}

/// `selu` -- the console's *selected unit*, and the one of the eight `sel*`
/// accessors a shipped map reaches for: `5_Great_Battles_Britain`'s
/// `Maps/3/Sequences/seq8.vs` polls `selu.IsValid()` and `selu.name ==
/// "Senator"` on every pass of its narration loop.
///
/// 0x006a66b0, in the in-game arm: the selection's **first** entry (0x005b8ac0
/// answers null for an empty selection and the head otherwise) when that
/// object carries `kSyncUnit`, and the invalid handle when the selection is
/// empty or its head is not a unit -- a selected building is not a unit, and
/// the second unit behind a selected building is not consulted. The editor
/// arm, taken under the loader's session flag, reads the editor's own
/// selection and is not reproduced: no script runs under that flag here.
///
/// Whose selection: the running script's screen, which is `_GetSelection`'s
/// rule two functions down and the same `screen_of`.
HostOutcome fn_selu(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("selu: no world");
  const Screen screen = screen_of(ctx);
  const auto invalid = HostOutcome::ok_with(Value::object(script::ObjectRef{script::kNoType, 0}));
  if (!screen.ok()) return invalid;
  const std::span<const ObjectId> ids = screen.table->player(screen.local).ids();
  if (ids.empty()) return invalid;
  const WorldObject* head = world->find(ids.front());
  if (head == nullptr || !head->state.flags.is_unit) return invalid;
  return HostOutcome::ok_with(Value::object(script::ObjectRef{kTypeObj, head->id}));
}

/// `_GetSelection()` -- 4 sites: two debug dumps, the catapult build verifier
/// and the caravan tutorial.
///
/// A **copy** into a pooled list, not an alias. 0x005b5690 allocates a fresh
/// `ObjList` and copies the selection's ids into it one at a time, which is
/// what the callers need -- `BUILD_CATAPULT_VERIFY.VS` runs `GetCanExecCmd`
/// and `ClearDead` over what it gets back, and neither may touch what the
/// player has selected.
HostOutcome fn_get_selection(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("_GetSelection: no world");
  ObjListPool& pool = objlist_pool_of(*world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  std::vector<ObjectId>* out = pool.mutable_items(list);
  if (out == nullptr) return HostOutcome::failed("_GetSelection: the pool refused a new list");
  const Screen screen = screen_of(ctx);
  if (screen.ok()) {
    const std::span<const ObjectId> ids = screen.table->player(screen.local).ids();
    out->assign(ids.begin(), ids.end());
  }
  return HostOutcome::ok_with(make_objlist_value(list));
}

// -- the `Sel*` family ---------------------------------------------------------
//
// `SelAvgLevel`, `SelAvgStamina`, `SelAvgFood`, `SelAvgDamage`, `SelAvgArmor`,
// `SelHealth` and `SelMaxHealth`: seven free functions registered in one block
// at 0x004cad4b, with **no call site in any `.vs` file** -- they are reached
// only from the inline `script="..."` of the `Multi*` classes' `<value0..5>`,
// which is what the info bar evaluates over a multiple selection
// (`sim/infobar.hpp`). `imcheck surface` counts them declared and never called.
//
// Every one of them (0x004c8860 and the six after it) walks the local player's
// selection and takes each object that carries `SyncFlags` bit 22 -- the unit
// bit -- and, in all but `SelAvgLevel`, each *army member* of every selected
// hero as well (`test 0x1000000`, the hero bit, then the deque at hero+0x1d4).
// The averages divide with a half added first -- `(sum + n/2) / n` -- so they
// round; an empty selection answers 0, except `SelAvgLevel`, which answers 1
// (`mov eax, 1` at 0x004c893e). The sums do not divide.
//
// The per-unit numbers are the same ones the member entry points answer --
// `level` through `vtbl+0x114`, stamina at +0xc4, damage +0xdc, armor +0xe4,
// health +0xc0, maxhealth +0xc8, food +0x16c -- so each is read here by
// dispatching to that member rather than by a second copy of its rule.

/// Dispatch `name/0` on `id` through the registry the running script sees.
[[nodiscard]] std::int64_t member_stat(CallContext& ctx, std::string_view name, ObjectId id) {
  const script::HostRegistry* registry = ctx.scheduler != nullptr ? ctx.scheduler->registry() : nullptr;
  if (registry == nullptr) return 0;
  const std::uint32_t index = registry->find(script::CallKind::member, name, 0);
  if (index == script::kUnresolvedHost) return 0;
  const script::HostEntry& entry = registry->entry(index);
  if (entry.fn == nullptr) return 0;
  Value receiver = Value::object(script::ObjectRef{kTypeObj, id});
  CallContext sub = ctx;
  sub.arguments = std::span<Value>(&receiver, 1);
  sub.name = name;
  sub.kind = script::CallKind::member;
  const HostOutcome out = entry.fn(sub);
  if (out.status != script::HostStatus::ok || !out.value.is_integer()) return 0;
  return out.value.as_integer();
}

struct SelStat {
  std::int64_t sum = 0;
  std::int64_t count = 0;
};

[[nodiscard]] SelStat sel_stat(CallContext& ctx, std::string_view name, bool armies) {
  SelStat out;
  World* world = world_of(ctx);
  const Screen screen = screen_of(ctx);
  if (world == nullptr || !screen.ok()) return out;
  const HeroSystem* heroes = hero_system_of(*world);
  for (const ObjectId id : screen.table->player(screen.local).ids()) {
    const WorldObject* slot = world->find(id);
    if (slot == nullptr) continue;
    if (slot->state.flags.is_unit) {
      out.sum += member_stat(ctx, name, id);
      ++out.count;
    }
    if (armies && slot->state.flags.is_hero && heroes != nullptr) {
      if (const HeroRecord* hero = heroes->hero(id)) {
        for (const ObjectId member : hero->army) {
          if (world->find(member) == nullptr) continue;
          out.sum += member_stat(ctx, name, member);
          ++out.count;
        }
      }
    }
  }
  return out;
}

template <bool kArmies, std::int32_t kEmpty>
HostOutcome sel_average(CallContext& ctx, std::string_view name) {
  const SelStat stat = sel_stat(ctx, name, kArmies);
  if (stat.count == 0) return HostOutcome::ok_with(Value::integer(kEmpty));
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>((stat.sum + stat.count / 2) / stat.count)));
}

HostOutcome fn_sel_avg_level(CallContext& ctx) { return sel_average<false, 1>(ctx, "level"); }
HostOutcome fn_sel_avg_stamina(CallContext& ctx) { return sel_average<true, 0>(ctx, "stamina"); }
HostOutcome fn_sel_avg_food(CallContext& ctx) { return sel_average<true, 0>(ctx, "food"); }
HostOutcome fn_sel_avg_damage(CallContext& ctx) { return sel_average<true, 0>(ctx, "damage"); }
HostOutcome fn_sel_avg_armor(CallContext& ctx) { return sel_average<true, 0>(ctx, "armor_slash"); }
HostOutcome fn_sel_health(CallContext& ctx) {
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(sel_stat(ctx, "health", true).sum)));
}
HostOutcome fn_sel_max_health(CallContext& ctx) {
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(sel_stat(ctx, "maxhealth", true).sum)));
}

/// `IsSelectionAssigned(player)` -- 1 site, and it is not about the selection.
///
/// Ten numbered control groups, and true at the first non-empty one. See the
/// header: this is the evidence for `ShortcutTable::kSlots`.
HostOutcome fn_is_selection_assigned(CallContext& ctx) {
  HostContext* host = host_context_of(ctx);
  if (host == nullptr || host->shortcuts == nullptr || ctx.count() < 1) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const PlayerId player = ctx.arg(0).is_integer() ? player_from_script(ctx.arg(0).as_integer())
                                                  : kNoPlayer;
  if (player == kNoPlayer) return HostOutcome::ok_with(Value::boolean(false));
  for (std::size_t slot = 0; slot < ShortcutTable::kSlots; ++slot) {
    if (!host->shortcuts->group(player, static_cast<std::int32_t>(slot)).empty()) {
      return HostOutcome::ok_with(Value::boolean(true));
    }
  }
  return HostOutcome::ok_with(Value::boolean(false));
}

/// `WasSelectionAssigned(player)` -- 1 site, beside the one above.
///
/// Reads `/Player<n>/SelectionAssigned` out of the environment store and asks
/// whether any bit is set. **No bounds check**: 0x004c6a00 formats the number
/// it was given straight into the key, so player 99 reads a key nothing has
/// written and answers false. Nothing in this engine sets a bit -- the writers
/// are the input layer's, and `SetShortcutSel` is not one of them -- which is
/// exactly the distinction `GENERALADVICE6.VS` is drawing.
HostOutcome fn_was_selection_assigned(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr || ctx.count() < 1 || !ctx.arg(0).is_integer()) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  const EnvSystem* env = env_of(*world);
  if (env == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  return HostOutcome::ok_with(
      Value::boolean(env->env().read_int(EnvScope::root(),
                                         selection_assigned_key(ctx.arg(0).as_integer())) != 0));
}

/// `obj._LastSelectionTime()` -- 1 site, and it is a question about a habit.
///
/// `BUILDINGSADVICE9.VS` walks the player's buildings and counts the ones that
/// answer anything but -1, which is *has this player ever clicked on one of
/// these?* An unresolvable receiver answers -1 too (0x005abd60 initialises the
/// result to -1 before it tries the handle), so there is one answer for "never"
/// and for "not a thing".
HostOutcome fn_last_selection_time(CallContext& ctx) {
  World* world = world_of(ctx);
  HostContext* host = host_context_of(ctx);
  if (world == nullptr || host == nullptr || host->selections == nullptr) {
    return HostOutcome::ok_with(Value::integer(-1));
  }
  const WorldObject* slot = object_arg(*world, ctx.arg(0));
  if (slot == nullptr) return HostOutcome::ok_with(Value::integer(-1));
  return HostOutcome::ok_with(Value::integer(host->selections->last_selected(slot->id)));
}


/// `ObjList::ExecDefaultCmd(pt, obj, bReplace, bModifier)` and the `Obj`
/// form -- **one call site, and it was the sole blocker of
/// `GROUP_UNITSOUT.VS`**, the `groupdispatch=` script of the `unitsout`
/// command whose last line specifies this whole file (see the header).
///
/// `gbr.exe` registers it twice at arity 4: `ObjList::ExecDefaultCmd`
/// (0x0055e140, `[0, 5, 23, 6, 20, 7, 7]`) and `Obj::ExecDefaultCmd`
/// (0x005b52b0, `[0, 5, 20, 6, 20, 7, 7]`) -- void, the receiver, a point, an
/// object, two bools. This engine's registry keys on `(kind, name, arity)`,
/// so one body answers both and tells the receivers apart by their value.
/// Both are the same four steps: pop the arguments, turn the object into a
/// handle word (`0xffff` when it names nothing), and call 0x004f4ff0 -- the
/// right click's own core, documented at `issue_default_order` -- with the
/// list, the point, that handle, the modifier, the replace flag, and **the
/// player -1**. The `Obj` form wraps its receiver in a one-element list first,
/// and prints `The function 'Obj::ExecDefaultCmd' called for an uninitialized
/// or invalid object.` and returns when it names nothing.
///
/// So everything below the argument shuffle is `issue_default_order` with a
/// `kNoPlayer` issuer, whose meaning that function's note records: control is
/// the script's business, liveness is tested, and the queued commands are not
/// a player action. The verifiers run on the real VM through
/// `ScriptOrderVerifier`, built on the calling script's own scheduler and host
/// context -- the same pair a click's verifier is built on -- so a
/// `verify=` script behind a default order is asked here exactly as it is
/// asked under the mouse. A call with no scheduler resolves with no verifier,
/// where every conditional candidate blocks rather than passes.
///
/// A receiver that is neither a list nor an object handle is the original's
/// invalid path: nothing happens and nothing traps. The point is required; a
/// missing object is a click on bare ground.
HostOutcome fn_exec_default_cmd(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed("ExecDefaultCmd: no world");
  if (ctx.count() < 5 || !is_point(ctx.arg(1))) {
    return HostOutcome::failed("ExecDefaultCmd: expected a point, an object and two flags");
  }
  std::vector<ObjectId> actors;
  if (is_objlist(ctx.arg(0))) {
    const std::span<const ObjectId> items = objlist_pool_of(*world).items(objlist_of(ctx.arg(0)));
    actors.assign(items.begin(), items.end());
  } else if (ctx.arg(0).is_object() && ctx.arg(0).as_object().id != kNoObject &&
             world->find(ctx.arg(0).as_object().id) != nullptr) {
    // The liveness test here is the original's invalid-object path; the issue
    // loop skips a dead id on its own, so a fault that drops it survives the
    // suite as an equivalence.

    actors.push_back(ctx.arg(0).as_object().id);
  } else {
    return HostOutcome::ok_void();
  }
  const CommandTable* table = order_command_table(*world);
  if (table == nullptr) return HostOutcome::ok_void();

  OrderTarget target;
  target.point = unpack_point(ctx.arg(1));
  if (ctx.arg(2).is_object() && ctx.arg(2).as_object().id != kNoObject) {
    target.object = ctx.arg(2).as_object().id;
  }
  const OrderMode mode = ctx.arg(3).truthy_scalar() ? OrderMode::replace : OrderMode::append;
  const bool modifier = ctx.arg(4).truthy_scalar();

  HostContext* host = host_context_of(ctx);
  if (ctx.scheduler != nullptr && host != nullptr) {
    ScriptOrderVerifier verifier(*ctx.scheduler, *host);
    (void)issue_default_order(*world, *table, actors, target, mode, modifier, kNoPlayer,
                              &verifier);
  } else {
    (void)issue_default_order(*world, *table, actors, target, mode, modifier, kNoPlayer, nullptr);
  }
  return HostOutcome::ok_void();
}

constexpr std::size_t kEntryCount = 9;

}  // namespace

std::size_t orders_host_entry_count() noexcept { return kEntryCount; }

std::size_t register_orders_host(script::HostRegistry& registry) {
  const std::size_t before = registry.implemented();
  constexpr script::CallKind kFree = script::CallKind::free_function;
  constexpr script::CallKind kMember = script::CallKind::member;
  registry.define(kFree, "ClearSelection", 1, &fn_clear_selection);             // 18
  registry.define(kFree, "SwapSelectedObj", 2, &fn_swap_selected);              //  7
  registry.define(kFree, "_GetSelection", 0, &fn_get_selection);                //  4
  registry.define(kFree, "selu", 0, &fn_selu);                                  //  2, one map
  registry.define(kFree, "IsSelectionAssigned", 1, &fn_is_selection_assigned);  //  1
  // The `Multi*` classes' info-bar scripts; no `.vs` file calls one.
  registry.define(kFree, "SelAvgLevel", 0, &fn_sel_avg_level);
  registry.define(kFree, "SelAvgStamina", 0, &fn_sel_avg_stamina);
  registry.define(kFree, "SelAvgFood", 0, &fn_sel_avg_food);
  registry.define(kFree, "SelAvgDamage", 0, &fn_sel_avg_damage);
  registry.define(kFree, "SelAvgArmor", 0, &fn_sel_avg_armor);
  registry.define(kFree, "SelHealth", 0, &fn_sel_health);
  registry.define(kFree, "SelMaxHealth", 0, &fn_sel_max_health);
  registry.define(kFree, "WasSelectionAssigned", 1, &fn_was_selection_assigned);//  1
  registry.define(kMember, "Select", 1, &fn_select);                            // 11
  registry.define(kMember, "Deselect", 0, &fn_deselect);                        //  2
  registry.define(kMember, "_LastSelectionTime", 0, &fn_last_selection_time);   //  1
  registry.define(kMember, "ExecDefaultCmd", 4, &fn_exec_default_cmd);          //  1, both receivers
  // `GetSelection/0` is declared and stays unbound: `gbr.exe` registers no such
  // name, so `DEBUG_DUMP.VS`'s one call site could never have run. The header
  // has the evidence. The two-argument `IsSelectionAssigned` and
  // `WasSelectionAssigned` are registered in the original and reached by no
  // shipped script, so they are not bound either.
  return registry.implemented() - before;
}

}  // namespace imperivm::core::sim
