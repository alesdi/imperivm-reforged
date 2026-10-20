// The command queue and the cross-domain orders.
// See include/imperivm/core/sim/command.hpp.

#include "imperivm/core/sim/command.hpp"
#include "imperivm/core/sim/stats.hpp"

#include "imperivm/core/sim/host_context.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>

#include "imperivm/core/game/class_graph.hpp"
#include "imperivm/core/script/scheduler.hpp"
#include "imperivm/core/sim/combat.hpp"
#include "imperivm/core/sim/economy.hpp"
#include "imperivm/core/sim/entrance.hpp"
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
      // Anything but `no` is yes, and absent is `no` (0x004f3f80..0x004f405a).
      const auto flag = [&](std::string_view attribute) {
        const std::string_view value = doc.attribute(n, attribute);
        return !value.empty() && !equal_fold(value, "no");
      };
      def.train_command = flag("traincommand");
      def.research_command = flag("researchcommand");
      def.immediate = flag("immediate");
      def.plr_research = flag("plrresearch");
      // A player research is a research (0x004f406d).
      if (def.plr_research) def.research_command = true;
    }
    def.cost_gold = doc.attribute_int(n, "costgold", 0);
    def.cost_food = doc.attribute_int(n, "costfood", 0);
    def.cost_pop = doc.attribute_int(n, "costpop", 0);
    def.cost_stamina = doc.attribute_int(n, "coststamina", 0);
    def.exec_delay = doc.attribute_int(n, "execdelay", 0);
    def.priority = doc.attribute_int(n, "priority", 0);
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
    def.on_add_remove.assign(doc.attribute(n, "onaddremovescript"));
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
    // The point: the list is a script value only once the command is launched
    // (`launch_chunk`).
    case CommandArgKind::list_point: return pack_point(point);
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
  set_commanded(id, false);
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
    // **A class's methods are keyed on the name and the script's signature**,
    // as the loader keys them (0x0059a830 builds the key, 0x0059ece0 finds or
    // inserts it, 0x0059ad40 looks it up the same way): `Unit` binds `attack`
    // on a unit (`unit_attack.vs`, with its verifier) and `attack` on the
    // ground (`unit_advance.vs`), and both are its methods. The ancestry is
    // merged root first, a declaration replacing the inherited one of its own
    // name and signature -- `Bear`'s `attack` on a unit replaces `Unit`'s
    // and leaves its ground form to its own second declaration -- and
    // appended after the name's others when its signature is new. A
    // signature is read only when a name is declared again, since only then
    // does it decide anything; one that cannot be read matches, which is the
    // name-only merge this used to be.
    const std::vector<ClassIndex> chain = graph.ancestry(index);
    for (auto link = chain.rbegin(); link != chain.rend(); ++link) {
      for (const ClassMethod& method : graph.at(*link).methods) {
        MethodEntry entry;
        entry.sig.assign(method.sig);
        entry.script.assign(method.vs);
        entry.onfinish.assign(method.onfinish);
        auto row = built.methods.end();
        auto last_of_name = built.methods.end();
        for (auto it = built.methods.begin(); it != built.methods.end(); ++it) {
          if (!equal_fold(it->sig, entry.sig)) continue;
          last_of_name = it;
          if (row != built.methods.end()) continue;
          const bool known = (it->shaped || shape_of(*it)) && (entry.shaped || shape_of(entry));
          if (!known || it->shape == entry.shape) row = it;
        }
        if (row != built.methods.end()) {
          *row = std::move(entry);
        } else if (last_of_name != built.methods.end()) {
          built.methods.insert(last_of_name + 1, std::move(entry));
        } else {
          const auto at = std::upper_bound(
              built.methods.begin(), built.methods.end(), entry.sig,
              [](const std::string& needle, const MethodEntry& e) { return less_fold(needle, e.sig); });
          built.methods.insert(at, std::move(entry));
        }
      }
    }
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

const CommandSystem::MethodEntry* CommandSystem::method_row(const ClassMethods& methods,
                                                            std::string_view verb,
                                                            CommandArgKind kind) {
  const MethodEntry* first = method_row(methods, verb);
  if (first == nullptr) return nullptr;
  const MethodEntry* end = methods.methods.data() + methods.methods.size();
  if (first + 1 == end || !equal_fold(first[1].sig, verb)) return first;
  std::string_view wanted;
  switch (kind) {
    case CommandArgKind::none: wanted = ""; break;
    case CommandArgKind::point: wanted = "point"; break;
    case CommandArgKind::object: wanted = "obj"; break;
    case CommandArgKind::list_point: wanted = "objlist,point"; break;
  }
  for (const MethodEntry* row = first; row != end && equal_fold(row->sig, verb); ++row) {
    if (!row->shaped || row->shape == wanted) return row;
  }
  return nullptr;
}

bool CommandSystem::shape_of(MethodEntry& entry) const {
  if (scheduler_ == nullptr || entry.script.empty()) return false;
  const std::uint32_t index = library_ != nullptr ? library_->chunk_for(entry.script)
                                                  : scheduler_->find_chunk(entry.script);
  if (index == script::kNoChunk || index >= scheduler_->chunk_count()) return false;
  const script::Chunk& chunk = scheduler_->chunk(index);
  std::string shape;
  for (std::size_t i = 1; i < chunk.parameter_count && i < chunk.locals.size(); ++i) {
    if (i > 1) shape.push_back(',');
    for (const char c : chunk.locals[i].type_name) {
      shape.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  entry.shape = std::move(shape);
  entry.shaped = true;
  return true;
}

std::string_view CommandSystem::script_for(World& world, ObjectId id,
                                           std::string_view verb) const {
  const ClassGraph* graph = world.class_graph();
  const WorldObject* slot = world.find(id);
  if (graph == nullptr || slot == nullptr || slot->class_index == kNoClass) return {};
  const MethodEntry* row = method_row(methods_for(*graph, slot->class_index), verb);
  return row == nullptr ? std::string_view{} : std::string_view{row->script};
}

std::string_view CommandSystem::script_for(World& world, ObjectId id, std::string_view verb,
                                           CommandArgKind kind) const {
  const ClassGraph* graph = world.class_graph();
  const WorldObject* slot = world.find(id);
  if (graph == nullptr || slot == nullptr || slot->class_index == kNoClass) return {};
  const MethodEntry* row = method_row(methods_for(*graph, slot->class_index), verb, kind);
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
  const MethodEntry* row =
      method_row(methods_for(*graph, slot->class_index), command.verb, command.arg_kind);
  if (row == nullptr || row->onfinish.empty()) return;
  const std::uint32_t chunk = library_ != nullptr ? library_->chunk_for(row->onfinish)
                                                  : scheduler_->find_chunk(row->onfinish);
  if (chunk == script::kNoChunk) return;
  // `// void, Obj This, bool bCanceled` -- every shipped `onfinish` file.
  const script::Value args[2] = {script::Value::object(kTypeObj, id),
                                 script::Value::boolean(canceled)};
  // **Run now, to its end, inside whatever ended the command** (0x005b19f0
  // calls 0x0059af00, which finds the class's `onfinish` by the verb and runs
  // it through the synchronous launcher 0x006a0360), with the ended command's
  // row strings and costs ambient for the length of the call (0x005b19f0
  // writes them into the globals from the head's row and puts them back
  // after). It was spawned and kept on a list until it ran, a pass late. No
  // shipped `onfinish` suspends.
  const Command* outer = hooked_;
  hooked_ = &command;
  const script::CallReport ran = scheduler_->call(chunk, args);
  hooked_ = outer;
  if (ran.status == script::ExecStatus::failed) {
    hook_traps_.push_back(
        script::FailedScript{ran.id, scheduler_->chunk(chunk).source_name, ran.trap});
  }
}

void CommandSystem::retire(World& world, ObjectId id, Command& command, bool canceled) {
  retire(command);
  // A copy: the `onfinish` runs inside this call and can reach the queue the
  // command stands in.
  if (command.started) {
    const Command over = command;
    finish_command(world, id, over, canceled);
  }
}

bool CommandSystem::launch(World& world, ObjectId id, Command& command, GameTime now) {
  command.started = true;
  command.started_at = now;
  if (scheduler_ == nullptr) return true;  // queued, running, never retires
  return launch_chunk(world, id, command,
                      chunk_of(world, id, command.verb, command.arg_kind, /*count_miss=*/true));
}

std::uint32_t CommandSystem::chunk_of(World& world, ObjectId id, std::string_view verb,
                                      CommandArgKind kind, bool count_miss) {
  const std::string_view path = script_for(world, id, verb, kind);
  if (path.empty()) return script::kNoChunk;

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
  if (chunk == script::kNoChunk && count_miss) {
    // Counted rather than dropped: a verb whose file will not compile is a
    // broken installation or a missing behaviour, and it used to be invisible.
    if (const auto at = launch_failures_.find(std::string(verb)); at != launch_failures_.end()) {
      ++at->second;
    } else {
      launch_failures_.emplace(std::string(verb), std::size_t{1});
    }
  }
  return chunk;
}

bool CommandSystem::launch_chunk(World& world, ObjectId id, Command& command, std::uint32_t chunk) {
  if (chunk == script::kNoChunk) return false;

  // The receiver first, the command's argument second: exactly what the
  // shipped scripts declare. `UNIT_MOVE.VS` opens `// void, Obj This, point pt`
  // and `UNIT_ENTER.VS` `// void, Obj This, Obj bld`.
  if (command.arg_kind == CommandArgKind::list_point) {
    // `SHIP_UNBOARD.VS`'s `// void, Obj This, ObjList ol, point pt`: the list
    // is a temporary of the script it is handed to, and goes with it.
    ObjListPool& pool = objlist_pool_of(world);
    const ObjListId list = pool.acquire_temporary(script::kNoScript);
    if (std::vector<ObjectId>* items = pool.mutable_items(list)) *items = command.list;
    const script::Value args[3] = {script::Value::object(kTypeObj, id), make_objlist_value(list),
                                   pack_point(command.point)};
    command.script = scheduler_->spawn(chunk, args, script::ObjectRef{kTypeObj, id});
    if (command.script == script::kNoScript) {
      pool.release_script(script::kNoScript);
      return false;
    }
    pool.adopt(list, command.script);
    return true;
  }
  script::Value args[2] = {script::Value::object(kTypeObj, id), command.argument()};
  const std::span<const script::Value> window(
      args, command.arg_kind == CommandArgKind::none ? std::size_t{1} : std::size_t{2});
  command.script = scheduler_->spawn(chunk, window, script::ObjectRef{kTypeObj, id});
  if (command.script == script::kNoScript) return false;
  return true;
}

/// **One script holds an object's command slot, and its `idle` is a command
/// in its queue from the moment it exists.** `gbr.exe`'s object start
/// virtual (0x005aec40, which every factory path calls as an object is
/// constructed, and the unit, building and other overrides 0x005d3090,
/// 0x004d64d0, 0x004db2d0, 0x0053c730 call first) opens with the object's own
/// script-end virtual for slot 0 (`vtbl+0x1c` with 0, 0x005b5160): with
/// nothing queued there is no `onfinish` and nothing to pop, and 0x005b4f20
/// finds the queue empty, builds the class's `idle` (0x005aef20, drawing a
/// command id), pushes it (0x005b4960) and starts it. Only then does it spawn
/// the class's behaviours into slots 1.. (0x006a07e0). So every live object's
/// queue holds a running command -- the desync dumps say the same from the
/// outside: every live object's queue has an entry, and the only empty ones
/// are the dying units' -- and an order displaces the idle as it displaces
/// any other head.
///
/// This engine started that first `idle` straight on the scheduler, outside
/// any queue, after every object's behaviours, and ended it when the object
/// launched its first command (`end_resting_idle`); an `AddCommand` behind it
/// started at the next pass, where the original's waits behind the running
/// `idle` until something ends it, and `CmdCount` and `command` answered for
/// an empty queue.
bool CommandSystem::start_slot(World& world, ObjectId id) {
  if (world.find(id) == nullptr) return false;
  // Only an object whose class binds the default verb: anything else would
  // keep an empty queue the pass walks for nothing.
  if (default_verb_.empty() || script_for(world, id, default_verb_).empty()) return false;
  if (const CommandQueue* q = find(id); q != nullptr && !q->entries.empty()) return false;
  // With nothing queued, the end has no `onfinish` to run and nothing to pop,
  // and goes straight to 0x005b4f20. **Its dead test (`vtbl+0x50`) is not
  // asked here, a labelled reading:** whether an object's health is written
  // before its start virtual runs was not traced, and an object with no
  // health yet (an `Object` whose class declares none) would otherwise never
  // be given its `idle`, which every one of them was given before.
  (void)queue(id);
  start_head(world, id);
  const CommandQueue* q = find(id);
  return q != nullptr && !q->entries.empty() && q->entries.front().script != script::kNoScript;
}

void CommandSystem::service(World& world, Entry& entry, GameTime now) {
  // **What is left for the pass.** The original never waits for one: a
  // command's end starts the next there and then (`end_slot`), and an object
  // always has a head. The pass catches what this engine can still leave
  // behind -- a head whose script was ended by something that told the queue
  // nothing (`AIBreakScript` on a command's thread, a scheduler without the
  // step hook, as unit fixtures run it) and a head queued on an empty queue,
  // which only an object with no `idle` method has -- and settles it the
  // same way, a pass late.
  (void)now;
  std::vector<Command>& entries = entry.queue.entries;
  if (!entries.empty() && entries.front().started) {
    if (scheduler_ == nullptr) return;  // running, by definition, forever
    const Command& head = entries.front();
    if (head.script != script::kNoScript && scheduler_->alive(head.script)) return;
    // The script returned (or trapped, or was killed). The command is over,
    // and its `onfinish`, if the class binds one, runs with `bCanceled` false.
    end_slot(world, entry.id, /*canceled=*/false);
    return;
  }
  start_head(world, entry.id, /*from_pass=*/true);
}

void CommandSystem::start_head(World& world, ObjectId id, bool from_pass) {
  // 0x005b4f20, the command advance: the head starts, or -- with nothing
  // queued -- the class's `idle` is looked up (0x005aef20, which builds the
  // command and so draws its id), pushed (0x005b4960) and started. Its name
  // becomes the running command's (`[obj+0x10c]`).
  //
  // No scheduler: a head is marked running and never retires, which is what
  // a test of queue mechanics wants.
  bool refilled = false;
  while (true) {
    CommandQueue* q = find(id);
    if (q == nullptr) return;
    if (q->entries.empty()) {
      if (refilled || default_verb_.empty() || scheduler_ == nullptr) break;
      if (script_for(world, id, default_verb_).empty()) break;
      Command fallback;
      fallback.id = world.next_command_id();
      fallback.verb = default_verb_;
      q->entries.push_back(std::move(fallback));
      refilled = true;
      continue;
    }
    Command& head = q->entries.front();
    if (head.started) break;
    if (scheduler_ == nullptr) {
      (void)launch(world, id, head, world.time());
      break;
    }
    const std::uint32_t chunk = chunk_of(world, id, head.verb, head.arg_kind, /*count_miss=*/from_pass);
    if (chunk != script::kNoChunk) {
      head.started = true;
      head.started_at = world.time();
      if (launch_chunk(world, id, head, chunk) || !from_pass) break;
    } else if (!from_pass) {
      // A verb the class binds no compiled script for is left for the pass,
      // which counts it and retires it once, as it always has.
      break;
    }
    // The class binds no script for this verb. Retiring is the only safe
    // answer: leaving it at the head would block the queue forever, and
    // retrying it every turn would do the same more slowly. (In the original
    // every bound method has its script; this is the engine's.)
    q->entries.erase(q->entries.begin());
  }
  track_formation(world, id);
}

void CommandSystem::end_slot(World& world, ObjectId id, bool canceled, bool start_next) {
  // 0x005b5160, the object's script-end virtual (`vtbl+0x1c`) for slot 0 --
  // what a command's thread ending calls, whether it returned (0x0069f7a0,
  // after the slice) or was ended by another script (0x0069fee0) -- and its
  // first half 0x005b19f0: the head's `onfinish`, then the head popped, then,
  // unless the object is dead (`vtbl+0x50`), the next one started (0x005b4f20).
  CommandQueue* q = find(id);
  if (q != nullptr && !q->entries.empty()) {
    Command& head = q->entries.front();
    // The thread is gone before the hook runs (0x00687ce0 / 0x0069fee0 delete
    // it first), so while the `onfinish` runs the slot is empty and an ending
    // of index 0 inside it does nothing (`remove_at`).
    retire(head);
    const Command over = head;
    ending_.push_back(id);
    finish_command(world, id, over, canceled);
    ending_.pop_back();
    // 0x005b5160 pops whatever stands at the front: nothing in the `onfinish`
    // can have taken it, since index 0 cannot be ended without a thread.
    q = find(id);
    if (q != nullptr && !q->entries.empty()) q->entries.erase(q->entries.begin());
    track_formation(world, id);
  }
  if (!start_next) return;
  const WorldObject* slot = world.find(id);
  if (slot == nullptr || object_is_dead(*slot)) return;
  start_head(world, id);
}

void CommandSystem::after_slice(World& world, script::ScriptId script) {
  // 0x0069f7a0: once a thread's slice is over and it has finished or failed,
  // its message is deleted and the owner's `vtbl+0x1c` is told -- for a
  // command's thread, `end_slot`, with the cancel flag down. A thread that
  // ended by its own `KillCommand` (the stop flag) ends here too.
  if (scheduler_ == nullptr || script == script::kNoScript) return;
  const script::ScriptRecord* record = scheduler_->find(script);
  if (record == nullptr || !record->dead || record->owner.type != kTypeObj) return;
  const ObjectId id = record->owner.id;
  const CommandQueue* q = find(id);
  if (q == nullptr || q->entries.empty() || q->entries.front().script != script) return;
  // **A command whose script trapped is ended, and the next waits for the
  // command pass -- an engine choice, not the original's rule.** 0x0069f7a0
  // treats a failed thread as a finished one (`vtbl+0x1c` with `failed` set)
  // and 0x005b4f20 starts the next at once, on the same millisecond: the
  // wheel's insert (0x00688060) points the sweep at a thread armed on the
  // millisecond being swept, so nothing in the original bounds a command that
  // ends at once followed by one that hands back at once. Nothing has to:
  // its interpreter does not stop a thread where this one traps -- an
  // integer division by zero prints "Division by zero" and pushes the
  // dividend (0x00694ac0), for one -- so its threads end only where their
  // scripts end. Here a trap is this engine's, and a script that traps on
  // its first slice, every time, under an `idle` that queues it again and
  // returns, spun one pass for ever: `EAGLE_IDLE.VS` queues `move` and
  // returns, and `EAGLE_MOVE.VS` divides by `GetAnimDuration(0)`, which is 0
  // in a session built without entities (`imconform`'s thin session). Its
  // next command starts a pass later, as every command's did before the
  // end was made synchronous.
  const bool trapped = record->execution.status == script::ExecStatus::failed;
  end_slot(world, id, /*canceled=*/false, /*start_next=*/!trapped);
}

void CommandSystem::advance(World& world, const Turn& turn) {
  // Ascending id, which is spawn order. `service` never inserts a queue, so the
  // vector cannot move under this walk.
  for (Entry& entry : queues_) {
    service(world, entry, turn.time);
    track_formation(world, entry);
  }
}

void CommandSystem::track_formation(World& world, ObjectId id) {
  const std::size_t at = lower_bound(id);
  if (at < queues_.size() && queues_[at].id == id) track_formation(world, queues_[at]);
}

void CommandSystem::track_formation(World& world, Entry& entry) {
  const Command* head = entry.queue.running();
  const bool standing =
      head != nullptr && head->started && head->verb == kStandPositionCommand;
  if (standing == entry.standing) return;
  entry.standing = standing;
  // 0x005b4f20: the command advance rebuilds a unit whose command named
  // `stand_position` as it ends, and again as one starts, which is when the
  // party formation's bonus comes and goes (`docs/formats/unit-stats.md`).
  recalc_bonuses(world, entry.id);
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

bool CommandSystem::run_add_remove(ObjectId id, const Command& command, bool add) {
  if (scheduler_ == nullptr || command.name.empty()) return true;
  const CommandDef* row = table_.find(command.name);
  if (row == nullptr || row->on_add_remove.empty()) return true;
  const std::uint32_t chunk = library_ != nullptr ? library_->chunk_for(row->on_add_remove)
                                                  : scheduler_->find_chunk(row->on_add_remove);
  if (chunk == script::kNoChunk) return true;
  // `// bool, Obj This, bool bAdd` -- the shipped script's own header, and the
  // two words 0x004e7600 hands the interpreter.
  const script::Value args[2] = {script::Value::object(kTypeObj, id),
                                 script::Value::boolean(add)};
  // The row's `cmdparam` and costs are ambient for the length of the call and
  // no longer, as the original's globals are; nested, the outer one comes back.
  const Command* outer = hooked_;
  hooked_ = &command;
  const script::CallReport ran = scheduler_->call(chunk, args);
  hooked_ = outer;
  if (ran.status == script::ExecStatus::failed) {
    hook_traps_.push_back(
        script::FailedScript{ran.id, scheduler_->chunk(chunk).source_name, ran.trap});
  }
  // The answer starts out true and only a returned value overwrites it.
  if (ran.status != script::ExecStatus::finished || ran.result.is_nil()) return true;
  return ran.result.truthy_scalar();
}

bool CommandSystem::accept(World& world, ObjectId id, const Command& command) {
  if (!charge(world, id, command)) return false;
  if (!run_add_remove(id, command, /*add=*/true)) {
    // Refused by the row's script after it was paid for: the payment goes back
    // (0x005b18bc, `vtbl+0x88`), and nothing is queued.
    settle(world, id, command, /*refund=*/true);
    return false;
  }
  // **A `plrresearch` row is held once per player** (0x005b1883..0x005b18ad):
  // after the payment and the script, a set on the owner's player record
  // (`[obj+0x70]`, keyed by the row's index `[def+0x1e4]`) refuses the row
  // while it is up and is raised otherwise; the release (0x005b18d0) and the
  // pop of a finished head (0x005b5160) lower it. So a second tavern cannot
  // start `Tribute` while the first has it queued. The refusal returns
  // **without** putting the payment back -- the original's own order, kept.
  //
  // Derived here rather than stored: the set is up exactly while some object
  // of that player holds a command of the row, which a walk of the queues
  // answers. **One difference, labelled:** the original never lowers it for
  // an object destroyed with the row queued (its queue is not released), and
  // this walk forgets such an object with its queue.
  const CommandDef* row = command.name.empty() ? nullptr : table_.find(command.name);
  if (row == nullptr || !row->plr_research) return true;
  const ObjectState* self = world.state(id);
  const PlayerId owner = self == nullptr ? kNoPlayer : self->owner;
  for (const Entry& entry : queues_) {
    const ObjectState* other = world.state(entry.id);
    if (other == nullptr || other->owner != owner) continue;
    for (const Command& held : entry.queue.entries) {
      if (equal_fold(held.name, row->name)) return false;
    }
  }
  return true;
}

void CommandSystem::release(World& world, ObjectId id, const Command& command) {
  settle(world, id, command, /*refund=*/true);
  // 0x005b07d0 calls `vtbl+0x90` only for a row with a cost (`[def+0x1e8]`,
  // `[def+0x1ec]`, `[def+0x1f0]`), and the hook is inside it.
  const CommandDef* row = command.name.empty() ? nullptr : table_.find(command.name);
  if (row == nullptr || (row->cost_gold == 0 && row->cost_food == 0 && row->cost_pop == 0)) {
    return;
  }
  (void)run_add_remove(id, command, /*add=*/false);
}

bool CommandSystem::remove_at(World& world, ObjectId id, std::size_t index) {
  CommandQueue* q = find(id);
  if (q == nullptr || index >= q->entries.size()) return false;
  // **0x005b07d0 does nothing at all without a thread in the command slot**
  // (0x0069f950 with 0): no release, no end. The one moment a live object's
  // slot is empty is inside its own end, while the ended command's
  // `onfinish` runs (`end_slot`), so a `KillCommand` or a replace there
  // leaves the head for the end to pop.
  if (std::find(ending_.begin(), ending_.end(), id) != ending_.end()) return false;
  // Released while it still stands in the queue, as 0x005b07d0 does: a copy,
  // because the hook can reach this queue.
  const Command released = q->entries[index];
  release(world, id, released);
  q = find(id);
  if (q == nullptr) return true;
  std::size_t at = index;
  if (at >= q->entries.size() || q->entries[at].id != released.id) {
    const auto found = std::find_if(q->entries.begin(), q->entries.end(),
                                    [&](const Command& c) { return c.id == released.id; });
    if (found == q->entries.end()) return true;
    at = static_cast<std::size_t>(found - q->entries.begin());
  }
  if (at != 0) {
    // Anything behind the head never ran and is erased (`vtbl+0x0(1)`, the
    // command's destructor), its `onfinish` never told.
    retire(q->entries[at]);
    q->entries.erase(q->entries.begin() + static_cast<std::ptrdiff_t>(at));
    return true;
  }
  Command& head = q->entries.front();
  const bool live = scheduler_ != nullptr && head.started && head.script != script::kNoScript &&
                    scheduler_->alive(head.script);
  const bool own = live && head.script == caller_;
  if (own && scheduler_->stop(head.script)) {
    // **Ended by its own script**: 0x0069fee0 finds the thread is the one
    // running and only raises its stop flag (`[ctx+0x40]`). The script ends as
    // the host call returns, and its end (0x0069f7a0 -> `after_slice`) pops it
    // with the cancel flag (0x009bdb18) down, so its `onfinish` is told
    // `false`, and starts the next. Until then it stays at the head.
    return true;
  }
  if (live) {
    // **Ended by another script**: 0x0069fee0 deletes the thread and calls the
    // owner's `vtbl+0x1c` there and then, under the cancel flag: the
    // `onfinish` told `true`, the head popped, the next started -- all inside
    // the call that ended it.
    // (A script ending its own head from outside its slice -- a test's host
    // call, nothing shipped -- has no stop flag to raise and is ended here,
    // told it finished, as the stop would have told it.)
    end_slot(world, id, /*canceled=*/!own);
    return true;
  }
  // A head with no thread to end. In the original a live object's head
  // always has one; here it is a head queued on an empty queue and not
  // started yet, or any head with no scheduler at all. It goes unrun -- its
  // `onfinish` told it was cancelled if it had started -- and what follows it
  // is left to the caller, or to the pass.
  retire(world, id, head, /*canceled=*/true);
  q = find(id);
  if (q != nullptr && !q->entries.empty() && q->entries.front().id == released.id) {
    q->entries.erase(q->entries.begin());
  }
  track_formation(world, id);
  return true;
}

std::uint32_t CommandSystem::set_command(World& world, ObjectId id, std::string_view verb,
                                         const Command& prototype) {
  if (world.find(id) == nullptr) return 0;
  (void)queue(id);

  // The command takes its id when it is made, before the insert can refuse it
  // (0x00599302 in the constructor), so a refused one has used its id.
  Command command = prototype;
  command.id = world.next_command_id();
  command.verb.assign(verb);
  return set_made(world, id, std::move(command));
}

std::uint32_t CommandSystem::set_made(World& world, ObjectId id, Command command) {
  if (world.find(id) == nullptr) return 0;
  (void)queue(id);
  command.script = script::kNoScript;
  command.started = false;
  const std::uint32_t issued = command.id;
  // Paid for, and its row's script asked, before the queue it replaces is
  // refunded (0x005b4e90 asks the accept test first), so a replace cannot
  // spend what the commands it clears are holding, and a refused one leaves
  // them running.
  if (!accept(world, id, command)) return 0;

  // Aborts what is running, which `AI HELPERS\GUARD.VS` requires: it issues
  // `SetCommand("move", pt)` to a unit whose running command is `idle`, and
  // `UNIT_IDLE.VS` is a `while(1)` that never returns on its own. A running
  // command that is replaced is a cancelled one: its `onfinish` gets `true`
  // and its cost comes back. In the original's order (0x005b4e90 with the
  // flag): the pending tail last first (`vtbl+0xc0(0)`, 0x005b1120), the new
  // command pushed, then the old head (0x005b07d0 with 0).
  const CommandQueue* q = find(id);
  const bool had_head = q != nullptr && !q->entries.empty();
  const std::uint32_t head = had_head ? q->entries.front().id : 0u;
  for (std::size_t i = had_head ? q->entries.size() : 0; i-- > 1;) (void)remove_at(world, id, i);
  queue(id).entries.push_back(std::move(command));
  q = find(id);
  // **And the new command starts now, inside the call.** Ending the head
  // (0x005b07d0) ends its thread and starts the next command through
  // 0x005b4f20 there and then (`end_slot`), and the command's script is a
  // thread armed at the wheel's clock (0x00688240), so it runs in this same
  // pass as soon as the script that gave the order yields. Leaving the start
  // to the next command pass, as this once did, opened a window the shipped
  // scripts walk into: `OUTPOST_BEHAVIOR.VS` orders a garrisoned defender out
  // with `guardadvance`, `Sleep(200)`s and looks again for one still inside
  // -- and with the order not yet started the same defender was still there,
  // was ordered again (which cancels the order that never ran), and never
  // left. When the head is the calling script's own, it ends as the call
  // returns, and the new command starts then.
  if (had_head && q->entries.size() > 1 && q->entries.front().id == head) {
    (void)remove_at(world, id, 0);
  }
  // The new command at the head of a queue that had none, or behind a head
  // that had no thread to end: it starts, as the end would have started it.
  if (const CommandQueue* now_q = find(id); now_q != nullptr && !now_q->entries.empty() &&
                                            now_q->entries.front().id == issued &&
                                            !now_q->entries.front().started) {
    start_head(world, id);
  }
  track_formation(world, id);
  return issued;
}

std::uint32_t CommandSystem::add_command(World& world, ObjectId id, bool front,
                                         std::string_view verb, const Command& prototype) {
  if (world.find(id) == nullptr) return 0;
  (void)queue(id);

  Command command = prototype;
  command.id = world.next_command_id();
  command.verb.assign(verb);
  return insert_made(world, id, front, std::move(command));
}

std::uint32_t CommandSystem::insert_made(World& world, ObjectId id, bool front, Command command) {
  if (world.find(id) == nullptr) return 0;
  (void)queue(id);
  command.script = script::kNoScript;
  command.started = false;
  const std::uint32_t issued = command.id;
  if (!accept(world, id, command)) return 0;

  // `front` inserts at index 1 -- behind the running command, ahead of the rest
  // -- and never at index 0. See the header: the `AddCommand(true, ...);
  // KillCommand();` idiom only works if the runner survives the insert. Found
  // again after the accept test, whose script could have added a queue.
  CommandQueue& q = queue(id);
  const std::size_t at = front && !q.entries.empty() ? std::size_t{1} : q.entries.size();
  q.entries.insert(q.entries.begin() + static_cast<std::ptrdiff_t>(at), std::move(command));
  return issued;
}

std::uint32_t CommandSystem::append_order(World& world, ObjectId id, std::string_view verb,
                                          const Command& prototype) {
  if (world.find(id) == nullptr) return 0;
  Command command = prototype;
  command.id = world.next_command_id();
  command.verb.assign(verb);
  return append_made(world, id, std::move(command));
}

std::uint32_t CommandSystem::append_made(World& world, ObjectId id, Command command) {
  // A resting head gives way; anything else keeps running and the order waits
  // its turn behind it, as `ExecCmd`'s five-deep training queue needs. See the
  // header for why this is here and not in `add_command`. The order is paid for
  // and pushed first and the head ended after, as 0x005b4e90 does, so an order
  // the settlement cannot pay for leaves the resting head alone.
  const CommandQueue* before = find(id);
  const bool resting = before != nullptr && !before->entries.empty() &&
                       equal_fold(before->entries.front().verb, default_verb_);
  const std::uint32_t head = resting ? before->entries.front().id : 0u;
  const bool was_empty = before == nullptr || before->entries.empty();
  const std::uint32_t issued = insert_made(world, id, /*front=*/false, std::move(command));
  if (issued == 0) return issued;
  if (was_empty) {
    // An empty queue: the order is the head, and starts now, as it would
    // have started once the head it was appended behind was ended.
    start_head(world, id);
    return issued;
  }
  if (!resting) return issued;
  const CommandQueue* q = find(id);
  if (q != nullptr && !q->entries.empty() && q->entries.front().id == head) {
    (void)remove_at(world, id, 0);
  }
  return issued;
}

bool CommandSystem::kill_command(World& world, ObjectId id) {
  return remove_at(world, id, 0);
}

std::size_t CommandSystem::clear_commands(World& world, ObjectId id) {
  // 0x005b1120 with 0: every index from the last down to 1, each through
  // 0x005b07d0, so the last queued is released first.
  const CommandQueue* q = find(id);
  if (q == nullptr || q->entries.size() <= 1) return 0;
  std::size_t dropped = 0;
  for (std::size_t i = q->entries.size(); i-- > 1;) {
    if (remove_at(world, id, i)) ++dropped;
  }
  return dropped;
}

bool CommandSystem::cancel_command(World& world, ObjectId id, std::uint32_t command_id) {
  const CommandQueue* q = find(id);
  if (q == nullptr || command_id == 0) return false;
  const auto at = std::find_if(q->entries.begin(), q->entries.end(),
                               [command_id](const Command& c) { return c.id == command_id; });
  if (at == q->entries.end()) return false;
  return remove_at(world, id, static_cast<std::size_t>(at - q->entries.begin()));
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
  // Inside a row's `onaddremovescript`, `groupdispatch`, immediate method or
  // an ended command's `onfinish`, none of which is a queued command's own
  // script: the command it was run for,
  // as the original's ambient globals say for as long as the call lasts.
  return hooked_;
}

std::size_t CommandSystem::command_count_named(ObjectId id, std::string_view row) const noexcept {
  const CommandQueue* q = find(id);
  if (q == nullptr) return 0;
  std::size_t n = 0;
  for (const Command& command : q->entries) {
    if (!command.name.empty() && command.name == row) ++n;
  }
  return n;
}

bool CommandSystem::binds(World& world, ObjectId id, std::string_view verb, CommandArgKind kind) {
  const std::string path(script_for(world, id, verb, kind));
  if (path.empty()) return false;
  if (scheduler_ == nullptr) return true;
  const std::uint32_t chunk_index = library_ != nullptr ? library_->chunk_for(path)
                                                        : scheduler_->find_chunk(path);
  if (chunk_index == script::kNoChunk || chunk_index >= scheduler_->chunk_count()) return true;
  const script::Chunk& chunk = scheduler_->chunk(chunk_index);
  // The receiver is parameter 0; the template's one argument, if any, is 1.
  const auto second_is = [&](std::string_view type) {
    return chunk.parameter_count == 2 && chunk.locals.size() > 1 &&
           equal_fold(chunk.locals[1].type_name, type);
  };
  switch (kind) {
    case CommandArgKind::none: return chunk.parameter_count == 1;
    case CommandArgKind::point: return second_is("point");
    case CommandArgKind::object: return second_is("Obj");
    case CommandArgKind::list_point:
      return chunk.parameter_count == 3 && chunk.locals.size() > 2 &&
             equal_fold(chunk.locals[1].type_name, "ObjList") &&
             equal_fold(chunk.locals[2].type_name, "point");
  }
  return false;
}

bool CommandSystem::commanded(ObjectId id) const noexcept {
  return std::binary_search(commanded_.begin(), commanded_.end(), id);
}

void CommandSystem::set_commanded(ObjectId id, bool on) {
  const auto at = std::lower_bound(commanded_.begin(), commanded_.end(), id);
  const bool held = at != commanded_.end() && *at == id;
  if (on && !held) commanded_.insert(at, id);
  if (!on && held) commanded_.erase(at);
}

namespace {

/// A command carrying `def` -- its name, parameter, costs and delay -- the
/// shape the per-object issue gives every command it builds (`[cmd+0x2c]`).
Command row_command(const CommandDef& def) {
  Command command;
  command.param = def.param;
  command.name = def.name;
  command.cost_gold = def.cost_gold;
  command.cost_food = def.cost_food;
  command.cost_pop = def.cost_pop;
  command.cost_stamina = def.cost_stamina;
  command.delay = def.exec_delay;
  return command;
}

}  // namespace

bool CommandSystem::dispatch(World& world, std::span<const ObjectId> actors, const CommandDef& def,
                             Point point, ObjectId object, bool replace, bool modifier,
                             std::int32_t player_arg) {
  if (scheduler_ == nullptr || def.group_dispatch.empty()) return false;
  const std::uint32_t chunk = library_ != nullptr ? library_->chunk_for(def.group_dispatch)
                                                  : scheduler_->find_chunk(def.group_dispatch);
  if (chunk == script::kNoChunk || chunk >= scheduler_->chunk_count()) return false;
  ObjListPool& pool = objlist_pool_of(world);
  // The actors the row is enabled for (0x004ef589's `vtbl+0xd8`), in order.
  const ObjListId list = pool.acquire_temporary(script::kNoScript);
  if (std::vector<ObjectId>* items = pool.mutable_items(list)) {
    for (const ObjectId id : actors) {
      if (world.find(id) != nullptr && command_enabled(world, id, def.name)) items->push_back(id);
    }
  }
  const std::array<script::Value, 6> args{
      make_objlist_value(list),
      pack_point(point),
      object != kNoObject ? script::Value::object(kTypeObj, object)
                          : script::Value::object(script::ObjectRef{script::kNoType, 0}),
      script::Value::boolean(replace),
      script::Value::boolean(modifier),
      script::Value::integer(player_arg),
  };
  // The row is ambient for exactly the call, and the outer one comes back.
  const Command ambient = row_command(def);
  const Command* outer = hooked_;
  hooked_ = &ambient;
  const script::CallReport ran = scheduler_->call(chunk, args);
  hooked_ = outer;
  // The list it was handed is `kNoScript`'s, as a verifier's is, and goes now.
  pool.release_script(script::kNoScript);
  if (ran.status == script::ExecStatus::failed) {
    scheduler_->report_trap(
        script::FailedScript{ran.id, scheduler_->chunk(chunk).source_name, ran.trap});
  }
  return ran.id != script::kNoScript;
}

bool CommandSystem::run_method(World& world, ObjectId id, std::string_view verb,
                               const CommandDef& row) {
  if (scheduler_ == nullptr || world.find(id) == nullptr) return false;
  const std::string path(script_for(world, id, verb, CommandArgKind::none));
  const std::uint32_t chunk = path.empty()                ? script::kNoChunk
                              : library_ != nullptr ? library_->chunk_for(path)
                                                    : scheduler_->find_chunk(path);
  if (chunk == script::kNoChunk || chunk >= scheduler_->chunk_count()) return false;
  const script::Value self = script::Value::object(kTypeObj, id);
  const Command ambient = row_command(row);
  const Command* outer = hooked_;
  hooked_ = &ambient;
  const script::CallReport ran = scheduler_->call(chunk, std::span<const script::Value>(&self, 1));
  hooked_ = outer;
  if (ran.status == script::ExecStatus::failed) {
    scheduler_->report_trap(
        script::FailedScript{ran.id, scheduler_->chunk(chunk).source_name, ran.trap});
  }
  return true;
}

void CommandSystem::run_immediate(World& world, ObjectId id, Command command) {
  // The command was built, so it has its id (0x00599302), whatever follows.
  command.id = world.next_command_id();
  if (!accept(world, id, command)) return;
  if (scheduler_ == nullptr) return;
  const std::string path(script_for(world, id, command.verb, command.arg_kind));
  const std::uint32_t chunk = path.empty()                ? script::kNoChunk
                              : library_ != nullptr ? library_->chunk_for(path)
                                                    : scheduler_->find_chunk(path);
  if (chunk == script::kNoChunk || chunk >= scheduler_->chunk_count()) return;
  script::Value args[2] = {script::Value::object(kTypeObj, id), command.argument()};
  const std::span<const script::Value> window(
      args, command.arg_kind == CommandArgKind::none ? std::size_t{1} : std::size_t{2});
  // Run through the command's own `vtbl+0xc` (0x00598a70 -> 0x006a0360): to its
  // end, now, with the row ambient (0x004ef917..0x004ef9df). Never queued, so
  // never released: what it paid stays paid.
  const Command* outer = hooked_;
  hooked_ = &command;
  const script::CallReport ran = scheduler_->call(chunk, window);
  hooked_ = outer;
  if (ran.status == script::ExecStatus::failed) {
    scheduler_->report_trap(
        script::FailedScript{ran.id, scheduler_->chunk(chunk).source_name, ran.trap});
  }
}

bool CommandSystem::issue_research(World& world, ObjectId id, Command command) {
  const CommandQueue* q = find(id);
  // At most one command, and that one `idle` (case and all, 0x00429c60): an
  // ordinary order. So is an empty queue, which only an object whose class
  // binds no `idle` has.
  if (q == nullptr || q->entries.empty()) return false;
  if (q->entries.size() <= 1 && q->entries.front().verb == kDefaultCommandVerb) return false;

  command.id = world.next_command_id();
  const std::string head_name = q->entries.front().name;
  const CommandDef* head_row = head_name.empty() ? nullptr : table_.find(head_name);
  // Dropped unless the runner came in through a row that is not a research,
  // and only then is the accept test asked (0x004efa45..0x004efa7c).
  if (head_row == nullptr || head_row->research_command) return true;
  if (!accept(world, id, command)) return true;
  // The runner's script stops with no callback, its `onfinish` is told it was
  // cancelled, and the research goes in front of it and starts
  // (0x004efa82..0x004efaba). The runner keeps its place behind, unrefunded,
  // and starts again from its beginning when it reaches the head.
  CommandQueue& queue_ref = queue(id);
  if (queue_ref.entries.empty()) {
    queue_ref.entries.push_back(std::move(command));
    return true;
  }
  // 0x0069fee0 with no callback, the cancel flag up, 0x005b19f0 (the
  // runner's `onfinish`, told it was cancelled), the research pushed to the
  // front (0x004ebc90) and started (0x005b4f20), all inside the issue.
  const std::uint32_t runner_id = queue_ref.entries.front().id;
  retire(world, id, queue_ref.entries.front(), /*canceled=*/true);
  CommandQueue& after = queue(id);
  for (Command& runner : after.entries) {
    if (runner.id != runner_id) continue;
    runner.started = false;
    runner.script = script::kNoScript;
  }
  after.entries.insert(after.entries.begin(), std::move(command));
  track_formation(world, id);
  start_head(world, id);
  return true;
}

std::size_t CommandSystem::issue(World& world, std::span<const ObjectId> actors,
                                 const CommandDef& def, Point point, ObjectId object, bool replace,
                                 bool modifier, std::int32_t player_arg, bool user) {
  // Every unit handed the order is marked commanded first, enabled or not
  // (0x004ef410..0x004ef480): a unit by its own flag, a hero by his army's
  // (0x005f5520 applying 0x004e7890 to his formation). **The hero himself is
  // marked too -- an inference:** `TOWNHALL_AUTOTRAIN.VS` reads
  // `u.hero.GetCommanded`, which only answers anything if he is.
  const HeroSystem* heroes = hero_system_of(world);
  for (const ObjectId id : actors) {
    const WorldObject* slot = world.find(id);
    if (slot == nullptr || !slot->state.flags.is_unit) continue;
    set_commanded(id, true);
    const HeroRecord* record = heroes == nullptr ? nullptr : heroes->hero(id);
    if (record == nullptr) continue;
    for (const ObjectId member : record->army) set_commanded(member, true);
  }

  if (!def.group_dispatch.empty()) {
    return dispatch(world, actors, def, point, object, replace, modifier, player_arg) ? 1 : 0;
  }

  const std::string_view verb = def.method.empty() ? std::string_view{def.name} : def.method;
  std::size_t issued = 0;
  // The group-move set-up (0x004efada..0x004efbb3): asked once, of the first
  // actor that gets this far, when the list's first member is aboard a ship.
  const ObjectId ship = carrier_of_first(world, actors);
  bool set_up = false;
  bool give = true;
  for (const ObjectId id : actors) {
    if (world.find(id) == nullptr || !command_enabled(world, id, def.name)) continue;
    Command command = row_command(def);
    command.user = user;
    command.verb.assign(verb);
    // The method by shape: the object form when an object was given, then the
    // point form, then the bare form; none of them, and the actor is skipped
    // ("Cmd Not resolved to method").
    if (object != kNoObject && binds(world, id, verb, CommandArgKind::object)) {
      command.arg_kind = CommandArgKind::object;
      command.object = object;
    } else if (binds(world, id, verb, CommandArgKind::point)) {
      command.arg_kind = CommandArgKind::point;
      command.point = point;
    } else if (!binds(world, id, verb, CommandArgKind::none)) {
      continue;
    }
    if (def.immediate) {
      run_immediate(world, id, std::move(command));
      ++issued;
      continue;
    }
    if (def.research_command && issue_research(world, id, command)) {
      ++issued;
      continue;
    }
    // Made -- its id drawn -- before the set-up, which makes the ship's
    // `unboard` after it and inserts it first.
    command.id = world.next_command_id();
    if (!set_up) {
      set_up = true;
      if (!def.research_command && ship != kNoObject) {
        give = group_move_set_up(world, ship, actors, command.arg_kind, point, object, replace);
      }
    }
    // Refused by the set-up: every actor's command is made and dropped
    // (0x004efbb3 with `local_9c` 0), its id spent.
    if (!give) continue;
    const std::uint32_t made = replace && !def.train_command
                                   ? set_made(world, id, std::move(command))
                                   : append_made(world, id, std::move(command));
    if (made != 0) ++issued;
  }
  return issued;
}

ObjectId CommandSystem::carrier_of_first(const World& world, std::span<const ObjectId> actors) {
  // 0x004e9100: the list's first member, as a unit (`CVXUnit` cast), in a
  // holder (`[unit+0x154] != 0xffff`), and the object that holder belongs to
  // (0x005319a0, `[holder+0xe]`) -- a ship; a settlement's holder belongs to
  // none (`held_by_carrier`).
  if (actors.empty()) return kNoObject;
  const WorldObject* first = world.find(actors.front());
  if (first == nullptr || !first->state.flags.is_unit) return kNoObject;
  if (!held_by_carrier(world, first->id)) return kNoObject;
  return first->state.holder - 1;
}

bool CommandSystem::group_move_set_up(World& world, ObjectId ship,
                                      std::span<const ObjectId> actors, CommandArgKind kind,
                                      Point point, ObjectId object, bool replace) {
  // 0x004e8ae0: whether anybody gets the order, and whether the ship is to
  // put them ashore first.
  bool unboard = true;
  switch (kind) {
    case CommandArgKind::none:
    case CommandArgKind::list_point:
      // Nothing to sail to: nobody gets it.
      return false;
    case CommandArgKind::point:
      // 0x004e8a80: the terrain index at the point, half a cell rounded, is
      // deep water (13): nobody gets it.
      if (terrain_at(world.terrain(), point) == kDeepWaterIndex) return false;
      break;
    case CommandArgKind::object: {
      const WorldObject* target = world.find(object);
      if (target == nullptr) return true;
      // A water unit's class (`[class+0xb30]`, `water_unit`): nobody.
      if (class_flag(world, *target, "water_unit")) return false;
      if (target->state.flags.is_unit && target->state.is_held() &&
          target->state.holder != kNoObject) {
        // Held, by anything but this ship -- a settlement's holder belongs to
        // no object, so a garrisoned target is one -- refuses; held by this
        // one, everybody is already where the target is.
        if (!held_by_carrier(world, target->id) || target->state.holder - 1 != ship) return false;
        unboard = false;
      }
      break;
    }
  }
  if (!unboard) return true;

  // 0x004ef1c0: the passengers are the list, in order, then each hero's army
  // members not already in it (0x0050e2a0 finds, 0x0041f290 appends) ...
  std::vector<ObjectId> passengers(actors.begin(), actors.end());
  if (const HeroSystem* heroes = hero_system_of(world); heroes != nullptr) {
    for (const ObjectId id : actors) {
      const HeroRecord* record = heroes->hero(id);
      if (record == nullptr) continue;
      for (const ObjectId member : record->army) {
        if (std::find(passengers.begin(), passengers.end(), member) == passengers.end()) {
          passengers.push_back(member);
        }
      }
    }
  }
  // ... and the ship gets `unboard` (0x005aef20 kind 5) to the point, or to
  // the target's position (`vtbl+0x3c`): the order form (`vtbl+0xb8` with 1)
  // when the order replaces, else appended behind what it is doing
  // (`vtbl+0xbc` with 0, the `AddCommand` insert).
  //
  // **A ship whose class binds no kind-5 `unboard` gets nothing here, a
  // labelled reading:** 0x005aef20 answers null and 0x004ef1c0 hands the null
  // to the insert, whose accept test (`vtbl+0x8c`) was not traced with one.
  // Every shipped ship class that carries passengers binds it.
  if (!binds(world, ship, "unboard", CommandArgKind::list_point)) return true;
  Command unboard_command;
  unboard_command.id = world.next_command_id();
  unboard_command.verb = "unboard";
  unboard_command.arg_kind = CommandArgKind::list_point;
  unboard_command.point = kind == CommandArgKind::object ? world.resolve_position(object) : point;
  unboard_command.list = std::move(passengers);
  if (replace) {
    (void)set_made(world, ship, std::move(unboard_command));
  } else {
    (void)insert_made(world, ship, /*front=*/false, std::move(unboard_command));
  }
  return true;
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
  out.ids = receiver_objects(ctx, *out.world, receiver, accept_query);
  return out;
}

/// Records the calling script on the command system for one queue call
/// (`CommandSystem::set_caller`), and puts the previous one back.
struct CallerScope {
  CommandSystem& commands;
  script::ScriptId previous;
  CallerScope(CommandSystem& system, script::ScriptId script)
      : commands(system), previous(system.set_caller(script)) {}
  ~CallerScope() { (void)commands.set_caller(previous); }
  CallerScope(const CallerScope&) = delete;
  CallerScope& operator=(const CallerScope&) = delete;
};

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

/// Which of the three registrations a receiver reaches: `gbr.exe` registers
/// every queue member on `Obj`, `ObjList` and `Query` with bodies that differ
/// in how they treat a dead member and a missing method (below). A named
/// object orders the object it names, so it takes the `Obj` form; a bare
/// `<group>` name resolves to a query.
enum class ReceiverForm : std::uint8_t { object, list, query };

[[nodiscard]] ReceiverForm receiver_form(const Value& receiver) noexcept {
  if (receiver.is_string()) return ReceiverForm::query;
  if (!receiver.is_object()) return ReceiverForm::object;
  const script::TypeId type = receiver.as_object().type;
  if (type == kTypeObjList) return ReceiverForm::list;
  if (type == kTypeQuery) return ReceiverForm::query;
  return ReceiverForm::object;
}

/// `vtbl+0x50` on the base object (0x004e1f50): health `[obj+0xc0]` is zero.
[[nodiscard]] bool dead(const World& world, ObjectId id) noexcept {
  const ObjectState* state = world.state(id);
  return state == nullptr || state->health <= 0;
}

/// The list and query forms refuse the whole call when their object argument
/// names nothing ("Parameter #N ... is uninitialized or invalid object",
/// 0x00562800, 0x00578690, 0x00578870); the `Obj` form passes the handle on.
[[nodiscard]] bool object_argument_missing(World& world, ReceiverForm form,
                                           const Command& prototype) {
  return form != ReceiverForm::object && prototype.arg_kind == CommandArgKind::object &&
         world.find(prototype.object) == nullptr;
}

/// A script's `SetCommand` on one object (0x005b0a50 and its eight siblings):
/// the pending tail is cleared first (`vtbl+0xc0` with 0, 0x005b1120)
/// whatever follows; then the method is looked up by shape (`binds`); found,
/// the command is appended through `vtbl+0xbc` and the head ended (0x005b07d0
/// with 0) -- which is `CommandSystem::set_command` on the emptied tail. Not
/// found, the `Obj` form stops there and the head keeps running, while the
/// list and query forms end the head anyway (`end_head_on_miss`).
void script_set(World& world, CommandSystem& commands, ObjectId id, std::string_view verb,
                const Command& prototype, bool end_head_on_miss) {
  (void)commands.clear_commands(world, id);
  if (!commands.binds(world, id, verb, prototype.arg_kind)) {
    if (end_head_on_miss) (void)commands.kill_command(world, id);
    return;
  }
  (void)commands.set_command(world, id, verb, prototype);
}

/// `AddCommand(front, verb[, arg])`. 215 sites, the most used order in the
/// corpus.
///
/// `Obj::AddCommand` (0x005afe90, 0x005aff70, 0x005b0070, for no argument, a
/// point and an object) looks the verb up by shape (0x005aef20 with kind 0, 1
/// or 2; `CommandSystem::binds`) and on a miss queues nothing -- no command is
/// built, so no id is drawn. Found, it inserts through `vtbl+0xbc`
/// (0x005b5c30): the accept test, then index 1 for `front` and the back
/// otherwise. No dead test. The `ObjList` forms (0x00563020, 0x00561e30,
/// 0x00562800) skip a dead member; the `Query` forms (0x00577c30,
/// 0x00577f30, 0x00578690) refresh the query and take every member. Both list
/// forms do nothing at all when their object argument names nothing.
HostOutcome add_command_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  const CallerScope caller(*targets.commands, ctx.script);
  if (ctx.count() < 3 || !ctx.arg(2).is_string()) return HostOutcome::failed(kNoVerb);
  const bool front = ctx.arg(1).truthy_scalar();
  const std::string& verb = ctx.arg(2).as_string();
  const Command prototype = ctx.count() > 3 ? prototype_from(ctx.arg(3)) : Command{};
  const ReceiverForm form = receiver_form(ctx.arg(0));
  if (object_argument_missing(*targets.world, form, prototype)) return HostOutcome::ok_void();
  for (const ObjectId id : targets.ids) {
    if (form == ReceiverForm::list && dead(*targets.world, id)) continue;
    if (!targets.commands->binds(*targets.world, id, verb, prototype.arg_kind)) continue;
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
/// Otherwise it looks the verb up in its bare form (0x005aef20 with kind 0,
/// `binds`; a miss prints `SneakCommand: This object does not have a '%s'
/// command! Command not added.` and returns), and then: clones the running
/// command (its `vtbl+0x14`, 0x00599c40) and pushes the clone to the front
/// (`vtbl+0xbc` with 1), pushes the new command to the front the same way, and
/// ends the command at index 0 (0x005b07d0 with 0) -- the one that was
/// running. So `[run, rest...]` becomes `[verb, run', rest...]`: *do this now,
/// then pick up where you were*, which is what lets `convert` interrupt a march
/// and the march resume once the warrior is a peasant.
///
/// Here: the clone is a copy of the running `Command` less its coroutine;
/// `add_command(front)` twice is the two front pushes -- each lands at index
/// 1, behind the runner, so the second goes ahead of the first -- and
/// `kill_command` ends the runner. An empty queue is left empty, as the
/// original leaves it.
///
/// **The ids are drawn as the original draws them.** The lookup builds the
/// new command first (0x005aef20; its id drawn by the constructor, 0x00599302
/// through 0x0051d4b0), and the clone second: its constructor (0x00599210)
/// draws an id as every command's does, and the clone then takes the
/// runner's id and row over it (0x00599cdb). So the resumed command keeps the
/// id it had -- a cancel by id still finds it -- and the counter moves by two.
HostOutcome sneak_command_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  const CallerScope caller(*targets.commands, ctx.script);
  if (ctx.count() < 2 || !ctx.arg(1).is_string()) return HostOutcome::failed(kNoVerb);
  const std::string& verb = ctx.arg(1).as_string();
  for (const ObjectId id : targets.ids) {
    const CommandQueue* q = targets.commands->find(id);
    if (q == nullptr || q->entries.empty()) continue;
    if (!targets.commands->binds(*targets.world, id, verb, CommandArgKind::none)) continue;
    q = targets.commands->find(id);
    if (q == nullptr || q->entries.empty()) continue;
    Command made;
    made.id = targets.world->next_command_id();
    made.verb = verb;
    Command clone = q->entries.front();
    (void)targets.world->next_command_id();
    // `clone.id` stays the runner's.
    (void)targets.commands->insert_made(*targets.world, id, /*front=*/true, std::move(clone));
    (void)targets.commands->insert_made(*targets.world, id, /*front=*/true, std::move(made));
    (void)targets.commands->kill_command(*targets.world, id);
  }
  return HostOutcome::ok_void();
}

/// `SetCommand(verb[, arg])`. Replaces the queue outright, running command
/// included; see `script_set` for the order and for what a verb the class
/// does not bind in that shape does. `Obj::SetCommand` (0x005b0a50,
/// 0x005b0b30, 0x005b0c30) has no dead test; the `ObjList` forms (0x00561c10,
/// 0x00562050, 0x00562a70) skip a dead member; the `Query` forms (0x00577db0,
/// 0x005780d0, 0x00578870) take every member of the refreshed query. The list
/// forms do nothing when their object argument names nothing.
HostOutcome set_command_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  const CallerScope caller(*targets.commands, ctx.script);
  if (ctx.count() < 2 || !ctx.arg(1).is_string()) return HostOutcome::failed(kNoVerb);
  const std::string& verb = ctx.arg(1).as_string();
  const Command prototype = ctx.count() > 2 ? prototype_from(ctx.arg(2)) : Command{};
  const ReceiverForm form = receiver_form(ctx.arg(0));
  if (object_argument_missing(*targets.world, form, prototype)) return HostOutcome::ok_void();
  for (const ObjectId id : targets.ids) {
    if (form == ReceiverForm::list && dead(*targets.world, id)) continue;
    script_set(*targets.world, *targets.commands, id, verb, prototype,
               /*end_head_on_miss=*/form != ReceiverForm::object);
  }
  return HostOutcome::ok_void();
}

/// `SetCommandOffset(verb, pt)` / `AddCommandOffset(front, verb, pt)`: the
/// group-move spread. `ObjList` and `Query` only, six sites and two.
///
/// **Read** (0x00562520 and 0x00562280 on `ObjList`, 0x00578460 and
/// 0x00578280 on `Query`): the list's centre is the plain mean of its members'
/// positions (0x004cd1b0, each member's `vtbl+0xc8`; members that name nothing
/// are left out of the count) and the offset is `pt` less that centre. Each
/// live member is then given its own position plus the offset, **held inside
/// the map rectangle** (`[0x009a721c]+0xb4..0xc0`, the one `ClipDestToMap`
/// clamps to, here `clamp_to_map`), and ordered there exactly as the point
/// form of `SetCommand` / `AddCommand` orders it: the tail cleared, the method
/// looked up in its point form, and on a miss the head ended anyway. **Two
/// differences between the four bodies:** only the `Set` forms clamp -- the
/// `Add` forms (0x00562280, 0x00578280) hand on the raw position plus offset --
/// and only the `ObjList` forms skip a dead member; the `Query` forms refresh
/// the query (`vtbl+0x1c`) and take every member it holds.
///
/// `verb_at` is the argument index of the verb; the point follows it.
/// `front` is `std::nullopt` for the `Set` form, which replaces the queue, and
/// carries the flag for the `Add` form, which pushes onto it.
HostOutcome command_offset(CallContext& ctx, std::size_t verb_at,
                           std::optional<bool> front) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  const CallerScope caller(*targets.commands, ctx.script);
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
  const MovementSystem* movement = movement_system(world);
  const bool skip_dead = receiver_form(ctx.arg(0)) == ReceiverForm::list;
  for (const ObjectId id : targets.ids) {
    if (skip_dead && dead(world, id)) continue;
    const Point at = world.resolve_position(id);
    Command prototype;
    prototype.arg_kind = CommandArgKind::point;
    prototype.point = Point{anchor.x + at.x - centre.x, anchor.y + at.y - centre.y};
    if (!front.has_value() && movement != nullptr) {
      prototype.point = movement->grid().clamp_to_map(prototype.point);
    }
    if (front.has_value()) {
      if (targets.commands->binds(world, id, verb, CommandArgKind::point)) {
        targets.commands->add_command(world, id, *front, verb, prototype);
      }
    } else {
      script_set(world, *targets.commands, id, verb, prototype, /*end_head_on_miss=*/true);
    }
  }
  return HostOutcome::ok_void();
}

HostOutcome set_command_offset_impl(CallContext& ctx) {
  return command_offset(ctx, 1, std::nullopt);
}

/// `ObjList.AddCommandOffset(front, verb, pt)` -- the queueing form, and the
/// only difference is the queueing. Two sites, both in
/// `6_Great_loses_Boudicca` map 1 sequence 16, laying out a three-leg advance
/// one leg per call.
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
///
/// `Obj::KillCommand` (0x005b0d30) refuses a dead object ("called for a dead
/// object") and otherwise ends index 0 through 0x005b07d0; the `ObjList` form
/// (0x00562d20) skips dead members; the `Query` form (0x00578a80) takes every
/// member. 0x005b07d0 itself does nothing unless the object has a live script
/// in its command slot (0x0069f950), which in the original is every live
/// object's head; here a head queued this pass and not yet started is ended
/// all the same.
HostOutcome kill_command_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  const CallerScope caller(*targets.commands, ctx.script);
  const bool skip_dead = receiver_form(ctx.arg(0)) != ReceiverForm::query;
  for (const ObjectId id : targets.ids) {
    if (skip_dead && dead(*targets.world, id)) continue;
    targets.commands->kill_command(*targets.world, id);
  }
  return HostOutcome::ok_void();
}

/// `ClearCommands()`: drop the pending tail, leave the runner (`vtbl+0xc0`
/// with 0, 0x005b1120: every index from the last down to 1 through
/// 0x005b07d0). The same dead rule as `KillCommand`: `Obj` (0x005aa250)
/// refuses a dead object, `ObjList` (0x00562ea0) skips one, `Query`
/// (0x00578b60) does not ask.
HostOutcome clear_commands_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  const CallerScope caller(*targets.commands, ctx.script);
  const bool skip_dead = receiver_form(ctx.arg(0)) != ReceiverForm::query;
  for (const ObjectId id : targets.ids) {
    if (skip_dead && dead(*targets.world, id)) continue;
    targets.commands->clear_commands(*targets.world, id);
  }
  return HostOutcome::ok_void();
}

/// `command()` / `command(i)`: the name of the command at that queue index.
///
/// `Obj::command/0` (0x005abe40) answers the name of the command the object
/// last started, a string the start writes on the object (`[obj+0x110]`);
/// `Obj::command/1` (0x005ae620) answers the verb (`[cmd+0x34]`) of the
/// command at that index, and `""` past the end. Every started object's queue
/// holds its running `idle` from its start (`CommandSystem::start_slot`), so
/// both read the head's verb here.
///
/// **The bare form on an empty queue answers the default verb, `idle` -- a
/// labelled reading.** The queue is empty only for an object whose class binds
/// no `idle`, or a dying one; the original's string still holds the last
/// command it started, which this engine does not keep. `ES_STRONGHOLD.VS`
/// asks `barrack.command == "idle"` before it trains anything.
HostOutcome command_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  const std::int32_t index =
      ctx.count() > 1 && ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
  if (index < 0) return HostOutcome::ok_with(Value::string(std::string()));
  if (ctx.count() <= 1 && self.commands->command_count(self.id) == 0) {
    return HostOutcome::ok_with(Value::string(std::string(self.commands->default_verb())));
  }
  return HostOutcome::ok_with(Value::string(
      std::string(self.commands->command_name(self.id, static_cast<std::size_t>(index)))));
}

/// `CmdCount()` / `CmdCount(name)`.
///
/// `Obj::CmdCount/0` (0x005aa2c0) answers the queue's length, `[obj+0x98]`,
/// the running command included. `Obj::CmdCount/1` (0x005ae4d0) walks the
/// queue and counts the commands whose `<cmd>` row is called `name`, byte for
/// byte (`CommandSystem::command_count_named`) -- **by row, not by verb**: the
/// AI writes `barrack.CmdCount(cmd)` beside `barrack.ExecCmd(cmd, ...)` with the
/// one row name, and a train row's verb is `train`. A dead receiver of the bare
/// form prints and leaves the answer slot unwritten in the original; the count
/// is answered here.
HostOutcome cmd_count_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  const std::size_t n = ctx.count() > 1 && ctx.arg(1).is_string()
                            ? self.commands->command_count_named(self.id, ctx.arg(1).as_string())
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
  // `Progress()` (0x005ae890) refuses a dead receiver -- prints and returns 0,
  // no bar and no suspend. `Progress(ms)` (0x005ac530) asks nothing about
  // health.
  if constexpr (!kExplicitDuration) {
    if (dead(*self.world, self.id)) return HostOutcome::ok_void();
  }

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

/// `GetCommanded()`: whether the unit has been given an order since a script
/// last cleared the flag -- the unit's own bit, raised by the per-object issue
/// on every unit an order reaches (`CommandSystem::commanded`).
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
  // Bit 0x80000 of `[unit+0x194]` (0x005d81e0): the unit's, not a command's.
  return HostOutcome::ok_with(Value::boolean(self.commands->commanded(self.id)));
}

HostOutcome set_commanded_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) {
    if (self.error == kNoReceiver) return HostOutcome::ok_void();
    return HostOutcome::failed(self.error);
  }
  // 0x005d8180 writes the bit, queue or no queue.
  self.commands->set_commanded(self.id, ctx.count() > 1 && ctx.arg(1).truthy_scalar());
  return HostOutcome::ok_void();
}

// -- the player-facing command table ---------------------------------------

/// `ExecCmd(name, pt, obj, replace)`: issue a `DATA\COMMANDS` row as the
/// player's interface issues it. 22 sites: the AI's training, its market and
/// village orders, a hero's skill autocast, a siege's `advance`.
///
/// `Obj::ExecCmd` (0x005b5470) refuses a receiver that names nothing and a
/// dead one ("called for a dead object"), looks the row up by name (a miss
/// prints "Invalid command" and returns), and hands the per-object issue
/// (0x004ef3d0, `CommandSystem::issue`) a one-member list, the row, the point,
/// the object, `bReplace`, no modifier and the receiver's player.
/// `ObjList::ExecCmd` (0x0055e240) hands it the list itself and no player
/// (-1). Both are registered **void**: no script can read an answer.
///
/// What the issue does with a row is all of its own -- a `groupdispatch` run
/// with the row ambient, an `immediate` method run at once, a research that
/// jumps the queue, an ordinary order through the order insert, a train row
/// never replacing -- and so is the commanded flag it raises on every unit.
/// The cost is charged there, by the accept test, as for a player's press:
/// the shipped AI's `SpentGoldOnArmy` beside the call is a counter of its
/// own, not a payment.
///
/// **The fourth argument is `replace`**, which `GROUP_UNITSOUT.VS` names for us:
/// it forwards its own parameters as `l.ExecDefaultCmd(pt, obj, bReplace,
/// bModifier)`. Every shipped site passes `false`.
HostOutcome exec_cmd_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx, /*accept_query=*/false);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  const CallerScope caller(*targets.commands, ctx.script);
  if (ctx.count() < 2 || !ctx.arg(1).is_string()) return HostOutcome::failed(kNoVerb);
  World& world = *targets.world;
  const ReceiverForm form = receiver_form(ctx.arg(0));
  std::int32_t player_arg = 0;  // `ObjList::ExecCmd` passes -1, plus one
  if (form == ReceiverForm::object) {
    if (targets.ids.empty() || dead(world, targets.ids.front())) return HostOutcome::ok_void();
    const ObjectState* state = world.state(targets.ids.front());
    player_arg = state == nullptr || state->owner == kNoPlayer
                     ? 0
                     : static_cast<std::int32_t>(state->owner) + 1;
  }
  const CommandDef* def = targets.commands->table().find(ctx.arg(1).as_string());
  if (def == nullptr) return HostOutcome::ok_void();
  const Point point = ctx.count() > 2 && is_point(ctx.arg(2)) ? unpack_point(ctx.arg(2)) : Point{};
  const ObjectId object = ctx.count() > 3 ? object_of(ctx.arg(3)) : kNoObject;
  const bool replace = ctx.count() > 4 && ctx.arg(4).truthy_scalar();
  (void)targets.commands->issue(world, targets.ids, *def, point, object, replace,
                                /*modifier=*/false, player_arg, /*user=*/false);
  return HostOutcome::ok_void();
}

/// `ObjList::GetCanExecCmd(name)` -> `ObjList`: the members the row is
/// enabled for. One site, `BUILD_CATAPULT_VERIFY.VS`'s
/// `builders = _GetSelection().GetCanExecCmd("build_catapult");`.
///
/// 0x00560300 makes a new list, looks the row's index up by name (a negative
/// one leaves the list empty), and appends every member whose `vtbl+0xd8`
/// accepts that index (0x00560461) -- `CommandSystem::command_enabled`, the
/// whole-object flag then the disabled set. **It does not ask whether the
/// class binds the row's method**; that is the issue's question, not this
/// one's. The list is the calling script's temporary. This used to answer a
/// bool for a single object, which is neither the receiver nor the answer the
/// one shipped site is written for.
HostOutcome get_can_exec_cmd_impl(CallContext& ctx) {
  Receivers targets = receivers_of(ctx);
  if (!targets.ok()) return HostOutcome::failed(targets.error);
  if (ctx.count() < 2 || !ctx.arg(1).is_string()) return HostOutcome::failed(kNoVerb);
  World& world = *targets.world;
  ObjListPool& pool = objlist_pool_of(world);
  const ObjListId list = pool.acquire_temporary(ctx.script);
  const CommandDef* def = targets.commands->table().find(ctx.arg(1).as_string());
  if (def != nullptr) {
    if (std::vector<ObjectId>* items = pool.mutable_items(list)) {
      for (const ObjectId id : targets.ids) {
        if (targets.commands->command_enabled(world, id, def->name)) items->push_back(id);
      }
    }
  }
  return HostOutcome::ok_with(make_objlist_value(list));
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
  // 0x00425630: the row by name (the registry's `+0x14`); a miss answers false
  // and **writes neither argument**, which keep what the caller had in them.
  // A hit writes `costfood` (`[def+0x1ec]`) and `costgold` (`[def+0x1e8]`).
  const CommandDef* def = commands->table().find(ctx.arg(0).as_string());
  if (def == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  ctx.out(1) = Value::integer(def->cost_gold);
  ctx.out(2) = Value::integer(def->cost_food);
  return HostOutcome::ok_with(Value::boolean(true));
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
/// **The activity, step by step** (0x005d2ca0). It runs when it is set and
/// again each time the animation it started or the pose it held has run out,
/// with the budget `B` that is left:
///
///   1. The base animation is table index 12 for an idle and 19 for a taunt
///      (the entity's `idx` 13 and 20). With none, go to 3.
///   2. An idle picks its variant first: `n` is 1, 2, 3 or 4 as the indices
///      13, 14 and 15 after the base are present in turn (0x005a81b0), and it
///      plays index `12 + rand(0, n - 1)` from the synchronised generator --
///      no draw at all when `n` is 1. If that animation's length `L` is at
///      most `B`, then for `t` = 0, 100, ... up to `B - L`: when the unit's
///      walk index is 0 (here walk slot 1) and `rand(1, 15)` -- `rand(1, 3)`
///      for a taunt -- is 1, or the force flag is up, the flag drops, the
///      animation starts `t` ms on, `B` loses `t + L` (0x0053e6c0's answer)
///      and the step is over until it ends. A unit on another walk index
///      draws nothing.
///   3. Otherwise, with budget left the unit holds its pose for all of it
///      (0x0053ced0) and the budget is 0; with none left, the activity wakes
///      the script (0x0069f8f0) and goes back to 0.
///
/// So an idle on the map costs its whole `ms` and spends a draw per 100 ms of
/// it until a fidget plays, and one per variant choice at every step,
/// including the last. **Here** the steps are the call's re-entries: the
/// first run drops the target and takes the first step, and each step that
/// waits answers `retry` for the time to the next one, keeping the budget
/// and a fidget still to start in the call's word (`CallContext::scratch`);
/// the step that wakes the script answers done. **Approximated:** the
/// original holds the first step while an animation the unit was already
/// playing runs out (its timer 0x1b); here it is taken at once. The fidget
/// starts on the world's clock, not the script's millisecond.
///
/// A handle that names no object is held for `ms` as before: the original
/// dereferences null there, which is a fault rather than a behaviour, and a
/// wait is what keeps an idle loop from spinning on one.
enum class IdleKind : std::uint8_t { idle, taunt };

/// The call's word: the budget left in the low 24 bits, and the `idx` of a
/// fidget still to start above them, 0 for none.
constexpr std::uint32_t kIdleBudgetMask = 0x00FFFFFFu;
constexpr std::uint32_t kIdleAnimShift = 24;

[[nodiscard]] std::uint32_t idle_word(std::int64_t budget, std::int32_t pending) noexcept {
  const std::int64_t clamped = std::clamp<std::int64_t>(budget, 0, kIdleBudgetMask);
  return static_cast<std::uint32_t>(clamped) |
         (static_cast<std::uint32_t>(pending) << kIdleAnimShift);
}

[[nodiscard]] HostOutcome idle_wait(CallContext& ctx, std::int64_t wait, std::int64_t budget,
                                    std::int32_t pending) {
  if (ctx.scratch != nullptr) *ctx.scratch = static_cast<std::int32_t>(idle_word(budget, pending));
  HostOutcome out;
  out.status = script::HostStatus::retry;
  out.suspend_for = wait > 0 ? wait : 0;
  return out;
}

/// The declared length of the entity's animation `idx`, or -1 without one.
[[nodiscard]] std::int32_t entity_anim_length(const WorldObject& slot, std::int32_t idx) {
  if (slot.object == nullptr || slot.object->entity == nullptr) return -1;
  const EntityAnim* anim = slot.object->entity->anim(idx);
  return anim == nullptr ? -1 : anim->duration;
}

/// One step of the activity with `budget` left; see above.
HostOutcome idle_step(CallContext& ctx, const Self& self, IdleKind kind, std::int64_t budget,
                      bool force) {
  World& world = *self.world;
  const WorldObject* slot = world.find(self.id);
  // Table index + 1 is the entity's `idx`.
  const std::int32_t base = kind == IdleKind::idle ? 13 : 20;
  if (slot != nullptr && entity_anim_length(*slot, base) >= 0) {
    std::int32_t chosen = base;
    if (kind == IdleKind::idle) {
      std::int32_t variants = 1;
      if (entity_anim_length(*slot, base + 1) >= 0) {
        variants = 2;
        if (entity_anim_length(*slot, base + 2) >= 0) {
          variants = entity_anim_length(*slot, base + 3) >= 0 ? 4 : 3;
        }
      }
      chosen = base + world.rng().between(0, variants - 1);
    }
    const std::int32_t length = entity_anim_length(*slot, chosen);
    const MovementSystem* movement = movement_system(world);
    const MoveState* move = movement == nullptr ? nullptr : movement->find(self.id);
    const bool walk_index_zero = move == nullptr || move->walk_anim == 1;
    if (length >= 0 && length <= budget) {
      for (std::int64_t t = 0; t <= budget - length; t += 100) {
        if (!walk_index_zero) continue;
        const bool fidget =
            world.rng().between(1, kind == IdleKind::idle ? 15 : 3) == 1 || force;
        if (!fidget) continue;
        const std::int64_t left = budget - (t + length);
        if (t > 0) return idle_wait(ctx, t, left, chosen);
        (void)world.play_anim(self.id, chosen, AnimRepeat::hold);
        return idle_wait(ctx, length, left, 0);
      }
    }
  }
  if (budget > 0) return idle_wait(ctx, budget, 0, 0);
  // The wake. On a re-entry the script goes on; on the first run the call
  // still answered 3, which ends the slice (0x0069d5fb), and the wake at once
  // runs the script again on the next pass here.
  if (ctx.first_call) return HostOutcome::sleep_for(0);
  return HostOutcome::ok_void();
}

/// `Unit::Idle(ms)`, `Unit::Taunt(ms)` and `Ship::Idle()`; see above.
HostOutcome idle_activity(CallContext& ctx, const Self& self, std::int64_t ms, IdleKind kind,
                          bool force = false) {
  const WorldObject* slot = self.world->find(self.id);
  if (slot == nullptr || slot->state.position == kHeldPosition) {
    // The held branch: `ms` in the wait cell and return 1; its re-entry pops
    // the arguments and is done, which is a timed wait.
    if (!ctx.first_call) return HostOutcome::ok_void();
    return HostOutcome::sleep_for(ms > 0 ? ms : 0);
  }
  if (!ctx.first_call && ctx.scratch != nullptr) {
    const auto word = static_cast<std::uint32_t>(*ctx.scratch);
    const std::int64_t budget = word & kIdleBudgetMask;
    const auto pending = static_cast<std::int32_t>(word >> kIdleAnimShift);
    if (pending != 0) {
      (void)self.world->play_anim(self.id, pending, AnimRepeat::hold);
      return idle_wait(ctx, entity_anim_length(*slot, pending), budget, 0);
    }
    return idle_step(ctx, self, kind, budget, false);
  }
  if (CombatSystem* combat = combat_system_of(*self.world); combat != nullptr) {
    (void)combat->stop(self.id);
  }
  return idle_step(ctx, self, kind, ms > 0 ? ms : 0, force);
}

HostOutcome idle_impl(CallContext& ctx) {
  const Self self = resolve(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (ctx.count() > 1) {
    const std::int64_t ms = ctx.arg(1).is_integer() ? ctx.arg(1).as_integer() : 0;
    return idle_activity(ctx, self, ms, IdleKind::idle);
  }
  // `Ship::Idle()` (0x005c6ea0): the budget is the length of the ship's own
  // idle animation (table index 12, `[entity+0x149c]` -> `+0x4c`) and the
  // force flag `[ship+0x1b8]` goes up, so its first step plays the fidget
  // whatever the draw. **Not read:** the `[ship+0x1cc]` gate under which it
  // sets no activity at all.
  // A ship with no such animation is a null dereference in the original;
  // here it idles `kDefaultIdleSlice`, which keeps `SHIP_IDLE.VS`'s loop from
  // spinning.
  const WorldObject* slot = self.world->find(self.id);
  std::int64_t ms = kDefaultIdleSlice;
  if (slot != nullptr) {
    const std::int32_t own = entity_anim_length(*slot, 13);
    if (own >= 0) ms = own;
  }
  return idle_activity(ctx, self, ms, IdleKind::idle, /*force=*/true);
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
/// **Not reproduced:** the flag. `Idle` now takes the activity's steps
/// (`idle_activity`), and the flag would make the next step's first draw
/// play its fidget whatever it says; but a flag kept on the unit is state to
/// save and to agree on, and its one shipped writer is a hen. `Ship::Idle`'s
/// own force is the call's, and is passed to its first step directly.
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
  return idle_activity(ctx, self, ms, IdleKind::taunt);
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

/// `GotoAttack(target, slice, flag[, give_up])` -- `Unit::GotoAttack`
/// (0x005d6a90).
///
/// `Goto` with the arrival annulus taken from the attacker's own weapon instead
/// of from an argument -- which is exactly what its signature is missing next to
/// `Goto`'s, and what `UNIT_ENGAGE.VS` needs:
///
///     while (!.GotoAttack(u, 1500, true, 15000)) { ...retarget... }
///     while (.Attack(u));
///
/// The body: a receiver naming nothing answers **false** and does nothing
/// else. Otherwise, **on every run, first or re-entry, the unit's combat
/// target becomes the argument** (`[unit+0x1a8]`, the count zeroed when it
/// changes; `CombatSystem::hand_target`) -- the empty handle for an argument
/// naming nothing -- and the call is the object worker 0x005d40c0 with the
/// unit's own `range` and `min_range` (`[unit+0xd4]`, `[unit+0xd8]`): a target
/// that is the attacker answers true, one it may not approach (a garrisoned or
/// concealed enemy unit, or nothing at all) lays no route and waits the slice
/// (`goto_may_approach`), a unit is an object goal whose band (capped at 920,
/// `object_route_range`) is measured between the edges, and anything else is
/// a point goal at its centre with both radii added before the cap of 999.
/// The answer comes at the re-entry (`run_goto`).
///
/// **Approximated:** the inner bound is the class profile's and is carried
/// and not acted on, as for every `Goto`. A point argument, which no
/// shipped site passes, walks to the point as `Goto` would.
HostOutcome goto_attack_impl(CallContext& ctx) {
  const Mover self = mover_of(ctx);
  if (self.error == kNoReceiver && ctx.count() > 0 && self.world != nullptr &&
      receiver_departed(*self.world, ctx.arg(0))) {
    return HostOutcome::ok_with(Value::boolean(false));
  }
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (self.world->find(self.id) == nullptr) return HostOutcome::ok_with(Value::boolean(false));
  GotoOrder order;
  const ObjectId named = object_of(ctx.arg(1));
  const bool object_argument = !is_point(ctx.arg(1));
  if (object_argument && named == kNoObject && !receiver_departed(*self.world, ctx.arg(1))) {
    return HostOutcome::failed(kNoPoint);
  }
  const ObjectId target =
      named != kNoObject && self.world->find(named) != nullptr ? named : kNoObject;
  CombatSystem* combat = combat_system_of(*self.world);
  if (object_argument && combat != nullptr) (void)combat->hand_target(self.id, target);

  std::int32_t weapon = 0;
  std::int32_t reach = 0;
  if (combat != nullptr) {
    if (const Combatant* attacker = combat->find(self.id); attacker != nullptr) {
      const CombatProfile profile = combat->profile(attacker->class_index);
      // The stored range, as `in_attack_range` measures it (0x005a7800).
      weapon = combat->range_of(*attacker);
      reach = profile.radius;
      order.min_range = profile.min_range;
      const Combatant* defender = target == kNoObject ? nullptr : combat->find(target);
      if (defender != nullptr) reach += combat->profile(defender->class_index).radius;
    }
  }
  if (object_argument) {
    order.object_form = true;
    order.target = target;
    if (target != kNoObject) {
      order.dest = self.world->resolve_position(target);
      const WorldObject* other = self.world->find(target);
      order.range = other != nullptr && other->state.flags.is_unit
                        ? object_route_range(weapon) + reach
                        : point_route_range(weapon + reach);
    }
  } else {
    order.dest = unpack_point(ctx.arg(1));
    order.range = point_route_range(weapon + reach);
  }

  order.slice = ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0;
  order.give_up = ctx.count() > 4 && ctx.arg(4).is_integer() ? ctx.arg(4).as_integer() : -1;
  order.lock_destination = true;
  return run_goto(ctx, *self.world, *self.movement, self.id, order);
}

/// `GotoEnter(dest, range, slice, flag, give_up)` -- `Unit::GotoEnter`
/// (0x005d6620): `Goto`'s point form exactly, and `UNIT_ENTER.VS` puts the
/// two in the arms of one `if`. A receiver naming nothing answers **true**;
/// the destination is clamped to the map and written back into the call's own
/// argument (0x005d6675 ..); the range is `SetDest`'s, at most 999; the one
/// difference from `Goto` is the lock flag -- see the header and
/// `GotoOrder::give_up` -- and the answer comes at the re-entry (`run_goto`).
/// An object argument, which no shipped site passes, routes to where the
/// object stands.
HostOutcome goto_enter_impl(CallContext& ctx) {
  const Mover self = mover_of(ctx);
  if (self.error == kNoReceiver && ctx.count() > 0 && self.world != nullptr &&
      receiver_departed(*self.world, ctx.arg(0))) {
    return HostOutcome::ok_with(Value::boolean(true));
  }
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (self.world->find(self.id) == nullptr) return HostOutcome::ok_with(Value::boolean(true));
  GotoOrder order;
  if (is_point(ctx.arg(1))) {
    order.dest = self.movement->grid().clamp_to_map(unpack_point(ctx.arg(1)));
    ctx.out(1) = pack_point(order.dest);
  } else if (const ObjectId target = object_of(ctx.arg(1)); target != kNoObject) {
    order.target = target;
    order.dest = self.world->resolve_position(target);
  } else {
    return HostOutcome::failed(kNoPoint);
  }
  order.range = point_route_range(ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0);
  order.slice = ctx.arg(3).is_integer() ? ctx.arg(3).as_integer() : 0;
  order.give_up = ctx.count() > 5 && ctx.arg(5).is_integer() ? ctx.arg(5).as_integer() : -1;
  // 0x005d6620 drops the combat target on its first pass, once the unit is
  // out of any holder (0x005d6810..0x005d6820), as `Goto`'s point form does.
  order.drop_target = true;
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

/// Send one marching member to `station`, its slot's sample. A member
/// already going there -- within `repath_threshold` of it, on this hero's
/// march -- is left alone, so a marching column does not run a fresh A* per
/// member per call. The order carries the formation's lock flag
/// (`Formation::should_lock`), which makes the member free-spot tested once
/// the march is over (`MovementSystem::lock_owner`).
void march_member(World& world, MovementSystem& movement, ObjectId hero, ObjectId id,
                  Point station, bool lock) {
  const MoveState& member = movement.state(id);
  if (member.goto_active && member.party == hero &&
      within(member.target, station, MovementSystem::repath_threshold())) {
    return;
  }
  movement.order_goto(world, id, station, 0, 0, hero, lock);
}

/// 0x005f4390's hand-out: every unit the layout has just given a place takes
/// `form_move` with the replace flag (0x005aef20, 0x005b4e90 with 1).
void hand_out_form_move(World& world, std::span<const ObjectId> joined) {
  CommandSystem* commands = command_system(world);
  if (commands == nullptr) return;
  for (const ObjectId id : joined) {
    if (world.find(id) != nullptr) (void)commands->set_command(world, id, kFormMove, Command{});
  }
}

/// 0x005f2c10, the march ended: every member with a slot that is running
/// `form_move` loses its route and has that command's script stopped
/// (0x0069fee0), which pops it and starts the next -- `UNIT_FORM_MOVE.VS`
/// does not run on to its closing `Face`.
void end_march(World& world, MovementSystem& movement, const Formation& form) {
  CommandSystem* commands = command_system(world);
  std::vector<ObjectId> members;
  for (const FormationSlot& slot : form.slots) members.push_back(slot.unit);
  for (const ObjectId id : members) {
    if (world.find(id) == nullptr || !on_march(world, id)) continue;
    movement.stop(world, id);
    if (commands != nullptr) (void)commands->kill_command(world, id);
  }
}

/// How far short of its station a unit on a formation route is timed to be
/// while the formation's path goes on: 0x85, subtracted from every distance
/// the sample's deadline (0x005f589e, 0x005f59a5) and each step's duration
/// (0x00419e7a) are taken over, and nowhere once the path is spent.
constexpr std::int32_t kSampleShort = 133;

/// Where a march's formation is laid, and which way it faces.
///
/// The original's formation has a route of its own (`CVXFormObj::SetDest`,
/// 0x005f2cc0, laid from where the hero stands) and a centre that steps along
/// it a sample at a time (`CreateNextSample`, 0x005f5bd0, 200 units a step,
/// `kFormationStride`). Each sample's stations are laid around the new centre
/// facing the way it moved (`PlaceSample`, 0x005f5a20) -- the hero's own
/// station *is* the centre (0x005f5a84) -- and a sample is made when a unit
/// comes within 133 of its station (0x00419c78 .. 0x00419d72). So at the
/// moment a sample is laid, the centre stands one step beyond the 133 its
/// leader is timed short of: `kFormationStride + kSampleShort` ahead of him
/// along the route, or at its end. The sample at the end is the last one
/// (`[form+0x88]` cleared, 0x005f5cff), and no distance is taken short of it.
///
/// **Inferred onto this engine's march**, which has no formation route of its
/// own: the hero's route stands for it, as it does in `MovementSystem::
/// marching`, and the samples are laid whenever `place_army` runs -- the
/// hero's every `FormKeepMoving` -- rather than when the first unit nears its
/// station. Laying them around the hero where he stood made every station a
/// place he had already left, and the last of them, laid up to a
/// `FormKeepMoving` before he arrived, a place short of where he stopped.
struct MarchCentre {
  Point at;
  Point facing;
  /// Whether this is the route's end: the formation's last sample.
  bool last = true;
  /// How far along the hero's route the centre is from him.
  std::int64_t along = 0;
  /// How much of the route is left past the centre: the formation's path
  /// left at the sample (`[[form+0x84]+8]`'s `vtbl+0x14`).
  std::int64_t left = 0;
};

[[nodiscard]] MarchCentre march_centre(World& world, MovementSystem& movement, ObjectId hero) {
  const MoveState& lead = movement.state(hero);
  MarchCentre centre{world.resolve_position(hero), lead.facing, true, 0};
  if (!lead.has_path || lead.waypoints.size() < 2) return centre;
  const std::int64_t travelled = std::min(lead.travelled(), lead.path_length);
  const std::int64_t ahead = travelled + kFormationStride + kSampleShort;
  const std::int64_t at = std::min(ahead, lead.path_length);
  std::size_t segment = 0;
  centre.at = point_along(lead.waypoints, at, &segment);
  centre.last = ahead >= lead.path_length;
  centre.along = at - travelled;
  centre.left = lead.path_length - at;
  // The heading the centre moved in, which is the route's leg under it.
  const std::size_t leg = std::min(segment, lead.waypoints.size() - 2);
  const Point heading{lead.waypoints[leg + 1].x - lead.waypoints[leg].x,
                      lead.waypoints[leg + 1].y - lead.waypoints[leg].y};
  if (heading.x != 0 || heading.y != 0) centre.facing = heading;
  return centre;
}

/// Pace a march: the speed each of its units walks at until the stations are
/// laid again (`MoveState::march_rate`).
///
/// **The formation's speed** (`UpdateMinSpeed`, 0x005f5260) is the mean of
/// the leader's and every member's own speed, held to at most one and a half
/// times the slowest's (0x005f54c2 .. 0x005f54da). **The sample's deadline**
/// (0x005f5850) is now plus the mean, over the hero and every member, of the
/// time each needs at that speed to cover the straight distance to its
/// station, less 133 while the path goes on. **Each unit's step** on a route
/// aimed at the formation (0x00419e52) then takes the time left to the
/// deadline shared over what is left of its route to its station, less the
/// same 133 -- and is held to between 0.4 and 2 times the step's time at the
/// formation's speed (0x005f2130), so a unit walks between half and two and a
/// half times the formation's speed, whatever its own. The hero is one of
/// them: his route is aimed at the formation (0x0052e830), so he walks its
/// pace and not his own, and a straggler is hurried as he is held back. A
/// unit already at its distance, or a deadline already past, takes the fast
/// end (0x00419e91 writes a duration of 1, which the clamp lifts).
///
/// Integer here where the original averages the speeds in floating point
/// (0x005f5419, 0x005f54b0); its slowest is tracked by step length alone
/// (0x005f53d9 updates `[esp+0x10]` and not `[esp+0x14]`) where this takes the
/// whole rate -- an inference that reads the quirk as a slip. Not reproduced:
/// the time the unit's current animation has left (0x0053cef0), which the
/// deadline adds per unit, and the snap of a step to the deadline within
/// 100 ms (0x00419ec1).
void pace_march(World& world, MovementSystem& movement, ObjectId hero, const MarchCentre& centre,
                std::span<const ObjectId> marchers, std::span<const Point> stations) {
  MoveState& lead = movement.state(hero);
  struct Pacer {
    MoveState* move;
    std::int64_t straight;  ///< to its station, for the deadline
    std::int64_t walk;      ///< along its route to its station, for its pace
  };
  std::vector<Pacer> pacers;
  const std::int64_t short_of = centre.last ? 0 : kSampleShort;
  const auto less_short = [&](std::int64_t d) { return d > short_of ? d - short_of : 0; };
  // Only a route can be paced: a hero standing still leads no formation, and
  // nothing he lays round himself is walked at any pace but its own.
  if (lead.has_path && lead.own_rate() > 0) {
    pacers.push_back({&lead,
                      less_short(isqrt(dist_sq(world.resolve_position(hero), centre.at))),
                      less_short(centre.along)});
  }
  for (std::size_t i = 0; i < marchers.size() && !pacers.empty(); ++i) {
    MoveState& move = movement.state(marchers[i]);
    if (move.own_rate() <= 0) continue;  // a unit that cannot walk sets no pace
    const std::int64_t straight =
        less_short(isqrt(dist_sq(world.resolve_position(marchers[i]), stations[i])));
    const std::int64_t walk = move.has_path ? less_short(movement.remaining(marchers[i])) : 0;
    pacers.push_back({&move, straight, walk});
  }
  if (pacers.empty()) {
    lead.march_rate = 0;
    return;
  }

  std::int64_t sum = 0;
  std::int64_t slowest = pacers.front().move->own_rate();
  for (const Pacer& p : pacers) {
    sum += p.move->own_rate();
    slowest = std::min(slowest, p.move->own_rate());
  }
  const auto n = static_cast<std::int64_t>(pacers.size());
  std::int64_t speed = sum / n;
  if (2 * speed > 3 * slowest) speed = 3 * slowest / 2;
  if (speed <= 0) speed = 1;

  std::int64_t total = 0;
  for (const Pacer& p : pacers) total += p.straight * kSpeedScale / speed;
  const std::int64_t deadline = (total + n / 2) / n;

  const std::int64_t slow = speed / 2;
  const std::int64_t fast = speed * 5 / 2;
  for (const Pacer& p : pacers) {
    std::int64_t rate = deadline > 0 ? p.walk * kSpeedScale / deadline : fast;
    if (p.walk <= 0) rate = fast;
    p.move->march_rate = std::clamp(rate, slow > 0 ? slow : 1, fast);
  }
}

/// How near the end of the formation's path a settled orientation takes
/// over the formation's direction (0x005f5c7d, `< 200`).
constexpr std::int64_t kOrientationReach = 200;

/// Move a hero's march on: the formation (`sim/formation.hpp`) updated and
/// sampled, its members sent to their slots' samples, and the march paced
/// (`pace_march`). `update` is the hero's `FormKeepMoving`; without it this
/// is the first sample `FormSetupAndMoveTo` makes.
///
/// **Nothing without the formation's route** (`Formation::path`): `UpdateMove`
/// (0x005f5e30) and `CreateNextSample` (0x005f5bd0) both return at once when
/// `[form+0x84]` is null.
///
/// **The update** (`UpdateMove`) runs the layout if it is marked -- a member
/// joined or left, a shape was set -- and then, when it ran and a sample had
/// been laid, lays the current sample again where it stands; otherwise it
/// chooses the members' slots again if that is marked.
///
/// **The next sample** (`CreateNextSample`) is made when the centre has moved
/// on: here the centre stands `kFormationStride + kSampleShort` ahead of the
/// hero on his route (`march_centre`), where the original's steps along its
/// own route as units near their stations. The direction is the step the
/// centre took; a step against the last one marks the slots for choosing
/// again ("too radical change in direction", 0x005f5c4b); and within 200 of
/// the route's end a settled orientation (`FinalPartyOrientation`) takes
/// over. A centre that cannot move before any sample was laid ends the march
/// (0x005f2c10): what a setup to where the hero already stands does.
///
/// **Only a member running `form_move` is moved.** The original's members
/// follow the formation's samples through a route aimed at it (0x00417790),
/// which `UNIT_FORM_MOVE.VS`'s `FormAcceptMove` sets up; a member given any
/// other command since -- a squad order, `stand_position` at the march's end
/// -- has a script and a route of its own. **Nor is a member still inside a
/// holder**: it leaves in its own `FormAcceptMove`, one per exit slot.
std::size_t place_army(World& world, MovementSystem& movement, HeroSystem& heroes,
                       ObjectId hero, bool update) {
  const HeroRecord* record = heroes.hero(hero);
  Formation* form = movement.formation(hero);
  if (record == nullptr || form == nullptr || !form->path) return 0;
  const FormationTable& table = movement.formations();
  std::vector<ObjectId> joined;

  if (update) {
    const bool marked = form->rebuild;
    (void)formation_rebuild(world, *form, table, joined);
    if (form->samples < 1 || !marked) {
      formation_reassign(world, *form);
    } else {
      formation_place_sample(world, *form, table, form->centre, form->dir, joined);
    }
  }

  const MarchCentre centre = march_centre(world, movement, hero);
  if (centre.at != form->centre) {
    const Point step{centre.at.x - form->centre.x, centre.at.y - form->centre.y};
    if (static_cast<std::int64_t>(form->dir.x) * step.x +
            static_cast<std::int64_t>(form->dir.y) * step.y <
        0) {
      form->reassign = true;
    }
    Point dir = step;
    const Point settled = record->party_orientation;
    if ((settled.x != 0 || settled.y != 0) && centre.left < kOrientationReach) dir = settled;
    formation_place_sample(world, *form, table, centre.at, dir, joined);
    ++form->samples;
  } else if (form->samples == 0) {
    end_march(world, movement, *form);
  }
  hand_out_form_move(world, joined);
  // A command started above may have run a script that made another hero's
  // formation, which moves the formations in memory: look this one up again.
  form = movement.formation(hero);
  if (form == nullptr || form->samples == 0) return 0;

  // The march is one party: its members and the hero leading it do not block
  // one another's steps (`sim/movement.hpp`, inference 3).
  movement.state(hero).party = hero;
  std::vector<ObjectId> marchers;
  std::vector<Point> marching_to;
  const bool lock = form->should_lock;
  const std::vector<FormationSlot> slots = form->slots;
  for (const FormationSlot& slot : slots) {
    if (slot.unit == kNoObject || world.find(slot.unit) == nullptr) continue;
    if (!on_march(world, slot.unit)) continue;
    if (const ObjectState* state = world.state(slot.unit); state != nullptr && state->is_held()) {
      continue;
    }
    march_member(world, movement, hero, slot.unit, slot.sample, lock);
    marchers.push_back(slot.unit);
    marching_to.push_back(slot.sample);
  }
  pace_march(world, movement, hero, centre, marchers, marching_to);
  return slots.size();
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
    // The `Obj` form (0x0052e890) refuses a target that names nothing before
    // anything else: "Parameter #3 ... invalid object", and the hero's route
    // and his army are left exactly as they were.
    if (self.world->find(target) == nullptr) return HostOutcome::ok_void();
    dest = self.world->resolve_position(target);
  } else {
    return HostOutcome::failed(kNoPoint);
  }
  // The formation's route is laid by `SetDest` (0x005f2cc0 -> 0x0041a4c0),
  // which keeps at most 999 of the range.
  const std::int32_t range =
      point_route_range(ctx.arg(2).is_integer() ? ctx.arg(2).as_integer() : 0);
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

  // `CVXFormObj::SetDest` (0x005f2cc0; the object form 0x005f2ee0): the
  // formation's centre on the hero, the lock flag at `[form+0x8c]`, a
  // requested orientation settled (`+0x54` into `+0x5c`, the request
  // cleared; the object form clears both), no sample yet.
  Formation& form = self.movement->formation_of(self.id);
  form.centre = self.world->resolve_position(self.id);
  form.should_lock = ctx.count() > 4 && ctx.arg(4).truthy_scalar();
  form.samples = 0;
  if (HeroRecord* record = heroes->hero(self.id)) {
    record->party_orientation = target == kNoObject ? record->party_orientation_request : Point{};
    record->party_orientation_request = Point{};
  }
  // The formation's route. The hero's own stands for it here, laid as the
  // original lays his route aimed at the formation (0x00417790).
  const MoveOutcome outcome =
      target == kNoObject
          ? self.movement->order_goto(*self.world, self.id, dest, range, min_range)
          : self.movement->order_goto_object(*self.world, self.id, target, range, min_range);

  if (outcome == MoveOutcome::blocked) {
    // No route laid: the march is ended (0x005f2c10) and the route object
    // deleted.
    form.path = false;
    end_march(*self.world, *self.movement, form);
    return HostOutcome::ok_void();
  }
  // A route laid: every member with a slot -- not one still pending -- has
  // its route deleted (0x005d3830 with 0), its sample cleared to `(-1, -1)`
  // and `form_move` with the replace flag (0x005f2b50), which ends whatever
  // it was running. A pending member is handed it by the layout, below.
  form.path = true;
  std::vector<ObjectId> slotted;
  for (FormationSlot& slot : form.slots) {
    slot.sample = kNoSample;
    slotted.push_back(slot.unit);
  }
  for (const ObjectId member : slotted) {
    if (self.world->find(member) != nullptr) self.movement->stop(*self.world, member);
  }
  hand_out_form_move(*self.world, slotted);

  // And the first sample (`CreateNextSample`, 0x005f5bd0).
  place_army(*self.world, *self.movement, *heroes, self.id, false);
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
/// target itself here. This engine's march (`place_army`) orders each member
/// on the map straight to its slot's sample through the movement system,
/// in the same call that hands it `form_move`, before the command's script
/// first runs. So a member that reaches this call on the map already has its
/// order, and the answer is to finish, so the script's `while (.HasPath())`
/// takes over; the member's route then lasts as long as the march does
/// (`MovementSystem::marching`). **A member with no slot** -- still pending,
/// or dropped by a layout no `<Class>` entry of its shape matches -- is the
/// script-ending exit, as the original's with no record at `[unit+0x1c0]`.
///
/// **A held member leaves here, one exit slot at a time.** Aboard a ship it
/// polls every 100 ms; in a settlement it asks `garrison_exit` for the slot,
/// towards its station, and while the settlement's last exit is too recent
/// the call suspends for the wait it answers and runs again whole. Out, it is
/// sent to its station as `place_army` sends the others. The station is the
/// point at `+0x10` of the member's slot record, its sample, which
/// `form_move`'s hand-out (0x005f2bc5) resets to `(-1, -1)` and the next
/// sample fills; one not yet filled, or a hero back inside, sends it out
/// towards the hero and leaves it at the door. Until this, `place_army`
/// stepped every held member out in the one call that set the march up, and
/// a column left a town as one pile on its door.
///
/// **Every member that gets this far drops its combat target** (0x005d798b,
/// `CombatSystem::drop_target`). A unit with no hero is the original's
/// script-ending exit (0x005d7a1c returns 2), and so is one whose hero has no army
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
  // No hero, or no march to take a place in: the body returns 2 (0x005d7a1c),
  // which ends the calling script there -- `UNIT_FORM_MOVE.VS` never reaches
  // its `while (.HasPath())` or its closing `Face`, and `form_move` is over.
  if (record == nullptr || world->find(hero) == nullptr) return HostOutcome::end_script();

  // No slot -- the party record at `[unit+0x1c0]` -- is the same exit: a
  // unit still pending has no place to step out to.
  const Formation* form = movement->formation(hero);
  const FormationSlot* slot = form == nullptr ? nullptr : form->slot_of(id);
  if (slot == nullptr) return HostOutcome::end_script();

  if (const ObjectState* state = world->state(id); state != nullptr && state->is_held()) {
    // The member steps out towards its slot's sample (`[slot+0x10]`). One not
    // yet placed -- `(-1, -1)` since the march was set up -- or a hero back
    // inside has no place on the map: it steps out towards where he is.
    std::optional<Point> station;
    if (const ObjectState* lead = world->state(hero);
        lead != nullptr && !lead->is_held() && slot->sample != kNoSample) {
      station = slot->sample;
    }
    const bool lock = form->should_lock;
    const Point toward = station.value_or(unit_pos_rh(*world, hero));
    if (const std::int32_t wait = step_out_wait(*world, id, toward, now_of(ctx, *world));
        wait > 0) {
      return wait_to_step_out(wait);
    }
    if (station.has_value() && on_march(*world, id)) {
      movement->state(hero).party = hero;
      march_member(*world, *movement, hero, id, *station, lock);
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
/// call's result. Each call is the formation's `UpdateMove` (0x005f5e30) --
/// the layout rerun if marked, the current sample laid again if it was --
/// and then its next sample round the march's centre ahead of him
/// (`place_army`, `march_centre`), pacing every unit of it (`pace_march`),
/// which is what makes the formation move with him rather than pile up at
/// the destination he was given or trail behind him.
///
/// **Two functions answer to this name, and only the unit's drops the
/// target.** `Hero::FormKeepMoving` (0x0052eaf0) keeps the hero's formation
/// moving and leaves his combat target alone; `Unit::FormKeepMoving`
/// (0x005d7a40), which `UNIT_FORM_MOVE.VS`'s army members reach, empties the
/// target handle at `[unit+0x1a8]` with the attack count beside it on its
/// first entry (0x005d7a7b), then sets the marching state and suspends for
/// `ms` as the hero's does. Every shipped site but that one declares its
/// receiver `Hero`. The original picks the function by the receiver's
/// declared type; this registry keys on name and arity alone, so **the
/// receiver being a registered hero is read as the `Hero` form** -- an
/// inference that agrees with all 29 sites.
///
/// **The re-entry drops an arrived route** (0x0052eb5b, 0x005d7ab5): run
/// again after its wait, either form steps nothing and, when the unit holds a
/// route that is arrived (`IsArrived`, 0x00417c10), deletes it (0x005d3830
/// with 0), so the script's `while (.HasPath())` ends there rather than a
/// slice later. A member's route is aimed at the formation, and is arrived
/// only once the formation's own path is spent (0x00417c10's third mode,
/// 0x005f22f0): here, only once the march is over (`MovementSystem::marching`)
/// and the member stands in its station's band, its slot's sample.
HostOutcome form_keep_moving_impl(CallContext& ctx) {
  const Mover self = mover_of(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  if (!ctx.first_call) {
    if (self.world->find(self.id) != nullptr) {
      const MoveState* move = self.movement->find(self.id);
      if (move != nullptr && move->goto_active && !self.movement->marching(self.id, *move) &&
          self.movement->is_arrived(*self.world, self.id)) {
        self.movement->stop(*self.world, self.id);
      }
    }
    return HostOutcome::ok_void();
  }
  HeroSystem* heroes = hero_system_of(*self.world);
  if (heroes == nullptr || heroes->hero(self.id) == nullptr) {
    if (CombatSystem* combat = combat_system_of(*self.world); combat != nullptr) {
      (void)combat->drop_target(self.id);
    }
  }
  if (heroes != nullptr) place_army(*self.world, *self.movement, *heroes, self.id, true);

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
  HostOutcome out;
  out.status = script::HostStatus::retry;
  out.suspend_for = wait > 0 ? wait : 0;
  return out;
}

/// What 0x004185a0 adds to an incomplete path's length.
constexpr std::int64_t kIncompletePathLeft = 32000;

/// `FormPathLeft()`: world units still to walk on the hero's own route.
///
/// Both sites compare it against a radius -- `if (.FormPathLeft() <
/// .FormRadius() + s.radius) break;` -- so it is a distance and not a time.
///
/// 0x0052ebc0 asks the formation's route record (`[[hero+0x2b0]+0x84]`) and
/// answers 0 without one -- a hero who has never set a march up, or whose
/// last setup laid no route (`Formation::path`), whatever he walks; with one,
/// 0x004185a0 answers what is left of its path **plus 32,000 when the path is
/// incomplete** -- a route that stops short of its goal never reads as nearly
/// there. Here the hero's own route stands for the formation's while it is
/// the march's (`MoveState::party` is himself); a route of any other order
/// is not the formation's, whose path was spent when he stopped leading it
/// (approximated, as the march is).
HostOutcome form_path_left_impl(CallContext& ctx) {
  const Mover self = mover_of(ctx);
  if (!self.ok()) return HostOutcome::failed(self.error);
  const Formation* form = self.movement->formation(self.id);
  const MoveState* move = self.movement->find(self.id);
  if (form == nullptr || !form->path || move == nullptr || !move->has_path ||
      move->party != self.id) {
    return HostOutcome::ok_with(Value::integer(0));
  }
  std::int64_t left = self.movement->remaining(self.id);
  if (!move->path_complete) left += kIncompletePathLeft;
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
