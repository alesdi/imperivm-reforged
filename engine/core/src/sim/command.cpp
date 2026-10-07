// The command queue and the cross-domain orders.
// See include/imperivm/core/sim/command.hpp.

#include "imperivm/core/sim/command.hpp"

#include "imperivm/core/sim/host_context.hpp"

#include <algorithm>
#include <optional>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/match.hpp"
#include "imperivm/core/sim/hero.hpp"
#include "imperivm/core/sim/movement.hpp"
#include "imperivm/core/sim/objlist.hpp"
#include "imperivm/core/sim/world_host.hpp"
#include "imperivm/core/xml.hpp"

namespace imperivm::core::sim {
namespace {

[[nodiscard]] constexpr char fold(char c) noexcept {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool equal_fold(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (fold(a[i]) != fold(b[i])) return false;
  }
  return true;
}

/// Strict weak ordering on folded names. The sort key of every table here, so
/// that lookup is a binary search and iteration order does not depend on the
/// order files were read in.
[[nodiscard]] bool less_fold(std::string_view a, std::string_view b) noexcept {
  const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const char x = fold(a[i]);
    const char y = fold(b[i]);
    if (x != y) return x < y;
  }
  return a.size() < b.size();
}

}  // namespace

// --------------------------------------------------------------------------
// the command table
// --------------------------------------------------------------------------

std::size_t CommandTable::lower_bound(std::string_view name) const noexcept {
  std::size_t lo = 0;
  std::size_t hi = commands_.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (less_fold(commands_[mid].name, name)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

const CommandDef* CommandTable::find(std::string_view name) const noexcept {
  const std::size_t at = lower_bound(name);
  if (at < commands_.size() && equal_fold(commands_[at].name, name)) return &commands_[at];
  return nullptr;
}

void CommandTable::set(const CommandDef& def) {
  const std::size_t at = lower_bound(def.name);
  if (at < commands_.size() && equal_fold(commands_[at].name, def.name)) {
    commands_[at] = def;
    return;
  }
  commands_.insert(commands_.begin() + static_cast<std::ptrdiff_t>(at), def);
}

Status CommandTable::merge(std::span<const std::byte> xml) {
  Result<XmlDocument> parsed = XmlDocument::parse(xml);
  if (!parsed) return parsed.error();
  const XmlDocument& doc = *parsed;
  if (doc.empty()) return FormatError::malformed;

  const NodeIndex root = doc.root();
  // Refused rather than tolerated: handing this a `.SC.XML` would otherwise
  // silently produce an empty table and a mystery three systems away.
  if (doc.node(root).name != "commands") return FormatError::bad_magic;

  for (NodeIndex n = doc.child(root, "cmd"); n != kNoNode; n = doc.next(n, "cmd")) {
    const std::string_view name = doc.attribute(n, "name");
    if (name.empty()) continue;

    CommandDef def;
    def.name.assign(name);
    const std::string_view method = doc.attribute(n, "method");
    def.method.assign(method.empty() ? name : method);
    def.param.assign(doc.attribute(n, "param"));
    def.button.assign(doc.attribute(n, "button"));
    def.queue_icon.assign(doc.attribute(n, "queueicon"));
    def.key.assign(doc.attribute(n, "key"));
    {
      const std::string_view train = doc.attribute(n, "traincommand");
      def.train_command = train == "yes" || train == "1" || train == "true";
    }
    def.cost_gold = doc.attribute_int(n, "costgold", 0);
    def.cost_food = doc.attribute_int(n, "costfood", 0);
    def.cost_pop = doc.attribute_int(n, "costpop", 0);
    def.cost_stamina = doc.attribute_int(n, "coststamina", 0);
    def.exec_delay = doc.attribute_int(n, "execdelay", 0);
    def.priority = doc.attribute_int(n, "priority", 0);
    def.immediate = doc.attribute_int(n, "immediate", 0) != 0;
    def.offset = doc.attribute_int(n, "offset", 0) != 0;
    // The two display strings, which the `rollover` family formats and nothing
    // in the simulation reads. 384 of the 404 rows carry a `rollover` and 253
    // a `description`; see `sim/feedback.hpp`.
    def.rollover.assign(doc.attribute(n, "rollover"));
    def.description.assign(doc.attribute(n, "description"));
    // `<src obj="…">`: which classes offer this command. In document order,
    // which is the order `FindResearchLab` would see if it walked from this
    // side -- it walks from the building's, so the order decides nothing, and
    // it is preserved rather than sorted because a file's order is a fact.
    for (NodeIndex src = doc.child(n, "src"); src != kNoNode; src = doc.next(src, "src")) {
      const std::string_view obj = doc.attribute(src, "obj");
      if (!obj.empty()) def.sources.emplace_back(obj);
    }
    for (NodeIndex src = doc.child(n, "nsrc"); src != kNoNode; src = doc.next(src, "nsrc")) {
      const std::string_view obj = doc.attribute(src, "obj");
      if (!obj.empty()) def.excludes.emplace_back(obj);
    }
    // The interface's half of the row: what enables the button, what runs
    // when it is pressed as a group, what the cursor shows, and what it can
    // be aimed at.
    def.group_verifier.assign(doc.attribute(n, "groupverifier"));
    def.group_dispatch.assign(doc.attribute(n, "groupdispatch"));
    def.cursor.assign(doc.attribute(n, "cursor"));
    for (NodeIndex text = doc.child(n, "cmdtext"); text != kNoNode;
         text = doc.next(text, "cmdtext")) {
      def.targets.emplace_back(doc.attribute(text, "target"));
    }
    set(def);
  }
  return {};
}

// --------------------------------------------------------------------------
// one command
// --------------------------------------------------------------------------

script::Value Command::argument() const noexcept {
  switch (arg_kind) {
    case CommandArgKind::point: return pack_point(point);
    case CommandArgKind::object: return script::Value::object(kTypeObj, object);
    case CommandArgKind::none: break;
  }
  return script::Value::nil();
}

// --------------------------------------------------------------------------
// the system
// --------------------------------------------------------------------------

std::size_t CommandSystem::lower_bound(ObjectId id) const noexcept {
  std::size_t lo = 0;
  std::size_t hi = queues_.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (queues_[mid].id < id) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

CommandQueue& CommandSystem::queue(ObjectId id) {
  const std::size_t at = lower_bound(id);
  if (at < queues_.size() && queues_[at].id == id) return queues_[at].queue;
  Entry entry;
  entry.id = id;
  return queues_.insert(queues_.begin() + static_cast<std::ptrdiff_t>(at), std::move(entry))
      ->queue;
}

const CommandQueue* CommandSystem::find(ObjectId id) const noexcept {
  const std::size_t at = lower_bound(id);
  if (at < queues_.size() && queues_[at].id == id) return &queues_[at].queue;
  return nullptr;
}

CommandQueue* CommandSystem::find(ObjectId id) noexcept {
  return const_cast<CommandQueue*>(static_cast<const CommandSystem*>(this)->find(id));
}

void CommandSystem::retire(Command& command) {
  if (command.script != script::kNoScript && scheduler_ != nullptr) {
    scheduler_->kill(command.script);
  }
  command.script = script::kNoScript;
}

std::size_t CommandSystem::forget_departed(const World& world) {
  std::vector<ObjectId> gone;
  for (const Entry& entry : queues_) {
    if (world.find(entry.id) == nullptr) gone.push_back(entry.id);
  }
  for (const ObjectId id : gone) forget(id);
  return gone.size();
}

void CommandSystem::forget(ObjectId id) {
  // The disabled set goes with the queue. An object id is never reused --
  // `World::next_object_id` is monotone -- so this cannot resurrect somebody
  // else's set; it is here so that a long game does not accumulate one entry
  // per dead building that a script had ever taken a command from.
  const std::size_t disabled_at = disabled_lower_bound(id);
  if (disabled_at < disabled_.size() && disabled_[disabled_at].id == id) {
    disabled_.erase(disabled_.begin() + static_cast<std::ptrdiff_t>(disabled_at));
  }
  const std::size_t at = lower_bound(id);
  if (at >= queues_.size() || queues_[at].id != id) return;
  for (Command& command : queues_[at].queue.entries) retire(command);
  queues_.erase(queues_.begin() + static_cast<std::ptrdiff_t>(at));
}

std::size_t CommandSystem::disabled_lower_bound(ObjectId id) const noexcept {
  std::size_t lo = 0;
  std::size_t hi = disabled_.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (disabled_[mid].id < id) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

std::span<const std::string> CommandSystem::disabled_commands(ObjectId id) const noexcept {
  const std::size_t at = disabled_lower_bound(id);
  if (at >= disabled_.size() || disabled_[at].id != id) return {};
  return disabled_[at].names;
}

namespace {

/// The building family's own clause of `vtbl+0xd8`: **a ruin offers its repair
/// and nothing else, and a building that is not a ruin never offers it.**
///
/// All seven building-family vtables reach 0x004de580 -- `CVXBuilding`,
/// `CVXBarrack` and two more hold it in the slot, and the town hall's
/// (0x005d2110) and two others' overrides end by calling it -- and it asks one
/// question before handing over to the object's predicate (the flag and the
/// list, below):
///
///   * at damage tier 3 (`[obj+0x204]`, `ObjectState::damage_state`, the tier
///     `IsBroken` answers), only the row whose `method=` is the building's
///     restoring verb passes: `"repopulate"` when the class says
///     `auto_repair="yes"` (`[class+0x2ec]`, the village houses) and
///     `"repair"` otherwise. And not while that verb is already the running
///     command (`[obj+0x10c]`, the string `WaitIdle` reads), so a ruin being
///     repaired does not offer a second repair;
///   * at any other tier the row whose method is `"repair"` does not pass.
///     `"repopulate"` is not tested there: the village's own verifier asks for
///     a broken house.
///
/// Both method tests (0x004e88f0 and 0x004e8920) compare the row's `method=`
/// byte for byte, case and all, where the rest of the class graph folds; so
/// does the running-command test (0x00429c60). Every shipped repair row spells
/// its method in lower case, so the difference is not visible in the data.
///
/// This is what keeps "Ripara" off a fort at full health: the row's
/// `groupverifier`, `VERIFY_CMDCOST_BUILDING.VS`, never asks about health, and
/// `repair tavern` has no verifier at all.
bool building_offers(const CommandSystem& commands, const World& world, const WorldObject& slot,
                     const CommandDef& def) {
  if (slot.state.damage_state == 3) {
    const ClassGraph* graph = world.class_graph();
    const bool auto_repair = graph != nullptr && slot.class_index != kNoClass &&
                             graph->property(slot.class_index, "auto_repair") == "yes";
    const std::string_view verb = auto_repair ? "repopulate" : "repair";
    if (def.method != verb) return false;
    return commands.command_name(slot.id, 0) != verb;
  }
  return def.method != "repair";
}

}  // namespace

bool CommandSystem::command_enabled(const World& world, ObjectId id,
                                    std::string_view name) const noexcept {
  const WorldObject* slot = world.find(id);
  if (slot == nullptr) return false;
  // The two halves, in the original's order: the whole-object flag first, then
  // the by-name set. `[obj+0x9c]` is tested before the list is walked at all.
  if (slot->state.flags.commands_disabled) return false;
  const CommandDef* def = table_.find(name);
  if (def == nullptr) return false;
  if (slot->state.flags.is_building && !building_offers(*this, world, *slot, *def)) return false;
  for (const std::string& disabled : disabled_commands(id)) {
    if (equal_fold(disabled, def->name)) return false;
  }
  return true;
}

bool CommandSystem::disable_command(const World& world, ObjectId id, std::string_view name) {
  if (!command_enabled(world, id, name)) return false;
  // `command_enabled` has already refused a name the table does not carry, so
  // this cannot be null; the row is re-read for its canonical spelling, which
  // is what gets stored.
  const CommandDef* def = table_.find(name);
  if (def == nullptr) return false;

  const std::size_t at = disabled_lower_bound(id);
  if (at >= disabled_.size() || disabled_[at].id != id) {
    DisabledEntry entry;
    entry.id = id;
    disabled_.insert(disabled_.begin() + static_cast<std::ptrdiff_t>(at), std::move(entry));
  }
  std::vector<std::string>& names = disabled_[at].names;
  const auto where = std::lower_bound(
      names.begin(), names.end(), def->name,
      [](const std::string& lhs, const std::string& key) { return less_fold(lhs, key); });
  names.insert(where, def->name);
  return true;
}

bool CommandSystem::enable_command(ObjectId id, std::string_view name) {
  const std::size_t at = disabled_lower_bound(id);
  if (at >= disabled_.size() || disabled_[at].id != id) return false;
  std::vector<std::string>& names = disabled_[at].names;
  const std::size_t before = names.size();
  // Every copy, which is the original's own `remove`: 0x004635c0 walks the
  // whole list and unlinks each node carrying the value rather than stopping at
  // the first.
  //
  // **Not observable, and kept for what it says.** Nothing can put two copies
  // in: `disable_command` is gated on the name not already being there, and the
  // loader refuses a set that is not strictly ascending. A fault that stops at
  // the first copy survives the suite for that reason. It is the original's
  // shape and it is one line, so it stands rather than being narrowed to a
  // reading the executable does not support.
  names.erase(std::remove_if(names.begin(), names.end(),
                             [name](const std::string& held) {
                               return equal_fold(held, name);
                             }),
              names.end());
  const bool removed = names.size() != before;
  // An empty set is no set: the row goes, so that `disabled_commands` and the
  // save both see exactly the objects that have lost something.
  if (names.empty()) disabled_.erase(disabled_.begin() + static_cast<std::ptrdiff_t>(at));
  return removed;
}

const CommandSystem::ClassMethods& CommandSystem::methods_for(const ClassGraph& graph,
                                                              ClassIndex index) const {
  std::size_t lo = 0;
  std::size_t hi = method_cache_.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (method_cache_[mid].index < index) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < method_cache_.size() && method_cache_[lo].index == index) return method_cache_[lo];

  ClassMethods built;
  built.index = index;
  if (index < graph.size()) {
    // `resolved_methods` merges the ancestry with the nearest declaration
    // winning, which is what a `<method sig>` on `Unit` inherited by 134 unit
    // classes needs, and it returns them sorted by sig.
    for (const ClassMethod& method : graph.resolved_methods(index)) {
      MethodEntry entry;
      entry.sig.assign(method.sig);
      entry.script.assign(method.vs);
      entry.onfinish.assign(method.onfinish);
      built.methods.push_back(std::move(entry));
    }
    std::sort(built.methods.begin(), built.methods.end(),
              [](const MethodEntry& a, const MethodEntry& b) { return less_fold(a.sig, b.sig); });
  }
  return *method_cache_.insert(method_cache_.begin() + static_cast<std::ptrdiff_t>(lo),
                               std::move(built));
}

const CommandSystem::MethodEntry* CommandSystem::method_row(const ClassMethods& methods,
                                                            std::string_view verb) {
  std::size_t lo = 0;
  std::size_t hi = methods.methods.size();
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    if (less_fold(methods.methods[mid].sig, verb)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < methods.methods.size() && equal_fold(methods.methods[lo].sig, verb)) {
    return &methods.methods[lo];
  }
  return nullptr;
}

std::string_view CommandSystem::script_for(World& world, ObjectId id,
                                           std::string_view verb) const {
  const ClassGraph* graph = world.class_graph();
  const WorldObject* slot = world.find(id);
  if (graph == nullptr || slot == nullptr || slot->class_index == kNoClass) return {};
  const MethodEntry* row = method_row(methods_for(*graph, slot->class_index), verb);
  return row == nullptr ? std::string_view{} : std::string_view{row->script};
}

void CommandSystem::finish_command(World& world, ObjectId id, const Command& command, bool canceled) {
  // `!started` is belt and braces: both callers only pass a command that
  // ran, and the sweep labels it so.
  if (scheduler_ == nullptr || !command.started) return;
  // A command's end clears the unit's `Goto` failure stamp, so the next
  // command's give-up runs from its own first failure (`GotoOrder::give_up`).
  // Inferred: slot 9 of the seven unit vtables (0x005d2940) clears
  // `[unit+0x150]` and the walking flags before its base (0x005b5160) pops the
  // command queue's head, which reads as this moment, cancel or not.
  if (MovementSystem* movement = movement_system(world); movement != nullptr) {
    if (MoveState* move = movement->find(id); move != nullptr) {
      move->goto_failed_at = kNoGotoFailure;
    }
  }
  const ClassGraph* graph = world.class_graph();
  const WorldObject* slot = world.find(id);
  if (graph == nullptr || slot == nullptr || slot->class_index == kNoClass) return;
  const MethodEntry* row = method_row(methods_for(*graph, slot->class_index), command.verb);
  if (row == nullptr || row->onfinish.empty()) return;
  const std::uint32_t chunk = library_ != nullptr ? library_->chunk_for(row->onfinish)
                                                  : scheduler_->find_chunk(row->onfinish);
  if (chunk == script::kNoChunk) return;
  // `// void, Obj This, bool bCanceled` -- every shipped `onfinish` file.
  const script::Value args[2] = {script::Value::object(kTypeObj, id),
                                 script::Value::boolean(canceled)};
  const script::ScriptId script = scheduler_->spawn(chunk, args, script::ObjectRef{kTypeObj, id});
  if (script == script::kNoScript) return;
  Command kept = command;
  kept.script = script;
  finishing_.push_back(std::move(kept));
}

void CommandSystem::retire(World& world, ObjectId id, Command& command, bool canceled) {
  const bool started = command.started;
  retire(command);
  if (started) finish_command(world, id, command, canceled);
}

bool CommandSystem::launch(World& world, ObjectId id, Command& command, GameTime now) {
  command.started = true;
  command.started_at = now;
  if (scheduler_ == nullptr) return true;  // queued, running, never retires

  const std::string_view path = script_for(world, id, command.verb);
  if (path.empty()) return false;

  // **The file has to be compiled before it can be spawned, and nothing else
  // compiles it.** `Scheduler::spawn_by_name` is a lookup in the already-loaded
  // library and answers `kNoScript` for anything that is not there yet; the
  // only files that *are* there are `AI.INI`'s `[Scripts]` manifest and the
  // per-object `idle` methods the session compiles at start. A class `<method>`
  // like `move` is named by neither, so before this every player order, every
  // `AddCommand(true, "engage")` and every `enter` resolved to a real path,
  // failed to spawn, and was retired by `service` without a word.
  const std::uint32_t chunk = library_ != nullptr ? library_->chunk_for(path)
                                                  : scheduler_->find_chunk(path);
  if (chunk == script::kNoChunk) {
    // Counted rather than dropped: a verb whose file will not compile is a
    // broken installation or a missing behaviour, and it used to be invisible.
    if (const auto at = launch_failures_.find(command.verb); at != launch_failures_.end()) {
      ++at->second;
    } else {
      launch_failures_.emplace(std::string(command.verb), std::size_t{1});
    }
    return false;
  }

  // The receiver first, the command's argument second: exactly what the
  // shipped scripts declare. `UNIT_MOVE.VS` opens `// void, Obj This, point pt`
  // and `UNIT_ENTER.VS` `// void, Obj This, Obj bld`.
  script::Value args[2] = {script::Value::object(kTypeObj, id), command.argument()};
  const std::span<const script::Value> window(
      args, command.arg_kind == CommandArgKind::none ? std::size_t{1} : std::size_t{2});
  command.script = scheduler_->spawn(chunk, window, script::ObjectRef{kTypeObj, id});
  if (command.script == script::kNoScript) return false;
  end_resting_idle(world, id);
  return true;
}

/// **One script holds an object's command slot, and an idle only holds it while
/// nothing else does.** In `gbr.exe` the idle is a command: when the running one
/// ends, 0x005b07d0 starts the next through 0x005b4f20, and a queue with nothing
/// left gets the class's `idle` method pushed as a command (0x005aef20 finds the
/// method, 0x005b4960 queues it) whose name becomes the running command at
/// `obj+0x10c`. The desync dumps say the same from the outside: every live
/// object's queue holds an entry, and the only empty ones are the fifteen dying
/// units. An order therefore always displaces the idle; the two never run
/// together.
///
/// This engine starts an object's first `idle` straight on the scheduler, from
/// `GameSession::start_object_scripts`, outside any queue -- so nothing ended it
/// when the object was ordered, and it ran beside every command it was given.
/// The shipped unit idle halts its unit every four seconds or so (`Stop(2000)`,
/// then `Idle(1900 + rand(100))`, in a loop), so a unit walking any order
/// stopped on that rhythm and stood until the order's own `Goto` slice ran out
/// and laid the route again: playtest #12, "a walking legionary stops for about
/// a second every four seconds". It also left two idles on anything whose queue
/// later drained and refilled.
///
/// So a command the object launches ends that idle: the live script it owns
/// running its class's `idle` file that no queue entry holds, and none other.
/// Behaviours run other files and are untouched; a queued `idle` -- the one a
/// drained queue refills with, or a `SetCommand("idle")` -- is a queue entry
/// and is retired the queue's own way. **The moment is this engine's, and
/// inferred:** the object virtual that queues a command (0x005b4e90, reached
/// through the vtables and not traced to a host entry point) ends a running
/// command named `idle` in its appending branch, so the original's idle
/// plausibly goes when the order is queued. Here it goes when the order
/// starts, at the next command pass, which `GameSession::advance` runs before
/// the next script pass; only an order queued by a script partway through a
/// pass leaves the idle one more run in it.
///
/// Found by what it owns rather than recorded when it was spawned, so it needs
/// no state of its own: a save holds the scheduler's coroutines and the queues,
/// and the answer is the same after a load.
void CommandSystem::end_resting_idle(World& world, ObjectId id) {
  if (scheduler_ == nullptr) return;
  const std::string_view path = script_for(world, id, default_verb_);
  if (path.empty()) return;
  // Looked up, not compiled: an idle that is running was compiled already, and
  // one that was not has nothing to end.
  const std::uint32_t chunk = scheduler_->find_chunk(path);
  if (chunk == script::kNoChunk) return;
  const CommandQueue* q = find(id);
  const script::ObjectRef owner{kTypeObj, id};
  // Collected before any kill: `kill` only marks, but the walk stays a read.
  std::vector<script::ScriptId> resting;
  for (const script::ScriptRecord& record : scheduler_->scripts()) {
    if (record.dead || record.owner != owner || record.chunk_index != chunk) continue;
    bool queued = false;
    if (q != nullptr) {
      for (const Command& command : q->entries) queued = queued || command.script == record.id;
    }
    if (!queued) resting.push_back(record.id);
  }
  for (const script::ScriptId script : resting) (void)scheduler_->kill(script);
}

void CommandSystem::service(World& world, Entry& entry, GameTime now) {
  std::vector<Command>& entries = entry.queue.entries;
  // Bounded by construction: nothing enqueues during a turn, and the refill
  // below only fires for a verb the class actually binds, so at most one
  // command is added and the loop shortens on every other pass.
  bool refilled = false;
  while (true) {
    if (entries.empty()) {
      if (refilled || default_verb_.empty() || scheduler_ == nullptr) return;
      if (script_for(world, entry.id, default_verb_).empty()) return;
      Command fallback;
      fallback.id = world.next_command_id();
      fallback.verb = default_verb_;
      entries.push_back(std::move(fallback));
      refilled = true;
      continue;
    }

    Command& head = entries.front();
    if (!head.started) {
      if (launch(world, entry.id, head, now)) return;
      // The class binds no script for this verb. Retiring is the only safe
      // answer: leaving it at the head would block the queue forever, and
      // retrying it every turn would do the same more slowly.
      entries.erase(entries.begin());
      continue;
    }
    if (scheduler_ == nullptr) return;  // running, by definition, forever
    if (head.script != script::kNoScript && scheduler_->alive(head.script)) return;
    // The script returned (or trapped, or was killed). The command is over,
    // and its `onfinish`, if the class binds one, runs with `bCanceled` false.
    head.script = script::kNoScript;
    const Command over = std::move(head);
    entries.erase(entries.begin());
    finish_command(world, entry.id, over, /*canceled=*/false);
  }
}

void CommandSystem::advance(World& world, const Turn& turn) {
  // The `onfinish` scripts that have run their course, before anything else
  // spawns one: a finished one is a dead script id, and `command_of_script`
  // must not answer for it.
  if (scheduler_ != nullptr) {
    finishing_.erase(std::remove_if(finishing_.begin(), finishing_.end(),
                                    [&](const Command& c) {
                                      return c.script == script::kNoScript ||
                                             !scheduler_->alive(c.script);
                                    }),
                     finishing_.end());
  }
  // Ascending id, which is spawn order. `service` never inserts a queue, so the
  // vector cannot move under this walk.
  for (Entry& entry : queues_) service(world, entry, turn.time);
}

namespace {

/// The settlement that pays for what `id` does, or null: the object's own
/// back-link first, then the store's search over the triple and the anchor.
[[nodiscard]] Settlement* paying_settlement(World& world, ObjectId id) {
  EconomySystem* economy = economy_of(world);
  const WorldObject* slot = world.find(id);
  if (economy == nullptr || slot == nullptr) return nullptr;
  if (slot->settlement != kNoObject) {
    if (Settlement* s = economy->settlements().for_object(slot->settlement); s != nullptr) {
      return s;
    }
  }
  return economy->settlements().for_object(id);
}

/// Take a command's cost out of its settlement, or put it back.
///
/// **Charged when the command is queued, and put back when it is cancelled.**
/// The ASUS tick-2 dump is the evidence for the first half: 200 ms into the
/// match, five commands stand queued on five buildings -- `hireheroM` and
/// `Barrack Level 1` on one town hall's arena and barracks, `Roman Scout`
/// and `MGladiator Shows` on another's, `Free Wine` on a third's -- and each
/// town's gold is its 10,014 less exactly the queued rows' `costgold`:
/// 8,914, 7,414, 8,414. Nothing in a method script pays (`BARRACK_TRAIN.VS`
/// places the unit and sets its level; `RESEARCH.VS` writes the ledger), and
/// `SpentGoldOnArmy` is a counter, so the engine's command core is what
/// charges, and it does so before the command has run. The refund on cancel
/// is the reading the rest of the genre takes and this engine used to read
/// nothing at all: the cost was never taken, so every computer player's
/// gold only ever rose and every "can afford" test in the AI answered yes.
/// Population goes the same way, `cost_pop` off the settlement's count, which
/// `VERIFY_CMDCOST_BUILDING.VS` guards with `cmdcost_pop + MinPopulation >
/// population`.
///
/// The charge is only ever taken through `charge` below, which refuses what
/// the settlement cannot pay, so a refund always puts back exactly what was
/// taken.
void settle(World& world, ObjectId id, const Command& command, bool refund) {
  if (command.cost_gold <= 0 && command.cost_food <= 0 && command.cost_pop <= 0) return;
  Settlement* s = paying_settlement(world, id);
  if (s == nullptr) return;
  // The owner's *gold spent* and *food spent* move with the charge, and back
  // with the refund: 0x004df1a7 / 0x004df1d2 add them where the payment is
  // taken and 0x004df49f / 0x004df4fd subtract them where it is put back.
  MatchSystem* match = match_system_of(world);
  const std::int32_t sign = refund ? -1 : 1;
  if (match != nullptr) {
    if (command.cost_gold > 0) match->record_gold_spent(s->owner, sign * command.cost_gold);
    if (command.cost_food > 0) match->record_food_spent(s->owner, sign * command.cost_food);
  }
  if (refund) {
    if (command.cost_gold > 0) (void)s->warehouse.store(Resource::gold, command.cost_gold);
    if (command.cost_food > 0) (void)s->warehouse.store(Resource::food, command.cost_food);
    if (command.cost_pop > 0) s->population += command.cost_pop;
    return;
  }
  if (command.cost_gold > 0) (void)s->warehouse.take(Resource::gold, command.cost_gold);
  if (command.cost_food > 0) (void)s->warehouse.take(Resource::food, command.cost_food);
  if (command.cost_pop > 0) s->population = std::max(0, s->population - command.cost_pop);
}

/// Pay for a command about to be queued, or refuse it. False is a refusal, and
/// then nothing is taken and the command must not be queued.
///
/// **Every insert asks this first, on every peer.** The order insert
/// (`vtbl+0xb8`, 0x005b4e90), `AddCommand`'s (`vtbl+0xbc`, 0x005b5c30) and the
/// per-object issue's immediate path (0x004ef921) all open with the accept
/// test (`vtbl+0x8c`, 0x005b1760), which for a row with a `costgold`,
/// `costfood` or `costpop` calls the object's payment (`vtbl+0x84`); a false
/// answer returns before anything is inserted, and the per-object issue
/// deletes the refused command (0x004efbe2). On a building the payment is
/// 0x004df070: it refuses when the settlement's gold is below `costgold`, its
/// food below `costfood`, or -- for a row with a population cost --
/// its population below `costpop + MinPopulation`, tested in that order and
/// all or nothing, and only then takes the three and moves the spent counters.
/// The refusal shows its owner a "gold lack", "food lack" or "pop lack"
/// message, which is not modelled. The bar posts the press without asking
/// (0x005e39f0), so a training the town cannot afford is turned away where the
/// order executes, and a Ctrl press's `TrainMultipleCount` repeats, each a
/// whole execution (0x004e5e60), queue as many as there is money for.
///
/// **Two readings, labelled.** Anything that is not a building pays nothing in
/// the original (its `vtbl+0x84` accepts and takes nothing, 0x00467a30); no
/// shipped row with a cost is offered to one, and here an object pays through
/// whatever settlement it resolves to, as before. And an object that resolves
/// to no settlement is accepted for nothing, as before: the original's
/// building always has one.
[[nodiscard]] bool charge(World& world, ObjectId id, const Command& command) {
  if (command.cost_gold <= 0 && command.cost_food <= 0 && command.cost_pop <= 0) return true;
  const Settlement* s = paying_settlement(world, id);
  if (s == nullptr) return true;
  if (s->warehouse.gold < command.cost_gold) return false;
  if (s->warehouse.food < command.cost_food) return false;
  if (command.cost_pop > 0) {
    const EconomySystem* economy = economy_of(world);
    const std::int32_t floor = economy == nullptr ? 0 : economy->rules().min_population;
    if (s->population < command.cost_pop + floor) return false;
  }
  settle(world, id, command, /*refund=*/false);
  return true;
}

}  // namespace

std::uint32_t CommandSystem::set_command(World& world, ObjectId id, std::string_view verb,
                                         const Command& prototype) {
  if (world.find(id) == nullptr) return 0;
  CommandQueue& q = queue(id);

  // The command takes its id when it is made, before the insert can refuse it
  // (0x00599302 in the constructor), so a refused one has used its id.
  Command command = prototype;
  command.id = world.next_command_id();
  command.verb.assign(verb);
  command.script = script::kNoScript;
  command.started = false;
  const std::uint32_t issued = command.id;
  // Paid for before the queue it replaces is refunded (0x005b4e90 asks the
  // accept test first), so a replace cannot spend what the commands it
  // clears are holding, and a refused one leaves them running.
  if (!charge(world, id, command)) return 0;

  // Aborts what is running, which `AI HELPERS\GUARD.VS` requires: it issues
  // `SetCommand("move", pt)` to a unit whose running command is `idle`, and
  // `UNIT_IDLE.VS` is a `while(1)` that never returns on its own. A running
  // command that is replaced is a cancelled one: its `onfinish` gets `true`
  // and its cost comes back.
  for (Command& old : q.entries) {
    settle(world, id, old, /*refund=*/true);
    retire(world, id, old, /*canceled=*/true);
  }
  q.entries.clear();
  q.entries.push_back(std::move(command));
  return issued;
}

std::uint32_t CommandSystem::add_command(World& world, ObjectId id, bool front,
                                         std::string_view verb, const Command& prototype) {
  if (world.find(id) == nullptr) return 0;
  CommandQueue& q = queue(id);

  Command command = prototype;
  command.id = world.next_command_id();
  command.verb.assign(verb);
  command.script = script::kNoScript;
  command.started = false;
  const std::uint32_t issued = command.id;
  if (!charge(world, id, command)) return 0;

  // `front` inserts at index 1 -- behind the running command, ahead of the rest
  // -- and never at index 0. See the header: the `AddCommand(true, ...);
  // KillCommand();` idiom only works if the runner survives the insert.
  const std::size_t at = front && !q.entries.empty() ? std::size_t{1} : q.entries.size();
  q.entries.insert(q.entries.begin() + static_cast<std::ptrdiff_t>(at), std::move(command));
  return issued;
}

std::uint32_t CommandSystem::append_order(World& world, ObjectId id, std::string_view verb,
                                          const Command& prototype) {
  // A resting head gives way; anything else keeps running and the order waits
  // its turn behind it, as `ExecCmd`'s five-deep training queue needs. See the
  // header for why this is here and not in `add_command`. The order is paid for
  // and pushed first and the head ended after, as 0x005b4e90 does, so an order
  // the settlement cannot pay for leaves the resting head alone.
  const CommandQueue* before = find(id);
  const bool resting = before != nullptr && !before->entries.empty() &&
                       equal_fold(before->entries.front().verb, default_verb_);
  const std::uint32_t issued = add_command(world, id, /*front=*/false, verb, prototype);
  if (issued == 0 || !resting) return issued;
  CommandQueue& q = queue(id);
  settle(world, id, q.entries.front(), /*refund=*/true);
  retire(world, id, q.entries.front(), /*canceled=*/true);
  q.entries.erase(q.entries.begin());
  return issued;
}

bool CommandSystem::kill_command(World& world, ObjectId id) {
  CommandQueue* q = find(id);
  if (q == nullptr || q->entries.empty()) return false;
  settle(world, id, q->entries.front(), /*refund=*/true);
  retire(world, id, q->entries.front(), /*canceled=*/true);
  q->entries.erase(q->entries.begin());
  return true;
}

std::size_t CommandSystem::clear_commands(World& world, ObjectId id) {
  CommandQueue* q = find(id);
  if (q == nullptr || q->entries.size() <= 1) return 0;
  const std::size_t dropped = q->entries.size() - 1;
  for (std::size_t i = 1; i < q->entries.size(); ++i) {
    settle(world, id, q->entries[i], /*refund=*/true);
    retire(q->entries[i]);
  }
  q->entries.erase(q->entries.begin() + 1, q->entries.end());
  return dropped;
}

bool CommandSystem::cancel_command(World& world, ObjectId id, std::uint32_t command_id) {
  CommandQueue* q = find(id);
  if (q == nullptr || command_id == 0) return false;
  const auto at = std::find_if(q->entries.begin(), q->entries.end(),
                               [command_id](const Command& c) { return c.id == command_id; });
  if (at == q->entries.end()) return false;
  if (at == q->entries.begin()) return kill_command(world, id);
  settle(world, id, *at, /*refund=*/true);
  retire(*at);
  q->entries.erase(at);
  return true;
}

std::string_view CommandSystem::command_name(ObjectId id, std::size_t index) const noexcept {
  const CommandQueue* q = find(id);
  if (q == nullptr || index >= q->entries.size()) return {};
  return q->entries[index].verb;
}

std::size_t CommandSystem::command_count(ObjectId id) const noexcept {
  const CommandQueue* q = find(id);
  return q == nullptr ? 0 : q->entries.size();
}

std::size_t CommandSystem::command_count(ObjectId id, std::string_view verb) const noexcept {
  const CommandQueue* q = find(id);
  if (q == nullptr) return 0;
  std::size_t n = 0;
  for (const Command& command : q->entries) {
    if (equal_fold(command.verb, verb)) ++n;
  }
  return n;
}

const Command* CommandSystem::command_of_script(script::ScriptId script) const noexcept {
  if (script == script::kNoScript) return nullptr;
  for (const Entry& entry : queues_) {
    for (const Command& command : entry.queue.entries) {
      if (command.script == script) return &command;
    }
  }
  for (const Command& command : finishing_) {
    if (command.script == script) return &command;
  }
  return nullptr;
}

CommandSystem* command_system(World& world) noexcept {
  for (System* system : world.systems()) {
    if (system != nullptr && system->name() == "command") {
      return static_cast<CommandSystem*>(system);
    }
  }
  return nullptr;
}

const CommandSystem* command_system(const World& world) noexcept {
  for (const System* system : world.systems()) {
    if (system != nullptr && system->name() == "command") {
      return static_cast<const CommandSystem*>(system);
    }
  }
  return nullptr;
}

// --------------------------------------------------------------------------
// the host slice
// --------------------------------------------------------------------------

namespace {

using script::CallContext;
using script::CallKind;
using script::HostOutcome;
using script::HostRegistry;
using script::Value;

constexpr const char* kNoWorld = "command: no World behind CallContext::user";
constexpr const char* kNoSystem = "command: no command system registered with the world";
constexpr const char* kNoReceiver = "command: receiver is not an object or an ObjList";
constexpr const char* kNoVerb = "command: argument is not a command name";
constexpr const char* kNoMovement = "command: no movement system registered with the world";
constexpr const char* kNoHeroes = "command: no hero system registered with the world";
constexpr const char* kNoPoint = "command: argument is not a point or an object";

[[nodiscard]] ObjectId object_of(const Value& value) noexcept {
  if (!value.is_object() || value.as_object().type != kTypeObj) return kNoObject;
  return value.as_object().id;
}

/// The receiver of a queue-mutating call, which the corpus writes both as a
/// single object and as an `ObjList`: `sq.Units.SetCommand("advance", pt)`,
/// `hero.army.SetCommand("idle")`, `ol.AddCommand(true, "advanceenter", tgt)`.
/// The list form applies to every member, in list order.
struct Receivers {
  World* world = nullptr;
  CommandSystem* commands = nullptr;
  /// Copied out of the pool rather than viewed: issuing a command can touch the
  /// pool, and a span into a vector that moves is a use-after-free waiting for
  /// a bigger list.
  std::vector<ObjectId> ids;
  const char* error = nullptr;

  [[nodiscard]] bool ok() const noexcept { return error == nullptr; }
};

[[nodiscard]] Receivers receivers_of(CallContext& ctx, bool accept_query = true) {
  Receivers out;
  out.world = world_of(ctx);
  if (out.world == nullptr) {
    out.error = kNoWorld;
    return out;
  }
  out.commands = command_system(*out.world);
  if (out.commands == nullptr) {
    out.error = kNoSystem;
    return out;
  }
  if (ctx.count() == 0) {
    out.error = kNoReceiver;
    return out;
  }
  const Value& receiver = ctx.arg(0);
  // One expander, in `sim/world_host.hpp`, and the reason it lives there
  // rather than here is written above its declaration: this function used to
  // take `Obj` and `ObjList` only, while `membership_receivers` eight files
  // away took all four types, and the disagreement made 545 shipped command
  // sites unreachable. A bare `<group>` name resolves to a `Query`.
  if (!is_receiver(*out.world, receiver, accept_query)) {
    // A handle that names nothing any more is an *empty* receiver set, not a
    // type error: the original's command members shrug at it and return 0
    // (`receiver_departed`). Integers, strings, points, and a query handed
    // to the one entry point that refuses queries still refuse by name.
    if (receiver_departed(*out.world, receiver)) return out;
    out.error = kNoReceiver;
    return out;
  }
  out.ids = receiver_objects(*out.world, receiver, accept_query);
  return out;
}

/// The single-object form: everything that reads rather than orders.
struct Self {
  World* world = nullptr;
  CommandSystem* commands = nullptr;
  ObjectId id = kNoObject;
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
  self.commands = command_system(*self.world);
  if (self.commands == nullptr) {
    self.error = kNoSystem;
    return self;
  }
  if (ctx.count() == 0 || (self.id = object_of(ctx.arg(0))) == kNoObject) {
    self.error = kNoReceiver;
  }
  return self;
}

[[nodiscard]] GameTime now_of(CallContext& ctx, World& world) {
  if (ctx.scheduler != nullptr) return ctx.scheduler->now();
  return world.time();
}

/// Turn a call argument into the argument a queued command carries.
[[nodiscard]] Command prototype_from(const Value& value) {
  Command command;
  if (is_point(value)) {
    command.arg_kind = CommandArgKind::point;
    command.point = unpack_point(value);
  } else {
    const ObjectId id = object_of(value);
    if (id != kNoObject) {
      command.arg_kind = CommandArgKind::object;
      command.object = id;
    }
  }
  return command;
}

// -- the queue -------------------------------------------------------------

/// `AddCommand(front, verb[, arg])`. 215 sites, the most used order in the
/// corpus.
HostOutcome add_command_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  if (ctx.count() < 3 || !ctx.arg(2).is_string()) return HostOutcome::failed(kNoVerb);
  const bool front = ctx.arg(1).truthy_scalar();
  const std::string& verb = ctx.arg(2).as_string();
  const Command prototype =
      ctx.count() > 3 ? prototype_from(ctx.arg(3)) : Command{};
  for (const ObjectId id : targets.ids) {
    targets.commands->add_command(*targets.world, id, front, verb, prototype);
  }
  return HostOutcome::ok_void();
}

/// `Obj::SneakCommand(verb)` -- 6 sites, all `TEUTON_MUTATE_GD.VS`, the
/// `groupdispatch=` of the Teuton Master's convert: `u.SneakCommand("convert")`
/// on every unit of the selection once the research is in.
///
/// `0x005b0930` pops the string, resolves the receiver (an invalid one prints
/// `Obj::SneakCommand called for invalid object` and returns), and does
/// nothing at all while the session flag at 0x00a8734c is raised -- the loader
/// raises it (0x006d890c) and lowers it (0x006e9bd9), and no script runs
/// under it here -- or while the object's queue is empty (`[obj+0x98]`).
/// Otherwise it looks the verb up among the class's own methods (0x005aef20
/// with kind 0; a miss prints `SneakCommand: This object does not have a '%s'
/// command! Command not added.` and returns), and then: clones the running
/// command (its `vtbl+0x14`) and pushes the clone to the front (`vtbl+0xbc`
/// with 1), pushes the new command to the front the same way, and ends the
/// command at index 0 (0x005b07d0 with 0) -- the one that was running. So
/// `[run, rest...]` becomes `[verb, run', rest...]`: *do this now, then pick
/// up where you were*, which is what lets `convert` interrupt a march and the
/// march resume once the warrior is a peasant.
///
/// Here: `script_for` is the method lookup; the clone is a copy of the running
/// `Command` less its id and coroutine, which `add_command` reissues anyway;
/// `add_command(front)` twice is the two front pushes -- each lands at index 1,
/// behind the runner, so the second goes ahead of the first -- and
/// `kill_command` ends the runner. An empty queue is left empty, as the
/// original leaves it.
HostOutcome sneak_command_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  if (ctx.count() < 2 || !ctx.arg(1).is_string()) return HostOutcome::failed(kNoVerb);
  const std::string& verb = ctx.arg(1).as_string();
  for (const ObjectId id : targets.ids) {
    const CommandQueue* q = targets.commands->find(id);
    if (q == nullptr || q->entries.empty()) continue;
    if (targets.commands->script_for(*targets.world, id, verb).empty()) continue;
    const Command clone = q->entries.front();
    const std::string running = clone.verb;
    (void)targets.commands->add_command(*targets.world, id, /*front=*/true, running, clone);
    (void)targets.commands->add_command(*targets.world, id, /*front=*/true, verb, Command{});
    (void)targets.commands->kill_command(*targets.world, id);
  }
  return HostOutcome::ok_void();
}

/// `SetCommand(verb[, arg])`. Replaces the queue outright, running command
/// included.
HostOutcome set_command_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  if (ctx.count() < 2 || !ctx.arg(1).is_string()) return HostOutcome::failed(kNoVerb);
  const std::string& verb = ctx.arg(1).as_string();
  const Command prototype = ctx.count() > 2 ? prototype_from(ctx.arg(2)) : Command{};
  for (const ObjectId id : targets.ids) {
    targets.commands->set_command(*targets.world, id, verb, prototype);
  }
  return HostOutcome::ok_void();
}

/// `ObjList.SetCommandOffset(verb, pt)`: the group-move spread.
///
/// **Inferred.** Six sites, all on an `ObjList`, all with a point, and all
/// paired with a single-unit `AddCommand` to the same point --
/// `ol.SetCommandOffset("move", ptCenter); .AddCommand(true, "move", ptCenter);`
/// in `WOLF_ENGAGE.VS`. Read as `SetCommand` where each member keeps its
/// current offset from the group's centre, so a pack that scatters arrives
/// scattered rather than stacked on one cell. The `<cmd ... offset="1">`
/// attribute on exactly the verbs it is used with (`move`, `advance`) is what
/// suggested the reading; nothing proves the offset is measured from the
/// centroid rather than from the caller.
/// The spread both offset entry points give their members.
///
/// `verb_at` is the argument index of the verb; the point follows it.
/// `front` is `std::nullopt` for the `Set` form, which replaces the queue, and
/// carries the flag for the `Add` form, which pushes onto it.
HostOutcome command_offset(CallContext& ctx, std::size_t verb_at,
                           std::optional<bool> front) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  if (ctx.count() < verb_at + 2 || !ctx.arg(verb_at).is_string()) {
    return HostOutcome::failed(kNoVerb);
  }
  if (!is_point(ctx.arg(verb_at + 1))) return HostOutcome::failed(kNoPoint);
  const std::string& verb = ctx.arg(verb_at).as_string();
  const Point anchor = unpack_point(ctx.arg(verb_at + 1));
  World& world = *targets.world;

  if (targets.ids.empty()) return HostOutcome::ok_void();
  std::int64_t sx = 0;
  std::int64_t sy = 0;
  for (const ObjectId id : targets.ids) {
    const Point at = world.resolve_position(id);
    sx += at.x;
    sy += at.y;
  }
  const Point centre{static_cast<std::int32_t>(sx / static_cast<std::int64_t>(targets.ids.size())),
                     static_cast<std::int32_t>(sy / static_cast<std::int64_t>(targets.ids.size()))};
  for (const ObjectId id : targets.ids) {
    const Point at = world.resolve_position(id);
    Command prototype;
    prototype.arg_kind = CommandArgKind::point;
    prototype.point = Point{anchor.x + at.x - centre.x, anchor.y + at.y - centre.y};
    if (front.has_value()) {
      targets.commands->add_command(world, id, *front, verb, prototype);
    } else {
      targets.commands->set_command(world, id, verb, prototype);
    }
  }
  return HostOutcome::ok_void();
}

HostOutcome set_command_offset_impl(CallContext& ctx) {
  return command_offset(ctx, 1, std::nullopt);
}

/// `ObjList.AddCommandOffset(front, verb, pt)` -- the queueing form, and the
/// only difference is the queueing.
///
/// Two sites, both in `6_Great_loses_Boudicca` map 1 sequence 16, and both
/// immediately after a `SetCommandOffset` to a different point: the sequence
/// lays out a three-leg advance for a group of Roman besiegers, one leg per
/// call. That is the shape `SetCommand`/`AddCommand` already have, and
/// `gbr.exe` registers the pair the same way -- 0x00562280 and 0x00562520 on
/// `ObjList`, 0x00578280 and 0x00578460 on `Query`, differing in the two calls
/// to 0x005b07d0 that only the `Set` form makes, which is the clear.
///
/// **It inherits the `Set` form's inference and its caveat.** The spread is
/// measured from the group's centroid, which nothing proves; see above. Both
/// call `front` with `true` at every shipped site, so the back-insert is
/// transcribed from the registration's `bool` rather than exercised.
HostOutcome add_command_offset_impl(CallContext& ctx) {
  return command_offset(ctx, 2, ctx.count() > 1 && ctx.arg(1).truthy_scalar());
}

/// `KillCommand()`: end the running command.
///
/// **Not "kill the object".** `ESH_FOODTRADE.VS` does
/// `wagon.AddCommand(false, "unload", hall); wagon.KillCommand();` -- killing
/// the mule there would destroy the gold it was just loaded with -- and
/// `AI HELPERS\GUARD.VS` pairs it with `AddCommand(true, ...)` as the explicit
/// two-step form of what `SetCommand` does in one.
HostOutcome kill_command_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  for (const ObjectId id : targets.ids) {
    targets.commands->kill_command(*targets.world, id);
  }
  return HostOutcome::ok_void();
}

/// `ClearCommands()`: drop the pending tail, leave the runner.
HostOutcome clear_commands_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  for (const ObjectId id : targets.ids) targets.commands->clear_commands(*targets.world, id);
  return HostOutcome::ok_void();
}

/// `command()` / `command(i)`: the name of the command at that queue index.
///
/// **An object with nothing queued answers the default verb, `idle`, at index
/// 0 -- a reading.** The original keeps the running command's name as a string
/// on the object (`obj+0x10c`, which `WaitIdle` compares against `"idle"` at
/// 0x005ece5a), and an object that has never been ordered is running its
/// `idle` there. This engine starts that first `idle` straight on the scheduler
/// (`GameSession::start_object_scripts`), outside any queue, so the queue is
/// empty and the name used to come back `""`. `ES_STRONGHOLD.VS` asks
/// `barrack.command == "idle"` before it trains anything, so a barracks nobody
/// had ordered never trained. `CmdCount` is left at 0: it counts the queue, and
/// `barrack.CmdCount < 5` reads the same either way.
HostOutcome command_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  const std::int32_t index =
      ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  if (index < 0) return HostOutcome::ok_with(Value::string(std::string()));
  if (index == 0 && self.commands->command_count(self.id) == 0) {
    return HostOutcome::ok_with(Value::string(std::string(self.commands->default_verb())));
  }
  return HostOutcome::ok_with(Value::string(
      std::string(self.commands->command_name(self.id, static_cast<std::size_t>(index)))));
}

/// `CmdCount()` / `CmdCount(verb)`.
HostOutcome cmd_count_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  const std::size_t n = ctx.count() > 1 && ctx.arg(1).is_string()
                            ? self.commands->command_count(self.id, ctx.arg(1).as_string())
                            : self.commands->command_count(self.id);
  return HostOutcome::ok_with(Value::integer(static_cast<std::int32_t>(n)));
}

/// `.cmddelay`: the running command's `<cmd execdelay>`.
HostOutcome cmd_delay_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  const CommandQueue* q = self.commands->find(self.id);
  const Command* running = q == nullptr ? nullptr : q->running();
  return HostOutcome::ok_with(Value::integer(running == nullptr ? 0 : running->delay));
}

/// `.Progress()` and `.Progress(ms)` -- 23 sites in 23 scripts, and the sole
/// blocker of fourteen of them.
///
/// **A suspend, and the shipped scripts are written around it being one.**
/// `Obj::Progress` is registered *suspending* twice over (0x005ae890 and
/// 0x005ac530), and both bodies read the interpreter's first-call byte: on the
/// first call they stamp the object's progress bar and **return status 1**,
/// which suspends the coroutine, and on the resume they clear the bar and
/// return 0. So `BUILDING_REPAIR.VS`'s three lines -- `while (.health <
/// .maxhealth) { .Progress(); .Heal(...); }` -- are a loop that ticks once per
/// bar rather than a busy spin.
///
/// `HostStatus::suspend` here resumes the script *after* the call rather than
/// re-entering it, so there is no second pass and none is wanted: the whole of
/// the original's resume pass is clearing a bar this engine does not draw.
/// `HostStatus::retry`, which does re-enter, is the wrong primitive -- it is
/// for a predicate that has to be re-tested, and this is a timer.
///
/// The two forms differ in **where the duration comes from and in one guard**:
///
///   * `Progress()` walks the object's command deque to the running command,
///     reads its definition's `execdelay` -- the same number `.cmddelay`
///     reports -- and, **when that is zero, does not suspend at all**
///     (0x005ae952). A command with no delay runs its script straight through.
///   * `Progress(ms)` takes the argument and suspends unconditionally, zero
///     included. `BARRACK_TRAIN.VS` writes `.Progress((.cmddelay * perc) / 100)`
///     -- a partial bar for a partly-paid recruit -- which is why the argument
///     form exists at all.
///
/// **The bar's endpoints are display and are not stored.** The original writes
/// `[obj+0x128] = now` and `[obj+0x12c] = now + duration`, and nothing in the
/// registered surface reads either back; a field with no reader is not state
/// here (`ObjectState::user`'s note has the rule and the one exception). The
/// original also routes the duration through a global at `[0x00a77eac]`, so a
/// `Progress()` whose command lookup fails suspends for whatever the *previous*
/// call left there. That is a stale global rather than a rule, and it is not
/// reproduced: a lookup that fails here reads zero and does not suspend.
///
/// A receiver that does not resolve prints *"called for an uninitialized or
/// invalid object"* and returns without suspending; a dead one prints
/// *"called for a dead object"* and does the same. Both are the discard sink,
/// so both are an ordinary return here.
template <bool kExplicitDuration>
HostOutcome progress_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);

  std::int64_t duration = 0;
  if constexpr (kExplicitDuration) {
    if (ctx.count() > 1 && ctx.arg(1).is_integer()) duration = ctx.arg(1).as_integer();
    // **Belt and braces, and knowingly.** The original has no clamp: it stamps
    // `now + ms` and suspends, so a negative gives an already-expired bar and a
    // wake-up in the past -- which `Scheduler` would also treat as ready at
    // once. So this changes nothing any caller can see, no shipped site passes
    // a negative, and a fault injected into this line survives the suite. It is
    // here because `suspend_for` is an interface others implement against and a
    // negative in it is a value no reader should have to think about.
    if (duration < 0) duration = 0;
  } else {
    const CommandQueue* q = self.commands->find(self.id);
    const Command* running = q == nullptr ? nullptr : q->running();
    duration = running == nullptr ? 0 : running->delay;
    // The no-argument form's own guard: no delay, no bar, no suspend.
    if (duration <= 0) return HostOutcome::ok_void();
  }
  // The bar, for the info bar's queue strip. `now` is the scheduler's clock
  // when there is one, which is what the suspension is measured against.
  if (CommandQueue* q = self.commands->find(self.id)) {
    const GameTime now = ctx.scheduler != nullptr ? ctx.scheduler->now() : self.world->time();
    q->progress_start = now;
    q->progress_end = now + duration;
  }
  return HostOutcome::sleep_for(duration);
}

/// `GetCommanded()`: whether the running command came from the player.
///
/// The corpus annotates the setter for us -- `u.SetCommanded(false); /// clear
/// user commanded flag` in `TOWNHALL_AUTOTRAIN.VS` -- and reads it to decide
/// whether the autotrain script may take a unit over. A unit the player has
/// just ordered somewhere is left alone.
///
/// **A receiver that does not resolve answers false, and so does not trap.**
/// `Unit::GetCommanded` (0x005d81e0) and `Unit::SetCommanded` (0x005d8180)
/// look the handle up and, when it names nothing, format *"called for an
/// uninitialized or invalid object"* into 0x00686eb0 -- a bare `ret` in retail,
/// so nothing prints -- and then push false or return. The script reaches that
/// branch: `TOWNHALL_AUTOTRAIN.VS:182-184` is `Sleep(1); u =
/// olOutside[i].AsUnit; if (u.GetCommanded) continue;`, and a unit that dies
/// during the `Sleep(1)` comes back from `AsUnit` as the invalid handle. Balcans
/// met it once in 4,000 turns. A missing world or command system is still this
/// engine's error, since the original has no such state.
HostOutcome get_commanded_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoReceiver) return HostOutcome::ok_with(Value::boolean(false));
    return HostOutcome::failed(self.error);
  }
  const CommandQueue* q = self.commands->find(self.id);
  const Command* running = q == nullptr ? nullptr : q->running();
  return HostOutcome::ok_with(Value::boolean(running != nullptr && running->user));
}

HostOutcome set_commanded_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoReceiver) return HostOutcome::ok_void();
    return HostOutcome::failed(self.error);
  }
  CommandQueue* q = self.commands->find(self.id);
  if (q == nullptr || q->entries.empty()) return HostOutcome::ok_void();
  q->entries.front().user = ctx.count() > 1 && ctx.arg(1).truthy_scalar();
  return HostOutcome::ok_void();
}

// -- the player-facing command table ---------------------------------------

/// `ExecCmd(name, pt, obj, replace)`: run a `DATA\COMMANDS` row.
///
/// The fourth argument is `replace`, which `GROUP_UNITSOUT.VS` names for us: it
/// forwards its own parameters as `l.ExecDefaultCmd(pt, obj, bReplace,
/// bModifier)`, the same position in the sibling entry point. Every one of the
/// 22 `ExecCmd` sites passes `false`, and they are right to: a barracks queues
/// `n` training commands with `for (i = 0; i < n; i += 1) b.ExecCmd(cmd, ...)`
/// and then counts them with `CmdCount(cmd)`, which only works if each call
/// appends. It appends as an order, though (`CommandSystem::append_order`): a
/// barracks resting on `idle` stops resting, where a script's `AddCommand`
/// would have queued behind an `idle` that never returns.
///
/// Returns whether the row exists, so that a caller can tell a misspelt command
/// from one it simply cannot afford. **The cost is not charged here**: the
/// shipped AI charges it itself (`set.SpentGoldOnArmy(gold + food / 2)` right
/// before the call), and double-charging would be a silent economy bug.
HostOutcome exec_cmd_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx, /*accept_query=*/false);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  if (ctx.count() < 2 || !ctx.arg(1).is_string()) return HostOutcome::failed(kNoVerb);
  const CommandDef* def = targets.commands->table().find(ctx.arg(1).as_string());
  if (def == nullptr) return HostOutcome::ok_with(Value::boolean(false));

  // `Obj::ExecCmd` (0x005b5470) and `ObjList::ExecCmd` (0x0055e240) both reach
  // the per-object issue 0x004ef3d0, which masks the flag off for a
  // `traincommand="yes"` row (0x004efbbb): a train row appends even when told
  // to replace. No shipped site asks, and `issue_order` does the same.
  const bool replace = ctx.count() > 4 && ctx.arg(4).truthy_scalar() && !def->train_command;
  Command prototype;
  // The object argument wins over the point when both are given: a command with
  // a target ("enter", "capture") is aimed at the object, and the shipped sites
  // pass a dummy point beside it.
  if (ctx.count() > 3 && object_of(ctx.arg(3)) != kNoObject) {
    prototype.arg_kind = CommandArgKind::object;
    prototype.object = object_of(ctx.arg(3));
  } else if (ctx.count() > 2 && is_point(ctx.arg(2))) {
    prototype.arg_kind = CommandArgKind::point;
    prototype.point = unpack_point(ctx.arg(2));
  }
  prototype.param = def->param;
  prototype.name = def->name;
  prototype.cost_gold = def->cost_gold;
  prototype.cost_food = def->cost_food;
  prototype.cost_pop = def->cost_pop;
  prototype.cost_stamina = def->cost_stamina;
  prototype.delay = def->exec_delay;

  for (const ObjectId id : targets.ids) {
    if (replace) {
      targets.commands->set_command(*targets.world, id, def->method, prototype);
    } else {
      targets.commands->append_order(*targets.world, id, def->method, prototype);
    }
  }
  return HostOutcome::ok_with(Value::boolean(true));
}

/// `GetCanExecCmd(name)`: the row exists, this object's class binds its method,
/// and nothing has taken the command away. One site, on a unit, for
/// "build_catapult".
///
/// **The third clause is the original's own.** 0x00560461, inside
/// `ObjList::GetCanExecCmd` (0x00560300), calls the object's `vtbl+0xd8` -- the
/// predicate that reads `[obj+0x9c]` and then walks the disabled list at
/// `[obj+0x78]`. `CommandSystem::command_enabled` is that predicate, and this
/// is one of its two shipped readers; the other is the interface's own button
/// availability (0x005e787b), which `CommandBar` now asks it of every selected
/// object.
HostOutcome get_can_exec_cmd_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (ctx.count() < 2 || !ctx.arg(1).is_string()) return HostOutcome::failed(kNoVerb);
  const CommandDef* def = self.commands->table().find(ctx.arg(1).as_string());
  if (def == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  if (!self.commands->command_enabled(*self.world, self.id, def->name)) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  return HostOutcome::ok_with(
      Value::boolean(!self.commands->script_for(*self.world, self.id, def->method).empty()));
}

/// `Obj::CmdDisable(name)` -- 2 sites, and with `Unit::AddBonus` it finishes
/// `3_Great_Losses_Egypt`'s `seq2.vs`: the mission opens by taking "BuySlaves"
/// off its market and "Chariot of Osiris" off its temple.
///
/// `0x005ab770` pops the string and the receiver, asks the global command
/// registry for the name's index, **returns without writing when that index is
/// negative**, and otherwise calls the object's `vtbl+0xdc`. That virtual
/// (0x005b5250) is three things in order: the `vtbl+0xd8` predicate, an append
/// to the list at `[obj+0x78]`, and a poke at the interface global that rebuilds
/// the command bar. Only the first two are state.
///
/// **A dead receiver is not refused**, which is worth saying because its
/// neighbour `SetCmdEnable` refuses one by name. There is no `IsAlive` test in
/// either the entry point or the virtual; the object is resolved and written to
/// whatever its health. This engine follows, and `command_enabled` reads the
/// same set for the living and the dead.
HostOutcome cmd_disable_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (ctx.count() < 2 || !ctx.arg(1).is_string()) return HostOutcome::failed(kNoVerb);
  self.commands->disable_command(*self.world, self.id, ctx.arg(1).as_string());
  return HostOutcome::ok_void();
}

/// `GetCmdCost(name, gold, food)` -> bool, writing through arguments two and
/// three. 22 sites, every one of the form
/// `if (GetCmdCost(cmd, gold, food)) if (set.CanAfford(gold, food))`, which is
/// what fixes the return value as "the command exists" rather than "it is
/// affordable".
HostOutcome get_cmd_cost_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  CommandSystem* commands = command_system(*world);
  if (commands == nullptr) return HostOutcome::failed(kNoSystem);
  if (ctx.count() < 3 || !ctx.arg(0).is_string()) return HostOutcome::failed(kNoVerb);
  const CommandDef* def = commands->table().find(ctx.arg(0).as_string());
  ctx.out(1) = Value::integer(def == nullptr ? 0 : def->cost_gold);
  ctx.out(2) = Value::integer(def == nullptr ? 0 : def->cost_food);
  return HostOutcome::ok_with(Value::boolean(def != nullptr));
}

/// `GetCmdStaminaCost(name)` -> `<cmd coststamina>`. 42 sites, all druid and
/// hero magic: "learn", "teach", "heal", "curse", "cripple", "revitalize".
HostOutcome get_cmd_stamina_cost_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  CommandSystem* commands = command_system(*world);
  if (commands == nullptr) return HostOutcome::failed(kNoSystem);
  if (ctx.count() < 1 || !ctx.arg(0).is_string()) return HostOutcome::failed(kNoVerb);
  const CommandDef* def = commands->table().find(ctx.arg(0).as_string());
  return HostOutcome::ok_with(Value::integer(def == nullptr ? 0 : def->cost_stamina));
}

// -- Idle ------------------------------------------------------------------

/// `Idle([ms])`: stand about for a while.
///
/// **An order, not a predicate.** Its 63 call sites are all bare statements:
/// not one is a condition, not one is assigned, and the inventory's return
/// column is blank because no typed local ever receives it. `UNIT_IDLE.VS`'s
/// `.Idle(1900 + rand(100))` and `WOLF_IDLE.VS`'s `.Idle(2000)` are the shape,
/// and `SHIP_IDLE.VS`'s `while(1) { ... if (.Stop(200)) .Idle(); }` spins the
/// scheduler if `Idle` costs no game time.
///
/// It does not stop the unit's route: every site that wants that calls `Stop`
/// right next to it -- `while (!.Stop(1000)); .Idle(1500);`.
///
/// **What `Unit::Idle` does** (0x005d6030), read off `gbr.exe`. It runs on the
/// suspending registrar, so the interpreter tells it a first entry from a
/// resume by a flag byte it writes above the arguments (opcode 0x1E's handler,
/// 0x0069d564: set on the first entry, cleared on the resume).
///
///   * **A held receiver** -- position `(-1, -1)`, a unit inside a building or
///     a ship -- writes `ms` to the wait cell and returns 1, suspend; its
///     resume pops the arguments and returns 0. A plain timed wait.
///   * **Anything on the map** pops the arguments, sets the unit's activity to
///     4 (the setter 0x005a7680), **drops the combat target** (0x005d60b7: the
///     handle at `[unit+0x1a8]` emptied and the attack count beside it zeroed,
///     only when there is a target) and returns **3**.
///
/// **What a 3 is.** The suspending call's dispatch (the jump table at
/// 0x0069dbc4) treats 0 as done, 1 as suspend-and-run-again, 2 as end the
/// script, and 3 as *done, but end the slice*: the call is stepped over as a 0
/// is, the slice's wait cell is set to -1 and the budget is forced out
/// (0x0069d5fb). The scheduler reads a -1 as no timed wake at all: it takes
/// the coroutine off its timer queue (0x0069f842 -> 0x00687bf0). What wakes it
/// is the unit, when the activity the call started is over.
///
/// **And the activity spends the `ms` the call popped.** The idle and taunt
/// activity (0x005d2ca0, reached from the unit's activity dispatch at
/// 0x005d5bc2 for 4 and 5) reads its budget from the slot just above the
/// script's stack pointer -- which is where `Idle`'s argument was before the
/// pop, and the pop is made *before* the activity is set so that it is still
/// there. `Ship::Idle` (0x005c6ea0), the arity-0 form, writes its own duration
/// into that same slot before it returns 3, which is the second reading and
/// agrees. The activity holds the idle pose and plays fidget animations within
/// that budget, in 100 ms steps, and when the budget is spent it wakes the
/// script (0x005d2de0 -> 0x0069f8f0, a wake at once) and sets the activity
/// back to 0. So an `Idle(1900)` on the map costs about 1,900 ms of game time
/// in both branches, and the difference between them is the target.
///
/// **Here:** the held branch is the timed wait it was; the other drops the
/// target and is a suspension of `ms`, which is what the activity's budget
/// amounts to. `CombatSystem::stop` is the drop, as it is for `Taunt`: it also
/// moves an engaging unit out of the engaging state, which the activity write
/// does too. A non-positive `ms` still ends the slice, as a 3 always does --
/// one pass, the shortest wait this scheduler has -- and no shipped site
/// passes one.
///
/// **Not reproduced, recorded:** the fidget animations are drawn from the
/// synchronised generator (`[0x996ff4] + 0x12a0`, its `vtbl+0x14`, a one in 15
/// chance per 100 ms step for an idle, one in 3 for a taunt), so in `gbr.exe`
/// an idle on the map spends random draws that this engine does not. The
/// activity write itself is absent for the reasons `Taunt` gives below.
/// A handle that names no object is held for `ms` as before: the original
/// dereferences null there, which is a fault rather than a behaviour, and a
/// wait is what keeps an idle loop from spinning on one.
HostOutcome idle_activity(const Self& self, std::int64_t ms) {
  const HostOutcome wait = HostOutcome::sleep_for(ms > 0 ? ms : 0);
  const WorldObject* slot = self.world->find(self.id);
  if (slot == nullptr || slot->state.position == kHeldPosition) return wait;
  if (CombatSystem* combat = combat_system_of(*self.world); combat != nullptr) {
    (void)combat->stop(self.id);
  }
  return wait;
}

HostOutcome idle_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  std::int64_t ms = kDefaultIdleSlice;
  if (ctx.count() > 1 && ctx.arg(1).is_integer()) ms = ctx.arg(1).as_integer();
  return idle_activity(self, ms);
}

/// `ForceIdle()` -- one shipped site, `HEN_IDLE.VS`, on the line before its
/// `Idle(2000)`.
///
/// `0x005d88b0` is `[obj+0x1b8] = 1` and nothing else, and the flag has exactly
/// one reader: the unit's idle loop at `0x005d2d70`, which polls in hundred-
/// millisecond steps and breaks out early when it is set -- clearing it at
/// `0x005d2da8` and starting a fresh idle animation. So the entry point means
/// *stop holding the pose and pick a new one now*. `Ship::Idle` (0x005c6ef0)
/// writes the same 1 into the same slot, which is the second reading of it and
/// agrees.
///
/// **There is nothing here for it to cut short**, and that is why it stores
/// nothing. `Idle` above is a suspension and only a suspension: it starts no
/// animation, so there is no pose to replace and no wait to interrupt, and the
/// section comment on `Idle` already says why -- *"whether it also selects an
/// idle animation is unknown; the deer's `SetWalkAnim(13)` suggests the slot
/// exists but nothing ties it to this entry point"*. A flag written here and
/// read by nobody would be state two peers could disagree about for no reason.
/// The day `Idle` animates, this is the other half of it.
///
/// It still resolves its receiver, because the original does and because
/// answering without looking is how an entry point stops being a test of
/// anything.
HostOutcome force_idle_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  return HostOutcome::ok_void();
}

/// `u.Taunt(ms)` -- 6 sites, and the sole blocker of four `SUBAI` scripts.
///
/// **`Unit::Taunt` and `Unit::Idle` are the same function.** 0x005d60f0 and
/// 0x005d6030 differ in exactly one byte: the constant they hand to the
/// activity setter at 0x005a7680, 5 for a taunt and 4 for an idle. Both take
/// `(Unit, int)` on the suspend registrar, and both do three things in this
/// order:
///
///   1. If the receiver's position is `(-1, -1)` -- the two globals at
///      0x008212bc and 0x008212c0, which are `0xffffffff` with no writer
///      anywhere in `.text` -- **suspend for the argument and then return**.
///      That is the *held* case: a unit inside a building or a ship has no map
///      position, and its location is its holder's. It cannot taunt, so it
///      waits instead.
///   2. Otherwise set the activity and **drop the combat target**: the
///      `target != 0xffff -> target = 0xffff, attacks = 0` pair that also opens
///      `Stop`, both `Goto`s, `GotoAttack`, `FormKeepMoving`, `Attack` and
///      `AttackEveryone`. It is this family's "a new order supersedes combat"
///      prologue.
///   3. Return 3: the call is done and the slice ends with no timed wake
///      (0x0069d5fb), and the taunt activity wakes the script once it has
///      spent the argument the call popped (see `Idle` above). **So a
///      `Taunt(2000)` on the map is a two-second taunt.**
///
/// **This used to cost one scheduler pass.** The reading was that a 3 is
/// fire-and-forget and the 2,000 every shipped site passes is consumed only
/// by the held branch; it missed that the activity (0x005d2ca0) takes its
/// budget from the slot the argument was popped from. The difference is the
/// capture rate: `UNIT_CAPTURE.VS` and `HERO_CAPTURE.VS` loop
/// `.Taunt(2000); b.settlement.DecreaseLoyalty(1);`, which took a point of
/// loyalty every pass rather than every two seconds.
///
/// **The activity is deliberately not written** (nor, for `Idle`, the idle
/// one): `+0x130` is a small transient enum this engine does not model, it is
/// not in the original's serialiser list, and a value written here and read by nobody would be state two peers
/// could disagree about for no reason. Dropping the target *is* modelled, and
/// `CombatSystem::stop` is that pair -- it also moves the unit out of the
/// engaging animation, which the original does too, just into a taunt pose
/// rather than an idle one. This engine has no taunt pose to move it into.
///
/// `gbr.exe` registers `Unit::Idle` **once**, at arity 1; the arity-0 form this
/// engine serves from the same body is `Ship::Idle` (0x005c6ea0), which sets
/// the `ForceIdle` flag and supplies its own duration (see `Idle` above).
HostOutcome taunt_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  std::int64_t ms = 0;
  if (ctx.count() > 1 && ctx.arg(1).is_integer()) ms = ctx.arg(1).as_integer();
  return idle_activity(self, ms);
}

// -- the cross-domain orders -----------------------------------------------

/// Everything a `Goto`-family call needs beyond the queue.
struct Mover {
  World* world = nullptr;
  MovementSystem* movement = nullptr;
  ObjectId id = kNoObject;
  const char* error = nullptr;

  [[nodiscard]] bool ok() const noexcept { return error == nullptr; }
};

[[nodiscard]] Mover mover_of(CallContext& ctx) {
  Mover self;
  self.world = world_of(ctx);
  if (self.world == nullptr) {
    self.error = kNoWorld;
    return self;
  }
  self.movement = movement_system(*self.world);
  if (self.movement == nullptr) {
    self.error = kNoMovement;
    return self;
  }
  if (ctx.count() == 0 || (self.id = object_of(ctx.arg(0))) == kNoObject) {
    self.error = kNoReceiver;
  }
  return self;
}

/// `GotoAttack(target, slice, flag[, give_up])`.
///
/// `Goto` with the arrival annulus taken from the attacker's own weapon instead
/// of from an argument -- which is exactly what its signature is missing next to
/// `Goto`'s, and what `UNIT_ENGAGE.VS` needs:
///
///     while (!.GotoAttack(u, 1500, true, 15000)) { ...retarget... }
///     while (.Attack(u));
///
/// So this returns true precisely when `Attack` would connect, and it gets
/// there by asking combat for the same numbers `in_attack_range` uses: the
/// attacker's `range` plus both radii for the outer bound, its `min_range` for
/// the inner. With no combat system registered it degrades to walking onto the
/// target, which is what a `range` of zero means.
HostOutcome goto_attack_impl(CallContext& ctx) {
  const Mover self = mover_of(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  const ObjectId target = object_of(ctx.arg(1));
  GotoOrder order;
  if (target != kNoObject) {
    order.target = target;
    order.dest = self.world->resolve_position(target);
  } else if (is_point(ctx.arg(1))) {
    order.dest = unpack_point(ctx.arg(1));
  } else {
    return HostOutcome::failed(kNoPoint);
  }

  if (CombatSystem* combat = combat_system_of(*self.world); combat != nullptr) {
    const Combatant* attacker = combat->find(self.id);
    if (attacker != nullptr) {
      const CombatProfile profile = combat->profile(attacker->class_index);
      order.range = profile.range + profile.radius;
      order.min_range = profile.min_range;
      const Combatant* defender = target == kNoObject ? nullptr : combat->find(target);
      if (defender != nullptr) {
        order.range += combat->profile(defender->class_index).radius;
      }
    }
  }

  order.slice = ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  order.give_up = ctx.count() > 4 && ctx.arg(4).is_integer() ? ctx.arg(4).as_integer() : -1;
  order.lock_destination = true;
  return run_goto(ctx, *self.world, *self.movement, self.id, order);
}

/// `GotoEnter(dest, range, slice, flag, give_up)`: `Goto`'s signature exactly,
/// and `UNIT_ENTER.VS` puts the two in the arms of one `if`. The one
/// difference is the lock flag -- see the header and `GotoOrder::give_up`.
HostOutcome goto_enter_impl(CallContext& ctx) {
  const Mover self = mover_of(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  GotoOrder order;
  if (is_point(ctx.arg(1))) {
    order.dest = unpack_point(ctx.arg(1));
  } else if (const ObjectId target = object_of(ctx.arg(1)); target != kNoObject) {
    order.target = target;
    order.dest = self.world->resolve_position(target);
  } else {
    return HostOutcome::failed(kNoPoint);
  }
  order.range = ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  order.slice = ctx.arg(3).is_integer() ? ctx.arg(3).as_integer() : 0;
  order.give_up = ctx.count() > 5 && ctx.arg(5).is_integer() ? ctx.arg(5).as_integer() : -1;
  return run_goto(ctx, *self.world, *self.movement, self.id, order);
}

/// The verb `CVXFormObj` hands every member when its march is set up
/// (0x005f2b50, the string at 0x007d5698), bound by `UNIT.SC.XML` to
/// `UNIT_FORM_MOVE.VS`.
constexpr std::string_view kFormMove = "form_move";

/// Whether `id` is on its hero's march: running the march's verb. A world
/// with no command system has no verbs to run, and every member marches.
[[nodiscard]] bool on_march(World& world, ObjectId id) {
  const CommandSystem* commands = command_system(world);
  if (commands == nullptr) return true;
  return commands->command_count(id) > 0 && commands->command_name(id, 0) == kFormMove;
}

/// What a held unit waits before it may step out of its holder towards
/// `toward`, or 0 once it is out (or was never in): the opening the
/// formation entry points share with `Goto`. A unit held by an object -- a
/// ship's passenger -- waits 100 ms and asks again (`held_by_carrier`); one in
/// a settlement waits its turn at the exit (`garrison_exit`, throttled).
constexpr std::int32_t kCarrierExitPoll = 100;

[[nodiscard]] std::int32_t step_out_wait(World& world, ObjectId id, Point toward, GameTime now) {
  const ObjectState* state = world.state(id);
  if (state == nullptr || !state->is_held()) return 0;
  if (held_by_carrier(world, id)) return kCarrierExitPoll;
  return garrison_exit(world, id, toward, now);
}

/// A call that has to wait before its unit is out: suspended and run again
/// whole, which is what the original's `return 1` with the wait cell written
/// is for an entry point that does not read its re-entry flag.
[[nodiscard]] HostOutcome wait_to_step_out(std::int32_t wait) {
  HostOutcome out;
  out.status = script::HostStatus::retry;
  out.suspend_for = wait;
  return out;
}

/// Where each member of `hero`'s army stands around `anchor`, facing
/// `facing`: `members` and `stations` filled in army order, and the number
/// placed returned -- the first that many have a station.
///
/// The offsets, the three placements and the spacing all come from
/// `DATA\FORMATIONS.XML` through `formation_offsets`; nothing is invented here.
std::size_t army_stations(World& world, MovementSystem& movement, const HeroRecord& record,
                          ObjectId hero, Point anchor, Point facing,
                          std::vector<FormationMember>& members, std::vector<Point>& stations) {
  members.clear();
  stations.clear();
  const MoveState& lead = movement.state(hero);
  const FormationClassDef* formation = lead.formation.empty()
                                           ? movement.formations().default_formation()
                                           : movement.formations().find(lead.formation);
  if (formation == nullptr) return 0;

  const ClassGraph* graph = world.class_graph();
  members.reserve(record.army.size());
  for (const ObjectId id : record.army) {
    FormationMember member;
    member.id = id;
    member.formation_radius = movement.state(id).formation_radius;
    const WorldObject* slot = world.find(id);
    if (graph != nullptr && slot != nullptr && slot->class_index != kNoClass) {
      member.placement = formation->placement_of(graph->at(slot->class_index).id);
    }
    members.push_back(member);
  }

  std::vector<Point> offsets(members.size());
  const std::size_t placed = formation_offsets(*formation, members, facing, offsets);
  stations.resize(placed);
  for (std::size_t i = 0; i < placed; ++i) {
    stations[i] = Point{anchor.x + offsets[i].x, anchor.y + offsets[i].y};
  }
  return placed;
}

/// Send one marching member to `station`. A member already going there --
/// within `repath_threshold` of it, on this hero's march -- is left alone, so
/// a marching column does not run a fresh A* per member per call. The order
/// carries the formation's lock flag (`MoveState::form_lock` on the hero),
/// which makes the member free-spot tested once the march is over
/// (`MovementSystem::lock_owner`).
void march_member(World& world, MovementSystem& movement, ObjectId hero, ObjectId id,
                  Point station) {
  const bool lock = movement.state(hero).form_lock;
  const MoveState& member = movement.state(id);
  if (member.goto_active && member.party == hero &&
      within(member.target, station, MovementSystem::repath_threshold())) {
    return;
  }
  movement.order_goto(world, id, station, 0, 0, hero, lock);
}

/// Place a hero's army at its formation offsets around `anchor`.
///
/// **Only a member running `form_move` is on the march.** The original's
/// members follow the formation object's samples through a route aimed at
/// it (`SetFormation`, 0x00417790), which `UNIT_FORM_MOVE.VS`'s
/// `FormAcceptMove` sets up; a member given any other command since -- a
/// squad order, `stand_position` at the march's end -- has a script of its
/// own and a route of its own, and the march no longer moves it. Here every
/// member used to be re-ordered on every call whatever it was running, and
/// as nothing gave it `form_move` it was running `UNIT_IDLE.VS`, whose
/// `Stop(2000)` dropped the station's route every few seconds: an army
/// marched in fits and was left wherever the last `Stop` caught it when its
/// hero arrived -- on Crossroads, columns of four and five to a 16-unit cell.
///
/// **Nor is a member still inside a holder.** It leaves in its own
/// `FormAcceptMove`, one per exit slot, and is sent to its station there.
/// This used to step every held member out itself, the whole column through
/// one door in one call.
std::size_t place_army(World& world, MovementSystem& movement, HeroSystem& heroes, ObjectId hero,
                       Point anchor, Point facing) {
  const HeroRecord* record = heroes.hero(hero);
  if (record == nullptr || record->army.empty()) return 0;

  // The march is one party: its members and the hero leading it do not block
  // one another's steps (`sim/movement.hpp`, inference 3).
  movement.state(hero).party = hero;
  std::vector<FormationMember> members;
  std::vector<Point> stations;
  const std::size_t placed =
      army_stations(world, movement, *record, hero, anchor, facing, members, stations);
  for (std::size_t i = 0; i < placed; ++i) {
    if (!on_march(world, members[i].id)) continue;
    if (const ObjectState* state = world.state(members[i].id);
        state != nullptr && state->is_held()) {
      continue;
    }
    march_member(world, movement, hero, members[i].id, stations[i]);
  }
  return placed;
}

/// `FormSetupAndMoveTo(dest, range, min_range, flag)`.
///
/// Arguments two and three are the arrival annulus:
/// `FormSetupAndMoveTo(b, 100, 100, false)` uses both halves, and
/// `FormSetupAndMoveTo(holder, GetConst("GiveDistance"), 0, true)` names the
/// second one a distance outright. The fourth is the family's undecided
/// boolean; see the header.
HostOutcome form_setup_and_move_to_impl(CallContext& ctx) {
  const Mover self = mover_of(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  HeroSystem* heroes = hero_system_of(*self.world);
  if (heroes == nullptr) return HostOutcome::failed(kNoHeroes);

  Point dest{};
  ObjectId target = kNoObject;
  if (is_point(ctx.arg(1))) {
    dest = unpack_point(ctx.arg(1));
  } else if ((target = object_of(ctx.arg(1))) != kNoObject) {
    dest = self.world->resolve_position(target);
  } else {
    return HostOutcome::failed(kNoPoint);
  }
  const std::int32_t range = ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  const std::int32_t min_range = ctx.arg(3).is_integer() ? ctx.arg(3).as_integer() : 0;

  // **The hero steps out of his holder first, and waits his turn to**
  // (0x0052e65d .. 0x0052e6cc): aboard a ship he polls every 100 ms; in a
  // settlement he takes the exit slot towards `dest` (0x005d3f20), and while
  // the settlement's last exit is too recent the call suspends for what it
  // answers and runs again whole -- the body never reads its re-entry flag.
  // Nothing of the march is set up until he is out, so neither is the army's
  // `form_move`. This used to step him out at once whatever the slot said.
  if (const std::int32_t wait = step_out_wait(*self.world, self.id, dest, now_of(ctx, *self.world));
      wait > 0) {
    return wait_to_step_out(wait);
  }

  // `CVXFormObj::SetDest` (0x005f2cc0) keeps the flag at `[form+0x8c]`.
  self.movement->state(self.id).form_lock = ctx.count() > 4 && ctx.arg(4).truthy_scalar();
  const MoveOutcome outcome =
      target == kNoObject
          ? self.movement->order_goto(*self.world, self.id, dest, range, min_range)
          : self.movement->order_goto_object(*self.world, self.id, target, range, min_range);

  // The march is handed to the army. `SetDest` that lays a route gives every
  // member `form_move` (0x005f2b50): its route deleted (0x005d3830(0)) and the
  // command inserted with the replace flag (0x005b4e90 with 1), which ends
  // whatever it was running. One that lays none ends the march instead
  // (0x005f2c10): every member running `form_move` loses its route, and
  // `UNIT_FORM_MOVE.VS`'s `while (.HasPath())` lets it go.
  const HeroRecord* record = heroes->hero(self.id);
  CommandSystem* commands = command_system(*self.world);
  if (record != nullptr && commands != nullptr) {
    const std::vector<ObjectId> army = record->army;
    for (const ObjectId member : army) {
      if (outcome == MoveOutcome::blocked) {
        if (on_march(*self.world, member)) self.movement->stop(*self.world, member);
        continue;
      }
      self.movement->stop(*self.world, member);
      (void)commands->set_command(*self.world, member, kFormMove, Command{});
    }
  }
  if (outcome == MoveOutcome::blocked) return HostOutcome::ok_void();

  const MoveState& lead = self.movement->state(self.id);
  place_army(*self.world, *self.movement, *heroes, self.id,
             self.world->resolve_position(self.id), lead.facing);
  return HostOutcome::ok_void();
}

/// `Unit::FormAcceptMove()` -- 1 site, and it was the sole blocker of
/// `UNIT_FORM_MOVE.VS` (17 sites), the script the `form_move` verb runs on an
/// army member: `.FormAcceptMove(); while (.HasPath()) .FormKeepMoving(1000);`
/// and then a turn to face the party's final orientation.
///
/// `0x005d7880` is registered as suspending, with no arguments. An invalid
/// receiver prints `The function 'Unit::FormAcceptMove' called for an
/// uninitialized or invalid object.` and finishes. So does a unit with no
/// pending party move -- the party record at `[unit+0x1c0]` with a target at
/// `+4` -- or no hero at `[unit+0x170]`; that exit returns 2. A member inside
/// a holder is the interesting half: while the holder's exit slot at
/// `[holder+0xe]` names somebody (0x005319a0), it waits 100 ms and asks again;
/// then 0x005d3f20 reads the settlement's exit timing (`[settlement+0x9c]`,
/// `+0xa0`) against the clock and waits out its own turn to leave. Only then
/// does it step out (0x005d2c70), build itself a path object if it has none
/// (0x00418e90, 0x005d3830), aim it at the party's target (0x00417790), set
/// its state to 3 -- marching -- and finish.
///
/// **What runs here is the finishing half, and why the rest does not.** The
/// original's party-move code hands each member a `form_move` command
/// (0x005f2b50) and a target through that record, and the member fetches the
/// target itself here. This engine's formation march is `place_army`: the
/// same `FormSetupAndMoveTo` that gives the members `form_move` orders each
/// member on the map straight to its station through the movement system,
/// before the command's script first runs. So a member that reaches this call
/// on the map already has its order, and the answer is to finish, so the
/// script's `while (.HasPath())` takes over; the member's route then lasts as
/// long as the march does (`MovementSystem::marching`).
///
/// **A held member leaves here, one exit slot at a time.** Aboard a ship it
/// polls every 100 ms; in a settlement it asks `garrison_exit` for the slot,
/// towards its station, and while the settlement's last exit is too recent
/// the call suspends for the wait it answers and runs again whole. Out, it is
/// sent to its station as `place_army` sends the others. **The station is an
/// inference:** the original steps out towards the point at `+0x10` of the
/// member's party record, which `form_move`'s hand-out (0x005f2bc5) resets to
/// `(-1, -1)` and the formation then fills; it is read as the member's place
/// in the formation. Until this, `place_army` stepped every held member out
/// in the one call that set the march up, and a column left a town as one
/// pile on its door.
///
/// **Every member that gets this far drops its combat target** (0x005d798b,
/// `CombatSystem::drop_target`). A unit with no hero is the original's
/// finished-at-once exit (0x005d7a1c), and so is one whose hero has no army
/// record here.
HostOutcome form_accept_move_impl(CallContext& ctx) {
  World* world = world_of(ctx);
  if (world == nullptr) return HostOutcome::failed(kNoWorld);
  // The receiver is looked at so that a handle to nothing is the original's
  // printed-and-finished path rather than a trap.
  const ObjectId id = object_of(ctx.arg(0));
  HeroSystem* heroes = hero_system_of(*world);
  MovementSystem* movement = movement_system(*world);
  if (id == kNoObject || world->find(id) == nullptr || heroes == nullptr || movement == nullptr) {
    return HostOutcome::ok_void();
  }
  const ObjectId hero = heroes->hero_of(id);
  const HeroRecord* record = hero == kNoObject ? nullptr : heroes->hero(hero);
  if (record == nullptr || world->find(hero) == nullptr) return HostOutcome::ok_void();

  if (const ObjectState* state = world->state(id); state != nullptr && state->is_held()) {
    // A hero back inside has no formation on the map to take a place in: the
    // member steps out towards where he is held and stays at the door.
    std::optional<Point> station;
    if (const ObjectState* lead = world->state(hero); lead != nullptr && !lead->is_held()) {
      std::vector<FormationMember> members;
      std::vector<Point> stations;
      const std::size_t placed =
          army_stations(*world, *movement, *record, hero, world->resolve_position(hero),
                        movement->state(hero).facing, members, stations);
      for (std::size_t i = 0; i < placed; ++i) {
        if (members[i].id == id) station = stations[i];
      }
    }
    const Point toward = station.value_or(unit_pos_rh(*world, hero));
    if (const std::int32_t wait = step_out_wait(*world, id, toward, now_of(ctx, *world));
        wait > 0) {
      return wait_to_step_out(wait);
    }
    if (station.has_value() && on_march(*world, id)) {
      movement->state(hero).party = hero;
      march_member(*world, *movement, hero, id, *station);
    }
  }
  if (CombatSystem* combat = combat_system_of(*world); combat != nullptr) {
    (void)combat->drop_target(id);
  }
  return HostOutcome::ok_void();
}

/// `FormKeepMoving(ms)`: advance a formation march by up to `ms`.
///
/// `HERO_MOVE.VS` is the whole protocol -- `FormSetupAndMoveTo` once, then
/// `while (.HasPath()) .FormKeepMoving(1500);` -- so this suspends and returns
/// nothing, and the loop's exit condition is the hero's own path, not this
/// call's result. Each call re-places the army around where the hero is *now*,
/// which is what makes the formation follow him rather than pile up at the
/// destination he was given.
HostOutcome form_keep_moving_impl(CallContext& ctx) {
  const Mover self = mover_of(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (HeroSystem* heroes = hero_system_of(*self.world); heroes != nullptr) {
    const MoveState& lead = self.movement->state(self.id);
    place_army(*self.world, *self.movement, *heroes, self.id,
               self.world->resolve_position(self.id), lead.facing);
  }

  // **The full `ms`, never the ETA.** This used to clamp the wait to
  // `movement->eta(self.id)` and return immediately when there was nothing left
  // to walk, which reads well against `HERO_MOVE.VS`'s
  // `while (.HasPath()) .FormKeepMoving(1500);` -- the last slice lands exactly
  // on arrival -- and is not what the original does.
  //
  // 0x0052eaf0 is two phases and neither consults a path. The first entry (the
  // interpreter's re-entry flag at `[sp-1]` is clear) sets the hero's march
  // state to 3, raises bit 0x10000 of `[hero+0x194]`, writes the `ms` argument
  // to the scheduler's wait global at `[0xa77eac]` and returns **1** -- suspend.
  // The re-entry clears the flag and returns 0. There is no branch on distance
  // anywhere in the body.
  //
  // The march the clamp was written for is unaffected: `HERO_MOVE.VS` exits on
  // `.HasPath`, not on this call's wait, so all the clamp bought was a shorter
  // final slice -- and it cost a divergence on every call whose ETA is under
  // its argument, which is most of them.
  //
  // Recorded because it was nearly justified with the wrong evidence:
  // `HERO_IDLE.VS` calls `.FormSetupAndMoveTo(.pos, ...)` and then
  // `.FormKeepMoving(1000)`, and that call is the only suspension in that arm
  // of its `while(1)`, so the clamp looked like the cause of a non-yielding
  // loop the corpus sweep found there. It is not: a goto to the position the
  // object already occupies leaves `has_path` false, `eta` returns **-1** for
  // that, and `eta >= 0` was never true. The clamp is wrong on the
  // disassembly's evidence alone. What spins in `hero_idle.vs` is still open.
  const std::int64_t wait =
      ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  if (wait <= 0) return HostOutcome::ok_void();

  HostOutcome out;
  out.status = script::HostStatus::suspend;
  out.suspend_for = wait;
  return out;
}

/// `FormPathLeft()`: world units still to walk on the hero's own route.
///
/// Both sites compare it against a radius -- `if (.FormPathLeft() <
/// .FormRadius() + s.radius) break;` -- so it is a distance and not a time.
HostOutcome form_path_left_impl(CallContext& ctx) {
  const Mover self = mover_of(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  const std::int64_t left = self.movement->remaining(self.id);
  return HostOutcome::ok_with(
      Value::integer(static_cast<std::int32_t>(std::min<std::int64_t>(left, 0x7FFFFFFF))));
}

/// The number of `define` calls `register_command_host` makes. Kept next to the
/// list so the two cannot drift.
constexpr std::size_t kEntryCount = 34;

}  // namespace

std::size_t command_host_entry_count() noexcept { return kEntryCount; }

std::size_t register_command_host(HostRegistry& registry) {
  std::size_t defined = 0;
  const auto member = [&](std::string_view name, std::uint16_t arity, script::HostFn fn) {
    registry.define(CallKind::member, name, arity, fn);
    ++defined;
  };
  const auto free_fn = [&](std::string_view name, std::uint16_t arity, script::HostFn fn) {
    registry.define(CallKind::free_function, name, arity, fn);
    ++defined;
  };

  // Descending corpus call frequency; see docs/formats/vs-host-api.md.
  member("AddCommand", 2, &add_command_impl);            // 215 sites, with /3
  member("SneakCommand", 1, &sneak_command_impl);        //   6, and a whole file behind it
  member("AddCommand", 3, &add_command_impl);
  member("SetCommand", 1, &set_command_impl);            // 200, with /2
  member("SetCommand", 2, &set_command_impl);
  member("command", 0, &command_impl);                   // 137, with /1
  member("command", 1, &command_impl);
  member("Idle", 0, &idle_impl);                         //  63, with /1
  member("Idle", 1, &idle_impl);
  member("ForceIdle", 0, &force_idle_impl);              //   1, HEN_IDLE.VS
  // `Idle`'s twin, one constant apart in `gbr.exe`. 6 sites, and the sole
  // blocker of `HERO_CAPTURE.VS`, `UNIT_CAPTURE.VS`, `HERO_TAUNT.VS` and
  // `MELEE_ATTACK.VS` -- 243 call sites between them.
  member("Taunt", 1, &taunt_impl);                       //   6
  member("GotoAttack", 3, &goto_attack_impl);            //  47, with /4
  member("GotoAttack", 4, &goto_attack_impl);
  member("KillCommand", 0, &kill_command_impl);          //  43
  member("CmdCount", 0, &cmd_count_impl);                //  29, with /1
  member("CmdCount", 1, &cmd_count_impl);
  member("FormKeepMoving", 1, &form_keep_moving_impl);   //  29
  member("FormAcceptMove", 0, &form_accept_move_impl);  //   1, and a whole file behind it
  member("FormSetupAndMoveTo", 4, &form_setup_and_move_to_impl);  // 26
  member("GotoEnter", 5, &goto_enter_impl);              //  22
  member("ExecCmd", 4, &exec_cmd_impl);                  //  22
  member("GetCommanded", 0, &get_commanded_impl);        //  14
  member("cmddelay", 0, &cmd_delay_impl);                //   8
  member("Progress", 0, &progress_impl<false>);          //  14
  member("Progress", 1, &progress_impl<true>);           //   9
  member("SetCommandOffset", 2, &set_command_offset_impl);  //  6
  member("AddCommandOffset", 3, &add_command_offset_impl);  //  2
  member("ClearCommands", 0, &clear_commands_impl);      //   2
  member("SetCommanded", 1, &set_commanded_impl);        //   2
  member("FormPathLeft", 0, &form_path_left_impl);       //   2
  member("GetCanExecCmd", 1, &get_can_exec_cmd_impl);    //   1
  member("CmdDisable", 1, &cmd_disable_impl);            //   2, and a whole file behind it

  free_fn("GetCmdStaminaCost", 1, &get_cmd_stamina_cost_impl);  // 42
  free_fn("GetCmdCost", 3, &get_cmd_cost_impl);                 // 22

  // Deliberately still declared and unimplemented:
  //
  //   `Squad.SetCmd/4,5` (30 sites) and `Squad.AIDest/0` (36) have **moved**
  //   rather than stayed blocked: `sim/squad.hpp` now packs a squad's
  //   `(index, player)` key into a handle and `sim/gaika.hpp` settles that a
  //   GAIKA is an integer index, so both are implemented in
  //   `register_squad_host`, next to the table they read. `SetCmd` calls
  //   `CommandSystem::set_command` once per squad member; the composition is
  //   documented at its declaration.
  //
  //   `ExecDefaultCmd/4` (1 site). It resolves a right-click against the
  //   `<defaultcmd target="...">` blocks, which are a *cursor* rule -- which
  //   command a class offers against which target class -- and the class graph
  //   carries them but nothing establishes the tie-break between the several
  //   `<cmd>` rows in one block.
  //
  //   `ForceIdle/0`, `Squad.ClrCmd/3`, `Squad.DelOrder/0`. One or two sites
  //   each and no context that distinguishes a reading; a plausible guess here
  //   is exactly the silent divergence an unimplemented entry point exists to
  //   prevent. (`SneakCommand/1` used to be listed here and is above;
  //   `SetCmdEnable/1` and `FormAcceptMove/0` live in `world_host.cpp` and
  //   `movement.cpp`.)
  return defined;
}

}  // namespace imperivm::core::sim
